/*
 * OTA protocol and manifest format are adapted from nrl-esp32's OTA service
 * (MIT). Board-specific safety and UI integration are native to Open FMO.
 */
#include "ota_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "services/config_store.h"
#include "services/network_manager.h"
#include "services/radio_at.h"
#include "version.h"

static const char *TAG = "ota";
static StaticTask_t s_ota_tcb;
static StackType_t s_ota_stack[16384 / sizeof(StackType_t)];
static const char *NVS_NAMESPACE = "nrl_ota";
static const uint32_t CHECK_PERIOD_MS = 60U * 60U * 1000U;
static const uint32_t BOOT_CHECK_DELAY_MS = 30U * 1000U;

typedef struct {
    fmo_ota_status_t status;
    char token[96];
    char requested_version[FMO_OTA_VERSION_MAX];
    bool check_requested;
    bool update_requested;
    SemaphoreHandle_t lock;
} ota_state_t;

/* ~4 KB state block: plain data guarded by a mutex, PSRAM is safe and
 * frees internal DRAM for the esp_psram DMA pool reservation. */
static EXT_RAM_BSS_ATTR ota_state_t s_ota;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "%s", value != NULL ? value : "");
}

static bool is_http_url(const char *url)
{
    return url != NULL &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static void set_error(const char *error)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copy_text(s_ota.status.last_error, sizeof(s_ota.status.last_error), error);
    xSemaphoreGive(s_ota.lock);
}

static void set_progress(uint32_t bytes, uint32_t size)
{
    uint32_t percent = size > 0 ? (uint32_t)(((uint64_t)bytes * 100U) / size) : 0;
    if (percent > 100) percent = 100;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.update_bytes = bytes;
    s_ota.status.update_size = size;
    s_ota.status.update_percent = (uint8_t)percent;
    xSemaphoreGive(s_ota.lock);
}

static void prepare_safe_state(void)
{
    gpio_set_level(FMO_RADIO_PTT, 0);
    (void)radio_at_set_rf_enabled(false);
    ESP_LOGI(TAG, "RF/PTT disabled for firmware update");
}

