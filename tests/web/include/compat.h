#pragma once

#include <stddef.h>
#include <string.h>

static inline size_t web_test_strlcpy(char *out, const char *input, size_t size)
{
    size_t length = strlen(input);
    if (size) {
        size_t copied = length < size - 1 ? length : size - 1;
        memcpy(out, input, copied);
        out[copied] = 0;
    }
    return length;
}

#undef strlcpy
#define strlcpy web_test_strlcpy
