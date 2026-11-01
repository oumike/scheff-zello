#include <stdint.h>
#include <stdlib.h>
#include "channel_list.h"
#include "file_store.h"
#include "settings.h"
#include "xml_util.h"
#include "yaml_util.h"
#include "nvs.h"
#include "host.h"

#define CHECK(condition) do { \
    if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); return 1; } \
} while (0)

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f)) {
        fprintf(stderr, "fixture write failed: %s\n", path);
        exit(2);
    }
}

static bool file_equals(const char *path, const char *text)
{
    char buf[64];
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == strlen(text) && !memcmp(buf, text, n);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int recover(void)
{
    write_file("list.bak", "previous");
    write_file("list.tmp", "incomplete");
    CHECK(file_store_recover("list") == ESP_OK);
    CHECK(file_equals("list", "previous"));
    CHECK(file_store_recover("list") == ESP_OK);
    write_file("list.bak", "older");
    CHECK(file_store_recover("list") == ESP_OK);
    CHECK(file_equals("list", "previous"));
    CHECK(remove("list") == 0);
    if (exists("list.bak")) CHECK(remove("list.bak") == 0);
    CHECK(file_store_recover("list") == ESP_OK);
    CHECK(!exists("list"));
    return 0;
}

static int promote(void)
{
    for (unsigned failure = 0; failure < 2; failure++) {
        write_file("list", "previous");
        write_file("list.tmp", "new");
        storage_rename_calls = 0;
        storage_rename_failures = 1u << failure;
        CHECK(file_store_commit("list") != ESP_OK);
        CHECK(file_equals("list", "previous"));
    }
    write_file("list", "previous");
    write_file("list.tmp", "new");
    storage_rename_calls = 0;
    storage_rename_failures = (1u << 1) | (1u << 2);
    CHECK(file_store_commit("list") != ESP_OK);
    CHECK(file_equals("list", "previous") || file_equals("list.bak", "previous"));
    storage_rename_failures = storage_rename_calls = 0;
    CHECK(file_store_recover("list") == ESP_OK);
    CHECK(file_equals("list", "previous"));
    write_file("list.tmp", "new");
    storage_fail_backup_remove = true;
    CHECK(file_store_commit("list") == ESP_OK);
    CHECK(file_equals("list", "new"));
    CHECK(file_store_recover("list") == ESP_OK);
    CHECK(file_equals("list", "new"));
    write_file("list.tmp", "next");
    CHECK(file_store_commit("list") != ESP_OK);
    CHECK(file_equals("list", "new"));
    storage_fail_backup_remove = false;
    CHECK(file_store_commit("list") == ESP_OK);
    CHECK(file_equals("list", "next"));
    CHECK(file_store_commit("list") == ESP_ERR_NOT_FOUND);
    CHECK(file_equals("list", "next"));
    return 0;
}

static int channel_save_failure(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "old", .desc = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    channel_entry_t replacement = {.name = "new"};
    storage_rename_calls = 0;
    storage_rename_failures = 0xffffffffu;
    CHECK(channel_list_put("old", &replacement) != ESP_OK);
    CHECK(exists(CHANNEL_LIST_PATH) || exists(CHANNEL_LIST_PATH ".bak"));
    CHECK(channel_list_find("old", NULL));
    CHECK(!channel_list_find("new", NULL));
    storage_rename_calls = 0;
    CHECK(channel_list_delete("old") != ESP_OK);
    CHECK(channel_list_count() == 1 && channel_list_find("old", NULL));
    storage_rename_calls = 0;
    CHECK(channel_list_set_favorite("old", true) != ESP_OK);
    channel_entry_t entry;
    CHECK(channel_list_find("old", &entry) && !entry.favorite);
    storage_rename_calls = 0;
    channel_list_record_join("old");
    CHECK(channel_list_find("old", &entry) && entry.uses == 0);
    storage_rename_calls = 0;
    channel_list_record_join("unsaved");
    CHECK(channel_list_count() == 1 && !channel_list_find("unsaved", NULL));
    storage_rename_failures = storage_rename_calls = 0;
    CHECK(channel_list_init() == ESP_OK);
    CHECK(channel_list_count() == 1 && channel_list_find("old", NULL));
    return 0;
}

static int channel_recovery(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    CHECK(rename(CHANNEL_LIST_PATH, CHANNEL_LIST_PATH ".bak") == 0);
    write_file(CHANNEL_LIST_PATH ".tmp", "<zello-channels><channel name='partial'/>");
    CHECK(channel_list_init() == ESP_OK);
    CHECK(exists(CHANNEL_LIST_PATH));
    CHECK(channel_list_count() == 1 && channel_list_find("retained", NULL));
    write_file(CHANNEL_LIST_PATH, "<zello-channels><channel name='partial'/>");
    CHECK(channel_list_init() == ESP_ERR_INVALID_ARG);
    CHECK(channel_list_count() == 1 && channel_list_find("retained", NULL));
    return 0;
}

static int malformed_import(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    const char *invalid[] = {
        "<zello-channels><channel name=\"new\"/>",
        "<zello-channels><channel name=\"new\"/></wrong>",
        "<zello-channels><channel name=\"new\"/><channel/></zello-channels>",
        "<zello-channels><channel name=\"new\" uses=\"junk\"/></zello-channels>",
        "<zello-channels><channel name=\"new\" favorite=\"maybe\"/></zello-channels>",
        "<zello-channels><channel name=\"new\" desc=\"&bogus;\"/></zello-channels>",
        "<zello-channels><channel name=\"new\"/></zello-channels>junk",
        "<zello-channels><channel name=\"new\" name=\"other\"/></zello-channels>",
        "<zello-channels><channel name=\"new\" desc=\"unterminated/></zello-channels>",
        "<zello-channels><channel name=\"new\" uses=\"-1\"/></zello-channels>",
        "<zello-channels><channel name=\"new\" uses=\"4294967296\"/></zello-channels>",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        size_t count = 99;
        CHECK(channel_list_import_buffer(invalid[i], &count) == ESP_ERR_INVALID_ARG);
        CHECK(channel_list_count() == 1 && channel_list_find("retained", NULL));
        CHECK(count == 0);
    }
    size_t count = 99;
    CHECK(channel_list_import_buffer("<zello-channels/>", &count) == ESP_ERR_NOT_FOUND);
    CHECK(count == 0 && channel_list_find("retained", NULL));
    return 0;
}

static int import_limits(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    char xml[6000] = "<zello-channels>";
    for (unsigned i = 0; i <= CHANNEL_LIST_MAX; i++) {
        char entry[40];
        snprintf(entry, sizeof(entry), "<channel name='Channel %u'/>", i);
        strcat(xml, entry);
    }
    strcat(xml, "</zello-channels>");
    size_t count = 99;
    CHECK(channel_list_import_buffer(xml, &count) == ESP_ERR_INVALID_ARG);
    CHECK(count == 0 && channel_list_find("retained", NULL));
    channel_entry_t entries[] = {{.name = "new"}, {.name = ""}};
    CHECK(channel_list_replace(entries, 2, &count) == ESP_ERR_INVALID_ARG);
    CHECK(count == 0 && channel_list_count() == 1 && channel_list_find("retained", NULL));
    char long_entry[256];
    memset(long_entry, 'x', sizeof(long_entry));
    strcpy(xml, "<zello-channels><channel name='new' desc='");
    strncat(xml, long_entry, 64);
    strcat(xml, "'/></zello-channels>");
    CHECK(channel_list_import_buffer(xml, &count) == ESP_ERR_INVALID_ARG);
    CHECK(count == 0 && channel_list_find("retained", NULL));
    return 0;
}

static int valid_import(void)
{
    CHECK(channel_list_init() == ESP_OK);
    const char *xml =
        "<?xml version='1.0' encoding='utf-8' standalone='yes'?>"
        "<zello-channels version='1'><!-- <channel name='ignored'/> -->"
        "<channel name = 'Radio' desc = 'first > &amp; &#x1f4fb;' uses = '4294967295'>"
        "  <!-- empty body --> </channel>"
        "<channel name='radio' favorite='true'/>"
        "<channel name='Radio' desc='duplicate ignored'/>"
        "</zello-channels>";
    size_t count = 0;
    CHECK(channel_list_import_buffer(xml, &count) == ESP_OK);
    CHECK(count == 2 && channel_list_count() == 2);
    channel_entry_t entry;
    CHECK(channel_list_find("Radio", &entry));
    CHECK(!strcmp(entry.desc, "first > & \xf0\x9f\x93\xbb") && entry.uses == UINT32_MAX);
    CHECK(channel_list_find("radio", &entry) && entry.favorite);
    CHECK(!channel_list_find("ignored", NULL));
    CHECK(channel_list_init() == ESP_OK);
    CHECK(channel_list_find("Radio", &entry) && !strcmp(entry.desc, "first > & \xf0\x9f\x93\xbb"));
    return 0;
}

static int xml_document(void)
{
    const char *valid[] = {
        ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<scheff-backup version=\"1\">"
        "<settings username=\"a &amp; b\"/><channels><channel name=\"&lt;a&gt;\"/>"
        "</channels></scheff-backup>"),
        ("\xEF\xBB\xBF<!--before--><zello-channels><channel name = 'x' />"
        "</zello-channels><!--after-->"),
        "<zello-channels/>",
    };
    const char *invalid[] = {
        "", "not xml", "<root>", "<root><child/></wrong>", "<root/> <other/>",
        "<root x=\"a\" x=\"b\"/>", "<root x=\"&bogus;\"/>",
        "<root x=\"unterminated/>", "<root><child></root>",
        "<root/><!--", "<root><!--x--y--></root>", "<root/>junk",
        "<root x=\"a\"y=\"b\"/>", "<root x=\"<bad\"/>", "<root x='&#0;'/>",
        "<root x='&#xD800;'/>", "<root x='&#x110000;'/>", "<root>&amp</root>",
        "<?xml?><root/>", "<?xml encoding='UTF-8'?><root/>",
        "<?xml version='2.0'?><root/>", " <?xml version='1.0'?><root/>",
        "<!DOCTYPE root><root/>", "<root>\xc0\xaf</root>", "<root>\x01</root>",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++)
        CHECK(xml_validate_document(valid[i]) == ESP_OK);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        CHECK(xml_validate_document(invalid[i]) == ESP_ERR_INVALID_ARG);
    return 0;
}

