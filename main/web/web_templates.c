#include "web/web_templates.h"
#include "nmea_editor/nmea_templates.h"
#include "system/nmea_clock.h"
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

typedef struct { const char *name; char type; size_t offset,size; double min,max; const char *options; } field_t;
typedef struct { const char *name; size_t offset,size; const field_t *fields; size_t count; } group_t;
static const field_t gps_fields[] = {
    { "time_utc", 's', offsetof(nmea_gps_t, time_utc), sizeof(((nmea_gps_t *)0)->time_utc), -1000000, 1000000, "" },
    { "date_dmy", 's', offsetof(nmea_gps_t, date_dmy), sizeof(((nmea_gps_t *)0)->date_dmy), -1000000, 1000000, "" },
    { "lat", 's', offsetof(nmea_gps_t, lat), sizeof(((nmea_gps_t *)0)->lat), -1000000, 1000000, "" },
    { "lon", 's', offsetof(nmea_gps_t, lon), sizeof(((nmea_gps_t *)0)->lon), -1000000, 1000000, "" },
    { "lat_dir", 'c', offsetof(nmea_gps_t, lat_dir), sizeof(((nmea_gps_t *)0)->lat_dir), -1000000, 1000000, "NS" },
    { "lon_dir", 'c', offsetof(nmea_gps_t, lon_dir), sizeof(((nmea_gps_t *)0)->lon_dir), -1000000, 1000000, "EW" },
    { "sog_kn", 'n', offsetof(nmea_gps_t, sog_kn), sizeof(((nmea_gps_t *)0)->sog_kn), 0, 100000, "" },
    { "cog_deg", 'n', offsetof(nmea_gps_t, cog_deg), sizeof(((nmea_gps_t *)0)->cog_deg), 0, 360, "" },
    { "track_true_deg", 'n', offsetof(nmea_gps_t, track_true_deg), sizeof(((nmea_gps_t *)0)->track_true_deg), 0, 360, "" },
    { "fix", 'c', offsetof(nmea_gps_t, fix), sizeof(((nmea_gps_t *)0)->fix), -1000000, 1000000, "AV" },
    { "sats", 'u', offsetof(nmea_gps_t, sats), sizeof(((nmea_gps_t *)0)->sats), 0, 99, "" },
    { "talker_id", 's', offsetof(nmea_gps_t, talker_id), sizeof(((nmea_gps_t *)0)->talker_id), -1000000, 1000000, "" },
    { "prefix", 'c', offsetof(nmea_gps_t, prefix), sizeof(((nmea_gps_t *)0)->prefix), -1000000, 1000000, "$!" },
    { "send_rmc", 'b', offsetof(nmea_gps_t, send_rmc), sizeof(((nmea_gps_t *)0)->send_rmc), -1000000, 1000000, "" },
    { "send_gga", 'b', offsetof(nmea_gps_t, send_gga), sizeof(((nmea_gps_t *)0)->send_gga), -1000000, 1000000, "" },
    { "send_zda", 'b', offsetof(nmea_gps_t, send_zda), sizeof(((nmea_gps_t *)0)->send_zda), -1000000, 1000000, "" },
    { "send_gll", 'b', offsetof(nmea_gps_t, send_gll), sizeof(((nmea_gps_t *)0)->send_gll), -1000000, 1000000, "" },
    { "send_vtg", 'b', offsetof(nmea_gps_t, send_vtg), sizeof(((nmea_gps_t *)0)->send_vtg), -1000000, 1000000, "" },
    { "add_crc", 'b', offsetof(nmea_gps_t, add_crc), sizeof(((nmea_gps_t *)0)->add_crc), -1000000, 1000000, "" },
    { "rate_hz", 'n', offsetof(nmea_gps_t, rate_hz), sizeof(((nmea_gps_t *)0)->rate_hz), 0.5, 10, "" },
};
static const field_t gyro_fields[] = {
    { "heading_true_deg", 'n', offsetof(nmea_gyro_t, heading_true_deg), sizeof(((nmea_gyro_t *)0)->heading_true_deg), 0, 360, "" },
    { "heading_mag_deg", 'n', offsetof(nmea_gyro_t, heading_mag_deg), sizeof(((nmea_gyro_t *)0)->heading_mag_deg), 0, 360, "" },
    { "deviation_deg", 'n', offsetof(nmea_gyro_t, deviation_deg), sizeof(((nmea_gyro_t *)0)->deviation_deg), -1000000, 1000000, "" },
    { "dev_dir", 'c', offsetof(nmea_gyro_t, dev_dir), sizeof(((nmea_gyro_t *)0)->dev_dir), -1000000, 1000000, "EW" },
    { "variation_deg", 'n', offsetof(nmea_gyro_t, variation_deg), sizeof(((nmea_gyro_t *)0)->variation_deg), -1000000, 1000000, "" },
    { "var_dir", 'c', offsetof(nmea_gyro_t, var_dir), sizeof(((nmea_gyro_t *)0)->var_dir), -1000000, 1000000, "EW" },
    { "talker_id", 's', offsetof(nmea_gyro_t, talker_id), sizeof(((nmea_gyro_t *)0)->talker_id), -1000000, 1000000, "" },
    { "prefix", 'c', offsetof(nmea_gyro_t, prefix), sizeof(((nmea_gyro_t *)0)->prefix), -1000000, 1000000, "$!" },
    { "send_hdg", 'b', offsetof(nmea_gyro_t, send_hdg), sizeof(((nmea_gyro_t *)0)->send_hdg), -1000000, 1000000, "" },
    { "send_hdt", 'b', offsetof(nmea_gyro_t, send_hdt), sizeof(((nmea_gyro_t *)0)->send_hdt), -1000000, 1000000, "" },
    { "send_hdm", 'b', offsetof(nmea_gyro_t, send_hdm), sizeof(((nmea_gyro_t *)0)->send_hdm), -1000000, 1000000, "" },
    { "add_crc", 'b', offsetof(nmea_gyro_t, add_crc), sizeof(((nmea_gyro_t *)0)->add_crc), -1000000, 1000000, "" },
    { "rate_hz", 'n', offsetof(nmea_gyro_t, rate_hz), sizeof(((nmea_gyro_t *)0)->rate_hz), 0.5, 10, "" },
    { "rot_deg_min", 'n', offsetof(nmea_gyro_t, rot_deg_min), sizeof(((nmea_gyro_t *)0)->rot_deg_min), -720, 720, "" },
    { "rot_status", 'c', offsetof(nmea_gyro_t, rot_status), sizeof(((nmea_gyro_t *)0)->rot_status), -1000000, 1000000, "AV" },
    { "send_rot", 'b', offsetof(nmea_gyro_t, send_rot), sizeof(((nmea_gyro_t *)0)->send_rot), -1000000, 1000000, "" },
    { "follow_rot", 'b', offsetof(nmea_gyro_t, follow_rot), sizeof(((nmea_gyro_t *)0)->follow_rot), -1000000, 1000000, "" },
};
static const field_t log_fields[] = {
    { "hdg_water_true_deg", 'n', offsetof(nmea_log_t, hdg_water_true_deg), sizeof(((nmea_log_t *)0)->hdg_water_true_deg), 0, 360, "" },
    { "hdg_water_mag_deg", 'n', offsetof(nmea_log_t, hdg_water_mag_deg), sizeof(((nmea_log_t *)0)->hdg_water_mag_deg), 0, 360, "" },
    { "water_speed_kn", 'n', offsetof(nmea_log_t, water_speed_kn), sizeof(((nmea_log_t *)0)->water_speed_kn), 0, 100000, "" },
    { "water_speed_kph", 'n', offsetof(nmea_log_t, water_speed_kph), sizeof(((nmea_log_t *)0)->water_speed_kph), 0, 100000, "" },
    { "dist_total_nm", 'n', offsetof(nmea_log_t, dist_total_nm), sizeof(((nmea_log_t *)0)->dist_total_nm), 0, 100000, "" },
    { "dist_trip_nm", 'n', offsetof(nmea_log_t, dist_trip_nm), sizeof(((nmea_log_t *)0)->dist_trip_nm), 0, 100000, "" },
    { "stern_speed_kn", 'n', offsetof(nmea_log_t, stern_speed_kn), sizeof(((nmea_log_t *)0)->stern_speed_kn), 0, 100000, "" },
    { "talker_id", 's', offsetof(nmea_log_t, talker_id), sizeof(((nmea_log_t *)0)->talker_id), -1000000, 1000000, "" },
    { "prefix", 'c', offsetof(nmea_log_t, prefix), sizeof(((nmea_log_t *)0)->prefix), -1000000, 1000000, "$!" },
    { "send_vhw", 'b', offsetof(nmea_log_t, send_vhw), sizeof(((nmea_log_t *)0)->send_vhw), -1000000, 1000000, "" },
    { "send_vlw", 'b', offsetof(nmea_log_t, send_vlw), sizeof(((nmea_log_t *)0)->send_vlw), -1000000, 1000000, "" },
    { "send_vbw", 'b', offsetof(nmea_log_t, send_vbw), sizeof(((nmea_log_t *)0)->send_vbw), -1000000, 1000000, "" },
    { "add_crc", 'b', offsetof(nmea_log_t, add_crc), sizeof(((nmea_log_t *)0)->add_crc), -1000000, 1000000, "" },
    { "rate_hz", 'n', offsetof(nmea_log_t, rate_hz), sizeof(((nmea_log_t *)0)->rate_hz), 0.5, 10, "" },
};
static const field_t echo_fields[] = {
    { "depth_ft", 'n', offsetof(nmea_echo_t, depth_ft), sizeof(((nmea_echo_t *)0)->depth_ft), 0, 100000, "" },
    { "depth_m", 'n', offsetof(nmea_echo_t, depth_m), sizeof(((nmea_echo_t *)0)->depth_m), 0, 100000, "" },
    { "depth_fathom", 'n', offsetof(nmea_echo_t, depth_fathom), sizeof(((nmea_echo_t *)0)->depth_fathom), 0, 100000, "" },
    { "keel_offset_m", 'n', offsetof(nmea_echo_t, keel_offset_m), sizeof(((nmea_echo_t *)0)->keel_offset_m), -1000000, 1000000, "" },
    { "talker_id", 's', offsetof(nmea_echo_t, talker_id), sizeof(((nmea_echo_t *)0)->talker_id), -1000000, 1000000, "" },
    { "prefix", 'c', offsetof(nmea_echo_t, prefix), sizeof(((nmea_echo_t *)0)->prefix), -1000000, 1000000, "$!" },
    { "send_dbt", 'b', offsetof(nmea_echo_t, send_dbt), sizeof(((nmea_echo_t *)0)->send_dbt), -1000000, 1000000, "" },
    { "send_dpt", 'b', offsetof(nmea_echo_t, send_dpt), sizeof(((nmea_echo_t *)0)->send_dpt), -1000000, 1000000, "" },
    { "send_dbk", 'b', offsetof(nmea_echo_t, send_dbk), sizeof(((nmea_echo_t *)0)->send_dbk), -1000000, 1000000, "" },
    { "send_dbs", 'b', offsetof(nmea_echo_t, send_dbs), sizeof(((nmea_echo_t *)0)->send_dbs), -1000000, 1000000, "" },
    { "add_crc", 'b', offsetof(nmea_echo_t, add_crc), sizeof(((nmea_echo_t *)0)->add_crc), -1000000, 1000000, "" },
    { "rate_hz", 'n', offsetof(nmea_echo_t, rate_hz), sizeof(((nmea_echo_t *)0)->rate_hz), 0.5, 10, "" },
};
static const field_t weather_fields[] = {
    { "wind_dir_true_deg", 'n', offsetof(nmea_weather_t, wind_dir_true_deg), sizeof(((nmea_weather_t *)0)->wind_dir_true_deg), 0, 360, "" },
    { "wind_dir_mag_deg", 'n', offsetof(nmea_weather_t, wind_dir_mag_deg), sizeof(((nmea_weather_t *)0)->wind_dir_mag_deg), 0, 360, "" },
    { "wind_speed_kn", 'n', offsetof(nmea_weather_t, wind_speed_kn), sizeof(((nmea_weather_t *)0)->wind_speed_kn), 0, 100000, "" },
    { "wind_speed_ms", 'n', offsetof(nmea_weather_t, wind_speed_ms), sizeof(((nmea_weather_t *)0)->wind_speed_ms), 0, 100000, "" },
    { "wind_speed_kph", 'n', offsetof(nmea_weather_t, wind_speed_kph), sizeof(((nmea_weather_t *)0)->wind_speed_kph), 0, 100000, "" },
    { "wind_angle_rel_deg", 'n', offsetof(nmea_weather_t, wind_angle_rel_deg), sizeof(((nmea_weather_t *)0)->wind_angle_rel_deg), 0, 360, "" },
    { "rel_ref", 'c', offsetof(nmea_weather_t, rel_ref), sizeof(((nmea_weather_t *)0)->rel_ref), -1000000, 1000000, "RT" },
    { "wind_side", 'c', offsetof(nmea_weather_t, wind_side), sizeof(((nmea_weather_t *)0)->wind_side), -1000000, 1000000, "LR" },
    { "water_temp_C", 'n', offsetof(nmea_weather_t, water_temp_C), sizeof(((nmea_weather_t *)0)->water_temp_C), -100, 200, "" },
    { "talker_id", 's', offsetof(nmea_weather_t, talker_id), sizeof(((nmea_weather_t *)0)->talker_id), -1000000, 1000000, "" },
    { "prefix", 'c', offsetof(nmea_weather_t, prefix), sizeof(((nmea_weather_t *)0)->prefix), -1000000, 1000000, "$!" },
    { "send_mwd", 'b', offsetof(nmea_weather_t, send_mwd), sizeof(((nmea_weather_t *)0)->send_mwd), -1000000, 1000000, "" },
    { "send_mwv", 'b', offsetof(nmea_weather_t, send_mwv), sizeof(((nmea_weather_t *)0)->send_mwv), -1000000, 1000000, "" },
    { "send_vwr", 'b', offsetof(nmea_weather_t, send_vwr), sizeof(((nmea_weather_t *)0)->send_vwr), -1000000, 1000000, "" },
    { "send_vwt", 'b', offsetof(nmea_weather_t, send_vwt), sizeof(((nmea_weather_t *)0)->send_vwt), -1000000, 1000000, "" },
    { "send_mtw", 'b', offsetof(nmea_weather_t, send_mtw), sizeof(((nmea_weather_t *)0)->send_mtw), -1000000, 1000000, "" },
    { "add_crc", 'b', offsetof(nmea_weather_t, add_crc), sizeof(((nmea_weather_t *)0)->add_crc), -1000000, 1000000, "" },
    { "rate_hz", 'n', offsetof(nmea_weather_t, rate_hz), sizeof(((nmea_weather_t *)0)->rate_hz), 0.5, 10, "" },
};

