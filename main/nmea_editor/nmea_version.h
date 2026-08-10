/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA 0183 transmitter format profiles and persistence API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NMEA_VERSION_2_1 = 0,
    NMEA_VERSION_2_3,
    NMEA_VERSION_4_0,
    NMEA_VERSION_4_10,
    NMEA_VERSION_4_11,
    NMEA_VERSION_COUNT
} nmea_version_t;

typedef struct {
    nmea_version_t version;
    const char *short_name;
    bool position_mode;  /* RMC/GLL/VTG Mode Indicator (2.3+) */
    bool rmc_nav_status; /* RMC Navigational Status (4.10+) */
    bool ths_heading;    /* THS replaces generated HDT in 4.11 profile */
} nmea_version_profile_t;

/* Load the selected version from its own NVS key. Default is NMEA 2.3. */
esp_err_t nmea_version_init(void);

/* Change the active version, optionally persist it, and refresh all TX groups. */
esp_err_t nmea_version_set(nmea_version_t version, bool persist);

/* Pure runtime selector used by the formatter tests and by NVS loading. */
bool nmea_version_set_runtime(nmea_version_t version);

nmea_version_t nmea_version_get(void);
const nmea_version_profile_t *nmea_version_profile(void);
const char *nmea_version_short_name(void);
const char *nmea_version_options(void);

#ifdef __cplusplus
}
#endif

