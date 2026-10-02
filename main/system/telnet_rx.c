/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#include "system/telnet_rx.h"

size_t telnet_rx_filter(telnet_rx_t *rx, uint8_t *buf, size_t len)
{
    if (!rx || !buf) return 0;
    size_t out = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = buf[i];
        switch (rx->state) {
        case TELNET_RX_DATA:
            if (b == 0xffu) {
                rx->state = TELNET_RX_IAC;
                break;
            }
            /* Sending CR now avoids a deferred byte expanding the next recv()
             * buffer or overwriting unread input. Ignore only its NVT suffix;
             * negotiation bytes between CR and the suffix do not consume it. */
            if (rx->after_cr && (b == '\n' || b == 0)) {
                rx->after_cr = false;
                break;
            }
            rx->after_cr = (b == '\r');
            buf[out++] = b;
            break;
        case TELNET_RX_IAC:
            if (b == 0xffu) {
                rx->after_cr = false;
                buf[out++] = b;
                rx->state = TELNET_RX_DATA;
            } else if (b == 0xfau) {
                rx->state = TELNET_RX_SB;
            } else if (b >= 0xfbu && b <= 0xfeu) {
                rx->state = TELNET_RX_NEGOPT;
            } else {
                rx->state = TELNET_RX_DATA;
            }
            break;
        case TELNET_RX_NEGOPT:
            rx->state = TELNET_RX_DATA;
            break;
        case TELNET_RX_SB:
            if (b == 0xffu) rx->state = TELNET_RX_SB_IAC;
            break;
        case TELNET_RX_SB_IAC:
            /* Escaped IAC is still subnegotiation data, not another command. */
            rx->state = (b == 0xf0u) ? TELNET_RX_DATA : TELNET_RX_SB;
            break;
        }
    }
    return out;
}
