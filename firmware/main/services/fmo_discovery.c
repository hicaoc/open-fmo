#include "fmo_discovery.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "services/fmo_aprs.h"
#include "services/network_manager.h"
#include "services/server_directory.h"

#define DISCOVERY_HOST "rotate.aprs2.net"
#define DISCOVERY_PORT "10152"
#define RECONNECT_MS 30000U
#define USER_SSID_CACHE_SIZE 128U

static const char *TAG = "fmo_discovery";
static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[8192 / sizeof(StackType_t)];
static portMUX_TYPE s_identity_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_callsign[16] = "NOCALL";
static uint8_t s_callsign_ssid;
static uint32_t s_identity_generation;
static portMUX_TYPE s_user_cache_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    char callsign[7];
    uint8_t ssid;
    uint32_t sequence;
} user_ssid_entry_t;

static user_ssid_entry_t s_user_ssids[USER_SSID_CACHE_SIZE];
static uint32_t s_user_ssid_sequence;

static void cache_user_ssid(const char *callsign, uint8_t ssid)
{
    size_t target = SIZE_MAX;
    size_t oldest = 0;
    portENTER_CRITICAL(&s_user_cache_lock);
    for (size_t i = 0; i < USER_SSID_CACHE_SIZE; ++i) {
        if (strcmp(s_user_ssids[i].callsign, callsign) == 0) {
            target = i;
            break;
        }
        if (s_user_ssids[i].callsign[0] == '\0' && target == SIZE_MAX) {
            target = i;
        }
        if (s_user_ssids[i].sequence < s_user_ssids[oldest].sequence) {
            oldest = i;
        }
    }
    if (target == SIZE_MAX) target = oldest;
    strlcpy(s_user_ssids[target].callsign, callsign,
            sizeof(s_user_ssids[target].callsign));
    s_user_ssids[target].ssid = ssid;
    s_user_ssids[target].sequence = ++s_user_ssid_sequence;
    portEXIT_CRITICAL(&s_user_cache_lock);
}

bool fmo_discovery_lookup_ssid(const char *callsign, uint8_t *ssid)
{
    if (callsign == NULL || callsign[0] == '\0' || ssid == NULL) return false;
    char upper[7] = {0};
    size_t length = strnlen(callsign, sizeof(upper));
    if (length == 0 || length >= sizeof(upper)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!isalnum((unsigned char)callsign[i])) return false;
        upper[i] = (char)toupper((unsigned char)callsign[i]);
    }
    bool found = false;
    portENTER_CRITICAL(&s_user_cache_lock);
    for (size_t i = 0; i < USER_SSID_CACHE_SIZE; ++i) {
        if (strcmp(s_user_ssids[i].callsign, upper) == 0) {
            *ssid = s_user_ssids[i].ssid;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_user_cache_lock);
    return found;
}

static uint16_t aprs_passcode(const char *callsign)
{
    uint16_t hash = 0x73e2;
    char root[10] = {0};
    size_t length = 0;
    while (callsign[length] != '\0' && callsign[length] != '-' &&
           length + 1 < sizeof(root)) {
        root[length] = (char)toupper((unsigned char)callsign[length]);
        ++length;
    }
    for (size_t i = 0; i < length; i += 2) {
        hash ^= (uint16_t)((uint8_t)root[i] << 8U);
        if (i + 1 < length) hash ^= (uint8_t)root[i + 1];
    }
    return hash & 0x7fffU;
}

static void identity_snapshot(char *callsign, size_t size, uint8_t *ssid)
{
    portENTER_CRITICAL(&s_identity_lock);
    strlcpy(callsign, s_callsign, size);
    *ssid = s_callsign_ssid;
    portEXIT_CRITICAL(&s_identity_lock);
}

