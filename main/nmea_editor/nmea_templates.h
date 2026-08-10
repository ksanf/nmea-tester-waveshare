/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NMEA_TEMPLATES_H
#define NMEA_TEMPLATES_H

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include "rs485/rs485_engine.h"

/* ─────────── Shared limits ─────────── */
#define NMEA_TXT6     7   /* "hhmmss", "ddmmyy" + '\0'      */
#define NMEA_LATSTR  12   /* "ddmm.mmmm"                    */
#define NMEA_LONSTR  13   /* "dddmm.mmmm"                   */

/* ─────────── GPS: RMC/GGA/ZDA/GLL/VTG ─────────── */
typedef struct {
    char   time_utc[NMEA_TXT6];   /* HHMMSS      */
    char   date_dmy[NMEA_TXT6];   /* DDMMYY      */
    char   lat[NMEA_LATSTR];      /* ddmm.mmmm   */
    char   lon[NMEA_LONSTR];      /* dddmm.mmmm  */
    char   lat_dir;               /* 'N'/'S'     */
    char   lon_dir;               /* 'E'/'W'     */
    float  sog_kn;                /* Speed over ground */
    float  cog_deg;               /* Course over ground */
    float  track_true_deg;        /* for VTG     */
    char   fix;                   /* 'A'=valid / 'V' */
    uint8_t sats;                 /* 0-12        */
    char   talker_id[3];          /* Talker ID, e.g., "GP" */
    char   prefix;                /* Message prefix, e.g., '$' or '!' */

    /* Per-sentence transmit switches. */
    bool send_rmc, send_gga, send_zda, send_gll, send_vtg;
    bool add_crc;
    float rate_hz;
} nmea_gps_t;

/* ─────────── GYRO: HDG/HDT/HDM/THS/ROT ─────────── */
typedef struct {
    float  heading_true_deg;      /* HDT         */
    float  heading_mag_deg;       /* HDM         */
    float  deviation_deg;         /* Used by HDG */
    char   dev_dir;               /* 'E'/'W'     */
    float  variation_deg;         /* Used by HDG */
    char   var_dir;               /* 'E'/'W'     */
    char   talker_id[3];          /* Talker ID, e.g., "HE" */
    char   prefix;                /* Message prefix, e.g., '$' or '!' */

    bool send_hdg, send_hdt, send_hdm;
    bool add_crc;
    float rate_hz;

    /* Appended in template blob v2 to keep the v1 prefix migratable. */
    float rot_deg_min;            /* ROT, signed: '-' = turn to port */
    char  rot_status;             /* ROT: 'A'=valid / 'V'=invalid    */
    bool  send_rot;               /* transmit ROT sentence            */
    bool  follow_rot;             /* advance heading fields from ROT  */
} nmea_gyro_t;

/* ─────────── LOG: VHW/VLW/VBW ─────────── */
typedef struct {
    float  hdg_water_true_deg;    /* VHW, true   */
    float  hdg_water_mag_deg;     /* VHW, mag    */
    float  water_speed_kn;        /* VHW / VBW   */
    float  water_speed_kph;       /* VHW         */
    float  dist_total_nm;         /* VLW total   */
    float  dist_trip_nm;          /* VLW trip    */
    float  stern_speed_kn;        /* VBW         */
    char   talker_id[3];          /* Talker ID, e.g., "VW" */
    char   prefix;                /* Message prefix, e.g., '$' or '!' */

    bool send_vhw, send_vlw, send_vbw;
    bool add_crc;
    float rate_hz;
} nmea_log_t;

/* ─────────── ECHO: DBT/DPT/DBK/DBS ─────────── */
typedef struct {
    float  depth_ft;              /* DBT         */
    float  depth_m;               /* DPT/DBS     */
    float  depth_fathom;          /* DBK         */
    float  keel_offset_m;         /* DPT field 2 */
    char   talker_id[3];          /* Talker ID, e.g., "SD" */
    char   prefix;                /* Message prefix, e.g., '$' or '!' */

    bool send_dbt, send_dpt, send_dbk, send_dbs;
    bool add_crc;
    float rate_hz;
} nmea_echo_t;

