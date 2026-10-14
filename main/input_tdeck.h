#pragma once

// T-Deck keyboard and trackball as LVGL input, plus the focus group they
// drive. ESP32-S3 builds only.

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

// Registers the keypad input and starts keeping the focus group in step with
// the screen or overlay in front. Call after display_init().
esp_err_t input_tdeck_init(void);

// The screen on which the trackball click and the space bar mean talk (Home).
// Must be called with the LVGL lock held.
void input_tdeck_set_talk_screen(lv_obj_t *scr);

// True while the talk screen is in front with nothing over it. Safe to call
// from any task.
bool input_tdeck_talk_active(void);
