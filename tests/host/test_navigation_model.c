#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "app/navigation_model.h"
#include "nmea_editor/nmea_templates.h"

nmea_gps_t g_nmea_gps;
nmea_gyro_t g_nmea_gyro;
nmea_log_t g_nmea_log;
nmea_echo_t g_nmea_echo;
nmea_weather_t g_nmea_weather;
static unsigned dirty_groups;
static unsigned clock_syncs;

void nmea_mark_dirty(int group) { dirty_groups |= 1U << group; }
void nmea_templates_gps_snapshot(nmea_gps_t *out) { *out = g_nmea_gps; }
void nmea_templates_gyro_snapshot(nmea_gyro_t *out) { *out = g_nmea_gyro; }
void nmea_templates_snapshot_all(nmea_templates_snapshot_t *out)
{
    *out = (nmea_templates_snapshot_t){g_nmea_gps, g_nmea_gyro,
                                     g_nmea_log, g_nmea_echo, g_nmea_weather};
}
void nmea_templates_gps_update_fields(const nmea_gps_t *src, uint32_t fields)
{
    if (fields & NMEA_GPS_FIELDS_COORDS) {
        memcpy(g_nmea_gps.lat, src->lat, sizeof(src->lat));
        memcpy(g_nmea_gps.lon, src->lon, sizeof(src->lon));
        g_nmea_gps.lat_dir = src->lat_dir; g_nmea_gps.lon_dir = src->lon_dir;
    }
    if (fields & NMEA_GPS_FIELDS_TIME) memcpy(g_nmea_gps.time_utc, src->time_utc, sizeof(src->time_utc));
    if (fields & NMEA_GPS_FIELDS_DATE) memcpy(g_nmea_gps.date_dmy, src->date_dmy, sizeof(src->date_dmy));
    if (fields & NMEA_GPS_FIELDS_MOTION) {
        g_nmea_gps.sog_kn = src->sog_kn; g_nmea_gps.cog_deg = src->cog_deg;
        g_nmea_gps.track_true_deg = src->track_true_deg; g_nmea_gps.fix = src->fix;
        g_nmea_gps.sats = src->sats;
    }
}
void nmea_templates_gyro_update_fields(const nmea_gyro_t *src, uint32_t fields)
{
    if (fields & NMEA_GYRO_FIELD_HEADING_TRUE) g_nmea_gyro.heading_true_deg = src->heading_true_deg;
    if (fields & NMEA_GYRO_FIELD_HEADING_MAG) g_nmea_gyro.heading_mag_deg = src->heading_mag_deg;
    if (fields & NMEA_GYRO_FIELD_DEVIATION) g_nmea_gyro.deviation_deg = src->deviation_deg;
    if (fields & NMEA_GYRO_FIELD_VARIATION) g_nmea_gyro.variation_deg = src->variation_deg;
    if (fields & NMEA_GYRO_FIELD_DEV_DIR) g_nmea_gyro.dev_dir = src->dev_dir;
    if (fields & NMEA_GYRO_FIELD_VAR_DIR) g_nmea_gyro.var_dir = src->var_dir;
}
esp_err_t nmea_templates_patch(rs485_group_t group, const void *src,
                               const uint8_t *mask, size_t size)
{
    void *dst = group == GRP_LOG ? (void *)&g_nmea_log :
                group == GRP_ECHO ? (void *)&g_nmea_echo : (void *)&g_nmea_weather;
    assert(group == GRP_LOG || group == GRP_ECHO || group == GRP_WX);
    for (size_t i = 0; i < size; ++i) if (mask[i]) ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
    nmea_mark_dirty(group);
    return 0;
}
bool nmea_clock_sync_from_template(void) { clock_syncs++; return true; }
static void near_(float a, float b) { assert(fabsf(a - b) < 0.002f); }

static void basic_(void)
{
    navigation_snapshot_t s;
    navigation_model_reset();
    navigation_model_snapshot_at(&s, 0);
    assert(s.valid_fields == 0 && s.stale_fields == NAVIGATION_ALL_FIELDS);
    navigation_model_process_at("$GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230326,,,A", 0);
    navigation_model_snapshot_at(&s, 0);
    assert((s.valid_fields & 15U) == 15U && !(s.stale_fields & 15U));
    near_(s.lat, 48.1173f); near_(s.lon, 11.5166667f);
    assert(s.lat_dir == 'N' && s.lon_dir == 'E');
    assert(strcmp(s.time, "123519") == 0 && strcmp(s.date, "23-03-2026") == 0);
    near_(s.gps_sog, 22.4f); near_(s.gps_cog, 84.4f);
    navigation_model_process_at("$HEHDT,26.8,T*13\r\n", 100);
    navigation_model_process_at("$VWVHW,20.0,T,19.0,M,3.5,N,6.5,K", 100);
    navigation_model_process_at("$SDDBT,19.0,f,5.8,M,3.2,F", 100);
    navigation_model_process_at("$WIMWV,45.0,R,10.0,M,A", 100);
    navigation_model_process_at("$WIMTW,16.3,C", 100);
    navigation_model_snapshot_at(&s, 100);
    assert(s.valid_fields == NAVIGATION_ALL_FIELDS && s.stale_fields == 0);
    near_(s.gyro_heading, 19.0f); near_(s.log_speed, 3.5f); near_(s.depth, 5.8f);
    near_(s.wind_speed, 19.438445f); assert(!s.wind_true); near_(s.temp_c, 16.3f);
    navigation_model_snapshot_at(&s, 5000);
    assert(!(s.stale_fields & NAVIGATION_COORDS));
    navigation_model_snapshot_at(&s, 5001);
    assert(s.stale_fields & NAVIGATION_COORDS);
    assert(!(s.stale_fields & NAVIGATION_GYRO));
    navigation_model_snapshot_at(&s, 5101);
    assert(s.stale_fields == NAVIGATION_ALL_FIELDS);
    near_(s.depth, 5.8f); /* retained value, not a fake fresh zero */
}

