/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Interface for shared UI button helpers.
 */

#pragma once
#include "lvgl.h"

/**
 * @brief Apply the themed gradient background to a screen.
 *
 * @param screen LVGL screen object created with lv_obj_create(NULL).
 */
void dialog_ui_apply_screen_bg(lv_obj_t *screen);

/**
 * @brief Create a fully styled button.
 *
 * @param parent Parent LVGL object.
 * @param x, y Top-left coordinates.
 * @param w, h Width and height.
 * @param txt Button label.
 * @param bg_hex 24-bit background color.
 * @param cb LVGL callback; may be NULL.
 * @param user_data LVGL callback data; may be NULL.
 *
 * @return Pointer to the created button.
 */
lv_obj_t *dialog_ui_create_button(lv_obj_t *parent,
                                  int16_t   x,
                                  int16_t   y,
                                  int16_t   w,
                                  int16_t   h,
                                  const char *txt,
                                  uint32_t  bg_hex,
                                  lv_event_cb_t cb,
                                  void      *user_data,
                                  const lv_font_t *font);

/**
 * @brief Apply the shared checked-state style to an existing button.
 *
 * @param btn Button created with dialog_ui_create_button().
 * @param bg_hex Background color for the checked state.
 */
void dialog_ui_apply_checked_style(lv_obj_t *btn, uint32_t bg_hex);
