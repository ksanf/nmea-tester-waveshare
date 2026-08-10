/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NUMERIC_EDITOR_H
#define NUMERIC_EDITOR_H

#include "lvgl.h"

typedef struct {
    uint8_t start_raw;     /* Field start in the raw buffer. */
    uint8_t digits;        /* Number of digits in the field. */
    uint16_t min_val;      /* Minimum field value. */
    uint16_t max_val;      /* Maximum field value. */
} field_limit_t;

void num_edit_init(void);
void num_edit_cancel(void);
void num_edit_bind(lv_obj_t *label, char *raw, const int8_t *map, uint8_t len,
                   void (*fmt)(char *), void (*commit)(void),
                   const field_limit_t *limits, uint8_t num_fields);

#endif /* NUMERIC_EDITOR_H */
