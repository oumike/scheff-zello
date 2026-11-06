#include "ui.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board.h"
#include "display.h"
#include "net_wifi.h"
#include "onboard.h"
#include "settings.h"
#include "ui_internal.h"
#include "zello_client.h"

static const char *TAG = "ui";

#define STATUS_REFRESH_MS       1000
#define BATTERY_REFRESH_TICKS   30

static lv_obj_t *s_status_bar, *s_wifi_icon, *s_ap_icon, *s_battery_icon, *s_battery_pct;
static lv_obj_t *s_zello_arrows[2];
static uint8_t s_battery_tick;

static const char *battery_symbol(uint8_t percent)
{
    if (percent >= 88) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 63) return LV_SYMBOL_BATTERY_3;
    if (percent >= 38) return LV_SYMBOL_BATTERY_2;
    if (percent >= 13) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

// The Zello link as an up and a down arrow, drawn as lines since the symbol
// font has no such glyph. Each arrow is one polyline: shaft, then the head
// traced out and back from the tip.
#define D UI_ZELLO_ICON_DIV
#define ZELLO_ICON_W (24 / UI_ZELLO_ICON_DIV)
#define ZELLO_ICON_H (28 / UI_ZELLO_ICON_DIV)
static const lv_point_precise_t s_arrow_up[] = {{7 / D, 25 / D}, {7 / D, 3 / D}, {1 / D, 9 / D}, {7 / D, 3 / D},
                                                {13 / D, 9 / D}};
static const lv_point_precise_t s_arrow_down[] = {{17 / D, 3 / D}, {17 / D, 25 / D}, {11 / D, 19 / D},
                                                  {17 / D, 25 / D}, {23 / D, 19 / D}};
#undef D

static lv_obj_t *zello_icon_create(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, ZELLO_ICON_W, ZELLO_ICON_H);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    const lv_point_precise_t *const shapes[2] = {s_arrow_up, s_arrow_down};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *line = lv_line_create(box);
        lv_line_set_points(line, shapes[i], 5);
        lv_obj_set_style_line_width(line, UI_ZELLO_ICON_DIV > 1 ? 2 : 3, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        s_zello_arrows[i] = line;
    }
    return box;
}

// Green online, yellow on the way there, red otherwise.
static lv_color_t zello_color(void)
{
    switch (zello_state()) {
    case ZELLO_ONLINE: return UI_COLOR_GO_LIT;
    case ZELLO_CONNECTING:
    case ZELLO_LOGGING_IN: return UI_COLOR_WARN;
    default: return UI_COLOR_STOP;
    }
}

