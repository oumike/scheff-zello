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
// A CP0_K_* key or ASCII character from the window's keyboard, past the
// keymap.
void cp0_keys_push(uint32_t key, bool down);
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
// A resizable window, at the size and place it was last closed at (kept in
// window.conf, in the working directory).
bool sdlwin_open(const char *title);
// Runs SDL's event loop on the calling (main) thread until quit() is true,
// the window closes, or Ctrl+Q.
void sdlwin_run(bool (*quit)(void));
void sdlwin_close(const char *screenshot_bmp);
// The window's size in points: the UI's resolution.
void sdlwin_size(int *w, int *h);
// The new size, once, after the window was resized.
bool sdlwin_take_resize(int *w, int *h);
// Rows y0..y1-1 of a w x h frame.
void sdlwin_push(const uint16_t *frame, int w, int h, int y0, int y1);
void sdlwin_set_level(uint8_t level);
// The mouse in the window, for LVGL's pointer.
bool sdlwin_pointer(int16_t *x, int16_t *y);
// The wheel turned since the last call, in hundredths of a notch: positive
// is up (towards the top of the content), or left for x.
void sdlwin_take_wheel(int *x100, int *y100);
typedef enum { SDLWIN_CURSOR_ARROW, SDLWIN_CURSOR_HAND, SDLWIN_CURSOR_TEXT } sdlwin_cursor_t;
void sdlwin_set_cursor(sdlwin_cursor_t cursor);

// ---- the LCD (fb_linux.c) ----
bool fb_open(void);
void fb_push(const uint16_t *frame, int y0, int y1);
void fb_dark(bool on);
void fb_close(void);
