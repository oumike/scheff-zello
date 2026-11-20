#pragma once

// The data partition is a directory on Linux (linux_paths.h); mounting it
// only makes sure it exists.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef int wl_handle_t;
#define WL_INVALID_HANDLE -1

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *label,
                                           const esp_vfs_fat_mount_config_t *cfg, wl_handle_t *wl);