static int channel_body(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    const char *invalid[] = {
        "<zello-channels><channel name='new'>unexpected data</channel></zello-channels>",
        "<zello-channels><channel name='new'><channel name='nested'/></channel></zello-channels>",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        CHECK(channel_list_import_buffer(invalid[i], NULL) == ESP_ERR_INVALID_ARG);
        CHECK(channel_list_count() == 1 && channel_list_find("retained", NULL));
    }
    return 0;
}

static int invalid_channel_text(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t old = {.name = "retained"};
    CHECK(channel_list_put(NULL, &old) == ESP_OK);
    channel_entry_t invalid[] = {
        {.name = "bad\x01"},
        {.name = "bad", .desc = "\xc0\xaf"},
        {.name = "bad", .desc = "\xed\xa0\x80"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        CHECK(channel_list_put("retained", &invalid[i]) != ESP_OK);
        CHECK(channel_list_count() == 1 && channel_list_find("retained", NULL));
        CHECK(channel_list_init() == ESP_OK);
        CHECK(channel_list_find("retained", NULL));
    }
    return 0;
}

static int exact_names(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t entries[] = {{.name = "Radio", .uses = 1}, {.name = "radio", .uses = 2}};
    size_t count;
    CHECK(channel_list_replace(entries, 2, &count) == ESP_OK);
    CHECK(count == 2 && channel_list_count() == 2);
    CHECK(!channel_list_find("RADIO", NULL));
    CHECK(channel_list_delete("Radio") == ESP_OK);
    channel_entry_t entry;
    CHECK(channel_list_find("radio", &entry) && entry.uses == 2);
    CHECK(!channel_list_find("Radio", NULL));
    return 0;
}

static int xml_attributes(void)
{
    const char *xml = "<channel desc=\"pretend name='fake'\" name = \"real &amp; exact\"/>";
    char name[64] = "";
    CHECK(xml_attr(xml, strchr(xml, '>'), "name", name, sizeof(name)) == 1);
    CHECK(!strcmp(name, "real & exact"));
    const char *document = "<root><!-- <channel name='ignored'/> --><channel name='real'/></root>";
    const char *element = xml_find_element(document, "channel");
    CHECK(element && xml_attr(element, strchr(element, '>'), "name", name, sizeof(name)) == 1);
    CHECK(!strcmp(name, "real"));
    return 0;
}

static int rename_collision(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t entries[] = {{.name = "one"}, {.name = "two", .desc = "unrelated"}};
    CHECK(channel_list_replace(entries, 2, NULL) == ESP_OK);
    channel_entry_t replacement = {.name = "two", .desc = "replacement"};
    CHECK(channel_list_put("one", &replacement) == ESP_ERR_INVALID_ARG);
    CHECK(channel_list_count() == 2 && channel_list_find("one", NULL));
    channel_entry_t entry;
    CHECK(channel_list_find("two", &entry) && !strcmp(entry.desc, "unrelated"));
    return 0;
}

static int large_roundtrip(void)
{
    CHECK(channel_list_init() == ESP_OK);
    channel_entry_t entries[CHANNEL_LIST_MAX] = {0};
    for (size_t i = 0; i < CHANNEL_LIST_MAX; i++) {
        memset(entries[i].name, '"', sizeof(entries[i].name) - 1);
        char number[4];
        snprintf(number, sizeof(number), "%03u", (unsigned)i);
        memcpy(entries[i].name, number, 3);
        memset(entries[i].desc, '\'', sizeof(entries[i].desc) - 1);
        entries[i].uses = UINT32_MAX;
    }
    CHECK(channel_list_replace(entries, CHANNEL_LIST_MAX, NULL) == ESP_OK);
    struct stat st;
    CHECK(stat(CHANNEL_LIST_PATH, &st) == 0 && st.st_size > 100 * 1024);
    // Make a distinct persisted fixture; a rejected reload must not pass by
    // leaving the list that was already resident in RAM.
    CHECK(rename(CHANNEL_LIST_PATH, "large.xml") == 0);
    channel_entry_t small = {.name = "small"};
    CHECK(channel_list_replace(&small, 1, NULL) == ESP_OK);
    CHECK(remove(CHANNEL_LIST_PATH) == 0);
    CHECK(rename("large.xml", CHANNEL_LIST_PATH) == 0);
    CHECK(channel_list_init() == ESP_OK);
    CHECK(channel_list_count() == CHANNEL_LIST_MAX);
    channel_entry_t entry;
    CHECK(channel_list_find(entries[127].name, &entry));
    CHECK(!strcmp(entry.desc, entries[127].desc) && entry.uses == UINT32_MAX);
    return 0;
}

static esp_err_t password_value(void *ctx, const char *section, int item,
                                const char *key, const char *value)
{
    (void)item;
    if (section && !strcmp(section, "settings") && key && !strcmp(key, "password"))
        strlcpy(ctx, value, 128);
    return ESP_OK;
}

static int yaml_suffix(void)
{
    const char *invalid[] = {
        "settings:\n  password: []junk\n",
        "settings:\n  password: {}junk\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        char password[128] = "retained";
        int line = 0;
        CHECK(yaml_parse(invalid[i], password_value, password, &line) == ESP_ERR_INVALID_ARG);
        CHECK(!strcmp(password, "retained") && line == 2);
    }
    return 0;
}

typedef struct {
    int sections;
    int values;
    char value[128];
} yaml_section_result_t;

static esp_err_t section_value(void *ctx, const char *section, int item,
                               const char *key, const char *value)
{
    (void)section;
    (void)item;
    yaml_section_result_t *result = ctx;
    if (!key) {
        result->sections++;
    } else {
        result->values++;
        strlcpy(result->value, value, sizeof(result->value));
    }
    return ESP_OK;
}

static int yaml_completed_flow(void)
{
    const char *invalid[] = {
        "channels: []\n  - name: New\n",
        "channels: {}\n  - name: New\n",
        "settings: {}\n  password: New\n",
        "settings: []\n  password: New\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        yaml_section_result_t result = {.value = "retained"};
        int line = 0;
        CHECK(yaml_parse(invalid[i], section_value, &result, &line) == ESP_ERR_INVALID_ARG);
        CHECK(line == 2 && result.sections == 1 && result.values == 0);
        CHECK(!strcmp(result.value, "retained"));
    }
    return 0;
}

static int yaml_sections(void)
{
    const struct { const char *text; int sections; int values; const char *value; } valid[] = {
        {"channels: [] # none\n", 1, 0, "retained"},
        {"settings: {}\n", 1, 0, "retained"},
        {"channels:\n", 1, 0, "retained"},
        {"settings:\n", 1, 0, "retained"},
        {"channels:\n  - name: New\n", 1, 1, "New"},
        {"settings:\n  password: New\n", 1, 1, "New"},
        {"channels: []\nsettings:\n  password: New\n", 2, 1, "New"},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        yaml_section_result_t result = {.value = "retained"};
        CHECK(yaml_parse(valid[i].text, section_value, &result, NULL) == ESP_OK);
        CHECK(result.sections == valid[i].sections && result.values == valid[i].values);
        CHECK(!strcmp(result.value, valid[i].value));
    }
    return 0;
}

static esp_err_t append_text(void *ctx, const char *text, size_t size)
{
    strncat(ctx, text, size);
    return ESP_OK;
}

static int yaml_valid(void)
{
    char password[128] = "old";
    CHECK(yaml_parse("settings:\n  password: [] # empty\n", password_value, password, NULL) == ESP_OK);
    CHECK(password[0] == 0);
    CHECK(yaml_parse("settings:\n  password: {}\n", password_value, password, NULL) == ESP_OK);
    CHECK(password[0] == 0);
    char yaml[512] = "settings:\n";
    CHECK(yaml_write_str(append_text, yaml, "  ", "password", "a\\b\"c\n\t: # &") == ESP_OK);
    CHECK(yaml_parse(yaml, password_value, password, NULL) == ESP_OK);
    CHECK(!strcmp(password, "a\\b\"c\n\t: # &"));
    return 0;
}

static int numeric_bounds(void)
{
    CHECK(settings_load() == ESP_OK);
    CHECK(settings_set("volume", "100") == ESP_OK);
    CHECK(settings_set("mic_gain", "42") == ESP_OK);
    CHECK(settings_set("volume", "101") == ESP_ERR_INVALID_ARG);
    CHECK(settings_set("mic_gain", "43") == ESP_ERR_INVALID_ARG);
    CHECK(settings_set("volume", "") == ESP_ERR_INVALID_ARG);
    CHECK(settings_set("mic_gain", "-1") == ESP_ERR_INVALID_ARG);
    CHECK(settings_set("volume", "999999999999999999999") == ESP_ERR_INVALID_ARG);
    CHECK(g_settings.volume == 100 && g_settings.mic_gain == 42);
    CHECK(settings_set("volume", "0") == ESP_OK);
    CHECK(settings_set("mic_gain", "0") == ESP_OK);
    return 0;
}

static int snapshot_failure(void)
{
    CHECK(settings_load() == ESP_OK);
    CHECK(settings_set("username", "old-user") == ESP_OK);
    CHECK(settings_set("password", "old-password") == ESP_OK);
    CHECK(settings_save() == ESP_OK);
    CHECK(settings_set("username", "new-user") == ESP_OK);
    CHECK(settings_set("password", "new-password") == ESP_OK);
    storage_fail_nvs_save = true;
    CHECK(settings_save() != ESP_OK);
    storage_fail_nvs_save = false;
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.username, "old-user"));
    CHECK(!strcmp(g_settings.password, "old-password"));
    return 0;
}

