#include "mdc_signaling.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "mdc_decode.h"

#define MDC_DISPLAY_HOLD_MS 5000U

typedef struct {
    mdc_signal_source_t source;
    uint32_t sample_rate;
    mdc_decoder_t *decoder;
} decoder_slot_t;

static const char *TAG = "mdc1200";
static portMUX_TYPE s_spin = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_initialized;
static decoder_slot_t s_slots[4];
static mdc_signal_status_t s_last;
static uint32_t s_last_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void decoded(int frame_count, unsigned char opcode,
                    unsigned char argument, unsigned short unit_id,
                    unsigned char extra0, unsigned char extra1,
                    unsigned char extra2, unsigned char extra3, void *context)
{
    (void)extra0;
    (void)extra1;
    (void)extra2;
    (void)extra3;
    const decoder_slot_t *slot = context;
    if (slot == NULL || !s_initialized) return;
    portENTER_CRITICAL(&s_spin);
    s_last.source = slot->source;
    s_last.frame_count = (uint8_t)frame_count;
    s_last.opcode = opcode;
    s_last.argument = argument;
    s_last.unit_id = unit_id;
    s_last.age_ms = 0;
    s_last_ms = now_ms();
    portEXIT_CRITICAL(&s_spin);
    ESP_LOGI(TAG, "MDC %s: ID=%04X OP=%02X ARG=%02X frames=%d",
             slot->source == MDC_SOURCE_NRL ? "NRL" : "RF",
             unit_id, opcode, argument, frame_count);
}

bool mdc_signaling_init(void)
{
    if (s_initialized) return true;
    const uint32_t rates[] = {8000U, 16000U};
    size_t index = 0;
    for (int source = MDC_SOURCE_RADIO; source <= MDC_SOURCE_NRL; ++source) {
        for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
            decoder_slot_t *slot = &s_slots[index++];
            slot->source = (mdc_signal_source_t)source;
            slot->sample_rate = rates[r];
            slot->decoder = mdc_decoder_new((int)rates[r]);
            if (slot->decoder == NULL ||
                mdc_decoder_set_callback(slot->decoder, decoded, slot) != 0) {
                ESP_LOGE(TAG, "decoder allocation failed: source=%d rate=%lu",
                         source, (unsigned long)rates[r]);
                return false;
            }
        }
    }
    s_initialized = true;
    ESP_LOGI(TAG, "MDC1200 RX ready: RF/NRL at 8/16 kHz, display hold=5s");
    return true;
}

void mdc_signaling_feed(mdc_signal_source_t source, const int16_t *samples,
                        size_t sample_count, uint32_t sample_rate_hz)
{
    if (samples == NULL || sample_count == 0 || !s_initialized) return;
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); ++i) {
        decoder_slot_t *slot = &s_slots[i];
        if (slot->source == source && slot->sample_rate == sample_rate_hz) {
            /* The imported decoder API predates const-correct PCM inputs. */
            (void)mdc_decoder_process_samples(slot->decoder,
                                               (mdc_sample_t *)samples,
                                               (int)sample_count);
            return;
        }
    }
}

bool mdc_signaling_get_recent(mdc_signal_status_t *status)
{
    if (status == NULL || !s_initialized) return false;
    portENTER_CRITICAL(&s_spin);
    const uint32_t age = s_last_ms != 0 ? now_ms() - s_last_ms : UINT32_MAX;
    *status = s_last;
    status->age_ms = age;
    portEXIT_CRITICAL(&s_spin);
    return age <= MDC_DISPLAY_HOLD_MS;
}
