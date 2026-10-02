/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#include "app/navigation_model.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_wire.h"
#include "system/nmea_clock.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_INSTRUMENT_PANEL
#include "config_logs.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_nav_lock = portMUX_INITIALIZER_UNLOCKED;
#define NAV_LOCK() portENTER_CRITICAL(&s_nav_lock)
#define NAV_UNLOCK() portEXIT_CRITICAL(&s_nav_lock)
static uint32_t now_ms_(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t s_nav_lock = PTHREAD_MUTEX_INITIALIZER;
#define NAV_LOCK() ((void)pthread_mutex_lock(&s_nav_lock))
#define NAV_UNLOCK() ((void)pthread_mutex_unlock(&s_nav_lock))
static uint32_t now_ms_(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U);
}
#endif

static navigation_snapshot_t s_nav;
static const char *TAG = "navigation_model";

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


/* Parse one sentence into a sparse patch. Parsing/float conversion happens
 * outside the short state lock; unrelated concurrent sources cannot be lost. */
static void parse_coords_(navigation_snapshot_t *n, char **p, uint32_t now)
{
    float lat, lon;
    bool lat_ok = parse_coord_(next_tok(p), 90, &lat);
    char lat_dir = *next_tok(p);
    bool lon_ok = parse_coord_(next_tok(p), 180, &lon);
    char lon_dir = *next_tok(p);
    if (!lat_ok || !lon_ok || (lat_dir != 'N' && lat_dir != 'S') ||
        (lon_dir != 'E' && lon_dir != 'W')) return;
    n->lat = lat; n->lon = lon; n->lat_dir = lat_dir; n->lon_dir = lon_dir;
    n->coords_ts = now;
    n->valid_fields |= NAVIGATION_COORDS;
}

static void parse_utc_(navigation_snapshot_t *n, const char *s, uint32_t now)
{
    if (parse_time_(s, n->time)) {
        n->time_ts = now;
        n->valid_fields |= NAVIGATION_TIME;
    }
}

static void parse_motion_(navigation_snapshot_t *n, const char *sog,
                          const char *cog, uint32_t now)
{
    if (parse_float_(sog, 0.0f, 1000.0f, &n->gps_sog) &&
        parse_float_(cog, 0.0f, 360.0f, &n->gps_cog)) {
        n->gps_hdgspd_ts = now;
        n->valid_fields |= NAVIGATION_GPS_MOTION;
    }
}

static void parse_heading_(navigation_snapshot_t *n, const char *s, uint32_t now)
{
    if (parse_float_(s, 0.0f, 360.0f, &n->gyro_heading)) {
        n->has_gyro_heading = true; n->gyro_ts = now;
        n->valid_fields |= NAVIGATION_GYRO;
    }
}

static void parse_log_(navigation_snapshot_t *n, const char *s, uint32_t now)
{
    if (parse_float_(s, -1000.0f, 1000.0f, &n->log_speed)) {
        n->has_log_speed = true; n->log_ts = now;
        n->valid_fields |= NAVIGATION_LOG;
    }
}

static bool parse_uint_(const char *s, unsigned min, unsigned max, unsigned *out)
{
    char *end;
    unsigned long value;
    if (!s || !*s || !isdigit((unsigned char)*s)) return false;
    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno || *end || value < min || value > max) return false;
    *out = (unsigned)value;
    return true;
}

