#pragma once

// Shared pieces of the touch UI. Everything here must be called with the LVGL
// lock held: from LVGL callbacks (which already hold it), or between
// display_lock()/display_unlock().

#include <stdbool.h>
#include "sdkconfig.h"
#include "lvgl.h"
#include "channel_list.h"
#include "display.h"
#include "net_wifi.h"

#if CONFIG_IDF_TARGET_ESP32S3

// T-Deck and Wio Tracker L2: a 320x240 LCD with square corners, and the
// home screen laid out for landscape. Everything is half the size first
// tried here; the fonts are as close to half as LVGL's built-in Montserrat
// goes (8 px is its smallest).
#define UI_PAD    3
#define UI_GAP    3
#define UI_BTN_H  18
#define UI_FIELD_H 16
#define UI_RADIUS 3
#define UI_SAFE_TOP    0
#define UI_SAFE_BOTTOM 0

#define UI_FONT_SMALL (&lv_font_montserrat_8)
#define UI_FONT       (&lv_font_montserrat_10)
#define UI_FONT_TITLE (&lv_font_montserrat_12)
#define UI_STATUS_BAR_W     84
#define UI_STATUS_BAR_H     14
#define UI_SPINNER_SIZE     20
#define UI_ICON_BUTTON_SIZE 16
#define UI_EDIT_BUTTON_H    15
#define UI_CLEAR_BUTTON_W   16
#define UI_TALK_BUTTON_H    28
#define UI_SLIDER_H         7
#define UI_VOLUME_VALUE_W   22
#define UI_METER_H          6  // any thinner and it reads as a hairline
// The Zello link icon is drawn in pixels: this divides its size.
#define UI_ZELLO_ICON_DIV   2

// The home screen as rows across a wide screen.
#define UI_LANDSCAPE    1
// The T-Deck types text on its keyboard, so the on-screen one is never shown;
// the Wio Tracker L2 is touch only.
#if CONFIG_ZELLO_BOARD_TDECK
#define UI_HAS_KEYBOARD 1
#else
#define UI_HAS_KEYBOARD 0
#endif

#else

#define UI_PAD    16
#define UI_GAP    14
#define UI_BTN_H  96
#define UI_FIELD_H 72
#define UI_RADIUS 14
// The AMOLED's corners are curved: keep content this far from the top and
// bottom edges. Every screen and full-screen form gets it from
// ui_layout_create(); anything placed outside that layout must add it itself.
#define UI_SAFE_TOP    72
#define UI_SAFE_BOTTOM 32

#define UI_FONT_SMALL (&lv_font_montserrat_20)
#define UI_FONT       (&lv_font_montserrat_28)
#define UI_FONT_TITLE (&lv_font_montserrat_36)
#define UI_STATUS_BAR_W     240
#define UI_STATUS_BAR_H     48
#define UI_SPINNER_SIZE     96
#define UI_ICON_BUTTON_SIZE 80
#define UI_EDIT_BUTTON_H    56
#define UI_CLEAR_BUTTON_W   80
#define UI_TALK_BUTTON_H    260
#define UI_SLIDER_H         28
#define UI_VOLUME_VALUE_W   72
#define UI_METER_H          18

#define UI_ZELLO_ICON_DIV   1

#define UI_LANDSCAPE    0
#define UI_HAS_KEYBOARD 0

#endif

// Zello's own palette, dark: the accent is Zello yellow, and the talk button
// stays green/red so the "safe to talk" signal reads the same as on a radio.
#define UI_COLOR_BG      lv_color_hex(0x000000)
#define UI_COLOR_PANEL   lv_color_hex(0x1a1d23)
#define UI_COLOR_MUTED   lv_color_hex(0x9aa3ad)
#define UI_COLOR_ACCENT  lv_color_hex(0xf5c518)
#define UI_COLOR_LINK    lv_color_hex(0x2f7df6)
#define UI_COLOR_GO      lv_color_hex(0x1f9d55)
#define UI_COLOR_GO_LIT  lv_color_hex(0x35d07f)
#define UI_COLOR_STOP    lv_color_hex(0xd23c3c)
#define UI_COLOR_WARN    lv_color_hex(0xe0a526)

