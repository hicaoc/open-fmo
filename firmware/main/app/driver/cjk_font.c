/* Compact renderer for the Noto Sans SC bitmap imported from nrl-esp32.
 * Font source: Noto Sans SC Regular, SIL Open Font License 1.1. */
#include "cjk_font.h"

#include <stddef.h>
#include <string.h>

extern const uint8_t cjk_font_start[] asm("_binary_cjk16_bin_start");
extern const uint8_t cjk_font_end[] asm("_binary_cjk16_bin_end");

#define FONT_HEADER_SIZE 12U
#define FONT_RECORD_SIZE 10U
#define FONT_CANVAS_SIZE 16

static uint16_t read_u16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static const uint8_t *find_glyph(uint32_t codepoint, uint32_t *count_out)
{
    const size_t size = (size_t)(cjk_font_end - cjk_font_start);
    if (size < FONT_HEADER_SIZE || memcmp(cjk_font_start, "FMOF", 4) != 0 ||
        read_u32(cjk_font_start + 4) != 1) return NULL;
    const uint32_t count = read_u32(cjk_font_start + 8);
    if (FONT_HEADER_SIZE + (size_t)count * FONT_RECORD_SIZE > size) return NULL;
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        const uint8_t *record = cjk_font_start + FONT_HEADER_SIZE +
                                (size_t)middle * FONT_RECORD_SIZE;
        uint16_t current = read_u16(record);
        if (current < codepoint) low = middle + 1;
        else high = middle;
    }
    if (low >= count) return NULL;
    const uint8_t *record = cjk_font_start + FONT_HEADER_SIZE +
                            (size_t)low * FONT_RECORD_SIZE;
    if (read_u16(record) != codepoint) return NULL;
    if (count_out != NULL) *count_out = count;
    return record;
}

static uint16_t blend_rgb565(uint16_t background, uint16_t foreground,
                             uint8_t coverage)
{
    /* Threshold to binary: crisp pixel-aligned edges on small LCD.
     * Coverage 0-15 from 4-bit AA; snap at midpoint eliminates blur. */
    if (coverage < 7) return background;
    return foreground;
}

bool cjk_font_draw(gfx_canvas_t *canvas, int x, int y, uint32_t codepoint,
                   int scale, uint16_t foreground, uint16_t background)
{
    uint32_t count = 0;
    const uint8_t *record = find_glyph(codepoint, &count);
    if (record == NULL || scale <= 0) return false;
    const uint32_t offset = read_u32(record + 2);
    const uint8_t width = record[6];
    const uint8_t height = record[7];
    const size_t bitmap_base = FONT_HEADER_SIZE + (size_t)count * FONT_RECORD_SIZE;
    const size_t byte_count = ((size_t)width * height + 1) / 2;
    if (bitmap_base + offset + byte_count > (size_t)(cjk_font_end - cjk_font_start)) {
        return false;
    }
    const uint8_t *bitmap = cjk_font_start + bitmap_base + offset;
    const int output_size = 8 * scale;
    const int source_x0 = (FONT_CANVAS_SIZE - width) / 2;
    const int source_y0 = (FONT_CANVAS_SIZE - height) / 2;
    for (int py = 0; py < output_size; ++py) {
        int source_y = py * FONT_CANVAS_SIZE / output_size - source_y0;
        for (int px = 0; px < output_size; ++px) {
            int source_x = px * FONT_CANVAS_SIZE / output_size - source_x0;
            uint8_t coverage = 0;
            if (source_x >= 0 && source_y >= 0 && source_x < width &&
                source_y < height) {
                size_t pixel = (size_t)source_y * width + source_x;
                uint8_t packed = bitmap[pixel / 2];
                coverage = (pixel & 1U) == 0 ? packed >> 4 : packed & 0x0f;
            }
            gfx_pixel(canvas, x + px, y + py,
                      blend_rgb565(background, foreground, coverage));
        }
    }
    return true;
}
