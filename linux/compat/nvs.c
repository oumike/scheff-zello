// NVS over files (see nvs.h). Paths are relative: the app runs in its
// config directory.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
    esp_err_t error;
} s_handles[MAX_HANDLES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static int sync_fd(int fd)
{
    int result;
    do result = fsync(fd); while (result != 0 && errno == EINTR);
    return result;
}

static esp_err_t sync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return ESP_FAIL;
    esp_err_t err = sync_fd(fd) == 0 ? ESP_OK : ESP_FAIL;
    if (close(fd) != 0) err = ESP_FAIL;
    return err;
}

esp_err_t nvs_flash_init(void)
{
    if (mkdir(NVS_DIR, 0700) == 0) return sync_dir(".");
    return errno == EEXIST ? ESP_OK : ESP_FAIL;
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

static esp_err_t open_locked(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!valid_name(ns) || !out || (mode != NVS_READONLY && mode != NVS_READWRITE)) return ESP_ERR_INVALID_ARG;
    char dir[64];
    snprintf(dir, sizeof(dir), NVS_DIR "/%s", ns);
    struct stat st;
    if (stat(dir, &st) != 0) {
        if (errno != ENOENT) return ESP_FAIL;
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        if (nvs_flash_init() != ESP_OK) return ESP_FAIL;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return ESP_FAIL;
        if (sync_dir(NVS_DIR) != ESP_OK) return ESP_FAIL;
    } else if (!S_ISDIR(st.st_mode)) {
        return ESP_FAIL;
    }
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i].used) {
            s_handles[i].used = 1;
            s_handles[i].mode = mode;
            s_handles[i].error = ESP_OK;
            snprintf(s_handles[i].ns, sizeof(s_handles[i].ns), "%s", ns);
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    pthread_mutex_lock(&s_lock);
    esp_err_t err = open_locked(ns, mode, out);
    pthread_mutex_unlock(&s_lock);
    return err;
}

void nvs_close(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    if (h >= 1 && h <= MAX_HANDLES) s_handles[h - 1].used = 0;
    pthread_mutex_unlock(&s_lock);
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    esp_err_t err = h >= 1 && h <= MAX_HANDLES && s_handles[h - 1].used ?
                    s_handles[h - 1].error : ESP_ERR_INVALID_ARG;
    pthread_mutex_unlock(&s_lock);
    return err;
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
    if (!f) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
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

static esp_err_t write_key_locked(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    char p[64], tmp[80];
    if ((!data && len) || !key_path(h, key, p, sizeof(p))) return ESP_ERR_INVALID_ARG;
    if (s_handles[h - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", p);
    int fd = mkstemp(tmp);
    if (fd < 0) return ESP_FAIL;
    esp_err_t err = fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 ? ESP_OK : ESP_FAIL;
    const unsigned char *b = data;
    size_t done = 0;
    while (err == ESP_OK && done < len) {
        ssize_t w = write(fd, b + done, len - done);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            err = ESP_FAIL;
            break;
        }
        done += (size_t)w;
    }
    if (err == ESP_OK && sync_fd(fd) != 0) err = ESP_FAIL;
    if (close(fd) != 0) err = ESP_FAIL;
    if (err != ESP_OK || rename(tmp, p) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    *strrchr(p, '/') = 0;
    return sync_dir(p);
}

static esp_err_t write_key(nvs_handle_t h, const char *key, const void *data, size_t len)
{
    pthread_mutex_lock(&s_lock);
    esp_err_t err = write_key_locked(h, key, data, len);
    if (err != ESP_OK && h >= 1 && h <= MAX_HANDLES && s_handles[h - 1].used)
        s_handles[h - 1].error = err;
    pthread_mutex_unlock(&s_lock);
    return err;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    if (!len) return ESP_ERR_INVALID_ARG;
    void *data;
    size_t n;
    pthread_mutex_lock(&s_lock);
    esp_err_t err = read_key(h, key, &data, &n);
    pthread_mutex_unlock(&s_lock);
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
    if (!len) return ESP_ERR_INVALID_ARG;
    void *data;
    size_t n;
    pthread_mutex_lock(&s_lock);
    esp_err_t err = read_key(h, key, &data, &n);
    pthread_mutex_unlock(&s_lock);
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
    if (!value) return ESP_ERR_INVALID_ARG;
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
    pthread_mutex_lock(&s_lock);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (key_path(h, key, p, sizeof(p))) {
        err = ESP_ERR_INVALID_STATE;
        if (s_handles[h - 1].mode == NVS_READWRITE) {
            err = unlink(p) == 0 ? ESP_OK : errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
            if (err == ESP_OK) {
                *strrchr(p, '/') = 0;
                err = sync_dir(p);
            }
            if (err != ESP_OK) s_handles[h - 1].error = err;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

static esp_err_t erase_all_locked(nvs_handle_t h)
{
    if (h < 1 || h > MAX_HANDLES || !s_handles[h - 1].used) return ESP_ERR_INVALID_ARG;
    if (s_handles[h - 1].mode != NVS_READWRITE) return ESP_ERR_INVALID_STATE;
    char dir[64];
    snprintf(dir, sizeof(dir), NVS_DIR "/%s", s_handles[h - 1].ns);
    DIR *d = opendir(dir);
    if (!d) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[128];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (unlink(p) != 0) err = ESP_FAIL;
    }
    if (closedir(d) != 0) err = ESP_FAIL;
    if (sync_dir(dir) != ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    esp_err_t err = erase_all_locked(h);
    if (err != ESP_OK && h >= 1 && h <= MAX_HANDLES && s_handles[h - 1].used)
        s_handles[h - 1].error = err;
    pthread_mutex_unlock(&s_lock);
    return err;
}
