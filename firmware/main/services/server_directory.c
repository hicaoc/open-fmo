#include "server_directory.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "services/network_manager.h"
#include "services/storage_fs.h"

#define SERVER_MAX 32
#define FMO_SERVER_MAX 256
#define CACHE_VERSION 2
#define RESPONSE_CAPACITY 32768
#define NRL_CACHE_FILE "nrl_servers.json"
#define FMO_CACHE_FILE "fmo_servers.json"

static const char *TAG = "servers";
static void expire_stale_fmo_servers(void);
static StaticTask_t s_refresh_tcb;
static StackType_t s_refresh_stack[16384 / sizeof(StackType_t)];
static const char *SERVER_API = "https://www.nrlptt.com/api/platform-servers";

static const nrl_server_t k_fallback[] = {
    {"NRL \u4e3b\u670d\u52a1\u5668", "m.nrlptt.com", 60050, 0, 0, 0},
    {"\u6c5f\u82cf", "js.nrlptt.com", 60050, 0, 0, 10},
    {"BH1OSW", "bh1osw.nrlptt.com", 60050, 0, 0, 20},
    {"BA1GM", "ba1gm.nrlptt.com", 60050, 0, 0, 30},
    {"73HAM", "ham.73ham.com", 60050, 0, 0, 40},
    {"\u7762\u5b81", "nrl.bd4two.site", 60050, 0, 0, 50},
    {"\u626c\u5dde", "nrlptt.bd4vki.xyz", 60050, 0, 0, 60},
    {"\u5b89\u5fbd", "ah.nrlptt.com", 60050, 0, 0, 70},
};

typedef struct {
    uint32_t version;
    uint32_t count;
    nrl_server_t servers[SERVER_MAX];
} server_cache_t;

/* 11 KB of server lists; plain data so PSRAM is safe and keeps the
 * internal DRAM free for the esp_psram DMA pool reservation. */
static EXT_RAM_BSS_ATTR nrl_server_t s_lists[2][SERVER_MAX];
static EXT_RAM_BSS_ATTR size_t s_counts[2];
static unsigned s_active;
static uint32_t s_generation;
static SemaphoreHandle_t s_lock;
static EXT_RAM_BSS_ATTR fmo_server_t s_fmo_lists[2][FMO_SERVER_MAX];
static EXT_RAM_BSS_ATTR size_t s_fmo_counts[2];
static unsigned s_fmo_active;
static uint32_t s_fmo_generation;
static bool s_fmo_dirty;

static bool json_string(const char *begin, const char *end, const char *key,
                        char *out, size_t out_size)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *value = strstr(begin, needle);
    if (value == NULL || value >= end) return false;
    value = strchr(value + strlen(needle), ':');
    if (value == NULL || value >= end) return false;
    do { ++value; } while (value < end && isspace((unsigned char)*value));
    if (value >= end || *value != '"') return false;
    ++value;
    size_t used = 0;
    while (value < end && *value != '"') {
        char current = *value++;
        if (current == '\\' && value < end) {
            current = *value++;
            if (current == 'n' || current == 't') current = ' ';
        }
        if (used + 1 < out_size) out[used++] = current;
    }
    out[used] = '\0';
    return value < end && *value == '"';
}

static bool json_integer(const char *begin, const char *end, const char *key,
                         int *out)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *value = strstr(begin, needle);
    if (value == NULL || value >= end) return false;
    value = strchr(value + strlen(needle), ':');
    if (value == NULL || value >= end) return false;
    ++value;
    while (value < end && isspace((unsigned char)*value)) ++value;
    if (value < end && *value == '"') ++value;
    char *after = NULL;
    long parsed = strtol(value, &after, 10);
    if (after == value || after > end) return false;
    *out = (int)parsed;
    return true;
}

static bool json_boolean(const char *begin, const char *end, const char *key,
                         bool *out)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *value = strstr(begin, needle);
    if (value == NULL || value >= end) return false;
    value = strchr(value + strlen(needle), ':');
    if (value == NULL || value >= end) return false;
    ++value;
    while (value < end && isspace((unsigned char)*value)) ++value;
    if (end - value >= 4 && strncmp(value, "true", 4) == 0) *out = true;
    else if (end - value >= 5 && strncmp(value, "false", 5) == 0) *out = false;
    else return false;
    return true;
}

