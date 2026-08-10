/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NMEA_TABS_GPS_H
#define NMEA_TABS_GPS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the GPS tab inside the supplied container.
 *
 * @param parent Flex-column LVGL container positioned by nmea_editor; the tab
 *               fills the entire container.
 */
void gps_tab_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
#endif /* NMEA_TABS_GPS_H */