static int migration(void)
{
    CHECK(nvs_set_str(1, "username", "legacy-user") == ESP_OK);
    CHECK(nvs_set_str(1, "password", "legacy-password") == ESP_OK);
    CHECK(nvs_set_u8(1, "volume", 19) == ESP_OK);
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.username, "legacy-user") && g_settings.volume == 19);
    CHECK(!strcmp(g_settings.password, "legacy-password"));
    CHECK(nvs_set_str(1, "username", "stale-legacy") == ESP_OK);
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.username, "legacy-user"));
    return 0;
}

static int migration_failure(void)
{
    CHECK(nvs_set_str(1, "password", "legacy-password") == ESP_OK);
    strlcpy(g_settings.password, "live-password", sizeof(g_settings.password));
    storage_fail_nvs_save = true;
    CHECK(settings_load() != ESP_OK);
    CHECK(!strcmp(g_settings.password, "live-password"));
    storage_fail_nvs_save = false;
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.password, "legacy-password"));
    return 0;
}

static int migrate_legacy_numeric(uint8_t volume, uint8_t mic_gain)
{
    CHECK(nvs_set_str(1, "username", "legacy-user") == ESP_OK);
    CHECK(nvs_set_str(1, "password", "legacy-password") == ESP_OK);
    CHECK(nvs_set_str(1, "auth_token", "legacy-token") == ESP_OK);
    CHECK(nvs_set_str(1, "work_network", "legacy-work") == ESP_OK);
    CHECK(nvs_set_str(1, "channel", "Legacy channel") == ESP_OK);
    CHECK(nvs_set_str(1, "wifi_ssid", "legacy-ssid") == ESP_OK);
    CHECK(nvs_set_str(1, "wifi_pass", "legacy-wifi") == ESP_OK);
    CHECK(nvs_set_u8(1, "network", ZELLO_NET_WORK) == ESP_OK);
    CHECK(nvs_set_u8(1, "volume", volume) == ESP_OK);
    CHECK(nvs_set_u8(1, "mic_gain", mic_gain) == ESP_OK);
    CHECK(nvs_set_u8(1, "auto_connect", 0) == ESP_OK);
    CHECK(nvs_set_u8(1, "ptt_latch", 1) == ESP_OK);
    CHECK(settings_load() == ESP_OK);
    CHECK(g_settings.volume == 100 && g_settings.mic_gain == 42);
    CHECK(!strcmp(g_settings.username, "legacy-user"));
    CHECK(!strcmp(g_settings.password, "legacy-password"));
    CHECK(!strcmp(g_settings.auth_token, "legacy-token"));
    CHECK(!strcmp(g_settings.work_network, "legacy-work"));
    CHECK(!strcmp(g_settings.channel, "Legacy channel"));
    CHECK(!strcmp(g_settings.wifi_ssid, "legacy-ssid"));
    CHECK(!strcmp(g_settings.wifi_pass, "legacy-wifi"));
    CHECK(g_settings.network == ZELLO_NET_WORK && !g_settings.auto_connect && g_settings.ptt_latch);
    CHECK(settings_set("volume", "101") == ESP_ERR_INVALID_ARG);
    CHECK(settings_set("mic_gain", "43") == ESP_ERR_INVALID_ARG);
    CHECK(nvs_set_str(1, "password", "stale-legacy") == ESP_OK);
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.password, "legacy-password"));
    CHECK(g_settings.volume == 100 && g_settings.mic_gain == 42);
    g_settings.volume = 101;
    CHECK(settings_save() == ESP_ERR_INVALID_ARG);
    CHECK(settings_load() == ESP_OK && g_settings.volume == 100);
    g_settings.mic_gain = 43;
    CHECK(settings_save() == ESP_ERR_INVALID_ARG);
    CHECK(settings_load() == ESP_OK && g_settings.mic_gain == 42);
    return 0;
}

