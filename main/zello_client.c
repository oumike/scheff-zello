#include "zello_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "audio.h"
#include "channel_list.h"
#include "crash_report.h"
#include "opus_codec.h"
#include "settings.h"

static const char *TAG = "zello";

// Zello's binary frame: a one-byte type, then two big-endian 32-bit fields.
#define PKT_TYPE_AUDIO  0x01
#define PKT_HEADER_LEN  9

// A 60 ms Opus voice packet is a couple of hundred bytes; this leaves room for
// a talker using a longer frame or a much higher bitrate.
#define PACKET_MAX      1024
#define TX_QUEUE_DEPTH  4
#define RX_QUEUE_DEPTH  12

// Decoded audio can be up to 120 ms per packet.
#define DECODE_MAX      (AUDIO_RATE / 1000 * 120)

// Largest WebSocket message we reassemble. The logon we send is the biggest
// thing in either direction (a developer token is a long JWT).
#define MESSAGE_MAX     4096

#define WS_BUFFER_SIZE  2048
#define WS_TASK_STACK   8192
// libopus is built with VAR_ARRAYS, so its scratch buffers live on the calling
// task's stack and how deep it goes depends on the audio. The encoder
// overflowed 16 KB and was measured at ~25 KB; the decoder peaks near 4 KB.
// Both come out of internal RAM, which TLS also needs.
#define TX_TASK_STACK   32768
#define RX_TASK_STACK   16384
#define SEND_TIMEOUT_MS 400

// How long a start_stream may go unanswered before we give up on the press.
#define STREAM_START_TIMEOUT_MS 3000
// Incoming audio with no packets for this long counts as "stopped talking",
// in case on_stream_stop never arrives.
#define RX_IDLE_TIMEOUT_MS 1500
// How often the worker task re-checks those two timeouts.
#define TICK_MS 250

#define LOG_LINES    20
#define LOG_LINE_MAX 96

typedef struct {
    int16_t pcm[AUDIO_FRAME];
} tx_frame_t;

typedef struct {
    uint16_t len;
    uint8_t data[PACKET_MAX];
} rx_packet_t;

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_lock;        // guards s_status, s_log and the seq/stream fields
static SemaphoreHandle_t s_conn_lock;   // serializes connect/disconnect
// Held around every use of s_ws, so tearing the client down cannot free it
// under a send that is already in flight on another task.
static SemaphoreHandle_t s_send_lock;
static QueueHandle_t s_tx_q, s_rx_q;
static void (*s_changed_cb)(void);

static zello_status_t s_status;
static int s_seq = 1;
static int s_logon_seq;
static int s_stream_seq;
static uint32_t s_tx_stream;   // stream we transmit on, 0 when none
static uint32_t s_tx_packet;   // packet_id within it, counts from 1
static uint32_t s_rx_stream;
static int64_t s_stream_start_us;
static int64_t s_last_rx_us;
static volatile bool s_ptt_wanted;
static volatile bool s_want_connect;
static volatile bool s_stop_requested;  // auth failure: the worker closes the socket

static char s_log[LOG_LINES][LOG_LINE_MAX];
static size_t s_log_count;  // total lines ever added; the ring holds the last LOG_LINES

// Reassembly of fragmented messages.
static uint8_t *s_asm;
static size_t s_asm_len;
static int s_asm_op;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void notify(void)
{
    if (s_changed_cb) s_changed_cb();
}

// The only path that touches s_ws to send. Returns false if there is no client
// or the write failed.
static bool ws_send(bool text, const char *data, int len)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int sent = -1;
    if (s_ws) {
        sent = text ? esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(SEND_TIMEOUT_MS))
                    : esp_websocket_client_send_bin(s_ws, data, len, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    }
    xSemaphoreGive(s_send_lock);
    return sent >= 0;
}

// ---- log -------------------------------------------------------------------

static void log_line_locked(const char *text)
{
    strlcpy(s_log[s_log_count % LOG_LINES], text, LOG_LINE_MAX);
    s_log_count++;
}

