/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Instrument panel and NMEA parser with LVGL recolor markup.
 */

#include "ui/instrument_panel.h"
#include "app/navigation_model.h"
#include "ui/ui_theme.h"
#include <stdio.h>

#define FONT_MAIN &lv_font_montserrat_16
#define LINE_H 18
#define ROWS 3
#define COLS 4
#define CELL_H LINE_H

/* ────────── LVGL ────────── */
static lv_obj_t *panel = NULL;
static lv_obj_t *lbl[ROWS * COLS] = {0};

static uint32_t panel_bg_hex_(void)
{
    return ui_theme_get()->panel_bg;
}

static const char *panel_lbl_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "404a54";
        case UI_THEME_BW:    return "d0d0d0";
        default:             return "ffff00";
    }
}

static const char *panel_val_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "1f252b";
        case UI_THEME_BW:    return "ffffff";
        default:             return "ffffff";
    }
}

/* ────────── ui_update ────────── */
static void ui_update(void){
    /* Do not touch widgets after the panel has been deleted. */
    if (!panel) return;

    char buf[64];
    navigation_snapshot_t nav;
    navigation_model_snapshot(&nav);
    bool sc = (nav.stale_fields & NAVIGATION_COORDS) != 0;
    bool st = (nav.stale_fields & NAVIGATION_TIME) != 0;
    bool sd = (nav.stale_fields & NAVIGATION_DATE) != 0;
    bool sh_gps = (nav.stale_fields & NAVIGATION_GPS_MOTION) != 0;
    bool sh_gyro = (nav.stale_fields & NAVIGATION_GYRO) != 0;
    bool sh_log = (nav.stale_fields & NAVIGATION_LOG) != 0;
    bool sdpt = (nav.stale_fields & NAVIGATION_DEPTH) != 0;
    bool sw = (nav.stale_fields & NAVIGATION_WIND) != 0;
    bool stp = (nav.stale_fields & NAVIGATION_TEMPERATURE) != 0;
    const char *c_lbl = panel_lbl_hex_();
    const char *c_val = panel_val_hex_();

    /* A valid first label implies that the label grid is initialized. */
    if (!lbl[0]) return;

    /* Col 0: LAT | UTC | gyro_heading */
    // Row 0, Col 0: LAT
    if (sc) snprintf(buf, sizeof(buf), "#%s -- #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s##%s %.4f %c #", c_lbl, LV_SYMBOL_GPS, c_val, nav.lat, nav.lat_dir);
    lv_label_set_text(lbl[0], buf);

    // Row 1, Col 0: UTC
    if (st) snprintf(buf, sizeof(buf), "#%s --:--:-- #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %c%c:%c%c:%c%c #",
                 c_lbl, LV_SYMBOL_BELL, c_val, nav.time[0], nav.time[1], nav.time[2], nav.time[3], nav.time[4], nav.time[5]);
    lv_label_set_text(lbl[4], buf);

    // Row 2, Col 0: gyro_heading
    if (sh_gyro) snprintf(buf, sizeof(buf), "#%s --.-° #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %.1f° GY #", c_lbl, LV_SYMBOL_REFRESH, c_val, nav.gyro_heading);
    lv_label_set_text(lbl[8], buf);

    /* Col 1: LON | DATE | log_speed */
    // Row 0, Col 1: LON
    if (sc) snprintf(buf, sizeof(buf), "#%s -- #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s##%s %.4f %c #", c_lbl, LV_SYMBOL_GPS, c_val, nav.lon, nav.lon_dir);
    lv_label_set_text(lbl[1], buf);

    // Row 1, Col 1: DATE
    if (sd) snprintf(buf, sizeof(buf), "#%s --.--.---- #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s #", c_val, nav.date);
    lv_label_set_text(lbl[5], buf);

    // Row 2, Col 1: log_speed
    if (sh_log) snprintf(buf, sizeof(buf), "#%s --.- kn #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %.1f kn LG #", c_lbl, LV_SYMBOL_DRIVE, c_val, nav.log_speed);
    lv_label_set_text(lbl[9], buf);

    /* Col 2: gps_sog | gps_cog | DEPTH */
    // Row 0, Col 2: gps_sog
    if (sh_gps) snprintf(buf, sizeof(buf), "#%s --.- kn #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %.1f kn GP #", c_lbl, LV_SYMBOL_DRIVE, c_val, nav.gps_sog);
    lv_label_set_text(lbl[2], buf);

    // Row 1, Col 2: gps_cog
    if (sh_gps) snprintf(buf, sizeof(buf), "#%s --.-° #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %.1f° GP #", c_lbl, LV_SYMBOL_REFRESH, c_val, nav.gps_cog);
    lv_label_set_text(lbl[6], buf);

    // Row 2, Col 2: DEPTH
    if (sdpt) snprintf(buf, sizeof(buf), "#%s --.- m #", c_val);
    else snprintf(buf, sizeof(buf), "#%s %s ##%s %.1f m #", c_lbl, LV_SYMBOL_DOWN, c_val, nav.depth);
    lv_label_set_text(lbl[10], buf);

    /* Col 3: wind_angle | wind_speed | temp_c; blank after timeout. */
    // Row 0, Col 3: wind_angle
    if (sw) lv_label_set_text(lbl[3], "");
    else { snprintf(buf, sizeof(buf), "#%s %.1f° %c #", c_val, nav.wind_angle, nav.wind_true ? 'T' : 'R');
    lv_label_set_text(lbl[3], buf); }

    // Row 1, Col 3: wind_speed
    if (sw) lv_label_set_text(lbl[7], "");
    else { snprintf(buf, sizeof(buf), "#%s %.1f kn #", c_val, nav.wind_speed);
    lv_label_set_text(lbl[7], buf); }

    // Row 2, Col 3: temp_c
    if (stp) lv_label_set_text(lbl[11], "");
    else { snprintf(buf, sizeof(buf), "#%s %.1f° C #", c_val, nav.temp_c);
    lv_label_set_text(lbl[11], buf); }
}

