// esp_websocket_client over libcurl (see esp_websocket_client.h).
//
// One thread per client connects, reads and reconnects. libcurl's easy handle
// isn't thread safe, so every curl call holds the client's mutex; the thread
// waits for data with poll() outside it, and handlers run outside it too, so
// they can send.

#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "websocket";
const char *WEBSOCKET_EVENTS = "WEBSOCKET_EVENTS";

#define MESSAGE_LIMIT (256 * 1024)
#define IDLE_TIMEOUT_MS 60000

struct esp_websocket_client {
    char *uri;
    int reconnect_ms, network_timeout_ms;
    unsigned ping_ms;
    bool close_reconnect, auto_reconnect;
    void *user_context;

    esp_event_handler_t handler;
    void *handler_arg;

    pthread_mutex_t lock;  // guards curl and connected
    CURL *curl;
    bool connected;

    pthread_t thread;
    bool started;
    atomic_bool run;
    atomic_bool self_destroy;  // destroy() was called from the client's own thread
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit(esp_websocket_client_handle_t c, int32_t id, esp_websocket_event_data_t *d)
{
    esp_websocket_event_data_t blank = {0};
    if (!d) d = &blank;
    d->client = c;
    d->user_context = c->user_context;
    if (c->handler && atomic_load(&c->run)) c->handler(c->handler_arg, WEBSOCKET_EVENTS, id, d);
}

static void emit_error(esp_websocket_client_handle_t c, int32_t id, esp_websocket_error_type_t type, int http,
                       int sock_errno)
{
    esp_websocket_event_data_t d = {0};
    d.error_handle.error_type = type;
    d.error_handle.esp_ws_handshake_status_code = http;
    d.error_handle.esp_transport_sock_errno = sock_errno;
    d.error_handle.esp_tls_last_esp_err = type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT ? ESP_FAIL : ESP_OK;
    emit(c, id, &d);
}

// Sleeps up to ms, waking early when the client stops.
static void nap(esp_websocket_client_handle_t c, int ms)
{
    int64_t end = now_ms() + ms;
    while (atomic_load(&c->run) && now_ms() < end) {
        struct timespec ts = {0, 50 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

static int progress_cb(void *p, curl_off_t a, curl_off_t b, curl_off_t c2, curl_off_t d)
{
    (void)a, (void)b, (void)c2, (void)d;
    return atomic_load(&((esp_websocket_client_handle_t)p)->run) ? 0 : 1;
}

static void close_curl_locked(esp_websocket_client_handle_t c)
{
    if (c->curl) curl_easy_cleanup(c->curl);
    c->curl = NULL;
    c->connected = false;
}

// Connects; on failure says why in *http (the handshake's HTTP status, if any).
static bool do_connect(esp_websocket_client_handle_t c, int *http, CURLcode *rc_out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, c->uri);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)c->network_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, c);
    CURLcode rc = curl_easy_perform(curl);
    *rc_out = rc;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    *http = (int)code;
    if (rc != CURLE_OK) {
        curl_easy_cleanup(curl);
        return false;
    }
    pthread_mutex_lock(&c->lock);
    c->curl = curl;
    c->connected = true;
    pthread_mutex_unlock(&c->lock);
    return true;
}

typedef enum { READ_OK, READ_CLOSED, READ_FAILED } read_result_t;

typedef struct {
    char *buf;
    size_t len, cap;
    int op;  // of the message being assembled; 0 when none
} assembly_t;

// Reads what has arrived. Each whole message goes to the handler (with the
// lock released). Returns READ_CLOSED after the server's close frame.
static read_result_t read_available(esp_websocket_client_handle_t c, assembly_t *a, int *close_code, bool *got_data)
{
    char chunk[4096];
    for (;;) {
        size_t n = 0;
        const struct curl_ws_frame *meta = NULL;
        pthread_mutex_lock(&c->lock);
        CURLcode rc = c->curl ? curl_ws_recv(c->curl, chunk, sizeof(chunk), &n, &meta) : CURLE_RECV_ERROR;
        pthread_mutex_unlock(&c->lock);
        if (rc == CURLE_AGAIN) return READ_OK;
        if (rc == CURLE_GOT_NOTHING) return READ_CLOSED;
        if (rc != CURLE_OK || !meta) {
            ESP_LOGW(TAG, "recv: %s", curl_easy_strerror(rc));
            return READ_FAILED;
        }
        *got_data = true;
        int flags = meta->flags;
        if (flags & CURLWS_CLOSE) {
            if (meta->offset == 0 && n >= 2) *close_code = ((unsigned char)chunk[0] << 8) | (unsigned char)chunk[1];
            if (meta->bytesleft == 0) {
                esp_websocket_event_data_t d = {.data_ptr = chunk, .data_len = (int)n, .fin = true, .op_code = 0x08,
                                                .payload_len = (int)n};
                emit(c, WEBSOCKET_EVENT_DATA, &d);
                return READ_CLOSED;
            }
            continue;
        }
        if (flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl answers pings itself

        int op = (flags & CURLWS_BINARY) ? 0x02 : 0x01;
        if (!a->op) a->op = op;
        if (a->op > 0 && a->len + n <= MESSAGE_LIMIT) {
            if (a->len + n > a->cap) {
                size_t cap = a->cap ? a->cap * 2 : 8192;
                while (cap < a->len + n) cap *= 2;
                char *nb = realloc(a->buf, cap);
                if (!nb) {
                    a->op = -1;  // drop the rest of this message
                } else {
                    a->buf = nb;
                    a->cap = cap;
                }
            }
            if (a->op > 0) {
                memcpy(a->buf + a->len, chunk, n);
                a->len += n;
            }
        } else if (a->op > 0) {
            ESP_LOGW(TAG, "dropping a message over %d bytes", MESSAGE_LIMIT);
            a->op = -1;
        }
        // The message ends with its last frame's last byte: CURLWS_CONT marks
        // frames that more fragments follow.
        if (meta->bytesleft == 0 && !(flags & CURLWS_CONT)) {
            if (a->op > 0) {
                esp_websocket_event_data_t d = {.data_ptr = a->buf, .data_len = (int)a->len, .fin = true,
                                                .op_code = (uint8_t)a->op, .payload_len = (int)a->len};
                emit(c, WEBSOCKET_EVENT_DATA, &d);
            }
            a->len = 0;
            a->op = 0;
        }
    }
}

static curl_socket_t active_socket(esp_websocket_client_handle_t c)
{
    curl_socket_t fd = CURL_SOCKET_BAD;
    pthread_mutex_lock(&c->lock);
    if (c->curl) curl_easy_getinfo(c->curl, CURLINFO_ACTIVESOCKET, &fd);
    pthread_mutex_unlock(&c->lock);
    return fd;
}

static void *client_thread(void *arg)
{
    esp_websocket_client_handle_t c = arg;
    assembly_t a = {0};
    while (atomic_load(&c->run)) {
        emit(c, WEBSOCKET_EVENT_BEFORE_CONNECT, NULL);
        int http = 0;
        CURLcode crc = CURLE_OK;
        if (!do_connect(c, &http, &crc)) {
            if (!atomic_load(&c->run)) break;
            ESP_LOGW(TAG, "connect: %s (http %d)", curl_easy_strerror(crc), http);
            bool handshake = http >= 300;
            emit_error(c, WEBSOCKET_EVENT_ERROR,
                       handshake ? WEBSOCKET_ERROR_TYPE_HANDSHAKE : WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT, http, 0);
            emit_error(c, WEBSOCKET_EVENT_DISCONNECTED,
                       handshake ? WEBSOCKET_ERROR_TYPE_HANDSHAKE : WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT, http, 0);
            if (!c->auto_reconnect) break;
            nap(c, c->reconnect_ms);
            continue;
        }
        emit(c, WEBSOCKET_EVENT_CONNECTED, NULL);

        int64_t last_rx = now_ms(), last_ping = now_ms();
        int close_code = 0;
        read_result_t why = READ_OK;
        esp_websocket_error_type_t err_type = WEBSOCKET_ERROR_TYPE_NONE;
        a.len = 0;
        a.op = 0;
        while (atomic_load(&c->run)) {
            curl_socket_t fd = active_socket(c);
            if (fd == CURL_SOCKET_BAD) {
                why = READ_FAILED;
                err_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
                break;
            }
            struct pollfd p = {.fd = fd, .events = POLLIN};
            // Read even without poll() saying so: TLS may hold decrypted
            // bytes the socket no longer shows.
            poll(&p, 1, 200);
            bool got = false;
            why = read_available(c, &a, &close_code, &got);
            if (why != READ_OK) {
                err_type = why == READ_CLOSED ? WEBSOCKET_ERROR_TYPE_SERVER_CLOSE : WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
                break;
            }
            int64_t t = now_ms();
            if (got) last_rx = t;
            if (t - last_rx > IDLE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "nothing from the server for %d s", IDLE_TIMEOUT_MS / 1000);
                why = READ_FAILED;
                err_type = WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT;
                break;
            }
            if (c->ping_ms && t - last_ping >= (int64_t)c->ping_ms) {
                last_ping = t;
                size_t sent;
                pthread_mutex_lock(&c->lock);
                if (c->curl) curl_ws_send(c->curl, "", 0, &sent, 0, CURLWS_PING);
                pthread_mutex_unlock(&c->lock);
            }
        }

        pthread_mutex_lock(&c->lock);
        if (why == READ_CLOSED && c->curl) {
            size_t sent;
            unsigned char body[2] = {(unsigned char)(close_code >> 8), (unsigned char)close_code};
            curl_ws_send(c->curl, (const char *)body, close_code ? 2 : 0, &sent, 0, CURLWS_CLOSE);
        }
        close_curl_locked(c);
        pthread_mutex_unlock(&c->lock);
        if (!atomic_load(&c->run)) break;

        if (why == READ_CLOSED) {
            esp_websocket_event_data_t d = {0};
            d.error_handle.error_type = WEBSOCKET_ERROR_TYPE_SERVER_CLOSE;
            d.close_status_code = close_code;
            emit(c, WEBSOCKET_EVENT_CLOSED, &d);
            if (!c->close_reconnect) break;
        } else {
            emit_error(c, WEBSOCKET_EVENT_DISCONNECTED, err_type, 0, 0);
        }
        if (!c->auto_reconnect) break;
        nap(c, c->reconnect_ms);
    }

    free(a.buf);
    pthread_mutex_lock(&c->lock);
    close_curl_locked(c);
    pthread_mutex_unlock(&c->lock);
    if (atomic_load(&c->self_destroy)) {
        pthread_mutex_destroy(&c->lock);
        free(c->uri);
        free(c);
    }
    return NULL;
}

static void curl_init_once(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, curl_init_once);
    if (!cfg || !cfg->uri) return NULL;
    esp_websocket_client_handle_t c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->uri = strdup(cfg->uri);
    if (!c->uri) {
        free(c);
        return NULL;
    }
    c->reconnect_ms = cfg->reconnect_timeout_ms > 0 ? cfg->reconnect_timeout_ms : 10000;
    c->network_timeout_ms = cfg->network_timeout_ms > 0 ? cfg->network_timeout_ms : 10000;
    c->ping_ms = (unsigned)(cfg->ping_interval_sec ? cfg->ping_interval_sec : 10) * 1000u;
    c->close_reconnect = cfg->enable_close_reconnect;
    c->auto_reconnect = !cfg->disable_auto_reconnect;
    c->user_context = cfg->user_context;
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c, esp_websocket_event_id_t event,
                                        esp_event_handler_t handler, void *arg)
{
    (void)event;
    if (!c) return ESP_ERR_INVALID_ARG;
    c->handler = handler;
    c->handler_arg = arg;
    return ESP_OK;
}

esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (c->started) return ESP_FAIL;
    atomic_store(&c->run, true);
    if (pthread_create(&c->thread, NULL, client_thread, c) != 0) {
        atomic_store(&c->run, false);
        return ESP_FAIL;
    }
    c->started = true;
    return ESP_OK;
}

esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (!c->started) return ESP_FAIL;
    atomic_store(&c->run, false);
    if (pthread_equal(pthread_self(), c->thread)) return ESP_FAIL;
    pthread_join(c->thread, NULL);
    c->started = false;
    return ESP_OK;
}

esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    atomic_store(&c->run, false);
    if (c->started && pthread_equal(pthread_self(), c->thread)) {
        // Called from a handler: the thread frees the client as it exits.
        atomic_store(&c->self_destroy, true);
        pthread_detach(c->thread);
        return ESP_OK;
    }
    if (c->started) pthread_join(c->thread, NULL);
    pthread_mutex_destroy(&c->lock);
    free(c->uri);
    free(c);
    return ESP_OK;
}

static int send_frame(esp_websocket_client_handle_t c, const char *data, int len, TickType_t timeout,
                      unsigned flags)
{
    if (!c) return -1;
    if (len < 0) len = data ? (int)strlen(data) : 0;
    int64_t deadline = now_ms() + (timeout == portMAX_DELAY ? 3600000 : (int64_t)timeout);
    size_t done = 0;
    pthread_mutex_lock(&c->lock);
    if (!c->connected || !c->curl) {
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    for (;;) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(c->curl, data + done, (size_t)len - done, &sent, 0, flags);
        if (rc == CURLE_OK) {
            done += sent;
            if (done >= (size_t)len) break;
            continue;
        }
        if (rc != CURLE_AGAIN || now_ms() >= deadline) {
            ESP_LOGW(TAG, "send: %s", curl_easy_strerror(rc));
            pthread_mutex_unlock(&c->lock);
            return -1;
        }
        done += sent;
        curl_socket_t fd = CURL_SOCKET_BAD;
        curl_easy_getinfo(c->curl, CURLINFO_ACTIVESOCKET, &fd);
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        poll(&p, 1, 50);
    }
    pthread_mutex_unlock(&c->lock);
    return len;
}

int esp_websocket_client_send_text(esp_websocket_client_handle_t c, const char *data, int len, TickType_t timeout)
{
    return send_frame(c, data, len, timeout, CURLWS_TEXT);
}

int esp_websocket_client_send_bin(esp_websocket_client_handle_t c, const char *data, int len, TickType_t timeout)
{
    return send_frame(c, data, len, timeout, CURLWS_BINARY);
}

bool esp_websocket_client_is_connected(esp_websocket_client_handle_t c)
{
    if (!c) return false;
    pthread_mutex_lock(&c->lock);
    bool r = c->connected;
    pthread_mutex_unlock(&c->lock);
    return r;
}
