/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "system/nmea_clock.h"
#include "nmea_editor/nmea_templates.h"
#include "config/config_nmea_tester.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_CLOCK
#include "config_logs.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "nmea_clock";

#if NMEA_CLOCK_FROM_RTC
    #include "system/rtc_driver.h" /* esp_err_t rtc_get_time_tm(struct tm *tm); */
#endif

/* ─── Helper: two digits with a leading zero ───────────────────────── */
static inline void fmt2(char *d, int v)
{
    d[0] = '0' + (v / 10);
    d[1] = '0' + (v % 10);
}

/* ─── ASCII to integer (two digits) ────────────────────────────────── */
static inline int dec2(const char *p)
{
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static bool six_digits_(const char *s)
{
    if (!s || s[6] != '\0') return false;
    for (size_t i = 0; i < 6; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static int days_in_month_(int year, int month)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31 };
    int result;

    if (month < 1 || month > 12) return 0;
    result = days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        result = 29;
    }
    return result;
}

/* ─── Read UTC from the selected source ────────────────────────────── */
static void get_utc_tm(struct tm *tm_out)
{
#if NMEA_CLOCK_FROM_RTC
    if (rtc_get_time_tm(tm_out) == ESP_OK) return;
    ESP_LOGW(TAG, "RTC read failed, falling back to CPU");
#endif
    time_t now = time(NULL);
    gmtime_r(&now, tm_out);
}

/* ─── Write UTC back to the template ───────────────────────────────── */
static void update_template(const struct tm *tm)
{
    char time_utc[NMEA_TXT6];
    char date_dmy[NMEA_TXT6];

    /* HHMMSS */
    fmt2(&time_utc[0], tm->tm_hour);
    fmt2(&time_utc[2], tm->tm_min);
    fmt2(&time_utc[4], tm->tm_sec);
    time_utc[6] = '\0';
    /* DDMMYY */
    fmt2(&date_dmy[0], tm->tm_mday);
    fmt2(&date_dmy[2], tm->tm_mon + 1);
    fmt2(&date_dmy[4], tm->tm_year % 100);
    date_dmy[6] = '\0';
    nmea_templates_gps_time_update(time_utc, date_dmy);
    /* Mark the GPS group dirty when the update mechanism is enabled. */
    /* groups[GRP_GPS].dirty = true; */
}

/* ─── Convert template fields to struct tm; return false if invalid ── */
static bool template_to_tm(struct tm *tm_out)
{
    char time_utc[NMEA_TXT6];
    char date_dmy[NMEA_TXT6];
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;

    if (!tm_out) return false;
    nmea_templates_gps_time_snapshot(time_utc, date_dmy);
    if (!six_digits_(time_utc) || !six_digits_(date_dmy)) return false;

    hour = dec2(&time_utc[0]);
    minute = dec2(&time_utc[2]);
    second = dec2(&time_utc[4]);
    day = dec2(&date_dmy[0]);
    month = dec2(&date_dmy[2]);
    year = 2000 + dec2(&date_dmy[4]);
    if (hour > 23 || minute > 59 || second > 59 || month < 1 || month > 12 ||
        day < 1 || day > days_in_month_(year, month)) {
        ESP_LOGW(TAG, "Invalid template date/time: %s %s",
                 date_dmy, time_utc);
        return false;
    }

    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_sec = second;
    tm_out->tm_min = minute;
    tm_out->tm_hour = hour;
    tm_out->tm_mday = day;
    tm_out->tm_mon = month - 1; /* 0…11 */
    tm_out->tm_year = year - 1900;
    tm_out->tm_isdst = 0;
    return true;
}

/* ─── One shot: apply the template to the system clock ─────────────── */
bool nmea_clock_sync_from_template(void)
{
#if NMEA_CLOCK_FROM_RTC
    /* Route the update to the driver when using the hardware RTC. */
    struct tm tm_rtc;
    if (!template_to_tm(&tm_rtc)) return false;
    time_t epoch = mktime(&tm_rtc);
    if (epoch == (time_t)-1) return false;
    return rtc_set_time_epoch(epoch) == ESP_OK;
#else
    char time_utc[NMEA_TXT6];
    char date_dmy[NMEA_TXT6];
    struct tm t;
    if (!template_to_tm(&t)) return false;
    time_t epoch = mktime(&t); /* Interpret as UTC. */
    if (epoch == (time_t)-1) return false;
    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, NULL);
    nmea_templates_gps_time_snapshot(time_utc, date_dmy);
    ESP_LOGI(TAG, "System clock set from template → %s %s",
             date_dmy, time_utc);
    return true;
#endif
}

/* ─── Periodic task ────────────────────────────────────────────────── */
static void clock_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Clock source: %s",
             NMEA_CLOCK_FROM_RTC ? "External RTC" : "CPU/SNTP");
    TickType_t tick = xTaskGetTickCount();
    while (true) {
        struct tm tm_utc;
        get_utc_tm(&tm_utc);
        update_template(&tm_utc);
        vTaskDelayUntil(&tick, pdMS_TO_TICKS(1000));
    }
}

/* ─── public API ───────────────────────────────────────────────────── */
void nmea_clock_init(void)
{
#if NMEA_CLOCK_FROM_RTC
    /* 1. Init RTC first — we need to talk to it before deciding
     *    whether to trust the template or the battery-backed time. */
    if (app_rtc_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed, using CPU clock");
    }

    struct tm tm_rtc;
    if (rtc_get_time_tm(&tm_rtc) == ESP_OK) {
        /* 2. RTC kept time on battery — template follows the hardware. */
        update_template(&tm_rtc);
        ESP_LOGI(TAG, "Template updated from RTC: %s %s",
                 g_nmea_gps.date_dmy, g_nmea_gps.time_utc);
    } else {
        /* 3. RTC lost power (OS flag set or read error).
         *    Use the stored template as initial time and seed the RTC. */
        ESP_LOGW(TAG, "RTC time invalid, seeding from template");
        if (nmea_clock_sync_from_template()) {
            ESP_LOGI(TAG, "RTC seeded from stored template: %s %s",
                     g_nmea_gps.date_dmy, g_nmea_gps.time_utc);
        }
    }
#else
    /* CPU/SNTP mode: template → system clock (original behaviour). */
    if (nmea_clock_sync_from_template()) {
        ESP_LOGI(TAG, "Clock initialized from stored template: %s %s",
                 g_nmea_gps.date_dmy, g_nmea_gps.time_utc);
    }
#endif
    if (xTaskCreatePinnedToCore(clock_task, "nmea_clock",
                               4096, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create clock task");
    }
}
