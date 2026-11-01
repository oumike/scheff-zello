#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef int wl_handle_t;
#define WL_INVALID_HANDLE -1
#define CONFIG_WL_SECTOR_SIZE 4096
typedef struct {
    bool format_if_mount_failed;
    int max_files;
    int allocation_unit_size;
} esp_vfs_fat_mount_config_t;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *path, const char *label,
                                         const esp_vfs_fat_mount_config_t *cfg,
                                         wl_handle_t *handle);
