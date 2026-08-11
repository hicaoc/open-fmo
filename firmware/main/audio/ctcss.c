#include "ctcss.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#define CTCSS_PI 3.14159265358979323846
#define CTCSS_TONE_AMPLITUDE 1500.0

static const char *CTCSS_TAG = "ctcss";

/* EIA standard PL tones, including the commonly used split tones. */
static const float s_tones[CTCSS_TONE_COUNT] = {
    67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
    94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f,
    127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 151.4f, 156.7f, 159.8f, 162.2f,
    165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f,
    192.8f, 196.6f, 199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f,
    233.6f, 241.8f, 250.3f, 254.1f,
};

static void clear_window(ctcss_detector_t *detector)
{
    memset(detector->q1, 0, sizeof(detector->q1));
    memset(detector->q2, 0, sizeof(detector->q2));
    detector->energy = 0.0;
    detector->sample_count = 0;
}

static bool tone_matches(float actual, float expected)
{
    return fabsf(actual - expected) <= 1.0f;
}

void ctcss_detector_configure(ctcss_detector_t *detector, float expected_hz,
                              uint32_t sample_rate_hz)
{
    if (detector == NULL) return;
    memset(detector, 0, sizeof(*detector));
    detector->candidate = -1;
    detector->expected_hz = expected_hz;
    detector->sample_rate_hz = sample_rate_hz;
    if (sample_rate_hz < 1000U) {
        detector->state = CTCSS_GATE_DISABLED;
        return;
    }
    /* Two consecutive 250 ms decisions keep false opens low while limiting
     * the start-of-call qualification delay to about 500 ms. */
    detector->window_samples = sample_rate_hz / 4U;
    /* Always compute coefficients so detection works even when no specific
     * tone is expected (expected_hz=0 means "detect any, don't gate"). */
    for (size_t i = 0; i < CTCSS_TONE_COUNT; ++i) {
        detector->coefficients[i] =
            2.0f * cosf((float)(2.0 * CTCSS_PI) * s_tones[i] /
                        (float)sample_rate_hz);
    }
    /* Always SEARCHING so feed() processes samples; gating decision is
     * made by the caller based on whether expected_hz > 0. */
    detector->state = CTCSS_GATE_SEARCHING;
}

void ctcss_detector_reset(ctcss_detector_t *detector)
{
    if (detector == NULL) return;
    clear_window(detector);
    detector->candidate = -1;
    detector->stable_windows = 0;
    detector->silent_windows = 0;
    detector->detected_hz = 0.0f;
    detector->state = CTCSS_GATE_SEARCHING;
}

static void evaluate_window(ctcss_detector_t *detector)
{
    int best_index = -1;
    double best_power = 0.0;
    double second_power = 0.0;
    for (size_t i = 0; i < CTCSS_TONE_COUNT; ++i) {
        const double q1 = detector->q1[i];
        const double q2 = detector->q2[i];
        const double power = q1 * q1 + q2 * q2 -
                             detector->coefficients[i] * q1 * q2;
        if (power > best_power) {
            second_power = best_power;
            best_power = power;
            best_index = (int)i;
        } else if (power > second_power) {
            second_power = power;
        }
    }

    const double normalized = detector->energy > 0.0
        ? (2.0 * best_power) /
              ((double)detector->sample_count * detector->energy)
        : 0.0;
    const double rms = detector->sample_count > 0
        ? sqrt(detector->energy / (double)detector->sample_count) : 0.0;
    const bool valid = best_index >= 0 && rms >= 8.0 && normalized >= 0.008 &&
                       best_power >= second_power * 1.5;

    /* Periodic debug: log every evaluation so we can see actual levels */
    ESP_LOGD(CTCSS_TAG, "win: rms=%.1f norm=%.4f best=%d pwr=%.0f 2nd=%.0f %s",
             rms, normalized, best_index, best_power, second_power,
             valid ? "VALID" : "weak");

    if (valid) {
        detector->silent_windows = 0;
        if (detector->candidate == best_index) {
            if (detector->stable_windows < UINT8_MAX) {
                ++detector->stable_windows;
            }
        } else {
            detector->candidate = best_index;
            detector->stable_windows = 1;
        }
        if (detector->stable_windows >= 2) {
            detector->detected_hz = s_tones[best_index];
            detector->state = tone_matches(detector->detected_hz,
                                           detector->expected_hz)
                ? CTCSS_GATE_MATCHED : CTCSS_GATE_REJECTED;
            ESP_LOGI(CTCSS_TAG, "detected %.1f Hz (expect %.1f) -> %s",
                     (double)detector->detected_hz,
                     (double)detector->expected_hz,
                     detector->state == CTCSS_GATE_MATCHED ? "MATCH" : "REJECT");
        }
    } else {
        detector->candidate = -1;
        detector->stable_windows = 0;
        if (detector->silent_windows < UINT8_MAX) ++detector->silent_windows;
        if (detector->silent_windows >= 2) {
            detector->detected_hz = 0.0f;
            detector->state = CTCSS_GATE_REJECTED;
        }
    }
    clear_window(detector);
}

ctcss_gate_state_t ctcss_detector_feed(ctcss_detector_t *detector,
                                       const int16_t *samples, size_t count)
{
    if (detector == NULL || samples == NULL ||
        detector->state == CTCSS_GATE_DISABLED) {
        return CTCSS_GATE_DISABLED;
    }
    for (size_t n = 0; n < count; ++n) {
        const float sample = (float)samples[n];
        detector->energy += (double)sample * sample;
        for (size_t i = 0; i < CTCSS_TONE_COUNT; ++i) {
            const float q0 = sample + detector->coefficients[i] *
                                      detector->q1[i] - detector->q2[i];
            detector->q2[i] = detector->q1[i];
            detector->q1[i] = q0;
        }
        if (++detector->sample_count >= detector->window_samples) {
            evaluate_window(detector);
        }
    }
    return detector->state;
}

float ctcss_detector_detected_hz(const ctcss_detector_t *detector)
{
    return detector != NULL ? detector->detected_hz : 0.0f;
}

void ctcss_generator_configure(ctcss_generator_t *generator,
                               float frequency_hz, uint32_t sample_rate_hz)
{
    if (generator == NULL) return;
    generator->frequency_hz = frequency_hz;
    generator->sample_rate_hz = sample_rate_hz;
    generator->phase = 0.0;
}

void ctcss_generator_mix(ctcss_generator_t *generator, int16_t *samples,
                         size_t count)
{
    if (generator == NULL || samples == NULL ||
        generator->frequency_hz <= 0.0f || generator->sample_rate_hz == 0) {
        return;
    }
    const double increment = 2.0 * CTCSS_PI * generator->frequency_hz /
                             generator->sample_rate_hz;
    double phase = generator->phase;
    for (size_t i = 0; i < count; ++i) {
        int32_t mixed = samples[i] + (int32_t)(sin(phase) * CTCSS_TONE_AMPLITUDE);
        if (mixed > INT16_MAX) mixed = INT16_MAX;
        if (mixed < INT16_MIN) mixed = INT16_MIN;
        samples[i] = (int16_t)mixed;
        phase += increment;
        if (phase >= 2.0 * CTCSS_PI) phase -= 2.0 * CTCSS_PI;
    }
    generator->phase = phase;
}
