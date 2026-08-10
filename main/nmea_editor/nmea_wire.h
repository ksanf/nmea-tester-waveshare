/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Common NMEA wire-frame construction.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Build a NUL-terminated sentence with exactly one CR/LF suffix.
 *
 * Existing trailing CR/LF bytes are removed before the canonical suffix is
 * appended. The returned length is the number of wire bytes and excludes the
 * terminating NUL.
 *
 * @return Wire length, or 0 for empty, unterminated, or oversized input.
 */
size_t nmea_wire_frame_build(char *frame,
                             size_t frame_capacity,
                             const char *sentence);

/**
 * @brief Validate prefix, NMEA-0183 length and an optional/required checksum.
 *
 * CR/LF at the end is accepted. If @p require_checksum is false, a sentence
 * without `*HH` is accepted, but a present checksum must always be valid.
 */
bool nmea_wire_sentence_valid(const char *sentence, bool require_checksum);
