/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Registry of human-readable NM2K PGN decoders for monitor screen.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "n2k_transport.h"

typedef bool (*nm2k_decode_fn_t)(const n2k_msg_t *msg, char *line, size_t cap);

bool nm2k_decode_message(const n2k_msg_t *msg, char *line, size_t cap);
