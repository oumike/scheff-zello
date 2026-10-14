#include "input_tdeck.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "display.h"
#include "zello_client.h"

static const char *TAG = "input";

// One LVGL keypad carries both the keyboard and the trackball: rolling moves
// focus (or a dropdown's selection, or a slider), the click is Enter except on
// the talk screen, where ptt.c owns it.

#define KEY_QUEUE_LEN     16
#define MOVE_DEBOUNCE_US  (45 * 1000)  // trackball bounce, as camillia-mt
#define FOCUS_REFRESH_MS  150
#define FOCUS_MAX         96

enum { TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT, TB_COUNT };
static const int s_tb_pins[TB_COUNT] = {BOARD_TBALL_UP, BOARD_TBALL_DOWN, BOARD_TBALL_LEFT, BOARD_TBALL_RIGHT};
static volatile uint32_t s_tb_count[TB_COUNT];

static i2c_master_dev_handle_t s_kb;
static lv_group_t *s_group;
static lv_obj_t *s_talk_screen;
static volatile bool s_talk_active;

static uint32_t s_queue[KEY_QUEUE_LEN];
static uint8_t s_q_head, s_q_len;

static void IRAM_ATTR tb_isr(void *arg)
{
    s_tb_count[(int)(intptr_t)arg]++;
}

static void enqueue(uint32_t key)
{
    if (s_q_len == KEY_QUEUE_LEN) return;
    s_queue[(s_q_head + s_q_len++) % KEY_QUEUE_LEN] = key;
}

static bool focused_is(const lv_obj_class_t *cls)
{
    lv_obj_t *f = lv_group_get_focused(s_group);
    return f && lv_obj_check_type(f, cls);
}

// A keyboard character: 0 when none is waiting.
static uint8_t keyboard_read(void)
{
    uint8_t c = 0;
    if (s_kb && i2c_master_receive(s_kb, &c, 1, 20) != ESP_OK) c = 0;
    return c;
}

static void poll_keyboard(void)
{
    uint8_t c = keyboard_read();
    if (!c) return;
    switch (c) {
    case 0x0D: enqueue(LV_KEY_ENTER); break;
    case 0x08: enqueue(LV_KEY_BACKSPACE); break;
    case 0x1B: enqueue(LV_KEY_ESC); break;
    case ' ':
        // Space talks on the talk screen, toggling (the keyboard reports
        // presses only, so it can't be held). In a text field it types.
        if (s_talk_active && !focused_is(&lv_textarea_class)) {
            ESP_LOGI(TAG, "space: talk %s", zello_ptt() ? "off" : "on");
            zello_set_ptt(!zello_ptt());
        } else {
            enqueue(' ');
        }
        break;
    default:
        if (c >= 0x20 && c < 0x7F) enqueue(c);
        break;
    }
}

static void poll_trackball(void)
{
    static int64_t last_move_us;
    static bool click_was_down;

    uint32_t n[TB_COUNT];
    for (int i = 0; i < TB_COUNT; i++) {
        n[i] = s_tb_count[i];
        s_tb_count[i] = 0;
    }
    int64_t now = esp_timer_get_time();
    if (now - last_move_us >= MOVE_DEBOUNCE_US) {
        bool dropdown_open = focused_is(&lv_dropdown_class) && lv_dropdown_is_open(lv_group_get_focused(s_group));
        uint32_t key = 0;
        if (n[TB_UP]) key = dropdown_open ? LV_KEY_UP : LV_KEY_PREV;
        else if (n[TB_DOWN]) key = dropdown_open ? LV_KEY_DOWN : LV_KEY_NEXT;
        else if (n[TB_LEFT]) key = LV_KEY_LEFT;
        else if (n[TB_RIGHT]) key = LV_KEY_RIGHT;
        if (key) {
            enqueue(key);
            last_move_us = now;
        }
    }

    // The click is Enter everywhere but the talk screen.
    bool down = gpio_get_level(BOARD_TBALL_CLICK) == 0;
    if (down && !click_was_down && !s_talk_active) enqueue(LV_KEY_ENTER);
    click_was_down = down;
}

