#pragma once

// The part of espressif/esp_websocket_client that zello_client.c uses, over
// libcurl's WebSocket API. Messages are delivered whole (one DATA event, fin
// set, payload_offset 0), which the client's reassembly accepts as is.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

typedef struct esp_websocket_client *esp_websocket_client_handle_t;

extern const char *WEBSOCKET_EVENTS;

typedef enum {
    WEBSOCKET_EVENT_ANY = -1,
    WEBSOCKET_EVENT_ERROR = 0,
    WEBSOCKET_EVENT_CONNECTED,
    WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_CLOSED,
    WEBSOCKET_EVENT_BEFORE_CONNECT,
    WEBSOCKET_EVENT_FINISH,
} esp_websocket_event_id_t;

typedef enum {
    WEBSOCKET_ERROR_TYPE_NONE = 0,
    WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT,
    WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT,
    WEBSOCKET_ERROR_TYPE_HANDSHAKE,
    WEBSOCKET_ERROR_TYPE_SERVER_CLOSE,
} esp_websocket_error_type_t;

typedef struct {
    esp_err_t esp_tls_last_esp_err;
    int esp_tls_stack_err;
    int esp_tls_cert_verify_flags;
    esp_websocket_error_type_t error_type;
    int esp_ws_handshake_status_code;
    int esp_transport_sock_errno;
} esp_websocket_error_codes_t;

typedef struct {
    const char *data_ptr;
    int data_len;
    bool fin;
    uint8_t op_code;
    esp_websocket_client_handle_t client;
    void *user_context;
    int payload_len;
    int payload_offset;
    esp_websocket_error_codes_t error_handle;
    int close_status_code;
} esp_websocket_event_data_t;

typedef struct {
    const char *uri;
    int buffer_size;
    int task_stack;
    int task_prio;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int reconnect_timeout_ms;
    int network_timeout_ms;
    size_t ping_interval_sec;
    bool keep_alive_enable;
    bool enable_close_reconnect;
    bool disable_auto_reconnect;
    void *user_context;
} esp_websocket_client_config_t;

esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, esp_websocket_event_id_t event,
                                        esp_event_handler_t handler, void *arg);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int len,
                                   TickType_t timeout);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data, int len,
                                  TickType_t timeout);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client);
