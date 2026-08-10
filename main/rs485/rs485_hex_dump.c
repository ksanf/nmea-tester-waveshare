/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "rs485/rs485_hex_dump.h"

#include <inttypes.h>
#include <stdio.h>

size_t rs485_hex_dump_format(char *dst, size_t dst_size, uint32_t offset,
                             const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    char *out;
    int prefix_len;

    if (!dst || dst_size < RS485_HEX_DUMP_LINE_CAPACITY || (!data && len != 0)) {
        return 0;
    }
    if (len > RS485_HEX_DUMP_BYTES_PER_LINE) {
        len = RS485_HEX_DUMP_BYTES_PER_LINE;
    }

    prefix_len = snprintf(dst, dst_size, "%04" PRIX32 ": ", offset);
    if (prefix_len < 0 || (size_t)prefix_len >= dst_size) {
        dst[0] = '\0';
        return 0;
    }
    out = dst + prefix_len;

    for (size_t i = 0; i < RS485_HEX_DUMP_BYTES_PER_LINE; i++) {
        if (i < len) {
            const uint8_t byte = data[i];
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 0x0Fu];
            *out++ = ' ';
        } else {
            *out++ = ' ';
            *out++ = ' ';
            *out++ = ' ';
        }
    }

    *out++ = ' ';
    *out++ = '|';
    for (size_t i = 0; i < RS485_HEX_DUMP_BYTES_PER_LINE; i++) {
        if (i >= len) {
            *out++ = ' ';
            continue;
        }
        const uint8_t byte = data[i];
        *out++ = (byte >= 0x20u && byte < 0x7Fu) ? (char)byte : '.';
    }
    *out++ = '|';
    *out = '\0';

    return (size_t)(out - dst);
}
