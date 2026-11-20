// NVS over files (see nvs.h). Paths are relative: the app runs in its
// config directory.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_DIR "nvs"
#define MAX_HANDLES 16

static struct {
    char ns[16];
    nvs_open_mode_t mode;
    int used;
} s_handles[MAX_HANDLES];

esp_err_t nvs_flash_init(void)
{
    if (mkdir(NVS_DIR, 0700) != 0 && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    DIR *d = opendir(NVS_DIR);
    if (!d) return ESP_OK;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        nvs_handle_t h;
        char ns[16];
        snprintf(ns, sizeof(ns), "%s", e->d_name);
        if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_close(h);
        }
        char p[64];
        snprintf(p, sizeof(p), NVS_DIR "/%s", ns);
        rmdir(p);
    }
    closedir(d);
    return ESP_OK;
}

static int valid_name(const char *s)
{
    if (!s || !*s || strlen(s) > 15) return 0;
    for (; *s; s++)
        if (*s == '/' || *s == '.') return 0;
    return 1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!valid_name(ns) || !out) return ESP_ERR_INVALID_ARG;
    char dir[64];
    snprintf(dir, sizeof(dir), NVS_DIR "/%s", ns);
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        if (mkdir(NVS_DIR, 0700) != 0 && errno != EEXIST) return ESP_FAIL;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return ESP_FAIL;
    }
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i].used) {
            s_handles[i].used = 1;
            s_handles[i].mode = mode;
            snprintf(s_handles[i].ns, sizeof(s_handles[i].ns), "%s", ns);
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t h)
{
    if (h >= 1 && h <= MAX_HANDLES) s_handles[h - 1].used = 0;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static int key_path(nvs_handle_t h, const char *key, char *out, size_t n)
{
    if (h < 1 || h > MAX_HANDLES || !s_handles[h - 1].used || !valid_name(key)) return 0;
    snprintf(out, n, NVS_DIR "/%s/%s", s_handles[h - 1].ns, key);
    return 1;
}

static esp_err_t read_key(nvs_handle_t h, const char *key, void **data, size_t *len)
{
    char p[64];
    if (!key_path(h, key, p, sizeof(p))) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NVS_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    void *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    *data = buf;
    *len = (size_t)n;
    return ESP_OK;
}

static esp_err_t write_key(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    char p[64], tmp[72];
    if (!key_path(h, key, p, sizeof(p))) return ESP_ERR_INVALID_ARG;
    if (s_handles[h - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return ESP_FAIL;
    const unsigned char *b = data;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, b + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return ESP_FAIL;
        }
        done += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, p) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    void *data;
    size_t n;
    esp_err_t err = read_key(h, key, &data, &n);
    if (err != ESP_OK) return err;
    if (out) {
        if (*len < n) err = ESP_ERR_NVS_INVALID_LENGTH;
        else memcpy(out, data, n);
    }
    *len = n;
    free(data);
    return err;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    return write_key(h, key, value, len);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    void *data;
    size_t n;
    esp_err_t err = read_key(h, key, &data, &n);
    if (err != ESP_OK) return err;
    if (out) {
        if (*len < n + 1) err = ESP_ERR_NVS_INVALID_LENGTH;
        else {
            memcpy(out, data, n);
            out[n] = 0;
        }
    }
    *len = n + 1;
    free(data);
    return err;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    return write_key(h, key, value, strlen(value));
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    size_t len = 1;
    return nvs_get_blob(h, key, out, &len);
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) { return write_key(h, key, &value, 1); }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    char p[64];
    if (!key_path(h, key, p, sizeof(p))) return ESP_ERR_INVALID_ARG;
    return unlink(p) == 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    if (h < 1 || h > MAX_HANDLES || !s_handles[h - 1].used) return ESP_ERR_INVALID_ARG;
    char dir[64];
    snprintf(dir, sizeof(dir), NVS_DIR "/%s", s_handles[h - 1].ns);
    DIR *d = opendir(dir);
    if (!d) return ESP_OK;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[128];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    return ESP_OK;
}