static bool json_string_at(const char *begin, const char *end, const char *key,
                           char *out, size_t out_size)
{
    if (begin == NULL || end == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *value = strstr(begin, needle);
    if (value == NULL || value >= end) return false;
    value += strlen(needle);
    size_t pos = 0;
    while (value < end && *value != '\0' && *value != '"' && pos + 1 < out_size) {
        if (*value == '\\' && value + 1 < end) ++value;
        out[pos++] = *value++;
    }
    out[pos] = '\0';
    return value < end && *value == '"';
}

static bool parse_manifest(const char *json)
{
    fmo_ota_release_t parsed[FMO_OTA_RELEASE_MAX] = {0};
    size_t count = 0;
    const char *cursor = json;
    while (count < FMO_OTA_RELEASE_MAX) {
        const char *entry = strstr(cursor, "{\"version\":");
        if (entry == NULL) break;
        const char *end = strchr(entry, '}');
        if (end == NULL) break;
        if (json_string_at(entry, end, "version", parsed[count].version,
                           sizeof(parsed[count].version)) &&
            json_string_at(entry, end, "url", parsed[count].url,
                           sizeof(parsed[count].url))) {
            (void)json_string_at(entry, end, "notes", parsed[count].notes,
                                 sizeof(parsed[count].notes));
            ++count;
        }
        cursor = end + 1;
    }
    char latest[FMO_OTA_VERSION_MAX] = {0};
    (void)json_string_at(json, json + strlen(json), "latest_version", latest,
                         sizeof(latest));
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.release_count = count;
    memcpy(s_ota.status.releases, parsed, sizeof(parsed));
    copy_text(s_ota.status.latest_version, sizeof(s_ota.status.latest_version), latest);
    xSemaphoreGive(s_ota.lock);
    return (count == 0) == (latest[0] == '\0');
}

static void api_url(char *out, size_t out_size, const char *suffix)
{
    char base[FMO_OTA_URL_MAX];
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copy_text(base, sizeof(base), s_ota.status.server_url);
    xSemaphoreGive(s_ota.lock);
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') base[--length] = '\0';
    snprintf(out, out_size, "%s%s", base, suffix != NULL ? suffix : "");
}

static bool check_for_releases(void)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.checking = true;
    s_ota.status.last_error[0] = '\0';
    xSemaphoreGive(s_ota.lock);

    network_status_t network = {0};
    network_manager_get_status(&network);
    if (!network.station_connected) {
        set_error("Wi-Fi station is not connected");
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.status.checking = false;
        s_ota.status.last_check_ms = now_ms();
        xSemaphoreGive(s_ota.lock);
        return false;
    }

    char token[sizeof(s_ota.token)];
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copy_text(token, sizeof(token), s_ota.token);
    xSemaphoreGive(s_ota.lock);

    uint8_t mac[6] = {0};
    fmo_config_t device = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)config_store_load(&device);
    char body[512];
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%02X%02X%02X%02X%02X%02X\","
             "\"board_type\":\"%s\",\"firmware_version\":\"%s\","
             "\"metadata\":{\"nrl_callsign\":\"%s\",\"nrl_ssid\":%u,"
             "\"firmware_name\":\"%s\"}}",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], FMO_BOARD_TYPE,
             FMO_FIRMWARE_VERSION, device.callsign, (unsigned)device.callsign_ssid,
             FMO_FIRMWARE_NAME);

    char endpoint[FMO_OTA_URL_MAX + 32];
    api_url(endpoint, sizeof(endpoint), "/api/v1/devices/check");
    const size_t response_capacity = 8192;
    char *response = heap_caps_malloc(response_capacity,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) response = heap_caps_malloc(response_capacity, MALLOC_CAP_8BIT);
    bool ok = false;
    if (response == NULL) {
        set_error("cannot allocate OTA manifest buffer");
        goto done;
    }

    esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        set_error("cannot create OTA HTTP client");
        heap_caps_free(response);
        goto done;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token[0] != '\0') esp_http_client_set_header(client, "X-Device-Token", token);
    int status = 0;
    if (esp_http_client_open(client, strlen(body)) == ESP_OK &&
        esp_http_client_write(client, body, strlen(body)) == (int)strlen(body) &&
        esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int total = 0;
            while (total < (int)response_capacity - 1) {
                int count = esp_http_client_read(client, response + total,
                                                 response_capacity - 1 - total);
                if (count <= 0) break;
                total += count;
            }
            response[total] = '\0';
            ok = total > 0 && parse_manifest(response);
            if (!ok) set_error("invalid OTA manifest");
        }
    }
    if (!ok && status != 200) {
        char error[80];
        snprintf(error, sizeof(error), "OTA check HTTP status %d", status);
        set_error(error);
    }
    esp_http_client_cleanup(client);
    heap_caps_free(response);

done:
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.checking = false;
    s_ota.status.last_check_ms = now_ms();
    if (ok) s_ota.status.last_error[0] = '\0';
    xSemaphoreGive(s_ota.lock);
    return ok;
}

static bool absolute_release_url(const char *release_url, char *out, size_t out_size)
{
    if (is_http_url(release_url)) {
        copy_text(out, out_size, release_url);
        return true;
    }
    if (release_url == NULL || release_url[0] == '\0') return false;
    char base[FMO_OTA_URL_MAX];
    api_url(base, sizeof(base), "");
    snprintf(out, out_size, "%s%s%s", base, release_url[0] == '/' ? "" : "/",
             release_url);
    return is_http_url(out);
}

