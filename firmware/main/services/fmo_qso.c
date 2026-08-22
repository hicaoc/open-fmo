#include "fmo_qso.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/audio_passthrough.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/app_notice.h"
#include "services/aprs_service.h"
#include "services/fmo_aprs.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_link.h"
#include "services/server_directory.h"
#include "services/storage_fs.h"

#define QUERY_TIMEOUT_S 10
#define QUERY_RETRY_S 3
#define JUMP_TIMEOUT_S 10
#define RING_TIMEOUT_S 7
#define ACCEPT_TIMEOUT_S 60
#define IN_RING_TIMEOUT_S 60
#define FAILED_HOLD_S 5

#define QSO_LOG_MAX 16
#define QSO_LOG_FILE "qso_log.json"
#define QSO_DEDUP_MAX 16
#define QSO_FIELD_MAX 8

static const char *TAG = "fmo_qso";
static SemaphoreHandle_t s_lock;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[6144 / sizeof(StackType_t)];

/* Config snapshot (updated by fmo_qso_update_config). */
static char s_callsign[16];
static char s_station_name[97];
static char s_qso_msg[385];
static uint32_t s_freq_x10000;
static bool s_position_set;
static int32_t s_lat_e6;
static int32_t s_lon_e6;

/* Call state. */
static fmo_qso_phase_t s_phase;
static bool s_outgoing;
static char s_peer[16];
static uint32_t s_peer_uid;
static uint32_t s_srv_uid;
static char s_srv_name[97];
static char s_in_msgid[12];  /* msgId of the CALL being rung (echoed back) */
static char s_detail[96];
static time_t s_deadline;
static time_t s_last_sent;
static time_t s_failed_at;
static time_t s_last_tone;
static uint32_t s_seq = 1;
static uint32_t s_log_id = 1;

/* Duplicate-message suppression (the same signaling line can arrive twice). */
static char s_seen[QSO_DEDUP_MAX][48];
static size_t s_seen_head;

static fmo_qso_log_entry_t s_log[QSO_LOG_MAX];
static size_t s_log_count;

