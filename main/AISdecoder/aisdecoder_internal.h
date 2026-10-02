/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "AISdecoder/aisdecoder_runtime.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool aisdecoder_parse_is_sentence(const char *line);
void aisdecoder_parse_feed_line(const char *line);
void aisdecoder_parse_reset(void);

bool aisdecoder_decode_init(void);
void aisdecoder_decode_reset(void);
void aisdecoder_decode_deinit(void);
void aisdecoder_decode_feed_payload(const char *payload);
size_t aisdecoder_decode_collect(aisdecoder_target_t *out, size_t max_targets, uint32_t now_ms);
uint32_t aisdecoder_decode_updates(void);
