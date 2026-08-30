#include "fmo_activate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "nvs.h"
#include "sodium.h"

#include "services/fmo_cert_store.h"
#include "services/network_manager.h"
#include "services/storage_fs.h"
#include "version.h"

#define ACTIVATE_PATH "/api/device/activate"
#define DEVICE_KEY_FILE "cert_devicekey.json"
#define RESPONSE_CAPACITY (32U * 1024U)

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
} cbor_writer_t;

typedef struct {
    uint8_t mac[6];
    uint64_t timestamp;
    uint8_t nonce[6];
    uint8_t firmware_hash[32];
    uint8_t public_key[32];
    uint8_t signature[64];
} activate_request_t;

typedef struct {
    char result[12];
    long code;
    char reason[96];
    char *user_cert;
    char *intermediate_cert;
} activate_response_t;

static const char *NVS_NAMESPACE = "fmo_activate";
static const char *NVS_HOST_KEY = "host";
static const char *DEFAULT_HOST = "www.hamptt.com";
static char s_last[128];
static uint64_t s_last_epoch;

static void set_message(char *message, size_t message_size, const char *text)
{
    if (message != NULL && message_size > 0) {
        strlcpy(message, text, message_size);
    }
}

static void cbor_bytes(cbor_writer_t *writer, const void *data, size_t size)
{
    if (!writer->ok || size > writer->capacity - writer->size) {
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
    if (value < 24) encoded[0] = (uint8_t)((major << 5) | value);
    else if (value <= UINT8_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 24); encoded[1] = (uint8_t)value; size = 2;
    } else if (value <= UINT16_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 25);
        encoded[1] = (uint8_t)(value >> 8); encoded[2] = (uint8_t)value; size = 3;
    } else if (value <= UINT32_MAX) {
        encoded[0] = (uint8_t)((major << 5) | 26);
        for (size_t i = 0; i < 4; ++i) encoded[i + 1] = (uint8_t)(value >> (24 - i * 8));
        size = 5;
    } else {
        encoded[0] = (uint8_t)((major << 5) | 27);
        for (size_t i = 0; i < 8; ++i) encoded[i + 1] = (uint8_t)(value >> (56 - i * 8));
        size = 9;
    }
    cbor_bytes(writer, encoded, size);
}

static void cbor_text(cbor_writer_t *writer, const char *text)
{
    cbor_head(writer, 3, strlen(text));
    cbor_bytes(writer, text, strlen(text));
}

static void cbor_blob(cbor_writer_t *writer, const uint8_t *data, size_t size)
{
    cbor_head(writer, 2, size);
    cbor_bytes(writer, data, size);
}

static bool build_signature_payload(const activate_request_t *request,
                                    uint8_t *out, size_t capacity,
                                    size_t *out_size)
{
    cbor_writer_t writer = {.data = out, .capacity = capacity, .ok = true};
    cbor_head(&writer, 4, 10);
    cbor_text(&writer, "FMO");
    cbor_head(&writer, 0, 4);
    cbor_text(&writer, "activateReq");
    cbor_blob(&writer, request->mac, sizeof(request->mac));
    cbor_head(&writer, 0, request->timestamp);
    cbor_blob(&writer, request->nonce, sizeof(request->nonce));
    cbor_text(&writer, FMO_FIRMWARE_VERSION);
    cbor_blob(&writer, request->firmware_hash, sizeof(request->firmware_hash));
    cbor_text(&writer, "CN");
    cbor_blob(&writer, request->public_key, sizeof(request->public_key));
    if (!writer.ok) return false;
    *out_size = writer.size;
    return true;
}

static void hex_encode(const uint8_t *data, size_t size, char *out, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    out[size * 2] = '\0';
}

