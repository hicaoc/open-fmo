#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "audio/audio_passthrough.h"
#include "audio/nrl_audio_codec.h"
#include "audio/mdc_signaling.h"
#include "app/driver/es8311_codec.h"
#include "app/driver/status_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "services/app_notice.h"
#include "services/aprs_service.h"
#include "services/config_store.h"
#include "services/fmo_discovery.h"
#include "services/fmo_link.h"
#include "services/fmo_qso.h"
#include "services/network_manager.h"
#include "services/net_radio.h"
#include "services/nrl_link.h"
#include "services/ota_service.h"
#include "services/radio_at.h"
#include "services/server_directory.h"
#include "version.h"

#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0xffff
#define COLOR_ORANGE 0xfd20
#define COLOR_GRAY   0x8410
#define COLOR_DARK   0x2104
#define COLOR_GREEN  0x07e0
#define COLOR_RED    0xf800

/* Cached config: only re-reads NVS when generation changes (web/encoder save) */
static fmo_config_t s_cfg_cache;
static uint32_t s_cfg_gen = UINT32_MAX;

/* Selected station on the net-radio detail page (not the playing one) */
static int s_net_radio_sel = -1;

static const fmo_config_t *cfg_get(void)
{
    uint32_t gen = config_store_generation();
    if (gen != s_cfg_gen) {
        config_store_load(&s_cfg_cache);
        s_cfg_gen = gen;
    }
    return &s_cfg_cache;
}
#define COLOR_PURPLE 0xA01F

static const char *k_menu_en[] = {
    "Servers", "Radio", "CTCSS", "APRS",
    "Network", "Audio", "System OTA", "Language", "Net Radio"
};
static const char *k_menu_zh[] = {
    "\u670d\u52a1\u5668\u5217\u8868", "\u5c04\u9891\u8bbe\u7f6e",
    "\u4e9a\u97f3\u8bbe\u7f6e", "APRS\u8bbe\u7f6e",
    "\u7f51\u7edc\u8bbe\u7f6e", "\u97f3\u9891\u8bbe\u7f6e",
    "\u7cfb\u7edf\u5347\u7ea7", "\u8bed\u8a00", "\u7f51\u7edc\u7535\u53f0"
};
static const int k_menu_count = sizeof(k_menu_en) / sizeof(k_menu_en[0]);

static const char *tr(const app_ui_t *ui, const char *english, const char *chinese)
{
    return ui->chinese ? chinese : english;
}

static const char *menu_text(const app_ui_t *ui, int index)
{
    return ui->chinese ? k_menu_zh[index] : k_menu_en[index];
}

void app_ui_init(app_ui_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    ui->page = APP_UI_MAIN;
    ui->chinese = true;
    strlcpy(ui->callsign, "NOCALL", sizeof(ui->callsign));
    ui->tx_mhz = 438.5000f;
    ui->rx_mhz = 438.5000f;
}

static void change_server(app_ui_t *ui, int direction)
{
    const int count = (int)server_directory_count();
    if (count <= 0) return;
    int next = (int)ui->server_index + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    ui->server_index = (size_t)next;
    ui->server_change_pending = true;
    /* Keep the scroll preview visible while turning and 3s after */
    ui->server_preview_until_us = esp_timer_get_time() + 3000000;
}

static void change_fmo_favorite(app_ui_t *ui, int direction)
{
    const int count = (int)fmo_server_directory_favorite_count();
    if (count <= 0) return;
    int next = (int)ui->fmo_favorite_index + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    ui->fmo_favorite_index = (size_t)next;
    ui->fmo_server_change_pending = true;
    ui->server_preview_until_us = esp_timer_get_time() + 3000000;
}

static void change_fmo_directory(app_ui_t *ui, int direction)
{
    const int count = (int)fmo_server_directory_count();
    if (count <= 0) return;
    int next = (int)ui->fmo_server_index + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    ui->fmo_server_index = (size_t)next;
}

/* While the net radio is playing, the knob switches stations instead of
 * servers (the preview list shows stations, see render_main). */
static void change_station(app_ui_t *ui, int direction)
{
    const int count = (int)net_radio_count();
    if (count <= 0) return;
    int next = net_radio_get_current() + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    net_radio_play((size_t)next);
    ui->server_preview_until_us = esp_timer_get_time() + 3000000;
}

static const float k_ctcss_tones[] = {
    0.0f, 67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f,
    88.5f, 91.5f, 94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f,
    114.8f, 118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f,
    151.4f, 156.7f, 159.8f, 162.2f, 165.5f, 167.9f, 171.3f, 173.8f,
    177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f, 199.5f,
    203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f,
    250.3f, 254.1f
};
#define UI_CTCSS_COUNT (sizeof(k_ctcss_tones) / sizeof(k_ctcss_tones[0]))

static int ctcss_find_index(float hz)
{
    for (size_t i = 0; i < UI_CTCSS_COUNT; ++i) {
        if (k_ctcss_tones[i] == hz) return (int)i;
    }
    int best = 0;
    float best_diff = 9999.0f;
    for (size_t i = 0; i < UI_CTCSS_COUNT; ++i) {
        float diff = k_ctcss_tones[i] - hz;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) { best_diff = diff; best = (int)i; }
    }
    return best;
}

