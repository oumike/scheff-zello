// The screensaver, and the screen button that brings it up, the way
// camillia-mt does its lock screen:
//
//   UI --(button tap, or idle)--> SAVER --(dwell)--> DARK
//    ^                              |                  |
//    +-------(button held 2 s)------+------------------+
//
// A tap puts the UI away; only a two-second hold brings it back, which is the
// one input a pocket doesn't produce. A tap on a dark screen lights the
// screensaver again. The screensaver is a schefflera bouncing slowly round a
// black screen, carrying the Zello status and the channel's latest activity
// with it (moving all of it keeps the AMOLED from burning in).
//
// The screen button: BOOT on the T-Display-P4, Power on the Tanmatsu, Wake on
// the Wio Tracker L2. The T-Deck has none to spare, so, as on camillia-mt,
// holding the trackball click puts it away from anywhere but Home (where the
// click is talk) and wakes it; a short click stays Enter, and a typed key
// wakes it too.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"
#include "onboard.h"
#include "ui.h"
#include "ui_internal.h"
#include "zello_client.h"
#if UI_HAS_KEYBOARD
#include "input_keys.h"
#endif

static const char *TAG = "saver";

#define IDLE_MS        (2 * 60 * 1000)   // UI untouched this long: screensaver
#define DARK_MS        (10 * 60 * 1000)  // screensaver up this long: dark panel
#define HOLD_MS        2000
#define DEBOUNCE_MS    25
#define TICK_MS        25
#define MOVE_EVERY     2                 // ticks per bounce step
#define TEXT_EVERY     20                // ticks per status refresh
#define ACTIVITY_LINES 2

#if CONFIG_ZELLO_BOARD_TANMATSU
#define PLANT 150
#define BOX_W 460
#define STEP  2
#elif CONFIG_IDF_TARGET_ESP32S3
#define PLANT 72
#define BOX_W 200
#define STEP  1
#else
#define PLANT 220
#define BOX_W 480
#define STEP  2
#endif

typedef enum { SAVER_OFF, SAVER_ON, SAVER_DARK } saver_state_t;
static volatile saver_state_t s_state;

static lv_obj_t *s_overlay, *s_box, *s_status, *s_activity;
static uint32_t s_since;  // when the screensaver last lit
static int32_t s_x, s_y, s_dx = STEP, s_dy = STEP;
static uint32_t s_ticks;
static char s_status_cache[160];
static size_t s_log_seen = SIZE_MAX;
static char s_talker_seen[64];  // who was shown talking, or "" for the log

// ---- the plant -------------------------------------------------------------

// A schefflera's palmate leaf: seven leaflets fanned round the top of a
// stalk, the middle one longest.
#define LEAFLETS 7
static const int16_t kAngle[LEAFLETS] = {-112, -75, -37, 0, 37, 75, 112};
static const uint8_t kLength[LEAFLETS] = {62, 82, 95, 100, 95, 82, 62};  // % of the longest
static lv_point_precise_t s_leaf_pts[LEAFLETS][2], s_rib_pts[LEAFLETS][2], s_stalk_pts[2];

static lv_obj_t *line(lv_obj_t *parent, const lv_point_precise_t *pts, int32_t width, lv_color_t color)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, 2);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_pos(l, 0, 0);
    return l;
}

