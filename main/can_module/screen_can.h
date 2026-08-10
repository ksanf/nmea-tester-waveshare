/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   CAN submodule selection screen interface.
 */

#ifndef SCREEN_CAN_H
#define SCREEN_CAN_H

#include "lvgl.h"

/**
 * @brief Show the CAN submodule selection screen
 * @param parent Parent LVGL object (main menu)
 * @return Pointer to the created LVGL screen
 */
lv_obj_t *screen_can_show(lv_obj_t *parent);

#endif // SCREEN_CAN_H
