/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   RGB display and LVGL initialization interface.
 */

#pragma once
#include <stdbool.h>
#include <esp_err.h>
#include "lvgl.h"
/**
 * @brief Initialize the Waveshare ESP32-S3-Touch-LCD-5/5B RGB display and LVGL.
 */
void screen_init(void);
lv_disp_t *screen_get_display(void);
bool screen_orientation_is_rotated(void);
esp_err_t screen_orientation_set_rotated(bool rotated, bool persist);
