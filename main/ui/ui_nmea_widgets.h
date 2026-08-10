/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "lvgl.h"

/* Shared dimensions and colors. */
#define UI_ROW_H_PX       42
#define UI_ROW_GAP_PX     20
#define UI_LABEL_W_PX    160
#define UI_FIELD_X_PX   (UI_LABEL_W_PX + 8)

/* Reset the vertical cursor for a new block or right-hand column. */
void ui_set_cursor_y(int new_y);
void ui_form_parent_apply(lv_obj_t *parent);

/* Base widgets */
lv_obj_t *ui_textfield_create (lv_obj_t*, const char*, const char*, uint16_t, uint16_t);
lv_obj_t *ui_int_stepper_create   (lv_obj_t*, const char*, int32_t,  int32_t,int32_t,int32_t);
lv_obj_t *ui_float_stepper_create (lv_obj_t*, const char*, float,    float);
lv_obj_t *ui_dropdown_create      (lv_obj_t*, const char*, const char**, uint8_t, uint8_t);
lv_obj_t *ui_checkbox_create      (lv_obj_t*, const char*, bool);
lv_obj_t *ui_latlon_create        (lv_obj_t*, const char*, const char*, const char*, uint16_t);
lv_obj_t *ui_value_label_create(lv_obj_t *parent,
                                const char *title,
                                const char *value,
                                uint16_t width);
typedef struct {
    void    *dst;       /* Destination for the result.       */
    uint8_t  is_str2;   /* 0 = char, 1 = two-character text. */
    int      grp_id;    /* GRP_GPS / GRP_GYRO …             */
} ui_drop_ctx_t;       
void ui_dropdown_generic_cb(lv_event_t *e);
bool ui_dropdown_bind(lv_obj_t *dd, void *dst, uint8_t is_str2, int grp_id);
