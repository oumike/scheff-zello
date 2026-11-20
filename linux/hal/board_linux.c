// The rest of the board on Linux: the backlight and battery through sysfs,
// storage as the config directory, and the ESP-only services as no-ops.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "console_cmds.h"
#include "crash_report.h"
#include "esp_log.h"
#include "linux_hal.h"
#include "ptt.h"
#include "sdcard.h"

static const char *TAG = "board";

static char s_backlight[288];  // /sys/class/backlight/<first>, or empty
static int s_max_brightness, s_launcher_brightness = -1;
static char s_battery[288];    // /sys/class/power_supply/<battery>, or empty

static int read_int(const char *dir, const char *name)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool read_word(const char *dir, const char *name, char *out, size_t size)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%31s", out) == 1;
    (void)size;
    fclose(f);
    return ok;
}

static void write_int(const char *dir, const char *name, int v)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}

static void find_nodes(void)
{
    DIR *d = opendir("/sys/class/backlight");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[288];
        snprintf(dir, sizeof(dir), "/sys/class/backlight/%s", e->d_name);
        if (read_int(dir, "max_brightness") > 0) {
            snprintf(s_backlight, sizeof(s_backlight), "%s", dir);
            s_max_brightness = read_int(dir, "max_brightness");
            s_launcher_brightness = read_int(dir, "brightness");
            break;
        }
    }
    if (d) closedir(d);
    d = opendir("/sys/class/power_supply");
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[288], type[32];
        snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", e->d_name);
        if (read_word(dir, "type", type, sizeof(type)) && !strcmp(type, "Battery")) {
            snprintf(s_battery, sizeof(s_battery), "%s", dir);
            break;
        }
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "battery %s, backlight %s", s_battery[0] ? s_battery : "none",
             s_backlight[0] ? s_backlight : "none");
}

esp_err_t board_init(void)
{
    if (!hal_windowed()) find_nodes();
    return ESP_OK;
}

bool hal_backlight_present(void) { return s_backlight[0] != 0; }

void hal_backlight_set(uint8_t level)
{
    if (!s_backlight[0]) return;
    int v = ((int)level * s_max_brightness + 127) / 255;
    if (level > 0 && v == 0) v = 1;
    write_int(s_backlight, "brightness", v);
}

void hal_backlight_restore(void)
{
    if (s_backlight[0] && s_launcher_brightness >= 0) write_int(s_backlight, "brightness", s_launcher_brightness);
}

esp_err_t board_audio_power(bool on)
{
    (void)on;
    return ESP_OK;
}

esp_err_t board_battery_percent(uint8_t *percent)
{
    static time_t s_read_at;
    static int s_percent = -1;
    if (!s_battery[0]) return ESP_ERR_NOT_SUPPORTED;
    time_t now = time(NULL);
    if (s_percent < 0 || now - s_read_at >= 5) {
        s_percent = read_int(s_battery, "capacity");
        s_read_at = now;
    }
    if (s_percent < 0) return ESP_FAIL;
    *percent = (uint8_t)(s_percent > 100 ? 100 : s_percent);
    return ESP_OK;
}

i2c_master_bus_handle_t board_main_i2c_bus(void) { return NULL; }
i2c_master_bus_handle_t board_codec_i2c_bus(void) { return NULL; }

// Storage is the config directory, the working directory.
esp_err_t sdcard_mount(void) { return ESP_OK; }
bool sdcard_mounted(void) { return true; }

// Crash reports are an ESP core-dump feature.
void crash_report_init(void) {}
void crash_report_start(void) {}
void crash_report_disconnect(const char *why) { (void)why; }
void crash_report_print_status(void) {}
esp_err_t crash_report_list(crash_report_file_cb cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return ESP_ERR_NOT_FOUND;
}
FILE *crash_report_open(const char *name)
{
    (void)name;
    return NULL;
}
int crash_report_delete_all(void) { return 0; }
const char *crash_report_last_reset(void) { return "poweron"; }

void console_start(void) {}
void ptt_button_start(void) {}
