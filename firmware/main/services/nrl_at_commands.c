#include "nrl_at_commands.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_system.h"

#include "app/driver/es8311_codec.h"
#include "app/driver/status_io.h"
#include "audio/audio_passthrough.h"
#include "audio/nrl_audio_codec.h"
#include "services/aprs_service.h"
#include "services/config_store.h"
#include "services/espnow_link.h"
#include "services/network_manager.h"
#include "services/nrl_link.h"
#include "services/ota_service.h"
#include "services/radio_at.h"

#define TAG "nrl_at"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void reply_text(nrl_at_result_t *r, const char *text)
{
    r->should_reply = true;
    size_t len = strlen(text);
    if (len >= NRL_AT_REPLY_CAPACITY) len = NRL_AT_REPLY_CAPACITY - 1;
    memcpy(r->payload, text, len);
    r->payload[len] = '\0';
    r->payload_size = len;
}

static void reply_kv(nrl_at_result_t *r, const char *key, const char *value)
{
    char buf[NRL_AT_REPLY_CAPACITY];
    snprintf(buf, sizeof(buf), "AT+%s=%s\r\n", key, value);
    reply_text(r, buf);
}

static void reply_ok(nrl_at_result_t *r, const char *key)
{
    reply_kv(r, key, "OK");
}

static void reply_err(nrl_at_result_t *r, const char *key, const char *reason)
{
    char buf[NRL_AT_REPLY_CAPACITY];
    snprintf(buf, sizeof(buf), "AT+%s=ERROR:%s\r\n", key, reason);
    reply_text(r, buf);
}

static bool str_eq_nocase(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

/* Append one formatted line to a multi-line reply (used by the READ=123
 * full dump). Returns false when the reply buffer is full. */
static bool reply_append(nrl_at_result_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[NRL_AT_REPLY_CAPACITY];
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return false;
    if (r->payload_size + (size_t)n >= NRL_AT_REPLY_CAPACITY) return false;
    memcpy(r->payload + r->payload_size, tmp, (size_t)n);
    r->payload_size += (size_t)n;
    r->payload[r->payload_size] = '\0';
    r->should_reply = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Individual command handlers                                         */
/* ------------------------------------------------------------------ */

static void cmd_status(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    (void)value; (void)query; (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    char buf[NRL_AT_REPLY_CAPACITY];
    snprintf(buf, sizeof(buf),
             "AT+STATUS="
             "RX=%.4f,TX=%.4f,"
             "RXCTCSS=%.1f,TXCTCSS=%.1f,"
             "SQL=%u,PWR=%u,RF=%s,"
             "VOL=%u,%u,"
             "MICGAIN=%u,"
             "CALL=%s,SSID=%u,"
             "VOICE=%s,"
             "APRS=%s,ESPNOW=%s,"
             "NRL=%s:%u"
             "\r\n",
             (double)cfg.radio_rx_mhz, (double)cfg.radio_tx_mhz,
             (double)cfg.rx_ctcss_hz, (double)cfg.tx_ctcss_hz,
             (unsigned)cfg.squelch, (unsigned)cfg.tx_power,
             cfg.rf_enabled ? "ON" : "OFF",
             (unsigned)cfg.rx_volume, (unsigned)cfg.tx_volume,
             (unsigned)cfg.mic_gain,
             cfg.callsign, (unsigned)cfg.callsign_ssid,
             cfg.voice_codec == 1 ? "OPUS" : "G711",
             cfg.aprs_enabled ? "ON" : "OFF",
             cfg.espnow_enabled ? "ON" : "OFF",
             cfg.nrl_host, (unsigned)cfg.nrl_port);
    reply_text(r, buf);
}

static void cmd_rx(const char *value, bool query, nrl_at_source_t src,
                   nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", (double)cfg.radio_rx_mhz);
        reply_kv(r, "RX", buf);
        return;
    }
    float mhz = strtof(value, NULL);
    if (mhz < 1.0f || mhz > 1300.0f) {
        reply_err(r, "RX", "RANGE");
        return;
    }
    cfg.radio_rx_mhz = mhz;
    config_store_save(&cfg);
    radio_at_set_frequency(false, mhz);
    reply_ok(r, "RX");
}

static void cmd_tx(const char *value, bool query, nrl_at_source_t src,
                   nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", (double)cfg.radio_tx_mhz);
        reply_kv(r, "TX", buf);
        return;
    }
    float mhz = strtof(value, NULL);
    if (mhz < 1.0f || mhz > 1300.0f) {
        reply_err(r, "TX", "RANGE");
        return;
    }
    cfg.radio_tx_mhz = mhz;
    config_store_save(&cfg);
    radio_at_set_frequency(true, mhz);
    reply_ok(r, "TX");
}

static void cmd_rxctcss(const char *value, bool query, nrl_at_source_t src,
                        nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", (double)cfg.rx_ctcss_hz);
        reply_kv(r, "RXCTCSS", buf);
        return;
    }
    float hz = strtof(value, NULL);
    if (hz < 0.0f || hz > 300.0f) {
        reply_err(r, "RXCTCSS", "RANGE");
        return;
    }
    cfg.rx_ctcss_hz = hz;
    config_store_save(&cfg);
    nrl_audio_codec_configure_ctcss(cfg.rx_ctcss_hz, cfg.tx_ctcss_hz);
    reply_ok(r, "RXCTCSS");
}

