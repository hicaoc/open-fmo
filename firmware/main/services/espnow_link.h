#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"

esp_err_t espnow_link_start(const fmo_config_t *config);
bool espnow_link_set_enabled(bool enabled);
bool espnow_link_is_enabled(void);
esp_err_t espnow_link_send_g711(const int16_t *pcm8k, size_t samples);
esp_err_t espnow_link_send_opus(const int16_t *pcm16k, size_t samples);

