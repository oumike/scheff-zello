#pragma once

// The CardputerZero's keyboard after its keymap: characters (ASCII), the
// navigation keys below, and the space bar as a press and a release so it can
// be held to talk. Brightness keys are handled in the HAL; holding Esc for
// 3 seconds quits the app.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    CP0_K_ENTER = 0x100,
    CP0_K_ESC,
    CP0_K_UP,
    CP0_K_DOWN,
    CP0_K_LEFT,
    CP0_K_RIGHT,
    CP0_K_TAB,
    CP0_K_BACKTAB,
    CP0_K_BACKSPACE,
    CP0_K_DELETE,
    CP0_K_HOME,
    CP0_K_END,
    CP0_K_SPACE,
    CP0_K_VOL_UP,
    CP0_K_VOL_DOWN,
};

typedef struct {
    uint32_t key;  // an ASCII character or a CP0_K_*
    bool down;
} cp0_key_event_t;

esp_err_t cp0_keys_start(void);
// The next key, if any; never blocks.
bool cp0_keys_next(cp0_key_event_t *ev);
