/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef SCREEN_SETTINGS_H
#define SCREEN_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief Load the settings screen.
 *
 * Creates a standalone LVGL screen with the current settings controls.
 *
 * @param parent Unused; the screen is created independently.
 */
void screen_settings_show(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_SETTINGS_H
