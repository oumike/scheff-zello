#include "web_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "audio.h"
#include "channel_list.h"
#include "crash_report.h"
#include "file_store.h"
#include "net_wifi.h"
#include "onboard.h"
#include "sdcard.h"
#include "settings.h"
#include "xml_util.h"
#include "yaml_util.h"
#include "zello_client.h"

static const char *TAG = "web";

#define JOIN_TIMEOUT_MS 20000
// A developer token is a long JWT, so the account POST is the biggest body we
// accept by a wide margin.
#define BODY_MAX   2048
#define BODY_READ_TIMEOUT_MS 15000
#define BODY_TIMEOUT_RETRIES 3
#define SCAN_MAX   30
// A full legacy XML list can exceed 100 KiB when each string byte is escaped.
#define IMPORT_MAX (4096 + 6 * sizeof(settings_t) + CHANNEL_LIST_MAX * \
                    (128 + 6 * (sizeof(((channel_entry_t *)0)->name) + sizeof(((channel_entry_t *)0)->desc))))
#define BACKUP_FILE_NAME "scheff-zello-backup.yaml"
#define SD_BACKUP_PATH   SDCARD_MOUNT "/" BACKUP_FILE_NAME
// Before the rename to Scheff for Zello (and YAML) the SD card backup was
// zello-p4-backup.xml; a card may still hold one, and import reads it.
#define SD_LEGACY_BACKUP_PATH SDCARD_MOUNT "/zello-p4-backup.xml"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_httpd;
static bool s_ap_stop_pending;

static struct {
    enum { JOIN_IDLE, JOIN_BUSY, JOIN_OK, JOIN_FAILED } state;
    char ssid[33];
    char pass[65];
    char msg[96];
} s_join;

// ---- helpers ---------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, txt);
    free(txt);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

static esp_err_t reject_request(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    send_error(req, status, msg);
    // Returning success would make HTTPD drain the remaining body before it
    // handles a queued close, defeating our deadline for a slow sender.
    return ESP_FAIL;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static esp_err_t read_body(httpd_req_t *req, size_t max, bool external, char **out)
{
    *out = NULL;
    const size_t size = req->content_len;
    esp_err_t err = ESP_OK;
    char *body = NULL;
    if (!size || size > max) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        body = external ? heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM) : malloc(size + 1);
        if (!body) err = ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    unsigned timeouts = 0;
    TickType_t started = xTaskGetTickCount();
    while (err == ESP_OK && got < size) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(BODY_READ_TIMEOUT_MS)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts < BODY_TIMEOUT_RETRIES) continue;
            err = ESP_ERR_TIMEOUT;
        } else if (n <= 0 || (size_t)n > size - got) {
            err = ESP_FAIL;
        } else {
            got += (size_t)n;
        }
    }
    if (err == ESP_OK &&
        (TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(BODY_READ_TIMEOUT_MS)) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK && memchr(body, 0, got)) err = ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) {
        if (body) memset(body, 0, size + 1);
        free(body);
        return err;
    }
    body[got] = 0;
    *out = body;
    return ESP_OK;
}

static cJSON *read_json(httpd_req_t *req)
{
    char *body = NULL;
    if (read_body(req, BODY_MAX, false, &body) != ESP_OK) return NULL;
    size_t size = req->content_len;
    bool valid = true, in_string = false;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)body[i];
        if (in_string && c == '\\') {
            // cJSON represents strings with C terminators, so a decoded NUL
            // would silently change an identifier (or even an object key).
            if (i + 5 < size && !memcmp(body + i + 1, "u0000", 5)) valid = false;
            i++;
        } else if (c == '"') {
            in_string = !in_string;
        } else if (c < 0x20 && (in_string || (c != '\t' && c != '\r' && c != '\n'))) {
            valid = false;
        }
    }
    const char *end = NULL;
    cJSON *j = valid ? cJSON_ParseWithLengthOpts(body, size + 1, &end, false) : NULL;
    if (j) {
        while (end < body + size && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
        if (end != body + size || !cJSON_IsObject(j)) {
            cJSON_Delete(j);
            j = NULL;
        }
    }
    // The token and the passwords passed through here; don't leave them in the
    // heap for whatever allocates this block next.
    memset(body, 0, size);
    free(body);
    return j;
}

static esp_err_t check_empty_body(httpd_req_t *req, bool allow_json)
{
    if (!req->content_len) return ESP_OK;
    // Command buttons post {}, while GET handlers never accept a body.
    cJSON *j = allow_json ? read_json(req) : NULL;
    bool empty_object = j && cJSON_GetArraySize(j) == 0;
    cJSON_Delete(j);
    if (empty_object) return ESP_OK;
    return reject_request(req, "400 Bad Request",
                          allow_json ? "Send an empty body or an empty JSON object."
                                     : "This request does not accept a body.");
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool json_string_field(cJSON *obj, const char *key, size_t size, bool required, const char **out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v && !required) {
        *out = "";
        return true;
    }
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= size ||
        (required && !v->valuestring[0])) return false;
    *out = v->valuestring;
    return true;
}

