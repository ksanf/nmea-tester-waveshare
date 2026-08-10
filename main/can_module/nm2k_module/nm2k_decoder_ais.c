/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoders for AIS-related NMEA2000 PGNs:
 *          129038 — AIS Class A Position Report
 *          129039 — AIS Class B Position Report
 *          129793 — AIS UTC/Date Report
 *          129794 — AIS Class A Static & Voyage
 *          129809 — AIS Class B Static
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nm2k_decoder.h"

/* ─── Utilities ────────────────────────────────────────────── */

static inline uint32_t r32_(const uint8_t *d, size_t off)
{
    return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
           ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

static inline int32_t rs32_(const uint8_t *d, size_t off)
{
    return (int32_t)r32_(d, off);
}

static inline uint16_t r16_(const uint8_t *d, size_t off)
{
    return (uint16_t)d[off] | ((uint16_t)d[off + 1] << 8);
}

/* Coordinate in 1e-7 degrees to degrees with seven fractional digits */
static void fmt_coord_(int32_t raw, char *buf, size_t cap)
{
    if (raw == 0x4FFFFFFF) {
        snprintf(buf, cap, "NA");
        return;
    }
    uint32_t abs_raw = (uint32_t)(raw < 0 ? -raw : raw);
    char sign = (raw < 0) ? '-' : '+';
    snprintf(buf, cap, "%c%" PRIu32 ".%07" PRIu32,
             sign, abs_raw / 10000000u, abs_raw % 10000000u);
}

/* Text field padded with 0xFF */
static void text_fld_(const uint8_t *src, uint8_t len, char *dst, size_t dst_cap)
{
    size_t w = 0;
    if (!src || !dst || dst_cap == 0u) return;
    for (uint8_t i = 0; i < len && w + 1 < dst_cap; ++i) {
        if (src[i] >= 0x20u && src[i] <= 0x7Eu) {
            dst[w++] = (char)src[i];
        } else if (src[i] == 0u || src[i] == 0xFFu) {
            break;
        }
    }
    dst[w] = '\0';
}

static const char *nav_status_(uint8_t ns)
{
    switch (ns) {
        case 0u: return "UnderWay";
        case 1u: return "AtAnchor";
        case 2u: return "NotComm";
        case 3u: return "Restricted";
        case 4u: return "Constrained";
        case 5u: return "Moored";
        case 6u: return "Aground";
        case 7u: return "Fishing";
        case 8u: return "Sailing";
        default: return "?";
    }
}

/* Raw COG (1e-4 rad) to tenths of a degree */
static uint16_t cog_tenths_(uint16_t raw)
{
    if (raw == 0xFFFF) return 0xFFFFu;
    uint32_t v = (uint32_t)raw * 1800000u / 3141592u;
    return (uint16_t)(v % 36000u);
}

/* ─── 129038 — AIS Class A Position Report ───────────────── */
static bool decode_129038_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint32_t mmsi;
    uint8_t  ns;
    int32_t  lat_raw, lon_raw;
    uint16_t sog_raw, cog_raw;
    char lat_s[24], lon_s[24];

    if (msg->pgn != 129038u || msg->len < 26u) return false;

    mmsi    = r32_(msg->data, 0);
    ns      = msg->data[4] & 0x0Fu;
    sog_raw = r16_(msg->data, 7);
    lon_raw = rs32_(msg->data, 11);
    lat_raw = rs32_(msg->data, 15);
    cog_raw = r16_(msg->data, 19);

    if (lat_raw == 0x4FFFFFFF || lon_raw == 0x4FFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " AIS-A MMSI=%09" PRIu32 " %s pos=NA",
                 msg->pgn, mmsi, nav_status_(ns));
        return true;
    }

    fmt_coord_(lat_raw, lat_s, sizeof(lat_s));
    fmt_coord_(lon_raw, lon_s, sizeof(lon_s));

    uint16_t sog_t = (sog_raw != 0xFFFF) ? ((sog_raw + 5u) / 10u) : 0xFFFFu;
    uint16_t cog_t = cog_tenths_(cog_raw);

    if (sog_t != 0xFFFFu && cog_t != 0xFFFFu) {
        snprintf(line, cap, "%06" PRIu32 " AIS-A MMSI=%09" PRIu32 " %s %s %s %u.%ukn %u.%udeg",
                 msg->pgn, mmsi, nav_status_(ns), lat_s, lon_s,
                 sog_t / 10u, sog_t % 10u,
                 cog_t / 10u, cog_t % 10u);
    } else {
        snprintf(line, cap, "%06" PRIu32 " AIS-A MMSI=%09" PRIu32 " %s %s %s",
                 msg->pgn, mmsi, nav_status_(ns), lat_s, lon_s);
    }
    return true;
}

