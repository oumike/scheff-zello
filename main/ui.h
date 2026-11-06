#pragma once

#include <stdbool.h>

// Touch UI. Call after display_init(), net_wifi_init() and channel_list_init().
// Boots into Wi-Fi setup when no network is saved, then the Zello account, then
// the channel picker; once all three are done it stays on the home screen.
void ui_start(void);

// True while the screensaver is up or the screen is dark: the UI is out of
// sight, and nothing may talk. Safe to call from any task.
bool ui_saver_active(void);

// A key typed while the screensaver is up wakes the screen and is swallowed:
// returns true if it did. Call with the LVGL lock held.
bool ui_saver_key(void);
