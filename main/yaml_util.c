#include "yaml_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Longest value we decode: a developer token is about 1 KB.
#define VALUE_MAX 1280
#define KEY_MAX   48

// ---- writing ---------------------------------------------------------------

esp_err_t yaml_write_text(yaml_write_cb_t write, void *ctx, const char *text)
{
    return write(ctx, text, strlen(text));
}

static esp_err_t write_key(yaml_write_cb_t write, void *ctx, const char *indent, const char *key)
{
    esp_err_t err = yaml_write_text(write, ctx, indent);
    if (err == ESP_OK) err = yaml_write_text(write, ctx, key);
    if (err == ESP_OK) err = yaml_write_text(write, ctx, ": ");
    return err;
}

esp_err_t yaml_write_str(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, const char *value)
{
    esp_err_t err = write_key(write, ctx, indent, key);
    if (err == ESP_OK) err = yaml_write_text(write, ctx, "\"");
    // Escape in runs: plain bytes (UTF-8 included) go out as they are.
    const char *run = value;
    for (const char *p = value; *p && err == ESP_OK; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char hex[5];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\t') esc = "\\t";
        else if (c == '\r') esc = "\\r";
        else if (c < 0x20 || c == 0x7f) {
            snprintf(hex, sizeof(hex), "\\x%02X", c);
            esc = hex;
        }
        if (!esc) continue;
        if (p > run) err = write(ctx, run, p - run);
        if (err == ESP_OK) err = yaml_write_text(write, ctx, esc);
        run = p + 1;
    }
    if (err == ESP_OK && *run) err = yaml_write_text(write, ctx, run);
    if (err == ESP_OK) err = yaml_write_text(write, ctx, "\"\n");
    return err;
}

esp_err_t yaml_write_u32(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, uint32_t value)
{
    char num[16];
    snprintf(num, sizeof(num), "%lu\n", (unsigned long)value);
    esp_err_t err = write_key(write, ctx, indent, key);
    return err == ESP_OK ? yaml_write_text(write, ctx, num) : err;
}

esp_err_t yaml_write_bool(yaml_write_cb_t write, void *ctx, const char *indent, const char *key, bool value)
{
    esp_err_t err = write_key(write, ctx, indent, key);
    return err == ESP_OK ? yaml_write_text(write, ctx, value ? "true\n" : "false\n") : err;
}

// ---- reading ---------------------------------------------------------------

static void put_utf8(char *out, size_t *n, uint32_t cp)
{
    if (cp < 0x80) {
        out[(*n)++] = cp;
    } else if (cp < 0x800) {
        out[(*n)++] = 0xC0 | (cp >> 6);
        out[(*n)++] = 0x80 | (cp & 0x3F);
    } else {
        out[(*n)++] = 0xE0 | (cp >> 12);
        out[(*n)++] = 0x80 | ((cp >> 6) & 0x3F);
        out[(*n)++] = 0x80 | (cp & 0x3F);
    }
}

// Only a comment may follow a closing quote.
static bool only_comment(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p == end || *p == '#';
}

// Decodes the scalar in [p, end) into `out`. False if it is malformed or too
// long.
static bool decode_value(const char *p, const char *end, char *out, size_t size)
{
    while (p < end && *p == ' ') p++;
    // Flow collections are handled separately, only when exactly empty.
    if (p < end && (*p == '[' || *p == '{')) return false;
    size_t n = 0;
    if (p < end && *p == '"') {
        for (p++; p < end && *p != '"'; p++) {
            if (n + 4 >= size) return false;
            if (*p != '\\') {
                out[n++] = *p;
                continue;
            }
            if (++p >= end) return false;
            uint32_t cp;
            int digits = 0;
            switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case '0': out[n++] = '\0'; break;
            case '"': case '\\': case '/': case ' ': out[n++] = *p; break;
            case 'x': digits = 2; break;
            case 'u': digits = 4; break;
            default: return false;
            }
            if (!digits) continue;
            if (end - p <= digits) return false;
            char hex[5] = {0};
            memcpy(hex, p + 1, digits);
            char *stop;
            cp = strtoul(hex, &stop, 16);
            if (*stop) return false;
            put_utf8(out, &n, cp);
            p += digits;
        }
        if (p >= end || !only_comment(p + 1, end)) return false;
    } else if (p < end && *p == '\'') {
        for (p++; p < end; p++) {
            if (*p == '\'') {
                if (p + 1 < end && p[1] == '\'') {
                    p++;  // '' is a quote
                } else {
                    break;
                }
            }
            if (n + 1 >= size) return false;
            out[n++] = *p;
        }
        if (p >= end || !only_comment(p + 1, end)) return false;
    } else {
        // Plain: up to a " #" comment, trailing blanks trimmed.
        const char *stop = p;
        while (stop < end && !(*stop == '#' && (stop == p || stop[-1] == ' '))) stop++;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
        if ((size_t)(stop - p) >= size) return false;
        memcpy(out, p, stop - p);
        n = stop - p;
        out[n] = 0;
        if (!strcmp(out, "~") || !strcmp(out, "null")) n = 0;
    }
    out[n] = 0;
    return true;
}

