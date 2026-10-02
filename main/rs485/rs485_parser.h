/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester RX-485 parser screen with a bit indicator.
 */

#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <esp_err.h>

#include "rs485/rs485_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Create the RX-485 Parser screen and start receiving data.
 *
 * @param scr_main Main screen used by the Back button.
 * @return Newly created LVGL screen object.
 */
lv_obj_t *rs485_parser_create(lv_obj_t *scr_main);

void rs485_parser_resume(void);
void rs485_parser_flush_lines(void);
/* View-only calls require the LVGL lock; they never stop the runtime. */
void rs485_parser_view_suspend(bool suspended);
lv_obj_t *rs485_parser_view_show(lv_obj_t *parent);
void rs485_parser_view_destroy(void);

#ifdef __cplusplus
}
#endif
