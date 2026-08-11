/* AFSK APRS glue, ported from NRL-ESP32 (aprs_service.cpp audio parts).
 *
 * RX: two demodulator taps (0 = radio mic / RF, 1 = NRL network downlink).
 *     Audio tasks stash PCM into per-tap rings; this task resamples each
 *     ring to 9600 Hz (linear interpolation) and feeds MODEM_DECODE_CH().
 * TX: one modulator. The task pulls 8 kHz samples from
 *     MODEM_BAUDRATE_TIMER_HANDLER() and fans them out over the enabled
 *     routes: speaker output queue (VOX keys the radio) and/or G.711
 *     A-law frames over the NRL voice uplink.
 * All modem/AX.25 state lives on the AFSK task; other tasks only touch
 * the rings and the small locked queues. */

#include "audio/aprs_afsk.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio/aprs/aprs_ax25.h"
#include "audio/aprs/aprs_modem.h"

extern "C" {
#include "audio/audio_passthrough.h"
#include "audio/nrl_g711.h"
#include "services/net_radio.h"
#include "services/nrl_link.h"
}

static const char *TAG = "aprs_afsk";

#define RF_RING_SAMPLES   16384u  /* ~1 s of the 16 kHz mic tap */
#define NRL_RING_SAMPLES  8192u   /* ~1 s of the 8 kHz NRL downlink */
#define TX_LINE_MAX       400u
#define TX_QUEUE_SLOTS    4u
#define RX_FRAME_SLOTS    4u
#define TX_CHUNK_SAMPLES  160u    /* 20 ms at MODEM_TX_SAMPLE_RATE */
#define TX_AMPLITUDE      180     /* per 8-bit sine step (~0.7 FS) */
#define G711_FRAME_SAMPLES 160u   /* 20 ms at 8 kHz */

typedef struct {
    int16_t *buf;
    size_t size;
    volatile size_t head;    /* audio task writes */
    volatile size_t tail;    /* AFSK task consumes */
    volatile uint32_t rate;  /* input sample rate, 8000 or 16000 */
    uint32_t phase;          /* Q16 resampler phase */
    int16_t last;            /* previous input sample */
    uint32_t level_acc;
    uint32_t level_n;
    uint16_t level_mv;
} afsk_ring_t;

static afsk_ring_t s_rf_ring;
static afsk_ring_t s_nrl_ring;

static volatile bool s_rx_rf;
static volatile bool s_rx_nrl;
static volatile bool s_tx_rf;
static volatile bool s_tx_nrl;
static volatile bool s_tx_active;

/* Pending TX lines (service task -> AFSK task) */
static char s_tx_lines[TX_QUEUE_SLOTS][TX_LINE_MAX];
static uint8_t s_tx_head;
static uint8_t s_tx_tail;
static SemaphoreHandle_t s_tx_lock;

/* Decoded frames (AFSK task -> service task) */
typedef struct {
    char line[TX_LINE_MAX];
    uint8_t source;
} afsk_rx_frame_t;
static afsk_rx_frame_t s_rx_frames[RX_FRAME_SLOTS];
static uint8_t s_rx_head;
static uint8_t s_rx_tail;
static SemaphoreHandle_t s_rx_lock;

static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[16384 / sizeof(StackType_t)];

/* ------------------------------------------------------------------ */
/* RX: taps, resampling, demodulation                                  */
/* ------------------------------------------------------------------ */

static void ring_feed(afsk_ring_t *ring, const int16_t *pcm, size_t count,
                      uint32_t rate)
{
    if (ring->buf == NULL || pcm == NULL || count == 0) return;
    ring->rate = rate;
    size_t head = ring->head;
    const size_t tail = ring->tail;
    for (size_t i = 0; i < count; ++i) {
        const size_t next = (head + 1) % ring->size;
        if (next == tail) break;  /* ring full: drop the rest */
        ring->buf[head] = pcm[i];
        head = next;
    }
    ring->head = head;
}

/* Resample one ring to MODEM_RX_SAMPLE_RATE (linear interpolation) and
 * feed the given demodulator instance sample-by-sample. */
