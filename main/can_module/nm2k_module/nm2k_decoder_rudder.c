/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoder for PGN 127245 Rudder.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "nm2k_decoder.h"

static bool angle_is_na_(int16_t raw)
{
    return raw == (int16_t)0x7FFF || raw == (int16_t)0x7FFE || raw == (int16_t)-1;
}

static int32_t rad1e4_to_deg_tenths_(int16_t raw)
{
    int32_t v = (int32_t)raw * 573;
    if (v >= 0) {
        return (v + 5000) / 10000;
    }
    return (v - 5000) / 10000;
}

static const char *dir_order_name_(uint8_t order)
{
    switch (order) {
        case 0u: return NULL;
        case 1u: return "ord=STBD";
        case 2u: return "ord=PORT";
        case 7u: return "ord=NA";
        default: return "ord=?";
    }
}

static const char *pos_side_(int16_t raw)
{
    if (raw > 0) return "STBD";
    if (raw < 0) return "PORT";
    return "MID";
}

bool nm2k_decode_rudder(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t instance;
    uint8_t direction_order;
    int16_t angle_order_raw;
    int16_t position_raw;
    int32_t position_tenths;
    const char *dir_order_txt;

    if (!msg || !line || cap == 0u) return false;
    if (msg->pgn != 127245u || msg->len < 8u) return false;

    instance = msg->data[0];
    direction_order = msg->data[1] & 0x07u;
    angle_order_raw = (int16_t)((uint16_t)msg->data[2] | ((uint16_t)msg->data[3] << 8));
    position_raw = (int16_t)((uint16_t)msg->data[4] | ((uint16_t)msg->data[5] << 8));
    dir_order_txt = dir_order_name_(direction_order);

    if (angle_is_na_(position_raw)) {
        snprintf(line, cap,
                 "%06" PRIu32 " RUDDER %u pos=NA%s%s",
                 msg->pgn,
                 (unsigned)instance,
                 dir_order_txt ? " " : "",
                 dir_order_txt ? dir_order_txt : "");
        return true;
    }

    position_tenths = rad1e4_to_deg_tenths_(position_raw);
    if (angle_is_na_(angle_order_raw)) {
        snprintf(line, cap,
                 "%06" PRIu32 " RUDDER %u %s %" PRId32 ".%" PRId32 "deg%s%s",
                 msg->pgn,
                 (unsigned)instance,
                 pos_side_(position_raw),
                 position_tenths / 10,
                 position_tenths < 0 ? -(position_tenths % 10) : (position_tenths % 10),
                 dir_order_txt ? " " : "",
                 dir_order_txt ? dir_order_txt : "");
    } else {
        snprintf(line, cap,
                 "%06" PRIu32 " RUDDER %u %s %" PRId32 ".%" PRId32 "deg cmd-set%s%s",
                 msg->pgn,
                 (unsigned)instance,
                 pos_side_(position_raw),
                 position_tenths / 10,
                 position_tenths < 0 ? -(position_tenths % 10) : (position_tenths % 10),
                 dir_order_txt ? " " : "",
                 dir_order_txt ? dir_order_txt : "");
    }
    return true;
}
