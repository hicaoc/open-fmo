#pragma once

#include "esp_err.h"

/**
 * Start the COM-port (USB Serial/JTAG console) AT command listener.
 * Lines typed on the serial console ("AT+KEY=VALUE", CR or LF
 * terminated) are executed exactly like NRL network AT packets, with
 * the reply printed back to the same console.
 */
esp_err_t serial_at_init(void);
