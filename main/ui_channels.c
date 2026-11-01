// Channel picker: the saved channels, favorites first, with a search box and a
// form for adding or editing one. Used full screen during onboarding and as an
// overlay from Home afterwards. Tapping a channel joins it.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "onboard.h"
#include "settings.h"
#include "ui_internal.h"
#include "zello_client.h"

#define SEARCH_DEBOUNCE_MS 300

static lv_obj_t *s_root, *s_list, *s_search, *s_kb;
static lv_timer_t *s_search_timer;
static bool s_onboarding;

static channel_entry_t *s_entries;
static size_t s_entry_count;
static channel_entry_t s_menu_entry;  // the entry whose gear menu is open

// Add/edit form, on top of whatever opened it.
static lv_obj_t *s_form, *s_f_name, *s_f_desc;
static char s_form_orig[sizeof(((channel_entry_t *)0)->name)];

static void refresh_list(void);

static void close_self(void)
{
    if (s_onboarding) return;  // ui.c owns the onboarding screen
    ui_channels_destroy();
}

// ---- joining ---------------------------------------------------------------

static void join_channel(const char *name)
{
    const char *err = NULL;
    if (onboard_save_channel(name, &err) != ESP_OK) {
        ui_modal_message(err ? err : "Couldn't save the channel.", UI_COLOR_WARN, true, NULL);
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Joining\n%s...", name);
    ui_modal_message(msg, UI_COLOR_GO_LIT, false, NULL);
    ui_modal_close_after(1200, NULL);
    close_self();
}

static void item_clicked(lv_event_t *e)
{
    join_channel(s_entries[(size_t)(intptr_t)lv_event_get_user_data(e)].name);
}

// ---- add / edit form -------------------------------------------------------

static void close_form(void)
{
    if (s_form) lv_obj_delete(s_form);
    s_form = NULL;
}

static void form_cancel(lv_event_t *e)
{
    close_form();
}

static void form_save(lv_event_t *e)
{
    channel_entry_t entry = {0};
    if (s_form_orig[0]) channel_list_find(s_form_orig, &entry);  // keep favorite and use count
    strlcpy(entry.name, lv_textarea_get_text(s_f_name), sizeof(entry.name));
    strlcpy(entry.desc, lv_textarea_get_text(s_f_desc), sizeof(entry.desc));
    if (!entry.name[0]) {
        ui_modal_message("Enter the channel name, exactly as it appears in Zello.", UI_COLOR_WARN, true, NULL);
        return;
    }

    esp_err_t err = channel_list_put(s_form_orig[0] ? s_form_orig : NULL, &entry);
    if (err == ESP_ERR_NO_MEM) {
        ui_modal_message("The channel list is full. Delete one first.", UI_COLOR_WARN, true, NULL);
        return;
    }
    if (err != ESP_OK) {
        ui_modal_message("Couldn't save the channel list.", UI_COLOR_STOP, true, NULL);
        return;
    }
    bool renamed_current = s_form_orig[0] && !strcmp(s_form_orig, g_settings.channel);
    close_form();
    refresh_list();
    // Adding a channel by hand means you want to be in it; so does renaming
    // the one you're already in.
    if (!s_form_orig[0] || renamed_current) join_channel(entry.name);
}

static void form_delete(lv_event_t *e)
{
    if (!s_form_orig[0]) return;
    channel_list_delete(s_form_orig);
    close_form();
    refresh_list();
}

static void name_focused(lv_event_t *e)
{
    lv_keyboard_set_mode(lv_event_get_user_data(e), LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void open_form(const channel_entry_t *entry)
{
    close_form();
    lv_obj_t *kb;
    lv_obj_t *col = ui_layout_create(lv_layer_top(), &s_form, &kb);
    s_form_orig[0] = 0;
    if (entry) strlcpy(s_form_orig, entry->name, sizeof(s_form_orig));

    ui_title(col, entry ? "Edit Channel" : "Add a Channel");
    ui_label(col, "Channel names are matched exactly, including spaces and capitals.", UI_FONT_SMALL, UI_COLOR_MUTED);

    s_f_name = ui_field(col, "Channel name", "e.g. My Channel", kb);
    lv_textarea_set_max_length(s_f_name, sizeof(s_menu_entry.name) - 1);
    lv_obj_add_event_cb(s_f_name, name_focused, LV_EVENT_FOCUSED, kb);

    s_f_desc = ui_field(col, "Note (optional)", "Only shown here", kb);
    lv_textarea_set_max_length(s_f_desc, sizeof(s_menu_entry.desc) - 1);
    lv_obj_add_event_cb(s_f_desc, name_focused, LV_EVENT_FOCUSED, kb);

    if (entry) {
        lv_textarea_set_text(s_f_name, entry->name);
        lv_textarea_set_text(s_f_desc, entry->desc);
    }

    ui_spacer(col);
    ui_button(col, entry ? LV_SYMBOL_SAVE "  Save" : LV_SYMBOL_OK "  Add and join", UI_COLOR_GO, form_save, NULL);
    if (entry) ui_button(col, LV_SYMBOL_TRASH "  Delete", UI_COLOR_STOP, form_delete, NULL);
    ui_button(col, "Cancel", UI_COLOR_PANEL, form_cancel, NULL);
}

static void add_clicked(lv_event_t *e)
{
    open_form(NULL);
}

// ---- per-entry menu --------------------------------------------------------

static void entry_menu_chosen(int index)
{
    if (index == 0) {
        open_form(&s_menu_entry);
    } else if (index == 1) {
        if (channel_list_set_favorite(s_menu_entry.name, !s_menu_entry.favorite) != ESP_OK) {
            ui_modal_message("Couldn't save the change.", UI_COLOR_STOP, true, NULL);
            return;
        }
        refresh_list();
    }
}

static void gear_clicked(lv_event_t *e)
{
    // Copy it: the list is rebuilt while the menu is open.
    s_menu_entry = s_entries[(size_t)(intptr_t)lv_event_get_user_data(e)];
    const char *items[] = {
        LV_SYMBOL_EDIT "  Edit",
        s_menu_entry.favorite ? LV_SYMBOL_CLOSE "  Unfavorite" : LV_SYMBOL_PLUS "  Favorite",
    };
    const lv_color_t colors[] = {UI_COLOR_LINK, UI_COLOR_WARN};
    ui_modal_menu(s_menu_entry.name, items, colors, 2, entry_menu_chosen);
}

// ---- list ------------------------------------------------------------------

static void refresh_list(void)
{
    lv_obj_clean(s_list);
    const char *query = lv_textarea_get_text(s_search);
    s_entry_count = s_entries ? channel_list_get(query, false, s_entries, CHANNEL_LIST_MAX) : 0;

    if (!s_entry_count) {
        const char *text = query[0] ? "No channels match that."
                                    : "No channels yet.\n\nTap \"Add a channel\" and type its name exactly as it "
                                      "appears in Zello.";
        lv_obj_t *l = ui_label(s_list, text, UI_FONT_SMALL, UI_COLOR_MUTED);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, UI_PAD * 3, 0);
        return;
    }

    for (size_t i = 0; i < s_entry_count; i++) {
        const channel_entry_t *en = &s_entries[i];
        bool current = !strcmp(en->name, g_settings.channel);

        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(b, UI_COLOR_PANEL, 0);
        lv_obj_set_style_radius(b, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(b, UI_PAD, 0);
        lv_obj_set_style_pad_column(b, UI_GAP, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (current) {
            lv_obj_set_style_border_color(b, UI_COLOR_ACCENT, 0);
            lv_obj_set_style_border_width(b, 2, 0);
        }
        lv_obj_add_event_cb(b, item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Text column; not clickable, so taps on it reach the row.
        lv_obj_t *txt = lv_obj_create(b);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(txt, 0, 0);
        lv_obj_set_style_pad_row(txt, 4, 0);
        lv_obj_set_style_bg_opa(txt, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(txt, 0, 0);
        lv_obj_remove_flag(txt, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *gear = lv_button_create(b);
        lv_obj_set_size(gear, UI_ICON_BUTTON_SIZE, UI_ICON_BUTTON_SIZE);
        lv_obj_set_style_bg_color(gear, UI_COLOR_BG, 0);
        lv_obj_set_style_radius(gear, UI_RADIUS, 0);
        lv_obj_add_event_cb(gear, gear_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *gl = lv_label_create(gear);
        lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(gl, UI_FONT, 0);
        lv_obj_center(gl);

        lv_obj_t *name = lv_label_create(txt);
        lv_label_set_text(name, en->name);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name, UI_FONT, 0);
        if (en->favorite) lv_obj_set_style_text_color(name, UI_COLOR_WARN, 0);

        lv_obj_t *meta = lv_label_create(txt);
        lv_label_set_text(meta, current ? "Current channel" : en->favorite ? "Favorite" : "Channel");
        lv_obj_set_style_text_font(meta, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(meta, current ? UI_COLOR_ACCENT : UI_COLOR_LINK, 0);

        if (en->desc[0]) {
            lv_obj_t *desc = lv_label_create(txt);
            lv_label_set_text(desc, en->desc);
            lv_obj_set_width(desc, LV_PCT(100));
            lv_label_set_long_mode(desc, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(desc, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(desc, UI_COLOR_MUTED, 0);
        }
    }
}

static void search_timer_cb(lv_timer_t *t)
{
    s_search_timer = NULL;  // one-shot
    refresh_list();
}

// Typing restarts a short timer, so the list rebuilds once per pause rather
// than on every keystroke.
static void search_changed(lv_event_t *e)
{
    if (s_search_timer) lv_timer_delete(s_search_timer);
    s_search_timer = lv_timer_create(search_timer_cb, SEARCH_DEBOUNCE_MS, NULL);
    lv_timer_set_repeat_count(s_search_timer, 1);
}

static void search_clear(lv_event_t *e)
{
    lv_textarea_set_text(s_search, "");
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_search, LV_STATE_FOCUSED);
}

static void back_clicked(lv_event_t *e)
{
    ui_channels_destroy();
}

// ---- screen ----------------------------------------------------------------

static lv_obj_t *build(lv_obj_t *parent, const char *title, const char *subtitle)
{
    lv_obj_t *col = ui_layout_create(parent, &s_root, &s_kb);
    if (!s_entries) s_entries = heap_caps_calloc(CHANNEL_LIST_MAX, sizeof(*s_entries), MALLOC_CAP_SPIRAM);

    ui_title(col, title);
    ui_label(col, subtitle, UI_FONT_SMALL, UI_COLOR_MUTED);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, UI_GAP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_search = lv_textarea_create(row);
    lv_textarea_set_one_line(s_search, true);
    lv_textarea_set_placeholder_text(s_search, "Search channels");
    lv_textarea_set_max_length(s_search, 24);
    lv_obj_set_flex_grow(s_search, 1);
    lv_obj_set_style_text_font(s_search, UI_FONT, 0);
    lv_obj_set_style_radius(s_search, UI_RADIUS, 0);
    ui_field_attach_keyboard(s_search, s_kb);
    lv_obj_add_event_cb(s_search, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *clr = ui_button(row, LV_SYMBOL_CLOSE, UI_COLOR_PANEL, search_clear, NULL);
    lv_obj_set_size(clr, UI_CLEAR_BUTTON_W, UI_FIELD_H);

    s_list = lv_obj_create(col);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, UI_GAP / 2, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    ui_button(col, LV_SYMBOL_PLUS "  Add a channel", UI_COLOR_LINK, add_clicked, NULL);
    if (!s_onboarding) ui_button(col, LV_SYMBOL_LEFT "  Back", UI_COLOR_PANEL, back_clicked, NULL);
    refresh_list();
    return col;
}

void ui_channels_onboard(void)
{
    s_onboarding = true;
    lv_obj_t *col = build(NULL, "Choose a Channel",
                          "Type the name of a Zello channel to join. You can add more later and switch between them.");
    ui_web_hint(col);
    lv_screen_load(s_root);
}

void ui_channels_show(void)
{
    if (s_root) return;
    s_onboarding = false;
    build(lv_layer_top(), "Channels", "Tap a channel to join it. The gear edits or favorites one.");
}

void ui_channels_destroy(void)
{
    close_form();
    if (s_search_timer) {
        lv_timer_delete(s_search_timer);
        s_search_timer = NULL;
    }
    lv_obj_t *root = s_root;
    s_root = NULL;
    s_list = s_search = s_kb = NULL;
    if (!root) return;
    if (s_onboarding) {
        lv_obj_delete_async(root);  // a screen: deleted once the next one is up
    } else {
        lv_obj_delete(root);  // an overlay on the top layer
    }
}
