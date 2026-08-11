#include "aprs_service.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "version.h"
#include "lwip/sockets.h"
#include "audio/aprs_afsk.h"
#include "esp_attr.h"
#include "services/app_notice.h"
#include "services/network_manager.h"

#define APRS_RECONNECT_MS 30000U
#define APRS_LOOP_MS 100U
#define APRS_RECENT_STATIONS 8U

static const char *TAG = "aprs";
static SemaphoreHandle_t s_lock;
static fmo_config_t s_config;
static uint32_t s_generation;
static bool s_connected;
static bool s_send_now;
static uint32_t s_rx_count;
static uint32_t s_tx_count;
static char s_last_source[16];
static char s_last_text[96];
static aprs_recent_packet_t s_recent[APRS_RECENT_STATIONS];
static size_t s_recent_count;
/* APRS only uses sockets and RAM once running, so its stack can live in
 * PSRAM.  Keeping it out of internal DRAM is important after Wi-Fi, the web
 * portal, and the AFSK decoder have started on the 2 MB-PSRAM target. */
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[8192 / sizeof(StackType_t)];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool aprs_digits(const char *text, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!isdigit((unsigned char)text[i])) return false;
    }
    return true;
}

static bool parse_uncompressed_position(const char *position, double *latitude,
                                        double *longitude, float *speed_kmh,
                                        bool *has_speed, float *course_deg,
                                        bool *has_course)
{
    if (strlen(position) < 19 || position[4] != '.' || position[14] != '.' ||
        !aprs_digits(position, 4) || !aprs_digits(position + 5, 2) ||
        !aprs_digits(position + 9, 5) || !aprs_digits(position + 15, 2)) {
        return false;
    }
    const char lat_hemi = (char)toupper((unsigned char)position[7]);
    const char lon_hemi = (char)toupper((unsigned char)position[17]);
    if ((lat_hemi != 'N' && lat_hemi != 'S') ||
        (lon_hemi != 'E' && lon_hemi != 'W')) return false;

    const int lat_degrees = (position[0] - '0') * 10 + position[1] - '0';
    const double lat_minutes = (position[2] - '0') * 10 + position[3] - '0' +
                               (position[5] - '0') / 10.0 +
                               (position[6] - '0') / 100.0;
    const int lon_degrees = (position[9] - '0') * 100 +
                            (position[10] - '0') * 10 + position[11] - '0';
    const double lon_minutes = (position[12] - '0') * 10 + position[13] - '0' +
                               (position[15] - '0') / 10.0 +
                               (position[16] - '0') / 100.0;
    if (lat_degrees > 90 || lon_degrees > 180 ||
        lat_minutes >= 60.0 || lon_minutes >= 60.0) return false;
    *latitude = lat_degrees + lat_minutes / 60.0;
    *longitude = lon_degrees + lon_minutes / 60.0;
    if (lat_hemi == 'S') *latitude = -*latitude;
    if (lon_hemi == 'W') *longitude = -*longitude;

    const char *extension = position + 19;
    *has_speed = strlen(extension) >= 7 && extension[3] == '/' &&
                 aprs_digits(extension, 3) && aprs_digits(extension + 4, 3);
    if (*has_speed) {
        const int course = (extension[0] - '0') * 100 +
                           (extension[1] - '0') * 10 + extension[2] - '0';
        const int knots = (extension[4] - '0') * 100 +
                          (extension[5] - '0') * 10 + extension[6] - '0';
        *speed_kmh = knots * 1.852f;
        *course_deg = (float)course;
        *has_course = true;
    }
    return true;
}