/* ─────────── WEATHER: MWD/MWV/VWR/VWT/MTW ─────────── */
typedef struct {
    float  wind_dir_true_deg;     /* MWD (true)  */
    float  wind_dir_mag_deg;      /* MWD (mag)   */
    float  wind_speed_kn;         /* MWD/MWV/VWT */
    float  wind_speed_ms;         /* MWV         */
    float  wind_speed_kph;        /* VWR         */
    float  wind_angle_rel_deg;    /* MWV/VWR/VWT */
    char   rel_ref;               /* MWV: 'R' or 'T' */
    char   wind_side;             /* VWR/VWT: 'L' or 'R' */
    float  water_temp_C;          /* MTW         */
    char   talker_id[3];          /* Talker ID, e.g., "WI" */
    char   prefix;                /* Message prefix, e.g., '$' or '!' */

    bool send_mwd, send_mwv, send_vwr, send_vwt, send_mtw;
    bool add_crc;
    float rate_hz;
} nmea_weather_t;

typedef struct {
    bool active;
    bool dirty;
} nmea_group_state_t;
/* Mark a group dirty. */
void nmea_mark_dirty(int grp_id);   /* grp_id == enum GRP_GPS … GRP_WX */

extern nmea_group_state_t groups[GRP_COUNT];

/* ─────────── Global instances ─────────── */
extern nmea_gps_t     g_nmea_gps;
extern nmea_gyro_t    g_nmea_gyro;
extern nmea_log_t     g_nmea_log;
extern nmea_echo_t    g_nmea_echo;
extern nmea_weather_t g_nmea_weather;

/* Clock task and formatter run on different cores. These helpers provide a
 * coherent GPS/time snapshot without exposing the internal lock. */
void nmea_templates_gps_snapshot(nmea_gps_t *out);
typedef enum {
    NMEA_GPS_FIELDS_COORDS = 1u << 0,
    NMEA_GPS_FIELDS_TIME   = 1u << 1,
    NMEA_GPS_FIELDS_DATE   = 1u << 2,
    NMEA_GPS_FIELDS_MOTION = 1u << 3,
} nmea_gps_field_mask_t;
void nmea_templates_gps_update_fields(const nmea_gps_t *src, uint32_t fields);
void nmea_templates_gps_time_update(const char time_utc[NMEA_TXT6],
                                    const char date_dmy[NMEA_TXT6]);
void nmea_templates_gps_time_snapshot(char time_utc[NMEA_TXT6],
                                      char date_dmy[NMEA_TXT6]);

/* The LVGL editor and transmitter task access GYRO from different cores. */
void nmea_templates_gyro_snapshot(nmea_gyro_t *out);
typedef enum {
    NMEA_GYRO_FIELD_HEADING_TRUE = 1u << 0,
    NMEA_GYRO_FIELD_HEADING_MAG  = 1u << 1,
    NMEA_GYRO_FIELD_DEVIATION    = 1u << 2,
    NMEA_GYRO_FIELD_DEV_DIR      = 1u << 3,
    NMEA_GYRO_FIELD_VARIATION    = 1u << 4,
    NMEA_GYRO_FIELD_VAR_DIR      = 1u << 5,
    NMEA_GYRO_FIELD_TALKER       = 1u << 6,
    NMEA_GYRO_FIELD_PREFIX       = 1u << 7,
    NMEA_GYRO_FIELD_SEND_HDG     = 1u << 8,
    NMEA_GYRO_FIELD_SEND_HDT     = 1u << 9,
    NMEA_GYRO_FIELD_SEND_HDM     = 1u << 10,
    NMEA_GYRO_FIELD_CRC          = 1u << 11,
    NMEA_GYRO_FIELD_RATE         = 1u << 12,
    NMEA_GYRO_FIELD_ROT_RATE     = 1u << 13,
    NMEA_GYRO_FIELD_ROT_STATUS   = 1u << 14,
    NMEA_GYRO_FIELD_SEND_ROT     = 1u << 15,
    NMEA_GYRO_FIELD_FOLLOW_ROT   = 1u << 16,
} nmea_gyro_field_mask_t;
void nmea_templates_gyro_update_fields(const nmea_gyro_t *src, uint32_t fields);
void nmea_templates_gyro_advance(uint32_t elapsed_ms);

esp_err_t nmea_templates_init(void);
esp_err_t nmea_templates_load(void);
esp_err_t nmea_templates_save_now(void);
void nmea_templates_schedule_save(void);

#endif /* NMEA_TEMPLATES_H */