static bool decode_fixed(const char *text, uint8_t *out, size_t expected)
{
    if (text == NULL) return false;
    const int variants[] = {sodium_base64_VARIANT_URLSAFE_NO_PADDING,
                            sodium_base64_VARIANT_URLSAFE,
                            sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
                            sodium_base64_VARIANT_ORIGINAL};
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        size_t actual = 0;
        if (sodium_base642bin(out, expected, text, strlen(text), NULL,
                              &actual, NULL, variants[i]) == 0 &&
            actual == expected) return true;
    }
    return false;
}

static char *b64url_alloc(const uint8_t *data, size_t size)
{
    size_t capacity = sodium_base64_ENCODED_LEN(
        size, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    char *out = malloc(capacity);
    if (out != NULL) sodium_bin2base64(out, capacity, data, size,
                                       sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return out;
}

static esp_err_t ensure_device_key(uint8_t seed[32], uint8_t public_key[32],
                                   char *message, size_t message_size)
{
    uint8_t *raw = NULL;
    size_t size = 0;
    esp_err_t read_error = storage_fs_read(DEVICE_KEY_FILE, &raw, &size, 1024);
    if (read_error == ESP_OK) {
        cJSON *root = cJSON_ParseWithLength((const char *)raw, size);
        free(raw);
        const cJSON *seed_json = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "seed");
        const cJSON *pub_json = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "pubKey");
        bool valid = cJSON_IsString(seed_json) && cJSON_IsString(pub_json) &&
                     decode_fixed(seed_json->valuestring, seed, 32) &&
                     decode_fixed(pub_json->valuestring, public_key, 32);
        cJSON_Delete(root);
        if (valid) return ESP_OK;
        set_message(message, message_size, "deviceKey file is invalid; upload a valid deviceKey JSON");
        return ESP_ERR_INVALID_CRC;
    }
    if (read_error != ESP_ERR_NOT_FOUND) {
        set_message(message, message_size, "deviceKey storage is unavailable");
        return read_error;
    }

    randombytes_buf(seed, 32);
    uint8_t secret[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_seed_keypair(public_key, secret, seed) != 0) {
        set_message(message, message_size, "device key generation failed");
        return ESP_FAIL;
    }
    sodium_memzero(secret, sizeof(secret));
    char *seed_text = b64url_alloc(seed, 32);
    char *pub_text = b64url_alloc(public_key, 32);
    if (seed_text == NULL || pub_text == NULL) {
        free(seed_text); free(pub_text);
        return ESP_ERR_NO_MEM;
    }
    char json[192];
    int written = snprintf(json, sizeof(json),
                           "{\"type\":\"deviceKey\",\"seed\":\"%s\",\"pubKey\":\"%s\"}",
                           seed_text, pub_text);
    free(seed_text); free(pub_text);
    if (written <= 0 || (size_t)written >= sizeof(json)) return ESP_FAIL;
    return fmo_cert_store_put(FMO_CERT_DEVICE_KEY, json, (size_t)written,
                              message, message_size);
}

