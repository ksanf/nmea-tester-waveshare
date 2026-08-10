/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Decoders for Engine/DC/Propulsion PGNs:
 *          127488 — Engine Rapid (RPM, boost, etc.)
 *          127489 — Engine Dynamic (load, fuel rate, etc.)
 *          127493 — Transmission Parameters (gear, oil press, temp)
 *          127508 — DC Detailed Status (battery, alternator)
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

/* ─── 127488 — Engine Rapid (RPM, boost, oil pressure) ────── */
static bool decode_127488_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  instance;
    uint16_t rpm_raw;
    int16_t  boost_raw;       /* 0.01 kPa */
    int16_t  oil_press_raw;   /* 0.01 kPa */

    if (msg->pgn != 127488u || msg->len < 8u) return false;

    instance = msg->data[0];
    rpm_raw = r16_(msg->data, 1);          /* 1/4 RPM */
    boost_raw = rs16_(msg->data, 3);
    oil_press_raw = rs16_(msg->data, 5);

    if (rpm_raw == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " ENG%u RPM=NA",
                 msg->pgn, (unsigned)instance);
        return true;
    }

    uint16_t rpm = (uint16_t)((uint32_t)rpm_raw / 4u);  /* Whole RPM */

    /* Convert kPa to bar and display tenths of a bar. */
    if (boost_raw != (int16_t)0x7FFF || oil_press_raw != (int16_t)0x7FFF) {
        int16_t boost_bar  = (boost_raw != (int16_t)0x7FFF)
                             ? (int16_t)((int32_t)boost_raw / 100)
                             : (int16_t)0x7FFF;
        int16_t oil_bar    = (oil_press_raw != (int16_t)0x7FFF)
                             ? (int16_t)((int32_t)oil_press_raw / 100)
                             : (int16_t)0x7FFF;

        if (boost_bar != (int16_t)0x7FFF && oil_bar != (int16_t)0x7FFF) {
            snprintf(line, cap, "%06" PRIu32 " ENG%u RPM=%u boost=%d.%dbar oil=%d.%dbar",
                     msg->pgn, (unsigned)instance,
                     (unsigned)rpm,
                     (int)(boost_bar / 10), (int)(boost_bar < 0 ? -(boost_bar % 10) : (boost_bar % 10)),
                     (int)(oil_bar / 10), (int)(oil_bar < 0 ? -(oil_bar % 10) : (oil_bar % 10)));
        } else {
            snprintf(line, cap, "%06" PRIu32 " ENG%u RPM=%u",
                     msg->pgn, (unsigned)instance, (unsigned)rpm);
        }
    } else {
        snprintf(line, cap, "%06" PRIu32 " ENG%u RPM=%u",
                 msg->pgn, (unsigned)instance, (unsigned)rpm);
    }
    return true;
}

/* ─── 127489 — Engine Dynamic (load, fuel rate) ──────────── */
static bool decode_127489_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  instance;
    int16_t  load_raw;           /* percent */
    uint16_t fuel_rate_raw;      /* 0.01 L/h */

    if (msg->pgn != 127489u || msg->len < 8u) return false;

    instance = msg->data[0];
    load_raw = rs16_(msg->data, 1);
    fuel_rate_raw = r16_(msg->data, 5);

    if (load_raw == (int16_t)0x7FFF && fuel_rate_raw == 0xFFFF) {
        snprintf(line, cap, "%06" PRIu32 " ENG%u NA", msg->pgn, (unsigned)instance);
        return true;
    }

    if (fuel_rate_raw != 0xFFFF) {
        /* Convert 0.01 L/h to tenths of L/h. */
        uint16_t fuel_tenths = (fuel_rate_raw + 5u) / 10u;
        snprintf(line, cap, "%06" PRIu32 " ENG%u load=%d%% fuel=%u.%uL/h",
                 msg->pgn, (unsigned)instance,
                 (int)load_raw,
                 (unsigned)(fuel_tenths / 10u), (unsigned)(fuel_tenths % 10u));
    } else {
        snprintf(line, cap, "%06" PRIu32 " ENG%u load=%d%%",
                 msg->pgn, (unsigned)instance, (int)load_raw);
    }
    return true;
}

/* ─── 127493 — Transmission Parameters ───────────────────── */
static bool decode_127493_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  instance;
    uint8_t  gear;

    if (msg->pgn != 127493u || msg->len < 8u) return false;

    instance = msg->data[0];
    gear = msg->data[1] & 0x0Fu;

    const char *gear_s = (gear == 0u) ? "N" :
                         (gear == 1u) ? "Fwd" :
                         (gear == 2u) ? "Rev" : "?";

    snprintf(line, cap, "%06" PRIu32 " GEAR%u %s", msg->pgn, (unsigned)instance, gear_s);
    return true;
}

/* ─── 127508 — DC Detailed Status (battery, alternator) ──── */
static bool decode_127508_(const n2k_msg_t *msg, char *line, size_t cap)
{
    uint8_t  instance;
    uint8_t  dc_type;
    uint16_t voltage_raw;    /* 0.01 V */
    int16_t  current_raw;    /* 0.1 A */

    if (msg->pgn != 127508u || msg->len < 8u) return false;

    instance    = msg->data[0];
    dc_type     = msg->data[1] & 0x0Fu;
    voltage_raw = r16_(msg->data, 2);
    current_raw = rs16_(msg->data, 4);

    const char *type_s = (dc_type == 0u) ? "Batt" :
                         (dc_type == 1u) ? "Alt" :
                         (dc_type == 2u) ? "Conv" : "?";

    if (voltage_raw == 0xFFFF && current_raw == (int16_t)0x7FFF) {
        snprintf(line, cap, "%06" PRIu32 " DC%u %s NA", msg->pgn, (unsigned)instance, type_s);
        return true;
    }

    if (voltage_raw != 0xFFFF) {
        /* 0.01 V → 0.1 V */
        uint16_t v_tenths = voltage_raw / 10u;
        if (current_raw != (int16_t)0x7FFF) {
            /* Already expressed in 0.1 A. */
            int16_t c_tenths = current_raw;
            snprintf(line, cap, "%06" PRIu32 " DC%u %s %u.%uV %d.%dA",
                     msg->pgn, (unsigned)instance, type_s,
                     (unsigned)(v_tenths / 10u), (unsigned)(v_tenths % 10u),
                     (int)(c_tenths / 10), (int)(c_tenths < 0 ? -(c_tenths % 10) : (c_tenths % 10)));
        } else {
            snprintf(line, cap, "%06" PRIu32 " DC%u %s %u.%uV",
                     msg->pgn, (unsigned)instance, type_s,
                     (unsigned)(v_tenths / 10u), (unsigned)(v_tenths % 10u));
        }
    } else {
        snprintf(line, cap, "%06" PRIu32 " DC%u %s A=%d.%dA",
                 msg->pgn, (unsigned)instance, type_s,
                 (int)(current_raw / 10), (int)(current_raw < 0 ? -(current_raw % 10) : (current_raw % 10)));
    }
    return true;
}

/* ─── Public API ──────────────────────────────────────────── */
bool nm2k_decode_engine(const n2k_msg_t *msg, char *line, size_t cap)
{
    if (!msg || !line || cap == 0u) return false;

    switch (msg->pgn) {
    case 127488u: return decode_127488_(msg, line, cap);
    case 127489u: return decode_127489_(msg, line, cap);
    case 127493u: return decode_127493_(msg, line, cap);
    case 127508u: return decode_127508_(msg, line, cap);
    default:      return false;
    }
}
