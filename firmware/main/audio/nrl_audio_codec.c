#include "nrl_audio_codec.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/driver/status_io.h"
#include "audio/aprs_afsk.h"
#include "audio/audio_passthrough.h"
#include "audio/ctcss.h"
#include "audio/nrl_g711.h"
#include "audio/mdc_signaling.h"
#include "audio/opus_voice.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/net_radio.h"
#include "services/nrl_link.h"

#define AUDIO_PACKET_MAX 1024U
#define OPUS_FRAME_SAMPLES 320U
#define DOWNLINK_HANG_MS 300U

typedef struct {
    uint8_t type;
    uint16_t size;
    uint8_t payload[AUDIO_PACKET_MAX];
} audio_packet_t;

static const char *TAG = "nrl_codec";
#define DOWNLINK_QUEUE_CAPACITY 8U
static QueueHandle_t s_queue;
/* Static PSRAM-backed queue storage: ~8 KB of 1 KB items would otherwise
 * come out of the nearly-full internal DRAM. The queue is only touched
 * by tasks (no ISR context), so PSRAM data access is safe. */
static EXT_RAM_BSS_ATTR uint8_t s_queue_storage[DOWNLINK_QUEUE_CAPACITY * sizeof(audio_packet_t)];
static StaticQueue_t s_queue_static;
static SemaphoreHandle_t s_lock;
static StaticTask_t s_decode_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_decode_stack[32768 / sizeof(StackType_t)];
static opus_voice_encoder_t *s_opus_encoder;
static opus_voice_decoder_t *s_opus_decoder;
static nrl_audio_pcm_sink_t s_sink;
static void *s_sink_context;
static ctcss_detector_t s_rx_ctcss;
static ctcss_generator_t s_tx_ctcss;
static float s_rx_ctcss_hz;
static float s_tx_ctcss_hz;
static uint32_t s_rx_sample_rate;
static uint32_t s_tx_sample_rate;
static uint32_t s_carrier_generation;
static ctcss_gate_state_t s_rx_ctcss_state = CTCSS_GATE_DISABLED;
static float s_rx_detected_hz;
static bool s_ctcss_configured;
static volatile int s_rx_codec = -1;  /* -1=none, 0=G.711, 1=Opus */

static void receive_packet(uint8_t type, const uint8_t *payload,
                           size_t payload_size, void *context)
{
    (void)context;
    if (type != NRL_PACKET_TYPE_VOICE &&
        type != NRL_PACKET_TYPE_SERVER_VOICE &&
        type != NRL_PACKET_TYPE_OPUS_VOICE) return;
    if (payload == NULL || payload_size == 0 || payload_size > AUDIO_PACKET_MAX) return;
    /* NRL downlink voice preempts net-radio playback (voice has priority) */
    net_radio_notify_nrl_voice();
    audio_packet_t packet = {
        .type = type,
        .size = (uint16_t)payload_size,
    };
    memcpy(packet.payload, payload, payload_size);
    if (xQueueSend(s_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "downlink queue full; dropping type=%u", (unsigned)type);
    }
}

void nrl_audio_receive_encoded(uint8_t type, const uint8_t *payload,
                               size_t payload_size)
{
    receive_packet(type, payload, payload_size, NULL);
}

static void deliver_pcm(int16_t *pcm, size_t samples, uint32_t rate)
{
    mdc_signaling_feed(MDC_SOURCE_NRL, pcm, samples, rate);
    /* APRS NRL tap: decode AFSK from the network downlink audio before
     * any local tone injection. */
    aprs_afsk_feed_nrl(pcm, samples, rate);
    /* This is network downlink audio about to leave through the radio DAC.
     * Add the configured sub-audible tone in software because the BK4802
     * controller explicitly does not implement TCTCSS. */
    if (s_tx_ctcss_hz > 0.0f) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_tx_sample_rate != rate) {
            ctcss_generator_configure(&s_tx_ctcss, s_tx_ctcss_hz, rate);
            s_tx_sample_rate = rate;
        }
        ctcss_generator_mix(&s_tx_ctcss, pcm, samples);
        xSemaphoreGive(s_lock);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nrl_audio_pcm_sink_t sink = s_sink;
    void *context = s_sink_context;
    xSemaphoreGive(s_lock);
    if (sink != NULL) sink(pcm, samples, rate, context);
}