static void log_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_add(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    lock();
    log_line_locked(line);
    unlock();
    ESP_LOGI(TAG, "%s", line);
    notify();
}

void zello_get_log(char *out, size_t size)
{
    out[0] = 0;
    lock();
    size_t first = s_log_count > LOG_LINES ? s_log_count - LOG_LINES : 0;
    for (size_t i = first; i < s_log_count; i++) {
        if (i > first) strlcat(out, "\n", size);
        strlcat(out, s_log[i % LOG_LINES], size);
    }
    unlock();
}

// ---- status ----------------------------------------------------------------

void zello_get_status(zello_status_t *out)
{
    lock();
    *out = s_status;
    unlock();
}

zello_state_t zello_state(void)
{
    lock();
    zello_state_t st = s_status.state;
    unlock();
    return st;
}

static void set_state(zello_state_t state, const char *error)
{
    lock();
    s_status.state = state;
    if (error) strlcpy(s_status.error, error, sizeof(s_status.error));
    if (state != ZELLO_ONLINE) {
        s_status.users_online = 0;
        s_status.receiving = false;
        s_status.talker[0] = 0;
    }
    if (state != ZELLO_ONLINE && state != ZELLO_LOGGING_IN) {
        s_status.ptt = s_status.starting = false;
        s_tx_stream = 0;
    }
    unlock();
    notify();
}

bool zello_ptt(void)
{
    lock();
    bool ptt = s_status.ptt || s_status.starting;
    unlock();
    return ptt;
}

// ---- sending ---------------------------------------------------------------

// Allocates the next seq, optionally recording it in `slot` (the field that
// matches the reply to the command) before the command is on the wire, so a
// fast answer cannot arrive before we know what it answers.
static int next_seq(int *slot)
{
    lock();
    int seq = s_seq++;
    if (slot) *slot = seq;
    unlock();
    return seq;
}

// Sends `root`, which it takes ownership of, with `seq` attached. Never called
// with s_lock held: the write can block for the whole send timeout, and the UI
// reads the status from the LVGL task.
static bool send_command(cJSON *root, int seq)
{
    cJSON_AddNumberToObject(root, "seq", seq);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return false;
    bool ok = ws_send(true, text, strlen(text));
    if (!ok) ESP_LOGW(TAG, "could not send a command");
    free(text);
    return ok;
}

static void send_logon(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", "logon");
    if (g_settings.auth_token[0]) cJSON_AddStringToObject(root, "auth_token", g_settings.auth_token);
    cJSON_AddStringToObject(root, "username", g_settings.username);
    cJSON_AddStringToObject(root, "password", g_settings.password);
    cJSON_AddStringToObject(root, "channel", g_settings.channel);

    lock();
    strlcpy(s_status.channel, g_settings.channel, sizeof(s_status.channel));
    unlock();

    bool ok = send_command(root, next_seq(&s_logon_seq));
    if (ok) {
        set_state(ZELLO_LOGGING_IN, NULL);
        log_add("Logging in as %s", g_settings.username);
    } else {
        set_state(ZELLO_FAILED, "Couldn't send the logon.");
    }
}

static void send_start_stream(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", "start_stream");
    cJSON_AddStringToObject(root, "type", "audio");
    cJSON_AddStringToObject(root, "codec", "opus");
    cJSON_AddStringToObject(root, "codec_header", opus_codec_header_base64());
    cJSON_AddNumberToObject(root, "packet_duration", AUDIO_FRAME_MS);

    int seq = next_seq(&s_stream_seq);
    lock();
    s_status.starting = true;
    s_stream_start_us = esp_timer_get_time();
    unlock();
    notify();

    if (!send_command(root, seq)) {
        s_ptt_wanted = false;
        lock();
        s_status.starting = false;
        unlock();
        notify();
    }
}

static void send_stop_stream(uint32_t stream_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", "stop_stream");
    cJSON_AddNumberToObject(root, "stream_id", stream_id);
    send_command(root, next_seq(NULL));
}

