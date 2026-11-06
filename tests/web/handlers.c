#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "../../main/sdcard.h"
#undef SDCARD_MOUNT
#define SDCARD_MOUNT "sdcard"

static FILE *test_fopen(const char *path, const char *mode);
static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *file);
static int test_fflush(FILE *file);
static int test_fclose(FILE *file);

#define fopen test_fopen
#define fwrite test_fwrite
#define fflush test_fflush
#define fclose test_fclose
#include "../../main/web_config.c"
#undef fopen
#undef fwrite
#undef fflush
#undef fclose

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static channel_entry_t saved[CHANNEL_LIST_MAX];
static size_t saved_count;
static int settings_writes, fail_heap, fail_open, fail_write, fail_flush, fail_close;
static int sd_mount_calls, crash_deletions;
static TickType_t ticks;
settings_t g_settings;
const char index_html_start[] = "";
const char index_html_end[] = "";

static httpd_req_t request(const void *body, size_t length)
{
    httpd_req_t req = {.body = body, .content_len = length};
    strcpy(req.status, "200 OK");
    return req;
}

static httpd_req_t json_request(const char *body)
{
    return request(body, strlen(body));
}

static void reset(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.volume = 40;
    strcpy(g_settings.channel, "Keep");
    memset(saved, 0, sizeof(saved));
    saved_count = 1;
    strcpy(saved[0].name, "Keep");
    strcpy(saved[0].desc, "original note");
    saved[0].favorite = true;
    settings_writes = fail_heap = fail_open = fail_write = fail_flush = fail_close = 0;
    sd_mount_calls = crash_deletions = 0;
    ticks = 0;
    s_join.state = JOIN_IDLE;
}

static void long_identity(char *out)
{
    memset(out, 'a', 65);
    out[65] = 0;
    memcpy(saved[0].name, out, 64);
    saved[0].name[64] = 0;
}

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(text, 1, strlen(text), f) != strlen(text) || fclose(f)) abort();
}

static bool file_equals(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char data[4096];
    size_t size = fread(data, 1, sizeof(data), f);
    fclose(f);
    return size == strlen(text) && memcmp(data, text, size) == 0;
}

static cJSON *response(httpd_req_t *req)
{
    cJSON *j = cJSON_Parse(req->response);
    if (!j) abort();
    return j;
}

static const char settings_xml[] =
    "<settings version=\"1\" network=\"consumer\" username=\"restored-user\" "
    "password=\"pass\" auth_token=\"token\" work_network=\"\" channel=\"Keep\" "
    "wifi_ssid=\"\" wifi_pass=\"\" volume=\"61\" mic_gain=\"12\" "
    "auto_connect=\"true\" ptt_latch=\"false\"/>";

static int test_json_complete(void)
{
    static const char *invalid[] = {
        "{}trailing", "{}{}", "[]", "\"not an object\"", "null", "1",
        "{\"name\":\"Keep\\u0000hidden\"}", "{\"na\\u0000me\":\"Keep\"}",
        "{}\v", "{\"name\":\"Keep\"",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        httpd_req_t req = json_request(invalid[i]);
        cJSON *j = read_json(&req);
        if (j) {
            fprintf(stderr, "Accepted invalid JSON: %s\n", invalid[i]);
            cJSON_Delete(j);
            return 1;
        }
    }
    httpd_req_t valid = json_request("{\"name\":\"literal \\\\u0000\"} \t\r\n");
    cJSON *j = read_json(&valid);
    CHECK(cJSON_IsObject(j));
    CHECK(strcmp(json_str(j, "name"), "literal \\u0000") == 0);
    cJSON_Delete(j);
    return 0;
}

