#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Net radio: a small internet-radio player.
 *
 * - Station list persisted in its own NVS namespace ("netradio"),
 *   fixed-size entries (name 48B, url 200B), capacity NET_RADIO_STATION_MAX.
 * - A single player task streams HTTP(S) audio, sniffs the format from the
 *   first bytes, decodes with esp_audio_simple_dec, resamples to 16 kHz mono
 *   and feeds audio_passthrough_queue_output().
 * - NRL downlink voice preempts playback (net_radio_notify_nrl_voice()).
 */

#define NET_RADIO_NAME_MAX    48
#define NET_RADIO_URL_MAX     200
#define NET_RADIO_STATION_MAX 16

typedef enum {
    NET_RADIO_STATE_IDLE = 0,
    NET_RADIO_STATE_CONNECTING,
    NET_RADIO_STATE_PLAYING,
    NET_RADIO_STATE_ERROR,
} net_radio_state_t;

typedef struct {
    net_radio_state_t state;
    int current;                          /* playing/requested index, -1 none */
    char station_name[NET_RADIO_NAME_MAX];
    char error[48];                       /* reason when state == ERROR */
} net_radio_status_t;

/* Load the station list from NVS. Call once after config_store_init(). */
esp_err_t net_radio_init(void);

/* Station list */
size_t net_radio_count(void);
bool net_radio_get(size_t index, char *name, size_t name_size,
                   char *url, size_t url_size);
bool net_radio_add(const char *name, const char *url);
bool net_radio_remove(size_t index);
int net_radio_get_current(void);

/* Playback control (all non-blocking; the player task reacts within ~1 s) */
bool net_radio_play(size_t index);
void net_radio_stop(void);
bool net_radio_next(void);
bool net_radio_prev(void);
bool net_radio_is_playing(void);          /* CONNECTING or PLAYING */
void net_radio_get_status(net_radio_status_t *status);

/* Hook for nrl_audio_codec: an NRL downlink voice packet arrived.
 * Stops radio playback so voice is not mixed with music. */
void net_radio_notify_nrl_voice(void);
