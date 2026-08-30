#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"
#include "services/server_directory.h"

typedef struct {
    bool configured;
    bool connected;
    bool receiving;
    esp_err_t last_error;
    char server_name[FMO_SERVER_NAME_MAX];
    char server_callsign[16];
    char client_id[48];
    char role[8];           /* role accepted by the SAS on this connection */
    char voice_callsign[8];
    char voice_codec[8];
    uint32_t rx_frames;
    uint32_t parse_errors;
} fmo_link_status_t;

esp_err_t fmo_link_start(const fmo_config_t *config);
void fmo_link_update_config(const fmo_config_t *config);

/* Ask the internal-RAM FMO control task to reload the certificate chain and
 * reconnect.  Call after a certificate upload; do not read SPIFFS from an
 * external-PSRAM task such as the audio pipeline. */
void fmo_link_request_certificate_refresh(void);
void fmo_link_get_status(fmo_link_status_t *status);
bool fmo_link_tx_begin(void);
bool fmo_link_tx_feed_pcm16(const int16_t *samples, size_t sample_count);
void fmo_link_tx_end(void);

/* Snapshot of the currently selected FMO server (directory entry), used by
 * the QSO engine to answer QTHQRY with our server uid. */
bool fmo_link_get_selected_server(fmo_server_t *server);
/* True while the MQTT link is up on the server with this directory key. */
bool fmo_link_connected_to(const char *key);
/* Runtime-only server switch (QSO jump to the callee's server): updates the
 * requested key without touching NVS.  False when the key is not in the
 * directory. */
bool fmo_link_jump_to_key(const char *key);
/* Thin arbitrary-topic publish for non-voice payloads (QSO records).
 * `length` is the payload byte count (not necessarily NUL-terminated). */
bool fmo_link_publish(const char *topic, const char *payload, int length);
