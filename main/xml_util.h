#pragma once

// Just enough XML for the channel list and the settings backup: writing
// escaped attributes, and pulling a named attribute back out of a single
// element. Shared by channel_list.c (the stored list) and web_config.c (the
// export/import file), which use the same on-disk shape.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Sink for the writers: an HTTP chunk, a FILE, ...
typedef esp_err_t (*xml_write_cb_t)(void *ctx, const char *data, size_t len);

// Validates the complete supported XML document without changing any state:
// one root, balanced tags, quoted unique attributes, entities and an optional
// XML declaration/comments. DTDs and other unsupported constructs are rejected.
// ESP_ERR_INVALID_ARG includes malformed/truncated documents.
esp_err_t xml_validate_document(const char *xml);

esp_err_t xml_write_text(xml_write_cb_t write, void *ctx, const char *text);

// Writes valid UTF-8 XML text with &, <, >, " and ' replaced by entities.
// Invalid XML characters are rejected rather than writing an unreadable file.
esp_err_t xml_write_escaped(xml_write_cb_t write, void *ctx, const char *text);

// Writes ` name="value"`, with the value escaped.
esp_err_t xml_write_attr(xml_write_cb_t write, void *ctx, const char *name, const char *value);
esp_err_t xml_write_attr_u32(xml_write_cb_t write, void *ctx, const char *name, uint32_t value);
esp_err_t xml_write_attr_bool(xml_write_cb_t write, void *ctx, const char *name, bool value);

// Reads attribute `name` out of the element that starts at `element` (its '<')
// and ends at `end` (its '>'), decoding entities into `out`. Returns 1 when
// found, 0 when absent, and -1 when the element is malformed or the value does
// not fit.
int xml_attr(const char *element, const char *end, const char *name, char *out, size_t size);

// Typed forms of xml_attr. Both leave `*value` alone and return false when the
// attribute is missing or not a valid value of that type.
bool xml_attr_bool(const char *element, const char *end, const char *name, bool *value);
bool xml_attr_u32(const char *element, const char *end, const char *name, uint32_t max, uint32_t *value);

// Start of the next `<name` element at or after `from`, or NULL. Only matches
// a complete tag name, so "<channel" does not match "<channels".
const char *xml_find_element(const char *from, const char *name);

// Closing '>' of an opening tag, ignoring any '>' inside quoted attributes.
const char *xml_element_end(const char *element);
