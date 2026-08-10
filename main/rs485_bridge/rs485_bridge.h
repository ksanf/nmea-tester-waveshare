/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *rs485_bridge_create(lv_obj_t *parent);
lv_obj_t *rs485_bridge_get_screen(void);

#ifdef __cplusplus
}
#endif
