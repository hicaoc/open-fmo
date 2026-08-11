#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t nv3007_init(void);
esp_err_t nv3007_set_backlight(bool enabled);
esp_err_t nv3007_set_backlight_percent(uint8_t percent);
esp_err_t nv3007_flush_rgb565(const uint16_t *pixels, int width, int height);
