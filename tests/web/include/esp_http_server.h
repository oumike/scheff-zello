#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"

#define HTTPD_SOCK_ERR_TIMEOUT (-3)
#define ESP_ERR_HTTPD_RESULT_TRUNC 0x7007
#define HTTP_GET 0
#define HTTP_POST 1
typedef void *httpd_handle_t;

typedef struct {
    size_t content_len;
    httpd_handle_t handle;
    const unsigned char *body;
    const char *query;
    size_t offset, chunk_size;
    int timeouts, recv_calls;
    unsigned recv_ticks;
    bool close_header;
    char status[40], response[16384];
} httpd_req_t;

typedef struct {
    int max_uri_handlers, stack_size;
    bool lru_purge_enable;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})

typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;

int httpd_req_recv(httpd_req_t *req, char *out, size_t size);
size_t httpd_req_get_url_query_len(httpd_req_t *req);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *out, size_t size);
esp_err_t httpd_query_key_value(const char *query, const char *key, char *out, size_t size);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, ssize_t size);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *data);
esp_err_t httpd_resp_send_500(httpd_req_t *req);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, ssize_t size);
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
