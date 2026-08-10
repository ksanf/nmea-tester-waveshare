/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester main-screen interface.
 */

#ifndef SCREEN_UI_H
#define SCREEN_UI_H

#include "lvgl.h"

/**
 * @brief Initialize the main screen.
 */
void screen_ui_init(void);

/**
 * @brief Show the main screen.
 */
void screen_ui_show(void);

/**
 * @brief Get the main screen.
 * @return Pointer to the LVGL main screen.
 */
lv_obj_t *screen_ui_get_main(void);

#endif // SCREEN_UI_H