static const char *object_end(const char *begin)
{
    bool quoted = false;
    bool escaped = false;
    int depth = 0;
    for (const char *cursor = begin; *cursor != '\0'; ++cursor) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*cursor == '\\') escaped = true;
            else if (*cursor == '"') quoted = false;
            continue;
        }
        if (*cursor == '"') quoted = true;
        else if (*cursor == '{') ++depth;
        else if (*cursor == '}' && --depth == 0) return cursor;
    }
    return NULL;
}

static int compare_server(const void *left, const void *right)
{
    const nrl_server_t *a = left;
    const nrl_server_t *b = right;
    if (a->sort_order != b->sort_order) return a->sort_order - b->sort_order;
    return strcmp(a->name, b->name);
}

static size_t parse_server_list(const char *json, nrl_server_t *out)
{
    const char *data = strstr(json, "\"data\"");
    if (data == NULL || (data = strchr(data, '[')) == NULL) return 0;
    size_t count = 0;
    const char *cursor = data + 1;
    while (count < SERVER_MAX && (cursor = strchr(cursor, '{')) != NULL) {
        const char *end = object_end(cursor);
        if (end == NULL) break;
        nrl_server_t server = {0};
        int port = 0, online = 0, total = 0;
        int sort_order = (int)count;
        bool hidden = false;
        (void)json_boolean(cursor, end, "hidden", &hidden);
        (void)json_integer(cursor, end, "sort_order", &sort_order);
        (void)json_integer(cursor, end, "online", &online);
        (void)json_integer(cursor, end, "total", &total);
        if (!hidden && json_string(cursor, end, "name", server.name,
                                   sizeof(server.name)) &&
            json_string(cursor, end, "host", server.host, sizeof(server.host)) &&
            json_integer(cursor, end, "port", &port) && port > 0 && port <= 65535) {
            char *colon = strrchr(server.host, ':');
            if (colon != NULL && colon[1] != '\0') {
                bool digits = true;
                for (char *p = colon + 1; *p; ++p) {
                    digits &= isdigit((unsigned char)*p) != 0;
                }
                if (digits) *colon = '\0';
            }
            server.port = (uint16_t)port;
            server.online = online > 0 ? (uint32_t)online : 0;
            server.total = total > 0 ? (uint32_t)total : 0;
            server.sort_order = sort_order;
            out[count++] = server;
        }
        cursor = end + 1;
        if (*cursor == ']') break;
    }
    qsort(out, count, sizeof(out[0]), compare_server);
    return count;
}

static void publish_servers(const nrl_server_t *servers, size_t count)
{
    if (count == 0 || count > SERVER_MAX) return;
    unsigned next = s_active ^ 1U;
    memcpy(s_lists[next], servers, count * sizeof(servers[0]));
    s_counts[next] = count;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active = next;
    ++s_generation;
    xSemaphoreGive(s_lock);
}

static void save_nrl_cache(const nrl_server_t *servers, size_t count)
{
    if (!storage_fs_is_ready() || servers == NULL || count == 0) return;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "servers");
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(root, "version", CACHE_VERSION);
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return;
        }
        cJSON_AddStringToObject(item, "name", servers[i].name);
        cJSON_AddStringToObject(item, "host", servers[i].host);
        cJSON_AddNumberToObject(item, "port", servers[i].port);
        cJSON_AddNumberToObject(item, "online", servers[i].online);
        cJSON_AddNumberToObject(item, "total", servers[i].total);
        cJSON_AddNumberToObject(item, "sort", servers[i].sort_order);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != NULL) {
        esp_err_t error = storage_fs_write_atomic(NRL_CACHE_FILE, json,
                                                   strlen(json));
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "NRL FS cache save failed: %s",
                     esp_err_to_name(error));
        }
        cJSON_free(json);
    }
}

