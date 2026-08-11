#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

typedef enum {
    ENCODER_EVENT_CLOCKWISE,
    ENCODER_EVENT_COUNTER_CLOCKWISE,
    ENCODER_EVENT_PRESS,
    ENCODER_EVENT_LONG_PRESS,
    ENCODER_EVENT_BOOT_PRESS,
} encoder_event_type_t;

typedef struct {
    encoder_event_type_t type;
} encoder_event_t;

esp_err_t encoder_init(QueueHandle_t *event_queue);
