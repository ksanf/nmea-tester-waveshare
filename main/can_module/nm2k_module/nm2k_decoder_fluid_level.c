/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoder for PGN 127505 Fluid Level / tank sensors.
 */

#include <inttypes.h>
#include <stdio.h>

#define CFG_LOG_MODULE LOG_CFG_NM2K_DECODER
#include "config_logs.h"

#include "nm2k_decoder.h"

#define TAG "nm2k_dec"

static const char *fluid_type_name_(uint8_t fluid_type)
{
    switch (fluid_type) {
        case 0u: return "Fuel/Diesel";
        case 1u: return "Fresh Water";
        case 2u: return "Waste Water";
        case 3u: return "LiveWell";
        case 4u: return "Oil";
        case 5u: return "Black Water";
        case 6u: return "Gasoline";
        default: return "Unknown";
    }
}

bool nm2k_decode_fluid_level(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t fluid_instance;
    uint8_t fluid_type;
    uint16_t level_raw;
    uint32_t capacity_raw;
    uint32_t level_tenths;
    bool capacity_dna;

    if (!msg || !line || cap == 0u) return false;
    if (msg->pgn != 127505u || msg->len < 8u) return false;

    /* Standard PGN 127505 layout:
     * low nibble = Fluid Instance, high nibble = Fluid Type. */
    fluid_instance = msg->data[0] & 0x0Fu;
    fluid_type = (msg->data[0] >> 4) & 0x0Fu;
    level_raw = (uint16_t)msg->data[1] | ((uint16_t)msg->data[2] << 8);
    capacity_raw = (uint32_t)msg->data[3] |
                   ((uint32_t)msg->data[4] << 8) |
                   ((uint32_t)msg->data[5] << 16) |
                   ((uint32_t)msg->data[6] << 24);
    capacity_dna = (capacity_raw == 0xFFFFFFFFu);

    if (level_raw == 0xFFFFu) {
        snprintf(line, cap,
                 "%06" PRIu32 " TANK %u %s level=NA",
                 msg->pgn,
                 (unsigned)fluid_instance,
                 fluid_type_name_(fluid_type));
    } else {
        /* 1 raw unit = 0.004%; report in tenths without float formatting in RX task. */
        level_tenths = ((uint32_t)level_raw + 12u) / 25u;
        if (capacity_dna) {
            snprintf(line, cap,
                     "%06" PRIu32 " TANK %u %s %" PRIu32 ".%" PRIu32 "%%",
                     msg->pgn,
                     (unsigned)fluid_instance,
                     fluid_type_name_(fluid_type),
                     level_tenths / 10u,
                     level_tenths % 10u);
        } else {
            snprintf(line, cap,
                     "%06" PRIu32 " TANK %u %s %" PRIu32 ".%" PRIu32 "%% cap-set",
                     msg->pgn,
                     (unsigned)fluid_instance,
                     fluid_type_name_(fluid_type),
                     level_tenths / 10u,
                     level_tenths % 10u);
        }
    }
    return true;
}