static time_t now_s(void)
{
    return time(NULL);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Uppercase base callsign without any "-SSID" suffix. */
static void base_call(const char *callsign, char *out, size_t size)
{
    size_t used = 0;
    if (size == 0) return;
    for (const char *p = callsign != NULL ? callsign : ""; *p && *p != '-' &&
         used + 1 < size; ++p) {
        out[used++] = (char)toupper((unsigned char)*p);
    }
    out[used] = '\0';
}

static bool base_eq(const char *a, const char *b)
{
    char ba[16], bb[16];
    base_call(a, ba, sizeof(ba));
    base_call(b, bb, sizeof(bb));
    return ba[0] != '\0' && strcmp(ba, bb) == 0;
}

static uint32_t own_uid(void)
{
    fmo_identity_status_t identity;
    if (fmo_cert_store_status(&identity) != ESP_OK || !identity.ready) return 0;
    return identity.uid;
}

void fmo_qso_maidenhead(int32_t lat_e6, int32_t lon_e6, char out[7])
{
    double lat = (double)lat_e6 / 1000000.0;
    double lon = (double)lon_e6 / 1000000.0;
    if (out == NULL) return;
    out[0] = '\0';
    if (!isfinite(lat) || !isfinite(lon)) return;
    double lo = fmod(lon + 180.0, 360.0);
    if (lo < 0.0) lo += 360.0;
    if (lo > 359.999999) lo = 359.999999;
    if (lat > 90.0) lat = 90.0;
    if (lat < -90.0) lat = -90.0;
    double la = lat + 90.0;
    if (la > 179.999999) la = 179.999999;
    out[0] = (char)('A' + (int)(lo / 20.0));
    out[1] = (char)('A' + (int)(la / 10.0));
    out[2] = (char)('0' + (int)(fmod(lo, 20.0) / 2.0));
    out[3] = (char)('0' + (int)fmod(la, 10.0));
    out[4] = (char)('a' + (int)(fmod(lo, 2.0) * 12.0));
    out[5] = (char)('a' + (int)(fmod(la, 1.0) * 24.0));
    out[6] = '\0';
}

/* JSON-string escape into out (drops control characters). */
static void json_escape(char *out, size_t out_size, const char *input)
{
    size_t used = 0;
    if (out_size == 0) return;
    for (const char *p = input != NULL ? input : ""; *p && used + 2 < out_size;
         ++p) {
        if (*p == '"' || *p == '\\') out[used++] = '\\';
        if ((unsigned char)*p >= 0x20) out[used++] = *p;
    }
    out[used] = '\0';
}

/* ------------------------------------------------------------------ */
/* Message build / send                                                */
/* ------------------------------------------------------------------ */

/* Body of an APFMO0 message: ":TO<padded 9>:<payload>{<msgId>".  When
 * `msg_id` is NULL a fresh local sequence number is used; replies pass the
 * incoming msgId to echo it. */
static bool build_message_body(const char *to, const char *payload,
                               const char *msg_id, char *out, size_t size)
{
    char padded[10];
    char upper[16];
    size_t used = 0;
    for (const char *p = to; *p && used + 1 < sizeof(upper); ++p) {
        upper[used++] = (char)toupper((unsigned char)*p);
    }
    upper[used] = '\0';
    memset(padded, ' ', 9);
    padded[9] = '\0';
    size_t call_len = strlen(upper);
    if (call_len > 9) call_len = 9;
    memcpy(padded, upper, call_len);
    char id[12];
    if (msg_id != NULL && msg_id[0] != '\0') {
        strlcpy(id, msg_id, sizeof(id));
    } else {
        uint32_t seq;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        seq = s_seq++;
        xSemaphoreGive(s_lock);
        snprintf(id, sizeof(id), "%lu", (unsigned long)seq);
    }
    int written = snprintf(out, size, ":%s:%s{%s", padded, payload, id);
    return written > 0 && (size_t)written < size;
}

static bool send_to(const char *to, const char *payload, const char *msg_id)
{
    char body[256];
    if (!build_message_body(to, payload, msg_id, body, sizeof(body))) {
        return false;
    }
    const bool sent = aprs_service_send_fmo_v4_frame("APFMO0", body);
    ESP_LOGI(TAG, "TX %s %s", to, sent ? body : "(send failed)");
    return sent;
}

/* ------------------------------------------------------------------ */
/* QSO log (RAM ring + best-effort flash persistence)                  */
/* ------------------------------------------------------------------ */

static void persist_log_locked(void)
{
    if (!storage_fs_is_ready()) return;
    size_t capacity = 512 + s_log_count * 384;
    char *json = malloc(capacity);
    if (json == NULL) return;
    size_t used = snprintf(json, capacity, "[");
    for (size_t i = 0; i < s_log_count && used + 384 < capacity; ++i) {
        const fmo_qso_log_entry_t *e = &s_log[i];
        char peer[32], result[80], comment[FMO_QSO_LOG_COMMENT_MAX * 2];
        char grid[16], relay[128];
        json_escape(peer, sizeof(peer), e->peer);
        json_escape(result, sizeof(result), e->result);
        json_escape(comment, sizeof(comment), e->comment);
        json_escape(grid, sizeof(grid), e->grid);
        json_escape(relay, sizeof(relay), e->relay);
        used += snprintf(json + used, capacity - used,
                         "%s{\"ts\":%lld,\"dir\":\"%s\",\"peer\":\"%s\","
                         "\"uid\":%lu,\"result\":\"%s\",\"comment\":\"%s\","
                         "\"grid\":\"%s\",\"relay\":\"%s\"}",
                         i == 0 ? "" : ",", (long long)e->ts, e->dir, peer,
                         (unsigned long)e->peer_uid, result, comment, grid,
                         relay);
    }
    snprintf(json + used, capacity - used, "]");
    if (storage_fs_write_atomic(QSO_LOG_FILE, json, strlen(json)) != ESP_OK) {
        ESP_LOGW(TAG, "QSO log persist failed");
    }
    free(json);
}

static void record_locked_ts(int64_t ts, const char *dir, const char *peer,
                             uint32_t peer_uid, const char *result,
                             const char *comment, const char *grid,
                             const char *relay)
{
    if (s_log_count == QSO_LOG_MAX) {
        memmove(s_log, s_log + 1, sizeof(s_log) - sizeof(s_log[0]));
        s_log_count = QSO_LOG_MAX - 1;
    }
    fmo_qso_log_entry_t *e = &s_log[s_log_count++];
    memset(e, 0, sizeof(*e));
    e->ts = ts > 0 ? ts : (int64_t)now_s();
    strlcpy(e->dir, dir, sizeof(e->dir));
    strlcpy(e->peer, peer, sizeof(e->peer));
    e->peer_uid = peer_uid;
    strlcpy(e->result, result, sizeof(e->result));
    strlcpy(e->comment, comment != NULL ? comment : "", sizeof(e->comment));
    strlcpy(e->grid, grid != NULL ? grid : "", sizeof(e->grid));
    strlcpy(e->relay, relay != NULL ? relay : "", sizeof(e->relay));
    persist_log_locked();
}

static void record_locked(const char *dir, const char *peer, uint32_t peer_uid,
                          const char *result, const char *comment,
                          const char *grid, const char *relay)
{
    record_locked_ts(0, dir, peer, peer_uid, result, comment, grid, relay);
}

static void record(const char *dir, const char *peer, uint32_t peer_uid,
                   const char *result)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    record_locked(dir, peer, peer_uid, result, NULL, NULL, NULL);
    xSemaphoreGive(s_lock);
}

static void load_log(void)
{
    uint8_t *data = NULL;
    size_t size = 0;
    if (storage_fs_read(QSO_LOG_FILE, &data, &size,
                        32U * 1024U) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, size);
    free(data);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (s_log_count >= QSO_LOG_MAX) break;
        fmo_qso_log_entry_t *e = &s_log[s_log_count];
        memset(e, 0, sizeof(*e));
        cJSON *v;
        if ((v = cJSON_GetObjectItem(item, "ts")) != NULL) {
            e->ts = (int64_t)cJSON_GetNumberValue(v);
        }
        if ((v = cJSON_GetObjectItem(item, "dir")) != NULL) {
            strlcpy(e->dir, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->dir));
        }
        if ((v = cJSON_GetObjectItem(item, "peer")) != NULL) {
            strlcpy(e->peer, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->peer));
        }
        if ((v = cJSON_GetObjectItem(item, "uid")) != NULL) {
            e->peer_uid = (uint32_t)cJSON_GetNumberValue(v);
        }
        if ((v = cJSON_GetObjectItem(item, "result")) != NULL) {
            strlcpy(e->result, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->result));
        }
        if ((v = cJSON_GetObjectItem(item, "comment")) != NULL) {
            strlcpy(e->comment, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->comment));
        }
        if ((v = cJSON_GetObjectItem(item, "grid")) != NULL) {
            strlcpy(e->grid, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->grid));
        }
        if ((v = cJSON_GetObjectItem(item, "relay")) != NULL) {
            strlcpy(e->relay, cJSON_GetStringValue(v) != NULL
                    ? cJSON_GetStringValue(v) : "", sizeof(e->relay));
        }
        ++s_log_count;
    }
    cJSON_Delete(root);
    s_log_id = (uint32_t)s_log_count + 1;
    ESP_LOGI(TAG, "QSO log loaded: %u entries", (unsigned)s_log_count);
}

