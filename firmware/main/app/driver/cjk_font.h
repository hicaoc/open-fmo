#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

bool cjk_font_draw(gfx_canvas_t *canvas, int x, int y, uint32_t codepoint,
                   int scale, uint16_t foreground, uint16_t background);
