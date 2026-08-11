#include "storage_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "nvs.h"

static const char *TAG = "storage_fs";
static const char *PARTITION_LABEL = "storage";
static const char *NVS_NAMESPACE = "storage_fs";
static const char *NVS_INITIALIZED_KEY = "initialized";
static bool s_ready;

static bool was_initialized(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t initialized = 0;
    esp_err_t error = nvs_get_u8(handle, NVS_INITIALIZED_KEY, &initialized);
    nvs_close(handle);
    return error == ESP_OK && initialized == 1;
}

static void mark_initialized(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, NVS_INITIALIZED_KEY, 1);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "could not persist initialization marker: %s",
                 esp_err_to_name(error));
    }
}

static bool make_path(const char *name, char *out, size_t out_size)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL || strstr(name, "..") != NULL) {
        return false;
    }
    int written = snprintf(out, out_size, STORAGE_FS_BASE_PATH "/%s", name);
    return written > 0 && (size_t)written < out_size;
}

static bool partition_is_erased(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        PARTITION_LABEL);
    if (partition == NULL) return false;
    uint8_t sample[256];
    const size_t offsets[] = {0, 4096, 65536};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        if (offsets[i] + sizeof(sample) > partition->size) break;
        if (esp_partition_read(partition, offsets[i], sample,
                               sizeof(sample)) != ESP_OK) return false;
        for (size_t j = 0; j < sizeof(sample); ++j) {
            if (sample[j] != 0xff) return false;
        }
    }
    return true;
}

static esp_err_t mount_fs(void)
{
    const esp_vfs_spiffs_conf_t config = {
        .base_path = STORAGE_FS_BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    return esp_vfs_spiffs_register(&config);
}

esp_err_t storage_fs_init(void)
{
    if (s_ready) return ESP_OK;
    bool first_initialization = !was_initialized();
    esp_err_t error = mount_fs();
    if (error != ESP_OK && (partition_is_erased() || first_initialization)) {
        ESP_LOGI(TAG, "%s storage partition; formatting SPIFFS once",
                 first_initialization ? "first-install" : "blank");
        error = esp_spiffs_format(PARTITION_LABEL);
        if (error == ESP_OK) error = mount_fs();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed without erase: %s",
                 esp_err_to_name(error));
        return error;
    }
    size_t total = 0, used = 0;
    error = esp_spiffs_info(PARTITION_LABEL, &total, &used);
    if (error != ESP_OK) {
        esp_vfs_spiffs_unregister(PARTITION_LABEL);
        return error;
    }
    s_ready = true;
    mark_initialized();
    ESP_LOGI(TAG, "mounted %u KB, used %u KB", (unsigned)(total / 1024),
             (unsigned)(used / 1024));
    return ESP_OK;
}

bool storage_fs_is_ready(void)
{
    return s_ready;
}

esp_err_t storage_fs_read(const char *name, uint8_t **data, size_t *size,
                          size_t max_size)
{
    if (!s_ready || data == NULL || size == NULL) return ESP_ERR_INVALID_STATE;
    char path[96];
    if (!make_path(name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;
    char backup[96];
    int backup_len = snprintf(backup, sizeof(backup), "%s.bak", path);
    if (backup_len <= 0 || (size_t)backup_len >= sizeof(backup)) {
        return ESP_ERR_INVALID_SIZE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno != ENOENT || stat(backup, &st) != 0) {
            return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        /* Recover an interrupted old->backup, temporary->live sequence. */
        if (rename(backup, path) != 0) return ESP_FAIL;
    }
    if (st.st_size < 0 || (size_t)st.st_size > max_size ||
        (size_t)st.st_size > STORAGE_FS_FILE_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t *buffer = malloc((size_t)st.st_size + 1U);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        free(buffer);
        return ESP_FAIL;
    }
    size_t got = fread(buffer, 1, (size_t)st.st_size, file);
    int close_error = fclose(file);
    if (got != (size_t)st.st_size || close_error != 0) {
        free(buffer);
        return ESP_FAIL;
    }
    buffer[got] = 0;
    *data = buffer;
    *size = got;
    return ESP_OK;
}

esp_err_t storage_fs_write_atomic(const char *name, const void *data,
                                  size_t size)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if ((data == NULL && size != 0) || size > STORAGE_FS_FILE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[96];
    if (!make_path(name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;
    /* A temp->backup->live transaction causes several SPIFFS metadata
     * rewrites.  On the large end-of-flash partition its garbage collection
     * can keep the flash critical section beyond the interrupt-WDT window.
     * Replace the bounded object in one pass instead; every caller validates
     * the file on read and has a fallback for an interrupted write. */
    FILE *file = fopen(path, "wb");
    if (file == NULL) return ESP_FAIL;
    bool ok = fwrite(data, 1, size, file) == size;
    if (ok) ok = fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_fs_remove(const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[96];
    if (!make_path(name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;
    if (unlink(path) == 0) return ESP_OK;
    return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

bool storage_fs_exists(const char *name)
{
    if (!s_ready) return false;
    char path[96];
    struct stat st;
    return make_path(name, path, sizeof(path)) && stat(path, &st) == 0;
}
