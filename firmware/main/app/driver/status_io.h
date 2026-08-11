#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* BH4TDV-compatible meanings:
 * LED1: network audio is keying the local radio
 * LED2: local radio SQL / receiving
 * LED3: server heartbeat (solid online, slow blink offline)
 */
esp_err_t status_io_init(void);
void status_io_notify_heartbeat(void);
void status_io_reset_heartbeat(void);
void status_io_set_network_ptt(bool active);
bool status_io_is_sql_active(void);
bool status_io_is_raw_sql_active(void);
bool status_io_is_network_ptt(void);
uint32_t status_io_carrier_generation(void);
void status_io_configure_ctcss_gate(bool required);
void status_io_set_ctcss_gate(bool matched, bool rejected);

/* Audio VU levels (0-255 peak, updated by passthrough task). */
void status_io_set_vu(uint8_t mic_level, uint8_t spk_level);
void status_io_get_vu(uint8_t *mic_level, uint8_t *spk_level);
