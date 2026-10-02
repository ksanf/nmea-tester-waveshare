#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config_nmea_tester.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_regen.h"
#include "nmea_editor/nmea_motion.h"
#include "nmea_editor/nmea_version.h"

nmea_gps_t g_nmea_gps = {
    .time_utc = "123519",
    .date_dmy = "230394",
    .lat = "4807.0380",
    .lon = "01131.0000",
    .lat_dir = 'N',
    .lon_dir = 'E',
    .sog_kn = 22.4f,
    .cog_deg = 84.4f,
    .track_true_deg = 84.4f,
    .fix = 'A',
    .sats = 8,
    .talker_id = "GP",
    .prefix = '$',
    .send_rmc = true,
    .send_gga = true,
    .send_zda = true,
    .send_gll = true,
    .send_vtg = true,
    .add_crc = true,
    .rate_hz = 1.0f,
};

void nmea_templates_gps_snapshot(nmea_gps_t *out)
{
    if (out) *out = g_nmea_gps;
}

nmea_gyro_t g_nmea_gyro = {
    .heading_true_deg = 26.8f,
    .heading_mag_deg = 25.6f,
    .deviation_deg = 1.2f,
    .dev_dir = 'E',
    .variation_deg = 5.0f,
    .var_dir = 'W',
    .talker_id = "HE",
    .prefix = '$',
    .send_hdg = true,
    .send_hdt = true,
    .send_hdm = true,
    .add_crc = true,
    .rate_hz = 1.0f,
    .rot_deg_min = -12.3f,
    .rot_status = 'A',
    .send_rot = true,
    .follow_rot = true,
};

void nmea_templates_gyro_snapshot(nmea_gyro_t *out)
{
    if (out) *out = g_nmea_gyro;
}

nmea_log_t g_nmea_log = {
    .hdg_water_true_deg = 27.1f,
    .hdg_water_mag_deg = 25.6f,
    .water_speed_kn = 6.7f,
    .water_speed_kph = 12.4f,
    .dist_total_nm = 100.0f,
    .dist_trip_nm = 4.2f,
    .stern_speed_kn = 0.3f,
    .talker_id = "VW",
    .prefix = '$',
    .send_vhw = true,
    .send_vlw = true,
    .send_vbw = true,
    .add_crc = true,
    .rate_hz = 1.0f,
};

nmea_echo_t g_nmea_echo = {
    .depth_ft = 19.0f,
    .depth_m = 5.8f,
    .depth_fathom = 3.2f,
    .keel_offset_m = 1.2f,
    .talker_id = "SD",
    .prefix = '$',
    .send_dbt = true,
    .send_dpt = true,
    .send_dbk = true,
    .send_dbs = true,
    .add_crc = true,
    .rate_hz = 1.0f,
};

nmea_weather_t g_nmea_weather = {
    .wind_dir_true_deg = 90.0f,
    .wind_dir_mag_deg = 85.0f,
    .wind_speed_kn = 12.5f,
    .wind_speed_ms = 6.4f,
    .wind_speed_kph = 23.1f,
    .wind_angle_rel_deg = 45.0f,
    .rel_ref = 'R',
    .wind_side = 'R',
    .water_temp_C = 16.3f,
    .talker_id = "WI",
    .prefix = '$',
    .send_mwd = true,
    .send_mwv = true,
    .send_vwr = true,
    .send_vwt = true,
    .send_mtw = true,
    .add_crc = true,
    .rate_hz = 1.0f,
};

static unsigned checksum_(const char *sentence)
{
    unsigned checksum = 0;

    for (const unsigned char *p = (const unsigned char *)sentence + 1; *p; ++p) {
        checksum ^= *p;
    }
    return checksum;
}

static void fail_(const char *group, unsigned index,
                  const char *expected, const char *actual)
{
    fprintf(stderr, "%s[%u]\n  expected: %s\n  actual:   %s\n",
            group, index, expected, actual);
    exit(EXIT_FAILURE);
}

