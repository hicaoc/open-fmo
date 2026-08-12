#include "es8311_codec.h"

#include "board_config.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";
static bool s_present;
static bool s_configured;

/* Default/cap volumes: MIC (ADC) defaults to 160 and is capped at 170;
 * speaker (DAC) allows the full 0-255 register range. */
#define ES8311_ADC_VOLUME_DEFAULT  160
#define ES8311_DAC_VOLUME_DEFAULT  180
#define ES8311_ADC_VOLUME_MAX      170

esp_err_t es8311_codec_read(uint8_t reg, uint8_t *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    return i2c_bus_write_read(FMO_ES8311_ADDR, &reg, 1, value, 1, 100);
}

esp_err_t es8311_codec_write(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[] = {reg, value};
    return i2c_bus_write(FMO_ES8311_ADDR, bytes, sizeof(bytes), 100);
}

esp_err_t es8311_codec_init_control(void)
{
    s_present = false;
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "shared I2C bus init failed");
    /* ES8311 power-up can be slow; give it generous time. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t write_error = ESP_FAIL;
    for (int attempt = 0; attempt < 20; ++attempt) {
        write_error = es8311_codec_write(0x44, 0x08);
        if (write_error == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (write_error != ESP_OK) {
        ESP_RETURN_ON_ERROR(write_error, TAG, "factory REG44 write did not ACK");
    }
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x44, 0x08), TAG,
                        "factory second REG44 write did not ACK");
    s_present = true;
    uint8_t reset_reg = 0;
    esp_err_t read_error = es8311_codec_read(0x00, &reset_reg);
    if (read_error == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 ACK at 0x%02X, REG00=0x%02X",
                 FMO_ES8311_ADDR, reset_reg);
    } else {
        ESP_LOGW(TAG, "ES8311 ACK at 0x%02X; REG00 read deferred",
                 FMO_ES8311_ADDR);
    }
    return ESP_OK;
}

/*
 * Full ES8311 register configuration aligned to the reference project
 * (nrl-esp32 es8311_configure_codec). Must be called AFTER I2S MCLK is
 * running because the ES8311 internal bias circuits require MCLK.
 *
 * Sequence: pre-power → clock → reset → sample format → power-up analog
 * → ADC config → DAC unmute → volume.
 */
