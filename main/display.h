#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"

#if CONFIG_ZELLO_BOARD_TDISPLAY_P4
// RM69A10 4.1" AMOLED (568x1232 portrait, MIPI-DSI).
#define DISPLAY_H_RES 568
#define DISPLAY_V_RES 1232
#elif CONFIG_ZELLO_BOARD_TANMATSU
// ST7701 4" LCD (480x800 portrait, MIPI-DSI), turned to 800x480 landscape.
#define DISPLAY_H_RES 800
#define DISPLAY_V_RES 480
#else
// T-Deck ST7789 2.8" LCD (SPI) or Wio Tracker L2 NV3031B LCD (QSPI), both
// turned to 320x240 landscape.
#define DISPLAY_H_RES 320
#define DISPLAY_V_RES 240
#endif

esp_err_t display_init(void);
// Draws the first UI frame and turns the panel up; display_init() leaves it
// dark so nothing shows before there is a UI to show.
void display_reveal(void);

// LVGL is not thread safe: hold this around any lv_* call made outside LVGL's
// own callbacks (which already run with it held).
bool display_lock(void);
void display_unlock(void);

// Colour bars made by the DSI controller itself, with no frame buffer read
// from PSRAM: for telling display-link trouble from memory trouble.
// T-Display-P4 only; the other boards return ESP_ERR_NOT_SUPPORTED.
esp_err_t display_set_test_pattern(bool on);
bool display_test_pattern(void);

// Panel brightness, normalized to 0-255.
void display_set_brightness(uint8_t level);

// The backlight off (a dark panel; LVGL keeps running) or back at the level
// display_reveal() lit it to.
void display_backlight(bool on);
