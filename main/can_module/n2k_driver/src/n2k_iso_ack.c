/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "n2k_iso_ack.h"
#include "n2k_transport.h"

esp_err_t n2k_iso_ack_send(uint8_t priority, uint8_t sa, uint8_t dst,
                           n2k_ack_type_t type, uint32_t pgn)
{
    uint8_t d[8]={0};
    d[0]=(uint8_t)type;
    /* Bytes 1..3: group function (0), reserved by PGN 59392 for ISO ACK; leave zero. */
    d[4]=(uint8_t)(pgn & 0xFF);
    d[5]=(uint8_t)((pgn>>8) & 0xFF);
    d[6]=(uint8_t)((pgn>>16) & 0xFF);
    d[7]=0xFF;
    return n2k_send_single(priority, 59392, sa, dst, d, 8, 10);
}
