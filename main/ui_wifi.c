// Wi-Fi setup: shown on first boot (no saved network). Scan and pick from a
// list, or type the SSID; then the password (not masked) and Join. The scan
// list and the join are shared with Settings, which changes networks later.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "input_keys.h"
#include "net_wifi.h"
#include "onboard.h"
#include "settings.h"
#include "ui_internal.h"

#define SCAN_MAX      30
#define JOIN_TIMEOUT  20000

#define HINT_DEFAULT "Open networks don't need a password - leave it blank."
#define HINT_OPEN    "This network is open - no password needed. Tap Join."

static lv_obj_t *s_setup_scr, *s_ssid, *s_pass, *s_hint, *s_kb, *s_web_hint;
static lv_obj_t *s_scan_scr, *s_scan_list, *s_scan_status, *s_scan_again;

static net_wifi_ap_t s_aps[SCAN_MAX];
static int s_ap_count;
static bool s_scanning;

// Where the scan list goes back to, and who hears about the network picked.
static lv_obj_t *s_scan_back_to;
static void (*s_scan_picked)(const net_wifi_ap_t *ap);

typedef struct {
    char ssid[33];
    char pass[65];
    bool keep_old;  // on failure, go back to the saved network
} join_req_t;

static join_req_t s_joined;  // the network that just joined, saved after "Connected!"

static void start_scan(void);
static void scan_close(void);

// ---- join ------------------------------------------------------------------

static void joined_continue(void)
{
    if (onboard_save_wifi(s_joined.ssid, s_joined.pass) == ESP_OK) {
        ui_modal_message("Wi-Fi saved.\nRestarting...", UI_COLOR_GO_LIT, false, NULL);
    } else {
        ui_modal_message("Connected, but couldn't save the Wi-Fi settings.", UI_COLOR_STOP, true, NULL);
    }
}

void ui_wifi_destroy(void)
{
    if (s_setup_scr) lv_obj_delete_async(s_setup_scr);
    s_setup_scr = NULL;
    s_web_hint = NULL;
    scan_close();
}

static void join_worker(void *arg)
{
    join_req_t *req = arg;
    net_join_result_t res = net_wifi_join(req->ssid, req->pass, JOIN_TIMEOUT);

    char msg[128];
    // A failed join leaves the station down; after onboarding that would
    // strand the device, so rejoin the network it was on.
    if (res != NET_JOIN_OK && req->keep_old && g_settings.wifi_ssid[0]) {
        net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
    }

    display_lock();
    if (res == NET_JOIN_OK) s_joined = *req;
    switch (res) {
    case NET_JOIN_OK:
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  Connected!\nRestarting...\n%s", req->ssid);
        ui_modal_message(msg, UI_COLOR_GO_LIT, false, NULL);
        ui_modal_close_after(1000, joined_continue);
        break;
    case NET_JOIN_BAD_PASSWORD:
        ui_modal_message("Couldn't connect:\nthe password was rejected.", UI_COLOR_STOP, true, NULL);
        break;
    case NET_JOIN_NOT_FOUND:
        snprintf(msg, sizeof(msg), "Couldn't find \"%s\".\nCheck the name, or scan again.", req->ssid);
        ui_modal_message(msg, UI_COLOR_STOP, true, NULL);
        break;
    case NET_JOIN_TIMEOUT:
        ui_modal_message("Timed out waiting for the network.\nTry again.", UI_COLOR_STOP, true, NULL);
        break;
    default:
        ui_modal_message("Couldn't connect to the network.", UI_COLOR_STOP, true, NULL);
        break;
    }
    display_unlock();
    free(req);
}

void ui_wifi_join(const char *ssid, const char *pass, bool keep_old)
{
    join_req_t *req = calloc(1, sizeof(*req));
    if (!req) return;
    strlcpy(req->ssid, ssid, sizeof(req->ssid));
    strlcpy(req->pass, pass, sizeof(req->pass));
    req->keep_old = keep_old;

    char msg[80];
    snprintf(msg, sizeof(msg), "Connecting to\n%s...", req->ssid);
    ui_modal_busy(msg);
    ui_run_async("wifi_join", join_worker, req);
}

static void join_clicked(lv_event_t *e)
{
    const char *ssid = lv_textarea_get_text(s_ssid);
    if (!ssid[0]) {
        ui_modal_message("Enter a network name first,\nor tap Scan.", UI_COLOR_WARN, true, NULL);
        return;
    }
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    ui_wifi_join(ssid, lv_textarea_get_text(s_pass), false);
}

// ---- scan screen -----------------------------------------------------------

void ui_wifi_prompt_password(lv_obj_t *pass, bool secure)
{
    lv_textarea_set_text(pass, "");
    // Prompt for the password straight away; an open network skips it.
    if (!secure) return;
#if UI_HAS_KEYBOARD
    // Focus belongs to the keyboard's group: marking the field focused by
    // hand would blink its cursor while keys went to another field.
    input_keys_focus(pass);
#else
    lv_obj_add_state(pass, LV_STATE_FOCUSED);
    lv_obj_send_event(pass, LV_EVENT_FOCUSED, NULL);
#endif
}

static void setup_picked(const net_wifi_ap_t *ap)
{
    lv_textarea_set_text(s_ssid, ap->ssid);
    lv_label_set_text(s_hint, ap->secure ? HINT_DEFAULT : HINT_OPEN);
    lv_obj_set_style_text_color(s_hint, ap->secure ? UI_COLOR_MUTED : UI_COLOR_GO_LIT, 0);
    ui_wifi_prompt_password(s_pass, ap->secure);
}