// ---- screens ----
void ui_wifi_show(void);
void ui_wifi_destroy(void);
// The scan list as its own screen: picking a network (or Back) returns to
// `back_to`, and `picked` hears which one.
void ui_wifi_scan(lv_obj_t *back_to, void (*picked)(const net_wifi_ap_t *ap));
void ui_wifi_scan_destroy(void);
// Clears `pass` and, for a secured network, focuses it for typing.
void ui_wifi_prompt_password(lv_obj_t *pass, bool secure);
// Joins in the background behind a busy modal. Success saves the network and
// restarts; failure says why, and with `keep_old` rejoins the saved network.
void ui_wifi_join(const char *ssid, const char *pass, bool keep_old);
// Account form during onboarding; after it, Settings: Wi-Fi and the account
// on one screen behind one Save.
void ui_account_onboard(void);
void ui_account_edit(void);
void ui_account_destroy(void);
// Channel picker: full screen during onboarding, an overlay afterwards.
void ui_channels_onboard(void);
void ui_channels_show(void);
void ui_channels_destroy(void);
void ui_home_show(void);
// Redraws the username header after the account changed.
void ui_home_refresh_account(void);
// Fills `label` with how to reach the web config right now (setup hotspot
// and/or the device's IP); empty when neither is up.
void ui_web_hint_update(lv_obj_t *label);
// A web-config hint label under `parent` that keeps itself current as the
// hotspot comes and goes and the station gets an address.
lv_obj_t *ui_web_hint(lv_obj_t *parent);

// ---- building blocks ----

// A screen laid out as a scrolling content column above an on-screen keyboard
// that appears while a text field has focus. Returns the content column.
lv_obj_t *ui_screen_create(lv_obj_t **out_screen, lv_obj_t **out_keyboard);
// The same layout as a full-size child of `parent` (e.g. lv_layer_top() for a
// full-screen form). Delete *out_root to close it.
lv_obj_t *ui_layout_create(lv_obj_t *parent, lv_obj_t **out_root, lv_obj_t **out_keyboard);
lv_obj_t *ui_title(lv_obj_t *parent, const char *text);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user);
// Labelled one-line text field; shows `kb` while focused.
lv_obj_t *ui_field(lv_obj_t *parent, const char *label, const char *placeholder, lv_obj_t *kb);
void ui_field_attach_keyboard(lv_obj_t *ta, lv_obj_t *kb);
// A flexible transparent filler that pushes what follows to the bottom.
lv_obj_t *ui_spacer(lv_obj_t *parent);

// ---- temporary status modal (one at a time, on the top layer) ----
void ui_modal_busy(const char *text);
// Replaces the modal with `text`. With `ok`, adds an OK button that closes it
// and calls `on_close`; without, the caller closes it (ui_modal_close_after).
void ui_modal_message(const char *text, lv_color_t color, bool ok, void (*on_close)(void));
void ui_modal_close(void);
// A choice: `title`, one stacked button per item, then Cancel. Tapping an item
// closes the modal and calls `cb` with its index; Cancel just closes it.
void ui_modal_menu(const char *title, const char *const *items, const lv_color_t *colors, int count,
                   void (*cb)(int index));
// Closes the modal after `ms`, then calls `then` (may be NULL).
void ui_modal_close_after(uint32_t ms, void (*then)(void));

// Runs `fn(arg)` on a short-lived worker task, for blocking work (scan, join,
// connect) that must not stall LVGL. `fn` takes display_lock() itself before
// touching the UI.
void ui_run_async(const char *name, void (*fn)(void *), void *arg);
