#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

// Browser-based setup (Wi-Fi, the Zello account and channels) on port 80. Until
// onboarding is complete it also brings up a board-named open setup hotspot,
// with the page at http://192.168.4.1 (no captive portal). The hotspot turns
// off as soon as station Wi-Fi gets an IP; the web server itself remains
// available there. Call after net_wifi_init().
esp_err_t web_config_start(void);

// The newest backup at the root of the microSD card (scheff-zello-backup*.yaml
// or .yml, or an old zello-p4-backup*.xml), for the device to offer during
// onboarding. Mounts the card. Fills in the file name and its modification
// time; returns false if there is no card or no backup on it.
bool web_config_sd_backup_find(char *name, size_t size, time_t *mtime);

// Writes the complete backup to the SD card, as the web config's SD export
// does. `msg` gets a one-line result for the screen either way.
esp_err_t web_config_sd_backup_export(char *msg, size_t size);

// Imports a backup found above, exactly as the web config's SD import does.
// `msg` gets a one-line result for the screen either way.
esp_err_t web_config_sd_backup_import(const char *name, char *msg, size_t size);
