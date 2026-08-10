/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   CAN Bridge UI and tasks for the Sailor Inmarsat-C terminal tunnel.
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Create the UI and start the CAN bridge
 * @param parent Parent LVGL object
 * @return Pointer to the LVGL screen
 */
lv_obj_t *bridge_can_create(lv_obj_t *parent);

/**
 * @brief Start the CAN bridge
 * @param parent Parent LVGL object
 */
void bridge_can_module_start(lv_obj_t *parent);
