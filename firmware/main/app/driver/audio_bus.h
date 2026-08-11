#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_types.h"
#include "esp_err.h"

/* Shared full-duplex I2S bus used by the codec and the NRL audio pipeline. */
esp_err_t audio_bus_init(uint32_t sample_rate_hz);
void audio_bus_deinit(void);
bool audio_bus_is_ready(void);
esp_err_t audio_bus_get_channels(i2s_chan_handle_t *tx_channel,
                                 i2s_chan_handle_t *rx_channel);