/* ------------------------------------------------------------------ */
/* State transitions                                                   */
/* ------------------------------------------------------------------ */

static void set_phase_locked(fmo_qso_phase_t phase, const char *detail)
{
    s_phase = phase;
    strlcpy(s_detail, detail != NULL ? detail : "", sizeof(s_detail));
    if (phase == FMO_QSO_PHASE_FAILED) s_failed_at = now_s();
}

/* Publish the local QSO record to the peer on FMO/QSO/UID/<peer uid>.
 * Single hook: only fired on transitions into ESTABLISHED (accept on either
 * side); cancel/reject/timeout never publish. */
static void publish_qso_record(void)
{
    char peer[16];
    uint32_t peer_uid;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(peer, s_peer, sizeof(peer));
    peer_uid = s_peer_uid;
    xSemaphoreGive(s_lock);
    if (peer_uid == 0) return;

    fmo_link_status_t link = {0};
    fmo_link_get_status(&link);
    if (!link.connected) {
        ESP_LOGW(TAG, "QSO record not sent: MQTT not connected");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char from_call[16], comment[385], station_name[97];
    strlcpy(from_call, s_callsign, sizeof(from_call));
    strlcpy(comment, s_qso_msg, sizeof(comment));
    strlcpy(station_name, s_station_name, sizeof(station_name));
    const bool position_set = s_position_set;
    const int32_t lat_e6 = s_lat_e6, lon_e6 = s_lon_e6;
    const uint32_t freq_x10000 = s_freq_x10000;
    const uint32_t log_id = s_log_id++;
    xSemaphoreGive(s_lock);

    char base[16], grid[7] = "";
    base_call(from_call, base, sizeof(base));
    if (position_set) fmo_qso_maidenhead(lat_e6, lon_e6, grid);

    char from_j[32], to_j[32], comment_j[770], grid_j[16], relay_j[192];
    char admin_j[32];
    json_escape(from_j, sizeof(from_j), base);
    json_escape(to_j, sizeof(to_j), peer);
    json_escape(comment_j, sizeof(comment_j), comment);
    json_escape(grid_j, sizeof(grid_j), grid);
    json_escape(relay_j, sizeof(relay_j),
                station_name[0] != '\0' ? station_name : link.server_name);
    json_escape(admin_j, sizeof(admin_j), link.server_callsign);

    char *json = malloc(1280);
    if (json == NULL) return;
    int written = snprintf(json, 1280,
        "{\"logId\":%lu,\"timestamp\":%llu,\"freqHz\":%llu,"
        "\"fromCallsign\":\"%s\",\"fromGrid\":\"%s\","
        "\"toCallsign\":\"%s\",\"toGrid\":\"\","
        "\"toComment\":\"%s\",\"mode\":\"FMO\","
        "\"relayName\":\"%s\",\"relayAdmin\":\"%s\"}",
        (unsigned long)log_id, (unsigned long long)now_s(),
        (unsigned long long)freq_x10000 * 100ULL,
        from_j, grid_j, to_j, comment_j, relay_j, admin_j);
    if (written > 0 && written < 1280) {
        char topic[40];
        snprintf(topic, sizeof(topic), "FMO/QSO/UID/%lu",
                 (unsigned long)peer_uid);
        if (fmo_link_publish(topic, json, written)) {
            ESP_LOGI(TAG, "QSO record sent to %s", topic);
        } else {
            ESP_LOGW(TAG, "QSO record publish to %s failed", topic);
        }
    }
    free(json);
}

static void set_phase(fmo_qso_phase_t phase, const char *detail)
{
    bool established = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_phase_locked(phase, detail);
    established = phase == FMO_QSO_PHASE_ESTABLISHED;
    xSemaphoreGive(s_lock);
    if (established) publish_qso_record();
}

static void fail(const char *peer, uint32_t peer_uid, const char *reason)
{
    record("out", peer, peer_uid, reason);
    char detail[96];
    snprintf(detail, sizeof(detail), "呼叫 %s 失败：%s", peer, reason);
    set_phase(FMO_QSO_PHASE_FAILED, detail);
}

/* ------------------------------------------------------------------ */
/* Field parsing                                                       */
/* ------------------------------------------------------------------ */

/* Split "VERB,a,b,c" into verb + fields (in-place, static-size arrays). */
static size_t split_fields(char *payload, char *verb, size_t verb_size,
                           char *fields[QSO_FIELD_MAX])
{
    size_t count = 0;
    char *save = NULL;
    char *token = strtok_r(payload, ",", &save);
    if (token == NULL) return 0;
    strlcpy(verb, token, verb_size);
    while ((token = strtok_r(NULL, ",", &save)) != NULL &&
           count < QSO_FIELD_MAX) {
        fields[count++] = token;
    }
    return count;
}

/* Q<num> / U<num> / S<num> markers; LA* and F* fields are skipped; the
 * first remaining field is the (UTF-8) name. */
static void parse_fields(char **fields, size_t count, uint32_t *q, uint32_t *u,
                         uint32_t *s, char *name, size_t name_size)
{
    *q = 0;
    *u = 0;
    *s = 0;
    if (name_size > 0) name[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        const char *f = fields[i];
        const size_t len = strlen(f);
        bool digits = len >= 2;
        for (size_t j = 1; j < len && digits; ++j) {
            digits = isdigit((unsigned char)f[j]);
        }
        if (digits && (f[0] == 'Q' || f[0] == 'U' || f[0] == 'S')) {
            const uint32_t value = (uint32_t)strtoul(f + 1, NULL, 10);
            if (f[0] == 'Q') *q = value;
            else if (f[0] == 'U') *u = value;
            else *s = value;
            continue;
        }
        if (f[0] == 'F' || (f[0] == 'L' && f[1] == 'A')) continue;
        if (name[0] == '\0') strlcpy(name, f, name_size);
    }
}

/* ------------------------------------------------------------------ */
/* Outgoing call leg                                                   */
/* ------------------------------------------------------------------ */

static bool send_call_locked_peer(void)
{
    /* Copies state, then sends CALL with a fresh msgId.  Caller must NOT
     * hold the lock. */
    char peer[16], srv_name[97];
    uint32_t peer_uid, srv_uid;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(peer, s_peer, sizeof(peer));
    strlcpy(srv_name, s_srv_name, sizeof(srv_name));
    peer_uid = s_peer_uid;
    srv_uid = s_srv_uid;
    xSemaphoreGive(s_lock);
    char payload[176];
    snprintf(payload, sizeof(payload), "CALL,Q%lu,U%lu,S%lu,%s",
             (unsigned long)own_uid(), (unsigned long)peer_uid,
             (unsigned long)srv_uid, srv_name);
    return send_to(peer, payload, NULL);
}

static void enter_calling(void)
{
    if (!send_call_locked_peer()) {
        char peer[16];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(peer, s_peer, sizeof(peer));
        set_phase_locked(FMO_QSO_PHASE_FAILED, "发送 CALL 失败");
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "CALL to %s failed", peer);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_phase_locked(FMO_QSO_PHASE_CALLING, "呼叫已发出，等待对方应答…");
    s_deadline = now_s() + RING_TIMEOUT_S;
    xSemaphoreGive(s_lock);
}

bool fmo_qso_call(const char *peer, uint32_t peer_uid, char *error,
                  size_t error_size)
{
    if (error != NULL && error_size > 0) error[0] = '\0';
    if (s_lock == NULL) return false;
    char target[16];
    strlcpy(target, peer != NULL ? peer : "", sizeof(target));
    /* Trim + uppercase. */
    size_t len = strlen(target);
    while (len > 0 && isspace((unsigned char)target[len - 1])) {
        target[--len] = '\0';
    }
    char *p = target;
    while (isspace((unsigned char)*p)) ++p;
    if (p != target) memmove(target, p, strlen(p) + 1);
    for (char *c = target; *c; ++c) *c = (char)toupper((unsigned char)*c);

    #define CALL_FAIL(msg) do {                                     \
        if (error != NULL) strlcpy(error, (msg), error_size);       \
        return false;                                               \
    } while (0)

    if (target[0] == '\0') CALL_FAIL("请输入对方呼号");
    if (peer_uid == 0) CALL_FAIL("需要对方 UID（设备无呼号-UID 查询表）");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_phase != FMO_QSO_PHASE_IDLE &&
                      s_phase != FMO_QSO_PHASE_FAILED;
    char own[16];
    strlcpy(own, s_callsign, sizeof(own));
    xSemaphoreGive(s_lock);
    if (busy) CALL_FAIL("当前有进行中的 QSO，请先取消/结束");
    if (base_eq(target, own)) CALL_FAIL("不能呼叫自己");
    if (!aprs_service_is_verified()) {
        CALL_FAIL("APRS-IS 未验证登录（先连接 APRS 且 passcode 正确）");
    }
    char payload[48];
    snprintf(payload, sizeof(payload), "QTHQRY,Q%lu,U%lu",
             (unsigned long)own_uid(), (unsigned long)peer_uid);
    if (!send_to(target, payload, NULL)) {
        CALL_FAIL("QTHQRY 发送失败（APRS 门控未通过）");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_peer, target, sizeof(s_peer));
    s_peer_uid = peer_uid;
    s_outgoing = true;
    const time_t t = now_s();
    s_last_sent = t;
    s_deadline = t + QUERY_TIMEOUT_S;
    char detail[96];
    snprintf(detail, sizeof(detail), "正在查询 %s 所在服务器…", target);
    set_phase_locked(FMO_QSO_PHASE_QUERYING, detail);
    xSemaphoreGive(s_lock);
    return true;
    #undef CALL_FAIL
}

