#pragma once

#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
