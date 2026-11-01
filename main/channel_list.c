#include "channel_list.h"
#include "file_store.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "channels";

#define DATA_MOUNT "/data"
#define DATA_LABEL "storage"
#define TMP_PATH CHANNEL_LIST_PATH ".tmp"

static const char FILE_HEADER[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<zello-channels version=\"1\">\n";
static const char FILE_FOOTER[] = "</zello-channels>\n";
// Every input byte can expand to six bytes (&quot; or &apos;). Include all
// fixed syntax and the longest decimal counter so every legal saved list fits.
#define FILE_MAX (sizeof(FILE_HEADER) - 1 + sizeof(FILE_FOOTER) - 1 + CHANNEL_LIST_MAX * \
    (6 * (sizeof(((channel_entry_t *)0)->name) - 1 + sizeof(((channel_entry_t *)0)->desc) - 1) + \
     sizeof("  <channel name=\"\" desc=\"\" favorite=\"false\" uses=\"4294967295\"/>\n") - 1))

static channel_entry_t s_list[CHANNEL_LIST_MAX];
static size_t s_count;
static SemaphoreHandle_t s_lock;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static void (*s_changed_cb)(void);

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

// ---- ordering --------------------------------------------------------------

static int compare(const void *a, const void *b)
{
    const channel_entry_t *x = a, *y = b;
    if (x->favorite != y->favorite) return x->favorite ? -1 : 1;
    if (x->uses != y->uses) return x->uses > y->uses ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void sort_list(void)
{
    qsort(s_list, s_count, sizeof(s_list[0]), compare);
}

// Index of `name` in the list, or -1.
static int index_of(const char *name)
{
    for (size_t i = 0; i < s_count; i++) {
        if (!strcmp(s_list[i].name, name)) return (int)i;
    }
    return -1;
}

// ---- storage ---------------------------------------------------------------

static esp_err_t file_write(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, ctx) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_entries(xml_write_cb_t write, void *ctx, const char *indent,
                               const channel_entry_t *entries, size_t count)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count && err == ESP_OK; i++) {
        const channel_entry_t *e = &entries[i];
        err = xml_write_text(write, ctx, indent);
        if (err == ESP_OK) err = xml_write_text(write, ctx, "<channel");
        if (err == ESP_OK) err = xml_write_attr(write, ctx, "name", e->name);
        if (err == ESP_OK) err = xml_write_attr(write, ctx, "desc", e->desc);
        if (err == ESP_OK) err = xml_write_attr_bool(write, ctx, "favorite", e->favorite);
        if (err == ESP_OK) err = xml_write_attr_u32(write, ctx, "uses", e->uses);
        if (err == ESP_OK) err = xml_write_text(write, ctx, "/>\n");
    }
    return err;
}

esp_err_t channel_list_write(xml_write_cb_t write, void *ctx, const char *indent)
{
    lock();
    esp_err_t err = write_entries(write, ctx, indent, s_list, s_count);
    unlock();
    return err;
}

static esp_err_t save_locked(const channel_entry_t *entries, size_t count)
{
    if (s_wl == WL_INVALID_HANDLE) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(TMP_PATH, "w");
    if (!f) return ESP_FAIL;
    esp_err_t err = xml_write_text(file_write, f, FILE_HEADER);
    if (err == ESP_OK) err = write_entries(file_write, f, "  ", entries, count);
    if (err == ESP_OK) err = xml_write_text(file_write, f, FILE_FOOTER);
    if (fflush(f) != 0) err = ESP_FAIL;
    if (fclose(f) != 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        remove(TMP_PATH);
        return err;
    }
    return file_store_commit(CHANNEL_LIST_PATH);
}

static bool valid_entry(const channel_entry_t *entry)
{
    return entry && entry->name[0] && memchr(entry->name, 0, sizeof(entry->name)) &&
           memchr(entry->desc, 0, sizeof(entry->desc));
}

static esp_err_t append_entry(channel_entry_t *entries, size_t *count, const channel_entry_t *entry)
{
    if (!valid_entry(entry)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < *count; i++)
        if (!strcmp(entries[i].name, entry->name)) return ESP_OK;
    if (*count == CHANNEL_LIST_MAX) return ESP_ERR_INVALID_ARG;
    entries[(*count)++] = *entry;
    return ESP_OK;
}

