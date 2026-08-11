#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define NRL_AT_REPLY_CAPACITY 1024U
#define NRL_PACKET_TYPE_AT_COMMAND 11U

typedef struct {
    bool should_reply;
    bool reboot;
    uint8_t payload[NRL_AT_REPLY_CAPACITY];
    size_t payload_size;
} nrl_at_result_t;

typedef enum {
    NRL_AT_SOURCE_REMOTE = 0,
    NRL_AT_SOURCE_SERIAL = 1,
} nrl_at_source_t;

void nrl_at_handle_payload(const uint8_t *payload, size_t payload_size,
                           nrl_at_source_t source, nrl_at_result_t *result);
