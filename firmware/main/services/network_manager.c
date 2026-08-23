#include "network_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "services/web_portal.h"
#include "services/dns_server.h"
#include "services/ble_provision.h"

static const char *TAG = "network";
static network_status_t s_status;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_scan_lock;
static EventGroupHandle_t s_events;
static fmo_wifi_profile_t s_profiles[FMO_WIFI_PROFILE_MAX];
static size_t s_profile_count;
static unsigned s_config_ap_client_count;
#define PROVISION_SCAN_MAX 24U
/* The cached scan list is only copied while the flash cache is enabled; keep
 * its roughly 3 KB in PSRAM so network protocol task stacks retain enough
 * contiguous internal RAM. */
static EXT_RAM_BSS_ATTR wifi_ap_record_t s_scan_cache[PROVISION_SCAN_MAX];
static uint16_t s_scan_cache_count;
static bool s_scan_cache_ready;

#define EVENT_GOT_IP          BIT0
#define EVENT_DISCONNECTED    BIT1
#define EVENT_PROFILES_CHANGED BIT2

static void status_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void status_unlock(void) { xSemaphoreGive(s_lock); }

static void copy_profiles_locked(const fmo_config_t *config)
{
    memset(s_profiles, 0, sizeof(s_profiles));
    s_profile_count = config_store_wifi_count(config);
    if (s_profile_count > 0) {
        memcpy(s_profiles, config->wifi_profiles,
               s_profile_count * sizeof(s_profiles[0]));
    } else if (config->wifi_ssid[0] != '\0') {
        s_profile_count = 1;
        strlcpy(s_profiles[0].ssid, config->wifi_ssid,
                sizeof(s_profiles[0].ssid));
        strlcpy(s_profiles[0].password, config->wifi_password,
                sizeof(s_profiles[0].password));
    }
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = data;
        ++s_config_ap_client_count;
        ble_provision_set_web_client_connected(true);
        ESP_LOGI(TAG, "provision client connected: " MACSTR " aid=%u",
                 MAC2STR(event->mac), (unsigned)event->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        if (s_config_ap_client_count > 0) --s_config_ap_client_count;
        ble_provision_set_web_client_connected(s_config_ap_client_count > 0);
        ESP_LOGI(TAG, "provision client disconnected: " MACSTR " aid=%u",
                 MAC2STR(event->mac), (unsigned)event->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        status_lock();
        s_status.station_connected = false;
        s_status.ip_address[0] = '\0';
        s_status.wifi_rssi_dbm = -127;
        status_unlock();
        xEventGroupSetBits(s_events, EVENT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *event = data;
        ESP_LOGI(TAG, "DHCP lease assigned: " IPSTR " to " MACSTR "%s%s",
                 IP2STR(&event->ip), MAC2STR(event->mac),
                 event->hostname[0] ? " host=" : "",
                 event->hostname[0] ? event->hostname : "");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        wifi_ap_record_t access_point = {0};
        int8_t rssi = -127;
        if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
            rssi = access_point.rssi;
        }
        status_lock();
        s_status.station_connected = true;
        s_status.wifi_rssi_dbm = rssi;
        snprintf(s_status.ip_address, sizeof(s_status.ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        status_unlock();
        ESP_LOGI(TAG, "station IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, EVENT_GOT_IP);
    }
}

esp_err_t network_manager_scan_records(wifi_ap_record_t *records, uint16_t *count)
{
    if (records == NULL || count == NULL || *count == 0 || s_scan_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    /* esp_wifi_scan_start() requires a STA interface.  First-boot provisioning
     * deliberately runs in pure AP mode for reliable DHCP, so add STA only for
     * this user-requested scan and remove it immediately afterwards.  Changing
     * AP -> APSTA leaves the SoftAP and its DHCP lease alive. */
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    esp_err_t error = esp_wifi_get_mode(&original_mode);
    const bool temporary_sta = error == ESP_OK && original_mode == WIFI_MODE_AP;
    if (temporary_sta) {
        error = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (error == ESP_OK) error = esp_wifi_scan_start(&scan, true);
    if (error == ESP_OK) error = esp_wifi_scan_get_ap_records(count, records);
    if (error == ESP_OK) {
        s_scan_cache_count = *count < PROVISION_SCAN_MAX
            ? *count : PROVISION_SCAN_MAX;
        memcpy(s_scan_cache, records,
               s_scan_cache_count * sizeof(s_scan_cache[0]));
        s_scan_cache_ready = true;
    }
    if (temporary_sta) {
        esp_err_t restore_error = esp_wifi_set_mode(WIFI_MODE_AP);
        if (error == ESP_OK) error = restore_error;
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(error));
    }
    xSemaphoreGive(s_scan_lock);
    return error;
}

esp_err_t network_manager_cached_scan_records(wifi_ap_record_t *records,
                                              uint16_t *count)
{
    if (records == NULL || count == NULL || *count == 0 || s_scan_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_scan_cache_ready) {
        xSemaphoreGive(s_scan_lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t copied = *count < s_scan_cache_count ? *count : s_scan_cache_count;
    memcpy(records, s_scan_cache, copied * sizeof(records[0]));
    *count = copied;
    xSemaphoreGive(s_scan_lock);
    return ESP_OK;
}

static const wifi_ap_record_t *find_profile_ap(
    const fmo_wifi_profile_t *profile,
    const wifi_ap_record_t *records, uint16_t count)
{
    const wifi_ap_record_t *best = NULL;
    for (uint16_t i = 0; i < count; ++i) {
        if (strcmp(profile->ssid, (const char *)records[i].ssid) == 0 &&
            (best == NULL || records[i].rssi > best->rssi)) {
            best = &records[i];
        }
    }
    return best;
}

static esp_err_t set_config_ap_enabled(bool enabled)
{
    status_lock();
    bool unchanged = s_status.config_ap_running == enabled;
    status_unlock();
    if (unchanged) return ESP_OK;

    esp_err_t error = esp_wifi_set_mode(enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (error == ESP_OK) {
        status_lock();
        s_status.config_ap_running = enabled;
        status_unlock();
        ESP_LOGI(TAG, "config AP %s", enabled ? "enabled" : "disabled after STA connect");
    } else {
        ESP_LOGW(TAG, "config AP mode change failed: %s", esp_err_to_name(error));
    }
    return error;
}

static bool connect_profile(const fmo_wifi_profile_t *profile,
                            const wifi_ap_record_t *access_point)
{
    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, profile->ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, profile->password,
            sizeof(station.sta.password));
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station.sta.failure_retry_cnt = 3;
    station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (access_point != NULL) {
        station.sta.bssid_set = true;
        memcpy(station.sta.bssid, access_point->bssid,
               sizeof(station.sta.bssid));
        station.sta.channel = access_point->primary;
    }

    xEventGroupClearBits(s_events, EVENT_GOT_IP | EVENT_DISCONNECTED);
    if (esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK) return false;
    if (access_point != NULL) {
        ESP_LOGI(TAG, "trying saved Wi-Fi: %s, strongest RSSI=%d dBm channel=%u",
                 profile->ssid, (int)access_point->rssi,
                 (unsigned)access_point->primary);
    } else {
        ESP_LOGI(TAG, "trying saved Wi-Fi: %s", profile->ssid);
    }
    if (esp_wifi_connect() != ESP_OK) return false;
    EventBits_t bits = xEventGroupWaitBits(
        s_events, EVENT_GOT_IP | EVENT_DISCONNECTED | EVENT_PROFILES_CHANGED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & EVENT_GOT_IP) != 0;
}

static void connection_task(void *arg)
{
    (void)arg;
    for (;;) {
        fmo_wifi_profile_t profiles[FMO_WIFI_PROFILE_MAX] = {0};
        size_t profile_count;
        status_lock();
        profile_count = s_profile_count;
        memcpy(profiles, s_profiles, profile_count * sizeof(profiles[0]));
        status_unlock();
        xEventGroupClearBits(s_events, EVENT_PROFILES_CHANGED);

        if (profile_count == 0) {
            (void)set_config_ap_enabled(true);
            xEventGroupWaitBits(s_events, EVENT_PROFILES_CHANGED,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        /* First boot uses pure AP mode for the most reliable DHCP exchange.
         * A saved profile needs STA as well, so add it only when connection
         * work actually begins and keep the config AP alive until GOT_IP. */
        status_lock();
        bool config_ap_running = s_status.config_ap_running;
        status_unlock();
        if (config_ap_running) {
            esp_err_t mode_error = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (mode_error != ESP_OK) {
                ESP_LOGW(TAG, "enable STA for saved Wi-Fi failed: %s",
                         esp_err_to_name(mode_error));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        wifi_ap_record_t records[32] = {0};
        uint16_t record_count = sizeof(records) / sizeof(records[0]);
        if (network_manager_scan_records(records, &record_count) != ESP_OK) {
            record_count = 0;
        }

        bool connected = false;
        for (int visible_pass = 1; visible_pass >= 0 && !connected; --visible_pass) {
            for (size_t i = 0; i < profile_count; ++i) {
                const wifi_ap_record_t *access_point = find_profile_ap(
                    &profiles[i], records, record_count);
                bool visible = access_point != NULL;
                if (visible != (visible_pass != 0)) continue;
                connected = connect_profile(&profiles[i], access_point);
                if ((xEventGroupGetBits(s_events) & EVENT_PROFILES_CHANGED) != 0) {
                    connected = false;
                    break;
                }
                if (connected) break;
            }
        }

        if (connected) {
            (void)set_config_ap_enabled(false);
            for (;;) {
                EventBits_t bits = xEventGroupWaitBits(
                    s_events, EVENT_DISCONNECTED | EVENT_PROFILES_CHANGED,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                if ((bits & (EVENT_DISCONNECTED | EVENT_PROFILES_CHANGED)) != 0) {
                    break;
                }
                wifi_ap_record_t access_point = {0};
                if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
                    status_lock();
                    s_status.wifi_rssi_dbm = access_point.rssi;
                    status_unlock();
                }
            }
        } else {
            (void)set_config_ap_enabled(true);
            ESP_LOGW(TAG, "none of %u saved Wi-Fi networks connected",
                     (unsigned)profile_count);
            xEventGroupWaitBits(s_events, EVENT_PROFILES_CHANGED,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        }
    }
}

esp_err_t network_manager_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    memset(&s_status, 0, sizeof(s_status));
    s_status.wifi_rssi_dbm = -127;
    s_scan_cache_count = 0;
    s_scan_cache_ready = false;
    s_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == NULL || s_scan_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    status_lock();
    copy_profiles_locked(config);
    bool start_config_ap = s_profile_count == 0;
    status_unlock();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == NULL || sta_netif == NULL) return ESP_ERR_NO_MEM;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   network_event, NULL), TAG,
                        "Wi-Fi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                   network_event, NULL), TAG,
                        "IP event register failed");

    uint8_t ap_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP), TAG,
                        "read AP MAC failed");
    snprintf(s_status.config_ap_ssid, sizeof(s_status.config_ap_ssid),
             "OpenFMO-%02X%02X", ap_mac[4], ap_mac[5]);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_status.config_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_status.config_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;
    /* With no saved credentials there is no reason to keep STA enabled. Pure
     * AP mode avoids APSTA channel/state transitions during association and
     * DHCP, which are especially costly while BLE provisioning is available. */
    /* ESP-IDF 6 rejects WIFI_IF_AP configuration while the current mode is
     * STA-only.  Configure the AP while stopped in AP mode, then select the
     * actual startup mode.  The saved AP config remains available if a later
     * connection failure enables APSTA provisioning. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG,
                        "AP config failed");
    if (start_config_ap) {
        /* Scan before exposing the provisioning AP.  Phones then associate
         * only after the radio has returned to pure AP mode, while the Web UI
         * can immediately serve this cached list without another channel-hop
         * scan during DHCP or captive-portal startup. */
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                            "pre-scan STA mode failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                            "STA mode failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    if (start_config_ap) {
        wifi_ap_record_t initial_records[PROVISION_SCAN_MAX] = {0};
        uint16_t initial_count = PROVISION_SCAN_MAX;
        ESP_LOGI(TAG, "scanning Wi-Fi before showing provisioning AP");
        esp_err_t scan_error = network_manager_scan_records(initial_records,
                                                            &initial_count);
        if (scan_error == ESP_OK) {
            ESP_LOGI(TAG, "provisioning pre-scan cached %u access points",
                     (unsigned)initial_count);
        } else {
            ESP_LOGW(TAG, "provisioning pre-scan unavailable: %s",
                     esp_err_to_name(scan_error));
        }
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG,
                            "provisioning AP mode failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20),
                            TAG, "AP bandwidth failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "disable Wi-Fi power save failed");
    status_lock();
    s_status.config_ap_running = start_config_ap;
    status_unlock();
    if (xTaskCreate(connection_task, "wifi_connect", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(web_portal_start(), TAG, "web portal failed");
    if (start_config_ap) {
        dns_server_start();
        ESP_LOGI(TAG, "config AP: %s (open) portal=http://192.168.4.1",
                 s_status.config_ap_ssid);
    } else {
        ESP_LOGI(TAG, "saved Wi-Fi found; starting in STA-only mode");
    }
    return ESP_OK;
}

void network_manager_get_status(network_status_t *status)
{
    if (status == NULL || s_lock == NULL) return;
    status_lock();
    *status = s_status;
    status_unlock();
}

esp_err_t network_manager_update_profiles(const fmo_config_t *config, bool reconnect)
{
    if (config == NULL || s_lock == NULL || s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    status_lock();
    copy_profiles_locked(config);
    status_unlock();
    xEventGroupSetBits(s_events, EVENT_PROFILES_CHANGED);
    if (reconnect) (void)esp_wifi_disconnect();
    return ESP_OK;
}

esp_err_t network_manager_enter_provision(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    status_lock();
    bool already = s_status.config_ap_running;
    status_unlock();
    esp_err_t err = set_config_ap_enabled(true);
    if (err == ESP_OK && !already) {
        dns_server_start();
    }
    ESP_LOGI(TAG, "provisioning AP forced on: %s", s_status.config_ap_ssid);
    return err;
}