void fmo_qso_answer(bool accept)
{
    if (s_lock == NULL) return;
    char peer[16], msgid[12];
    uint32_t peer_uid;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase != FMO_QSO_PHASE_INCOMING) {
        xSemaphoreGive(s_lock);
        return;
    }
    strlcpy(peer, s_peer, sizeof(peer));
    strlcpy(msgid, s_in_msgid, sizeof(msgid));
    peer_uid = s_peer_uid;
    xSemaphoreGive(s_lock);

    if (accept) {
        if (!send_to(peer, "CALLANS,ACCEPT", msgid)) return;
        record("in", peer, peer_uid, "已接听");
        char detail[96];
        snprintf(detail, sizeof(detail), "与 %s 的 QSO 已建立", peer);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_outgoing = false;
        set_phase_locked(FMO_QSO_PHASE_ESTABLISHED, detail);
        xSemaphoreGive(s_lock);
        publish_qso_record();
        app_notice_post_info("FMO QSO established", 4000);
    } else {
        send_to(peer, "CALLANS,REJECT", msgid);
        record("in", peer, peer_uid, "已拒绝");
        char detail[96];
        snprintf(detail, sizeof(detail), "已拒绝 %s 的呼叫", peer);
        set_phase(FMO_QSO_PHASE_IDLE, detail);
    }
}