void app_ui_handle(app_ui_t *ui, encoder_event_type_t event)
{
    if (ui == NULL) return;
    if (event == ENCODER_EVENT_LONG_PRESS) {
        if (ui->page == APP_UI_DETAIL) {
            /* Leaving the net radio page stops playback */
            if (ui->menu_index == 8) net_radio_stop();
            ui->page = APP_UI_MENU;
            ui->server_select_mode = 0;
        }
        else if (ui->page == APP_UI_MENU) ui->page = APP_UI_MAIN;
        /* Main page: long press stops net radio playback */
        else if (net_radio_is_playing()) net_radio_stop();
        return;
    }

    const int direction = event == ENCODER_EVENT_CLOCKWISE ? 1 :
                          event == ENCODER_EVENT_COUNTER_CLOCKWISE ? -1 : 0;
    if (ui->page == APP_UI_MAIN) {
        if (direction != 0) {
            if (net_radio_is_playing()) change_station(ui, direction);
            else {
                int64_t now = esp_timer_get_time();
                if (now >= ui->server_preview_until_us) {
                    ui->server_select_mode = 0;
                }
                if (ui->server_select_mode == 0) {
                    ui->server_select_mode = direction < 0 ? 2 : 1;
                }
                if (ui->server_select_mode == 2) {
                    change_fmo_favorite(ui, direction);
                } else {
                    change_server(ui, direction);
                }
            }
        }
        else if (event == ENCODER_EVENT_PRESS) ui->page = APP_UI_MENU;
    } else if (ui->page == APP_UI_MENU) {
        if (direction != 0) {
            ui->menu_index += direction;
            if (ui->menu_index < 0) ui->menu_index = k_menu_count - 1;
            if (ui->menu_index >= k_menu_count) ui->menu_index = 0;
        } else if (event == ENCODER_EVENT_PRESS) {
            ui->page = APP_UI_DETAIL;
            if (ui->menu_index == 0) ui->server_select_mode = 0;
        }
    } else if (ui->page == APP_UI_DETAIL) {
        if (ui->menu_index == 0) {
            if (direction != 0) {
                if (ui->server_select_mode == 0) {
                    ui->server_select_mode = direction < 0 ? 2 : 1;
                }
                if (ui->server_select_mode == 2) {
                    change_fmo_directory(ui, direction);
                } else {
                    change_server(ui, direction);
                }
            } else if (event == ENCODER_EVENT_PRESS &&
                       ui->server_select_mode == 2) {
                const fmo_server_t *server =
                    fmo_server_directory_get(ui->fmo_server_index);
                if (server != NULL) {
                    (void)fmo_server_directory_set_favorite(
                        server->key, !server->favorite);
                }
            }
        } else if (ui->menu_index == 1) {
            /* Radio detail: rotate adjusts current field, press switches field */
            fmo_config_t cfg;
            config_store_load(&cfg);
            if (event == ENCODER_EVENT_PRESS) {
                ui->radio_adjust_field = (ui->radio_adjust_field + 1) % 5;
            } else if (direction != 0) {
                switch (ui->radio_adjust_field) {
                case 0: /* RX freq */
                    cfg.radio_rx_mhz += direction * 0.005f;
                    if (cfg.radio_rx_mhz < 1.0f) cfg.radio_rx_mhz = 1.0f;
                    if (cfg.radio_rx_mhz > 1300.0f) cfg.radio_rx_mhz = 1300.0f;
                    ui->rx_mhz = cfg.radio_rx_mhz;
                    radio_at_set_frequency(false, cfg.radio_rx_mhz);
                    break;
                case 1: /* TX freq */
                    cfg.radio_tx_mhz += direction * 0.005f;
                    if (cfg.radio_tx_mhz < 1.0f) cfg.radio_tx_mhz = 1.0f;
                    if (cfg.radio_tx_mhz > 1300.0f) cfg.radio_tx_mhz = 1300.0f;
                    ui->tx_mhz = cfg.radio_tx_mhz;
                    radio_at_set_frequency(true, cfg.radio_tx_mhz);
                    break;
                case 2: /* SQL */
                {
                    int sql = (int)cfg.squelch + direction;
                    if (sql < 0) sql = 0;
                    if (sql > 10) sql = 10;
                    cfg.squelch = (uint8_t)sql;
                    radio_at_set_squelch(cfg.squelch);
                    break;
                }
                case 3: /* PWR */
                {
                    int pwr = (int)cfg.tx_power + direction;
                    if (pwr < 0) pwr = 0;
                    if (pwr > 2) pwr = 2;
                    cfg.tx_power = (uint8_t)pwr;
                    radio_at_set_tx_power(cfg.tx_power);
                    break;
                }
                case 4: /* RF enable */
                    cfg.rf_enabled = !cfg.rf_enabled;
                    radio_at_set_rf_enabled(cfg.rf_enabled);
                    break;
                }
                config_store_save(&cfg);
            }
        } else if (ui->menu_index == 2) {
            /* CTCSS detail: rotate steps tone, press switches RX/TX */
            fmo_config_t cfg;
            config_store_load(&cfg);
            if (event == ENCODER_EVENT_PRESS) {
                ui->ctcss_adjust_field ^= 1;
            } else if (direction != 0) {
                float *target = ui->ctcss_adjust_field == 0
                    ? &cfg.rx_ctcss_hz : &cfg.tx_ctcss_hz;
                int idx = ctcss_find_index(*target);
                idx += direction;
                if (idx < 0) idx = 0;
                if (idx >= (int)UI_CTCSS_COUNT) idx = (int)UI_CTCSS_COUNT - 1;
                *target = k_ctcss_tones[idx];
                if (ui->ctcss_adjust_field == 0)
                    ui->rx_ctcss_hz = cfg.rx_ctcss_hz;
                else
                    ui->tx_ctcss_hz = cfg.tx_ctcss_hz;
                config_store_save(&cfg);
                nrl_audio_codec_configure_ctcss(cfg.rx_ctcss_hz, cfg.tx_ctcss_hz);
            }
        } else if (ui->menu_index == 5) {
            /* Audio detail: rotate adjusts field, press switches field */
            fmo_config_t cfg;
            config_store_load(&cfg);
            if (event == ENCODER_EVENT_PRESS) {
                ui->audio_adjust_field = (ui->audio_adjust_field + 1) % 7;
            } else if (direction != 0) {
                switch (ui->audio_adjust_field) {
                case 0: /* default TX network */
                    cfg.tx_network = cfg.tx_network == 0 ? 1 : 0;
                    audio_passthrough_set_tx_network(cfg.tx_network);
                    break;
                case 1: /* NRL codec toggle */
                    cfg.voice_codec = cfg.voice_codec == 0 ? 1 : 0;
                    audio_passthrough_set_voice_codec(cfg.voice_codec);
                    break;
                case 2: /* simultaneous RX policy */
                    cfg.audio_policy = cfg.audio_policy == 0 ? 1 : 0;
                    audio_passthrough_set_audio_policy(cfg.audio_policy);
                    break;
                case 3: /* RX vol */
                {
                    int vol = (int)cfg.rx_volume + direction;
                    if (vol < 0) vol = 0;
                    if (vol > 10) vol = 10;
                    cfg.rx_volume = (uint8_t)vol;
                    radio_at_set_volume(false, cfg.rx_volume);
                    break;
                }
                case 4: /* TX vol */
                {
                    int vol = (int)cfg.tx_volume + direction;
                    if (vol < 0) vol = 0;
                    if (vol > 10) vol = 10;
                    cfg.tx_volume = (uint8_t)vol;
                    radio_at_set_volume(true, cfg.tx_volume);
                    break;
                }
                case 5: /* software mic gain 1-5 */
                {
                    int gain = (int)cfg.mic_gain + direction;
                    if (gain < FMO_MIC_GAIN_MIN) gain = FMO_MIC_GAIN_MIN;
                    if (gain > FMO_MIC_GAIN_MAX) gain = FMO_MIC_GAIN_MAX;
                    cfg.mic_gain = (uint8_t)gain;
                    audio_passthrough_set_mic_gain(cfg.mic_gain);
                    break;
                }
                case 6: /* ES8311 REG13 HPSW */
                    cfg.es8311_hp_drive = !cfg.es8311_hp_drive;
                    (void)es8311_codec_set_headphone_drive(
                        cfg.es8311_hp_drive);
                    break;
                }
                config_store_save(&cfg);
            }
        } else if (ui->menu_index == 3) {
            /* APRS detail: press switches field, rotate toggles it */
            if (event == ENCODER_EVENT_PRESS) {
                ui->aprs_adjust_field =
                    (uint8_t)((ui->aprs_adjust_field + 1) % 11);
            } else if (direction != 0) {
                if (ui->aprs_adjust_field == 0) {
                    ui->aprs_enabled = !ui->aprs_enabled;
                    ui->aprs_change_pending = true;
                } else {
                    fmo_config_t cfg;
                    config_store_load(&cfg);
                    switch (ui->aprs_adjust_field) {
                    case 1: cfg.aprs_rf_rx = !cfg.aprs_rf_rx; break;
                    case 2: cfg.aprs_rf_tx = !cfg.aprs_rf_tx; break;
                    case 3: cfg.aprs_nrl_rx = !cfg.aprs_nrl_rx; break;
                    case 4: cfg.aprs_nrl_tx = !cfg.aprs_nrl_tx; break;
                    case 5: cfg.aprs_fwd ^= FMO_APRS_FWD_RF_TO_IS; break;
                    case 6: cfg.aprs_fwd ^= FMO_APRS_FWD_IS_TO_RF; break;
                    case 7: cfg.aprs_fwd ^= FMO_APRS_FWD_NRL_TO_IS; break;
                    case 8: cfg.aprs_fwd ^= FMO_APRS_FWD_IS_TO_NRL; break;
                    case 9: cfg.aprs_fwd ^= FMO_APRS_FWD_RF_TO_NRL; break;
                    case 10: cfg.aprs_fwd ^= FMO_APRS_FWD_NRL_TO_RF; break;
                    default: break;
                    }
                    config_store_save(&cfg);
                    aprs_service_update_config(&cfg);
                }
            }
        } else if (ui->menu_index == 4 &&
                   (direction != 0 || event == ENCODER_EVENT_PRESS)) {
            ui->espnow_enabled = !ui->espnow_enabled;
            ui->espnow_change_pending = true;
        } else if (ui->menu_index == 7 && direction != 0) {
            ui->chinese = !ui->chinese;
            ui->language_change_pending = true;
        } else if (ui->menu_index == 6 && event == ENCODER_EVENT_PRESS) {
            fmo_ota_ui_status_t ota = {0};
            ota_service_get_ui_status(&ota);
            if (!ota.checking && !ota.updating) {
                if (ota.latest_version[0] != '\0' &&
                    strcmp(ota.latest_version, FMO_FIRMWARE_VERSION) != 0) {
                    (void)ota_service_update_version(ota.latest_version);
                } else {
                    (void)ota_service_check_now();
                }
            }
        } else if (ui->menu_index == 8) {
            /* Net radio: rotate = select (live-switch while playing),
             * press = play/stop toggle */
            const int count = (int)net_radio_count();
            if (count > 0) {
                if (s_net_radio_sel < 0 || s_net_radio_sel >= count) {
                    const int cur = net_radio_get_current();
                    s_net_radio_sel = (cur >= 0 && cur < count) ? cur : 0;
                }
                if (direction != 0) {
                    s_net_radio_sel += direction;
                    if (s_net_radio_sel < 0) s_net_radio_sel = count - 1;
                    if (s_net_radio_sel >= count) s_net_radio_sel = 0;
                    if (net_radio_is_playing()) {
                        (void)net_radio_play((size_t)s_net_radio_sel);
                    }
                } else if (event == ENCODER_EVENT_PRESS) {
                    net_radio_status_t radio = {0};
                    net_radio_get_status(&radio);
                    if (net_radio_is_playing() &&
                        radio.current == s_net_radio_sel) {
                        net_radio_stop();
                    } else {
                        (void)net_radio_play((size_t)s_net_radio_sel);
                    }
                }
            }
        } else if (event == ENCODER_EVENT_PRESS) {
            ui->page = APP_UI_MENU;
        }
    }
}