static char *build_request_json(const activate_request_t *request)
{
    char mac[13], nonce[13], hash[65], pub[65], signature[129];
    hex_encode(request->mac, sizeof(request->mac), mac, true);
    hex_encode(request->nonce, sizeof(request->nonce), nonce, false);
    hex_encode(request->firmware_hash, sizeof(request->firmware_hash), hash, false);
    hex_encode(request->public_key, sizeof(request->public_key), pub, false);
    hex_encode(request->signature, sizeof(request->signature), signature, false);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "version", "4");
    cJSON_AddStringToObject(root, "action", "activate");
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "timestamp", (double)request->timestamp);
    cJSON_AddStringToObject(root, "firmwareVersion", FMO_FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "firmwareHash", hash);
    cJSON_AddStringToObject(root, "countryCode", "CN");
    cJSON_AddStringToObject(root, "devicePublicKey", pub);
    cJSON_AddStringToObject(root, "nonce", nonce);
    cJSON_AddStringToObject(root, "signature", signature);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static bool parse_response(const char *body, size_t size, activate_response_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(body, size);
    const cJSON *result = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *code = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsString(result) || !cJSON_IsNumber(code)) {
        cJSON_Delete(root); return false;
    }
    strlcpy(out->result, result->valuestring, sizeof(out->result));
    out->code = code->valueint;
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (cJSON_IsString(reason)) strlcpy(out->reason, reason->valuestring, sizeof(out->reason));
    bool valid = true;
    if (strcmp(out->result, "ok") == 0) {
        const cJSON *package = cJSON_GetObjectItemCaseSensitive(root, "certPackage");
        const cJSON *user = cJSON_GetObjectItemCaseSensitive(package, "userCert");
        const cJSON *intermediate = cJSON_GetObjectItemCaseSensitive(package, "intermediateCert");
        if (!cJSON_IsObject(user) || !cJSON_IsObject(intermediate) ||
            (out->user_cert = cJSON_PrintUnformatted(user)) == NULL ||
            (out->intermediate_cert = cJSON_PrintUnformatted(intermediate)) == NULL) {
            free(out->user_cert); free(out->intermediate_cert);
            out->user_cert = out->intermediate_cert = NULL;
            valid = false;
        }
    }
    cJSON_Delete(root);
    return valid;
}

static void free_response(activate_response_t *response)
{
    free(response->user_cert);
    free(response->intermediate_cert);
}

void fmo_activate_get_host(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    strlcpy(out, DEFAULT_HOST, out_size);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = out_size;
    if (nvs_get_str(nvs, NVS_HOST_KEY, out, &size) != ESP_OK || out[0] == '\0') {
        strlcpy(out, DEFAULT_HOST, out_size);
    }
    nvs_close(nvs);
}