static int legacy_numeric_migration(void) { return migrate_legacy_numeric(101, 43); }
static int legacy_numeric_max(void) { return migrate_legacy_numeric(255, 255); }

static int invalid_legacy(void)
{
    char too_long[80];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    CHECK(nvs_set_str(1, "username", too_long) == ESP_OK);
    strlcpy(g_settings.username, "live-user", sizeof(g_settings.username));
    CHECK(settings_load() == ESP_ERR_NVS_INVALID_LENGTH);
    CHECK(!strcmp(g_settings.username, "live-user"));
    CHECK(nvs_set_str(1, "username", "legacy-user") == ESP_OK);
    CHECK(nvs_set_str(1, "volume", "101") == ESP_OK);
    CHECK(settings_load() == ESP_ERR_NVS_TYPE_MISMATCH);
    CHECK(!strcmp(g_settings.username, "live-user"));
    CHECK(nvs_set_u8(1, "volume", 100) == ESP_OK);
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.username, "legacy-user") && g_settings.volume == 100);
    return 0;
}

static int invalid_snapshot(void)
{
    CHECK(settings_load() == ESP_OK);
    CHECK(settings_set("password", "retained") == ESP_OK);
    CHECK(settings_save() == ESP_OK);
    CHECK(storage_corrupt_snapshot());
    CHECK(settings_load() != ESP_OK);
    CHECK(!strcmp(g_settings.password, "retained"));
    return 0;
}