static int test_json_nul(void)
{
    static const char body[] = "{\"name\":\"Keep\"}\0{\"hidden\":true}";
    httpd_req_t req = request(body, sizeof(body) - 1);
    h_channel_delete(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(saved_count == 1 && strcmp(saved[0].name, "Keep") == 0);
    return 0;
}

static int test_trailing_json_no_mutation(void)
{
    httpd_req_t req = json_request("{\"name\":\"Keep\"}invalid");
    h_channel_delete(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(saved_count == 1);
    return 0;
}

static int test_timeout(bool upload, bool trickle)
{
    const char *text = upload ? "settings:\n  volume: 61\n" : "{\"name\":\"Keep\"}";
    httpd_req_t req = json_request(text);
    if (trickle) {
        req.chunk_size = 1;
        req.recv_ticks = 5000;
    } else {
        req.timeouts = 10;
    }
    req.handle = &req;
    esp_err_t result = upload ? h_import_upload(&req) : h_channel_delete(&req);
    CHECK(req.recv_calls <= 5);
    CHECK(result != ESP_OK);  // HTTPD must close, not purge the unread body.
    CHECK(req.close_header);
    CHECK(strncmp(req.status, "200", 3) != 0);
    CHECK(saved_count == 1 && settings_writes == 0);
    return 0;
}

static int test_body_lengths(void)
{
    httpd_req_t too_large = request("{}", BODY_MAX + 1);
    too_large.handle = &too_large;
    CHECK(h_channel_delete(&too_large) != ESP_OK);
    CHECK(too_large.recv_calls == 0 && too_large.close_header);
    httpd_req_t incomplete = request("{", 20);
    incomplete.chunk_size = 1;
    incomplete.handle = &incomplete;
    incomplete.body = NULL;
    CHECK(h_channel_delete(&incomplete) != ESP_OK);
    CHECK(incomplete.close_header);
    return 0;
}

static int test_upload_nul(void)
{
    static const char body[] = "settings:\n  volume: 61\n\0channels: []";
    httpd_req_t req = request(body, sizeof(body) - 1);
    req.handle = &req;
    h_import_upload(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(g_settings.volume == 40 && settings_writes == 0);
    return 0;
}

static int test_fragmented_body(void)
{
    httpd_req_t req = json_request("{\"name\":\"  Café  \"} \t\r\n");
    req.chunk_size = 3;
    req.timeouts = 2;
    cJSON *j = read_json(&req);
    CHECK(j && strcmp(json_str(j, "name"), "  Café  ") == 0);
    CHECK(req.offset == req.content_len);
    cJSON_Delete(j);
    return 0;
}

static int test_identifier(const char *action)
{
    char name[66], body[400];
    long_identity(name);
    if (!strcmp(action, "orig"))
        snprintf(body, sizeof(body), "{\"orig_name\":\"%s\",\"name\":\"Renamed\",\"desc\":\"\"}", name);
    else if (!strcmp(action, "desc"))
        snprintf(body, sizeof(body), "{\"orig_name\":\"%s\",\"name\":\"%s\",\"desc\":\"%s\"}",
                 saved[0].name, saved[0].name, name);
    else
        snprintf(body, sizeof(body), "{\"name\":\"%s\",\"favorite\":false}", name);
    httpd_req_t req = json_request(body);
    if (!strcmp(action, "delete")) h_channel_delete(&req);
    else if (!strcmp(action, "favorite")) h_channel_favorite(&req);
    else if (!strcmp(action, "join")) h_channel_join(&req);
    else h_channel_save(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(saved_count == 1 && strlen(saved[0].name) == 64);
    CHECK(saved[0].favorite && strcmp(saved[0].desc, "original note") == 0);
    CHECK(strcmp(g_settings.channel, "Keep") == 0);
    return 0;
}

static int test_invalid_types(void)
{
    static const struct {
        esp_err_t (*handler)(httpd_req_t *);
        const char *body;
    } cases[] = {
        {h_channel_save, "{\"orig_name\":42,\"name\":\"New\"}"},
        {h_channel_save, "{\"name\":\"Keep\",\"desc\":true}"},
        {h_channel_save, "{\"name\":42}"},
        {h_channel_delete, "{\"name\":false}"},
        {h_channel_favorite, "{\"name\":\"Keep\",\"favorite\":\"true\"}"},
        {h_channel_favorite, "{\"name\":\"Keep\"}"},
        {h_channel_join, "{\"name\":[]}"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        httpd_req_t req = json_request(cases[i].body);
        cases[i].handler(&req);
        CHECK(strncmp(req.status, "400", 3) == 0);
        CHECK(saved_count == 1 && saved[0].favorite);
        CHECK(strcmp(saved[0].name, "Keep") == 0);
    }
    return 0;
}

static int test_exact_names(void)
{
    httpd_req_t req = json_request("{\"name\":\"  Café  \",\"desc\":\"note\"}");
    h_channel_save(&req);
    CHECK(strncmp(req.status, "200", 3) == 0);
    CHECK(channel_list_find("  Café  ", NULL));
    char name[67], body[256];
    for (int i = 0; i < 33; i++) memcpy(name + i * 2, "\xc3\xa9", 2);
    name[66] = 0;
    snprintf(body, sizeof(body), "{\"name\":\"%s\"}", name);
    req = json_request(body);
    h_channel_save(&req);
    CHECK(strncmp(req.status, "400", 3) == 0 && saved_count == 2);
    name[64] = 0;
    snprintf(body, sizeof(body), "{\"name\":\"%s\"}", name);
    req = json_request(body);
    h_channel_save(&req);
    CHECK(strncmp(req.status, "200", 3) == 0);
    CHECK(channel_list_find(name, NULL));
    return 0;
}

static int test_query_validation(void)
{
    static const char *queries[] = {"q=%00Keep", "q=bad%", "q=bad%XZ"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(*queries); i++) {
        httpd_req_t req = json_request("");
        req.query = queries[i];
        h_channels(&req);
        CHECK(strncmp(req.status, "400", 3) == 0);
    }
    char query[80] = "q=";
    memset(query + 2, 'a', 65);
    query[67] = 0;
    httpd_req_t req = json_request("");
    req.query = query;
    h_channels(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    return 0;
}

static int test_query_utf8(void)
{
    char query[200] = "q=";
    for (int i = 0; i < 32; i++) {
        memcpy(saved[0].name + i * 2, "\xc3\xa9", 2);
        memcpy(query + 2 + i * 6, "%C3%A9", 6);
    }
    saved[0].name[64] = 0;
    query[194] = 0;
    strcpy(saved[1].name, "Not a match");
    saved_count = 2;
    httpd_req_t req = json_request("");
    req.query = query;
    h_channels(&req);
    CHECK(strncmp(req.status, "200", 3) == 0);
    cJSON *j = response(&req);
    CHECK(cJSON_GetArraySize(j) == 1);
    CHECK(strcmp(json_str(cJSON_GetArrayItem(j, 0), "name"), saved[0].name) == 0);
    cJSON_Delete(j);
    return 0;
}

static int test_current_identity(void)
{
    strcpy(saved[1].name, "keep");
    saved_count = 2;
    httpd_req_t req = json_request("");
    h_channels(&req);
    cJSON *j = response(&req);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(j, 0), "current")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(j, 1), "current")));
    cJSON_Delete(j);
    return 0;
}

static int test_settings_xml(bool empty_channels)
{
    char xml[2048];
    snprintf(xml, sizeof(xml), "<zello-backup>%s%s</zello-backup>", settings_xml,
             empty_channels ? "<channels/>" : "");
    httpd_req_t req = json_request("");
    { import_outcome_t o; import_backup(xml, &o); send_import(&req, &o, "test"); }
    CHECK(strncmp(req.status, "200", 3) == 0);
    CHECK(g_settings.volume == 61 && strcmp(g_settings.username, "restored-user") == 0);
    CHECK(saved_count == 1 && strcmp(saved[0].name, "Keep") == 0);
    cJSON *j = response(&req);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(j, "settings")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "channels")));
    CHECK(cJSON_GetObjectItem(j, "count")->valueint == 1);
    cJSON_Delete(j);
    return 0;
}

