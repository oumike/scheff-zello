#include "console_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_console.h"
#include "audio.h"
#include "display.h"
#include "channel_list.h"
#include "crash_report.h"
#include "net_wifi.h"
#include "settings.h"
#include "zello_client.h"

// Join argv[from..] with spaces. A channel name or a text message can contain
// them, and the REPL splits on whitespace.
static void join(char *out, size_t len, int argc, char **argv, int from)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) strlcat(out, " ", len);
        strlcat(out, argv[i], len);
    }
}

static int on_off(const char *s, bool cur)
{
    if (!s) return !cur;
    return !strcasecmp(s, "on") || !strcmp(s, "1");
}

static int cmd_status(int argc, char **argv)
{
    static const char *const states[] = {"offline", "connecting", "signing in", "online", "failed"};
    zello_status_t st;
    zello_get_status(&st);
    net_wifi_print_status();
    printf("Zello: %s as %s on %s\n", states[st.state],
           g_settings.username[0] ? g_settings.username : "(no username)",
           g_settings.network == ZELLO_NET_WORK ? g_settings.work_network : "zello.io");
    printf("Channel: %s", g_settings.channel[0] ? g_settings.channel : "(none)");
    if (st.state == ZELLO_ONLINE) printf("  -  %lu listening", (unsigned long)st.users_online);
    printf("  %s  rx %lu / tx %lu packets\n",
           st.ptt ? "TX" : st.receiving ? "RX" : "--", (unsigned long)st.rx_packets, (unsigned long)st.tx_packets);
    if (st.error[0]) printf("Last error: %s\n", st.error);
    printf("Saved channels: %u\n", (unsigned)channel_list_count());
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        settings_print();
        return 0;
    }
    char value[SETTINGS_TOKEN_MAX];
    join(value, sizeof(value), argc, argv, 2);
    esp_err_t err = settings_set(argv[1], value);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("Unknown key '%s'. Keys:\n", argv[1]);
        settings_print();
        return 1;
    }
    if (err != ESP_OK) {
        printf("Invalid value\n");
        return 1;
    }
    settings_save();
    if (!strcmp(argv[1], "volume")) audio_set_volume(g_settings.volume);
    if (!strcmp(argv[1], "mic_gain")) audio_set_mic_gain(g_settings.mic_gain);
    if (!strncmp(argv[1], "wifi_", 5)) printf("Saved. Run 'wifi' to reconnect.\n");
    if (!strcmp(argv[1], "username") || !strcmp(argv[1], "password") || !strcmp(argv[1], "auth_token") ||
        !strcmp(argv[1], "network") || !strcmp(argv[1], "work_network")) {
        printf("Saved. Run 'login' to sign in again.\n");
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2) {
        strlcpy(g_settings.wifi_ssid, argv[1], sizeof(g_settings.wifi_ssid));
        strlcpy(g_settings.wifi_pass, argc >= 3 ? argv[2] : "", sizeof(g_settings.wifi_pass));
        settings_save();
    }
    if (!g_settings.wifi_ssid[0]) {
        net_wifi_print_status();
        printf("Usage: wifi <ssid> [password]\n");
        return 1;
    }
    return net_wifi_connect(g_settings.wifi_ssid, g_settings.wifi_pass) == ESP_OK ? 0 : 1;
}

static int cmd_login(int argc, char **argv)
{
    return zello_connect() == ESP_OK ? 0 : 1;
}

static int cmd_logout(int argc, char **argv)
{
    zello_disconnect();
    return 0;
}

static int cmd_channel(int argc, char **argv)
{
    if (argc < 2) {
        printf("Channel: %s\n", g_settings.channel[0] ? g_settings.channel : "(none)");
        printf("Usage: channel <name>\n");
        return 0;
    }
    char name[sizeof(g_settings.channel)];
    join(name, sizeof(name), argc, argv, 1);
    return zello_set_channel(name) == ESP_OK ? 0 : 1;
}

