/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "../bridge_can_module/can_byte_ring.h"
#include <assert.h>
#include <stdio.h>

static void test_atomic_input_and_revoke(void)
{
    uint8_t storage[5], out[8];
    can_byte_ring_t ring = CAN_BYTE_RING_INIT(storage);
    const uint8_t first[] = {'a', 0, '\r', '\n'};
    assert(can_byte_ring_write(&ring, first, sizeof(first)));
    assert(!can_byte_ring_write(&ring, (const uint8_t *)"XY", 2));
    assert(ring.count == 4); /* A rejected command cannot leave a partial prefix. */
    assert(can_byte_ring_read(&ring, out, 2) == 2);
    assert(out[0] == 'a' && out[1] == 0);
    assert(can_byte_ring_write(&ring, (const uint8_t *)"XYZ", 3));
    assert(can_byte_ring_read(&ring, out, sizeof(out)) == 5);
    assert(memcmp(out, "\r\nXYZ", 5) == 0); /* Wraparound preserves wire bytes. */
    assert(can_byte_ring_write(&ring, (const uint8_t *)"old", 3));
    can_byte_ring_clear(&ring); /* revoked owner's pending input */
    assert(can_byte_ring_write(&ring, (const uint8_t *)"new", 3));
    assert(can_byte_ring_read(&ring, out, sizeof(out)) == 3);
    assert(memcmp(out, "new", 3) == 0);
}

static void test_slow_consumer(void)
{
    uint8_t storage[5], out[8];
    can_byte_ring_t ring = CAN_BYTE_RING_INIT(storage);
    can_byte_ring_append(&ring, (const uint8_t *)"abc", 3);
    can_byte_ring_append(&ring, (const uint8_t *)"defghijk", 8);
    assert(ring.count == 5 && ring.dropped == 6);
    assert(can_byte_ring_read(&ring, out, sizeof(out)) == 5);
    assert(memcmp(out, "ghijk", 5) == 0);
    can_byte_ring_note_drop(&ring, UINT32_MAX);
    can_byte_ring_note_drop(&ring, 100);
    assert(ring.dropped == UINT32_MAX);
}

static void test_mixed_chunks(void)
{
    uint8_t storage[17], expected[17], out[40], input[37];
    can_byte_ring_t ring = CAN_BYTE_RING_INIT(storage);
    size_t expected_count = 0;
    uint32_t expected_dropped = 0, random = 1;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        random = random * 1664525u + 1013904223u;
        size_t n = (random >> 8) % sizeof(input);
        if (random & 1u) {
            for (size_t i = 0; i < n; ++i) {
                input[i] = (uint8_t)(iteration + i);
                if (expected_count == sizeof(expected)) {
                    memmove(expected, expected + 1, --expected_count);
                    ++expected_dropped;
                }
                expected[expected_count++] = input[i];
            }
            can_byte_ring_append(&ring, input, n);
        } else {
            size_t take = n < expected_count ? n : expected_count;
            assert(can_byte_ring_read(&ring, out, n) == take);
            assert(memcmp(out, expected, take) == 0);
            expected_count -= take;
            memmove(expected, expected + take, expected_count);
        }
        assert(ring.count == expected_count && ring.count <= sizeof(storage));
        assert(ring.dropped == expected_dropped);
    }
}

int main(void)
{
    test_atomic_input_and_revoke();
    test_slow_consumer();
    test_mixed_chunks();
    puts("CAN terminal ring tests passed");
    return 0;
}