static int test_malformed_xml(void)
{
    char xml[2048];
    snprintf(xml, sizeof(xml), "<zello-backup>%s<channel name=\"New\"/></zello-backup>garbage",
             settings_xml);
    httpd_req_t req = json_request("");
    { import_outcome_t o; import_backup(xml, &o); send_import(&req, &o, "test"); }
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(settings_writes == 0 && g_settings.volume == 40);
    CHECK(saved_count == 1 && strcmp(saved[0].name, "Keep") == 0);
    return 0;
}

static int test_xml_quoted_settings(void)
{
    char xml[2048];
    snprintf(xml, sizeof(xml), "<zello-backup>%s</zello-backup>", settings_xml);
    char *username = strstr(xml, "restored-user");
    memcpy(username, "user>network1", 13);
    httpd_req_t req = json_request("");
    { import_outcome_t o; import_backup(xml, &o); send_import(&req, &o, "test"); }
    CHECK(strncmp(req.status, "200", 3) == 0);
    CHECK(strcmp(g_settings.username, "user>network1") == 0);
    CHECK(saved_count == 1 && strcmp(saved[0].name, "Keep") == 0);
    return 0;
}

static int test_large_valid_xml(void)
{
    const size_t capacity = 140000;
    char *xml = calloc(1, capacity);
    CHECK(xml);
    char *p = xml;
#define APPEND(...) do { \
    size_t remaining = capacity - (size_t)(p - xml); \
    int n = snprintf(p, remaining, __VA_ARGS__); \
    CHECK(n >= 0 && (size_t)n < remaining); \
    p += n; \
} while (0)
    APPEND("<zello-channels version=\"1\">");
    for (unsigned i = 0; i < 128; i++) {
        APPEND("<channel name=\"%03u", i);
        for (unsigned j = 0; j < 61; j++) APPEND("&quot;");
        APPEND("\" desc=\"");
        for (unsigned j = 0; j < 63; j++) APPEND("&quot;");
        APPEND("\" favorite=\"false\" uses=\"4294967295\"/>");
    }
    APPEND("</zello-channels>");
