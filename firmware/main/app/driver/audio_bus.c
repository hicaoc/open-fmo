#include "audio_bus.h"

#include "board_config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "audio_bus";
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static bool s_ready;

static bool pins_are_valid(void)
{
    return FMO_I2S_MCLK >= 0 && FMO_I2S_BCLK >= 0 && FMO_I2S_WS >= 0 &&
           FMO_I2S_DOUT >= 0 && FMO_I2S_DIN >= 0;
}

void audio_bus_deinit(void)
{
    if (s_tx_channel != NULL) {
        (void)i2s_channel_disable(s_tx_channel);
        (void)i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    if (s_rx_channel != NULL) {
        (void)i2s_channel_disable(s_rx_channel);
        (void)i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    s_ready = false;
}

esp_err_t audio_bus_init(uint32_t sample_rate_hz)
{
    if (s_ready) return ESP_OK;
    if (sample_rate_hz == 0) return ESP_ERR_INVALID_ARG;
    if (!pins_are_valid()) {
        ESP_LOGW(TAG, "I2S pins are not configured; bus remains disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    audio_bus_deinit();

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(FMO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 3;
    channel_config.dma_frame_num = sample_rate_hz / 100;
    channel_config.auto_clear_after_cb = true;

    /* Snapshot heap before I2S DMA allocation */
    unsigned int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    unsigned dma_before = heap_caps_get_free_size(MALLOC_CAP_DMA);

    esp_err_t err = i2s_new_channel(&channel_config, &s_tx_channel, &s_rx_channel);
    if (err != ESP_OK) goto fail;

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = FMO_I2S_MCLK,
            .bclk = FMO_I2S_BCLK,
            .ws = FMO_I2S_WS,
            .dout = FMO_I2S_DOUT,
            .din = FMO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(s_tx_channel, &config);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_init_std_mode(s_rx_channel, &config);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_tx_channel);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_rx_channel);
    if (err != ESP_OK) goto fail;

    s_ready = true;
    /* DMA buffer allocation diagnostic (after full setup) */
    {
        unsigned int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        unsigned psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        unsigned dma_after = heap_caps_get_free_size(MALLOC_CAP_DMA);
        ESP_LOGI(TAG, "I2S alloc: internal-%u psram-%u dma-%u",
                 int_before - int_after, psram_before - psram_after,
                 dma_before - dma_after);
    }
    ESP_LOGI(TAG, "ready: rate=%lu mclk=%d bclk=%d ws=%d dout=%d din=%d",
             (unsigned long)sample_rate_hz, FMO_I2S_MCLK, FMO_I2S_BCLK,
             FMO_I2S_WS, FMO_I2S_DOUT, FMO_I2S_DIN);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(err));
    audio_bus_deinit();
    return err;
}

bool audio_bus_is_ready(void)
{
    return s_ready;
}

esp_err_t audio_bus_get_channels(i2s_chan_handle_t *tx_channel,
                                 i2s_chan_handle_t *rx_channel)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (tx_channel != NULL) *tx_channel = s_tx_channel;
    if (rx_channel != NULL) *rx_channel = s_rx_channel;
    return ESP_OK;
}