static void cmd_txctcss(const char *value, bool query, nrl_at_source_t src,
                        nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f", (double)cfg.tx_ctcss_hz);
        reply_kv(r, "TXCTCSS", buf);
        return;
    }
    float hz = strtof(value, NULL);
    if (hz < 0.0f || hz > 300.0f) {
        reply_err(r, "TXCTCSS", "RANGE");
        return;
    }
    cfg.tx_ctcss_hz = hz;
    config_store_save(&cfg);
    nrl_audio_codec_configure_ctcss(cfg.rx_ctcss_hz, cfg.tx_ctcss_hz);
    reply_ok(r, "TXCTCSS");
}

static void cmd_sql(const char *value, bool query, nrl_at_source_t src,
                    nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.squelch);
        reply_kv(r, "SQL", buf);
        return;
    }
    int level = atoi(value);
    if (level < 0 || level > 10) {
        reply_err(r, "SQL", "RANGE");
        return;
    }
    cfg.squelch = (uint8_t)level;
    config_store_save(&cfg);
    radio_at_set_squelch(cfg.squelch);
    reply_ok(r, "SQL");
}

static void cmd_pwr(const char *value, bool query, nrl_at_source_t src,
                    nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.tx_power);
        reply_kv(r, "PWR", buf);
        return;
    }
    int level = atoi(value);
    if (level < 0 || level > 2) {
        reply_err(r, "PWR", "RANGE");
        return;
    }
    cfg.tx_power = (uint8_t)level;
    config_store_save(&cfg);
    radio_at_set_tx_power(cfg.tx_power);
    reply_ok(r, "PWR");
}

static void cmd_rf(const char *value, bool query, nrl_at_source_t src,
                   nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "RF", cfg.rf_enabled ? "ON" : "OFF");
        return;
    }
    bool en = str_eq_nocase(value, "ON") || str_eq_nocase(value, "1");
    cfg.rf_enabled = en;
    config_store_save(&cfg);
    radio_at_set_rf_enabled(en);
    reply_ok(r, "RF");
}

static void cmd_vol(const char *value, bool query, nrl_at_source_t src,
                    nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u,%u",
                 (unsigned)cfg.rx_volume, (unsigned)cfg.tx_volume);
        reply_kv(r, "VOL", buf);
        return;
    }
    /* Accept "RX,TX" or single value for both */
    int rx_vol = 0, tx_vol = 0;
    if (strchr(value, ',') != NULL) {
        sscanf(value, "%d,%d", &rx_vol, &tx_vol);
    } else {
        rx_vol = tx_vol = atoi(value);
    }
    if (rx_vol < 0 || rx_vol > 100 || tx_vol < 0 || tx_vol > 100) {
        reply_err(r, "VOL", "RANGE");
        return;
    }
    cfg.rx_volume = (uint8_t)rx_vol;
    cfg.tx_volume = (uint8_t)tx_vol;
    config_store_save(&cfg);
    radio_at_set_volume(false, cfg.rx_volume);
    radio_at_set_volume(true, cfg.tx_volume);
    reply_ok(r, "VOL");
}

static void cmd_call(const char *value, bool query, nrl_at_source_t src,
                     nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s-%u", cfg.callsign,
                 (unsigned)cfg.callsign_ssid);
        reply_kv(r, "CALL", buf);
        return;
    }
    /* Parse "CALLSIGN" or "CALLSIGN-SSID" */
    char cs[16] = {0};
    int ssid = cfg.callsign_ssid;
    const char *dash = strchr(value, '-');
    if (dash != NULL) {
        size_t clen = (size_t)(dash - value);
        if (clen > 6) clen = 6;
        memcpy(cs, value, clen);
        cs[clen] = '\0';
        ssid = atoi(dash + 1);
        if (ssid < 0 || ssid > 15) ssid = 0;
    } else {
        strlcpy(cs, value, sizeof(cs));
    }
    if (cs[0] == '\0') {
        reply_err(r, "CALL", "EMPTY");
        return;
    }
    /* Uppercase */
    for (char *p = cs; *p; ++p) *p = (char)toupper((unsigned char)*p);
    strlcpy(cfg.callsign, cs, sizeof(cfg.callsign));
    cfg.callsign_ssid = (uint8_t)ssid;
    config_store_save(&cfg);
    nrl_link_update_config(&cfg);
    reply_ok(r, "CALL");
}

