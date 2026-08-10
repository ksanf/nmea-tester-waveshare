/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "AISdecoder/aisdecoder.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AISDEC_MAX_TARGETS         256
#define AISDEC_MAX_LINE             192
#define AISDEC_STALE_MS             600000U
#define AISDEC_FRAG_TIMEOUT_MS      5000U

typedef struct {
    bool used;
    uint32_t last_seen_ms;
    uint32_t mmsi;
    uint8_t msg_type;
    uint8_t nav_status;
    float lat;
    float lon;
    float sog;
    float cog;
    uint16_t heading;
    char name[21];
    char call[8];
} aisdecoder_target_t;

bool aisdecoder_parse_is_sentence(const char *line);
void aisdecoder_parse_feed_line(const char *line);

void aisdecoder_decode_reset(void);
void aisdecoder_decode_deinit(void);
void aisdecoder_decode_feed_payload(const char *payload);
size_t aisdecoder_decode_collect(aisdecoder_target_t *out, size_t max_targets, uint32_t now_ms);