bool fmo_activate_set_host(const char *host)
{
    if (host == NULL) return false;
    while (isspace((unsigned char)*host)) ++host;
    size_t length = strlen(host);
    while (length > 0 && isspace((unsigned char)host[length - 1])) --length;
    if (length == 0 || length > FMO_ACTIVATE_HOST_MAX) return false;
    char cleaned[FMO_ACTIVATE_HOST_MAX + 1];
    memcpy(cleaned, host, length); cleaned[length] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool ok = nvs_set_str(nvs, NVS_HOST_KEY, cleaned) == ESP_OK &&
              nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

void fmo_activate_get_status(char *last, size_t last_size, uint64_t *last_epoch)
{
    if (last != NULL && last_size > 0) strlcpy(last, s_last, last_size);
    if (last_epoch != NULL) *last_epoch = s_last_epoch;
}

esp_err_t fmo_activate_run(char *message, size_t message_size)
{
    if (message != NULL && message_size > 0) message[0] = '\0';
    if (sodium_init() < 0) return ESP_ERR_INVALID_STATE;
    time_t now = time(NULL);
    if (now < 1700000000) {
        set_message(message, message_size, "SNTP time is not synchronized");
        goto fail_state;
    }
    network_status_t network;
    network_manager_get_status(&network);
    if (!network.station_connected) {
        set_message(message, message_size, "Wi-Fi station is not connected");
        goto fail_state;
    }
    activate_request_t request = {.timestamp = (uint64_t)now};
    esp_read_mac(request.mac, ESP_MAC_WIFI_STA);
    esp_fill_random(request.nonce, sizeof(request.nonce));
    char elf_sha[65] = {};
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    bool have_hash = strlen(elf_sha) == 64;
    if (have_hash) {
        for (size_t i = 0; i < 32; ++i) {
            char pair[3] = {elf_sha[i * 2], elf_sha[i * 2 + 1], '\0'};
            unsigned value;
            if (sscanf(pair, "%2x", &value) != 1) { have_hash = false; break; }
            request.firmware_hash[i] = (uint8_t)value;
        }
    }
    if (!have_hash) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        have_hash = running != NULL &&
            esp_partition_get_sha256(running, request.firmware_hash) == ESP_OK;
    }
    if (!have_hash) {
        set_message(message, message_size, "firmware hash is unavailable");
        goto fail_state;
    }
    uint8_t seed[32], key_public[32];
    esp_err_t error = ensure_device_key(seed, key_public, message, message_size);
    if (error != ESP_OK) goto done;
    memcpy(request.public_key, key_public, sizeof(key_public));
    uint8_t payload[256], secret[crypto_sign_SECRETKEYBYTES], derived[32];
    size_t payload_size = 0;
    if (!build_signature_payload(&request, payload, sizeof(payload), &payload_size) ||
        crypto_sign_seed_keypair(derived, secret, seed) != 0 ||
        sodium_memcmp(derived, request.public_key, sizeof(derived)) != 0 ||
        crypto_sign_detached(request.signature, NULL, payload, payload_size, secret) != 0) {
        sodium_memzero(secret, sizeof(secret));
        set_message(message, message_size, "activation request signing failed");
        error = ESP_FAIL; goto done;
    }
    sodium_memzero(secret, sizeof(secret));
    char *json = build_request_json(&request);
    if (json == NULL) { error = ESP_ERR_NO_MEM; goto done; }
    char host[FMO_ACTIVATE_HOST_MAX + 1], url[192];
    fmo_activate_get_host(host, sizeof(host));
    snprintf(url, sizeof(url),
             strncmp(host, "http://", 7) == 0 || strncmp(host, "https://", 8) == 0
                 ? "%s" ACTIVATE_PATH : "https://%s" ACTIVATE_PATH, host);
    char *response = malloc(RESPONSE_CAPACITY);
    if (response == NULL) { free(json); error = ESP_ERR_NO_MEM; goto done; }
    esp_http_client_config_t config = {.url = url, .method = HTTP_METHOD_POST,
                                       .timeout_ms = 15000,
                                       .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    int total = 0, status = 0;
    if (client != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (esp_http_client_open(client, strlen(json)) == ESP_OK &&
            esp_http_client_write(client, json, strlen(json)) == (int)strlen(json) &&
            esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            while (total < (int)RESPONSE_CAPACITY - 1) {
                int read = esp_http_client_read(client, response + total,
                                                RESPONSE_CAPACITY - 1 - (size_t)total);
                if (read <= 0) break;
                total += read;
            }
            response[total] = '\0';
        }
        esp_http_client_cleanup(client);
    }
    free(json);
    if (total <= 0 || status != 200) {
        free(response);
        snprintf(message, message_size, "certificate request failed (HTTP %d)", status);
        error = ESP_FAIL; goto done;
    }
    activate_response_t parsed;
    if (!parse_response(response, (size_t)total, &parsed)) {
        free(response); set_message(message, message_size, "invalid activation response");
        error = ESP_FAIL; goto done;
    }
    free(response);
    if (strcmp(parsed.result, "ok") != 0) {
        snprintf(message, message_size, "activation %s (code %ld): %s",
                 parsed.result, parsed.code, parsed.reason);
        free_response(&parsed); error = ESP_FAIL; goto done;
    }
    error = fmo_cert_store_put(FMO_CERT_USER, parsed.user_cert,
                               strlen(parsed.user_cert), message, message_size);
    if (error == ESP_OK) error = fmo_cert_store_put(FMO_CERT_INTERMEDIATE,
        parsed.intermediate_cert, strlen(parsed.intermediate_cert), message, message_size);
    free_response(&parsed);
    if (error == ESP_OK) set_message(message, message_size, "certificate issued and installed");

done:
    if (message != NULL && message[0] != '\0') {
        strlcpy(s_last, message, sizeof(s_last));
        s_last_epoch = (uint64_t)time(NULL);
    }
    return error;

fail_state:
    if (message != NULL && message[0] != '\0') {
        strlcpy(s_last, message, sizeof(s_last));
        s_last_epoch = (uint64_t)time(NULL);
    }
    return ESP_ERR_INVALID_STATE;
}
