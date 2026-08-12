#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Detection: can be called before I2S clocks are running. */
esp_err_t es8311_codec_init_control(void);
bool es8311_codec_is_present(void);

/* Full register configuration: MUST be called after I2S MCLK is running. */
esp_err_t es8311_codec_configure(void);
bool es8311_codec_is_configured(void);

esp_err_t es8311_codec_read(uint8_t reg, uint8_t *value);
esp_err_t es8311_codec_write(uint8_t reg, uint8_t value);
esp_err_t es8311_codec_set_dac_mute(bool muted);
esp_err_t es8311_codec_set_adc_volume(uint8_t volume);
esp_err_t es8311_codec_set_dac_volume(uint8_t volume);
esp_err_t es8311_codec_set_headphone_drive(bool enabled);
