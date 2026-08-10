/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Address Claim (60928) + ISO Request (59904) handler for NMEA2000/J1939.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "n2k_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t name;           /* 64-bit ISO NAME (LSB first in the frame) */
    uint8_t  preferred_sa;   /* Preferred SA (0..252); 0xFE/0xFF are reserved */
    uint8_t  priority;       /* Address Claim priority (usually 6) */
} n2k_addr_cfg_t;

/* Initialize subscriptions and retain the configuration. */
esp_err_t n2k_addr_init(const n2k_addr_cfg_t *cfg);

/* Start Address Claim by sending PGN 60928 with the current SA. */
esp_err_t n2k_addr_start_claim(void);

/* Current SA */
uint8_t   n2k_addr_get_sa(void);

/* Set the SA manually and resend PGN 60928. */
esp_err_t n2k_addr_set_sa(uint8_t sa);

/* Optional callback invoked when the SA changes. */
typedef void (*n2k_sa_changed_cb_t)(uint8_t new_sa, void *user);
esp_err_t n2k_addr_set_on_change(n2k_sa_changed_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
