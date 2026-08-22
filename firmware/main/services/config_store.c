#include "config_store.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *NAMESPACE = "open_fmo";
static const char *CONFIG_KEY = "config_v1";
static volatile uint32_t s_generation;

void config_store_defaults(fmo_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
    strlcpy(config->callsign, "NOCALL", sizeof(config->callsign));
    config->callsign_ssid = 0;
    strlcpy(config->fmo_callsign, "NOCALL", sizeof(config->fmo_callsign));
    config->fmo_callsign_ssid = 0;
    config->nrl_port = 60050;
    strlcpy(config->nrl_host, "m.nrlptt.com", sizeof(config->nrl_host));
    strlcpy(config->nrl_server_key, "m.nrlptt.com:60050",
            sizeof(config->nrl_server_key));
    config->radio_tx_mhz = 438.5000f;
    config->radio_rx_mhz = 438.5000f;
    config->squelch = 5;
    config->rx_volume = 5;
    config->tx_volume = 5;
    config->tx_power = 0;
    config->rf_enabled = true;
    config->ble_provisioning_enabled = true;
    config->ui_language = 1;
    config->aprs_enabled = false;
    config->es8311_dac_vol = FMO_ES8311_DAC_VOL_DEFAULT;  /* speaker default */
    config->es8311_adc_vol = FMO_ES8311_ADC_VOL_DEFAULT;  /* mic default 160 */
    config->es8311_hp_drive = false;                       /* REG13 HPSW off */
    config->fmo_mqtt_no_local = true;
    config->mic_gain = FMO_MIC_GAIN_DEFAULT;              /* software gain 1x */
    config->aprs_position_set = true;
    config->aprs_latitude_e6 = 30251100;   /* 30.2511 */
    config->aprs_longitude_e6 = 120148400; /* 120.1484 */
    config->aprs_ssid = 10;
    config->aprs_beacon_interval_s = 600;
    config->aprs_server_port = 14580;
    strlcpy(config->aprs_server_host, "asia.aprs2.net",
            sizeof(config->aprs_server_host));
    strlcpy(config->aprs_comment, "Open FMO",
            sizeof(config->aprs_comment));
    config->aprs_rf_rx = true;   /* decode AFSK from the radio mic */
    config->aprs_rf_tx = false;
    config->aprs_nrl_rx = true;  /* decode AFSK from NRL downlink */
    config->aprs_nrl_tx = false;
    config->aprs_fwd = FMO_APRS_FWD_DEFAULT;
    /* FMO-V4 STATION broadcast: off by default; the operator fills the
     * station name / country code on the APRS config page; online/peak
     * stay 0 (= automatic) unless the operator overrides them. */
    config->fmo_station_beacon_enabled = false;
    config->fmo_station_beacon_interval_min = 10;
    config->fmo_coverage_km = 100;
    /* FMO personal BEACON: off by default, freq 0 keeps the send gate
     * closed until the operator fills a frequency in. */
    config->fmo_beacon_enabled = false;
}

esp_err_t config_store_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed; erasing only the NVS partition");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        error = nvs_flash_init();
    }
    return error;
}

