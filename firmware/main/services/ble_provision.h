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
