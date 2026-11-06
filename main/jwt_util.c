#include "jwt_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Zello's payloads are a few hundred bytes; anything longer isn't one.
#define PAYLOAD_MAX 1024

static int b64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// Decodes base64url (padding optional) into `out`, NUL-terminated. Returns the
// decoded length, or -1 on a bad character or too little room.
static int b64url_decode(const char *in, size_t len, char *out, size_t cap)
{
    while (len && in[len - 1] == '=') len--;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64url_value(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= cap) return -1;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = 0;
    return (int)n;
}

bool jwt_expiry(const char *token, int64_t *exp)
{
    if (!token || !exp) return false;
    const char *dot1 = strchr(token, '.');
    if (!dot1) return false;
    const char *payload = dot1 + 1;
    const char *dot2 = strchr(payload, '.');
    if (!dot2 || dot2 == payload) return false;

    char json[PAYLOAD_MAX];
    if (b64url_decode(payload, (size_t)(dot2 - payload), json, sizeof(json)) <= 0) return false;

    // A key, not the same text inside some string value: it follows `{` or `,`.
    for (const char *p = strstr(json, "\"exp\""); p; p = strstr(p + 1, "\"exp\"")) {
        const char *b = p;
        while (b > json && isspace((unsigned char)b[-1])) b--;
        if (b == json || (b[-1] != '{' && b[-1] != ',')) continue;
        const char *v = p + 5;
        while (isspace((unsigned char)*v)) v++;
        if (*v++ != ':') continue;
        while (isspace((unsigned char)*v)) v++;
        if (!isdigit((unsigned char)*v)) return false;
        char *end;
        long long value = strtoll(v, &end, 10);
        if (value <= 0) return false;
        *exp = value;
        return true;
    }
    return false;
}
