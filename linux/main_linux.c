// scheff-zello on Linux: the M5Stack CardputerZero (on its LCD from the
// launcher, or in a window on its desktop) and the desktop simulator. The
// same app as main/app_main.c over linux/compat and linux/hal; settings and
// the channel list live in ~/.config/scheff-zello, the working directory.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "audio.h"
#include "board.h"
#include "channel_list.h"
#include "display.h"
#include "esp_log.h"
#include "hal/linux_hal.h"
#include "input_keys.h"
#include "linux_app.h"
#include "net_wifi.h"
#include "settings.h"
#include "ui.h"
#include "web_config.h"
#include "zello_client.h"

static const char *TAG = "main";

#define DESKTOP_ENTRY_SRC "/usr/share/scheff-zello/scheff-zello.desktop"

static atomic_bool s_quit;
static atomic_bool s_cleaned;
static int s_lock_fd = -1;

void linux_app_request_quit(void) { atomic_store(&s_quit, true); }

static bool should_quit(void) { return atomic_load(&s_quit) || cp0_keys_quit_requested(); }

void linux_app_before_exit(void)
{
    if (atomic_exchange(&s_cleaned, true)) return;
    cp0_keys_stop();
    if (!hal_windowed()) {
        fb_close();
        hal_backlight_restore();
    }
    if (s_lock_fd >= 0) close(s_lock_fd);
    s_lock_fd = -1;
}

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&s_quit, true);
}

static int mkdirs(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(buf, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

// ~/.config/scheff-zello (or $XDG_CONFIG_HOME's), made and entered.
static bool enter_config_dir(void)
{
    char dir[512];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/scheff-zello", xdg);
    else if (home && *home) snprintf(dir, sizeof(dir), "%s/.config/scheff-zello", home);
    else return false;
    if (mkdirs(dir) != 0 || chdir(dir) != 0) {
        ESP_LOGE(TAG, "can't use %s: %s", dir, strerror(errno));
        return false;
    }
    return true;
}

// The desktop session's menu entry, copied in once so the app can be
// started from the desktop too.
static void install_desktop_entry(void)
{
    const char *home = getenv("HOME");
    FILE *in = home ? fopen(DESKTOP_ENTRY_SRC, "r") : NULL;
    if (!in) return;
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/.local/share/applications", home);
    snprintf(path, sizeof(path), "%s/scheff-zello.desktop", dir);
    FILE *out = mkdirs(dir) == 0 ? fopen(path, "w") : NULL;
    if (out) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

// Whether to draw into a window: always in the simulator; on the device when
// started from a desktop session, unless SCHEFF_DISPLAY says otherwise.
static bool want_window(void)
{
#ifdef ZELLO_LINUX_SIM
    return true;
#else
    const char *d = getenv("SCHEFF_DISPLAY");
    if (d && !strcmp(d, "window")) return true;
    if (d && !strcmp(d, "lcd")) return false;
    const char *w = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    return (w && *w) || (x && *x);
#endif
}

// app_main.c's start-up, less the ESP-only parts.
static void *app_thread(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(settings_load());
    ESP_ERROR_CHECK(board_init());
    esp_err_t disp_err = display_init();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(disp_err));
        linux_app_request_quit();
        return NULL;
    }
    if (input_keys_init() != ESP_OK) ESP_LOGE(TAG, "keyboard unavailable");
    if (channel_list_init() != ESP_OK) ESP_LOGE(TAG, "channel list storage unavailable");

    if (audio_init(zello_mic_frame) != ESP_OK) ESP_LOGE(TAG, "audio unavailable");
    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);

    ESP_ERROR_CHECK(zello_init());
    ESP_ERROR_CHECK(net_wifi_init());
    web_config_start();
    ui_start();
    display_reveal();

    if (!g_settings.username[0]) ESP_LOGW(TAG, "no Zello account configured: set it up on screen or in the web config");

    // The OS has the network, so the account alone decides it.
    if (g_settings.auto_connect && g_settings.username[0]) {
        while (g_settings.auto_connect && !should_quit() && !net_wifi_wait_ip(1000)) {}
        if (g_settings.auto_connect && !should_quit()) {
            if (!net_wifi_wait_time(10000)) ESP_LOGW(TAG, "clock not set yet; the first sign-in may need a retry");
            esp_err_t err = zello_auto_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "automatic sign-in failed: %s", esp_err_to_name(err));
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    linux_app_save_args(argv);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!enter_config_dir()) return 1;
    s_lock_fd = open(".lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s_lock_fd < 0 || flock(s_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        ESP_LOGE(TAG, "scheff-zello is already running");
        return 1;
    }
    install_desktop_entry();

    // The window or the LCD opens here: SDL's video must stay on this thread.
    bool window = want_window();
    hal_display_choose(window);
    if (window) {
        if (!sdlwin_open("Scheff Zello")) return 1;
    } else if (!fb_open()) {
        return 1;
    }

    pthread_t app;
    if (pthread_create(&app, NULL, app_thread, NULL) != 0) return 1;
    pthread_detach(app);

    if (window) {
        sdlwin_run(should_quit);
        const char *shot = getenv("SCHEFF_SCREENSHOT");
        linux_app_before_exit();
        sdlwin_close(shot);
    } else {
        while (!should_quit()) usleep(50 * 1000);
        linux_app_before_exit();
    }
    ESP_LOGI(TAG, "bye");
    // The other threads (network, audio, LVGL) end with the process.
    _exit(0);
}