void fmo_qso_cancel(void)
{
    if (s_lock == NULL) return;
    char peer[16], msgid[12];
    uint32_t peer_uid;
    bool outgoing;
    fmo_qso_phase_t phase;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    phase = s_phase;
    strlcpy(peer, s_peer, sizeof(peer));
    strlcpy(msgid, s_in_msgid, sizeof(msgid));
    peer_uid = s_peer_uid;
    outgoing = s_outgoing;
    xSemaphoreGive(s_lock);

    switch (phase) {
    case FMO_QSO_PHASE_QUERYING:
    case FMO_QSO_PHASE_JUMPING:
    case FMO_QSO_PHASE_CALLING:
    case FMO_QSO_PHASE_RINGING: {
        char payload[48];
        snprintf(payload, sizeof(payload), "CALLCANCEL,Q%lu,U%lu",
                 (unsigned long)own_uid(), (unsigned long)peer_uid);
        send_to(peer, payload, NULL);
        record("out", peer, peer_uid, "已取消");
        char detail[96];
        snprintf(detail, sizeof(detail), "已取消对 %s 的呼叫", peer);
        set_phase(FMO_QSO_PHASE_IDLE, detail);
        break;
    }
    case FMO_QSO_PHASE_INCOMING:
        send_to(peer, "CALLANS,REJECT", msgid);
        record("in", peer, peer_uid, "已拒绝");
        set_phase(FMO_QSO_PHASE_IDLE, "已拒绝来电");
        break;
    case FMO_QSO_PHASE_ESTABLISHED:
        record(outgoing ? "out" : "in", peer, peer_uid, "已结束");
        set_phase(FMO_QSO_PHASE_IDLE, "QSO 已结束");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Incoming signaling                                                  */
/* ------------------------------------------------------------------ */

static void on_qthqry(const char *from, char **fields, size_t count,
                      const char *msg_id)
{
    uint32_t q, u, s;
    char name[97];
    parse_fields(fields, count, &q, &u, &s, name, sizeof(name));
    (void)q;
    (void)s;
    /* U is the query target: not asking us -> stay silent. */
    const uint32_t my_uid = own_uid();
    if (u != 0 && my_uid != 0 && u != my_uid) return;

    fmo_server_t server;
    if (!fmo_link_get_selected_server(&server) || server.uid == 0) {
        ESP_LOGW(TAG, "QTHQRY from %s: no selected server, not answering",
                 from);
        return;
    }
    char station[97];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(station, s_station_name, sizeof(station));
    xSemaphoreGive(s_lock);
    if (station[0] == '\0') strlcpy(station, server.name, sizeof(station));

    char la[20];
    time_t t = now_s();
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(la, sizeof(la), "%Y%m%d%H%M%SZ", &tm_utc);
    char payload[176];
    snprintf(payload, sizeof(payload), "QTHANS,F1,U%lu,S%lu,LA%s,%s",
             (unsigned long)my_uid, (unsigned long)server.uid, la, station);
    if (send_to(from, payload, msg_id)) {
        ESP_LOGI(TAG, "answered %s QTHQRY (S%lu %s)", from,
                 (unsigned long)server.uid, station);
    }
}

static void on_qthans(const char *from, char **fields, size_t count)
{
    uint32_t q, u, srv_uid;
    char srv_name[97];
    parse_fields(fields, count, &q, &u, &srv_uid, srv_name, sizeof(srv_name));
    (void)q;
    (void)u;
    if (srv_uid == 0) {
        ESP_LOGW(TAG, "QTHANS from %s without server id", from);
        return;
    }
    char key[24];
    snprintf(key, sizeof(key), "uid:%lu", (unsigned long)srv_uid);

    char peer[16];
    uint32_t peer_uid;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_phase != FMO_QSO_PHASE_QUERYING || !base_eq(from, s_peer)) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_srv_uid = srv_uid;
    strlcpy(s_srv_name, srv_name, sizeof(s_srv_name));
    set_phase_locked(FMO_QSO_PHASE_JUMPING, "跳台到对方服务器…");
    s_deadline = now_s() + JUMP_TIMEOUT_S;
    strlcpy(peer, s_peer, sizeof(peer));
    peer_uid = s_peer_uid;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s is on server S%lu (%s), jumping", from,
             (unsigned long)srv_uid, srv_name);

    if (fmo_link_connected_to(key)) {
        enter_calling();
        return;
    }
    if (!fmo_link_jump_to_key(key)) {
        fail(peer, peer_uid, "对方服务器不在目录");
    }
    /* Otherwise the tick watches fmo_link_connected_to(). */
}

