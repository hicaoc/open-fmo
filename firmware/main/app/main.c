#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "encoder.h"
#include "es8311_codec.h"
#include "nv3007.h"
#include "status_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "app_ui.h"
#include "audio/nrl_audio_codec.h"
#include "audio/audio_passthrough.h"
#include "audio/mdc_signaling.h"
#include "gfx.h"
#include "services/config_store.h"
#include "services/app_notice.h"
#include "services/aprs_service.h"
#include "services/espnow_link.h"
#include "services/network_manager.h"
#include "services/net_radio.h"
#include "services/ble_provision.h"
#include "esp_bt.h"
#include "services/nrl_link.h"
#include "services/ota_service.h"
#include "services/radio_at.h"
#include "services/server_directory.h"
#include "services/storage_fs.h"
#include "services/fmo_discovery.h"
#include "services/fmo_link.h"
#include "services/fmo_qso.h"
#include "services/fmo_station_beacon.h"
#include "services/serial_at.h"
#include "esp_sntp.h"

static const char *TAG = "open_fmo";

/* BOOT button (GPIO 0): menu enter/confirm. */
#define BOOT_PIN GPIO_NUM_0

static void boot_button_task(void *arg)
{
    QueueHandle_t event_queue = (QueueHandle_t)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool pressed = false;

    for (;;) {
        const bool level = gpio_get_level(BOOT_PIN) == 0; /* active LOW */

        if (level && !pressed) {
            pressed = true;
        } else if (!level && pressed) {
            pressed = false;
            encoder_event_t event = {.type = ENCODER_EVENT_BOOT_PRESS};
            if (xQueueSend(event_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(TAG, "BOOT menu event dropped");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t amplifier_enable_early(void)
{
    const gpio_config_t amp = {
        .pin_bit_mask = 1ULL << FMO_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "amplifier enable config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(FMO_AMP_EN, 1), TAG,
                        "amplifier enable failed");
    return ESP_OK;
}

static esp_err_t safe_io_init(void)
{
    const uint64_t output_mask = (1ULL << FMO_RADIO_PTT) |
                                 (1ULL << FMO_AT_EN) |
                                 (1ULL << FMO_LED1) |
                                 (1ULL << FMO_LED2) |
                                 (1ULL << FMO_LED3);
    gpio_config_t outputs = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "safe output config failed");
    gpio_set_level(FMO_RADIO_PTT, 0);
    gpio_set_level(FMO_AT_EN, 1);
    /* GPIO21 was configured before the codec probe; do not reset it here. */
    gpio_set_level(FMO_AMP_EN, 1);
    gpio_set_level(FMO_LED1, !FMO_LED1_ACTIVE_LEVEL);
    gpio_set_level(FMO_LED2, !FMO_LED2_ACTIVE_LEVEL);
    gpio_set_level(FMO_LED3, !FMO_LED3_ACTIVE_LEVEL);

    gpio_config_t net_ptt = {
        .pin_bit_mask = 1ULL << FMO_NET_PTT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&net_ptt);
}

static void apply_radio_config(const fmo_config_t *config)
{
    unsigned failed = 0;
#define APPLY_RF_SETTING(name, expression) do {                              \
        esp_err_t setting_error = (expression);                              \
        if (setting_error != ESP_OK) {                                       \
            ++failed;                                                        \
            ESP_LOGW(TAG, "RF setting %s failed: %s", (name),              \
                     esp_err_to_name(setting_error));                        \
        }                                                                    \
    } while (0)

    APPLY_RF_SETTING("RX frequency",
                     radio_at_set_frequency(false, config->radio_rx_mhz));
    APPLY_RF_SETTING("TX frequency",
                     radio_at_set_frequency(true, config->radio_tx_mhz));
    APPLY_RF_SETTING("squelch", radio_at_set_squelch(config->squelch));
    APPLY_RF_SETTING("RX volume",
                     radio_at_set_volume(false, config->rx_volume));
    APPLY_RF_SETTING("TX volume",
                     radio_at_set_volume(true, config->tx_volume));
    APPLY_RF_SETTING("TX power", radio_at_set_tx_power(config->tx_power));
    if (config->freq_tune_hz != 0) {
        APPLY_RF_SETTING("freq tune",
                         radio_at_set_freq_tune(config->freq_tune_hz));
    }

    /* Enabling the receiver is safety-critical for this gateway. Always send
     * this command even if an older module firmware rejects another setting. */
    esp_err_t rf_error = radio_at_set_rf_enabled(config->rf_enabled);
    if (rf_error != ESP_OK) {
        ++failed;
        ESP_LOGE(TAG, "RF %s failed: %s",
                 config->rf_enabled ? "enable" : "disable",
                 esp_err_to_name(rf_error));
    }
#undef APPLY_RF_SETTING

    ESP_LOGI(TAG,
             "RF config applied: rx=%.4f tx=%.4f sql=%u enabled=%d failures=%u",
             config->radio_rx_mhz, config->radio_tx_mhz,
             (unsigned)config->squelch, config->rf_enabled ? 1 : 0, failed);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Open FMO phase P0, ESP32-S3 + NV3007 landscape UI");
    /* Keep the confirmed rear PA on from the earliest safe point. Leave all
     * other board outputs untouched while probing the codec, matching the
     * successful early hardware scan's "no power enable" pass. */
    ESP_ERROR_CHECK(amplifier_enable_early());
    vTaskDelay(pdMS_TO_TICKS(100));
    bool codec_present = es8311_codec_init_control() == ESP_OK;
    if (!codec_present) {
        ESP_LOGW(TAG, "ES8311 initial probe failed; retrying after board IO setup");
    }
    ESP_ERROR_CHECK(safe_io_init());
    if (!codec_present) {
        vTaskDelay(pdMS_TO_TICKS(500));
        codec_present = es8311_codec_init_control() == ESP_OK;
        if (!codec_present) {
            ESP_LOGW(TAG, "ES8311 retry failed; I2S remains disabled");
        }
    }
    ESP_ERROR_CHECK(status_io_init());
    ESP_ERROR_CHECK(config_store_init());
    esp_err_t storage_error = storage_fs_init();
    if (storage_error != ESP_OK) {
        ESP_LOGW(TAG, "persistent server/certificate storage unavailable: %s",
                 esp_err_to_name(storage_error));
    }
    ESP_ERROR_CHECK(server_directory_init());
    fmo_config_t config;
    ESP_ERROR_CHECK(config_store_load(&config));

    /* Release BT controller memory early unless BLE provisioning is needed */
    const bool need_ble = config.ble_provisioning_enabled &&
                          config_store_wifi_count(&config) == 0;
    if (!need_ble) {
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    }

    ESP_ERROR_CHECK(nv3007_init());

    gfx_canvas_t canvas = {0};
    ESP_ERROR_CHECK(gfx_canvas_create(&canvas, FMO_LCD_WIDTH, FMO_LCD_HEIGHT));

    QueueHandle_t encoder_queue = NULL;
    ESP_ERROR_CHECK(encoder_init(&encoder_queue));
    xTaskCreate(boot_button_task, "boot_btn", 3072, encoder_queue, 6, NULL);

    app_ui_t ui;
    app_ui_init(&ui);
    ui.codec_present = codec_present;
    ui.espnow_enabled = config.espnow_enabled;
    ui.aprs_enabled = config.aprs_enabled;
    ui.chinese = config.ui_language != 0;
    strlcpy(ui.callsign, config.callsign, sizeof(ui.callsign));
    ui.callsign_ssid = config.callsign_ssid;
    ui.tx_mhz = config.radio_tx_mhz;
    ui.rx_mhz = config.radio_rx_mhz;
    ui.tx_ctcss_hz = config.tx_ctcss_hz;
    ui.rx_ctcss_hz = config.rx_ctcss_hz;
    size_t configured_server = server_directory_find(config.nrl_host, config.nrl_port);
    if (configured_server != SIZE_MAX) ui.server_index = configured_server;
    else if (config.selected_server < server_directory_count())
        ui.server_index = config.selected_server;
    size_t configured_fmo = fmo_server_directory_find(config.fmo_server_key);
    if (configured_fmo != SIZE_MAX) {
        ui.fmo_server_index = configured_fmo;
        const fmo_server_t *configured =
            fmo_server_directory_get(configured_fmo);
        for (size_t i = 0; i < fmo_server_directory_favorite_count(); ++i) {
            const fmo_server_t *favorite =
                fmo_server_directory_get_favorite(i);
            if (favorite != NULL && configured != NULL &&
                strcmp(favorite->key, configured->key) == 0) {
                ui.fmo_favorite_index = i;
                break;
            }
        }
    }
    app_ui_render(&ui, &canvas);
    ESP_ERROR_CHECK(nv3007_flush_rgb565(canvas.pixels, canvas.width, canvas.height));

    if (radio_at_init() == ESP_OK) {
        char module_name[32];
        esp_err_t probe_error = ESP_FAIL;
        for (unsigned attempt = 1; attempt <= 10; ++attempt) {
            probe_error = radio_at_probe(module_name, sizeof(module_name));
            if (probe_error == ESP_OK) break;
            ESP_LOGW(TAG, "RF module probe %u/10 failed: %s", attempt,
                     esp_err_to_name(probe_error));
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (probe_error == ESP_OK) {
            ui.radio_present = true;
            ESP_LOGI(TAG, "RF module: %s", module_name);
            apply_radio_config(&config);
        } else {
            ESP_LOGW(TAG, "RF module did not answer AT+NAME?");
        }
    }
    ESP_ERROR_CHECK(ota_service_init());
    /* COM-port (USB Serial/JTAG console) AT command listener */
    ESP_ERROR_CHECK(serial_at_init());
    ESP_ERROR_CHECK(network_manager_start(&config));

    /* Start NTP time sync (China Standard Time UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    /* Start BLE only for first-time provisioning (no saved WiFi) */
    if (need_ble) {
        esp_err_t ble_err = ble_provision_start();
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "BLE provisioning unavailable: %s",
                     esp_err_to_name(ble_err));
        }
    }
    ESP_ERROR_CHECK(aprs_service_start(&config));
    ESP_ERROR_CHECK(fmo_discovery_start(&config));
    esp_err_t nrl_err = nrl_link_start(&config);
    if (nrl_err != ESP_OK) {
        ESP_LOGW(TAG, "nrl_link_start failed: %s", esp_err_to_name(nrl_err));
    }
    /* MDC1200 signaling decoder (independent of Opus codec) */
    ESP_ERROR_CHECK(mdc_signaling_init() ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(nrl_audio_codec_init());
    nrl_audio_codec_configure_ctcss(config.rx_ctcss_hz, config.tx_ctcss_hz);
    /* Start audio hardware: I2S bus → ES8311 configure → passthrough task */
    if (codec_present) {
        esp_err_t audio_err = audio_passthrough_start();
        if (audio_err == ESP_OK) {
            audio_passthrough_set_voice_codec(config.voice_codec);
            audio_passthrough_set_mic_gain(config.mic_gain);
            audio_passthrough_set_audio_policy(config.audio_policy);
            audio_passthrough_set_tx_network(config.tx_network);
        } else {
            ESP_LOGW(TAG, "audio passthrough start failed: %s",
                     esp_err_to_name(audio_err));
        }
    }
    esp_err_t fmo_err = fmo_link_start(&config);
    if (fmo_err != ESP_OK) {
        ESP_LOGW(TAG, "fmo_link_start failed: %s", esp_err_to_name(fmo_err));
    }
    /* FMO-V4 STATION broadcast (own-server advertisement via APRS-IS);
     * runs gated on link/role/APRS state, so start order is not critical. */
    esp_err_t beacon_err = fmo_station_beacon_start(&config);
    if (beacon_err != ESP_OK) {
        ESP_LOGW(TAG, "fmo_station_beacon_start failed: %s",
                 esp_err_to_name(beacon_err));
    }
    /* FMO QSO call signaling (APFMO0 messages + MQTT QSO records); gated on
     * APRS/link state at runtime, so start order is not critical. */
    esp_err_t qso_err = fmo_qso_start(&config);
    if (qso_err != ESP_OK) {
        ESP_LOGW(TAG, "fmo_qso_start failed: %s", esp_err_to_name(qso_err));
    }
    ESP_ERROR_CHECK(espnow_link_start(&config));
    /* Net radio: station list (NVS) + decoder registration; the player task
     * starts on demand when a station is played. */
    ESP_ERROR_CHECK(net_radio_init());
    ESP_LOGI(TAG, "UI ready: rotate=server/menu, knob press=TX network, "
                  "BOOT=enter, hold=back");

    int64_t server_changed_at_us = 0;
    bool server_save_pending = false;
    bool fmo_save_pending = false;
    int64_t last_render_at_us = 0;
    int64_t last_input_at_us = esp_timer_get_time();
    uint32_t server_generation = server_directory_generation();
    uint32_t config_generation = config_store_generation();
    bool render_needed = true;
    while (true) {
        uint32_t current_generation = server_directory_generation();
        if (current_generation != server_generation) {
            size_t selected = server_directory_find(config.nrl_host, config.nrl_port);
            if (selected != SIZE_MAX) ui.server_index = selected;
            else if (ui.server_index >= server_directory_count()) ui.server_index = 0;
            server_generation = current_generation;
            render_needed = true;
        }
        /* External config change (web/AT): refresh the local copy and follow
         * the saved server on screen, unless the knob has a selection in
         * flight. */
        uint32_t new_config_generation = config_store_generation();
        if (new_config_generation != config_generation) {
            config_generation = new_config_generation;
            if (!ui.server_change_pending && !server_save_pending &&
                !ui.fmo_server_change_pending && !fmo_save_pending) {
                if (config_store_load(&config) == ESP_OK) {
                    size_t selected = server_directory_find(config.nrl_host,
                                                            config.nrl_port);
                    if (selected != SIZE_MAX && selected != ui.server_index) {
                        ui.server_index = selected;
                        render_needed = true;
                    }
                    size_t fmo_selected =
                        fmo_server_directory_find(config.fmo_server_key);
                    if (fmo_selected != SIZE_MAX) {
                        ui.fmo_server_index = fmo_selected;
                    }
                }
            }
        }
        /* Incoming FMO call: pull the UI back to the main page so the
         * ringing overlay (and the answer/reject knob hint) is visible. */
        fmo_qso_status_t qso_poll = {0};
        fmo_qso_get_status(&qso_poll);
        if (qso_poll.incoming && ui.page != APP_UI_MAIN) {
            ui.page = APP_UI_MAIN;
            render_needed = true;
        }
        encoder_event_t event;
        if (xQueueReceive(encoder_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            last_input_at_us = esp_timer_get_time();
            fmo_qso_status_t qso_status = {0};
            fmo_qso_get_status(&qso_status);
            if (qso_status.incoming) {
                /* Incoming FMO call: knob short press answers, long press
                 * rejects; all other knob actions are swallowed while
                 * ringing so the call is never answered accidentally. */
                if (event.type == ENCODER_EVENT_PRESS) {
                    fmo_qso_answer(true);
                    ESP_LOGI(TAG, "FMO call answered");
                } else if (event.type == ENCODER_EVENT_LONG_PRESS) {
                    fmo_qso_answer(false);
                    ESP_LOGI(TAG, "FMO call rejected");
                }
            } else if (event.type == ENCODER_EVENT_PRESS) {
                fmo_config_t fresh;
                esp_err_t tx_error = config_store_load(&fresh);
                if (tx_error == ESP_OK) {
                    fresh.tx_network = fresh.tx_network == 0 ? 1 : 0;
                    tx_error = config_store_save(&fresh);
                }
                if (tx_error == ESP_OK) {
                    config.tx_network = fresh.tx_network;
                    audio_passthrough_set_tx_network(fresh.tx_network);
                    const char *network = fresh.tx_network == 1
                        ? "FMO OPUS" : "NRL";
                    char notice[32];
                    snprintf(notice, sizeof(notice), "TX -> %s", network);
                    app_notice_post_info(notice, 3000);
                    ESP_LOGI(TAG, "TX network switched to %s", network);
                } else {
                    ESP_LOGE(TAG, "TX network switch failed: %s",
                             esp_err_to_name(tx_error));
                }
            } else {
                const encoder_event_type_t ui_event =
                    event.type == ENCODER_EVENT_BOOT_PRESS
                        ? ENCODER_EVENT_PRESS : event.type;
                app_ui_handle(&ui, ui_event);
            }
            render_needed = true;
            if (ui.language_change_pending) {
                /* Reload before saving: web/AT may have changed other fields
                 * since boot, and a full-blob save of the stale local copy
                 * would clobber them. */
                fmo_config_t fresh;
                esp_err_t language_error = config_store_load(&fresh);
                if (language_error == ESP_OK) {
                    fresh.ui_language = ui.chinese ? 1 : 0;
                    language_error = config_store_save(&fresh);
                }
                if (language_error == ESP_OK) {
                    config.ui_language = fresh.ui_language;
                    ui.language_change_pending = false;
                    ESP_LOGI(TAG, "UI language saved: %s",
                             ui.chinese ? "zh" : "en");
                } else {
                    ESP_LOGE(TAG, "UI language save failed: %s",
                             esp_err_to_name(language_error));
                }
            }
            if (ui.espnow_change_pending) {
                if (espnow_link_set_enabled(ui.espnow_enabled)) {
                    fmo_config_t fresh;
                    esp_err_t espnow_error = config_store_load(&fresh);
                    if (espnow_error == ESP_OK) {
                        fresh.espnow_enabled = ui.espnow_enabled;
                        espnow_error = config_store_save(&fresh);
                    }
                    if (espnow_error == ESP_OK) {
                        config.espnow_enabled = fresh.espnow_enabled;
                        ui.espnow_change_pending = false;
                        ESP_LOGI(TAG, "ESP-NOW saved: %s",
                                 ui.espnow_enabled ? "on" : "off");
                    } else {
                        ESP_LOGE(TAG, "ESP-NOW save failed: %s",
                                 esp_err_to_name(espnow_error));
                    }
                } else {
                    ui.espnow_enabled = false;
                    ui.espnow_change_pending = false;
                    ESP_LOGW(TAG, "ESP-NOW could not be enabled");
                }
            }
            if (ui.aprs_change_pending) {
                if (aprs_service_set_enabled(ui.aprs_enabled)) {
                    fmo_config_t fresh;
                    esp_err_t aprs_error = config_store_load(&fresh);
                    if (aprs_error == ESP_OK) {
                        fresh.aprs_enabled = ui.aprs_enabled;
                        aprs_error = config_store_save(&fresh);
                    }
                    if (aprs_error == ESP_OK) {
                        config.aprs_enabled = fresh.aprs_enabled;
                        ui.aprs_change_pending = false;
                        ESP_LOGI(TAG, "APRS saved: %s",
                                 ui.aprs_enabled ? "on" : "off");
                    } else {
                        ESP_LOGE(TAG, "APRS save failed: %s",
                                 esp_err_to_name(aprs_error));
                    }
                } else {
                    ui.aprs_enabled = false;
                    ui.aprs_change_pending = false;
                    ESP_LOGW(TAG, "APRS could not be enabled");
                }
            }
            if ((event.type == ENCODER_EVENT_CLOCKWISE ||
                 event.type == ENCODER_EVENT_COUNTER_CLOCKWISE) &&
                (ui.server_change_pending ||
                 ui.fmo_server_change_pending)) {
                server_changed_at_us = esp_timer_get_time();
            }
            ESP_LOGI(TAG, "encoder event=%d page=%d server=%u menu=%d",
                     event.type, ui.page, (unsigned)ui.server_index, ui.menu_index);
        }
        /* Menu idle timeout: 10s without input returns to the main page.
         * Suppressed while net radio is playing so music is not interrupted
         * (and the page keeps showing the live playback state). Also
         * suppressed while an OTA update is running so the upgrade page
         * stays on screen until it finishes. */
        if (ui.page != APP_UI_MAIN &&
            !(ui.page == APP_UI_DETAIL && ui.menu_index == 8 &&
              net_radio_is_playing()) &&
            esp_timer_get_time() - last_input_at_us >= 10000000) {
            fmo_ota_ui_status_t ota_status = {0};
            ota_service_get_ui_status(&ota_status);
            if (!ota_status.updating) {
                ui.page = APP_UI_MAIN;
                render_needed = true;
                ESP_LOGI(TAG, "menu idle timeout, back to main page");
            }
        }
        /* Rotation settled for 800ms: apply the new server immediately so the
         * link switches fast, but defer the NVS write until the knob has been
         * stable for 10s to avoid hammering flash while scrolling. */
        if (ui.server_change_pending && server_changed_at_us != 0 &&
            esp_timer_get_time() - server_changed_at_us >= 800000) {
            const nrl_server_t *server = server_directory_get(ui.server_index);
            config.selected_server = ui.server_index;
            if (server != NULL) {
                strlcpy(config.nrl_host, server->host, sizeof(config.nrl_host));
                config.nrl_port = server->port;
            }
            nrl_link_update_config(&config);
            ui.server_change_pending = false;
            server_save_pending = true;
            server_changed_at_us = esp_timer_get_time();
            render_needed = true;
            ESP_LOGI(TAG, "server switched: %s:%u (save deferred)",
                     config.nrl_host, config.nrl_port);
        }
        if (ui.fmo_server_change_pending && server_changed_at_us != 0 &&
            esp_timer_get_time() - server_changed_at_us >= 800000) {
            const fmo_server_t *server =
                fmo_server_directory_get_favorite(ui.fmo_favorite_index);
            if (server != NULL) {
                strlcpy(config.fmo_server_key, server->key,
                        sizeof(config.fmo_server_key));
                strlcpy(config.fmo_host, server->host,
                        sizeof(config.fmo_host));
                config.fmo_port = server->port;
                fmo_link_update_config(&config);
                fmo_save_pending = true;
                ESP_LOGI(TAG, "FMO server switched: %s:%u (save deferred)",
                         config.fmo_host, config.fmo_port);
            }
            ui.fmo_server_change_pending = false;
            server_changed_at_us = esp_timer_get_time();
            render_needed = true;
        }
        /* Persist the server choice 10s after the last rotation. Reload from
         * NVS first so fields changed via web/AT since boot are not clobbered
         * by this task's stale copy of the config. */
        if ((server_save_pending || fmo_save_pending) &&
            server_changed_at_us != 0 &&
            esp_timer_get_time() - server_changed_at_us >= 10000000) {
            fmo_config_t fresh;
            esp_err_t error = config_store_load(&fresh);
            if (error == ESP_OK) {
                if (server_save_pending) {
                    const nrl_server_t *server =
                        server_directory_get(ui.server_index);
                    fresh.selected_server = ui.server_index;
                    if (server != NULL) {
                        strlcpy(fresh.nrl_host, server->host,
                                sizeof(fresh.nrl_host));
                        fresh.nrl_port = server->port;
                    }
                }
                if (fmo_save_pending) {
                    const fmo_server_t *server =
                        fmo_server_directory_get_favorite(
                            ui.fmo_favorite_index);
                    if (server != NULL) {
                        strlcpy(fresh.fmo_server_key, server->key,
                                sizeof(fresh.fmo_server_key));
                        strlcpy(fresh.fmo_host, server->host,
                                sizeof(fresh.fmo_host));
                        fresh.fmo_port = server->port;
                    }
                }
                error = config_store_save(&fresh);
            }
            if (error == ESP_OK) {
                config = fresh;
                ESP_LOGI(TAG, "server saved: %s:%u", config.nrl_host,
                         config.nrl_port);
            } else {
                ESP_LOGE(TAG, "server save failed: %s", esp_err_to_name(error));
            }
            server_save_pending = false;
            fmo_save_pending = false;
            server_changed_at_us = 0;
        }
        const int64_t now_us = esp_timer_get_time();
        const int64_t refresh_interval_us = ui.page == APP_UI_MAIN ? 100000 : 1000000;
        if (render_needed || now_us - last_render_at_us >= refresh_interval_us) {
            app_ui_render(&ui, &canvas);
            ESP_ERROR_CHECK(nv3007_flush_rgb565(canvas.pixels, canvas.width,
                                                canvas.height));
            render_needed = false;
            last_render_at_us = now_us;
        }
        ble_provision_poll();
    }
}
