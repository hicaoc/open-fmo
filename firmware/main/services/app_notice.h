#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_NOTICE_NONE = 0,
    APP_NOTICE_APRS,
    APP_NOTICE_INFO,
} app_notice_kind_t;

typedef struct {
    app_notice_kind_t kind;
    char text[128];
    uint32_t remaining_ms;
} app_notice_t;

void app_notice_post_aprs(const char *source, const char *message);
void app_notice_post_info(const char *message, uint32_t duration_ms);
bool app_notice_get(app_notice_t *notice);