static void ring_demod(afsk_ring_t *ring, uint8_t demod)
{
    if (ring->buf == NULL) return;
    const uint32_t rate = ring->rate;
    if (rate != 8000u && rate != 16000u) {
        ring->tail = ring->head;  /* unsupported rate: drain */
        return;
    }
    const uint32_t step =
        (uint32_t)((uint64_t)65536u * rate / MODEM_RX_SAMPLE_RATE);
    size_t tail = ring->tail;
    const size_t head = ring->head;
    while (tail != head) {
        const int16_t sample = ring->buf[tail];
        tail = (tail + 1) % ring->size;

        ring->level_acc += (uint32_t)abs(sample);
        if (++ring->level_n >= 1024) {
            ring->level_mv = (uint16_t)((ring->level_acc / 1024) >> 4);
            ring->level_acc = 0;
            ring->level_n = 0;
        }

        ring->phase += 65536u;
        while (ring->phase >= step) {
            ring->phase -= step;
            const int32_t frac =
                (int32_t)(((uint64_t)ring->phase << 8) / step);
            const int32_t out =
                (int32_t)sample +
                ((((int32_t)ring->last - (int32_t)sample) * frac) >> 8);
            MODEM_DECODE_CH((int16_t)out, ring->level_mv, demod);
        }
        ring->last = sample;
    }
    ring->tail = tail;
}

void aprs_afsk_feed_rf(const int16_t *pcm, size_t samples, uint32_t rate)
{
    if (!s_rx_rf) return;
    ring_feed(&s_rf_ring, pcm, samples, rate);
}

void aprs_afsk_feed_nrl(const int16_t *pcm, size_t samples, uint32_t rate)
{
    if (!s_rx_nrl) return;
    ring_feed(&s_nrl_ring, pcm, samples, rate);
}

/* ------------------------------------------------------------------ */
/* RX: decoded frame collection                                        */
/* ------------------------------------------------------------------ */

static void rx_frame_poll(void)
{
    while (Ax25NewRxFrames()) {
        uint8_t *frame = NULL;
        uint16_t size = 0;
        int8_t peak = 0, valley = 0;
        uint8_t level = 0, corrected = 0;
        uint16_t mv = 0;
        uint8_t rx_source = 0;
        if (!Ax25ReadNextRxFrame(&frame, &size, &peak, &valley, &level,
                                 &corrected, &mv, &rx_source)) {
            break;
        }
        AX25Msg msg = {};
        ax25_decode(frame, size, mv, &msg);
        if (msg.ctrl != AX25_CTRL_UI || msg.pid != AX25_PID_NOLAYER3) {
            continue;
        }

        /* Rebuild the TNC2 monitor line */
        afsk_rx_frame_t slot;
        memset(&slot, 0, sizeof(slot));
        slot.source = rx_source;
        int off = snprintf(slot.line, sizeof(slot.line), "%s", msg.src.call);
        if (msg.src.ssid > 0) {
            off += snprintf(slot.line + off, sizeof(slot.line) - off, "-%u",
                            msg.src.ssid);
        }
        off += snprintf(slot.line + off, sizeof(slot.line) - off, ">%s",
                        msg.dst.call);
        if (msg.dst.ssid > 0) {
            off += snprintf(slot.line + off, sizeof(slot.line) - off, "-%u",
                            msg.dst.ssid);
        }
        for (uint8_t i = 0;
             i < msg.rpt_count && off < (int)sizeof(slot.line) - 16; ++i) {
            off += snprintf(slot.line + off, sizeof(slot.line) - off, ",%s",
                            msg.rpt_list[i].call);
            if (msg.rpt_list[i].ssid > 0) {
                off += snprintf(slot.line + off, sizeof(slot.line) - off,
                                "-%u", msg.rpt_list[i].ssid);
            }
            if (msg.rpt_flags & (1u << i)) {
                off += snprintf(slot.line + off, sizeof(slot.line) - off, "*");
            }
        }
        const size_t info_len =
            (msg.len < sizeof(slot.line) - (size_t)off - 2)
                ? msg.len
                : sizeof(slot.line) - off - 2;
        off += snprintf(slot.line + off, sizeof(slot.line) - off, ":");
        memcpy(slot.line + off, msg.info, info_len);
        slot.line[off + (int)info_len] = '\0';

        if (s_rx_lock == NULL) continue;
        xSemaphoreTake(s_rx_lock, portMAX_DELAY);
        const uint8_t next = (s_rx_head + 1) % RX_FRAME_SLOTS;
        if (next != s_rx_tail) {
            s_rx_frames[s_rx_head] = slot;
            s_rx_head = next;
        } else {
            ESP_LOGW(TAG, "RX frame queue full; dropping");
        }
        xSemaphoreGive(s_rx_lock);
        ESP_LOGI(TAG, "%s RX: %s",
                 rx_source == APRS_AFSK_SOURCE_NRL ? "NRL" : "RF", slot.line);
    }
}

