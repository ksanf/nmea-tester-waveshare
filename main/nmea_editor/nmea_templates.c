/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_motion.h"
#include "system/nvs_rw.h"
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_NMEA_TEMPLATES
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <nvs.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define TAG "nmea_templates"
#define NMEA_TPL_NAMESPACE "nmea_tpl"
#define NMEA_TPL_KEY       "blob"
#define NMEA_TPL_VERSION   2u
#define NMEA_TPL_SAVE_DEBOUNCE_MS 500

/* Exact GYRO layout stored by template blob v1. */
typedef struct {
    float  heading_true_deg;
    float  heading_mag_deg;
    float  deviation_deg;
    char   dev_dir;
    float  variation_deg;
    char   var_dir;
    char   talker_id[3];
    char   prefix;
    bool send_hdg, send_hdt, send_hdm;
    bool add_crc;
    float rate_hz;
} nmea_gyro_v1_t;

_Static_assert(offsetof(nmea_gyro_t, rot_deg_min) == sizeof(nmea_gyro_v1_t),
               "GYRO v1 migration prefix changed");

typedef struct {
    uint32_t version;
    nmea_gps_t gps;
    nmea_gyro_v1_t gyro;
    nmea_log_t log;
    nmea_echo_t echo;
    nmea_weather_t weather;
} nmea_templates_blob_v1_t;

typedef struct {
    uint32_t version;
    nmea_gps_t gps;
    nmea_gyro_t gyro;
    nmea_log_t log;
    nmea_echo_t echo;
    nmea_weather_t weather;
} nmea_templates_blob_t;

static TimerHandle_t s_save_timer;
static portMUX_TYPE s_gps_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_gyro_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_gyro_revision;

static esp_err_t nmea_templates_save_internal_(void);
static void nmea_templates_save_timer_cb_(TimerHandle_t timer);
static void nmea_templates_log_summary_(const char *prefix);
static void nmea_templates_sanitize_(void);
static void nmea_templates_load_v1_(const nmea_templates_blob_v1_t *old);

/* ───────── GPS ───────── */
nmea_gps_t g_nmea_gps = {
    .time_utc = "000000",
    .date_dmy = "150725",
    .lat      = "4306.9300", .lat_dir = 'N',
    .lon      = "13153.1300", .lon_dir = 'E',
    .sog_kn   = 0.0f, .cog_deg = 0.0f, .track_true_deg = 0.0f,
    .fix = 'A', .sats = 8,
    .talker_id = "GP",      /* GPS talker ID */
    .prefix = '$',          /* Standard NMEA prefix */
    .send_rmc = true, .send_gga = true, .send_zda = false,
    .send_gll = false, .send_vtg = false,
    .add_crc  = true,
    .rate_hz  = 1.0f,
};

/* ───────── GYRO ───────── */
nmea_gyro_t g_nmea_gyro = {
    .heading_true_deg = 121.0f,
    .heading_mag_deg  = 120.5f,
    .deviation_deg    = 0.0f, .dev_dir = 'E',
    .variation_deg    = 5.0f, .var_dir = 'W',
    .talker_id = "HE",      /* Heading talker ID */
    .prefix = '$',          /* Standard NMEA prefix */
    .send_hdg = true, .send_hdt = true, .send_hdm = false,
    .add_crc  = true,
    .rate_hz  = 1.0f,
    .rot_deg_min = 0.0f,
    .rot_status = 'A',
    .send_rot = false,
    .follow_rot = false,
};

/* ───────── LOG ───────── */
nmea_log_t g_nmea_log = {
    .hdg_water_true_deg = 0.0f,
    .hdg_water_mag_deg  = 0.0f,
    .water_speed_kn     = 0.0f,
    .water_speed_kph    = 0.0f,
    .dist_total_nm      = 0.0f,
    .dist_trip_nm       = 0.0f,
    .stern_speed_kn     = 0.0f,
    .talker_id = "VW",      /* Speed through water talker ID */
    .prefix = '$',          /* Standard NMEA prefix */
    .send_vhw = true, .send_vlw = true, .send_vbw = false,
    .add_crc  = true,
    .rate_hz  = 1.0f,
};

