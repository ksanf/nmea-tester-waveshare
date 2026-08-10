/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define RS485_HEX_DUMP_BYTES_PER_LINE 16u
#define RS485_HEX_DUMP_LINE_CAPACITY  80u

/**
 * @brief Format up to 16 bytes as an offset/hex/ASCII dump line.
 *
 * Non-printable bytes (outside 0x20..0x7E) are represented by '.'.
 * Missing bytes in a short final line are padded with spaces.
 *
 * @return Number of characters written, excluding NUL; 0 on invalid arguments.
 */
size_t rs485_hex_dump_format(char *dst, size_t dst_size, uint32_t offset,
                             const uint8_t *data, size_t len);
