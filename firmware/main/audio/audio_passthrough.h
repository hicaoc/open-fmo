#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Audio passthrough: manages the I2S hardware loop, ES8311 codec, and
 * bidirectional PCM data flow between the network codec layer and the
 * physical audio hardware.
 *
 * I2S runs at 16 kHz stereo 16-bit (MCLK = 256 * Fs = 4.096 MHz).
 * - Downlink (network → speaker): 8 kHz G.711 is upsampled 2x; 16 kHz
 *   Opus is passed through directly.
 * - Uplink (mic → network): 16 kHz mic frames are captured; Opus encodes
 *   them natively, G.711 downsamples 2x to 8 kHz.
 */

/* Initialize I2S bus, configure ES8311, and start the passthrough task.
 * Must be called after es8311_codec_init_control() succeeds. */
esp_err_t audio_passthrough_start(void);

/* Stop the passthrough task and release I2S. */
void audio_passthrough_stop(void);

bool audio_passthrough_is_running(void);

/* Push decoded PCM into the speaker output queue. Handles rate conversion:
 * - 16 kHz samples are queued directly.
 * - 8 kHz samples are upsampled 2x (linear interpolation). */
size_t audio_passthrough_queue_output(const int16_t *samples,
                                      size_t sample_count,
                                      uint32_t sample_rate);

typedef enum {
    AUDIO_NETWORK_NRL = 0,
    AUDIO_NETWORK_FMO = 1,
} audio_network_source_t;

typedef struct {
    bool nrl_active;
    bool fmo_active;
    audio_network_source_t primary;
    char nrl_callsign[16];
    char nrl_codec[8];
    char fmo_callsign[16];
    char fmo_codec[8];
} audio_network_status_t;

/* FMO has an independent jitter queue so concurrent NRL/FMO audio can either
 * be mixed or arbitrated without losing on-screen metadata. */
size_t audio_passthrough_queue_fmo_output(const int16_t *samples,
                                          size_t sample_count,
                                          uint32_t sample_rate);
void audio_passthrough_note_network_voice(audio_network_source_t source,
                                          const char *callsign,
                                          const char *codec);
void audio_passthrough_get_network_status(audio_network_status_t *status);
void audio_passthrough_set_audio_policy(uint8_t policy); /* 0=mix, 1=first */
void audio_passthrough_set_tx_network(uint8_t network); /* 0=NRL, 1=FMO */

/* Clear the speaker output queue (e.g. on PTT release). */
void audio_passthrough_clear_output(void);

/* TX voice codec selection (matches reference project):
 *   0 = G.711 A-law 8 kHz (compatible, default)
 *   1 = Opus 16 kHz wideband
 * RX always accepts both types regardless of this setting. */
void audio_passthrough_set_voice_codec(uint8_t codec);
uint8_t audio_passthrough_get_voice_codec(void);

/* Software mic amplification applied right after DC removal:
 * gain 1 = off (default), up to 5x, saturating. */
void audio_passthrough_set_mic_gain(uint8_t gain);
uint8_t audio_passthrough_get_mic_gain(void);
