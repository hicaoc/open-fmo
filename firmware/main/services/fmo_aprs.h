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

/* Convert a UTF-8 string to CP936/GBK bytes.  Returns false on characters
 * that have no GBK mapping; ASCII passes through unchanged.
 * Kept for reference/legacy tooling only: the FMO-V4 send path no longer
 * calls it — wire text is UTF-8 per the protocol spec (the map server
 * rejects GBK).  The receive side still parses legacy GBK station names via
 * the gbk_to_utf8() direction in fmo_aprs.c. */
bool fmo_aprs_utf8_to_gbk(const char *input, char *output, size_t output_size);

/* Compare two callsigns by base part only: any "-SSID" suffix and letter
 * case are ignored ("BG9JYT-14" == "bg9jyt").  Used by the own-server gate
 * where the server list may carry an SSID but the certificate never does. */
bool fmo_aprs_base_callsign_eq(const char *a, const char *b);