static size_t load_nrl_cache(nrl_server_t *servers)
{
    if (!storage_fs_is_ready() || servers == NULL) return 0;
    uint8_t *data = NULL;
    size_t size = 0;
    if (storage_fs_read(NRL_CACHE_FILE, &data, &size, 64U * 1024U) != ESP_OK) {
        return 0;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, size);
    free(data);
    if (root == NULL) return 0;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "servers");
    size_t count = 0;
    if (cJSON_IsNumber(version) && version->valueint == CACHE_VERSION &&
        cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            if (count >= SERVER_MAX) break;
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *host = cJSON_GetObjectItemCaseSensitive(item, "host");
            cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
            if (!cJSON_IsString(name) || !cJSON_IsString(host) ||
                !cJSON_IsNumber(port) || port->valueint <= 0 ||
                port->valueint > 65535) continue;
            nrl_server_t *server = &servers[count++];
            strlcpy(server->name, name->valuestring, sizeof(server->name));
            strlcpy(server->host, host->valuestring, sizeof(server->host));
            server->port = (uint16_t)port->valueint;
            cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "online");
            server->online = cJSON_IsNumber(value) && value->valuedouble > 0
                ? (uint32_t)value->valuedouble : 0;
            value = cJSON_GetObjectItemCaseSensitive(item, "total");
            server->total = cJSON_IsNumber(value) && value->valuedouble > 0
                ? (uint32_t)value->valuedouble : 0;
            value = cJSON_GetObjectItemCaseSensitive(item, "sort");
            server->sort_order = cJSON_IsNumber(value) ? value->valueint
                                                        : (int)count;
        }
    }
    cJSON_Delete(root);
    if (count > 0) qsort(servers, count, sizeof(servers[0]), compare_server);
    return count;
}

static void fingerprint_hex(const uint8_t fingerprint[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = digits[fingerprint[i] >> 4];
        out[i * 2 + 1] = digits[fingerprint[i] & 0x0f];
    }
    out[64] = '\0';
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool fingerprint_parse(const char *text, uint8_t out[32])
{
    if (text == NULL || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_nibble(text[i * 2]);
        int low = hex_nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static size_t load_fmo_cache(fmo_server_t *servers)
{
    if (!storage_fs_is_ready() || servers == NULL) return 0;
    uint8_t *data = NULL;
    size_t size = 0;
    if (storage_fs_read(FMO_CACHE_FILE, &data, &size, 128U * 1024U) != ESP_OK) {
        return 0;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)data, size);
    free(data);
    if (root == NULL) return 0;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "servers");
    size_t count = 0;
    if (cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            if (count >= FMO_SERVER_MAX) break;
            cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
            cJSON *host = cJSON_GetObjectItemCaseSensitive(item, "host");
            cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
            if (!cJSON_IsString(key) || !key->valuestring[0] ||
                !cJSON_IsString(host) || !host->valuestring[0] ||
                !cJSON_IsNumber(port) || port->valueint <= 0 ||
                port->valueint > 65535) continue;
            fmo_server_t *server = &servers[count++];
            strlcpy(server->key, key->valuestring, sizeof(server->key));
            strlcpy(server->host, host->valuestring, sizeof(server->host));
            server->port = (uint16_t)port->valueint;
            cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(value)) {
                strlcpy(server->name, value->valuestring, sizeof(server->name));
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "callsign");
            if (cJSON_IsString(value)) {
                strlcpy(server->callsign, value->valuestring,
                        sizeof(server->callsign));
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "uid");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) {
                server->uid = (uint32_t)value->valuedouble;
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "online");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) {
                server->online = (uint32_t)value->valuedouble;
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "total");
            if (cJSON_IsNumber(value) && value->valuedouble > 0) {
                server->total = (uint32_t)value->valuedouble;
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "ssid");
            if (cJSON_IsNumber(value) && value->valueint >= 0 &&
                value->valueint <= 15) {
                server->ssid = (uint8_t)value->valueint;
                server->has_ssid = true;
            }
            value = cJSON_GetObjectItemCaseSensitive(item, "last_seen");
            if (cJSON_IsNumber(value)) server->last_seen = (int64_t)value->valuedouble;
            value = cJSON_GetObjectItemCaseSensitive(item, "favorite");
            server->favorite = cJSON_IsTrue(value);
            value = cJSON_GetObjectItemCaseSensitive(item, "fingerprint");
            if (cJSON_IsString(value)) {
                server->has_fingerprint = fingerprint_parse(
                    value->valuestring, server->fingerprint);
            }
        }
    }
    cJSON_Delete(root);
    return count;
}

