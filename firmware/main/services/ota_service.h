#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FMO_OTA_URL_MAX 192
#define FMO_OTA_VERSION_MAX 64
#define FMO_OTA_RELEASE_MAX 8

typedef struct {
    char version[FMO_OTA_VERSION_MAX];
    char url[FMO_OTA_URL_MAX];
    char notes[160];
} fmo_ota_release_t;

typedef struct {
    char server_url[FMO_OTA_URL_MAX];
    char last_error[128];
    char latest_version[FMO_OTA_VERSION_MAX];
    bool configured;
    bool checking;
    bool updating;
    uint32_t update_bytes;
    uint32_t update_size;
    uint8_t update_percent;
    uint32_t last_check_ms;
    size_t release_count;
    fmo_ota_release_t releases[FMO_OTA_RELEASE_MAX];
} fmo_ota_status_t;

/* Compact snapshot without the release list (~0.5 KB instead of ~3 KB).
 * Use this in stack-constrained contexts (main/UI/AT tasks); the web
 * portal keeps using fmo_ota_status_t because it renders the list. */
typedef struct {
    char server_url[FMO_OTA_URL_MAX];
    char last_error[128];
    char latest_version[FMO_OTA_VERSION_MAX];
    bool configured;
    bool checking;
    bool updating;
    uint32_t update_bytes;
    uint32_t update_size;
    uint8_t update_percent;
    uint32_t last_check_ms;
} fmo_ota_ui_status_t;

esp_err_t ota_service_init(void);
void ota_service_get_status(fmo_ota_status_t *out);
void ota_service_get_ui_status(fmo_ota_ui_status_t *out);
bool ota_service_set_config(const char *server_url, const char *device_token);
bool ota_service_check_now(void);
bool ota_service_update_version(const char *version);

/* The Web upload handler uses these to share progress with the screen. */
bool ota_service_local_begin(uint32_t image_size);
void ota_service_local_progress(uint32_t bytes, uint32_t image_size);
void ota_service_local_end(bool success, const char *error);

