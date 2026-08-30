#include "fmo_aprs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ff.h"
#include "services/fmo_protocol.h"

#define FMO_APRS_LINE_MAX 800
#define FMO_APRS_TOKEN_MAX 24

static bool parse_uint(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

bool fmo_aprs_parse_source_ssid(const char *line, size_t line_size,
                                char callsign[7], uint8_t *ssid)
{
    if (line == NULL || callsign == NULL || ssid == NULL || line_size == 0 ||
        line_size >= FMO_APRS_LINE_MAX) return false;
    const char *gt = memchr(line, '>', line_size);
    if (gt == NULL || gt == line || (size_t)(gt - line) > 9 ||
        (size_t)(line + line_size - gt) < 6 ||
        memcmp(gt + 1, "APFMO", 5) != 0) return false;

    const char *dash = memchr(line, '-', (size_t)(gt - line));
    size_t base_size = dash == NULL ? (size_t)(gt - line)
                                    : (size_t)(dash - line);
    if (base_size == 0 || base_size > 6) return false;
    for (size_t i = 0; i < base_size; ++i) {
        unsigned char ch = (unsigned char)line[i];
        if (!isalnum(ch)) return false;
        callsign[i] = (char)toupper(ch);
    }
    callsign[base_size] = '\0';

    uint32_t parsed_ssid = 0;
    if (dash != NULL) {
        if (dash + 1 == gt) return false;
        for (const char *p = dash + 1; p < gt; ++p) {
            if (!isdigit((unsigned char)*p)) return false;
            parsed_ssid = parsed_ssid * 10U + (uint32_t)(*p - '0');
            if (parsed_ssid > 15) return false;
        }
    }
    *ssid = (uint8_t)parsed_ssid;
    return true;
}

static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 0x80) { ++p; continue; }
        unsigned needed;
        uint32_t code;
        if ((*p & 0xe0) == 0xc0) { needed = 1; code = *p & 0x1f; }
        else if ((*p & 0xf0) == 0xe0) { needed = 2; code = *p & 0x0f; }
        else if ((*p & 0xf8) == 0xf0) { needed = 3; code = *p & 0x07; }
        else return false;
        ++p;
        for (unsigned i = 0; i < needed; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) return false;
            code = (code << 6) | (*p & 0x3f);
        }
        if ((needed == 1 && code < 0x80) ||
            (needed == 2 && code < 0x800) ||
            (needed == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

static bool gbk_to_utf8(const char *input, char *output, size_t output_size)
{
    const uint8_t *source = (const uint8_t *)input;
    size_t used = 0;
    while (*source != 0) {
        uint32_t codepoint;
        if (*source < 0x80) {
            codepoint = *source++;
        } else {
            if (source[1] == 0) return false;
            WCHAR gbk = (WCHAR)(((uint16_t)source[0] << 8) | source[1]);
            codepoint = ff_oem2uni(gbk, 936);
            source += 2;
            if (codepoint == 0) return false;
        }
        size_t required = codepoint < 0x80 ? 1 :
                          codepoint < 0x800 ? 2 : 3;
        if (used + required + 1 > output_size) return false;
        if (required == 1) {
            output[used++] = (char)codepoint;
        } else if (required == 2) {
            output[used++] = (char)(0xc0 | (codepoint >> 6));
            output[used++] = (char)(0x80 | (codepoint & 0x3f));
        } else {
            output[used++] = (char)(0xe0 | (codepoint >> 12));
            output[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
            output[used++] = (char)(0x80 | (codepoint & 0x3f));
        }
    }
    output[used] = '\0';
    return used > 0;
}

bool fmo_aprs_utf8_to_gbk(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0) return false;
    const uint8_t *source = (const uint8_t *)input;
    size_t used = 0;
    while (*source != 0) {
        uint32_t codepoint;
        if (*source < 0x80) {
            codepoint = *source++;
        } else {
            unsigned needed;
            if ((*source & 0xe0) == 0xc0) {
                needed = 1;
                codepoint = *source & 0x1f;
            } else if ((*source & 0xf0) == 0xe0) {
                needed = 2;
                codepoint = *source & 0x0f;
            } else {
                return false;  /* 4-byte UTF-8 has no CP936 mapping */
            }
            ++source;
            for (unsigned i = 0; i < needed; ++i, ++source) {
                if ((*source & 0xc0) != 0x80) return false;
                codepoint = (codepoint << 6) | (*source & 0x3f);
            }
            if ((needed == 1 && codepoint < 0x80) ||
                (needed == 2 && codepoint < 0x800)) return false;
        }
        if (codepoint < 0x80) {
            if (used + 2 > output_size) return false;
            output[used++] = (char)codepoint;
            continue;
        }
        DWORD oem = ff_uni2oem(codepoint, 936);
        if (oem == 0 || oem > 0xffffu) return false;
        const size_t required = oem < 0x100 ? 1 : 2;
        if (used + required + 1 > output_size) return false;
        if (required == 1) {
            output[used++] = (char)oem;
        } else {
            /* FatFS returns DBCS codes with the lead byte high, matching the
             * (source[0] << 8) | source[1] form ff_oem2uni() accepts. */
            output[used++] = (char)(oem >> 8);
            output[used++] = (char)(oem & 0xff);
        }
    }
    output[used] = '\0';
    return used > 0;
}

static bool looks_like_host(const char *token)
{
    /* FMO structured fields such as SIG:<base64url>, CERT:<base64url> and
     * FREQ:<number> contain a colon.  Treating ':' as a generic hostname
     * character allowed the trailing signature to overwrite the real MQTT
     * host parsed earlier in the STATION packet.  Current FMO STATION
     * beacons advertise DNS names or IPv4 addresses (both contain a dot);
     * legacy SH:<host> is handled separately by the caller. */
    if (token == NULL || token[0] == '\0' || strchr(token, '.') == NULL ||
        strchr(token, ':') != NULL) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p) {
        if (!(isalnum(*p) || *p == '.' || *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

bool fmo_aprs_parse_station(const char *line, size_t line_size,
                            fmo_server_t *server)
{
    if (line == NULL || server == NULL || line_size == 0 ||
        line_size >= FMO_APRS_LINE_MAX) return false;
    char copy[FMO_APRS_LINE_MAX];
    memcpy(copy, line, line_size);
    copy[line_size] = '\0';
    char *gt = strchr(copy, '>');
    char *colon = strchr(copy, ':');
    if (gt == NULL || colon == NULL || gt >= colon) return false;
    char source[16] = {0};
    size_t source_size = (size_t)(gt - copy);
    if (source_size >= sizeof(source)) source_size = sizeof(source) - 1;
    memcpy(source, copy, source_size);
    char *marker = strstr(colon + 1, "FMO-V4,STATION,");
    if (marker == NULL) return false;
    marker += strlen("FMO-V4,STATION,");

    char *tokens[FMO_APRS_TOKEN_MAX];
    size_t count = 0;
    char *cursor = marker;
    while (cursor != NULL && count < FMO_APRS_TOKEN_MAX) {
        tokens[count++] = cursor;
        char *comma = strchr(cursor, ',');
        if (comma == NULL) break;
        *comma = '\0';
        cursor = comma + 1;
    }

    memset(server, 0, sizeof(*server));
    strlcpy(server->callsign, source, sizeof(server->callsign));
    char source_callsign[7] = {0};
    uint8_t source_ssid = 0;
    if (fmo_aprs_parse_source_ssid(line, line_size, source_callsign,
                                   &source_ssid)) {
        strlcpy(server->callsign, source_callsign,
                sizeof(server->callsign));
        server->has_ssid = memchr(line, '-', (size_t)(gt - copy)) != NULL;
        server->ssid = source_ssid;
    }
    const char *display_name = NULL;
    fmo_public_cert_t certificate = {0};
    bool have_certificate = false;
    for (size_t i = 0; i < count; ++i) {
        char *token = tokens[i];
        if (strncmp(token, "CERT:", 5) == 0) {
            have_certificate = fmo_protocol_parse_beacon_cert(token + 5,
                                                               &certificate);
        } else if (token[0] == 'P' && isdigit((unsigned char)token[1])) {
            uint32_t port = 0;
            if (parse_uint(token + 1, &port) && port > 0 && port <= 65535) {
                server->port = (uint16_t)port;
            }
        } else if (token[0] == 'U' && isdigit((unsigned char)token[1])) {
            char *slash = strchr(token + 1, '/');
            if (slash != NULL) {
                *slash = '\0';
                (void)parse_uint(token + 1, &server->online);
                (void)parse_uint(slash + 1, &server->total);
            }
        } else if (strncmp(token, "SH:", 3) == 0 &&
                   looks_like_host(token + 3)) {
            strlcpy(server->host, token + 3, sizeof(server->host));
        } else if (looks_like_host(token)) {
            strlcpy(server->host, token, sizeof(server->host));
        } else if (strncmp(token, "SIG:", 4) != 0 &&
                   !(strlen(token) == 2 && isupper((unsigned char)token[0]) &&
                     isupper((unsigned char)token[1])) &&
                   token[0] != 'F' && token[0] != '\0') {
            display_name = token;
        }
    }
    if (have_certificate) {
        server->uid = certificate.uid;
        strlcpy(server->callsign, certificate.callsign,
                sizeof(server->callsign));
        memcpy(server->fingerprint, certificate.fingerprint,
               sizeof(server->fingerprint));
        server->has_fingerprint = true;
    }
    if (server->host[0] == '\0' || server->port == 0) return false;
    if (display_name != NULL && valid_utf8(display_name)) {
        strlcpy(server->name, display_name, sizeof(server->name));
    } else if (display_name != NULL &&
               gbk_to_utf8(display_name, server->name,
                           sizeof(server->name))) {
        /* Converted legacy FMO APRS CP936/GBK station name. */
    } else {
        strlcpy(server->name, server->callsign, sizeof(server->name));
    }
    if (server->uid != 0) {
        snprintf(server->key, sizeof(server->key), "uid:%lu",
                 (unsigned long)server->uid);
    } else {
        snprintf(server->key, sizeof(server->key), "%s:%u", server->host,
                 (unsigned)server->port);
    }
    server->last_seen = (int64_t)time(NULL);
    return true;
}

bool fmo_aprs_base_callsign_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;
    size_t na = 0, nb = 0;
    while (a[na] != '\0' && a[na] != '-') na++;
    while (b[nb] != '\0' && b[nb] != '-') nb++;
    return na > 0 && na == nb && strncasecmp(a, b, na) == 0;
}
