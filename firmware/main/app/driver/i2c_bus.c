#include "i2c_bus.h"

#include <stdbool.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";
static SemaphoreHandle_t s_mutex;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_codec;

static esp_err_t ensure_mutex(void)
{
    if (s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t ensure_bus_locked(void)
{
    if (s_bus != NULL) return ESP_OK;
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = FMO_I2C_SDA,
        .scl_io_num = FMO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &s_bus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "master bus ready: port=0 sda=%d scl=%d speed=%lu",
                 FMO_I2C_SDA, FMO_I2C_SCL,
                 (unsigned long)FMO_I2C_FREQ_HZ);
        ESP_LOGI(TAG, "idle levels: sda=%d scl=%d",
                 gpio_get_level(FMO_I2C_SDA), gpio_get_level(FMO_I2C_SCL));
    }
    return err;
}

static esp_err_t get_device_locked(uint8_t address,
                                   i2c_master_dev_handle_t *device)
{
    if (device == NULL || address > 0x7f) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_bus_locked();
    if (err != ESP_OK) return err;
    if (address != FMO_ES8311_ADDR) return ESP_ERR_NOT_SUPPORTED;
    if (s_codec == NULL) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = FMO_I2C_FREQ_HZ,
        };
        err = i2c_master_bus_add_device(s_bus, &config, &s_codec);
        if (err != ESP_OK) return err;
    }
    *device = s_codec;
    return ESP_OK;
}

esp_err_t i2c_bus_init(void)
{
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = ensure_bus_locked();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_probe(uint8_t address, int timeout_ms)
{
    if (address > 0x7f) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    err = i2c_master_probe(s_bus, address, timeout_ms);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write(uint8_t address, const uint8_t *data, size_t size,
                        int timeout_ms)
{
    if (data == NULL || size == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    i2c_master_dev_handle_t device = NULL;
    err = get_device_locked(address, &device);
    if (err == ESP_OK) {
        err = i2c_master_transmit(device, data, size, timeout_ms);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write_read(uint8_t address,
                             const uint8_t *write_data, size_t write_size,
                             uint8_t *read_data, size_t read_size,
                             int timeout_ms)
{
    if (write_data == NULL || write_size == 0 ||
        read_data == NULL || read_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    i2c_master_dev_handle_t device = NULL;
    err = get_device_locked(address, &device);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(device,
                                          write_data, write_size,
                                          read_data, read_size,
                                          timeout_ms);
    }
    xSemaphoreGive(s_mutex);
    return err;
}
