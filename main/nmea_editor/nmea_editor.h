/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NMEA_EDITOR_H
#define NMEA_EDITOR_H

#include "lvgl.h"

/*────────────── Module version ─────────────*/
#define NMEA_EDITOR_VERSION_MAJOR 1
#define NMEA_EDITOR_VERSION_MINOR 1
#define NMEA_EDITOR_VERSION_PATCH 0
#define NMEA_EDITOR_VERSION_STR   "1.1.0"


#ifdef __cplusplus
extern "C" {
#endif

/* Presentation only; call under the LVGL lock. Valid pending numeric input
 * is applied, invalid drafts roll back. The tab is rebuilt from templates. */
void nmea_editor_suspend(bool suspended);
void nmea_editor_create(void);   /* Create or show the editor screen. */

#ifdef __cplusplus
}
#endif
#endif /* NMEA_EDITOR_H */
