/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief Waveshare board-control interface.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t waveshare_i2c_init(void);
esp_err_t waveshare_board_init(void);
esp_err_t waveshare_lcd_set_backlight(bool on);
esp_err_t waveshare_lcd_reset(void);
esp_err_t waveshare_touch_set_reset(bool released);
esp_err_t waveshare_touch_reset(void);

#ifdef __cplusplus
}
#endif