static bool parse_compressed_position(const char *position, double *latitude,
                                      double *longitude, float *speed_kmh,
                                      bool *has_speed, float *course_deg,
                                      bool *has_course)
{
    if (strlen(position) < 13) return false;
    uint32_t lat_code = 0;
    uint32_t lon_code = 0;
    for (int i = 1; i <= 8; ++i) {
        unsigned char value = (unsigned char)position[i];
        if (value < 33 || value > 123) return false;
        if (i <= 4) lat_code = lat_code * 91U + (uint32_t)(value - 33);
        else lon_code = lon_code * 91U + (uint32_t)(value - 33);
    }
    *latitude = 90.0 - lat_code / 380926.0;
    *longitude = -180.0 + lon_code / 190463.0;

    const int course_code = (unsigned char)position[10] - 33;
    const int speed_code = (unsigned char)position[11] - 33;
    *has_speed = course_code >= 0 && course_code <= 89 &&
                 speed_code >= 0 && speed_code <= 89;
    if (*has_speed) {
        *speed_kmh = (float)((pow(1.08, speed_code) - 1.0) * 1.852);
        *course_deg = (float)(course_code * 4);
        *has_course = true;
    }
    return isfinite(*latitude) && isfinite(*longitude) &&
           *latitude >= -90.0 && *latitude <= 90.0 &&
           *longitude >= -180.0 && *longitude <= 180.0;
}

static bool parse_packet_position(const char *text, double *latitude,
                                  double *longitude, float *speed_kmh,
                                  bool *has_speed, float *course_deg,
                                  bool *has_course)
{
    if (text == NULL || text[0] == '\0') return false;
    const char *position = NULL;
    if (text[0] == '!' || text[0] == '=') {
        position = text + 1;
    } else if ((text[0] == '/' || text[0] == '@') && strlen(text) > 8) {
        position = text + 8;
    } else {
        return false;
    }
    *has_speed = false;
    *speed_kmh = 0.0f;
    *has_course = false;
    *course_deg = 0.0f;
    if (isdigit((unsigned char)position[0])) {
        return parse_uncompressed_position(position, latitude, longitude,
                                           speed_kmh, has_speed,
                                           course_deg, has_course);
    }
    return parse_compressed_position(position, latitude, longitude,
                                     speed_kmh, has_speed,
                                     course_deg, has_course);
}

static float distance_km(double lat1, double lon1, double lat2, double lon2)
{
    const double radians = M_PI / 180.0;
    const double dlat = (lat2 - lat1) * radians;
    const double dlon = (lon2 - lon1) * radians;
    const double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
                     cos(lat1 * radians) * cos(lat2 * radians) *
                     sin(dlon / 2.0) * sin(dlon / 2.0);
    const double bounded = a > 1.0 ? 1.0 : (a < 0.0 ? 0.0 : a);
    return (float)(6371.0 * 2.0 * atan2(sqrt(bounded), sqrt(1.0 - bounded)));
}

static void store_recent_locked(const aprs_recent_packet_t *packet)
{
    size_t existing = s_recent_count;
    for (size_t i = 0; i < s_recent_count; ++i) {
        if (strcmp(s_recent[i].callsign, packet->callsign) == 0) {
            existing = i;
            break;
        }
    }
    if (existing == s_recent_count && s_recent_count < APRS_RECENT_STATIONS) {
        ++s_recent_count;
    } else if (existing == s_recent_count) {
        existing = APRS_RECENT_STATIONS - 1;
    }
    for (size_t i = existing; i > 0; --i) s_recent[i] = s_recent[i - 1];
    s_recent[0] = *packet;
}

