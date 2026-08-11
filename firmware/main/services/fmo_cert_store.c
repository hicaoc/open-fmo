#include "fmo_cert_store.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "sodium.h"
#include "storage_fs.h"

#define USER_FILE "cert_user.json"
#define INT_FILE "cert_int.json"
#define KEY_FILE "cert_devicekey.json"
#define CERT_FILE_MAX (24U * 1024U)

typedef struct {
    char callsign[16];
    uint32_t uid;
    uint64_t iat;
    uint64_t exp;
    uint32_t issuer_sn;
    uint8_t public_key[32];
    uint8_t signature[64];
    uint8_t fingerprint[32];
} user_cert_t;

typedef struct {
    uint8_t seed[32];
    uint8_t public_key[32];
} device_key_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
} cbor_writer_t;

static const char *file_for_kind(fmo_cert_kind_t kind)
{
    switch (kind) {
        case FMO_CERT_USER: return USER_FILE;
        case FMO_CERT_INTERMEDIATE: return INT_FILE;
        case FMO_CERT_DEVICE_KEY: return KEY_FILE;
        default: return NULL;
    }
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

static bool number_u64(const cJSON *object, const char *name, uint64_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
        value->valuedouble > (double)UINT64_MAX) return false;
    uint64_t parsed = (uint64_t)value->valuedouble;
    if ((double)parsed != value->valuedouble) return false;
    *out = parsed;
    return true;
}

static bool decode32(const char *text, uint8_t out[32])
{
    if (text == NULL) return false;
    size_t size = 0;
    const int variants[] = {
        sodium_base64_VARIANT_URLSAFE_NO_PADDING,
        sodium_base64_VARIANT_URLSAFE,
        sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
        sodium_base64_VARIANT_ORIGINAL,
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        if (sodium_base642bin(out, 32, text, strlen(text), NULL, &size, NULL,
                              variants[i]) == 0 && size == 32) return true;
    }
    return false;
}

static bool decode64(const char *text, uint8_t out[64])
{
    if (text == NULL) return false;
    size_t size = 0;
    const int variants[] = {
        sodium_base64_VARIANT_URLSAFE_NO_PADDING,
        sodium_base64_VARIANT_URLSAFE,
        sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
        sodium_base64_VARIANT_ORIGINAL,
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        if (sodium_base642bin(out, 64, text, strlen(text), NULL, &size, NULL,
                              variants[i]) == 0 && size == 64) return true;
    }
    return false;
}

static void cbor_bytes(cbor_writer_t *writer, const void *data, size_t size)
{
    if (!writer->ok || writer->size + size > writer->capacity) {
        writer->ok = false;
        return;
    }
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
}

static void cbor_head(cbor_writer_t *writer, uint8_t major, uint64_t value)
{
    uint8_t encoded[9];
    size_t size = 1;
    if (value < 24) {
        encoded[0] = (uint8_t)((major << 5) | value);
    } else if (value <= UINT8_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 24);
        encoded[1] = (uint8_t)value;
        size = 2;
    } else if (value <= UINT16_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 25);
        encoded[1] = (uint8_t)(value >> 8);
        encoded[2] = (uint8_t)value;
        size = 3;
    } else if (value <= UINT32_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 26);
        for (size_t i = 0; i < 4; ++i) encoded[1 + i] = (uint8_t)(value >> (24 - i * 8));
        size = 5;
    } else {
        encoded[0] = (uint8_t)((major << 5) | 27);
        for (size_t i = 0; i < 8; ++i) encoded[1 + i] = (uint8_t)(value >> (56 - i * 8));
        size = 9;
    }
    cbor_bytes(writer, encoded, size);
}

static void cbor_uint(cbor_writer_t *writer, uint64_t value)
{
    cbor_head(writer, 0, value);
}

static void cbor_text(cbor_writer_t *writer, const char *text)
{
    size_t size = strlen(text);
    cbor_head(writer, 3, size);
    cbor_bytes(writer, text, size);
}

static void cbor_blob(cbor_writer_t *writer, const uint8_t *data, size_t size)
{
    cbor_head(writer, 2, size);
    cbor_bytes(writer, data, size);
}

