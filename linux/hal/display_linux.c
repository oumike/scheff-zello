// LVGL on Linux: one full-screen RGB565 buffer drawn in direct mode, its
// changed rows handed to the LCD's framebuffer or to the window, and a thread
// running LVGL's timers under a recursive lock (esp_lvgl_port's model).

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "display.h"
#include "esp_log.h"
#include "linux_hal.h"
#include "lvgl.h"

static const char *TAG = "display";

static pthread_mutex_t s_lvgl_lock;
static pthread_t s_thread;
static atomic_bool s_running;
static bool s_window;
static uint16_t *s_buf;

static atomic_int s_level = 0;  // dark until display_reveal()
static atomic_bool s_lit = true;

#define BRIGHTNESS_DEFAULT 255

void hal_display_choose(bool window) { s_window = window; }
bool hal_windowed(void) { return s_window; }

static uint32_t tick_cb(void) { return hal_now_ms(); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)px;
    if (s_window) sdlwin_push(s_buf, area->y1, area->y2 + 1);
    else fb_push(s_buf, area->y1, area->y2 + 1);
    lv_display_flush_ready(disp);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int16_t x, y;
    bool down = sdlwin_pointer(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void *lvgl_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&s_running)) {
        pthread_mutex_lock(&s_lvgl_lock);
        uint32_t next = lv_timer_handler();
        pthread_mutex_unlock(&s_lvgl_lock);
        if (next > 10 || next == LV_NO_TIMER_READY) next = 10;
        if (next < 1) next = 1;
        usleep(next * 1000);
    }
    return NULL;
}

// The window dims; on the LCD it is the backlight, or without one a black
// framebuffer.
static void apply(void)
{
    int level = atomic_load(&s_level);
    bool dark = !atomic_load(&s_lit) || level == 0;
    if (s_window) {
        sdlwin_set_level(dark ? 0 : (uint8_t)level);
    } else if (hal_backlight_present()) {
        hal_backlight_set(dark ? 0 : (uint8_t)level);
    } else {
        fb_dark(dark);
    }
}

esp_err_t display_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lvgl_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    if (!disp) return ESP_ERR_NO_MEM;
    size_t bytes = (size_t)DISPLAY_H_RES * DISPLAY_V_RES * 2;
    s_buf = calloc(1, bytes);
    if (!s_buf) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    if (s_window) {
        lv_indev_t *ptr = lv_indev_create();
        lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(ptr, pointer_read);
    }
    apply();

    atomic_store(&s_running, true);
    if (pthread_create(&s_thread, NULL, lvgl_thread, NULL) != 0) return ESP_FAIL;
    ESP_LOGI(TAG, "%dx%d in a %s", DISPLAY_H_RES, DISPLAY_V_RES, s_window ? "window" : "framebuffer");
    return ESP_OK;
}

void hal_display_stop(void)
{
    if (atomic_exchange(&s_running, false)) pthread_join(s_thread, NULL);
}

void display_reveal(void)
{
    if (display_lock()) {
        lv_refr_now(NULL);
        display_unlock();
    }
    usleep(50 * 1000);
    display_set_brightness(BRIGHTNESS_DEFAULT);
}

bool display_lock(void)
{
    return pthread_mutex_lock(&s_lvgl_lock) == 0;
}

void display_unlock(void)
{
    pthread_mutex_unlock(&s_lvgl_lock);
}

esp_err_t display_set_test_pattern(bool on)
{
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

bool display_test_pattern(void)
{
    return false;
}

void display_set_brightness(uint8_t level)
{
    atomic_store(&s_level, level);
    atomic_store(&s_lit, level > 0);
    apply();
}

void display_backlight(bool on)
{
    atomic_store(&s_lit, on);
    if (on && atomic_load(&s_level) == 0) atomic_store(&s_level, BRIGHTNESS_DEFAULT);
    apply();
}

// The brightness keys: a step of the level the app keeps lit at.
void hal_backlight_step(int delta)
{
    if (s_window) return;  // a monitor has its own brightness
    int v = atomic_load(&s_level) + delta;
    if (v < 16) v = 16;
    if (v > 255) v = 255;
    atomic_store(&s_level, v);
    apply();
}