static esp_err_t parse_entries(const char *xml, channel_entry_t *entries, size_t *count)
{
    *count = 0;
    esp_err_t err = xml_validate_document(xml);
    if (err != ESP_OK) return err;
    for (const char *p = xml_find_element(xml, "channel"); p;
         p = xml_find_element(p + 1, "channel")) {
        const char *end = xml_element_end(p);
        if (!end) return ESP_ERR_INVALID_ARG;
        if (end[-1] != '/') {
            const char *body = end + 1;
            for (;;) {
                while (isspace((unsigned char)*body)) body++;
                if (strncmp(body, "<!--", 4)) break;
                body = strstr(body + 4, "-->") + 3;  // already validated
            }
            if (strncmp(body, "</channel", 9) ||
                (!isspace((unsigned char)body[9]) && body[9] != '>')) return ESP_ERR_INVALID_ARG;
        }
        channel_entry_t e = {0};
        if (xml_attr(p, end, "name", e.name, sizeof(e.name)) != 1 ||
            xml_attr(p, end, "desc", e.desc, sizeof(e.desc)) < 0) return ESP_ERR_INVALID_ARG;
        char text[12];
        int present = xml_attr(p, end, "favorite", text, sizeof(text));
        if (present < 0 || (present && !xml_attr_bool(p, end, "favorite", &e.favorite))) {
            return ESP_ERR_INVALID_ARG;
        }
        present = xml_attr(p, end, "uses", text, sizeof(text));
        if (present < 0 || (present && !xml_attr_u32(p, end, "uses", UINT32_MAX, &e.uses))) {
            return ESP_ERR_INVALID_ARG;
        }
        err = append_entry(entries, count, &e);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void notify(void)
{
    if (s_changed_cb) s_changed_cb();
}

esp_err_t channel_list_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(DATA_MOUNT, DATA_LABEL, &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not mount %s: %s", DATA_MOUNT, esp_err_to_name(err));
        return err;
    }

    err = file_store_recover(CHANNEL_LIST_PATH);
    if (err != ESP_OK) return err;
    FILE *f = fopen(CHANNEL_LIST_PATH, "r");
    if (!f) {
        if (errno != ENOENT) return ESP_FAIL;
        ESP_LOGI(TAG, "no channel list yet");
        return ESP_OK;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > FILE_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        ESP_LOGW(TAG, "invalid %ld byte channel list", size);
        return ESP_ERR_INVALID_ARG;
    }
    char *xml = malloc(size + 1);
    if (!xml) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(xml, 1, size, f);
    bool ok = got == (size_t)size && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok || memchr(xml, 0, got)) {
        free(xml);
        return ok ? ESP_ERR_INVALID_ARG : ESP_FAIL;
    }
    xml[got] = 0;

    channel_entry_t *entries = malloc(sizeof(s_list));
    if (!entries) {
        free(xml);
        return ESP_ERR_NO_MEM;
    }
    size_t n;
    err = parse_entries(xml, entries, &n);
    if (err == ESP_OK) {
        lock();
        memcpy(s_list, entries, n * sizeof(*entries));
        s_count = n;
        sort_list();
        unlock();
        ESP_LOGI(TAG, "loaded %u channels", (unsigned)n);
    }
    free(entries);
    free(xml);
    return err;
}

// ---- reads -----------------------------------------------------------------

size_t channel_list_count(void)
{
    lock();
    size_t n = s_count;
    unlock();
    return n;
}

size_t channel_list_favorite_count(void)
{
    lock();
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        if (s_list[i].favorite) n++;
    }
    unlock();
    return n;
}

static bool matches(const channel_entry_t *e, const char *query)
{
    return !query || !query[0] || strcasestr(e->name, query) || strcasestr(e->desc, query);
}

size_t channel_list_get(const char *query, bool favorites_only, channel_entry_t *out, size_t max)
{
    lock();
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max; i++) {
        if (favorites_only && !s_list[i].favorite) continue;
        if (!matches(&s_list[i], query)) continue;
        out[n++] = s_list[i];
    }
    unlock();
    return n;
}

bool channel_list_find(const char *name, channel_entry_t *out)
{
    lock();
    int i = index_of(name);
    if (i >= 0 && out) *out = s_list[i];
    unlock();
    return i >= 0;
}

// ---- writes ----------------------------------------------------------------

