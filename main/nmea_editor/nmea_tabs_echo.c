/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_tabs_echo.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/ui_colors.h"
#include "ui/ui_nmea_widgets.h"
#include "ui/ui_theme.h"
#include "rs485/rs485_simui.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_TABS_ECHO
#include "config_logs.h"
#include <stdio.h>
#include <stdlib.h>

/* ───────── Layout constants ───────── */
#define GAP_X 16   /* horizontal gap */
#define GAP_Y 12   /* vertical gap   */
#define LINE_H 44 /* canonical row height */

LV_FONT_DECLARE(lv_font_montserrat_16)


/* ───────── Callback helpers ───────── */
static void cb_bool(lv_event_t *e)
{
    bool *dst = lv_event_get_user_data(e);
    const bool value = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    (void)nmea_templates_write_field(dst, &value, sizeof(value));
}

static void cb_fstep(lv_event_t *e)
{
    lv_obj_t *lbl = lv_event_get_user_data(e);
    float *dst    = lv_obj_get_user_data(lbl);
    const float value = strtof(lv_label_get_text(lbl), NULL);
    (void)nmea_templates_write_field(dst, &value, sizeof(value));
}

/* ───────── Build tab ───────── */
void echo_tab_create(lv_obj_t *parent)
{
    nmea_echo_t snapshot;
    nmea_templates_echo_snapshot(&snapshot);
    ui_form_parent_apply(parent);

    int y = 0;

    /* ---------- Message & CRC checkboxes (2‑column grid) ------------ */
    static const char *lbl_chk[5] = {"DBT","DPT","DBK","DBS","CRC"};
    bool *flag_chk[5] = {&g_nmea_echo.send_dbt,&g_nmea_echo.send_dpt,
                         &g_nmea_echo.send_dbk,&g_nmea_echo.send_dbs,
                         &g_nmea_echo.add_crc};
    const bool flag_values[5] = {snapshot.send_dbt, snapshot.send_dpt, snapshot.send_dbk, snapshot.send_dbs, snapshot.add_crc};


    for(int i=0;i<5;i++){
        int x   = (i % 3) * (230 + GAP_X);
        int row = i / 3;
        int y0  = y + row*(LINE_H + GAP_Y);
        ui_set_cursor_y(y0);
        lv_obj_t *cb = ui_checkbox_create(parent, lbl_chk[i], flag_values[i]);
        lv_obj_set_pos(cb, x, y0);
        lv_obj_add_event_cb(cb, cb_bool, LV_EVENT_VALUE_CHANGED, flag_chk[i]);
    }
    /* rows used: ceil(5/3)=2 */
    y += 2*LINE_H + GAP_Y;

    /* ---------- Frequency ------------------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_freq = ui_float_stepper_create(parent, "Freq (Hz)",
                                                snapshot.rate_hz, 0.1f);
    lv_obj_set_user_data(sp_freq, &g_nmea_echo.rate_hz);
    lv_obj_add_event_cb(sp_freq, cb_fstep, LV_EVENT_CLICKED, sp_freq);
    y += LINE_H + GAP_Y;

    /* ---- Talker dropdown ------------------------------------------ */
    static const char *talk_opts[] = {"SD","YX"};
    int sel = (strcmp(snapshot.talker_id,"YX") == 0) ? 1 : 0;

    ui_set_cursor_y(y);
    lv_obj_t *dd_talker = ui_dropdown_create(parent, "Talker",
                                             talk_opts, 2, sel);
    (void)ui_dropdown_bind(dd_talker, g_nmea_echo.talker_id, 1, GRP_ECHO);
    y += LINE_H + GAP_Y;

    /* ---- Prefix dropdown ------------------------------------------ */
    static const char *pref_opts[] = {"$","!"};

    ui_set_cursor_y(y);
    lv_obj_t *dd_prefix = ui_dropdown_create(parent, "Prefix",
                                             pref_opts, 2,
                                             (snapshot.prefix=='!') ? 1 : 0);
    (void)ui_dropdown_bind(dd_prefix, &g_nmea_echo.prefix, 0, GRP_ECHO);
    y += LINE_H + GAP_Y;

    /* ---------- Depth (m) ------------------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_depth_m = ui_float_stepper_create(parent, "Depth (m)",
                                                  snapshot.depth_m, 0.1f);
    lv_obj_set_user_data(sp_depth_m, &g_nmea_echo.depth_m);
    lv_obj_add_event_cb(sp_depth_m, cb_fstep, LV_EVENT_CLICKED, sp_depth_m);
    y += LINE_H + GAP_Y;

    /* ---------- Depth (ft) ----------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_depth_ft = ui_float_stepper_create(parent, "Depth (ft)",
                                                   snapshot.depth_ft, 0.1f);
    lv_obj_set_user_data(sp_depth_ft, &g_nmea_echo.depth_ft);
    lv_obj_add_event_cb(sp_depth_ft, cb_fstep, LV_EVENT_CLICKED, sp_depth_ft);
    y += LINE_H + GAP_Y;

    /* ---------- Depth (fth) ---------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_depth_fth = ui_float_stepper_create(parent, "Depth (fth)",
                                                    snapshot.depth_fathom, 0.1f);
    lv_obj_set_user_data(sp_depth_fth, &g_nmea_echo.depth_fathom);
    lv_obj_add_event_cb(sp_depth_fth, cb_fstep, LV_EVENT_CLICKED, sp_depth_fth);
    y += LINE_H + GAP_Y;

    /* ---------- Keel offset (m) ------------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_offset = ui_float_stepper_create(parent, "Keel offset (m)",
                                                 snapshot.keel_offset_m, 0.1f);
    lv_obj_set_user_data(sp_offset, &g_nmea_echo.keel_offset_m);
    lv_obj_add_event_cb(sp_offset, cb_fstep, LV_EVENT_CLICKED, sp_offset);
    y += LINE_H + GAP_Y;

    /* Final layout update */
    lv_obj_update_layout(parent);
    ESP_LOGI("echo_tab", "ECHO tab ready (layout v1.1.1, ctx free fix)");
}
