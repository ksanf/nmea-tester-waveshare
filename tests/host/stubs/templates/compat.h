#pragma once
#include <stddef.h>
#include <string.h>
/* ESP-IDF provides strlcpy; glibc before 2.38 may not. */
static inline size_t host_strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
#define strlcpy host_strlcpy