// ---- backup ----------------------------------------------------------------

// The backup is YAML: every setting, then the channel list. Strings are
// always quoted, so a password full of punctuation survives a hand edit.
static esp_err_t backup_write(yaml_write_cb_t write, void *ctx)
{
    channel_entry_t *list = heap_caps_malloc(CHANNEL_LIST_MAX * sizeof(*list), MALLOC_CAP_SPIRAM);
    if (!list) return ESP_ERR_NO_MEM;
    size_t n = channel_list_get(NULL, false, list, CHANNEL_LIST_MAX);

    esp_err_t err = yaml_write_text(write, ctx,
                                    "# Scheff for Zello backup: every setting and the channel list.\n"
                                    "# It holds your Wi-Fi password, Zello password and developer token\n"
                                    "# in clear text; store it accordingly.\n"
                                    "scheff-backup: 1\n"
                                    "settings:\n");
#define W(call) do { if (err == ESP_OK) err = call; } while (0)
    W(yaml_write_str(write, ctx, "  ", "network", g_settings.network == ZELLO_NET_WORK ? "work" : "consumer"));
    W(yaml_write_str(write, ctx, "  ", "username", g_settings.username));
    W(yaml_write_str(write, ctx, "  ", "password", g_settings.password));
    W(yaml_write_str(write, ctx, "  ", "auth_token", g_settings.auth_token));
    W(yaml_write_str(write, ctx, "  ", "work_network", g_settings.work_network));
    W(yaml_write_str(write, ctx, "  ", "channel", g_settings.channel));
    W(yaml_write_str(write, ctx, "  ", "wifi_ssid", g_settings.wifi_ssid));
    W(yaml_write_str(write, ctx, "  ", "wifi_pass", g_settings.wifi_pass));
    W(yaml_write_u32(write, ctx, "  ", "volume", g_settings.volume));
    W(yaml_write_u32(write, ctx, "  ", "mic_gain", g_settings.mic_gain));
    W(yaml_write_bool(write, ctx, "  ", "auto_connect", g_settings.auto_connect));
    W(yaml_write_bool(write, ctx, "  ", "ptt_latch", g_settings.ptt_latch));
    W(yaml_write_text(write, ctx, n ? "channels:\n" : "channels: []\n"));
    for (size_t i = 0; i < n; i++) {
        W(yaml_write_str(write, ctx, "  - ", "name", list[i].name));
        W(yaml_write_str(write, ctx, "    ", "desc", list[i].desc));
        W(yaml_write_bool(write, ctx, "    ", "favorite", list[i].favorite));
        W(yaml_write_u32(write, ctx, "    ", "uses", list[i].uses));
    }
#undef W
    free(list);
    return err;
}

// What a YAML backup held. Settings start as the current ones, so a file that
// lists only some of them (a hand-trimmed one, say) changes only those.
typedef struct {
    settings_t settings;
    bool has_settings, has_channels;
    channel_entry_t *channels;
    size_t count;
    int item;  // the list index `channels[count - 1]` came from
} yaml_backup_t;

