/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include "AISdecoder/aisdecoder_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AISDEC_COLOR_BG_TOP          0x5656EE
#define AISDEC_COLOR_BG_BOTTOM       0x2E3339
#define AISDEC_COLOR_TITLE           0xE8ECEF
#define AISDEC_COLOR_BAR_TOP         0x41464D
#define AISDEC_COLOR_BAR_BOTTOM      0x262B31
#define AISDEC_COLOR_BAR_BORDER      0x7D848C
#define AISDEC_COLOR_BAR_TEXT        0xFFCA9F
#define AISDEC_COLOR_LIST_TOP        0x3C4148
#define AISDEC_COLOR_LIST_BOTTOM     0x252A30
#define AISDEC_COLOR_LIST_BORDER     0x747B84
#define AISDEC_COLOR_LIST_TEXT       0x49FF63
#define AISDEC_COLOR_LIST_CAPTION    0xFFE14A

lv_obj_t *aisdecoder_create(lv_obj_t *parent);
lv_obj_t *aisdecoder_get_screen(void);
/* Presentation only; call while holding the LVGL lock. */
void aisdecoder_view_suspend(bool suspended);
/* Release presentation without selecting a screen or changing RX ownership. */
void aisdecoder_view_destroy(void);

bool aisdecoder_is_ais_sentence(const char *line);
void aisdecoder_feed_nmea_line(const char *line);

#ifdef __cplusplus
}
#endif
