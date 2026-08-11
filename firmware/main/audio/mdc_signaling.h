#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MDC_SOURCE_RADIO = 0,
    MDC_SOURCE_NRL,
} mdc_signal_source_t;

typedef struct {
    mdc_signal_source_t source;
    uint8_t frame_count;
    uint8_t opcode;
    uint8_t argument;
    uint16_t unit_id;
    uint32_t age_ms;
} mdc_signal_status_t;

bool mdc_signaling_init(void);
void mdc_signaling_feed(mdc_signal_source_t source, const int16_t *samples,
                        size_t sample_count, uint32_t sample_rate_hz);
/* A decoded tail remains available for five seconds after reception. */
bool mdc_signaling_get_recent(mdc_signal_status_t *status);