static esp_err_t save_fmo_cache(const fmo_server_t *servers, size_t count)
{
    if (!storage_fs_is_ready()) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "servers");
    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "version", 1);
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = &servers[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(items, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "key", server->key);
        cJSON_AddStringToObject(item, "name", server->name);
        cJSON_AddStringToObject(item, "host", server->host);
        cJSON_AddStringToObject(item, "callsign", server->callsign);
        cJSON_AddNumberToObject(item, "port", server->port);
        cJSON_AddNumberToObject(item, "uid", server->uid);
        cJSON_AddNumberToObject(item, "online", server->online);
        cJSON_AddNumberToObject(item, "total", server->total);
        if (server->has_ssid) {
            cJSON_AddNumberToObject(item, "ssid", server->ssid);
        }
        cJSON_AddNumberToObject(item, "last_seen", (double)server->last_seen);
        cJSON_AddBoolToObject(item, "favorite", server->favorite);
        if (server->has_fingerprint) {
            char hex[65];
            fingerprint_hex(server->fingerprint, hex);
            cJSON_AddStringToObject(item, "fingerprint", hex);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    esp_err_t error = storage_fs_write_atomic(FMO_CACHE_FILE, json,
                                               strlen(json));
    cJSON_free(json);
    return error;
}

static bool download_servers(void)
{
    char *response = heap_caps_malloc(RESPONSE_CAPACITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        response = heap_caps_malloc(RESPONSE_CAPACITY, MALLOC_CAP_8BIT);
    }
    if (response == NULL) return false;
    esp_http_client_config_t config = {
        .url = SERVER_API,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool ok = false;
    if (client != NULL && esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0 &&
        esp_http_client_get_status_code(client) == 200) {
        int received = 0;
        while (received < RESPONSE_CAPACITY - 1) {
            int read = esp_http_client_read(client, response + received,
                                            RESPONSE_CAPACITY - 1 - received);
            if (read <= 0) break;
            received += read;
        }
        response[received] = '\0';
        nrl_server_t *parsed = calloc(SERVER_MAX, sizeof(*parsed));
        size_t count = parsed != NULL ? parse_server_list(response, parsed) : 0;
        if (count > 0) {
            publish_servers(parsed, count);
            save_nrl_cache(parsed, count);
            ESP_LOGI(TAG, "downloaded %u servers with occupancy", (unsigned)count);
            ok = true;
        }
        free(parsed);
    }
    if (client != NULL) esp_http_client_cleanup(client);
    heap_caps_free(response);
    return ok;
}

static void refresh_task(void *argument)
{
    (void)argument;
    while (true) {
        expire_stale_fmo_servers();
        (void)fmo_server_directory_flush();
        network_status_t network = {0};
        network_manager_get_status(&network);
        if (!network.station_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (download_servers()) {
            vTaskDelay(pdMS_TO_TICKS(60U * 1000U));
        } else {
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
}

esp_err_t server_directory_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    memcpy(s_lists[0], k_fallback, sizeof(k_fallback));
    s_counts[0] = sizeof(k_fallback) / sizeof(k_fallback[0]);
    s_active = 0;
    s_generation = 1;

    size_t fs_count = load_nrl_cache(s_lists[0]);
    if (fs_count > 0) {
        s_counts[0] = fs_count;
        ESP_LOGI(TAG, "loaded %u NRL servers from FS", (unsigned)fs_count);
    }

    server_cache_t *cache = calloc(1, sizeof(*cache));
    nvs_handle_t nvs;
    if (fs_count == 0 && cache != NULL &&
        nvs_open("nrl_servers", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(*cache);
        if (nvs_get_blob(nvs, "directory", cache, &size) == ESP_OK &&
            size == sizeof(*cache) && cache->version == CACHE_VERSION &&
            cache->count > 0 && cache->count <= SERVER_MAX) {
            memcpy(s_lists[0], cache->servers,
                   cache->count * sizeof(cache->servers[0]));
            s_counts[0] = cache->count;
            ESP_LOGI(TAG, "loaded %u cached servers", (unsigned)cache->count);
            save_nrl_cache(s_lists[0], s_counts[0]);
        }
        nvs_close(nvs);
    }
    free(cache);
    s_fmo_counts[0] = load_fmo_cache(s_fmo_lists[0]);
    s_fmo_active = 0;
    s_fmo_generation = 1;
    s_fmo_dirty = false;
    if (s_fmo_counts[0] > 0) {
        ESP_LOGI(TAG, "loaded %u FMO servers from FS",
                 (unsigned)s_fmo_counts[0]);
    }
    return xTaskCreateStatic(refresh_task, "server_refresh",
                             sizeof(s_refresh_stack) / sizeof(s_refresh_stack[0]),
                             NULL, 3, s_refresh_stack, &s_refresh_tcb) != NULL
               ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t server_directory_count(void)
{
    if (s_lock == NULL) return sizeof(k_fallback) / sizeof(k_fallback[0]);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_counts[s_active];
    xSemaphoreGive(s_lock);
    return count;
}

const nrl_server_t *server_directory_get(size_t index)
{
    if (s_lock == NULL) {
        return index < sizeof(k_fallback) / sizeof(k_fallback[0])
                   ? &k_fallback[index] : NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned active = s_active;
    size_t count = s_counts[active];
    xSemaphoreGive(s_lock);
    return index < count ? &s_lists[active][index] : NULL;
}

size_t server_directory_find(const char *host, uint16_t port)
{
    size_t count = server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const nrl_server_t *server = server_directory_get(i);
        if (server != NULL && strcmp(server->host, host) == 0 &&
            server->port == port) return i;
    }
    return SIZE_MAX;
}

uint32_t server_directory_generation(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

size_t fmo_server_directory_count(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_fmo_counts[s_fmo_active];
    xSemaphoreGive(s_lock);
    return count;
}

const fmo_server_t *fmo_server_directory_get(size_t index)
{
    if (s_lock == NULL) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned active = s_fmo_active;
    size_t count = s_fmo_counts[active];
    xSemaphoreGive(s_lock);
    return index < count ? &s_fmo_lists[active][index] : NULL;
}

size_t fmo_server_directory_find(const char *key)
{
    if (key == NULL || key[0] == '\0') return SIZE_MAX;
    size_t count = fmo_server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = fmo_server_directory_get(i);
        if (server != NULL && strcmp(server->key, key) == 0) return i;
    }
    return SIZE_MAX;
}

size_t fmo_server_directory_favorite_count(void)
{
    size_t favorites = 0;
    size_t count = fmo_server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = fmo_server_directory_get(i);
        if (server != NULL && server->favorite) ++favorites;
    }
    return favorites;
}

const fmo_server_t *fmo_server_directory_get_favorite(size_t favorite_index)
{
    size_t count = fmo_server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = fmo_server_directory_get(i);
        if (server != NULL && server->favorite) {
            if (favorite_index == 0) return server;
            --favorite_index;
        }
    }
    return NULL;
}

/* Non-favorite servers with no STATION beacon for this long are expired by
 * the periodic sweep (favorites are kept forever). */
#define FMO_SERVER_STALE_SEC (7U * 24U * 3600U)

/* Drop non-favorite servers unheard from for FMO_SERVER_STALE_SEC.  Skipped
 * while the clock is not yet synced (early boot before NTP): with last_seen
 * persisted across reboots, a long power-off would otherwise wipe the whole
 * list before the feed can refresh it.  Surviving entries are copied to the
 * inactive list like upsert does, so readers holding pointers into the old
 * active list keep seeing valid memory. */
static void expire_stale_fmo_servers(void)
{
    const time_t now = time(NULL);
    if (now < (time_t)1700000000 || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned active = s_fmo_active;
    unsigned next = active ^ 1U;
    size_t count = s_fmo_counts[active];
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = &s_fmo_lists[active][i];
        if (!server->favorite &&
            now - (time_t)server->last_seen > (time_t)FMO_SERVER_STALE_SEC) {
            continue;
        }
        s_fmo_lists[next][kept++] = *server;
    }
    if (kept != count) {
        ESP_LOGI(TAG, "expired %u stale FMO servers (last beacon > %u days)",
                 (unsigned)(count - kept),
                 (unsigned)(FMO_SERVER_STALE_SEC / 86400U));
        s_fmo_counts[next] = kept;
        s_fmo_active = next;
        ++s_fmo_generation;
        s_fmo_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t fmo_server_directory_upsert(const fmo_server_t *server)
{
    if (server == NULL || server->key[0] == '\0' || server->host[0] == '\0' ||
        server->port == 0 || s_lock == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned active = s_fmo_active;
    unsigned next = active ^ 1U;
    size_t count = s_fmo_counts[active];
    memcpy(s_fmo_lists[next], s_fmo_lists[active],
           count * sizeof(s_fmo_lists[next][0]));
    size_t index = count;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_fmo_lists[next][i].key, server->key) == 0) {
            index = i;
            break;
        }
    }
    if (index == count) {
        if (count >= FMO_SERVER_MAX) {
            /* Full: evict the stalest non-favorite entry (by last_seen, the
             * time of its last STATION beacon) so new discoveries replace
             * servers that stopped broadcasting.  Favorites and recently
             * seen servers (which includes the selected one while it is
             * online) are never evicted. */
            size_t oldest = SIZE_MAX;
            for (size_t i = 0; i < count; ++i) {
                if (s_fmo_lists[next][i].favorite) continue;
                if (oldest == SIZE_MAX ||
                    s_fmo_lists[next][i].last_seen <
                        s_fmo_lists[next][oldest].last_seen) {
                    oldest = i;
                }
            }
            if (oldest == SIZE_MAX) {
                xSemaphoreGive(s_lock);
                return ESP_ERR_NO_MEM;
            }
            index = oldest;
        } else {
            ++count;
        }
    }
    bool favorite = index < s_fmo_counts[active]
        ? s_fmo_lists[next][index].favorite : false;
    s_fmo_lists[next][index] = *server;
    s_fmo_lists[next][index].favorite = favorite || server->favorite;
    s_fmo_counts[next] = count;
    s_fmo_active = next;
    ++s_fmo_generation;
    s_fmo_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fmo_server_directory_set_favorite(const char *key, bool favorite)
{
    if (key == NULL || key[0] == '\0' || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned active = s_fmo_active;
    unsigned next = active ^ 1U;
    size_t count = s_fmo_counts[active];
    size_t index = SIZE_MAX;
    memcpy(s_fmo_lists[next], s_fmo_lists[active],
           count * sizeof(s_fmo_lists[next][0]));
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_fmo_lists[next][i].key, key) == 0) {
            index = i;
            break;
        }
    }
    if (index == SIZE_MAX) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_fmo_lists[next][index].favorite = favorite;
    s_fmo_counts[next] = count;
    s_fmo_active = next;
    ++s_fmo_generation;
    s_fmo_dirty = true;
    xSemaphoreGive(s_lock);
    return fmo_server_directory_flush();
}

uint32_t fmo_server_directory_generation(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_fmo_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

esp_err_t fmo_server_directory_flush(void)
{
    if (s_lock == NULL || !storage_fs_is_ready()) return ESP_ERR_INVALID_STATE;
    fmo_server_t *snapshot = calloc(FMO_SERVER_MAX, sizeof(*snapshot));
    if (snapshot == NULL) return ESP_ERR_NO_MEM;
    uint32_t snapshot_generation;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_fmo_dirty) {
        xSemaphoreGive(s_lock);
        free(snapshot);
        return ESP_OK;
    }
    size_t count = s_fmo_counts[s_fmo_active];
    snapshot_generation = s_fmo_generation;
    memcpy(snapshot, s_fmo_lists[s_fmo_active], count * sizeof(snapshot[0]));
    xSemaphoreGive(s_lock);
    esp_err_t error = save_fmo_cache(snapshot, count);
    free(snapshot);
    if (error == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_fmo_generation == snapshot_generation) s_fmo_dirty = false;
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGW(TAG, "FMO FS cache save failed: %s", esp_err_to_name(error));
    }
    return error;
}
