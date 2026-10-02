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
#include <stdint.h>
#include <stdatomic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static const char *TAG = "nmea_clock";
static atomic_bool s_cpu_time_valid;
static _Atomic(SemaphoreHandle_t) s_clock_mutex;

/* Keep one task mutex for the lifetime of the clock. Allocate before publishing
 * it; a racing initializer deletes only its unused candidate. I2C and template
 * operations run under this mutex, never inside a critical section. */
static bool clock_lock_(void)
{
    SemaphoreHandle_t mutex = atomic_load_explicit(&s_clock_mutex, memory_order_acquire);
    if (!mutex) {
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        if (!candidate) return false;
        SemaphoreHandle_t expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&s_clock_mutex, &expected, candidate,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            mutex = candidate;
        } else {
            vSemaphoreDelete(candidate);
            mutex = expected;
        }
    }
    return xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void clock_unlock_(void)
{
    xSemaphoreGive(atomic_load_explicit(&s_clock_mutex, memory_order_acquire));
}

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

/* DDMMYY represents 2000..2099. Convert UTC directly: mktime() would
 * interpret it in the process timezone and silently normalize bad dates. */
static bool utc_epoch_(const struct tm *t, time_t *out)
{
    if (!t || !out || t->tm_year < 100 || t->tm_year > 199 ||
        t->tm_mon < 0 || t->tm_mon > 11 || t->tm_mday < 1 ||
        t->tm_mday > days_in_month_(t->tm_year + 1900, t->tm_mon + 1) ||
        t->tm_hour < 0 || t->tm_hour > 23 || t->tm_min < 0 || t->tm_min > 59 ||
        t->tm_sec < 0 || t->tm_sec > 59) return false;
    int64_t days = 0;
    for (int year = 2000; year < t->tm_year + 1900; ++year)
        days += days_in_month_(year, 2) == 29 ? 366 : 365;
    for (int month = 1; month <= t->tm_mon; ++month)
        days += days_in_month_(t->tm_year + 1900, month);
    days += t->tm_mday - 1;
    *out = (time_t)(INT64_C(946684800) + days * 86400 +
                   t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
    return true;
}

static bool sync_cpu_(time_t epoch)
{
    const struct timeval tv = { .tv_sec = epoch };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "CPU clock synchronization failed");
        return false;
    }
    atomic_store_explicit(&s_cpu_time_valid, true, memory_order_release);
    return true;
}

/* Keep the CPU clock anchored to each valid RTC reading. During an I2C/RTC
 * outage it advances normally from that last reading instead of from 1970.
 * Never publish an uninitialized or out-of-range fallback as valid NMEA time. */
/* Caller holds the clock mutex through publication of this reading. */
static bool get_utc_tm(struct tm *tm_out)
{
    time_t epoch;
#if NMEA_CLOCK_FROM_RTC
    if (rtc_get_time_tm(tm_out) == ESP_OK && utc_epoch_(tm_out, &epoch)) {
        (void)sync_cpu_(epoch);
        return true;
    }
    ESP_LOGW(TAG, "RTC read failed, using last synchronized CPU clock");
#endif
    if (!atomic_load_explicit(&s_cpu_time_valid, memory_order_acquire)) return false;
    time_t now = time(NULL);
    return gmtime_r(&now, tm_out) != NULL && utc_epoch_(tm_out, &epoch);
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

/* Caller holds the clock mutex; publish only after both clocks accepted the
 * edit so an in-flight older read cannot overwrite the new template or backup. */
static esp_err_t set_epoch_locked_(time_t epoch, const struct tm *utc)
{
#if NMEA_CLOCK_FROM_RTC
    const esp_err_t err = rtc_set_time_epoch(epoch);
    if (err != ESP_OK) return err;
#endif
    if (!sync_cpu_(epoch)) return ESP_FAIL;
    update_template(utc);
    return ESP_OK;
}

esp_err_t nmea_clock_set_time_epoch(time_t epoch)
{
    struct tm utc;
    time_t checked;
    if (!gmtime_r(&epoch, &utc) || !utc_epoch_(&utc, &checked) || checked != epoch)
        return ESP_ERR_INVALID_ARG;
    if (!clock_lock_()) return ESP_ERR_NO_MEM;
    esp_err_t err = set_epoch_locked_(epoch, &utc);
    clock_unlock_();
    return err;
}

/* One-shot: apply template time through the synchronized clock setter. */
bool nmea_clock_sync_from_template(void)
{
    if (!clock_lock_()) return false;
    struct tm utc;
    time_t epoch;
    bool ok = template_to_tm(&utc) && utc_epoch_(&utc, &epoch) &&
              set_epoch_locked_(epoch, &utc) == ESP_OK;
    clock_unlock_();
    return ok;
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
        if (clock_lock_()) {
            if (get_utc_tm(&tm_utc)) update_template(&tm_utc);
            clock_unlock_();
        }
        vTaskDelayUntil(&tick, pdMS_TO_TICKS(1000));
    }
}

/* ─── public API ───────────────────────────────────────────────────── */
void nmea_clock_init(void)
{
    if (!clock_lock_()) {
        ESP_LOGE(TAG, "Clock mutex allocation failed");
        return;
    }
#if NMEA_CLOCK_FROM_RTC
    if (app_rtc_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed, using CPU clock");
    }
    struct tm initial;
    time_t epoch;
    if (rtc_get_time_tm(&initial) == ESP_OK && utc_epoch_(&initial, &epoch)) {
        (void)sync_cpu_(epoch);
        update_template(&initial);
        ESP_LOGI(TAG, "RTC time loaded; CPU backup synchronized");
    } else if (template_to_tm(&initial) && utc_epoch_(&initial, &epoch)) {
        /* Seed the backup even when a missing/broken RTC cannot be written.
         * This is a last stored time, not a claim that elapsed power-off time
         * can be reconstructed without a working battery-backed clock. */
        (void)sync_cpu_(epoch);
        if (rtc_set_time_epoch(epoch) != ESP_OK)
            ESP_LOGW(TAG, "RTC unavailable; CPU continues from stored time");
    } else {
        ESP_LOGW(TAG, "No valid RTC or stored time; clock update skipped");
    }
#else
    struct tm initial;
    time_t epoch;
    if (template_to_tm(&initial) && utc_epoch_(&initial, &epoch))
        (void)sync_cpu_(epoch);
#endif
    clock_unlock_();
    if (xTaskCreatePinnedToCore(clock_task, "nmea_clock",
                               4096, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create clock task");
    }
}