static bool install_version(const char *version)
{
    fmo_ota_release_t release = {0};
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    for (size_t i = 0; i < s_ota.status.release_count; ++i) {
        if (strcmp(s_ota.status.releases[i].version, version) == 0) {
            release = s_ota.status.releases[i];
            break;
        }
    }
    s_ota.status.updating = true;
    s_ota.status.update_bytes = 0;
    s_ota.status.update_size = 0;
    s_ota.status.update_percent = 0;
    xSemaphoreGive(s_ota.lock);

    char url[FMO_OTA_URL_MAX * 2];
    if (release.url[0] == '\0' ||
        !absolute_release_url(release.url, url, sizeof(url))) {
        set_error("release URL is invalid or version is missing");
        return false;
    }
    prepare_safe_state();
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
        .bulk_flash_erase = false,
    };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t error = esp_https_ota_begin(&ota, &handle);
    uint32_t image_size = 0;
    if (error == ESP_OK) {
        int reported_size = esp_https_ota_get_image_size(handle);
        if (reported_size > 0) image_size = (uint32_t)reported_size;
        do {
            error = esp_https_ota_perform(handle);
            int read = esp_https_ota_get_image_len_read(handle);
            reported_size = esp_https_ota_get_image_size(handle);
            if (reported_size > 0) image_size = (uint32_t)reported_size;
            if (read >= 0) set_progress((uint32_t)read, image_size);
        } while (error == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
        if (error == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
            error = ESP_FAIL;
        }
        if (error == ESP_OK) error = esp_https_ota_finish(handle);
        else esp_https_ota_abort(handle);
    }
    if (error != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "firmware update failed: %s",
                 esp_err_to_name(error));
        set_error(message);
        return false;
    }
    if (image_size > 0) set_progress(image_size, image_size);
    ESP_LOGI(TAG, "OTA image %s installed; rebooting", version);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return true;
}

static void ota_task(void *argument)
{
    (void)argument;
    const uint32_t boot_ms = now_ms();
    while (true) {
        bool do_check;
        bool do_update;
        char version[FMO_OTA_VERSION_MAX];
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        bool due = s_ota.status.configured &&
                   ((s_ota.status.last_check_ms == 0 &&
                     now_ms() - boot_ms >= BOOT_CHECK_DELAY_MS) ||
                    (s_ota.status.last_check_ms != 0 &&
                     now_ms() - s_ota.status.last_check_ms >= CHECK_PERIOD_MS));
        do_check = s_ota.check_requested || due;
        do_update = s_ota.update_requested;
        copy_text(version, sizeof(version), s_ota.requested_version);
        s_ota.check_requested = false;
        s_ota.update_requested = false;
        xSemaphoreGive(s_ota.lock);
        if (do_check) (void)check_for_releases();
        if (do_update) (void)install_version(version);
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.status.updating = false;
        xSemaphoreGive(s_ota.lock);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t ota_service_init(void)
{
    if (s_ota.lock != NULL) return ESP_OK;
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.lock = xSemaphoreCreateMutex();
    if (s_ota.lock == NULL) return ESP_ERR_NO_MEM;

    bool saved_url = false;
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        size_t size = sizeof(s_ota.status.server_url);
        saved_url = nvs_get_str(nvs, "url", s_ota.status.server_url, &size) == ESP_OK;
        size = sizeof(s_ota.token);
        (void)nvs_get_str(nvs, "token", s_ota.token, &size);
        nvs_close(nvs);
    }
    if (!saved_url) copy_text(s_ota.status.server_url,
                              sizeof(s_ota.status.server_url),
                              FMO_OTA_DEFAULT_SERVER);
    s_ota.status.configured = s_ota.status.server_url[0] != '\0';
    ESP_LOGI(TAG, "OTA server=%s board=%s version=%s", s_ota.status.server_url,
             FMO_BOARD_TYPE, FMO_FIRMWARE_VERSION);
    return xTaskCreateStatic(ota_task, "nrl_ota",
                             sizeof(s_ota_stack) / sizeof(s_ota_stack[0]),
                             NULL, 3, s_ota_stack, &s_ota_tcb) != NULL
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void ota_service_get_status(fmo_ota_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_ota.lock == NULL) return;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *out = s_ota.status;
    xSemaphoreGive(s_ota.lock);
}

void ota_service_get_ui_status(fmo_ota_ui_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_ota.lock == NULL) return;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    const fmo_ota_status_t *status = &s_ota.status;
    memcpy(out->server_url, status->server_url, sizeof(out->server_url));
    memcpy(out->last_error, status->last_error, sizeof(out->last_error));
    memcpy(out->latest_version, status->latest_version,
           sizeof(out->latest_version));
    out->configured = status->configured;
    out->checking = status->checking;
    out->updating = status->updating;
    out->update_bytes = status->update_bytes;
    out->update_size = status->update_size;
    out->update_percent = status->update_percent;
    out->last_check_ms = status->last_check_ms;
    xSemaphoreGive(s_ota.lock);
}