static void cmd_voice(const char *value, bool query, nrl_at_source_t src,
                      nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "VOICE", cfg.voice_codec == 1 ? "OPUS" : "G711");
        return;
    }
    uint8_t codec;
    if (str_eq_nocase(value, "OPUS") || str_eq_nocase(value, "1")) {
        codec = 1;
    } else if (str_eq_nocase(value, "G711") || str_eq_nocase(value, "0")) {
        codec = 0;
    } else {
        reply_err(r, "VOICE", "G711|OPUS");
        return;
    }
    cfg.voice_codec = codec;
    config_store_save(&cfg);
    audio_passthrough_set_voice_codec(codec);
    reply_ok(r, "VOICE");
}

/* Software mic amplification. Named MIC_PCM_GAIN after the NRL-ESP32
 * reference (which accepts 0.1-5.0 as a float multiplier); we store an
 * integer 1-5, so fractional input is rounded. */
static void cmd_mic_pcm_gain(const char *value, bool query,
                             nrl_at_source_t src, nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.mic_gain);
        reply_kv(r, "MIC_PCM_GAIN", buf);
        return;
    }
    float gain = strtof(value, NULL);
    int level = (int)(gain + 0.5f);
    if (level < FMO_MIC_GAIN_MIN || level > FMO_MIC_GAIN_MAX) {
        reply_err(r, "MIC_PCM_GAIN", "RANGE");
        return;
    }
    cfg.mic_gain = (uint8_t)level;
    config_store_save(&cfg);
    audio_passthrough_set_mic_gain(cfg.mic_gain);
    reply_ok(r, "MIC_PCM_GAIN");
}

static void cmd_ptt(const char *value, bool query, nrl_at_source_t src,
                    nrl_at_result_t *r)
{
    (void)src;
    if (query) {
        reply_kv(r, "PTT", status_io_is_network_ptt() ? "1" : "0");
        return;
    }
    bool active = (atoi(value) != 0) || str_eq_nocase(value, "ON");
    status_io_set_network_ptt(active);
    reply_ok(r, "PTT");
}

static void cmd_aprs(const char *value, bool query, nrl_at_source_t src,
                     nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "APRS", cfg.aprs_enabled ? "ON" : "OFF");
        return;
    }
    bool en = str_eq_nocase(value, "ON") || str_eq_nocase(value, "1");
    cfg.aprs_enabled = en;
    config_store_save(&cfg);
    aprs_service_set_enabled(en);
    reply_ok(r, "APRS");
}

static void cmd_espnow(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "ESPNOW", cfg.espnow_enabled ? "ON" : "OFF");
        return;
    }
    bool en = str_eq_nocase(value, "ON") || str_eq_nocase(value, "1");
    cfg.espnow_enabled = en;
    config_store_save(&cfg);
    espnow_link_set_enabled(en);
    reply_ok(r, "ESPNOW");
}

static void cmd_wifi_ssid(const char *value, bool query, nrl_at_source_t src,
                          nrl_at_result_t *r)
{
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "WIFI_SSID", cfg.wifi_ssid);
        return;
    }
    if (src != NRL_AT_SOURCE_SERIAL) {
        reply_err(r, "WIFI_SSID", "SERIAL_ONLY");
        return;
    }
    strlcpy(cfg.wifi_ssid, value, sizeof(cfg.wifi_ssid));
    config_store_save(&cfg);
    reply_ok(r, "WIFI_SSID");
}

/* Query-only: current DHCP address of the station interface (the
 * reference firmware exposes AT+WIFI_IP the same way). */
static void cmd_wifi_ip(const char *value, bool query, nrl_at_source_t src,
                        nrl_at_result_t *r)
{
    (void)value;
    (void)src;
    if (!query) {
        reply_err(r, "WIFI_IP", "READONLY");
        return;
    }
    network_status_t net = {0};
    network_manager_get_status(&net);
    reply_kv(r, "WIFI_IP",
             net.station_connected && net.ip_address[0] != '\0'
                 ? net.ip_address : "0.0.0.0");
}