static void expect_group_(const char *group,
                          char lines[][NMEA_SENT_MAX], uint8_t count,
                          const char *const *bodies, size_t expected_count)
{
    if (count != expected_count) {
        fprintf(stderr, "%s count: expected %zu, actual %u\n",
                group, expected_count, (unsigned)count);
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < expected_count; ++i) {
        char expected[NMEA_SENT_MAX];
        snprintf(expected, sizeof(expected), "%s*%02X",
                 bodies[i], checksum_(bodies[i]));

        if (strcmp(lines[i], expected) != 0) {
            fail_(group, (unsigned)i, expected, lines[i]);
        }
        if (lines[i][0] != '$' || lines[i][6] != ',') {
            fail_(group, (unsigned)i, "standard header followed by comma", lines[i]);
        }
        if (strlen(lines[i]) + 2u > NMEA0183_WIRE_MAX) {
            fail_(group, (unsigned)i, "NMEA 0183 wire-length limit", lines[i]);
        }
    }
}

static void test_all_sentences_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;

    static const char *const gps[] = {
        "$GPRMC,123519,A,4807.0380,N,01131.0000,E,22.4,84.4,230394,,,A",
        "$GPGGA,123519,4807.0380,N,01131.0000,E,1,08,1.0,0.0,M,0.0,M,,",
        "$GPZDA,123519,23,03,2094,,",
        "$GPGLL,4807.0380,N,01131.0000,E,123519,A,A",
        "$GPVTG,84.4,T,,M,22.4,N,41.5,K,A",
    };
    regen_gps(lines, &count);
    expect_group_("GPS", lines, count, gps, sizeof(gps) / sizeof(gps[0]));

    static const char *const gyro[] = {
        "$HEHDG,25.6,1.2,E,5.0,W",
        "$HEHDM,25.6,M",
        "$HEHDT,26.8,T",
        "$HEROT,-12.3,A",
    };
    regen_gyro(lines, &count);
    expect_group_("GYRO", lines, count, gyro, sizeof(gyro) / sizeof(gyro[0]));

    static const char *const log[] = {
        "$VWVLW,100.0,N,4.2,N,,N,,N",
        "$VWVBW,6.7,0.0,A,,,V,0.3,A,,V",
        "$VWVHW,27.1,T,25.6,M,6.7,N,12.4,K",
    };
    regen_log(lines, &count);
    expect_group_("LOG", lines, count, log, sizeof(log) / sizeof(log[0]));

    static const char *const echo[] = {
        "$SDDPT,5.8,1.2",
        "$SDDBT,19.0,f,5.8,M,3.2,F",
        "$SDDBS,19.0,f,5.8,M,3.2,F",
        "$SDDBK,15.1,f,4.6,M,2.5,F",
    };
    regen_echo(lines, &count);
    expect_group_("ECHO", lines, count, echo, sizeof(echo) / sizeof(echo[0]));

    static const char *const weather[] = {
        "$WIMWD,90.0,T,85.0,M,12.5,N,6.4,M",
        "$WIMWV,45.0,R,12.5,N,A",
        "$WIVWR,45.0,R,12.5,N,6.4,M,23.1,K",
        "$WIVWT,45.0,R,12.5,N,6.4,M,23.1,K",
        "$WIMTW,16.3,C",
    };
    regen_weather(lines, &count);
    expect_group_("WEATHER", lines, count, weather,
                  sizeof(weather) / sizeof(weather[0]));
}

static void test_invalid_fix_modes_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    const char old_fix = g_nmea_gps.fix;

    g_nmea_gps.fix = 'V';
    static const char *const expected[] = {
        "$GPRMC,123519,V,4807.0380,N,01131.0000,E,22.4,84.4,230394,,,N",
        "$GPGGA,123519,4807.0380,N,01131.0000,E,0,08,1.0,0.0,M,0.0,M,,",
        "$GPZDA,123519,23,03,2094,,",
        "$GPGLL,4807.0380,N,01131.0000,E,123519,V,N",
        "$GPVTG,84.4,T,,M,22.4,N,41.5,K,N",
    };
    regen_gps(lines, &count);
    expect_group_("GPS invalid", lines, count, expected,
                  sizeof(expected) / sizeof(expected[0]));
    g_nmea_gps.fix = old_fix;
}

static void test_wind_side_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    const char old_side = g_nmea_weather.wind_side;

    g_nmea_weather.wind_side = 'L';
    regen_weather(lines, &count);
    if (!strstr(lines[2], "VWR,45.0,L,") || !strstr(lines[3], "VWT,45.0,L,")) {
        fail_("WEATHER side", 0, "L in VWR and VWT", lines[2]);
    }
    g_nmea_weather.wind_side = old_side;
}

