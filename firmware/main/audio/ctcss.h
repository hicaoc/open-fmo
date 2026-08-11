#pragma once

#include <stddef.h>
#include <stdint.h>

#define CTCSS_TONE_COUNT 50U

typedef enum {
    CTCSS_GATE_DISABLED = 0,
    CTCSS_GATE_SEARCHING,
    CTCSS_GATE_MATCHED,
    CTCSS_GATE_REJECTED,
} ctcss_gate_state_t;

typedef struct {
    float coefficients[CTCSS_TONE_COUNT];
    float q1[CTCSS_TONE_COUNT];
    float q2[CTCSS_TONE_COUNT];
    double energy;
    uint32_t sample_rate_hz;
    size_t window_samples;
    size_t sample_count;
    float expected_hz;
    float detected_hz;
    int candidate;
    uint8_t stable_windows;
    uint8_t silent_windows;
    ctcss_gate_state_t state;
} ctcss_detector_t;

typedef struct {
    double phase;
    float frequency_hz;
    uint32_t sample_rate_hz;
} ctcss_generator_t;

void ctcss_detector_configure(ctcss_detector_t *detector, float expected_hz,
                              uint32_t sample_rate_hz);
void ctcss_detector_reset(ctcss_detector_t *detector);
ctcss_gate_state_t ctcss_detector_feed(ctcss_detector_t *detector,
                                       const int16_t *samples, size_t count);
float ctcss_detector_detected_hz(const ctcss_detector_t *detector);

void ctcss_generator_configure(ctcss_generator_t *generator,
                               float frequency_hz, uint32_t sample_rate_hz);
void ctcss_generator_mix(ctcss_generator_t *generator, int16_t *samples,
                         size_t count);
