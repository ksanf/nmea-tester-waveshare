/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA2000/J1939 L2 layer: 29-bit ID packing/parsing, PGNs, and PDU1/PDU2.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <esp_err.h>
#include <esp_timer.h>
#include "driver/twai.h"

#include "can_driver.h"  /* Lower-level driver: can_driver_init/receive/send/... */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- N2K/J1939 addresses ---- */
#define N2K_ADDR_GLOBAL  0xFFU   /* Global broadcast */
#define N2K_ADDR_NULL    0xFEU   /* No address / NULL */

/* ---- Constants and masks for the 29-bit identifier ---- */
/* 29-bit ID = [Priority:3][EDP:1][DP:1][PF:8][PS:8][SA:8] */
#define N2K_ID_PRIORITY_SHIFT   26
#define N2K_ID_EDP_SHIFT        25
#define N2K_ID_DP_SHIFT         24
#define N2K_ID_PF_SHIFT         16
#define N2K_ID_PS_SHIFT          8
#define N2K_ID_SA_SHIFT          0

#define N2K_ID_PRIORITY_MASK   (0x7U)
#define N2K_ID_EDP_MASK        (0x1U)
#define N2K_ID_DP_MASK         (0x1U)
#define N2K_ID_PF_MASK         (0xFFU)
#define N2K_ID_PS_MASK         (0xFFU)
#define N2K_ID_SA_MASK         (0xFFU)

/* ---- PGN helpers ----
 * PGN = (DP<<16) | (PF<<8) | (PDU2 ? PS : 0x00)
 * PDU1 when PF < 240: PS is the Destination Address and the low PGN byte is 0x00.
 * PDU2 when PF >= 240: PS is the Group Extension and the low PGN byte is PS.
 */
static inline uint8_t n2k_pgn_pf(uint32_t pgn) { return (uint8_t)((pgn >> 8) & 0xFFU); }
static inline uint8_t n2k_pgn_ps(uint32_t pgn) { return (uint8_t)(pgn & 0xFFU); }
static inline uint8_t n2k_pgn_dp(uint32_t pgn) { return (uint8_t)((pgn >> 16) & 0x01U); }
static inline bool    n2k_pgn_is_pdu2(uint32_t pgn) { return n2k_pgn_pf(pgn) >= 240U; }

/* Convert a PGN to canonical form:
 * - For PDU1 (PF < 240), the low PGN byte must be 0x00.
 */
static inline uint32_t n2k_pgn_canonical(uint32_t pgn) {
    uint8_t pf = n2k_pgn_pf(pgn);
    if (pf < 240U) {
        return (pgn & 0x03FF00U); /* DP:bit16, PF:bits[15..8], PS forced 0x00 */
    }
    return pgn & 0x03FFFFU; /* DP:bit16 + PF + PS */
}

/* ---- Parsed PDU ---- */
typedef struct {
    /* ID fields */
    uint8_t  priority;  /* 0..7 */
    uint8_t  edp;       /* Usually 0 for N2K */
    uint8_t  dp;        /* Data Page bit */
    uint8_t  pf;        /* PDU Format */
    uint8_t  ps;        /* PDU Specific (DA for PDU1, GE for PDU2) */
    uint8_t  sa;        /* Source Address */

    /* PGN/addressing */
    uint32_t pgn;       /* Canonical PGN (low byte is 0x00 for PDU1) */
    uint8_t  dst;       /* 0xFF for PDU2/broadcast, otherwise DA */
    bool     is_pdu2;   /* PF >= 240 */

    /* Payload (single-frame layer, up to 8 bytes) */
    uint8_t  len;
    uint8_t  data[8];

    /* Metadata */
    uint64_t timestamp_us; /* Receive/transmit timestamp (esp_timer_get_time) */
} n2k_ll_frame_t;

/* ---- ID packing/parsing ---- */
uint32_t n2k_pack_id(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da_or_ge);
void     n2k_parse_id(uint32_t id, n2k_ll_frame_t *out);

/* ---- I/O over can_driver (single-frame layer, up to 8 bytes) ---- */
esp_err_t n2k_ll_send(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t da,
                      const uint8_t *payload, uint8_t len, uint32_t timeout_ms);

esp_err_t n2k_ll_receive(n2k_ll_frame_t *out, uint32_t timeout_ms);

/* ---- Validate a single-frame PGN ---- */
static inline bool n2k_ll_len_valid(uint8_t len) { return len <= 8U; }

#ifdef __cplusplus
}
#endif