bool ota_service_set_config(const char *server_url, const char *device_token)
{
    if (s_ota.lock == NULL || server_url == NULL || device_token == NULL ||
        strlen(server_url) >= FMO_OTA_URL_MAX || strlen(device_token) >= sizeof(s_ota.token) ||
        (server_url[0] != '\0' && !is_http_url(server_url))) return false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t error = nvs_set_str(nvs, "url", server_url);
    if (error == ESP_OK) error = nvs_set_str(nvs, "token", device_token);
    if (error == ESP_OK) error = nvs_commit(nvs);
    nvs_close(nvs);
    if (error != ESP_OK) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    copy_text(s_ota.status.server_url, sizeof(s_ota.status.server_url), server_url);
    copy_text(s_ota.token, sizeof(s_ota.token), device_token);
    s_ota.status.configured = server_url[0] != '\0';
    s_ota.status.last_error[0] = '\0';
    s_ota.status.latest_version[0] = '\0';
    s_ota.status.release_count = 0;
    xSemaphoreGive(s_ota.lock);
    return true;
}

bool ota_service_check_now(void)
{
    if (s_ota.lock == NULL) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    bool accepted = s_ota.status.configured && !s_ota.status.checking &&
                    !s_ota.status.updating;
    if (accepted) {
        s_ota.status.last_error[0] = '\0';
        s_ota.check_requested = true;
    }
    xSemaphoreGive(s_ota.lock);
    return accepted;
}

bool ota_service_update_version(const char *version)
{
    if (s_ota.lock == NULL || version == NULL || version[0] == '\0' ||
        strlen(version) >= FMO_OTA_VERSION_MAX) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    bool accepted = s_ota.status.configured && !s_ota.status.checking &&
                    !s_ota.status.updating;
    if (accepted) {
        s_ota.status.last_error[0] = '\0';
        copy_text(s_ota.requested_version, sizeof(s_ota.requested_version), version);
        s_ota.update_requested = true;
    }
    xSemaphoreGive(s_ota.lock);
    return accepted;
}

bool ota_service_local_begin(uint32_t image_size)
{
    if (s_ota.lock == NULL) return false;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    bool accepted = !s_ota.status.checking && !s_ota.status.updating;
    if (accepted) {
        s_ota.status.updating = true;
        s_ota.status.last_error[0] = '\0';
        s_ota.status.update_bytes = 0;
        s_ota.status.update_size = image_size;
        s_ota.status.update_percent = 0;
    }
    xSemaphoreGive(s_ota.lock);
    if (accepted) prepare_safe_state();
    return accepted;
}

void ota_service_local_progress(uint32_t bytes, uint32_t image_size)
{
    if (s_ota.lock != NULL) set_progress(bytes, image_size);
}

void ota_service_local_end(bool success, const char *error)
{
    if (s_ota.lock == NULL) return;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (!success) {
        copy_text(s_ota.status.last_error, sizeof(s_ota.status.last_error), error);
        s_ota.status.updating = false;
    } else {
        s_ota.status.update_percent = 100;
    }
    xSemaphoreGive(s_ota.lock);
}