#undef APPEND
    size_t length = (size_t)(p - xml);
    CHECK(length > 100 * 1024);
    httpd_req_t req = request(xml, length);
    req.chunk_size = 1024;
    h_import_upload(&req);
    CHECK(strncmp(req.status, "200", 3) == 0 && req.offset == length);
    CHECK(strlen(saved[0].name) == 64 && !strncmp(saved[0].name, "000\"", 4));
    mkdir("sdcard", 0700);
    put_file(SD_LEGACY_BACKUP_PATH, xml);
    char *loaded = NULL;
    CHECK(backup_read_file(SD_LEGACY_BACKUP_PATH, &loaded) == ESP_OK);
    CHECK(strlen(loaded) == length && strcmp(loaded, xml) == 0);
    free(loaded);
    free(xml);
    return 0;
}

static int test_yaml_capacity(unsigned count, bool invalid_excess)
{
    char yaml[16384];
    size_t used = (size_t)snprintf(yaml, sizeof(yaml),
                                 "scheff-backup: 1\nsettings:\n  volume: 61\nchannels:\n");
    for (unsigned i = 0; i < count; i++) {
        int n = snprintf(yaml + used, sizeof(yaml) - used,
                         "  - name: \"Channel %03u\"\n    favorite: %s\n    uses: 3\n",
                         i, invalid_excess && i == 128 ? "not-a-boolean" : "true");
        CHECK(n > 0 && (size_t)n < sizeof(yaml) - used);
        used += (size_t)n;
    }
    httpd_req_t req = request(yaml, used);
    h_import_upload(&req);
    if (count > 128) {
        if (strncmp(req.status, "400", 3)) {
            fprintf(stderr, "%u YAML entries: HTTP %s, %zu stored channels, volume %u\n",
                    count, req.status, saved_count, g_settings.volume);
        }
        CHECK(strncmp(req.status, "400", 3) == 0);
        CHECK(saved_count == 1 && !strcmp(saved[0].name, "Keep"));
        CHECK(g_settings.volume == 40 && settings_writes == 0);
    } else {
        CHECK(strncmp(req.status, "200", 3) == 0 && saved_count == count);
        CHECK(!strcmp(saved[0].name, "Channel 000") && saved[0].favorite && saved[0].uses == 3);
        if (count == 128) CHECK(!strcmp(saved[127].name, "Channel 127"));
        CHECK(g_settings.volume == 61 && settings_writes == 1);
    }
    return 0;
}

static int test_sd_request_body(bool importing, const char *kind)
{
    static const char backup[] = "settings:\n  volume: 61\n";
    mkdir("sdcard", 0700);
    put_file(SD_BACKUP_PATH, backup);
    put_file(SD_BACKUP_PATH ".bak", "previous generation");
    put_file(SD_BACKUP_PATH ".tmp", "unfinished");
    httpd_req_t req = json_request("{} \r\n");
    if (!strcmp(kind, "timeout")) req.timeouts = 10;
    else if (!strcmp(kind, "trickle")) {
        req.chunk_size = 1;
        req.recv_ticks = 5000;
    } else if (!strcmp(kind, "oversize")) req.content_len = BODY_MAX + 1;
    else req = json_request("{\"unsupported\":true}");
    esp_err_t result = importing ? h_sd_import(&req) : h_sd_export(&req);
    if (result == ESP_OK) {
        fprintf(stderr, "SD %s %s: returned success, consumed %zu/%zu bytes, mounted %d times\n",
                importing ? "import" : "export", kind, req.offset, req.content_len, sd_mount_calls);
    }
    CHECK(result != ESP_OK && req.close_header);
    CHECK(strncmp(req.status, "200", 3) != 0 && req.recv_calls <= 5);
    if (!strcmp(kind, "oversize")) CHECK(req.recv_calls == 0);
    CHECK(sd_mount_calls == 0 && settings_writes == 0 && g_settings.volume == 40);
    CHECK(saved_count == 1 && !strcmp(saved[0].name, "Keep"));
    CHECK(file_equals(SD_BACKUP_PATH, backup));
    CHECK(file_equals(SD_BACKUP_PATH ".bak", "previous generation"));
    CHECK(file_equals(SD_BACKUP_PATH ".tmp", "unfinished"));
    return 0;
}