esp_err_t zello_send_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (zello_state() != ZELLO_ONLINE) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "command", "send_text_message");
    cJSON_AddStringToObject(root, "channel", g_settings.channel);
    cJSON_AddStringToObject(root, "text", text);
    bool ok = send_command(root, next_seq(NULL));
    if (ok) log_add("You: %s", text);
    return ok ? ESP_OK : ESP_FAIL;
}

// ---- transmit --------------------------------------------------------------

void zello_set_ptt(bool on)
{
    if (on) {
        if (zello_state() != ZELLO_ONLINE) {
            ESP_LOGW(TAG, "talk ignored: not signed in to a channel");
            return;
        }
        lock();
        bool busy = s_status.receiving;
        bool already = s_status.ptt || s_status.starting;
        unlock();
        // Half duplex: Zello would reject the stream anyway while a talker
        // holds the channel.
        if (busy || already) {
            ESP_LOGI(TAG, "talk ignored: %s", busy ? "someone else is talking" : "already transmitting");
            return;
        }
        s_ptt_wanted = true;
        send_start_stream();
        return;
    }

    s_ptt_wanted = false;
    lock();
    uint32_t stream = s_tx_stream;
    bool was = s_status.ptt;
    s_status.ptt = s_status.starting = false;
    s_tx_stream = 0;
    unlock();
    if (stream) send_stop_stream(stream);
    if (was) {
        xQueueReset(s_tx_q);
        notify();
    }
}

void zello_mic_frame(const int16_t *pcm, size_t samples)
{
    if (!s_ptt_wanted || samples != AUDIO_FRAME) return;
    lock();
    bool live = s_status.ptt && s_tx_stream;
    unlock();
    if (!live) return;
    // Never block the capture task: a frame we can't queue is a frame the
    // network is too far behind to use anyway.
    xQueueSend(s_tx_q, pcm, 0);
}

// Logs each new low in a codec task's stack headroom, so the margin above
// libopus's deepest path shows up in the log before it runs out.
static void log_stack_low(UBaseType_t *low)
{
    UBaseType_t free = uxTaskGetStackHighWaterMark(NULL);
    if (free < *low) {
        *low = free;
        ESP_LOGI(TAG, "%s stack headroom %u bytes", pcTaskGetName(NULL), (unsigned)free);
    }
}

static void tx_task(void *arg)
{
    tx_frame_t *frame = malloc(sizeof(*frame));
    uint8_t *packet = malloc(PKT_HEADER_LEN + PACKET_MAX);
    UBaseType_t stack_low = TX_TASK_STACK;
    if (!frame || !packet) {
        ESP_LOGE(TAG, "no memory for the transmit path");
        vTaskDelete(NULL);
    }
    for (;;) {
        if (xQueueReceive(s_tx_q, frame, portMAX_DELAY) != pdTRUE) continue;

        lock();
        uint32_t stream = s_status.ptt ? s_tx_stream : 0;
        uint32_t id = s_tx_packet;
        if (stream) s_tx_packet++;
        unlock();
        if (!stream) continue;  // released while this frame was queued

        int len = opus_codec_encode(frame->pcm, packet + PKT_HEADER_LEN, PACKET_MAX);
        log_stack_low(&stack_low);
        if (len <= 0) {
            ESP_LOGW(TAG, "encode failed (%d)", len);
            continue;
        }
        packet[0] = PKT_TYPE_AUDIO;
        packet[1] = stream >> 24; packet[2] = stream >> 16; packet[3] = stream >> 8; packet[4] = stream;
        packet[5] = id >> 24; packet[6] = id >> 16; packet[7] = id >> 8; packet[8] = id;

        if (!ws_send(false, (const char *)packet, PKT_HEADER_LEN + len)) {
            ESP_LOGW(TAG, "dropped a voice packet");
            continue;
        }
        lock();
        s_status.tx_packets++;
        unlock();
    }
}

// ---- receive ---------------------------------------------------------------