esp_err_t config_store_load(fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    config_store_defaults(config);

    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(error, TAG, "NVS open failed");

    fmo_config_t stored = {0};
    bool migrated = false;
    size_t size = sizeof(stored);
    error = nvs_get_blob(handle, CONFIG_KEY, &stored, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(error, TAG, "config read failed");
    if (stored.schema_version == 3 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->ui_language = 1;
        config->espnow_enabled = false;
        config->rf_enabled = true;
        migrated = true;
        ESP_LOGI(TAG, "migrated v3 config with saved Wi-Fi profiles to v7");
    } else if (stored.schema_version == 4 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->espnow_enabled = false;
        config->rf_enabled = true;
        migrated = true;
        ESP_LOGI(TAG, "migrated v4 config to v7 (ESP-NOW off, RF enabled)");
    } else if (stored.schema_version == 5 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        /* Early Open FMO builds defaulted RF off even though this gateway is
         * expected to receive continuously. Restore it once; v6 keeps later
         * user changes made through the web UI. */
        config->rf_enabled = true;
        migrated = true;
        ESP_LOGI(TAG, "migrated v5 config to v7 (RF receiver enabled)");
    } else if (stored.schema_version == 6 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->aprs_enabled = false;
        config->aprs_position_set = false;
        config->aprs_ssid = stored.callsign_ssid <= 15 ? stored.callsign_ssid : 0;
        config->aprs_beacon_interval_s = 600;
        config->aprs_server_port = 14580;
        strlcpy(config->aprs_server_host, "asia.aprs2.net",
                sizeof(config->aprs_server_host));
        strlcpy(config->aprs_comment, "Open FMO",
                sizeof(config->aprs_comment));
        config->voice_codec = 0;
        migrated = true;
        ESP_LOGI(TAG, "migrated v6 config to v8 (fixed-position APRS off)");
    } else if (stored.schema_version == 7 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->voice_codec = 0;  /* default G.711 for backward compat */
        config->es8311_dac_vol = FMO_ES8311_DAC_VOL_DEFAULT;
        config->es8311_adc_vol = FMO_ES8311_ADC_VOL_DEFAULT;
        migrated = true;
        ESP_LOGI(TAG, "migrated v7 config to v10 (voice_codec=G.711, es8311 vol)");
    } else if (stored.schema_version == 8 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->es8311_dac_vol = FMO_ES8311_DAC_VOL_DEFAULT;
        config->es8311_adc_vol = FMO_ES8311_ADC_VOL_DEFAULT;
        migrated = true;
        ESP_LOGI(TAG, "migrated v8 config to v10 (es8311 vol)");
    } else if (stored.schema_version == 10 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->aprs_rf_rx = true;
        config->aprs_rf_tx = false;
        config->aprs_nrl_rx = true;
        config->aprs_nrl_tx = false;
        config->aprs_fwd = FMO_APRS_FWD_DEFAULT;
        migrated = true;
        ESP_LOGI(TAG, "migrated v10 config to v11 (APRS AFSK switches)");
    } else if (stored.schema_version == 11 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->mic_gain = FMO_MIC_GAIN_DEFAULT;
        migrated = true;
        ESP_LOGI(TAG, "migrated v11 config to v12 (software mic gain)");
    } else if (stored.schema_version == 12 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        snprintf(config->nrl_server_key, sizeof(config->nrl_server_key),
                 "%s:%u", config->nrl_host, (unsigned)config->nrl_port);
        config->fmo_server_key[0] = '\0';
        config->fmo_host[0] = '\0';
        config->fmo_port = 0;
        config->tx_network = 0;
        config->audio_policy = 0;
        migrated = true;
        ESP_LOGI(TAG, "migrated v12 config to v13 (dual-network settings)");
    } else if (stored.schema_version == 13 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        strlcpy(config->fmo_callsign, stored.callsign,
                sizeof(config->fmo_callsign));
        config->fmo_callsign_ssid = stored.callsign_ssid <= 15
            ? stored.callsign_ssid : 0;
        migrated = true;
        ESP_LOGI(TAG, "migrated v13 config to v14 (separate FMO identity)");
    } else if (stored.schema_version == 14 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->es8311_hp_drive = false;
        migrated = true;
        ESP_LOGI(TAG, "migrated v14 config to v15 (ES8311 headphone drive)");
    } else if (stored.schema_version == 15 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        config->fmo_mqtt_no_local = true;
        migrated = true;
        ESP_LOGI(TAG, "migrated v15 config to v16 (MQTT 5 No Local enabled)");
    } else if (stored.schema_version == 16 && size <= sizeof(stored)) {
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        /* Tail fields keep the defaults set above (broadcast off, 10 min,
         * 100 km coverage, empty name/country, zero counters). */
        migrated = true;
        ESP_LOGI(TAG, "migrated v16 config to v17 (FMO station broadcast)");
    } else if (stored.schema_version == 17 && size <= sizeof(stored)) {
        /* v17 stored fmo_station_name[49]; widening it to 97 bytes (32 CJK
         * chars in UTF-8) shifted the tail fields, so copy the blob
         * piecewise instead of a straight memcpy.  The old name already
         * fits in 49 bytes and is kept as-is. */
        typedef struct {
            char name[49];
            char country[3];
            uint16_t coverage_km;
            uint16_t online;
            uint16_t peak;
        } config_v17_tail_t;
        const size_t head_size = offsetof(fmo_config_t, fmo_station_name);
        config_v17_tail_t tail;
        memcpy(&tail, (const uint8_t *)&stored + head_size, sizeof(tail));
        memcpy(config, &stored, head_size);
        memcpy(config->fmo_station_name, tail.name, sizeof(tail.name) - 1);
        config->fmo_station_name[sizeof(tail.name) - 1] = '\0';
        memcpy(config->fmo_country, tail.country, sizeof(tail.country));
        config->fmo_coverage_km = tail.coverage_km;
        config->fmo_station_online = tail.online;
        config->fmo_station_peak = tail.peak;
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        migrated = true;
        ESP_LOGI(TAG, "migrated v17 config to v18 (32-char station name)");
    } else if (stored.schema_version == 18 && size <= sizeof(stored)) {
        /* v19 only appends the personal-beacon tail, so the v18 blob is a
         * strict prefix of the new layout: a plain copy on top of the
         * defaults above (beacon off, empty texts, zero freq) is enough. */
        memcpy(config, &stored, size);
        config->schema_version = FMO_CONFIG_SCHEMA_VERSION;
        migrated = true;
        ESP_LOGI(TAG, "migrated v18 config to v19 (personal beacon)");
    } else if (size == sizeof(stored) &&
               stored.schema_version == FMO_CONFIG_SCHEMA_VERSION) {
        *config = stored;
    } else {
        ESP_LOGW(TAG, "ignoring incompatible config (size=%u version=%u)",
                 (unsigned)size, (unsigned)stored.schema_version);
        return ESP_OK;
    }
    if (migrated && stored.schema_version < 13) {
        strlcpy(config->fmo_callsign, config->callsign,
                sizeof(config->fmo_callsign));
        config->fmo_callsign_ssid = config->callsign_ssid <= 15
            ? config->callsign_ssid : 0;
    }
    config->callsign[sizeof(config->callsign) - 1] = '\0';
    config->fmo_callsign[sizeof(config->fmo_callsign) - 1] = '\0';
    config->nrl_host[sizeof(config->nrl_host) - 1] = '\0';
    config->wifi_ssid[sizeof(config->wifi_ssid) - 1] = '\0';
    config->wifi_password[sizeof(config->wifi_password) - 1] = '\0';
    config->aprs_server_host[sizeof(config->aprs_server_host) - 1] = '\0';
    config->aprs_comment[sizeof(config->aprs_comment) - 1] = '\0';
    config->nrl_server_key[sizeof(config->nrl_server_key) - 1] = '\0';
    config->fmo_server_key[sizeof(config->fmo_server_key) - 1] = '\0';
    config->fmo_host[sizeof(config->fmo_host) - 1] = '\0';
    config->fmo_station_name[sizeof(config->fmo_station_name) - 1] = '\0';
    config->fmo_country[sizeof(config->fmo_country) - 1] = '\0';
    config->fmo_rig[sizeof(config->fmo_rig) - 1] = '\0';
    config->fmo_ant[sizeof(config->fmo_ant) - 1] = '\0';
    config->fmo_aprs_msg[sizeof(config->fmo_aprs_msg) - 1] = '\0';
    config->fmo_notice[sizeof(config->fmo_notice) - 1] = '\0';
    config->fmo_qso_msg[sizeof(config->fmo_qso_msg) - 1] = '\0';
    if (config->fmo_station_beacon_interval_min != 5 &&
        config->fmo_station_beacon_interval_min != 10 &&
        config->fmo_station_beacon_interval_min != 60) {
        config->fmo_station_beacon_interval_min = 10;
    }
    if (config->fmo_coverage_km > 5000) config->fmo_coverage_km = 5000;
    if (config->ui_language > 1) config->ui_language = 1;
    if (config->voice_codec > 1) config->voice_codec = 0;
    if (config->tx_network > 1) config->tx_network = 0;
    if (config->audio_policy > 1) config->audio_policy = 0;
    if (config->fmo_callsign_ssid > 15) config->fmo_callsign_ssid = 0;
    /* Clamp ES8311 mic volume: older NVS blobs may hold values above
     * the 170 cap (speaker uses the full 0-255 range). */
    if (config->es8311_adc_vol > FMO_ES8311_ADC_VOL_MAX) {
        config->es8311_adc_vol = FMO_ES8311_ADC_VOL_MAX;
    }
    if (config->mic_gain < FMO_MIC_GAIN_MIN || config->mic_gain > FMO_MIC_GAIN_MAX) {
        config->mic_gain = FMO_MIC_GAIN_DEFAULT;
    }
    if (config->aprs_ssid > 15) config->aprs_ssid = 0;
    config->aprs_fwd &= FMO_APRS_FWD_MASK;
    if (config->aprs_beacon_interval_s < 30 ||
        config->aprs_beacon_interval_s > 3600) {
        config->aprs_beacon_interval_s = 600;
    }
    for (size_t i = 0; i < FMO_WIFI_PROFILE_MAX; ++i) {
        config->wifi_profiles[i].ssid[sizeof(config->wifi_profiles[i].ssid) - 1] = '\0';
        config->wifi_profiles[i].password[sizeof(config->wifi_profiles[i].password) - 1] = '\0';
    }
    if (config->wifi_profiles[0].ssid[0] == '\0' && config->wifi_ssid[0] != '\0') {
        strlcpy(config->wifi_profiles[0].ssid, config->wifi_ssid,
                sizeof(config->wifi_profiles[0].ssid));
        strlcpy(config->wifi_profiles[0].password, config->wifi_password,
                sizeof(config->wifi_profiles[0].password));
    }
    strlcpy(config->wifi_ssid, config->wifi_profiles[0].ssid,
            sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, config->wifi_profiles[0].password,
            sizeof(config->wifi_password));
    if (migrated) {
        esp_err_t save_error = config_store_save(config);
        if (save_error != ESP_OK) {
            ESP_LOGW(TAG, "migrated config save failed: %s",
                     esp_err_to_name(save_error));
        }
    }
    return ESP_OK;
}

size_t config_store_wifi_count(const fmo_config_t *config)
{
    if (config == NULL) return 0;
    size_t count = 0;
    while (count < FMO_WIFI_PROFILE_MAX && config->wifi_profiles[count].ssid[0] != '\0') {
        ++count;
    }
    return count;
}

static void sync_primary_wifi(fmo_config_t *config)
{
    strlcpy(config->wifi_ssid, config->wifi_profiles[0].ssid,
            sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, config->wifi_profiles[0].password,
            sizeof(config->wifi_password));
}

bool config_store_wifi_add(fmo_config_t *config, const char *ssid,
                           const char *password, bool promote)
{
    if (config == NULL || ssid == NULL || password == NULL || ssid[0] == '\0' ||
        strlen(ssid) > 32 || strlen(password) > 64) return false;
    size_t count = config_store_wifi_count(config);
    size_t index = FMO_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(config->wifi_profiles[i].ssid, ssid) == 0) {
            index = i;
            break;
        }
    }
    if (index == FMO_WIFI_PROFILE_MAX) {
        if (count >= FMO_WIFI_PROFILE_MAX) return false;
        index = count;
        memset(&config->wifi_profiles[index], 0, sizeof(config->wifi_profiles[index]));
    }
    strlcpy(config->wifi_profiles[index].ssid, ssid,
            sizeof(config->wifi_profiles[index].ssid));
    if (password[0] != '\0' || config->wifi_profiles[index].password[0] == '\0') {
        strlcpy(config->wifi_profiles[index].password, password,
                sizeof(config->wifi_profiles[index].password));
    }
    if (promote && index > 0) {
        fmo_wifi_profile_t selected = config->wifi_profiles[index];
        for (size_t i = index; i > 0; --i) {
            config->wifi_profiles[i] = config->wifi_profiles[i - 1];
        }
        config->wifi_profiles[0] = selected;
    }
    sync_primary_wifi(config);
    return true;
}

bool config_store_wifi_remove(fmo_config_t *config, size_t index)
{
    if (config == NULL || index >= config_store_wifi_count(config)) return false;
    for (size_t i = index; i + 1 < FMO_WIFI_PROFILE_MAX; ++i) {
        config->wifi_profiles[i] = config->wifi_profiles[i + 1];
    }
    memset(&config->wifi_profiles[FMO_WIFI_PROFILE_MAX - 1], 0,
           sizeof(config->wifi_profiles[0]));
    sync_primary_wifi(config);
    return true;
}

esp_err_t config_store_save(const fmo_config_t *config)
{
    if (config == NULL || config->schema_version != FMO_CONFIG_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "NVS open failed");
    esp_err_t error = nvs_set_blob(handle, CONFIG_KEY, config, sizeof(*config));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error == ESP_OK) ++s_generation;
    return error;
}

uint32_t config_store_generation(void)
{
    return s_generation;
}
