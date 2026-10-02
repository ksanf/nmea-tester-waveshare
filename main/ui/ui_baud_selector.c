/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/ui_baud_selector.h"
#include "app/app_controller.h"
#include "ui/ui_theme.h"
#include "rs485/rs485_driver.h"
#include <stdio.h>
#include <string.h>
#include "lvgl.h"

/* Supported baud rates.                                                 */
static const uint32_t BAUDS[] = {2400, 4800, 9600, 19200, 38400, 115200};
#define BAUD_CNT (sizeof(BAUDS) / sizeof(BAUDS[0]))

/* Font enabled in lv_conf.h. */
LV_FONT_DECLARE(lv_font_montserrat_20)

/* ——— Synchronize the selector from runtime state ——————————————— */
void ui_baud_selector_sync(lv_obj_t *dropdown)
{
    if (!dropdown || !lv_obj_is_valid(dropdown)) return;
    const uint32_t baud = rs485_get_baudrate();
    for (uint32_t i = 0; i < BAUD_CNT; ++i) {
        if (BAUDS[i] == baud) {
            if (lv_dropdown_get_selected(dropdown) != i) {
                lv_dropdown_set_selected(dropdown, i);
            }
            return;
        }
    }
}

static void event_cb(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target(e);
    const uint32_t selected = lv_dropdown_get_selected(dropdown);
    if (selected >= BAUD_CNT || !app_controller_local_baud(BAUDS[selected])) {
        ui_baud_selector_sync(dropdown);
    }
}

/* ——— Public API ————————————————————————— */
lv_obj_t *ui_baud_selector_create(lv_obj_t *parent)
{
    /* 1. Build "4800 Bd\n9600 Bd\n..." without a trailing newline. */
    char opts[80] = {0};
    size_t used = 0;
    for(size_t i = 0; i < BAUD_CNT; ++i) {
        int written = snprintf(opts + used, sizeof(opts) - used, "%lu Bd%s",
                               (unsigned long)BAUDS[i],
                               (i < BAUD_CNT - 1) ? "\n" : "");
        if (written < 0 || (size_t)written >= sizeof(opts) - used) break;
        used += (size_t)written;
    }

    /* 2. Create the dropdown. */
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_size(dd, 200, 44);                       /* Width and height */
    lv_obj_align(dd, LV_ALIGN_TOP_MID, 0, 20);          /* Top-center position */

    ui_theme_apply_dropdown(dd, &lv_font_montserrat_20);

    /* 4. Select the driver's current baud rate. */
    int sel = 0;
    for(int i = 0; i < BAUD_CNT; ++i)
        if(BAUDS[i] == rs485_get_baudrate()) { sel = i; break; }
    lv_dropdown_set_selected(dd, sel);                  /* Defaults to 4800 Bd. */

    /* 5. Handle value changes only. */
    lv_obj_add_event_cb(dd, event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}