static int cmd_channels(int argc, char **argv)
{
    channel_entry_t *list = calloc(CHANNEL_LIST_MAX, sizeof(*list));
    if (!list) return 1;
    size_t n = channel_list_get(argc >= 2 ? argv[1] : NULL, false, list, CHANNEL_LIST_MAX);
    for (size_t i = 0; i < n; i++) {
        printf("  %c%c %-40s %s\n", list[i].favorite ? '*' : ' ',
               !strcmp(list[i].name, g_settings.channel) ? '>' : ' ', list[i].name, list[i].desc);
    }
    printf("%u channel%s\n", (unsigned)n, n == 1 ? "" : "s");
    free(list);
    return 0;
}

static int cmd_ptt(int argc, char **argv)
{
    zello_set_ptt(on_off(argc >= 2 ? argv[1] : NULL, zello_ptt()));
    return 0;
}

static int cmd_text(int argc, char **argv)
{
    char text[257];
    join(text, sizeof(text), argc, argv, 1);
    if (zello_send_text(text) != ESP_OK) {
        printf("Not signed in to a channel\n");
        return 1;
    }
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    char *log = malloc(ZELLO_LOG_MAX);
    if (!log) return 1;
    zello_get_log(log, ZELLO_LOG_MAX);
    printf("%s\n", log);
    free(log);
    return 0;
}

static int cmd_dsipattern(int argc, char **argv)
{
    bool on = on_off(argc >= 2 ? argv[1] : NULL, display_test_pattern());
    esp_err_t err = display_set_test_pattern(on);
    if (err != ESP_OK) {
        printf("Couldn't switch the test pattern: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("DSI test pattern %s\n", on ? "on: colour bars, no frame buffer reads" : "off");
    return 0;
}

static int cmd_loopback(int argc, char **argv)
{
    bool on = on_off(argc >= 2 ? argv[1] : NULL, audio_loopback());
    audio_set_loopback(on);
    printf("Mic->speaker loopback %s\n", on ? "on (keep the volume low to avoid howl)" : "off");
    return 0;
}

static int cmd_crash(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "test")) {
        printf("Crashing on purpose; a report goes to the microSD card after the reboot.\n");
        fflush(stdout);
        abort();
    }
    crash_report_print_status();
    return 0;
}

static void reg(const char *cmd, const char *help, const char *hint, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = {.command = cmd, .help = help, .hint = hint, .func = fn};
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "scheff>";
    // A developer token pasted into "set auth_token ..." is the longest line
    // anyone will type here.
    rc.max_cmdline_length = SETTINGS_TOKEN_MAX + 64;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &rc, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#else
    printf("No console transport configured\n");
    return;
#endif

    esp_console_register_help_command();
    reg("status", "Show Wi-Fi and Zello status", NULL, cmd_status);
    reg("set", "Show settings, or set one and save to flash", "[<key> <value>]", cmd_set);
    reg("wifi", "Connect to Wi-Fi (saves credentials)", "[<ssid> [<password>]]", cmd_wifi);
    reg("login", "Sign in to Zello now", NULL, cmd_login);
    reg("logout", "Disconnect from Zello", NULL, cmd_logout);
    reg("channel", "Show or join a channel", "[<name>]", cmd_channel);
    reg("channels", "List the saved channels", "[<filter>]", cmd_channels);
    reg("ptt", "Toggle transmit (or hold the BOOT button)", "[on|off]", cmd_ptt);
    reg("text", "Send a text message to the channel", "<text>", cmd_text);
    reg("log", "Show recent channel activity", NULL, cmd_log);
    reg("loopback", "Mic to speaker loopback test", "[on|off]", cmd_loopback);
    reg("crash", "Show the last reset and crash reports; 'crash test' crashes on purpose", "[test]", cmd_crash);
    reg("dsipattern", "Display test: colour bars from the DSI controller, no PSRAM reads", "[on|off]",
        cmd_dsipattern);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