bool aprs_service_parse_coordinate(const char *text, bool latitude,
                                   int32_t *microdegrees)
{
    if (text == NULL || microdegrees == NULL) return false;
    while (isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) --length;
    if (length == 0 || length >= 32) return false;

    char input[32];
    memcpy(input, text, length);
    input[length] = '\0';

    char hemisphere = '\0';
    char tail = (char)toupper((unsigned char)input[length - 1]);
    if (tail == 'N' || tail == 'S' || tail == 'E' || tail == 'W') {
        if ((latitude && tail != 'N' && tail != 'S') ||
            (!latitude && tail != 'E' && tail != 'W')) {
            return false;
        }
        hemisphere = tail;
        input[--length] = '\0';
        while (length > 0 && isspace((unsigned char)input[length - 1])) {
            input[--length] = '\0';
        }
        if (length == 0) return false;
    }

    char *end = NULL;
    double encoded = strtod(input, &end);
    if (end == input || !isfinite(encoded)) return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end != '\0') return false;

    bool negative = signbit(encoded);
    double magnitude = fabs(encoded);
    const double dm_threshold = latitude ? 100.0 : 1000.0;
    double degrees;
    if (magnitude >= dm_threshold) {
        degrees = floor(magnitude / 100.0);
        double minutes = magnitude - degrees * 100.0;
        if (minutes < 0.0 || minutes >= 60.0) return false;
        degrees += minutes / 60.0;
    } else {
        degrees = magnitude;
    }

    const double limit = latitude ? 90.0 : 180.0;
    if (degrees > limit) return false;
    if (hemisphere != '\0') {
        negative = hemisphere == 'S' || hemisphere == 'W';
    }
    if (negative) degrees = -degrees;
    long long scaled = llround(degrees * 1000000.0);
    const long long scaled_limit = latitude ? 90000000LL : 180000000LL;
    if (scaled < -scaled_limit || scaled > scaled_limit) return false;
    *microdegrees = (int32_t)scaled;
    return true;
}

static void own_callsign(const fmo_config_t *config, char *out, size_t size)
{
    char base[8] = {0};
    size_t used = 0;
    for (const char *p = config->callsign; *p && used < 6; ++p) {
        if (*p == '-') break;
        if (isalnum((unsigned char)*p)) base[used++] = (char)toupper((unsigned char)*p);
    }
    if (used == 0) strlcpy(base, "NOCALL", sizeof(base));
    if (config->aprs_ssid > 0) {
        snprintf(out, size, "%s-%u", base, (unsigned)config->aprs_ssid);
    } else {
        strlcpy(out, base, size);
    }
}

static uint16_t aprs_passcode(const char *callsign)
{
    uint16_t hash = 0x73e2;
    char root[10] = {0};
    size_t length = 0;
    while (callsign[length] && callsign[length] != '-' && length + 1 < sizeof(root)) {
        root[length] = (char)toupper((unsigned char)callsign[length]);
        ++length;
    }
    for (size_t i = 0; i < length; i += 2) {
        hash ^= (uint16_t)((uint8_t)root[i] << 8);
        if (i + 1 < length) hash ^= (uint8_t)root[i + 1];
    }
    return hash & 0x7fff;
}

static void format_coord(int32_t value_e6, bool latitude, char *out, size_t size)
{
    double value = (double)value_e6 / 1000000.0;
    char hemi = latitude ? (value < 0 ? 'S' : 'N') : (value < 0 ? 'W' : 'E');
    value = fabs(value);
    int degrees = (int)value;
    double minutes = (value - degrees) * 60.0;
    if (minutes >= 59.99995) {  /* avoid "60.0000" after rounding */
        ++degrees;
        minutes = 0.0;
    }
    snprintf(out, size, latitude ? "%02d%07.4f%c" : "%03d%07.4f%c",
             degrees, minutes, hemi);
}

