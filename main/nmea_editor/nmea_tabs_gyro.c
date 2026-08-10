/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_tabs_gyro.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_version.h"
#include "ui/ui_colors.h"
#include "ui/ui_nmea_widgets.h"
#include "ui/ui_theme.h"
#include "rs485/rs485_simui.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_TABS_GYRO
#include "config_logs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>    /* malloc/free */
#include <string.h>

/* ───────── Layout constants ───────── */
#define GAP_X 16   /* horizontal gap */
#define GAP_Y 12   /* vertical gap   */
#define LINE_H 44 /* canonical row height */
#define FLD_W          150    /* width of value label */

LV_FONT_DECLARE(lv_font_montserrat_16)

static inline void dirty(void){ nmea_mark_dirty(GRP_GYRO); }

/* ───────── LVGL callbacks ───────── */
static void cb_bool(lv_event_t *e)
{
    const uint32_t field = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    const bool value = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    nmea_gyro_t patch = {0};

    switch (field) {
    case NMEA_GYRO_FIELD_SEND_HDG:   patch.send_hdg = value; break;
    case NMEA_GYRO_FIELD_SEND_HDT:   patch.send_hdt = value; break;
    case NMEA_GYRO_FIELD_SEND_HDM:   patch.send_hdm = value; break;
    case NMEA_GYRO_FIELD_SEND_ROT:   patch.send_rot = value; break;
    case NMEA_GYRO_FIELD_FOLLOW_ROT: patch.follow_rot = value; break;
    case NMEA_GYRO_FIELD_CRC:        patch.add_crc = value; break;
    default: return;
    }
    nmea_templates_gyro_update_fields(&patch, field);
    dirty();
}

static void cb_dropdown(lv_event_t *e)
{
    const uint32_t field = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    char buf[4] = {0};
    nmea_gyro_t patch = {0};

    lv_dropdown_get_selected_str(lv_event_get_target(e), buf, sizeof(buf));
    switch (field) {
    case NMEA_GYRO_FIELD_TALKER:
        strlcpy(patch.talker_id, buf, sizeof(patch.talker_id));
        break;
    case NMEA_GYRO_FIELD_PREFIX:     patch.prefix = buf[0]; break;
    case NMEA_GYRO_FIELD_DEV_DIR:    patch.dev_dir = buf[0]; break;
    case NMEA_GYRO_FIELD_VAR_DIR:    patch.var_dir = buf[0]; break;
    case NMEA_GYRO_FIELD_ROT_STATUS: patch.rot_status = buf[0]; break;
    default: return;
    }
    nmea_templates_gyro_update_fields(&patch, field);
    dirty();
}

static void cb_fstep(lv_event_t *e)
{
    const uint32_t field = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *lbl = lv_event_get_target(e);
    const float value = strtof(lv_label_get_text(lbl), NULL);
    nmea_gyro_t patch = {0};

    switch (field) {
    case NMEA_GYRO_FIELD_RATE:         patch.rate_hz = value; break;
    case NMEA_GYRO_FIELD_HEADING_TRUE: patch.heading_true_deg = value; break;
    case NMEA_GYRO_FIELD_HEADING_MAG:  patch.heading_mag_deg = value; break;
    case NMEA_GYRO_FIELD_DEVIATION:    patch.deviation_deg = value; break;
    case NMEA_GYRO_FIELD_VARIATION:    patch.variation_deg = value; break;
    case NMEA_GYRO_FIELD_ROT_RATE:     patch.rot_deg_min = value; break;
    default: return;
    }
    nmea_templates_gyro_update_fields(&patch, field);
    dirty();
}

