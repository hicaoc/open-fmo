#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"

esp_err_t fmo_discovery_start(const fmo_config_t *config);
void fmo_discovery_update_config(const fmo_config_t *config);
bool fmo_discovery_lookup_ssid(const char *callsign, uint8_t *ssid);
