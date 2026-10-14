// Home, which on Zello is also the talk screen: who you are, which channel
// you're in, the recent channel activity, and one big hold-to-talk button.
// Channels and Settings (Wi-Fi and the account) open from the two buttons
// at the top.

#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "display.h"
#include "net_wifi.h"
#include "settings.h"
#include "ui_internal.h"
#include "zello_client.h"
#if CONFIG_ZELLO_BOARD_TDECK
#include "input_tdeck.h"
#endif

#define REFRESH_MS 200

static lv_obj_t *s_scr, *s_username, *s_channel, *s_status, *s_web, *s_log;
static lv_obj_t *s_meter, *s_talk, *s_talk_label;
static lv_timer_t *s_timer;
static char s_log_cache[ZELLO_LOG_MAX];
static char s_status_cache[160];

// ---- header ----------------------------------------------------------------

static void settings_clicked(lv_event_t *e)
{
    ui_account_edit();
}

static void channels_clicked(lv_event_t *e)
{
    ui_channels_show();
}

void ui_home_refresh_account(void)
{
    if (!s_channel) return;
    // The landscape layout has no room for the username.
    if (s_username) lv_label_set_text(s_username, g_settings.username[0] ? g_settings.username : "Not signed in");
    lv_label_set_text_fmt(s_channel, LV_SYMBOL_LIST "  %s",
                          g_settings.channel[0] ? g_settings.channel : "No channel yet");
}

// ---- talk button -----------------------------------------------------------

static char s_talk_cache[96];

// The talk button's second line. It spans the full width in landscape, so
// there it follows on the same line.
#if UI_LANDSCAPE
#define TALK_SEP "  -  "
#else
#define TALK_SEP "\n"
#endif

static void set_talk(lv_color_t color, const char *text)
{
    // Called from the refresh timer, so skip the invalidation when nothing
    // about the button has changed.
    if (!strcmp(text, s_talk_cache)) return;
    strlcpy(s_talk_cache, text, sizeof(s_talk_cache));
    lv_obj_set_style_bg_color(s_talk, color, 0);
    lv_obj_set_style_bg_color(s_talk, color, LV_STATE_PRESSED);
    lv_label_set_text(s_talk_label, text);
}

static void talk_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (g_settings.ptt_latch) {
        // Latched: one tap starts, the next stops. Ignore the release.
        if (code == LV_EVENT_CLICKED) zello_set_ptt(!zello_ptt());
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        zello_set_ptt(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        zello_set_ptt(false);
    }
}

// The volume value label rides in the event's user data, so the row can sit
// on any screen.
static void volume_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        int volume = lv_slider_get_value(lv_event_get_target(e));
        g_settings.volume = volume;
        audio_set_volume(volume);
        lv_label_set_text_fmt(lv_event_get_user_data(e), "%d%%", volume);
        // A keypad step is a whole change: let it be heard. A drag beeps once,
        // when it is let go.
        if (!lv_obj_has_state(lv_event_get_target(e), LV_STATE_PRESSED)) audio_beep();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DEFOCUSED) {
        // Released a drag, or (keypad) moved on after stepping it.
        if (code != LV_EVENT_DEFOCUSED) audio_beep();
        settings_save();
    }
}

