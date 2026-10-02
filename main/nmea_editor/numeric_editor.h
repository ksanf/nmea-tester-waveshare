/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NUMERIC_EDITOR_H
#define NUMERIC_EDITOR_H

#include "lvgl.h"

#include "nmea_editor/numeric_editor_validation.h"

void num_edit_init(void);
void num_edit_cancel(void);
/* Under the LVGL lock: apply a valid dirty field, otherwise restore its last
 * applied raw value. Close keyboard/cursor; false reports a rolled-back edit. */
bool num_edit_finish(void);
void num_edit_bind(lv_obj_t *label, char *raw, const int8_t *map, uint8_t len,
                   void (*fmt)(char *), void (*commit)(void),
                   const field_limit_t *limits, uint8_t num_fields);

#endif /* NUMERIC_EDITOR_H */
