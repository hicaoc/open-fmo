#include "gfx.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "cjk_font.h"

/* 5x7 column font, ASCII 0x20..0x5f. Lowercase is rendered as uppercase to
 * keep the phase-0 renderer compact. */
static const uint8_t k_font[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},
    {0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},
    {0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},
    {0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},
    {0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},
    {0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
};

esp_err_t gfx_canvas_create(gfx_canvas_t *canvas, int width, int height)
{
    if (canvas == NULL || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;
    const size_t size = (size_t)width * height * sizeof(uint16_t);
    uint16_t *pixels = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) pixels = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (pixels == NULL) return ESP_ERR_NO_MEM;
    canvas->width = width;
    canvas->height = height;
    canvas->pixels = pixels;
    gfx_reset_clip(canvas);
    return ESP_OK;
}

void gfx_canvas_destroy(gfx_canvas_t *canvas)
{
    if (canvas != NULL) {
        free(canvas->pixels);
        memset(canvas, 0, sizeof(*canvas));
    }
}

void gfx_clear(gfx_canvas_t *canvas, uint16_t color)
{
    if (canvas == NULL || canvas->pixels == NULL) return;
    const int count = canvas->width * canvas->height;
    for (int i = 0; i < count; ++i) canvas->pixels[i] = color;
}

void gfx_pixel(gfx_canvas_t *canvas, int x, int y, uint16_t color)
{
    if (canvas == NULL || canvas->pixels == NULL || x < canvas->clip_x0 ||
        y < canvas->clip_y0 || x >= canvas->clip_x1 || y >= canvas->clip_y1) return;
    canvas->pixels[y * canvas->width + x] = color;
}

void gfx_hline(gfx_canvas_t *canvas, int x, int y, int width, uint16_t color)
{
    gfx_fill_rect(canvas, x, y, width, 1, color);
}

void gfx_fill_rect(gfx_canvas_t *canvas, int x, int y, int width, int height,
                   uint16_t color)
{
    if (canvas == NULL || canvas->pixels == NULL || width <= 0 || height <= 0) return;
    int x0 = x < canvas->clip_x0 ? canvas->clip_x0 : x;
    int y0 = y < canvas->clip_y0 ? canvas->clip_y0 : y;
    int x1 = x + width > canvas->clip_x1 ? canvas->clip_x1 : x + width;
    int y1 = y + height > canvas->clip_y1 ? canvas->clip_y1 : y + height;
    for (int py = y0; py < y1; ++py) {
        uint16_t *row = canvas->pixels + py * canvas->width;
        for (int px = x0; px < x1; ++px) row[px] = color;
    }
}

void gfx_set_clip(gfx_canvas_t *canvas, int x, int y, int width, int height)
{
    if (canvas == NULL) return;
    canvas->clip_x0 = x < 0 ? 0 : x;
    canvas->clip_y0 = y < 0 ? 0 : y;
    canvas->clip_x1 = x + width > canvas->width ? canvas->width : x + width;
    canvas->clip_y1 = y + height > canvas->height ? canvas->height : y + height;
}

void gfx_reset_clip(gfx_canvas_t *canvas)
{
    if (canvas == NULL) return;
    canvas->clip_x0 = 0;
    canvas->clip_y0 = 0;
    canvas->clip_x1 = canvas->width;
    canvas->clip_y1 = canvas->height;
}

void gfx_rect(gfx_canvas_t *canvas, int x, int y, int width, int height,
              uint16_t color)
{
    gfx_hline(canvas, x, y, width, color);
    gfx_hline(canvas, x, y + height - 1, width, color);
    gfx_fill_rect(canvas, x, y, 1, height, color);
    gfx_fill_rect(canvas, x + width - 1, y, 1, height, color);
}

static const uint8_t *glyph_for(char value)
{
    unsigned char c = (unsigned char)value;
    if (c >= 'a' && c <= 'z') c = (unsigned char)toupper(c);
    if (c < 0x20 || c > 0x5f) c = '?';
    return k_font[c - 0x20];
}

int gfx_text_width(const char *text, int scale)
{
    if (text == NULL || scale <= 0) return 0;
    int width = 0;
    const uint8_t *cursor = (const uint8_t *)text;
    while (*cursor != 0) {
        if (*cursor < 0x80) {
            ++cursor;
            width += 6 * scale;
        } else {
            if ((*cursor & 0xe0) == 0xc0 && cursor[1] != 0) cursor += 2;
            else if ((*cursor & 0xf0) == 0xe0 && cursor[1] != 0 && cursor[2] != 0) cursor += 3;
            else if ((*cursor & 0xf8) == 0xf0 && cursor[1] != 0 && cursor[2] != 0 && cursor[3] != 0) cursor += 4;
            else ++cursor;
            width += 9 * scale;
        }
    }
    return width;
}

static uint32_t utf8_next(const char **text)
{
    const uint8_t *cursor = (const uint8_t *)*text;
    uint32_t codepoint;
    if (cursor[0] < 0x80) {
        codepoint = cursor[0];
        *text += 1;
    } else if ((cursor[0] & 0xe0) == 0xc0 && cursor[1] != 0) {
        codepoint = ((uint32_t)(cursor[0] & 0x1f) << 6) | (cursor[1] & 0x3f);
        *text += 2;
    } else if ((cursor[0] & 0xf0) == 0xe0 && cursor[1] != 0 && cursor[2] != 0) {
        codepoint = ((uint32_t)(cursor[0] & 0x0f) << 12) |
                    ((uint32_t)(cursor[1] & 0x3f) << 6) | (cursor[2] & 0x3f);
        *text += 3;
    } else if ((cursor[0] & 0xf8) == 0xf0 && cursor[1] != 0 &&
               cursor[2] != 0 && cursor[3] != 0) {
        codepoint = ((uint32_t)(cursor[0] & 0x07) << 18) |
                    ((uint32_t)(cursor[1] & 0x3f) << 12) |
                    ((uint32_t)(cursor[2] & 0x3f) << 6) | (cursor[3] & 0x3f);
        *text += 4;
    } else {
        codepoint = '?';
        *text += 1;
    }
    return codepoint;
}

void gfx_text(gfx_canvas_t *canvas, int x, int y, const char *text,
              int scale, uint16_t foreground, uint16_t background,
              int max_width)
{
    if (canvas == NULL || text == NULL || scale <= 0) return;
    const int start_x = x;
    while (*text != '\0') {
        const char *next = text;
        uint32_t codepoint = utf8_next(&next);
        if (codepoint >= 0x80) {
            if (max_width > 0 && x + 8 * scale > start_x + max_width) break;
            if (cjk_font_draw(canvas, x, y, codepoint, scale,
                              foreground, background)) {
                gfx_fill_rect(canvas, x + 8 * scale, y, scale, 8 * scale, background);
                x += 9 * scale;
                text = next;
                continue;
            }
        }
        if (max_width > 0 && x + 5 * scale > start_x + max_width) break;
        const uint8_t *glyph = glyph_for((char)codepoint);
        text = next;
        for (int column = 0; column < 5; ++column) {
            for (int row = 0; row < 7; ++row) {
                const uint16_t color = (glyph[column] & (1U << row))
                                           ? foreground : background;
                gfx_fill_rect(canvas, x + column * scale, y + row * scale,
                              scale, scale, color);
            }
        }
        gfx_fill_rect(canvas, x + 5 * scale, y, scale, 7 * scale, background);
        x += 6 * scale;
    }
}
