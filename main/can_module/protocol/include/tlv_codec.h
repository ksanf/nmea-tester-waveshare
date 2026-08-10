/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Parse short EF/EA/EE frames and extract short TLVs from large EF blocks on TLV_BCAST.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "sailor_proto_common.h"   /* sp_err_t, sp_id_fields_t, sp_channel_t */
#include "l2_link.h"               /* sp_l2_frame_t */
#include "l3l4_transport.h"        /* sp_app_block_t (shared L3L4 block) */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================= TLV value types ======================= */

typedef enum {
    SP_TLV_VAL_U8,
    SP_TLV_VAL_U16BE,
    SP_TLV_VAL_U32BE,
    SP_TLV_VAL_INDEX,
    SP_TLV_VAL_BYTES
} sp_tlv_val_type_t;

typedef uint8_t sp_tlv_tag_t;

/* Use local value 0xFF for Unknown if shared headers do not define it. */
#ifndef SP_TLV_TAG_UNKNOWN
#define SP_TLV_TAG_UNKNOWN ((sp_tlv_tag_t)0xFF)
#endif

typedef struct {
    sp_tlv_tag_t       tag;     /* 0x0C, 0x58, 0x8C, ... or SP_TLV_TAG_UNKNOWN */
    uint8_t            sub;     /* Subtag for 0x58, 0x0C, etc. */
    sp_tlv_val_type_t  vtype;   /* Value type below */
    union {
        uint8_t    u8;
        uint16_t   u16;
        uint32_t   u32;
        struct { const uint8_t *p; uint8_t len; } bytes; /* Pointer is valid during the callback. */
    } v;
} sp_tlv_item_t;

typedef struct {
    uint32_t dedup_ms;      /* EA/EE deduplication window (PGN+payload); 0 disables it */
    bool     detect_cycle;  /* Estimate the period for 0x0C/SUB=0x10 (idx 1..13) */
} sp_tlv_config_t;

typedef struct {
    bool     has_cycle;     /* True when tag 0x0C/SUB=0x10 is present in the stream */
    uint8_t  idx;           /* Last index in 1..13 */
    uint32_t period_ms;     /* EMA estimate between indices; 0 when unavailable */
} sp_tlv_cycle_info_t;

/* ======================= Upward RX callback ======================= */

/**
 * @brief Callback for parsed TLV results.
 *
 * @param pgn    Source-frame PGN (EF/EA/EE for L2; large-block EF for L3L4)
 * @param id     Source CAN ID fields (pri,dp,pf,ps,sa)
 * @param items  Array of parsed items
 * @param n_items Number of items in the array
 * @param ci     Cycle detector information for 0x0C/0x10; data may be unavailable
 * @param user   User context passed to init()
 */
typedef void (*sp_tlv_rx_cb_t)(
    uint32_t pgn,
    const sp_id_fields_t *id,
    const sp_tlv_item_t *items,
    uint8_t n_items,
    const sp_tlv_cycle_info_t *ci,
    void *user
);

/* ======================= API ======================= */

/**
 * @brief Initialize/deinitialize the TLV module.
 *
 * @param cfg   Deduplication/cycle configuration
 * @param on_rx Parsing callback; NULL makes the module consume inputs without reporting them
 * @param user  User context
 */
sp_err_t sp_tlv_init(const sp_tlv_config_t *cfg, sp_tlv_rx_cb_t on_rx, void *user);
void     sp_tlv_deinit(void);

/**
 * @brief L2 input for short single-frame EF/EA/EE messages.
 *
 * Pass every L2 frame here. The module:
 *  - attempts to parse a 6..8-byte TLV from EF frames with signature 5F 99 04,
 *  - deduplicates EA/EE, decodes EE into U16/U8 values, and otherwise returns BYTES.
 * These are raw short frames and must not contain FF padding.
 */
void     sp_tlv_on_l2_frame(const sp_l2_frame_t *frm);

/**
 * @brief L3L4 input for extracting short TLVs from a large EF block.
 *
 * Called only by the L3L4 transport for a parsed application block on
 * `SP_CH_TLV_BCAST` (strategy CH = 0x0000). The module scans `blk->app_data`,
 * extracts 6-, 7-, or 8-byte EF-TLV subsequences (5F 99 04 ...), and reports
 * the result through on_rx().
 *
 * @param id   CAN ID fields of the source FP stream (pri,dp,pf,ps,sa)
 * @param blk  Parsed application block; see l3l4_transport.h
 */
void     sp_tlv_on_app_block(const sp_id_fields_t *id, const sp_app_block_t *blk);

#ifdef __cplusplus
}
#endif
