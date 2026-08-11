#include "status_io.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SLOW_BLINK_MS 500U
#define HEARTBEAT_MISSED_MS 6500U

static SemaphoreHandle_t s_lock;
static uint32_t s_last_heartbeat_ms;
static bool s_network_ptt;
static bool s_ctcss_required;
static bool s_ctcss_matched;
static bool s_ctcss_rejected;
static uint32_t s_carrier_generation;
static volatile uint8_t s_vu_mic;
static volatile uint8_t s_vu_spk;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int led_active_level(gpio_num_t pin)
{
    if (pin == FMO_LED2) return FMO_LED2_ACTIVE_LEVEL;
    if (pin == FMO_LED3) return FMO_LED3_ACTIVE_LEVEL;
    return FMO_LED1_ACTIVE_LEVEL;
}

static void write_led(gpio_num_t pin, bool on)
{
    const int active = led_active_level(pin);
    gpio_set_level(pin, on ? active : !active);
}

static void status_task(void *argument)
{
    (void)argument;
    int last_sql_level = -1;
    while (true) {
        uint32_t now = now_ms();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t heartbeat = s_last_heartbeat_ms;
        bool network_ptt = s_network_ptt;
        xSemaphoreGive(s_lock);

        bool heartbeat_ok = heartbeat != 0 && now - heartbeat <= HEARTBEAT_MISSED_MS;
        bool blink = ((now / SLOW_BLINK_MS) & 1U) == 0;
        int sql_level = gpio_get_level(FMO_NET_PTT);
        bool raw_sql_active = sql_level == 1;
        if (sql_level != last_sql_level) {
            ESP_LOGI("status_io", "radio SQL GPIO%d=%d active=%d",
                     FMO_NET_PTT, sql_level, raw_sql_active ? 1 : 0);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ++s_carrier_generation;
            if (raw_sql_active && s_ctcss_required) {
                s_ctcss_matched = false;
                s_ctcss_rejected = false;
            }
            xSemaphoreGive(s_lock);
            last_sql_level = sql_level;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool sql_active = raw_sql_active &&
                          (!s_ctcss_required || s_ctcss_matched);
        xSemaphoreGive(s_lock);
        write_led(FMO_LED1, network_ptt);
        write_led(FMO_LED2, sql_active);
        write_led(FMO_LED3, heartbeat_ok ? true : blink);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t status_io_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    write_led(FMO_LED1, false);
    write_led(FMO_LED2, false);
    write_led(FMO_LED3, false);
    return xTaskCreate(status_task, "status_io", 2048, NULL, 3, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void status_io_notify_heartbeat(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_heartbeat_ms = now_ms();
    xSemaphoreGive(s_lock);
}

void status_io_set_network_ptt(bool active)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_network_ptt = active;
    xSemaphoreGive(s_lock);
    gpio_set_level(FMO_RADIO_PTT, active ? 1 : 0);
    write_led(FMO_LED1, active);
}

bool status_io_is_sql_active(void)
{
    if (gpio_get_level(FMO_NET_PTT) != 1) return false;
    /* CTCSS gate: if required, only active when matched */
    if (s_lock == NULL) return true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = !s_ctcss_required || s_ctcss_matched;
    xSemaphoreGive(s_lock);
    return active;
}

bool status_io_is_raw_sql_active(void)
{
    return gpio_get_level(FMO_NET_PTT) == 1;
}

bool status_io_is_network_ptt(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_network_ptt;
    xSemaphoreGive(s_lock);
    return active;
}

uint32_t status_io_carrier_generation(void)
{
    if (s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t generation = s_carrier_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

void status_io_configure_ctcss_gate(bool required)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctcss_required = required;
    s_ctcss_matched = !required;
    s_ctcss_rejected = false;
    xSemaphoreGive(s_lock);
}

void status_io_set_ctcss_gate(bool matched, bool rejected)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctcss_matched = matched;
    s_ctcss_rejected = rejected;
    xSemaphoreGive(s_lock);
}

void status_io_reset_heartbeat(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_heartbeat_ms = 0;
    xSemaphoreGive(s_lock);
}

void status_io_set_vu(uint8_t mic_level, uint8_t spk_level)
{
    s_vu_mic = mic_level;
    s_vu_spk = spk_level;
}

void status_io_get_vu(uint8_t *mic_level, uint8_t *spk_level)
{
    if (mic_level) *mic_level = s_vu_mic;
    if (spk_level) *spk_level = s_vu_spk;
}
