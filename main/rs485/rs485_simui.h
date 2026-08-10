/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef RS485_SIMUI_H
#define RS485_SIMUI_H
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create or show the simulator screen. */
lv_obj_t *rs485_simui_create(lv_obj_t *parent);

/** Return the screen handle so other screens can navigate back. */
lv_obj_t *rs485_simui_get_screen(void);

/** Display a manually transmitted sentence in the TX485 window. */
void rs485_simui_log_tx(const char *msg);
void rs485_simui_flush_log(void);

#ifdef __cplusplus
}
#endif
#endif /* RS485_SIMUI_H */
