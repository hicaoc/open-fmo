#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "server_directory.h"

typedef enum {
    FMO_CERT_USER = 0,
    FMO_CERT_INTERMEDIATE,
    FMO_CERT_DEVICE_KEY,
} fmo_cert_kind_t;

typedef struct {
    bool user_present;
    bool intermediate_present;
    bool device_key_present;
    bool ready;
    char callsign[16];
    uint32_t uid;
    uint64_t issued_at;
    uint64_t expires_at;
    uint8_t fingerprint[32];
} fmo_identity_status_t;

/* Validate one uploaded JSON object before committing it to the flash-end FS.
 * Device private-key material is never exposed by a read API. */
esp_err_t fmo_cert_store_put(fmo_cert_kind_t kind, const char *json,
                             size_t json_size, char *error,
                             size_t error_size);
esp_err_t fmo_cert_store_status(fmo_identity_status_t *status);

/* Build the SAS MQTT CONNECT credentials. The returned password is allocated
 * with malloc() and belongs to the caller. */
esp_err_t fmo_cert_store_build_credentials(const fmo_server_t *server,
                                           const char *role,
                                           char *username,
                                           size_t username_size,
                                           char **password);

