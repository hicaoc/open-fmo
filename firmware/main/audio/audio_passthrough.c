#include "audio_passthrough.h"

#include <string.h>

#include "app/driver/audio_bus.h"
#include "app/driver/es8311_codec.h"
#include "app/driver/status_io.h"
#include "audio/nrl_audio_codec.h"
#include "audio/nrl_g711.h"
#include "audio/aprs_afsk.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "services/espnow_link.h"
#include "services/config_store.h"
#include "services/fmo_link.h"

static const char *TAG = "audio_pt";

#define SAMPLE_RATE      16000U
#define FRAME_SAMPLES    160U    /* 10 ms at 16 kHz */
#define I2S_SLOTS        2U
#define FRAME_BYTES      (FRAME_SAMPLES * sizeof(int16_t))
#define I2S_FRAME_BYTES  (FRAME_SAMPLES * I2S_SLOTS * sizeof(int16_t))
#define I2S_WAIT_MS      20U

/* Output queue: ~1.28 s of 16 kHz audio */
#define OUTPUT_QUEUE_SAMPLES  (FRAME_SAMPLES * 128U)
/* Prime threshold: 60 ms before starting playback (anti-jitter) */
#define OUTPUT_PRIME_SAMPLES  (FRAME_SAMPLES * 6U)

/* Mic uplink interval: send every 20 ms (320 samples at 16k for Opus) */
#define OPUS_FRAME_SAMPLES 320U
/* G.711 frame: 20 ms at 8 kHz = 160 samples */
#define G711_FRAME_SAMPLES 160U

static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[32768 / sizeof(StackType_t)];
static volatile bool s_running;
static volatile bool s_task_exited;

/* Circular output queue – keep in internal RAM for audio coherency */
static DRAM_ATTR int16_t s_queue[OUTPUT_QUEUE_SAMPLES];
static size_t s_queue_head;
static size_t s_queue_tail;
static size_t s_queue_count;
static bool s_queue_playing;
/* CPU-only FMO jitter buffer; I2S DMA never accesses this queue. */
static EXT_RAM_BSS_ATTR int16_t s_fmo_queue[OUTPUT_QUEUE_SAMPLES];
static size_t s_fmo_head;
static size_t s_fmo_tail;
static size_t s_fmo_count;
static bool s_fmo_playing;
static int64_t s_queue_first_us;
static int64_t s_fmo_first_us;
static SemaphoreHandle_t s_queue_mutex;

typedef struct {
    char callsign[16];
    char codec[8];
    int64_t started_us;
    int64_t last_us;
} voice_meta_t;

static voice_meta_t s_voice_meta[2];
static volatile uint8_t s_audio_policy;
static int s_first_primary = -1;
static int64_t s_primary_last_us;

/* Mic accumulation buffer for uplink */
static DRAM_ATTR int16_t s_mic_buf[OPUS_FRAME_SAMPLES];
static size_t s_mic_fill;

/* Software mic amplification (1 = off, up to 5x) */
static volatile uint8_t s_mic_gain = 1;

/* TX voice codec: 0=G.711 8kHz (default), 1=Opus 16kHz */
static volatile uint8_t s_voice_codec;  /* 0 or 1 */
static volatile uint8_t s_tx_network;   /* 0=NRL, 1=FMO */
static bool s_sql_was_active;
static bool s_fmo_tx_started;

/* ------------------------------------------------------------------ */
/* Output queue helpers                                                */
/* ------------------------------------------------------------------ */

static void queue_clear_locked(void)
{
    s_queue_head = 0;
    s_queue_tail = 0;
    s_queue_count = 0;
    s_queue_playing = false;
    s_fmo_head = 0;
    s_fmo_tail = 0;
    s_fmo_count = 0;
    s_fmo_playing = false;
    s_queue_first_us = 0;
    s_fmo_first_us = 0;
    s_first_primary = -1;
}

