/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAVIGATION_STALE_MS 5000U

typedef enum {
    NAVIGATION_COORDS = 1U << 0,
    NAVIGATION_TIME = 1U << 1,
    NAVIGATION_DATE = 1U << 2,
    NAVIGATION_GPS_MOTION = 1U << 3,
    NAVIGATION_GYRO = 1U << 4,
    NAVIGATION_LOG = 1U << 5,
    NAVIGATION_DEPTH = 1U << 6,
    NAVIGATION_WIND = 1U << 7,
    NAVIGATION_TEMPERATURE = 1U << 8,
    NAVIGATION_ALL_FIELDS = (1U << 9) - 1U
} navigation_field_t;

/* Values retain their last observation after expiry. Coordinates are absolute
 * degrees plus hemisphere (as on the local panel); speeds are knots. A field
 * is usable only when its valid bit is set and its stale bit is clear.
 * Timestamps use monotonic milliseconds, wrapping as uint32_t. */
typedef struct {
    float lat; char lat_dir;
    float lon; char lon_dir;
    uint32_t coords_ts;
    char time[7];                 /* HHMMSS UTC */
    char date[11];                /* DD-MM-YYYY */
    uint32_t time_ts, date_ts;
    float gps_cog, gps_sog;
    uint32_t gps_hdgspd_ts;
    float gyro_heading; bool has_gyro_heading;
    uint32_t gyro_ts;
    float log_speed; bool has_log_speed;
    uint32_t log_ts;
    float depth; bool has_depth;
    uint32_t depth_ts;
    float wind_angle, wind_speed; bool wind_true;
    uint32_t weather_ts;
    float temp_c;
    uint32_t temp_ts;
    uint32_t valid_fields;
    uint32_t stale_fields;
    uint32_t revision;
    uint32_t now_ms;
} navigation_snapshot_t;

/* All model operations are thread-safe and never access LVGL. */
void navigation_model_reset(void);
void navigation_model_process(const char *line);
void navigation_model_process_at(const char *line, uint32_t now_ms);
void navigation_model_snapshot(navigation_snapshot_t *out);
void navigation_model_snapshot_at(navigation_snapshot_t *out, uint32_t now_ms);

/* Copy only fresh fields into simulator templates, preserving other settings.
 * Call from the application's serialized command/UI executor; template storage
 * has its own lifetime and synchronization. Returns NAVIGATION_* copied bits. */
uint32_t navigation_model_refresh_templates(void);

#ifdef __cplusplus
}
#endif
