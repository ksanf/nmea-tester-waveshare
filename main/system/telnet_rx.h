/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TELNET_RX_DATA = 0,
    TELNET_RX_IAC,
    TELNET_RX_NEGOPT,
    TELNET_RX_SB,
    TELNET_RX_SB_IAC,
} telnet_rx_state_t;

typedef struct {
    telnet_rx_state_t state;
    bool after_cr;
} telnet_rx_t;

/* Zero-initialize per connection. Filters in place; output never exceeds len.
 * CR is delivered immediately, even if the peer sends no further bytes. */
size_t telnet_rx_filter(telnet_rx_t *rx, uint8_t *buf, size_t len);