static esp_err_t backup_yaml_value(void *ctx, const char *section, int item, const char *key, const char *value)
{
    yaml_backup_t *b = ctx;
    if (!section) {
        // "zello-backup" is what the first YAML backups called it.
        if (key && (!strcmp(key, "scheff-backup") || !strcmp(key, "zello-backup")) && strcmp(value, "1")) {
            return ESP_ERR_INVALID_VERSION;
        }
        return ESP_OK;  // other top-level keys are not ours to judge
    }

    if (!strcmp(section, "settings")) {
        b->has_settings = true;
        if (!key) return ESP_OK;
        settings_t *s = &b->settings;
        uint32_t number;
#define STR(field) \
    if (!strcmp(key, #field)) \
        return strlcpy(s->field, value, sizeof(s->field)) < sizeof(s->field) ? ESP_OK : ESP_ERR_INVALID_ARG
        STR(username);
        STR(password);
        STR(auth_token);
        STR(work_network);
        STR(channel);
        STR(wifi_ssid);
        STR(wifi_pass);
#undef STR
        if (!strcmp(key, "network")) {
            if (!strcmp(value, "work")) s->network = ZELLO_NET_WORK;
            else if (!strcmp(value, "consumer")) s->network = ZELLO_NET_CONSUMER;
            else return ESP_ERR_INVALID_ARG;
        } else if (!strcmp(key, "volume")) {
            if (!yaml_u32(value, 100, &number)) return ESP_ERR_INVALID_ARG;
            s->volume = number;
        } else if (!strcmp(key, "mic_gain")) {
            if (!yaml_u32(value, 42, &number)) return ESP_ERR_INVALID_ARG;
            s->mic_gain = number;
        } else if (!strcmp(key, "auto_connect")) {
            if (!yaml_bool(value, &s->auto_connect)) return ESP_ERR_INVALID_ARG;
        } else if (!strcmp(key, "ptt_latch")) {
            if (!yaml_bool(value, &s->ptt_latch)) return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    if (!strcmp(section, "channels")) {
        b->has_channels = true;
        if (!key) return ESP_OK;
        if (item < 0) return ESP_ERR_INVALID_ARG;  // a map where the list should be
        if (b->count == 0 || item != b->item) {
            if (b->count == CHANNEL_LIST_MAX) return ESP_ERR_INVALID_ARG;
            memset(&b->channels[b->count++], 0, sizeof(b->channels[0]));
            b->item = item;
        }
        channel_entry_t *e = &b->channels[b->count - 1];
        if (!strcmp(key, "name")) {
            if (strlcpy(e->name, value, sizeof(e->name)) >= sizeof(e->name)) return ESP_ERR_INVALID_ARG;
        } else if (!strcmp(key, "desc")) {
            if (strlcpy(e->desc, value, sizeof(e->desc)) >= sizeof(e->desc)) return ESP_ERR_INVALID_ARG;
        } else if (!strcmp(key, "favorite")) {
            if (!yaml_bool(value, &e->favorite)) return ESP_ERR_INVALID_ARG;
        } else if (!strcmp(key, "uses")) {
            if (!yaml_u32(value, UINT32_MAX, &e->uses)) return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    return ESP_OK;  // an unknown section is ignored
}

// Reads the <settings> element out of an old XML backup. `*found` says whether
// there was one at all: a bare channel list is a valid thing to import.
static esp_err_t backup_parse_settings_xml(const char *xml, settings_t *settings, bool *found)
{
    const char *element = xml_find_element(xml, "settings");
    *found = element != NULL;
    if (!element) return ESP_OK;
    const char *end = xml_element_end(element);
    if (!end) return ESP_ERR_INVALID_ARG;

    char value[16];
    if (xml_attr(element, end, "version", value, sizeof(value)) != 1 || strcmp(value, "1")) {
        return ESP_ERR_INVALID_VERSION;
    }

    memset(settings, 0, sizeof(*settings));
#define READ_STR(field) \
    do { \
        if (xml_attr(element, end, #field, settings->field, sizeof(settings->field)) != 1) \
            return ESP_ERR_INVALID_ARG; \
    } while (0)
    READ_STR(username);
    READ_STR(password);
    READ_STR(auth_token);
    READ_STR(work_network);
    READ_STR(channel);
    READ_STR(wifi_ssid);
    READ_STR(wifi_pass);
#undef READ_STR

    if (xml_attr(element, end, "network", value, sizeof(value)) != 1) return ESP_ERR_INVALID_ARG;
    if (!strcmp(value, "work")) settings->network = ZELLO_NET_WORK;
    else if (!strcmp(value, "consumer")) settings->network = ZELLO_NET_CONSUMER;
    else return ESP_ERR_INVALID_ARG;

    uint32_t number;
    if (!xml_attr_u32(element, end, "volume", 100, &number)) return ESP_ERR_INVALID_ARG;
    settings->volume = number;
    if (!xml_attr_u32(element, end, "mic_gain", 42, &number)) return ESP_ERR_INVALID_ARG;
    settings->mic_gain = number;
    if (!xml_attr_bool(element, end, "auto_connect", &settings->auto_connect)) return ESP_ERR_INVALID_ARG;
    if (!xml_attr_bool(element, end, "ptt_latch", &settings->ptt_latch)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static void restored_wifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass);
    onboard_settings_changed();
    vTaskDelete(NULL);
}

static esp_err_t backup_apply_settings(const settings_t *settings, bool *wifi_changed, bool *reconnecting)
{
    settings_t previous = g_settings;
    *wifi_changed = strcmp(settings->wifi_ssid, previous.wifi_ssid) ||
                    strcmp(settings->wifi_pass, previous.wifi_pass);
    *reconnecting = *wifi_changed && settings->wifi_ssid[0];
    g_settings = *settings;
    esp_err_t err = settings_save();
    if (err != ESP_OK) {
        g_settings = previous;
        return err;
    }

    audio_set_volume(g_settings.volume);
    audio_set_mic_gain(g_settings.mic_gain);
    zello_reconfigure();
    if (*reconnecting) {
        if (xTaskCreate(restored_wifi_task, "restore_wifi", 4096, NULL, 5, NULL) != pdPASS) {
            *reconnecting = false;
            onboard_settings_changed();
        }
    } else {
        onboard_settings_changed();
    }
    return ESP_OK;
}

// ---- handlers --------------------------------------------------------------

static esp_err_t h_index(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    zello_status_t zs;
    zello_get_status(&zs);

    cJSON *root = cJSON_CreateObject();
    char ip[16] = "";
    bool sta = net_wifi_sta_ip(ip, sizeof(ip));

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", g_settings.wifi_ssid);
    cJSON_AddBoolToObject(wifi, "connected", sta);
    cJSON_AddStringToObject(wifi, "ip", ip);

    // Neither password nor the token ever leaves the device; only whether one
    // is stored.
    cJSON *acct = cJSON_AddObjectToObject(root, "account");
    cJSON_AddStringToObject(acct, "network", g_settings.network == ZELLO_NET_WORK ? "work" : "consumer");
    cJSON_AddStringToObject(acct, "username", g_settings.username);
    cJSON_AddStringToObject(acct, "work_network", g_settings.work_network);
    cJSON_AddBoolToObject(acct, "has_password", g_settings.password[0] != 0);
    cJSON_AddBoolToObject(acct, "has_token", g_settings.auth_token[0] != 0);

    static const char *const STATES[] = {"offline", "connecting", "signing_in", "online", "failed"};
    cJSON *zello = cJSON_AddObjectToObject(root, "zello");
    cJSON_AddStringToObject(zello, "state", STATES[zs.state]);
    cJSON_AddStringToObject(zello, "channel", g_settings.channel);
    cJSON_AddNumberToObject(zello, "users_online", zs.users_online);
    cJSON_AddBoolToObject(zello, "talking", zs.ptt);
    cJSON_AddBoolToObject(zello, "receiving", zs.receiving);
    cJSON_AddStringToObject(zello, "talker", zs.talker);
    cJSON_AddStringToObject(zello, "error", zs.error);

    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(ap, "active", net_wifi_ap_active());
    cJSON_AddStringToObject(ap, "ssid", net_wifi_ap_active() ? net_wifi_ap_ssid() : "");

    static const char *const JOIN_STATES[] = {"idle", "joining", "ok", "failed"};
    cJSON *join = cJSON_AddObjectToObject(root, "join");
    cJSON_AddStringToObject(join, "state", JOIN_STATES[s_join.state]);
    cJSON_AddStringToObject(join, "ssid", s_join.ssid);
    cJSON_AddStringToObject(join, "msg", s_join.msg);

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddNumberToObject(audio, "volume", g_settings.volume);
    cJSON_AddNumberToObject(audio, "mic_gain", g_settings.mic_gain);
    cJSON_AddBoolToObject(audio, "ptt_latch", g_settings.ptt_latch);

    cJSON_AddNumberToObject(root, "channels", channel_list_count());
    cJSON_AddNumberToObject(root, "favorites", channel_list_favorite_count());
    cJSON_AddBoolToObject(root, "onboarded", onboard_complete());
    return send_json(req, root);
}

static esp_err_t h_log(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    char *log = malloc(ZELLO_LOG_MAX);
    if (!log) return httpd_resp_send_500(req);
    zello_get_log(log, ZELLO_LOG_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "log", log);
    free(log);
    return send_json(req, root);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    net_wifi_ap_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    if (!aps) return httpd_resp_send_500(req);
    int n = net_wifi_scan(aps, SCAN_MAX);
    if (n < 0) {
        free(aps);
        return send_error(req, "503 Service Unavailable", "Scan failed. Try again.");
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    return send_json(req, arr);
}

static void join_task(void *arg)
{
    net_join_result_t res = net_wifi_join(s_join.ssid, s_join.pass, JOIN_TIMEOUT_MS);
    switch (res) {
    case NET_JOIN_OK: s_join.msg[0] = 0; break;
    case NET_JOIN_BAD_PASSWORD: strlcpy(s_join.msg, "The password was rejected.", sizeof(s_join.msg)); break;
    case NET_JOIN_NOT_FOUND:
        snprintf(s_join.msg, sizeof(s_join.msg), "Couldn't find \"%s\". Check the name, or scan again.", s_join.ssid);
        break;
    case NET_JOIN_TIMEOUT: strlcpy(s_join.msg, "Timed out waiting for the network.", sizeof(s_join.msg)); break;
    default: strlcpy(s_join.msg, "Couldn't connect to the network.", sizeof(s_join.msg)); break;
    }
    if (res == NET_JOIN_OK && onboard_save_wifi(s_join.ssid, s_join.pass) != ESP_OK) {
        res = NET_JOIN_FAILED;
        strlcpy(s_join.msg, "Connected, but couldn't save the Wi-Fi settings.", sizeof(s_join.msg));
    }
    memset(s_join.pass, 0, sizeof(s_join.pass));
    s_join.state = res == NET_JOIN_OK ? JOIN_OK : JOIN_FAILED;
    vTaskDelete(NULL);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    const char *ssid = json_str(j, "ssid");
    if (!ssid[0] || strlen(ssid) > 32) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "Enter a network name.");
    }
    if (s_join.state == JOIN_BUSY) {
        cJSON_Delete(j);
        return send_error(req, "409 Conflict", "Already connecting. Wait a moment.");
    }
    strlcpy(s_join.ssid, ssid, sizeof(s_join.ssid));
    strlcpy(s_join.pass, json_str(j, "pass"), sizeof(s_join.pass));
    cJSON_Delete(j);
    s_join.msg[0] = 0;
    s_join.state = JOIN_BUSY;
    // Answer first: joining can move the hotspot's channel and drop the phone
    // for a moment. The page polls /api/status for the outcome.
    if (xTaskCreate(join_task, "web_join", 4096, NULL, 5, NULL) != pdPASS) {
        s_join.state = JOIN_FAILED;
        return send_error(req, "500 Internal Server Error", "Couldn't start the join.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", "joining");
    return send_json(req, root);
}

static esp_err_t h_account(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    const char *err = NULL;
    // A blank password or token keeps the stored one; the page never sees
    // either, so it cannot echo them back.
    esp_err_t e = onboard_save_account(json_str(j, "network"), json_str(j, "username"), json_str(j, "password"),
                                       json_str(j, "auth_token"), json_str(j, "work_network"), &err);
    cJSON_Delete(j);
    if (e != ESP_OK) return send_error(req, "400 Bad Request", err ? err : "Couldn't save.");
    return send_ok(req);
}

static esp_err_t h_audio(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    cJSON *volume = cJSON_GetObjectItemCaseSensitive(j, "volume");
    cJSON *gain = cJSON_GetObjectItemCaseSensitive(j, "mic_gain");
    cJSON *latch = cJSON_GetObjectItemCaseSensitive(j, "ptt_latch");
    if (cJSON_IsNumber(volume) && volume->valuedouble >= 0 && volume->valuedouble <= 100) {
        g_settings.volume = (uint8_t)volume->valuedouble;
        audio_set_volume(g_settings.volume);
    }
    if (cJSON_IsNumber(gain) && gain->valuedouble >= 0 && gain->valuedouble <= 42) {
        g_settings.mic_gain = (uint8_t)gain->valuedouble;
        audio_set_mic_gain(g_settings.mic_gain);
    }
    if (cJSON_IsBool(latch)) g_settings.ptt_latch = cJSON_IsTrue(latch);
    cJSON_Delete(j);
    if (settings_save() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Couldn't save the audio settings.");
    }
    return send_ok(req);
}

// ---- channels --------------------------------------------------------------

// Decodes %XX and '+' in a query value, in place.
static bool url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%') {
            if (!isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2])) return false;
            char hex[3] = {s[1], s[2], 0};
            char decoded = (char)strtol(hex, NULL, 16);
            if (!decoded) return false;
            *o++ = decoded;
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
    return true;
}

// GET /api/channels[?q=text]
static esp_err_t h_channels(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    enum { QUERY_MAX = sizeof(((channel_entry_t *)0)->name) - 1 };
    char qs[3 * QUERY_MAX + 3] = "", query[sizeof(qs)] = "";
    esp_err_t err = httpd_req_get_url_query_str(req, qs, sizeof(qs));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return send_error(req, "400 Bad Request", "The channel search is too long or invalid.");
    }
    if (err == ESP_OK) {
        err = httpd_query_key_value(qs, "q", query, sizeof(query));
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return send_error(req, "400 Bad Request", "The channel search is too long or invalid.");
        }
    }
    if (!url_decode(query) || strlen(query) > QUERY_MAX) {
        return send_error(req, "400 Bad Request", "The channel search must fit in 64 UTF-8 bytes.");
    }

    channel_entry_t *list = calloc(CHANNEL_LIST_MAX, sizeof(*list));
    if (!list) return httpd_resp_send_500(req);
    size_t n = channel_list_get(query, false, list, CHANNEL_LIST_MAX);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", list[i].name);
        cJSON_AddStringToObject(o, "desc", list[i].desc);
        cJSON_AddNumberToObject(o, "uses", list[i].uses);
        cJSON_AddBoolToObject(o, "favorite", list[i].favorite);
        cJSON_AddBoolToObject(o, "current", !strcmp(list[i].name, g_settings.channel));
        cJSON_AddItemToArray(arr, o);
    }
    free(list);
    return send_json(req, arr);
}

// POST /api/channels {orig_name, name, desc}: add or edit.
static esp_err_t h_channel_save(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    channel_entry_t entry = {0};
    const char *orig, *name, *desc, *error = NULL;
    if (!json_string_field(j, "name", sizeof(entry.name), true, &name)) {
        error = "A channel name of 1 to 64 UTF-8 bytes is required.";
    } else if (!json_string_field(j, "orig_name", sizeof(entry.name), false, &orig)) {
        error = "The original channel name must be a string of at most 64 UTF-8 bytes.";
    } else if (!json_string_field(j, "desc", sizeof(entry.desc), false, &desc)) {
        error = "The channel note must be a string of at most 63 UTF-8 bytes.";
    }
    if (error) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", error);
    }
    if (orig[0]) channel_list_find(orig, &entry);  // keep the favorite flag and use count
    strlcpy(entry.name, name, sizeof(entry.name));
    strlcpy(entry.desc, desc, sizeof(entry.desc));
    esp_err_t err = channel_list_put(orig[0] ? orig : NULL, &entry);
    cJSON_Delete(j);
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "400 Bad Request", "The channel list is full. Delete one first.");
    }
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the channel list.");
    return send_ok(req);
}

