/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void nmea_clock_init(void);   /* Start the FreeRTOS task. */
/* Read HHMMSS/DDMMYY from g_nmea_gps and apply them to the system clock
 * through settimeofday or rtc_set_time. Returns true for a valid template. */
bool nmea_clock_sync_from_template(void);

#ifdef __cplusplus
}
#endif
