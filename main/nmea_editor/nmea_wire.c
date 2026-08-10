/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Common NMEA wire-frame construction.
 */

#include "nmea_editor/nmea_wire.h"
#include "config/config_nmea_tester.h"

#include <string.h>

static int hex_value_(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool nmea_wire_sentence_valid(const char *sentence, bool require_checksum)
{
    const char *star;
    size_t len;
    unsigned char checksum = 0;
    int hi;
    int lo;

    if (!sentence || (sentence[0] != '$' && sentence[0] != '!')) return false;
    len = 0;
    while (len <= NMEA0183_WIRE_MAX && sentence[len] != '\0') len++;
    if (len > NMEA0183_WIRE_MAX) return false;
    while (len > 0 && (sentence[len - 1u] == '\r' || sentence[len - 1u] == '\n')) {
        len--;
    }
    if (len < 2u || len + 2u > NMEA0183_WIRE_MAX) return false;

    star = memchr(sentence + 1, '*', len - 1u);
    if (!star) return !require_checksum;
    const size_t star_pos = (size_t)(star - sentence);
    if (star_pos + 3u != len) return false;
    hi = hex_value_(star[1]);
    lo = hex_value_(star[2]);
    if (hi < 0 || lo < 0) return false;
    for (const char *p = sentence + 1; p < star; ++p) {
        checksum ^= (unsigned char)*p;
    }
    return checksum == (unsigned char)((hi << 4) | lo);
}

size_t nmea_wire_frame_build(char *frame,
                             size_t frame_capacity,
                             const char *sentence)
{
    size_t body_len = 0;

    if (!frame || frame_capacity == 0) return 0;
    frame[0] = '\0';
    if (!sentence) return 0;

    while (body_len < frame_capacity && sentence[body_len] != '\0') {
        body_len++;
    }
    if (body_len == frame_capacity) return 0;

    while (body_len > 0 &&
           (sentence[body_len - 1u] == '\r' || sentence[body_len - 1u] == '\n')) {
        body_len--;
    }
    if (body_len == 0 || body_len + 3u > frame_capacity) return 0;

    memcpy(frame, sentence, body_len);
    frame[body_len] = '\r';
    frame[body_len + 1u] = '\n';
    frame[body_len + 2u] = '\0';
    return body_len + 2u;
}
