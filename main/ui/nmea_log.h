/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester ring buffer and recolored message-log interface.
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"

/**
 * @brief Initialize the NMEA log window.
 * @param parent Parent LVGL object.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param w Window width.
 * @param h Window height.
 */
void nmea_log_init(lv_obj_t *parent, int x, int y, int w, int h);

/**
 * @brief Add a sentence using the built-in NMEA recoloring.
 * @param sentence Complete NMEA sentence ($...*hh).
 */
void nmea_log_add(const char *sentence);

/**
 * @brief Add plain text without NMEA recoloring.
 * @param text Arbitrary text.
 */
void nmea_log_add_plain(const char *text);

/** Add one raw hex dump line with per-section colors in the color theme. */
void nmea_log_add_hex_line(const char *text);

/** Clear all displayed and buffered log lines. */
void nmea_log_clear(void);

/** Select the tall condensed presentation used by raw hex dumps. */
void nmea_log_set_raw_mode(bool enabled);

/** Pause/resume only the presentation timer; retained log/markup is unchanged.
 * Call from the LVGL task or while holding its lock. */
void nmea_log_view_suspend(bool suspended);
