#include "fmo_station_beacon.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sodium.h"
#include "services/aprs_service.h"
#include "services/fmo_aprs.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_link.h"
#include "services/server_directory.h"

/* Hard floor between two broadcasts, as in the reference firmware. */
#define MIN_GAP_MS 60000U
#define LOOP_MS 1000U
/* Personal BEACON: fixed 10-minute period on the reference firmware
 * (independent timer inside the same 1 s loop; MIN_GAP_MS still applies as
 * the rate limiter). */
#define BEACON_INTERVAL_MS 600000U

/* Online roster.  Real-network observation: every online device publishes
 * an 8-byte heartbeat to FMO/LATE/UID_V1/<uid> about once a minute, so the
 * count of distinct uids seen inside this window matches the broker's own
 * FMO/SERVER_INFO online count. */
#define ONLINE_WINDOW_MS 120000U
#define ROSTER_MAX 512U
/* Historical peak lives in its own NVS key (config schema untouched);
 * flash writes are throttled to one per minute when the peak grows. */
#define PEAK_NVS_NAMESPACE "open_fmo"
#define PEAK_NVS_KEY "st_peak"
#define PEAK_WRITE_MIN_MS 60000U

typedef struct {
    uint32_t uid;
    uint32_t seen_ms;
} roster_entry_t;

static const char *TAG = "fmo_station";
static SemaphoreHandle_t s_lock;
static fmo_config_t s_config;
static bool s_running;
static bool s_ready;
static uint32_t s_tx_count;
static uint32_t s_last_tx_ms;
static char s_gate[64] = "未启用";
static char s_host[64];
static uint16_t s_port;
/* Task stack lives in PSRAM (same rationale as aprs_service). */
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[8192 / sizeof(StackType_t)];
/* Large work buffers are module-static so the task stack stays small; only
 * the beacon task touches them. */
static uint8_t s_cert_blob[512];
static uint8_t s_tbs[512];
static char s_cert_b64[704];
static char s_sig_b64[96];
static char s_comment[896];
static char s_name_wire[97];  /* 32 chars x 3 UTF-8 bytes + NUL */
/* Personal-BEACON state and work buffers (module-static, beacon task only).
 * s_name_utf8/s_last_online/s_last_peak latch the values of the last
 * successful STATION broadcast so the APFMO1 notice repeats them. */
static uint32_t s_beacon_tx_count;
static uint32_t s_beacon_last_tx_ms;
static char s_beacon_gate[64] = "未启用";
static char s_name_utf8[97];
static uint32_t s_last_online;
static uint32_t s_last_peak;
static char s_rig_wire[49];   /* 16 chars x 3 UTF-8 bytes + NUL */
static char s_ant_wire[49];
static char s_text_utf8[640]; /* APFMO1/2 body assembly (UTF-8 on the wire) */
/* Static roster (4 KB): no heap, no task-stack cost.  The MQTT task feeds
 * it, the beacon task prunes it, the HTTP task reads the counters. */
static portMUX_TYPE s_roster_lock = portMUX_INITIALIZER_UNLOCKED;
static roster_entry_t s_roster[ROSTER_MAX];
static size_t s_roster_count;
static uint32_t s_auto_online;
static uint32_t s_auto_peak;      /* max online this run, seeded from NVS */
static uint32_t s_stored_peak;    /* value last read from / written to NVS */
static uint32_t s_last_peak_write_ms;

/* Minimal deterministic-CBOR writer (same encoding rules as fmo_cert_store:
 * shortest-form integers, definite lengths). */
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
} cbor_writer_t;

static void cbor_bytes(cbor_writer_t *writer, const void *data, size_t size)
{
    if (!writer->ok || writer->size + size > writer->capacity) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

static void cbor_head(cbor_writer_t *writer, uint8_t major, uint64_t value)
{
    uint8_t encoded[9];
    size_t size = 1;
    if (value < 24) {
        encoded[0] = (uint8_t)((major << 5) | value);
    } else if (value <= UINT8_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 24);
        encoded[1] = (uint8_t)value;
        size = 2;
    } else if (value <= UINT16_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 25);
        encoded[1] = (uint8_t)(value >> 8);
        encoded[2] = (uint8_t)value;
        size = 3;
    } else if (value <= UINT32_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 26);
        for (size_t i = 0; i < 4; ++i) encoded[1 + i] = (uint8_t)(value >> (24 - i * 8));
        size = 5;
    } else {
        encoded[0] = (uint8_t)((major << 5) | 27);
        for (size_t i = 0; i < 8; ++i) encoded[1 + i] = (uint8_t)(value >> (56 - i * 8));
        size = 9;
    }
    cbor_bytes(writer, encoded, size);
}