static int test_sd_supported_bodies(bool importing)
{
    static const char *bodies[] = {"", "{}", " \t{}\r\n"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(*bodies); i++) {
        reset();
        mkdir("sdcard", 0700);
        put_file(SD_BACKUP_PATH, "settings:\n  volume: 61\n");
        httpd_req_t req = json_request(bodies[i]);
        esp_err_t result = importing ? h_sd_import(&req) : h_sd_export(&req);
        CHECK(result == ESP_OK && !strncmp(req.status, "200", 3));
        CHECK(req.offset == req.content_len && !req.close_header);
        if (importing) CHECK(g_settings.volume == 61 && settings_writes == 1);
        else {
            char *backup = NULL;
            CHECK(backup_read_file(SD_BACKUP_PATH, &backup) == ESP_OK);
            CHECK(strstr(backup, "scheff-backup: 1") && strstr(backup, "volume: 40"));
            free(backup);
        }
    }
    return 0;
}

static int test_bodyless_get(bool nonempty)
{
    httpd_req_t req = json_request(nonempty ? "{}" : "");
    req.timeouts = 10;
    esp_err_t result = h_status(&req);
    CHECK(req.recv_calls == 0);
    if (nonempty) {
        CHECK(result != ESP_OK && req.close_header && !strncmp(req.status, "400", 3));
    } else {
        CHECK(result == ESP_OK && !req.close_header && !strncmp(req.status, "200", 3));
        cJSON *j = response(&req);
        CHECK(!strcmp(json_str(cJSON_GetObjectItem(j, "zello"), "channel"), "Keep"));
        cJSON_Delete(j);
    }
    return 0;
}

static int test_crash_delete_body(bool stalled)
{
    httpd_req_t req = json_request("{}");
    if (stalled) req.timeouts = 10;
    esp_err_t result = h_crash_delete(&req);
    if (stalled) {
        CHECK(result != ESP_OK && req.close_header && req.recv_calls <= 5);
        CHECK(crash_deletions == 0);
    } else {
        CHECK(result == ESP_OK && req.offset == req.content_len);
        CHECK(crash_deletions == 1);
    }
    return 0;
}

static int test_sd_export(const char *failure)
{
    mkdir("sdcard", 0700);
    remove(SD_BACKUP_PATH);
    remove(SD_BACKUP_PATH ".bak");
    remove(SD_BACKUP_PATH ".tmp");
    put_file(SD_BACKUP_PATH, "last good backup");
    fail_heap = !strcmp(failure, "malloc");
    fail_open = !strcmp(failure, "open");
    fail_write = !strcmp(failure, "write");
    fail_flush = !strcmp(failure, "flush");
    fail_close = !strcmp(failure, "close");
    httpd_req_t req = json_request("{}");
    h_sd_export(&req);
    CHECK(strncmp(req.status, "500", 3) == 0);
    CHECK(file_equals(SD_BACKUP_PATH, "last good backup"));
    CHECK(fopen(SD_BACKUP_PATH ".tmp", "rb") == NULL);
    return 0;
}

static int test_sd_success(void)
{
    mkdir("sdcard", 0700);
    put_file(SD_BACKUP_PATH, "previous");
    httpd_req_t req = json_request("{}");
    h_sd_export(&req);
    CHECK(strncmp(req.status, "200", 3) == 0);
    char *text = NULL;
    CHECK(backup_read_file(SD_BACKUP_PATH, &text) == ESP_OK);
    CHECK(strstr(text, "scheff-backup: 1") && strstr(text, "\"Keep\""));
    free(text);
    return 0;
}

