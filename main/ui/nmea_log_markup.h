/*
 * Copyright (c) 2026 S.Zhurba. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 *
 * @brief Safe LVGL recolor markup formatting for the NMEA log.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *text;
    size_t length;
    uint32_t color;
} nmea_log_markup_span_t;

typedef uint16_t (*nmea_log_glyph_width_cb_t)(unsigned char current,
                                               unsigned char next,
                                               void *context);

/**
 * Format colored spans using LVGL #RRGGBB text# commands.
 *
 * Hard line breaks are inserted before the visible text reaches max_width_px.
 * Every physical line closes its active color command, because LVGL resets its
 * recolor parser after an automatic line wrap. Literal '#' bytes are escaped.
 */
bool nmea_log_markup_format(char *dst,
                            size_t dst_size,
                            const nmea_log_markup_span_t *spans,
                            size_t span_count,
                            uint16_t max_width_px,
                            int16_t letter_space_px,
                            nmea_log_glyph_width_cb_t glyph_width_cb,
                            void *glyph_context);

/** Escape literal '#' bytes for an LVGL label with recolor enabled. */
bool nmea_log_markup_escape_plain(char *dst,
                                  size_t dst_size,
                                  const char *src);