// POST /api/channels/delete {name}
static esp_err_t h_channel_delete(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    const char *name;
    if (!json_string_field(j, "name", sizeof(((channel_entry_t *)0)->name), true, &name)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "A channel name of 1 to 64 UTF-8 bytes is required.");
    }
    esp_err_t err = channel_list_delete(name);
    cJSON_Delete(j);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "No such channel.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the channel list.");
    return send_ok(req);
}

// POST /api/channels/favorite {name, favorite}
static esp_err_t h_channel_favorite(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    const char *name;
    cJSON *favorite = cJSON_GetObjectItemCaseSensitive(j, "favorite");
    if (!json_string_field(j, "name", sizeof(((channel_entry_t *)0)->name), true, &name) ||
        !cJSON_IsBool(favorite)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "A channel name of 1 to 64 UTF-8 bytes and a boolean favorite are required.");
    }
    bool fav = cJSON_IsTrue(favorite);
    esp_err_t err = channel_list_set_favorite(name, fav);
    cJSON_Delete(j);
    if (err == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "No such channel.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't save the channel list.");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "favorite", fav);
    return send_json(req, root);
}

// POST /api/channels/join {name}: make it the channel we're signed in to.
static esp_err_t h_channel_join(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    const char *name;
    if (!json_string_field(j, "name", sizeof(((channel_entry_t *)0)->name), true, &name)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "A channel name of 1 to 64 UTF-8 bytes is required.");
    }
    const char *err = NULL;
    esp_err_t result = onboard_save_channel(name, &err);
    cJSON_Delete(j);
    if (result != ESP_OK) {
        return send_error(req, "400 Bad Request", err ? err : "Couldn't join that channel.");
    }
    return send_ok(req);
}

