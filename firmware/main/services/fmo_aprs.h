#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "services/server_directory.h"

bool fmo_aprs_parse_station(const char *line, size_t line_size,
                            fmo_server_t *server);

/* Extract the FMO APRS source identity (CALLSIGN-SSID).  Voice RAW frames
 * carry only the six-character base callsign, so discovery uses this mapping
 * to restore the remote station's SSID for the UI. */
bool fmo_aprs_parse_source_ssid(const char *line, size_t line_size,
                                char callsign[7], uint8_t *ssid);
