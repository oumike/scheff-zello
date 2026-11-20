#include "onboard.h"

#include <string.h>
#include <strings.h>
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
static bool s_restart_pending;

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
    return g_settings.wifi_ssid[0] != 0;
#endif
}

bool onboard_account_done(void)
{
    if (!g_settings.username[0] || !g_settings.password[0]) return false;
    if (g_settings.network == ZELLO_NET_CONSUMER) return g_settings.auth_token[0] != 0;
    return g_settings.work_network[0] != 0;
}

bool onboard_channel_done(void)
{
    return g_settings.channel[0] != 0;
}

esp_err_t onboard_save_wifi(const char *ssid, const char *pass)
{
    strlcpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid));
    strlcpy(g_settings.wifi_pass, pass ? pass : "", sizeof(g_settings.wifi_pass));
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save Wi-Fi settings: %s", esp_err_to_name(err));
        return err;
    }
    notify();
    if (!s_restart_pending) {
        s_restart_pending = true;
        ESP_LOGI(TAG, "Wi-Fi onboarding complete; restarting in %d ms", WIFI_RESTART_DELAY_MS);
        if (xTaskCreate(wifi_restart_task, "wifi_restart", 2048, NULL, 3, NULL) != pdPASS) esp_restart();
    }
    return ESP_OK;
}

esp_err_t onboard_save_account(const char *network, const char *username, const char *password, const char *token,
                               const char *work_network, const char **err)
{
    settings_t previous = g_settings;

    if (network && network[0]) {
        if (settings_set("network", network) != ESP_OK) {
            if (err) *err = "Choose either the consumer network or Zello Work.";
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!username || !username[0]) {
        g_settings = previous;
        if (err) *err = "Enter your Zello username.";
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(g_settings.username, username, sizeof(g_settings.username));
    // Blank means "keep what's stored": neither the web page nor the screen
    // ever displays the saved password or token.
    if (password && password[0]) strlcpy(g_settings.password, password, sizeof(g_settings.password));
    if (token && token[0]) strlcpy(g_settings.auth_token, token, sizeof(g_settings.auth_token));
    if (work_network) strlcpy(g_settings.work_network, work_network, sizeof(g_settings.work_network));

    if (!g_settings.password[0]) {
        g_settings = previous;
        if (err) *err = "Enter your Zello password.";
        return ESP_ERR_INVALID_ARG;
    }
    if (g_settings.network == ZELLO_NET_CONSUMER && !g_settings.auth_token[0]) {
        g_settings = previous;
        if (err) *err = "The consumer network needs a developer token from developers.zello.com.";
        return ESP_ERR_INVALID_ARG;
    }
    if (g_settings.network == ZELLO_NET_WORK && !g_settings.work_network[0]) {
        g_settings = previous;
        if (err) *err = "Enter your Zello Work network name.";
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t e = settings_save();
    if (e != ESP_OK) {
        g_settings = previous;
        if (err) *err = "Couldn't save the settings.";
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