static void invalid_and_wrap_(void)
{
    navigation_snapshot_t a, b;
    navigation_model_reset();
    navigation_model_process_at("$HEHDT,26.8,T*13", UINT32_MAX - 99U);
    navigation_model_snapshot_at(&a, 50);
    assert(a.valid_fields == NAVIGATION_GYRO && !(a.stale_fields & NAVIGATION_GYRO));
    const char *bad[] = { "$HEHDT,50.0,T*00", "$HEHDT,nan,T", "$HEHDT,inf,T",
        "$HEHDT,abc,T", "$HEHDT,400.0,T", "HEHDT,20.0,T", "$GPGGA,,,,,,,", "$WIMWV,1,R,1,N,V" };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) navigation_model_process_at(bad[i], 51);
    navigation_model_snapshot_at(&b, 51);
    assert(a.revision == b.revision && b.valid_fields == NAVIGATION_GYRO);
    near_(b.gyro_heading, 26.8f);
    navigation_model_snapshot_at(&b, 5001);
    assert(b.stale_fields & NAVIGATION_GYRO);
    navigation_model_process_at("$WIVWR,30,L,8,N,4,M,15,K", 100);
    navigation_model_process_at("$GPZDA,235959,31,12,2026,,", 100);
    navigation_model_snapshot_at(&b, 100);
    near_(b.wind_angle, -30); assert(!b.wind_true);
    assert(strcmp(b.date, "31-12-2026") == 0);
}

static void refresh_(void)
{
    navigation_model_reset();
    g_nmea_gps.rate_hz = 2; g_nmea_gps.send_gga = true;
    g_nmea_gyro.follow_rot = true; g_nmea_gyro.rot_deg_min = 10;
    g_nmea_log.rate_hz = 3; g_nmea_echo.send_dbk = true;
    g_nmea_weather.rate_hz = 4; g_nmea_weather.send_mtw = true;
    dirty_groups = clock_syncs = 0;
    navigation_model_process("$GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230326,,,A");
    navigation_model_process("$HEHDT,26.8,T*13");
    navigation_model_process("$VWVBW,5.0,0,A,,,V");
    navigation_model_process("$SDDPT,7.0,0.0");
    navigation_model_process("$WIMWV,45.0,T,10.0,N,A");
    navigation_model_process("$WIMTW,16.3,C");
    assert(navigation_model_refresh_templates() == NAVIGATION_ALL_FIELDS);
    assert(dirty_groups == 31 && clock_syncs == 1);
    assert(g_nmea_gps.rate_hz == 2 && g_nmea_gps.send_gga);
    assert(g_nmea_gyro.follow_rot && g_nmea_gyro.rot_deg_min == 10);
    assert(g_nmea_log.rate_hz == 3 && g_nmea_echo.send_dbk);
    assert(g_nmea_weather.rate_hz == 4 && g_nmea_weather.send_mtw);
    near_(g_nmea_log.water_speed_kph, 9.26f);
    near_(g_nmea_echo.depth_ft, 22.96588f);
    near_(g_nmea_weather.wind_speed_kn, 10.0f);
    near_(g_nmea_weather.water_temp_C, 16.3f);
    navigation_model_reset();
    assert(navigation_model_refresh_templates() == 0);
    near_(g_nmea_echo.depth_m, 7.0f);
}

static void *gps_writer_(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 10000; i++) {
        navigation_model_process_at(i & 1U ?
            "$GPRMC,010101,A,0100,N,00200,E,1,1,010126,,,A" :
            "$GPRMC,020202,A,0200,N,00400,E,2,2,020226,,,A", 100);
    }
    return NULL;
}
static void *gyro_writer_(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 10000; i++) navigation_model_process_at("$HEHDT,45,T", 100);
    return NULL;
}
static void concurrent_(void)
{
    pthread_t gps, gyro;
    navigation_snapshot_t s;
    navigation_model_reset();
    assert(pthread_create(&gps, NULL, gps_writer_, NULL) == 0);
    assert(pthread_create(&gyro, NULL, gyro_writer_, NULL) == 0);
    for (unsigned i = 0; i < 10000; i++) {
        navigation_model_snapshot_at(&s, 100);
        if (s.valid_fields & NAVIGATION_COORDS) {
            near_(s.lon, 2 * s.lat); near_(s.gps_sog, s.gps_cog);
        }
    }
    pthread_join(gps, NULL); pthread_join(gyro, NULL);
    navigation_model_snapshot_at(&s, 100);
    assert((s.valid_fields & (NAVIGATION_COORDS | NAVIGATION_GYRO)) ==
           (NAVIGATION_COORDS | NAVIGATION_GYRO));
}
int main(void)
{
    basic_(); invalid_and_wrap_(); refresh_(); concurrent_();
    puts("Navigation model parsing, freshness, refresh and concurrency tests passed");
    return 0;
}
