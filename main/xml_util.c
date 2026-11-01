#include "xml_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define XML_MAX_DEPTH 32
#define XML_MAX_ATTRIBUTES 32

typedef struct {
    const char *start;
    size_t len;
} span_t;

typedef struct {
    span_t name;
    const char *value;
    const char *value_end;
} attribute_t;

static bool xml_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && xml_space(*p)) p++;
    return p;
}

static bool name_start(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

static bool read_name(const char **cursor, const char *end, span_t *name)
{
    const char *p = *cursor;
    if (p == end || !name_start((unsigned char)*p)) return false;
    name->start = p++;
    while (p < end && (name_start((unsigned char)*p) || (*p >= '0' && *p <= '9') ||
                       *p == '-' || *p == '.')) p++;
    name->len = p - name->start;
    *cursor = p;
    return true;
}

static bool same_name(span_t a, span_t b)
{
    return a.len == b.len && !memcmp(a.start, b.start, a.len);
}

static bool xml_character(uint32_t cp)
{
    return cp == 9 || cp == 10 || cp == 13 || (cp >= 0x20 && cp <= 0xd7ff) ||
           (cp >= 0xe000 && cp <= 0xfffd) || (cp >= 0x10000 && cp <= 0x10ffff);
}

static bool valid_utf8(const char *p)
{
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        uint32_t cp = c, min = 0;
        int extra = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 0x1f; extra = 1; min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 0x0f; extra = 2; min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 0x07; extra = 3; min = 0x10000;
        } else if (c >= 0x80) {
            return false;
        }
        for (int i = 0; i < extra; i++) {
            c = (unsigned char)*p;
            if ((c & 0xc0) != 0x80) return false;
            p++;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (cp < min || !xml_character(cp)) return false;
    }
    return true;
}

static bool entity(const char **cursor, const char *end, uint32_t *cp)
{
    const char *p = *cursor;
    static const struct { const char *text; char value; } named[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t len = strlen(named[i].text);
        if ((size_t)(end - p) >= len && !memcmp(p, named[i].text, len)) {
            *cp = named[i].value;
            *cursor = p + len;
            return true;
        }
    }
    if (end - p < 4 || p[0] != '&' || p[1] != '#') return false;
    p += 2;
    unsigned base = 10;
    if (p < end && *p == 'x') { base = 16; p++; }
    const char *digits = p;
    uint32_t value = 0;
    while (p < end && *p != ';') {
        unsigned digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else return false;
        if (digit >= base || value > (0x10ffffu - digit) / base) return false;
        value = value * base + digit;
        p++;
    }
    if (p == digits || p == end || !xml_character(value)) return false;
    *cp = value;
    *cursor = p + 1;
    return true;
}

static bool read_attribute(const char **cursor, const char *end, attribute_t *attr)
{
    const char *p = *cursor;
    if (!read_name(&p, end, &attr->name)) return false;
    p = skip_space(p, end);
    if (p == end || *p++ != '=') return false;
    p = skip_space(p, end);
    if (p == end || (*p != '"' && *p != '\'')) return false;
    char quote = *p++;
    attr->value = p;
    while (p < end && *p != quote) {
        if (*p == '<') return false;
        if (*p == '&') {
            uint32_t cp;
            if (!entity(&p, end, &cp)) return false;
        } else {
            p++;
        }
    }
    if (p == end) return false;
    attr->value_end = p++;
    *cursor = p;
    return true;
}

static bool declaration_attribute(const attribute_t *attr, size_t index, unsigned *seen)
{
    span_t version = {"version", 7}, encoding = {"encoding", 8}, standalone = {"standalone", 10};
    size_t len = attr->value_end - attr->value;
    if (same_name(attr->name, version)) {
        if (index != 0 || len != 3 || memcmp(attr->value, "1.0", 3)) return false;
        *seen |= 1;
    } else if (same_name(attr->name, encoding)) {
        if (!(*seen & 1) || (*seen & 4) || len != 5 || strncasecmp(attr->value, "UTF-8", 5)) return false;
        *seen |= 2;
    } else if (same_name(attr->name, standalone)) {
        if (!(*seen & 1) || !((len == 3 && !memcmp(attr->value, "yes", 3)) ||
                             (len == 2 && !memcmp(attr->value, "no", 2)))) return false;
        *seen |= 4;
    } else {
        return false;
    }
    return true;
}

