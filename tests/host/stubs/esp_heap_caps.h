#ifndef TEST_STUB_ESP_HEAP_CAPS_H
#define TEST_STUB_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_DEFAULT  0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_DMA      0

static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

#endif