/* ------------------------------------------------------------------ */
/* TX: pump and NRL G.711 fan-out                                      */
/* ------------------------------------------------------------------ */

static int16_t s_nrl_tx_buf[G711_FRAME_SAMPLES];
static size_t s_nrl_tx_fill;
static bool s_tx_pumping;
static int64_t s_tx_start_us;
static uint32_t s_tx_pushed_samples;

static void nrl_tx_flush(void)
{
    if (s_nrl_tx_fill == 0) return;
    uint8_t payload[G711_FRAME_SAMPLES];
    for (size_t i = 0; i < s_nrl_tx_fill; ++i) {
        payload[i] = nrl_g711_encode_alaw(s_nrl_tx_buf[i]);
    }
    (void)nrl_link_send(NRL_PACKET_TYPE_VOICE, payload, s_nrl_tx_fill);
    s_nrl_tx_fill = 0;
}

static void nrl_tx_push(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        s_nrl_tx_buf[s_nrl_tx_fill++] = samples[i];
        if (s_nrl_tx_fill == G711_FRAME_SAMPLES) nrl_tx_flush();
    }
}

/* Local 8k -> 16k linear upsampler so we can hand the speaker queue
 * 16 kHz frames directly (avoids its shared static upsample buffer). */
static void tx_pump(void)
{
    static int16_t chunk[TX_CHUNK_SAMPLES];
    static int16_t up[TX_CHUNK_SAMPLES * 2];

    if (!ModemIsTransmitting()) {
        if (s_tx_pumping) {
            nrl_tx_flush();
            s_tx_active = false;
            ESP_LOGI(TAG, "TX done");
        }
        s_tx_pumping = false;
        return;
    }
    if (!s_tx_pumping) {
        s_tx_pumping = true;
        s_tx_active = true;
        s_tx_start_us = esp_timer_get_time();
        s_tx_pushed_samples = 0;
        s_nrl_tx_fill = 0;
        net_radio_notify_nrl_voice();  /* preempt net-radio playback */
        ESP_LOGI(TAG, "TX start (rf=%d nrl=%d)", s_tx_rf ? 1 : 0,
                 s_tx_nrl ? 1 : 0);
    }
    /* Keep ~200 ms of samples ahead of wall clock; the speaker queue
     * absorbs the lead. */
    const int64_t elapsed_us = esp_timer_get_time() - s_tx_start_us;
    const uint32_t target =
        (uint32_t)(elapsed_us * MODEM_TX_SAMPLE_RATE / 1000000) +
        MODEM_TX_SAMPLE_RATE / 5;
    while (s_tx_pushed_samples < target && ModemIsTransmitting()) {
        size_t n = TX_CHUNK_SAMPLES;
        for (size_t i = 0; i < TX_CHUNK_SAMPLES; ++i) {
            const uint8_t s = MODEM_BAUDRATE_TIMER_HANDLER();
            chunk[i] = (int16_t)(((int)s - 128) * TX_AMPLITUDE);
            if (!ModemIsTransmitting()) {
                for (size_t k = i + 1; k < TX_CHUNK_SAMPLES; ++k) {
                    chunk[k] = 0;
                }
                break;
            }
        }
        if (s_tx_rf) {
            for (size_t i = 0; i < n; ++i) {
                const int16_t cur = chunk[i];
                const int16_t next =
                    (i + 1 < n) ? chunk[i + 1] : cur;
                up[i * 2] = cur;
                up[i * 2 + 1] =
                    (int16_t)(((int32_t)cur + (int32_t)next) / 2);
            }
            audio_passthrough_queue_output(up, n * 2, 16000);
        }
        if (s_tx_nrl) nrl_tx_push(chunk, n);
        s_tx_pushed_samples += (uint32_t)n;
        if (!ModemIsTransmitting()) break;
    }
}

/* ------------------------------------------------------------------ */
/* AFSK task                                                           */
/* ------------------------------------------------------------------ */

static void modem_ptt(bool on)
{
    /* VOX keying only (same as NRL-ESP32); the flag parks the mic
     * uplink so the modem audio owns the NRL voice accumulator. */
    ESP_LOGI(TAG, "PTT %s", on ? "on" : "off");
}

