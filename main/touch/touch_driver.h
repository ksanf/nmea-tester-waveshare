/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   GT911 touch driver and LVGL input integration.
 */

#pragma once

#include <esp_err.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nmea_touch_init(lv_disp_t *disp);
lv_indev_t *touch_driver_get_indev(void);
bool touch_driver_read_raw(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
