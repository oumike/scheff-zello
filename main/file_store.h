#pragma once

#include "esp_err.h"

// Restore path + ".bak" only when path is missing. An existing main file is
// authoritative; path + ".tmp" is never promoted by recovery. Missing main
// and backup files are normal on first boot and return ESP_OK.
esp_err_t file_store_recover(const char *path);

// Promote a successfully written, flushed and closed path + ".tmp", using
// path + ".bak" to retain the old file across a failed/interrupted FAT rename.
// Callers serialize access to a given path. Obsolete-backup cleanup failure
// after promotion is logged, but does not turn a completed save into failure.
esp_err_t file_store_commit(const char *path);
