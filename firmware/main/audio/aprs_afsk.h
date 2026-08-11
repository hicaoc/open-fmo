#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bell 202 AFSK APRS channel glue (ported from NRL-ESP32):
 *  - RX: two demodulators. 0 = radio mic (RF) tap, 1 = NRL network
 *    downlink tap. Audio tasks feed the taps; a dedicated task resamples
 *    to 9600 Hz and demodulates.
 *  - TX: one modulator at 8 kHz. Generated PCM fans out over the enabled
 *    routes: speaker queue (radio VOX keying) and/or NRL voice uplink
 *    (G.711 A-law frames).
 * All modem/AX.25 state is touched by the AFSK task only; the public API
 * is thread-safe (audio tasks feed rings, services push/pull queues). */

#define APRS_AFSK_SOURCE_RF 0U
#define APRS_AFSK_SOURCE_NRL 1U

esp_err_t aprs_afsk_init(void);

/* Feed received PCM for demodulation (safe from audio tasks). Samples are
 * dropped silently when the corresponding RX switch is off or the ring is
 * full. Supported rates: 8000 and 16000 Hz. */
void aprs_afsk_feed_rf(const int16_t *pcm, size_t samples, uint32_t rate);
void aprs_afsk_feed_nrl(const int16_t *pcm, size_t samples, uint32_t rate);

/* Live RX switches consulted by the feed functions. */
void aprs_afsk_set_rx_routes(bool rf_rx, bool nrl_rx);

/* Live TX routes for the modulator output. */
void aprs_afsk_set_tx_routes(bool rf_tx, bool nrl_tx);

/* Queue one TNC2 monitor line ("SRC>DST,path:info") for AFSK transmission.
 * Returns false when the queue is full or the line cannot be encoded. */
bool aprs_afsk_send_line(const char *tnc2);

/* Pop one decoded frame. `source` receives APRS_AFSK_SOURCE_*. */
bool aprs_afsk_get_rx_frame(char *line, size_t size, uint8_t *source);

/* True while an AFSK frame is on air (the mic uplink must be parked). */
bool aprs_afsk_is_tx_active(void);

#ifdef __cplusplus
}
#endif