static void cmd_wifi_pass(const char *value, bool query, nrl_at_source_t src,
                          nrl_at_result_t *r)
{
    if (query) {
        reply_kv(r, "WIFI_PASS", "****");
        return;
    }
    if (src != NRL_AT_SOURCE_SERIAL) {
        reply_err(r, "WIFI_PASS", "SERIAL_ONLY");
        return;
    }
    fmo_config_t cfg;
    config_store_load(&cfg);
    strlcpy(cfg.wifi_password, value, sizeof(cfg.wifi_password));
    config_store_save(&cfg);
    reply_ok(r, "WIFI_PASS");
}

static void cmd_otaurl(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    if (query) {
        fmo_ota_ui_status_t st;
        ota_service_get_ui_status(&st);
        reply_kv(r, "OTAURL", st.server_url);
        return;
    }
    if (src != NRL_AT_SOURCE_SERIAL) {
        reply_err(r, "OTAURL", "SERIAL_ONLY");
        return;
    }
    if (!ota_service_set_config(value, "")) {
        reply_err(r, "OTAURL", "INVALID");
        return;
    }
    reply_ok(r, "OTAURL");
}

static void cmd_otacheck(const char *value, bool query, nrl_at_source_t src,
                         nrl_at_result_t *r)
{
    (void)value; (void)query; (void)src;
    if (!ota_service_check_now()) {
        reply_err(r, "OTACHECK", "BUSY");
        return;
    }
    reply_ok(r, "OTACHECK");
}

static void cmd_reboot(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    (void)value; (void)query;
    if (src != NRL_AT_SOURCE_SERIAL) {
        reply_err(r, "REBOOT", "SERIAL_ONLY");
        return;
    }
    reply_ok(r, "REBOOT");
    r->reboot = true;
}

static void cmd_freqtune(const char *value, bool query, nrl_at_source_t src,
                         nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)cfg.freq_tune_hz);
        reply_kv(r, "FREQTUNE", buf);
        return;
    }
    long hz = strtol(value, NULL, 10);
    if (hz < -5000 || hz > 5000) {
        reply_err(r, "FREQTUNE", "RANGE");
        return;
    }
    cfg.freq_tune_hz = (int16_t)hz;
    config_store_save(&cfg);
    radio_at_set_freq_tune(cfg.freq_tune_hz);
    reply_ok(r, "FREQTUNE");
}

/* Hardware mic volume (ES8311 ADC register), named after the NRL-ESP32
 * reference command. */
static void cmd_mic_gain(const char *value, bool query, nrl_at_source_t src,
                         nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.es8311_adc_vol);
        reply_kv(r, "MIC_GAIN", buf);
        return;
    }
    int vol = atoi(value);
    if (vol < 0 || vol > FMO_ES8311_ADC_VOL_MAX) {
        reply_err(r, "MIC_GAIN", "RANGE");
        return;
    }
    cfg.es8311_adc_vol = (uint8_t)vol;
    config_store_save(&cfg);
    (void)es8311_codec_set_adc_volume(cfg.es8311_adc_vol);
    reply_ok(r, "MIC_GAIN");
}

/* Speaker/line-out volume (ES8311 DAC register), named after the
 * NRL-ESP32 VOLUME command. */
static void cmd_volume(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.es8311_dac_vol);
        reply_kv(r, "VOLUME", buf);
        return;
    }
    int vol = atoi(value);
    if (vol < 0 || vol > FMO_ES8311_DAC_VOL_MAX) {
        reply_err(r, "VOLUME", "RANGE");
        return;
    }
    cfg.es8311_dac_vol = (uint8_t)vol;
    config_store_save(&cfg);
    (void)es8311_codec_set_dac_volume(cfg.es8311_dac_vol);
    reply_ok(r, "VOLUME");
}

/* Callsign SSID (reference splits CALL / SSID; FMO's CALL accepts the
 * combined "CALL-SSID" form too). */
static void cmd_ssid(const char *value, bool query, nrl_at_source_t src,
                     nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.callsign_ssid);
        reply_kv(r, "SSID", buf);
        return;
    }
    int ssid = atoi(value);
    if (ssid < 0 || ssid > 15) {
        reply_err(r, "SSID", "RANGE");
        return;
    }
    cfg.callsign_ssid = (uint8_t)ssid;
    config_store_save(&cfg);
    nrl_link_update_config(&cfg);
    reply_ok(r, "SSID");
}

/* NRL server host/port (reference D_IP / D_PORT) */
static void cmd_d_ip(const char *value, bool query, nrl_at_source_t src,
                     nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "D_IP", cfg.nrl_host);
        return;
    }
    if (value[0] == '\0') {
        reply_err(r, "D_IP", "EMPTY");
        return;
    }
    strlcpy(cfg.nrl_host, value, sizeof(cfg.nrl_host));
    config_store_save(&cfg);
    nrl_link_update_config(&cfg);
    reply_ok(r, "D_IP");
}