static int test_sd_recovery(bool exporting)
{
    mkdir("sdcard", 0700);
    remove(SD_BACKUP_PATH);
    put_file(SD_BACKUP_PATH ".bak", "last good backup");
    put_file(SD_BACKUP_PATH ".tmp", "incomplete");
    if (exporting) {
        fail_heap = 1;
        httpd_req_t req = json_request("{}");
        h_sd_export(&req);
        CHECK(strncmp(req.status, "500", 3) == 0);
        CHECK(file_equals(SD_BACKUP_PATH, "last good backup"));
    } else {
        char *text = NULL;
        CHECK(backup_read_file(SD_BACKUP_PATH, &text) == ESP_OK);
        CHECK(strcmp(text, "last good backup") == 0);
        free(text);
    }
    return 0;
}

static int test_sd_nul(void)
{
    mkdir("sdcard", 0700);
    FILE *f = fopen(SD_BACKUP_PATH, "wb");
    static const char body[] = "settings:\n  volume: 61\n\0ignored";
    CHECK(f && fwrite(body, 1, sizeof(body) - 1, f) == sizeof(body) - 1);
    fclose(f);
    httpd_req_t req = json_request("{}");
    h_sd_import(&req);
    CHECK(strncmp(req.status, "400", 3) == 0);
    CHECK(settings_writes == 0 && g_settings.volume == 40);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    reset();
    const char *name = argv[1];
    if (!strcmp(name, "json-complete")) return test_json_complete();
    if (!strcmp(name, "json-nul")) return test_json_nul();
    if (!strcmp(name, "json-no-mutation")) return test_trailing_json_no_mutation();
    if (!strcmp(name, "json-timeout")) return test_timeout(false, false);
    if (!strcmp(name, "upload-timeout")) return test_timeout(true, false);
    if (!strcmp(name, "json-trickle")) return test_timeout(false, true);
    if (!strcmp(name, "upload-trickle")) return test_timeout(true, true);
    if (!strcmp(name, "body-lengths")) return test_body_lengths();
    if (!strcmp(name, "upload-nul")) return test_upload_nul();
    if (!strcmp(name, "body-fragmented")) return test_fragmented_body();
    if (!strncmp(name, "identifier-", 11)) return test_identifier(name + 11);
    if (!strcmp(name, "invalid-types")) return test_invalid_types();
    if (!strcmp(name, "exact-names")) return test_exact_names();
    if (!strcmp(name, "query-validation")) return test_query_validation();
    if (!strcmp(name, "query-utf8")) return test_query_utf8();
    if (!strcmp(name, "current-identity")) return test_current_identity();
    if (!strcmp(name, "xml-settings")) return test_settings_xml(false);
    if (!strcmp(name, "xml-empty-channels")) return test_settings_xml(true);
    if (!strcmp(name, "xml-malformed")) return test_malformed_xml();
    if (!strcmp(name, "xml-quoted-settings")) return test_xml_quoted_settings();
    if (!strcmp(name, "xml-large-valid")) return test_large_valid_xml();
    if (!strcmp(name, "yaml-capacity-one")) return test_yaml_capacity(1, false);
    if (!strcmp(name, "yaml-capacity-full")) return test_yaml_capacity(128, false);
    if (!strcmp(name, "yaml-overflow")) return test_yaml_capacity(129, false);
    if (!strcmp(name, "yaml-overflow-invalid")) return test_yaml_capacity(129, true);
    if (!strncmp(name, "sd-import-body-", 15)) return test_sd_request_body(true, name + 15);
    if (!strncmp(name, "sd-export-body-", 15)) return test_sd_request_body(false, name + 15);
    if (!strcmp(name, "sd-import-supported")) return test_sd_supported_bodies(true);
    if (!strcmp(name, "sd-export-supported")) return test_sd_supported_bodies(false);
    if (!strcmp(name, "get-unexpected-body")) return test_bodyless_get(true);
    if (!strcmp(name, "get-empty-body")) return test_bodyless_get(false);
    if (!strcmp(name, "crash-delete-timeout")) return test_crash_delete_body(true);
    if (!strcmp(name, "crash-delete-supported")) return test_crash_delete_body(false);
    if (!strncmp(name, "sd-failure-", 11)) return test_sd_export(name + 11);
    if (!strcmp(name, "sd-success")) return test_sd_success();
    if (!strcmp(name, "sd-recover-read")) return test_sd_recovery(false);
    if (!strcmp(name, "sd-recover-export")) return test_sd_recovery(true);
    if (!strcmp(name, "sd-nul")) return test_sd_nul();
    fprintf(stderr, "Unknown test: %s\n", name);
    return 2;
}

