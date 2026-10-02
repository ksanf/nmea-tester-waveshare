/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
/* Shared English labels for the local bridge screen and web status. */
#include "sailor_telemetry.h"

typedef struct {
    bool known, fresh;
    char text[40];
} sailor_status_value_t;
typedef struct {
    sailor_status_value_t ocean, registration, protocol, channel;
} sailor_network_text_t;

void sailor_status_format_network(const protocol_antenna_status_t *status,
                                   sailor_network_text_t *out);