static void cmd_d_port(const char *value, bool query, nrl_at_source_t src,
                       nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.nrl_port);
        reply_kv(r, "D_PORT", buf);
        return;
    }
    int port = atoi(value);
    if (port < 1 || port > 65535) {
        reply_err(r, "D_PORT", "RANGE");
        return;
    }
    cfg.nrl_port = (uint16_t)port;
    config_store_save(&cfg);
    nrl_link_update_config(&cfg);
    reply_ok(r, "D_PORT");
}

/* APRS channel switches, named after the NRL-ESP32 reference:
 * APRS_RX (RF decode), APRS_TX (RF AFSK out), APRS_NRLRX / APRS_NRLTX
 * (NRL decode / AFSK out) and APRS_NET (APRS-IS link). */
static void cmd_aprs_switch(const char *value, bool query,
                            nrl_at_source_t src, nrl_at_result_t *r,
                            const char *name, int which)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    bool *target = NULL;
    switch (which) {
    case 0: target = &cfg.aprs_rf_rx; break;
    case 1: target = &cfg.aprs_rf_tx; break;
    case 2: target = &cfg.aprs_nrl_rx; break;
    case 3: target = &cfg.aprs_nrl_tx; break;
    case 4: target = &cfg.aprs_enabled; break;
    default: break;
    }
    if (target == NULL) return;
    if (query) {
        reply_kv(r, name, *target ? "ON" : "OFF");
        return;
    }
    bool en = str_eq_nocase(value, "ON") || str_eq_nocase(value, "1");
    *target = en;
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, name);
}

static void cmd_aprs_rx(const char *value, bool query, nrl_at_source_t src,
                        nrl_at_result_t *r)
{
    cmd_aprs_switch(value, query, src, r, "APRS_RX", 0);
}

static void cmd_aprs_tx(const char *value, bool query, nrl_at_source_t src,
                        nrl_at_result_t *r)
{
    cmd_aprs_switch(value, query, src, r, "APRS_TX", 1);
}

static void cmd_aprs_nlrx(const char *value, bool query, nrl_at_source_t src,
                          nrl_at_result_t *r)
{
    cmd_aprs_switch(value, query, src, r, "APRS_NRLRX", 2);
}

static void cmd_aprs_nltx(const char *value, bool query, nrl_at_source_t src,
                          nrl_at_result_t *r)
{
    cmd_aprs_switch(value, query, src, r, "APRS_NRLTX", 3);
}

static void cmd_aprs_net(const char *value, bool query, nrl_at_source_t src,
                         nrl_at_result_t *r)
{
    cmd_aprs_switch(value, query, src, r, "APRS_NET", 4);
}

/* Six-way gateway switches, wire format identical to the reference:
 *   AT+APRS_FWD=?            -> RF2IS=ON IS2RF=OFF ...
 *   AT+APRS_FWD=RF2IS,ON     -> set one direction */
static void cmd_aprs_fwd(const char *value, bool query, nrl_at_source_t src,
                         nrl_at_result_t *r)
{
    (void)src;
    static const char *dir_names[6] = {
        "RF2IS", "IS2RF", "NRL2IS", "IS2NRL", "RF2NRL", "NRL2RF"};
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query || value[0] == '\0') {
        char states[64];
        size_t used = 0;
        for (int i = 0; i < 6 && used + 12 < sizeof(states); ++i) {
            used += (size_t)snprintf(states + used, sizeof(states) - used,
                                     "%s%s=%s", i > 0 ? " " : "", dir_names[i],
                                     (cfg.aprs_fwd & (1u << i)) ? "ON" : "OFF");
        }
        reply_kv(r, "APRS_FWD", states);
        return;
    }
    char dir_name[8] = {0};
    char bool_part[8] = {0};
    if (sscanf(value, "%7[^,],%7s", dir_name, bool_part) != 2) {
        reply_err(r, "APRS_FWD", "SYNTAX");
        return;
    }
    int dir = -1;
    for (int i = 0; i < 6; ++i) {
        if (str_eq_nocase(dir_name, dir_names[i])) { dir = i; break; }
    }
    if (dir < 0) {
        reply_err(r, "APRS_FWD", "DIR");
        return;
    }
    bool en = str_eq_nocase(bool_part, "ON") || str_eq_nocase(bool_part, "1");
    if (en) cfg.aprs_fwd |= (uint8_t)(1u << dir);
    else cfg.aprs_fwd &= (uint8_t)~(1u << dir);
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    char reply[24];
    snprintf(reply, sizeof(reply), "%s=%s", dir_names[dir], en ? "ON" : "OFF");
    reply_kv(r, "APRS_FWD", reply);
}