// POST /api/text {text}
static esp_err_t h_text(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return reject_request(req, "400 Bad Request", "Bad request.");
    char text[257];
    strlcpy(text, json_str(j, "text"), sizeof(text));
    cJSON_Delete(j);
    esp_err_t err = zello_send_text(text);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "Not signed in to a channel.");
    if (err != ESP_OK) return send_error(req, "400 Bad Request", "Couldn't send that message.");
    return send_ok(req);
}

// ---- export / import -------------------------------------------------------

static esp_err_t backup_http_write(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk(ctx, data, len);
}

static esp_err_t backup_file_write(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, ctx) == len ? ESP_OK : ESP_FAIL;
}

// GET /api/export: all settings and the channel list as one download.
static esp_err_t h_export_download(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_type(req, "application/yaml");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"" BACKUP_FILE_NAME "\"");
    esp_err_t err = backup_write(backup_http_write, req);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t import_result(httpd_req_t *req, size_t count, const char *what, bool has_settings,
                               bool has_channels, bool wifi_changed, bool reconnecting)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddStringToObject(root, "from", what);
    cJSON_AddBoolToObject(root, "settings", has_settings);
    cJSON_AddBoolToObject(root, "channels", has_channels);
    cJSON_AddBoolToObject(root, "wifi_changed", wifi_changed);
    cJSON_AddBoolToObject(root, "reconnecting", reconnecting);
    return send_json(req, root);
}

