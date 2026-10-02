/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "lvgl.h"
#include "nmea_editor/nmea_tabs_gps.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/ui_colors.h"
#include "ui/ui_nmea_widgets.h"
#include "ui/ui_theme.h"
#include "nmea_editor/numeric_editor.h"          /* Shared input engine */
#include "rs485/rs485_simui.h"
#include "system/nmea_clock.h" 
#include <string.h>
#include <stdio.h>


/* ───────── Style ───────── */
#define GAP_X 16
#define GAP_Y 12
#define FLD_W 150
#define LINE_H 44
LV_FONT_DECLARE(lv_font_montserrat_16)

/* ───────── Raw buffers ───────── */
static char raw_time[7];           /* HHMMSS\0 */
static char raw_date[7];           /* DDMMYY\0 */
static char raw_lat [10];          /* ddmm.mmmm\0 */
static char raw_lon [11];          /* dddmm.mmmm\0 */

/* ───────── Raw values to formatted text ───────── */
static void fmt_time(char *o){ snprintf(o, 10, "%2.2s:%2.2s:%2.2s ",
                                        raw_time, raw_time+2, raw_time+4); }
static void fmt_date(char *o){ snprintf(o, 10, "%2.2s.%2.2s.%2.2s ",
                                        raw_date, raw_date+2, raw_date+4); }
static void fmt_lat (char *o){ snprintf(o,13, "%2.2s°%2.2s.%4.4s ",
                                        raw_lat, raw_lat+2, raw_lat+5); }
static void fmt_lon (char *o){ snprintf(o,14, "%3.3s°%2.2s.%4.4s ",
                                        raw_lon, raw_lon+3, raw_lon+6); }

/* Display index to raw index; -1 marks a separator. */
static const int8_t map_time[9] = {0,1,-1,2,3,-1,4,5,-2};
static const int8_t map_date[9] = {0,1,-1,2,3,-1,4,5,-2};
static const int8_t map_lat [11]= {0,1,-1,2,3,-1,5,6,7,8,-2};
static const int8_t map_lon [12]= {0,1,2,-1,3,4,-1,6,7,8,9,-2};

/* ───────── Field limits ───────── */
/* Time: HH:MM:SS */
static const field_limit_t time_limits[3] = {
    {0, 2, 0, 23},  /* HH */
    {2, 2, 0, 59},  /* MM */
    {4, 2, 0, 59}   /* SS */
};

/* Date: DD.MM.YY */
static const field_limit_t date_limits[3] = {
    {0, 2, 1, 31},  /* DD */
    {2, 2, 1, 12},  /* MM */
    {4, 2, 0, 99}   /* YY */
};

/* Latitude: dd°mm.mmmm */
static const field_limit_t lat_limits[3] = {
    {0, 2, 0, 90},   /* dd */
    {2, 2, 0, 59},   /* mm */
    {5, 4, 0, 9999}  /* mmmm */
};

/* Longitude: ddd°mm.mmmm */
static const field_limit_t lon_limits[3] = {
    {0, 3, 0, 180},  /* ddd */
    {3, 2, 0, 59},   /* mm */
    {6, 4, 0, 9999}  /* mmmm */
};

/* ───────── Simulator dirty flag ───────── */
/* ───────── Safe numeric-editor initialization ───────── */
static inline void num_edit_init_once(void)
{
    extern void num_edit_init(void);
    static bool done = false;
    if (!done) { num_edit_init(); done = true; }
}
/* ───────── Commit handlers ───────── */
static void commit_time(void)
{
    (void)nmea_templates_write_field(g_nmea_gps.time_utc, raw_time, sizeof(raw_time));
    (void)nmea_clock_sync_from_template();
}
static void commit_date(void)
{
    (void)nmea_templates_write_field(g_nmea_gps.date_dmy, raw_date, sizeof(raw_date));
    (void)nmea_clock_sync_from_template();
}
static void commit_lat(void)
{
    char value[NMEA_LATSTR] = {0};
    snprintf(value, sizeof(value), "%s", raw_lat);
    (void)nmea_templates_write_field(g_nmea_gps.lat, value, sizeof(value));
}
static void commit_lon(void)
{
    char value[NMEA_LONSTR] = {0};
    snprintf(value, sizeof(value), "%s", raw_lon);
    (void)nmea_templates_write_field(g_nmea_gps.lon, value, sizeof(value));
}

