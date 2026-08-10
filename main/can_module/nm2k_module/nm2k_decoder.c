/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Registry of built-in NM2K traffic decoders.
 */

#include "nm2k_decoder.h"

bool nm2k_decode_fluid_level(const n2k_msg_t *msg, char *line, size_t cap);
bool nm2k_decode_rudder(const n2k_msg_t *msg, char *line, size_t cap);
bool nm2k_decode_gps(const n2k_msg_t *msg, char *line, size_t cap);
bool nm2k_decode_ais(const n2k_msg_t *msg, char *line, size_t cap);
bool nm2k_decode_nav(const n2k_msg_t *msg, char *line, size_t cap);
bool nm2k_decode_engine(const n2k_msg_t *msg, char *line, size_t cap);

static const nm2k_decode_fn_t s_decoders[] = {
    nm2k_decode_rudder,
    nm2k_decode_fluid_level,
    nm2k_decode_gps,
    nm2k_decode_ais,
    nm2k_decode_nav,
    nm2k_decode_engine,
};

bool nm2k_decode_message(const n2k_msg_t *msg, char *line, size_t cap)
{
    for (size_t i = 0; i < (sizeof(s_decoders) / sizeof(s_decoders[0])); ++i) {
        if (s_decoders[i] && s_decoders[i](msg, line, cap)) {
            return true;
        }
    }
    return false;
}
