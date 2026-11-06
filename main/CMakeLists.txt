set(srcs "app_main.c" "board.c" "display.c" "audio.c" "ptt.c" "sdcard.c"
         "lv_mem_psram.c" "net_wifi.c" "settings.c" "file_store.c" "xml_util.c" "yaml_util.c" "channel_list.c"
         "opus_codec.c" "jwt_util.c" "zello_client.c" "onboard.c" "web_config.c"
         "console_cmds.c" "crash_report.c"
         "ui.c" "ui_wifi.c" "ui_account.c" "ui_channels.c" "ui_home.c")

# The T-Deck's and Tanmatsu's keyboards; the Wio Tracker L2's LCD driver.
set(board_requires "")
if(CONFIG_ZELLO_BOARD_TDECK)
    list(APPEND srcs "input_keys.c")
elseif(CONFIG_ZELLO_BOARD_TANMATSU)
    list(APPEND srcs "input_keys.c")
    list(APPEND board_requires badge-bsp esp-hosted-tanmatsu)
elseif(CONFIG_ZELLO_BOARD_WIO_TRACKER_L2)
    list(APPEND srcs "lcd_nv3031b.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES opus esp_driver_i2c esp_driver_i2s esp_driver_gpio
                  esp_driver_sdmmc esp_driver_spi esp_driver_ledc esp_adc esp_netif esp_event esp_wifi nvs_flash console
                  lwip esp_timer esp_lcd esp_lvgl_port lvgl fatfs wear_levelling
                  esp_http_server esp_websocket_client esp-tls mbedtls json sdmmc
                  espcoredump esp_partition esp_app_format esp_timer ${board_requires})
