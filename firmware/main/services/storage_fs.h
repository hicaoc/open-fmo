#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STORAGE_FS_BASE_PATH "/fs"
#define STORAGE_FS_FILE_MAX (128U * 1024U)

/* Mount the flash-end storage partition. A completely erased partition is
 * formatted once; an existing but damaged filesystem is never auto-erased. */
esp_err_t storage_fs_init(void);
bool storage_fs_is_ready(void);

/* Names are flat SPIFFS object names (for example "nrl_servers.json"). */
esp_err_t storage_fs_read(const char *name, uint8_t **data, size_t *size,
                          size_t max_size);
/* Bounded single-object replacement. Readers must validate content and use
 * their documented fallback if power is lost during the write. */
esp_err_t storage_fs_write_atomic(const char *name, const void *data,
                                  size_t size);
esp_err_t storage_fs_remove(const char *name);
bool storage_fs_exists(const char *name);
