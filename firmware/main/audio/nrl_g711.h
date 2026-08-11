#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t nrl_g711_init(void);
uint8_t nrl_g711_encode_alaw(int16_t pcm);
int16_t nrl_g711_decode_alaw(uint8_t alaw);

