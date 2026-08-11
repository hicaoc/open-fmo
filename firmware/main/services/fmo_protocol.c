#include "fmo_protocol.h"

#include <string.h>

#include "sodium.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} cbor_reader_t;

static bool cbor_head(cbor_reader_t *reader, uint8_t *major, uint64_t *value)
{
    if (reader->offset >= reader->size) return false;
    uint8_t first = reader->data[reader->offset++];
    *major = first >> 5;
    uint8_t additional = first & 0x1f;
    if (additional < 24) {
        *value = additional;
        return true;
    }
    size_t bytes = additional == 24 ? 1 : additional == 25 ? 2 :
                   additional == 26 ? 4 : additional == 27 ? 8 : 0;
    if (bytes == 0 || reader->offset + bytes > reader->size) return false;
    uint64_t parsed = 0;
    for (size_t i = 0; i < bytes; ++i) {
        parsed = (parsed << 8) | reader->data[reader->offset++];
    }
    *value = parsed;
    return true;
}

static bool cbor_uint(cbor_reader_t *reader, uint64_t *value)
{
    uint8_t major;
    return cbor_head(reader, &major, value) && major == 0;
}

static bool cbor_blob(cbor_reader_t *reader, uint8_t expected_major,
                      const uint8_t **data, size_t *size)
{
    uint8_t major;
    uint64_t length;
    if (!cbor_head(reader, &major, &length) || major != expected_major ||
        length > SIZE_MAX || reader->offset + (size_t)length > reader->size) {
        return false;
    }
    *data = reader->data + reader->offset;
    *size = (size_t)length;
    reader->offset += (size_t)length;
    return true;
}

static bool text_equals(const uint8_t *text, size_t size, const char *literal)
{
    size_t expected = strlen(literal);
    return size == expected && memcmp(text, literal, expected) == 0;
}

bool fmo_protocol_parse_beacon_cert(const char *base64url,
                                    fmo_public_cert_t *certificate)
{
    if (base64url == NULL || certificate == NULL || sodium_init() < 0) {
        return false;
    }
    uint8_t raw[512];
    size_t raw_size = 0;
    if (sodium_base642bin(raw, sizeof(raw), base64url, strlen(base64url),
                          NULL, &raw_size, NULL,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return false;
    }
    cbor_reader_t reader = {.data = raw, .size = raw_size};
    uint8_t major;
    uint64_t array_count;
    if (!cbor_head(&reader, &major, &array_count) || major != 4 ||
        array_count != 10) return false;
    size_t first_item = reader.offset;
    const uint8_t *text = NULL, *bytes = NULL;
    size_t text_size = 0, bytes_size = 0;
    uint64_t version = 0, algorithm = 0, uid = 0, iat = 0, exp = 0;
    if (!cbor_blob(&reader, 3, &text, &text_size) ||
        !text_equals(text, text_size, "FMO") ||
        !cbor_uint(&reader, &version) || version != 4 ||
        !cbor_blob(&reader, 3, &text, &text_size) ||
        !text_equals(text, text_size, "userCert") ||
        !cbor_uint(&reader, &algorithm) || algorithm > UINT32_MAX ||
        !cbor_blob(&reader, 3, &text, &text_size) || text_size == 0 ||
        text_size >= sizeof(certificate->callsign)) return false;
    memset(certificate, 0, sizeof(*certificate));
    memcpy(certificate->callsign, text, text_size);
    certificate->callsign[text_size] = '\0';
    if (!cbor_uint(&reader, &uid) || uid > UINT32_MAX ||
        !cbor_blob(&reader, 2, &bytes, &bytes_size) || bytes_size != 32) {
        return false;
    }
    memcpy(certificate->public_key, bytes, 32);
    if (!cbor_uint(&reader, &iat) || !cbor_uint(&reader, &exp)) return false;
    size_t signature_item = reader.offset;
    if (!cbor_blob(&reader, 2, &bytes, &bytes_size) || bytes_size != 64 ||
        reader.offset != raw_size) return false;

    uint8_t tbs[512];
    size_t tbs_payload = signature_item - first_item;
    if (1 + tbs_payload > sizeof(tbs)) return false;
    tbs[0] = 0x89; /* canonical CBOR array(9) */
    memcpy(tbs + 1, raw + first_item, tbs_payload);
    crypto_hash_sha256(certificate->fingerprint, tbs, 1 + tbs_payload);
    certificate->uid = (uint32_t)uid;
    certificate->algorithm = (uint32_t)algorithm;
    certificate->issued_at = iat;
    certificate->expires_at = exp;
    return true;
}

