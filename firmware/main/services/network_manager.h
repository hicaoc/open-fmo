#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "services/config_store.h"

typedef struct {
    bool station_connected;
    bool config_ap_running;
    char config_ap_ssid[24];
    char ip_address[16];
    int8_t wifi_rssi_dbm;
} network_status_t;

esp_err_t network_manager_start(const fmo_config_t *config);
esp_err_t network_manager_update_profiles(const fmo_config_t *config, bool reconnect);
esp_err_t network_manager_scan_records(wifi_ap_record_t *records, uint16_t *count);
/** Return the most recent scan without touching the radio. */
esp_err_t network_manager_cached_scan_records(wifi_ap_record_t *records,
                                              uint16_t *count);
void network_manager_get_status(network_status_t *status);
/** Force-enable config AP + DNS for provisioning (e.g. BOOT long-press). */
esp_err_t network_manager_enter_provision(void);
