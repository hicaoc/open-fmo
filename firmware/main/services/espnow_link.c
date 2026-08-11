#include "espnow_link.h"

#include <string.h>

#include "audio/nrl_audio_codec.h"
#include "audio/nrl_g711.h"
#include "audio/opus_voice.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/nrl_link.h"

#define ESPNOW_HEADER_SIZE 12U
#define ESPNOW_PACKET_MAX 250U
#define ESPNOW_G711_SAMPLES 160U
#define ESPNOW_OPUS_SAMPLES 320U

typedef struct {
    uint16_t size;
    uint8_t data[ESPNOW_PACKET_MAX];
} espnow_rx_packet_t;

static const char *TAG = "espnow";
static const uint8_t k_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_rx_queue;
static fmo_config_t s_config;
static volatile bool s_enabled;
static bool s_initialized;
static opus_voice_encoder_t *s_opus_encoder;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[4096 / sizeof(StackType_t)];

static void fill_header(uint8_t *packet, uint8_t type)
{
    memset(packet, 0, ESPNOW_HEADER_SIZE);
    memcpy(packet, "NRLE", 4);
    packet[4] = type;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(packet + 5, s_config.callsign,
           strnlen(s_config.callsign, 6));
    packet[11] = s_config.callsign_ssid;
    xSemaphoreGive(s_lock);
}

static void receive_callback(const esp_now_recv_info_t *info,
                             const uint8_t *data, int length)
{
    (void)info;
    if (!s_enabled || data == NULL || length <= (int)ESPNOW_HEADER_SIZE ||
        length > (int)ESPNOW_PACKET_MAX || memcmp(data, "NRLE", 4) != 0 ||
        (data[4] != NRL_PACKET_TYPE_VOICE &&
         data[4] != NRL_PACKET_TYPE_OPUS_VOICE)) return;
    espnow_rx_packet_t packet = {.size = (uint16_t)length};
    memcpy(packet.data, data, (size_t)length);
    (void)xQueueSend(s_rx_queue, &packet, 0);
}

static bool bring_up(void)
{
    if (s_initialized) return true;
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) return false;
    if (esp_now_init() != ESP_OK) return false;
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, k_broadcast, sizeof(k_broadcast));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) != ESP_OK ||
        esp_now_register_recv_cb(receive_callback) != ESP_OK) {
        esp_now_deinit();
        return false;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "link ready on current STA channel, %s",
             s_enabled ? "enabled" : "disabled");
    return true;
}

static void link_task(void *argument)
{
    (void)argument;
    while (!bring_up()) vTaskDelay(pdMS_TO_TICKS(1000));
    espnow_rx_packet_t packet;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        if (!s_enabled || packet.size <= ESPNOW_HEADER_SIZE) continue;
        nrl_audio_receive_encoded(packet.data[4],
                                  packet.data + ESPNOW_HEADER_SIZE,
                                  packet.size - ESPNOW_HEADER_SIZE);
    }
}

esp_err_t espnow_link_start(const fmo_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(6, sizeof(espnow_rx_packet_t));
    if (s_lock == NULL || s_rx_queue == NULL) return ESP_ERR_NO_MEM;
    s_config = *config;
    s_enabled = config->espnow_enabled;
    return xTaskCreateStatic(
               link_task, "espnow",
               sizeof(s_task_stack) / sizeof(s_task_stack[0]), NULL, 4,
               s_task_stack, &s_task_tcb) != NULL
        ? ESP_OK : ESP_ERR_NO_MEM;
}

bool espnow_link_set_enabled(bool enabled)
{
    if (enabled && !bring_up()) return false;
    s_enabled = enabled;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config.espnow_enabled = enabled;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", enabled ? "enabled" : "disabled");
    return true;
}

bool espnow_link_is_enabled(void)
{
    return s_enabled;
}

esp_err_t espnow_link_send_g711(const int16_t *pcm8k, size_t samples)
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;
    if (pcm8k == NULL || samples != ESPNOW_G711_SAMPLES) return ESP_ERR_INVALID_ARG;
    uint8_t packet[ESPNOW_HEADER_SIZE + ESPNOW_G711_SAMPLES];
    fill_header(packet, NRL_PACKET_TYPE_VOICE);
    for (size_t i = 0; i < samples; ++i) {
        packet[ESPNOW_HEADER_SIZE + i] = nrl_g711_encode_alaw(pcm8k[i]);
    }
    return esp_now_send(k_broadcast, packet, sizeof(packet));
}

esp_err_t espnow_link_send_opus(const int16_t *pcm16k, size_t samples)
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;
    if (pcm16k == NULL || samples != ESPNOW_OPUS_SAMPLES) return ESP_ERR_INVALID_ARG;
    if (s_opus_encoder == NULL) s_opus_encoder = opus_voice_encoder_open(20);
    if (s_opus_encoder == NULL) return ESP_ERR_NO_MEM;
    uint8_t packet[ESPNOW_PACKET_MAX];
    fill_header(packet, NRL_PACKET_TYPE_OPUS_VOICE);
    int encoded = opus_voice_encode(s_opus_encoder, pcm16k, samples,
                                    packet + ESPNOW_HEADER_SIZE,
                                    sizeof(packet) - ESPNOW_HEADER_SIZE);
    return encoded > 0
        ? esp_now_send(k_broadcast, packet, ESPNOW_HEADER_SIZE + encoded)
        : ESP_FAIL;
}