static bool read_attributes(const char **cursor, const char *end, bool declaration, bool *empty)
{
    const char *p = *cursor;
    span_t names[XML_MAX_ATTRIBUTES];
    size_t count = 0;
    unsigned declaration_fields = 0;
    for (;;) {
        const char *before = p;
        p = skip_space(p, end);
        if (p == end) return false;
        if (declaration && end - p >= 2 && p[0] == '?' && p[1] == '>') {
            *cursor = p + 2;
            return (declaration_fields & 1) != 0;
        }
        if (!declaration && (*p == '>' || (end - p >= 2 && p[0] == '/' && p[1] == '>'))) {
            *empty = *p == '/';
            *cursor = p + (*empty ? 2 : 1);
            return true;
        }
        if (p == before || count == XML_MAX_ATTRIBUTES) return false;
        attribute_t attr;
        if (!read_attribute(&p, end, &attr)) return false;
        for (size_t i = 0; i < count; i++)
            if (same_name(names[i], attr.name)) return false;
        if (declaration && !declaration_attribute(&attr, count, &declaration_fields)) return false;
        names[count++] = attr.name;
    }
}

esp_err_t xml_validate_document(const char *xml)
{
    if (!xml || !valid_utf8(xml)) return ESP_ERR_INVALID_ARG;
    const char *p = xml, *end = xml + strlen(xml);
    if (end - p >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;
    if (end - p >= 6 && !memcmp(p, "<?xml", 5) && xml_space(p[5])) {
        p += 5;
        bool empty = false;
        if (!read_attributes(&p, end, true, &empty)) return ESP_ERR_INVALID_ARG;
    }
    span_t stack[XML_MAX_DEPTH];
    size_t depth = 0;
    bool root = false;
    while (p < end) {
        if (*p != '<') {
            if (!depth && !xml_space(*p)) return ESP_ERR_INVALID_ARG;
            if (end - p >= 3 && !memcmp(p, "]]>", 3)) return ESP_ERR_INVALID_ARG;
            if (*p == '&') {
                uint32_t cp;
                if (!entity(&p, end, &cp)) return ESP_ERR_INVALID_ARG;
            } else {
                p++;
            }
            continue;
        }
        if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *close = strstr(p + 4, "--");
            if (!close || close[2] != '>') return ESP_ERR_INVALID_ARG;
            p = close + 3;
            continue;
        }
        p++;
        bool closing = p < end && *p == '/';
        if (closing) p++;
        span_t name;
        if (!read_name(&p, end, &name)) return ESP_ERR_INVALID_ARG;
        if (closing) {
            p = skip_space(p, end);
            if (!depth || p == end || *p++ != '>' || !same_name(name, stack[depth - 1])) {
                return ESP_ERR_INVALID_ARG;
            }
            depth--;
        } else {
            if (!depth && root) return ESP_ERR_INVALID_ARG;
            bool empty;
            if (!read_attributes(&p, end, false, &empty)) return ESP_ERR_INVALID_ARG;
            root = true;
            if (!empty) {
                if (depth == XML_MAX_DEPTH) return ESP_ERR_INVALID_ARG;
                stack[depth++] = name;
            }
        }
    }
    return root && !depth ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t xml_write_text(xml_write_cb_t write, void *ctx, const char *text)
{
    return write(ctx, text, strlen(text));
}

esp_err_t xml_write_escaped(xml_write_cb_t write, void *ctx, const char *text)
{
    if (!text || !valid_utf8(text)) return ESP_ERR_INVALID_ARG;
    const char *start = text;
    for (const char *p = text;; p++) {
        const char *entity = NULL;
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&apos;"; break;
        default: break;
        }
        if (entity) {
            if (p > start && write(ctx, start, p - start) != ESP_OK) return ESP_FAIL;
            if (xml_write_text(write, ctx, entity) != ESP_OK) return ESP_FAIL;
            start = p + 1;
        }
        if (!*p) return p > start ? write(ctx, start, p - start) : ESP_OK;
    }
}

