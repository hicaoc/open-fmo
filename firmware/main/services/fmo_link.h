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
    char voice_callsign[8];
    char voice_codec[8];
    uint32_t rx_frames;
    uint32_t parse_errors;
} fmo_link_status_t;

esp_err_t fmo_link_start(const fmo_config_t *config);
void fmo_link_update_config(const fmo_config_t *config);
void fmo_link_get_status(fmo_link_status_t *status);
bool fmo_link_tx_begin(void);
bool fmo_link_tx_feed_pcm16(const int16_t *samples, size_t sample_count);
void fmo_link_tx_end(void);