static lv_obj_t *plant_create(lv_obj_t *parent)
{
    const float hub_x = PLANT / 2.0f, hub_y = PLANT * 0.52f;
    const float longest = PLANT * 0.46f, start = PLANT * 0.06f;
    const int32_t leaf_w = PLANT * 15 / 100;
    const int32_t rib_w = PLANT / 70 > 1 ? PLANT / 70 : 1;
    const int32_t stalk_w = PLANT / 28 > 2 ? PLANT / 28 : 2;

    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PLANT, PLANT);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_stalk_pts[0] = (lv_point_precise_t){hub_x, hub_y};
    s_stalk_pts[1] = (lv_point_precise_t){hub_x, PLANT - stalk_w};
    line(p, s_stalk_pts, stalk_w, lv_color_hex(0x7a8b3a));

    for (int i = 0; i < LEAFLETS; i++) {
        float a = kAngle[i] * (float)M_PI / 180.0f;
        float s = sinf(a), c = cosf(a);
        // The rounded caps reach half a width past each end.
        float tip = longest * kLength[i] / 100.0f - leaf_w / 2.0f;
        s_leaf_pts[i][0] = (lv_point_precise_t){hub_x + s * start, hub_y - c * start};
        s_leaf_pts[i][1] = (lv_point_precise_t){hub_x + s * tip, hub_y - c * tip};
        s_rib_pts[i][0] = s_leaf_pts[i][0];
        s_rib_pts[i][1] = (lv_point_precise_t){hub_x + s * tip * 0.9f, hub_y - c * tip * 0.9f};
        line(p, s_leaf_pts[i], leaf_w, lv_color_hex(i % 2 ? 0x2e8b4e : 0x3fa65f));
    }
    for (int i = 0; i < LEAFLETS; i++) {
        lv_obj_t *rib = line(p, s_rib_pts[i], rib_w, lv_color_hex(0xb9eac5));
        lv_obj_set_style_line_opa(rib, LV_OPA_70, 0);
    }

    lv_obj_t *hub = lv_obj_create(p);
    lv_obj_remove_style_all(hub);
    lv_obj_set_size(hub, leaf_w, leaf_w);
    lv_obj_set_pos(hub, (int32_t)hub_x - leaf_w / 2, (int32_t)hub_y - leaf_w / 2);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0x2a6b3c), 0);
    return p;
}

// ---- status ----------------------------------------------------------------

static void refresh_text(bool force)
{
    zello_status_t st;
    zello_get_status(&st);

    char text[sizeof(s_status_cache)];
    ui_zello_describe(&st, text, sizeof(text));
    if (force || strcmp(text, s_status_cache)) {
        strlcpy(s_status_cache, text, sizeof(s_status_cache));
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, ui_zello_color(&st), 0);
    }

    if (st.receiving) {
        const char *who = st.talker[0] ? st.talker : "Someone";
        if (force || strcmp(who, s_talker_seen)) {
            lv_label_set_text_fmt(s_activity, LV_SYMBOL_VOLUME_MAX " %s is talking", who);
            lv_obj_set_style_text_color(s_activity, UI_COLOR_STOP, 0);
            strlcpy(s_talker_seen, who, sizeof(s_talker_seen));
        }
        return;
    }
    char recent[ACTIVITY_LINES * 160];
    size_t seen = zello_get_recent_log(recent, sizeof(recent), ACTIVITY_LINES);
    if (force || s_talker_seen[0] || seen != s_log_seen) {
        lv_label_set_text(s_activity, recent[0] ? recent : "No channel activity yet.");
        lv_obj_set_style_text_color(s_activity, UI_COLOR_MUTED, 0);
        s_log_seen = seen;
        s_talker_seen[0] = 0;
    }
}

// ---- states ----------------------------------------------------------------

