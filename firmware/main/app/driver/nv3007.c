#include "nv3007.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t s_lcd;
/* DMA line buffer pool: flush queues one transaction per row and reaps them
 * as they complete, so the CPU sleeps in the driver instead of spinning in
 * spi_device_polling_transmit for ~29ms per frame. */
#define LCD_DMA_LINE_COUNT 8
#define LCD_LINE_BYTES (FMO_LCD_WIDTH * 2)
static DMA_ATTR uint8_t s_dma_lines[LCD_DMA_LINE_COUNT][LCD_LINE_BYTES];

enum {
    INIT_CMD = 0,
    INIT_DATA = 1,
    INIT_DELAY_MS = 4,
    INIT_END = 5,
};

/* Exact NV3007 init operations extracted from the original firmware. */
static const uint8_t s_nv3007_init[] = {
    0x00, 0xFF, 0x01, 0xA5, 0x00, 0x9A, 0x01, 0x08, 0x00, 0x9B, 0x01, 0x08, 0x00, 0x9C, 0x01, 0xB0,
    0x00, 0x9D, 0x01, 0x16, 0x00, 0x9E, 0x01, 0xC4, 0x00, 0x8F, 0x01, 0x55, 0x01, 0x04, 0x00, 0x84,
    0x01, 0x90, 0x00, 0x83, 0x01, 0x7B, 0x00, 0x85, 0x01, 0x33, 0x00, 0x60, 0x01, 0x00, 0x00, 0x70,
    0x01, 0x00, 0x00, 0x61, 0x01, 0x02, 0x00, 0x71, 0x01, 0x02, 0x00, 0x62, 0x01, 0x04, 0x00, 0x72,
    0x01, 0x04, 0x00, 0x6C, 0x01, 0x29, 0x00, 0x7C, 0x01, 0x29, 0x00, 0x6D, 0x01, 0x31, 0x00, 0x7D,
    0x01, 0x31, 0x00, 0x6E, 0x01, 0x0F, 0x00, 0x7E, 0x01, 0x0F, 0x00, 0x66, 0x01, 0x21, 0x00, 0x76,
    0x01, 0x21, 0x00, 0x68, 0x01, 0x3A, 0x00, 0x78, 0x01, 0x3A, 0x00, 0x63, 0x01, 0x07, 0x00, 0x73,
    0x01, 0x07, 0x00, 0x64, 0x01, 0x05, 0x00, 0x74, 0x01, 0x05, 0x00, 0x65, 0x01, 0x02, 0x00, 0x75,
    0x01, 0x02, 0x00, 0x67, 0x01, 0x23, 0x00, 0x77, 0x01, 0x23, 0x00, 0x69, 0x01, 0x08, 0x00, 0x79,
    0x01, 0x08, 0x00, 0x6A, 0x01, 0x13, 0x00, 0x7A, 0x01, 0x13, 0x00, 0x6B, 0x01, 0x13, 0x00, 0x7B,
    0x01, 0x13, 0x00, 0x6F, 0x01, 0x00, 0x00, 0x7F, 0x01, 0x00, 0x00, 0x50, 0x01, 0x00, 0x00, 0x52,
    0x01, 0xD6, 0x00, 0x53, 0x01, 0x08, 0x00, 0x54, 0x01, 0x08, 0x00, 0x55, 0x01, 0x1E, 0x00, 0x56,
    0x01, 0x1C, 0x00, 0xA0, 0x01, 0x2B, 0x01, 0x24, 0x01, 0x00, 0x00, 0xA1, 0x01, 0x87, 0x00, 0xA2,
    0x01, 0x86, 0x00, 0xA5, 0x01, 0x00, 0x00, 0xA6, 0x01, 0x00, 0x00, 0xA7, 0x01, 0x00, 0x00, 0xA8,
    0x01, 0x36, 0x00, 0xA9, 0x01, 0x7E, 0x00, 0xAA, 0x01, 0x7E, 0x00, 0xB9, 0x01, 0x85, 0x00, 0xBA,
    0x01, 0x84, 0x00, 0xBB, 0x01, 0x83, 0x00, 0xBC, 0x01, 0x82, 0x00, 0xBD, 0x01, 0x81, 0x00, 0xBE,
    0x01, 0x80, 0x00, 0xBF, 0x01, 0x01, 0x00, 0xC0, 0x01, 0x02, 0x00, 0xC1, 0x01, 0x00, 0x00, 0xC2,
    0x01, 0x00, 0x00, 0xC3, 0x01, 0x00, 0x00, 0xC4, 0x01, 0x33, 0x00, 0xC5, 0x01, 0x7E, 0x00, 0xC6,
    0x01, 0x7E, 0x00, 0xC8, 0x01, 0x33, 0x01, 0x33, 0x00, 0xC9, 0x01, 0x68, 0x00, 0xCA, 0x01, 0x69,
    0x00, 0xCB, 0x01, 0x6A, 0x00, 0xCC, 0x01, 0x6B, 0x00, 0xCD, 0x01, 0x33, 0x01, 0x33, 0x00, 0xCE,
    0x01, 0x6C, 0x00, 0xCF, 0x01, 0x6D, 0x00, 0xD0, 0x01, 0x6E, 0x00, 0xD1, 0x01, 0x6F, 0x00, 0xAB,
    0x01, 0x03, 0x01, 0x67, 0x00, 0xAC, 0x01, 0x03, 0x01, 0x6B, 0x00, 0xAD, 0x01, 0x03, 0x01, 0x68,
    0x00, 0xAE, 0x01, 0x03, 0x01, 0x6C, 0x00, 0xB3, 0x01, 0x00, 0x00, 0xB4, 0x01, 0x00, 0x00, 0xB5,
    0x01, 0x00, 0x00, 0xB6, 0x01, 0x32, 0x00, 0xB7, 0x01, 0x7E, 0x00, 0xB8, 0x01, 0x7E, 0x00, 0xE0,
    0x01, 0x00, 0x00, 0xE1, 0x01, 0x03, 0x01, 0x0F, 0x00, 0xE2, 0x01, 0x04, 0x00, 0xE3, 0x01, 0x01,
    0x00, 0xE4, 0x01, 0x0E, 0x00, 0xE5, 0x01, 0x01, 0x00, 0xE6, 0x01, 0x19, 0x00, 0xE7, 0x01, 0x10,
    0x00, 0xE8, 0x01, 0x10, 0x00, 0xEA, 0x01, 0x12, 0x00, 0xEB, 0x01, 0xD0, 0x00, 0xEC, 0x01, 0x04,
    0x00, 0xED, 0x01, 0x07, 0x00, 0xEE, 0x01, 0x07, 0x00, 0xEF, 0x01, 0x09, 0x00, 0xF0, 0x01, 0xD0,
    0x00, 0xF1, 0x01, 0x0E, 0x01, 0x17, 0x00, 0xF2, 0x01, 0x2C, 0x01, 0x1B, 0x01, 0x0B, 0x01, 0x20,
    0x00, 0xE9, 0x01, 0x29, 0x00, 0xEC, 0x01, 0x04, 0x00, 0x35, 0x01, 0x00, 0x00, 0x44, 0x01, 0x00,
    0x01, 0x10, 0x00, 0x46, 0x01, 0x10, 0x00, 0xFF, 0x01, 0x00, 0x00, 0x3A, 0x01, 0x05, 0x00, 0x36,
    0x01, 0xA0, 0x00, 0x11, 0x04, 0xDC, 0x00, 0x29, 0x04, 0xC8, 0x05, 0x00,
};

