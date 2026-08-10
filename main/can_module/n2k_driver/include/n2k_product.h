/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <esp_err.h>

typedef struct {
    uint16_t manufacturer_code; /* 11 bits in N2K, stored in 16 bits here */
    uint32_t unique_id;         /* 21 bits in NAME, used here as a serial number */
    char     model_id[33];      /* NUL-terminated; truncated to the permitted length */
    char     sw_version[21];
    char     hw_version[21];
    char     serial_code[21];
} n2k_product_info_t;

/* Initialize and register PGNs 126996/126998. */
esp_err_t n2k_product_init(const n2k_product_info_t *info,
                           const char *inst_info1, const char *inst_info2);
