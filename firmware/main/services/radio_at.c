#include "radio_at.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "radio_at";
static SemaphoreHandle_t s_lock;

esp_err_t radio_at_init(void)
{
    const uart_config_t config = {
        .baud_rate = FMO_BK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(FMO_BK_UART_NUM, &config), TAG,
                        "UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(FMO_BK_UART_NUM, FMO_BK_UART_TX,
                                     FMO_BK_UART_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "UART pin config failed");
    esp_err_t error = uart_driver_install(FMO_BK_UART_NUM, 512, 0, 0, NULL, 0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d %d 8N1", FMO_BK_UART_NUM,
             FMO_BK_UART_TX, FMO_BK_UART_RX, FMO_BK_UART_BAUD);
    return ESP_OK;
}

esp_err_t radio_at_command(const char *command, char *response,
                           size_t response_size, uint32_t timeout_ms)
{
    if (command == NULL || response == NULL || response_size < 2 || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    response[0] = '\0';
    uart_flush_input(FMO_BK_UART_NUM);
    uart_write_bytes(FMO_BK_UART_NUM, command, strlen(command));
    uart_write_bytes(FMO_BK_UART_NUM, "\n", 1);
    uart_wait_tx_done(FMO_BK_UART_NUM, pdMS_TO_TICKS(100));

    size_t used = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t result = ESP_ERR_TIMEOUT;
    while (xTaskGetTickCount() < deadline && used + 1 < response_size) {
        uint8_t byte;
        int count = uart_read_bytes(FMO_BK_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (count != 1) continue;
        response[used++] = (char)byte;
        response[used] = '\0';
        if (strstr(response, "FAILED\n") || strstr(response, "INVALID\n")) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (strstr(response, "SUCCESS\n") || strstr(response, "OK\n")) {
            result = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGD(TAG, "%s -> %s", command, response);
    return result;
}

esp_err_t radio_at_probe(char *module_name, size_t module_name_size)
{
    if (module_name == NULL || module_name_size == 0) return ESP_ERR_INVALID_ARG;
    char response[96];
    ESP_RETURN_ON_ERROR(radio_at_command("AT+NAME?", response, sizeof(response), 500),
                        TAG, "radio module did not answer");
    const char *value = strstr(response, "NAME:");
    if (value == NULL) return ESP_ERR_INVALID_RESPONSE;
    value += strlen("NAME:");
    const char *end = strchr(value, '\n');
    size_t length = end ? (size_t)(end - value) : strlen(value);
    if (length >= module_name_size) length = module_name_size - 1;
    memcpy(module_name, value, length);
    module_name[length] = '\0';
    return ESP_OK;
}

static esp_err_t set_float(const char *name, float value)
{
    char command[48];
    char response[64];
    snprintf(command, sizeof(command), "AT+%s=%.4f", name, value);
    return radio_at_command(command, response, sizeof(response), 500);
}

esp_err_t radio_at_set_frequency(bool transmit, float mhz)
{
    return set_float(transmit ? "TXFREQ" : "RXFREQ", mhz);
}

esp_err_t radio_at_set_squelch(uint8_t level)
{
    if (level > 10) return ESP_ERR_INVALID_ARG;
    char command[24];
    char response[32];
    snprintf(command, sizeof(command), "AT+SQL=%u", level);
    return radio_at_command(command, response, sizeof(response), 500);
}

esp_err_t radio_at_set_volume(bool transmit, uint8_t level)
{
    if (level > 10) return ESP_ERR_INVALID_ARG;
    char command[24];
    char response[32];
    snprintf(command, sizeof(command), "AT+%sVOL=%u", transmit ? "TX" : "RX", level);
    return radio_at_command(command, response, sizeof(response), 500);
}

esp_err_t radio_at_set_tx_power(uint8_t level)
{
    static const char *const levels[] = {"LOW", "MID", "HIGH"};
    if (level >= sizeof(levels) / sizeof(levels[0])) return ESP_ERR_INVALID_ARG;
    char command[32];
    char response[32];
    snprintf(command, sizeof(command), "AT+TXPWR=%s", levels[level]);
    return radio_at_command(command, response, sizeof(response), 500);
}

esp_err_t radio_at_set_rf_enabled(bool enabled)
{
    char response[32];
    return radio_at_command(enabled ? "AT+RF=ENABLE" : "AT+RF=DISABLE",
                            response, sizeof(response), 500);
}

esp_err_t radio_at_set_freq_tune(int16_t hz)
{
    char command[32];
    char response[32];
    snprintf(command, sizeof(command), "AT+FREQTUNE=%d", (int)hz);
    return radio_at_command(command, response, sizeof(response), 500);
}