static const group_t template_groups[] = {
    { "gps", offsetof(nmea_templates_snapshot_t, gps), sizeof(nmea_gps_t), gps_fields, sizeof(gps_fields)/sizeof(field_t) },
    { "gyro", offsetof(nmea_templates_snapshot_t, gyro), sizeof(nmea_gyro_t), gyro_fields, sizeof(gyro_fields)/sizeof(field_t) },
    { "log", offsetof(nmea_templates_snapshot_t, log), sizeof(nmea_log_t), log_fields, sizeof(log_fields)/sizeof(field_t) },
    { "echo", offsetof(nmea_templates_snapshot_t, echo), sizeof(nmea_echo_t), echo_fields, sizeof(echo_fields)/sizeof(field_t) },
    { "weather", offsetof(nmea_templates_snapshot_t, weather), sizeof(nmea_weather_t), weather_fields, sizeof(weather_fields)/sizeof(field_t) },
};

int web_templates_group(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < GRP_COUNT; ++i) if (!strcmp(name, template_groups[i].name)) return i;
    return -1;
}
cJSON *web_templates_json(int group) {
    if (group < 0 || group >= GRP_COUNT) return NULL;
    nmea_templates_snapshot_t snapshot;
    nmea_templates_snapshot_all(&snapshot);
    const group_t *g = &template_groups[group];
    const uint8_t *base = (const uint8_t *)&snapshot + g->offset;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *values = cJSON_AddObjectToObject(root, "values");
    cJSON *fields = cJSON_AddArrayToObject(root, "fields");
    if (!values || !fields || !cJSON_AddStringToObject(root, "type", "template") ||
        !cJSON_AddStringToObject(root, "group", g->name)) goto fail;
    for (size_t i = 0; i < g->count; ++i) {
        const field_t *f = &g->fields[i]; const void *v = base + f->offset;
        cJSON *field = cJSON_CreateObject();
        if (!field) goto fail;
        if (!cJSON_AddItemToArray(fields, field)) { cJSON_Delete(field); goto fail; }
        char type[2] = { f->type, 0 };
        if (!cJSON_AddStringToObject(field, "name", f->name) ||
            !cJSON_AddStringToObject(field, "kind", type) ||
            !cJSON_AddNumberToObject(field, "min", f->min) ||
            !cJSON_AddNumberToObject(field, "max", f->max) ||
            !cJSON_AddNumberToObject(field, "size", f->size) ||
            !cJSON_AddStringToObject(field, "options", f->options)) goto fail;
        cJSON *value;
        if (f->type == 'b') value = cJSON_AddBoolToObject(values, f->name, *(const bool *)v);
        else if (f->type == 'n') value = cJSON_AddNumberToObject(values, f->name, *(const float *)v);
        else if (f->type == 'u') value = cJSON_AddNumberToObject(values, f->name, *(const uint8_t *)v);
        else if (f->type == 'c') { char text[2] = { *(const char *)v, 0 }; value = cJSON_AddStringToObject(values, f->name, text); }
        else value = cJSON_AddStringToObject(values, f->name, v);
        if (!value) goto fail;
    }
    return root;
fail:
    /* A partial schema could silently hide editable settings from the browser. */
    cJSON_Delete(root);
    return NULL;
}
static bool six_digits_(const char *s) {
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
static bool text_valid_(const char *name, const char *s) {
    if (!strcmp(name, "talker_id")) return strlen(s) == 2 && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= 'A' && s[1] <= 'Z';
    if (!strcmp(name, "time_utc") || !strcmp(name, "date_dmy")) return six_digits_(s);
    if (!strcmp(name, "lat") || !strcmp(name, "lon")) {
        const size_t whole = !strcmp(name, "lat") ? 4 : 5;
        if (strlen(s) < whole) return false;
        for (size_t i = 0; i < whole; ++i) if (s[i] < '0' || s[i] > '9') return false;
        if (s[whole]) {
            if (s[whole] != '.' || !s[whole + 1]) return false;
            for (const char *p = s + whole + 1; *p; ++p) if (*p < '0' || *p > '9') return false;
        }
        char *end; double v = strtod(s, &end);
        double deg = floor(v / 100), minutes = v - deg * 100;
        double max_deg = !strcmp(name, "lat") ? 90 : 180;
        return *s && !*end && isfinite(v) && v >= 0 && deg <= max_deg && minutes < 60 &&
            (deg < max_deg || minutes == 0);
    }
    return false;
}
static bool gps_epoch_(const nmea_gps_t *gps, time_t *epoch) {
    if (!six_digits_(gps->time_utc) || !six_digits_(gps->date_dmy)) return false;
    int hh, mm, ss, d, m, y;
    if (sscanf(gps->time_utc, "%2d%2d%2d", &hh,&mm,&ss) != 3 ||
        sscanf(gps->date_dmy, "%2d%2d%2d", &d,&m,&y) != 3) return false;
    if (hh>23 || mm>59 || ss>59 || d<1 || d>31 || m<1 || m>12) return false;
    /* DDMMYY represents 2000..2099. Build UTC seconds directly: mktime
     * interprets struct tm in the process timezone, while the RTC takes UTC. */
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned max_day = month_days[m - 1] + (m == 2 && y % 4 == 0);
    if ((unsigned)d > max_day) return false;
    int64_t days = 10957; /* 1970-01-01 to 2000-01-01 */
    for (int year = 0; year < y; ++year) days += 365 + (year % 4 == 0);
    for (int month = 1; month < m; ++month)
        days += month_days[month - 1] + (month == 2 && y % 4 == 0);
    days += d - 1;
    const int64_t seconds = days * 86400 + hh * 3600 + mm * 60 + ss;
    const time_t result = (time_t)seconds;
    if ((int64_t)result != seconds) return false;
    *epoch = result;
    return true;
}
esp_err_t web_templates_apply(int group, const cJSON *patch) {
    if (group < 0 || group >= GRP_COUNT || !cJSON_IsObject(patch)) return ESP_ERR_INVALID_ARG;
    if (!patch->child) return ESP_OK;
    nmea_templates_snapshot_t snapshot;
    nmea_templates_snapshot_all(&snapshot);
    const group_t *g = &template_groups[group];
    uint8_t *base = (uint8_t *)&snapshot + g->offset;
    uint8_t mask[sizeof(nmea_templates_snapshot_t)] = {0};
    bool clock_change = false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, patch) {
        if (!entry->string) return ESP_ERR_INVALID_ARG;
        const field_t *f = NULL;
        for (size_t i = 0; i < g->count; ++i) if (!strcmp(entry->string, g->fields[i].name)) { f = &g->fields[i]; break; }
        if (!f || mask[f->offset]) return ESP_ERR_INVALID_ARG; /* unknown or duplicate key */
        void *dst = base + f->offset;
        if (f->type == 'b') {
            if (!cJSON_IsBool(entry)) return ESP_ERR_INVALID_ARG;
            *(bool *)dst = cJSON_IsTrue(entry);
        } else if (f->type == 'n' || f->type == 'u') {
            if (!cJSON_IsNumber(entry) || !isfinite(entry->valuedouble) ||
                entry->valuedouble < f->min || entry->valuedouble > f->max) return ESP_ERR_INVALID_ARG;
            if (f->type == 'u') {
                if (floor(entry->valuedouble) != entry->valuedouble) return ESP_ERR_INVALID_ARG;
                *(uint8_t *)dst = (uint8_t)entry->valuedouble;
            } else *(float *)dst = (float)entry->valuedouble;
        } else {
            if (!cJSON_IsString(entry)) return ESP_ERR_INVALID_ARG;
            const char *str = entry->valuestring;
            if (f->type == 'c') {
                if (strlen(str) != 1 || !strchr(f->options, *str)) return ESP_ERR_INVALID_ARG;
                *(char *)dst = *str;
            } else {
                if (strlen(str) >= f->size || !text_valid_(f->name, str)) return ESP_ERR_INVALID_ARG;
                memset(dst, 0, f->size); memcpy(dst, str, strlen(str));
            }
        }
        memset(mask + f->offset, 1, f->size);
        if (group == GRP_GPS && (!strcmp(f->name,"time_utc") || !strcmp(f->name,"date_dmy"))) clock_change = true;
    }
    if (clock_change) {
        time_t epoch;
        if (!gps_epoch_(&snapshot.gps, &epoch)) return ESP_ERR_INVALID_ARG;
        esp_err_t err = nmea_clock_set_time_epoch(epoch);
        if (err != ESP_OK) return err;
    }
    /* The transmitter snapshots rate_hz when scheduling the next group. */
    return nmea_templates_patch(group, base, mask, g->size);
}
