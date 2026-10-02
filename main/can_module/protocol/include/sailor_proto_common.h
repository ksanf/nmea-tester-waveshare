/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared types, constants, and utilities for the Sailor-CAN stack (L2..L5).
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===== Header set version ===== */
#define SP_PROTO_VER_MAJOR 1
#define SP_PROTO_VER_MINOR 0
#define SP_PROTO_VER_PATCH 0
#define SP_PROTO_VER_STR   "1.0.0-prod"

/* ===== Basic errors ===== */
typedef int32_t sp_err_t;
enum {
    SP_OK            = 0,
    SP_E_TIMEOUT     = -1,
    SP_E_NOMEM       = -2,
    SP_E_INVAL       = -3,
    SP_E_STATE       = -4,
    SP_E_PROTO       = -5,
    SP_E_OVERFLOW    = -6,
    SP_E_UNSUPPORTED = -7
};

/* ===== 29-bit ID: pri|dp|pf|ps|sa ===== */
typedef struct {
    uint8_t pri;  /* 0..7 */
    uint8_t dp;   /* 0/1  */
    uint8_t pf;   /* PDU Format */
    uint8_t ps;   /* PDU Specific (DA for PDU1) */
    uint8_t sa;   /* Source Address */
} sp_id_fields_t;

static inline uint32_t sp_id_pack(const sp_id_fields_t *f) {
    return ((uint32_t)(f->pri & 0x7) << 26) |
           ((uint32_t)(f->dp  & 0x1) << 24) |
           ((uint32_t) f->pf         << 16) |
           ((uint32_t) f->ps         <<  8) |
           ((uint32_t) f->sa);
}
static inline sp_id_fields_t sp_id_unpack(uint32_t id) {
    sp_id_fields_t f;
    f.pri = (id >> 26) & 0x7;
    f.dp  = (id >> 24) & 0x1;
    f.pf  = (id >> 16) & 0xFF;
    f.ps  = (id >>  8) & 0xFF;
    f.sa  =  id        & 0xFF;
    return f;
}

/* PGN as defined by J1939/NMEA2000:
 * - PDU2 (pf>=240): PGN= dp<<16 | pf<<8 | 0x00  (PS=group/FF)
 * - PDU1 (pf< 240): PGN= dp<<16 | pf<<8 | 0x00  (PS=DA)
 */
static inline uint32_t sp_pgn_of(uint8_t dp, uint8_t pf) {
    return ((uint32_t)(dp & 0x1) << 16) | ((uint32_t)pf << 8);
}

/* ===== Channel PF values and PGNs used by Sailor ===== */
enum { SP_PF_EF = 0xEF, SP_PF_EA = 0xEA, SP_PF_EE = 0xEE };
enum { SP_PGN_EF = 0x00EF00, SP_PGN_EA = 0x00EA00, SP_PGN_EE = 0x00EE00 };

enum { SP_PRI_DEF = 3, SP_PRI_HIGH = 2, SP_PRI_LOW = 5 };

/* ===== L2 frame (raw payload up to 8 bytes) ===== */
typedef struct {
    sp_id_fields_t id;
    uint8_t        dlc;           /* 0..8 */
    uint8_t        data[8];
} sp_l2_frame_t;

/* ===== Reassembled L3 block (Fast-Packet) ===== */
typedef struct {
    sp_id_fields_t id;
    uint32_t       pgn;           /* convenience */
    uint8_t        sid;           /* Sequence ID (0..7) */
    const uint8_t *payload;       /* contiguous buffer */
    uint16_t       len;           /* 1..223 */
    bool           completed;     /* True when complete */
} sp_l3_block_t;

/* Transport events for telemetry and control logic */
typedef enum {
    SP_EVT_NONE = 0,
    SP_EVT_L2_RX_DROPPED,
    SP_EVT_L2_TX_DROPPED,
    SP_EVT_FP_TIMEOUT,
    SP_EVT_FP_LOST_SEQ,
    SP_EVT_FP_OVERLEN
} sp_event_t;

/* ===== Timing defaults (overridable) ===== */
typedef struct {
    uint16_t fp_block_timeout_ms;   /* single-block FP assembly timeout */
} sp_timing_t;

/* ===== NDP v2 header (6 bytes; v8 wire-format specification, section 4.2) =====
 *  offset  size  field
 *   0       1    SIG_A = 0x5F
 *   1       1    SIG_B = 0x99
 *   2       1    SIG_C = 0x02 (NDP v2)
 *   3       1    control — DATA 0x08..0x0F, ACK 0x00..0x07, Open/Accept/Abort 0x10..0x12
 *   4       1    page  — page index; 0 for the first page
 *   5       1    ch    — remote_port<<4 | local_port
 *
 * Full format (plen > 0):
 *   [6-byte header][len_lo][len_hi][payload[plen]]
 *
 * Short format (SHORT ACK, plen == 0):
 *   [6-byte header] - total_len=6, without len_lo/len_hi or payload
 */
#define SP_BIG_SIG_A 0x5F
#define SP_BIG_SIG_B 0x99
#define SP_BIG_SIG_C 0x02
