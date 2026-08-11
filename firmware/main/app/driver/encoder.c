#include "encoder.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "encoder";
static QueueHandle_t s_queue;

static void send_event(encoder_event_type_t type)
{
    const encoder_event_t event = {.type = type};
    (void)xQueueSend(s_queue, &event, 0);
}

static void encoder_task(void *arg)
{
    (void)arg;
    static const int8_t transition[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0,
    };

    uint8_t previous = (gpio_get_level(FMO_ENCODER_A) << 1) |
                       gpio_get_level(FMO_ENCODER_B);
    int accumulator = 0;
    bool raw_pressed = gpio_get_level(FMO_ENCODER_SW) == 0;
    bool stable_pressed = raw_pressed;
    TickType_t raw_changed_at = xTaskGetTickCount();
    TickType_t pressed_at = raw_changed_at;
    bool long_sent = false;

    while (true) {
        const uint8_t current = (gpio_get_level(FMO_ENCODER_A) << 1) |
                                gpio_get_level(FMO_ENCODER_B);
        if (current != previous) {
            accumulator += transition[(previous << 2) | current];
            previous = current;
            if (accumulator >= 4) {
                send_event(ENCODER_EVENT_CLOCKWISE);
                accumulator = 0;
            } else if (accumulator <= -4) {
                send_event(ENCODER_EVENT_COUNTER_CLOCKWISE);
                accumulator = 0;
            }
        }

        const TickType_t now = xTaskGetTickCount();
        const bool sample_pressed = gpio_get_level(FMO_ENCODER_SW) == 0;
        if (sample_pressed != raw_pressed) {
            raw_pressed = sample_pressed;
            raw_changed_at = now;
        }
        if (sample_pressed != stable_pressed &&
            now - raw_changed_at >= pdMS_TO_TICKS(25)) {
            stable_pressed = sample_pressed;
            if (stable_pressed) {
                pressed_at = now;
                long_sent = false;
            } else if (!long_sent) {
                send_event(ENCODER_EVENT_PRESS);
            }
        }
        if (stable_pressed && !long_sent &&
            now - pressed_at >= pdMS_TO_TICKS(800)) {
            long_sent = true;
            send_event(ENCODER_EVENT_LONG_PRESS);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t encoder_init(QueueHandle_t *event_queue)
{
    if (event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << FMO_ENCODER_A) |
                        (1ULL << FMO_ENCODER_B) |
                        (1ULL << FMO_ENCODER_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "GPIO config failed");

    s_queue = xQueueCreate(16, sizeof(encoder_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(encoder_task, "encoder", 3072, NULL, 8, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    *event_queue = s_queue;
    ESP_LOGI(TAG, "A=%d B=%d SW=%d", FMO_ENCODER_A, FMO_ENCODER_B,
             FMO_ENCODER_SW);
    return ESP_OK;
}
