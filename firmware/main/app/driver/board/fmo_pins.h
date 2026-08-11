/**
 * @file fmo_pins.h
 * @brief FMO Gateway (SenHaiX 8800) ESP32-S3 GPIO Pin Definitions
 * @note  Hardware: ESP32-S3 QFN56 rev v0.2, SenHaiX 8800 NFM Over Internet Gateway
 * @author BG5ESN / Open FMO Project
 * @date 2026-08-01
 *
 * Confirmed pins marked [CONFIRMED], unconfirmed marked [TBD]
 */

#ifndef FMO_PINS_H
#define FMO_PINS_H

/* ========================================================================== */
/*  BK4802 Radio Module (via PY32 MCU)                                        */
/* ========================================================================== */

/** UART TX -> PY32 RX (PA3), 19200 8N1, use UART_NUM_0 */
#define FMO_BK_UART_TX          15
/** UART RX <- PY32 TX (PA2), 19200 8N1 */
#define FMO_BK_UART_RX          16
/** UART peripheral index (must use UART_NUM_0, UART_NUM_2 does not work) */
#define FMO_BK_UART_NUM         0
/** Baud rate for BK4802 AT communication */
#define FMO_BK_UART_BAUD        19200

/** Radio PTT output: HIGH = BK4802 transmit, LOW = receive */
#define FMO_RADIO_PTT           6

/** AT EN output: controls PY32 AT command enable (PB5 on PY32, internal pull-up,
 *  HIGH = AT enabled. PY32 defaults enabled so this pin may be optional) */
#define FMO_AT_EN               5       /* Factory firmware: output HIGH */

/* ========================================================================== */
/*  NET PTT (Network Transmit Trigger)                                        */
/* ========================================================================== */

/** NET PTT input: HIGH = local radio receiving signal, trigger network send */
#define FMO_NET_PTT             17

/* ========================================================================== */
/*  ES8311 Audio Codec (I2C)                                                  */
/* ========================================================================== */

/** I2C SCL for ES8311, recovered from the supplied factory image. */
#define FMO_I2C_SCL             12
/** I2C SDA for ES8311, recovered from the supplied factory image. */
#define FMO_I2C_SDA             14
/** ES8311 I2C address (7-bit) */
#define FMO_ES8311_ADDR         0x18
/** Control-bus clock used by the supplied factory image. */
#define FMO_I2C_FREQ_HZ         20000

/** I2S1 wiring recovered from the supplied factory firmware. */
#define FMO_I2S_PORT             1
#define FMO_I2S_MCLK            11
#define FMO_I2S_BCLK            10
#define FMO_I2S_WS               8
/** ESP32-S3 data output -> ES8311 DAC input. */
#define FMO_I2S_DOUT             7
/** ES8311 ADC output -> ESP32-S3 data input. */
#define FMO_I2S_DIN              9

/* ========================================================================== */
/*  Audio Amplifier                                                           */
/* ========================================================================== */

/** Rear audio power amplifier enable: HIGH = on; keep enabled. */
#define FMO_AMP_EN              21

/* ========================================================================== */
/*  LEDs                                                                      */
/* ========================================================================== */

#define FMO_LED1                1
#define FMO_LED2                2
#define FMO_LED3                4

/** Indicator polarity recovered by board testing. LED2 is opposite to the
 *  two sink-driven indicators. */
#define FMO_LED1_ACTIVE_LEVEL   1
#define FMO_LED2_ACTIVE_LEVEL   1
#define FMO_LED3_ACTIVE_LEVEL   1

/* ========================================================================== */
/*  Rotary Encoder                                                            */
/* ========================================================================== */

#define FMO_ENCODER_A           40
#define FMO_ENCODER_B           41
#define FMO_ENCODER_SW          42

/* ========================================================================== */
/*  USB (native USB-OTG, ESP32-S3 fixed)                                     */
/* ========================================================================== */

#define FMO_USB_DM              19
#define FMO_USB_DP              20

/* ========================================================================== */
/*  SPI Flash (internal, ESP32-S3 fixed)                                     */
/* ========================================================================== */

/* GPIO 26-32: SPI flash (SPICS1, SPIHD, SPIWP, SPICS0, SPICLK, SPIQ, SPID) */

/* ========================================================================== */
/*  PSRAM (Octal SPI)                                                         */
/* ========================================================================== */

/* The ESP32-S3 package has 2MB embedded PSRAM; GPIO34/35/37 are available. */

/* ========================================================================== */
/*  TFT LCD (NV3007, 142x428, SPI) - [CONFIRMED]                              */
/* ========================================================================== */

#define FMO_LCD_SPI_CLK          38
#define FMO_LCD_SPI_MOSI         39
#define FMO_LCD_SPI_MISO         45    /* Optional for write-only operation */
#define FMO_LCD_SPI_CS           35
#define FMO_LCD_DC               37    /* Data/Command select */
#define FMO_LCD_RST              36
#define FMO_LCD_BL               34    /* Backlight enable, active HIGH */
#define FMO_LCD_BL_ACTIVE_LEVEL   1
#define FMO_LCD_SPI_FREQ_HZ      33000000

/* ========================================================================== */
/*  Remaining unknown GPIOs (for reference during further probing)            */
/* ========================================================================== */

/*
 * Unknown: 3, 13, 18, 33, 43, 44, 46, 47, 48
 *
 * Notes from probing:
 * - GPIO 12: ES8311 I2C SCL in the supplied factory image
 * - GPIO 5: radio-module AT enable, output HIGH in the factory firmware
 * - GPIO 3: always HIGH (external pull-up)
 * - GPIO 7/8/9/10/11: ES8311 I2S1 DOUT/WS/DIN/BCLK/MCLK
 * - GPIO 13: floating HIGH without pull-down
 * - GPIO 43, 44: ESP32-S3 default UART0 TX/RX (console if not using USB)
 * - GPIO 45, 46: ESP32-S3 strapping pins (VDD_SPI, GPIO46)
 * - GPIO 47, 48: general purpose (48 often used for RGB LED on devkits)
 */

#endif /* FMO_PINS_H */
