#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "audio/ctcss.h"

typedef struct {
    bool configured;
    float rx_expected_hz;
    float rx_detected_hz;
    float tx_hz;
    ctcss_gate_state_t rx_state;
} nrl_audio_ctcss_status_t;

typedef void (*nrl_audio_pcm_sink_t)(const int16_t *samples,
                                     size_t sample_count,
                                     uint32_t sample_rate, void *context);

esp_err_t nrl_audio_codec_init(void);
void nrl_audio_codec_configure_ctcss(float rx_hz, float tx_hz);
void nrl_audio_codec_get_ctcss_status(nrl_audio_ctcss_status_t *status);
void nrl_audio_codec_set_sink(nrl_audio_pcm_sink_t sink, void *context);
esp_err_t nrl_audio_send_g711(const int16_t *pcm8k, size_t sample_count);
esp_err_t nrl_audio_send_opus(const int16_t *pcm16k, size_t sample_count);
/* Shared RF RX tone gate used before routing microphone audio to FMO. */
bool nrl_audio_radio_rx_accept(const int16_t *pcm, size_t sample_count,
                               uint32_t sample_rate);
void nrl_audio_receive_encoded(uint8_t type, const uint8_t *payload,
                               size_t payload_size);

/* Last received downlink voice codec: 0=G.711, 1=Opus, -1=none yet. */
int nrl_audio_codec_get_rx_codec(void);
