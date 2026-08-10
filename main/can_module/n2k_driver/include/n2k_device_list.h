/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

typedef struct {
    bool     used;
    uint8_t  sa;
    uint64_t name;
    uint64_t last_seen_us;
} n2k_device_t;

esp_err_t n2k_devlist_init(uint32_t offline_ms);
int       n2k_devlist_get(n2k_device_t *out, int max); /* Return a snapshot. */