/* Beacon interval seconds (reference APRS_INTERVAL) */
static void cmd_aprs_interval(const char *value, bool query,
                              nrl_at_source_t src, nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.aprs_beacon_interval_s);
        reply_kv(r, "APRS_INTERVAL", buf);
        return;
    }
    int sec = atoi(value);
    if (sec < 30 || sec > 3600) {
        reply_err(r, "APRS_INTERVAL", "RANGE");
        return;
    }
    cfg.aprs_beacon_interval_s = (uint16_t)sec;
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, "APRS_INTERVAL");
}

/* Fixed-position beacon switch (reference APRS_FIXED) */
static void cmd_aprs_fixed(const char *value, bool query,
                           nrl_at_source_t src, nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "APRS_FIXED", cfg.aprs_position_set ? "ON" : "OFF");
        return;
    }
    bool en = str_eq_nocase(value, "ON") || str_eq_nocase(value, "1");
    cfg.aprs_position_set = en;
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, "APRS_FIXED");
}

/* APRS-IS server "host:port" (reference APRS_SERVER) */
static void cmd_aprs_server(const char *value, bool query,
                            nrl_at_source_t src, nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s:%u", cfg.aprs_server_host,
                 (unsigned)cfg.aprs_server_port);
        reply_kv(r, "APRS_SERVER", buf);
        return;
    }
    char host[65] = {0};
    unsigned port = 14580;
    const char *colon = strrchr(value, ':');
    if (colon != NULL && colon != value) {
        const size_t host_len = (size_t)(colon - value);
        if (host_len >= sizeof(host) || sscanf(colon + 1, "%u", &port) != 1 ||
            port < 1 || port > 65535) {
            reply_err(r, "APRS_SERVER", "SYNTAX");
            return;
        }
        memcpy(host, value, host_len);
        host[host_len] = '\0';
    } else {
        strlcpy(host, value, sizeof(host));
    }
    if (host[0] == '\0') {
        reply_err(r, "APRS_SERVER", "EMPTY");
        return;
    }
    strlcpy(cfg.aprs_server_host, host, sizeof(cfg.aprs_server_host));
    cfg.aprs_server_port = (uint16_t)port;
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, "APRS_SERVER");
}

/* APRS SSID (reference APRS_SSID) */
static void cmd_aprs_ssid(const char *value, bool query, nrl_at_source_t src,
                          nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)cfg.aprs_ssid);
        reply_kv(r, "APRS_SSID", buf);
        return;
    }
    int ssid = atoi(value);
    if (ssid < 0 || ssid > 15) {
        reply_err(r, "APRS_SSID", "RANGE");
        return;
    }
    cfg.aprs_ssid = (uint8_t)ssid;
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, "APRS_SSID");
}

/* Beacon comment (reference APRS_COMMENT) */
static void cmd_aprs_comment(const char *value, bool query,
                             nrl_at_source_t src, nrl_at_result_t *r)
{
    (void)src;
    fmo_config_t cfg;
    config_store_load(&cfg);
    if (query) {
        reply_kv(r, "APRS_COMMENT", cfg.aprs_comment);
        return;
    }
    strlcpy(cfg.aprs_comment, value, sizeof(cfg.aprs_comment));
    config_store_save(&cfg);
    aprs_service_update_config(&cfg);
    reply_ok(r, "APRS_COMMENT");
}

