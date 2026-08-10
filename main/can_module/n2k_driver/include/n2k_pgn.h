/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "n2k_transport.h"

/* PGN behavior policy */
typedef enum {
    N2K_PGN_PASSIVE = 0,     /* Receive through the callback only */
    N2K_PGN_RESPOND_TO_REQUEST,/* Respond to ISO Request (59904) */
    N2K_PGN_PERIODIC_TX       /* Periodic timer-based transmission (unused here) */
} n2k_pgn_policy_t;

/* Codec context */
typedef struct {
    uint32_t pgn;
    n2k_pgn_policy_t policy;
    /* decode: process an incoming reassembled message; responses may use the API */
    void (*on_rx)(const n2k_msg_t *m, void *user);
    /* encode: build the PGN payload for request responses or periodic transmission.
       Must return the byte length (up to 1785); transport is selected automatically. */
    int  (*encode)(uint8_t *out, uint16_t out_cap, void *user);
    void *user;
} n2k_pgn_desc_t;

esp_err_t n2k_pgn_init(void);
esp_err_t n2k_pgn_register(const n2k_pgn_desc_t *desc);

/* Internal helper: invoke encode and transmit using automatic transport selection. */
esp_err_t n2k_pgn_send_encoded(uint8_t priority, uint8_t sa, uint8_t dst,
                               const n2k_pgn_desc_t *d);