static bool send_line(int fd, const char *line)
{
    size_t length = strlen(line);
    char wire[420];
    if (length + 3 > sizeof(wire)) return false;
    memcpy(wire, line, length);
    wire[length++] = '\r';
    wire[length++] = '\n';
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, wire + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* AFSK gateway helpers (ported from NRL-ESP32)                        */
/* ------------------------------------------------------------------ */

/* Pacing for relayed AFSK frames so a busy APRS-IS feed or a chattering
 * RF channel cannot peg the modulator (beacons share it). */
#define FWD_AFSK_MIN_GAP_MS 1000U
static uint32_t s_last_fwd_afsk_ms;

static bool fwd_gap_ok(void)
{
    const uint32_t now = now_ms();
    if (now - s_last_fwd_afsk_ms < FWD_AFSK_MIN_GAP_MS) return false;
    s_last_fwd_afsk_ms = now;
    return true;
}

/* True when the TNC2 line was originated by this station (our own
 * callsign-SSID right before '>'); such packets must never be relayed
 * back towards any channel or the gateway loops on its own traffic. */
static bool fwd_is_own_source(const char *line, const char *call)
{
    const size_t n = strlen(call);
    return strncmp(line, call, n) == 0 && line[n] == '>';
}

/* True when the header path contains an APRS-IS q construct (",qAR,"
 * etc.): the packet already passed a gate and must not be gated to IS
 * again. */
static bool fwd_has_q_construct(const char *line)
{
    const char *colon = strchr(line, ':');
    const size_t head_len =
        (colon != NULL) ? (size_t)(colon - line) : strlen(line);
    for (size_t i = 1; i + 2 < head_len; ++i) {
        if (line[i] == ',' && line[i + 1] == 'q' &&
            isalpha((unsigned char)line[i + 2])) {
            return true;
        }
    }
    return false;
}

/* True when the header path already lists `call` as a digipeater. Stops
 * RF<->NRL ping-pong between two gateway nodes: each hop appends its own
 * callsign when relaying, so a returning copy is recognized and dropped. */
static bool fwd_path_contains_call(const char *line, const char *call)
{
    const char *gt = strchr(line, '>');
    const char *colon = strchr(line, ':');
    if (gt == NULL || colon == NULL || colon <= gt) return false;
    const size_t call_len = strlen(call);
    for (const char *p = gt + 1; p < colon; ++p) {
        if (*p != ',') continue;
        if ((size_t)(colon - (p + 1)) >= call_len &&
            strncmp(p + 1, call, call_len) == 0) {
            const char after = p[1 + call_len];
            if (after == ',' || after == ':' || after == '*') return true;
        }
    }
    return false;
}

/* Rebuild a TNC2 header stripping APRS-IS-only digipeaters (q constructs,
 * TCPIP) so the frame is legal on an AFSK channel. */
static bool fwd_sanitize_header(const char *line, char *out, size_t out_size)
{
    const char *colon = strchr(line, ':');
    const char *gt = strchr(line, '>');
    if (colon == NULL || gt == NULL || colon <= gt) return false;
    size_t used = 0;
    const size_t prefix_len = (size_t)(gt - line) + 1u;  /* includes '>' */
    if (prefix_len >= out_size) return false;
    memcpy(out, line, prefix_len);
    used = prefix_len;
    out[used] = '\0';
    const char *p = gt + 1;
    while (p < colon) {
        const char *comma = strchr(p, ',');
        const char *end = (comma != NULL && comma < colon) ? comma : colon;
        const size_t tok_len = (size_t)(end - p);
        char tok[12] = {0};
        const bool is_q = tok_len >= 2 && p[0] == 'q' &&
                          isalpha((unsigned char)p[1]);
        bool is_tcpip = false;
        if (tok_len == 5 && strncasecmp(p, "TCPIP", 5) == 0) is_tcpip = true;
        if (!is_q && !is_tcpip && tok_len > 0 && tok_len < sizeof(tok)) {
            memcpy(tok, p, tok_len);
            if (tok_len > 1 && tok[tok_len - 1] == '*') {
                tok[tok_len - 1] = '\0';  /* heard marker re-added below */
            }
            if (used + 1 + strlen(tok) + 1 < out_size) {
                out[used++] = ',';
                memcpy(out + used, tok, strlen(tok));
                used += strlen(tok);
                out[used] = '\0';
            }
        }
        p = (comma != NULL && comma < colon) ? comma + 1 : colon;
    }
    if (used + strlen(colon) + 1 > out_size) return false;
    memcpy(out + used, colon, strlen(colon) + 1);
    return true;
}

/* Append our callsign to the path so other gateway nodes can detect the
 * hop and break RF<->NRL loops. */
static bool fwd_append_own_digi(char *txt, size_t size, const char *call)
{
    char *colon = strchr(txt, ':');
    if (colon == NULL || strlen(txt) + strlen(call) + 1 >= size) return false;
    memmove(colon + strlen(call) + 1, colon, strlen(colon) + 1);
    *colon = ',';
    memcpy(colon + 1, call, strlen(call));
    return true;
}

/* Upload a locally heard packet to APRS-IS with the qAR construct (iGate). */
static bool fwd_to_is(int fd, const char *line, const char *call)
{
    const char *colon = strchr(line, ':');
    if (colon == NULL || fd < 0) return false;
    char fwd[440];
    snprintf(fwd, sizeof(fwd), "%.*s,qAR,%s%s",
             (int)(colon - line), line, call, colon);
    return send_line(fd, fwd);
}

/* Inject a TNC2 line into the AFSK TX queue (out over whatever routes are
 * enabled: radio speaker and/or NRL uplink). `sanitize` strips IS-only
 * header entries first; `add_own_digi` appends our callsign to the path so
 * other gateway nodes can detect the hop and break RF<->NRL loops. */
static bool fwd_to_afsk(const char *line, bool sanitize, bool add_own_digi,
                        const char *call)
{
    if (!fwd_gap_ok()) return false;
    char txt[400];
    if (sanitize) {
        if (!fwd_sanitize_header(line, txt, sizeof(txt))) return false;
    } else {
        snprintf(txt, sizeof(txt), "%s", line);
    }
    if (add_own_digi) fwd_append_own_digi(txt, sizeof(txt), call);
    return aprs_afsk_send_line(txt);
}

static int connect_is(const fmo_config_t *config)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)config->aprs_server_port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    int error = getaddrinfo(config->aprs_server_host, port, &hints, &result);
    if (error != 0 || result == NULL) {
        ESP_LOGW(TAG, "DNS failed for %s", config->aprs_server_host);
        if (result != NULL) freeaddrinfo(result);
        return -1;
    }
    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return -1;
    }
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect %s:%u failed errno=%d", config->aprs_server_host,
                 (unsigned)config->aprs_server_port, errno);
        close(fd);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);

    char call[16];
    own_callsign(config, call, sizeof(call));
    char login[180];
    if (config->aprs_position_set) {
        snprintf(login, sizeof(login),
                 "user %s pass %u vers OpenFMO 1.0 filter m/100", call,
                 (unsigned)aprs_passcode(call));
    } else {
        snprintf(login, sizeof(login), "user %s pass %u vers OpenFMO 1.0", call,
                 (unsigned)aprs_passcode(call));
    }
    if (!send_line(fd, login)) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "APRS-IS connected: %s:%u as %s fixed=%d", 
             config->aprs_server_host, (unsigned)config->aprs_server_port,
             call, config->aprs_position_set ? 1 : 0);
    return fd;
}

