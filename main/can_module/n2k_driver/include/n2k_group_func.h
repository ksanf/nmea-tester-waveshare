/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <esp_err.h>

/* Initialize reception of PGN 126208 and send basic ACK responses. */
esp_err_t n2k_group_func_init(void);