/* ───────── LVGL helper callbacks ───────── */
static void cb_bool(lv_event_t *e){
    bool *dst = lv_event_get_user_data(e);
    const bool value = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    (void)nmea_templates_write_field(dst, &value, sizeof(value));
}
static void cb_dir(lv_event_t *e){
    char *dst = lv_event_get_user_data(e); char b[2];
    lv_dropdown_get_selected_str(lv_event_get_target(e), b, 2);
    (void)nmea_templates_write_field(dst, b, 1);
}
static void cb_fstep(lv_event_t *e){
    lv_obj_t *lbl = lv_event_get_user_data(e);
    float *dst = lv_obj_get_user_data(lbl);
    const float value = strtof(lv_label_get_text(lbl), NULL);
    (void)nmea_templates_write_field(dst, &value, sizeof(value));
}
static void cb_istep(lv_event_t *e){
    lv_obj_t *lbl = lv_event_get_user_data(e);
    uint8_t *dst = lv_obj_get_user_data(lbl);
    const uint8_t value = (uint8_t)atoi(lv_label_get_text(lbl));
    (void)nmea_templates_write_field(dst, &value, sizeof(value));
}

/* ───────── Main tab builder ───────── */
void gps_tab_create(lv_obj_t *parent)
{
    nmea_gps_t gps_snapshot;
    /* Copy template values into the raw buffers. */
    nmea_templates_gps_snapshot(&gps_snapshot);
    memcpy(raw_time, gps_snapshot.time_utc, 6); raw_time[6]='\0';
    memcpy(raw_date, gps_snapshot.date_dmy, 6); raw_date[6]='\0';
    memcpy(raw_lat, gps_snapshot.lat, sizeof(raw_lat)-1); raw_lat[sizeof(raw_lat)-1] = '\0';
    memcpy(raw_lon, gps_snapshot.lon, sizeof(raw_lon)-1); raw_lon[sizeof(raw_lon)-1] = '\0';

    ui_form_parent_apply(parent);

    char buf[16];
    int y = 0;
    num_edit_init_once();   
    /* ---------- Sentence checkboxes ---------------------------------- */
    static const char*lbl[6] = {"RMC","GGA","ZDA","GLL","VTG","CRC"};
    bool *flag[6] = {&g_nmea_gps.send_rmc,&g_nmea_gps.send_gga,&g_nmea_gps.send_zda,
                     &g_nmea_gps.send_gll,&g_nmea_gps.send_vtg,&g_nmea_gps.add_crc};
    const bool flag_values[6] = {gps_snapshot.send_rmc, gps_snapshot.send_gga, gps_snapshot.send_zda, gps_snapshot.send_gll, gps_snapshot.send_vtg, gps_snapshot.add_crc};


    for(int i=0;i<6;i++){
        int x = (i%3)*(220+GAP_X), row = i/3, y0 = row*(LINE_H+GAP_Y);
        ui_set_cursor_y(y0);
        lv_obj_t *cb = ui_checkbox_create(parent, lbl[i], flag_values[i]);
        lv_obj_set_pos(cb, x, y0);
        lv_obj_add_event_cb(cb, cb_bool, LV_EVENT_VALUE_CHANGED, flag[i]);
    }
    y += 2*LINE_H + GAP_Y;
	
    /* ---------- Transmit rate ---------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_freq = ui_float_stepper_create(parent,"Freq (Hz)",gps_snapshot.rate_hz,0.1f);
    lv_obj_set_user_data(sp_freq, &g_nmea_gps.rate_hz);
    lv_obj_add_event_cb(sp_freq, cb_fstep, LV_EVENT_CLICKED, sp_freq);
    y += LINE_H + GAP_Y;
	/* ---- Talker dropdown -------------------------------------------- */
	static const char *talk_opts[] = {"GP","GL","GN"};
	int sel = 0;
	for(int i=0;i<3;i++) if(strcmp(gps_snapshot.talker_id,talk_opts[i])==0) sel=i;

	ui_set_cursor_y(y);
	lv_obj_t *dd_talker = ui_dropdown_create(parent,"Talker",talk_opts,3,sel);
	(void)ui_dropdown_bind(dd_talker, g_nmea_gps.talker_id, 1, GRP_GPS);
	y += LINE_H + GAP_Y + 8;

	/* ---- Prefix dropdown -------------------------------------------- */
	static const char *pref_opts[] = {"$","!"};
	ui_set_cursor_y(y);
	lv_obj_t *dd_prefix = ui_dropdown_create(parent,"Prefix",pref_opts,2,
                                        (gps_snapshot.prefix=='!')?1:0);
	(void)ui_dropdown_bind(dd_prefix, &g_nmea_gps.prefix, 0, GRP_GPS);
	y += LINE_H + GAP_Y + 10;

   /* ---------- Time --------------------------------------------------- */
	ui_set_cursor_y(y);
	fmt_time(buf);
	lv_obj_t *lab_time = ui_value_label_create(parent, "Time UTC", buf, FLD_W);
	num_edit_bind(lab_time, raw_time, map_time, 9, fmt_time, commit_time, time_limits, 3);
	y += LINE_H + GAP_Y;

/* ---------- Date --------------------------------------------------- */
	ui_set_cursor_y(y);
	fmt_date(buf);
	lv_obj_t *lab_date = ui_value_label_create(parent, "Date DMY", buf, FLD_W);
	num_edit_bind(lab_date, raw_date, map_date, 9, fmt_date, commit_date, date_limits, 3);
	y += LINE_H + GAP_Y;
  /* ---------- Latitude + dropdown ---------------------------------- */
    ui_set_cursor_y(y);
    fmt_lat(buf);
    lv_obj_t *lab_lat = ui_latlon_create(parent, "Latitude", buf,
                                         (gps_snapshot.lat_dir=='S')?"S":"N", FLD_W);
    num_edit_bind(lab_lat, raw_lat, map_lat, 11, fmt_lat, commit_lat, lat_limits, 3);

    lv_obj_t *dd_lat = lv_obj_get_child(lv_obj_get_parent(lab_lat),
                        lv_obj_get_child_cnt(lv_obj_get_parent(lab_lat))-1);
    lv_obj_add_event_cb(dd_lat, cb_dir, LV_EVENT_VALUE_CHANGED, &g_nmea_gps.lat_dir);
    y += LINE_H + GAP_Y;

    /* ---------- Longitude + dropdown --------------------------------- */
    ui_set_cursor_y(y);
    fmt_lon(buf);
    lv_obj_t *lab_lon = ui_latlon_create(parent, "Longitude", buf,
                                         (gps_snapshot.lon_dir=='W')?"W":"E", FLD_W);
    num_edit_bind(lab_lon, raw_lon, map_lon, 12, fmt_lon, commit_lon, lon_limits, 3);

    lv_obj_t *dd_lon = lv_obj_get_child(lv_obj_get_parent(lab_lon),
                        lv_obj_get_child_cnt(lv_obj_get_parent(lab_lon))-1);
    lv_obj_add_event_cb(dd_lon, cb_dir, LV_EVENT_VALUE_CHANGED, &g_nmea_gps.lon_dir);
    y += LINE_H + GAP_Y;

    /* ---------- SOG / COG -------------------------------------------- */
    ui_set_cursor_y(y);
    lv_obj_t *sp_sog = ui_float_stepper_create(parent,"SOG (kn)",gps_snapshot.sog_kn,0.1f);
    lv_obj_set_user_data(sp_sog, &g_nmea_gps.sog_kn);
    lv_obj_add_event_cb(sp_sog, cb_fstep, LV_EVENT_CLICKED, sp_sog);
    y += LINE_H + GAP_Y;

    ui_set_cursor_y(y);
    lv_obj_t *sp_cog = ui_float_stepper_create(parent,"COG (°)",gps_snapshot.cog_deg,0.1f);
    lv_obj_set_user_data(sp_cog, &g_nmea_gps.cog_deg);
    lv_obj_add_event_cb(sp_cog, cb_fstep, LV_EVENT_CLICKED, sp_cog);
    y += LINE_H + GAP_Y;

    /* ---------- Fix / Sat -------------------------------------------- */
    ui_set_cursor_y(y);
    static const char *fx[] = {"A","V"};
    lv_obj_t *dd_fx = ui_dropdown_create(parent,"Fix",fx,2,(gps_snapshot.fix=='V'));
    lv_obj_add_event_cb(dd_fx, cb_dir, LV_EVENT_VALUE_CHANGED, &g_nmea_gps.fix);
    y += LINE_H + GAP_Y + 4;

    ui_set_cursor_y(y);
    lv_obj_t *sp_sat = ui_int_stepper_create(parent,"Sat",gps_snapshot.sats,0,12,1);
    lv_obj_set_user_data(sp_sat, &g_nmea_gps.sats);
    lv_obj_add_event_cb(sp_sat, cb_istep, LV_EVENT_CLICKED, sp_sat);

    lv_obj_update_layout(parent);
}