static void afsk_task(void *argument)
{
    (void)argument;
    for (;;) {
        /* RX: resample + demod both taps */
        if (s_rx_rf) ring_demod(&s_rf_ring, 0);
        if (s_rx_nrl) ring_demod(&s_nrl_ring, 1);

        /* TX: encode one pending line per loop when the buffer has room */
        if (s_tx_lock != NULL && s_tx_tail != s_tx_head) {
            char line[TX_LINE_MAX];
            xSemaphoreTake(s_tx_lock, portMAX_DELAY);
            memcpy(line, s_tx_lines[s_tx_tail], TX_LINE_MAX);
            s_tx_tail = (uint8_t)((s_tx_tail + 1) % TX_QUEUE_SLOTS);
            xSemaphoreGive(s_tx_lock);
            ax25frame frame;
            if (ax25_encode(frame, line, (int)strlen(line))) {
                AX25Ctx ctx;
                static uint8_t raw[AX25_FRAME_MAX_SIZE];
                const int size = hdlcFrame(raw, sizeof(raw), &ctx, &frame);
                if (size > 0 &&
                    Ax25WriteTxFrame(raw, (uint16_t)size) != NULL) {
                    Ax25TransmitBuffer();
                } else {
                    ESP_LOGW(TAG, "TX frame encode/queue failed");
                }
            } else {
                ESP_LOGW(TAG, "ax25_encode failed: %.60s", line);
            }
        }

        Ax25TransmitCheck();
        tx_pump();
        rx_frame_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t aprs_afsk_init(void)
{
    if (s_task != NULL) return ESP_OK;

    s_rf_ring.buf = (int16_t *)heap_caps_malloc(
        RF_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_nrl_ring.buf = (int16_t *)heap_caps_malloc(
        NRL_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_rf_ring.buf == NULL || s_nrl_ring.buf == NULL) {
        ESP_LOGE(TAG, "RX ring allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_rf_ring.size = RF_RING_SAMPLES;
    s_nrl_ring.size = NRL_RING_SAMPLES;
    s_rf_ring.rate = 16000;
    s_nrl_ring.rate = 8000;

    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL || s_rx_lock == NULL) return ESP_ERR_NO_MEM;

    ModemConfig.modem = MODEM_1200;
    ModemConfig.flatAudioIn = 0;
    ModemInit();
    Ax25Init(0);
    ModemSetPttCallback(modem_ptt);

    if (xTaskCreateStaticPinnedToCore(
            afsk_task, "aprs_afsk",
            sizeof(s_task_stack) / sizeof(s_task_stack[0]), NULL, 5,
            s_task_stack, &s_task_tcb, 0) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFSK modem ready: 1200 Bd, RX demods 2, TX 8 kHz");
    return ESP_OK;
}

void aprs_afsk_set_rx_routes(bool rf_rx, bool nrl_rx)
{
    s_rx_rf = rf_rx;
    s_rx_nrl = nrl_rx;
}

void aprs_afsk_set_tx_routes(bool rf_tx, bool nrl_tx)
{
    s_tx_rf = rf_tx;
    s_tx_nrl = nrl_tx;
}

bool aprs_afsk_send_line(const char *tnc2)
{
    if (tnc2 == NULL || tnc2[0] == '\0' || s_tx_lock == NULL) return false;
    if (!s_tx_rf && !s_tx_nrl) return false;
    bool queued = false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const uint8_t next = (uint8_t)((s_tx_head + 1) % TX_QUEUE_SLOTS);
    if (next != s_tx_tail) {
        strlcpy(s_tx_lines[s_tx_head], tnc2, TX_LINE_MAX);
        s_tx_head = next;
        queued = true;
    }
    xSemaphoreGive(s_tx_lock);
    if (!queued) ESP_LOGW(TAG, "TX line queue full");
    return queued;
}

bool aprs_afsk_get_rx_frame(char *line, size_t size, uint8_t *source)
{
    if (line == NULL || size == 0 || s_rx_lock == NULL) return false;
    bool got = false;
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if (s_rx_tail != s_rx_head) {
        strlcpy(line, s_rx_frames[s_rx_tail].line, size);
        if (source != NULL) *source = s_rx_frames[s_rx_tail].source;
        s_rx_tail = (uint8_t)((s_rx_tail + 1) % RX_FRAME_SLOTS);
        got = true;
    }
    xSemaphoreGive(s_rx_lock);
    return got;
}

bool aprs_afsk_is_tx_active(void)
{
    return s_tx_active;
}