static void dispatch_(navigation_snapshot_t *n, const char *id, char *p, uint32_t now)
{
    if (strcmp(id, "RMC") == 0) {
        parse_utc_(n, next_tok(&p), now);
        next_tok(&p); /* Preserve panel semantics: observe reported fields. */
        parse_coords_(n, &p, now);
        char *sog = next_tok(&p), *cog = next_tok(&p);
        parse_motion_(n, sog, cog, now);
        if (parse_rmc_date_(next_tok(&p), n->date)) {
            n->date_ts = now; n->valid_fields |= NAVIGATION_DATE;
        }
    } else if (strcmp(id, "GGA") == 0) {
        parse_utc_(n, next_tok(&p), now);
        parse_coords_(n, &p, now);
    } else if (strcmp(id, "GLL") == 0) {
        parse_coords_(n, &p, now);
        parse_utc_(n, next_tok(&p), now);
    } else if (strcmp(id, "ZDA") == 0) {
        parse_utc_(n, next_tok(&p), now);
        unsigned day, month, year;
        char *d = next_tok(&p), *m = next_tok(&p), *y = next_tok(&p);
        if (parse_uint_(d, 1, 31, &day) && parse_uint_(m, 1, 12, &month) &&
            parse_uint_(y, 2000, 2099, &year)) {
            snprintf(n->date, sizeof(n->date), "%02u-%02u-%04u", day, month, year);
            n->date_ts = now; n->valid_fields |= NAVIGATION_DATE;
        }
    } else if (strcmp(id, "VTG") == 0) {
        char *cog = next_tok(&p);
        next_tok(&p); next_tok(&p); next_tok(&p);
        parse_motion_(n, next_tok(&p), cog, now);
    } else if (strcmp(id, "VBW") == 0) {
        parse_log_(n, next_tok(&p), now);
    } else if (strcmp(id, "THS") == 0 || strcmp(id, "HDT") == 0 ||
               strcmp(id, "HDG") == 0 || strcmp(id, "HDM") == 0) {
        parse_heading_(n, next_tok(&p), now);
    } else if (strcmp(id, "VHW") == 0) {
        next_tok(&p); next_tok(&p);
        parse_heading_(n, next_tok(&p), now);
        next_tok(&p);
        parse_log_(n, next_tok(&p), now);
    } else if (strcmp(id, "DPT") == 0 || strcmp(id, "DBT") == 0 ||
               strcmp(id, "DBS") == 0 || strcmp(id, "DBK") == 0) {
        if (strcmp(id, "DPT") != 0) { next_tok(&p); next_tok(&p); }
        if (parse_float_(next_tok(&p), -10000.0f, 100000.0f, &n->depth)) {
            n->has_depth = true; n->depth_ts = now;
            n->valid_fields |= NAVIGATION_DEPTH;
        }
    } else if (strcmp(id, "MWV") == 0) {
        char *a = next_tok(&p), *ref = next_tok(&p), *s = next_tok(&p);
        char *unit = next_tok(&p), *status = next_tok(&p);
        if (*status != 'A' || (*ref != 'R' && *ref != 'T') ||
            !parse_float_(a, 0.0f, 360.0f, &n->wind_angle) ||
            !parse_float_(s, 0.0f, 1000.0f, &n->wind_speed)) return;
        if (*unit == 'M') n->wind_speed *= 1.9438445f;
        else if (*unit == 'K') n->wind_speed /= 1.852f;
        else if (*unit != 'N') return;
        n->wind_true = *ref == 'T';
        n->weather_ts = now; n->valid_fields |= NAVIGATION_WIND;
    } else if (strcmp(id, "MWD") == 0) {
        char *a = next_tok(&p);
        next_tok(&p); next_tok(&p); next_tok(&p);
        char *s = next_tok(&p);
        if (parse_float_(a, 0.0f, 360.0f, &n->wind_angle) &&
            parse_float_(s, 0.0f, 1000.0f, &n->wind_speed)) {
            n->wind_true = true; n->weather_ts = now;
            n->valid_fields |= NAVIGATION_WIND;
        }
    } else if (strcmp(id, "VWR") == 0 || strcmp(id, "VWT") == 0) {
        char *a = next_tok(&p), *side = next_tok(&p), *s = next_tok(&p);
        char *unit = next_tok(&p);
        if ((*side == 'L' || *side == 'R') && *unit == 'N' &&
            parse_float_(a, 0.0f, 180.0f, &n->wind_angle) &&
            parse_float_(s, 0.0f, 1000.0f, &n->wind_speed)) {
            if (*side == 'L') n->wind_angle = -n->wind_angle;
            n->wind_true = strcmp(id, "VWT") == 0;
            n->weather_ts = now; n->valid_fields |= NAVIGATION_WIND;
        }
    } else if (strcmp(id, "MTW") == 0) {
        if (parse_float_(next_tok(&p), -273.15f, 1000.0f, &n->temp_c)) {
            n->temp_ts = now; n->valid_fields |= NAVIGATION_TEMPERATURE;
        }
    }
}

