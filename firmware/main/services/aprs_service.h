#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"

typedef struct {
    bool enabled;
    bool position_set;
    bool connected;
    uint32_t rx_count;
    uint32_t tx_count;
    char last_source[16];
    char last_text[96];
} aprs_status_t;

typedef struct {
    char callsign[16];
    float distance_km;
    float speed_kmh;
    float course_deg;
    bool has_distance;
    bool has_speed;
    bool has_course;
    uint32_t received_ms;
} aprs_recent_packet_t;

esp_err_t aprs_service_start(const fmo_config_t *config);
void aprs_service_update_config(const fmo_config_t *config);
bool aprs_service_set_enabled(bool enabled);
void aprs_service_get_status(aprs_status_t *status);
size_t aprs_service_get_recent(aprs_recent_packet_t *packets, size_t capacity);
bool aprs_service_send_beacon_now(void);

/* Accept WGS-84 decimal degrees (31.8885, 118.8141) or APRS/NMEA
 * degrees+minutes (3153.3100N, 11848.8460E). Hemisphere is optional; a
 * leading minus sign selects south/west. The result is normalized to signed
 * microdegrees for persistent storage. */
bool aprs_service_parse_coordinate(const char *text, bool latitude,
                                   int32_t *microdegrees);