static void show(const char *why)
{
    if (s_state != SAVER_OFF) return;
    if (zello_ptt()) zello_set_ptt(false);

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_style_bg_color(s_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    // Taps land here and go no further.
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, BOX_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_box, UI_GAP, 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    plant_create(s_box);
    s_status = ui_label(s_box, "", UI_FONT, UI_COLOR_MUTED);
    s_activity = ui_label(s_box, "", UI_FONT_SMALL, UI_COLOR_MUTED);
    lv_obj_t *labels[] = {s_status, s_activity};
    for (size_t i = 0; i < 2; i++) {
        lv_obj_set_width(labels[i], LV_PCT(100));
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    refresh_text(true);

    lv_obj_update_layout(s_box);
    s_x = (DISPLAY_H_RES - BOX_W) / 2;
    s_y = (DISPLAY_V_RES - lv_obj_get_height(s_box)) / 2;
    lv_obj_set_pos(s_box, s_x, s_y);

    s_state = SAVER_ON;
    s_since = lv_tick_get();
    ESP_LOGI(TAG, "screensaver on (%s)", why);
}

static void wake(const char *why)
{
    if (s_state == SAVER_OFF) return;
    if (s_state == SAVER_DARK) display_backlight(true);
    lv_obj_delete(s_overlay);
    s_overlay = s_box = s_status = s_activity = NULL;
    s_state = SAVER_OFF;
    lv_display_trigger_activity(NULL);
    ESP_LOGI(TAG, "screen awake (%s)", why);
}

static void go_dark(void)
{
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    display_backlight(false);
    s_state = SAVER_DARK;
    ESP_LOGI(TAG, "screen dark");
}

static void relight(void)
{
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    refresh_text(true);
    display_backlight(true);
    s_state = SAVER_ON;
    s_since = lv_tick_get();
    ESP_LOGI(TAG, "screensaver lit again");
}

bool ui_saver_active(void)
{
    return s_state != SAVER_OFF;
}

bool ui_saver_key(void)
{
    if (s_state == SAVER_OFF) return false;
    wake("key");
    return true;
}

// ---- the screen button -----------------------------------------------------

static bool button_down(void)
{
#if CONFIG_ZELLO_BOARD_TDISPLAY_P4
    return gpio_get_level(BOARD_WAKE_BUTTON) == 0;
#elif CONFIG_ZELLO_BOARD_TANMATSU
    return input_keys_power_button();
#elif CONFIG_ZELLO_BOARD_WIO_TRACKER_L2
    bool pressed = false;
    board_wake_button(&pressed);
    return pressed;
#elif CONFIG_ZELLO_BOARD_TDECK
    return gpio_get_level(BOARD_TBALL_CLICK) == 0;
#else
    return false;
#endif
}

static void button_tap(void)
{
#if CONFIG_ZELLO_BOARD_TDECK
    // The click: Enter, and nothing while the UI is away.
    if (s_state == SAVER_OFF) input_keys_click();
#else
    if (s_state == SAVER_OFF) show("button");
    else if (s_state == SAVER_DARK) relight();
#endif
}

static void button_hold(void)
{
    if (s_state != SAVER_OFF) {
        wake("button held");
        return;
    }
#if CONFIG_ZELLO_BOARD_TDECK
    // On Home the click is talk, and holding it is a long transmission.
    if (!input_keys_talk_active()) show("trackball held");
#endif
    // Otherwise already awake: the hold is spent, so letting go isn't a tap.
}

static void button_poll(void)
{
    static bool raw, stable, claimed;
    static uint32_t changed, pressed_at;

    bool now = button_down();
    if (now != raw) {
        raw = now;
        changed = lv_tick_get();
    }
    if (stable != raw && lv_tick_elaps(changed) >= DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            pressed_at = lv_tick_get();
            claimed = false;
        } else if (!claimed) {
            button_tap();
        }
    }
    if (stable && !claimed && lv_tick_elaps(pressed_at) >= HOLD_MS) {
        claimed = true;
        button_hold();
    }
}

// ---- timer -----------------------------------------------------------------

static void bounce(void)
{
    int32_t w = lv_obj_get_width(s_box), h = lv_obj_get_height(s_box);
    int32_t max_x = DISPLAY_H_RES - w, min_y = UI_SAFE_TOP, max_y = DISPLAY_V_RES - UI_SAFE_BOTTOM - h;
    if (max_x < 0) max_x = 0;
    if (max_y < min_y) max_y = min_y;
    s_x += s_dx;
    s_y += s_dy;
    if (s_x <= 0 || s_x >= max_x) s_dx = s_x <= 0 ? STEP : -STEP;
    if (s_y <= min_y || s_y >= max_y) s_dy = s_y <= min_y ? STEP : -STEP;
    s_x = LV_CLAMP(0, s_x, max_x);
    s_y = LV_CLAMP(min_y, s_y, max_y);
    lv_obj_set_pos(s_box, s_x, s_y);
}

static void tick(lv_timer_t *t)
{
    button_poll();
    s_ticks++;

    if (s_state == SAVER_OFF) {
        // Transmitting (on a button LVGL doesn't see) is using the device.
        if (zello_ptt()) lv_display_trigger_activity(NULL);
        if (onboard_complete() && lv_display_get_inactive_time(NULL) >= IDLE_MS) show("idle");
        return;
    }

    // Anything opened since (a modal, a busy spinner) stays behind it.
    lv_obj_t *top = lv_layer_top();
    if (lv_obj_get_index(s_overlay) != (int32_t)lv_obj_get_child_count(top) - 1) {
        lv_obj_move_foreground(s_overlay);
    }

    if (s_state == SAVER_DARK) return;
    if (lv_tick_elaps(s_since) >= DARK_MS) {
        go_dark();
        return;
    }
    if (s_ticks % TEXT_EVERY == 0) refresh_text(false);
    if (s_ticks % MOVE_EVERY == 0) bounce();
}

void ui_saver_init(void)
{
#if CONFIG_ZELLO_BOARD_TDISPLAY_P4
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_WAKE_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
#endif
    lv_timer_create(tick, TICK_MS, NULL);
}
