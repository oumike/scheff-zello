#pragma once

// esp_lcd panel driver for the NV3031B (240x320 TFT on QSPI), as fitted to the
// Seeed Wio Tracker L2. The panel IO must be QSPI (quad_mode) with 32-bit
// commands and 8-bit parameters, SPI mode 3, and no DC line. The controller
// has no reset pin of its own here: hardware reset is the board's job, and
// esp_lcd_panel_reset() sends a software reset.

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"

esp_err_t lcd_new_panel_nv3031b(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *config,
                                esp_lcd_panel_handle_t *ret_panel);
