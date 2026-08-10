/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoders for navigation/environmental PGNs:
 *          126992 — System Time
 *          127250 — Vessel Heading
 *          127257 — Attitude (roll, pitch, yaw)
 *          130306 — Wind Speed
 *          130310 — Environmental Temperature
 *          130311 — Environmental Pressure
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "nm2k_decoder.h"

/* ─── Utilities ────────────────────────────────────────────── */

static inline uint16_t r16_(const uint8_t *d, size_t off)
{
    return (uint16_t)d[off] | ((uint16_t)d[off + 1] << 8);
}

static inline int16_t rs16_(const uint8_t *d, size_t off)
{
    return (int16_t)r16_(d, off);
}

/* 1e-4 radians to whole degrees */
static int16_t rad1e4_to_deg_(int16_t raw)
{
    if (raw == (int16_t)0x7FFF) return 0x7FFF;
    /* Convert raw * 1e-4 radians to degrees; scale to tenths of a degree. */
    int32_t v;
    int32_t sign = 1;
    if (raw < 0) { sign = -1; raw = (int16_t)(-raw); }
    v = (int32_t)raw * 18000 / 31416;  /* Tenths of a degree */
    return (int16_t)(v * sign);
}

/* ─── 126992 — System Time ──────────────────────────────── */
static bool decode_126992_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t date_raw;
    uint32_t time_raw;
    uint8_t  source;

    if (msg->pgn != 126992u || msg->len < 6u) return false;

    date_raw = r16_(msg->data, 0);
    time_raw = (uint32_t)msg->data[2] |
               ((uint32_t)msg->data[3] << 8) |
               ((uint32_t)msg->data[4] << 16) |
               ((uint32_t)msg->data[5] << 24);
    source = (msg->len >= 7u) ? (msg->data[6] & 0x0Fu) : 0xFFu;

    if (date_raw == 0xFFFF && time_raw == 0xFFFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " SYSTIME=NA", msg->pgn);
        return true;
    }

    uint32_t total_sec = (uint32_t)date_raw * 86400u + time_raw / 1000u;
    uint32_t hh = (total_sec % 86400u) / 3600u;
    uint32_t mm = ((total_sec % 86400u) % 3600u) / 60u;
    uint32_t ss = total_sec % 60u;

    const char *src_s = (source == 0u) ? "GPS" :
                        (source == 1u) ? "Int" :
                        (source == 2u) ? "Radio" : "?";
    snprintf(line, cap, "%06" PRIu32 " SYS %02u:%02u:%02u %s",
             msg->pgn, (unsigned)hh, (unsigned)mm, (unsigned)ss, src_s);
    return true;
}

/* ─── 127250 — Vessel Heading ────────────────────────────── */
static bool decode_127250_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t heading_raw;
    uint8_t  ref;

    if (msg->pgn != 127250u || msg->len < 8u) return false;

    heading_raw = r16_(msg->data, 1);
    ref = msg->data[7] & 0x03u;

    const char *ref_s = (ref == 0u) ? "MAG" :
                        (ref == 1u) ? "TRUE" :
                        (ref == 2u) ? "MAG" : "ERR";

    if (heading_raw == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " HEADING=NA %s", msg->pgn, ref_s);
        return true;
    }

    int16_t hdg_tenths = rad1e4_to_deg_((int16_t)heading_raw);
    if (hdg_tenths == 0x7FFF) {
        snprintf(line, cap, "%06" PRIu32 " HEADING NA %s", msg->pgn, ref_s);
        return true;
    }

    snprintf(line, cap, "%06" PRIu32 " HEADING %u.%udeg %s",
             msg->pgn,
             (unsigned)(hdg_tenths / 10), (unsigned)(hdg_tenths < 0 ? -(hdg_tenths % 10) : (hdg_tenths % 10)),
             ref_s);
    return true;
}

/* ─── 127257 — Attitude (roll, pitch, yaw) ───────────────── */
static bool decode_127257_(const n2k_msg_t *msg, char *line, size_t cap)
{
    int16_t yaw_raw, pitch_raw, roll_raw;
    int16_t yaw_t, pitch_t, roll_t;

    if (msg->pgn != 127257u || msg->len < 7u) return false;

    yaw_raw   = rs16_(msg->data, 1);
    pitch_raw = rs16_(msg->data, 3);
    roll_raw  = rs16_(msg->data, 5);

    yaw_t   = rad1e4_to_deg_(yaw_raw);
    pitch_t = rad1e4_to_deg_(pitch_raw);
    roll_t  = rad1e4_to_deg_(roll_raw);

    snprintf(line, cap, "%06" PRIu32 " ATT yaw=%d.%d pitch=%d.%d roll=%d.%d",
             msg->pgn,
             yaw_t / 10, (yaw_t < 0 ? -(yaw_t % 10) : (yaw_t % 10)),
             pitch_t / 10, (pitch_t < 0 ? -(pitch_t % 10) : (pitch_t % 10)),
             roll_t / 10, (roll_t < 0 ? -(roll_t % 10) : (roll_t % 10)));
    return true;
}

