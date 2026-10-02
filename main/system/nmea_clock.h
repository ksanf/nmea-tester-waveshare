/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

void nmea_clock_init(void);   /* Start the FreeRTOS task. */
/* Set UTC in the supported 2000..2099 interval. Successful writes update RTC
 * (when enabled), CPU fallback and template under the same clock mutex. */
esp_err_t nmea_clock_set_time_epoch(time_t epoch);
/* Copy HHMMSS/DDMMYY from the template through the same synchronized setter.
 * Returns false on invalid fields or a failed clock write. */
bool nmea_clock_sync_from_template(void);

#ifdef __cplusplus
}
#endif
