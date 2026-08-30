#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FMO_ACTIVATE_HOST_MAX 128U

/* Certificate platform host persisted in NVS.  The default is hamptt.com. */
void fmo_activate_get_host(char *out, size_t out_size);
bool fmo_activate_set_host(const char *host);

/* Last activation result, retained in RAM for the Web portal. */
void fmo_activate_get_status(char *last, size_t last_size,
                             uint64_t *last_epoch);

/* Generate/load the per-device Ed25519 key, sign an FMO-V4 activateReq with
 * the STA MAC, and install the returned user/intermediate certificates. */
esp_err_t fmo_activate_run(char *message, size_t message_size);
