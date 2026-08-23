#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * BLE Wi-Fi provisioning service (NimBLE NUS).
 *
 * Advertises as "OpenFMO-CFG" with the Nordic UART Service UUID.
 * Accepts text commands over BLE to configure WiFi, NRL server, callsign etc.
 * Automatically stops when WiFi STA connects (frees radio for voice), and
 * restarts if WiFi drops for an extended period.
 */

esp_err_t ble_provision_start(void);
void ble_provision_poll(void);
bool ble_provision_is_active(void);
void ble_provision_stop(void);

/* Pause BLE advertising while a browser is associated with the provisioning
 * SoftAP. ESP32-S3 has one 2.4 GHz radio, so this gives DHCP and HTTP an
 * uninterrupted window. Advertising resumes after the last Web client leaves. */
void ble_provision_set_web_client_connected(bool connected);