static esp_err_t import_yaml(httpd_req_t *req, const char *text, const char *what)
{
    yaml_backup_t *b = heap_caps_calloc(1, sizeof(*b), MALLOC_CAP_SPIRAM);
    channel_entry_t *list = heap_caps_malloc(CHANNEL_LIST_MAX * sizeof(*list), MALLOC_CAP_SPIRAM);
    if (!b || !list) {
        free(b);
        free(list);
        return httpd_resp_send_500(req);
    }
    b->settings = g_settings;
    b->channels = list;

    int line = 0;
    esp_err_t err = yaml_parse(text, backup_yaml_value, b, &line);
    char msg[96];
    const char *fail = NULL, *status = "400 Bad Request";
    if (err == ESP_ERR_INVALID_VERSION) {
        fail = "This backup is from a newer version; nothing was changed.";
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(msg, sizeof(msg), "Line %d of the backup isn't valid; nothing was changed.", line);
        fail = msg;
    } else if (err != ESP_OK) {
        fail = "Import failed; nothing was changed.";
        status = "500 Internal Server Error";
    } else if (!b->has_settings && !b->has_channels) {
        fail = "That file has no settings or channels; nothing was changed.";
    }

    // An empty list in the file leaves the device's list alone rather than
    // wiping it.
    size_t count = channel_list_count();
    bool channels_replaced = false;
    if (!fail && b->count) {
        err = channel_list_replace(b->channels, b->count, &count);
        if (err == ESP_OK) {
            channels_replaced = true;
        } else if (err != ESP_ERR_NOT_FOUND) {
            fail = "Import failed; nothing was changed.";
            status = "500 Internal Server Error";
        }
    }

    bool wifi_changed = false, reconnecting = false;
    if (!fail && b->has_settings && backup_apply_settings(&b->settings, &wifi_changed, &reconnecting) != ESP_OK) {
        fail = channels_replaced ? "The channel list was restored, but the settings could not be saved."
                                 : "The settings could not be saved.";
        status = "500 Internal Server Error";
    }
    bool has_settings = b->has_settings;
    memset(b, 0, sizeof(*b));  // it held the passwords and the token
    free(b);
    free(list);
    if (fail) return send_error(req, status, fail);
    return import_result(req, count, what, has_settings, channels_replaced, wifi_changed, reconnecting);
}

