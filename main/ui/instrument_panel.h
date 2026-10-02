/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief NMEA instrument-panel interface.
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/*━━━━━━━━━━ Public data structure ━━━━━━━━━*/
typedef struct {
    /* Positional */
    float latitude;   char lat_dir;   /* 'N' / 'S' */
    float longitude;  char lon_dir;   /* 'E' / 'W' */

    /* Time/Date */
    uint8_t hour, minute, second;     /* UTC */
    uint8_t day, month; uint16_t year;

    /* Navigation */
    float course_deg; bool from_gyro; /* true = GYRO, false = GPS */
    float speed_knots; bool from_log; /* true = LOG,  false = GPS */

    /* Depth */
    float depth_m;    bool has_depth;

    /* CRC state */
    bool  crc_available;
    bool  crc_ok;           /* Display "?" when available is false. */
} nmea_data_t;

/*━━━━━━━━━━ API ━━━━━━━━━*/

/** Create the panel. */
lv_obj_t *instrument_panel_init(lv_obj_t *parent,
                                int x, int y,
                                int w, int h);

/** Thread-safe model adapter; does not require a panel or the LVGL lock. */
void instrument_panel_process(const char *line);

/** Force a UI refresh after direct data changes. */
void instrument_panel_update(const nmea_data_t *ext_data);

/** Render a model snapshot; call on the LVGL task or with its lock held. */
void instrument_panel_flush(void);

void refresh_template(void);

#ifdef __cplusplus
}
#endif
