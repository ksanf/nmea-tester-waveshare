/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Sailor-CAN L3+L4 transport: NMEA2000 Fast-Packet plus application blocks.
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "sailor_proto_common.h"  /* sp_err_t, sp_event_t, sp_id_fields_t, sp_l2_frame_t, sp_timing_t, sp_pgn_of */
#include <stdint.h>
#include <stdbool.h>

/* ===================== L3 constants (Fast-Packet) ===================== */

/* Standard Fast-Packet limits */
enum {
    SP_FP_MAX_PAYLOAD       = 223,   /* Maximum payload bytes in one FP block */
    SP_FP_FIRST_DATA_BYTES  = 6,     /* Byte2..7 in the first frame */
    SP_FP_NEXT_DATA_BYTES   = 7,     /* Byte1..7 in subsequent frames */
    SP_FP_MAX_FRAMES        = 32,    /* idx 0..31 */
    SP_FP_IDX_MASK          = 0x1F,  /* Low 5 bits of Byte0 */
    SP_FP_SID_MASK          = 0x07,  /* High 3 bits of Byte0 (SID 0..7) */
    SP_FP_SID_SHIFT         = 5
};

/* ===================== Application block (shared TX/RX structure) ===================== */

/**
 * @brief Shared application block structure for TX and RX.
 *
 * Contains header metadata plus a pointer to raw application data.
 * Internal payload wire format, for reference:
 *   [SIG0 SIG1 SIG2][SER][CHAN_LE_L][CHAN_LE_H][LEN_LE_L][LEN_LE_H][APP...]
 *
 * On TX:
 *   - Fill sig[3], series, channel_le, app_data, and app_len.
 *   - The module writes LEN_LE = app_len and packs the block into Fast-Packet.
 *
 * On RX:
 *   - The module fills the structure and invokes the callback.
 *   - app_data and app_len remain valid only during the callback; copy data if needed later.
 */
typedef struct {
    uint8_t        sig[3];       /**< 3-byte block signature, e.g. {SP_BIG_SIG_A, SP_BIG_SIG_B, SP_BIG_SIG_C} */
    uint8_t        series;       /**< 1-byte series counter, e.g. 0x08..0x0F */
    uint16_t       channel_le;   /**< Channel identifier in little-endian order */
    uint8_t        page_idx;     /**< TERM_OUT continuation page index from Byte4 (0x00/0x01/0x02/...) */
    uint8_t        page_final;   /**< 1 for the final FP block of a TERM_OUT page (wire_len < 223) */
    const uint8_t *app_data;     /**< Pointer to raw APP data without the header */
    uint16_t       app_len;      /**< APP data length in bytes */
} sp_app_block_t;

/* ===================== Upper-layer callbacks ===================== */

/**
 * @brief Callback for a complete received application block.
 *
 * @param id        CAN ID fields of the source frame (pri,dp,pf,ps,sa)
 * @param pgn       PGN derived from dp/pf
 * @param sid       Fast-Packet series identifier (0..7)
 * @param blk       Parsed application block; see sp_app_block_t
 * @param user      User context passed to sp_tr_init()
 */
typedef void (*sp_tr_app_rx_cb_t)(
    const sp_id_fields_t *id,
    uint32_t              pgn,
    uint8_t               sid,
    const sp_app_block_t *blk,
    void                 *user
);

/**
 * @brief Transport event callback for diagnostics.
 *
 * @param evt   Event code: SP_EVT_FP_TIMEOUT / SP_EVT_FP_LOST_SEQ / SP_EVT_FP_OVERLEN / SP_EVT_L2_RX_DROPPED
 * @param id    Frame identifier, when applicable
 * @param sid   Series SID, when applicable
 * @param user  User context
 */
typedef void (*sp_tr_evt_cb_t)(sp_event_t evt, const sp_id_fields_t *id, uint8_t sid, void *user);

/* ===================== Public API ===================== */

/**
 * @brief Initialize the L3+L4 transport.
 *
 * @param timing     Timing settings; only fp_block_timeout_ms is used. NULL selects 200 ms.
 * @param max_slots  Maximum concurrent FP series being assembled (8 recommended, minimum 1).
 * @param on_app     Required callback for a complete application block.
 * @param on_evt     Optional callback for timeout/sequence/overlength/no-slot events.
 * @param user       User context returned unchanged to callbacks.
 * @return           SP_OK or an error code.
 *
 * Constraints:
 *   - Uses FreeRTOS mutexes and heap; do not call from an ISR.
 *   - The internal buffer passed to on_app() is released immediately after the callback returns.
 */
sp_err_t sp_tr_init(const sp_timing_t *timing,
                    uint16_t max_slots,
                    sp_tr_app_rx_cb_t on_app,
                    sp_tr_evt_cb_t on_evt,
                    void *user);

/**
 * @brief Deinitialize and release all slots and resources.
 */
void     sp_tr_deinit(void);

/**
 * @brief RX entry point; pass every received L2 frame here.
 *
 * @param frm   Received L2 frame (dlc 0..8, data[8])
 *
 * Notes:
 *   - Call from task context, not an ISR, because this function uses a mutex.
 *   - The module assembles FP streams by {PGN, SA, (PS for PDU1), SID}.
 *   - on_app() is invoked exactly once for each completed block.
 */
void     sp_tr_on_l2_frame(const sp_l2_frame_t *frm);

/**
 * @brief Transmit an application block.
 *
 * @param id        CAN ID fields (pri,dp,pf,ps,sa)
 * @param blk       Block containing the header and raw application data
 * @param io_sid    NULL or *io_sid==0xFF selects SID automatically, cycling through 0..7.
 *                  Otherwise *io_sid in 0..7 is used and incremented modulo 8 after transmission.
 * @return          SP_OK or an error code.
 *
 * Packing details:
 *   - The payload represented by Byte1 of the first frame is built as:
 *       [SIG3][SER][CHAN_LE(2)][LEN_LE(2)=blk->app_len][APP...]
 *   - The first frame carries the first 6 bytes (SIG3,SER,CHAN_LE), followed by 7-byte chunks:
 *       first the 2-byte LEN_LE, then APP data in 7-byte chunks.
 *   - Every frame uses DLC=8 and unused bytes are padded with 0xFF.
 *   - The total Fast-Packet payload must not exceed 223 bytes.
 */
sp_err_t sp_tr_send_app(const sp_id_fields_t *id,
                        const sp_app_block_t *blk,
                        uint8_t *io_sid);

/**
 * @brief Transmit a SHORT ACK, a 6-byte NDP packet without a payload.
 *
 * Wire format: one CAN frame with DLC=8:
 *   Byte0 = (SID<<5)|0
 *   Byte1 = 0x06 (total_len = 6)
 *   Byte2 = 0x5F (magic)
 *   Byte3 = 0x99 (magic)
 *   Byte4 = 0x02 (NDP v2)
 *   Byte5 = cnt  (rolling counter)
 *   Byte6 = 0x00 (reserved)
 *   Byte7 = ch   (channel byte)
 *
 * @param id   CAN ID fields (pri,dp,pf,ps,sa)
 * @param cnt  Rolling counter inserted into byte 5 of the NDP header
 * @param ch   Channel byte (0x04, 0x13, 0xA0, etc.)
 */
sp_err_t sp_tr_send_short_ack(const sp_id_fields_t *id, uint8_t cnt, uint16_t ch);

#ifdef __cplusplus
}
#endif