static FILE *test_fopen(const char *path, const char *mode)
{
    if (fail_open && strstr(path, ".tmp") && strchr(mode, 'w')) {
        errno = EIO;
        return NULL;
    }
    return fopen(path, mode);
}

static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (fail_write && count) return fwrite(data, size, count - 1, file);
    return fwrite(data, size, count, file);
}

static int test_fflush(FILE *file)
{
    int result = fflush(file);
    return fail_flush ? EOF : result;
}

static int test_fclose(FILE *file)
{
    int result = fclose(file);
    return fail_close ? EOF : result;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    if (fail_heap) return NULL;
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    if (fail_heap) return NULL;
    return calloc(count, size);
}

int httpd_req_recv(httpd_req_t *req, char *out, size_t size)
{
    req->recv_calls++;
    ticks += req->recv_ticks;
    if (req->timeouts > 0) {req->timeouts--; return HTTPD_SOCK_ERR_TIMEOUT;}
    if (!req->body) return 0;
    if (size > req->content_len - req->offset) size = req->content_len - req->offset;
    if (req->chunk_size && size > req->chunk_size) size = req->chunk_size;
    memcpy(out, req->body + req->offset, size);
    req->offset += size;
    return (int)size;
}

size_t httpd_req_get_url_query_len(httpd_req_t *req) {return req->query ? strlen(req->query) : 0;}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *out, size_t size)
{
    if (!req->query) return ESP_ERR_NOT_FOUND;
    return strlcpy(out, req->query, size) < size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
}

esp_err_t httpd_query_key_value(const char *query, const char *key, char *out, size_t size)
{
    for (const char *p = query; p && *p; p = strchr(p, '&'), p = p ? p + 1 : NULL) {
        size_t key_len = strlen(key);
        if (strncmp(p, key, key_len) || p[key_len] != '=') continue;
        p += key_len + 1;
        const char *end = strchr(p, '&');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= size) {
            memcpy(out, p, size - 1);
            out[size - 1] = 0;
            return ESP_ERR_HTTPD_RESULT_TRUNC;
        }
        memcpy(out, p, n);
        out[n] = 0;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    strlcpy(req->status, status, sizeof(req->status));
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type) {(void)req; (void)type; return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{
    if (!strcmp(name, "Connection") && !strcmp(value, "close")) req->close_header = true;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, ssize_t size)
{
    if (size < 0) size = (ssize_t)strlen(data);
    if ((size_t)size >= sizeof(req->response)) abort();
    memcpy(req->response, data, size);
    req->response[size] = 0;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *data) {return httpd_resp_send(req, data, -1);}
esp_err_t httpd_resp_send_500(httpd_req_t *req) {return httpd_resp_set_status(req, "500 Internal Server Error");}
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, ssize_t size)
{
    (void)req; (void)data; (void)size; return ESP_OK;
}
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    (void)handle; (void)config; return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle; (void)uri; return ESP_OK;
}

