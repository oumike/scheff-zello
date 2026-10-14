#include "net_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_check.h"
#include "esp_event.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "board.h"
#endif
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

#define BIT_GOT_IP    BIT0
#define BIT_JOIN_FAIL BIT1

// Attempts net_wifi_join() makes before giving up on a network that is
// neither rejecting the password nor missing.
#define JOIN_MAX_ATTEMPTS 3

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif, *s_ap_netif;
static bool s_ap_on;
static char s_ap_ssid[33];
static SemaphoreHandle_t s_join_lock;
static bool s_want_connect;
static bool s_joining;
static int s_join_attempts;
static uint8_t s_join_reason;

static bool reason_is_auth(uint8_t r)
{
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE;
}

static bool reason_is_not_found(uint8_t r)
{
    return r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD || r == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

#if CONFIG_IDF_TARGET_ESP32P4
static esp_err_t c6_reset_cb(void *arg, bool level)
{
    return board_c6_enable(level);
}
#endif

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        wifi_event_sta_disconnected_t *d = data;
        if (s_joining) {
            // During a UI join, fail fast on a wrong password or missing
            // network rather than retrying forever.
            s_join_reason = d->reason;
            if (reason_is_auth(d->reason) || reason_is_not_found(d->reason) ||
                ++s_join_attempts >= JOIN_MAX_ATTEMPTS) {
                ESP_LOGW(TAG, "join failed (reason %d)", d->reason);
                s_want_connect = false;
                xEventGroupSetBits(s_events, BIT_JOIN_FAIL);
                return;
            }
        }
        if (s_want_connect) {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t net_wifi_init(void)
{
    s_events = xEventGroupCreate();
    s_join_lock = xSemaphoreCreateMutex();

#if CONFIG_IDF_TARGET_ESP32P4
    // The P4 reaches Wi-Fi through the C6 coprocessor over ESP-Hosted. The S3
    // has its own radio, and esp_wifi_init() below is all it needs.
    ESP_RETURN_ON_FALSE(esp_hosted_sdio_set_reset_callback(c6_reset_cb, NULL) == ESP_TRANSPORT_OK,
                        ESP_FAIL, TAG, "hosted reset cb");
    ESP_RETURN_ON_ERROR(board_c6_enable(true), TAG, "c6 power");
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted_init");
#endif

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "scheff-zello");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    // Voice over a WebSocket: modem power save adds latency and packet bunching.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // TLS certificate validity needs a real clock; SNTP provides it.
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    return ESP_OK;
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass && *pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    s_want_connect = false;
    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    s_want_connect = true;
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return esp_wifi_connect();
}

bool net_wifi_wait_ip(uint32_t timeout_ms)
{
    return xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & BIT_GOT_IP;
}

bool net_wifi_has_ip(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_GOT_IP);
}

void net_wifi_print_status(void)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    if (net_wifi_has_ip() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        printf("Wi-Fi: \"%s\" RSSI %d dBm, IP " IPSTR "\n", (char *)ap.ssid, ap.rssi, IP2STR(&ip.ip));
    } else {
        printf("Wi-Fi: not connected\n");
    }
}

int net_wifi_scan(net_wifi_ap_t *out, int max)
{
    // A scan cannot run while the driver is busy retrying a connection.
    if (!net_wifi_has_ip() && s_want_connect) {
        s_want_connect = false;
        esp_wifi_disconnect();
    }

    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) n = 40;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    // Records arrive strongest first; keep the first of each SSID.
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < count && !dup; j++) dup = !strcmp(out[j].ssid, ssid);
        if (dup) continue;
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN && recs[i].authmode != WIFI_AUTH_OWE;
        count++;
    }
    free(recs);
    ESP_LOGI(TAG, "scan found %d networks", count);
    return count;
}

net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    // The screen and the web page can both start a join; one at a time.
    if (xSemaphoreTake(s_join_lock, 0) != pdTRUE) return NET_JOIN_FAILED;
    // Clear a stale GOT_IP too: the disconnect event from any earlier link
    // arrives asynchronously and must not read as this join succeeding.
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_JOIN_FAIL);
    s_join_attempts = 0;
    s_join_reason = 0;
    s_joining = true;

    net_join_result_t res;
    if (net_wifi_connect(ssid, pass) != ESP_OK) {
        res = NET_JOIN_FAILED;
    } else {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_JOIN_FAIL, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(timeout_ms));
        if (bits & BIT_GOT_IP) {
            res = NET_JOIN_OK;
        } else if (bits & BIT_JOIN_FAIL) {
            res = reason_is_auth(s_join_reason)        ? NET_JOIN_BAD_PASSWORD
                  : reason_is_not_found(s_join_reason) ? NET_JOIN_NOT_FOUND
                                                       : NET_JOIN_FAILED;
        } else {
            res = NET_JOIN_TIMEOUT;
        }
    }
    s_joining = false;
    if (res != NET_JOIN_OK) {
        s_want_connect = false;
        esp_wifi_disconnect();
    }
    xSemaphoreGive(s_join_lock);
    return res;
}

esp_err_t net_wifi_ap_start(void)
{
    if (s_ap_on) return ESP_OK;
    if (!s_ap_netif) {
        // No captive portal: nothing answers DNS here, so a phone treats the
        // hotspot as a network without internet and leaves the browser alone.
        // The setup page is at http://192.168.4.1.
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Scheff-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            // Open, so setup needs nothing typed on the phone.
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    s_ap_on = true;
    ESP_LOGI(TAG, "setup hotspot \"%s\" up (open)", s_ap_ssid);
    return ESP_OK;
}

void net_wifi_ap_stop(void)
{
    if (!s_ap_on) return;
    s_ap_on = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup hotspot off");
}

bool net_wifi_ap_active(void)
{
    return s_ap_on;
}

const char *net_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

bool net_wifi_wait_time(uint32_t timeout_ms)
{
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

bool net_wifi_sta_ip(char *buf, size_t size)
{
    esp_netif_ip_info_t ip;
    if (!net_wifi_has_ip() || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) return false;
    snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
    return true;
}