esp_err_t es8311_codec_configure(void)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    if (s_configured) return ESP_OK;

    uint8_t regv = 0;

    /* Pre-power: REG0D = 0xFA */
    if (es8311_codec_read(0x0D, &regv) == ESP_OK && regv != 0xFA) {
        ESP_RETURN_ON_ERROR(es8311_codec_write(0x0D, 0xFA), TAG, "REG0D pre-power");
    }

    /* GPIO / I2C_WL */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x44, 0x08), TAG, "REG44 (1)");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x44, 0x08), TAG, "REG44 (2)");

    /* Initial clock setup */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x01, 0x30), TAG, "REG01");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x02, 0x00), TAG, "REG02");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x03, 0x10), TAG, "REG03");
    /* REG16: ADC sync + scale=4 */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x16, 0x24), TAG, "REG16");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x04, 0x10), TAG, "REG04");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x05, 0x00), TAG, "REG05");

    /* Power management */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x0B, 0x00), TAG, "REG0B");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x0C, 0x00), TAG, "REG0C");

    /* Bias generators */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x10, 0x1F), TAG, "REG10 bias");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x11, 0x7F), TAG, "REG11 bias");
    vTaskDelay(pdMS_TO_TICKS(40));

    /* Reset + slave mode */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x00, 0x80), TAG, "REG00 reset");

    /* Use MCLK */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x01, 0x3F), TAG, "REG01 mclk");

    /* SCLK polarity: clear bit5 of REG06 */
    if (es8311_codec_read(0x06, &regv) == ESP_OK) {
        ESP_RETURN_ON_ERROR(es8311_codec_write(0x06, (uint8_t)(regv & ~0x20)),
                            TAG, "REG06 sclk");
    }

    /* Clock configuration for 16 kHz, 16-bit (MCLK=256*Fs=4.096MHz)
     * pre_div=1, pre_mult=1, adc_div=1, dac_div=1
     * lrck=0x00FF (256-1), bclk_div=4, adc_osr=0x10, dac_osr=0x20 */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x01, 0x3F), TAG, "REG01 clk_en");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x02, 0x00), TAG, "REG02 pre");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x03, 0x10), TAG, "REG03 adc_osr");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x04, 0x20), TAG, "REG04 dac_osr");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x05, 0x00), TAG, "REG05 div");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x06, 0x03), TAG, "REG06 bclk");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x07, 0x00), TAG, "REG07 lrck_h");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x08, 0xFF), TAG, "REG08 lrck_l");

    /* I2S format: 16-bit */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x09, 0x0C), TAG, "REG09 sdpin");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x0A, 0x0C), TAG, "REG0A spdout");

    /* Output drive: LINE mode (differential) */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x13, 0x00), TAG, "REG13 drive");

    /* ADC HPF coefficient: hpfs1=10 (matches reference project) */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x1B, 0x0A), TAG, "REG1B hpf");
    /* ADC EQ bypass (bit6=1) + HPF DISABLED (bit5=0) + hpfs2=10
     * NOTE: EQ MUST be bypassed when no coefficients are loaded,
     * otherwise zero-coeff EQ attenuates ADC to rail.
     * HPF disabled to pass CTCSS sub-audible tones (67-254 Hz);
     * DC offset is removed by software filter in audio_passthrough. */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x1C, 0x4A), TAG, "REG1C eq");

    /* REG44 final: I2C_WL set, ADCDAT_SEL=0 (ADC both slots) */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x44, 0x08), TAG, "REG44 final");

    /* Start */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x00, 0x80), TAG, "REG00 start");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x01, 0x3F), TAG, "REG01 start");

    /* Enable DAC/ADC interface (clear bit6 of REG09/REG0A) */
    if (es8311_codec_read(0x09, &regv) == ESP_OK) {
        ESP_RETURN_ON_ERROR(es8311_codec_write(0x09, (uint8_t)(regv & ~0x40)),
                            TAG, "REG09 en");
    }
    if (es8311_codec_read(0x0A, &regv) == ESP_OK) {
        ESP_RETURN_ON_ERROR(es8311_codec_write(0x0A, (uint8_t)(regv & ~0x40)),
                            TAG, "REG0A en");
    }

    /* ADC volume */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x17, ES8311_ADC_VOLUME_DEFAULT),
                        TAG, "REG17 adc vol");

    /* Power up PGA/ADC: REG0E = 0x02 */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x0E, 0x02), TAG, "REG0E pga");

    /* Enable DAC: REG12 = 0x00 */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x12, 0x00), TAG, "REG12 dac");

    /* REG14: LINSEL + PGA gain=10 */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x14, 0x1A), TAG, "REG14 pga");

    /* Power up analog: REG0D = 0x06 (VMIDSEL=2 normal, all PDN=0 active) */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x0D, 0x06), TAG, "REG0D analog");
    vTaskDelay(pdMS_TO_TICKS(40));

    /* ADC config: REG15 ramp rate */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x15, 0x40), TAG, "REG15 ramp");

    /* DAC DRC/EQ: disabled, EQ bypass */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x34, 0x00), TAG, "REG34 drc");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x35, 0x00), TAG, "REG35 drc");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x37, 0x08), TAG, "REG37 eq_byp");

    /* GP control */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x45, 0x00), TAG, "REG45 gp");

    /* DAC unmute + volume */
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x31, 0x00), TAG, "REG31 unmute");
    ESP_RETURN_ON_ERROR(es8311_codec_write(0x32, ES8311_DAC_VOLUME_DEFAULT),
                        TAG, "REG32 dac vol");

    s_configured = true;
    ESP_LOGI(TAG, "ES8311 fully configured: 16kHz/16bit, ADC vol=0x%02X, DAC vol=%u",
             ES8311_ADC_VOLUME_DEFAULT, ES8311_DAC_VOLUME_DEFAULT);
    /* Verify critical registers by read-back */
    {
        uint8_t r0d = 0, r0e = 0, r17 = 0, r09 = 0, r0a = 0, r01 = 0;
        uint8_t r14 = 0, r16 = 0, r00 = 0, r0b = 0;
        es8311_codec_read(0x00, &r00);
        es8311_codec_read(0x01, &r01);
        es8311_codec_read(0x09, &r09);
        es8311_codec_read(0x0A, &r0a);
        es8311_codec_read(0x0B, &r0b);
        es8311_codec_read(0x0D, &r0d);
        es8311_codec_read(0x0E, &r0e);
        es8311_codec_read(0x14, &r14);
        es8311_codec_read(0x16, &r16);
        es8311_codec_read(0x17, &r17);
        ESP_LOGI(TAG, "readback: REG00=0x%02X REG01=0x%02X REG09=0x%02X REG0A=0x%02X",
                 r00, r01, r09, r0a);
        ESP_LOGI(TAG, "readback: REG0B=0x%02X REG0D=0x%02X REG0E=0x%02X "
                 "REG14=0x%02X REG16=0x%02X REG17=0x%02X",
                 r0b, r0d, r0e, r14, r16, r17);
    }
    return ESP_OK;
}

bool es8311_codec_is_present(void)
{
    return s_present;
}

bool es8311_codec_is_configured(void)
{
    return s_configured;
}

esp_err_t es8311_codec_set_dac_mute(bool muted)
{
    uint8_t value;
    ESP_RETURN_ON_ERROR(es8311_codec_read(0x31, &value), TAG, "REG31 read failed");
    value = muted ? (uint8_t)(value | 0x60) : (uint8_t)(value & ~0x60);
    return es8311_codec_write(0x31, value);
}

esp_err_t es8311_codec_set_adc_volume(uint8_t volume)
{
    if (volume > ES8311_ADC_VOLUME_MAX) volume = ES8311_ADC_VOLUME_MAX;
    return es8311_codec_write(0x17, volume);
}

esp_err_t es8311_codec_set_dac_volume(uint8_t volume)
{
    /* REG32 is an 8-bit volume register: full 0-255 range is valid. */
    return es8311_codec_write(0x32, volume);
}

esp_err_t es8311_codec_set_headphone_drive(bool enabled)
{
    /* REG13 HPSW (bit 4): 0=line output, 1=headphone output driver. */
    return es8311_codec_write(0x13, enabled ? 0x10 : 0x00);
}
