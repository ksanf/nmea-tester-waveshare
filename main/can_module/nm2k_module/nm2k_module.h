/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NM2K monitor screen backed by internal NMEA2000 stack.
 */

#pragma once

#include <lvgl.h>

lv_obj_t *nm2k_create(lv_obj_t *parent);
void nm2k_module_start(lv_obj_t *parent);
