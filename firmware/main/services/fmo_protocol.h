#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char callsign[16];
    uint32_t uid;
    uint32_t algorithm;
    uint8_t public_key[32];
    uint8_t fingerprint[32];
    uint64_t issued_at;
    uint64_t expires_at;
} fmo_public_cert_t;

/* Decode the FMO-V4 CERT field (base64url CBOR userCert) and calculate the
 * canonical SHA-256 TBS fingerprint used by SAS. */
bool fmo_protocol_parse_beacon_cert(const char *base64url,
                                    fmo_public_cert_t *certificate);