/* ───────── ECHO ───────── */
nmea_echo_t g_nmea_echo = {
    .depth_ft      = 19.0f,   /* 5.8 m  ≈ 19 ft */
    .depth_m       = 5.8f,
    .depth_fathom  = 3.2f,
    .keel_offset_m = 1.2f,
    .talker_id = "SD",      /* Sounder/Depth talker ID */
    .prefix = '$',          /* Standard NMEA prefix */
    .send_dbt = true, .send_dpt = true,
    .send_dbk = false, .send_dbs = false,
    .add_crc  = true,
    .rate_hz  = 1.0f,
};

/* ───────── WEATHER ────── */
nmea_weather_t g_nmea_weather = {
    .wind_dir_true_deg = 45.0f,
    .wind_dir_mag_deg  = 40.0f,
    .wind_speed_kn     = 12.5f,
    .wind_speed_ms     = 6.4f,
    .wind_speed_kph    = 23.1f,
    .wind_angle_rel_deg= 45.0f,
    .rel_ref           = 'R',      /* Relative */
    .wind_side         = 'R',      /* Starboard/right */
    .water_temp_C      = 16.3f,
    .talker_id = "WI",      /* Weather instruments */
    .prefix = '$',          /* Standard NMEA prefix */
    .send_mwd = true, .send_mwv = true,
    .send_vwr = false, .send_vwt = false, .send_mtw = true,
    .add_crc  = true,
    .rate_hz  = 1.0f,
};
void nmea_mark_dirty(int grp_id)
{
    if(grp_id >= 0 && grp_id < GRP_COUNT) {
        groups[grp_id].dirty = true;
        rs485_engine_mark_dirty((rs485_group_t)grp_id);
    }
    nmea_templates_schedule_save();
}
/* ───────── Group States ───────── */
nmea_group_state_t groups[GRP_COUNT] = {
    [GRP_GPS]  = { .active = false, .dirty = false },
    [GRP_GYRO] = { .active = false, .dirty = false },
    [GRP_LOG]  = { .active = false, .dirty = false },
    [GRP_ECHO] = { .active = false, .dirty = false },
    [GRP_WX]   = { .active = false, .dirty = false },
};

void nmea_templates_gps_snapshot(nmea_gps_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_gps_mux);
    *out = g_nmea_gps;
    portEXIT_CRITICAL(&s_gps_mux);
}

void nmea_templates_gps_update_fields(const nmea_gps_t *src, uint32_t fields)
{
    if (!src) return;
    portENTER_CRITICAL(&s_gps_mux);
    if (fields & NMEA_GPS_FIELDS_COORDS) {
        memcpy(g_nmea_gps.lat, src->lat, sizeof(g_nmea_gps.lat));
        memcpy(g_nmea_gps.lon, src->lon, sizeof(g_nmea_gps.lon));
        g_nmea_gps.lat_dir = src->lat_dir;
        g_nmea_gps.lon_dir = src->lon_dir;
    }
    if (fields & NMEA_GPS_FIELDS_TIME) {
        memcpy(g_nmea_gps.time_utc, src->time_utc, sizeof(g_nmea_gps.time_utc));
    }
    if (fields & NMEA_GPS_FIELDS_DATE) {
        memcpy(g_nmea_gps.date_dmy, src->date_dmy, sizeof(g_nmea_gps.date_dmy));
    }
    if (fields & NMEA_GPS_FIELDS_MOTION) {
        g_nmea_gps.sog_kn = src->sog_kn;
        g_nmea_gps.cog_deg = src->cog_deg;
        g_nmea_gps.track_true_deg = src->track_true_deg;
        g_nmea_gps.fix = src->fix;
        g_nmea_gps.sats = src->sats;
    }
    portEXIT_CRITICAL(&s_gps_mux);
}

void nmea_templates_gps_time_update(const char time_utc[NMEA_TXT6],
                                    const char date_dmy[NMEA_TXT6])
{
    if (!time_utc || !date_dmy) return;
    portENTER_CRITICAL(&s_gps_mux);
    memcpy(g_nmea_gps.time_utc, time_utc, NMEA_TXT6);
    memcpy(g_nmea_gps.date_dmy, date_dmy, NMEA_TXT6);
    portEXIT_CRITICAL(&s_gps_mux);
}

