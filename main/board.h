#pragma once

// Board support for the four targets:
//  - LilyGO T-Display-P4 V1.0 (ESP32-P4): the two I2C buses, the XL9535 I/O
//    expander rails that gate audio / Wi-Fi / SD / display power, and the fuel
//    gauge.
//  - LilyGO T-Deck / T-Deck Plus (ESP32-S3): the peripheral power switch, the
//    shared I2C bus (keyboard, touch, mic ADC), the SPI bus shared by the LCD,
//    microSD and the unused LoRa radio, and the battery divider.
//  - Seeed Wio Tracker L2 (ESP32-S3): the PCA9555 I/O expander that holds the
//    LCD, touch, audio amplifier and microSD power and reset lines, the shared
//    I2C bus, the LCD's QSPI bus, and the ADS1115 battery ADC.
//  - Nicolai Electronics Tanmatsu (ESP32-P4): everything through badge.team's
//    badge-bsp, which drives the CH32V203 coprocessor (keyboard, backlight,
//    power, the C6 radio's power, the speaker amplifier) and the ES8156 DAC.
// The boards are told apart by CONFIG_ZELLO_BOARD_* (Kconfig).

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#if CONFIG_ZELLO_BOARD_TDISPLAY_P4

// I2C bus 1: XL9535 I/O expander, touch, RTC, fuel gauge.
#define BOARD_I2C1_SDA 7
#define BOARD_I2C1_SCL 8
// I2C bus 2: ES8311 codec, IMU, haptics.
#define BOARD_I2C2_SDA 20
#define BOARD_I2C2_SCL 21

// ES8311 I2S (port 0)
#define BOARD_I2S_MCLK 13
#define BOARD_I2S_BCLK 12
#define BOARD_I2S_WS   9
#define BOARD_I2S_DOUT 10  // P4 -> codec DAC
#define BOARD_I2S_DIN  11  // codec ADC -> P4
#define BOARD_ES8311_ADDR 0x18

// BOOT button, active low: the screen button, as camillia-mt uses it (tap
// for the screensaver, hold to wake). Talking is on the screen. Shared with
// the Ethernet RMII TXD1, so it can't stay a button if Ethernet is enabled.
#define BOARD_BOOT_BUTTON 35
#define BOARD_WAKE_BUTTON BOARD_BOOT_BUTTON

#elif CONFIG_ZELLO_BOARD_TANMATSU

// Most of the hardware is reached through badge-bsp; these are only the pins
// the BSP leaves to us. No talk button (and no microphone): BOARD_PTT_BUTTON
// stays undefined.

// microSD on SDMMC slot 0 in 4-bit mode (IO MUX pins CLK 43, CMD 44, D0-D3
// 39-42), powered by the P4's on-chip LDO channel 4.

#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2

#include "driver/spi_common.h"

// Pins as camillia-mt's hal/hw_wio_tracker_l2.h and Tactility's device tree.

// One I2C bus: PCA9555 expander (0x21), GT911 touch (0x5D), LP5814 backlight
// driver (0x2C), ADS1115 battery ADC (0x48), ES8311 DAC (0x18), ES7243E mic
// ADC (0x14).
#define BOARD_I2C_SDA 47
#define BOARD_I2C_SCL 48

// NV3031B 240x320 LCD on QSPI (SPI3). It has no DC line; its reset is on the
// expander.
#define BOARD_LCD_SCLK 42
#define BOARD_LCD_D0   41
#define BOARD_LCD_D1   40
#define BOARD_LCD_D2   39
#define BOARD_LCD_D3   38
#define BOARD_LCD_CS   46
#define BOARD_LP5814_ADDR 0x2C

// microSD on SDMMC, 1-bit.
#define BOARD_SD_CLK 2
#define BOARD_SD_CMD 3
#define BOARD_SD_D0  1

#define BOARD_LORA_CS 21  // held high so the (unused) radio stays quiet

// The only button on the MCU itself, active low (also the BOOT strap).
#define BOARD_USER_BUTTON 0
#define BOARD_PTT_BUTTON  BOARD_USER_BUTTON

// Audio on I2S0: ES8311 DAC into the speaker amplifier, and ES7243E ADC from
// the microphone, sharing the clocks. Both are I2S slaves.
#define BOARD_I2S_MCLK 10
#define BOARD_I2S_BCLK 11
#define BOARD_I2S_WS   12
#define BOARD_I2S_DOUT 16  // S3 -> ES8311
#define BOARD_I2S_DIN  15  // ES7243E -> S3
#define BOARD_ES8311_ADDR  0x18
#define BOARD_ES7243E_ADDR 0x14

