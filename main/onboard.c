#include "onboard.h"

#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "channel_list.h"
#include "settings.h"
#include "zello_client.h"

static const char *TAG = "onboard";

#define WIFI_RESTART_DELAY_MS 3000

static void (*s_ui_cb)(void);
static void (*s_web_cb)(void);
static atomic_bool s_restart_pending;

static void wifi_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_RESTART_DELAY_MS));
    esp_restart();
}

static void notify(void)
{
    if (s_ui_cb) s_ui_cb();
    if (s_web_cb) s_web_cb();
}

bool onboard_wifi_done(void)
{
#if CONFIG_ZELLO_BOARD_CARDPUTER_ZERO
    return true;  // the OS owns the network
#else
    settings_t settings;
    settings_get(&settings);
    return settings.wifi_ssid[0] != 0;
#endif
}

bool onboard_account_done(void)
{
    settings_t settings;
    settings_get(&settings);
    if (!settings.username[0] || !settings.password[0]) return false;
    if (settings.network == ZELLO_NET_CONSUMER) return settings.auth_token[0] != 0;
    return settings.work_network[0] != 0;
}

bool onboard_channel_done(void)
{
    settings_t settings;
    settings_get(&settings);
    return settings.channel[0] != 0;
}

static esp_err_t update_wifi(settings_t *values, void *ctx)
{
    const char *const *args = ctx;
    esp_err_t err = settings_set_field(values, "wifi_ssid", args[0]);
    return err == ESP_OK ? settings_set_field(values, "wifi_pass", args[1]) : err;
}

esp_err_t onboard_save_wifi(const char *ssid, const char *pass)
{
    const char *args[] = {ssid, pass ? pass : ""};
    esp_err_t err = settings_update(update_wifi, args);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save Wi-Fi settings: %s", esp_err_to_name(err));
        return err;
    }
    notify();
    if (!atomic_exchange(&s_restart_pending, true)) {
        ESP_LOGI(TAG, "Wi-Fi onboarding complete; restarting in %d ms", WIFI_RESTART_DELAY_MS);
        if (xTaskCreate(wifi_restart_task, "wifi_restart", 2048, NULL, 3, NULL) != pdPASS) esp_restart();
    }
    return ESP_OK;
}

typedef struct {
    const char *network, *username, *password, *token, *work_network;
    const char *error;
} account_update_t;

static esp_err_t update_account(settings_t *values, void *ctx)
{
    account_update_t *a = ctx;

    if (a->network && a->network[0]) {
        if (settings_set_field(values, "network", a->network) != ESP_OK) {
            a->error = "Choose either the consumer network or Zello Work.";
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!a->username || !a->username[0]) {
        a->error = "Enter your Zello username.";
        return ESP_ERR_INVALID_ARG;
    }
    // Blank means "keep what's stored": neither the web page nor the screen
    // ever displays the saved password or token.
    if (settings_set_field(values, "username", a->username) != ESP_OK ||
        (a->password && a->password[0] && settings_set_field(values, "password", a->password) != ESP_OK) ||
        (a->token && a->token[0] && settings_set_field(values, "auth_token", a->token) != ESP_OK) ||
        (a->work_network && settings_set_field(values, "work_network", a->work_network) != ESP_OK)) {
        a->error = "An account field is too long.";
        return ESP_ERR_INVALID_ARG;
    }

    if (!values->password[0]) {
        a->error = "Enter your Zello password.";
        return ESP_ERR_INVALID_ARG;
    }
    if (values->network == ZELLO_NET_CONSUMER && !values->auth_token[0]) {
        a->error = "The consumer network needs a developer token from developers.zello.com.";
        return ESP_ERR_INVALID_ARG;
    }
    if (values->network == ZELLO_NET_WORK && !values->work_network[0]) {
        a->error = "Enter your Zello Work network name.";
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t onboard_save_account(const char *network, const char *username, const char *password, const char *token,
                               const char *work_network, const char **err)
{
    account_update_t update = {network, username, password, token, work_network, NULL};
    esp_err_t e = settings_update(update_account, &update);
    if (e != ESP_OK) {
        if (err) *err = update.error ? update.error : "Couldn't save the settings.";
        return e;
    }
    zello_reconfigure();
    notify();
    return ESP_OK;
}

esp_err_t onboard_save_channel(const char *channel, const char **err)
{
    if (!channel || !channel[0]) {
        if (err) *err = "Enter a channel name.";
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(channel) >= sizeof(g_settings.channel)) {
        if (err) *err = "That channel name is too long.";
        return ESP_ERR_INVALID_ARG;
    }

    channel_entry_t entry = {0};
    if (!channel_list_find(channel, &entry)) {
        strlcpy(entry.name, channel, sizeof(entry.name));
        channel_list_put(NULL, &entry);  // remembered even if the join fails
    }

    esp_err_t e = zello_set_channel(channel);
    if (e != ESP_OK) {
        if (err) *err = "Couldn't save the channel.";
        return e;
    }
    notify();
    return ESP_OK;
}

void onboard_settings_changed(void)
{
    notify();
}

void onboard_set_ui_cb(void (*cb)(void))
{
    s_ui_cb = cb;
}

void onboard_set_web_cb(void (*cb)(void))
{
    s_web_cb = cb;
}