void nmea_templates_gps_time_snapshot(char time_utc[NMEA_TXT6],
                                      char date_dmy[NMEA_TXT6])
{
    if (!time_utc || !date_dmy) return;
    portENTER_CRITICAL(&s_gps_mux);
    memcpy(time_utc, g_nmea_gps.time_utc, NMEA_TXT6);
    memcpy(date_dmy, g_nmea_gps.date_dmy, NMEA_TXT6);
    portEXIT_CRITICAL(&s_gps_mux);
}

void nmea_templates_gyro_snapshot(nmea_gyro_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_gyro_mux);
    *out = g_nmea_gyro;
    portEXIT_CRITICAL(&s_gyro_mux);
}

void nmea_templates_gyro_update_fields(const nmea_gyro_t *src, uint32_t fields)
{
    if (!src || fields == 0u) return;

    const float heading_true = nmea_motion_heading_advance(src->heading_true_deg, 0.0f, 0u);
    const float heading_mag = nmea_motion_heading_advance(src->heading_mag_deg, 0.0f, 0u);
    const float deviation = isfinite(src->deviation_deg) ? src->deviation_deg : 0.0f;
    const float variation = isfinite(src->variation_deg) ? src->variation_deg : 0.0f;
    const float rot = isfinite(src->rot_deg_min) ? src->rot_deg_min : 0.0f;
    float rate = src->rate_hz;
    if (!isfinite(rate) || rate < 0.5f || rate > 10.0f) rate = 1.0f;

    portENTER_CRITICAL(&s_gyro_mux);
    if (fields & NMEA_GYRO_FIELD_HEADING_TRUE) g_nmea_gyro.heading_true_deg = heading_true;
    if (fields & NMEA_GYRO_FIELD_HEADING_MAG)  g_nmea_gyro.heading_mag_deg = heading_mag;
    if (fields & NMEA_GYRO_FIELD_DEVIATION)    g_nmea_gyro.deviation_deg = deviation;
    if (fields & NMEA_GYRO_FIELD_DEV_DIR) {
        g_nmea_gyro.dev_dir = (src->dev_dir == 'W') ? 'W' : 'E';
    }
    if (fields & NMEA_GYRO_FIELD_VARIATION)    g_nmea_gyro.variation_deg = variation;
    if (fields & NMEA_GYRO_FIELD_VAR_DIR) {
        g_nmea_gyro.var_dir = (src->var_dir == 'W') ? 'W' : 'E';
    }
    if (fields & NMEA_GYRO_FIELD_TALKER) {
        memcpy(g_nmea_gyro.talker_id, src->talker_id, sizeof(g_nmea_gyro.talker_id));
        g_nmea_gyro.talker_id[2] = '\0';
    }
    if (fields & NMEA_GYRO_FIELD_PREFIX) {
        g_nmea_gyro.prefix = (src->prefix == '!') ? '!' : '$';
    }
    if (fields & NMEA_GYRO_FIELD_SEND_HDG)     g_nmea_gyro.send_hdg = src->send_hdg;
    if (fields & NMEA_GYRO_FIELD_SEND_HDT)     g_nmea_gyro.send_hdt = src->send_hdt;
    if (fields & NMEA_GYRO_FIELD_SEND_HDM)     g_nmea_gyro.send_hdm = src->send_hdm;
    if (fields & NMEA_GYRO_FIELD_CRC)          g_nmea_gyro.add_crc = src->add_crc;
    if (fields & NMEA_GYRO_FIELD_RATE)         g_nmea_gyro.rate_hz = rate;
    if (fields & NMEA_GYRO_FIELD_ROT_RATE)     g_nmea_gyro.rot_deg_min = rot;
    if (fields & NMEA_GYRO_FIELD_ROT_STATUS) {
        g_nmea_gyro.rot_status = (src->rot_status == 'V') ? 'V' : 'A';
    }
    if (fields & NMEA_GYRO_FIELD_SEND_ROT)     g_nmea_gyro.send_rot = src->send_rot;
    if (fields & NMEA_GYRO_FIELD_FOLLOW_ROT)   g_nmea_gyro.follow_rot = src->follow_rot;
    s_gyro_revision++;
    portEXIT_CRITICAL(&s_gyro_mux);
}