// The top Wake button: expander IO0, active low. The expander pulls this
// line low when any of its inputs changes.
#define BOARD_EXPANDER_INT 45

// The SPI host the LCD uses.
spi_host_device_t board_spi_host(void);

// microSD card supply (expander).
esp_err_t board_sd_power(bool on);

// Reads the Wake button. Only touches the bus when the expander has flagged a
// change or the button was down last time, so it is cheap to poll.
esp_err_t board_wake_button(bool *pressed);

#elif CONFIG_ZELLO_BOARD_TDECK

#include "driver/spi_common.h"

// Pins as camillia-mt's hal/hw_tdeck.h and LilyGO's T-Deck pin map.

// Must be driven high first: it switches on power to the LCD, keyboard,
// touch, audio and radio.
#define BOARD_POWERON 10

// One I2C bus: keyboard (0x55), GT911 touch (0x5D), ES7210 mic ADC (0x40).
#define BOARD_I2C_SDA 18
#define BOARD_I2C_SCL 8

// SPI2, shared by the LCD, the microSD slot and the (unused) LoRa radio.
#define BOARD_SPI_SCK  40
#define BOARD_SPI_MOSI 41
#define BOARD_SPI_MISO 38

// ST7789 320x240 LCD.
#define BOARD_LCD_CS 12
#define BOARD_LCD_DC 11
#define BOARD_LCD_BL 42

#define BOARD_TOUCH_INT 16
#define BOARD_SD_CS     39
#define BOARD_LORA_CS   9  // held high so the radio stays off the bus

// Keyboard: an ESP32-C3 behind I2C that reports one character per read.
#define BOARD_KB_ADDR 0x55
#define BOARD_KB_INT  46

// Trackball: one line pulses per step of each direction. The lines don't
// match their schematic names; these are the directions they report, as
// camillia-mt wires its interrupts (keyboard.cpp).
#define BOARD_TBALL_UP    3
#define BOARD_TBALL_DOWN  15
#define BOARD_TBALL_LEFT  1
#define BOARD_TBALL_RIGHT 2
#define BOARD_TBALL_CLICK 0  // also the BOOT strap, active low
#define BOARD_PTT_BUTTON  BOARD_TBALL_CLICK

// Speaker: MAX98357A class-D amplifier on I2S0, output only.
#define BOARD_SPK_BCLK 7
#define BOARD_SPK_WS   5
#define BOARD_SPK_DOUT 6

// Microphone: ES7210 ADC on I2S1, input only.
#define BOARD_MIC_MCLK 48
#define BOARD_MIC_BCLK 47
#define BOARD_MIC_WS   21
#define BOARD_MIC_DIN  14
#define BOARD_ES7210_ADDR 0x40

// Battery: VBAT through a 2:1 divider.
#define BOARD_BAT_ADC 4

// The SPI host the LCD and microSD share.
spi_host_device_t board_spi_host(void);

#endif

esp_err_t board_init(void);

// Audio power. On the T-Display-P4, the shared OUT_5V domain (NS4150B speaker
// amp and the ES8311 analog supply); on the Wio Tracker L2 and the Tanmatsu
// the speaker amplifier; on the T-Deck audio is always powered.
esp_err_t board_audio_power(bool on);

// Reads the battery state of charge (0-100%): the BQ27220 gauge on the
// T-Display-P4, the coprocessor's estimate on the Tanmatsu, an estimate from
// the cell voltage on the ESP32-S3 boards.
esp_err_t board_battery_percent(uint8_t *percent);

// The T-Display-P4's two buses (expander and touch; codec). On the other
// boards both return the one shared bus.
i2c_master_bus_handle_t board_main_i2c_bus(void);
i2c_master_bus_handle_t board_codec_i2c_bus(void);

#if CONFIG_ZELLO_BOARD_TDISPLAY_P4

// ESP32-C6 coprocessor EN line (active high).
esp_err_t board_c6_enable(bool on);

// microSD card supply (XL9535 IO15, active low).
esp_err_t board_sd_power(bool on);

// RM69A10 AMOLED and GT9895 touch reset lines (XL9535). false holds the part
// in reset, true releases it.
esp_err_t board_screen_reset_release(bool released);
esp_err_t board_touch_reset_release(bool released);

#elif CONFIG_ZELLO_BOARD_TANMATSU

// The SD card has no switched supply; this is a no-op kept so sdcard.c can
// treat both P4 boards alike.
esp_err_t board_sd_power(bool on);

#endif