static esp_err_t lcd_tx(bool data, const void *buffer, size_t length)
{
    gpio_set_level(FMO_LCD_DC, data ? 1 : 0);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = buffer,
    };
    return spi_device_polling_transmit(s_lcd, &transaction);
}

static esp_err_t lcd_cmd(uint8_t command)
{
    return lcd_tx(false, &command, 1);
}

static esp_err_t lcd_data(const void *data, size_t length)
{
    return lcd_tx(true, data, length);
}

esp_err_t nv3007_init(void)
{
    gpio_config_t backlight = {
        .pin_bit_mask = 1ULL << FMO_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight), "lcd", "backlight GPIO config failed");
    gpio_set_level(FMO_LCD_BL, !FMO_LCD_BL_ACTIVE_LEVEL);

    gpio_config_t controls = {
        .pin_bit_mask = (1ULL << FMO_LCD_DC) | (1ULL << FMO_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&controls), "lcd", "control GPIO config failed");
    gpio_set_level(FMO_LCD_DC, 1);
    gpio_set_level(FMO_LCD_RST, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = FMO_LCD_SPI_MOSI,
        .miso_io_num = FMO_LCD_SPI_MISO,
        .sclk_io_num = FMO_LCD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_LINE_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "lcd", "SPI bus init failed");

    spi_device_interface_config_t device = {
        .clock_speed_hz = FMO_LCD_SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = FMO_LCD_SPI_CS,
        .queue_size = LCD_DMA_LINE_COUNT,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device, &s_lcd), "lcd", "SPI device add failed");

    /* The original firmware performs this exact 1/0/1 reset sequence. */
    gpio_set_level(FMO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(FMO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(FMO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    for (size_t i = 0; i + 1 < sizeof(s_nv3007_init); i += 2) {
        uint8_t op = s_nv3007_init[i];
        uint8_t value = s_nv3007_init[i + 1];
        if (op == INIT_CMD) {
            ESP_RETURN_ON_ERROR(lcd_cmd(value), "lcd", "init command failed");
        } else if (op == INIT_DATA) {
            ESP_RETURN_ON_ERROR(lcd_data(&value, 1), "lcd", "init data failed");
        } else if (op == INIT_DELAY_MS) {
            vTaskDelay(pdMS_TO_TICKS(value));
        } else if (op == INIT_END) {
            break;
        }
    }
    /* 0xA0 is reverse-landscape for this physical panel mounting. */
    const uint8_t madctl = FMO_LCD_MADCTL_LANDSCAPE;
    ESP_RETURN_ON_ERROR(lcd_cmd(0x36), "lcd", "MADCTL command failed");
    ESP_RETURN_ON_ERROR(lcd_data(&madctl, 1), "lcd", "MADCTL data failed");
    return nv3007_set_backlight(true);
}

static esp_err_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t coordinates[4];
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), "lcd", "CASET failed");
    coordinates[0] = x0 >> 8;
    coordinates[1] = x0;
    coordinates[2] = x1 >> 8;
    coordinates[3] = x1;
    ESP_RETURN_ON_ERROR(lcd_data(coordinates, sizeof(coordinates)), "lcd", "CASET data failed");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), "lcd", "RASET failed");
    coordinates[0] = y0 >> 8;
    coordinates[1] = y0;
    coordinates[2] = y1 >> 8;
    coordinates[3] = y1;
    ESP_RETURN_ON_ERROR(lcd_data(coordinates, sizeof(coordinates)), "lcd", "RASET data failed");
    return lcd_cmd(0x2C);
}

