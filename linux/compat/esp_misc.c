// esp_log, esp_err, esp_timer, esp_system and small stubs for Linux.

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "linux_app.h"

static struct timespec s_start;

__attribute__((constructor)) static void start_clock(void) { clock_gettime(CLOCK_MONOTONIC, &s_start); }

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)(ts.tv_sec - s_start.tv_sec) * 1000000 + (ts.tv_nsec - s_start.tv_nsec) / 1000;
}

void esp_log_write_line(char level, const char *tag, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%c (%lld) %s: %s\n", level, (long long)(esp_timer_get_time() / 1000), tag, line);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_NVS_NOT_INITIALIZED: return "ESP_ERR_NVS_NOT_INITIALIZED";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "UNKNOWN ERROR";
    }
}

static char s_exe[PATH_MAX];
static char **s_argv;

void linux_app_save_args(char **argv)
{
    s_argv = argv;
#ifdef __APPLE__
    uint32_t n = sizeof(s_exe);
    if (_NSGetExecutablePath(s_exe, &n) != 0) s_exe[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", s_exe, sizeof(s_exe) - 1);
    s_exe[n > 0 ? n : 0] = 0;
#endif
}

void esp_restart(void)
{
    ESP_LOGI("system", "restarting");
    linux_app_before_exit();
    if (s_exe[0] && s_argv) execv(s_exe, s_argv);
    _exit(0);
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *label,
                                           const esp_vfs_fat_mount_config_t *cfg, wl_handle_t *wl)
{
    (void)label;
    (void)cfg;
    if (wl) *wl = 0;
    if (mkdir(base_path, 0700) != 0 && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}
