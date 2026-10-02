/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_tabs_weather.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/ui_colors.h"
#include "ui/ui_nmea_widgets.h"
#include "ui/ui_theme.h"
#include "rs485/rs485_simui.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_TABS_WEATHER
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

static void cb_freq(lv_event_t *e)
{
    const float value = strtof(lv_label_get_text(lv_event_get_target(e)), NULL);
    (void)nmea_templates_write_field(&g_nmea_weather.rate_hz, &value, sizeof(value));
}

/* ───────── Build tab ───────── */
void weather_tab_create(lv_obj_t *parent)
{
    nmea_weather_t snapshot;
    nmea_templates_weather_snapshot(&snapshot);
    ui_form_parent_apply(parent);

    int y = 0;

    /* ---------- Message checkboxes (3×2 grid) ----------------------- */
    static const char *lbl_chk[6] = {"MWD","MWV","VWR","VWT","MTW","CRC"};
    bool *flag_chk[6] = {&g_nmea_weather.send_mwd,&g_nmea_weather.send_mwv,
                         &g_nmea_weather.send_vwr,&g_nmea_weather.send_vwt,
                         &g_nmea_weather.send_mtw,&g_nmea_weather.add_crc};
    const bool flag_values[6] = {snapshot.send_mwd, snapshot.send_mwv, snapshot.send_vwr, snapshot.send_vwt, snapshot.send_mtw, snapshot.add_crc};


    for(int i = 0; i < 6; i++){
        int x   = (i % 3) * (100 + GAP_X);
        int row = i / 3;
        int y0  = y + row * (LINE_H + GAP_Y);
        ui_set_cursor_y(y0);
        lv_obj_t *cb = ui_checkbox_create(parent, lbl_chk[i], flag_values[i]);
        lv_obj_set_pos(cb, x, y0);
        lv_obj_add_event_cb(cb, cb_bool, LV_EVENT_VALUE_CHANGED, flag_chk[i]);
    }
    y += 2 * LINE_H + GAP_Y;

    /* ---------- Frequency ------------------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_freq = ui_float_stepper_create(parent, "Freq (Hz)",
                                               snapshot.rate_hz, 0.1f);
    lv_obj_set_user_data(sp_freq, &g_nmea_weather.rate_hz);
    lv_obj_add_event_cb(sp_freq, cb_freq, LV_EVENT_CLICKED, sp_freq);
    y += LINE_H + GAP_Y;

    /* ---------- Talker dropdown ------------------------------------ */
    static const char *talk_opts[] = {"WI","WV","YX"};
    int sel_talker = 0;
    for(int i=0;i<3;i++) if(strcmp(snapshot.talker_id, talk_opts[i]) == 0) sel_talker = i;

    ui_set_cursor_y(y);
    lv_obj_t *dd_talker = ui_dropdown_create(parent, "Talker",
                                             talk_opts, 3, sel_talker);
    (void)ui_dropdown_bind(dd_talker, g_nmea_weather.talker_id, 1, GRP_WX);
    y += LINE_H + GAP_Y;

    /* ---------- Prefix dropdown ------------------------------------ */
    static const char *pref_opts[] = {"$","!"};

    ui_set_cursor_y(y);
    lv_obj_t *dd_prefix = ui_dropdown_create(parent, "Prefix",
                                             pref_opts, 2,
                                             (snapshot.prefix == '!') ? 1 : 0);
    (void)ui_dropdown_bind(dd_prefix, &g_nmea_weather.prefix, 0, GRP_WX);
    y += LINE_H + GAP_Y;

    /* ---------- Wind dir true -------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_dir_true = ui_float_stepper_create(parent, "Wind dir true (°)",
                                                   snapshot.wind_dir_true_deg, 0.1f);
    lv_obj_set_user_data(sp_dir_true, &g_nmea_weather.wind_dir_true_deg);
    lv_obj_add_event_cb(sp_dir_true, cb_fstep, LV_EVENT_CLICKED, sp_dir_true);
    y += LINE_H + GAP_Y;

    /* ---------- Wind dir magnetic ---------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_dir_mag = ui_float_stepper_create(parent, "Wind dir mag (°)",
                                                  snapshot.wind_dir_mag_deg, 0.1f);
    lv_obj_set_user_data(sp_dir_mag, &g_nmea_weather.wind_dir_mag_deg);
    lv_obj_add_event_cb(sp_dir_mag, cb_fstep, LV_EVENT_CLICKED, sp_dir_mag);
    y += LINE_H + GAP_Y;

    /* ---------- Relative angle ------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_ang_rel = ui_float_stepper_create(parent, "Rel angle (°)",
                                                  snapshot.wind_angle_rel_deg, 0.1f);
    lv_obj_set_user_data(sp_ang_rel, &g_nmea_weather.wind_angle_rel_deg);
    lv_obj_add_event_cb(sp_ang_rel, cb_fstep, LV_EVENT_CLICKED, sp_ang_rel);
    y += LINE_H + GAP_Y;

    /* ---------- MWV reference dropdown (R/T) ----------------------- */
    static const char *rt_opts[] = {"R","T"};
    int sel_ref = (snapshot.rel_ref == 'T') ? 1 : 0;

    ui_set_cursor_y(y);
    lv_obj_t *dd_ref = ui_dropdown_create(parent, "MWV ref",
                                          rt_opts, 2, sel_ref);
    (void)ui_dropdown_bind(dd_ref, &g_nmea_weather.rel_ref, 0, GRP_WX);
    y += LINE_H + GAP_Y;

    /* ---------- VWR/VWT side dropdown (L/R) ------------------------ */
    static const char *lr_opts[] = {"L","R"};
    int sel_side = (snapshot.wind_side == 'L') ? 0 : 1;

    ui_set_cursor_y(y);
    lv_obj_t *dd_side = ui_dropdown_create(parent, "Wind side",
                                           lr_opts, 2, sel_side);
    (void)ui_dropdown_bind(dd_side, &g_nmea_weather.wind_side, 0, GRP_WX);
    y += LINE_H + GAP_Y;

    /* ---------- Wind speeds ---------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_kn = ui_float_stepper_create(parent, "Speed kn",
                                             snapshot.wind_speed_kn, 0.1f);
    lv_obj_set_user_data(sp_kn, &g_nmea_weather.wind_speed_kn);
    lv_obj_add_event_cb(sp_kn, cb_fstep, LV_EVENT_CLICKED, sp_kn);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_ms = ui_float_stepper_create(parent, "Speed m/s",
                                             snapshot.wind_speed_ms, 0.1f);
    lv_obj_set_user_data(sp_ms, &g_nmea_weather.wind_speed_ms);
    lv_obj_add_event_cb(sp_ms, cb_fstep, LV_EVENT_CLICKED, sp_ms);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_kph = ui_float_stepper_create(parent, "Speed kph",
                                              snapshot.wind_speed_kph, 0.1f);
    lv_obj_set_user_data(sp_kph, &g_nmea_weather.wind_speed_kph);
    lv_obj_add_event_cb(sp_kph, cb_fstep, LV_EVENT_CLICKED, sp_kph);
    y += LINE_H + GAP_Y;

    /* ---------- Water temperature ----------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_temp = ui_float_stepper_create(parent, "Water temp (°C)",
                                               snapshot.water_temp_C, 0.1f);
    lv_obj_set_user_data(sp_temp, &g_nmea_weather.water_temp_C);
    lv_obj_add_event_cb(sp_temp, cb_fstep, LV_EVENT_CLICKED, sp_temp);
    y += LINE_H + GAP_Y;

    /* Final layout update */
    lv_obj_update_layout(parent);
    ESP_LOGI("weather_tab", "WEATHER tab ready (layout v1.1.2)");
}