static void cmd_read(const char *value, bool query, nrl_at_source_t src,
                     nrl_at_result_t *r)
{
    (void)src;
    /* AT+READ=123: full dump of every supported command with its current
     * value (wire-compatible with the NRL-ESP32 reference project). */
    if (!query && str_eq_nocase(value, "123")) {
        static const char *fwd_names[6] = {
            "RF2IS", "IS2RF", "NRL2IS", "IS2NRL", "RF2NRL", "NRL2RF"};
        fmo_config_t cfg;
        config_store_load(&cfg);
        fmo_ota_ui_status_t ota = {0};
        ota_service_get_ui_status(&ota);
        network_status_t net = {0};
        network_manager_get_status(&net);
        bool ok =
            reply_append(r, "AT+RX=%.4f\r\n", (double)cfg.radio_rx_mhz) &&
            reply_append(r, "AT+TX=%.4f\r\n", (double)cfg.radio_tx_mhz) &&
            reply_append(r, "AT+RXCTCSS=%.1f\r\n", (double)cfg.rx_ctcss_hz) &&
            reply_append(r, "AT+TXCTCSS=%.1f\r\n", (double)cfg.tx_ctcss_hz) &&
            reply_append(r, "AT+SQL=%u\r\n", (unsigned)cfg.squelch) &&
            reply_append(r, "AT+PWR=%u\r\n", (unsigned)cfg.tx_power) &&
            reply_append(r, "AT+RF=%s\r\n", cfg.rf_enabled ? "ON" : "OFF") &&
            reply_append(r, "AT+VOL=%u,%u\r\n", (unsigned)cfg.rx_volume,
                         (unsigned)cfg.tx_volume) &&
            reply_append(r, "AT+CALL=%s\r\n", cfg.callsign) &&
            reply_append(r, "AT+SSID=%u\r\n", (unsigned)cfg.callsign_ssid) &&
            reply_append(r, "AT+VOICE=%s\r\n",
                         cfg.voice_codec == 1 ? "OPUS" : "G711") &&
            reply_append(r, "AT+MIC_GAIN=%u\r\n",
                         (unsigned)cfg.es8311_adc_vol) &&
            reply_append(r, "AT+MIC_PCM_GAIN=%u\r\n",
                         (unsigned)cfg.mic_gain) &&
            reply_append(r, "AT+VOLUME=%u\r\n",
                         (unsigned)cfg.es8311_dac_vol) &&
            reply_append(r, "AT+D_IP=%s\r\n", cfg.nrl_host) &&
            reply_append(r, "AT+D_PORT=%u\r\n", (unsigned)cfg.nrl_port) &&
            reply_append(r, "AT+FREQTUNE=%d\r\n", (int)cfg.freq_tune_hz) &&
            reply_append(r, "AT+ESPNOW=%s\r\n",
                         cfg.espnow_enabled ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS=%s\r\n",
                         cfg.aprs_enabled ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS_NET=%s\r\n",
                         cfg.aprs_enabled ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS_RX=%s\r\n",
                         cfg.aprs_rf_rx ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS_TX=%s\r\n",
                         cfg.aprs_rf_tx ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS_NRLRX=%s\r\n",
                         cfg.aprs_nrl_rx ? "ON" : "OFF") &&
            reply_append(r, "AT+APRS_NRLTX=%s\r\n",
                         cfg.aprs_nrl_tx ? "ON" : "OFF");
        if (ok) {
            char fwd[64];
            size_t used = 0;
            for (int i = 0; i < 6 && used + 12 < sizeof(fwd); ++i) {
                used += (size_t)snprintf(fwd + used, sizeof(fwd) - used,
                                         "%s%s=%s", i > 0 ? " " : "",
                                         fwd_names[i],
                                         (cfg.aprs_fwd & (1u << i))
                                             ? "ON" : "OFF");
            }
            ok = reply_append(r, "AT+APRS_FWD=%s\r\n", fwd);
        }
        if (ok) {
            ok = reply_append(r, "AT+APRS_INTERVAL=%u\r\n",
                              (unsigned)cfg.aprs_beacon_interval_s) &&
                 reply_append(r, "AT+APRS_FIXED=%s\r\n",
                              cfg.aprs_position_set ? "ON" : "OFF") &&
                 reply_append(r, "AT+APRS_SERVER=%s:%u\r\n",
                              cfg.aprs_server_host,
                              (unsigned)cfg.aprs_server_port) &&
                 reply_append(r, "AT+APRS_SSID=%u\r\n",
                              (unsigned)cfg.aprs_ssid) &&
                 reply_append(r, "AT+APRS_COMMENT=%s\r\n", cfg.aprs_comment) &&
                 reply_append(r, "AT+WIFI_SSID=%s\r\n", cfg.wifi_ssid) &&
                 reply_append(r, "AT+WIFI_IP=%s\r\n",
                              net.station_connected && net.ip_address[0] != '\0'
                                  ? net.ip_address : "0.0.0.0") &&
                 reply_append(r, "AT+OTAURL=%s\r\n",
                              ota.configured ? ota.server_url : "OFF");
        }
        if (!ok) r->should_reply = false;
        return;
    }
    reply_text(r,
        "AT+READ="
        "STATUS,RX,TX,RXCTCSS,TXCTCSS,SQL,PWR,RF,VOL,"
        "CALL,SSID,VOICE,MIC_GAIN,MIC_PCM_GAIN,VOLUME,"
        "D_IP,D_PORT,PTT,APRS,ESPNOW,FREQTUNE,"
        "APRS_NET,APRS_RX,APRS_TX,APRS_NRLRX,APRS_NRLTX,"
        "APRS_FWD,APRS_INTERVAL,APRS_FIXED,APRS_SERVER,"
        "APRS_SSID,APRS_COMMENT,"
        "WIFI_SSID,WIFI_IP,WIFI_PASS,OTAURL,OTACHECK,REBOOT,READ"
        "\r\n");
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                      */
/* ------------------------------------------------------------------ */

typedef void (*at_handler_t)(const char *value, bool query,
                             nrl_at_source_t src, nrl_at_result_t *r);

typedef struct {
    const char *name;
    at_handler_t handler;
} at_entry_t;

static const at_entry_t s_commands[] = {
    { "STATUS",    cmd_status },
    { "RX",        cmd_rx },
    { "TX",        cmd_tx },
    { "RXCTCSS",   cmd_rxctcss },
    { "TXCTCSS",   cmd_txctcss },
    { "SQL",       cmd_sql },
    { "PWR",       cmd_pwr },
    { "RF",        cmd_rf },
    { "VOL",       cmd_vol },
    { "CALL",      cmd_call },
    { "SSID",      cmd_ssid },
    { "VOICE",     cmd_voice },
    { "MIC_GAIN",  cmd_mic_gain },
    { "MIC_PCM_GAIN", cmd_mic_pcm_gain },
    { "VOLUME",    cmd_volume },
    { "D_IP",      cmd_d_ip },
    { "D_PORT",    cmd_d_port },
    { "PTT",       cmd_ptt },
    { "APRS",      cmd_aprs },
    { "APRS_NET",  cmd_aprs_net },
    { "APRS_RX",   cmd_aprs_rx },
    { "APRS_TX",   cmd_aprs_tx },
    { "APRS_NRLRX", cmd_aprs_nlrx },
    { "APRS_NRLTX", cmd_aprs_nltx },
    { "APRS_FWD",  cmd_aprs_fwd },
    { "APRS_INTERVAL", cmd_aprs_interval },
    { "APRS_FIXED", cmd_aprs_fixed },
    { "APRS_SERVER", cmd_aprs_server },
    { "APRS_SSID", cmd_aprs_ssid },
    { "APRS_COMMENT", cmd_aprs_comment },
    { "ESPNOW",    cmd_espnow },
    { "WIFI_SSID", cmd_wifi_ssid },
    { "WIFI_IP",   cmd_wifi_ip },
    { "WIFI_PASS", cmd_wifi_pass },
    { "OTAURL",    cmd_otaurl },
    { "OTACHECK",  cmd_otacheck },
    { "REBOOT",    cmd_reboot },
    { "FREQTUNE",  cmd_freqtune },
    { "READ",      cmd_read },
};

#define AT_COMMAND_COUNT (sizeof(s_commands) / sizeof(s_commands[0]))

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

void nrl_at_handle_payload(const uint8_t *payload, size_t payload_size,
                           nrl_at_source_t source, nrl_at_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    if (payload == NULL || payload_size == 0) return;

    /* Copy to a working buffer, skip optional 0x01 serial prefix */
    const uint8_t *p = payload;
    size_t len = payload_size;
    if (len > 0 && p[0] == 0x01) { ++p; --len; }

    /* Strip leading whitespace / newlines */
    while (len > 0 && (*p == '\r' || *p == '\n' || *p == ' ')) { ++p; --len; }

    /* Must start with "AT+" */
    if (len < 3 || strncasecmp((const char *)p, "AT+", 3) != 0) {
        reply_err(result, "?", "SYNTAX");
        return;
    }
    p += 3;
    len -= 3;

    /* Extract key (up to '=' or '?' or end) */
    char key[24] = {0};
    size_t ki = 0;
    while (len > 0 && ki < sizeof(key) - 1 &&
           *p != '=' && *p != '?' && *p != '\r' && *p != '\n') {
        key[ki++] = (char)toupper((unsigned char)*p);
        ++p; --len;
    }
    key[ki] = '\0';

    bool query = false;
    char value[128] = {0};

    if (len > 0 && *p == '?') {
        query = true;
    } else if (len > 0 && *p == '=') {
        ++p; --len;
        /* Strip trailing \r\n */
        size_t vi = 0;
        while (len > 0 && vi < sizeof(value) - 1 &&
               *p != '\r' && *p != '\n') {
            value[vi++] = (char)*p;
            ++p; --len;
        }
        value[vi] = '\0';
    }
    /* else: bare "AT+KEY" treated as query */
    if (!query && value[0] == '\0' && (len == 0 || *p == '\r' || *p == '\n'))
        query = true;

    /* Find handler */
    for (size_t i = 0; i < AT_COMMAND_COUNT; ++i) {
        if (strcmp(key, s_commands[i].name) == 0) {
            ESP_LOGI(TAG, "AT+%s %s src=%d", key,
                     query ? "?" : value, (int)source);
            s_commands[i].handler(value, query, source, result);
            return;
        }
    }
    reply_err(result, key, "UNKNOWN");
}
