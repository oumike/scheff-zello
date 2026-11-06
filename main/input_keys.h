#pragma once

// A physical keyboard as LVGL input, plus the focus group it drives: the
// T-Deck's keyboard and trackball, or the Tanmatsu's keyboard. Built only for
// boards with a keyboard (UI_HAS_KEYBOARD).

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

// Registers the keypad input and starts keeping the focus group in step with
// the screen or overlay in front. Call after display_init().
esp_err_t input_keys_init(void);

// The talk screen (Home), where the T-Deck's trackball click and space bar
// mean talk, and its message field, which typing anywhere on that screen goes
// to. Must be called with the LVGL lock held.
void input_keys_set_talk_screen(lv_obj_t *scr, lv_obj_t *compose);

// True while the talk screen is in front with nothing over it. Safe to call
// from any task.
bool input_keys_talk_active(void);
