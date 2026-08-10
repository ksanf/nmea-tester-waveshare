/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoders for GNSS/Navigation PGNs:
 *          129025 — Position Rapid Update
 *          129026 — COG & SOG Rapid Update
 *          129029 — GNSS Position
 *          129033 — GNSS Time & Date
 *          129539 — GNSS DOP
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "nm2k_decoder.h"

/* ─── Inline helpers ──────────────────────────────────────── */

static inline uint16_t r16_(const uint8_t *d, size_t off)
{
    return (uint16_t)d[off] | ((uint16_t)d[off + 1] << 8);
}

static inline int32_t r32i_(const uint8_t *d, size_t off)
{
    return (int32_t)((uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
                     ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24));
}

/* 1e-7 degrees to whole degrees plus seven fractional digits; output: DDDMM.MMMMMM */
/* For simplicity, display it as an integer scaled by 1e7. */
static void fmt_coord_(int32_t raw, char *buf, size_t cap)
{
    if (raw == 0x7FFFFFFF) {
        snprintf(buf, cap, "NA");
        return;
    }
    uint32_t abs_raw = (uint32_t)(raw < 0 ? -raw : raw);
    char sign = (raw < 0) ? '-' : '+';
    snprintf(buf, cap, "%c%" PRIu32 ".%07" PRIu32,
             sign, abs_raw / 10000000u, abs_raw % 10000000u);
}

/* Raw COG (1e-4 rad) to whole degrees */
static uint16_t cog_tenths_(uint16_t raw)
{
    if (raw == 0xFFFF) return 0xFFFFu;
    /* Convert 1e-4 radians to degrees: multiply by 18000 / PI, yielding tenths of a degree. */
    uint32_t v = (uint32_t)raw * 1800000u / 3141592u;  /* Multiply by 180 / PI and by 10 for tenths. */
    return (uint16_t)(v % 36000u);  /* 0..35999 */
}

/* ─── 129025 — Position Rapid Update ───────────────────────── */
static bool decode_129025_(const n2k_msg_t *msg, char *line, size_t cap)
{
    int32_t lat, lon;
    char lat_s[24], lon_s[24];

    if (msg->pgn != 129025u || msg->len < 8u) return false;

    lat = r32i_(msg->data, 0);
    lon = r32i_(msg->data, 4);

    if (lat == 0x7FFFFFFF || lon == 0x7FFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " GNSS pos=NA", msg->pgn);
        return true;
    }

    fmt_coord_(lat, lat_s, sizeof(lat_s));
    fmt_coord_(lon, lon_s, sizeof(lon_s));
    snprintf(line, cap, "%06" PRIu32 " GNSS %s %s", msg->pgn, lat_s, lon_s);
    return true;
}

/* ─── 129026 — COG & SOG Rapid Update ─────────────────────── */
static bool decode_129026_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t sog_raw, cog_raw;
    uint8_t  ref;

    if (msg->pgn != 129026u || msg->len < 8u) return false;

    sog_raw = r16_(msg->data, 0);       /* 0.01 kn */
    cog_raw = r16_(msg->data, 2);       /* 1e-4 rad */
    ref = msg->data[4] & 0x03u;

    const char *ref_s = (ref == 0u) ? "TRUE" :
                        (ref == 1u) ? "MAG" : "ERR";

    if (sog_raw == 0xFFFF && cog_raw == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " COG/SOG=NA %s",
                 msg->pgn, ref_s);
        return true;
    }

    uint16_t sog_tenths = (sog_raw + 5u) / 10u; /* 0.01 kn → 0.1 kn */
    uint16_t cog_t = cog_tenths_(cog_raw);

    if (cog_t == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " COG=NA SOG=%u.%ukn %s",
                 msg->pgn,
                 sog_tenths / 10u, sog_tenths % 10u, ref_s);
    } else {
        snprintf(line, cap, "%06" PRIu32 " COG=%u.%u SOG=%u.%ukn %s",
                 msg->pgn,
                 cog_t / 10u, cog_t % 10u,
                 sog_tenths / 10u, sog_tenths % 10u, ref_s);
    }
    return true;
}