/**
 * @brief Clear widget references when the panel is deleted.
 */
static void panel_delete_cb(lv_event_t *e)
{
    for (int i = 0; i < ROWS * COLS; i++) lbl[i] = NULL;
    panel = NULL;
}

/* Legacy adapters. Parsing has no screen lifetime dependency. */
void instrument_panel_process(const char *line)
{
    navigation_model_process(line);
}

void instrument_panel_flush(void) {
    if (panel) ui_update();
}

lv_obj_t *instrument_panel_init(lv_obj_t *parent,int x,int y,int w,int h){
    panel = lv_obj_create(parent);
    lv_obj_set_size(panel,w,h); lv_obj_set_pos(panel,x,y);
    lv_obj_set_style_bg_color(panel,lv_color_hex(panel_bg_hex_()),0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(ui_theme_get()->border), 0);
    lv_obj_set_scrollbar_mode(panel,LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel,2,0);  // Compact padding
    lv_obj_set_style_pad_column(panel,2,0);
    lv_obj_set_style_pad_row(panel,0,0);

    // Grid layout: 3 rows, 4 cols
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {CELL_H, CELL_H, CELL_H, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(panel, col_dsc, row_dsc);
    lv_obj_set_layout(panel, LV_LAYOUT_GRID);

    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            int idx = row * COLS + col;
            lbl[idx] = lv_label_create(panel);
            lv_label_set_recolor(lbl[idx],true);
            lv_obj_set_style_text_font(lbl[idx], FONT_MAIN, 0);
            lv_obj_set_style_text_color(lbl[idx], lv_color_hex(ui_theme_get()->text), 0);
            lv_obj_set_style_text_align(lbl[idx], LV_TEXT_ALIGN_LEFT, 0);  // Left aligned
            lv_obj_set_grid_cell(lbl[idx], LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_CENTER, row, 1);
            lv_label_set_long_mode(lbl[idx], LV_LABEL_LONG_DOT);  // Ellipsize overflow
        }
    }

    /* Clear references when the panel is deleted. */
    lv_obj_add_event_cb(panel, panel_delete_cb, LV_EVENT_DELETE, NULL);

    ui_update();
    return panel;
}

void refresh_template(void)
{
    (void)navigation_model_refresh_templates();
}
