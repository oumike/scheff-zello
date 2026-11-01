#include "settings.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define NS "zello"
#define SNAPSHOT_KEY "snapshot"
#define SNAPSHOT_VERSION 1

static const char *TAG = "settings";

settings_t g_settings;

typedef enum { F_STR, F_BOOL, F_U8 } ftype_t;

typedef struct {
    const char *key;
    ftype_t type;
    size_t off;
    size_t size;
    bool secret;
    uint8_t max;
} field_t;

#define STR(k, secret) {#k, F_STR, offsetof(settings_t, k), sizeof(((settings_t *)0)->k), secret, 0}
// NVS keys are limited to 15 characters; all of ours fit.
static const field_t FIELDS[] = {
    STR(username, false),
    STR(password, true),
    STR(auth_token, true),
    STR(work_network, false),
    STR(channel, false),
    STR(wifi_ssid, false),
    STR(wifi_pass, true),
    {"network", F_U8, offsetof(settings_t, network), 1, false, ZELLO_NET_WORK},
    {"volume", F_U8, offsetof(settings_t, volume), 1, false, 100},
    {"mic_gain", F_U8, offsetof(settings_t, mic_gain), 1, false, 42},
    {"auto_connect", F_BOOL, offsetof(settings_t, auto_connect), 1, false, 1},
    {"ptt_latch", F_BOOL, offsetof(settings_t, ptt_latch), 1, false, 1},
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

// NVS commits each key independently. Keep credentials and their associated
// network settings in one versioned blob, not a series of per-field writes.
typedef struct {
    uint32_t version;
    uint32_t size;
    settings_t values;
} snapshot_t;

static bool valid_settings(const settings_t *settings)
{
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        const unsigned char *p = (const unsigned char *)settings + f->off;
        if (f->type == F_STR) {
            if (!memchr(p, 0, f->size)) return false;
        } else if (*p > f->max) {
            return false;
        }
    }
    return true;
}

static esp_err_t save_snapshot(const snapshot_t *snapshot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, SNAPSHOT_KEY, snapshot, sizeof(*snapshot));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->username, CONFIG_ZELLO_USERNAME, sizeof(s->username));
    strlcpy(s->password, CONFIG_ZELLO_PASSWORD, sizeof(s->password));
    strlcpy(s->auth_token, CONFIG_ZELLO_AUTH_TOKEN, sizeof(s->auth_token));
    strlcpy(s->work_network, CONFIG_ZELLO_WORK_NETWORK, sizeof(s->work_network));
    strlcpy(s->channel, CONFIG_ZELLO_CHANNEL, sizeof(s->channel));
    strlcpy(s->wifi_ssid, CONFIG_ZELLO_WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_pass, CONFIG_ZELLO_WIFI_PASSWORD, sizeof(s->wifi_pass));
#ifdef CONFIG_ZELLO_NETWORK_WORK
    s->network = ZELLO_NET_WORK;
#else
    s->network = ZELLO_NET_CONSUMER;
#endif
    s->volume = 70;
    s->mic_gain = 30;
    s->auto_connect = true;
    s->ptt_latch = false;
}

