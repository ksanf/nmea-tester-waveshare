/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <esp_err.h>

/* Types: per 59392 */
typedef enum {
    N2K_ACK = 0,
    N2K_NACK = 1,
    N2K_ACCESS_DENIED = 2,
    N2K_BUSY = 3
} n2k_ack_type_t;

/* Send an addressed ISO Acknowledgment for a PGN. */
esp_err_t n2k_iso_ack_send(uint8_t priority, uint8_t sa, uint8_t dst,
                           n2k_ack_type_t type, uint32_t pgn);