static int nvs_init_failure(void)
{
    CHECK(nvs_set_str(1, "password", "retained") == ESP_OK);
    storage_nvs_init_error = ESP_ERR_NVS_NO_FREE_PAGES;
    CHECK(settings_load() == ESP_ERR_NVS_NO_FREE_PAGES);
    storage_nvs_init_error = ESP_OK;
    CHECK(settings_load() == ESP_OK);
    CHECK(!strcmp(g_settings.password, "retained"));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    struct { const char *name; int (*run)(void); } tests[] = {
        {"recover", recover}, {"promote", promote},
        {"channel-save-failure", channel_save_failure}, {"malformed-import", malformed_import},
        {"channel-recovery", channel_recovery}, {"import-limits", import_limits},
        {"valid-import", valid_import},
        {"channel-body", channel_body}, {"invalid-channel-text", invalid_channel_text},
        {"xml-document", xml_document}, {"xml-attributes", xml_attributes}, {"exact-names", exact_names},
        {"rename-collision", rename_collision}, {"large-roundtrip", large_roundtrip},
        {"yaml-suffix", yaml_suffix}, {"yaml-valid", yaml_valid},
        {"yaml-completed-flow", yaml_completed_flow}, {"yaml-sections", yaml_sections},
        {"numeric-bounds", numeric_bounds}, {"snapshot-failure", snapshot_failure},
        {"migration", migration}, {"invalid-snapshot", invalid_snapshot},
        {"migration-failure", migration_failure}, {"invalid-legacy", invalid_legacy},
        {"legacy-numeric-migration", legacy_numeric_migration}, {"legacy-numeric-max", legacy_numeric_max},
        {"nvs-init-failure", nvs_init_failure},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); i++)
        if (!strcmp(tests[i].name, argv[1])) return tests[i].run();
    return 2;
}
