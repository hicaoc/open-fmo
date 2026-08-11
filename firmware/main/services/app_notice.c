#include "app_notice.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define APRS_NOTICE_DURATION_MS 15000U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_notice_t s_notice;
static uint32_t s_expires_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void post(app_notice_kind_t kind, const char *text, uint32_t duration_ms)
{
    if (text == NULL || text[0] == '\0' || duration_ms == 0) return;
    portENTER_CRITICAL(&s_lock);
    s_notice.kind = kind;
    strlcpy(s_notice.text, text, sizeof(s_notice.text));
    s_expires_ms = now_ms() + duration_ms;
    portEXIT_CRITICAL(&s_lock);
}

void app_notice_post_aprs(const char *source, const char *message)
{
    char text[sizeof(s_notice.text)];
    if (message == NULL || message[0] == '\0') return;
    if (source != NULL && source[0] != '\0') {
        snprintf(text, sizeof(text), "APRS %s: %s", source, message);
    } else {
        snprintf(text, sizeof(text), "APRS: %s", message);
    }
    post(APP_NOTICE_APRS, text, APRS_NOTICE_DURATION_MS);
}

void app_notice_post_info(const char *message, uint32_t duration_ms)
{
    post(APP_NOTICE_INFO, message, duration_ms);
}

bool app_notice_get(app_notice_t *notice)
{
    if (notice == NULL) return false;
    const uint32_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    const bool active = s_notice.kind != APP_NOTICE_NONE &&
                        (int32_t)(s_expires_ms - now) > 0;
    if (active) {
        *notice = s_notice;
        notice->remaining_ms = s_expires_ms - now;
    } else {
        memset(notice, 0, sizeof(*notice));
        s_notice.kind = APP_NOTICE_NONE;
    }
    portEXIT_CRITICAL(&s_lock);
    return active;
}
