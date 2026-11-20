// The network belongs to the OS (NetworkManager on the CardputerZero): the
// app only reports the address it has and waits for the clock.

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"
#include "net_wifi.h"

static const char *TAG = "net";

esp_err_t net_wifi_init(void) { return ESP_OK; }

esp_err_t net_wifi_connect(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    return ESP_OK;
}

bool net_wifi_sta_ip(char *buf, size_t size)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (struct ifaddrs *a = list; a && !found; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip))) continue;
        if (buf && size) snprintf(buf, size, "%s", ip);
        found = true;
    }
    freeifaddrs(list);
    return found;
}

bool net_wifi_has_ip(void) { return net_wifi_sta_ip(NULL, 0); }

bool net_wifi_wait_ip(uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited += 250) {
        if (net_wifi_has_ip()) return true;
        if (waited >= timeout_ms) return false;
        usleep(250 * 1000);
    }
}

bool net_wifi_wait_time(uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited += 250) {
        if (time(NULL) > 1577836800) return true;  // past 2020
        if (waited >= timeout_ms) return false;
        usleep(250 * 1000);
    }
}

int net_wifi_scan(net_wifi_ap_t *out, int max)
{
    (void)out;
    (void)max;
    return -1;
}

net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    (void)ssid;
    (void)pass;
    (void)timeout_ms;
    return NET_JOIN_FAILED;
}

esp_err_t net_wifi_ap_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void net_wifi_ap_stop(void) {}
bool net_wifi_ap_active(void) { return false; }
const char *net_wifi_ap_ssid(void) { return ""; }

void net_wifi_print_status(void)
{
    char ip[INET_ADDRSTRLEN];
    if (net_wifi_sta_ip(ip, sizeof(ip))) ESP_LOGI(TAG, "network up, %s", ip);
    else ESP_LOGI(TAG, "no network");
}
