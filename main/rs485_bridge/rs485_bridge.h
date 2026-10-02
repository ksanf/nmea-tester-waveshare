/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#include "rs485/rs485_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *rs485_bridge_create(lv_obj_t *parent);
lv_obj_t *rs485_bridge_get_screen(void);

/* View-only calls require the LVGL lock; they never stop the runtime. */
void rs485_bridge_view_suspend(bool suspended);
lv_obj_t *rs485_bridge_view_show(lv_obj_t *parent);
void rs485_bridge_view_destroy(void);

#ifdef __cplusplus
}
#endif
