#pragma once

/* Single source of truth for this board variant. */
#include "fmo_pins.h"

#define FMO_LCD_HOST SPI2_HOST
#define FMO_LCD_NATIVE_WIDTH 142
#define FMO_LCD_NATIVE_HEIGHT 428
#define FMO_LCD_WIDTH 428
#define FMO_LCD_HEIGHT 142
#define FMO_LCD_LANDSCAPE_OFFSET_X 0
#define FMO_LCD_LANDSCAPE_OFFSET_Y 14
/* Reverse landscape: the panel is physically mounted 180 degrees from the
 * controller's 0x60 landscape orientation. */
#define FMO_LCD_MADCTL_LANDSCAPE 0xA0