static int connect_aprs(void)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(DISCOVERY_HOST, DISCOVERY_PORT, &hints, &result) != 0 ||
        result == NULL) return -1;
    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd >= 0 && connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd < 0) return -1;
    char base[16];
    uint8_t ssid = 0;
    identity_snapshot(base, sizeof(base), &ssid);
    char station[24];
    if (ssid > 0) {
        snprintf(station, sizeof(station), "%s-%u", base, (unsigned)ssid);
    } else {
        strlcpy(station, base, sizeof(station));
    }
    const int passcode = strcasecmp(base, "NOCALL") == 0
        ? -1 : (int)aprs_passcode(base);
    char login[96];
    int login_size = snprintf(login, sizeof(login),
                              "user %s pass %d vers OpenFMO 0.1\r\n",
                              station, passcode);
    if (login_size <= 0 || (size_t)login_size >= sizeof(login) ||
        send(fd, login, (size_t)login_size, 0) != login_size) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "APRS discovery connected as %s to %s:%s", station,
             DISCOVERY_HOST, DISCOVERY_PORT);
    return fd;
}

static void discovery_task(void *argument)
{
    (void)argument;
    int fd = -1;
    uint32_t identity_generation = 0;
    uint32_t reconnect_wait = 0;
    char line[800];
    size_t used = 0;
    for (;;) {
        uint32_t current_identity_generation;
        portENTER_CRITICAL(&s_identity_lock);
        current_identity_generation = s_identity_generation;
        portEXIT_CRITICAL(&s_identity_lock);
        if (identity_generation != current_identity_generation) {
            if (fd >= 0) close(fd);
            fd = -1;
            used = 0;
            reconnect_wait = 0;
            identity_generation = current_identity_generation;
        }
        network_status_t network = {0};
        network_manager_get_status(&network);
        if (!network.station_connected) {
            if (fd >= 0) close(fd);
            fd = -1;
            used = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (fd < 0) {
            if (reconnect_wait > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                reconnect_wait -= reconnect_wait >= 1000 ? 1000 : reconnect_wait;
                continue;
            }
            fd = connect_aprs();
            if (fd < 0) {
                reconnect_wait = RECONNECT_MS;
                continue;
            }
        }
        char buffer[512];
        int received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0 || (received < 0 && errno != EAGAIN &&
                              errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "APRS discovery disconnected errno=%d",
                     received < 0 ? errno : 0);
            close(fd);
            fd = -1;
            used = 0;
            reconnect_wait = RECONNECT_MS;
            continue;
        }
        if (received < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (int i = 0; i < received; ++i) {
            char ch = buffer[i];
            if (ch == '\r' || ch == '\n') {
                if (used > 0) {
                    line[used] = '\0';
                    char user_callsign[7];
                    uint8_t user_ssid = 0;
                    if (fmo_aprs_parse_source_ssid(line, used, user_callsign,
                                                   &user_ssid)) {
                        cache_user_ssid(user_callsign, user_ssid);
                    }
                    if (strstr(line, "FMO-V4,STATION,") != NULL) {
                        fmo_server_t server;
                        if (fmo_aprs_parse_station(line, used, &server) &&
                            fmo_server_directory_upsert(&server) == ESP_OK) {
                            ESP_LOGI(TAG, "FMO server: %s %s:%u (%lu/%lu)",
                                     server.name, server.host,
                                     (unsigned)server.port,
                                     (unsigned long)server.online,
                                     (unsigned long)server.total);
                        }
                    }
                    used = 0;
                }
            } else if (used + 1 < sizeof(line)) {
                line[used++] = ch;
            } else {
                used = 0;
            }
        }
    }
}

void fmo_discovery_update_config(const fmo_config_t *config)
{
    if (config == NULL) return;
    portENTER_CRITICAL(&s_identity_lock);
    strlcpy(s_callsign, config->fmo_callsign, sizeof(s_callsign));
    s_callsign_ssid = config->fmo_callsign_ssid;
    ++s_identity_generation;
    portEXIT_CRITICAL(&s_identity_lock);
}

esp_err_t fmo_discovery_start(const fmo_config_t *config)
{
    if (s_task != NULL) return ESP_OK;
    fmo_discovery_update_config(config);
    s_task = xTaskCreateStatic(
        discovery_task, "fmo_discovery",
        sizeof(s_task_stack) / sizeof(s_task_stack[0]), NULL, 3,
        s_task_stack, &s_task_tcb);
    return s_task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}
