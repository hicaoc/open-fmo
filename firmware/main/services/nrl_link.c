#include "nrl_link.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "app/driver/status_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "services/network_manager.h"
#include "services/app_notice.h"
#include "services/nrl_at_commands.h"
#include "version.h"

#define HEARTBEAT_INTERVAL_MS 2000U
#define NRL_DEVICE_MODE 22U
#define RX_PACKET_CAPACITY 1200U
/* Keep packet identity slightly longer than the 900 ms audio metadata hang,
 * so the UI can render the incoming SSID at the callsign's lower right for
 * the full duration of an NRL voice burst. */
#define VOICE_IDENTITY_HANG_MS 1200U

static const char *TAG = "nrl_link";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_socket_lock;
static fmo_config_t s_config;
static uint32_t s_config_generation;
static uint16_t s_packet_counter;
static int s_socket = -1;
static struct sockaddr_in s_remote;
static nrl_link_packet_handler_t s_handler;
static void *s_handler_context;
static nrl_remote_identity_t s_last_identity;
static uint32_t s_last_voice_ms;
static uint32_t s_last_reply_ms;
/* 1 KB AT reply object; PSRAM-backed because internal DRAM is nearly
 * full (esp_psram needs 64 KB free to reserve its DMA pool). */
static EXT_RAM_BSS_ATTR nrl_at_result_t s_link_result;
/* link_task is single-instance, so its big working buffers can live in
 * PSRAM as statics instead of eating the 8 KB task stack (rx + framed
 * together would cost ~2.2 KB of stack while the AT handler runs). */
static EXT_RAM_BSS_ATTR uint8_t s_rx[RX_PACKET_CAPACITY];
static const char s_at_banner[] =
    FMO_FIRMWARE_NAME " v" FMO_FIRMWARE_VERSION "\r\n";
static EXT_RAM_BSS_ATTR uint8_t s_at_framed[1 + sizeof(s_at_banner) - 1 +
                                            NRL_AT_REPLY_CAPACITY];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static size_t build_packet(uint8_t type, const uint8_t *payload,
                           size_t payload_size, uint8_t *packet,
                           size_t capacity)
{
    const size_t total_size = NRL_PACKET_HEADER_SIZE + payload_size;
    if (packet == NULL || total_size > capacity || total_size > UINT16_MAX) {
        return 0;
    }

    fmo_config_t config;
    uint16_t counter;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    config = s_config;
    counter = ++s_packet_counter;
    xSemaphoreGive(s_lock);

    memset(packet, 0, total_size);
    memcpy(packet, "NRL2", 4);
    packet[4] = (uint8_t)(total_size >> 8);
    packet[5] = (uint8_t)total_size;
    /* Bytes 6..19 are the CPU/device identifier. The reference firmware
     * intentionally transmits zero here for compatibility. */
    packet[20] = type;
    packet[21] = 1;
    packet[22] = (uint8_t)(counter >> 8);
    packet[23] = (uint8_t)counter;
    memcpy(packet + 24, config.callsign,
           strnlen(config.callsign, 6));
    packet[30] = config.callsign_ssid;
    packet[31] = NRL_DEVICE_MODE;
    if (payload_size != 0 && payload != NULL) {
        memcpy(packet + NRL_PACKET_HEADER_SIZE, payload, payload_size);
    }
    return total_size;
}

static bool parse_packet(const uint8_t *packet, size_t packet_size,
                         uint8_t *type, const uint8_t **payload,
                         size_t *payload_size, char *callsign,
                         size_t callsign_size, uint8_t *ssid)
{
    if (packet == NULL || packet_size < NRL_PACKET_HEADER_SIZE ||
        memcmp(packet, "NRL2", 4) != 0) {
        return false;
    }
    uint16_t total = (uint16_t)(((uint16_t)packet[4] << 8) | packet[5]);
    if (total == NRL_PACKET_HEADER_SIZE && packet_size > total) {
        total = (uint16_t)packet_size;
    }
    if (total < NRL_PACKET_HEADER_SIZE || total > packet_size) return false;
    if (type != NULL) *type = packet[20];
    if (payload != NULL) *payload = packet + NRL_PACKET_HEADER_SIZE;
    if (payload_size != NULL) *payload_size = total - NRL_PACKET_HEADER_SIZE;
    if (callsign != NULL && callsign_size > 0) {
        size_t length = 0;
        while (length < 6 && packet[24 + length] != '\0' &&
               packet[24 + length] != ' ') {
            ++length;
        }
        if (length >= callsign_size) length = callsign_size - 1;
        memcpy(callsign, packet + 24, length);
        callsign[length] = '\0';
    }
    if (ssid != NULL) *ssid = packet[30];
    return true;
}

