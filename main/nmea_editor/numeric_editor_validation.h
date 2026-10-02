/* Screen-independent bounds validation for the fixed-digit editor. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t start_raw;
    uint8_t digits;
    uint16_t min_val;
    uint16_t max_val;
} field_limit_t;

static inline bool num_edit_fields_valid(const char *raw, size_t raw_len,
                                         const int8_t *map, uint8_t display_len,
                                         const field_limit_t *limits, uint8_t count)
{
    if (!raw || !map || !raw_len || (count && !limits)) return false;
    for (uint8_t i = 0; i < display_len; ++i) {
        if (map[i] < 0) continue;
        size_t index = (size_t)map[i];
        if (index >= raw_len || raw[index] < '0' || raw[index] > '9') return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        const field_limit_t *field = &limits[i];
        if (!field->digits || field->digits > 5 || field->min_val > field->max_val ||
            (size_t)field->start_raw + field->digits > raw_len) return false;
        uint32_t value = 0;
        for (uint8_t digit = 0; digit < field->digits; ++digit) {
            char ch = raw[field->start_raw + digit];
            if (ch < '0' || ch > '9') return false;
            value = value * 10u + (uint32_t)(ch - '0');
        }
        if (value < field->min_val || value > field->max_val) return false;
    }
    return true;
}