// A borderless, transparent flex row.
static lv_obj_t *plain_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, UI_GAP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// The speaker volume slider with its icon and value, saved as it changes.
static lv_obj_t *ui_volume_row(lv_obj_t *parent)
{
    lv_obj_t *volume_row = plain_row(parent);
    lv_obj_set_height(volume_row, UI_FIELD_H);
    lv_obj_t *volume_icon = lv_label_create(volume_row);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(volume_icon, UI_FONT, 0);
    lv_obj_set_style_text_color(volume_icon, UI_COLOR_MUTED, 0);
    lv_obj_t *volume = lv_slider_create(volume_row);
    lv_slider_set_range(volume, 0, 100);
    lv_slider_set_value(volume, g_settings.volume, LV_ANIM_OFF);
    lv_obj_set_height(volume, UI_SLIDER_H);
    lv_obj_set_flex_grow(volume, 1);
    lv_obj_t *value = lv_label_create(volume_row);
    lv_label_set_text_fmt(value, "%u%%", g_settings.volume);
    lv_obj_set_width(value, UI_VOLUME_VALUE_W);
    lv_obj_set_style_text_font(value, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(value, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(volume, volume_event, LV_EVENT_ALL, value);
    return volume_row;
}

// ---- periodic refresh ------------------------------------------------------

static void describe(const zello_status_t *st, char *out, size_t size)
{
    if (!net_wifi_has_ip()) {
        snprintf(out, size, LV_SYMBOL_WIFI " Connecting to %s...", g_settings.wifi_ssid);
        return;
    }
    switch (st->state) {
    case ZELLO_OFFLINE:
        snprintf(out, size, "%s", st->error[0] ? st->error : "Offline.");
        break;
    case ZELLO_CONNECTING:
        snprintf(out, size, "Connecting to Zello...");
        break;
    case ZELLO_LOGGING_IN:
        snprintf(out, size, "Signing in as %s...", g_settings.username);
        break;
    case ZELLO_ONLINE:
        snprintf(out, size, LV_SYMBOL_OK " In %s  -  %lu listening", st->channel,
                 (unsigned long)st->users_online);
        break;
    default:
        snprintf(out, size, "%s", st->error[0] ? st->error : "Couldn't sign in.");
        break;
    }
}

static lv_color_t status_color(const zello_status_t *st)
{
    if (!net_wifi_has_ip()) return UI_COLOR_WARN;
    switch (st->state) {
    case ZELLO_ONLINE: return UI_COLOR_MUTED;
    case ZELLO_FAILED: return UI_COLOR_STOP;
    default: return UI_COLOR_WARN;
    }
}

static void refresh(lv_timer_t *t)
{
    zello_status_t st;
    zello_get_status(&st);

    char text[sizeof(s_status_cache)];
    describe(&st, text, sizeof(text));
    if (strcmp(text, s_status_cache)) {
        strlcpy(s_status_cache, text, sizeof(s_status_cache));
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, status_color(&st), 0);
    }
    ui_web_hint_update(s_web);

    // The talk button doubles as the channel indicator: green when the channel
    // is yours to use, red while someone else holds it, grey when there is no
    // channel to talk on at all.
    if (st.state != ZELLO_ONLINE) {
        set_talk(UI_COLOR_PANEL, LV_SYMBOL_AUDIO "  OFFLINE");
    } else if (st.ptt) {
        set_talk(UI_COLOR_GO_LIT, LV_SYMBOL_AUDIO "  TRANSMITTING" TALK_SEP "release to listen");
    } else if (st.starting) {
        set_talk(UI_COLOR_WARN, LV_SYMBOL_AUDIO "  STARTING...");
    } else if (st.receiving) {
        char label[96];
        snprintf(label, sizeof(label), LV_SYMBOL_VOLUME_MAX "  %s" TALK_SEP "is talking", st.talker);
        set_talk(UI_COLOR_STOP, label);
    } else {
        set_talk(UI_COLOR_GO, g_settings.ptt_latch ? LV_SYMBOL_AUDIO "  TAP TO TALK"
                                                   : LV_SYMBOL_AUDIO "  HOLD TO TALK");
    }

    // The meter follows the mic while transmitting and playback otherwise, so
    // it always shows the audio that is actually moving.
    lv_bar_set_value(s_meter, st.ptt ? audio_mic_level() : audio_play_level(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_meter, st.ptt ? UI_COLOR_GO_LIT : UI_COLOR_LINK, LV_PART_INDICATOR);

    char log[sizeof(s_log_cache)];
    zello_get_log(log, sizeof(log));
    if (strcmp(log, s_log_cache)) {
        strlcpy(s_log_cache, log, sizeof(s_log_cache));
        lv_label_set_text(s_log, log[0] ? log : "Channel activity shows up here.");
        // Keep the newest line in view.
        lv_obj_scroll_to_y(lv_obj_get_parent(s_log), LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// ---- screen ----------------------------------------------------------------

#if !UI_LANDSCAPE
static lv_obj_t *icon_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(parent, text, UI_COLOR_PANEL, cb, NULL);
    lv_obj_set_size(b, LV_SIZE_CONTENT, UI_EDIT_BUTTON_H);
    lv_obj_set_style_pad_hor(b, UI_PAD * 2, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), UI_FONT_SMALL, 0);
    lv_obj_set_flex_grow(b, 1);
    return b;
}
#endif

// The level meter: the mic while transmitting, the speaker otherwise.
static void meter_create(lv_obj_t *parent)
{
    s_meter = lv_bar_create(parent);
    lv_obj_set_size(s_meter, LV_PCT(100), UI_METER_H);
    lv_bar_set_range(s_meter, 0, 100);
    lv_bar_set_value(s_meter, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_meter, UI_METER_H / 2, 0);
    lv_obj_set_style_bg_color(s_meter, UI_COLOR_PANEL, 0);
}

void ui_home_show(void)
{
    if (!s_scr) {
        lv_obj_t *col = ui_screen_create(&s_scr, NULL);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

#if UI_LANDSCAPE
        // One header row: the channel, then Channels and Settings as icon
        // buttons, leaving the right-hand end to the status bar.
        lv_obj_t *header = plain_row(col);
        lv_obj_set_style_pad_right(header, UI_STATUS_BAR_W, 0);
        s_channel = ui_label(header, "", UI_FONT, UI_COLOR_ACCENT);
        lv_obj_set_width(s_channel, 0);
        lv_obj_set_flex_grow(s_channel, 1);
        lv_label_set_long_mode(s_channel, LV_LABEL_LONG_DOT);
        for (int i = 0; i < 2; i++) {
            lv_obj_t *b = ui_button(header, i ? LV_SYMBOL_SETTINGS : LV_SYMBOL_LIST, UI_COLOR_PANEL,
                                    i ? settings_clicked : channels_clicked, NULL);
            lv_obj_set_size(b, UI_EDIT_BUTTON_H + UI_PAD, UI_EDIT_BUTTON_H);
        }

        s_status = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);
        lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
        s_web = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);
        lv_label_set_long_mode(s_web, LV_LABEL_LONG_DOT);
#else
        s_username = ui_title(col, "");
        s_channel = ui_label(col, "", UI_FONT, UI_COLOR_ACCENT);

        lv_obj_t *buttons = plain_row(col);
        icon_button(buttons, LV_SYMBOL_LIST "  Channels", channels_clicked);
        icon_button(buttons, LV_SYMBOL_SETTINGS "  Settings", settings_clicked);

        s_status = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);
        s_web = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);
#endif

        // Channel activity: talkers, text messages and errors.
        lv_obj_t *box = lv_obj_create(col);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_style_bg_color(box, UI_COLOR_PANEL, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(box, UI_PAD, 0);
        lv_obj_set_scroll_dir(box, LV_DIR_VER);
        s_log = ui_label(box, "Channel activity shows up here.", UI_FONT_SMALL, UI_COLOR_MUTED);

#if UI_LANDSCAPE
        meter_create(col);  // above the volume, under the activity panel
#endif
        lv_obj_t *volume = ui_volume_row(col);
#if UI_LANDSCAPE
        // Clear space above and below, between the meter and the talk button.
        lv_obj_set_height(volume, UI_FIELD_H + 4 * UI_PAD);
        lv_obj_set_style_pad_ver(volume, 2 * UI_PAD, 0);
#endif

#if !UI_LANDSCAPE
        meter_create(col);
#endif

        s_talk = lv_button_create(col);
        lv_obj_set_size(s_talk, LV_PCT(100), UI_TALK_BUTTON_H);
        lv_obj_set_style_radius(s_talk, UI_RADIUS * 3, 0);
        lv_obj_add_event_cb(s_talk, talk_event, LV_EVENT_ALL, NULL);
        s_talk_label = lv_label_create(s_talk);
        lv_obj_set_style_text_font(s_talk_label, UI_FONT_TITLE, 0);
        lv_obj_set_style_text_align(s_talk_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_talk_label);
        set_talk(UI_COLOR_PANEL, LV_SYMBOL_AUDIO "  OFFLINE");

#if CONFIG_ZELLO_BOARD_TDECK
        input_tdeck_set_talk_screen(s_scr);
#endif
        s_timer = lv_timer_create(refresh, REFRESH_MS, NULL);
        ui_home_refresh_account();
        refresh(NULL);
    }
    lv_screen_load(s_scr);
}
