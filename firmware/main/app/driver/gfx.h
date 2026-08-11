#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int width;
    int height;
    uint16_t *pixels;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
} gfx_canvas_t;

esp_err_t gfx_canvas_create(gfx_canvas_t *canvas, int width, int height);
void gfx_canvas_destroy(gfx_canvas_t *canvas);
void gfx_clear(gfx_canvas_t *canvas, uint16_t color);
void gfx_pixel(gfx_canvas_t *canvas, int x, int y, uint16_t color);
void gfx_hline(gfx_canvas_t *canvas, int x, int y, int width, uint16_t color);
void gfx_rect(gfx_canvas_t *canvas, int x, int y, int width, int height,
              uint16_t color);
void gfx_fill_rect(gfx_canvas_t *canvas, int x, int y, int width, int height,
                   uint16_t color);
void gfx_set_clip(gfx_canvas_t *canvas, int x, int y, int width, int height);
void gfx_reset_clip(gfx_canvas_t *canvas);
void gfx_text(gfx_canvas_t *canvas, int x, int y, const char *text,
              int scale, uint16_t foreground, uint16_t background,
              int max_width);
int gfx_text_width(const char *text, int scale);