bool nrl_audio_radio_rx_accept(const int16_t *pcm, size_t samples,
                               uint32_t rate)
{
    if (s_lock == NULL) return true;  /* codec not initialized */
    const uint32_t carrier_generation = status_io_carrier_generation();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx_sample_rate != rate) {
        ctcss_detector_configure(&s_rx_ctcss, s_rx_ctcss_hz, rate);
        s_rx_sample_rate = rate;
        s_carrier_generation = carrier_generation;
        ESP_LOGI(TAG, "CTCSS detector configured: expect %.1f Hz @ %lu Hz",
                 (double)s_rx_ctcss_hz, (unsigned long)rate);
    } else if (s_carrier_generation != carrier_generation) {
        ctcss_detector_reset(&s_rx_ctcss);
        s_carrier_generation = carrier_generation;
        ESP_LOGI(TAG, "CTCSS detector reset (new carrier gen=%lu)",
                 (unsigned long)carrier_generation);
    }
    s_rx_ctcss_state = ctcss_detector_feed(&s_rx_ctcss, pcm, samples);
    s_rx_detected_hz = ctcss_detector_detected_hz(&s_rx_ctcss);
    const bool matched = s_rx_ctcss_state == CTCSS_GATE_MATCHED;
    const bool rejected = s_rx_ctcss_state == CTCSS_GATE_REJECTED;
    xSemaphoreGive(s_lock);
    /* Bypass: no RX tone configured → detection ran for display, pass audio */
    if (s_rx_ctcss_hz <= 0.0f) {
        return true;
    }
    status_io_set_ctcss_gate(matched, rejected);
    return matched;
}

static void decode_task(void *argument)
{
    (void)argument;
    static int16_t test_input[OPUS_FRAME_SAMPLES];
    /* The decoder needs headroom for up to a 60 ms remote packet. */
    static int16_t test_output[OPUS_FRAME_SAMPLES * 3U];
    static uint8_t test_frame[OPUS_VOICE_MAX_FRAME_BYTES];
    int test_encoded = opus_voice_encode(s_opus_encoder, test_input,
                                         OPUS_FRAME_SAMPLES, test_frame,
                                         sizeof(test_frame));
    int test_decoded = test_encoded > 0
        ? opus_voice_decode(s_opus_decoder, test_frame, (size_t)test_encoded,
                            test_output,
                            sizeof(test_output) / sizeof(test_output[0]))
        : -1;
    if (test_encoded > 0 && test_decoded == OPUS_FRAME_SAMPLES) {
        ESP_LOGI(TAG, "G.711 A-law 8k ready; Opus self-test %d bytes -> %d samples",
                 test_encoded, test_decoded);
    } else {
        ESP_LOGE(TAG, "Opus self-test failed: encoded=%d decoded=%d",
                 test_encoded, test_decoded);
    }

    audio_packet_t packet;
    int16_t pcm[AUDIO_PACKET_MAX];
    int64_t last_voice_us = 0;
    bool ptt_active = false;
    for (;;) {
        if (xQueueReceive(s_queue, &packet, pdMS_TO_TICKS(50)) == pdTRUE) {
            int decoded = 0;
            uint32_t rate = 8000;
            if (packet.type == NRL_PACKET_TYPE_OPUS_VOICE) {
                rate = OPUS_VOICE_SAMPLE_RATE;
                decoded = opus_voice_decode(s_opus_decoder, packet.payload,
                                            packet.size, pcm,
                                            sizeof(pcm) / sizeof(pcm[0]));
                if (decoded > 0) s_rx_codec = 1;
            } else {
                decoded = packet.size;
                for (int i = 0; i < decoded; ++i) {
                    pcm[i] = nrl_g711_decode_alaw(packet.payload[i]);
                }
                if (decoded > 0) s_rx_codec = 0;
            }
            if (decoded > 0) {
                ptt_active = true;
                last_voice_us = esp_timer_get_time();
                nrl_remote_identity_t identity = {0};
                char callsign[16] = "NRL";
                if (nrl_link_get_last_identity(&identity)) {
                    snprintf(callsign, sizeof(callsign), "%s",
                             identity.callsign);
                }
                audio_passthrough_note_network_voice(
                    AUDIO_NETWORK_NRL, callsign,
                    packet.type == NRL_PACKET_TYPE_OPUS_VOICE
                        ? "OPUS" : "G711");
                deliver_pcm(pcm, (size_t)decoded, rate);
            }
        }
        if (ptt_active && esp_timer_get_time() - last_voice_us >=
                              (int64_t)DOWNLINK_HANG_MS * 1000) {
            ptt_active = false;
            s_rx_codec = -1;  /* clear codec indicator when voice ends */
        }
    }
}

