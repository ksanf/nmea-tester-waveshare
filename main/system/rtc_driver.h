/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

#include <esp_err.h>
#include <time.h>

/**
 * Read the current UTC time from the RTC into struct tm.
 * @param tm Destination structure.
 * @return ESP_OK on success, or ESP_FAIL if the clock is stopped or an error occurs.
 */
esp_err_t rtc_get_time_tm(struct tm *tm);

/**
 * Set the RTC from a UNIX epoch value in UTC.
 * @param epoch Seconds since 1970-01-01.
 * @return ESP_OK on success.
 */
esp_err_t rtc_set_time_epoch(time_t epoch);
esp_err_t app_rtc_init(void);
#endif // RTC_DRIVER_H
