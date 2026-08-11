#pragma once

#include <stddef.h>
#include <stdint.h>

#define OPUS_VOICE_SAMPLE_RATE 16000U
#define OPUS_VOICE_MAX_FRAME_BYTES 640U

typedef struct opus_voice_encoder opus_voice_encoder_t;
typedef struct opus_voice_decoder opus_voice_decoder_t;

opus_voice_encoder_t *opus_voice_encoder_open(uint32_t frame_ms);
opus_voice_encoder_t *opus_voice_encoder_open_ex(uint32_t sample_rate,
                                                 uint32_t frame_ms,
                                                 uint32_t bitrate);
int opus_voice_encode(opus_voice_encoder_t *encoder, const int16_t *pcm,
                      size_t samples, uint8_t *output, size_t capacity);
void opus_voice_encoder_close(opus_voice_encoder_t *encoder);

opus_voice_decoder_t *opus_voice_decoder_open(uint32_t frame_ms);
opus_voice_decoder_t *opus_voice_decoder_open_ex(uint32_t sample_rate,
                                                 uint32_t frame_ms);
int opus_voice_decode(opus_voice_decoder_t *decoder, const uint8_t *frame,
                      size_t frame_size, int16_t *output,
                      size_t output_samples);
void opus_voice_decoder_close(opus_voice_decoder_t *decoder);
