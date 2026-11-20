#pragma once

// The Linux builds' stand-in for ESP-IDF's generated sdkconfig.h: the
// CardputerZero board, and the account defaults Kconfig gives the firmware
// (all empty: everything is set up on screen or in the browser).

#define CONFIG_ZELLO_BOARD_CARDPUTER_ZERO 1

#define CONFIG_ZELLO_USERNAME       ""
#define CONFIG_ZELLO_PASSWORD       ""
#define CONFIG_ZELLO_AUTH_TOKEN     ""
#define CONFIG_ZELLO_CHANNEL        ""
#define CONFIG_ZELLO_WIFI_SSID      ""
#define CONFIG_ZELLO_WIFI_PASSWORD  ""
#define CONFIG_ZELLO_WORK_NETWORK   ""

#define CONFIG_WL_SECTOR_SIZE 4096
