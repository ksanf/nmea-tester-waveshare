/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SAILOR_NDP_CAPACITY 512u
#define SAILOR_NDP_SLOTS 4u
typedef struct {
    uint8_t sig[3], series, channel;
    const uint8_t *data;
    uint16_t len;
} sailor_ndp_message_t;
typedef struct {
    bool active;
    uint32_t key, at;
    uint8_t series, channel, next_page;
    uint16_t total, used;
    uint8_t data[SAILOR_NDP_CAPACITY];
} sailor_ndp_slot_t;
typedef struct { sailor_ndp_slot_t slots[SAILOR_NDP_SLOTS]; } sailor_ndp_pages_t;
/* Single RX consumer. Key is SA/DA/priority, NOT fast-packet SID.
 * Returns 1 complete, 0 waiting, -1 invalid. Output lives until next call.
 * A whole retransmission is returned again, so the session can re-ACK it. */
int sailor_ndp_pages_feed(sailor_ndp_pages_t *s, uint32_t key, uint32_t now,
                         const uint8_t *wire, size_t len, sailor_ndp_message_t *out);

/* Emit complete FP payloads. Caller holds its TX lock for ALL pages; every
 * callback borrows at most223 bytes and must consume/copy before returning. */
typedef bool (*sailor_ndp_page_send_fn)(void *user, const uint8_t *wire, size_t len);
bool sailor_ndp_pages_encode(const uint8_t sig[3], uint8_t series, uint8_t channel,
    const uint8_t *data, size_t len, sailor_ndp_page_send_fn send, void *user);
