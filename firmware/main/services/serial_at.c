/* COM-port (USB Serial/JTAG console) AT command listener.
 *
 * The device console runs on the ESP32-S3 built-in USB Serial/JTAG
 * (UART0 is owned by the BK4802 radio), so the VFS stdin/stdout file
 * descriptors are the COM port. Lines typed there ("AT+KEY=VALUE",
 * terminated by CR or LF) are executed by the same dispatcher as NRL
 * network AT packets, with source NRL_AT_SOURCE_SERIAL so the
 * serial-only commands (WIFI_SSID, WIFI_PASS, OTAURL, REBOOT) work.
 *
 * The reference project exposes the identical behavior on its debug
 * serial port (processSerialAtLine in nrl_audio_bridge.cpp).
 */

#include "serial_at.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/nrl_at_commands.h"

static const char *TAG = "serial_at";

/* Longest accepted command line (reference uses 288 for RADIOADD).
 * Stack: the heaviest path is AT+READ=123, which runs the same handler
 * chain as nrl_link (config/NVS snapshots + OTA/network snapshots +
 * newlib float printf + reply send). 6 KB overflowed on the ESP-IDF 6.2
 * toolchain; use 16 KB like nrl_link. */
#define SERIAL_AT_LINE_MAX 288U
#define SERIAL_AT_STACK_BYTES 12288U

static StaticTask_t s_tcb;
/* The stack MUST stay in internal RAM: AT commands trigger NVS flash
 * writes, and a PSRAM stack trips the esp_task_stack_is_sane_cache_disabled()
 * assert while the cache is disabled. */
static StackType_t s_stack[SERIAL_AT_STACK_BYTES / sizeof(StackType_t)];

/* 1 KB reply object; data (not stack) so PSRAM is safe here. */
static EXT_RAM_BSS_ATTR nrl_at_result_t s_result;

/* Write the reply back to the console. The VFS TX path converts every
 * '\n' into CRLF, so the '\r' bytes already present in the payload are
 * skipped to avoid "\r\r\n". */
static void write_reply(const nrl_at_result_t *result)
{
    const int fd = fileno(stdout);
    for (size_t i = 0; i < result->payload_size; ++i) {
        const char ch = (char)result->payload[i];
        if (ch == '\r') continue;
        (void)write(fd, &ch, 1);
    }
}

static void process_line(char *line)
{
    while (*line == ' ' || *line == '\t') ++line;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) return;
    /* Only treat AT lines as commands; ignore anything else typed on the
     * console (log noise, stray characters). */
    if (strncasecmp(line, "AT", 2) != 0) return;

    ESP_LOGI(TAG, "console command: %s", line);
    nrl_at_handle_payload((const uint8_t *)line, len,
                          NRL_AT_SOURCE_SERIAL, &s_result);
    if (s_result.should_reply && s_result.payload_size > 0) {
        write_reply(&s_result);
    }
    if (s_result.reboot) {
        ESP_LOGI(TAG, "reboot requested via console AT");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

static void serial_at_task(void *arg)
{
    (void)arg;
    static char line[SERIAL_AT_LINE_MAX];
    size_t len = 0;
    const int fd = fileno(stdin);

    for (;;) {
        char ch = 0;
        const ssize_t n = read(fd, &ch, 1);
        if (n <= 0) {
            /* Console VFS read is non-blocking; poll gently. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                process_line(line);
                len = 0;
            }
            continue;
        }
        if (len < SERIAL_AT_LINE_MAX - 1) {
            line[len++] = ch;
        }
    }
}

esp_err_t serial_at_init(void)
{
    return xTaskCreateStatic(serial_at_task, "serial_at",
                             sizeof(s_stack) / sizeof(s_stack[0]),
                             NULL, 3, s_stack, &s_tcb) != NULL
               ? ESP_OK : ESP_ERR_NO_MEM;
}
