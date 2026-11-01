#include "file_store.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "file_store";

static char *with_suffix(const char *path, const char *suffix)
{
    size_t len = strlen(path);
    char *result = malloc(len + 5);
    if (result) {
        memcpy(result, path, len);
        memcpy(result + len, suffix, 5);
    }
    return result;
}

static esp_err_t exists(const char *path, bool *found)
{
    struct stat info;
    if (stat(path, &info) == 0) {
        *found = true;
        return S_ISREG(info.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    *found = false;
    return errno == ENOENT ? ESP_OK : ESP_FAIL;
}

static esp_err_t recover(const char *path, const char *backup)
{
    bool main_exists, backup_exists;
    esp_err_t err = exists(path, &main_exists);
    if (err == ESP_OK) err = exists(backup, &backup_exists);
    if (err != ESP_OK) return err;
    if (!backup_exists) return ESP_OK;
    if (!main_exists) {
        if (rename(backup, path) == 0) return ESP_OK;
        ESP_LOGE(TAG, "could not restore %s: errno %d", path, errno);
        return ESP_FAIL;
    }
    if (remove(backup) != 0) {
        ESP_LOGW(TAG, "could not clean up %s: errno %d", backup, errno);
    }
    return ESP_OK;
}

esp_err_t file_store_recover(const char *path)
{
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    char *backup = with_suffix(path, ".bak");
    if (!backup) return ESP_ERR_NO_MEM;
    esp_err_t err = recover(path, backup);
    free(backup);
    return err;
}

esp_err_t file_store_commit(const char *path)
{
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    char *backup = with_suffix(path, ".bak");
    char *stage = with_suffix(path, ".tmp");
    if (!backup || !stage) {
        free(backup);
        free(stage);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = recover(path, backup);
    bool stage_exists = false, main_exists = false, backup_exists = false;
    if (err == ESP_OK) err = exists(stage, &stage_exists);
    if (err == ESP_OK && !stage_exists) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) err = exists(path, &main_exists);
    if (err == ESP_OK) err = exists(backup, &backup_exists);
    if (err == ESP_OK && backup_exists && remove(backup) != 0) err = ESP_FAIL;
    if (err == ESP_OK && main_exists && rename(path, backup) != 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        if (rename(stage, path) != 0) {
            err = ESP_FAIL;
            if (main_exists && rename(backup, path) != 0) {
                ESP_LOGE(TAG, "could not roll back %s; retained %s", path, backup);
            }
        } else if (main_exists && remove(backup) != 0) {
            ESP_LOGW(TAG, "saved %s, but could not clean up %s: errno %d", path, backup, errno);
        }
    }
    free(backup);
    free(stage);
    return err;
}