esp_err_t xml_write_attr(xml_write_cb_t write, void *ctx, const char *name, const char *value)
{
    if (xml_write_text(write, ctx, " ") != ESP_OK || xml_write_text(write, ctx, name) != ESP_OK ||
        xml_write_text(write, ctx, "=\"") != ESP_OK || xml_write_escaped(write, ctx, value) != ESP_OK) {
        return ESP_FAIL;
    }
    return xml_write_text(write, ctx, "\"");
}

esp_err_t xml_write_attr_u32(xml_write_cb_t write, void *ctx, const char *name, uint32_t value)
{
    char text[12];
    snprintf(text, sizeof(text), "%lu", (unsigned long)value);
    return xml_write_attr(write, ctx, name, text);
}

esp_err_t xml_write_attr_bool(xml_write_cb_t write, void *ctx, const char *name, bool value)
{
    return xml_write_attr(write, ctx, name, value ? "true" : "false");
}

int xml_attr(const char *element, const char *end, const char *name, char *out, size_t size)
{
    if (!element || !end || end <= element || !name || !out || !size || *element != '<') return -1;
    const char *p = element + 1;
    span_t tag, wanted = {name, strlen(name)};
    if (!read_name(&p, end, &tag)) return -1;
    int found = 0;
    while (p < end) {
        const char *before = p;
        p = skip_space(p, end);
        if (p == end || (p + 1 == end && *p == '/')) break;
        attribute_t attr;
        if (p == before || !read_attribute(&p, end, &attr)) return -1;
        if (!same_name(attr.name, wanted)) continue;
        if (found) return -1;
        size_t used = 0;
        for (const char *value = attr.value; value < attr.value_end;) {
            if (*value == '&') {
                uint32_t cp;
                if (!entity(&value, attr.value_end, &cp)) return -1;
                size_t bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                if (used + bytes >= size) return -1;
                if (bytes == 1) {
                    out[used++] = cp;
                } else {
                    out[used++] = (bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0) |
                                  (cp >> (6 * (bytes - 1)));
                    for (size_t i = bytes - 1; i > 0; i--)
                        out[used++] = 0x80 | ((cp >> (6 * (i - 1))) & 0x3f);
                }
            } else {
                if (used + 1 >= size) return -1;
                out[used++] = *value++;
            }
        }
        out[used] = 0;
        found = 1;
    }
    return found;
}

bool xml_attr_bool(const char *element, const char *end, const char *name, bool *value)
{
    char text[8];
    if (xml_attr(element, end, name, text, sizeof(text)) != 1) return false;
    if (!strcmp(text, "true")) *value = true;
    else if (!strcmp(text, "false")) *value = false;
    else return false;
    return true;
}

bool xml_attr_u32(const char *element, const char *end, const char *name, uint32_t max, uint32_t *value)
{
    char text[12];
    if (xml_attr(element, end, name, text, sizeof(text)) != 1) return false;
    if (!isdigit((unsigned char)text[0])) return false;
    for (const char *p = text; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    char *tail;
    errno = 0;
    unsigned long parsed = strtoul(text, &tail, 10);
    if (*tail || errno == ERANGE || parsed > max) return false;
    *value = parsed;
    return true;
}

const char *xml_find_element(const char *from, const char *name)
{
    if (!from || !name || !name[0]) return NULL;
    size_t len = strlen(name);
    const char *p = from;
    while ((p = strchr(p, '<'))) {
        if (!strncmp(p, "<!--", 4)) {
            p = strstr(p + 4, "-->");
            if (!p) return NULL;
            p += 3;
            continue;
        }
        if (!strncmp(p + 1, name, len)) {
            char next = p[1 + len];
            if (xml_space(next) || next == '>' || next == '/') return p;
        }
        p = xml_element_end(p);
        if (!p) return NULL;
        p++;
    }
    return NULL;
}

const char *xml_element_end(const char *element)
{
    if (!element || *element != '<') return NULL;
    char quote = 0;
    for (const char *p = element + 1; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}