// Each queued key goes to LVGL as a press, then a release on the next read.
static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last_key;
    static bool release_pending;

    if (release_pending) {
        release_pending = false;
        data->key = last_key;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = s_q_len > 0;
        return;
    }
    poll_keyboard();
    poll_trackball();
    if (s_q_len) {
        last_key = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % KEY_QUEUE_LEN;
        s_q_len--;
        release_pending = true;
        data->key = last_key;
        data->state = LV_INDEV_STATE_PRESSED;
        data->continue_reading = true;
    } else {
        data->key = last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- focus group ------------------------------------------------------------

// What the user is looking at: a full-screen form or modal on the top layer
// (the newest one), otherwise the loaded screen. The status bar lives on the
// top layer too, but it is small, so it is never picked.
static lv_obj_t *focus_root(void)
{
    lv_obj_t *top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_width(c) >= DISPLAY_H_RES && lv_obj_get_height(c) >= DISPLAY_V_RES) return c;
    }
    return lv_screen_active();
}

static bool focusable(lv_obj_t *o)
{
    static const lv_obj_class_t *const classes[] = {
        &lv_button_class, &lv_textarea_class, &lv_slider_class,
        &lv_dropdown_class, &lv_switch_class, &lv_checkbox_class,
    };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
        if (lv_obj_check_type(o, classes[i])) return true;
    }
    return false;
}

// Visible focusable objects under `o` in tree order. Hidden subtrees (the
// never-shown on-screen keyboard, fields for the other network type) are skipped.
static void collect(lv_obj_t *o, lv_obj_t **out, size_t *n)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || *n >= FOCUS_MAX) return;
    if (focusable(o)) out[(*n)++] = o;
    uint32_t count = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < count; i++) collect(lv_obj_get_child(o, i), out, n);
}

static void focus_refresh(lv_timer_t *t)
{
    static lv_obj_t *s_last[FOCUS_MAX];
    static size_t s_last_n;

    lv_obj_t *root = focus_root();
    s_talk_active = root == s_talk_screen && s_talk_screen;

    lv_obj_t *now[FOCUS_MAX];
    size_t n = 0;
    collect(root, now, &n);
    if (n == s_last_n && !memcmp(now, s_last, n * sizeof(now[0]))) return;

    // The set changed (another screen, an overlay, a list refilled): rebuild,
    // keeping focus where it was if that object is still here.
    lv_obj_t *focused = lv_group_get_focused(s_group);
    lv_group_remove_all_objs(s_group);
    bool kept = false;
    for (size_t i = 0; i < n; i++) {
        lv_group_add_obj(s_group, now[i]);
        if (now[i] == focused) kept = true;
    }
    if (kept) lv_group_focus_obj(focused);
    memcpy(s_last, now, n * sizeof(now[0]));
    s_last_n = n;
}

void input_tdeck_set_talk_screen(lv_obj_t *scr)
{
    s_talk_screen = scr;
}

bool input_tdeck_talk_active(void)
{
    return s_talk_active;
}

esp_err_t input_tdeck_init(void)
{
    // Probe first: polling a keyboard that isn't answering would fail (and
    // log) on every read.
    if (i2c_master_probe(board_main_i2c_bus(), BOARD_KB_ADDR, 50) == ESP_OK) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_KB_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(board_main_i2c_bus(), &dev, &s_kb) != ESP_OK) s_kb = NULL;
    }
    if (!s_kb) ESP_LOGE(TAG, "keyboard not found at 0x%02x", BOARD_KB_ADDR);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_TBALL_UP) | (1ULL << BOARD_TBALL_DOWN) | (1ULL << BOARD_TBALL_LEFT) |
                        (1ULL << BOARD_TBALL_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "trackball");
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    for (int i = 0; i < TB_COUNT; i++) gpio_isr_handler_add(s_tb_pins[i], tb_isr, (void *)(intptr_t)i);
    // The click line is set up by ptt_button_start(); it is only read here.

    ESP_RETURN_ON_FALSE(display_lock(), ESP_FAIL, TAG, "lvgl lock");
    s_group = lv_group_create();
    lv_group_set_wrap(s_group, true);
    lv_indev_t *kp = lv_indev_create();
    lv_indev_set_type(kp, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kp, keypad_read);
    lv_indev_set_group(kp, s_group);
    lv_timer_create(focus_refresh, FOCUS_REFRESH_MS, NULL);
    display_unlock();

    ESP_LOGI(TAG, "keyboard and trackball ready");
    return ESP_OK;
}
