#pragma once

// mbedtls_base64_encode, the one mbedTLS call the shared code makes.

#include <stddef.h>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A

static inline int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src,
                                        size_t slen)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((slen + 2) / 3) + 1;
    if (!dst || dlen < need) {
        *olen = need;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        if (i + 1 < slen) v |= (unsigned)src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        dst[o++] = tbl[(v >> 18) & 63];
        dst[o++] = tbl[(v >> 12) & 63];
        dst[o++] = i + 1 < slen ? tbl[(v >> 6) & 63] : '=';
        dst[o++] = i + 2 < slen ? tbl[v & 63] : '=';
    }
    dst[o] = 0;
    *olen = o;
    return 0;
}
