#include "host.h"
#undef fopen
#undef rename
#undef remove
#undef stat

#include <errno.h>
#include <stdlib.h>
#include "esp_vfs_fat.h"
#include "nvs_flash.h"

unsigned storage_rename_failures;
unsigned storage_rename_calls;
bool storage_fail_backup_remove;
bool storage_fail_nvs_save;
int storage_nvs_init_error;

static const char *local_path(const char *path)
{
    return !strncmp(path, "/data/", 6) ? path + 1 : path;
}

FILE *storage_fopen(const char *path, const char *mode)
{
    return fopen(local_path(path), mode);
}

int storage_stat(const char *path, struct stat *info)
{
    return stat(local_path(path), info);
}

int storage_rename(const char *from, const char *to)
{
    unsigned call = storage_rename_calls++;
    if (call < 32 && (storage_rename_failures & (1u << call))) {
        errno = EIO;
        return -1;
    }
    struct stat info;
    if (!stat(local_path(to), &info)) {
        errno = EEXIST;  // FatFs cannot rename over an existing destination.
        return -1;
    }
    return rename(local_path(from), local_path(to));
}

int storage_remove(const char *path)
{
    size_t n = strlen(path);
    if (storage_fail_backup_remove && n >= 4 && !strcmp(path + n - 4, ".bak")) {
        errno = EIO;
        return -1;
    }
    return remove(local_path(path));
}

#ifndef __APPLE__
size_t strlcpy(char *dest, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t used = n < size - 1 ? n : size - 1;
        memcpy(dest, src, used);
        dest[used] = 0;
    }
    return n;
}
#endif

const char *esp_err_to_name(esp_err_t err) { (void)err; return "host error"; }

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *path, const char *label,
                                         const esp_vfs_fat_mount_config_t *cfg,
                                         wl_handle_t *handle)
{
    (void)path;
    (void)label;
    (void)cfg;
    if (mkdir("data", 0700) && errno != EEXIST) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}

// NVS persists each set immediately; commit is a no-op in the installed IDF.
enum { STRING, BYTE, BLOB };
typedef struct {
    char key[16];
    int type;
    size_t size;
    unsigned char data[4096];
} nvs_value_t;
static nvs_value_t values[24];
static size_t value_count;
static bool namespace_exists;

static nvs_value_t *lookup(const char *key)
{
    for (size_t i = 0; i < value_count; i++)
        if (!strcmp(values[i].key, key)) return &values[i];
    return NULL;
}

static esp_err_t get(const char *key, int type, void *out, size_t *size)
{
    nvs_value_t *value = lookup(key);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (value->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    size_t capacity = *size;
    *size = value->size;
    if (!out) return ESP_OK;
    if (capacity < value->size) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, value->data, value->size);
    return ESP_OK;
}

static esp_err_t set(const char *key, int type, const void *data, size_t size)
{
    if (storage_fail_nvs_save && (type == BLOB || !strcmp(key, "password"))) return ESP_FAIL;
    nvs_value_t *value = lookup(key);
    if (!value) {
        if (value_count == 24 || strlen(key) >= 16) return ESP_FAIL;
        value = &values[value_count++];
        strlcpy(value->key, key, sizeof(value->key));
    }
    if (size > sizeof(value->data)) return ESP_ERR_NO_MEM;
    value->type = type;
    value->size = size;
    memcpy(value->data, data, size);
    namespace_exists = true;
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    (void)ns;
    if (!namespace_exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) namespace_exists = true;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size)
{
    (void)handle; return get(key, STRING, out, size);
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    (void)handle; size_t size = 1; return get(key, BYTE, out, &size);
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    (void)handle; return get(key, BLOB, out, size);
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle; return set(key, STRING, value, strlen(value) + 1);
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle; return set(key, BYTE, &value, 1);
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    (void)handle; return set(key, BLOB, value, size);
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t nvs_flash_init(void) { return storage_nvs_init_error; }
esp_err_t nvs_flash_erase(void)
{
    value_count = 0;
    namespace_exists = false;
    storage_nvs_init_error = ESP_OK;
    return ESP_OK;
}
bool storage_corrupt_snapshot(void)
{
    for (size_t i = 0; i < value_count; i++) {
        if (values[i].type == BLOB) {
            values[i].data[0] ^= 0xff;
            return true;
        }
    }
    return false;
}