static void rx_task(void *arg)
{
    rx_packet_t *packet = malloc(sizeof(*packet));
    int16_t *pcm = malloc(DECODE_MAX * sizeof(int16_t));
    UBaseType_t stack_low = RX_TASK_STACK;
    if (!packet || !pcm) {
        ESP_LOGE(TAG, "no memory for the receive path");
        vTaskDelete(NULL);
    }
    for (;;) {
        if (xQueueReceive(s_rx_q, packet, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            int samples = opus_codec_decode(packet->data, packet->len, pcm, DECODE_MAX);
            log_stack_low(&stack_low);
            if (samples > 0) {
                audio_play_push(pcm, samples);
            } else {
                ESP_LOGW(TAG, "decode failed (%d)", samples);
            }
            continue;
        }

        // Idle tick: time out a talker who stopped without saying so, a
        // start_stream that was never answered, and close a rejected session.
        int64_t now = esp_timer_get_time();
        bool changed = false;
        lock();
        if (s_status.receiving && now - s_last_rx_us > RX_IDLE_TIMEOUT_MS * 1000) {
            s_status.receiving = false;
            s_status.talker[0] = 0;
            s_rx_stream = 0;
            changed = true;
        }
        bool timed_out = s_status.starting && now - s_stream_start_us > STREAM_START_TIMEOUT_MS * 1000;
        if (timed_out) {
            s_status.starting = false;
            changed = true;
        }
        unlock();
        if (timed_out) log_add("The channel didn't answer the transmit request.");
        if (changed) notify();

        if (s_stop_requested) {
            s_stop_requested = false;
            xSemaphoreTake(s_conn_lock, portMAX_DELAY);
            if (s_ws) esp_websocket_client_stop(s_ws);
            xSemaphoreGive(s_conn_lock);
        }
    }
}

// ---- incoming messages -----------------------------------------------------

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static uint32_t json_u32(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) && v->valuedouble > 0 ? (uint32_t)v->valuedouble : 0;
}

// A reply to one of our commands: no "command", just our seq.
static void handle_response(int seq, cJSON *j)
{
    const char *error = json_str(j, "error");
    bool success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "success"));

    lock();
    bool is_logon = seq == s_logon_seq;
    bool is_stream = seq == s_stream_seq;
    unlock();

    if (is_logon) {
        if (success) {
            // The channel still has to come online; on_channel_status does that.
            log_add("Logged in.");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Zello rejected the logon: %s", error[0] ? error : "unknown reason");
            // Don't reconnect in a loop on bad credentials or a stale token.
            s_want_connect = false;
            s_stop_requested = true;
            set_state(ZELLO_FAILED, msg);
            log_add("%s", msg);
        }
        return;
    }

    if (is_stream) {
        uint32_t stream = json_u32(j, "stream_id");
        if (success && stream) {
            lock();
            s_tx_stream = stream;
            s_tx_packet = 1;
            s_status.starting = false;
            s_status.ptt = s_ptt_wanted;
            unlock();
            // Released before the server answered: close it straight away.
            if (!s_ptt_wanted) {
                send_stop_stream(stream);
                lock();
                s_tx_stream = 0;
                unlock();
            }
            notify();
        } else {
            s_ptt_wanted = false;
            lock();
            s_status.starting = false;
            s_status.ptt = false;
            unlock();
            log_add("Can't talk right now: %s", error[0] ? error : "the channel refused the stream");
        }
    }
}