/* ─── 129029 — GNSS Position ──────────────────────────────── */
static bool decode_129029_(const n2k_msg_t *msg, char *line, size_t cap)
{
    int32_t  lat, lon;
    uint16_t sog_raw, cog_raw;
    uint8_t  fix_qual, sat_count;
    char lat_s[24], lon_s[24];

    if (msg->pgn != 129029u || msg->len < 24u) return false;

    lat = r32i_(msg->data, 5);
    lon = r32i_(msg->data, 9);
    sog_raw = r16_(msg->data, 15);
    cog_raw = r16_(msg->data, 17);
    fix_qual = msg->data[19] & 0x0Fu;
    sat_count = msg->data[20];

    if (lat == 0x7FFFFFFF || lon == 0x7FFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " GNSS fix=%u sats=%u pos=NA",
                 msg->pgn, (unsigned)fix_qual, (unsigned)sat_count);
        return true;
    }

    fmt_coord_(lat, lat_s, sizeof(lat_s));
    fmt_coord_(lon, lon_s, sizeof(lon_s));

    if (sog_raw != 0xFFFF && cog_raw != 0xFFFF) {
        uint16_t sog_t = (sog_raw + 5u) / 10u;
        uint16_t cog_t = cog_tenths_(cog_raw);
        snprintf(line, cap, "%06" PRIu32 " GNSS %s %s %u.%ukn %u.%udeg fix=%u sats=%u",
                 msg->pgn, lat_s, lon_s,
                 sog_t / 10u, sog_t % 10u,
                 cog_t / 10u, cog_t % 10u,
                 (unsigned)fix_qual, (unsigned)sat_count);
    } else {
        snprintf(line, cap, "%06" PRIu32 " GNSS %s %s fix=%u sats=%u",
                 msg->pgn, lat_s, lon_s,
                 (unsigned)fix_qual, (unsigned)sat_count);
    }
    return true;
}

/* ─── 129033 — GNSS Time & Date ────────────────────────────── */
static bool decode_129033_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t date_raw;
    uint32_t time_raw;
    uint8_t  source;

    if (msg->pgn != 129033u || msg->len < 8u) return false;

    date_raw = r16_(msg->data, 0);
    time_raw = (uint32_t)msg->data[2] |
               ((uint32_t)msg->data[3] << 8) |
               ((uint32_t)msg->data[4] << 16) |
               ((uint32_t)msg->data[5] << 24);
    source = msg->data[6] & 0x0Fu;

    if (date_raw == 0xFFFF && time_raw == 0xFFFFFFFF) {
        snprintf(line, cap, "%06" PRIu32 " TIME=NA src=%u",
                 msg->pgn, (unsigned)source);
        return true;
    }

    uint32_t total_sec = (uint32_t)date_raw * 86400u + time_raw;
    uint32_t hh = (total_sec % 86400u) / 3600u;
    uint32_t mm = ((total_sec % 86400u) % 3600u) / 60u;
    uint32_t ss = total_sec % 60u;

    const char *src_s = (source == 0u) ? "GPS" :
                        (source == 1u) ? "GLONASS" :
                        (source == 2u) ? "Galileo" :
                        (source == 3u) ? "BeiDou" : "?";
    snprintf(line, cap, "%06" PRIu32 " TIME %02u:%02u:%02u %s",
             msg->pgn, (unsigned)hh, (unsigned)mm, (unsigned)ss, src_s);
    return true;
}

/* ─── 129539 — GNSS DOP ──────────────────────────────────── */
static bool decode_129539_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint16_t pdop, hdop, vdop, tdop;

    if (msg->pgn != 129539u || msg->len < 6u) return false;

    pdop = r16_(msg->data, 0);
    hdop = r16_(msg->data, 2);
    vdop = r16_(msg->data, 4);

    if (msg->len >= 8u) {
        tdop = r16_(msg->data, 6);
    } else {
        tdop = 0xFFFFu;
    }

    /* One unit is 0.01; display whole hundredths. */
    #define DOP_FMT(p, l) \
        ((p) == 0xFFFF) ? "NA" : l, \
        ((p) == 0xFFFF) ? 0u : (unsigned)((p) / 100u), \
        ((p) == 0xFFFF) ? 0u : (unsigned)((p) % 100u)

    if (tdop != 0xFFFF && msg->len >= 8u) {
        snprintf(line, cap,
                 "%06" PRIu32 " DOP P=%s%u.%02u H=%s%u.%02u V=%s%u.%02u T=%s%u.%02u",
                 msg->pgn,
                 DOP_FMT(pdop, ""), DOP_FMT(hdop, ""),
                 DOP_FMT(vdop, ""), DOP_FMT(tdop, ""));
    } else {
        snprintf(line, cap,
                 "%06" PRIu32 " DOP P=%s%u.%02u H=%s%u.%02u V=%s%u.%02u",
                 msg->pgn,
                 DOP_FMT(pdop, ""), DOP_FMT(hdop, ""), DOP_FMT(vdop, ""));
    }
    return true;

    #undef DOP_FMT
}

/* ─── Public API ──────────────────────────────────────────── */
bool nm2k_decode_gps(const n2k_msg_t *msg, char *line, size_t cap)
{
    if (!msg || !line || cap == 0u) return false;

    switch (msg->pgn) {
    case 129025u: return decode_129025_(msg, line, cap);
    case 129026u: return decode_129026_(msg, line, cap);
    case 129029u: return decode_129029_(msg, line, cap);
    case 129033u: return decode_129033_(msg, line, cap);
    case 129539u: return decode_129539_(msg, line, cap);
    default:      return false;
    }
}