static void on_call(const char *from, char **fields, size_t count,
                    const char *msg_id)
{
    uint32_t q, u, srv_uid;
    char name[97];
    parse_fields(fields, count, &q, &u, &srv_uid, name, sizeof(name));
    (void)q;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_phase != FMO_QSO_PHASE_IDLE &&
                      s_phase != FMO_QSO_PHASE_FAILED;
    xSemaphoreGive(s_lock);
    if (busy) {
        send_to(from, "CALLANS,BUSY", msg_id);
        ESP_LOGI(TAG, "busy, answered BUSY to %s", from);
        return;
    }
    if (!send_to(from, "CALLANS,RING", msg_id)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_peer, from, sizeof(s_peer));
    s_peer_uid = u;
    s_srv_uid = srv_uid;
    strlcpy(s_srv_name, name, sizeof(s_srv_name));
    strlcpy(s_in_msgid, msg_id, sizeof(s_in_msgid));
    s_outgoing = false;
    s_deadline = now_s() + IN_RING_TIMEOUT_S;
    s_last_tone = 0;
    char detail[96];
    snprintf(detail, sizeof(detail), "%s 呼入", from);
    set_phase_locked(FMO_QSO_PHASE_INCOMING, detail);
    xSemaphoreGive(s_lock);
    char notice[48];
    snprintf(notice, sizeof(notice), "FMO call from %s", from);
    app_notice_post_info(notice, 10000);
}

static void on_callans(const char *from, char **fields, size_t count)
{
    if (count == 0) return;
    const char *answer = fields[0];
    char peer[16];
    uint32_t peer_uid;
    fmo_qso_phase_t phase;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    phase = s_phase;
    if ((phase != FMO_QSO_PHASE_CALLING && phase != FMO_QSO_PHASE_RINGING) ||
        !base_eq(from, s_peer)) {
        xSemaphoreGive(s_lock);
        return;
    }
    strlcpy(peer, s_peer, sizeof(peer));
    peer_uid = s_peer_uid;
    xSemaphoreGive(s_lock);

    if (strcmp(answer, "RING") == 0) {
        if (phase == FMO_QSO_PHASE_CALLING) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            set_phase_locked(FMO_QSO_PHASE_RINGING, "对方振铃中…");
            s_deadline = now_s() + ACCEPT_TIMEOUT_S;
            xSemaphoreGive(s_lock);
        }
    } else if (strcmp(answer, "ACCEPT") == 0) {
        record("out", peer, peer_uid, "接通");
        char detail[96];
        snprintf(detail, sizeof(detail), "%s 已接听，QSO 建立", peer);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_phase_locked(FMO_QSO_PHASE_ESTABLISHED, detail);
        xSemaphoreGive(s_lock);
        publish_qso_record();
    } else {
        const char *text = "对方未应答";
        if (strcmp(answer, "REJECT") == 0) text = "对方拒绝";
        else if (strcmp(answer, "BUSY") == 0) text = "对方忙";
        else if (strcmp(answer, "DND") == 0) text = "对方免打扰";
        else if (strcmp(answer, "NOTFRIEND") == 0) text = "对方未加好友";
        else if (strcmp(answer, "NOSERVER") == 0) text = "对方无服务器";
        else if (strcmp(answer, "TIMEOUT") == 0) text = "对方超时";
        fail(peer, peer_uid, text);
    }
}

static void on_callcancel(const char *from)
{
    char peer[16];
    uint32_t peer_uid;
    bool outgoing;
    fmo_qso_phase_t phase;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    phase = s_phase;
    if ((phase != FMO_QSO_PHASE_INCOMING &&
         phase != FMO_QSO_PHASE_ESTABLISHED) || !base_eq(from, s_peer)) {
        xSemaphoreGive(s_lock);
        return;
    }
    strlcpy(peer, s_peer, sizeof(peer));
    peer_uid = s_peer_uid;
    outgoing = s_outgoing;
    xSemaphoreGive(s_lock);

    if (phase == FMO_QSO_PHASE_INCOMING) {
        record("in", peer, peer_uid, "对方取消");
        set_phase(FMO_QSO_PHASE_IDLE, "对方取消了呼叫");
    } else {
        record(outgoing ? "out" : "in", peer, peer_uid, "对方结束");
        set_phase(FMO_QSO_PHASE_IDLE, "对方结束了 QSO");
    }
}