esp_err_t nv3007_set_backlight(bool enabled)
{
    return gpio_set_level(FMO_LCD_BL,
                          enabled ? FMO_LCD_BL_ACTIVE_LEVEL
                                  : !FMO_LCD_BL_ACTIVE_LEVEL);
}

esp_err_t nv3007_set_backlight_percent(uint8_t percent)
{
    /* GPIO34 is confirmed as a binary enable. PWM brightness can be added
     * after checking whether the board's backlight stage accepts it. */
    return nv3007_set_backlight(percent != 0);
}

esp_err_t nv3007_flush_rgb565(const uint16_t *pixels, int width, int height)
{
    if (pixels == NULL || width != FMO_LCD_WIDTH || height != FMO_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(
        lcd_set_window(FMO_LCD_LANDSCAPE_OFFSET_X,
                       FMO_LCD_LANDSCAPE_OFFSET_Y,
                       FMO_LCD_LANDSCAPE_OFFSET_X + width - 1,
                       FMO_LCD_LANDSCAPE_OFFSET_Y + height - 1),
        "lcd", "set window failed");

    gpio_set_level(FMO_LCD_DC, 1);
    /* Queue one DMA transaction per row from the buffer pool. Reap the oldest
     * transaction before reusing its slot; between reaps the CPU sleeps in
     * the SPI driver instead of spinning on polling_transmit. */
    spi_transaction_t transactions[LCD_DMA_LINE_COUNT];
    esp_err_t error = ESP_OK;
    int queued = 0;
    int reaped = 0;
    for (int y = 0; y < height && error == ESP_OK; ++y) {
        if (queued - reaped == LCD_DMA_LINE_COUNT) {
            spi_transaction_t *done = NULL;
            error = spi_device_get_trans_result(s_lcd, &done, portMAX_DELAY);
            if (error != ESP_OK) break;
            ++reaped;
        }
        const int slot = queued % LCD_DMA_LINE_COUNT;
        uint8_t *line = s_dma_lines[slot];
        const uint16_t *source = pixels + y * width;
        for (int x = 0; x < width; ++x) {
            const uint16_t color = source[x];
            line[x * 2] = color >> 8;
            line[x * 2 + 1] = color;
        }
        spi_transaction_t *transaction = &transactions[slot];
        memset(transaction, 0, sizeof(*transaction));
        transaction->length = (size_t)width * 2 * 8;
        transaction->tx_buffer = line;
        error = spi_device_queue_trans(s_lcd, transaction, portMAX_DELAY);
        if (error == ESP_OK) ++queued;
    }
    while (reaped < queued) {
        spi_transaction_t *done = NULL;
        if (spi_device_get_trans_result(s_lcd, &done, portMAX_DELAY) != ESP_OK) {
            break;
        }
        ++reaped;
    }
    if (error != ESP_OK) {
        ESP_LOGE("lcd", "DMA flush failed: %s", esp_err_to_name(error));
    }
    return error;
}
