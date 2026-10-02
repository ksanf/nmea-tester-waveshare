/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_tabs_log.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/ui_colors.h"
#include "ui/ui_nmea_widgets.h"
#include "ui/ui_theme.h"
#include "rs485/rs485_simui.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_TABS_LOG
#include "config_logs.h"
#include <stdio.h>
#include <stdlib.h>

/* ───────── Layout constants ───────── */
#define GAP_X 16   /* horizontal gap */
#define GAP_Y 12   /* vertical gap   */
#define LINE_H 44 /* canonical row height */

LV_FONT_DECLARE(lv_font_montserrat_16)


/* ───────── Callbacks ───────── */
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

/* ───────── Tab builder ───────── */
void log_tab_create(lv_obj_t *parent)
{
    nmea_log_t snapshot;
    nmea_templates_log_snapshot(&snapshot);
    ui_form_parent_apply(parent);

    int y = 0;

    /* ---------- Message & CRC checkboxes (2×2) ----------------------- */
    static const char *lbl_chk[4]  = {"VHW", "VLW", "VBW", "CRC"};
    bool *flag_chk[4] = {&g_nmea_log.send_vhw, &g_nmea_log.send_vlw,
                         &g_nmea_log.send_vbw, &g_nmea_log.add_crc};
    const bool flag_values[4] = {snapshot.send_vhw, snapshot.send_vlw, snapshot.send_vbw, snapshot.add_crc};


    for(int i=0;i<4;i++){
        int x = (i % 2) * (120 + GAP_X);
        int row = i / 2;
        int y0 = y + row * (LINE_H + GAP_Y);
        ui_set_cursor_y(y0);
        lv_obj_t *cb = ui_checkbox_create(parent, lbl_chk[i], flag_values[i]);
        lv_obj_set_pos(cb, x, y0);
        lv_obj_add_event_cb(cb, cb_bool, LV_EVENT_VALUE_CHANGED, flag_chk[i]);
    }
    y += 2 * LINE_H + GAP_Y;

    /* ---------- Frequency ------------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_freq = ui_float_stepper_create(parent, "Freq (Hz)",
                                                snapshot.rate_hz, 0.1f);
    lv_obj_set_user_data(sp_freq, &g_nmea_log.rate_hz);
    lv_obj_add_event_cb(sp_freq, cb_fstep, LV_EVENT_CLICKED, sp_freq);
    y += LINE_H + GAP_Y;

    /* ---- Talker dropdown ------------------------------------------- */
    static const char *talk_opts[] = {"VW","WI","WV"};
    int sel = 0;
    for(int i=0;i<3;i++) if(strcmp(snapshot.talker_id,talk_opts[i])==0) sel = i;

    ui_set_cursor_y(y);
    lv_obj_t *dd_talker = ui_dropdown_create(parent, "Talker",
                                             talk_opts, 3, sel);
    (void)ui_dropdown_bind(dd_talker, g_nmea_log.talker_id, 1, GRP_LOG);
    y += LINE_H + GAP_Y;

    /* ---- Prefix dropdown ------------------------------------------- */
    static const char *pref_opts[] = {"$","!"};

    ui_set_cursor_y(y);
    lv_obj_t *dd_prefix = ui_dropdown_create(parent, "Prefix",
                                             pref_opts, 2,
                                             (snapshot.prefix=='!') ? 1 : 0);
    (void)ui_dropdown_bind(dd_prefix, &g_nmea_log.prefix, 0, GRP_LOG);
    y += LINE_H + GAP_Y;

    /* ---------- Water heading true ---------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_hdt = ui_float_stepper_create(parent, "Water hdg true (°)",
                                               snapshot.hdg_water_true_deg, 0.1f);
    lv_obj_set_user_data(sp_hdt, &g_nmea_log.hdg_water_true_deg);
    lv_obj_add_event_cb(sp_hdt, cb_fstep, LV_EVENT_CLICKED, sp_hdt);
    y += LINE_H + GAP_Y;

    /* ---------- Water heading magnetic ------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_hdm = ui_float_stepper_create(parent, "Water hdg mag (°)",
                                               snapshot.hdg_water_mag_deg, 0.1f);
    lv_obj_set_user_data(sp_hdm, &g_nmea_log.hdg_water_mag_deg);
    lv_obj_add_event_cb(sp_hdm, cb_fstep, LV_EVENT_CLICKED, sp_hdm);
    y += LINE_H + GAP_Y;

    /* ---------- Speeds ---------------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_kn = ui_float_stepper_create(parent, "Speed (kn)",
                                             snapshot.water_speed_kn, 0.1f);
    lv_obj_set_user_data(sp_kn, &g_nmea_log.water_speed_kn);
    lv_obj_add_event_cb(sp_kn, cb_fstep, LV_EVENT_CLICKED, sp_kn);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_kph = ui_float_stepper_create(parent, "Speed (kph)",
                                              snapshot.water_speed_kph, 0.1f);
    lv_obj_set_user_data(sp_kph, &g_nmea_log.water_speed_kph);
    lv_obj_add_event_cb(sp_kph, cb_fstep, LV_EVENT_CLICKED, sp_kph);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_stern = ui_float_stepper_create(parent, "Stern spd kn",
                                                snapshot.stern_speed_kn, 0.1f);
    lv_obj_set_user_data(sp_stern, &g_nmea_log.stern_speed_kn);
    lv_obj_add_event_cb(sp_stern, cb_fstep, LV_EVENT_CLICKED, sp_stern);
    y += LINE_H + GAP_Y;

    /* ---------- Distances ------------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_dist_tot = ui_float_stepper_create(parent, "Distance total nm",
                                                   snapshot.dist_total_nm, 0.1f);
    lv_obj_set_user_data(sp_dist_tot, &g_nmea_log.dist_total_nm);
    lv_obj_add_event_cb(sp_dist_tot, cb_fstep, LV_EVENT_CLICKED, sp_dist_tot);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_dist_trip = ui_float_stepper_create(parent, "Trip nm",
                                                    snapshot.dist_trip_nm, 0.1f);
    lv_obj_set_user_data(sp_dist_trip, &g_nmea_log.dist_trip_nm);
    lv_obj_add_event_cb(sp_dist_trip, cb_fstep, LV_EVENT_CLICKED, sp_dist_trip);
    y += LINE_H + GAP_Y;

    /* Final layout update */
    lv_obj_update_layout(parent);
    ESP_LOGI("log_tab", "LOG tab ready (layout v1.1.2, ctx free fix)");
}
