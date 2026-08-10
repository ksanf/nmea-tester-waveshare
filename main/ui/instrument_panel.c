/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Instrument panel and NMEA parser with LVGL recolor markup.
 */

#include "ui/instrument_panel.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "nmea_templates.h"
#include "nmea_editor/nmea_wire.h"
#include "system/nmea_clock.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_INSTRUMENT_PANEL
#include "config_logs.h"

#define TAG "instr_panel"

/* ────────── Constants ────────── */
#define TIMEOUT_MS     5000
#define FONT_MAIN      &lv_font_montserrat_16
#define LINE_H         18
#define ROWS           3
#define COLS           4
#define CELL_H         LINE_H

/* ────────── Data model ────────── */
typedef struct {
    /* Position */
    float lat; char lat_dir;
    float lon; char lon_dir;
    uint32_t coords_ts;

    /* Time and date */
    char  time[7];            /* HHMMSS */
    char  date[11];           /* DD-MM-YYYY */
    uint32_t time_ts, date_ts;

    /* GPS course and speed */
    float gps_cog, gps_sog; uint32_t gps_hdgspd_ts;

    /* Gyro heading */
    float gyro_heading; bool has_gyro_heading;
    uint32_t gyro_ts;

    /* Speed log */
    float log_speed; bool has_log_speed;
    uint32_t log_ts;

    /* Depth */
    float depth; bool has_depth;
    uint32_t depth_ts;

    /* Wind */
    float wind_angle, wind_speed; bool wind_true;
    uint32_t weather_ts;

    /* Water temperature */
    float temp_c; uint32_t temp_ts;
} nav_t;
static nav_t nav = {0};

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

/* ────────── helpers ────────── */
static char *next_tok(char **p) {
    char *s = *p;
    char *c = strchr(s, ',');
    if (c) { *c = '\0'; *p = c + 1; }
    else   { *p = s + strlen(s); }
    return s;
}

