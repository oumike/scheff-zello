#pragma once

// The channels this device knows about, kept as XML on the internal FAT
// partition at /data/zello-channels.xml:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <zello-channels version="1">
//     <channel name="Example Channel" desc="Weekend net" favorite="true"
//              uses="12"/>
//   </zello-channels>
//
// Zello has no public channel directory to download, so the list is the user's
// own: channels typed on the device or in the web config, plus every channel a
// successful logon confirms. `uses` counts successful joins and orders the list
// under the favorites.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CHANNEL_LIST_PATH "/data/zello-channels.xml"
#define CHANNEL_LIST_MAX  128

typedef struct {
    char name[65];
    char desc[64];
    uint32_t uses;
    bool favorite;
} channel_entry_t;

// Mounts /data and loads the list. A missing file is not an error: the list
// starts empty and is written on the first change.
esp_err_t channel_list_init(void);

size_t channel_list_count(void);
size_t channel_list_favorite_count(void);

// Copies the channels matching `query` (NULL or empty for all) into `out`,
// favorites first, then by use count, then by name. With `favorites_only`,
// non-favorites are skipped. Returns the number copied.
size_t channel_list_get(const char *query, bool favorites_only, channel_entry_t *out, size_t max);

bool channel_list_find(const char *name, channel_entry_t *out);

// Names are exact, case-sensitive identifiers (search and sorting are not).
// Adds or replaces an entry and rewrites the file. With `orig_name` set, the
// entry of that name is replaced, which may rename it; otherwise `entry` is
// added, replacing any of the same name. ESP_ERR_NO_MEM once the list is full.
// Renaming onto a different existing entry returns ESP_ERR_INVALID_ARG.
// A failed save leaves the in-memory list and recoverable old file intact.
esp_err_t channel_list_put(const char *orig_name, const channel_entry_t *entry);

esp_err_t channel_list_delete(const char *name);
esp_err_t channel_list_set_favorite(const char *name, bool favorite);

// Records a channel we just joined: adds it if it is new, and bumps its use
// count. Failing to save is logged rather than propagated - a successful join
// shouldn't look like a failure because the flash write didn't take.
void channel_list_record_join(const char *name);

// Validates the complete XML document and every <channel> entry, then replaces
// the list and saves it. Malformed/over-capacity input changes nothing and
// returns ESP_ERR_INVALID_ARG. ESP_ERR_NOT_FOUND for a well-formed empty list.
esp_err_t channel_list_import_buffer(const char *xml, size_t *count);

// Replaces the whole list with the named entries of `entries` (the first of a
// repeated exact name wins) and saves it. Invalid or over-capacity entries
// change nothing. ESP_ERR_NOT_FOUND for n == 0, also without replacement.
esp_err_t channel_list_replace(const channel_entry_t *entries, size_t n, size_t *count);

// Appends the list as <channel .../> elements, one per line, indented by
// `indent`. Used for both the file on /data and the backup download.
#include "xml_util.h"
esp_err_t channel_list_write(xml_write_cb_t write, void *ctx, const char *indent);

// Registers a callback run (on the caller's task) whenever the list changes.
void channel_list_set_changed_cb(void (*cb)(void));