static void handle_event(const char *command, cJSON *j)
{
    if (!strcmp(command, "on_channel_status")) {
        const char *channel = json_str(j, "channel");
        const char *status = json_str(j, "status");
        uint32_t users = json_u32(j, "users_online");
        bool online = !strcmp(status, "online");

        lock();
        if (channel[0]) strlcpy(s_status.channel, channel, sizeof(s_status.channel));
        s_status.users_online = users;
        unlock();

        if (online) {
            set_state(ZELLO_ONLINE, "");
            log_add("%s is online (%lu listening)", channel, (unsigned long)users);
            channel_list_record_join(channel);
        } else {
            set_state(ZELLO_LOGGING_IN, NULL);
            log_add("%s is offline", channel);
        }
        return;
    }

    if (!strcmp(command, "on_stream_start")) {
        uint32_t stream = json_u32(j, "stream_id");
        const char *from = json_str(j, "from");
        opus_codec_reset_decoder();
        lock();
        s_rx_stream = stream;
        s_status.receiving = true;
        strlcpy(s_status.talker, from[0] ? from : "someone", sizeof(s_status.talker));
        s_last_rx_us = esp_timer_get_time();
        unlock();
        log_add("%s is talking", from[0] ? from : "Someone");
        notify();
        return;
    }

    if (!strcmp(command, "on_stream_stop")) {
        uint32_t stream = json_u32(j, "stream_id");
        lock();
        bool ours = stream == s_rx_stream;
        if (ours) {
            s_status.receiving = false;
            s_status.talker[0] = 0;
            s_rx_stream = 0;
        }
        unlock();
        if (ours) notify();
        return;
    }

    if (!strcmp(command, "on_text_message")) {
        const char *from = json_str(j, "from");
        const char *text = json_str(j, "text");
        log_add("%s: %s", from[0] ? from : "?", text);
        return;
    }

    if (!strcmp(command, "on_error")) {
        const char *error = json_str(j, "error");
        lock();
        strlcpy(s_status.error, error, sizeof(s_status.error));
        unlock();
        log_add("Error: %s", error);
        return;
    }

    if (!strcmp(command, "on_image")) {
        log_add("%s sent a picture (not shown)", json_str(j, "from"));
        return;
    }
    if (!strcmp(command, "on_location")) {
        log_add("%s shared a location", json_str(j, "from"));
        return;
    }
}

static void handle_text(const char *text, size_t len)
{
    cJSON *j = cJSON_ParseWithLength(text, len);
    if (!j) {
        ESP_LOGW(TAG, "unparseable message (%u bytes)", (unsigned)len);
        return;
    }
    cJSON *command = cJSON_GetObjectItemCaseSensitive(j, "command");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(j, "seq");
    if (cJSON_IsString(command)) {
        handle_event(command->valuestring, j);
    } else if (cJSON_IsNumber(seq)) {
        handle_response((int)seq->valuedouble, j);
    }
    cJSON_Delete(j);
}

static void handle_binary(const uint8_t *data, size_t len)
{
    if (len <= PKT_HEADER_LEN || data[0] != PKT_TYPE_AUDIO) return;  // images use other types
    uint32_t stream = ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 8) | data[4];

    size_t payload = len - PKT_HEADER_LEN;
    if (payload > PACKET_MAX) return;

    bool new_talker = false;
    lock();
    // Audio can beat on_stream_start here; take the stream as read so the
    // first words aren't dropped.
    if (stream != s_rx_stream) {
        s_rx_stream = stream;
        new_talker = !s_status.receiving;
    }
    s_status.receiving = true;
    s_status.rx_packets++;
    s_last_rx_us = esp_timer_get_time();
    unlock();
    if (new_talker) notify();

    static rx_packet_t packet;  // only ever touched on the WebSocket task
    packet.len = payload;
    memcpy(packet.data, data + PKT_HEADER_LEN, payload);
    if (xQueueSend(s_rx_q, &packet, 0) != pdTRUE) ESP_LOGW(TAG, "receive queue full, dropping a packet");
}