/* ─── 130306 — Wind Speed ───────────────────────────────── */
static bool decode_130306_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t speed_raw;
    uint16_t angle_raw;
    uint8_t  reference;

    if (msg->pgn != 130306u || msg->len < 7u) return false;

    speed_raw = r16_(msg->data, 1);   /* 0.01 m/s */
    angle_raw = r16_(msg->data, 3);   /* 1e-4 rad */
    reference = msg->data[5] & 0x07u;

    const char *ref_s = (reference == 0u) ? "TRUE" :
                        (reference == 1u) ? "MAG" :
                        (reference == 2u) ? "App" : "ERR";

    if (speed_raw == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " WIND=NA %s", msg->pgn, ref_s);
        return true;
    }

    /* m/s x 1.94384 = knots; multiply speed_raw by 194, then divide by 10000 for tenths of a knot. */
    uint32_t knots_tenths = ((uint32_t)speed_raw * 194u + 5000u) / 10000u;
    int16_t ang_tenths = rad1e4_to_deg_((int16_t)angle_raw);

    snprintf(line, cap, "%06" PRIu32 " WIND %u.%udeg %u.%ukn %s",
             msg->pgn,
             (unsigned)(ang_tenths / 10), (unsigned)(ang_tenths % 10),
             (unsigned)(knots_tenths / 10u), (unsigned)(knots_tenths % 10u),
             ref_s);
    return true;
}

/* ─── 130310 — Environmental Temperature ────────────────── */
static bool decode_130310_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  temp_instance;
    uint8_t  temp_source;
    int16_t  temp_raw;
    uint8_t  humidity_raw;
    int16_t  temp_c_tenths;

    if (msg->pgn != 130310u || msg->len < 6u) return false;

    temp_instance = msg->data[1];
    temp_source = msg->data[2] & 0x0Fu;
    temp_raw = rs16_(msg->data, 3);     /* 0.01 K */
    humidity_raw = msg->data[5];

    const char *src_s = (temp_source == 0u) ? "Sea" :
                        (temp_source == 1u) ? "Out" :
                        (temp_source == 2u) ? "In" :
                        (temp_source == 3u) ? "EngRoom" :
                        (temp_source == 4u) ? "Fridge" : "?";

    if (temp_raw == (int16_t)0x7FFF) {
        snprintf(line, cap, "%06" PRIu32 " TEMP %s inst=%u NA",
                 msg->pgn, src_s, (unsigned)temp_instance);
        return true;
    }

    /* Convert 0.01 K to degrees Celsius by subtracting 273.15 and scaling to tenths. */
    int32_t k_tenths = (int32_t)temp_raw * 10 / 100;  /* 0.01 K * 10 gives tenths of a kelvin. */
    temp_c_tenths = (int16_t)(k_tenths - 27315);      /* Tenths of °C (2731.5 K to °C x 10) */
    /* temp_raw / 100 is kelvin; multiply by 10 for tenths, then subtract 27315. */
    temp_c_tenths = (int16_t)((int32_t)temp_raw / 10 - 27315);

    if (humidity_raw != 0xFFu) {
        snprintf(line, cap, "%06" PRIu32 " TEMP %s %d.%dC RH=%u%%",
                 msg->pgn, src_s,
                 (int)(temp_c_tenths / 10), (int)(temp_c_tenths < 0 ? -(temp_c_tenths % 10) : (temp_c_tenths % 10)),
                 (unsigned)humidity_raw);
    } else {
        snprintf(line, cap, "%06" PRIu32 " TEMP %s %d.%dC",
                 msg->pgn, src_s,
                 (int)(temp_c_tenths / 10), (int)(temp_c_tenths < 0 ? -(temp_c_tenths % 10) : (temp_c_tenths % 10)));
    }
    return true;
}

/* ─── 130311 — Environmental Pressure ───────────────────── */
static bool decode_130311_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  press_instance;
    int16_t  press_raw;

    if (msg->pgn != 130311u || msg->len < 4u) return false;

    press_instance = msg->data[1] & 0x0Fu;
    press_raw = rs16_(msg->data, 2);    /* 0.1 Pa */

    if (press_raw == (int16_t)0x7FFF) {
        snprintf(line, cap, "%06" PRIu32 " PRESS inst=%u NA",
                 msg->pgn, (unsigned)press_instance);
        return true;
    }

    /* 0.1 Pa → hPa: / 1000 */
    int32_t press_hpa = (int32_t)press_raw / 1000;
    snprintf(line, cap, "%06" PRIu32 " PRESS %" PRIi32 "hPa", msg->pgn, press_hpa);
    return true;
}

/* ─── Public API ──────────────────────────────────────────── */
bool nm2k_decode_nav(const n2k_msg_t *msg, char *line, size_t cap)
{
    if (!msg || !line || cap == 0u) return false;

    switch (msg->pgn) {
    case 126992u: return decode_126992_(msg, line, cap);
    case 127250u: return decode_127250_(msg, line, cap);
    case 127257u: return decode_127257_(msg, line, cap);
    case 130306u: return decode_130306_(msg, line, cap);
    case 130310u: return decode_130310_(msg, line, cap);
    case 130311u: return decode_130311_(msg, line, cap);
    default:      return false;
    }
}