static size_t queue_push_source(const int16_t *samples, size_t count,
                                bool fmo)
{
    if (samples == NULL || count == 0) return 0;
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    size_t written = 0;
    size_t *queued = fmo ? &s_fmo_count : &s_queue_count;
    size_t *tail = fmo ? &s_fmo_tail : &s_queue_tail;
    int16_t *queue = fmo ? s_fmo_queue : s_queue;
    int64_t *first = fmo ? &s_fmo_first_us : &s_queue_first_us;
    if (*queued == 0) *first = esp_timer_get_time();
    while (written < count && *queued < OUTPUT_QUEUE_SAMPLES) {
        queue[*tail] = samples[written++];
        *tail = (*tail + 1) % OUTPUT_QUEUE_SAMPLES;
        ++*queued;
    }
    xSemaphoreGive(s_queue_mutex);
    return written;
}

static size_t pop_one_locked(bool fmo, int16_t *dst, size_t count)
{
    size_t *queued = fmo ? &s_fmo_count : &s_queue_count;
    size_t *head = fmo ? &s_fmo_head : &s_queue_head;
    bool *playing = fmo ? &s_fmo_playing : &s_queue_playing;
    int16_t *queue = fmo ? s_fmo_queue : s_queue;
    if (!*playing) {
        if (*queued < OUTPUT_PRIME_SAMPLES) {
            memset(dst, 0, count * sizeof(*dst));
            return 0;
        }
        *playing = true;
    }
    size_t read = 0;
    while (read < count && *queued > 0) {
        dst[read++] = queue[*head];
        *head = (*head + 1) % OUTPUT_QUEUE_SAMPLES;
        --*queued;
    }
    if (read < count) *playing = false;
    if (read < count) memset(dst + read, 0, (count - read) * sizeof(*dst));
    return read;
}

static void discard_source_locked(bool fmo)
{
    if (fmo) {
        s_fmo_head = s_fmo_tail = s_fmo_count = 0;
        s_fmo_playing = false;
    } else {
        s_queue_head = s_queue_tail = s_queue_count = 0;
        s_queue_playing = false;
    }
}

static void refresh_voice_locked(int64_t now)
{
    bool any = false;
    for (size_t i = 0; i < 2; ++i) {
        if (s_voice_meta[i].last_us != 0 &&
            now - s_voice_meta[i].last_us > 900000) {
            memset(&s_voice_meta[i], 0, sizeof(s_voice_meta[i]));
        }
        any |= s_voice_meta[i].last_us != 0;
    }
    status_io_set_network_ptt(any);
}

static size_t queue_pop(int16_t *dst, size_t count)
{
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        memset(dst, 0, count * sizeof(int16_t));
        return 0;
    }
    int64_t now = esp_timer_get_time();
    refresh_voice_locked(now);
    static int16_t other[FRAME_SAMPLES];
    size_t read = 0;
    if (s_audio_policy == 0) {
        size_t nrl_read = pop_one_locked(false, dst, count);
        size_t fmo_read = pop_one_locked(true, other, count);
        read = nrl_read > fmo_read ? nrl_read : fmo_read;
        if (nrl_read > 0 && fmo_read > 0) {
            for (size_t i = 0; i < count; ++i) {
                int32_t mixed = ((int32_t)dst[i] + other[i]) / 2;
                dst[i] = (int16_t)mixed;
            }
        } else if (fmo_read > 0) {
            memcpy(dst, other, count * sizeof(*dst));
        }
    } else {
        bool nrl_ready = s_queue_playing ||
                         s_queue_count >= OUTPUT_PRIME_SAMPLES;
        bool fmo_ready = s_fmo_playing ||
                         s_fmo_count >= OUTPUT_PRIME_SAMPLES;
        if (s_first_primary < 0 && (nrl_ready || fmo_ready)) {
            s_first_primary = nrl_ready && fmo_ready
                ? (s_queue_first_us <= s_fmo_first_us ? 0 : 1)
                : (nrl_ready ? 0 : 1);
        }
        if (s_first_primary == 0) {
            read = pop_one_locked(false, dst, count);
            if (s_fmo_count > 0) discard_source_locked(true);
        } else if (s_first_primary == 1) {
            read = pop_one_locked(true, dst, count);
            if (s_queue_count > 0) discard_source_locked(false);
        } else {
            memset(dst, 0, count * sizeof(*dst));
        }
        if (read > 0) {
            s_primary_last_us = now;
        } else if (s_first_primary >= 0 &&
                   now - s_primary_last_us > 300000) {
            s_first_primary = -1;
        }
    }
    xSemaphoreGive(s_queue_mutex);
    if (read < count) memset(dst + read, 0, (count - read) * sizeof(*dst));
    return read;
}

