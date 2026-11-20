// A small HTTP/1.1 server with esp_http_server's API (see esp_http_server.h).

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "httpd";

#define MAX_HEAD 8192
#define MAX_RESP_HDRS 8

struct httpd_server {
    int listen_fd;
    unsigned recv_timeout_s;
    httpd_uri_t *uris;
    size_t n_uris, max_uris;
    pthread_t thread;
};

typedef struct {
    int fd;
    char pre[MAX_HEAD];  // body bytes that came in with the headers
    size_t pre_len, pre_off;
    size_t body_left;
    const char *status, *type;
    const char *hdr_k[MAX_RESP_HDRS], *hdr_v[MAX_RESP_HDRS];
    int n_hdr;
    bool head_sent, chunked, failed;
} conn_t;

static int send_all(conn_t *c, const char *buf, size_t len)
{
    while (len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
#else
        ssize_t n = send(c->fd, buf, len, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            c->failed = true;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_head(conn_t *c, long content_len)
{
    char h[2048];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n",
                     c->status ? c->status : "200 OK", c->type ? c->type : "text/html");
    for (int i = 0; i < c->n_hdr && n < (int)sizeof(h); i++) {
        if (!strcasecmp(c->hdr_k[i], "Connection")) continue;
        n += snprintf(h + n, sizeof(h) - n, "%s: %s\r\n", c->hdr_k[i], c->hdr_v[i]);
    }
    if (n < (int)sizeof(h)) {
        if (content_len >= 0) n += snprintf(h + n, sizeof(h) - n, "Content-Length: %ld\r\n\r\n", content_len);
        else n += snprintf(h + n, sizeof(h) - n, "Transfer-Encoding: chunked\r\n\r\n");
    }
    if (n >= (int)sizeof(h)) return -1;
    c->head_sent = true;
    return send_all(c, h, (size_t)n);
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    conn_t *c = r->aux;
    if (len > c->body_left) len = c->body_left;
    if (!len) return 0;
    if (c->pre_off < c->pre_len) {
        size_t n = c->pre_len - c->pre_off;
        if (n > len) n = len;
        memcpy(buf, c->pre + c->pre_off, n);
        c->pre_off += n;
        c->body_left -= n;
        return (int)n;
    }
    for (;;) {
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n > 0) {
            c->body_left -= (size_t)n;
            return (int)n;
        }
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return HTTPD_SOCK_ERR_TIMEOUT;
        return HTTPD_SOCK_ERR_FAIL;
    }
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len)
{
    const char *q = strchr(r->uri, '?');
    if (!q) return ESP_ERR_NOT_FOUND;
    q++;
    size_t n = strlen(q);
    if (!len) return ESP_ERR_INVALID_ARG;
    snprintf(buf, len, "%s", q);
    return n < len ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t len)
{
    if (!qry || !key || !val || !len) return ESP_ERR_INVALID_ARG;
    size_t klen = strlen(key);
    const char *p = qry;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == klen && !strncmp(p, key, klen)) {
            size_t vlen = (size_t)(end - eq - 1);
            size_t copy = vlen < len - 1 ? vlen : len - 1;
            memcpy(val, eq + 1, copy);
            val[copy] = 0;
            return vlen < len ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
        }
        p = *end ? end + 1 : end;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    ((conn_t *)r->aux)->status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    ((conn_t *)r->aux)->type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    conn_t *c = r->aux;
    if (c->n_hdr >= MAX_RESP_HDRS) return ESP_ERR_NO_MEM;
    c->hdr_k[c->n_hdr] = field;
    c->hdr_v[c->n_hdr] = value;
    c->n_hdr++;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len)
{
    conn_t *c = r->aux;
    if (c->head_sent) return ESP_ERR_INVALID_STATE;
    if (len < 0) len = buf ? (ssize_t)strlen(buf) : 0;
    if (send_head(c, (long)len) != 0) return ESP_FAIL;
    if (len && send_all(c, buf, (size_t)len) != 0) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len)
{
    conn_t *c = r->aux;
    if (!c->head_sent) {
        c->chunked = true;
        if (send_head(c, -1) != 0) return ESP_FAIL;
    }
    if (!c->chunked) return ESP_ERR_INVALID_STATE;
    if (len < 0) len = buf ? (ssize_t)strlen(buf) : 0;
    char sz[24];
    int n = snprintf(sz, sizeof(sz), "%zx\r\n", (size_t)len);
    if (send_all(c, sz, (size_t)n) != 0) return ESP_FAIL;
    if (len && send_all(c, buf, (size_t)len) != 0) return ESP_FAIL;
    if (send_all(c, "\r\n", 2) != 0) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t httpd_resp_send_500(httpd_req_t *r)
{
    httpd_resp_set_status(r, "500 Internal Server Error");
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, "Server has encountered an unexpected error", HTTPD_RESP_USE_STRLEN);
}

static void send_plain_error(conn_t *c, const char *status, const char *msg)
{
    c->status = status;
    c->type = "text/html";
    if (send_head(c, (long)strlen(msg)) == 0) send_all(c, msg, strlen(msg));
}