void navigation_model_reset(void)
{
    NAV_LOCK();
    const uint32_t revision = s_nav.revision + 1U;
    memset(&s_nav, 0, sizeof(s_nav));
    s_nav.revision = revision;
    NAV_UNLOCK();
}

void navigation_model_process_at(const char *line, uint32_t now_ms)
{
    navigation_snapshot_t n = {0};
    char in[83];
    if (!nmea_wire_sentence_valid(line, false)) return;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (len < 7U || line[6] != ',') return;
    memcpy(in, line, len);
    in[len] = '\0';
    char *star = strchr(in, '*');
    if (star) *star = '\0';
    in[6] = '\0';
    dispatch_(&n, in + 3, in + 7, now_ms);
    if (!n.valid_fields) return;

    NAV_LOCK();
    if (n.valid_fields & NAVIGATION_COORDS) {
        s_nav.lat = n.lat; s_nav.lon = n.lon;
        s_nav.lat_dir = n.lat_dir; s_nav.lon_dir = n.lon_dir;
        s_nav.coords_ts = n.coords_ts;
    }
    if (n.valid_fields & NAVIGATION_TIME) {
        memcpy(s_nav.time, n.time, sizeof(s_nav.time)); s_nav.time_ts = n.time_ts;
    }
    if (n.valid_fields & NAVIGATION_DATE) {
        memcpy(s_nav.date, n.date, sizeof(s_nav.date)); s_nav.date_ts = n.date_ts;
    }
    if (n.valid_fields & NAVIGATION_GPS_MOTION) {
        s_nav.gps_sog = n.gps_sog; s_nav.gps_cog = n.gps_cog;
        s_nav.gps_hdgspd_ts = n.gps_hdgspd_ts;
    }
    if (n.valid_fields & NAVIGATION_GYRO) {
        s_nav.gyro_heading = n.gyro_heading; s_nav.has_gyro_heading = true;
        s_nav.gyro_ts = n.gyro_ts;
    }
    if (n.valid_fields & NAVIGATION_LOG) {
        s_nav.log_speed = n.log_speed; s_nav.has_log_speed = true; s_nav.log_ts = n.log_ts;
    }
    if (n.valid_fields & NAVIGATION_DEPTH) {
        s_nav.depth = n.depth; s_nav.has_depth = true; s_nav.depth_ts = n.depth_ts;
    }
    if (n.valid_fields & NAVIGATION_WIND) {
        s_nav.wind_angle = n.wind_angle; s_nav.wind_speed = n.wind_speed;
        s_nav.wind_true = n.wind_true; s_nav.weather_ts = n.weather_ts;
    }
    if (n.valid_fields & NAVIGATION_TEMPERATURE) {
        s_nav.temp_c = n.temp_c; s_nav.temp_ts = n.temp_ts;
    }
    s_nav.valid_fields |= n.valid_fields;
    s_nav.revision++;
    NAV_UNLOCK();
}

void navigation_model_process(const char *line)
{
    navigation_model_process_at(line, now_ms_());
}

static void snapshot_age_(navigation_snapshot_t *out, uint32_t now_ms)
{
    out->now_ms = now_ms;
    out->stale_fields = NAVIGATION_ALL_FIELDS & ~out->valid_fields;
#define STALE(FIELD, TS) do { if ((uint32_t)(now_ms - out->TS) > NAVIGATION_STALE_MS) \
                                  out->stale_fields |= FIELD; } while (0)
    STALE(NAVIGATION_COORDS, coords_ts);
    STALE(NAVIGATION_TIME, time_ts);
    STALE(NAVIGATION_DATE, date_ts);
    STALE(NAVIGATION_GPS_MOTION, gps_hdgspd_ts);
    STALE(NAVIGATION_GYRO, gyro_ts);
    STALE(NAVIGATION_LOG, log_ts);
    STALE(NAVIGATION_DEPTH, depth_ts);
    STALE(NAVIGATION_WIND, weather_ts);
    STALE(NAVIGATION_TEMPERATURE, temp_ts);