static void update_last_source(uint8_t type, const char *callsign, uint8_t ssid,
                               uint32_t dmr_id)
{
    if (type != NRL_PACKET_TYPE_VOICE &&
        type != NRL_PACKET_TYPE_SERVER_VOICE &&
        type != NRL_PACKET_TYPE_OPUS_VOICE) return;
    if (callsign == NULL || callsign[0] == '\0') return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_voice_ms = now_ms();
    if (strcmp(callsign, s_last_identity.callsign) != 0 ||
        ssid != s_last_identity.ssid || dmr_id != s_last_identity.dmr_id ||
        type != s_last_identity.voice_type) {
        strlcpy(s_last_identity.callsign, callsign,
                sizeof(s_last_identity.callsign));
        s_last_identity.ssid = ssid;
        s_last_identity.dmr_id = dmr_id;
        s_last_identity.voice_type = type;
        ++s_last_identity.generation;
        ESP_LOGI(TAG, "NRL voice source: %s SSID=%u DMRID=%lu type=%u",
                 s_last_identity.callsign, (unsigned)ssid,
                 (unsigned long)dmr_id, (unsigned)type);
    }
    xSemaphoreGive(s_lock);
}

static void handle_text_notice(const char *callsign, uint8_t ssid,
                               const uint8_t *payload, size_t payload_size)
{
    static const char tag[] = "[aprs]";
    if (payload == NULL || payload_size <= sizeof(tag) - 1 ||
        strncasecmp((const char *)payload, tag, sizeof(tag) - 1) != 0) {
        return;
    }
    char message[96];
    size_t input = sizeof(tag) - 1;
    while (input < payload_size && payload[input] == ' ') ++input;
    size_t output = 0;
    while (input < payload_size && output + 1 < sizeof(message)) {
        const uint8_t ch = payload[input++];
        message[output++] = ch >= 0x20 && ch != 0x7f ? (char)ch : ' ';
    }
    while (output > 0 && message[output - 1] == ' ') --output;
    message[output] = '\0';
    char source[16];
    snprintf(source, sizeof(source), "%s-%u",
             callsign != NULL && callsign[0] != '\0' ? callsign : "NRL",
             (unsigned)ssid);
    app_notice_post_aprs(source, message);
    ESP_LOGI(TAG, "APRS text notice from %s: %s", source, message);
}

static void close_socket(void)
{
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    if (s_socket >= 0) close(s_socket);
    s_socket = -1;
    memset(&s_remote, 0, sizeof(s_remote));
    xSemaphoreGive(s_socket_lock);
}