static bool send_beacon(int fd, const fmo_config_t *config)
{
    if (!config->aprs_position_set) {
        ESP_LOGW(TAG, "beacon blocked: fixed position is not configured");
        return false;
    }
    char call[16], lat[12], lon[13], line[360];
    own_callsign(config, call, sizeof(call));
    format_coord(config->aprs_latitude_e6, true, lat, sizeof(lat));
    format_coord(config->aprs_longitude_e6, false, lon, sizeof(lon));
    /* Position beacon with NRL server info in comment (matches reference) */
    char comment[256];
    snprintf(comment, sizeof(comment), "@udp://%s:%u%s%s",
             config->nrl_host, (unsigned)config->nrl_port,
             config->aprs_comment[0] ? " " : "", config->aprs_comment);
    snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:!%s/%sI%s", call, lat, lon,
             comment);
    if (!send_line(fd, line)) return false;
    /* Mirror the beacon onto the enabled AFSK channels (header gets
     * sanitized of the IS-only TCPIP entry). */
    if (config->aprs_rf_tx || config->aprs_nrl_tx) {
        fwd_to_afsk(line, true, false, call);
    }
    /* Status line: firmware/board identity (matches reference NRL-ESP32 format) */
    snprintf(line, sizeof(line), "%s>NRLBOX,TCPIP*:>NRL-ESP32,%s,v%s,0.00v",
             call, FMO_BOARD_TYPE, FMO_FIRMWARE_VERSION);
    send_line(fd, line);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_tx_count;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "fixed-position beacon sent: %s", call);
    return true;
}

