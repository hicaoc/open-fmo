#include "fmo_frame.h"

#include <string.h>

#include "esp_rom_crc.h"

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

bool fmo_frame_parse(const uint8_t *frame, size_t frame_size,
                     fmo_frame_info_t *info,
                     fmo_frame_block_handler_t handler, void *context)
{
    static const uint8_t marker[5] = {0x3d, 0x14, 0x00, 0xe0, 0x3d};
    if (frame == NULL || info == NULL || frame_size < 64 ||
        frame[0] != 1 || read_le32(frame + 30) != frame_size ||
        esp_rom_crc32_le(0, frame + 64, frame_size - 64) !=
            read_le32(frame + 36)) return false;
    memset(info, 0, sizeof(*info));
    info->session = read_le16(frame + 6);
    memcpy(info->callsign, frame + 10, 6);
    info->callsign[6] = '\0';
    info->started_at = read_le32(frame + 22);
    info->timestamp = read_le32(frame + 26);
    info->block_count = read_le16(frame + 34);
    info->buffer_depth = frame[40];

    size_t offset = 64;
    uint16_t blocks = 0;
    while (offset < frame_size) {
        if (offset + 12 > frame_size) return false;
        size_t block_size = read_le16(frame + offset + 2);
        if (block_size < 16 || offset + block_size > frame_size) return false;
        const uint8_t *inner = frame + offset + 8;
        size_t inner_size = block_size - 8;
        size_t declared = read_le16(inner + 1);
        if (declared != inner_size || inner_size < 8 ||
            memcmp(inner + 3, marker, sizeof(marker)) != 0) return false;
        const uint8_t *payload = inner + 8;
        size_t payload_size = inner_size - 8;
        fmo_frame_codec_t codec = (fmo_frame_codec_t)inner[0];
        int16_t initial_sample = 0;
        uint8_t initial_index = 0;
        if (codec == FMO_FRAME_ADPCM) {
            if (payload_size != 328) return false;
            initial_sample = (int16_t)read_le16(payload + 2);
            initial_index = payload[4];
            payload += 8;
            payload_size -= 8;
        } else if (codec != FMO_FRAME_OPUS || payload_size == 0) {
            offset += block_size;
            ++blocks;
            continue;
        }
        if (handler != NULL &&
            !handler(context, codec, payload, payload_size, initial_sample,
                     initial_index)) return false;
        offset += block_size;
        ++blocks;
    }
    return offset == frame_size && blocks == info->block_count;
}

size_t fmo_frame_build_opus(uint8_t *output, size_t capacity,
                            const char *callsign, uint16_t session,
                            uint32_t started_at, uint32_t timestamp,
                            const uint8_t *const packets[],
                            const size_t packet_sizes[], size_t packet_count,
                            uint8_t buffer_depth)
{
    static const uint8_t marker[5] = {0x3d, 0x14, 0x00, 0xe0, 0x3d};
    if (output == NULL || callsign == NULL || packets == NULL ||
        packet_sizes == NULL || packet_count == 0 ||
        packet_count > UINT16_MAX || capacity < 64) return 0;
    size_t total = 64;
    for (size_t i = 0; i < packet_count; ++i) {
        if (packets[i] == NULL || packet_sizes[i] == 0 ||
            packet_sizes[i] > UINT16_MAX - 16 ||
            total > capacity - (16 + packet_sizes[i])) return 0;
        total += 16 + packet_sizes[i];
    }
    memset(output, 0, total);
    output[0] = 1;
    write_le16(output + 6, session);
    size_t callsign_size = strnlen(callsign, 6);
    for (size_t i = 0; i < callsign_size; ++i) {
        char ch = callsign[i];
        output[10 + i] = (uint8_t)(ch >= 'a' && ch <= 'z'
            ? ch - ('a' - 'A') : ch);
    }
    write_le32(output + 22, started_at);
    write_le32(output + 26, timestamp);
    write_le32(output + 30, (uint32_t)total);
    write_le16(output + 34, (uint16_t)packet_count);
    output[40] = buffer_depth;
    output[41] = 0xbf;
    output[42] = 0x01;
    size_t offset = 64;
    for (size_t i = 0; i < packet_count; ++i) {
        size_t block_size = 16 + packet_sizes[i];
        output[offset] = (uint8_t)(i + 1);
        write_le16(output + offset + 2, (uint16_t)block_size);
        uint8_t *inner = output + offset + 8;
        inner[0] = FMO_FRAME_OPUS;
        write_le16(inner + 1, (uint16_t)(8 + packet_sizes[i]));
        memcpy(inner + 3, marker, sizeof(marker));
        memcpy(inner + 8, packets[i], packet_sizes[i]);
        offset += block_size;
    }
    write_le32(output + 36,
               esp_rom_crc32_le(0, output + 64, total - 64));
    return total;
}
