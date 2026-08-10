/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_version.h"
#include "config/config_nmea_tester.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"

static inline char position_mode_(char fix)
{
    return (fix == 'A') ? 'A' : 'N';
}

static inline char wind_side_(char side)
{
    return (side == 'L') ? 'L' : 'R';
}

/* CRC + wire-length validation ************************************/
static bool finalize_sentence_(char *s, size_t capacity, bool add_checksum)
{
    size_t len;
    unsigned char cs = 0;

    if (!s || capacity == 0 || !*s) return false;
    len = strlen(s);
    if (len >= capacity) return false;

    for (const char *p = s + 1; *p && *p != '*'; ++p)
        cs ^= (unsigned char)*p;

    if (add_checksum) {
        if (len + 3u >= capacity) return false;
        snprintf(s + len, capacity - len, "*%02X", cs);
        len += 3u;
    }

    /* NMEA 0183 limit includes the terminating CR/LF pair. */
    return (len + 2u) <= NMEA0183_WIRE_MAX;
}

/* EMIT ************************************************************/
#define EMIT_IF(COND, FMT, ...)                                              \
    do {                                                                     \
        if ((COND) && *cnt < MAX_LNS) {                                      \
            int written_ = snprintf(lines[*cnt], NMEA_SENT_MAX,              \
                                    FMT, __VA_ARGS__);                        \
            if (written_ > 0 && written_ < NMEA_SENT_MAX &&                  \
                finalize_sentence_(lines[*cnt], NMEA_SENT_MAX,               \
                                   add_crc_flag)) {                           \
                (*cnt)++;                                                    \
            }                                                                \
        }                                                                    \
    } while (0)