static int parse_method(const char *m)
{
    if (!strcmp(m, "GET")) return HTTP_GET;
    if (!strcmp(m, "POST")) return HTTP_POST;
    if (!strcmp(m, "HEAD")) return HTTP_HEAD;
    if (!strcmp(m, "PUT")) return HTTP_PUT;
    if (!strcmp(m, "DELETE")) return HTTP_DELETE;
    return -1;
}

static void handle_conn(struct httpd_server *s, int fd)
{
    struct timeval tv = {.tv_sec = s->recv_timeout_s, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    conn_t *c = calloc(1, sizeof(*c));
    httpd_req_t *req = calloc(1, sizeof(*req));
    if (!c || !req) goto out;
    c->fd = fd;

    char head[MAX_HEAD + 1];
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        if (got >= MAX_HEAD) {
            send_plain_error(c, "431 Request Header Fields Too Large", "Header fields are too long");
            goto out;
        }
        ssize_t n = recv(fd, head + got, MAX_HEAD - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            goto out;
        }
        got += (size_t)n;
        head[got] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    *end = 0;
    size_t head_len = (size_t)(end - head) + 4;
    c->pre_len = got - head_len;
    memcpy(c->pre, head + head_len, c->pre_len);

    char method[8], uri[HTTPD_MAX_URI_LEN + 2];
    if (sscanf(head, "%7s %513s", method, uri) != 2) {
        send_plain_error(c, "400 Bad Request", "Bad request");
        goto out;
    }
    if (strlen(uri) > HTTPD_MAX_URI_LEN) {
        send_plain_error(c, "414 URI Too Long", "URI is too long");
        goto out;
    }
    size_t content_len = 0;
    for (char *line = strstr(head, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (!strncasecmp(line, "Content-Length:", 15)) content_len = strtoul(line + 15, NULL, 10);
    }

    req->handle = s;
    req->method = parse_method(method);
    snprintf((char *)req->uri, sizeof(req->uri), "%s", uri);
    req->content_len = content_len;
    req->aux = c;
    c->body_left = content_len;

    size_t path_len = strcspn(uri, "?");
    const httpd_uri_t *match = NULL;
    bool path_found = false;
    for (size_t i = 0; i < s->n_uris; i++) {
        const httpd_uri_t *u = &s->uris[i];
        if (strlen(u->uri) != path_len || strncmp(u->uri, uri, path_len)) continue;
        path_found = true;
        if ((int)u->method == req->method) {
            match = u;
            break;
        }
    }
    if (!match) {
        if (path_found) send_plain_error(c, "405 Method Not Allowed", "Request method for this URI is not handled by server");
        else send_plain_error(c, "404 Not Found", "Nothing matches the given URI");
        goto out;
    }
    req->user_ctx = match->user_ctx;
    // A handler that fails partway through a chunked download never sends the
    // final chunk: the closed connection tells the browser it broke.
    match->handler(req);
out:
    shutdown(fd, SHUT_WR);
    // Read what the client still sends (briefly), so closing doesn't reset
    // the connection before it has read our response.
    {
        char sink[512];
        struct pollfd p = {.fd = fd, .events = POLLIN};
        for (int i = 0; i < 8 && poll(&p, 1, 100) > 0; i++)
            if (recv(fd, sink, sizeof(sink), 0) <= 0) break;
    }
    close(fd);
    free(c);
    free(req);
}

static void *server_thread(void *arg)
{
    struct httpd_server *s = arg;
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EBADF || errno == EINVAL) break;
            usleep(100000);
            continue;
        }
        handle_conn(s, fd);
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    struct httpd_server *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->max_uris = config->max_uri_handlers;
    s->uris = calloc(s->max_uris ? s->max_uris : 1, sizeof(httpd_uri_t));
    s->recv_timeout_s = config->recv_wait_timeout ? config->recv_wait_timeout : 5;
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = fd >= 0;
    if (!v6) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !s->uris) goto fail;
    int one = 1, zero = 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);  // esp_restart() execs the app again
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int rc;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(config->server_port),
                                 .sin6_addr = in6addr_any};
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(config->server_port),
                                .sin_addr.s_addr = htonl(INADDR_ANY)};
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    }
    if (rc != 0 || listen(fd, 8) != 0) {
        ESP_LOGE(TAG, "port %u: %s", config->server_port, strerror(errno));
        goto fail;
    }
    s->listen_fd = fd;
    if (pthread_create(&s->thread, NULL, server_thread, s) != 0) goto fail;
    *handle = s;
    return ESP_OK;
fail:
    if (fd >= 0) close(fd);
    free(s->uris);
    free(s);
    return ESP_FAIL;
}

esp_err_t httpd_stop(httpd_handle_t s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
    pthread_join(s->thread, NULL);
    free(s->uris);
    free(s);
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t s, const httpd_uri_t *uri)
{
    if (!s || !uri) return ESP_ERR_INVALID_ARG;
    if (s->n_uris >= s->max_uris) return ESP_ERR_NO_MEM;
    s->uris[s->n_uris++] = *uri;
    return ESP_OK;
}
