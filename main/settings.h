#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Which Zello network the client talks to. The consumer network always needs a
// developer token; Zello Work takes the network name instead and accepts a
// plain username and password.
typedef enum {
    ZELLO_NET_CONSUMER,
    ZELLO_NET_WORK,
} zello_network_t;

// The developer token is a JWT signed with the key from the Zello developer
// console, so it is long: size the field for one with room to spare.
#define SETTINGS_TOKEN_MAX 1024

typedef struct {
    char username[33];
    char password[65];
    char auth_token[SETTINGS_TOKEN_MAX];
    char work_network[48];  // Zello Work network name (subdomain)
    char channel[65];       // the channel joined at logon
    char wifi_ssid[33];
    char wifi_pass[65];
    uint8_t network;    // zello_network_t
    uint8_t volume;     // 0-100
    uint8_t mic_gain;   // dB, 0-42
    bool auto_connect;  // join `channel` as soon as Wi-Fi is up
    bool ptt_latch;     // tap to start/stop talking instead of press-and-hold
} settings_t;

// Loaded at boot from a versioned NVS snapshot. During one-time migration,
// missing legacy fields use Kconfig defaults; legacy volume/mic_gain values
// above 100/42 are warned about and clamped. Other invalid persisted data
// returns an error without erasing it or replacing the current settings.
extern settings_t g_settings;

esp_err_t settings_load(void);
// Atomically persists all fields as one NVS blob, leaving the prior snapshot
// intact if the blob write fails. Legacy individual keys are not deleted.
esp_err_t settings_save(void);

// Set a field by name from a string (used by the console and the web config).
// Returns ESP_ERR_NOT_FOUND for an unknown key, ESP_ERR_INVALID_ARG for a bad
// value.
esp_err_t settings_set(const char *key, const char *value);
void settings_print(void);

// wss:// URL for the configured network, into `buf`.
void settings_ws_url(char *buf, size_t size);