/* origin: 0 = APRS-IS, otherwise AFSK RX with `afsk_source` =
 * APRS_AFSK_SOURCE_*. `is_fd` is the live APRS-IS socket (or -1). */
static void handle_line(const char *line, bool from_afsk, uint8_t afsk_source,
                        int is_fd)
{
    if (line == NULL || line[0] == '\0' || line[0] == '#') return;
    const char *gt = strchr(line, '>');
    const char *colon = strchr(line, ':');
    if (gt == NULL || colon == NULL || gt >= colon) return;
    char source[16];
    size_t source_len = (size_t)(gt - line);
    if (source_len >= sizeof(source)) source_len = sizeof(source) - 1;
    memcpy(source, line, source_len);
    source[source_len] = '\0';
    const char *text = colon + 1;
    char summary[96];
    strlcpy(summary, text, sizeof(summary));

    aprs_recent_packet_t packet = {0};
    strlcpy(packet.callsign, source, sizeof(packet.callsign));
    packet.received_ms = now_ms();
    double remote_latitude = 0.0;
    double remote_longitude = 0.0;
    float speed_kmh = 0.0f;
    float course_deg = 0.0f;
    bool has_speed = false;
    bool has_course = false;
    const bool has_position = parse_packet_position(
        text, &remote_latitude, &remote_longitude, &speed_kmh, &has_speed,
        &course_deg, &has_course);

    /* Build a concise notice: strip coordinates from position reports */
    char notice_text[96];
    if (has_position) {
        const char *p = text;
        if (*p == '!' || *p == '=') ++p;
        else if (*p == '/' || *p == '@') p += 8;
        if (isdigit((unsigned char)p[0])) {
            /* Uncompressed: 19 chars position */
            const char *ext = p + 19;
            if (strlen(ext) >= 7 && ext[3] == '/' &&
                aprs_digits(ext, 3) && aprs_digits(ext + 4, 3)) {
                ext += 7;  /* skip course/speed */
            }
            if (strncmp(ext, "/A=", 3) == 0) {
                ext += 3;
                while (isdigit((unsigned char)*ext) || *ext == '-') ++ext;
            }
            while (*ext == ' ' || *ext == '[' || *ext == ']') ++ext;
            if (*ext != '\0') strlcpy(notice_text, ext, sizeof(notice_text));
            else snprintf(notice_text, sizeof(notice_text), "%.0fkm %.0f\xc2\xb0",
                          (double)packet.distance_km, (double)course_deg);
        } else {
            /* Compressed: 13 chars */
            const char *ext = p + 13;
            while (*ext == ' ' || *ext == '[' || *ext == ']') ++ext;
            if (*ext != '\0') strlcpy(notice_text, ext, sizeof(notice_text));
            else strlcpy(notice_text, "position", sizeof(notice_text));
        }
    } else {
        strlcpy(notice_text, text, sizeof(notice_text));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_rx_count;
    strlcpy(s_last_source, source, sizeof(s_last_source));
    strlcpy(s_last_text, summary, sizeof(s_last_text));
    if (has_position && s_config.aprs_position_set) {
        packet.distance_km = distance_km(
            (double)s_config.aprs_latitude_e6 / 1000000.0,
            (double)s_config.aprs_longitude_e6 / 1000000.0,
            remote_latitude, remote_longitude);
        packet.has_distance = true;
    }
    packet.has_speed = has_speed;
    packet.speed_kmh = speed_kmh;
    packet.has_course = has_course;
    packet.course_deg = course_deg;
    store_recent_locked(&packet);
    xSemaphoreGive(s_lock);
    app_notice_post_aprs(source, notice_text);
    ESP_LOGI(TAG, "RX %s%s: %s", from_afsk
                 ? (afsk_source == APRS_AFSK_SOURCE_NRL ? "NRL " : "RF ") : "",
             source, summary);

    /* Gateway forwarding (mirrors the NRL-ESP32 reference). Own traffic
     * and already-gated packets (q constructs) are never re-gated to IS;
     * RF<->NRL hops append our callsign so returning copies are dropped. */
    fmo_config_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg = s_config;
    xSemaphoreGive(s_lock);
    char fwd_call[16];
    own_callsign(&cfg, fwd_call, sizeof(fwd_call));
    if (from_afsk) {
        const bool via_nrl = afsk_source == APRS_AFSK_SOURCE_NRL;
        if (fwd_is_own_source(line, fwd_call)) return;
        if ((cfg.aprs_fwd & (via_nrl ? FMO_APRS_FWD_NRL_TO_IS
                                     : FMO_APRS_FWD_RF_TO_IS)) &&
            is_fd >= 0 && cfg.aprs_enabled &&
            !fwd_has_q_construct(line)) {
            fwd_to_is(is_fd, line, fwd_call);
        }
        /* AFSK relay only makes sense when the destination transmitter is
         * on: RF->NRL needs the NRL uplink, NRL->RF the speaker route. */
        if ((cfg.aprs_fwd & (via_nrl ? FMO_APRS_FWD_NRL_TO_RF
                                     : FMO_APRS_FWD_RF_TO_NRL)) &&
            (via_nrl ? cfg.aprs_rf_tx : cfg.aprs_nrl_tx) &&
            !fwd_path_contains_call(line, fwd_call)) {
            fwd_to_afsk(line, false, true, fwd_call);
        }
    } else {
        /* Relay IS traffic onto the AFSK channels; each direction also
         * needs its transmitter route on. */
        const bool is_to_afsk =
            ((cfg.aprs_fwd & FMO_APRS_FWD_IS_TO_RF) && cfg.aprs_rf_tx) ||
            ((cfg.aprs_fwd & FMO_APRS_FWD_IS_TO_NRL) && cfg.aprs_nrl_tx);
        if (is_to_afsk && !fwd_is_own_source(line, fwd_call)) {
            fwd_to_afsk(line, true, false, fwd_call);
        }
    }
}

static void aprs_task(void *argument)
{
    (void)argument;
    int fd = -1;
    uint32_t active_generation = 0;
    uint32_t last_connect_attempt = 0;
    uint32_t last_beacon = 0;
    char rx_line[600];
    size_t rx_used = 0;

    for (;;) {
        fmo_config_t config;
        uint32_t generation;
        bool send_now;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        config = s_config;
        generation = s_generation;
        send_now = s_send_now;
        s_send_now = false;
        xSemaphoreGive(s_lock);
        network_status_t network = {0};
        network_manager_get_status(&network);

        if (generation != active_generation && fd >= 0) {
            close(fd);
            fd = -1;
            rx_used = 0;
        }
        active_generation = generation;
        const bool should_run = config.aprs_enabled && network.station_connected;
        if (!should_run && fd >= 0) {
            close(fd);
            fd = -1;
            rx_used = 0;
        }

        uint32_t now = now_ms();
        if (should_run && fd < 0 &&
            (last_connect_attempt == 0 || now - last_connect_attempt >= APRS_RECONNECT_MS)) {
            last_connect_attempt = now;
            fd = connect_is(&config);
            if (fd >= 0) {
                last_beacon = 0;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_connected = true;
                xSemaphoreGive(s_lock);
            }
        }

        if (fd >= 0) {
            uint32_t interval_ms = (uint32_t)config.aprs_beacon_interval_s * 1000U;
            if ((send_now || (config.aprs_position_set &&
                 (last_beacon == 0 || now - last_beacon >= interval_ms)))) {
                if (send_beacon(fd, &config)) last_beacon = now;
            }
            char buffer[256];
            for (;;) {
                int n = recv(fd, buffer, sizeof(buffer), 0);
                if (n == 0 || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
                    ESP_LOGW(TAG, "APRS-IS disconnected errno=%d", n < 0 ? errno : 0);
                    close(fd);
                    fd = -1;
                    rx_used = 0;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_connected = false;
                    xSemaphoreGive(s_lock);
                    break;
                }
                if (n < 0) break;
                for (int i = 0; i < n; ++i) {
                    char c = buffer[i];
                    if (c == '\r' || c == '\n') {
                        if (rx_used > 0) {
                            rx_line[rx_used] = '\0';
                            handle_line(rx_line, false, 0, fd);
                            rx_used = 0;
                        }
                    } else if (rx_used + 1 < sizeof(rx_line)) {
                        rx_line[rx_used++] = c;
                    } else {
                        rx_used = 0;
                    }
                }
            }
        }
        /* AFSK RX: decoded frames from the radio mic (RF) and NRL
         * downlink taps. */
        char afsk_line[400];
        uint8_t afsk_source = 0;
        while (aprs_afsk_get_rx_frame(afsk_line, sizeof(afsk_line),
                                      &afsk_source)) {
            handle_line(afsk_line, true, afsk_source, fd);
        }
        if (fd < 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_connected = false;
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(APRS_LOOP_MS));
    }
}

esp_err_t aprs_service_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    int32_t decimal_lat = 0, dm_lat = 0, decimal_lon = 0, dm_lon = 0;
    if (!aprs_service_parse_coordinate("31.8885", true, &decimal_lat) ||
        !aprs_service_parse_coordinate("3153.3100N", true, &dm_lat) ||
        !aprs_service_parse_coordinate("118.8141", false, &decimal_lon) ||
        !aprs_service_parse_coordinate("11848.8460E", false, &dm_lon) ||
        decimal_lat != dm_lat || decimal_lon != dm_lon ||
        aprs_service_parse_coordinate("3160.0000N", true, &dm_lat)) {
        ESP_LOGE(TAG, "coordinate parser self-test failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "coordinate parser ready: decimal + ddmm.mmmm WGS-84");
    if (aprs_afsk_init() == ESP_OK) {
        aprs_afsk_set_rx_routes(config->aprs_rf_rx, config->aprs_nrl_rx);
        aprs_afsk_set_tx_routes(config->aprs_rf_tx, config->aprs_nrl_tx);
    }
    if (s_lock != NULL) {
        aprs_service_update_config(config);
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_config = *config;
    s_generation = 1;
    if (xTaskCreateStatic(aprs_task, "aprs",
                          sizeof(s_task_stack) / sizeof(s_task_stack[0]),
                          NULL, 4, s_task_stack, &s_task_tcb) == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "service ready: enabled=%d fixed=%d server=%s:%u", 
             config->aprs_enabled ? 1 : 0, config->aprs_position_set ? 1 : 0,
             config->aprs_server_host, (unsigned)config->aprs_server_port);
    return ESP_OK;
}

void aprs_service_update_config(const fmo_config_t *config)
{
    if (config == NULL || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = *config;
    ++s_generation;
    xSemaphoreGive(s_lock);
    aprs_afsk_set_rx_routes(config->aprs_rf_rx, config->aprs_nrl_rx);
    aprs_afsk_set_tx_routes(config->aprs_rf_tx, config->aprs_nrl_tx);
}

bool aprs_service_set_enabled(bool enabled)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config.aprs_enabled = enabled;
    ++s_generation;
    xSemaphoreGive(s_lock);
    return true;
}


void aprs_service_get_status(aprs_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->enabled = s_config.aprs_enabled;
    status->position_set = s_config.aprs_position_set;
    status->connected = s_connected;
    status->rx_count = s_rx_count;
    status->tx_count = s_tx_count;
    strlcpy(status->last_source, s_last_source, sizeof(status->last_source));
    strlcpy(status->last_text, s_last_text, sizeof(status->last_text));
    xSemaphoreGive(s_lock);
}

size_t aprs_service_get_recent(aprs_recent_packet_t *packets, size_t capacity)
{
    if (packets == NULL || capacity == 0 || s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t count = s_recent_count < capacity ? s_recent_count : capacity;
    memcpy(packets, s_recent, count * sizeof(*packets));
    xSemaphoreGive(s_lock);
    return count;
}

bool aprs_service_send_beacon_now(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_config.aprs_enabled && s_config.aprs_position_set;
    if (allowed) s_send_now = true;
    xSemaphoreGive(s_lock);
    return allowed;
}
