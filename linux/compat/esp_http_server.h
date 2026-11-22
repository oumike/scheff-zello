#pragma once

// The part of ESP-IDF's esp_http_server the web config uses, over sockets:
// one server thread handles one request at a time, as on the device, and
// every response closes its connection.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

#define HTTPD_MAX_URI_LEN 512
#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_RESP_USE_STRLEN  -1

#define ESP_ERR_HTTPD_BASE         0xb000
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 3)

typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4 } httpd_method_t;

typedef struct httpd_server *httpd_handle_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[HTTPD_MAX_URI_LEN + 1];
    size_t content_len;
    void *aux;
    void *user_ctx;
} httpd_req_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    uint16_t server_port;
    uint16_t max_uri_handlers;
    uint16_t max_open_sockets;
    bool lru_purge_enable;
    size_t stack_size;
    unsigned recv_wait_timeout;
    unsigned send_wait_timeout;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG()                                                                         \
    {                                                                                                  \
        .server_port = 80, .max_uri_handlers = 8, .max_open_sockets = 7, .lru_purge_enable = false,    \
        .stack_size = 4096, .recv_wait_timeout = 5, .send_wait_timeout = 5,                            \
    }

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *buf, size_t len);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len);

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_500(httpd_req_t *r);
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    return httpd_resp_send(r, str, str ? HTTPD_RESP_USE_STRLEN : 0);
}
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str)
{
    return httpd_resp_send_chunk(r, str, str ? HTTPD_RESP_USE_STRLEN : 0);
}
