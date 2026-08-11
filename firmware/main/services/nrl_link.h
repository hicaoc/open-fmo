#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "services/config_store.h"

#define NRL_PACKET_HEADER_SIZE 48U
#define NRL_PACKET_TYPE_VOICE 1U
#define NRL_PACKET_TYPE_HEARTBEAT 2U
#define NRL_PACKET_TYPE_SERVER_HEARTBEAT 3U
#define NRL_PACKET_TYPE_TEXT 5U
#define NRL_PACKET_TYPE_OPUS_VOICE 8U
#define NRL_PACKET_TYPE_SERVER_VOICE 9U

typedef void (*nrl_link_packet_handler_t)(uint8_t type, const uint8_t *payload,
                                          size_t payload_size, void *context);

typedef struct {
    char callsign[7];
    uint8_t ssid;
    uint8_t voice_type;
    uint32_t dmr_id;
    uint32_t generation;
} nrl_remote_identity_t;

typedef struct {
    bool socket_ready;
    bool online;
    uint32_t last_reply_ms;
} nrl_link_status_t;

esp_err_t nrl_link_start(const fmo_config_t *config);
void nrl_link_update_config(const fmo_config_t *config);
esp_err_t nrl_link_send(uint8_t type, const uint8_t *payload,
                        size_t payload_size);
void nrl_link_set_packet_handler(nrl_link_packet_handler_t handler,
                                 void *context);
bool nrl_link_get_last_identity(nrl_remote_identity_t *identity);
void nrl_link_get_status(nrl_link_status_t *status);
