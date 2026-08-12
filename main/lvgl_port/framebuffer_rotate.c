/*
 * Copyright (c) 2026 S.Zhurba. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 *
 * @file    framebuffer_rotate.c
 * @brief   In-place RGB565 framebuffer rotation helpers.
 */

#include "lvgl_port/framebuffer_rotate.h"

#include <string.h>

void framebuffer_copy_region_rgb565(const uint16_t *src,
                                    uint16_t *dst,
                                    size_t width,
                                    size_t height,
                                    size_t x_start,
                                    size_t y_start,
                                    size_t x_end,
                                    size_t y_end,
                                    bool rotate_180)
{
    if (!src || !dst || width == 0U || height == 0U ||
        x_start > x_end || y_start > y_end ||
        x_end >= width || y_end >= height) {
        return;
    }

    if (!rotate_180) {
        const size_t row_pixels = x_end - x_start + 1U;
        for (size_t y = y_start; y <= y_end; ++y) {
            const size_t offset = y * width + x_start;
            memcpy(&dst[offset], &src[offset], row_pixels * sizeof(*src));
        }
        return;
    }

    for (size_t y = y_start; y <= y_end; ++y) {
        for (size_t x = x_start; x <= x_end; ++x) {
            const size_t src_index = y * width + x;
            const size_t dst_index = (height - y - 1U) * width
                                   + (width - x - 1U);
            dst[dst_index] = src[src_index];
        }
    }
}