void fmo_qso_handle_aprs_message(const char *from, const char *to,
                                 const char *payload, const char *msg_id)
{
    if (s_lock == NULL || from == NULL || payload == NULL) return;
    char own[16];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(own, s_callsign, sizeof(own));
    xSemaphoreGive(s_lock);
    if (!base_eq(to, own)) return;

    /* Dedup on from|verb|msgId: the same signaling line can be delivered
     * twice (e.g. multiple APRS-IS paths). */
    char verb[16] = "";
    {
        const char *comma = strchr(payload, ',');
        const size_t verb_len = comma != NULL
            ? (size_t)(comma - payload) : strlen(payload);
        strlcpy(verb, payload,
                verb_len + 1 < sizeof(verb) ? verb_len + 1 : sizeof(verb));
    }
    char key[48];
    snprintf(key, sizeof(key), "%s|%s|%s", from, verb,
             msg_id != NULL ? msg_id : "");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < QSO_DEDUP_MAX; ++i) {
        if (s_seen[i][0] != '\0' && strcmp(s_seen[i], key) == 0) {
            xSemaphoreGive(s_lock);
            return;
        }
    }
    strlcpy(s_seen[s_seen_head], key, sizeof(s_seen[0]));
    s_seen_head = (s_seen_head + 1) % QSO_DEDUP_MAX;
    xSemaphoreGive(s_lock);

    char buffer[224];
    strlcpy(buffer, payload, sizeof(buffer));
    char *fields[QSO_FIELD_MAX];
    const size_t count = split_fields(buffer, verb, sizeof(verb), fields);

    if (strcmp(verb, "QTHQRY") == 0) {
        on_qthqry(from, fields, count, msg_id);
    } else if (strcmp(verb, "QTHANS") == 0) {
        on_qthans(from, fields, count);
    } else if (strcmp(verb, "CALL") == 0) {
        on_call(from, fields, count, msg_id);
    } else if (strcmp(verb, "CALLANS") == 0) {
        on_callans(from, fields, count);
    } else if (strcmp(verb, "CALLCANCEL") == 0) {
        on_callcancel(from);
    }
}

/* Complete QSO record from the peer (FMO/QSO/UID/<own uid>): store it in the
 * local QSO log ("shown in the peer's QSO records"). */
void fmo_qso_handle_mqtt_record(const char *json, size_t size)
{
    if (s_lock == NULL || json == NULL || size == 0) return;
    cJSON *root = cJSON_ParseWithLength(json, size);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid QSO record JSON (%u bytes)", (unsigned)size);
        return;
    }
    const char *from = "", *comment = "", *grid = "", *relay = "";
    int64_t ts = (int64_t)now_s();
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "fromCallsign")) != NULL &&
        cJSON_GetStringValue(v) != NULL) from = cJSON_GetStringValue(v);
    if ((v = cJSON_GetObjectItem(root, "toComment")) != NULL &&
        cJSON_GetStringValue(v) != NULL) comment = cJSON_GetStringValue(v);
    if ((v = cJSON_GetObjectItem(root, "fromGrid")) != NULL &&
        cJSON_GetStringValue(v) != NULL) grid = cJSON_GetStringValue(v);
    if ((v = cJSON_GetObjectItem(root, "relayName")) != NULL &&
        cJSON_GetStringValue(v) != NULL) relay = cJSON_GetStringValue(v);
    if ((v = cJSON_GetObjectItem(root, "timestamp")) != NULL) {
        ts = (int64_t)cJSON_GetNumberValue(v);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    record_locked_ts(ts, "in", from, 0, "通联记录", comment, grid, relay);
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "QSO record from %s stored", from);
}

/* ------------------------------------------------------------------ */
/* Ring tone + tick                                                    */
/* ------------------------------------------------------------------ */

/* Short dual-tone burst into the FMO speaker queue (only while the audio
 * pipeline runs).  Synthesized in 50 ms chunks to keep buffers small. */
static void ring_tone(void)
{
    if (!audio_passthrough_is_running()) return;
    static int16_t chunk[400];
    double phase = 0.0;
    for (int seg = 0; seg < 16; ++seg) {  /* 16 x 50 ms = 800 ms */
        const double freq = (seg & 1) ? 660.0 : 880.0;
        const double step = 2.0 * M_PI * freq / 8000.0;
        for (size_t i = 0; i < sizeof(chunk) / sizeof(chunk[0]); ++i) {
            chunk[i] = (int16_t)(sin(phase) * 10000.0);
            phase += step;
        }
        audio_passthrough_queue_fmo_output(
            chunk, sizeof(chunk) / sizeof(chunk[0]), 8000);
    }
}

static void tick(void)
{
    if (s_lock == NULL) return;
    char peer[16];
    uint32_t peer_uid;
    fmo_qso_phase_t phase;
    time_t deadline, last_sent;
    time_t t = now_s();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    phase = s_phase;
    strlcpy(peer, s_peer, sizeof(peer));
    peer_uid = s_peer_uid;
    deadline = s_deadline;
    last_sent = s_last_sent;
    xSemaphoreGive(s_lock);

    switch (phase) {
    case FMO_QSO_PHASE_QUERYING:
        if (t >= deadline) {
            fail(peer, peer_uid, "对方未应答服务器查询");
        } else if (t - last_sent >= QUERY_RETRY_S) {
            /* Resend QTHQRY with a fresh msgId (matches the stock firmware's
             * {2 {3 {4 retransmissions). */
            char payload[48];
            snprintf(payload, sizeof(payload), "QTHQRY,Q%lu,U%lu",
                     (unsigned long)own_uid(), (unsigned long)peer_uid);
            if (send_to(peer, payload, NULL)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_phase == FMO_QSO_PHASE_QUERYING) s_last_sent = t;
                xSemaphoreGive(s_lock);
            }
        }
        break;
    case FMO_QSO_PHASE_JUMPING: {
        char key[24];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(key, sizeof(key), "uid:%lu", (unsigned long)s_srv_uid);
        xSemaphoreGive(s_lock);
        if (fmo_link_connected_to(key)) {
            enter_calling();
        } else if (t >= deadline) {
            fail(peer, peer_uid, "跳台失败");
        }
        break;
    }
    case FMO_QSO_PHASE_CALLING:
        if (t >= deadline) fail(peer, peer_uid, "对方无应答");
        break;
    case FMO_QSO_PHASE_RINGING:
        if (t >= deadline) fail(peer, peer_uid, "对方未接听（超时）");
        break;
    case FMO_QSO_PHASE_INCOMING:
        if (t >= deadline) {
            record("in", peer, peer_uid, "未接来电");
            char detail[96];
            snprintf(detail, sizeof(detail), "未接来电：%s", peer);
            set_phase(FMO_QSO_PHASE_IDLE, detail);
        } else {
            bool due;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            due = s_last_tone == 0 || t - s_last_tone >= 3;
            if (due) s_last_tone = t;
            xSemaphoreGive(s_lock);
            if (due) ring_tone();
        }
        break;
    case FMO_QSO_PHASE_FAILED:
        if (t - s_failed_at >= FAILED_HOLD_S) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_phase == FMO_QSO_PHASE_FAILED) s_phase = FMO_QSO_PHASE_IDLE;
            xSemaphoreGive(s_lock);
        }
        break;
    default:
        break;
    }
}