size_t channel_list_count(void) {return saved_count;}
size_t channel_list_favorite_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < saved_count; i++) count += saved[i].favorite;
    return count;
}
size_t channel_list_get(const char *query, bool favorites_only, channel_entry_t *out, size_t max)
{
    size_t count = 0;
    for (size_t i = 0; i < saved_count && count < max; i++) {
        if (favorites_only && !saved[i].favorite) continue;
        if (query && *query && !strstr(saved[i].name, query) && !strstr(saved[i].desc, query)) continue;
        out[count++] = saved[i];
    }
    return count;
}
bool channel_list_find(const char *name, channel_entry_t *out)
{
    for (size_t i = 0; i < saved_count; i++) {
        if (strcmp(name, saved[i].name)) continue;
        if (out) *out = saved[i];
        return true;
    }
    return false;
}
esp_err_t channel_list_put(const char *original, const channel_entry_t *entry)
{
    for (size_t i = 0; i < saved_count; i++) {
        if (!strcmp(saved[i].name, original ? original : entry->name)) {
            saved[i] = *entry;
            return ESP_OK;
        }
    }
    if (saved_count == CHANNEL_LIST_MAX) return ESP_ERR_NO_MEM;
    saved[saved_count++] = *entry;
    return ESP_OK;
}
esp_err_t channel_list_delete(const char *name)
{
    for (size_t i = 0; i < saved_count; i++) {
        if (strcmp(name, saved[i].name)) continue;
        memmove(saved + i, saved + i + 1, (--saved_count - i) * sizeof(*saved));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t channel_list_set_favorite(const char *name, bool favorite)
{
    for (size_t i = 0; i < saved_count; i++) {
        if (strcmp(name, saved[i].name)) continue;
        saved[i].favorite = favorite;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t channel_list_replace(const channel_entry_t *entries, size_t count, size_t *out)
{
    if (!count) return ESP_ERR_NOT_FOUND;
    memcpy(saved, entries, count * sizeof(*saved));
    saved_count = count;
    if (out) *out = count;
    return ESP_OK;
}
esp_err_t channel_list_import_buffer(const char *xml, size_t *count)
{
    esp_err_t err = xml_validate_document(xml);
    if (err != ESP_OK) return err;
    const char *element = xml_find_element(xml, "channel");
    if (!element) return ESP_ERR_NOT_FOUND;
    channel_entry_t entry = {0};
    const char *end = xml_element_end(element);
    if (!end || xml_attr(element, end, "name", entry.name, sizeof(entry.name)) != 1)
        return ESP_ERR_INVALID_ARG;
    return channel_list_replace(&entry, 1, count);
}

const char *esp_err_to_name(esp_err_t err) {(void)err; return "test error";}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)entry; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(TickType_t delay) {ticks += delay;}
void vTaskDelete(void *handle) {(void)handle;}
TickType_t xTaskGetTickCount(void) {return ticks;}
esp_err_t settings_save(void) {settings_writes++; return ESP_OK;}
void audio_set_volume(int volume) {(void)volume;}
void audio_set_mic_gain(int gain) {(void)gain;}
void zello_reconfigure(void) {}
void zello_get_status(zello_status_t *out) {memset(out, 0, sizeof(*out));}
void zello_get_log(char *out, size_t size) {if (size) out[0] = 0;}
esp_err_t zello_send_text(const char *text) {(void)text; return ESP_OK;}
esp_err_t net_wifi_connect(const char *ssid, const char *pass) {(void)ssid; (void)pass; return ESP_OK;}
bool net_wifi_sta_ip(char *out, size_t size) {if (size) out[0] = 0; return false;}
bool net_wifi_ap_active(void) {return false;}
bool net_wifi_has_ip(void) {return false;}
const char *net_wifi_ap_ssid(void) {return "";}
void net_wifi_ap_stop(void) {}
esp_err_t net_wifi_ap_start(void) {return ESP_OK;}
int net_wifi_scan(net_wifi_ap_t *aps, int max) {(void)aps; (void)max; return 0;}
net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout)
{
    (void)ssid; (void)pass; (void)timeout; return NET_JOIN_OK;
}
bool onboard_wifi_done(void) {return true;}
bool onboard_account_done(void) {return true;}
bool onboard_channel_done(void) {return true;}
void onboard_settings_changed(void) {}
void onboard_set_web_cb(void (*cb)(void)) {(void)cb;}
esp_err_t onboard_save_wifi(const char *ssid, const char *pass) {(void)ssid; (void)pass; return ESP_OK;}
esp_err_t onboard_save_account(const char *network, const char *user, const char *pass,
                               const char *token, const char *work, const char **err)
{
    (void)network; (void)user; (void)pass; (void)token; (void)work; (void)err; return ESP_OK;
}
esp_err_t onboard_save_channel(const char *name, const char **error)
{
    (void)error;
    strlcpy(g_settings.channel, name, sizeof(g_settings.channel));
    return ESP_OK;
}
esp_err_t sdcard_mount(void) {sd_mount_calls++; return ESP_OK;}
const char *crash_report_last_reset(void) {return "test";}
esp_err_t crash_report_list(crash_report_file_cb cb, void *ctx) {(void)cb; (void)ctx; return ESP_OK;}
FILE *crash_report_open(const char *name) {(void)name; return NULL;}
int crash_report_delete_all(void) {crash_deletions++; return 0;}
