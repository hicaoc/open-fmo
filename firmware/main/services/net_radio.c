#include "net_radio.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_passthrough.h"
#include "app/driver/status_io.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "esp_flac_dec.h"
#include "esp_mp3_dec.h"

static const char *TAG = "net_radio";

/* ------------------------------------------------------------------ */
/* Station list (NVS namespace "netradio")                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[NET_RADIO_NAME_MAX];
    char url[NET_RADIO_URL_MAX];
} station_entry_t;

static SemaphoreHandle_t s_lock;
static station_entry_t s_entries[NET_RADIO_STATION_MAX];
static size_t s_count;
static int s_current = -1;

static const char *k_nvs_namespace = "netradio";

static void list_lock(void)
{
    if (s_lock != NULL) (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void list_unlock(void)
{
    if (s_lock != NULL) (void)xSemaphoreGive(s_lock);
}

static bool url_valid(const char *url)
{
    return url != NULL &&
           (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) &&
           strlen(url) < NET_RADIO_URL_MAX;
}

/* Persist under the lock: count/current ride alongside the blob so a partial
 * write can't pair a stale count with new entries. */
static void save_locked(void)
{
    nvs_handle_t nvs;
    if (nvs_open(k_nvs_namespace, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "station list persist failed");
        return;
    }
    (void)nvs_set_u8(nvs, "count", (uint8_t)s_count);
    (void)nvs_set_i8(nvs, "cur", (int8_t)s_current);
    (void)nvs_set_blob(nvs, "list", s_entries, s_count * sizeof(station_entry_t));
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

/* ------------------------------------------------------------------ */
/* Player state                                                        */
/* ------------------------------------------------------------------ */

/* Command channel to the player task: -1 = none, -2 = stop, >=0 = play index.
 * The player polls this between stream reads, so stop/switch lands < 1 s. */
#define CMD_NONE (-1)
#define CMD_STOP (-2)
static volatile int s_cmd = CMD_NONE;

static volatile net_radio_state_t s_state = NET_RADIO_STATE_IDLE;
static char s_error[48];
static TaskHandle_t s_task;

/* While playing, the radio PTT is keyed the same way NRL downlink voice
 * does: on this hardware the speaker path is PTT-gated, so playback is
 * only audible (gateway speaker + RF) with the transmitter keyed.
 * s_ptt_held tracks ownership so we only release what we keyed; on voice
 * preemption the voice path re-keys within one packet, so always releasing
 * here is safe (worst case a brief TX dropout, never a stuck transmitter). */
static bool s_ptt_held;

/* Player task stack lives in PSRAM (static): internal DRAM/heap is nearly
 * full in this project, and SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY does not
 * redirect xTaskCreate stacks here (PLACE_TASK_STACKS_IN_EXT_RAM is off).
 * The player task never touches the flash cache-disabled path, so a PSRAM
 * stack is safe. */
#define PLAYER_STACK_BYTES 24576
static EXT_RAM_BSS_ATTR StackType_t s_player_stack[PLAYER_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_player_tcb;

/* Buffers are forced into PSRAM (.ext_ram.bss): internal DRAM is nearly
 * full in this project. All are written before first use, so the lack of
 * zero-init in external BSS does not matter. */
#define RAW_BYTES     8192
#define OUT_BYTES     32768   /* one decoded frame: FLAC 8192 samples*stereo */
#define MONO_SAMPLES  8192
#define OUT16K_SAMPLES 4096

static EXT_RAM_BSS_ATTR uint8_t s_raw[RAW_BYTES];
static EXT_RAM_BSS_ATTR uint8_t s_out[OUT_BYTES];
static EXT_RAM_BSS_ATTR int16_t s_mono[MONO_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t s_out16k[OUT16K_SAMPLES];

/* ------------------------------------------------------------------ */
/* Stream ring buffer (PSRAM): decouples the network read pace from   */
/* the real-time decode/output pace. 512 KB holds ~32 s at 128 kbps;  */
/* playback starts after PREBUFFER_BYTES arrive (or a 5 s cap), and   */
/* re-arms after an underrun.                                          */
/* ------------------------------------------------------------------ */

#define RING_BYTES       (512 * 1024)
#define PREBUFFER_BYTES  (48 * 1024)

static EXT_RAM_BSS_ATTR uint8_t s_ring[RING_BYTES];
static size_t s_ring_head;   /* read position */
static size_t s_ring_count;  /* bytes currently buffered */

static void ring_reset(void)
{
    s_ring_head = 0;
    s_ring_count = 0;
}

/* Caller guarantees n <= RING_BYTES - s_ring_count. */
static void ring_put(const uint8_t *data, size_t n)
{
    size_t tail = (s_ring_head + s_ring_count) % RING_BYTES;
    size_t first = RING_BYTES - tail;
    if (first > n) first = n;
    memcpy(s_ring + tail, data, first);
    memcpy(s_ring, data + first, n - first);
    s_ring_count += n;
}

/* Caller guarantees n <= s_ring_count. */
static void ring_peek(uint8_t *out, size_t n)
{
    size_t first = RING_BYTES - s_ring_head;
    if (first > n) first = n;
    memcpy(out, s_ring + s_ring_head, first);
    memcpy(out + first, s_ring, n - first);
}

/* Caller guarantees n <= s_ring_count. */
static void ring_skip(size_t n)
{
    s_ring_head = (s_ring_head + n) % RING_BYTES;
    s_ring_count -= n;
}

/* ------------------------------------------------------------------ */
/* Resampler: arbitrary input rate, mono/stereo 16-bit -> 16 kHz mono. */
/* Linear interpolation, 16.16 fixed-point phase across blocks.        */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t pos;    /* 16.16 index into the current input block; may be -1 */
    uint32_t step;  /* (in_rate << 16) / 16000 */
    int16_t last;   /* last mono sample of the previous block */
    bool primed;
} resampler_t;

static resampler_t s_rs;

static void resampler_reset(resampler_t *rs, uint32_t in_rate)
{
    if (in_rate == 0) in_rate = 16000;
    if (in_rate > 96000) in_rate = 96000;  /* sanity clamp */
    rs->pos = 0;
    rs->step = (uint32_t)(((uint64_t)in_rate << 16) / 16000u);
    rs->last = 0;
    rs->primed = true;
}

/* Consume the whole mono input block, appending 16 kHz samples to out.
 * Returns the number of output samples written (capped at out_max). */
static size_t resampler_feed(resampler_t *rs, const int16_t *in, size_t n,
                             int16_t *out, size_t out_max)
{
    if (n == 0 || !rs->primed) return 0;
    size_t produced = 0;
    while (produced < out_max) {
        int32_t i = rs->pos >> 16;              /* arithmetic shift: can be -1 */
        if (i + 1 >= (int32_t)n) break;         /* need in[i+1] to interpolate */
        int32_t a = (i < 0) ? rs->last : in[i];
        int32_t b = in[i + 1];
        uint32_t f = (uint32_t)rs->pos & 0xffffu;
        out[produced++] = (int16_t)(a + (((b - a) * (int32_t)f) >> 16));
        rs->pos += (int32_t)rs->step;
    }
    rs->last = in[n - 1];
    rs->pos -= (int32_t)(n << 16);              /* rebase to the next block */
    return produced;
}

/* ------------------------------------------------------------------ */
/* Format sniffing (mirrors the reference project's header detector)   */
/* ------------------------------------------------------------------ */

static esp_audio_simple_dec_type_t detect_type(const uint8_t *header, size_t got,
                                               const char *url)
{
    if (got >= 12 && memcmp(header, "RIFF", 4) == 0 &&
        memcmp(header + 8, "WAVE", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (got >= 4 && memcmp(header, "fLaC", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (got >= 8 && memcmp(header + 4, "ftyp", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (got >= 3 && memcmp(header, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (got >= 2 && header[0] == 0xFF && (header[1] & 0xF6) == 0xF0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;  /* ADTS sync */
    }
    if (got >= 2 && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;  /* bare MPEG frame sync */
    }
    if (got >= 1 && header[0] == 0x47) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    }
    const char *dot = strrchr(url, '.');
    if (dot != NULL) {
        if (strcasecmp(dot, ".aac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        if (strcasecmp(dot, ".flac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (strcasecmp(dot, ".m4a") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    /* Unrecognized radio streams are MP3 in practice */
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/* ------------------------------------------------------------------ */
/* HTTP(S) stream open: verify TLS first, retry once without           */
/* verification (radio CDNs often serve incomplete cert chains).       */
/* ------------------------------------------------------------------ */

static esp_http_client_handle_t http_open_stream(const char *url, int timeout_ms)
{
    for (int insecure = 0; insecure < 2; ++insecure) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = timeout_ms,  /* bounds each read; stop lands fast */
            .buffer_size = 2048,
        };
        if (insecure == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (http == NULL) return NULL;
        if (esp_http_client_open(http, 0) == ESP_OK) {
            if (insecure != 0) {
                ESP_LOGW(TAG, "TLS verification skipped for %s", url);
            }
            return http;
        }
        esp_http_client_cleanup(http);
        if (strncmp(url, "https://", 8) != 0) return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Output: resample + throttle into the passthrough queue (~1.28 s)    */
/* ------------------------------------------------------------------ */

static void push_throttled(const int16_t *samples, size_t count)
{
    size_t offset = 0;
    while (offset < count && s_cmd == CMD_NONE) {
        size_t written = audio_passthrough_queue_output(samples + offset,
                                                        count - offset, 16000);
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));  /* queue full: wait for the drain */
        }
        offset += written;
    }
}

static void output_pcm(const uint8_t *data, uint32_t bytes, uint8_t channels,
                       uint32_t sample_rate, uint8_t bits_per_sample)
{
    if (bits_per_sample != 16 || bytes < 2) return;
    const int16_t *in = (const int16_t *)data;
    size_t n = bytes / sizeof(int16_t);
    const int16_t *mono = in;
    size_t n_mono = n;
    if (channels == 2) {
        n_mono = n / 2;
        if (n_mono > MONO_SAMPLES) n_mono = MONO_SAMPLES;
        for (size_t i = 0; i < n_mono; ++i) {
            s_mono[i] = (int16_t)(((int32_t)in[i * 2] + in[i * 2 + 1]) / 2);
        }
        mono = s_mono;
    } else if (channels != 1) {
        return;
    }
    if (n_mono > MONO_SAMPLES) n_mono = MONO_SAMPLES;
    size_t produced = resampler_feed(&s_rs, mono, n_mono,
                                     s_out16k, OUT16K_SAMPLES);
    if (produced > 0) push_throttled(s_out16k, produced);
}

/* ------------------------------------------------------------------ */
/* Stream session                                                      */
/* ------------------------------------------------------------------ */

static void set_error(const char *reason)
{
    strlcpy(s_error, reason, sizeof(s_error));
    s_state = NET_RADIO_STATE_ERROR;
    ESP_LOGW(TAG, "stream error: %s", reason);
}

/* ------------------------------------------------------------------ */
/* HLS (m3u8) source — ported from the reference project (nrl-esp32    */
/* media_decoder.cpp). A media playlist of sequential segment URLs,    */
/* reloaded periodically for live streams. Segments are MPEG-TS or     */
/* raw AAC/MP3; the simple decoder handles all three, so this layer    */
/* only concatenates them into one byte stream.                        */
/* ------------------------------------------------------------------ */

#define HLS_MAX_SEGMENTS   12
#define HLS_URL_LEN        256
#define HLS_PLAYLIST_BYTES 16384

static EXT_RAM_BSS_ATTR char s_hls_seg_urls[HLS_MAX_SEGMENTS][HLS_URL_LEN];
static EXT_RAM_BSS_ATTR char s_hls_playlist[HLS_PLAYLIST_BYTES];

typedef struct {
    bool is_hls;
    bool eof;                      /* VOD played out / stream ended */
    esp_http_client_handle_t http; /* plain stream, or current HLS segment */
    int64_t last_activity_us;      /* bytes or playlist fetch, for stall detect */
    /* HLS state */
    char playlist_url[NET_RADIO_URL_MAX];
    uint64_t next_media_seq;       /* first sequence not yet queued */
    uint32_t target_duration_s;
    int64_t last_fetch_us;
    size_t seg_next;
    size_t seg_count;
    bool live;                     /* no #EXT-X-ENDLIST seen */
} stream_source_t;

/* Resolve `ref` against `base` (RFC-lite: absolute, host-relative, relative). */
static void hls_resolve_url(const char *base, const char *ref, char *out,
                            size_t cap)
{
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    if (ref[0] == '/') {
        /* scheme://host[:port] + ref */
        const char *scheme_end = strstr(base, "://");
        const char *host_end = scheme_end != NULL ? strchr(scheme_end + 3, '/') : NULL;
        int host_len = host_end != NULL ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, cap, "%.*s%s", host_len, base, ref);
        return;
    }
    /* Relative to the playlist's directory (strip any query string first) */
    char dir[NET_RADIO_URL_MAX];
    snprintf(dir, sizeof(dir), "%s", base);
    char *query = strchr(dir, '?');
    if (query != NULL) *query = '\0';
    char *slash = strrchr(dir, '/');
    if (slash != NULL && slash - dir > 7) slash[1] = '\0';  /* keep "https://" */
    snprintf(out, cap, "%s%s", dir, ref);
}

/* GET a small text resource (the playlist) into s_hls_playlist. */
static size_t hls_fetch_text(const char *url)
{
    esp_http_client_handle_t http = http_open_stream(url, 3000);
    if (http == NULL) {
        ESP_LOGW(TAG, "hls: playlist open failed: %s", url);
        s_hls_playlist[0] = '\0';
        return 0;
    }
    (void)esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    size_t got = 0;
    if (status == 200) {
        while (got + 1 < sizeof(s_hls_playlist)) {
            int n = esp_http_client_read(http, s_hls_playlist + got,
                                         (int)(sizeof(s_hls_playlist) - 1 - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
    } else {
        ESP_LOGW(TAG, "hls: playlist HTTP %d: %s", status, url);
    }
    (void)esp_http_client_close(http);
    esp_http_client_cleanup(http);
    s_hls_playlist[got] = '\0';
    return got;
}

/* Fetch + parse the playlist; follows master-playlist indirection (first
 * variant). Queues segments with sequence >= next_media_seq. */
static bool hls_load_playlist(stream_source_t *h)
{
    bool ok = false;
    for (int depth = 0; depth < 3; ++depth) {
        if (hls_fetch_text(h->playlist_url) == 0) break;
        const bool master = strstr(s_hls_playlist, "#EXT-X-STREAM-INF") != NULL;
        uint64_t first_seq = 0;
        size_t listed = 0;
        bool endlist = false;
        bool got_variant = false;
        h->seg_count = 0;
        h->seg_next = 0;

        char *save = NULL;
        bool after_stream_inf = false;
        for (char *line = strtok_r(s_hls_playlist, "\n", &save); line != NULL;
             line = strtok_r(NULL, "\n", &save)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
            if (line[0] == '\0') continue;
            if (master) {
                if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
                    after_stream_inf = true;
                } else if (after_stream_inf && line[0] != '#') {
                    /* First variant: adopt it as the media playlist, refetch */
                    char resolved[NET_RADIO_URL_MAX];
                    hls_resolve_url(h->playlist_url, line, resolved, sizeof(resolved));
                    snprintf(h->playlist_url, sizeof(h->playlist_url), "%s", resolved);
                    got_variant = true;
                    break;
                }
                continue;
            }
            if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                first_seq = strtoull(line + 22, NULL, 10);
            } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
                unsigned long v = strtoul(line + 22, NULL, 10);
                if (v > 0 && v <= 60) h->target_duration_s = (uint32_t)v;
            } else if (strcmp(line, "#EXT-X-ENDLIST") == 0) {
                endlist = true;
            } else if (line[0] != '#') {
                uint64_t seq = first_seq + listed;
                ++listed;
                if (seq >= h->next_media_seq && h->seg_count < HLS_MAX_SEGMENTS) {
                    hls_resolve_url(h->playlist_url, line,
                                    s_hls_seg_urls[h->seg_count], HLS_URL_LEN);
                    ++h->seg_count;
                }
            }
        }

        if (master) {
            if (!got_variant) {
                ESP_LOGW(TAG, "hls: master playlist without variants");
                break;
            }
            continue;  /* refetch the variant playlist */
        }
        h->next_media_seq = first_seq + listed;
        h->live = !endlist;
        h->last_fetch_us = esp_timer_get_time();
        h->last_activity_us = h->last_fetch_us;  /* stream is alive */
        ok = true;
        break;
    }
    return ok;
}

/* Sequential byte stream across segments; refreshes the playlist on live
 * streams. Returns 0 when no data is available right now; sets h->eof at
 * true end of stream (VOD done). */
static int hls_read(stream_source_t *h, uint8_t *dst, size_t size)
{
    while (!h->eof && s_cmd == CMD_NONE) {
        if (h->http == NULL) {
            if (h->seg_next >= h->seg_count) {
                if (!h->live) {
                    ESP_LOGI(TAG, "hls: vod played out");
                    h->eof = true;
                    return 0;
                }
                /* Live: poll the playlist, paced by the advertised duration */
                int64_t since_us = esp_timer_get_time() - h->last_fetch_us;
                int64_t min_wait_us = (int64_t)h->target_duration_s * 500000LL;
                if (since_us < min_wait_us) return 0;  /* caller retries */
                if (!hls_load_playlist(h)) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    return 0;
                }
                if (h->seg_count == 0) vTaskDelay(pdMS_TO_TICKS(300));
                return 0;
            }
            const char *url = s_hls_seg_urls[h->seg_next++];
            h->http = http_open_stream(url, 3000);
            if (h->http == NULL) {
                ESP_LOGW(TAG, "hls: segment open failed, skipping");
                continue;
            }
            (void)esp_http_client_fetch_headers(h->http);
            int status = esp_http_client_get_status_code(h->http);
            if (status != 200) {
                ESP_LOGW(TAG, "hls: segment HTTP %d, skipping", status);
                (void)esp_http_client_close(h->http);
                esp_http_client_cleanup(h->http);
                h->http = NULL;
                continue;
            }
        }
        int n = esp_http_client_read(h->http, (char *)dst, (int)size);
        if (n > 0) {
            h->last_activity_us = esp_timer_get_time();
            return n;
        }
        /* Segment finished (or errored): move on to the next one */
        (void)esp_http_client_close(h->http);
        esp_http_client_cleanup(h->http);
        h->http = NULL;
    }
    return 0;
}

/* Open an m3u8 source: load the playlist and queue the first segments.
 * The first segment connection is opened lazily by hls_read. */
static bool hls_open(stream_source_t *h, const char *url)
{
    snprintf(h->playlist_url, sizeof(h->playlist_url), "%s", url);
    h->target_duration_s = 6;
    if (!hls_load_playlist(h) || h->seg_count == 0) {
        ESP_LOGW(TAG, "hls: no playable segments: %s", url);
        return false;
    }
    ESP_LOGI(TAG, "hls: %s stream, %u segments queued, target %lus",
             h->live ? "live" : "vod", (unsigned)h->seg_count,
             (unsigned long)h->target_duration_s);
    return true;
}

/* ------------------------------------------------------------------ */
/* Unified source read/close (plain HTTP stream or HLS)                */
/* ------------------------------------------------------------------ */

static int source_read(stream_source_t *src, uint8_t *dst, size_t size)
{
    if (src->eof || s_cmd != CMD_NONE) return 0;
    if (!src->is_hls) {
        int n = esp_http_client_read(src->http, (char *)dst, (int)size);
        if (n > 0) {
            src->last_activity_us = esp_timer_get_time();
            return n;
        }
        return 0;  /* timeout/idle: caller decides; close shows as stall */
    }
    return hls_read(src, dst, size);
}

static void source_close(stream_source_t *src)
{
    if (src->http != NULL) {
        (void)esp_http_client_close(src->http);
        esp_http_client_cleanup(src->http);
        src->http = NULL;
    }
}

static void play_stream(int index, const char *url)
{
    s_error[0] = '\0';
    s_state = NET_RADIO_STATE_CONNECTING;
    ESP_LOGI(TAG, "connecting [%d] %s", index, url);

    stream_source_t src = {0};
    src.is_hls = strstr(url, ".m3u8") != NULL;
    src.last_activity_us = esp_timer_get_time();
    if (src.is_hls) {
        if (!hls_open(&src, url)) {
            set_error("playlist failed");
            return;
        }
    } else {
        src.http = http_open_stream(url, 1000);
        if (src.http == NULL) {
            set_error("connect failed");
            return;
        }
        /* Response headers must be fetched before reading, otherwise the
         * client reports a zero-length body and every read returns 0. */
        (void)esp_http_client_fetch_headers(src.http);
        int http_status = esp_http_client_get_status_code(src.http);
        if (http_status != 200) {
            char msg[32];
            snprintf(msg, sizeof(msg), "HTTP %d", http_status);
            set_error(msg);
            goto done;
        }
    }
    if (s_cmd != CMD_NONE) goto done;  /* stopped while connecting */

    ring_reset();

    /* Sniff the stream format from the first bytes */
    uint8_t header[16];
    size_t got = 0;
    while (got < sizeof(header) && s_cmd == CMD_NONE) {
        int n = source_read(&src, header + got, sizeof(header) - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (src.eof) {
            set_error("stream closed");
            goto done;
        } else if (esp_timer_get_time() - src.last_activity_us > 15000000) {
            set_error("read timeout");
            goto done;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (s_cmd != CMD_NONE || got == 0) goto done;

    esp_audio_simple_dec_type_t type = detect_type(header, got, url);
    if (esp_audio_simple_check_audio_type(type) != ESP_AUDIO_ERR_OK) {
        set_error("unsupported format");
        goto done;
    }
    ESP_LOGI(TAG, "format: %s", esp_audio_simple_dec_get_name(type));

    esp_audio_simple_dec_cfg_t dec_cfg = { .dec_type = type };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        set_error("decoder open failed");
        goto done;
    }

    ring_put(header, got);  /* the sniffed bytes are stream content */
    bool info_valid = false;
    bool started = false;
    int64_t prebuf_start_us = esp_timer_get_time();
    int stalled = 0;

    while (s_cmd == CMD_NONE) {
        /* Refill the ring from the network */
        size_t space = RING_BYTES - s_ring_count;
        if (space >= 1024 && !src.eof) {
            int n = source_read(&src, s_raw,
                                space > RAW_BYTES ? RAW_BYTES : space);
            if (n > 0) ring_put(s_raw, (size_t)n);
        }
        if (!src.eof &&
            esp_timer_get_time() - src.last_activity_us > 15000000) {
            esp_audio_simple_dec_close(dec);
            set_error("stream stalled");
            goto done;
        }

        /* Prebuffer before (re)starting the output; absorbs network jitter */
        if (!started) {
            if (s_ring_count < PREBUFFER_BYTES && !src.eof &&
                esp_timer_get_time() - prebuf_start_us < 5000000) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            started = true;
            ESP_LOGI(TAG, "playback starts, %u bytes buffered",
                     (unsigned)s_ring_count);
        }
        if (s_ring_count == 0) {
            if (src.eof) break;  /* VOD/finite stream played out cleanly */
            /* Underrun: re-arm the prebuffer and wait for the refill */
            started = false;
            prebuf_start_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Decode one chunk from the ring; the unconsumed tail stays queued.
         * TS chunks are rounded to whole 188-byte packets: presenting a
         * partial packet at the chunk boundary corrupts the demuxer's
         * output frames (shows up as AAC decode errors). */
        size_t chunk = s_ring_count > RAW_BYTES ? RAW_BYTES : s_ring_count;
        if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_TS) {
            chunk -= chunk % 188;
            if (chunk == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        ring_peek(s_raw, chunk);
        size_t offset = 0;
        while (offset < chunk && s_cmd == CMD_NONE) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = s_raw + offset,
                .len = (uint32_t)(chunk - offset),
                .eos = false,
            };
            esp_audio_simple_dec_out_t frame = {
                .buffer = s_out,
                .len = OUT_BYTES,
            };
            esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                esp_audio_simple_dec_close(dec);
                set_error("frame too large");
                goto done;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                offset = chunk;  /* skip the rest of this block */
                break;
            }
            offset += raw.consumed;
            if (frame.decoded_size > 0) {
                if (!info_valid) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                        ESP_LOGI(TAG, "stream: %lu Hz %uch %ubit",
                                 (unsigned long)info.sample_rate,
                                 (unsigned)info.channel,
                                 (unsigned)info.bits_per_sample);
                        resampler_reset(&s_rs, info.sample_rate);
                        info_valid = true;
                        s_state = NET_RADIO_STATE_PLAYING;
                        if (!s_ptt_held) {
                            status_io_set_network_ptt(true);
                            s_ptt_held = true;
                        }
                    }
                }
                if (info_valid) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                        output_pcm(frame.buffer, frame.decoded_size, info.channel,
                                   info.sample_rate, info.bits_per_sample);
                    }
                }
            }
            if (raw.consumed == 0 && frame.decoded_size == 0) {
                /* No progress: never drop bytes (breaks the TS demuxer);
                 * leave the block in the ring and wait for more data. */
                if (++stalled > 500) {  /* ~5 s without any progress */
                    esp_audio_simple_dec_close(dec);
                    set_error("decoder stalled");
                    goto done;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            stalled = 0;
        }
        ring_skip(offset);
    }
    esp_audio_simple_dec_close(dec);

done:
    source_close(&src);
    if (s_ptt_held) {
        status_io_set_network_ptt(false);
        s_ptt_held = false;
    }
    audio_passthrough_clear_output();  /* drop queued music tail */
    if (s_state == NET_RADIO_STATE_CONNECTING ||
        s_state == NET_RADIO_STATE_PLAYING) {
        s_state = NET_RADIO_STATE_IDLE;
    }
    ESP_LOGI(TAG, "stream ended (state=%d)", (int)s_state);
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_cmd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int index = s_cmd;
        s_cmd = CMD_NONE;
        if (index == CMD_STOP) continue;
        char url[NET_RADIO_URL_MAX];
        if (!net_radio_get((size_t)index, NULL, 0, url, sizeof(url))) continue;
        play_stream(index, url);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t net_radio_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) return ESP_ERR_NO_MEM;
    }
    /* Register only the codecs radio streams use; linking every decoder
     * would waste flash (same reasoning as the reference project). */
    static bool s_registered;
    if (!s_registered) {
        if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK ||
            esp_aac_dec_register() != ESP_AUDIO_ERR_OK ||
            esp_flac_dec_register() != ESP_AUDIO_ERR_OK ||
            esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "decoder register failed");
            return ESP_FAIL;
        }
        s_registered = true;
    }

    nvs_handle_t nvs;
    if (nvs_open(k_nvs_namespace, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t count = 0;
        if (nvs_get_u8(nvs, "count", &count) == ESP_OK &&
            count <= NET_RADIO_STATION_MAX) {
            size_t blob_size = count * sizeof(station_entry_t);
            if (count == 0 ||
                nvs_get_blob(nvs, "list", s_entries, &blob_size) == ESP_OK) {
                s_count = count;
            }
        }
        int8_t current = -1;
        if (nvs_get_i8(nvs, "cur", &current) == ESP_OK &&
            current >= 0 && (size_t)current < s_count) {
            s_current = current;
        }
        nvs_close(nvs);
    }
    /* NUL-terminate defensively in case the blob came from a future layout */
    for (size_t i = 0; i < s_count; ++i) {
        s_entries[i].name[NET_RADIO_NAME_MAX - 1] = '\0';
        s_entries[i].url[NET_RADIO_URL_MAX - 1] = '\0';
    }
    ESP_LOGI(TAG, "%u stations loaded", (unsigned)s_count);
    return ESP_OK;
}

size_t net_radio_count(void)
{
    return s_count;
}

bool net_radio_get(size_t index, char *name, size_t name_size,
                   char *url, size_t url_size)
{
    list_lock();
    const bool ok = index < s_count;
    if (ok) {
        if (name != NULL && name_size > 0) {
            snprintf(name, name_size, "%s", s_entries[index].name);
        }
        if (url != NULL && url_size > 0) {
            snprintf(url, url_size, "%s", s_entries[index].url);
        }
    }
    list_unlock();
    return ok;
}

bool net_radio_add(const char *name, const char *url)
{
    if (!url_valid(url) || (name != NULL && strlen(name) >= NET_RADIO_NAME_MAX)) {
        return false;
    }
    list_lock();
    if (s_count >= NET_RADIO_STATION_MAX) {
        list_unlock();
        return false;
    }
    station_entry_t *slot = &s_entries[s_count++];
    snprintf(slot->name, sizeof(slot->name), "%s",
             (name != NULL && name[0] != '\0') ? name : url);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    save_locked();
    list_unlock();
    return true;
}

bool net_radio_remove(size_t index)
{
    list_lock();
    if (index >= s_count) {
        list_unlock();
        return false;
    }
    const bool removing_current = (s_current == (int)index);
    memmove(&s_entries[index], &s_entries[index + 1],
            (s_count - index - 1) * sizeof(station_entry_t));
    --s_count;
    if (s_current == (int)index) {
        s_current = -1;
    } else if (s_current > (int)index) {
        --s_current;
    }
    save_locked();
    list_unlock();
    if (removing_current && net_radio_is_playing()) {
        net_radio_stop();
    }
    return true;
}

int net_radio_get_current(void)
{
    return s_current;
}

bool net_radio_play(size_t index)
{
    char name[NET_RADIO_NAME_MAX];
    if (!net_radio_get(index, name, sizeof(name), NULL, 0)) return false;
    list_lock();
    if (s_current != (int)index) {
        s_current = (int)index;
        save_locked();
    }
    list_unlock();
    s_cmd = (int)index;
    if (s_task == NULL) {
        /* TLS handshake + AAC/MP3 decode need plenty of stack headroom */
        s_task = xTaskCreateStaticPinnedToCore(
            player_task, "net_radio",
            PLAYER_STACK_BYTES / sizeof(StackType_t), NULL, 5,
            s_player_stack, &s_player_tcb, 1);
        if (s_task == NULL) {
            ESP_LOGE(TAG, "player task create failed");
            set_error("no memory");
            return false;
        }
    }
    return true;
}

void net_radio_stop(void)
{
    /* Unconditional: also cancels a play command not yet picked up */
    s_cmd = CMD_STOP;
}

bool net_radio_next(void)
{
    if (s_count == 0) return false;
    size_t next = (s_current < 0) ? 0 : ((size_t)s_current + 1) % s_count;
    return net_radio_play(next);
}

bool net_radio_prev(void)
{
    if (s_count == 0) return false;
    size_t prev = (s_current <= 0) ? s_count - 1 : (size_t)s_current - 1;
    return net_radio_play(prev);
}

bool net_radio_is_playing(void)
{
    return s_state == NET_RADIO_STATE_PLAYING ||
           s_state == NET_RADIO_STATE_CONNECTING;
}

void net_radio_get_status(net_radio_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    status->state = s_state;
    status->current = s_current;
    strlcpy(status->error, s_error, sizeof(status->error));
    if (s_current >= 0) {
        net_radio_get((size_t)s_current, status->station_name,
                      sizeof(status->station_name), NULL, 0);
    }
}

void net_radio_notify_nrl_voice(void)
{
    if (net_radio_is_playing()) {
        ESP_LOGI(TAG, "NRL voice preempts radio playback");
        net_radio_stop();
    }
}