/* ------------------------------------------------------------------ */
/* I2S frame read/write (stereo interleaved)                           */
/* ------------------------------------------------------------------ */

static bool i2s_read_mono(int16_t *dst)
{
    i2s_chan_handle_t rx = NULL;
    if (audio_bus_get_channels(NULL, &rx) != ESP_OK || rx == NULL) return false;

    static DRAM_ATTR int16_t raw[FRAME_SAMPLES * I2S_SLOTS];
    size_t total = 0;
    while (total < I2S_FRAME_BYTES) {
        size_t got = 0;
        if (i2s_channel_read(rx, (uint8_t *)raw + total,
                             I2S_FRAME_BYTES - total, &got,
                             I2S_WAIT_MS) != ESP_OK) {
            return false;
        }
        if (got == 0) { vTaskDelay(1); continue; }
        total += got;
    }
    /* Extract LEFT slot (mono mic) */
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
        dst[i] = raw[i * 2];
    }
    return true;
}

static bool i2s_write_mono(const int16_t *src)
{
    i2s_chan_handle_t tx = NULL;
    if (audio_bus_get_channels(&tx, NULL) != ESP_OK || tx == NULL) return false;

    static DRAM_ATTR int16_t raw[FRAME_SAMPLES * I2S_SLOTS];
    /* Duplicate mono into both slots */
    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
        raw[i * 2]     = src[i];
        raw[i * 2 + 1] = src[i];
    }
    size_t total = 0;
    while (total < I2S_FRAME_BYTES) {
        size_t put = 0;
        if (i2s_channel_write(tx, (const uint8_t *)raw + total,
                              I2S_FRAME_BYTES - total, &put,
                              I2S_WAIT_MS) != ESP_OK) {
            return false;
        }
        if (put == 0) { vTaskDelay(1); continue; }
        total += put;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Mic uplink: reference project approach                              */
/*   Opus mode:  accumulate 320 samples @16kHz → Opus encode          */
/*   G.711 mode: pair-average downsample 16k→8k, accumulate 160 @8kHz */
/*               → G.711 A-law encode                                 */
/* ------------------------------------------------------------------ */

static void mic_uplink_feed(const int16_t *frame, size_t count)
{
    if (s_voice_codec == 1u) {
        /* Opus 16kHz: accumulate 320 samples then encode */
        size_t offset = 0;
        while (offset < count) {
            size_t space = OPUS_FRAME_SAMPLES - s_mic_fill;
            size_t take = count - offset < space ? count - offset : space;
            memcpy(s_mic_buf + s_mic_fill, frame + offset, take * sizeof(int16_t));
            s_mic_fill += take;
            offset += take;
            if (s_mic_fill == OPUS_FRAME_SAMPLES) {
                nrl_audio_send_opus(s_mic_buf, OPUS_FRAME_SAMPLES);
                if (espnow_link_is_enabled()) {
                    espnow_link_send_opus(s_mic_buf, OPUS_FRAME_SAMPLES);
                }
                s_mic_fill = 0;
            }
        }
    } else {
        /* G.711 8kHz: pair-average downsample 16k→8k, then accumulate.
         * Reference project approach: average consecutive sample pairs. */
        size_t offset = 0;
        while (offset < count) {
            size_t remaining = count - offset;
            size_t pairs = remaining / 2u;
            for (size_t i = 0; i < pairs; ++i) {
                int32_t a = frame[offset + i * 2u];
                int32_t b = frame[offset + i * 2u + 1u];
                s_mic_buf[s_mic_fill++] = (int16_t)((a + b) / 2);
                if (s_mic_fill == G711_FRAME_SAMPLES) {
                    nrl_audio_send_g711(s_mic_buf, G711_FRAME_SAMPLES);
                    if (espnow_link_is_enabled()) {
                        espnow_link_send_g711(s_mic_buf, G711_FRAME_SAMPLES);
                    }
                    s_mic_fill = 0;
                }
            }
            offset += pairs * 2u;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Software DC removal (first-order IIR high-pass)                     */
/* Removes DC bias from BK4802 AOUT before VU/uplink processing.      */
/* ------------------------------------------------------------------ */

static int32_t s_dc_est;  /* DC estimate (Q16.16 fixed-point) */

static inline void dc_remove(int16_t *buf, size_t count)
{
    /* alpha = 0.995 → fc ≈ 12.7 Hz at 16 kHz (passes CTCSS 67+ Hz) */
    for (size_t i = 0; i < count; ++i) {
        int32_t x = buf[i];
        /* dc_est += alpha * (x - dc_est), approximated as:
         * dc_est = dc_est + (x - dc_est) / 200 */
        s_dc_est += (x - (s_dc_est >> 16)) * 328;  /* 328/65536 ≈ 1/200 */
        buf[i] = (int16_t)(x - (s_dc_est >> 16));
    }
}

/* ------------------------------------------------------------------ */
/* Passthrough task                                                    */
/* ------------------------------------------------------------------ */

static void passthrough_task(void *arg)
{
    (void)arg;
    static int16_t mic_frame[FRAME_SAMPLES];
    static int16_t playback_frame[FRAME_SAMPLES];
    uint32_t vu_counter = 0;
    uint32_t loop_count = 0;
    uint32_t read_fail = 0;
    s_dc_est = 0;

    ESP_LOGI(TAG, "task entered, I2S read test...");
    while (s_running) {
        if (!i2s_read_mono(mic_frame)) {
            if (++read_fail <= 3) {
                ESP_LOGW(TAG, "i2s_read_mono failed (%lu times)",
                         (unsigned long)read_fail);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        /* Remove DC bias from BK4802 AOUT */
        dc_remove(mic_frame, FRAME_SAMPLES);

        /* Software mic amplification before every consumer (AFSK tap,
         * uplink encode, VU meter). Saturating multiply. */
        const uint8_t gain = s_mic_gain;
        if (gain > 1u) {
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
                int32_t v = (int32_t)mic_frame[i] * gain;
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                mic_frame[i] = (int16_t)v;
            }
        }

        if (loop_count == 0) {
            int32_t peak_l = 0, peak_r = 0;
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
                int32_t vl = mic_frame[i] < 0 ? -mic_frame[i] : mic_frame[i];
                if (vl > peak_l) peak_l = vl;
            }
            /* Also check RIGHT slot from raw I2S data */
            static int16_t raw_diag[FRAME_SAMPLES * 2];
            i2s_chan_handle_t rx2 = NULL;
            if (audio_bus_get_channels(NULL, &rx2) == ESP_OK && rx2) {
                size_t got2 = 0;
                i2s_channel_read(rx2, raw_diag, sizeof(raw_diag), &got2, 20);
                for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
                    int32_t vr = raw_diag[i * 2 + 1];
                    vr = vr < 0 ? -vr : vr;
                    if (vr > peak_r) peak_r = vr;
                }
            }
            ESP_LOGI(TAG, "I2S slots: LEFT peak=%ld, RIGHT peak=%ld, raw[0..3]=%d %d %d %d",
                     (long)peak_l, (long)peak_r,
                     mic_frame[0], mic_frame[1], mic_frame[2], mic_frame[3]);
        }

        /* APRS RF tap: decode AFSK from the radio mic audio. Parked while
         * we transmit so our own frame cannot be re-demodulated. */
        if (!aprs_afsk_is_tx_active()) {
            aprs_afsk_feed_rf(mic_frame, FRAME_SAMPLES, SAMPLE_RATE);
        }

        /* Uplink: feed mic to NRL when raw RF squelch is open.
         * CTCSS detection happens inside nrl_audio_send_*() which gates
         * the actual network transmission based on tone match.
         * While an AFSK frame is on air the mic uplink is parked so the
         * modem audio cannot loop into the NRL voice channel. */
        if (status_io_is_raw_sql_active()) {
            if (!s_sql_was_active) {
                ESP_LOGI(TAG, "SQL open, feeding %s uplink",
                         s_tx_network == 1 ? "FMO Opus" :
                         (s_voice_codec == 1 ? "NRL Opus" : "NRL G711"));
                s_sql_was_active = true;
                if (s_tx_network == 1) {
                    s_fmo_tx_started = fmo_link_tx_begin();
                }
            }
            if (aprs_afsk_is_tx_active()) {
                s_mic_fill = 0;
                if (s_tx_network == 1 && s_fmo_tx_started) {
                    fmo_link_tx_end();
                    s_fmo_tx_started = false;
                }
            } else {
                if (s_tx_network == 1) {
                    if (!s_fmo_tx_started) {
                        s_fmo_tx_started = fmo_link_tx_begin();
                    }
                    if (nrl_audio_radio_rx_accept(
                            mic_frame, FRAME_SAMPLES, SAMPLE_RATE)) {
                        if (s_fmo_tx_started &&
                            !fmo_link_tx_feed_pcm16(mic_frame,
                                                    FRAME_SAMPLES)) {
                            s_fmo_tx_started = false;
                        }
                    }
                } else {
                    mic_uplink_feed(mic_frame, FRAME_SAMPLES);
                }
            }
        } else {
            if (s_sql_was_active) {
                ESP_LOGI(TAG, "SQL closed, uplink idle");
                if (s_tx_network == 1 && s_fmo_tx_started) {
                    fmo_link_tx_end();
                    s_fmo_tx_started = false;
                }
                s_sql_was_active = false;
            }
            s_mic_fill = 0;  /* discard partial frame on gate close */
        }

        /* Downlink: pop from output queue → speaker */
        queue_pop(playback_frame, FRAME_SAMPLES);
        if (!i2s_write_mono(playback_frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* VU meter: update every 5 frames (~50 ms) */
        if (++vu_counter >= 5u) {
            vu_counter = 0;
            loop_count++;
            int32_t mic_peak = 0, spk_peak = 0;
            for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
                int32_t m = mic_frame[i] < 0 ? -mic_frame[i] : mic_frame[i];
                int32_t s = playback_frame[i] < 0 ? -playback_frame[i] : playback_frame[i];
                if (m > mic_peak) mic_peak = m;
                if (s > spk_peak) spk_peak = s;
            }
            /* Scale 0-32767 → 0-255 */
            status_io_set_vu(
                (uint8_t)(mic_peak > 32767 ? 255 : (mic_peak * 255) / 32767),
                (uint8_t)(spk_peak > 32767 ? 255 : (spk_peak * 255) / 32767));
        }
        vTaskDelay(1);
    }

    s_task_exited = true;
    vTaskSuspend(NULL);
}

/* ------------------------------------------------------------------ */
/* Sink callback: receives decoded network audio from nrl_audio_codec  */
/* ------------------------------------------------------------------ */

static void speaker_sink(const int16_t *samples, size_t sample_count,
                         uint32_t sample_rate, void *context)
{
    (void)context;
    audio_passthrough_queue_output(samples, sample_count, sample_rate);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

size_t audio_passthrough_queue_output(const int16_t *samples,
                                      size_t sample_count,
                                      uint32_t sample_rate)
{
    if (samples == NULL || sample_count == 0) return 0;

    if (sample_rate == SAMPLE_RATE) {
        return queue_push_source(samples, sample_count, false);
    }

    if (sample_rate == 8000) {
        /* Upsample 8k → 16k: linear interpolation (2x) */
        static int16_t up_buf[FRAME_SAMPLES * 4];
        size_t up_count = sample_count * 2;
        if (up_count > sizeof(up_buf) / sizeof(up_buf[0])) {
            up_count = sizeof(up_buf) / sizeof(up_buf[0]);
            sample_count = up_count / 2;
        }
        for (size_t i = 0; i < sample_count; ++i) {
            int16_t cur = samples[i];
            int16_t next = (i + 1 < sample_count) ? samples[i + 1] : cur;
            up_buf[i * 2]     = cur;
            up_buf[i * 2 + 1] = (int16_t)(((int32_t)cur + (int32_t)next) / 2);
        }
        return queue_push_source(up_buf, up_count, false);
    }

    /* Unsupported rate: discard */
    return 0;
}

static size_t queue_fmo_resampled(const int16_t *samples, size_t sample_count,
                                  uint32_t sample_rate)
{
    if (samples == NULL || sample_count == 0) return 0;
    if (sample_rate == SAMPLE_RATE) {
        return queue_push_source(samples, sample_count, true);
    }
    if (sample_rate == 8000) {
        int16_t up_buf[FRAME_SAMPLES * 8];
        size_t consumed = 0, written = 0;
        while (consumed < sample_count) {
            size_t part = sample_count - consumed;
            if (part > sizeof(up_buf) / sizeof(up_buf[0]) / 2) {
                part = sizeof(up_buf) / sizeof(up_buf[0]) / 2;
            }
            for (size_t i = 0; i < part; ++i) {
                int16_t cur = samples[consumed + i];
                int16_t next = consumed + i + 1 < sample_count
                    ? samples[consumed + i + 1] : cur;
                up_buf[i * 2] = cur;
                up_buf[i * 2 + 1] =
                    (int16_t)(((int32_t)cur + next) / 2);
            }
            written += queue_push_source(up_buf, part * 2, true);
            consumed += part;
        }
        return written;
    }
    return 0;
}

size_t audio_passthrough_queue_fmo_output(const int16_t *samples,
                                          size_t sample_count,
                                          uint32_t sample_rate)
{
    return queue_fmo_resampled(samples, sample_count, sample_rate);
}

void audio_passthrough_note_network_voice(audio_network_source_t source,
                                          const char *callsign,
                                          const char *codec)
{
    if (source > AUDIO_NETWORK_FMO || s_queue_mutex == NULL) return;
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    int64_t now = esp_timer_get_time();
    voice_meta_t *meta = &s_voice_meta[source];
    if (meta->last_us == 0 || now - meta->last_us > 900000) {
        meta->started_us = now;
    }
    meta->last_us = now;
    snprintf(meta->callsign, sizeof(meta->callsign), "%s",
             callsign != NULL ? callsign : "");
    snprintf(meta->codec, sizeof(meta->codec), "%s",
             codec != NULL ? codec : "");
    refresh_voice_locked(now);
    xSemaphoreGive(s_queue_mutex);
}

void audio_passthrough_get_network_status(audio_network_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (s_queue_mutex == NULL ||
        xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    int64_t now = esp_timer_get_time();
    refresh_voice_locked(now);
    status->nrl_active = s_voice_meta[AUDIO_NETWORK_NRL].last_us != 0;
    status->fmo_active = s_voice_meta[AUDIO_NETWORK_FMO].last_us != 0;
    snprintf(status->nrl_callsign, sizeof(status->nrl_callsign), "%s",
             s_voice_meta[AUDIO_NETWORK_NRL].callsign);
    snprintf(status->nrl_codec, sizeof(status->nrl_codec), "%s",
             s_voice_meta[AUDIO_NETWORK_NRL].codec);
    snprintf(status->fmo_callsign, sizeof(status->fmo_callsign), "%s",
             s_voice_meta[AUDIO_NETWORK_FMO].callsign);
    snprintf(status->fmo_codec, sizeof(status->fmo_codec), "%s",
             s_voice_meta[AUDIO_NETWORK_FMO].codec);
    status->primary = status->fmo_active &&
                      (!status->nrl_active ||
                       s_voice_meta[AUDIO_NETWORK_FMO].started_us <
                       s_voice_meta[AUDIO_NETWORK_NRL].started_us)
        ? AUDIO_NETWORK_FMO : AUDIO_NETWORK_NRL;
    xSemaphoreGive(s_queue_mutex);
}

void audio_passthrough_set_audio_policy(uint8_t policy)
{
    s_audio_policy = policy == 1 ? 1 : 0;
}

void audio_passthrough_set_tx_network(uint8_t network)
{
    if (s_sql_was_active && s_tx_network == 1 && network != 1) {
        fmo_link_tx_end();
        s_fmo_tx_started = false;
    }
    s_tx_network = network == 1 ? 1 : 0;
}

void audio_passthrough_clear_output(void)
{
    if (s_queue_mutex == NULL) return;
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    queue_clear_locked();
    xSemaphoreGive(s_queue_mutex);
}

esp_err_t audio_passthrough_start(void)
{
    if (s_task != NULL) return ESP_OK;

    /* Step 1: Start I2S bus (MCLK begins running) */
    esp_err_t err = audio_bus_init(SAMPLE_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Step 2: Configure ES8311 registers (needs MCLK) */
    err = es8311_codec_configure();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 configure failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Allow ADC analog path to settle after power-up */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Apply saved ES8311 volume (defaults to max) */
    {
        fmo_config_t cfg;
        config_store_load(&cfg);
        es8311_codec_set_dac_volume(cfg.es8311_dac_vol);
        es8311_codec_set_adc_volume(cfg.es8311_adc_vol);
    }

    /* Step 3: Create output queue mutex */
    if (s_queue_mutex == NULL) {
        s_queue_mutex = xSemaphoreCreateMutex();
        if (s_queue_mutex == NULL) {
            ESP_LOGE(TAG, "mutex alloc failed (internal RAM exhausted)");
            return ESP_ERR_NO_MEM;
        }
    }
    queue_clear_locked();
    s_mic_fill = 0;

    /* Step 4: Register as the speaker sink for decoded network audio */
    nrl_audio_codec_set_sink(speaker_sink, NULL);

    /* Step 5: Start passthrough task – stack is static BSS (auto PSRAM) */
    s_running = true;
    s_task_exited = false;
    s_task = xTaskCreateStaticPinnedToCore(
        passthrough_task, "audio_pt",
        sizeof(s_task_stack) / sizeof(s_task_stack[0]),
        NULL, 10, s_task_stack, &s_task_tcb, 1);
    if (s_task == NULL) {
        ESP_LOGE(TAG, "static task create failed");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started: %uHz/16bit/stereo, frame=%u samples",
             (unsigned)SAMPLE_RATE, (unsigned)FRAME_SAMPLES);
    return ESP_OK;
}

void audio_passthrough_stop(void)
{
    if (s_task == NULL) return;
    s_running = false;
    for (int i = 0; i < 50 && !s_task_exited; ++i) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_task_exited = false;
    audio_bus_deinit();
    ESP_LOGI(TAG, "stopped");
}

bool audio_passthrough_is_running(void)
{
    return s_task != NULL;
}

void audio_passthrough_set_voice_codec(uint8_t codec)
{
    s_voice_codec = (codec <= 1u) ? codec : 0u;
    ESP_LOGI(TAG, "TX voice codec: %s", s_voice_codec ? "Opus 16kHz" : "G.711 8kHz");
}

void audio_passthrough_set_mic_gain(uint8_t gain)
{
    if (gain < 1u) gain = 1u;
    if (gain > 5u) gain = 5u;
    s_mic_gain = gain;
    ESP_LOGI(TAG, "software mic gain: %ux", (unsigned)gain);
}

uint8_t audio_passthrough_get_mic_gain(void)
{
    return s_mic_gain;
}

uint8_t audio_passthrough_get_voice_codec(void)
{
    return s_voice_codec;
}