static void test_optional_checksum_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    const bool old_crc = g_nmea_gyro.add_crc;

    g_nmea_gyro.add_crc = false;
    regen_gyro(lines, &count);
    if (strcmp(lines[2], "$HEHDT,26.8,T") != 0) {
        fail_("GYRO no checksum", 2, "$HEHDT,26.8,T", lines[2]);
    }
    if (strcmp(lines[3], "$HEROT,-12.3,A") != 0) {
        fail_("ROT no checksum", 3, "$HEROT,-12.3,A", lines[3]);
    }
    g_nmea_gyro.add_crc = old_crc;
}

static void test_rot_status_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    const char old_status = g_nmea_gyro.rot_status;

    g_nmea_gyro.rot_status = 'V';
    regen_gyro(lines, &count);
    if (count != 4u || !strstr(lines[3], "$HEROT,-12.3,V*")) {
        fail_("ROT invalid status", 3, "$HEROT,-12.3,V*hh", lines[3]);
    }
    g_nmea_gyro.rot_status = old_status;
}

static void expect_heading_(const char *name, float actual, float expected)
{
    if (fabsf(actual - expected) > 0.001f) {
        fprintf(stderr, "%s: expected %.3f, actual %.3f\n",
                name, expected, actual);
        exit(EXIT_FAILURE);
    }
}

static void test_rot_motion_(void)
{
    expect_heading_("ROT starboard wrap",
                    nmea_motion_heading_advance(359.0f, 60.0f, 2000u), 1.0f);
    expect_heading_("ROT port wrap",
                    nmea_motion_heading_advance(1.0f, -60.0f, 2000u), 359.0f);
    expect_heading_("ROT elapsed time",
                    nmea_motion_heading_advance(90.0f, 30.0f, 60000u), 120.0f);
    expect_heading_("ROT invalid rate",
                    nmea_motion_heading_advance(45.0f, NAN, 60000u), 45.0f);
}

static void test_overlength_sentence_is_rejected_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    const nmea_weather_t saved = g_nmea_weather;

    g_nmea_weather.send_mwd = true;
    g_nmea_weather.send_mwv = false;
    g_nmea_weather.send_vwr = false;
    g_nmea_weather.send_vwt = false;
    g_nmea_weather.send_mtw = false;
    g_nmea_weather.wind_dir_true_deg = FLT_MAX;
    g_nmea_weather.wind_dir_mag_deg = FLT_MAX;
    g_nmea_weather.wind_speed_kn = FLT_MAX;
    g_nmea_weather.wind_speed_ms = FLT_MAX;

    regen_weather(lines, &count);
    if (count != 0) {
        fail_("WEATHER length", 0, "overlength sentence rejected", lines[0]);
    }
    g_nmea_weather = saved;
}

static void expect_gyro_profile_(const char *group, bool use_ths)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;
    static const char *const gyro_hdt[] = {
        "$HEHDG,25.6,1.2,E,5.0,W",
        "$HEHDM,25.6,M",
        "$HEHDT,26.8,T",
        "$HEROT,-12.3,A",
    };
    static const char *const gyro_ths[] = {
        "$HEHDG,25.6,1.2,E,5.0,W",
        "$HEHDM,25.6,M",
        "$HETHS,26.8,S",
        "$HEROT,-12.3,A",
    };

    regen_gyro(lines, &count);
    expect_group_(group, lines, count,
                  use_ths ? gyro_ths : gyro_hdt,
                  sizeof(gyro_hdt) / sizeof(gyro_hdt[0]));
}

