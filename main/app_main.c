// Zello client for the LilyGO T-Display-P4 (Wi-Fi through the ESP32-C6
// coprocessor, voice through the ES8311 codec) and the LilyGO T-Deck (the
// S3's own Wi-Fi, the ES7210 mic ADC and MAX98357A amplifier, keyboard and
// trackball) and the Seeed Wio Tracker L2 (the S3's own Wi-Fi, ES8311 DAC and
// ES7243E mic ADC, touch only): the Zello Channels API over a WebSocket, Opus voice, and an
// LVGL touch UI backed by a browser-based config.

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "audio.h"
#include "board.h"
#include "channel_list.h"
#include "console_cmds.h"
#include "display.h"
#if CONFIG_ZELLO_BOARD_TDECK
#include "input_tdeck.h"
#endif
#include "net_wifi.h"
#include "ptt.h"
#include "settings.h"
#include "ui.h"
#include "web_config.h"
#include "zello_client.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(settings_load());
    ESP_ERROR_CHECK(board_init());

    // A dead display must not take the radio down with it: the serial console
    // still works without it.
    esp_err_t disp_err = display_init();
    if (disp_err != ESP_OK) ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(disp_err));
#if CONFIG_ZELLO_BOARD_TDECK
    if (disp_err == ESP_OK && input_tdeck_init() != ESP_OK) ESP_LOGE(TAG, "keyboard and trackball unavailable");
#endif
    if (channel_list_init() != ESP_OK) ESP_LOGE(TAG, "channel list storage unavailable");

    ESP_ERROR_CHECK(audio_init(zello_mic_frame));
    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);

    ESP_ERROR_CHECK(zello_init());

    // Wi-Fi needs a good slice of internal RAM, and on the T-Deck that is what
    // runs short; say how much is left when it starts.
    ESP_LOGI(TAG, "internal RAM before Wi-Fi: %u free, %u largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_ERROR_CHECK(net_wifi_init());
    if (g_settings.wifi_ssid[0]) {
        net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi configured: set it up on screen, or use 'wifi <ssid> <password>'");
    }

    web_config_start();

    ptt_button_start();
    if (disp_err == ESP_OK) {
        ui_start();
        display_reveal();
    }

    // Signing in needs both an address and a clock the TLS handshake can trust,
    // so wait for DHCP and then for SNTP rather than failing the first attempt.
    if (g_settings.auto_connect && g_settings.wifi_ssid[0]) {
        if (net_wifi_wait_ip(20000)) {
            if (!net_wifi_wait_time(10000)) ESP_LOGW(TAG, "clock not set yet; the first sign-in may need a retry");
            zello_connect();
        } else {
            ESP_LOGW(TAG, "no IP yet; sign in with 'login' once Wi-Fi is up");
        }
    }
    if (!g_settings.username[0]) {
        ESP_LOGW(TAG, "no Zello account configured: open the web config, or use 'set username <name>'");
    }
    console_start();
}