// A backup is YAML now; one that starts with '<' is an XML backup from before.
static esp_err_t import_backup(httpd_req_t *req, const char *text, const char *what)
{
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '<') return import_yaml(req, text, what);

    const char *xml = text;
    if (xml_validate_document(xml) != ESP_OK) {
        return send_error(req, "400 Bad Request", "The XML backup is malformed; nothing was changed.");
    }
    settings_t restored;
    bool has_settings;
    esp_err_t err = backup_parse_settings_xml(xml, &restored, &has_settings);
    if (err == ESP_ERR_INVALID_VERSION) {
        return send_error(req, "400 Bad Request", "This backup uses an unsupported settings version.");
    }
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request",
                          "The backup settings are incomplete or invalid; nothing was changed.");
    }

    size_t count = channel_list_count();
    err = channel_list_import_buffer(xml, &count);
    bool channels_replaced = err == ESP_OK;
    if (err == ESP_ERR_NOT_FOUND && !has_settings) {
        return send_error(req, "400 Bad Request", "That file has no <channel> entries; nothing was changed.");
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error",
                          "Import failed; nothing was changed.");
    }
    if (!channels_replaced) count = channel_list_count();

    bool wifi_changed = false, reconnecting = false;
    if (has_settings && backup_apply_settings(&restored, &wifi_changed, &reconnecting) != ESP_OK) {
        return send_error(req, "500 Internal Server Error",
                          channels_replaced ? "The channel list was restored, but the settings could not be saved."
                                            : "The settings could not be saved.");
    }

    return import_result(req, count, what, has_settings, channels_replaced, wifi_changed, reconnecting);
}

// POST /api/import: the body is a complete backup, or just a channel list.
static esp_err_t h_import_upload(httpd_req_t *req)
{
    char *buf = NULL;
    esp_err_t err = read_body(req, IMPORT_MAX, true, &buf);
    if (err == ESP_ERR_INVALID_SIZE) {
        return reject_request(req, "400 Bad Request", "That file is empty or too large.");
    }
    if (err == ESP_ERR_NO_MEM) return reject_request(req, "500 Internal Server Error", "Not enough memory for the upload.");
    if (err == ESP_ERR_TIMEOUT) return reject_request(req, "408 Request Timeout", "The upload timed out.");
    if (err != ESP_OK) {
        return reject_request(req, "400 Bad Request", "The upload is incomplete or contains a NUL byte.");
    }
    err = import_backup(req, buf, "upload");
    memset(buf, 0, req->content_len);  // the backup carries the passwords and the token
    free(buf);
    return err;
}

static esp_err_t sd_error(httpd_req_t *req)
{
    return send_error(req, "503 Service Unavailable", "No SD card found. Insert a FAT-formatted microSD card.");
}

// POST /api/sd/export and /api/sd/import: a complete backup at the root of the
// microSD card.
static esp_err_t h_sd_export(httpd_req_t *req)
{
    if (check_empty_body(req, true) != ESP_OK) return ESP_FAIL;
    if (sdcard_mount() != ESP_OK) return sd_error(req);
    if (file_store_recover(SD_BACKUP_PATH) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Couldn't recover the SD card backup.");
    }
    FILE *file = fopen(SD_BACKUP_PATH ".tmp", "w");
    if (!file) return send_error(req, "500 Internal Server Error", "Couldn't write to the SD card.");
    esp_err_t err = backup_write(backup_file_write, file);
    if (err == ESP_OK && fflush(file) != 0) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err == ESP_OK) err = file_store_commit(SD_BACKUP_PATH);
    if (err != ESP_OK) {
        remove(SD_BACKUP_PATH ".tmp");
        return send_error(req, "500 Internal Server Error", "Couldn't write to the SD card.");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", BACKUP_FILE_NAME);
    cJSON_AddNumberToObject(root, "count", channel_list_count());
    cJSON_AddBoolToObject(root, "settings", true);
    return send_json(req, root);
}

static esp_err_t backup_read_file(const char *path, char **out)
{
    *out = NULL;
    esp_err_t err = file_store_recover(path);
    if (err != ESP_OK) return err;
    FILE *file = fopen(path, "r");
    if (!file) return ESP_ERR_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size <= 0 || (size_t)size > IMPORT_MAX) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    char *data = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(data, 1, size, file);
    bool ok = got == (size_t)size && !ferror(file);
    if (fclose(file) != 0) ok = false;
    err = !ok ? ESP_FAIL : memchr(data, 0, got) ? ESP_ERR_INVALID_ARG : ESP_OK;
    if (err != ESP_OK) {
        memset(data, 0, size + 1);
        free(data);
        return err;
    }
    data[got] = 0;
    *out = data;
    return ESP_OK;
}