static void cbor_uint(cbor_writer_t *writer, uint64_t value)
{
    cbor_head(writer, 0, value);
}

static void cbor_text(cbor_writer_t *writer, const char *text)
{
    size_t size = strlen(text);
    cbor_head(writer, 3, size);
    cbor_bytes(writer, text, size);
}

static void cbor_blob(cbor_writer_t *writer, const uint8_t *data, size_t size)
{
    cbor_head(writer, 2, size);
    cbor_bytes(writer, data, size);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void fmo_station_note_uid(uint32_t uid)
{
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_roster_lock);
    size_t slot = SIZE_MAX;
    size_t oldest = 0;
    for (size_t i = 0; i < s_roster_count; ++i) {
        if (s_roster[i].uid == uid) {
            slot = i;
            break;
        }
        if (s_roster[i].seen_ms < s_roster[oldest].seen_ms) oldest = i;
    }
    if (slot != SIZE_MAX) {
        s_roster[slot].seen_ms = now;
    } else if (s_roster_count < ROSTER_MAX) {
        s_roster[s_roster_count].uid = uid;
        s_roster[s_roster_count].seen_ms = now;
        ++s_roster_count;
    } else {
        /* Roster full: evict the entry with the oldest heartbeat. */
        s_roster[oldest].uid = uid;
        s_roster[oldest].seen_ms = now;
    }
    portEXIT_CRITICAL(&s_roster_lock);
}

void fmo_station_clear_online(void)
{
    portENTER_CRITICAL(&s_roster_lock);
    s_roster_count = 0;
    s_auto_online = 0;
    portEXIT_CRITICAL(&s_roster_lock);
}

uint32_t fmo_station_online_auto(void)
{
    portENTER_CRITICAL(&s_roster_lock);
    uint32_t online = s_auto_online;
    portEXIT_CRITICAL(&s_roster_lock);
    return online;
}

uint32_t fmo_station_peak_auto(void)
{
    portENTER_CRITICAL(&s_roster_lock);
    uint32_t peak = s_auto_peak;
    portEXIT_CRITICAL(&s_roster_lock);
    return peak;
}

/* Drop entries older than the online window and refresh the counters;
 * called from the beacon task once per loop tick. */
static void roster_poll(void)
{
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_roster_lock);
    size_t kept = 0;
    for (size_t i = 0; i < s_roster_count; ++i) {
        if (now - s_roster[i].seen_ms < ONLINE_WINDOW_MS) {
            s_roster[kept++] = s_roster[i];
        }
    }
    s_roster_count = kept;
    s_auto_online = (uint32_t)kept;
    if (s_auto_online > s_auto_peak) s_auto_peak = s_auto_online;
    uint32_t peak = s_auto_peak;
    uint32_t stored = s_stored_peak;
    bool due = peak > stored && now - s_last_peak_write_ms >= PEAK_WRITE_MIN_MS;
    portEXIT_CRITICAL(&s_roster_lock);
    if (!due) return;
    nvs_handle_t handle;
    if (nvs_open(PEAK_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t error = nvs_set_u32(handle, PEAK_NVS_KEY, peak);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_roster_lock);
        s_stored_peak = peak;
        s_last_peak_write_ms = now;
        portEXIT_CRITICAL(&s_roster_lock);
        ESP_LOGI(TAG, "new online peak stored: %u", (unsigned)peak);
    }
}

