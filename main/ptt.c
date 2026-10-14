#include "ptt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"
#include "settings.h"
#include "zello_client.h"

static const char *TAG = "ptt";
#if CONFIG_ZELLO_BOARD_TDECK
#include "input_tdeck.h"
#endif

// Hold-to-talk on the board's button (BOOT on the P4, the trackball click on
// the T-Deck, the side button on GPIO0 on the Wio Tracker L2), debounced by requiring a stable level for three consecutive
// 10 ms samples. With ptt_latch set, a press toggles instead, which is easier
// for a long call than holding a small button.
//
// On the T-Deck the click also selects things, so it only talks while the
// home screen is in front; leaving it counts as letting go.
static void ptt_task(void *arg)
{
    bool pressed = false;
    int stable = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool now = gpio_get_level(BOARD_PTT_BUTTON) == 0;
#if CONFIG_ZELLO_BOARD_TDECK
        static bool s_raw_was;
        if (now && !s_raw_was && !input_tdeck_talk_active()) ESP_LOGI(TAG, "button ignored: not on the home screen");
        s_raw_was = now;
        if (!input_tdeck_talk_active()) now = false;
#endif
        if (now == pressed) {
            stable = 0;
            continue;
        }
        if (++stable < 3) continue;
        stable = 0;
        pressed = now;
        ESP_LOGI(TAG, "button %s", pressed ? "pressed" : "released");

        if (g_settings.ptt_latch) {
            if (pressed) zello_set_ptt(!zello_ptt());
        } else {
            zello_set_ptt(pressed);
        }
    }
}

void ptt_button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PTT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(ptt_task, "ptt", 2560, NULL, 10, NULL);
}