static esp_err_t h_sd_import(httpd_req_t *req)
{
    if (check_empty_body(req, true) != ESP_OK) return ESP_FAIL;
    if (sdcard_mount() != ESP_OK) return sd_error(req);
    char *text = NULL;
    esp_err_t err = backup_read_file(SD_BACKUP_PATH, &text);
    if (err == ESP_ERR_NOT_FOUND) err = backup_read_file(SD_LEGACY_BACKUP_PATH, &text);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "No " BACKUP_FILE_NAME " on the SD card.");
    }
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, "400 Bad Request", "That file is empty or too large.");
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "The backup contains a NUL byte.");
    if (err != ESP_OK) return send_error(req, "500 Internal Server Error", "Couldn't read the SD card backup.");
    err = import_backup(req, text, "sd");
    memset(text, 0, strlen(text));
    free(text);
    return err;
}

// ---- crash reports ---------------------------------------------------------

static void crash_list_add(void *ctx, const char *name, long size)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "size", size);
    cJSON_AddItemToArray(ctx, o);
}

// GET /api/crash: the last reset reason and every report file.
static esp_err_t h_crash_list(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "last_reset", crash_report_last_reset());
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    cJSON_AddBoolToObject(root, "sd", crash_report_list(crash_list_add, files) == ESP_OK);
    return send_json(req, root);
}

// GET /api/crash/file?name=NNNN-crash.txt: one report as a download.
static esp_err_t h_crash_file(httpd_req_t *req)
{
    if (check_empty_body(req, false) != ESP_OK) return ESP_FAIL;
    char query[96], name[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "No report named.");
    }
    FILE *f = crash_report_open(name);
    if (!f) return send_error(req, "404 Not Found", "No such report.");

    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    bool elf = strstr(name, ".elf") != NULL;
    httpd_resp_set_type(req, elf ? "application/octet-stream" : "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *buf = malloc(4096);
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, 4096, f)) > 0) err = httpd_resp_send_chunk(req, buf, n);
    free(buf);
    fclose(f);
    if (err != ESP_OK) return err == ESP_ERR_NO_MEM ? httpd_resp_send_500(req) : err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// POST /api/crash/delete: removes every report.
static esp_err_t h_crash_delete(httpd_req_t *req)
{
    if (check_empty_body(req, true) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "deleted", crash_report_delete_all());
    return send_json(req, root);
}

// ---- hotspot lifetime ------------------------------------------------------

// The hotspot goes as soon as station Wi-Fi has an address; from then on the
// web config is reached on the network the device joined.
static void ap_stop_task(void *arg)
{
    while (net_wifi_ap_active() && !net_wifi_has_ip()) vTaskDelay(pdMS_TO_TICKS(500));
    if (net_wifi_ap_active()) net_wifi_ap_stop();
    s_ap_stop_pending = false;
    vTaskDelete(NULL);
}

static void onboard_changed(void)
{
    if (net_wifi_ap_active() && !s_ap_stop_pending) {
        s_ap_stop_pending = true;
        if (xTaskCreate(ap_stop_task, "web_ap_stop", 3072, NULL, 3, NULL) != pdPASS) s_ap_stop_pending = false;
    }
}

esp_err_t web_config_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd start: %s", esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/account", .method = HTTP_POST, .handler = h_account},
        {.uri = "/api/audio", .method = HTTP_POST, .handler = h_audio},
        {.uri = "/api/channels", .method = HTTP_GET, .handler = h_channels},
        {.uri = "/api/channels", .method = HTTP_POST, .handler = h_channel_save},
        {.uri = "/api/channels/delete", .method = HTTP_POST, .handler = h_channel_delete},
        {.uri = "/api/channels/favorite", .method = HTTP_POST, .handler = h_channel_favorite},
        {.uri = "/api/channels/join", .method = HTTP_POST, .handler = h_channel_join},
        {.uri = "/api/text", .method = HTTP_POST, .handler = h_text},
        {.uri = "/api/export", .method = HTTP_GET, .handler = h_export_download},
        {.uri = "/api/import", .method = HTTP_POST, .handler = h_import_upload},
        {.uri = "/api/sd/export", .method = HTTP_POST, .handler = h_sd_export},
        {.uri = "/api/sd/import", .method = HTTP_POST, .handler = h_sd_import},
        {.uri = "/api/crash", .method = HTTP_GET, .handler = h_crash_list},
        {.uri = "/api/crash/file", .method = HTTP_GET, .handler = h_crash_file},
        {.uri = "/api/crash/delete", .method = HTTP_POST, .handler = h_crash_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);

    onboard_set_web_cb(onboard_changed);

    // Until onboarding is done, also offer the setup hotspot. Its monitor
    // turns it off after station Wi-Fi has a confirmed IP.
    if (!onboard_complete() && net_wifi_ap_start() == ESP_OK) {
        onboard_changed();
    }
    ESP_LOGI(TAG, "web config on port 80%s", net_wifi_ap_active() ? " (setup hotspot up)" : "");
    return ESP_OK;
}