void nmea_templates_gyro_advance(uint32_t elapsed_ms)
{
    float heading_true;
    float heading_mag;
    float rot;
    bool follow;
    uint32_t revision;

    if (elapsed_ms == 0u) return;

    portENTER_CRITICAL(&s_gyro_mux);
    heading_true = g_nmea_gyro.heading_true_deg;
    heading_mag = g_nmea_gyro.heading_mag_deg;
    rot = g_nmea_gyro.rot_deg_min;
    follow = g_nmea_gyro.follow_rot;
    revision = s_gyro_revision;
    portEXIT_CRITICAL(&s_gyro_mux);

    if (!follow || !isfinite(rot)) return;

    heading_true = nmea_motion_heading_advance(heading_true, rot, elapsed_ms);
    heading_mag = nmea_motion_heading_advance(heading_mag, rot, elapsed_ms);

    /* Do not overwrite a heading or ROT value edited while math was running. */
    portENTER_CRITICAL(&s_gyro_mux);
    if (revision == s_gyro_revision) {
        g_nmea_gyro.heading_true_deg = heading_true;
        g_nmea_gyro.heading_mag_deg = heading_mag;
    }
    portEXIT_CRITICAL(&s_gyro_mux);
}

esp_err_t nmea_templates_init(void)
{
    if (!s_save_timer) {
        s_save_timer = xTimerCreate("nmea_tpl_save",
                                    pdMS_TO_TICKS(NMEA_TPL_SAVE_DEBOUNCE_MS),
                                    pdFALSE,
                                    NULL,
                                    nmea_templates_save_timer_cb_);
        if (!s_save_timer) {
            ESP_LOGE(TAG, "save timer create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nmea_templates_load();
    nmea_templates_sanitize_();
    return err;
}

esp_err_t nmea_templates_load(void)
{
    void *handle;
    union {
        nmea_templates_blob_t current;
        nmea_templates_blob_v1_t v1;
    } stored = {0};
    size_t len = sizeof(stored);
    esp_err_t err = nvs_rw_open_ro(NMEA_TPL_NAMESPACE, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "templates not found in NVS, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open load failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_rw_read_blob(handle, NMEA_TPL_KEY, &stored, &len);
    nvs_rw_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "template blob not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        return err;
    }
    if (len == sizeof(stored.current) &&
        stored.current.version == NMEA_TPL_VERSION) {
        g_nmea_gps = stored.current.gps;
        g_nmea_gyro = stored.current.gyro;
        g_nmea_log = stored.current.log;
        g_nmea_echo = stored.current.echo;
        g_nmea_weather = stored.current.weather;
        s_gyro_revision++;
        nmea_templates_sanitize_();
        ESP_LOGI(TAG, "templates loaded from NVS");
        nmea_templates_log_summary_("load");
        return ESP_OK;
    }

    if (len == sizeof(stored.v1) && stored.v1.version == 1u) {
        nmea_templates_load_v1_(&stored.v1);
        nmea_templates_sanitize_();
        ESP_LOGI(TAG, "templates migrated from NVS v1 to v%u", NMEA_TPL_VERSION);
        nmea_templates_log_summary_("migrate");
        err = nmea_templates_save_internal_();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "migrated templates could not be persisted: %s",
                     esp_err_to_name(err));
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "template blob mismatch len=%u ver=%u",
             (unsigned)len, (unsigned)stored.current.version);
    return ESP_OK;
}

static void nmea_templates_load_v1_(const nmea_templates_blob_v1_t *old)
{
    if (!old) return;

    g_nmea_gps = old->gps;
    g_nmea_gyro = (nmea_gyro_t) {
        .heading_true_deg = old->gyro.heading_true_deg,
        .heading_mag_deg = old->gyro.heading_mag_deg,
        .deviation_deg = old->gyro.deviation_deg,
        .dev_dir = old->gyro.dev_dir,
        .variation_deg = old->gyro.variation_deg,
        .var_dir = old->gyro.var_dir,
        .talker_id = { old->gyro.talker_id[0], old->gyro.talker_id[1], '\0' },
        .prefix = old->gyro.prefix,
        .send_hdg = old->gyro.send_hdg,
        .send_hdt = old->gyro.send_hdt,
        .send_hdm = old->gyro.send_hdm,
        .add_crc = old->gyro.add_crc,
        .rate_hz = old->gyro.rate_hz,
        .rot_deg_min = 0.0f,
        .rot_status = 'A',
        .send_rot = false,
        .follow_rot = false,
    };
    g_nmea_log = old->log;
    g_nmea_echo = old->echo;
    g_nmea_weather = old->weather;
    s_gyro_revision++;
}

esp_err_t nmea_templates_save_now(void)
{
    if (s_save_timer) {
        (void)xTimerStop(s_save_timer, 0);
    }
    return nmea_templates_save_internal_();
}

void nmea_templates_schedule_save(void)
{
    if (!s_save_timer) {
        return;
    }
    (void)xTimerReset(s_save_timer, 0);
}

static void nmea_templates_save_timer_cb_(TimerHandle_t timer)
{
    (void)timer;
    (void)nmea_templates_save_internal_();
}

static esp_err_t nmea_templates_save_internal_(void)
{
    void *handle;
    nmea_templates_blob_t blob = {
        .version = NMEA_TPL_VERSION,
        .log = g_nmea_log,
        .echo = g_nmea_echo,
        .weather = g_nmea_weather,
    };
    nmea_templates_gps_snapshot(&blob.gps);
    nmea_templates_gyro_snapshot(&blob.gyro);
    esp_err_t err = nvs_rw_open_rw(NMEA_TPL_NAMESPACE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open save failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_rw_write_blob(handle, NMEA_TPL_KEY, &blob, sizeof(blob));
    nvs_rw_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "templates saved to NVS");
    nmea_templates_log_summary_("save");
    return ESP_OK;
}

static void nmea_templates_log_summary_(const char *prefix)
{
    nmea_gps_t gps;
    nmea_gyro_t gyro;
    nmea_templates_gps_snapshot(&gps);
    nmea_templates_gyro_snapshot(&gyro);
    ESP_LOGI(TAG,
             "%s gps{time=%s date=%s lat=%s%c lon=%s%c sog=%.1f cog=%.1f} "
             "gyro{hdt=%.1f rot=%.1f status=%c follow=%d} "
             "log{spd=%.1f} echo{depth=%.1f} wx{wind=%.1f/%.1f temp=%.1f}",
             prefix,
             gps.time_utc, gps.date_dmy,
             gps.lat, gps.lat_dir,
             gps.lon, gps.lon_dir,
             gps.sog_kn, gps.cog_deg,
             gyro.heading_true_deg,
             gyro.rot_deg_min,
             gyro.rot_status,
             gyro.follow_rot ? 1 : 0,
             g_nmea_log.water_speed_kn,
             g_nmea_echo.depth_m,
             g_nmea_weather.wind_angle_rel_deg,
             g_nmea_weather.wind_speed_kn,
             g_nmea_weather.water_temp_C);
}

static void nmea_templates_sanitize_(void)
{
    g_nmea_gps.time_utc[NMEA_TXT6 - 1] = '\0';
    g_nmea_gps.date_dmy[NMEA_TXT6 - 1] = '\0';
    g_nmea_gps.lat[NMEA_LATSTR - 1] = '\0';
    g_nmea_gps.lon[NMEA_LONSTR - 1] = '\0';
    g_nmea_gps.talker_id[2] = '\0';
    g_nmea_gyro.talker_id[2] = '\0';
    g_nmea_log.talker_id[2] = '\0';
    g_nmea_echo.talker_id[2] = '\0';
    g_nmea_weather.talker_id[2] = '\0';

    if (g_nmea_gps.prefix != '$' && g_nmea_gps.prefix != '!') g_nmea_gps.prefix = '$';
    if (g_nmea_gyro.prefix != '$' && g_nmea_gyro.prefix != '!') g_nmea_gyro.prefix = '$';
    if (g_nmea_log.prefix != '$' && g_nmea_log.prefix != '!') g_nmea_log.prefix = '$';
    if (g_nmea_echo.prefix != '$' && g_nmea_echo.prefix != '!') g_nmea_echo.prefix = '$';
    if (g_nmea_weather.prefix != '$' && g_nmea_weather.prefix != '!') g_nmea_weather.prefix = '$';

    if (!isfinite(g_nmea_gps.rate_hz) || g_nmea_gps.rate_hz < 0.5f || g_nmea_gps.rate_hz > 10.0f)
        g_nmea_gps.rate_hz = 1.0f;
    if (!isfinite(g_nmea_gyro.rate_hz) || g_nmea_gyro.rate_hz < 0.5f || g_nmea_gyro.rate_hz > 10.0f)
        g_nmea_gyro.rate_hz = 1.0f;
    g_nmea_gyro.heading_true_deg =
        nmea_motion_heading_advance(g_nmea_gyro.heading_true_deg, 0.0f, 0u);
    g_nmea_gyro.heading_mag_deg =
        nmea_motion_heading_advance(g_nmea_gyro.heading_mag_deg, 0.0f, 0u);
    if (!isfinite(g_nmea_gyro.deviation_deg)) g_nmea_gyro.deviation_deg = 0.0f;
    if (!isfinite(g_nmea_gyro.variation_deg)) g_nmea_gyro.variation_deg = 0.0f;
    if (!isfinite(g_nmea_gyro.rot_deg_min)) g_nmea_gyro.rot_deg_min = 0.0f;
    if (!isfinite(g_nmea_log.rate_hz) || g_nmea_log.rate_hz < 0.5f || g_nmea_log.rate_hz > 10.0f)
        g_nmea_log.rate_hz = 1.0f;
    if (!isfinite(g_nmea_echo.rate_hz) || g_nmea_echo.rate_hz < 0.5f || g_nmea_echo.rate_hz > 10.0f)
        g_nmea_echo.rate_hz = 1.0f;
    if (!isfinite(g_nmea_weather.rate_hz) || g_nmea_weather.rate_hz < 0.5f || g_nmea_weather.rate_hz > 10.0f)
        g_nmea_weather.rate_hz = 1.0f;

    if (g_nmea_gps.lat_dir != 'N' && g_nmea_gps.lat_dir != 'S') g_nmea_gps.lat_dir = 'N';
    if (g_nmea_gps.lon_dir != 'E' && g_nmea_gps.lon_dir != 'W') g_nmea_gps.lon_dir = 'E';
    if (g_nmea_gps.fix != 'A' && g_nmea_gps.fix != 'V') g_nmea_gps.fix = 'V';
    if (g_nmea_gyro.dev_dir != 'E' && g_nmea_gyro.dev_dir != 'W') g_nmea_gyro.dev_dir = 'E';
    if (g_nmea_gyro.var_dir != 'E' && g_nmea_gyro.var_dir != 'W') g_nmea_gyro.var_dir = 'E';
    if (g_nmea_gyro.rot_status != 'A' && g_nmea_gyro.rot_status != 'V') {
        g_nmea_gyro.rot_status = 'V';
    }
    if (g_nmea_weather.rel_ref != 'R' && g_nmea_weather.rel_ref != 'T') {
        g_nmea_weather.rel_ref = 'R';
    }
    if (g_nmea_weather.wind_side != 'L' && g_nmea_weather.wind_side != 'R') {
        /* This byte occupied structure padding in version 1 blobs. */
        g_nmea_weather.wind_side = 'R';
    }
    if (strcmp(g_nmea_weather.talker_id, "WI") != 0 &&
        strcmp(g_nmea_weather.talker_id, "WV") != 0 &&
        strcmp(g_nmea_weather.talker_id, "YX") != 0) {
        strlcpy(g_nmea_weather.talker_id, "WI", sizeof(g_nmea_weather.talker_id));
    }
}