esp_err_t settings_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_defaults(&g_settings);
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    snapshot_t snapshot = {0};
    size_t size = sizeof(snapshot);
    bool migrate = false;
    err = nvs_get_blob(h, SNAPSHOT_KEY, &snapshot, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        snapshot.version = SNAPSHOT_VERSION;
        snapshot.size = sizeof(settings_t);
        set_defaults(&snapshot.values);
        err = ESP_OK;
        for (size_t i = 0; i < NFIELDS && err == ESP_OK; i++) {
            const field_t *f = &FIELDS[i];
            void *p = (char *)&snapshot.values + f->off;
            size_t len = f->size;
            esp_err_t read_err = f->type == F_STR ? nvs_get_str(h, f->key, p, &len) :
                                                  nvs_get_u8(h, f->key, p);
            if (read_err == ESP_OK) migrate = true;
            else if (read_err != ESP_ERR_NVS_NOT_FOUND) err = read_err;
        }
        // The old setter accepted these fields through 255. Repair only that
        // legacy range bug; existing snapshots retain strict validation.
        if (err == ESP_OK && snapshot.values.volume > 100) {
            ESP_LOGW(TAG, "legacy volume %u exceeds 100; clamping for migration",
                     (unsigned)snapshot.values.volume);
            snapshot.values.volume = 100;
        }
        if (err == ESP_OK && snapshot.values.mic_gain > 42) {
            ESP_LOGW(TAG, "legacy mic_gain %u exceeds 42; clamping for migration",
                     (unsigned)snapshot.values.mic_gain);
            snapshot.values.mic_gain = 42;
        }
    } else if (err == ESP_OK && (size != sizeof(snapshot) ||
               snapshot.version != SNAPSHOT_VERSION || snapshot.size != sizeof(settings_t))) {
        err = ESP_ERR_INVALID_STATE;
    }
    nvs_close(h);
    if (err == ESP_OK && !valid_settings(&snapshot.values)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && migrate) err = save_snapshot(&snapshot);
    if (err == ESP_OK) g_settings = snapshot.values;
    memset(&snapshot, 0, sizeof(snapshot));
    return err;
}

esp_err_t settings_save(void)
{
    if (!valid_settings(&g_settings)) return ESP_ERR_INVALID_ARG;
    snapshot_t snapshot = {
        .version = SNAPSHOT_VERSION,
        .size = sizeof(settings_t),
        .values = g_settings,
    };
    esp_err_t err = save_snapshot(&snapshot);
    memset(&snapshot, 0, sizeof(snapshot));
    return err;
}

esp_err_t settings_set(const char *key, const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;
    // "network" takes the name as well as the number, since that is what the
    // console and the web config both have to hand.
    if (!strcmp(key, "network")) {
        if (!strcasecmp(value, "consumer") || !strcmp(value, "0")) {
            g_settings.network = ZELLO_NET_CONSUMER;
            return ESP_OK;
        }
        if (!strcasecmp(value, "work") || !strcmp(value, "1")) {
            g_settings.network = ZELLO_NET_WORK;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        if (strcmp(f->key, key) != 0) continue;
        void *p = (char *)&g_settings + f->off;
        switch (f->type) {
        case F_STR:
            if (strlen(value) >= f->size) return ESP_ERR_INVALID_ARG;
            strlcpy(p, value, f->size);
            return ESP_OK;
        case F_BOOL:
            *(bool *)p = !strcmp(value, "1") || !strcasecmp(value, "on") ||
                         !strcasecmp(value, "true") || !strcasecmp(value, "yes");
            return ESP_OK;
        case F_U8: {
            char *end;
            errno = 0;
            long v = strtol(value, &end, 10);
            if (end == value || *end || errno == ERANGE || v < 0 || v > f->max) {
                return ESP_ERR_INVALID_ARG;
            }
            *(uint8_t *)p = v;
            return ESP_OK;
        }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void settings_print(void)
{
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        const void *p = (const char *)&g_settings + f->off;
        if (!strcmp(f->key, "network")) {
            printf("  %-16s %s\n", f->key, g_settings.network == ZELLO_NET_WORK ? "work" : "consumer");
            continue;
        }
        switch (f->type) {
        case F_STR:
            printf("  %-16s %s\n", f->key,
                   f->secret ? (*(const char *)p ? "********" : "(unset)") : (const char *)p);
            break;
        case F_BOOL:
            printf("  %-16s %s\n", f->key, *(const bool *)p ? "on" : "off");
            break;
        case F_U8:
            printf("  %-16s %u\n", f->key, *(const uint8_t *)p);
            break;
        }
    }
}

void settings_ws_url(char *buf, size_t size)
{
    if (g_settings.network == ZELLO_NET_WORK && g_settings.work_network[0]) {
        snprintf(buf, size, "wss://zellowork.io/ws/%s", g_settings.work_network);
    } else {
        snprintf(buf, size, "wss://zello.io/ws");
    }
}
