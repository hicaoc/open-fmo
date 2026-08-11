#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FMO_FRAME_OPUS = 1,
    FMO_FRAME_ADPCM = 2,
} fmo_frame_codec_t;

typedef struct {
    char callsign[7];
    uint16_t session;
    uint32_t started_at;
    uint32_t timestamp;
    uint16_t block_count;
    uint8_t buffer_depth;
} fmo_frame_info_t;

typedef bool (*fmo_frame_block_handler_t)(void *context,
                                          fmo_frame_codec_t codec,
                                          const uint8_t *data,
                                          size_t data_size,
                                          int16_t adpcm_sample,
                                          uint8_t adpcm_index);

bool fmo_frame_parse(const uint8_t *frame, size_t frame_size,
                     fmo_frame_info_t *info,
                     fmo_frame_block_handler_t handler, void *context);

size_t fmo_frame_build_opus(uint8_t *output, size_t capacity,
                            const char *callsign, uint16_t session,
                            uint32_t started_at, uint32_t timestamp,
                            const uint8_t *const packets[],
                            const size_t packet_sizes[], size_t packet_count,
                            uint8_t buffer_depth);
