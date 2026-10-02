/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_baud_selector_create(lv_obj_t *parent);
/* Refresh the actual driver setting from an existing view timer/show callback;
 * no dedicated timer is allocated by this widget. Call under the LVGL lock. */
void ui_baud_selector_sync(lv_obj_t *dropdown);

#ifdef __cplusplus
}
#endif