esp_err_t channel_list_put(const char *orig_name, const channel_entry_t *entry)
{
    if (!valid_entry(entry)) return ESP_ERR_INVALID_ARG;
    lock();
    int at = orig_name && orig_name[0] ? index_of(orig_name) : -1;
    int clash = index_of(entry->name);
    if (clash >= 0 && clash != at) {
        if (orig_name && orig_name[0]) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
        at = clash;
    }
    size_t old_count = s_count;
    channel_entry_t previous = {0};
    if (at >= 0) previous = s_list[at];
    if (at < 0) {
        if (s_count >= CHANNEL_LIST_MAX) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
        at = (int)s_count++;
    }
    s_list[at] = *entry;
    esp_err_t err = save_locked(s_list, s_count);
    if (err != ESP_OK) {
        s_list[at] = previous;
        s_count = old_count;
    }
    sort_list();
    unlock();
    if (err == ESP_OK) notify();
    return err;
}

esp_err_t channel_list_delete(const char *name)
{
    lock();
    int at = index_of(name);
    if (at < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    channel_entry_t previous = s_list[at];
    s_list[at] = s_list[--s_count];
    esp_err_t err = save_locked(s_list, s_count);
    if (err != ESP_OK) {
        s_list[s_count++] = s_list[at];
        s_list[at] = previous;
    }
    sort_list();
    unlock();
    if (err == ESP_OK) notify();
    return err;
}

esp_err_t channel_list_set_favorite(const char *name, bool favorite)
{
    lock();
    int at = index_of(name);
    if (at < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    bool previous = s_list[at].favorite;
    s_list[at].favorite = favorite;
    esp_err_t err = save_locked(s_list, s_count);
    if (err != ESP_OK) s_list[at].favorite = previous;
    sort_list();
    unlock();
    if (err == ESP_OK) notify();
    return err;
}

void channel_list_record_join(const char *name)
{
    if (!name || !name[0] || strlen(name) >= sizeof(s_list[0].name)) return;
    lock();
    int at = index_of(name);
    size_t old_count = s_count;
    if (at < 0) {
        if (s_count >= CHANNEL_LIST_MAX) {
            unlock();
            return;
        }
        at = (int)s_count++;
        s_list[at] = (channel_entry_t){0};
        strlcpy(s_list[at].name, name, sizeof(s_list[at].name));
    }
    uint32_t previous = s_list[at].uses;
    s_list[at].uses++;
    esp_err_t err = save_locked(s_list, s_count);
    if (err != ESP_OK) {
        s_list[at].uses = previous;
        s_count = old_count;
    }
    sort_list();
    unlock();
    if (err != ESP_OK) ESP_LOGW(TAG, "could not record the join of \"%s\": %s", name, esp_err_to_name(err));
    else notify();
}

esp_err_t channel_list_import_buffer(const char *xml, size_t *count)
{
    if (count) *count = 0;
    channel_entry_t *entries = malloc(sizeof(s_list));
    if (!entries) return ESP_ERR_NO_MEM;

    size_t n;
    esp_err_t err = parse_entries(xml, entries, &n);
    if (err == ESP_OK && !n) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        qsort(entries, n, sizeof(*entries), compare);
        lock();
        err = save_locked(entries, n);
        if (err == ESP_OK) {
            memcpy(s_list, entries, n * sizeof(*entries));
            s_count = n;
        }
        unlock();
    }
    free(entries);
    if (err == ESP_OK && count) *count = n;
    if (err == ESP_OK) notify();
    return err;
}

esp_err_t channel_list_replace(const channel_entry_t *entries, size_t n, size_t *count)
{
    if (count) *count = 0;
    if (!n) return ESP_ERR_NOT_FOUND;
    if (!entries) return ESP_ERR_INVALID_ARG;
    channel_entry_t *replacement = malloc(sizeof(s_list));
    if (!replacement) return ESP_ERR_NO_MEM;

    size_t added = 0;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < n && err == ESP_OK; i++)
        err = append_entry(replacement, &added, &entries[i]);
    if (err == ESP_OK) {
        qsort(replacement, added, sizeof(*replacement), compare);
        lock();
        err = save_locked(replacement, added);
        if (err == ESP_OK) {
            memcpy(s_list, replacement, added * sizeof(*replacement));
            s_count = added;
        }
        unlock();
    }
    free(replacement);
    if (err == ESP_OK && count) *count = added;
    if (err == ESP_OK) notify();
    return err;
}

void channel_list_set_changed_cb(void (*cb)(void))
{
    s_changed_cb = cb;
}