static void ap_clicked(lv_event_t *e)
{
    const net_wifi_ap_t *ap = &s_aps[(int)(intptr_t)lv_event_get_user_data(e)];
    lv_screen_load_anim(s_scan_back_to, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    s_scan_picked(ap);
}

static const char *signal_text(int8_t rssi)
{
    return rssi >= -55 ? "Excellent" : rssi >= -67 ? "Good" : rssi >= -75 ? "Fair" : "Weak";
}

static void show_scan_results(void)
{
    lv_obj_clean(s_scan_list);
    if (s_ap_count < 0) {
        lv_label_set_text(s_scan_status, "Scan failed. Try again.");
    } else if (s_ap_count == 0) {
        lv_label_set_text(s_scan_status, "No networks found.");
    } else {
        lv_label_set_text_fmt(s_scan_status, "%d networks found. Tap one to use it.", s_ap_count);
    }

    for (int i = 0; i < s_ap_count; i++) {
        const net_wifi_ap_t *ap = &s_aps[i];
        lv_obj_t *b = lv_button_create(s_scan_list);
        lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, UI_COLOR_PANEL, 0);
        lv_obj_set_style_radius(b, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(b, UI_PAD, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_event_cb(b, ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(b);
        lv_label_set_text(name, ap->ssid);
        lv_obj_set_style_text_font(name, UI_FONT, 0);

        lv_obj_t *meta = lv_label_create(b);
        lv_label_set_text_fmt(meta, LV_SYMBOL_WIFI " %s (%d dBm)  -  %s", signal_text(ap->rssi), ap->rssi,
                              ap->secure ? "Secured" : "Open");
        lv_obj_set_style_text_font(meta, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(meta, ap->secure ? UI_COLOR_MUTED : UI_COLOR_GO_LIT, 0);
    }
    lv_obj_remove_state(s_scan_again, LV_STATE_DISABLED);
}

static void scan_worker(void *arg)
{
    int n = net_wifi_scan(s_aps, SCAN_MAX);
    display_lock();
    s_ap_count = n;
    s_scanning = false;
    if (s_scan_scr) show_scan_results();
    display_unlock();
}

static void start_scan(void)
{
    if (s_scanning) return;
    s_scanning = true;
    lv_obj_clean(s_scan_list);
    lv_obj_t *sp = lv_spinner_create(s_scan_list);
    lv_obj_set_size(sp, UI_SPINNER_SIZE, UI_SPINNER_SIZE);
    lv_label_set_text(s_scan_status, "Scanning for networks...");
    lv_obj_add_state(s_scan_again, LV_STATE_DISABLED);
    ui_run_async("wifi_scan", scan_worker, NULL);
}

static void scan_back(lv_event_t *e)
{
    lv_screen_load_anim(s_scan_back_to, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void scan_close(void)
{
    if (s_scan_scr) lv_obj_delete_async(s_scan_scr);
    s_scan_scr = NULL;
}

static void scan_again(lv_event_t *e)
{
    start_scan();
}

static void build_scan_screen(void)
{
    lv_obj_t *col = ui_screen_create(&s_scan_scr, NULL);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = ui_button(col, LV_SYMBOL_LEFT "  Back", UI_COLOR_PANEL, scan_back, NULL);
    lv_obj_set_width(back, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(back, UI_PAD * 2, 0);

    ui_title(col, "Choose a network");
    s_scan_status = ui_label(col, "", UI_FONT_SMALL, UI_COLOR_MUTED);

    s_scan_list = lv_obj_create(col);
    lv_obj_set_width(s_scan_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_scan_list, 1);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_scan_list, 0, 0);
    lv_obj_set_style_pad_row(s_scan_list, UI_GAP / 2, 0);
    lv_obj_set_style_bg_opa(s_scan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);

    s_scan_again = ui_button(col, LV_SYMBOL_REFRESH "  Scan again", UI_COLOR_ACCENT, scan_again, NULL);
}

void ui_wifi_scan(lv_obj_t *back_to, void (*picked)(const net_wifi_ap_t *ap))
{
    s_scan_back_to = back_to;
    s_scan_picked = picked;
    if (!s_scan_scr) build_scan_screen();
    lv_screen_load_anim(s_scan_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
    start_scan();
}

void ui_wifi_scan_destroy(void)
{
    scan_close();
}

static void scan_clicked(lv_event_t *e)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    ui_wifi_scan(s_setup_scr, setup_picked);
}

// ---- setup screen ----------------------------------------------------------

void ui_wifi_show(void)
{
    lv_obj_t *col = ui_screen_create(&s_setup_scr, &s_kb);

    ui_title(col, "Wi-Fi Setup");
    ui_label(col, "Scan for a nearby network, or type its name.", UI_FONT_SMALL, UI_COLOR_MUTED);
    ui_button(col, LV_SYMBOL_WIFI "  Scan for networks", UI_COLOR_ACCENT, scan_clicked, NULL);

    s_ssid = ui_field(col, "Network name (SSID)", "e.g. HomeWiFi", s_kb);
    lv_textarea_set_max_length(s_ssid, 32);

    // Plain text on purpose: easier to get right on a touch keyboard.
    s_pass = ui_field(col, "Password", "Password", s_kb);
    lv_textarea_set_max_length(s_pass, 64);
    s_hint = ui_label(col, HINT_DEFAULT, UI_FONT_SMALL, UI_COLOR_MUTED);

    ui_button(col, "Join", UI_COLOR_GO, join_clicked, NULL);

    s_web_hint = ui_web_hint(col);

    if (g_settings.wifi_ssid[0]) lv_textarea_set_text(s_ssid, g_settings.wifi_ssid);
    lv_screen_load(s_setup_scr);
}