#undef STALE
}

void navigation_model_snapshot_at(navigation_snapshot_t *out, uint32_t now_ms)
{
    if (!out) return;
    NAV_LOCK();
    *out = s_nav;
    NAV_UNLOCK();
    snapshot_age_(out, now_ms);
}

void navigation_model_snapshot(navigation_snapshot_t *out)
{
    if (!out) return;
    NAV_LOCK();
    *out = s_nav;
    NAV_UNLOCK();
    /* Sample the clock after the snapshot: an update on another core must not
     * make a just-received timestamp appear to be UINT32_MAX milliseconds old. */
    snapshot_age_(out, now_ms_());
}

uint32_t navigation_model_refresh_templates(void) {
    navigation_snapshot_t nav;
    navigation_model_snapshot(&nav);
    const uint32_t fresh = nav.valid_fields & ~nav.stale_fields;
    nmea_gps_t gps_update;
    nmea_gyro_t gyro_update;
    uint32_t gps_fields = 0;
    bool gps_clock_changed = false;
    bool gps_updated = false;
    bool gyro_updated = false;
    bool log_updated = false;
    bool echo_updated = false;
    bool wx_updated = false;

    nmea_templates_snapshot_t templates;
    nmea_templates_snapshot_all(&templates);
    gps_update = templates.gps;
    gyro_update = templates.gyro;
    nmea_log_t log_update = templates.log;
    nmea_echo_t echo_update = templates.echo;
    nmea_weather_t wx_update = templates.weather;
    uint8_t log_mask[sizeof(log_update)] = {0};
    uint8_t echo_mask[sizeof(echo_update)] = {0};
    uint8_t wx_mask[sizeof(wx_update)] = {0};
#define UPDATE_FIELD(GROUP, FIELD, VALUE) do { \
        GROUP##_update.FIELD = (VALUE); \
        memset(GROUP##_mask + ((uint8_t *)&GROUP##_update.FIELD - \
                              (uint8_t *)&GROUP##_update), \
               1, sizeof(GROUP##_update.FIELD)); \
    } while (0)

    // GPS: RMC, GGA, ZDA, GLL, VTG
    if (fresh & NAVIGATION_COORDS) {
        deg_to_lat_str_(nav.lat, gps_update.lat, sizeof(gps_update.lat));
        gps_update.lat_dir = nav.lat_dir;
        deg_to_lon_str_(nav.lon, gps_update.lon, sizeof(gps_update.lon));
        gps_update.lon_dir = nav.lon_dir;
        gps_fields |= NMEA_GPS_FIELDS_COORDS;
        gps_updated = true;
    }
    if (fresh & NAVIGATION_TIME) {
        memcpy(gps_update.time_utc, nav.time, 6);
        gps_update.time_utc[6] = '\0';
        gps_fields |= NMEA_GPS_FIELDS_TIME;
        gps_updated = true;
        gps_clock_changed = true;
    }
    if ((fresh & NAVIGATION_DATE) && strlen(nav.date) >= 10) {
        memcpy(gps_update.date_dmy + 0, nav.date + 0, 2); /* DD */
        memcpy(gps_update.date_dmy + 2, nav.date + 3, 2); /* MM */
        memcpy(gps_update.date_dmy + 4, nav.date + 8, 2); /* YY */
        gps_update.date_dmy[6] = '\0';
        gps_fields |= NMEA_GPS_FIELDS_DATE;
        gps_updated = true;
        gps_clock_changed = true;
    }
    if (fresh & NAVIGATION_GPS_MOTION) {
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
    if (fresh & NAVIGATION_GYRO) {
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
    if (fresh & NAVIGATION_LOG) {
        UPDATE_FIELD(log, water_speed_kn, nav.log_speed);
        UPDATE_FIELD(log, hdg_water_true_deg, nav.gyro_heading);
        UPDATE_FIELD(log, hdg_water_mag_deg, nav.gyro_heading);
        UPDATE_FIELD(log, water_speed_kph, nav.log_speed * 1.852f);
        UPDATE_FIELD(log, stern_speed_kn, 0.0f);
        UPDATE_FIELD(log, dist_total_nm, 0.0f);
        UPDATE_FIELD(log, dist_trip_nm, 0.0f);

        log_updated = true;
    }

    // ECHO: DPT, DBT, DBS, DBK
    if (fresh & NAVIGATION_DEPTH) {
        UPDATE_FIELD(echo, depth_m, nav.depth);
        UPDATE_FIELD(echo, depth_ft, nav.depth * 3.28084f);
        UPDATE_FIELD(echo, depth_fathom, nav.depth * 0.546807f);
        UPDATE_FIELD(echo, keel_offset_m, 0.0f);

        echo_updated = true;
    }

    // WEATHER: MWD, MWV, VWR, VWT, MTW
    if (fresh & NAVIGATION_WIND) {
        UPDATE_FIELD(wx, wind_dir_true_deg, nav.wind_true ? nav.wind_angle : 0.0f);
        UPDATE_FIELD(wx, wind_dir_mag_deg, nav.wind_angle);
        UPDATE_FIELD(wx, wind_speed_kn, nav.wind_speed);
        UPDATE_FIELD(wx, wind_speed_ms, nav.wind_speed * 0.514444f);
        UPDATE_FIELD(wx, wind_speed_kph, nav.wind_speed * 1.852f);
        UPDATE_FIELD(wx, wind_angle_rel_deg, nav.wind_angle);
        UPDATE_FIELD(wx, rel_ref, nav.wind_true ? 'T' : 'R');

        wx_updated = true;
    }
    if (fresh & NAVIGATION_TEMPERATURE) {
        UPDATE_FIELD(wx, water_temp_C, nav.temp_c);

        wx_updated = true;
    }

#undef UPDATE_FIELD
    if (log_updated) (void)nmea_templates_patch(GRP_LOG, &log_update, log_mask, sizeof(log_update));
    if (echo_updated) (void)nmea_templates_patch(GRP_ECHO, &echo_update, echo_mask, sizeof(echo_update));
    if (wx_updated) (void)nmea_templates_patch(GRP_WX, &wx_update, wx_mask, sizeof(wx_update));

    nmea_gps_t gps_snapshot;
    nmea_gyro_t gyro_snapshot;
    nmea_templates_gps_snapshot(&gps_snapshot);
    nmea_templates_gyro_snapshot(&gyro_snapshot);
    if (gps_updated || gyro_updated || log_updated || echo_updated || wx_updated) ESP_LOGI(TAG,
             "refresh gps=%d gyro=%d log=%d echo=%d wx=%d | "
             "gps{%s %s %s%c %s%c sog=%.1f cog=%.1f} "
             "gyro{%.1f} log{%.1f} echo{%.1f} wx{%.1f/%.1f temp=%.1f}",
             gps_updated, gyro_updated, log_updated, echo_updated, wx_updated,
             gps_snapshot.time_utc, gps_snapshot.date_dmy,
             gps_snapshot.lat, gps_snapshot.lat_dir,
             gps_snapshot.lon, gps_snapshot.lon_dir,
             gps_snapshot.sog_kn, gps_snapshot.cog_deg,
             gyro_snapshot.heading_true_deg,
             log_update.water_speed_kn,
             echo_update.depth_m,
             wx_update.wind_angle_rel_deg,
             wx_update.wind_speed_kn,
             wx_update.water_temp_C);

    if (gps_clock_changed && gps_snapshot.time_utc[0] && gps_snapshot.date_dmy[0]) {
        (void)nmea_clock_sync_from_template();
    }
    return fresh;
}