static void render_network_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    gfx_text(canvas, 12, 30,
             tr(ui, "ESP-NOW intercom", "ESP-NOW \u5bf9\u8bb2"),
             2, COLOR_GRAY, COLOR_BLACK, 402);
    gfx_fill_rect(canvas, 10, 55, 408, 34,
                  ui->espnow_enabled ? COLOR_GREEN : COLOR_DARK);
    gfx_text(canvas, 24, 64,
             ui->espnow_enabled ? "ESP-NOW: ON" : "ESP-NOW: OFF",
             2, ui->espnow_enabled ? COLOR_BLACK : COLOR_WHITE,
             ui->espnow_enabled ? COLOR_GREEN : COLOR_DARK, 380);
    gfx_text(canvas, 12, 103,
             tr(ui, "Rotate/press: switch", "\u65cb\u8f6c/\u6309\u4e0b: \u5f00\u5173"),
             2, COLOR_ORANGE, COLOR_BLACK, 402);
    gfx_text(canvas, 12, 122,
             tr(ui, "Hold: back", "\u957f\u6309: \u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 402);
}

static void render_aprs_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    aprs_status_t status = {0};
    aprs_service_get_status(&status);
    fmo_config_t cfg = *cfg_get();
    const uint8_t field = ui->aprs_adjust_field;
    char line[64];

    const bool values[11] = {
        ui->aprs_enabled,
        cfg.aprs_rf_rx, cfg.aprs_rf_tx,
        cfg.aprs_nrl_rx, cfg.aprs_nrl_tx,
        (cfg.aprs_fwd & FMO_APRS_FWD_RF_TO_IS) != 0,
        (cfg.aprs_fwd & FMO_APRS_FWD_IS_TO_RF) != 0,
        (cfg.aprs_fwd & FMO_APRS_FWD_NRL_TO_IS) != 0,
        (cfg.aprs_fwd & FMO_APRS_FWD_IS_TO_NRL) != 0,
        (cfg.aprs_fwd & FMO_APRS_FWD_RF_TO_NRL) != 0,
        (cfg.aprs_fwd & FMO_APRS_FWD_NRL_TO_RF) != 0,
    };
    const char *labels[11] = {
        "APRS-IS",
        tr(ui, "RF decode", "RF解码"),
        tr(ui, "RF emit", "RF发射"),
        tr(ui, "NRL decode", "NRL解码"),
        tr(ui, "NRL emit", "NRL发射"),
        "RF>IS", "IS>RF", "NRL>IS", "IS>NRL", "RF>NRL", "NRL>RF",
    };

    for (int i = 0; i < 11; ++i) {
        const bool selected = i == field;
        const uint16_t label_color = selected ? COLOR_ORANGE : COLOR_WHITE;
        const uint16_t value_color = values[i] ? COLOR_GREEN : COLOR_GRAY;
        /* Two columns: items 0-4 (IS/decode/emit) on the left, the six
         * forwarding switches on the right; six 16px rows fit the 96px
         * content area at scale 2. */
        const int x = i < 5 ? 8 : 220;
        const int y = 24 + (i < 5 ? i : i - 5) * 16;
        snprintf(line, sizeof(line), "%c %s:", selected ? '>' : ' ',
                 labels[i]);
        gfx_text(canvas, x, y, line, 2, label_color, COLOR_BLACK, 205);
        const int value_x = x + gfx_text_width(line, 2);
        gfx_text(canvas, value_x, y,
                 values[i] ? "ON" : "OFF", 2, value_color, COLOR_BLACK, 60);
    }
    /* Live IS link / counters fill the spare row under the left column */
    snprintf(line, sizeof(line), "%s RX:%lu TX:%lu",
             status.connected ? "IS:LINK" : "IS:--",
             (unsigned long)status.rx_count,
             (unsigned long)status.tx_count);
    gfx_text(canvas, 8, 24 + 5 * 16, line, 2,
             status.connected ? COLOR_GREEN : COLOR_GRAY,
             COLOR_BLACK, 205);
    gfx_text(canvas, 10, 122,
             tr(ui, "Rotate:switch Press:item Hold:back",
                    "旋转:开关 按下:选项 长按:返回"),
             2, COLOR_GRAY, COLOR_BLACK, 402);
}

static void render_header(gfx_canvas_t *canvas, const char *title)
{
    network_status_t network = {0};
    char signal[12];
    network_manager_get_status(&network);
    if (network.station_connected) {
        snprintf(signal, sizeof(signal), "%ddB", (int)network.wifi_rssi_dbm);
    } else {
        strlcpy(signal, "--dB", sizeof(signal));
    }
    const uint16_t wifi_color = network.station_connected ? COLOR_GREEN : COLOR_GRAY;
    gfx_fill_rect(canvas, 0, 0, canvas->width, 21, COLOR_BLACK);
    /* Wi-Fi RSSI right-aligned; title (freq/CTCSS) fills the remaining space */
    const int sig_w = gfx_text_width(signal, 2);
    const int sig_x = canvas->width - sig_w - 2;
    gfx_text(canvas, 3, 3, title, 2, COLOR_WHITE, COLOR_BLACK, sig_x - 3 - 4);
    gfx_text(canvas, sig_x, 3, signal, 2, wifi_color, COLOR_BLACK, sig_w + 2);
    gfx_hline(canvas, 0, 20, canvas->width, COLOR_GRAY);
}

static void format_server(char *out, size_t out_size, const nrl_server_t *server)
{
    if (server == NULL) {
        strlcpy(out, "-", out_size);
    } else if (server->total > 0) {
        snprintf(out, out_size, "%s %u/%u", server->name,
                 (unsigned)server->online, (unsigned)server->total);
    } else {
        strlcpy(out, server->name, out_size);
    }
}

static void format_fmo_server(char *out, size_t out_size,
                              const fmo_server_t *server)
{
    if (server == NULL) {
        strlcpy(out, "FMO: -", out_size);
    } else if (server->total > 0) {
        snprintf(out, out_size, "%s %u/%u", server->name,
                 (unsigned)server->online, (unsigned)server->total);
    } else {
        strlcpy(out, server->name, out_size);
    }
}

static void format_fmo_server_list(char *out, size_t out_size,
                                   const fmo_server_t *server)
{
    if (server == NULL) {
        strlcpy(out, "FMO: -", out_size);
        return;
    }
    char identity[24];
    if (server->has_ssid) {
        snprintf(identity, sizeof(identity), "%s-%u", server->callsign,
                 (unsigned)server->ssid);
    } else {
        strlcpy(identity, server->callsign, sizeof(identity));
    }
    if (server->total > 0) {
        snprintf(out, out_size, "%s / %s %u/%u", server->name, identity,
                 (unsigned)server->online, (unsigned)server->total);
    } else {
        snprintf(out, out_size, "%s / %s", server->name, identity);
    }
}

static void draw_marquee(gfx_canvas_t *canvas, int x, int y, int width,
                         const char *text, uint16_t foreground,
                         uint16_t background)
{
    const int text_width = gfx_text_width(text, 2);
    gfx_set_clip(canvas, x, y, width, 18);
    if (text_width <= width) {
        gfx_text(canvas, x, y, text, 2, foreground, background, 0);
    } else {
        char repeated[336];
        snprintf(repeated, sizeof(repeated), "%s   %s", text, text);
        const int gap_width = gfx_text_width("   ", 2);
        const int cycle = text_width + gap_width;
        /* Pause at left-aligned position, then scroll. */
        const int64_t pause_us = 1500000;  /* 1.5 s hold */
        const int64_t scroll_us = (int64_t)cycle * 40000; /* same speed */
        const int64_t total_us = pause_us + scroll_us;
        const int64_t phase = esp_timer_get_time() % total_us;
        int offset;
        if (phase < pause_us) {
            offset = 0;  /* paused, text left-aligned */
        } else {
            offset = (int)((phase - pause_us) / 40000);
            if (offset > cycle) offset = cycle;
        }
        gfx_text(canvas, x - offset, y, repeated, 2,
                 foreground, background, 0);
    }
    gfx_reset_clip(canvas);
}

/* Small radio pictogram (black on the orange server box), ~13x14 px:
 * antenna diagonal, body outline, speaker block, tuner lines. */
static void draw_radio_icon(gfx_canvas_t *canvas, int x, int y)
{
    gfx_pixel(canvas, x + 10, y, COLOR_BLACK);
    gfx_pixel(canvas, x + 9, y + 1, COLOR_BLACK);
    gfx_pixel(canvas, x + 8, y + 2, COLOR_BLACK);
    gfx_pixel(canvas, x + 7, y + 3, COLOR_BLACK);
    gfx_rect(canvas, x, y + 4, 13, 10, COLOR_BLACK);
    gfx_fill_rect(canvas, x + 2, y + 7, 3, 4, COLOR_BLACK);
    gfx_hline(canvas, x + 7, y + 7, 4, COLOR_BLACK);
    gfx_hline(canvas, x + 7, y + 10, 4, COLOR_BLACK);
}

static void render_provision(const app_ui_t *ui, gfx_canvas_t *canvas,
                             const network_status_t *net)
{
    gfx_text(canvas, 12, 22,
             tr(ui, "WiFi Provisioning", "\u914d\u7f51\u63d0\u793a"),
             3, COLOR_ORANGE, COLOR_BLACK, 400);
    char line[64];
    snprintf(line, sizeof(line), "WiFi: %s", net->config_ap_ssid);
    gfx_text(canvas, 12, 55, line, 2, COLOR_GREEN, COLOR_BLACK, 400);
    gfx_text(canvas, 12, 75,
             tr(ui, "Password: none (open)",
                    "\u5bc6\u7801: \u65e0\uff08\u5f00\u653e\uff09"),
             2, COLOR_WHITE, COLOR_BLACK, 400);
    gfx_text(canvas, 12, 95,
             tr(ui, "Open browser: 192.168.4.1",
                    "\u6d4f\u89c8\u5668\u6253\u5f00: 192.168.4.1"),
             2, COLOR_WHITE, COLOR_BLACK, 400);
    gfx_text(canvas, 12, 118,
             tr(ui, "After saving WiFi, device restarts",
                    "\u4fdd\u5b58WiFi\u540e\u8bbe\u5907\u81ea\u52a8\u91cd\u8fde"),
             2, COLOR_GRAY, COLOR_BLACK, 400);
    gfx_text(canvas, 12, 138, "BLE: OpenFMO-CFG",
             2, COLOR_GREEN, COLOR_BLACK, 400);
}

/* Server scroll preview: while the knob is turning (and for 3s after the
 * last click), one protocol owns the complete right-hand server/APRS area.
 * This gives the current server and the next two candidates three equally
 * sized, readable rows instead of squeezing candidates into tiny text. */