static void test_version_profiles_(void)
{
    char lines[MAX_LINES_IN_GRP][NMEA_SENT_MAX];
    uint8_t count = 0;

    static const char *const gps_21[] = {
        "$GPRMC,123519,A,4807.0380,N,01131.0000,E,22.4,84.4,230394,,",
        "$GPGGA,123519,4807.0380,N,01131.0000,E,1,08,1.0,0.0,M,0.0,M,,",
        "$GPZDA,123519,23,03,2094,,",
        "$GPGLL,4807.0380,N,01131.0000,E,123519,A",
        "$GPVTG,84.4,T,,M,22.4,N,41.5,K",
    };
    if (!nmea_version_set_runtime(NMEA_VERSION_2_1)) exit(EXIT_FAILURE);
    regen_gps(lines, &count);
    expect_group_("GPS 2.1", lines, count, gps_21,
                  sizeof(gps_21) / sizeof(gps_21[0]));
    expect_gyro_profile_("GYRO 2.1", false);

    static const char *const gps_23_40[] = {
        "$GPRMC,123519,A,4807.0380,N,01131.0000,E,22.4,84.4,230394,,,A",
        "$GPGGA,123519,4807.0380,N,01131.0000,E,1,08,1.0,0.0,M,0.0,M,,",
        "$GPZDA,123519,23,03,2094,,",
        "$GPGLL,4807.0380,N,01131.0000,E,123519,A,A",
        "$GPVTG,84.4,T,,M,22.4,N,41.5,K,A",
    };
    if (!nmea_version_set_runtime(NMEA_VERSION_2_3)) exit(EXIT_FAILURE);
    regen_gps(lines, &count);
    expect_group_("GPS 2.3", lines, count, gps_23_40,
                  sizeof(gps_23_40) / sizeof(gps_23_40[0]));
    expect_gyro_profile_("GYRO 2.3", false);

    if (!nmea_version_set_runtime(NMEA_VERSION_4_0)) exit(EXIT_FAILURE);
    regen_gps(lines, &count);
    expect_group_("GPS 4.0", lines, count, gps_23_40,
                  sizeof(gps_23_40) / sizeof(gps_23_40[0]));
    expect_gyro_profile_("GYRO 4.0", false);

    static const char *const gps_410[] = {
        "$GPRMC,123519,A,4807.0380,N,01131.0000,E,22.4,84.4,230394,,,A,V",
        "$GPGGA,123519,4807.0380,N,01131.0000,E,1,08,1.0,0.0,M,0.0,M,,",
        "$GPZDA,123519,23,03,2094,,",
        "$GPGLL,4807.0380,N,01131.0000,E,123519,A,A",
        "$GPVTG,84.4,T,,M,22.4,N,41.5,K,A",
    };
    if (!nmea_version_set_runtime(NMEA_VERSION_4_10)) exit(EXIT_FAILURE);
    regen_gps(lines, &count);
    expect_group_("GPS 4.10", lines, count, gps_410,
                  sizeof(gps_410) / sizeof(gps_410[0]));
    expect_gyro_profile_("GYRO 4.10", false);

    if (!nmea_version_set_runtime(NMEA_VERSION_4_11)) exit(EXIT_FAILURE);
    regen_gps(lines, &count);
    expect_group_("GPS 4.11", lines, count, gps_410,
                  sizeof(gps_410) / sizeof(gps_410[0]));
    expect_gyro_profile_("GYRO 4.11", true);

    if (nmea_version_set_runtime((nmea_version_t)NMEA_VERSION_COUNT)) {
        fputs("invalid NMEA profile accepted\n", stderr);
        exit(EXIT_FAILURE);
    }
    if (nmea_version_get() != NMEA_VERSION_4_11) {
        fputs("invalid NMEA profile changed active profile\n", stderr);
        exit(EXIT_FAILURE);
    }

    if (!nmea_version_set_runtime(NMEA_VERSION_2_3)) exit(EXIT_FAILURE);
}

int main(void)
{
    if (!nmea_version_set_runtime(NMEA_VERSION_2_3)) return EXIT_FAILURE;
    test_all_sentences_();
    test_invalid_fix_modes_();
    test_wind_side_();
    test_optional_checksum_();
    test_rot_status_();
    test_rot_motion_();
    test_overlength_sentence_is_rejected_();
    test_version_profiles_();
    puts("NMEA regeneration tests passed (21 formats, 5 profiles, ROT motion)");
    return EXIT_SUCCESS;
}

void nmea_templates_log_snapshot(nmea_log_t *out) { *out = g_nmea_log; }
void nmea_templates_echo_snapshot(nmea_echo_t *out) { *out = g_nmea_echo; }
void nmea_templates_weather_snapshot(nmea_weather_t *out) { *out = g_nmea_weather; }
