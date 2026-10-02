/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define NM2K_IDENTITY_MASK UINT32_C(0x1fffff)
#define NM2K_IDENTITY_SERIAL_SIZE 17u /* NM2K + all six MAC bytes in hex + NUL */

typedef struct {
    uint64_t name;
    uint32_t identity_number;
    char serial[NM2K_IDENTITY_SERIAL_SIZE];
} nm2k_identity_t;

/* Read the board MAC before enabling CAN. Failure clears the output; there is
 * no shared fallback identity. The 21-bit NAME field is a stable hash, not a
 * guarantee of collision-free allocation. Product serial retains the full MAC. */
esp_err_t nm2k_identity_read(nm2k_identity_t *out);