static void render_server_preview(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    const int x = 210;
    const int y = 23;
    const int width = 216;
    const int row_height = 26;
    const int visible_rows = 3;
    const bool is_fmo = ui->server_select_mode == 2;
    const uint16_t accent = is_fmo ? COLOR_ORANGE : COLOR_PURPLE;

    gfx_fill_rect(canvas, x, y, width, row_height * visible_rows,
                  COLOR_BLACK);

    if (is_fmo) {
        const size_t count = fmo_server_directory_favorite_count();
        if (count == 0) {
            gfx_text(canvas, x + 6, y + row_height, "FMO: no favorites", 2,
                     COLOR_ORANGE, COLOR_BLACK, width - 12);
            return;
        }
        const int rows = count < (size_t)visible_rows
            ? (int)count : visible_rows;
        for (int row = 0; row < rows; ++row) {
            const size_t index =
                (ui->fmo_favorite_index + (size_t)row) % count;
            const fmo_server_t *entry =
                fmo_server_directory_get_favorite(index);
            if (entry != NULL) {
                char line[160];
                format_fmo_server_list(line, sizeof(line), entry);
                const bool selected = row == 0;
                const int row_y = y + row * row_height;
                if (selected) {
                    gfx_fill_rect(canvas, x, row_y, width, row_height - 1,
                                  accent);
                } else {
                    gfx_hline(canvas, x, row_y, width, accent);
                }
                gfx_set_clip(canvas, x + 6, row_y + 5,
                             width - 12, row_height - 6);
                gfx_text(canvas, x + 6, row_y + 5, line, 2,
                         selected ? COLOR_BLACK : accent,
                         selected ? accent : COLOR_BLACK, 0);
                gfx_reset_clip(canvas);
            }
        }
        return;
    }
    const size_t count = server_directory_count();
    if (count == 0) return;
    const int rows = count < (size_t)visible_rows ? (int)count : visible_rows;
    for (int row = 0; row < rows; ++row) {
        const size_t idx = (ui->server_index + (size_t)row) % count;
        const nrl_server_t *entry = server_directory_get(idx);
        if (entry == NULL) continue;
        char line[120];
        format_server(line, sizeof(line), entry);
        const bool selected = row == 0;
        const int row_y = y + row * row_height;
        if (selected) {
            gfx_fill_rect(canvas, x, row_y, width, row_height - 1, accent);
        } else {
            gfx_hline(canvas, x, row_y, width, accent);
        }
        gfx_set_clip(canvas, x + 6, row_y + 5,
                     width - 12, row_height - 6);
        gfx_text(canvas, x + 6, row_y + 5, line, 2,
                 selected ? COLOR_WHITE : accent,
                 selected ? accent : COLOR_BLACK, 0);
        gfx_reset_clip(canvas);
    }
}

/* Station list counterpart of render_server_preview: shown while the net
 * radio is playing and the knob is being turned. */
static void render_station_preview(gfx_canvas_t *canvas)
{
    const int count = (int)net_radio_count();
    if (count <= 0) return;
    const int current = net_radio_get_current();
    for (int row = 0; row < 2; ++row) {
        int idx = (current + 1 + row) % count;
        if (idx < 0) idx += count;
        char name[NET_RADIO_NAME_MAX];
        if (!net_radio_get((size_t)idx, name, sizeof(name), NULL, 0)) {
            continue;
        }
        gfx_text(canvas, 212, 80 + row * 10, name, 1,
                 COLOR_GRAY, COLOR_BLACK, 214);
    }
}

/* FMO QSO signaling overlay: incoming-call popup (knob short press answers,
 * long press rejects), outgoing-call progress, and an established marker. */
static void render_qso_overlay(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    fmo_qso_status_t qso = {0};
    fmo_qso_get_status(&qso);
    switch (qso.phase) {
    case FMO_QSO_PHASE_INCOMING: {
        const bool blink = ((esp_timer_get_time() / 500000LL) & 1) == 0;
        const uint16_t bg = blink ? COLOR_RED : COLOR_ORANGE;
        gfx_fill_rect(canvas, 20, 40, 388, 62, bg);
        gfx_rect(canvas, 20, 40, 388, 62, COLOR_WHITE);
        gfx_text(canvas, 32, 46, tr(ui, "FMO incoming call",
                                    "FMO \u6765\u7535"),
                 2, COLOR_BLACK, bg, 180);
        gfx_text(canvas, 32, 64, qso.peer, 2, COLOR_BLACK, bg, 240);
        gfx_text(canvas, 32, 84,
                 tr(ui, "press = answer, hold = reject",
                    "\u77ed\u6309\u63a5\u542c  \u957f\u6309\u62d2\u7edd"),
                 1, COLOR_BLACK, bg, 364);
        break;
    }
    case FMO_QSO_PHASE_QUERYING:
    case FMO_QSO_PHASE_JUMPING:
    case FMO_QSO_PHASE_CALLING:
    case FMO_QSO_PHASE_RINGING:
    case FMO_QSO_PHASE_FAILED: {
        const bool failed = qso.phase == FMO_QSO_PHASE_FAILED;
        const uint16_t border = failed ? COLOR_RED : COLOR_ORANGE;
        gfx_fill_rect(canvas, 60, 50, 308, 42, COLOR_BLACK);
        gfx_rect(canvas, 60, 50, 308, 42, border);
        char line[64];
        snprintf(line, sizeof(line), "%s %s",
                 failed ? tr(ui, "call failed", "\u547c\u53eb\u5931\u8d25")
                        : tr(ui, "calling", "\u547c\u53eb\u4e2d"),
                 qso.peer);
        gfx_text(canvas, 68, 56, line, 2, border, COLOR_BLACK, 292);
        gfx_text(canvas, 68, 76, qso.detail, 1, COLOR_WHITE, COLOR_BLACK,
                 292);
        break;
    }
    case FMO_QSO_PHASE_ESTABLISHED: {
        char line[40];
        snprintf(line, sizeof(line), "QSO:%s", qso.peer);
        gfx_fill_rect(canvas, 8, 61, 198, 18, COLOR_BLACK);
        gfx_text(canvas, 8, 63, line, 2, COLOR_ORANGE, COLOR_BLACK, 198);
        break;
    }
    default:
        break;
    }
}