static bool build_user_tbs(const user_cert_t *cert, uint8_t *out,
                           size_t capacity, size_t *out_size)
{
    cbor_writer_t writer = {.data = out, .capacity = capacity, .ok = true};
    cbor_head(&writer, 4, 9);
    cbor_text(&writer, "FMO");
    cbor_uint(&writer, 4);
    cbor_text(&writer, "userCert");
    cbor_uint(&writer, cert->issuer_sn);
    cbor_text(&writer, cert->callsign);
    cbor_uint(&writer, cert->uid);
    cbor_blob(&writer, cert->public_key, sizeof(cert->public_key));
    cbor_uint(&writer, cert->iat);
    cbor_uint(&writer, cert->exp);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

static bool parse_user(const cJSON *root, user_cert_t *cert)
{
    memset(cert, 0, sizeof(*cert));
    uint64_t issuer = 0, uid = 0;
    const cJSON *subject = cJSON_GetObjectItemCaseSensitive(root, "subject");
    const cJSON *callsign = cJSON_GetObjectItemCaseSensitive(subject, "callsign");
    const cJSON *public_key = cJSON_GetObjectItemCaseSensitive(subject, "publicKey");
    const cJSON *signature = cJSON_GetObjectItemCaseSensitive(root, "signature");
    if (!cJSON_IsObject(root) || !cJSON_IsObject(subject) ||
        !cJSON_IsString(callsign) || callsign->valuestring[0] == '\0' ||
        strlen(callsign->valuestring) >= sizeof(cert->callsign) ||
        !cJSON_IsString(public_key) || !cJSON_IsString(signature) ||
        !number_u64(root, "issuerSn", &issuer) || issuer > UINT32_MAX ||
        !number_u64(subject, "uid", &uid) || uid > UINT32_MAX ||
        !number_u64(root, "iat", &cert->iat) ||
        !number_u64(root, "exp", &cert->exp) || cert->exp <= cert->iat ||
        !decode32(public_key->valuestring, cert->public_key) ||
        !decode64(signature->valuestring, cert->signature)) return false;
    for (const char *p = callsign->valuestring; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-') return false;
    }
    snprintf(cert->callsign, sizeof(cert->callsign), "%s", callsign->valuestring);
    cert->uid = (uint32_t)uid;
    cert->issuer_sn = (uint32_t)issuer;
    uint8_t tbs[256];
    size_t tbs_size = 0;
    if (!build_user_tbs(cert, tbs, sizeof(tbs), &tbs_size)) return false;
    crypto_hash_sha256(cert->fingerprint, tbs, tbs_size);
    return true;
}

static bool parse_key(const cJSON *root, device_key_t *key)
{
    const cJSON *seed = cJSON_GetObjectItemCaseSensitive(root, "seed");
    const cJSON *public_key = cJSON_GetObjectItemCaseSensitive(root, "pubKey");
    return cJSON_IsObject(root) && cJSON_IsString(seed) &&
           cJSON_IsString(public_key) &&
           decode32(seed->valuestring, key->seed) &&
           decode32(public_key->valuestring, key->public_key);
}

static bool parse_intermediate(const cJSON *root, uint8_t public_key[32])
{
    const cJSON *subject = cJSON_GetObjectItemCaseSensitive(root, "subject");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(subject, "publicKey");
    return cJSON_IsObject(root) && cJSON_IsObject(subject) &&
           cJSON_IsString(key) && decode32(key->valuestring, public_key);
}

static esp_err_t read_json(const char *name, uint8_t **raw, size_t *size,
                           cJSON **json)
{
    ESP_RETURN_ON_ERROR(storage_fs_read(name, raw, size, CERT_FILE_MAX),
                        "fmo_cert", "read %s", name);
    *json = cJSON_ParseWithLength((const char *)*raw, *size);
    if (*json == NULL) {
        free(*raw);
        *raw = NULL;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t fmo_cert_store_put(fmo_cert_kind_t kind, const char *json,
                             size_t json_size, char *error,
                             size_t error_size)
{
    if (sodium_init() < 0) {
        set_error(error, error_size, "证书加密库初始化失败");
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_fs_is_ready()) {
        set_error(error, error_size,
                  "Flash 文件系统未就绪，请完整刷写固件后重启");
        return ESP_ERR_INVALID_STATE;
    }
    const char *name = file_for_kind(kind);
    if (name == NULL || json == NULL || json_size == 0 ||
        json_size > CERT_FILE_MAX) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_ParseWithLength(json, json_size);
    if (root == NULL) {
        set_error(error, error_size, "JSON 格式错误");
        return ESP_ERR_INVALID_ARG;
    }
    bool valid = false;
    if (kind == FMO_CERT_USER) {
        user_cert_t cert;
        valid = parse_user(root, &cert);
    } else if (kind == FMO_CERT_DEVICE_KEY) {
        device_key_t key;
        valid = parse_key(root, &key);
    } else {
        uint8_t public_key[32];
        valid = parse_intermediate(root, public_key);
    }
    cJSON_Delete(root);
    if (!valid) {
        set_error(error, error_size, "证书字段或 Base64 内容无效");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = storage_fs_write_atomic(name, json, json_size);
    if (result != ESP_OK) set_error(error, error_size, "写入 Flash 文件系统失败");
    return result;
}

esp_err_t fmo_cert_store_status(fmo_identity_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->user_present = storage_fs_exists(USER_FILE);
    status->intermediate_present = storage_fs_exists(INT_FILE);
    status->device_key_present = storage_fs_exists(KEY_FILE);
    if (!status->user_present || !status->intermediate_present ||
        !status->device_key_present) return ESP_OK;

    uint8_t *user_raw = NULL, *int_raw = NULL, *key_raw = NULL;
    size_t size = 0;
    cJSON *user_json = NULL, *int_json = NULL, *key_json = NULL;
    esp_err_t result = read_json(USER_FILE, &user_raw, &size, &user_json);
    if (result != ESP_OK) goto done;
    result = read_json(INT_FILE, &int_raw, &size, &int_json);
    if (result != ESP_OK) goto done;
    result = read_json(KEY_FILE, &key_raw, &size, &key_json);
    if (result != ESP_OK) goto done;

    user_cert_t cert;
    device_key_t key;
    uint8_t intermediate_key[32], derived_public[32], secret[64];
    if (!parse_user(user_json, &cert) || !parse_key(key_json, &key) ||
        !parse_intermediate(int_json, intermediate_key) ||
        crypto_sign_seed_keypair(derived_public, secret, key.seed) != 0 ||
        sodium_memcmp(derived_public, key.public_key, 32) != 0 ||
        sodium_memcmp(derived_public, cert.public_key, 32) != 0) {
        result = ESP_ERR_INVALID_CRC;
        sodium_memzero(secret, sizeof(secret));
        goto done;
    }
    uint8_t tbs[256];
    size_t tbs_size = 0;
    if (!build_user_tbs(&cert, tbs, sizeof(tbs), &tbs_size) ||
        crypto_sign_verify_detached(cert.signature, tbs, tbs_size,
                                    intermediate_key) != 0) {
        result = ESP_ERR_INVALID_CRC;
        sodium_memzero(secret, sizeof(secret));
        goto done;
    }
    time_t now = time(NULL);
    if (now >= 1700000000 &&
        ((uint64_t)now < cert.iat || (uint64_t)now >= cert.exp)) {
        result = ESP_ERR_INVALID_STATE;
        sodium_memzero(secret, sizeof(secret));
        goto done;
    }
    sodium_memzero(secret, sizeof(secret));
    snprintf(status->callsign, sizeof(status->callsign), "%s", cert.callsign);
    status->uid = cert.uid;
    status->issued_at = cert.iat;
    status->expires_at = cert.exp;
    memcpy(status->fingerprint, cert.fingerprint, 32);
    status->ready = true;
    result = ESP_OK;

done:
    cJSON_Delete(user_json);
    cJSON_Delete(int_json);
    cJSON_Delete(key_json);
    free(user_raw);
    free(int_raw);
    free(key_raw);
    return result;
}

static bool build_proof_tbs(const fmo_server_t *server, const char *role,
                            uint64_t timestamp, const user_cert_t *cert,
                            uint8_t *out, size_t capacity, size_t *out_size)
{
    char upper[sizeof(server->callsign)];
    snprintf(upper, sizeof(upper), "%s", server->callsign);
    for (char *p = upper; *p; ++p) *p = (char)toupper((unsigned char)*p);
    cbor_writer_t writer = {.data = out, .capacity = capacity, .ok = true};
    cbor_head(&writer, 4, 12);
    cbor_text(&writer, "FMO");
    cbor_uint(&writer, 4);
    cbor_text(&writer, "serverAuthorizerReqHttp");
    cbor_uint(&writer, server->uid);
    cbor_text(&writer, upper);
    cbor_uint(&writer, server->uid);
    cbor_text(&writer, role);
    cbor_text(&writer, server->host);
    cbor_uint(&writer, server->port);
    cbor_blob(&writer, server->fingerprint, 32);
    cbor_uint(&writer, timestamp);
    cbor_blob(&writer, cert->fingerprint, 32);
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

static char *b64url_alloc(const uint8_t *data, size_t size)
{
    size_t capacity = sodium_base64_ENCODED_LEN(
        size, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *encoded = malloc(capacity);
    if (encoded != NULL) {
        sodium_bin2base64(encoded, capacity, data, size,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    }
    return encoded;
}

esp_err_t fmo_cert_store_build_credentials(const fmo_server_t *server,
                                           const char *role,
                                           char *username,
                                           size_t username_size,
                                           char **password)
{
    if (server == NULL || role == NULL || username == NULL ||
        username_size == 0 || password == NULL || !server->has_fingerprint ||
        server->uid == 0 || server->host[0] == '\0' ||
        server->callsign[0] == '\0') return ESP_ERR_INVALID_ARG;
    *password = NULL;
    fmo_identity_status_t status;
    ESP_RETURN_ON_ERROR(fmo_cert_store_status(&status), "fmo_cert",
                        "identity invalid");
    if (!status.ready) return ESP_ERR_INVALID_STATE;

    uint8_t *user_raw = NULL, *int_raw = NULL, *key_raw = NULL;
    size_t user_size = 0, int_size = 0, key_size = 0;
    cJSON *user_json = NULL, *int_json = NULL, *key_json = NULL;
    esp_err_t result = read_json(USER_FILE, &user_raw, &user_size, &user_json);
    if (result != ESP_OK) goto done;
    result = read_json(INT_FILE, &int_raw, &int_size, &int_json);
    if (result != ESP_OK) goto done;
    result = read_json(KEY_FILE, &key_raw, &key_size, &key_json);
    if (result != ESP_OK) goto done;

    user_cert_t cert;
    device_key_t key;
    if (!parse_user(user_json, &cert) || !parse_key(key_json, &key)) {
        result = ESP_ERR_INVALID_ARG;
        goto done;
    }
    uint64_t timestamp = (uint64_t)time(NULL);
    if (timestamp < 1700000000ULL) {
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    uint8_t tbs[512], secret[64], public_key[32], signature[64];
    size_t tbs_size = 0;
    if (!build_proof_tbs(server, role, timestamp, &cert, tbs, sizeof(tbs),
                         &tbs_size) ||
        crypto_sign_seed_keypair(public_key, secret, key.seed) != 0 ||
        crypto_sign_detached(signature, NULL, tbs, tbs_size, secret) != 0) {
        sodium_memzero(secret, sizeof(secret));
        result = ESP_FAIL;
        goto done;
    }
    sodium_memzero(secret, sizeof(secret));
    char *server_fp = b64url_alloc(server->fingerprint, 32);
    char *signature_text = b64url_alloc(signature, 64);
    if (server_fp == NULL || signature_text == NULL) {
        free(server_fp);
        free(signature_text);
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    cJSON *payload_json = cJSON_CreateObject();
    cJSON *package = cJSON_CreateObject();
    cJSON *proof = cJSON_CreateObject();
    cJSON *int_copy = cJSON_Duplicate(int_json, true);
    cJSON *user_copy = cJSON_Duplicate(user_json, true);
    if (payload_json == NULL || package == NULL || proof == NULL ||
        int_copy == NULL || user_copy == NULL) {
        cJSON_Delete(payload_json);
        cJSON_Delete(package);
        cJSON_Delete(proof);
        cJSON_Delete(int_copy);
        cJSON_Delete(user_copy);
        free(server_fp);
        free(signature_text);
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddItemToObject(package, "intermediateCert", int_copy);
    cJSON_AddItemToObject(package, "userCert", user_copy);
    cJSON_AddItemToObject(payload_json, "certPackage", package);
    cJSON_AddStringToObject(payload_json, "targetCallsign", server->callsign);
    cJSON_AddNumberToObject(payload_json, "targetUID", server->uid);
    cJSON_AddStringToObject(payload_json, "role", role);
    cJSON_AddStringToObject(payload_json, "targetUrl", server->host);
    cJSON_AddNumberToObject(payload_json, "targetPort", server->port);
    cJSON_AddStringToObject(payload_json, "serverFingerprint", server_fp);
    cJSON_AddNumberToObject(payload_json, "timestamp", (double)timestamp);
    cJSON_AddStringToObject(proof, "signature", signature_text);
    cJSON_AddItemToObject(payload_json, "proof", proof);
    free(server_fp);
    free(signature_text);
    char *payload = cJSON_PrintUnformatted(payload_json);
    cJSON_Delete(payload_json);
    if (payload == NULL) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    *password = b64url_alloc((const uint8_t *)payload, strlen(payload));
    cJSON_free(payload);
    if (*password == NULL) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    snprintf(username, username_size, "%s", cert.callsign);
    result = ESP_OK;

done:
    cJSON_Delete(user_json);
    cJSON_Delete(int_json);
    cJSON_Delete(key_json);
    free(user_raw);
    free(int_raw);
    free(key_raw);
    return result;
}