/* ───────── Tab builder ───────── */
void gyro_tab_create(lv_obj_t *parent)
{
    nmea_gyro_t gyro;
    nmea_templates_gyro_snapshot(&gyro);
    ui_form_parent_apply(parent);

    int y = 0;

    /* ---------- Top: messages, motion and CRC (2×3 grid) ------------ */
    const char *lbl_chk[6]  = {
        "HDG",
        nmea_version_profile()->ths_heading ? "THS" : "HDT",
        "HDM",
        "ROT",
        "FOLLOW ROT",
        "CRC"
    };
    const bool flag_chk[6] = {
        gyro.send_hdg, gyro.send_hdt, gyro.send_hdm,
        gyro.send_rot, gyro.follow_rot, gyro.add_crc
    };
    const uint32_t field_chk[6] = {
        NMEA_GYRO_FIELD_SEND_HDG,
        NMEA_GYRO_FIELD_SEND_HDT,
        NMEA_GYRO_FIELD_SEND_HDM,
        NMEA_GYRO_FIELD_SEND_ROT,
        NMEA_GYRO_FIELD_FOLLOW_ROT,
        NMEA_GYRO_FIELD_CRC,
    };
    for(int i=0;i<6;i++){
        int x   = (i%2)*(320+GAP_X);
        int row = i/2;
        int y0  = y + row*(LINE_H+GAP_Y);
        ui_set_cursor_y(y0);
        lv_obj_t *cb = ui_checkbox_create(parent,lbl_chk[i],flag_chk[i]);
        lv_obj_set_pos(cb,x,y0);
        lv_obj_add_event_cb(cb,cb_bool,LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)field_chk[i]);
    }
    y += 3*LINE_H + 2*GAP_Y;

    /* ---------- Frequency ------------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_freq = ui_float_stepper_create(parent,"Freq (Hz)",
                                                gyro.rate_hz,0.1f);
    lv_obj_add_event_cb(sp_freq,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_RATE);
    y += LINE_H + GAP_Y;

    /* ---- Talker dropdown ------------------------------------------- */
    static const char *talk_opts[] = {"HE","HC","II"};
    int sel = 0;
    for(int i=0;i<3;i++) if(strcmp(gyro.talker_id,talk_opts[i])==0) sel = i;

    ui_set_cursor_y(y);
    lv_obj_t *dd_talker = ui_dropdown_create(parent, "Talker",
                                             talk_opts, 3, sel);
    lv_obj_add_event_cb(dd_talker, cb_dropdown, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_TALKER);
    y += LINE_H + GAP_Y;

    /* ---- Prefix dropdown ------------------------------------------- */
    static const char *pref_opts[] = {"$","!"};

    ui_set_cursor_y(y);
    lv_obj_t *dd_prefix = ui_dropdown_create(parent, "Prefix",
                                             pref_opts, 2,
                                             (gyro.prefix=='!') ? 1 : 0);
    lv_obj_add_event_cb(dd_prefix, cb_dropdown, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_PREFIX);
    y += LINE_H + GAP_Y;

    /* ---------- Heading true ---------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_hdt = ui_float_stepper_create(parent,"Heading true (°)",
                                               gyro.heading_true_deg,0.1f);
    lv_obj_add_event_cb(sp_hdt,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_HEADING_TRUE);
    y += LINE_H + GAP_Y;

    /* ---------- Heading magnetic ------------------------------------ */
    ui_set_cursor_y(y);
    lv_obj_t *sp_hdm = ui_float_stepper_create(parent,"Heading mag (°)",
                                               gyro.heading_mag_deg,0.1f);
    lv_obj_add_event_cb(sp_hdm,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_HEADING_MAG);
    y += LINE_H + GAP_Y;

    /* ---------- Rate of turn and validity --------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_rot = ui_float_stepper_create(parent,"ROT (°/min, -P/+S)",
                                               gyro.rot_deg_min,0.1f);
    lv_obj_add_event_cb(sp_rot,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_ROT_RATE);
    y += LINE_H + GAP_Y;

    static const char *rot_status_opts[2] = {"A","V"};
    ui_set_cursor_y(y);
    lv_obj_t *dd_rot_status = ui_dropdown_create(parent,"ROT status",
                                                  rot_status_opts,2,
                                                  (gyro.rot_status=='V')?1:0);
    lv_obj_add_event_cb(dd_rot_status,cb_dropdown,LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_ROT_STATUS);
    y += LINE_H + GAP_Y;

    /* ---------- Deviation stepper ----------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_dev = ui_float_stepper_create(parent,"Deviation (°)",
                                               gyro.deviation_deg,0.1f);
    lv_obj_add_event_cb(sp_dev,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_DEVIATION);
    y += LINE_H + GAP_Y;

    /* ---------- Variation stepper ----------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_var = ui_float_stepper_create(parent,"Variation (°)",
                                               gyro.variation_deg,0.1f);
    lv_obj_add_event_cb(sp_var,cb_fstep,LV_EVENT_CLICKED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_VARIATION);
    y += LINE_H + GAP_Y;

    /* ---------- Bottom: E/W dropdowns for Dev & Var ----------------- */
    static const char *ew_opts[2] = {"E","W"};

    ui_set_cursor_y(y);
    lv_obj_t *dd_dev = ui_dropdown_create(parent,"Dev dir",
                                          ew_opts,2,(gyro.dev_dir=='W')?1:0);
    lv_obj_add_event_cb(dd_dev,cb_dropdown,LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_DEV_DIR);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *dd_var = ui_dropdown_create(parent,"Var dir",
                                          ew_opts,2,(gyro.var_dir=='W')?1:0);
    lv_obj_add_event_cb(dd_var,cb_dropdown,LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)NMEA_GYRO_FIELD_VAR_DIR);
    y += LINE_H + GAP_Y;

    /* Final layout update */
    lv_obj_update_layout(parent);
    ESP_LOGI("gyro_tab","GYRO tab ready (layout v1.1.3, ctx free fix)");
}
