/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
/* Fixed-capacity terminal byte storage. Callers provide synchronization.
 * Reads preserve binary bytes; input writes are all-or-nothing, output append
 * retains the latest bytes and reports every overwritten byte. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t capacity, head, count;
    uint32_t dropped;
} can_byte_ring_t;

#define CAN_BYTE_RING_INIT(storage) { .data = (storage), .capacity = sizeof(storage) }

static inline void can_byte_ring_clear(can_byte_ring_t *r)
{
    r->head = r->count = 0;
}

static inline void can_byte_ring_note_drop(can_byte_ring_t *r, size_t count)
{
    if (count > UINT32_MAX - r->dropped) r->dropped = UINT32_MAX;
    else r->dropped += (uint32_t)count;
}

static inline bool can_byte_ring_write(can_byte_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r || !r->capacity || (!data && len) || len > r->capacity - r->count) return false;
    if (!len) return true;
    size_t tail = (r->head + r->count) % r->capacity;
    size_t first = len < r->capacity - tail ? len : r->capacity - tail;
    memcpy(r->data + tail, data, first);
    if (len > first) memcpy(r->data, data + first, len - first);
    r->count += len;
    return true;
}

static inline void can_byte_ring_append(can_byte_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r || !r->capacity || !data || !len) return;
    if (len > r->capacity) {
        size_t skip = len - r->capacity;
        can_byte_ring_note_drop(r, skip);
        data += skip;
        len = r->capacity;
    }
    if (len > r->capacity - r->count) {
        size_t overwrite = len - (r->capacity - r->count);
        r->head = (r->head + overwrite) % r->capacity;
        r->count -= overwrite;
        can_byte_ring_note_drop(r, overwrite);
    }
    (void)can_byte_ring_write(r, data, len);
}

static inline size_t can_byte_ring_read(can_byte_ring_t *r, uint8_t *out, size_t cap)
{
    if (!r || !out || !cap || !r->count) return 0;
    size_t n = cap < r->count ? cap : r->count;
    size_t first = n < r->capacity - r->head ? n : r->capacity - r->head;
    memcpy(out, r->data + r->head, first);
    if (n > first) memcpy(out + first, r->data, n - first);
    r->head = (r->head + n) % r->capacity;
    r->count -= n;
    return n;
}
