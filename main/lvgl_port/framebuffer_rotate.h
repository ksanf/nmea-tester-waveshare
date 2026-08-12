/*
 * Copyright (c) 2026 S.Zhurba. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 *
 * @file    framebuffer_rotate.h
 * @brief   Small framebuffer rotation helpers shared with host tests.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void framebuffer_copy_region_rgb565(const uint16_t *src,
                                    uint16_t *dst,
                                    size_t width,
                                    size_t height,
                                    size_t x_start,
                                    size_t y_start,
                                    size_t x_end,
                                    size_t y_end,
                                    bool rotate_180);