static void status_update(lv_timer_t *timer)
{
    lv_color_t zc = zello_color();
    for (int i = 0; i < 2; i++) lv_obj_set_style_line_color(s_zello_arrows[i], zc, 0);

    lv_color_t wifi_color = net_wifi_has_ip() ? UI_COLOR_GO_LIT
                            : g_settings.wifi_ssid[0] ? UI_COLOR_STOP
                                                      : UI_COLOR_MUTED;
    lv_obj_set_style_text_color(s_wifi_icon, wifi_color, 0);
    if (net_wifi_ap_active()) {
        lv_obj_remove_flag(s_ap_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ap_icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_battery_tick == 0) {
        uint8_t percent;
        if (board_battery_percent(&percent) == ESP_OK) {
            lv_color_t color = percent <= 10   ? UI_COLOR_STOP
                               : percent <= 25 ? UI_COLOR_WARN
                                               : lv_color_white();
            lv_label_set_text(s_battery_icon, battery_symbol(percent));
            lv_obj_set_style_text_color(s_battery_icon, color, 0);
            lv_label_set_text_fmt(s_battery_pct, "%u%%", (unsigned)percent);
            lv_obj_set_style_text_color(s_battery_pct, color, 0);
            lv_obj_remove_flag(s_battery_pct, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_battery_tick = (s_battery_tick + 1) % BATTERY_REFRESH_TICKS;
}

static void status_ensure(void)
{
    if (!s_status_bar) {
        s_status_bar = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_status_bar, UI_STATUS_BAR_W, UI_STATUS_BAR_H);
        lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_status_bar, 0, 0);
        lv_obj_set_style_pad_all(s_status_bar, 0, 0);
        lv_obj_set_style_pad_column(s_status_bar, UI_GAP, 0);
        lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

        s_wifi_icon = lv_label_create(s_status_bar);
        lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(s_wifi_icon, UI_FONT, 0);

        zello_icon_create(s_status_bar);

        s_ap_icon = lv_label_create(s_status_bar);
        lv_label_set_text(s_ap_icon, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_text_font(s_ap_icon, UI_FONT, 0);
        lv_obj_set_style_text_color(s_ap_icon, UI_COLOR_ACCENT, 0);

        // Hidden until the first reading.
        s_battery_pct = lv_label_create(s_status_bar);
        lv_obj_set_style_text_font(s_battery_pct, UI_FONT_SMALL, 0);
        lv_obj_add_flag(s_battery_pct, LV_OBJ_FLAG_HIDDEN);

        s_battery_icon = lv_label_create(s_status_bar);
        lv_label_set_text(s_battery_icon, LV_SYMBOL_BATTERY_EMPTY);
        lv_obj_set_style_text_font(s_battery_icon, UI_FONT, 0);
        lv_obj_set_style_text_color(s_battery_icon, UI_COLOR_MUTED, 0);

        lv_obj_align(s_status_bar, LV_ALIGN_TOP_RIGHT, -UI_PAD, UI_SAFE_TOP + UI_PAD);
        status_update(NULL);
        lv_timer_create(status_update, STATUS_REFRESH_MS, NULL);
    }
    lv_obj_move_foreground(s_status_bar);
}

// ---- building blocks -------------------------------------------------------

static void kb_event(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void field_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);
#if !UI_HAS_KEYBOARD
        // With a physical keyboard the on-screen one stays hidden; it still
        // exists so every screen's keyboard handling works unchanged.
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
#endif
        // Once the keyboard has taken its space, bring the field into view.
        lv_obj_update_layout(lv_obj_get_screen(ta));
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    } else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        if (lv_keyboard_get_textarea(kb) == ta) {
            lv_keyboard_set_textarea(kb, NULL);
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_field_attach_keyboard(lv_obj_t *ta, lv_obj_t *kb)
{
    lv_obj_add_event_cb(ta, field_event, LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(ta, field_event, LV_EVENT_DEFOCUSED, kb);
}

static lv_obj_t *keyboard_create(lv_obj_t *parent)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), DISPLAY_V_RES * 2 / 5);
    lv_obj_set_style_text_font(kb, UI_FONT, 0);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

lv_obj_t *ui_screen_create(lv_obj_t **out_screen, lv_obj_t **out_keyboard)
{
    return ui_layout_create(NULL, out_screen, out_keyboard);
}

lv_obj_t *ui_layout_create(lv_obj_t *parent, lv_obj_t **out_root, lv_obj_t **out_keyboard)
{
    lv_obj_t *scr = lv_obj_create(parent);
    if (parent) {
        lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_set_style_radius(scr, 0, 0);
    }
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_top(scr, UI_SAFE_TOP, 0);
    lv_obj_set_style_pad_bottom(scr, UI_SAFE_BOTTOM, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_width(col, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_pad_all(col, UI_PAD, 0);
    lv_obj_set_style_pad_row(col, UI_GAP, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    if (out_keyboard) *out_keyboard = keyboard_create(scr);
    status_ensure();
    *out_root = scr;
    return col;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t *ui_title(lv_obj_t *parent, const char *text)
{
    return ui_label(parent, text, UI_FONT_TITLE, lv_color_white());
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, LV_PCT(100), UI_BTN_H);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_radius(b, UI_RADIUS, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, UI_FONT, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_field(lv_obj_t *parent, const char *label, const char *placeholder, lv_obj_t *kb)
{
    ui_label(parent, label, UI_FONT_SMALL, UI_COLOR_MUTED);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_size(ta, LV_PCT(100), UI_FIELD_H);
    lv_obj_set_style_text_font(ta, UI_FONT, 0);
    lv_obj_set_style_radius(ta, UI_RADIUS, 0);
    if (kb) ui_field_attach_keyboard(ta, kb);
    return ta;
}

lv_obj_t *ui_spacer(lv_obj_t *parent)
{
    lv_obj_t *sp = lv_obj_create(parent);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    return sp;
}

// ---- modal -----------------------------------------------------------------

static lv_obj_t *s_modal;
static lv_timer_t *s_modal_timer;
static void (*s_modal_on_close)(void);

static lv_obj_t *modal_box(void)
{
    if (s_modal_timer) {
        lv_timer_delete(s_modal_timer);
        s_modal_timer = NULL;
    }
    if (s_modal) lv_obj_delete(s_modal);
    s_modal_on_close = NULL;

    // Full-screen scrim that swallows touches to the screen underneath.
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_modal);
    lv_obj_set_size(box, DISPLAY_H_RES - 4 * UI_PAD, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, UI_COLOR_PANEL, 0);
    lv_obj_set_style_radius(box, UI_RADIUS * 2, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, UI_PAD * 2, 0);
    lv_obj_set_style_pad_row(box, UI_GAP * 2, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *modal_text(lv_obj_t *box, const char *text, lv_color_t color)
{
    lv_obj_t *l = ui_label(box, text, UI_FONT, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

void ui_modal_busy(const char *text)
{
    lv_obj_t *box = modal_box();
    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, UI_SPINNER_SIZE, UI_SPINNER_SIZE);
    modal_text(box, text, lv_color_white());
}

static void (*s_menu_cb)(int);

static void menu_clicked(lv_event_t *e)
{
    void (*cb)(int) = s_menu_cb;
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    s_menu_cb = NULL;
    ui_modal_close();
    if (cb && index >= 0) cb(index);
}

void ui_modal_menu(const char *title, const char *const *items, const lv_color_t *colors, int count,
                   void (*cb)(int index))
{
    lv_obj_t *box = modal_box();
    lv_obj_set_style_pad_row(box, UI_GAP, 0);
    lv_obj_t *t = ui_label(box, title, UI_FONT_TITLE, lv_color_white());
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    for (int i = 0; i < count; i++) ui_button(box, items[i], colors[i], menu_clicked, (void *)(intptr_t)i);
    ui_button(box, "Cancel", UI_COLOR_PANEL, menu_clicked, (void *)(intptr_t)-1);
    s_menu_cb = cb;
}

static void modal_ok(lv_event_t *e)
{
    ui_modal_close();
}

void ui_modal_message(const char *text, lv_color_t color, bool ok, void (*on_close)(void))
{
    lv_obj_t *box = modal_box();
    modal_text(box, text, color);
    if (ok) ui_button(box, "OK", UI_COLOR_ACCENT, modal_ok, NULL);
    s_modal_on_close = on_close;
}

void ui_modal_close(void)
{
    void (*then)(void) = s_modal_on_close;
    s_modal_on_close = NULL;
    if (s_modal_timer) {
        lv_timer_delete(s_modal_timer);
        s_modal_timer = NULL;
    }
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
    if (then) then();
}

static void modal_timer_cb(lv_timer_t *t)
{
    s_modal_timer = NULL;  // one-shot: LVGL deletes it after this run
    ui_modal_close();
}

void ui_modal_close_after(uint32_t ms, void (*then)(void))
{
    s_modal_on_close = then;
    if (s_modal_timer) lv_timer_delete(s_modal_timer);
    s_modal_timer = lv_timer_create(modal_timer_cb, ms, NULL);
    lv_timer_set_repeat_count(s_modal_timer, 1);
}

// ---- workers ---------------------------------------------------------------

typedef struct {
    void (*fn)(void *);
    void *arg;
} job_t;

static void job_task(void *p)
{
    job_t job = *(job_t *)p;
    free(p);
    job.fn(job.arg);
    vTaskDelete(NULL);
}

void ui_run_async(const char *name, void (*fn)(void *), void *arg)
{
    job_t *job = malloc(sizeof(*job));
    if (!job) return;
    *job = (job_t){fn, arg};
    if (xTaskCreate(job_task, name, 6144, job, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start %s", name);
        free(job);
    }
}

// ---- boot ------------------------------------------------------------------

void ui_web_hint_update(lv_obj_t *label)
{
    char ip[16];
    bool sta = net_wifi_sta_ip(ip, sizeof(ip));
    if (net_wifi_ap_active() && sta) {
        lv_label_set_text_fmt(label,
                              LV_SYMBOL_SETTINGS " Or use a browser: open http://%s, or join Wi-Fi \"%s\" "
                              "and open http://192.168.4.1",
                              ip, net_wifi_ap_ssid());
    } else if (net_wifi_ap_active()) {
        lv_label_set_text_fmt(label,
                              LV_SYMBOL_SETTINGS " Or set up from a phone or computer: join Wi-Fi \"%s\", "
                              "then open http://192.168.4.1",
                              net_wifi_ap_ssid());
    } else if (sta) {
        lv_label_set_text_fmt(label, LV_SYMBOL_SETTINGS " Web config: http://%s", ip);
    } else {
        lv_label_set_text(label, "");
    }
}

static void web_hint_tick(lv_timer_t *t)
{
    ui_web_hint_update(lv_timer_get_user_data(t));
}

static void web_hint_deleted(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

lv_obj_t *ui_web_hint(lv_obj_t *parent)
{
    lv_obj_t *label = ui_label(parent, "", UI_FONT_SMALL, UI_COLOR_MUTED);
    ui_web_hint_update(label);
    lv_timer_t *timer = lv_timer_create(web_hint_tick, STATUS_REFRESH_MS, label);
    lv_obj_add_event_cb(label, web_hint_deleted, LV_EVENT_DELETE, timer);
    return label;
}

// Onboarding: Wi-Fi, then the Zello account, then a channel, then Home for
// good. Steps can complete on the screen or in the web config; either way this
// moves on.
typedef enum { STAGE_NONE, STAGE_WIFI, STAGE_ACCOUNT, STAGE_CHANNEL, STAGE_HOME } stage_t;
static stage_t s_stage;

static void show_stage(void)
{
    stage_t next = !onboard_wifi_done()      ? STAGE_WIFI
                   : !onboard_account_done() ? STAGE_ACCOUNT
                   : !onboard_channel_done() ? STAGE_CHANNEL
                                             : STAGE_HOME;
    if (next != s_stage) {
        stage_t prev = s_stage;
        s_stage = next;
        switch (next) {
        case STAGE_WIFI: ui_wifi_show(); break;
        case STAGE_ACCOUNT: ui_account_onboard(); break;
        case STAGE_CHANNEL: ui_channels_onboard(); break;
        default: ui_home_show(); break;
        }
        if (prev == STAGE_WIFI) ui_wifi_destroy();
        if (prev == STAGE_ACCOUNT) ui_account_destroy();
        if (prev == STAGE_CHANNEL) ui_channels_destroy();
    }
    ui_home_refresh_account();
}

static void onboard_changed(void)
{
    if (!display_lock()) return;
    show_stage();
    display_unlock();
}

void ui_start(void)
{
    display_lock();
    onboard_set_ui_cb(onboard_changed);
    show_stage();
    ui_saver_init();
    display_unlock();
}
