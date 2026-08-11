#include "nrl_g711.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_heap_caps.h"

static bool s_ready;
static uint8_t *s_encode_table;
static int16_t s_decode_table[256];

static uint8_t encode_slow(int16_t pcm)
{
    uint8_t sign = 0;
    int16_t value;
    if (pcm < 0) {
        sign = 0x80;
        value = (int16_t)((~pcm) >> 4);
    } else {
        value = (int16_t)(pcm >> 4);
    }
    if (value > 15) {
        uint8_t exponent = 1;
        while (value > 31) {
            value >>= 1;
            ++exponent;
        }
        value = (int16_t)(value - 16 + (exponent << 4));
    }
    if (sign == 0) value |= 0x80;
    return (uint8_t)value ^ 0x55;
}

static int16_t decode_slow(uint8_t alaw)
{
    uint8_t code = alaw ^ 0x55;
    int16_t exponent = (int16_t)((code & 0x70) >> 4);
    int16_t mantissa = code & 0x0f;
    if (exponent > 0) mantissa += 16;
    mantissa = (int16_t)((mantissa << 4) + 8);
    if (exponent > 1) mantissa <<= (uint8_t)(exponent - 1);
    return (code & 0x80) != 0 ? mantissa : (int16_t)-mantissa;
}

esp_err_t nrl_g711_init(void)
{
    if (s_ready) return ESP_OK;
    s_encode_table = heap_caps_malloc(65536,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_encode_table == NULL) {
        s_encode_table = heap_caps_malloc(65536, MALLOC_CAP_8BIT);
    }
    for (size_t i = 0; i < 256; ++i) {
        s_decode_table[i] = decode_slow((uint8_t)i);
    }
    if (s_encode_table != NULL) {
        for (uint32_t i = 0; i < 65536; ++i) {
            s_encode_table[i] = encode_slow((int16_t)i);
        }
    }
    s_ready = true;
    return s_encode_table != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

uint8_t nrl_g711_encode_alaw(int16_t pcm)
{
    if (!s_ready) (void)nrl_g711_init();
    return s_encode_table != NULL ? s_encode_table[(uint16_t)pcm]
                                  : encode_slow(pcm);
}

int16_t nrl_g711_decode_alaw(uint8_t alaw)
{
    if (!s_ready) (void)nrl_g711_init();
    return s_decode_table[alaw];
}