static void render_main(const app_ui_t *ui, gfx_canvas_t *canvas)
{    char top[64];
    char tx_ctcss[12];
    char rx_ctcss[12];
    char server_line[160];
    char footer[80];
    nrl_remote_identity_t identity = {0};
    nrl_audio_ctcss_status_t ctcss = {0};
    network_status_t network = {0};
    fmo_config_t cfg = *cfg_get();
    audio_network_status_t voice = {0};
    audio_passthrough_get_network_status(&voice);
    /* Always show the knob selection; main.c syncs ui->server_index from the
     * saved config whenever it changes externally (web/AT). */
    const size_t disp_server_idx = ui->server_index;
    const nrl_server_t *server = server_directory_get(disp_server_idx);
    /* Knob turning (and 3s after): show scroll preview, freeze the marquee */
    const bool server_preview =
        esp_timer_get_time() < ui->server_preview_until_us;
    const fmo_server_t *fmo_server = NULL;
    if (server_preview && ui->server_select_mode == 2) {
        fmo_server =
            fmo_server_directory_get_favorite(ui->fmo_favorite_index);
    } else {
        size_t index = fmo_server_directory_find(cfg.fmo_server_key);
        if (index != SIZE_MAX) fmo_server = fmo_server_directory_get(index);
    }
    network_manager_get_status(&network);
    nrl_audio_codec_get_ctcss_status(&ctcss);

    /* Show provisioning page when WiFi is unconfigured */
    if (network.config_ap_running && !network.station_connected) {
        render_provision(ui, canvas, &network);
        return;
    }

    const float tx_ctcss_hz = ctcss.configured
        ? ctcss.tx_hz : cfg.tx_ctcss_hz;
    const float rx_ctcss_hz = ctcss.configured
        ? ctcss.rx_expected_hz : cfg.rx_ctcss_hz;

    if (tx_ctcss_hz > 0.0f) {
        snprintf(tx_ctcss, sizeof(tx_ctcss), "%.1f", tx_ctcss_hz);
    } else {
        strlcpy(tx_ctcss, "OFF", sizeof(tx_ctcss));
    }
    if (rx_ctcss_hz > 0.0f) {
        snprintf(rx_ctcss, sizeof(rx_ctcss), "%.1f", rx_ctcss_hz);
    } else {
        strlcpy(rx_ctcss, "OFF", sizeof(rx_ctcss));
    }
    snprintf(top, sizeof(top), "T%.3f/%s R%.3f/%s",
             cfg.radio_tx_mhz, tx_ctcss, cfg.radio_rx_mhz, rx_ctcss);
    render_header(canvas, top);
    const bool have_identity = nrl_link_get_last_identity(&identity);
    const bool have_voice = voice.nrl_active || voice.fmo_active;
    const bool rf_active = status_io_is_sql_active();
    const bool fmo_primary = have_voice &&
                             voice.primary == AUDIO_NETWORK_FMO;
    const bool nrl_identity_visible = have_voice && !fmo_primary &&
                                      have_identity;
    const bool standby = !have_voice && !have_identity && !rf_active;
    const bool standby_fmo = standby &&
        ((esp_timer_get_time() / 2000000LL) & 1LL) != 0;
    uint8_t fmo_voice_ssid = 0;
    const bool have_fmo_voice_ssid = voice.fmo_active &&
        fmo_discovery_lookup_ssid(voice.fmo_callsign, &fmo_voice_ssid);
    char fmo_voice_label[24];
    char nrl_voice_label[24];
    if (have_fmo_voice_ssid) {
        snprintf(fmo_voice_label, sizeof(fmo_voice_label), "%s-%u",
                 voice.fmo_callsign, (unsigned)fmo_voice_ssid);
    } else {
        strlcpy(fmo_voice_label, voice.fmo_callsign,
                sizeof(fmo_voice_label));
    }
    if (voice.nrl_active && have_identity &&
        strcmp(voice.nrl_callsign, identity.callsign) == 0) {
        snprintf(nrl_voice_label, sizeof(nrl_voice_label), "%s-%u",
                 voice.nrl_callsign, (unsigned)identity.ssid);
    } else {
        strlcpy(nrl_voice_label, voice.nrl_callsign,
                sizeof(nrl_voice_label));
    }
    const char *display_callsign = have_voice
        ? (fmo_primary ? voice.fmo_callsign
                       : (nrl_identity_visible ? identity.callsign
                                               : voice.nrl_callsign))
        : (standby_fmo ? cfg.fmo_callsign
                       : (have_identity ? identity.callsign : cfg.callsign));
    const uint8_t display_ssid = nrl_identity_visible ? identity.ssid :
        (fmo_primary && have_fmo_voice_ssid ? fmo_voice_ssid :
         (have_voice ? 0 : (standby_fmo ? cfg.fmo_callsign_ssid
                                       : (have_identity ? identity.ssid
                                                        : cfg.callsign_ssid))));
    const uint16_t callsign_color = have_voice
        ? (fmo_primary ? COLOR_ORANGE : COLOR_PURPLE)
        : (standby ? (standby_fmo ? COLOR_ORANGE : COLOR_PURPLE)
                   : (have_identity ? COLOR_PURPLE : COLOR_GREEN));
    /* Callsign blinks when RF squelch is active */
    const bool blink_on = !rf_active ||
        ((esp_timer_get_time() / 250000) & 1) == 0;
    if (blink_on) {
        gfx_text(canvas, 8, 28, display_callsign, 4,
                 callsign_color, COLOR_BLACK, 194);
    }
    char ssid_text[5];
    snprintf(ssid_text, sizeof(ssid_text), "%u", (unsigned)display_ssid);
    int ssid_x = 8 + gfx_text_width(display_callsign, 4) + 3;
    if (ssid_x < 196) {
        /* Show detected CTCSS tone when RF active, else network codec */
        if (rf_active) {
            nrl_audio_ctcss_status_t ctcss_st = {0};
            nrl_audio_codec_get_ctcss_status(&ctcss_st);
            char tone_text[12];
            if (ctcss_st.rx_detected_hz > 0.0f) {
                snprintf(tone_text, sizeof(tone_text), "%.1f",
                         (double)ctcss_st.rx_detected_hz);
            } else {
                strlcpy(tone_text, "--.-", sizeof(tone_text));
            }
            gfx_text(canvas, ssid_x, 28, tone_text, 2,
                     COLOR_GREEN, COLOR_BLACK, 206 - ssid_x);
        } else if (have_voice) {
            const char *codec = fmo_primary ? voice.fmo_codec : voice.nrl_codec;
            gfx_text(canvas, ssid_x, 28, codec, 2,
                     fmo_primary ? COLOR_ORANGE : COLOR_PURPLE,
                     COLOR_BLACK, 206 - ssid_x);
        } else if (standby) {
            gfx_text(canvas, ssid_x, 28, standby_fmo ? "FMO" : "NRL", 2,
                     standby_fmo ? COLOR_ORANGE : COLOR_PURPLE,
                     COLOR_BLACK, 206 - ssid_x);
        } else {
            int rx_codec = nrl_audio_codec_get_rx_codec();
            if (rx_codec >= 0) {
                const char *codec_label = rx_codec == 1 ? "OPUS" : "G711";
                uint16_t codec_color = rx_codec == 1 ? COLOR_PURPLE : COLOR_GREEN;
                gfx_text(canvas, ssid_x, 28, codec_label, 2,
                         codec_color, COLOR_BLACK, 206 - ssid_x);
            }
        }
        /* SSID: scale 2, below codec with gap. Codec h=14 @y=28 → bottom=42,
         * gap=3 → SSID y=45, h=14, bottom=59. */
        if (!have_voice || nrl_identity_visible ||
            (fmo_primary && have_fmo_voice_ssid)) {
            gfx_text(canvas, ssid_x, 45, ssid_text, 2,
                     callsign_color, COLOR_BLACK, 206 - ssid_x);
        }
    }

    gfx_fill_rect(canvas, 210, 23, 216, 27, COLOR_PURPLE);
    /* While the net radio is on air, this box shows the station name with a
     * radio icon instead of the NRL server. */
    net_radio_status_t radio_st = {0};
    net_radio_get_status(&radio_st);
    if (!voice.nrl_active && net_radio_is_playing() &&
        radio_st.station_name[0] != '\0') {
        draw_radio_icon(canvas, 215, 30);
        if (server_preview) {
            /* Hold the name still while switching so it stays readable */
            gfx_set_clip(canvas, 233, 29, 189, 18);
            gfx_text(canvas, 233, 29, radio_st.station_name, 2,
                     COLOR_WHITE, COLOR_PURPLE, 0);
            gfx_reset_clip(canvas);
        } else {
            draw_marquee(canvas, 233, 29, 189, radio_st.station_name,
                         COLOR_WHITE, COLOR_PURPLE);
        }
    } else {
        if (voice.nrl_active) {
            snprintf(server_line, sizeof(server_line), "%s %s",
                     nrl_voice_label, voice.nrl_codec);
        } else {
            format_server(server_line, sizeof(server_line), server);
        }
        if (server_preview) {
            /* Hold the name still while switching so it stays readable */
            gfx_set_clip(canvas, 216, 29, 204, 18);
            gfx_text(canvas, 216, 29, server_line, 2,
                     COLOR_WHITE, COLOR_PURPLE, 0);
            gfx_reset_clip(canvas);
        } else {
            draw_marquee(canvas, 216, 29, 204, server_line,
                         COLOR_WHITE, COLOR_PURPLE);
        }
    }

    gfx_fill_rect(canvas, 210, 51, 216, 27, COLOR_ORANGE);
    if (voice.fmo_active) {
        snprintf(server_line, sizeof(server_line), "%s %s",
                 fmo_voice_label, voice.fmo_codec);
    } else {
        format_fmo_server(server_line, sizeof(server_line), fmo_server);
    }
    if (server_preview) {
        gfx_set_clip(canvas, 216, 57, 204, 18);
        gfx_text(canvas, 216, 57, server_line, 2,
                 COLOR_BLACK, COLOR_ORANGE, 0);
        gfx_reset_clip(canvas);
    } else {
        draw_marquee(canvas, 216, 57, 204, server_line,
                     COLOR_BLACK, COLOR_ORANGE);
    }

    char dmr_info[32];
    char mdc_info[40];
    mdc_signal_status_t mdc = {0};
    const bool have_recent_mdc = mdc_signaling_get_recent(&mdc);
    if (have_identity && identity.dmr_id != 0) {
        snprintf(dmr_info, sizeof(dmr_info), "DMR:%lu",
                 (unsigned long)identity.dmr_id);
    } else {
        strlcpy(dmr_info, "DMR:-------", sizeof(dmr_info));
    }
    if (have_recent_mdc) {
        snprintf(mdc_info, sizeof(mdc_info),
                 "MDC:%04X OP:%02X ARG:%02X", mdc.unit_id,
                 mdc.opcode, mdc.argument);
    } else {
        strlcpy(mdc_info, "MDC:---- -- --", sizeof(mdc_info));
    }
    gfx_text(canvas, 8, 61, dmr_info, 2,
             identity.dmr_id != 0 ? COLOR_ORANGE : COLOR_WHITE,
             COLOR_BLACK, 198);
    gfx_text(canvas, 8, 81, mdc_info, 2,
             have_recent_mdc ? COLOR_ORANGE : COLOR_WHITE,
             COLOR_BLACK, 198);

    fmo_ota_ui_status_t ota = {0};
    ota_service_get_ui_status(&ota);
    if (ota.updating) {
        /* OTA in progress: show live progress instead of the APRS list so
         * the status stays visible even if the user left the OTA page. */
        char ota_line[32];
        snprintf(ota_line, sizeof(ota_line), "%s %u%%",
                 tr(ui, "Upgrading", "\u6b63\u5728\u5347\u7ea7"),
                 (unsigned)ota.update_percent);
        gfx_text(canvas, 214, 81, ota_line, 1,
                 COLOR_ORANGE, COLOR_BLACK, 210);
        const int bar_x = 214, bar_y = 92, bar_w = 208, bar_h = 7;
        gfx_rect(canvas, bar_x, bar_y, bar_w, bar_h, COLOR_GRAY);
        const int fill_w = (bar_w - 4) * (int)ota.update_percent / 100;
        if (fill_w > 0) {
            gfx_fill_rect(canvas, bar_x + 2, bar_y + 2, fill_w,
                          bar_h - 4, COLOR_GREEN);
        }
    } else if (server_preview) {
        if (net_radio_is_playing()) render_station_preview(canvas);
        else render_server_preview(ui, canvas);
    } else {
        aprs_recent_packet_t recent[2] = {0};
        const size_t recent_count = aprs_service_get_recent(recent, 2);
        if (recent_count == 0) {
            if (cfg.aprs_enabled) {
                gfx_text(canvas, 214, 82,
                         tr(ui, "APRS: waiting", "APRS: 等待数据"),
                         2, COLOR_GRAY, COLOR_BLACK, 210);
            } else {
                gfx_text(canvas, 214, 82,
                         tr(ui, "APRS: OFF", "APRS: 未启用"),
                         2, COLOR_DARK, COLOR_BLACK, 210);
            }
        } else {
            for (size_t i = 0; i < recent_count; ++i) {
                char distance[12];
                char course[8];
                char speed[12];
                char line[56];
                if (recent[i].has_distance) {
                    if (recent[i].distance_km < 1000.0f) {
                        snprintf(distance, sizeof(distance), "%.1fKM",
                                 recent[i].distance_km);
                    } else {
                        snprintf(distance, sizeof(distance), "%.0fKM",
                                 recent[i].distance_km);
                    }
                } else {
                    strlcpy(distance, "--.-KM", sizeof(distance));
                }
                if (recent[i].has_course) {
                    snprintf(course, sizeof(course), "%03.0f\xc2\xb0",
                             (double)recent[i].course_deg);
                } else {
                    strlcpy(course, "---\xc2\xb0", sizeof(course));
                }
                if (recent[i].has_speed) {
                    snprintf(speed, sizeof(speed), "%.0fKM/H", recent[i].speed_kmh);
                } else {
                    strlcpy(speed, "--KM/H", sizeof(speed));
                }
                snprintf(line, sizeof(line), "%-10.10s %8s %5s %8s",
                         recent[i].callsign, distance, course, speed);
                gfx_text(canvas, 212, 80 + (int)i * 10, line, 1,
                         i == 0 ? COLOR_ORANGE : COLOR_WHITE,
                         COLOR_BLACK, 214);
            }
        }
    }

    gfx_hline(canvas, 0, 100, canvas->width, COLOR_GRAY);
    app_notice_t notice = {0};
    const bool have_notice = app_notice_get(&notice);
    const char *status;
    char status_buffer[160];
    nrl_link_status_t nrl_status = {0};
    fmo_link_status_t fmo_status = {0};
    nrl_link_get_status(&nrl_status);
    fmo_link_get_status(&fmo_status);
    uint16_t status_color = COLOR_WHITE;
    if (have_notice) {
        status = notice.text;
        status_color = notice.kind == APP_NOTICE_APRS ? COLOR_ORANGE : COLOR_WHITE;
    } else if (ctcss.rx_expected_hz > 0.0f &&
               ctcss.rx_state == CTCSS_GATE_REJECTED) {
        status = tr(ui, "CTCSS mismatch - RF audio rejected",
                    "\u4e9a\u97f3\u9519\u8bef - \u5df2\u62d2\u7edd\u5c04\u9891\u8bed\u97f3");
        status_color = COLOR_RED;
    } else if (status_io_is_network_ptt()) {
        if (voice.nrl_active && voice.fmo_active) {
            status = cfg.audio_policy == 0
                ? tr(ui, "NRL + FMO mixed audio",
                     "NRL + FMO \u6df7\u97f3\u63a5\u6536")
                : tr(ui, "Dual audio: first arrival priority",
                     "\u53cc\u8def\u6765\u8bdd: \u5148\u6765\u4f18\u5148");
        } else if (voice.fmo_active) {
            status = tr(ui, "FMO audio transmitting to radio",
                        "FMO \u6765\u8bdd - \u6b63\u5728\u5411\u5c04\u9891\u53d1\u5c04");
        } else {
            status = tr(ui, "NRL audio transmitting to radio",
                        "NRL \u6765\u8bdd - \u6b63\u5728\u5411\u5c04\u9891\u53d1\u5c04");
        }
        status_color = voice.fmo_active && !voice.nrl_active
            ? COLOR_ORANGE : COLOR_PURPLE;
    } else if (status_io_is_sql_active()) {
        status = tr(ui, "Radio signal receiving",
                    "\u6536\u5230\u5c04\u9891\u4fe1\u53f7");
        status_color = COLOR_GREEN;
    } else if (!ui->radio_present) {
        status = tr(ui, "Radio module unavailable", "\u5c04\u9891\u6a21\u5757\u672a\u5c31\u7eea");
        status_color = COLOR_RED;
    } else if (!network.station_connected) {
        status = tr(ui, "Wi-Fi disconnected",
                    "Wi-Fi \u672a\u8fde\u63a5");
        status_color = COLOR_RED;
    } else if (!nrl_status.online) {
        status = nrl_status.socket_ready
            ? tr(ui, "NRL server is not responding",
                 "NRL \u670d\u52a1\u5668\u65e0\u54cd\u5e94")
            : tr(ui, "NRL server connection error",
                 "NRL \u670d\u52a1\u5668\u8fde\u63a5\u5f02\u5e38");
        status_color = COLOR_RED;
    } else if (cfg.fmo_server_key[0] == '\0') {
        status = tr(ui, "FMO server not selected",
                    "FMO \u672a\u9009\u62e9\u670d\u52a1\u5668");
        status_color = COLOR_RED;
    } else if (!fmo_status.configured ||
               fmo_status.last_error == ESP_ERR_NOT_FOUND) {
        status = tr(ui, "FMO server configuration error",
                    "FMO \u670d\u52a1\u5668\u914d\u7f6e\u5f02\u5e38");
        status_color = COLOR_RED;
    } else if (!fmo_status.connected &&
               fmo_status.last_error == ESP_ERR_INVALID_STATE) {
        status = tr(ui, "FMO certificate/SAS is not ready",
                    "FMO \u8bc1\u4e66/SAS \u672a\u5c31\u7eea");
        status_color = COLOR_RED;
    } else if (!fmo_status.connected && fmo_status.last_error != ESP_OK) {
        status = tr(ui, "FMO server connection error",
                    "FMO \u670d\u52a1\u5668\u8fde\u63a5\u5f02\u5e38");
        status_color = COLOR_RED;
    } else if (!fmo_status.connected) {
        status = tr(ui, "FMO server connecting",
                    "FMO \u670d\u52a1\u5668\u8fde\u63a5\u4e2d");
        status_color = COLOR_ORANGE;
    } else {
        snprintf(status_buffer, sizeof(status_buffer), "%s",
                tr(ui,
                    "Radio standby / NRL server online / FMO server connected",
                    "\u5c04\u9891\u5f85\u673a / NRL\u670d\u52a1\u5668\u5728\u7ebf / FMO\u670d\u52a1\u5668\u5df2\u8fde\u63a5"));
        status = status_buffer;
        status_color = COLOR_GREEN;
    }
    draw_marquee(canvas, 5, 105, 418, status,
                 status_color, COLOR_BLACK);
    gfx_hline(canvas, 0, 124, canvas->width, COLOR_GRAY);
    const char *ip_address = network.station_connected && network.ip_address[0] != '\0'
        ? network.ip_address
        : network.config_ap_running ? "192.168.4.1" : "--";
    snprintf(footer, sizeof(footer), "IP:%s", ip_address);
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {  /* NTP synced (year > 2000) */
        snprintf(footer + strlen(footer), sizeof(footer) - strlen(footer),
                 "  %04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }
    gfx_text(canvas, 5, 129, footer, 1, COLOR_WHITE, COLOR_BLACK, 310);

    /* Bottom-right: VU meters + CPU usage */
    uint8_t vu_mic = 0, vu_spk = 0;
    status_io_get_vu(&vu_mic, &vu_spk);
    /* CPU usage: compare per-core idle counter delta to wall-clock elapsed */
    static uint32_t prev_idle0, prev_idle1;
    static int64_t prev_cpu_time_us;
    uint32_t idle0 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
    uint32_t idle1 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
    int64_t now_cpu_us = esp_timer_get_time();
    int64_t elapsed_us = now_cpu_us - prev_cpu_time_us;
    uint8_t cpu0 = 0, cpu1 = 0;
    if (elapsed_us > 10000 && prev_cpu_time_us != 0) {
        uint32_t di0 = idle0 - prev_idle0;
        uint32_t di1 = idle1 - prev_idle1;
        uint32_t el = (uint32_t)(elapsed_us > 10000000 ? 10000000 : elapsed_us);
        uint32_t idle0_pct = (di0 * 100) / el;
        uint32_t idle1_pct = (di1 * 100) / el;
        cpu0 = (uint8_t)(idle0_pct >= 100 ? 0 : 100 - idle0_pct);
        cpu1 = (uint8_t)(idle1_pct >= 100 ? 0 : 100 - idle1_pct);
    }
    prev_idle0 = idle0;
    prev_idle1 = idle1;
    prev_cpu_time_us = now_cpu_us;
    char cpu_text[16];
    snprintf(cpu_text, sizeof(cpu_text), "%u/%u%%", cpu0, cpu1);
    gfx_text(canvas, 312, 129, cpu_text, 1, COLOR_GRAY, COLOR_BLACK, 50);
    /* MIC VU bar */
    gfx_text(canvas, 350, 125, "M", 1, COLOR_GREEN, COLOR_BLACK, 8);
    gfx_fill_rect(canvas, 360, 125, 65, 5, COLOR_DARK);
    int mic_w = (vu_mic * 65) / 255;
    if (mic_w > 0) gfx_fill_rect(canvas, 360, 125, mic_w, 5, COLOR_GREEN);
    /* SPK VU bar */
    gfx_text(canvas, 350, 133, "S", 1, COLOR_ORANGE, COLOR_BLACK, 8);
    gfx_fill_rect(canvas, 360, 133, 65, 5, COLOR_DARK);
    int spk_w = (vu_spk * 65) / 255;
    if (spk_w > 0) gfx_fill_rect(canvas, 360, 133, spk_w, 5, COLOR_ORANGE);

    /* FMO QSO signaling overlay covers everything else while active */
    render_qso_overlay(ui, canvas);
}

static void render_menu(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    render_header(canvas, tr(ui, "Settings", "\u8bbe\u7f6e\u83dc\u5355"));
    for (int index = 0; index < k_menu_count; ++index) {
        int column = index & 1;
        int row = index / 2;
        int x = 5 + column * 211;
        int y = 23 + row * 23;  /* 5 rows must fit the 142px-high canvas */
        bool selected = index == ui->menu_index;
        uint16_t background = selected ? COLOR_ORANGE : COLOR_BLACK;
        uint16_t foreground = selected ? COLOR_BLACK : COLOR_WHITE;
        gfx_fill_rect(canvas, x, y, 207, 22, background);
        char line[40];
        snprintf(line, sizeof(line), "%c %s", selected ? '>' : ' ',
                 menu_text(ui, index));
        gfx_text(canvas, x + 5, y + 3, line, 2, foreground, background, 197);
    }
}

static void render_server_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    if (ui->server_select_mode == 0) {
        gfx_text(canvas, 8, 31,
                 tr(ui, "Turn left: FMO list",
                    "\u5de6\u65cb: FMO \u670d\u52a1\u5668"),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 59,
                 tr(ui, "Turn right: NRL list",
                    "\u53f3\u65cb: NRL \u670d\u52a1\u5668"),
                 2, COLOR_PURPLE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 92,
                 tr(ui, "Web: /servers",
                    "Web \u7ba1\u7406: /servers"),
                 2, COLOR_GRAY, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 122,
                 tr(ui, "Hold: back", "\u957f\u6309: \u8fd4\u56de"),
                 2, COLOR_GRAY, COLOR_BLACK, 410);
        return;
    }
    if (ui->server_select_mode == 2) {
        const fmo_server_t *server =
            fmo_server_directory_get(ui->fmo_server_index);
        char line[160];
        gfx_text(canvas, 8, 25, "FMO",
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_fill_rect(canvas, 6, 43, 416, 31, COLOR_ORANGE);
        format_fmo_server_list(line, sizeof(line), server);
        draw_marquee(canvas, 14, 50, 400, line,
                     COLOR_BLACK, COLOR_ORANGE);
        snprintf(line, sizeof(line), "%s:%u  %s",
                 server != NULL ? server->host : "-",
                 server != NULL ? (unsigned)server->port : 0,
                 server != NULL && server->favorite ? "[FAVORITE]" : "");
        gfx_text(canvas, 8, 81, line, 2, COLOR_WHITE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 103,
                 tr(ui, "Press: favorite on/off",
                    "\u6309\u4e0b: \u6536\u85cf/\u53d6\u6d88\u6536\u85cf"),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 122,
                 tr(ui, "Rotate:list Hold:back",
                    "\u65cb\u8f6c:\u5217\u8868 \u957f\u6309:\u8fd4"),
                 2, COLOR_GRAY, COLOR_BLACK, 410);
        return;
    }
    const nrl_server_t *server = server_directory_get(ui->server_index);
    char line[160];
    gfx_text(canvas, 8, 26, tr(ui, "Server", "\u670d\u52a1\u5668"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
    gfx_fill_rect(canvas, 6, 44, 416, 31, COLOR_PURPLE);
    format_server(line, sizeof(line), server);
    draw_marquee(canvas, 14, 51, 400, line, COLOR_WHITE, COLOR_PURPLE);
    snprintf(line, sizeof(line), "%s:%u", server ? server->host : "-",
             server ? server->port : 0);
    gfx_text(canvas, 8, 82, line, 2, COLOR_WHITE, COLOR_BLACK, 410);
    gfx_text(canvas, 8, 122,
             tr(ui, "Rotate:select Press:confirm Hold:back",
                "\u65cb\u8f6c:\u9009\u62e9 \u6309\u4e0b:\u786e\u8ba4 \u957f\u6309:\u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
}

static void render_ota_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    fmo_ota_ui_status_t ota = {0};
    char line[112];
    ota_service_get_ui_status(&ota);
    snprintf(line, sizeof(line), "%s %s / %s",
             tr(ui, "Firmware", "\u56fa\u4ef6"),
             FMO_FIRMWARE_VERSION, FMO_BOARD_TYPE);
    gfx_text(canvas, 8, 24, line, 2, COLOR_WHITE, COLOR_BLACK, 414);
    if (ota.updating) {
        snprintf(line, sizeof(line), "%s %u%%",
                 tr(ui, "Updating", "\u6b63\u5728\u5347\u7ea7"),
                 (unsigned)ota.update_percent);
        gfx_text(canvas, 8, 49, line, 3, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_fill_rect(canvas, 8, 82, 412, 12, COLOR_DARK);
        gfx_fill_rect(canvas, 8, 82,
                      (int)(412U * ota.update_percent / 100U), 12, COLOR_ORANGE);
        gfx_text(canvas, 8, 104,
                 tr(ui, "Do not power off", "\u8bf7\u52ff\u65ad\u7535"),
                 2, COLOR_RED, COLOR_BLACK, 410);
    } else if (ota.checking) {
        gfx_text(canvas, 8, 52,
                 tr(ui, "Checking OTA server...",
                    "\u6b63\u5728\u68c0\u67e5 OTA \u670d\u52a1\u5668..."),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
    } else if (ota.last_error[0] != '\0') {
        gfx_text(canvas, 8, 47,
                 tr(ui, "OTA error", "OTA \u5347\u7ea7\u9519\u8bef"),
                 2, COLOR_RED, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 68, ota.last_error, 1, COLOR_WHITE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 103,
                 tr(ui, "Press: check again", "\u6309\u4e0b: \u91cd\u65b0\u68c0\u67e5"),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
    } else if (ota.latest_version[0] != '\0' &&
               strcmp(ota.latest_version, FMO_FIRMWARE_VERSION) != 0) {
        snprintf(line, sizeof(line), "%s %s",
                 tr(ui, "New version", "\u53d1\u73b0\u65b0\u7248\u672c"),
                 ota.latest_version);
        gfx_text(canvas, 8, 49, line, 3, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 86,
                 tr(ui, "Press: install", "\u6309\u4e0b: \u5b89\u88c5\u5347\u7ea7"),
                 2, COLOR_WHITE, COLOR_BLACK, 410);
    } else if (ota.latest_version[0] != '\0') {
        gfx_text(canvas, 8, 50,
                 tr(ui, "Firmware is current", "\u5f53\u524d\u5df2\u662f\u6700\u65b0\u7248\u672c"),
                 2, COLOR_GREEN, COLOR_BLACK, 410);
    } else {
        gfx_text(canvas, 8, 47, "NRL OTA", 2, COLOR_GRAY, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 67, ota.server_url, 1, COLOR_WHITE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 96,
                 tr(ui, "Press: check update", "\u6309\u4e0b: \u68c0\u67e5\u5347\u7ea7"),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
    }
    gfx_text(canvas, 8, 122,
             tr(ui, "Hold: back / Web: /update",
                "\u957f\u6309: \u8fd4\u56de / Web: /update"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
}

static void render_language_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    gfx_text(canvas, 12, 30, "Language / \u8bed\u8a00",
             2, COLOR_GRAY, COLOR_BLACK, 402);
    gfx_fill_rect(canvas, 10, 55, 408, 28,
                  !ui->chinese ? COLOR_ORANGE : COLOR_BLACK);
    gfx_text(canvas, 20, 62, !ui->chinese ? "> English" : "  English",
             2, !ui->chinese ? COLOR_BLACK : COLOR_WHITE,
             !ui->chinese ? COLOR_ORANGE : COLOR_BLACK, 380);
    gfx_fill_rect(canvas, 10, 88, 408, 28,
                  ui->chinese ? COLOR_ORANGE : COLOR_BLACK);
    gfx_text(canvas, 20, 95,
             ui->chinese ? "> \u4e2d\u6587" : "  \u4e2d\u6587",
             2, ui->chinese ? COLOR_BLACK : COLOR_WHITE,
             ui->chinese ? COLOR_ORANGE : COLOR_BLACK, 380);
    gfx_text(canvas, 12, 122,
             tr(ui, "Rotate: switch / Hold: back",
                "\u65cb\u8f6c: \u5207\u6362 / \u957f\u6309: \u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 402);
}

static void render_radio_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    fmo_config_t cfg = *cfg_get();
    char line[64];
    const uint8_t field = ui->radio_adjust_field;

    /* RX frequency */
    uint16_t c = field == 0 ? COLOR_ORANGE : COLOR_WHITE;
    snprintf(line, sizeof(line), "%c RX: %.4f MHz", field == 0 ? '>' : ' ',
             (double)cfg.radio_rx_mhz);
    gfx_text(canvas, 8, 28, line, 2, c, COLOR_BLACK, 410);

    /* TX frequency */
    c = field == 1 ? COLOR_ORANGE : COLOR_WHITE;
    snprintf(line, sizeof(line), "%c TX: %.4f MHz", field == 1 ? '>' : ' ',
             (double)cfg.radio_tx_mhz);
    gfx_text(canvas, 8, 50, line, 2, c, COLOR_BLACK, 410);

    /* Squelch */
    c = field == 2 ? COLOR_ORANGE : COLOR_WHITE;
    snprintf(line, sizeof(line), "%c SQL: %u", field == 2 ? '>' : ' ',
             (unsigned)cfg.squelch);
    gfx_text(canvas, 8, 72, line, 2, c, COLOR_BLACK, 200);

    /* TX Power */
    c = field == 3 ? COLOR_ORANGE : COLOR_WHITE;
    const char *pwr_str[] = {"LOW", "MID", "HIGH"};
    snprintf(line, sizeof(line), "%c PWR: %s", field == 3 ? '>' : ' ',
             pwr_str[cfg.tx_power <= 2 ? cfg.tx_power : 0]);
    gfx_text(canvas, 220, 72, line, 2, c, COLOR_BLACK, 200);

    /* RF enabled */
    c = field == 4 ? COLOR_ORANGE : (cfg.rf_enabled ? COLOR_GREEN : COLOR_RED);
    snprintf(line, sizeof(line), "%c RF: %s", field == 4 ? '>' : ' ',
             cfg.rf_enabled ? "ON" : "OFF");
    gfx_text(canvas, 8, 94, line, 2, c, COLOR_BLACK, 200);

    gfx_text(canvas, 8, 120,
             tr(ui, "Rotate:adjust Press:field Hold:back",
                "\u65cb\u8f6c:\u8c03\u6574 \u6309\u4e0b:\u5207\u6362 \u957f\u6309:\u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
}

static void render_ctcss_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    fmo_config_t cfg = *cfg_get();
    char line[64];
    const uint8_t field = ui->ctcss_adjust_field;

    /* RX CTCSS */
    uint16_t c = field == 0 ? COLOR_ORANGE : COLOR_WHITE;
    if (cfg.rx_ctcss_hz > 0.0f)
        snprintf(line, sizeof(line), "%c RX: %.1f Hz", field == 0 ? '>' : ' ',
                 (double)cfg.rx_ctcss_hz);
    else
        snprintf(line, sizeof(line), "%c RX: OFF", field == 0 ? '>' : ' ');
    gfx_text(canvas, 8, 28, line, 2, c, COLOR_BLACK, 410);

    /* TX CTCSS */
    c = field == 1 ? COLOR_ORANGE : COLOR_WHITE;
    if (cfg.tx_ctcss_hz > 0.0f)
        snprintf(line, sizeof(line), "%c TX: %.1f Hz", field == 1 ? '>' : ' ',
                 (double)cfg.tx_ctcss_hz);
    else
        snprintf(line, sizeof(line), "%c TX: OFF", field == 1 ? '>' : ' ');
    gfx_text(canvas, 8, 50, line, 2, c, COLOR_BLACK, 410);

    /* Detected tone */
    nrl_audio_ctcss_status_t st = {0};
    nrl_audio_codec_get_ctcss_status(&st);
    if (st.rx_detected_hz > 0.0f)
        snprintf(line, sizeof(line), "DET: %.1f Hz", (double)st.rx_detected_hz);
    else
        strlcpy(line, "DET: --", sizeof(line));
    gfx_text(canvas, 8, 78, line, 2, COLOR_GREEN, COLOR_BLACK, 410);

    gfx_text(canvas, 8, 120,
             tr(ui, "Rotate:tone Press:RX/TX Hold:back",
                "\u65cb\u8f6c:\u4e9a\u97f3 \u6309\u4e0b:RX/TX \u957f\u6309:\u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
}

static void render_audio_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    fmo_config_t cfg = *cfg_get();
    char line[64];
    const uint8_t field = ui->audio_adjust_field;
    for (uint8_t row = 0; row < 7; ++row) {
        switch (row) {
        case 0:
            snprintf(line, sizeof(line), "%c TX Network: %s",
                     field == row ? '>' : ' ',
                     cfg.tx_network == 1 ? "FMO OPUS" : "NRL");
            break;
        case 1:
            snprintf(line, sizeof(line), "%c NRL Codec: %s",
                     field == row ? '>' : ' ',
                     cfg.voice_codec == 1 ? "OPUS 16k" : "G711 8k");
            break;
        case 2:
            snprintf(line, sizeof(line), "%c Dual RX: %s",
                     field == row ? '>' : ' ',
                     cfg.audio_policy == 0 ? "MIX" : "FIRST");
            break;
        case 3:
            snprintf(line, sizeof(line), "%c RX Vol: %u",
                     field == row ? '>' : ' ', (unsigned)cfg.rx_volume);
            break;
        case 4:
            snprintf(line, sizeof(line), "%c TX Vol: %u",
                     field == row ? '>' : ' ', (unsigned)cfg.tx_volume);
            break;
        case 5:
            snprintf(line, sizeof(line), "%c MIC Gain: %ux",
                     field == row ? '>' : ' ', (unsigned)cfg.mic_gain);
            break;
        default:
            snprintf(line, sizeof(line), "%c ES8311 HP: %s",
                     field == row ? '>' : ' ',
                     cfg.es8311_hp_drive ? "ON" : "OFF");
            break;
        }
        uint16_t color = field == row ? COLOR_ORANGE : COLOR_WHITE;
        if (row == 0 && field != row) {
            color = cfg.tx_network == 1 ? COLOR_ORANGE : COLOR_PURPLE;
        }
        gfx_text(canvas, 8, 20 + row * 17, line, 2,
                 color, COLOR_BLACK, 410);
    }
}

static void render_net_radio_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    net_radio_status_t radio = {0};
    net_radio_get_status(&radio);
    const int count = (int)net_radio_count();
    char line[160];

    if (count == 0) {
        gfx_text(canvas, 8, 40,
                 tr(ui, "No stations yet", "还没有电台"),
                 2, COLOR_ORANGE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 70,
                 tr(ui, "Add stations on the Web page",
                    "请在 Web 页面添加电台"),
                 2, COLOR_WHITE, COLOR_BLACK, 410);
        gfx_text(canvas, 8, 122,
                 tr(ui, "Hold: back", "\u957f\u6309: \u8fd4\u56de"),
                 2, COLOR_GRAY, COLOR_BLACK, 410);
        return;
    }

    int sel = s_net_radio_sel;
    if (sel < 0 || sel >= count) {
        sel = (radio.current >= 0 && radio.current < count) ? radio.current : 0;
    }
    char name[NET_RADIO_NAME_MAX];
    if (!net_radio_get((size_t)sel, name, sizeof(name), NULL, 0)) {
        strlcpy(name, "-", sizeof(name));
    }

    /* Selected station: big orange bar, marquee for long names */
    snprintf(line, sizeof(line), "%d/%d %s", sel + 1, count, name);
    gfx_fill_rect(canvas, 6, 26, 416, 31, COLOR_ORANGE);
    draw_marquee(canvas, 14, 33, 400, line, COLOR_BLACK, COLOR_ORANGE);

    /* Playback state of the current (playing) station */
    const char *state_text;
    uint16_t state_color = COLOR_WHITE;
    switch (radio.state) {
    case NET_RADIO_STATE_CONNECTING:
        state_text = tr(ui, "Connecting", "连接中");
        state_color = COLOR_ORANGE;
        break;
    case NET_RADIO_STATE_PLAYING:
        state_text = tr(ui, "Playing", "播放中");
        state_color = COLOR_GREEN;
        break;
    case NET_RADIO_STATE_ERROR:
        state_text = tr(ui, "Error", "错误");
        state_color = COLOR_RED;
        break;
    default:
        state_text = tr(ui, "Stopped", "已停止");
        break;
    }
    if (radio.current >= 0 && radio.state != NET_RADIO_STATE_IDLE) {
        snprintf(line, sizeof(line), "%s: %s", state_text, radio.station_name);
    } else {
        strlcpy(line, state_text, sizeof(line));
    }
    gfx_text(canvas, 8, 66, line, 2, state_color, COLOR_BLACK, 410);
    if (radio.state == NET_RADIO_STATE_ERROR && radio.error[0] != '\0') {
        gfx_text(canvas, 8, 88, radio.error, 1, COLOR_WHITE, COLOR_BLACK, 410);
    }

    gfx_text(canvas, 8, 122,
             tr(ui, "Rotate:station Press:play/stop Hold:back",
                "\u65cb\u8f6c:\u9009\u53f0 \u6309\u4e0b:\u64ad\u653e/\u505c\u6b62 \u957f\u6309:\u8fd4\u56de"),
             2, COLOR_GRAY, COLOR_BLACK, 410);
}

static void render_detail(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    render_header(canvas, menu_text(ui, ui->menu_index));
    if (ui->menu_index == 0) {
        render_server_detail(ui, canvas);
    } else if (ui->menu_index == 1) {
        render_radio_detail(ui, canvas);
    } else if (ui->menu_index == 2) {
        render_ctcss_detail(ui, canvas);
    } else if (ui->menu_index == 3) {
        render_aprs_detail(ui, canvas);
    } else if (ui->menu_index == 4) {
        render_network_detail(ui, canvas);
    } else if (ui->menu_index == 5) {
        render_audio_detail(ui, canvas);
    } else if (ui->menu_index == 6) {
        render_ota_detail(ui, canvas);
    } else if (ui->menu_index == 7) {
        render_language_detail(ui, canvas);
    } else if (ui->menu_index == 8) {
        render_net_radio_detail(ui, canvas);
    } else {
        gfx_text(canvas, 12, 35,
                 tr(ui, "Migration in progress",
                    "\u529f\u80fd\u8fc1\u79fb\u4e2d"),
                 2, COLOR_ORANGE, COLOR_BLACK, 402);
        gfx_text(canvas, 12, 68,
                 tr(ui, "Press or hold to return",
                    "\u6309\u4e0b\u6216\u957f\u6309\u8fd4\u56de"),
                 2, COLOR_WHITE, COLOR_BLACK, 402);
    }
}

void app_ui_render(const app_ui_t *ui, gfx_canvas_t *canvas)
{
    if (ui == NULL || canvas == NULL) return;
    gfx_clear(canvas, COLOR_BLACK);
    if (ui->page == APP_UI_MAIN) render_main(ui, canvas);
    else if (ui->page == APP_UI_MENU) render_menu(ui, canvas);
    else render_detail(ui, canvas);

    gfx_fill_rect(canvas, 0, 0, 3, 3, COLOR_RED);
    gfx_fill_rect(canvas, canvas->width - 3, 0, 3, 3, COLOR_GREEN);
    gfx_fill_rect(canvas, 0, canvas->height - 3, 3, 3, COLOR_ORANGE);
    gfx_fill_rect(canvas, canvas->width - 3, canvas->height - 3, 3, 3, COLOR_WHITE);
}
