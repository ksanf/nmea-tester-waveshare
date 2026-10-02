/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   WiFi settings screen.
 */

#ifndef SCREEN_WIFI_H
#define SCREEN_WIFI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void screen_wifi_show(lv_obj_t *parent);
/* Pause presentation polling; an in-progress scan can finish independently.
 * Call while holding the LVGL lock. */
void screen_wifi_suspend(bool suspended);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_WIFI_H */
