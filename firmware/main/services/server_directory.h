#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char name[96];
    char host[64];
    uint16_t port;
    uint32_t online;
    uint32_t total;
    int sort_order;
} nrl_server_t;

#define FMO_SERVER_KEY_MAX 80
#define FMO_SERVER_NAME_MAX 96

typedef struct {
    char key[FMO_SERVER_KEY_MAX]; /* uid:<n>, otherwise host:port */
    char name[96];
    char host[64];
    char callsign[16];
    uint16_t port;
    uint32_t uid;
    uint32_t online;
    uint32_t total;
    uint8_t ssid;
    bool has_ssid;
    uint8_t fingerprint[32];
    bool has_fingerprint;
    bool favorite;
    int64_t last_seen;
} fmo_server_t;

esp_err_t server_directory_init(void);
size_t server_directory_count(void);
const nrl_server_t *server_directory_get(size_t index);
size_t server_directory_find(const char *host, uint16_t port);
uint32_t server_directory_generation(void);

/* FMO directory is populated by APRS discovery. Main-screen selection uses
 * only the favorite view; the settings page can enumerate the full table. */
size_t fmo_server_directory_count(void);
const fmo_server_t *fmo_server_directory_get(size_t index);
size_t fmo_server_directory_find(const char *key);
size_t fmo_server_directory_favorite_count(void);
const fmo_server_t *fmo_server_directory_get_favorite(size_t favorite_index);
esp_err_t fmo_server_directory_upsert(const fmo_server_t *server);
esp_err_t fmo_server_directory_set_favorite(const char *key, bool favorite);
uint32_t fmo_server_directory_generation(void);
esp_err_t fmo_server_directory_flush(void);