static void qso_task(void *argument)
{
    (void)argument;
    for (;;) {
        tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static bool self_test(void)
{
    char grid[7];
    fmo_qso_maidenhead(39900000, 116400000, grid);
    if (strcmp(grid, "OM89ev") != 0) return false;
    fmo_qso_maidenhead(32393200, 119370600, grid);
    if (strcmp(grid, "OM92qj") != 0) return false;
    fmo_qso_maidenhead(-33865000, -74006000, grid);
    if (strcmp(grid, "FF26xd") != 0) return false;

    char body[256];
    if (!build_message_body("BG8LLD", "QTHQRY,Q3187,U2533", "2", body,
                            sizeof(body)) ||
        strcmp(body, ":BG8LLD    :QTHQRY,Q3187,U2533{2") != 0) {
        return false;
    }
    /* Field parsing incl. msgId-echo vectors from the Rust reference. */
    char buf[224];
    strlcpy(buf, "F1,U2725,S2579,LA20260806010157Z,\xe6\xb2\xb3\xe5\x8c\x97\xe6\x9f\x90\xe5\x9c\xb0",
            sizeof(buf));
    char verb[16];
    char *fields[QSO_FIELD_MAX];
    size_t count = split_fields(buf, verb, sizeof(verb), fields);
    uint32_t q, u, s;
    char name[97];
    parse_fields(fields, count, &q, &u, &s, name, sizeof(name));
    if (u != 2725 || s != 2579 || q != 0 ||
        strcmp(name, "\xe6\xb2\xb3\xe5\x8c\x97\xe6\x9f\x90\xe5\x9c\xb0") != 0) {
        return false;
    }
    strlcpy(buf, "Q796,U2533,S2579,\xe6\xb5\x8b\xe8\xaf\x95\xe5\x8f\xb0", sizeof(buf));
    count = split_fields(buf, verb, sizeof(verb), fields);
    parse_fields(fields, count, &q, &u, &s, name, sizeof(name));
    if (q != 796 || u != 2533 || s != 2579 ||
        strcmp(name, "\xe6\xb5\x8b\xe8\xaf\x95\xe5\x8f\xb0") != 0) {
        return false;
    }
    return true;
}

esp_err_t fmo_qso_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock != NULL) {
        fmo_qso_update_config(config);
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    fmo_qso_update_config(config);
    if (!self_test()) {
        ESP_LOGE(TAG, "self-test failed (message format / grid)");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "self-test ok: APFMO0 message format, msgId echo, grid");
    load_log();
    if (xTaskCreateStatic(qso_task, "fmo_qso",
                          sizeof(s_task_stack) / sizeof(s_task_stack[0]),
                          NULL, 4, s_task_stack, &s_task_tcb) == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "service ready: callsign=%s", s_callsign);
    return ESP_OK;
}

void fmo_qso_update_config(const fmo_config_t *config)
{
    if (config == NULL || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    base_call(config->fmo_callsign, s_callsign, sizeof(s_callsign));
    strlcpy(s_station_name, config->fmo_station_name, sizeof(s_station_name));
    strlcpy(s_qso_msg, config->fmo_qso_msg, sizeof(s_qso_msg));
    s_freq_x10000 = config->fmo_freq_x10000;
    s_position_set = config->aprs_position_set;
    s_lat_e6 = config->aprs_latitude_e6;
    s_lon_e6 = config->aprs_longitude_e6;
    xSemaphoreGive(s_lock);
}

void fmo_qso_get_status(fmo_qso_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->phase = s_phase;
    status->outgoing = s_outgoing;
    status->incoming = s_phase == FMO_QSO_PHASE_INCOMING;
    strlcpy(status->peer, s_peer, sizeof(status->peer));
    status->peer_uid = s_peer_uid;
    strlcpy(status->detail, s_detail, sizeof(status->detail));
    xSemaphoreGive(s_lock);
}

size_t fmo_qso_get_log(fmo_qso_log_entry_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0 || s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_log_count < capacity ? s_log_count : capacity;
    for (size_t i = 0; i < count; ++i) {
        out[i] = s_log[s_log_count - 1 - i];  /* newest first */
    }
    xSemaphoreGive(s_lock);
    return count;
}
