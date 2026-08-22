#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"

typedef struct {
    bool enabled;         /* configured on */
    uint8_t interval_min; /* 5/10/60 */
    bool broadcasting;    /* all gates currently pass */
    uint32_t tx_count;
    uint32_t last_tx_ms;
    uint32_t auto_online; /* distinct heartbeat uids seen in the 120 s window */
    uint32_t auto_peak;   /* running peak of auto_online, seeded from NVS */
    char host[64];        /* advertised server host (selected FMO server) */
    uint16_t port;
    char gate[64];        /* first failing gate reason, or readiness text */
    /* Personal BEACON state (FMO-V4 BEACON, fixed 10-minute period). */
    bool beacon_enabled;
    uint32_t beacon_tx_count;
    uint32_t beacon_last_tx_ms;
    char beacon_gate[64];
} fmo_station_beacon_status_t;

esp_err_t fmo_station_beacon_start(const fmo_config_t *config);
void fmo_station_beacon_update_config(const fmo_config_t *config);
void fmo_station_beacon_get_status(fmo_station_beacon_status_t *status);

/* Online roster, fed by fmo_link from the FMO/LATE/UID_V1/<uid> heartbeats. */
void fmo_station_note_uid(uint32_t uid);
void fmo_station_clear_online(void); /* on MQTT disconnect; keeps the peak */
uint32_t fmo_station_online_auto(void);
uint32_t fmo_station_peak_auto(void);
