/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_motion.h"

#include <math.h>

float nmea_motion_heading_advance(float heading_deg,
                                  float rot_deg_min,
                                  uint32_t elapsed_ms)
{
    if (!isfinite(heading_deg)) heading_deg = 0.0f;
    if (!isfinite(rot_deg_min)) rot_deg_min = 0.0f;

    const double advanced = (double)heading_deg +
                            ((double)rot_deg_min * (double)elapsed_ms / 60000.0);
    double normalized = fmod(advanced, 360.0);
    if (normalized < 0.0) normalized += 360.0;
    return (float)normalized;
}
