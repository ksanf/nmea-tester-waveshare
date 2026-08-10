/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Pure NMEA 0183 transmitter format profile selection.
 */

#include "nmea_editor/nmea_version.h"

static const nmea_version_profile_t s_profiles[NMEA_VERSION_COUNT] = {
    [NMEA_VERSION_2_1] = {
        .version = NMEA_VERSION_2_1,
        .short_name = "2.1",
        .position_mode = false,
        .rmc_nav_status = false,
        .ths_heading = false,
    },
    [NMEA_VERSION_2_3] = {
        .version = NMEA_VERSION_2_3,
        .short_name = "2.3",
        .position_mode = true,
        .rmc_nav_status = false,
        .ths_heading = false,
    },
    [NMEA_VERSION_4_0] = {
        .version = NMEA_VERSION_4_0,
        .short_name = "4.0",
        .position_mode = true,
        .rmc_nav_status = false,
        .ths_heading = false,
    },
    [NMEA_VERSION_4_10] = {
        .version = NMEA_VERSION_4_10,
        .short_name = "4.10",
        .position_mode = true,
        .rmc_nav_status = true,
        .ths_heading = false,
    },
    [NMEA_VERSION_4_11] = {
        .version = NMEA_VERSION_4_11,
        .short_name = "4.11",
        .position_mode = true,
        .rmc_nav_status = true,
        .ths_heading = true,
    },
};

/* Keep existing installations output-compatible with the old formatter. */
static nmea_version_t s_version = NMEA_VERSION_2_3;

static bool version_valid_(nmea_version_t version)
{
    return version >= NMEA_VERSION_2_1 && version < NMEA_VERSION_COUNT;
}

bool nmea_version_set_runtime(nmea_version_t version)
{
    if (!version_valid_(version)) return false;
    s_version = version;
    return true;
}

nmea_version_t nmea_version_get(void)
{
    return s_version;
}

const nmea_version_profile_t *nmea_version_profile(void)
{
    return &s_profiles[s_version];
}

const char *nmea_version_short_name(void)
{
    return s_profiles[s_version].short_name;
}

const char *nmea_version_options(void)
{
    return "NMEA 0183 2.1\n"
           "NMEA 0183 2.3\n"
           "NMEA 0183 4.0\n"
           "NMEA 0183 4.10\n"
           "NMEA 0183 4.11";
}

