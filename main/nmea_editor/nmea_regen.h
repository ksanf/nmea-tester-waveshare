/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef NMEA_REGEN_H
#define NMEA_REGEN_H

#include <stdint.h>

/* Capacity for a combined packet from all groups.       */
/* GRP_COUNT = 5, with up to 40 rows total.              */
#define MAX_PACKET_LINES    (MAX_LINES_IN_GRP * 5)

/* ───── Per-group formatter prototypes ───────────────── */
#ifdef __cplusplus
extern "C" {
#endif

void regen_gps    (char lines[][NMEA_SENT_MAX], uint8_t *cnt);
void regen_gyro   (char lines[][NMEA_SENT_MAX], uint8_t *cnt);
void regen_log    (char lines[][NMEA_SENT_MAX], uint8_t *cnt);
void regen_echo   (char lines[][NMEA_SENT_MAX], uint8_t *cnt);
void regen_weather(char lines[][NMEA_SENT_MAX], uint8_t *cnt);

#ifdef __cplusplus
}
#endif
#endif /* NMEA_REGEN_H */
