#pragma once

// The CardputerZero HAL's internals, shared between its files.

#include <stdbool.h>
#include <stdint.h>

// Milliseconds on the monotonic clock.
uint32_t hal_now_ms(void);

// True when the app draws into a desktop window (or the simulator's) rather
// than straight onto the LCD.
bool hal_windowed(void);

// ---- keys (cp0_keys.c) ----
void cp0_keys_feed(uint16_t evdev_code, uint8_t value, uint32_t ms);
void cp0_keys_set_focus(bool focused);  // window mode: the window has focus
bool cp0_keys_quit_requested(void);     // Esc held for a second
void cp0_keys_stop(void);

// ---- the display (display_linux.c) ----
void hal_display_choose(bool window);  // before display_init()
void hal_display_stop(void);
void hal_backlight_step(int delta);    // the brightness keys

// ---- backlight (board_linux.c) ----
bool hal_backlight_present(void);
void hal_backlight_set(uint8_t level);  // 0-255
void hal_backlight_restore(void);       // the launcher's level, on exit

// ---- the window (sdl_window.c) ----
bool sdlwin_open(int w, int h, int scale, const char *title);
// Runs SDL's event loop on the calling (main) thread until quit() is true or
// the window closes.
void sdlwin_run(bool (*quit)(void));
void sdlwin_close(const char *screenshot_bmp);
void sdlwin_push(const uint16_t *frame, int y0, int y1);
void sdlwin_set_level(uint8_t level);
// The mouse on the panel, for LVGL's pointer.
bool sdlwin_pointer(int16_t *x, int16_t *y);

// ---- the LCD (fb_linux.c) ----
bool fb_open(void);
void fb_push(const uint16_t *frame, int y0, int y1);
void fb_dark(bool on);
void fb_close(void);
