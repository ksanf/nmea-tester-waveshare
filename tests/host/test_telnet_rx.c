/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#include "system/telnet_rx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t chunk(telnet_rx_t *rx, const uint8_t *src, size_t len, uint8_t *out)
{
    assert(len <= 128);
    struct { uint8_t before, data[128], after; } b = { .before = 0xa5, .after = 0x5a };
    memcpy(b.data, src, len);
    size_t n = telnet_rx_filter(rx, b.data, len);
    assert(n <= len && b.before == 0xa5 && b.after == 0x5a);
    memcpy(out, b.data, n);
    return n;
}

static void fragmented(const uint8_t *input, size_t len, const uint8_t *expected, size_t n)
{
    /* Every two-boundary split includes empty chunks and every three-way split
     * through IAC, CR/LF, CR/NUL and subnegotiation control sequences. */
    for (size_t a = 0; a <= len; ++a) {
        for (size_t b = a; b <= len; ++b) {
            telnet_rx_t rx = {0};
            uint8_t out[512];
            size_t got = chunk(&rx, input, a, out);
            got += chunk(&rx, input + a, b - a, out + got);
            got += chunk(&rx, input + b, len - b, out + got);
            assert(got == n && memcmp(out, expected, n) == 0);
        }
    }
    telnet_rx_t rx = {0};
    uint8_t out[512];
    size_t got = 0;
    for (size_t i = 0; i < len; ++i) got += chunk(&rx, input + i, 1, out + got);
    assert(got == n && memcmp(out, expected, n) == 0);
}

int main(void)
{
    static const uint8_t endings[] = "help\r\nnext\r\0again\rX\r\r\n";
    static const uint8_t normalized[] = "help\rnext\ragain\rX\r\r";
    fragmented(endings, sizeof(endings) - 1, normalized, sizeof(normalized) - 1);
    static const uint8_t control[] = {
        'A', '\r', 0xff, 0xfb, 1, '\n', 'B', 0xff, 0xff, 'C',
        0xff, 0xfa, 24, 'x', 0xff, 0xff, 0xf0, 'y', 0xff, 0xf0,
        'D', 0xff, 0xf1, 'E', 0xff, 0xfd, 3, '\r', 0xff, 0xff, '\n'
    };
    static const uint8_t filtered[] = { 'A','\r','B',0xff,'C','D','E','\r',0xff,'\n' };
    fragmented(control, sizeof(control), filtered, sizeof(filtered));

    /* Regression: CR at the end of a recv followed by all 128 printable bytes.
     * The former filter wrote 129 bytes and overwrote unread input in place. */
    telnet_rx_t rx = {0};
    uint8_t cr = '\r';
    assert(telnet_rx_filter(&rx, &cr, 1) == 1 && cr == '\r');
    uint8_t full[128], original[128];
    for (size_t i = 0; i < sizeof(full); ++i) full[i] = (uint8_t)('!' + i % 90);
    memcpy(original, full, sizeof(full));
    assert(telnet_rx_filter(&rx, full, sizeof(full)) == sizeof(full));
    assert(memcmp(full, original, sizeof(full)) == 0);

    /* Back-to-back CRs and a full CR-only recv cannot wait for another packet. */
    memset(full, '\r', sizeof(full));
    assert(telnet_rx_filter(&rx, full, sizeof(full)) == sizeof(full));
    cr = 0;
    assert(telnet_rx_filter(&rx, &cr, 1) == 0);
    assert(telnet_rx_filter(&rx, NULL, 0) == 0);
    puts("Telnet RX: fragmentation, bounded output, immediate Enter PASS");
    return 0;
}