esp_err_t nrl_audio_codec_init(void)
{
    if (s_queue != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreateStatic(DOWNLINK_QUEUE_CAPACITY, sizeof(audio_packet_t),
                                 s_queue_storage, &s_queue_static);
    if (s_lock == NULL || s_queue == NULL) return ESP_ERR_NO_MEM;
    esp_err_t g711_error = nrl_g711_init();
    if (g711_error != ESP_OK) {
        ESP_LOGW(TAG, "G.711 LUT unavailable; using algorithmic encoder");
    }
    s_opus_encoder = opus_voice_encoder_open(20);
    s_opus_decoder = opus_voice_decoder_open(20);
    if (s_opus_encoder == NULL || s_opus_decoder == NULL) {
        ESP_LOGE(TAG, "Opus 16k/20ms codec allocation failed");
        return ESP_ERR_NO_MEM;
    }
    nrl_link_set_packet_handler(receive_packet, NULL);
    /* Opus complexity 10 encoding needs 32 KB stack; EXT_RAM_BSS_ATTR on
     * s_decode_stack places it in PSRAM (internal DRAM is nearly full). */
    if (xTaskCreateStaticPinnedToCore(decode_task, "nrl_codec",
                                      sizeof(s_decode_stack) / sizeof(s_decode_stack[0]),
                                      NULL, 6, s_decode_stack,
                                      &s_decode_tcb, 1) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "G.711/Opus codecs allocated; self-test scheduled on 32 KB task");
    return ESP_OK;
}

void nrl_audio_codec_configure_ctcss(float rx_hz, float tx_hz)
{
    if (rx_hz < 0.0f) rx_hz = 0.0f;
    if (tx_hz < 0.0f) tx_hz = 0.0f;
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rx_ctcss_hz = rx_hz;
    s_tx_ctcss_hz = tx_hz;
    s_ctcss_configured = true;
    s_rx_sample_rate = 0;
    s_tx_sample_rate = 0;
    s_rx_detected_hz = 0.0f;
    /* Always SEARCHING: detection runs for display even when rx_hz=0 (OFF).
     * Gating (blocking audio) is handled separately in radio_rx_ctcss_accept. */
    s_rx_ctcss_state = CTCSS_GATE_SEARCHING;
    if (s_lock != NULL) xSemaphoreGive(s_lock);
    status_io_configure_ctcss_gate(rx_hz > 0.0f);
    ESP_LOGI(TAG, "software CTCSS configured: RX=%s%.1f TX=%s%.1f Hz",
             rx_hz > 0.0f ? "" : "OFF/", rx_hz,
             tx_hz > 0.0f ? "" : "OFF/", tx_hz);
}

void nrl_audio_codec_get_ctcss_status(nrl_audio_ctcss_status_t *status)
{
    if (status == NULL) return;
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    status->configured = s_ctcss_configured;
    status->rx_expected_hz = s_rx_ctcss_hz;
    status->rx_detected_hz = s_rx_detected_hz;
    status->tx_hz = s_tx_ctcss_hz;
    status->rx_state = s_rx_ctcss_state;
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

void nrl_audio_codec_set_sink(nrl_audio_pcm_sink_t sink, void *context)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sink = sink;
    s_sink_context = context;
    xSemaphoreGive(s_lock);
}

esp_err_t nrl_audio_send_g711(const int16_t *pcm8k, size_t sample_count)
{
    static uint32_t s_g711_count;
    if (pcm8k == NULL || sample_count == 0 || sample_count > 500) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nrl_audio_radio_rx_accept(pcm8k, sample_count, 8000)) {
        if (s_g711_count == 0) ESP_LOGW(TAG, "g711: CTCSS rejected");
        return ESP_ERR_INVALID_STATE;
    }
    mdc_signaling_feed(MDC_SOURCE_RADIO, pcm8k, sample_count, 8000);
    uint8_t payload[500];
    for (size_t i = 0; i < sample_count; ++i) {
        payload[i] = nrl_g711_encode_alaw(pcm8k[i]);
    }
    esp_err_t err = nrl_link_send(NRL_PACKET_TYPE_VOICE, payload, sample_count);
    if (++s_g711_count <= 3) {
        ESP_LOGI(TAG, "g711 TX #%lu: %u samples, send=%s",
                 (unsigned long)s_g711_count, (unsigned)sample_count,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t nrl_audio_send_opus(const int16_t *pcm16k, size_t sample_count)
{
    if (pcm16k == NULL || sample_count != OPUS_FRAME_SAMPLES ||
        s_opus_encoder == NULL) return ESP_ERR_INVALID_ARG;
    if (!nrl_audio_radio_rx_accept(pcm16k, sample_count,
                                   OPUS_VOICE_SAMPLE_RATE)) {
        return ESP_ERR_INVALID_STATE;
    }
    mdc_signaling_feed(MDC_SOURCE_RADIO, pcm16k, sample_count,
                       OPUS_VOICE_SAMPLE_RATE);
    uint8_t payload[OPUS_VOICE_MAX_FRAME_BYTES];
    int encoded = opus_voice_encode(s_opus_encoder, pcm16k, sample_count,
                                    payload, sizeof(payload));
    if (encoded <= 0) {
        ESP_LOGW(TAG, "opus encode failed: %d", encoded);
        return ESP_FAIL;
    }
    esp_err_t err = nrl_link_send(NRL_PACKET_TYPE_OPUS_VOICE, payload,
                                  (size_t)encoded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nrl_link_send failed: %s", esp_err_to_name(err));
    }
    return err;
}

int nrl_audio_codec_get_rx_codec(void)
{
    return s_rx_codec;
}
