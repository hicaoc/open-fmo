#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Shared board I2C bus. The factory firmware and the successful early probe
 * both use ESP-IDF's legacy command-link driver on this hardware. */
esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_probe(uint8_t address, int timeout_ms);
esp_err_t i2c_bus_write(uint8_t address, const uint8_t *data, size_t size,
                        int timeout_ms);
esp_err_t i2c_bus_write_read(uint8_t address,
                             const uint8_t *write_data, size_t write_size,
                             uint8_t *read_data, size_t read_size,
                             int timeout_ms);