/* ═══════════════════════════════════════════════════════════════════════ */
/*  GPS: RMC · GGA · ZDA · GLL · VTG                                       */
/* ═══════════════════════════════════════════════════════════════════════ */
void regen_gps(char lines[][NMEA_SENT_MAX], uint8_t *cnt)
{
    nmea_gps_t gps;
    *cnt = 0;
    nmea_templates_gps_snapshot(&gps);
    const nmea_gps_t *p = &gps;
    const nmea_version_profile_t profile = *nmea_version_profile();
    const char  prf          = p->prefix;
    const char *tk           = p->talker_id;
    const int   add_crc_flag = p->add_crc;
    const char  mode         = position_mode_(p->fix);
	//  ESP_LOGI("REGEN", "flags: RMC=%d GGA=%d ZDA=%d GLL=%d VTG=%d",
         //    p->send_rmc, p->send_gga, p->send_zda,
           //  p->send_gll, p->send_vtg);
    /* RMC ----------------------------------------------------------- */
    if (profile.rmc_nav_status) {
        EMIT_IF(p->send_rmc,
            "%c%sRMC,%s,%c,%s,%c,%s,%c,%.1f,%.1f,%s,,,%c,V",
            prf, tk,
            p->time_utc, p->fix,
            p->lat, p->lat_dir,
            p->lon, p->lon_dir,
            p->sog_kn, p->cog_deg,
            p->date_dmy, mode);
    } else if (profile.position_mode) {
        EMIT_IF(p->send_rmc,
            "%c%sRMC,%s,%c,%s,%c,%s,%c,%.1f,%.1f,%s,,,%c",
            prf, tk,
            p->time_utc, p->fix,
            p->lat, p->lat_dir,
            p->lon, p->lon_dir,
            p->sog_kn, p->cog_deg,
            p->date_dmy, mode);
    } else {
        EMIT_IF(p->send_rmc,
            "%c%sRMC,%s,%c,%s,%c,%s,%c,%.1f,%.1f,%s,,",
            prf, tk,
            p->time_utc, p->fix,
            p->lat, p->lat_dir,
            p->lon, p->lon_dir,
            p->sog_kn, p->cog_deg,
            p->date_dmy);
    }

    /* GGA ----------------------------------------------------------- */
    EMIT_IF(p->send_gga,
        "%c%sGGA,%s,%s,%c,%s,%c,%u,%02u,1.0,0.0,M,0.0,M,,",
        prf, tk,
        p->time_utc,
        p->lat, p->lat_dir,
        p->lon, p->lon_dir,
        (unsigned)(p->fix == 'A' ? 1u : 0u),
        (unsigned)p->sats);

    /* ZDA ----------------------------------------------------------- */
    EMIT_IF(p->send_zda,
        "%c%sZDA,%s,%2.2s,%2.2s,20%2.2s,,",
        prf, tk,
        p->time_utc,
        p->date_dmy, p->date_dmy + 2, p->date_dmy + 4);

    /* GLL ----------------------------------------------------------- */
    if (profile.position_mode) {
        EMIT_IF(p->send_gll,
            "%c%sGLL,%s,%c,%s,%c,%s,%c,%c",
            prf, tk,
            p->lat, p->lat_dir,
            p->lon, p->lon_dir,
            p->time_utc, p->fix, mode);
    } else {
        EMIT_IF(p->send_gll,
            "%c%sGLL,%s,%c,%s,%c,%s,%c",
            prf, tk,
            p->lat, p->lat_dir,
            p->lon, p->lon_dir,
            p->time_utc, p->fix);
    }

    /* VTG ----------------------------------------------------------- */
    if (profile.position_mode) {
        EMIT_IF(p->send_vtg,
            "%c%sVTG,%.1f,T,,M,%.1f,N,%.1f,K,%c",
            prf, tk,
            p->track_true_deg,
            p->sog_kn, p->sog_kn * 1.852f, mode);
    } else {
        EMIT_IF(p->send_vtg,
            "%c%sVTG,%.1f,T,,M,%.1f,N,%.1f,K",
            prf, tk,
            p->track_true_deg,
            p->sog_kn, p->sog_kn * 1.852f);
    }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  GYRO: HDG · HDM · HDT/THS · ROT                                        */
/* ═══════════════════════════════════════════════════════════════════════ */
void regen_gyro(char lines[][NMEA_SENT_MAX], uint8_t *cnt)
{
    nmea_gyro_t gyro;
    *cnt = 0;
    nmea_templates_gyro_snapshot(&gyro);
    const nmea_gyro_t *p = &gyro;
    const nmea_version_profile_t profile = *nmea_version_profile();
    const char  prf          = p->prefix;
    const char *tk           = p->talker_id;
    const int   add_crc_flag = p->add_crc;

    /* HDG: magnetic sensor heading with deviation and variation. ---- */
    EMIT_IF(p->send_hdg,
        "%c%sHDG,%.1f,%.1f,%c,%.1f,%c",
        prf, tk,
        p->heading_mag_deg,
        p->deviation_deg, p->dev_dir,
        p->variation_deg, p->var_dir);

    /* HDM: magnetic heading. --------------------------------------- */
    EMIT_IF(p->send_hdm,
        "%c%sHDM,%.1f,M",
        prf, tk,
        p->heading_mag_deg);

    if (profile.ths_heading) {
        /* THS includes the mandatory mode indicator. S = Simulator. */
        EMIT_IF(p->send_hdt,
            "%c%sTHS,%.1f,S",
            prf, tk,
            p->heading_true_deg);
    } else {
        /* HDT: true heading. --------------------------------------- */
        EMIT_IF(p->send_hdt,
            "%c%sHDT,%.1f,T",
            prf, tk,
            p->heading_true_deg);
    }

    /* ROT: signed degrees/minute; '-' means bow turns to port. */
    EMIT_IF(p->send_rot,
        "%c%sROT,%.1f,%c",
        prf, tk,
        p->rot_deg_min, p->rot_status);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  LOG: VLW · VBW · VHW                                                   */
/* ═══════════════════════════════════════════════════════════════════════ */
void regen_log(char lines[][NMEA_SENT_MAX], uint8_t *cnt)
{
    *cnt = 0;
    const nmea_log_t *p = &g_nmea_log;
    const char  prf          = p->prefix;
    const char *tk           = p->talker_id;
    const int   add_crc_flag = p->add_crc;

    /* VLW: distance traveled. -------------------------------------- */
    EMIT_IF(p->send_vlw,
        "%c%sVLW,%.1f,N,%.1f,N,,N,,N",
        prf, tk,
        p->dist_total_nm, p->dist_trip_nm);

    /* VBW: dual ground/water speed. -------------------------------- */
    EMIT_IF(p->send_vbw,
        "%c%sVBW,%.1f,0.0,A,,,V,%.1f,A,,V",
        prf, tk,
        p->water_speed_kn, p->stern_speed_kn);

    /* VHW: heading and speed through water. ------------------------ */
    EMIT_IF(p->send_vhw,
        "%c%sVHW,%.1f,T,%.1f,M,%.1f,N,%.1f,K",
        prf, tk,
        p->hdg_water_true_deg,
        p->hdg_water_mag_deg,
        p->water_speed_kn,
        p->water_speed_kph);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  ECHO: DPT · DBT · DBS · DBK                                            */
/* ═══════════════════════════════════════════════════════════════════════ */
void regen_echo(char lines[][NMEA_SENT_MAX], uint8_t *cnt)
{
    *cnt = 0;
    const nmea_echo_t *p = &g_nmea_echo;
    const char  prf          = p->prefix;
    const char *tk           = p->talker_id;
    const int   add_crc_flag = p->add_crc;

    const float depth_keel_m   = p->depth_m - p->keel_offset_m;
    const float depth_keel_ft  = depth_keel_m * FEET_PER_M;
    const float depth_keel_fth = depth_keel_m * FATHOM_PER_M;

    /* DPT: depth and transducer offset. ----------------------------- */
    EMIT_IF(p->send_dpt,
        "%c%sDPT,%.1f,%.1f",
        prf, tk,
        p->depth_m, p->keel_offset_m);

    /* DBT: depth below transducer. --------------------------------- */
    EMIT_IF(p->send_dbt,
        "%c%sDBT,%.1f,f,%.1f,M,%.1f,F",
        prf, tk,
        p->depth_ft, p->depth_m, p->depth_fathom);

    /* DBS: depth below surface; reuse the configured depth. -------- */
    EMIT_IF(p->send_dbs,
        "%c%sDBS,%.1f,f,%.1f,M,%.1f,F",
        prf, tk,
        p->depth_ft, p->depth_m, p->depth_fathom);

    /* DBK: depth below keel. --------------------------------------- */
    EMIT_IF(p->send_dbk,
        "%c%sDBK,%.1f,f,%.1f,M,%.1f,F",
        prf, tk,
        depth_keel_ft, depth_keel_m, depth_keel_fth);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/*  WEATHER: MWD · MWV · VWR · VWT · MTW                                  */
/* ═══════════════════════════════════════════════════════════════════════ */
void regen_weather(char lines[][NMEA_SENT_MAX], uint8_t *cnt)
{
    *cnt = 0;
    const nmea_weather_t *p = &g_nmea_weather;
    const char  prf          = p->prefix;
    const char *tk           = p->talker_id;
    const int   add_crc_flag = p->add_crc;

    /* MWD: wind direction and speed. ------------------------------- */
    EMIT_IF(p->send_mwd,
        "%c%sMWD,%.1f,T,%.1f,M,%.1f,N,%.1f,M",
        prf, tk,
        p->wind_dir_true_deg, p->wind_dir_mag_deg,
        p->wind_speed_kn, p->wind_speed_ms);

    /* MWV: relative or true wind. ---------------------------------- */
    EMIT_IF(p->send_mwv,
        "%c%sMWV,%.1f,%c,%.1f,N,A",
        prf, tk,
        p->wind_angle_rel_deg, p->rel_ref,
        p->wind_speed_kn);

    /* VWR: relative wind in multiple units. ------------------------ */
    EMIT_IF(p->send_vwr,
        "%c%sVWR,%.1f,%c,%.1f,N,%.1f,M,%.1f,K",
        prf, tk,
        p->wind_angle_rel_deg, wind_side_(p->wind_side),
        p->wind_speed_kn, p->wind_speed_ms, p->wind_speed_kph);

    /* VWT: true wind. --------------------------------------------- */
    EMIT_IF(p->send_vwt,
        "%c%sVWT,%.1f,%c,%.1f,N,%.1f,M,%.1f,K",
        prf, tk,
        p->wind_angle_rel_deg, wind_side_(p->wind_side),
        p->wind_speed_kn, p->wind_speed_ms, p->wind_speed_kph);

    /* MTW: water temperature. ------------------------------------- */
    EMIT_IF(p->send_mtw,
        "%c%sMTW,%.1f,C",
        prf, tk,
        p->water_temp_C);
}
