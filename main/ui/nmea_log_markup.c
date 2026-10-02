/*
 * Copyright (c) 2026 S.Zhurba. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 *
 * @brief Safe LVGL recolor markup formatting for the NMEA log.
 */

#include "ui/nmea_log_markup.h"

static bool append_char_(char *dst, size_t dst_size, size_t *length, char value)
{
    if (!dst || !length || *length + 1u >= dst_size) return false;
    dst[(*length)++] = value;
    dst[*length] = '\0';
    return true;
}

static bool append_color_open_(char *dst,
                               size_t dst_size,
                               size_t *length,
                               uint32_t color)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!dst || !length || *length + 8u >= dst_size) return false;

    dst[(*length)++] = '#';
    for (int shift = 20; shift >= 0; shift -= 4) {
        dst[(*length)++] = hex[(color >> shift) & 0x0Fu];
    }
    dst[(*length)++] = ' ';
    dst[*length] = '\0';
    return true;
}

static unsigned char next_visible_char_(const nmea_log_markup_span_t *spans,
                                        size_t span_count,
                                        size_t span_index,
                                        size_t char_index)
{
    if (char_index + 1u < spans[span_index].length) {
        return (unsigned char)spans[span_index].text[char_index + 1u];
    }

    for (size_t i = span_index + 1u; i < span_count; ++i) {
        if (spans[i].text && spans[i].length > 0u) {
            return (unsigned char)spans[i].text[0];
        }
    }
    return 0u;
}

static bool close_color_(char *dst,
                         size_t dst_size,
                         size_t *length,
                         bool *color_active)
{
    if (!*color_active) return true;
    if (!append_char_(dst, dst_size, length, '#')) return false;
    *color_active = false;
    return true;
}

bool nmea_log_markup_format(char *dst,
                            size_t dst_size,
                            const nmea_log_markup_span_t *spans,
                            size_t span_count,
                            uint16_t max_width_px,
                            int16_t letter_space_px,
                            nmea_log_glyph_width_cb_t glyph_width_cb,
                            void *glyph_context)
{
    size_t dst_len = 0u;
    uint32_t active_color = 0u;
    int32_t line_width = 0;
    bool color_active = false;
    bool line_has_text = false;

    if (!dst || dst_size == 0u || (!spans && span_count > 0u)) return false;
    dst[0] = '\0';

    for (size_t span_index = 0u; span_index < span_count; ++span_index) {
        const nmea_log_markup_span_t *span = &spans[span_index];

        if (!span->text || span->length == 0u) continue;

        for (size_t char_index = 0u; char_index < span->length; ++char_index) {
            const unsigned char current = (unsigned char)span->text[char_index];
            const unsigned char next = next_visible_char_(spans, span_count,
                                                           span_index, char_index);
            int32_t advance = glyph_width_cb
                                  ? (int32_t)glyph_width_cb(current, next,
                                                            glyph_context)
                                  : 1;

            if (line_has_text) advance += letter_space_px;
            if (advance < 0) advance = 0;

            if (line_has_text && max_width_px > 0u &&
                line_width + advance > (int32_t)max_width_px) {
                if (!close_color_(dst, dst_size, &dst_len, &color_active) ||
                    !append_char_(dst, dst_size, &dst_len, '\n')) {
                    dst[0] = '\0';
                    return false;
                }
                line_width = 0;
                line_has_text = false;
            }

            if (!color_active || active_color != span->color) {
                if (!close_color_(dst, dst_size, &dst_len, &color_active) ||
                    !append_color_open_(dst, dst_size, &dst_len, span->color)) {
                    dst[0] = '\0';
                    return false;
                }
                active_color = span->color;
                color_active = true;
            }

            if (current == '#') {
                /* Close color, emit ## as a literal '#', then restore color. */
                if (!close_color_(dst, dst_size, &dst_len, &color_active) ||
                    !append_char_(dst, dst_size, &dst_len, '#') ||
                    !append_char_(dst, dst_size, &dst_len, '#') ||
                    !append_color_open_(dst, dst_size, &dst_len, span->color)) {
                    dst[0] = '\0';
                    return false;
                }
                active_color = span->color;
                color_active = true;
            } else if (!append_char_(dst, dst_size, &dst_len, (char)current)) {
                dst[0] = '\0';
                return false;
            }

            line_width += advance;
            line_has_text = true;
        }
    }

    if (!close_color_(dst, dst_size, &dst_len, &color_active)) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

bool nmea_log_markup_escape_plain(char *dst,
                                  size_t dst_size,
                                  const char *src)
{
    size_t dst_len = 0u;

    if (!dst || dst_size == 0u || !src) return false;
    dst[0] = '\0';

    while (*src) {
        if (*src == '#') {
            if (!append_char_(dst, dst_size, &dst_len, '#') ||
                !append_char_(dst, dst_size, &dst_len, '#')) {
                return false;
            }
        } else if (!append_char_(dst, dst_size, &dst_len, *src)) {
            return false;
        }
        src++;
    }
    return true;
}
