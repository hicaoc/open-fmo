#include "opus_voice.h"

#include <stdlib.h>

#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

struct opus_voice_encoder {
    void *handle;
    size_t frame_samples;
};

struct opus_voice_decoder {
    void *handle;
};

static esp_opus_enc_frame_duration_t encoder_duration(uint32_t frame_ms)
{
    if (frame_ms == 20) return ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    if (frame_ms == 40) return ESP_OPUS_ENC_FRAME_DURATION_40_MS;
    if (frame_ms == 60) return ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    return ESP_OPUS_ENC_FRAME_DURATION_ARG;
}

static esp_opus_dec_frame_duration_t decoder_duration(uint32_t frame_ms)
{
    if (frame_ms == 20) return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    if (frame_ms == 40) return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    if (frame_ms == 60) return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
}

static void *codec_calloc(size_t size)
{
    void *memory = heap_caps_calloc(1, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : calloc(1, size);
}

opus_voice_encoder_t *opus_voice_encoder_open(uint32_t frame_ms)
{
    return opus_voice_encoder_open_ex(OPUS_VOICE_SAMPLE_RATE, frame_ms, 40000);
}

opus_voice_encoder_t *opus_voice_encoder_open_ex(uint32_t sample_rate,
                                                 uint32_t frame_ms,
                                                 uint32_t bitrate)
{
    if ((sample_rate != 8000 && sample_rate != 12000 &&
         sample_rate != 16000 && sample_rate != 24000 &&
         sample_rate != 48000) ||
        encoder_duration(frame_ms) == ESP_OPUS_ENC_FRAME_DURATION_ARG) return NULL;
    opus_voice_encoder_t *encoder = codec_calloc(sizeof(*encoder));
    if (encoder == NULL) return NULL;
    esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    config.sample_rate = sample_rate;
    config.channel = ESP_AUDIO_MONO;
    config.bits_per_sample = ESP_AUDIO_BIT16;
    config.bitrate = bitrate;
    config.frame_duration = encoder_duration(frame_ms);
    config.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    config.complexity = 10;
    config.enable_fec = false;
    config.enable_dtx = false;
    config.enable_vbr = true;
    if (esp_opus_enc_open(&config, sizeof(config), &encoder->handle) !=
        ESP_AUDIO_ERR_OK) {
        free(encoder);
        return NULL;
    }
    encoder->frame_samples = sample_rate / 1000U * frame_ms;
    return encoder;
}

int opus_voice_encode(opus_voice_encoder_t *encoder, const int16_t *pcm,
                      size_t samples, uint8_t *output, size_t capacity)
{
    if (encoder == NULL || encoder->handle == NULL || pcm == NULL ||
        output == NULL || samples != encoder->frame_samples) return -1;
    esp_audio_enc_in_frame_t input = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)(samples * sizeof(*pcm)),
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = output,
        .len = (uint32_t)capacity,
    };
    return esp_opus_enc_process(encoder->handle, &input, &out) == ESP_AUDIO_ERR_OK
        ? (int)out.encoded_bytes : -1;
}

void opus_voice_encoder_close(opus_voice_encoder_t *encoder)
{
    if (encoder == NULL) return;
    if (encoder->handle != NULL) esp_opus_enc_close(encoder->handle);
    free(encoder);
}

opus_voice_decoder_t *opus_voice_decoder_open(uint32_t frame_ms)
{
    return opus_voice_decoder_open_ex(OPUS_VOICE_SAMPLE_RATE, frame_ms);
}

opus_voice_decoder_t *opus_voice_decoder_open_ex(uint32_t sample_rate,
                                                 uint32_t frame_ms)
{
    if ((sample_rate != 8000 && sample_rate != 12000 &&
         sample_rate != 16000 && sample_rate != 24000 &&
         sample_rate != 48000) ||
        decoder_duration(frame_ms) == ESP_OPUS_DEC_FRAME_DURATION_INVALID) return NULL;
    opus_voice_decoder_t *decoder = codec_calloc(sizeof(*decoder));
    if (decoder == NULL) return NULL;
    esp_opus_dec_cfg_t config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    config.sample_rate = sample_rate;
    config.channel = ESP_AUDIO_MONO;
    config.frame_duration = decoder_duration(frame_ms);
    config.self_delimited = false;
    if (esp_opus_dec_open(&config, sizeof(config), &decoder->handle) !=
        ESP_AUDIO_ERR_OK) {
        free(decoder);
        return NULL;
    }
    return decoder;
}

int opus_voice_decode(opus_voice_decoder_t *decoder, const uint8_t *frame,
                      size_t frame_size, int16_t *output,
                      size_t output_samples)
{
    if (decoder == NULL || decoder->handle == NULL || frame == NULL ||
        output == NULL) return -1;
    esp_audio_dec_in_raw_t input = {
        .buffer = (uint8_t *)frame,
        .len = (uint32_t)frame_size,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = (uint8_t *)output,
        .len = (uint32_t)(output_samples * sizeof(*output)),
    };
    esp_audio_dec_info_t info = {0};
    return esp_opus_dec_decode(decoder->handle, &input, &out, &info) ==
        ESP_AUDIO_ERR_OK ? (int)(out.decoded_size / sizeof(*output)) : -1;
}

void opus_voice_decoder_close(opus_voice_decoder_t *decoder)
{
    if (decoder == NULL) return;
    if (decoder->handle != NULL) esp_opus_dec_close(decoder->handle);
    free(decoder);
}