// Why the socket went away, for the log and the disconnect reports.
static void log_ws_error(const char *what, const esp_websocket_event_data_t *d)
{
    static const char *const types[] = {"none", "TCP/TLS transport", "pong timeout", "handshake", "server close"};
    const esp_websocket_error_codes_t *e = &d->error_handle;
    unsigned t = e->error_type;
    ESP_LOGW(TAG, "%s: %s, esp-tls %s (0x%x), tls stack 0x%x, socket errno %d, http %d", what,
             t < sizeof(types) / sizeof(types[0]) ? types[t] : "?", esp_err_to_name(e->esp_tls_last_esp_err),
             e->esp_tls_last_esp_err, e->esp_tls_stack_err, e->esp_transport_sock_errno,
             e->esp_ws_handshake_status_code);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        log_add("Connected to Zello.");
        send_logon();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED: {
        zello_state_t was = zello_state();
        log_ws_error("disconnected", d);
        if (was != ZELLO_FAILED) {
            set_state(s_want_connect ? ZELLO_CONNECTING : ZELLO_OFFLINE, NULL);
            if (s_want_connect) log_add("Connection lost, reconnecting...");
        }
        // Only a session that was up: retries while Wi-Fi is down land here too.
        if (s_want_connect && (was == ZELLO_ONLINE || was == ZELLO_LOGGING_IN)) {
            crash_report_disconnect("connection lost");
        }
        break;
    }

    case WEBSOCKET_EVENT_CLOSED:
        // The client does not reconnect after a server-initiated close.
        log_ws_error("closed by the server", d);
        ESP_LOGW(TAG, "close code %d", d->close_status_code);
        if (s_want_connect) crash_report_disconnect("server closed the connection");
        break;

    case WEBSOCKET_EVENT_ERROR:
        log_ws_error("error", d);
        if (zello_state() != ZELLO_FAILED) set_state(ZELLO_CONNECTING, "Couldn't reach Zello.");
        break;

    case WEBSOCKET_EVENT_DATA: {
        int op = d->op_code & 0x0f;
        if (op == 0x08) {
            const uint8_t *p = (const uint8_t *)d->data_ptr;
            int code = d->data_len >= 2 ? (p[0] << 8) | p[1] : 0;
            ESP_LOGW(TAG, "server sent close, code %d: %.*s", code, d->data_len > 2 ? d->data_len - 2 : 0,
                     d->data_len > 2 ? d->data_ptr + 2 : "");
        }
        if (op == 0x08 || op == 0x09 || op == 0x0a) break;  // close, ping, pong

        const uint8_t *msg = (const uint8_t *)d->data_ptr;
        size_t len = d->data_len;
        if (d->payload_offset != 0 || (size_t)d->payload_len != len) {
            // A fragmented message: continuation frames carry op code 0, so
            // the first fragment's op code is the one that matters.
            if (d->payload_offset == 0) {
                s_asm_len = 0;
                s_asm_op = op;
            }
            if (s_asm_len + d->data_len <= MESSAGE_MAX) {
                memcpy(s_asm + s_asm_len, d->data_ptr, d->data_len);
                s_asm_len += d->data_len;
            } else {
                ESP_LOGW(TAG, "dropping an oversized message (%d bytes)", d->payload_len);
                s_asm_len = MESSAGE_MAX + 1;
            }
            if (s_asm_len < (size_t)d->payload_len) break;  // more fragments to come
            if (s_asm_len > MESSAGE_MAX) break;
            msg = s_asm;
            len = s_asm_len;
            op = s_asm_op;
        }
        if (op == 0x01) handle_text((const char *)msg, len);
        else if (op == 0x02) handle_binary(msg, len);
        break;
    }

    default:
        break;
    }
}

// ---- lifecycle -------------------------------------------------------------

esp_err_t zello_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_conn_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
#if CONFIG_IDF_TARGET_ESP32S3
    // The S3's internal RAM is too small to hold these alongside Wi-Fi and
    // TLS (FreeRTOS objects otherwise always land there): queues and codec
    // stacks go to PSRAM. Neither codec task writes flash, which a task with
    // a PSRAM stack must never do.
    s_tx_q = xQueueCreateWithCaps(TX_QUEUE_DEPTH, sizeof(tx_frame_t), MALLOC_CAP_SPIRAM);
    s_rx_q = xQueueCreateWithCaps(RX_QUEUE_DEPTH, sizeof(rx_packet_t), MALLOC_CAP_SPIRAM);
#else
    s_tx_q = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_frame_t));
    s_rx_q = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_packet_t));
#endif
    s_asm = malloc(MESSAGE_MAX);
    if (!s_lock || !s_conn_lock || !s_send_lock || !s_tx_q || !s_rx_q || !s_asm) return ESP_ERR_NO_MEM;

    esp_err_t err = opus_codec_init();
    if (err != ESP_OK) return err;

    // Opus needs tens of kilobytes of stack of its own, so both codec paths get
    // their own task rather than running on the capture or socket tasks.
