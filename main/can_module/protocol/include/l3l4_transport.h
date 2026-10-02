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
 * Metadata (header) and a pointer to the application payload.
 * Internal payload wire format (for reference):
 *   [SIG0 SIG1 SIG2][SER][PAGE=0][CHANNEL][LEN_LE_L][LEN_LE_H][APP...]
 *
 * On TX:
 *   - Fill sig[3], series, channel_le, app_data, and app_len.
 *   - The module writes LEN_LE = app_len and packs the block into Fast-Packet.
 *
 * On RX:
 *   - The module fills this structure and invokes the callback after assembling all NDP pages.
 *   - app_data and app_len are valid only during the callback; copy the data if it must be retained.
 */
typedef struct {
    uint8_t        sig[3];       /**< Three-byte block signature, e.g. {SP_BIG_SIG_A, SP_BIG_SIG_B, SP_BIG_SIG_C} */
    uint8_t        series;       /**< One-byte series counter, e.g. 0x08..0x0F */
    uint16_t       channel_le;   /**< One byte: remote_port<<4 | local_port; high byte is zero */
    uint8_t        page_idx;     /**< Zero: RX continuation pages are assembled before delivery */
    uint8_t        page_final;   /**< 1 for a fully assembled NDP message */
    const uint8_t *app_data;     /**< Pointer to the APP payload, excluding the header */
    uint16_t       app_len;      /**< APP payload length in bytes */
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
 * Encoding details:
 *   - The payload (length in byte 1 of the first frame) is encoded as:
 *       [SIG3][SER][PAGE=0][CHANNEL][LEN_LE(2)=blk->app_len][APP...]
 *   - The first frame carries six bytes (SIG3,SER,PAGE,CHANNEL), followed by seven-byte continuation chunks:
 *       first two LEN_LE bytes, then the APP payload in seven-byte chunks.
 *   - All frames use DLC=8; unused bytes are padded with 0xFF.
 *   - Fast-Packet bounds: each payload is at most 223 bytes; an NDP message of up to 512 bytes spans multiple FP pages.
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
 *   Byte5 = cnt  (ACK sequence 0..7)
 *   Byte6 = 0x00 (page)
 *   Byte7 = ch   (channel byte)
 *
 * @param id   CAN ID fields (pri,dp,pf,ps,sa)
 * @param cnt  ACK sequence (0..7)
 * @param ch   Channel byte (remote_port<<4 | local_port)
 */
sp_err_t sp_tr_send_short_ack(const sp_id_fields_t *id, uint8_t cnt, uint16_t ch);

#ifdef __cplusplus
}
#endif