static bool open_socket(const fmo_config_t *config)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *result = NULL;
    int dns_error = getaddrinfo(config->nrl_host, NULL, &hints, &result);
    if (dns_error != 0 || result == NULL) {
        ESP_LOGW(TAG, "resolve failed: %s (%d)", config->nrl_host, dns_error);
        if (result != NULL) freeaddrinfo(result);
        return false;
    }
    struct sockaddr_in remote = *(const struct sockaddr_in *)result->ai_addr;
    freeaddrinfo(result);
    remote.sin_port = htons(config->nrl_port);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int tos = 0xC0;
    (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(config->nrl_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGW(TAG, "UDP bind(%u) failed: errno=%d",
                 (unsigned)config->nrl_port, errno);
        close(fd);
        return false;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    s_socket = fd;
    s_remote = remote;
    xSemaphoreGive(s_socket_lock);
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntoa_r(remote.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "UDP local=%u target=%s(%s):%u heartbeat=%ums",
             (unsigned)config->nrl_port, config->nrl_host, ip,
             (unsigned)config->nrl_port, (unsigned)HEARTBEAT_INTERVAL_MS);
    return true;
}

esp_err_t nrl_link_send(uint8_t type, const uint8_t *payload,
                        size_t payload_size)
{
    /* Payload capacity covers a full AT+READ=123 reply framed with the
     * 0x02 marker and firmware banner (~1.05 KB). */
    uint8_t packet[NRL_PACKET_HEADER_SIZE + 1100];
    size_t size = build_packet(type, payload, payload_size,
                               packet, sizeof(packet));
    if (size == 0) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    if (s_socket < 0) {
        xSemaphoreGive(s_socket_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ssize_t sent = sendto(s_socket, packet, size, 0,
                          (struct sockaddr *)&s_remote, sizeof(s_remote));
    xSemaphoreGive(s_socket_lock);
    return sent == (ssize_t)size ? ESP_OK : ESP_FAIL;
}

static void link_task(void *argument)
{
    (void)argument;
    uint32_t applied_generation = 0;
    uint32_t last_heartbeat = 0;
    uint32_t next_open_attempt = 0;
    bool saw_reply = false;
    uint8_t *const rx = s_rx;

    for (;;) {
        network_status_t network = {0};
        network_manager_get_status(&network);
        fmo_config_t config;
        uint32_t generation;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        config = s_config;
        generation = s_config_generation;
        xSemaphoreGive(s_lock);

        if (!network.station_connected) {
            if (s_socket >= 0) close_socket();
            applied_generation = 0;
            last_heartbeat = 0;
            status_io_reset_heartbeat();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_last_reply_ms = 0;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        uint32_t now = now_ms();
        if (generation != applied_generation) {
            close_socket();
            status_io_reset_heartbeat();
            saw_reply = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_last_reply_ms = 0;
            xSemaphoreGive(s_lock);
            last_heartbeat = 0;
            next_open_attempt = 0;
            applied_generation = generation;
            ESP_LOGI(TAG, "target changed to %s:%u", config.nrl_host,
                     (unsigned)config.nrl_port);
        }
        if (s_socket < 0) {
            if ((int32_t)(now - next_open_attempt) >= 0) {
                if (!open_socket(&config)) next_open_attempt = now + 5000;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (last_heartbeat == 0 || now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
            if (nrl_link_send(NRL_PACKET_TYPE_HEARTBEAT, NULL, 0) == ESP_OK) {
                last_heartbeat = now;
            }
        }

        for (int i = 0; i < 8; ++i) {
            struct sockaddr_in from = {0};
            socklen_t from_size = sizeof(from);
            xSemaphoreTake(s_socket_lock, portMAX_DELAY);
            ssize_t received = s_socket >= 0
                ? recvfrom(s_socket, rx, RX_PACKET_CAPACITY, MSG_DONTWAIT,
                           (struct sockaddr *)&from, &from_size)
                : -1;
            xSemaphoreGive(s_socket_lock);
            if (received <= 0) break;
            uint8_t type = 0;
            const uint8_t *payload = NULL;
            size_t payload_size = 0;
            char callsign[7] = {0};
            uint8_t ssid = 0;
            if (!parse_packet(rx, (size_t)received, &type,
                              &payload, &payload_size, callsign,
                              sizeof(callsign), &ssid)) continue;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_last_reply_ms = now_ms();
            xSemaphoreGive(s_lock);
            status_io_notify_heartbeat();
            const uint32_t dmr_id = ((uint32_t)rx[6] << 16U) |
                                    ((uint32_t)rx[7] << 8U) | rx[8];
            update_last_source(type, callsign, ssid, dmr_id);
            if (type == NRL_PACKET_TYPE_TEXT) {
                handle_text_notice(callsign, ssid, payload, payload_size);
            }
            if (type == NRL_PACKET_TYPE_AT_COMMAND) {
                nrl_at_handle_payload(payload, payload_size,
                                      NRL_AT_SOURCE_REMOTE, &s_link_result);
                if (s_link_result.should_reply && s_link_result.payload_size > 0) {
                    /* The NRL app only accepts AT replies framed with the
                     * 0x02 reply marker followed by the firmware banner,
                     * exactly like the reference firmware's startAtReply(). */
                    size_t off = 0;
                    s_at_framed[off++] = 0x02;
                    memcpy(s_at_framed + off, s_at_banner,
                           sizeof(s_at_banner) - 1);
                    off += sizeof(s_at_banner) - 1;
                    memcpy(s_at_framed + off, s_link_result.payload,
                           s_link_result.payload_size);
                    off += s_link_result.payload_size;
                    nrl_link_send(NRL_PACKET_TYPE_AT_COMMAND, s_at_framed, off);
                }
            }
            if (!saw_reply) {
                ESP_LOGI(TAG, "server replied: type=%u bytes=%u",
                         (unsigned)type, (unsigned)received);
                saw_reply = true;
            }
            nrl_link_packet_handler_t handler;
            void *context;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            handler = s_handler;
            context = s_handler_context;
            xSemaphoreGive(s_lock);
            if (handler != NULL) handler(type, payload, payload_size, context);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t nrl_link_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_socket_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_socket_lock == NULL) return ESP_ERR_NO_MEM;
    s_config = *config;
    s_config_generation = 1;
    /* AT+READ=123 runs config/NVS snapshots, OTA/network snapshots, several
     * newlib float formatters and the reply send path on this task. 10 KB
     * overflowed on the ESP-IDF 6.2 toolchain when the server sent READ=123;
     * keep this stack internal (the handler can access flash) and leave enough
     * headroom for future AT fields. */
    return xTaskCreate(link_task, "nrl_link", 16384, NULL, 5, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

void nrl_link_update_config(const fmo_config_t *config)
{
    if (config == NULL || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = *config;
    ++s_config_generation;
    xSemaphoreGive(s_lock);
}

void nrl_link_set_packet_handler(nrl_link_packet_handler_t handler,
                                 void *context)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_handler = handler;
    s_handler_context = context;
    xSemaphoreGive(s_lock);
}

bool nrl_link_get_last_identity(nrl_remote_identity_t *identity)
{
    if (identity == NULL || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *identity = s_last_identity;
    const bool available = s_last_identity.callsign[0] != '\0' &&
        now_ms() - s_last_voice_ms <= VOICE_IDENTITY_HANG_MS;
    xSemaphoreGive(s_lock);
    return available;
}

void nrl_link_get_status(nrl_link_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_lock == NULL || s_socket_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->last_reply_ms = s_last_reply_ms;
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    status->socket_ready = s_socket >= 0;
    xSemaphoreGive(s_socket_lock);
    status->online = status->socket_ready && status->last_reply_ms != 0 &&
        now_ms() - status->last_reply_ms <= HEARTBEAT_INTERVAL_MS * 3U;
}
