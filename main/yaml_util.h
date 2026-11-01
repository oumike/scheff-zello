#pragma once

// Just enough YAML for the settings backup: writing `key: value` lines with
// every string double-quoted, and reading back the block-style subset a person
// is likely to leave after editing one by hand:
//
//   # comment
//   scheff-backup: 1
//   settings:
//     username: "n0call"
//     volume: 70
//   channels:
//     - name: 'My Channel'
//       favorite: true
//
// Top-level scalars, top-level sections holding a map or a list of maps,
// comments, "---", and plain, 'single' or "double" quoted values. Flow
// collections other than an empty [] or {} are not supported. An empty flow
// collection completes its section and cannot have indented children.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Sink for the writers: an HTTP chunk, a FILE, ...
typedef esp_err_t (*yaml_write_cb_t)(void *ctx, const char *data, size_t len);

esp_err_t yaml_write_text(yaml_write_cb_t write, void *ctx, const char *text);

// Writes `<indent><key>: <value>\n`; strings always double-quoted and escaped.
esp_err_t yaml_write_str(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, const char *value);
esp_err_t yaml_write_u32(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, uint32_t value);
esp_err_t yaml_write_bool(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, bool value);

// Called once per value, and once with `key` NULL when a section opens (so an
// empty `channels: []` still registers). `section` is NULL for a top-level
// scalar; `item` is the list index inside a list section, or -1. Returning
// anything but ESP_OK stops the parse with that error.
typedef esp_err_t (*yaml_value_cb_t)(void *ctx, const char *section, int item, const char *key,
                                     const char *value);

// Parses `text`. ESP_ERR_INVALID_ARG on a line it cannot read, or whose value
// the callback rejected with ESP_ERR_INVALID_ARG, with its 1-based number in
// `*bad_line`.
esp_err_t yaml_parse(const char *text, yaml_value_cb_t cb, void *ctx, int *bad_line);

// Scalar conversions for values handed to the callback.
bool yaml_bool(const char *value, bool *out);
bool yaml_u32(const char *value, uint32_t max, uint32_t *out);
