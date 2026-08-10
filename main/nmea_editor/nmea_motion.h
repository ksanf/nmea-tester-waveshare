/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * Pure helpers for deterministic NMEA transmitter motion simulation.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Advance a heading by a signed rate of turn. Negative ROT turns to port.
 * The result is always normalized to the [0, 360) degree interval. */
float nmea_motion_heading_advance(float heading_deg,
                                  float rot_deg_min,
                                  uint32_t elapsed_ms);

#ifdef __cplusplus
}
#endif