// Splits `key: value` in [p, end). The key is a plain word; `*value` points
// just past the colon.
static bool split_key(const char *p, const char *end, char *key, const char **value)
{
    const char *colon = p;
    while (colon < end && *colon != ':') {
        if (*colon == '"' || *colon == '\'' || *colon == '#') return false;
        colon++;
    }
    if (colon >= end || (colon + 1 < end && colon[1] != ' ')) return false;
    const char *kend = colon;
    while (kend > p && kend[-1] == ' ') kend--;
    if (kend == p || (size_t)(kend - p) >= KEY_MAX) return false;
    memcpy(key, p, kend - p);
    key[kend - p] = 0;
    *value = colon + 1;
    return true;
}

// True if [p, end) holds nothing but blanks and a comment, or an empty flow
// collection.
static bool empty_value(const char *p, const char *end, bool *flow_empty)
{
    while (p < end && *p == ' ') p++;
    *flow_empty = false;
    if (end - p >= 2 && (!strncmp(p, "[]", 2) || !strncmp(p, "{}", 2))) {
        p += 2;
        if (!only_comment(p, end)) return false;
        *flow_empty = true;
    }
    return only_comment(p, end);
}

esp_err_t yaml_parse(const char *text, yaml_value_cb_t cb, void *ctx, int *bad_line)
{
    char *value = malloc(VALUE_MAX);
    if (!value) return ESP_ERR_NO_MEM;
    char section[KEY_MAX] = "", key[KEY_MAX];
    bool in_section = false;
    int item = -1, dash_indent = -1, line_no = 0;
    esp_err_t err = ESP_OK;
    if (bad_line) *bad_line = 0;

    // A UTF-8 byte order mark is harmless; skip it.
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;

    for (const char *line = text; *line && err == ESP_OK;) {
        const char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        const char *next = *eol ? eol + 1 : eol;
        const char *end = eol;
        if (end > line && end[-1] == '\r') end--;
        line_no++;

        const char *p = line;
        while (p < end && *p == ' ') p++;
        int indent = p - line;
        if (p == end || *p == '#' || (end - p == 3 && (!strncmp(p, "---", 3) || !strncmp(p, "...", 3)))) {
            line = next;
            continue;
        }
        if (*p == '\t') {
            err = ESP_ERR_INVALID_ARG;  // YAML does not allow tabs for indentation
            break;
        }

        const char *rest;
        bool flow_empty;
        if (indent == 0) {
            // A top-level key: a scalar, or a section when nothing follows it.
            if (*p == '-' || !split_key(p, end, key, &rest)) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            item = dash_indent = -1;
            if (empty_value(rest, end, &flow_empty)) {
                strlcpy(section, key, sizeof(section));
                // Empty flow collections emit the marker, but are complete:
                // only a block section may accept subsequent indented lines.
                in_section = !flow_empty;
                err = cb(ctx, section, -1, NULL, NULL);
            } else {
                in_section = false;
                if (!decode_value(rest, end, value, VALUE_MAX)) {
                    err = ESP_ERR_INVALID_ARG;
                    break;
                }
                err = cb(ctx, NULL, -1, key, value);
            }
        } else if (!in_section) {
            err = ESP_ERR_INVALID_ARG;
            break;
        } else {
            if (*p == '-' && (p + 1 == end || p[1] == ' ')) {
                // A new list item, which may carry its first key on this line.
                item++;
                dash_indent = indent;
                p++;
                while (p < end && *p == ' ') p++;
                if (p == end || *p == '#') {
                    line = next;
                    continue;
                }
            } else if (item >= 0 && indent <= dash_indent) {
                err = ESP_ERR_INVALID_ARG;  // a map key where the list's items should be
                break;
            }
            if (!split_key(p, end, key, &rest) || (empty_value(rest, end, &flow_empty) && !flow_empty)) {
                err = ESP_ERR_INVALID_ARG;  // nested sections go no deeper than this
                break;
            }
            if (flow_empty) {
                value[0] = 0;
            } else if (!decode_value(rest, end, value, VALUE_MAX)) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            err = cb(ctx, section, item, key, value);
        }
        line = next;
    }
    if (err == ESP_ERR_INVALID_ARG && bad_line) *bad_line = line_no;
    memset(value, 0, VALUE_MAX);  // it may have held a password
    free(value);
    return err;
}

bool yaml_bool(const char *value, bool *out)
{
    static const char *const yes[] = {"true", "yes", "on"};
    static const char *const no[] = {"false", "no", "off"};
    for (int i = 0; i < 3; i++) {
        if (!strcasecmp(value, yes[i])) return *out = true, true;
        if (!strcasecmp(value, no[i])) return *out = false, true;
    }
    return false;
}

bool yaml_u32(const char *value, uint32_t max, uint32_t *out)
{
    if (!isdigit((unsigned char)value[0])) return false;
    char *stop;
    unsigned long v = strtoul(value, &stop, 10);
    if (*stop || v > max) return false;
    *out = v;
    return true;
}