static void peak_load(void)
{
    nvs_handle_t handle;
    uint32_t peak = 0;
    if (nvs_open(PEAK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    esp_err_t error = nvs_get_u32(handle, PEAK_NVS_KEY, &peak);
    nvs_close(handle);
    if (error != ESP_OK) return;
    portENTER_CRITICAL(&s_roster_lock);
    s_auto_peak = peak;
    s_stored_peak = peak;
    portEXIT_CRITICAL(&s_roster_lock);
}

static void upper_copy(char *out, size_t size, const char *input)
{
    size_t used = 0;
    for (const char *p = input; *p && used + 1 < size; ++p) {
        out[used++] = (char)toupper((unsigned char)*p);
    }
    out[used] = '\0';
}

/* FMO-V4 STATION TBS: deterministic CBOR array of 16 items
 * ["FMO",4,"STATION",callUpper,ssid,latStr,lonStr,certBlobHash,
 *  ccUpper,nameUtf8,host,port,coverageKm,online,peak,time(NULL)/600].
 * 30/30 real-network STATION broadcasts verify against this layout
 * (fmo-sim docs/firmware-analysis.md §8.3, verifier .tmp/verify_tbs7.py). */
static size_t build_tbs(const char *call_upper, uint32_t ssid,
                        const char *lat, const char *lon,
                        const uint8_t cert_blob_hash[32],
                        const char *cc_upper, const char *name_utf8,
                        const char *host, uint32_t port, uint32_t coverage_km,
                        uint32_t online, uint32_t peak, uint64_t time_slot,
                        uint8_t *out, size_t capacity)
{
    cbor_writer_t writer = {.data = out, .capacity = capacity, .ok = true};
    cbor_head(&writer, 4, 16);
    cbor_text(&writer, "FMO");
    cbor_uint(&writer, 4);
    cbor_text(&writer, "STATION");
    cbor_text(&writer, call_upper);
    cbor_uint(&writer, ssid);
    cbor_text(&writer, lat);
    cbor_text(&writer, lon);
    cbor_blob(&writer, cert_blob_hash, 32);
    cbor_text(&writer, cc_upper);
    cbor_text(&writer, name_utf8);
    cbor_text(&writer, host);
    cbor_uint(&writer, port);
    cbor_uint(&writer, coverage_km);
    cbor_uint(&writer, online);
    cbor_uint(&writer, peak);
    cbor_uint(&writer, time_slot);
    return writer.ok ? writer.size : 0;
}

/* FMO-V4 BEACON TBS: deterministic CBOR array of 10-13 items
 * ["FMO",4,"BEACON",callUpper,ssid,latStr,lonStr,certBlobHash,freqStr,
 *  (height, only when >0),(rig UTF-8, only when set),(ant UTF-8, only
 *  when set),time(NULL)/600].  Optional items are simply left out of the
 * array.  Verified against 9/9 real-network BEACON captures
 * (fmo-sim .tmp/verify_beacon.py); lat/lon strings and the freq "%.4f"
 * text are byte-identical to the wire frame, same as STATION. */
static size_t build_beacon_tbs(const char *call_upper, uint32_t ssid,
                               const char *lat, const char *lon,
                               const uint8_t cert_blob_hash[32],
                               const char *freq, uint32_t height_m,
                               const char *rig_utf8, const char *ant_utf8,
                               uint64_t time_slot,
                               uint8_t *out, size_t capacity)
{
    size_t items = 10;
    if (height_m > 0) ++items;
    if (rig_utf8[0] != '\0') ++items;
    if (ant_utf8[0] != '\0') ++items;
    cbor_writer_t writer = {.data = out, .capacity = capacity, .ok = true};
    cbor_head(&writer, 4, items);
    cbor_text(&writer, "FMO");
    cbor_uint(&writer, 4);
    cbor_text(&writer, "BEACON");
    cbor_text(&writer, call_upper);
    cbor_uint(&writer, ssid);
    cbor_text(&writer, lat);
    cbor_text(&writer, lon);
    cbor_blob(&writer, cert_blob_hash, 32);
    cbor_text(&writer, freq);
    if (height_m > 0) cbor_uint(&writer, height_m);
    if (rig_utf8[0] != '\0') cbor_text(&writer, rig_utf8);
    if (ant_utf8[0] != '\0') cbor_text(&writer, ant_utf8);
    cbor_uint(&writer, time_slot);
    return writer.ok ? writer.size : 0;
}

/* The three primary gates from the spec, in order:
 *   1. MQTT connected;
 *   2. the role the SAS accepted on this connection is exactly "super"
 *      ("admin" is deliberately NOT treated as super -- whether admin may
 *      broadcast is unverified, open point);
 *   3. the connected server's callsign equals the userCert callsign.
 * Plus the APRS-IS logresp-verified requirement, fixed position, synced
 * time, an operator-filled country code, and a resolvable server host/port.
 * Returns true when broadcasting may proceed; `reason` gets the first
 * failing gate (or NULL). */
static bool evaluate_gates(const fmo_config_t *config, const char **reason,
                           fmo_identity_status_t *identity,
                           fmo_server_t *server)
{
    *reason = NULL;
    if (!config->fmo_station_beacon_enabled) {
        *reason = "已在配置中关闭";
        return false;
    }
    if (fmo_cert_store_status(identity) != ESP_OK || !identity->ready) {
        *reason = "证书未就绪或已过期";
        return false;
    }
    /* Receiver cross-check: packet source base callsign must equal the cert
     * callsign, so the configured FMO callsign has to match too.  Compare
     * base parts only — the server list may carry an "-SSID" suffix. */
    if (!fmo_aprs_base_callsign_eq(config->fmo_callsign, identity->callsign)) {
        *reason = "FMO 呼号与证书不一致";
        return false;
    }
    fmo_link_status_t link = {0};
    fmo_link_get_status(&link);
    if (!link.connected) {
        *reason = "MQTT 未连接";
        return false;
    }
    if (strcmp(link.role, "super") != 0) {
        *reason = "当前登录角色不是 super";
        return false;
    }
    if (!fmo_aprs_base_callsign_eq(link.server_callsign, identity->callsign)) {
        *reason = "所选服务器呼号与本机证书不同";
        return false;
    }
    if (!aprs_service_is_verified()) {
        *reason = "APRS-IS 登录未验证";
        return false;
    }
    if (!config->aprs_position_set) {
        *reason = "未设置固定坐标";
        return false;
    }
    if ((uint64_t)time(NULL) < 1700000000ULL) {
        *reason = "系统时间未同步";
        return false;
    }
    if (strlen(config->fmo_country) != 2 ||
        !isalpha((unsigned char)config->fmo_country[0]) ||
        !isalpha((unsigned char)config->fmo_country[1])) {
        *reason = "国家码未填写";
        return false;
    }
    memset(server, 0, sizeof(*server));
    size_t index = fmo_server_directory_find(config->fmo_server_key);
    const fmo_server_t *selected =
        index == SIZE_MAX ? NULL : fmo_server_directory_get(index);
    if (selected != NULL) {
        *server = *selected;
    } else if (config->fmo_host[0] != '\0' && config->fmo_port != 0) {
        strlcpy(server->host, config->fmo_host, sizeof(server->host));
        server->port = config->fmo_port;
    }
    if (server->host[0] == '\0' || server->port == 0) {
        *reason = "未选择 FMO 服务器";
        return false;
    }
    return true;
}

/* Personal-BEACON gates, per the reference firmware: APRS-IS verified and
 * a valid certificate chain, plus an operator-set frequency.  Deliberately
 * NO MQTT/server/super gating -- the reference beacon fires with no server
 * connection at all.  Fixed position and synced time are required because
 * the signed TBS embeds both. */
static bool evaluate_beacon_gates(const fmo_config_t *config,
                                  const char **reason,
                                  fmo_identity_status_t *identity)
{
    *reason = NULL;
    if (!config->fmo_beacon_enabled) {
        *reason = "已在配置中关闭";
        return false;
    }
    if (fmo_cert_store_status(identity) != ESP_OK || !identity->ready) {
        *reason = "证书未就绪或已过期";
        return false;
    }
    if (!fmo_aprs_base_callsign_eq(config->fmo_callsign, identity->callsign)) {
        *reason = "FMO 呼号与证书不一致";
        return false;
    }
    if (!aprs_service_is_verified()) {
        *reason = "APRS-IS 登录未验证";
        return false;
    }
    if (!config->aprs_position_set) {
        *reason = "未设置固定坐标";
        return false;
    }
    if ((uint64_t)time(NULL) < 1700000000ULL) {
        *reason = "系统时间未同步";
        return false;
    }
    if (config->fmo_freq_x10000 == 0) {
        *reason = "未设置信标频率";
        return false;
    }
    return true;
}

static bool build_and_send(const fmo_config_t *config,
                           const fmo_identity_status_t *identity,
                           const fmo_server_t *server)
{
    size_t blob_size = 0;
    if (fmo_cert_store_build_cert_blob(s_cert_blob, sizeof(s_cert_blob),
                                       &blob_size) != ESP_OK) {
        ESP_LOGW(TAG, "CERT blob rebuild failed");
        return false;
    }
    uint8_t cert_blob_hash[32];
    crypto_hash_sha256(cert_blob_hash, s_cert_blob, blob_size);
    sodium_bin2base64(s_cert_b64, sizeof(s_cert_b64), s_cert_blob, blob_size,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    /* Position strings: same formatter as the packet prefix, so the TBS and
     * the wire position stay byte-identical. */
    char lat[12], lon[13];
    aprs_service_format_coord(config->aprs_latitude_e6, true, lat,
                              sizeof(lat));
    aprs_service_format_coord(config->aprs_longitude_e6, false, lon,
                              sizeof(lon));

    char call[16], cc[3];
    upper_copy(call, sizeof(call), identity->callsign);
    upper_copy(cc, sizeof(cc), config->fmo_country);
    /* Wire name is UTF-8, byte-identical to the TBS name (protocol spec;
     * the map server rejects GBK).  Empty name falls back to the callsign. */
    const char *name = config->fmo_station_name[0] != '\0'
        ? config->fmo_station_name : identity->callsign;
    strlcpy(s_name_wire, name, sizeof(s_name_wire));
    const uint32_t ssid = config->fmo_callsign_ssid <= 15
        ? config->fmo_callsign_ssid : 0;
    const uint64_t time_slot = (uint64_t)time(NULL) / 600ULL;
    /* 0 = automatic (heartbeat roster count / running peak), >0 = manual
     * override from the config page. */
    const uint32_t online = config->fmo_station_online > 0
        ? config->fmo_station_online : fmo_station_online_auto();
    const uint32_t peak = config->fmo_station_peak > 0
        ? config->fmo_station_peak : fmo_station_peak_auto();
    /* Latch for the APFMO1 login notice that follows a successful send. */
    strlcpy(s_name_utf8, name, sizeof(s_name_utf8));
    s_last_online = online;
    s_last_peak = peak;
    size_t tbs_size = build_tbs(call, ssid, lat, lon, cert_blob_hash, cc,
                                name, server->host, server->port,
                                config->fmo_coverage_km,
                                online, peak, time_slot,
                                s_tbs, sizeof(s_tbs));
    if (tbs_size == 0) {
        ESP_LOGW(TAG, "TBS build failed");
        return false;
    }
    uint8_t signature[64];
    if (fmo_cert_store_sign(s_tbs, tbs_size, signature) != ESP_OK) {
        ESP_LOGW(TAG, "TBS signing failed");
        return false;
    }
    sodium_bin2base64(s_sig_b64, sizeof(s_sig_b64), signature,
                      sizeof(signature),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    int written = snprintf(s_comment, sizeof(s_comment),
                           "FMO-V4,STATION,CERT:%s,%s,%s,%s,P%u,F%uKM,U%u/%u,SIG:%s",
                           s_cert_b64, cc, s_name_wire, server->host,
                           (unsigned)server->port,
                           (unsigned)config->fmo_coverage_km,
                           (unsigned)online, (unsigned)peak, s_sig_b64);
    if (written <= 0 || (size_t)written >= sizeof(s_comment)) {
        ESP_LOGW(TAG, "STATION comment too long");
        return false;
    }
    return aprs_service_send_fmo_v4_packet(s_comment);
}

/* APFMO1 server login notice, sent right after a successful STATION
 * broadcast: "><name>,正常,在线/峰值:<online>/<peak>,<notice>".  The body
 * is assembled in UTF-8 (name/online/peak latched by build_and_send) and
 * sent as-is: wire text is UTF-8 per the protocol spec. */
static void send_server_notice(const fmo_config_t *config)
{
    if (config->fmo_notice[0] == '\0') return;
    int body = snprintf(s_text_utf8, sizeof(s_text_utf8),
                        ">%s,正常,在线/峰值:%u/%u,%s", s_name_utf8,
                        (unsigned)s_last_online, (unsigned)s_last_peak,
                        config->fmo_notice);
    if (body <= 0 || (size_t)body >= sizeof(s_text_utf8)) {
        ESP_LOGW(TAG, "notice frame too long");
        return;
    }
    if (aprs_service_send_fmo_v4_frame("APFMO1", s_text_utf8)) {
        ESP_LOGI(TAG, "APFMO1 server notice sent");
    }
}

/* Personal BEACON: FMO-V4,BEACON signed broadcast on APFMO4, followed by
 * the APFMO2 personal message when one is configured. */
static bool build_and_send_beacon(const fmo_config_t *config,
                                  const fmo_identity_status_t *identity)
{
    size_t blob_size = 0;
    if (fmo_cert_store_build_cert_blob(s_cert_blob, sizeof(s_cert_blob),
                                       &blob_size) != ESP_OK) {
        ESP_LOGW(TAG, "CERT blob rebuild failed");
        return false;
    }
    uint8_t cert_blob_hash[32];
    crypto_hash_sha256(cert_blob_hash, s_cert_blob, blob_size);
    sodium_bin2base64(s_cert_b64, sizeof(s_cert_b64), s_cert_blob, blob_size,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    /* Position strings: same formatter as the packet prefix, so the TBS and
     * the wire position stay byte-identical. */
    char lat[12], lon[13];
    aprs_service_format_coord(config->aprs_latitude_e6, true, lat,
                              sizeof(lat));
    aprs_service_format_coord(config->aprs_longitude_e6, false, lon,
                              sizeof(lon));

    char call[16];
    upper_copy(call, sizeof(call), identity->callsign);
    const uint32_t ssid = config->fmo_callsign_ssid <= 15
        ? config->fmo_callsign_ssid : 0;
    /* FREQ is signed and sent as the same "%.4f" text. */
    char freq[24];
    snprintf(freq, sizeof(freq), "%.4f",
             (double)config->fmo_freq_x10000 / 10000.0);
    /* Wire RIG/ANT are the same UTF-8 text the TBS signs. */
    s_rig_wire[0] = '\0';
    s_ant_wire[0] = '\0';
    strlcpy(s_rig_wire, config->fmo_rig, sizeof(s_rig_wire));
    strlcpy(s_ant_wire, config->fmo_ant, sizeof(s_ant_wire));
    const uint64_t time_slot = (uint64_t)time(NULL) / 600ULL;
    size_t tbs_size = build_beacon_tbs(call, ssid, lat, lon, cert_blob_hash,
                                       freq, config->fmo_height_m,
                                       config->fmo_rig, config->fmo_ant,
                                       time_slot, s_tbs, sizeof(s_tbs));
    if (tbs_size == 0) {
        ESP_LOGW(TAG, "BEACON TBS build failed");
        return false;
    }
    uint8_t signature[64];
    if (fmo_cert_store_sign(s_tbs, tbs_size, signature) != ESP_OK) {
        ESP_LOGW(TAG, "BEACON TBS signing failed");
        return false;
    }
    sodium_bin2base64(s_sig_b64, sizeof(s_sig_b64), signature,
                      sizeof(signature),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);

    int written = snprintf(s_comment, sizeof(s_comment),
                           "FMO-V4,BEACON,CERT:%s,FREQ:%s",
                           s_cert_b64, freq);
    if (written > 0 && (size_t)written < sizeof(s_comment) &&
        config->fmo_height_m > 0) {
        written += snprintf(s_comment + written,
                            sizeof(s_comment) - (size_t)written,
                            ",HEIGHT:%u", (unsigned)config->fmo_height_m);
    }
    if (written > 0 && (size_t)written < sizeof(s_comment) &&
        s_rig_wire[0] != '\0') {
        written += snprintf(s_comment + written,
                            sizeof(s_comment) - (size_t)written,
                            ",RIG:%s", s_rig_wire);
    }
    if (written > 0 && (size_t)written < sizeof(s_comment) &&
        s_ant_wire[0] != '\0') {
        written += snprintf(s_comment + written,
                            sizeof(s_comment) - (size_t)written,
                            ",ANT:%s", s_ant_wire);
    }
    if (written > 0 && (size_t)written < sizeof(s_comment)) {
        written += snprintf(s_comment + written,
                            sizeof(s_comment) - (size_t)written,
                            ",SIG:%s", s_sig_b64);
    }
    /* The whole TNC2 frame must stay <= 512 chars, otherwise the reference
     * clients drop it: source call (<=9) + ">APFMO4,TCPIP*:" (15) +
     * "=<lat>F<lon>Ei" (25) + comment. */
    if (written <= 0 || (size_t)written >= sizeof(s_comment) ||
        (size_t)written + 49 > 512) {
        ESP_LOGW(TAG, "BEACON frame too long, send skipped");
        return false;
    }
    if (!aprs_service_send_fmo_v4_packet(s_comment)) return false;

    /* APFMO2 personal message follows a successful BEACON: "><UTF-8 text>". */
    if (config->fmo_aprs_msg[0] != '\0') {
        int body = snprintf(s_text_utf8, sizeof(s_text_utf8), ">%s",
                            config->fmo_aprs_msg);
        if (body > 0 && (size_t)body < sizeof(s_text_utf8)) {
            if (aprs_service_send_fmo_v4_frame("APFMO2", s_text_utf8)) {
                ESP_LOGI(TAG, "APFMO2 personal message sent");
            }
        } else {
            ESP_LOGW(TAG, "APRS message too long");
        }
    }
    return true;
}

static void publish_state(const char *reason, bool ready,
                          const fmo_server_t *server,
                          const char *beacon_reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_gate, sizeof(s_gate), "%s", reason);
    s_ready = ready;
    snprintf(s_beacon_gate, sizeof(s_beacon_gate), "%s", beacon_reason);
    if (server != NULL && server->host[0] != '\0') {
        strlcpy(s_host, server->host, sizeof(s_host));
        s_port = server->port;
    } else {
        s_host[0] = '\0';
        s_port = 0;
    }
    xSemaphoreGive(s_lock);
}

static void beacon_task(void *argument)
{
    (void)argument;
    const char *reason = "未启用";
    bool ready = false;
    fmo_identity_status_t identity = {0};
    fmo_server_t server = {0};
    const char *beacon_reason = "未启用";
    bool beacon_ready = false;
    fmo_identity_status_t beacon_identity = {0};
    unsigned tick = 0;
    for (;;) {
        /* Reload from NVS every tick: server/callsign changes made through
         * paths that do not notify this service (knob server switch, server
         * select API) are picked up within one second. */
        fmo_config_t config;
        if (config_store_load(&config) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_config = config;
        xSemaphoreGive(s_lock);

        uint32_t now = now_ms();
        roster_poll();
        uint32_t interval_ms =
            (uint32_t)config.fmo_station_beacon_interval_min * 60000U;
        if (interval_ms < MIN_GAP_MS) interval_ms = MIN_GAP_MS;
        const bool due = s_last_tx_ms == 0 || now - s_last_tx_ms >= interval_ms;
        /* Personal BEACON runs on its own fixed 10-minute timer inside the
         * same loop (MIN_GAP_MS is the rate-limit floor). */
        const bool beacon_due = s_beacon_last_tx_ms == 0 ||
                                now - s_beacon_last_tx_ms >= BEACON_INTERVAL_MS;
        /* Full gate evaluation re-reads and re-verifies the certificate
         * chain, so it runs every 5 s and whenever a send comes due. */
        if (tick++ % 5 == 0 || due || beacon_due) {
            reason = NULL;
            memset(&identity, 0, sizeof(identity));
            memset(&server, 0, sizeof(server));
            ready = evaluate_gates(&config, &reason, &identity, &server);
            beacon_reason = NULL;
            memset(&beacon_identity, 0, sizeof(beacon_identity));
            beacon_ready = evaluate_beacon_gates(&config, &beacon_reason,
                                                 &beacon_identity);
        }
        if (ready && due) {
            if (build_and_send(&config, &identity, &server)) {
                s_last_tx_ms = now;
                ++s_tx_count;
                reason = "已广播";
                ESP_LOGI(TAG, "STATION broadcast sent: %s:%u",
                         server.host, (unsigned)server.port);
                /* APFMO1 server login notice follows the STATION frame. */
                send_server_notice(&config);
            } else {
                reason = "发送失败";
            }
        } else if (ready) {
            reason = "就绪";
        }
        if (beacon_ready && beacon_due) {
            if (build_and_send_beacon(&config, &beacon_identity)) {
                s_beacon_last_tx_ms = now;
                ++s_beacon_tx_count;
                beacon_reason = "已广播";
                ESP_LOGI(TAG, "BEACON sent: freq %.4f MHz",
                         (double)config.fmo_freq_x10000 / 10000.0);
            } else {
                beacon_reason = "发送失败";
            }
        } else if (beacon_ready) {
            beacon_reason = "就绪";
        }
        publish_state(reason != NULL ? reason : "未知状态", ready, &server,
                      beacon_reason != NULL ? beacon_reason : "未知状态");
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}

static bool self_test(void)
{
    /* Wire text is UTF-8 now (no GBK conversion on the send path): the
     * name copy must pass CJK bytes through unchanged ("中" = E4 B8 AD). */
    char wire[16];
    strlcpy(wire, "中", sizeof(wire));
    if ((uint8_t)wire[0] != 0xe4 || (uint8_t)wire[1] != 0xb8 ||
        (uint8_t)wire[2] != 0xad || wire[3] != '\0') return false;
    /* TBS layout smoke test: 16-item array header, deterministic output. */
    uint8_t hash[32] = {0};
    uint8_t sample[512], repeat[512];
    size_t first = build_tbs("N0CALL", 0, "3952.80N", "11931.57E", hash,
                             "CN", "测试台", "fmo.example.com", 1883, 100, 0,
                             0, 2900000ULL, sample, sizeof(sample));
    size_t second = build_tbs("N0CALL", 0, "3952.80N", "11931.57E", hash,
                              "CN", "测试台", "fmo.example.com", 1883, 100, 0,
                              0, 2900000ULL, repeat, sizeof(repeat));
    if (first == 0 || first != second || sample[0] != 0x90 ||
        memcmp(sample, repeat, first) != 0) return false;
    /* BEACON TBS known-answer vector (13 items, all optional elements
     * present; layout verified against 9/9 real-network captures,
     * fmo-sim .tmp/verify_beacon.py). */
    static const uint8_t k_beacon_tbs[] = {
        0x8d, 0x63, 0x46, 0x4d, 0x4f, 0x04, 0x66, 0x42, 0x45, 0x41, 0x43,
        0x4f, 0x4e, 0x66, 0x4e, 0x30, 0x43, 0x41, 0x4c, 0x4c, 0x00, 0x68,
        0x33, 0x39, 0x35, 0x32, 0x2e, 0x38, 0x30, 0x4e, 0x69, 0x31, 0x31,
        0x39, 0x33, 0x31, 0x2e, 0x35, 0x37, 0x45, 0x58, 0x20, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x34, 0x33,
        0x39, 0x2e, 0x38, 0x32, 0x35, 0x30, 0x18, 0x32, 0x66, 0x46, 0x54,
        0x2d, 0x38, 0x39, 0x31, 0x66, 0x47, 0x50, 0x2d, 0x41, 0x4e, 0x54,
        0x1a, 0x00, 0x2c, 0x40, 0x20,
    };
    size_t beacon_len = build_beacon_tbs("N0CALL", 0, "3952.80N",
                                         "11931.57E", hash, "439.8250", 50,
                                         "FT-891", "GP-ANT", 2900000ULL,
                                         sample, sizeof(sample));
    if (beacon_len != sizeof(k_beacon_tbs) ||
        memcmp(sample, k_beacon_tbs, beacon_len) != 0) return false;
    /* With height 0 and empty rig/ant the optional elements collapse into
     * a 10-item array. */
    beacon_len = build_beacon_tbs("N0CALL", 0, "3952.80N", "11931.57E",
                                  hash, "439.8250", 0, "", "", 2900000ULL,
                                  sample, sizeof(sample));
    return beacon_len > 0 && sample[0] == 0x8a;
}

esp_err_t fmo_station_beacon_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (sodium_init() < 0) return ESP_ERR_INVALID_STATE;
    if (!self_test()) {
        ESP_LOGE(TAG, "station beacon self-test failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "self-test passed: UTF-8 wire passthrough + STATION/BEACON TBS layout");
    peak_load();
    if (s_lock != NULL) {
        fmo_station_beacon_update_config(config);
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_config = *config;
    if (xTaskCreateStatic(beacon_task, "fmo_station",
                          sizeof(s_task_stack) / sizeof(s_task_stack[0]),
                          NULL, 3, s_task_stack, &s_task_tcb) == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "service ready: enabled=%d interval=%umin",
             config->fmo_station_beacon_enabled ? 1 : 0,
             (unsigned)config->fmo_station_beacon_interval_min);
    return ESP_OK;
}

void fmo_station_beacon_update_config(const fmo_config_t *config)
{
    if (config == NULL || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = *config;
    xSemaphoreGive(s_lock);
}

void fmo_station_beacon_get_status(fmo_station_beacon_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->enabled = s_config.fmo_station_beacon_enabled;
    status->interval_min = s_config.fmo_station_beacon_interval_min;
    status->tx_count = s_tx_count;
    status->last_tx_ms = s_last_tx_ms;
    status->broadcasting = s_running && s_ready;
    status->beacon_enabled = s_config.fmo_beacon_enabled;
    status->beacon_tx_count = s_beacon_tx_count;
    status->beacon_last_tx_ms = s_beacon_last_tx_ms;
    strlcpy(status->beacon_gate, s_beacon_gate, sizeof(status->beacon_gate));
    portENTER_CRITICAL(&s_roster_lock);
    status->auto_online = s_auto_online;
    status->auto_peak = s_auto_peak;
    portEXIT_CRITICAL(&s_roster_lock);
    strlcpy(status->host, s_host, sizeof(status->host));
    status->port = s_port;
    strlcpy(status->gate, s_gate, sizeof(status->gate));
    xSemaphoreGive(s_lock);
}
