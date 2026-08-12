#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FMO_CONFIG_SCHEMA_VERSION 15
#define FMO_WIFI_PROFILE_MAX 5

/* ES8311 volume limits: mic (ADC) defaults to 160 and caps at 170;
 * speaker (DAC) defaults to 200 and caps at 180. */
#define FMO_ES8311_ADC_VOL_DEFAULT 160
#define FMO_ES8311_ADC_VOL_MAX 170
#define FMO_ES8311_DAC_VOL_DEFAULT 200
#define FMO_ES8311_DAC_VOL_MAX 255

/* Software mic gain multiplier applied after the ES8311 ADC (1 = off,
 * up to 5x). */
#define FMO_MIC_GAIN_DEFAULT 1
#define FMO_MIC_GAIN_MIN 1
#define FMO_MIC_GAIN_MAX 5

/* APRS gateway forwarding directions (aprs_fwd bitmap bits), mirroring
 * the NRL-ESP32 channel model: RF (radio AFSK), NRL (network voice
 * AFSK) and APRS-IS (TCP). */
#define FMO_APRS_FWD_RF_TO_IS  (1u << 0)  /* iGate uplink: radio RX -> IS */
#define FMO_APRS_FWD_IS_TO_RF  (1u << 1)  /* IS -> radio AFSK TX */
#define FMO_APRS_FWD_NRL_TO_IS (1u << 2)  /* NRL RX -> IS */
#define FMO_APRS_FWD_IS_TO_NRL (1u << 3)  /* IS -> AFSK over NRL uplink */
#define FMO_APRS_FWD_RF_TO_NRL (1u << 4)  /* radio RX -> NRL uplink */
#define FMO_APRS_FWD_NRL_TO_RF (1u << 5)  /* NRL RX -> radio AFSK TX */
#define FMO_APRS_FWD_MASK      0x3Fu
#define FMO_APRS_FWD_DEFAULT   (FMO_APRS_FWD_RF_TO_IS | FMO_APRS_FWD_NRL_TO_IS)

typedef struct {
    char ssid[33];
    char password[65];
} fmo_wifi_profile_t;

typedef struct {
    uint32_t schema_version;
    char callsign[16];
    uint8_t callsign_ssid;
    uint8_t nrl_channel;
    uint16_t selected_server;
    char nrl_host[64];
    uint16_t nrl_port;
    char wifi_ssid[33];
    char wifi_password[65];
    fmo_wifi_profile_t wifi_profiles[FMO_WIFI_PROFILE_MAX];
    float radio_tx_mhz;
    float radio_rx_mhz;
    float tx_ctcss_hz;
    float rx_ctcss_hz;
    uint8_t squelch;
    uint8_t rx_volume;
    uint8_t tx_volume;
    uint8_t tx_power;
    bool rf_enabled;
    bool ble_provisioning_enabled;
    uint8_t ui_language;
    bool espnow_enabled;
    bool aprs_enabled;
    uint8_t voice_codec;   /* 0=G.711 8kHz (default), 1=Opus 16kHz */
    uint8_t es8311_dac_vol;  /* ES8311 speaker volume 0-255 (default 200) */
    uint8_t es8311_adc_vol;  /* ES8311 mic volume 0-170 (default 160) */
    bool aprs_position_set;
    uint8_t aprs_ssid;
    uint16_t aprs_beacon_interval_s;
    int32_t aprs_latitude_e6;
    int32_t aprs_longitude_e6;
    uint16_t aprs_server_port;
    char aprs_server_host[65];
    char aprs_comment[81];
    int16_t freq_tune_hz;   /* BK4802 carrier offset -5000~+5000 Hz */
    /* APRS AFSK gateway switches (schema 11) */
    bool aprs_rf_rx;   /* demodulate radio mic audio (AFSK RX) */
    bool aprs_rf_tx;   /* AFSK out to the radio via the speaker */
    bool aprs_nrl_rx;  /* demodulate NRL network downlink audio */
    bool aprs_nrl_tx;  /* AFSK out over the NRL voice uplink */
    uint8_t aprs_fwd;  /* bitmap of aprs_fwd_dir_t forwarding switches */
    uint8_t mic_gain;  /* software mic amplification 1-5 (default 1, schema 12) */
    /* Dual-network settings (schema 13). Stable keys survive directory reorder. */
    char nrl_server_key[80];
    char fmo_server_key[80];
    char fmo_host[64];
    uint16_t fmo_port;
    uint8_t tx_network;   /* 0=NRL, 1=FMO */
    uint8_t audio_policy; /* 0=mix, 1=first arrival wins */
    /* FMO identity is separate from the legacy callsign/callsign_ssid pair,
     * which remains the NRL identity.  The base call must match userCert;
     * the SSID is used by the FMO APRS connection only. */
    char fmo_callsign[16];
    uint8_t fmo_callsign_ssid;
    bool es8311_hp_drive; /* REG13 HPSW: enable headphone output driver */
} fmo_config_t;

esp_err_t config_store_init(void);
void config_store_defaults(fmo_config_t *config);
esp_err_t config_store_load(fmo_config_t *config);
esp_err_t config_store_save(const fmo_config_t *config);
uint32_t config_store_generation(void);
size_t config_store_wifi_count(const fmo_config_t *config);
bool config_store_wifi_add(fmo_config_t *config, const char *ssid,
                           const char *password, bool promote);
bool config_store_wifi_remove(fmo_config_t *config, size_t index);