static bool parse_float_(const char *s, float min_value, float max_value, float *out)
{
    char *end = NULL;
    float value;

    if (!s || !*s || !out) return false;
    errno = 0;
    value = strtof(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(value) ||
        value < min_value || value > max_value) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_coord_(const char *s, int max_degrees, float *out)
{
    float raw;
    int degrees;
    float minutes;

    if (!parse_float_(s, 0.0f, (float)(max_degrees * 100 + 60), &raw)) return false;
    degrees = (int)(raw / 100.0f);
    minutes = raw - (float)(degrees * 100);
    if (degrees > max_degrees || minutes < 0.0f || minutes >= 60.0f ||
        (degrees == max_degrees && minutes > 0.0f)) {
        return false;
    }
    *out = (float)degrees + minutes / 60.0f;
    return true;
}

static bool parse_time_(const char *s, char out[7])
{
    if (!s || strlen(s) < 6u) return false;
    for (int i = 0; i < 6; ++i) if (!isdigit((unsigned char)s[i])) return false;
    if ((s[6] != '\0' && s[6] != '.') ||
        ((s[0] - '0') * 10 + s[1] - '0') > 23 ||
        ((s[2] - '0') * 10 + s[3] - '0') > 59 ||
        ((s[4] - '0') * 10 + s[5] - '0') > 60) {
        return false;
    }
    memcpy(out, s, 6);
    out[6] = '\0';
    return true;
}

static bool parse_rmc_date_(const char *s, char out[11])
{
    if (!s || strlen(s) != 6u) return false;
    for (int i = 0; i < 6; ++i) if (!isdigit((unsigned char)s[i])) return false;
    const unsigned day = (unsigned)((s[0] - '0') * 10 + s[1] - '0');
    const unsigned month = (unsigned)((s[2] - '0') * 10 + s[3] - '0');
    if (day < 1u || day > 31u || month < 1u || month > 12u) return false;
    snprintf(out, 11, "%.2s-%.2s-20%.2s", s, s + 2, s + 4);
    return true;
}

static void deg_to_lat_str_(float deg, char *out, size_t out_sz)
{
    float abs_deg = fabsf(deg);
    int d = (int)abs_deg;
    float m = (abs_deg - (float)d) * 60.0f;
    snprintf(out, out_sz, "%02d%07.4f", d, m);
}

static void deg_to_lon_str_(float deg, char *out, size_t out_sz)
{
    float abs_deg = fabsf(deg);
    int d = (int)abs_deg;
    float m = (abs_deg - (float)d) * 60.0f;
    snprintf(out, out_sz, "%03d%07.4f", d, m);
}

/* ────────── NMEA parsers ────────── */
static void p_zda(char *p, uint32_t now) {
    char *tok = next_tok(&p);
    if (parse_time_(tok, nav.time)) nav.time_ts = now;
    char *d1 = next_tok(&p), *d2 = next_tok(&p), *d3 = next_tok(&p);
    unsigned day, month, year;
    if (sscanf(d1, "%u", &day) == 1 && sscanf(d2, "%u", &month) == 1 &&
        sscanf(d3, "%u", &year) == 1 && day >= 1u && day <= 31u &&
        month >= 1u && month <= 12u && year >= 2000u && year <= 2099u) {
        snprintf(nav.date, sizeof(nav.date), "%02u-%02u-%04u", day, month, year);
        nav.date_ts = now;
    }
}
static void p_rmc(char *p, uint32_t now) {
    char *tok = next_tok(&p);
    if (parse_time_(tok, nav.time)) nav.time_ts = now;
    next_tok(&p);                           /* Status */
    float lat, lon, sog, cog;
    tok = next_tok(&p); const bool lat_ok = parse_coord_(tok, 90, &lat);
    tok = next_tok(&p); const char lat_dir = *tok;
    tok = next_tok(&p); const bool lon_ok = parse_coord_(tok, 180, &lon);
    tok = next_tok(&p); const char lon_dir = *tok;
    if (lat_ok && lon_ok && (lat_dir == 'N' || lat_dir == 'S') &&
        (lon_dir == 'E' || lon_dir == 'W')) {
        nav.lat = lat; nav.lon = lon; nav.lat_dir = lat_dir; nav.lon_dir = lon_dir;
        nav.coords_ts = now;
    }
    tok = next_tok(&p); const bool sog_ok = parse_float_(tok, 0.0f, 1000.0f, &sog);
    tok = next_tok(&p); const bool cog_ok = parse_float_(tok, 0.0f, 360.0f, &cog);
    if (sog_ok && cog_ok) {
        nav.gps_sog = sog; nav.gps_cog = cog; nav.gps_hdgspd_ts = now;
    }
    char *d = next_tok(&p);
    if (parse_rmc_date_(d, nav.date)) nav.date_ts = now;
}
static void p_gga(char *p, uint32_t now){
    char *t = next_tok(&p); if (parse_time_(t, nav.time)) nav.time_ts = now;
    float lat, lon;
    char *tok = next_tok(&p); const bool lat_ok = parse_coord_(tok, 90, &lat);
    tok = next_tok(&p); const char lat_dir = *tok;
    tok = next_tok(&p); const bool lon_ok = parse_coord_(tok, 180, &lon);
    tok = next_tok(&p); const char lon_dir = *tok;
    if (lat_ok && lon_ok && (lat_dir == 'N' || lat_dir == 'S') &&
        (lon_dir == 'E' || lon_dir == 'W')) {
        nav.lat = lat; nav.lon = lon; nav.lat_dir = lat_dir; nav.lon_dir = lon_dir;
        nav.coords_ts = now;
    }
}
static void p_gll(char *p, uint32_t now){
    float lat, lon;
    char *tok = next_tok(&p); const bool lat_ok = parse_coord_(tok, 90, &lat);
    tok = next_tok(&p); const char lat_dir = *tok;
    tok = next_tok(&p); const bool lon_ok = parse_coord_(tok, 180, &lon);
    tok = next_tok(&p); const char lon_dir = *tok;
    if (lat_ok && lon_ok && (lat_dir == 'N' || lat_dir == 'S') &&
        (lon_dir == 'E' || lon_dir == 'W')) {
        nav.lat = lat; nav.lon = lon; nav.lat_dir = lat_dir; nav.lon_dir = lon_dir;
        nav.coords_ts = now;
    }
    char *t = next_tok(&p); if (parse_time_(t, nav.time)) nav.time_ts = now;
    next_tok(&p); /* Status */
}
static void p_vtg(char *p, uint32_t now){
    char *tok = next_tok(&p); if (*tok) nav.gps_cog = atof(tok);
    next_tok(&p); next_tok(&p); next_tok(&p);
    tok = next_tok(&p); if (*tok) nav.gps_sog = atof(tok);
    nav.gps_hdgspd_ts = now;
}
static void p_vbw(char *p, uint32_t now){
    char *tok = next_tok(&p); if (*tok) { nav.log_speed = atof(tok); nav.has_log_speed = true; nav.log_ts = now; }
    next_tok(&p); next_tok(&p); next_tok(&p);
}
static void p_ths(char *p, uint32_t now){
    char *tok = next_tok(&p); if (*tok) { nav.gyro_heading = atof(tok); nav.has_gyro_heading = true; nav.gyro_ts = now; }
}
static void p_hdt(char *p, uint32_t now){ p_ths(p, now); }
static void p_hdg(char *p, uint32_t now){
    char *tok = next_tok(&p); if (*tok) { nav.gyro_heading = atof(tok); nav.has_gyro_heading = true; nav.gyro_ts = now; }
}
static void p_hdm(char *p, uint32_t now){
    char *tok = next_tok(&p); if (*tok) { nav.gyro_heading = atof(tok); nav.has_gyro_heading = true; nav.gyro_ts = now; }
}
static void p_vhw(char *p, uint32_t now){
    next_tok(&p); next_tok(&p);
    char *tok = next_tok(&p); if (*tok) { nav.gyro_heading = atof(tok); nav.has_gyro_heading = true; nav.gyro_ts = now; }
    next_tok(&p);
    tok = next_tok(&p); if (*tok) { nav.log_speed = atof(tok); nav.has_log_speed = true; nav.log_ts = now; }
}
static void p_depth(const char *id, char *p, uint32_t now){
    char *tok;
    if (!strncmp(id + 2, "DPT", 3)) {
        tok = next_tok(&p); if (*tok) nav.depth = atof(tok);
    } else { // DBT, DBS, DBK
        next_tok(&p); next_tok(&p); // skip feet, f
        tok = next_tok(&p); if (*tok) nav.depth = atof(tok);
        tok = next_tok(&p); // skip M, assume it's M
    }
    nav.has_depth = true; nav.depth_ts = now;
}
static void p_dbk(char *p, uint32_t now){
    next_tok(&p); next_tok(&p);
    char *tok = next_tok(&p); if (*tok) { nav.depth = atof(tok); nav.has_depth = true; nav.depth_ts = now; }
}

/* Weather */
static void p_mwd(char *p, uint32_t now){
    char *ang = next_tok(&p); /* True angle */
    next_tok(&p);             /* "T" */
    next_tok(&p); next_tok(&p); /* Magnetic angle and "M" */
    char *spd = next_tok(&p); next_tok(&p); /* Knots and "N" */
    if (*ang) nav.wind_angle = atof(ang);
    if (*spd) nav.wind_speed = atof(spd);
    nav.wind_true = true; nav.weather_ts = now;
}
static void p_mwv(char *p, uint32_t now){
    char *ang  = next_tok(&p); char *reference = next_tok(&p);
    char *spd  = next_tok(&p); char *unit = next_tok(&p);
    char *status = next_tok(&p);
    float angle, speed;
    if (*status == 'A' && parse_float_(ang, 0.0f, 360.0f, &angle) &&
        parse_float_(spd, 0.0f, 1000.0f, &speed) &&
        (*reference == 'R' || *reference == 'T')) {
        if (*unit == 'M') speed *= 1.9438445f;
        else if (*unit == 'K') speed /= 1.852f;
        else if (*unit != 'N') return;
        nav.wind_angle = angle;
        nav.wind_speed = speed;
        nav.wind_true = (*reference == 'T');
        nav.weather_ts = now;
    }
}
static void p_vwr(char *p, uint32_t now){
    char *ang_tok = next_tok(&p);
    char side = *next_tok(&p);      /* L/R */
    char *spd = next_tok(&p); char *unit = next_tok(&p);
    if (*ang_tok) {
        float ang = atof(ang_tok);
        nav.wind_angle = (side == 'L') ? -ang : ang;
    }
    if (*spd && *unit == 'N') nav.wind_speed = atof(spd);
    nav.wind_true = false; nav.weather_ts = now;
}
static void p_vwt(char *p, uint32_t now){
    char *ang_tok = next_tok(&p);
    char side = *next_tok(&p);      /* L/R */
    char *spd = next_tok(&p); char *unit = next_tok(&p);
    if (*ang_tok) {
        float ang = atof(ang_tok);
        nav.wind_angle = (side == 'L') ? -ang : ang;
    }
    if (*spd && *unit == 'N') nav.wind_speed = atof(spd);
    nav.wind_true = true; nav.weather_ts = now;
}
static void p_mtw(char *p, uint32_t now){
    char *temp = next_tok(&p);
    if (*temp) nav.temp_c = atof(temp);
    nav.temp_ts = now;
}

/* ────────── dispatch ────────── */
static void dispatch(const char *id, char *p, uint32_t now){
    if      (!strncmp(id+2,"ZDA",3)) p_zda(p, now);
    else if (!strncmp(id+2,"RMC",3)) p_rmc(p, now);
    else if (!strncmp(id+2,"GGA",3)) p_gga(p, now);
    else if (!strncmp(id+2,"GLL",3)) p_gll(p, now);
    else if (!strncmp(id+2,"VTG",3)) p_vtg(p, now);
    else if (!strncmp(id+2,"VBW",3)) p_vbw(p, now);
    else if (!strncmp(id+2,"THS",3)) p_ths(p, now);
    else if (!strncmp(id+2,"HDT",3)) p_hdt(p, now);
    else if (!strncmp(id+2,"HDG",3)) p_hdg(p, now);
    else if (!strncmp(id+2,"HDM",3)) p_hdm(p, now);
    else if (!strncmp(id+2,"VHW",3)) p_vhw(p, now);
    else if (!strncmp(id+2,"DPT",3)||!strncmp(id+2,"DBT",3)||!strncmp(id+2,"DBS",3)) p_depth(id,p, now);
    else if (!strncmp(id+2,"DBK",3)) p_dbk(p, now);
    else if (!strncmp(id+2,"MWD",3)) p_mwd(p, now);
    else if (!strncmp(id+2,"MWV",3)) p_mwv(p, now);
    else if (!strncmp(id+2,"VWR",3)) p_vwr(p, now);
    else if (!strncmp(id+2,"VWT",3)) p_vwt(p, now);
    else if (!strncmp(id+2,"MTW",3)) p_mtw(p, now);
}

/* ────────── ui_update ────────── */
static void ui_update(void){
    /* Do not touch widgets after the panel has been deleted. */
    if (!panel) return;

    char buf[64];
    uint32_t now = lv_tick_get();
    bool sc = nav.coords_ts == 0 || (now - nav.coords_ts) > TIMEOUT_MS;
    bool st = nav.time_ts == 0 || (now - nav.time_ts) > TIMEOUT_MS;
    bool sd = nav.date_ts == 0 || (now - nav.date_ts) > TIMEOUT_MS;
    bool sh_gps = nav.gps_hdgspd_ts == 0 || (now - nav.gps_hdgspd_ts) > TIMEOUT_MS;
    bool sh_gyro = !nav.has_gyro_heading || (now - nav.gyro_ts > TIMEOUT_MS);
    bool sh_log = !nav.has_log_speed || (now - nav.log_ts > TIMEOUT_MS);
    bool sdpt = !nav.has_depth || (now - nav.depth_ts > TIMEOUT_MS);
    bool sw = nav.weather_ts == 0 || (now - nav.weather_ts) > TIMEOUT_MS;
    bool stp = nav.temp_ts == 0 || (now - nav.temp_ts) > TIMEOUT_MS;
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

/* ────────── main API ────────── */
void instrument_panel_process(const char *line){
    /* Ignore input until the panel is created. */
    if (!panel) return;
    if (!line || strlen(line) < 6) return;
    if (!nmea_wire_sentence_valid(line, false)) return;

    const char *s = (line[0]=='$'||line[0]=='!') ? line+1 : line;
    char in[256]; strncpy(in,s,sizeof(in)-1); in[sizeof(in)-1]='\0';
    char id[6]={0}; char *p=in; strncpy(id,p,5); p+=5; if(*p==',') p++;
    uint32_t now = lv_tick_get();
    dispatch(id, p, now);
    /* ui_update moved to poll_cb — called once after all lines */
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

void refresh_template(void) {
    uint32_t now = lv_tick_get();
    nmea_gps_t gps_update;
    nmea_gyro_t gyro_update;
    uint32_t gps_fields = 0;
    bool gps_clock_changed = false;
    bool gps_updated = false;
    bool gyro_updated = false;
    bool log_updated = false;
    bool echo_updated = false;
    bool wx_updated = false;

    nmea_templates_gps_snapshot(&gps_update);
    nmea_templates_gyro_snapshot(&gyro_update);

    // GPS: RMC, GGA, ZDA, GLL, VTG
    if (nav.coords_ts != 0 && (now - nav.coords_ts) <= TIMEOUT_MS) {
        deg_to_lat_str_(nav.lat, gps_update.lat, sizeof(gps_update.lat));
        gps_update.lat_dir = nav.lat_dir;
        deg_to_lon_str_(nav.lon, gps_update.lon, sizeof(gps_update.lon));
        gps_update.lon_dir = nav.lon_dir;
        gps_fields |= NMEA_GPS_FIELDS_COORDS;
        gps_updated = true;
    }
    if (nav.time_ts != 0 && (now - nav.time_ts) <= TIMEOUT_MS) {
        memcpy(gps_update.time_utc, nav.time, 6);
        gps_update.time_utc[6] = '\0';
        gps_fields |= NMEA_GPS_FIELDS_TIME;
        gps_updated = true;
        gps_clock_changed = true;
    }
    if (nav.date_ts != 0 && (now - nav.date_ts) <= TIMEOUT_MS && strlen(nav.date) >= 10) {
        memcpy(gps_update.date_dmy + 0, nav.date + 0, 2); /* DD */
        memcpy(gps_update.date_dmy + 2, nav.date + 3, 2); /* MM */
        memcpy(gps_update.date_dmy + 4, nav.date + 8, 2); /* YY */
        gps_update.date_dmy[6] = '\0';
        gps_fields |= NMEA_GPS_FIELDS_DATE;
        gps_updated = true;
        gps_clock_changed = true;
    }
    if (nav.gps_hdgspd_ts != 0 && (now - nav.gps_hdgspd_ts) <= TIMEOUT_MS) {
        gps_update.sog_kn = nav.gps_sog;
        gps_update.cog_deg = nav.gps_cog;
        gps_update.track_true_deg = nav.gps_cog;
        gps_update.fix = 'A';
        gps_update.sats = 8;
        gps_fields |= NMEA_GPS_FIELDS_MOTION;
        gps_updated = true;
    }
    if (gps_fields != 0) {
        nmea_templates_gps_update_fields(&gps_update, gps_fields);
        nmea_mark_dirty(GRP_GPS);
    }

    // GYRO: HDG, HDT, HDM
    if (nav.has_gyro_heading && (now - nav.gyro_ts) <= TIMEOUT_MS) {
        gyro_update.heading_true_deg = nav.gyro_heading;
        gyro_update.heading_mag_deg = nav.gyro_heading;
        gyro_update.deviation_deg = 0.0f;
        gyro_update.variation_deg = 0.0f;
        gyro_update.dev_dir = 'E';
        gyro_update.var_dir = 'E';
        nmea_templates_gyro_update_fields(
            &gyro_update,
            NMEA_GYRO_FIELD_HEADING_TRUE | NMEA_GYRO_FIELD_HEADING_MAG |
            NMEA_GYRO_FIELD_DEVIATION | NMEA_GYRO_FIELD_VARIATION |
            NMEA_GYRO_FIELD_DEV_DIR | NMEA_GYRO_FIELD_VAR_DIR);
        nmea_mark_dirty(GRP_GYRO);
        gyro_updated = true;
    }

    // LOG: VHW, VBW
    if (nav.has_log_speed && (now - nav.log_ts) <= TIMEOUT_MS) {
        g_nmea_log.water_speed_kn = nav.log_speed;
        g_nmea_log.hdg_water_true_deg = nav.gyro_heading;
        g_nmea_log.hdg_water_mag_deg = nav.gyro_heading;
        g_nmea_log.water_speed_kph = nav.log_speed * 1.852f;
        g_nmea_log.stern_speed_kn = 0.0f;
        g_nmea_log.dist_total_nm = 0.0f;
        g_nmea_log.dist_trip_nm = 0.0f;
        nmea_mark_dirty(GRP_LOG);
        log_updated = true;
    }

    // ECHO: DPT, DBT, DBS, DBK
    if (nav.has_depth && (now - nav.depth_ts) <= TIMEOUT_MS) {
        g_nmea_echo.depth_m = nav.depth;
        g_nmea_echo.depth_ft = nav.depth * 3.28084f;
        g_nmea_echo.depth_fathom = nav.depth * 0.546807f;
        g_nmea_echo.keel_offset_m = 0.0f;
        nmea_mark_dirty(GRP_ECHO);
        echo_updated = true;
    }

    // WEATHER: MWD, MWV, VWR, VWT, MTW
    if (nav.weather_ts != 0 && (now - nav.weather_ts) <= TIMEOUT_MS) {
        g_nmea_weather.wind_dir_true_deg = nav.wind_true ? nav.wind_angle : 0.0f;
        g_nmea_weather.wind_dir_mag_deg = nav.wind_angle;
        g_nmea_weather.wind_speed_kn = nav.wind_speed;
        g_nmea_weather.wind_speed_ms = nav.wind_speed * 0.514444f;
        g_nmea_weather.wind_speed_kph = nav.wind_speed * 1.852f;
        g_nmea_weather.wind_angle_rel_deg = nav.wind_angle;
        g_nmea_weather.rel_ref = nav.wind_true ? 'T' : 'R';
        nmea_mark_dirty(GRP_WX);
        wx_updated = true;
    }
    if (nav.temp_ts != 0 && (now - nav.temp_ts) <= TIMEOUT_MS) {
        g_nmea_weather.water_temp_C = nav.temp_c;
        nmea_mark_dirty(GRP_WX);
        wx_updated = true;
    }

    nmea_gps_t gps_snapshot;
    nmea_gyro_t gyro_snapshot;
    nmea_templates_gps_snapshot(&gps_snapshot);
    nmea_templates_gyro_snapshot(&gyro_snapshot);
    ESP_LOGI(TAG,
             "refresh gps=%d gyro=%d log=%d echo=%d wx=%d | "
             "gps{%s %s %s%c %s%c sog=%.1f cog=%.1f} "
             "gyro{%.1f} log{%.1f} echo{%.1f} wx{%.1f/%.1f temp=%.1f}",
             gps_updated, gyro_updated, log_updated, echo_updated, wx_updated,
             gps_snapshot.time_utc, gps_snapshot.date_dmy,
             gps_snapshot.lat, gps_snapshot.lat_dir,
             gps_snapshot.lon, gps_snapshot.lon_dir,
             gps_snapshot.sog_kn, gps_snapshot.cog_deg,
             gyro_snapshot.heading_true_deg,
             g_nmea_log.water_speed_kn,
             g_nmea_echo.depth_m,
             g_nmea_weather.wind_angle_rel_deg,
             g_nmea_weather.wind_speed_kn,
             g_nmea_weather.water_temp_C);

    if (gps_clock_changed && gps_snapshot.time_utc[0] && gps_snapshot.date_dmy[0]) {
        (void)nmea_clock_sync_from_template();
    }
}