#if CONFIG_IDF_TARGET_ESP32S3
    if (xTaskCreateWithCaps(tx_task, "zello_tx", TX_TASK_STACK, NULL, 18, NULL, MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreateWithCaps(rx_task, "zello_rx", RX_TASK_STACK, NULL, 17, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#else
    if (xTaskCreate(tx_task, "zello_tx", TX_TASK_STACK, NULL, 18, NULL) != pdPASS ||
        xTaskCreate(rx_task, "zello_rx", RX_TASK_STACK, NULL, 17, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

// True when there is enough configured to attempt a logon.
static bool account_ready(const char **why)
{
    if (!g_settings.username[0] || !g_settings.password[0]) {
        if (why) *why = "No Zello account set up yet.";
        return false;
    }
    if (g_settings.network == ZELLO_NET_CONSUMER && !g_settings.auth_token[0]) {
        if (why) *why = "The consumer network needs a developer token.";
        return false;
    }
    if (g_settings.network == ZELLO_NET_WORK && !g_settings.work_network[0]) {
        if (why) *why = "Enter your Zello Work network name.";
        return false;
    }
    if (!g_settings.channel[0]) {
        if (why) *why = "Pick a channel to join.";
        return false;
    }
    return true;
}

// Tears the session down. Call with s_conn_lock held.
static void disconnect_locked(void)
{
    s_want_connect = false;
    s_ptt_wanted = false;
    if (s_ws) {
        // Take the send lock first so no task is inside a write on the handle
        // we are about to free.
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        esp_websocket_client_handle_t ws = s_ws;
        s_ws = NULL;
        xSemaphoreGive(s_send_lock);
        esp_websocket_client_destroy(ws);
    }
    xQueueReset(s_tx_q);
    xQueueReset(s_rx_q);
    audio_play_flush();
}

void zello_disconnect(void)
{
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    disconnect_locked();
    xSemaphoreGive(s_conn_lock);
    set_state(ZELLO_OFFLINE, NULL);
}

esp_err_t zello_connect(void)
{
    const char *why = NULL;
    if (!account_ready(&why)) {
        set_state(ZELLO_OFFLINE, why);
        return ESP_ERR_INVALID_ARG;
    }

    char url[160];
    settings_ws_url(url, sizeof(url));

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = WS_BUFFER_SIZE,
        .task_stack = WS_TASK_STACK,
        .task_prio = 6,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        // Zello closes a silent socket; the built-in ping keeps it alive
        // between transmissions.
        .ping_interval_sec = 25,
        .keep_alive_enable = true,
    };

    // One lock across both halves: two reconnects racing here would otherwise
    // each tear down the other's client.
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    disconnect_locked();
    s_ws = esp_websocket_client_init(&cfg);
    esp_err_t err = s_ws ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    if (err == ESP_OK) {
        s_want_connect = true;
        err = esp_websocket_client_start(s_ws);
    }
    if (err != ESP_OK) disconnect_locked();
    xSemaphoreGive(s_conn_lock);

    if (err != ESP_OK) {
        set_state(ZELLO_FAILED, "Couldn't open the connection.");
        return err;
    }
    set_state(ZELLO_CONNECTING, "");
    log_add("Connecting to %s", url);
    return ESP_OK;
}

static void reconfigure_task(void *arg)
{
    if (account_ready(NULL)) {
        zello_connect();
    } else {
        zello_disconnect();
    }
    vTaskDelete(NULL);
}

void zello_reconfigure(void)
{
    // Closing the old session waits for the WebSocket task to finish, which can
    // take a moment. Every caller here is an LVGL callback or an HTTP handler,
    // so hand it to a worker rather than stalling the screen or the request.
    if (xTaskCreate(reconfigure_task, "zello_reconf", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the reconnect");
    }
}

esp_err_t zello_set_channel(const char *name)
{
    if (name && name[0]) {
        strlcpy(g_settings.channel, name, sizeof(g_settings.channel));
        esp_err_t err = settings_save();
        if (err != ESP_OK) return err;
    }
    zello_reconfigure();
    return ESP_OK;
}

void zello_set_changed_cb(void (*cb)(void))
{
    s_changed_cb = cb;
}
