#pragma once

#include <stdbool.h>
#include "esp_err.h"

#include "sdkconfig.h"

#if CONFIG_ZELLO_BOARD_CARDPUTER_ZERO
// The CardputerZero keeps backups and crash reports in the app's folder,
// which the app runs in.
#define SDCARD_MOUNT "."
#define SDCARD_DIR_NAME "~/.config/scheff-zello"
#define SDCARD_ON       "in " SDCARD_DIR_NAME
#define SDCARD_FROM     "from " SDCARD_DIR_NAME
#define SDCARD_MISSING  "Couldn't open " SDCARD_DIR_NAME "."
#define SDCARD_LOOKING  "Looking in " SDCARD_DIR_NAME "..."
#define SDCARD_NOT_FOUND_HINT "No backup found. Put scheff-zello-backup.yaml in " SDCARD_DIR_NAME "."
#else
#define SDCARD_MOUNT "/sdcard"
#define SDCARD_ON       "on the SD card"
#define SDCARD_FROM     "from the SD card"
#define SDCARD_MISSING  "No SD card found. Insert a FAT-formatted microSD card."
#define SDCARD_LOOKING  "Looking on the SD card..."
#define SDCARD_NOT_FOUND_HINT "No backup found. Insert the microSD card holding scheff-zello-backup.yaml."
#endif

// Mounts the board's microSD card at /sdcard if it isn't already. Safe to call
// repeatedly: a card inserted after boot is picked up on the next call.
esp_err_t sdcard_mount(void);
bool sdcard_mounted(void);