/* ─── 129039 — AIS Class B Position Report ───────────────── */
static bool decode_129039_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint32_t mmsi;
    int32_t  lat_raw, lon_raw;
    uint16_t sog_raw, cog_raw;
    char lat_s[24], lon_s[24];

    if (msg->pgn != 129039u || msg->len < 22u) return false;

    mmsi    = r32_(msg->data, 0);
    sog_raw = r16_(msg->data, 5);
    lon_raw = rs32_(msg->data, 9);
    lat_raw = rs32_(msg->data, 13);
    cog_raw = r16_(msg->data, 17);

    if (lat_raw == 0x4FFFFFFF || lon_raw == 0x4FFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " AIS-B MMSI=%09" PRIu32 " pos=NA",
                 msg->pgn, mmsi);
        return true;
    }

    fmt_coord_(lat_raw, lat_s, sizeof(lat_s));
    fmt_coord_(lon_raw, lon_s, sizeof(lon_s));

    uint16_t sog_t = (sog_raw != 0xFFFF) ? ((sog_raw + 5u) / 10u) : 0xFFFFu;
    uint16_t cog_t = cog_tenths_(cog_raw);

    if (sog_t != 0xFFFFu && cog_t != 0xFFFFu) {
        snprintf(line, cap, "%06" PRIu32 " AIS-B MMSI=%09" PRIu32 " %s %s %u.%ukn %u.%udeg",
                 msg->pgn, mmsi, lat_s, lon_s,
                 sog_t / 10u, sog_t % 10u,
                 cog_t / 10u, cog_t % 10u);
    } else {
        snprintf(line, cap, "%06" PRIu32 " AIS-B MMSI=%09" PRIu32 " %s %s",
                 msg->pgn, mmsi, lat_s, lon_s);
    }
    return true;
}

/* ─── 129793 — AIS UTC/Date Report ────────────────────────── */
static bool decode_129793_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint32_t mmsi;
    int32_t  lat_raw, lon_raw;
    uint16_t sog_raw, cog_raw;
    char lat_s[24], lon_s[24];

    if (msg->pgn != 129793u || msg->len < 22u) return false;

    mmsi    = r32_(msg->data, 0);
    lat_raw = rs32_(msg->data, 11);
    lon_raw = rs32_(msg->data, 15);
    sog_raw = r16_(msg->data, 19);
    cog_raw = r16_(msg->data, 21);

    if (lat_raw == 0x4FFFFFFF || lon_raw == 0x4FFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " AIS-UTC MMSI=%09" PRIu32 " pos=NA",
                 msg->pgn, mmsi);
        return true;
    }

    fmt_coord_(lat_raw, lat_s, sizeof(lat_s));
    fmt_coord_(lon_raw, lon_s, sizeof(lon_s));

    uint16_t sog_t = (sog_raw != 0xFFFF) ? ((sog_raw + 5u) / 10u) : 0xFFFFu;
    uint16_t cog_t = cog_tenths_(cog_raw);

    if (sog_t != 0xFFFFu && cog_t != 0xFFFFu) {
        snprintf(line, cap, "%06" PRIu32 " AIS-UTC MMSI=%09" PRIu32 " %s %s %u.%ukn %u.%udeg",
                 msg->pgn, mmsi, lat_s, lon_s,
                 sog_t / 10u, sog_t % 10u,
                 cog_t / 10u, cog_t % 10u);
    } else {
        snprintf(line, cap, "%06" PRIu32 " AIS-UTC MMSI=%09" PRIu32 " %s %s",
                 msg->pgn, mmsi, lat_s, lon_s);
    }
    return true;
}

/* ─── 129794 — AIS Class A Static & Voyage ───────────────── */
static bool decode_129794_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint32_t mmsi;
    char name[21];
    char callsign[8];
    uint8_t  ship_type;
    uint8_t  imo;

    if (msg->pgn != 129794u || msg->len < 52u) return false;

    mmsi = r32_(msg->data, 0);
    text_fld_(&msg->data[4], 20, name, sizeof(name));
    text_fld_(&msg->data[24], 7, callsign, sizeof(callsign));
    ship_type = msg->data[31] & 0x3Fu;
    imo = msg->data[32];

    snprintf(line, cap, "%06" PRIu32 " AIS-A MMSI=%09" PRIu32 " \"%s\" IMO=%u type=%u CALL=%s",
             msg->pgn, mmsi, name, (unsigned)imo, (unsigned)ship_type, callsign);
    return true;
}

/* ─── 129809 — AIS Class B Static ────────────────────────── */
static bool decode_129809_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint32_t mmsi;
    char name[21];
    uint8_t ship_type;
    char callsign[8];

    if (msg->pgn != 129809u || msg->len < 36u) return false;

    mmsi = r32_(msg->data, 0);
    text_fld_(&msg->data[4], 20, name, sizeof(name));
    text_fld_(&msg->data[24], 7, callsign, sizeof(callsign));
    ship_type = msg->data[31] & 0x3Fu;

    snprintf(line, cap, "%06" PRIu32 " AIS-B MMSI=%09" PRIu32 " \"%s\" CALL=%s type=%u",
             msg->pgn, mmsi, name, callsign, (unsigned)ship_type);
    return true;
}

/* ─── Public API ──────────────────────────────────────────── */
bool nm2k_decode_ais(const n2k_msg_t *msg, char *line, size_t cap)
{
    if (!msg || !line || cap == 0u) return false;

    switch (msg->pgn) {
    case 129038u: return decode_129038_(msg, line, cap);
    case 129039u: return decode_129039_(msg, line, cap);
    case 129793u: return decode_129793_(msg, line, cap);
    case 129794u: return decode_129794_(msg, line, cap);
    case 129809u: return decode_129809_(msg, line, cap);
    default:      return false;
    }
}
