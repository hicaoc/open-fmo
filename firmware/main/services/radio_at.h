#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t radio_at_init(void);
esp_err_t radio_at_command(const char *command, char *response,
                           size_t response_size, uint32_t timeout_ms);
esp_err_t radio_at_probe(char *module_name, size_t module_name_size);
esp_err_t radio_at_set_frequency(bool transmit, float mhz);
esp_err_t radio_at_set_squelch(uint8_t level);
esp_err_t radio_at_set_volume(bool transmit, uint8_t level);
esp_err_t radio_at_set_tx_power(uint8_t level);
esp_err_t radio_at_set_rf_enabled(bool enabled);
esp_err_t radio_at_set_freq_tune(int16_t hz);
