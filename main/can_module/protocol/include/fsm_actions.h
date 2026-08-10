/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   FSM TX actions; rules and routing live in strategy_map.
 */

#pragma once
#include <stdint.h>
#include "sailor_proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialization and peer-address updates */
sp_err_t sp_actions_init(void);
void     sp_actions_set_peer_sa(uint8_t sa);

/* Set/get the 8-byte TCU NAME. The FSM must set it during initialization. */
void     sp_actions_set_tcu_name(const uint8_t name[8]);
const uint8_t* sp_actions_get_tcu_name(void);
void     sp_actions_reset_short_ack_a0(void);

/* ======================== ACK for SLink flow control (ch=0x40 to ch=0x04) ========================
 *
 * Supported MT variants send ch=0x40 in two forms:
 *   - len=3: respond with a short ch=0x04 len=0 packet containing only the
 *            6-byte NDP header, without a length field or payload.
 *   - len=16: respond with ch=0x04 len=16 [tcu_serial_8][mt_serial_8].
 *             Serial numbers come from the FSM context.
 *             If tcu_serial or mt_serial is NULL, send an empty short ACK.
 *
 * MT may send ch=0x40 at any time after connection setup without waiting for the
 * handshake to finish. Send the ACK immediately regardless of the current FSM state.
 */
sp_err_t sp_action_send_ack_ch04(uint8_t incoming_app_len,
                                  const uint8_t *tcu_serial,  /* 8 bytes or NULL */
                                  const uint8_t *mt_serial);  /* 8 bytes or NULL */

/* ======================== Connection Request / Accept handshake ========================
 *
 * Handshake step 1: addressed TCU-to-MT packet, ch=0x13, len=16,
 * payload=[tcu_serial_8][mt_serial_8].
 * CAN ID: 0DEF[mt][tcu] (PRI=3, DP=0, PF=EF, PS=peer_sa, SA=local_sa).
 *
 * tcu_serial and mt_serial are required and must each contain 8 bytes.
 * Returns SP_E_STATE when peer_sa is unknown.
 */
sp_err_t sp_action_send_conn_req(const uint8_t tcu_serial[8],
                                  const uint8_t mt_serial[8]); 

/* Single-frame EF TLV keepalive wire format, e.g. 5F 99 04 58 02 <ctr>. */
sp_err_t sp_action_send_bcast_ef_small(void);

/* EE status (8 bytes) */
sp_err_t sp_action_send_bcast_ee_status(void);

/* EA pointer (3 bytes: 00 EE 00) */
sp_err_t sp_action_send_bcast_ea_pointer(void);

/* Short Fast-Packet keepalive (total_len=0x06) through L2.
 * Data format:
 *   [ (SID<<5)|0 ][ 0x06 ][ 5F 99 02 ][ cnt ][ 00 ][ ch_lo ]
 * ch_lo:
 *   - 0xFF: broadcast echo (PRI=3, DP=0)
 *   - addressed channel low byte: 0x0A, 0xA0, 0x13, 0x04, ...
 *     (PRI=6, DP=1; requires a known peer_sa)
 */
sp_err_t sp_action_send_term_keepalive6(uint8_t cnt, uint8_t ch_lo);

/* ======================== Large frames (L3/L4 Fast-Packet; transport adds FF padding) ======================== */

/* Two-phase compatibility ANNOUNCE: capabilities, then full after ANN_CAP_GAP_MS. */
sp_err_t sp_action_send_announce_capabilities(void);
sp_err_t sp_action_send_announce_full(void);

/* Optional legacy API: broadcast ANNOUNCE (SIG=5F 99 03, CH=0x0000) in one call. */
sp_err_t sp_action_send_big_ef_announce(uint8_t seq_hint);

/* Short addressed dialog request (DP=1, PS=peer), matching observed devices. */
sp_err_t sp_action_send_dialog_req(void);

/* Service KA/data on specific addressed channels (DP=1, PS=peer):
 *  - 0x0013: initial snapshot/initiator
 *  - 0x0004: companion keepalive/empty packet
 */
sp_err_t sp_action_send_service_keepalive_0013(void);
sp_err_t sp_action_send_service_keepalive_0004(void);

/* Arbitrary payload on service channels, packed into Fast-Packet. */
sp_err_t sp_action_send_service_0013_payload(const uint8_t *app, uint16_t app_len);
sp_err_t sp_action_send_service_0004_payload(const uint8_t *app, uint16_t app_len);

/* Send a generic large snapshot (24-byte payload from local/peer EE data) on a channel.
 * ch_le   - little-endian channel code, e.g. 0x0013 or 0x00FF
 * series  - series number, following the reference sequence 0x10, 0x11, 0x12, ...
 * ee1/ee2 - 8 bytes each; NULL fills the corresponding block with zeros
 */
sp_err_t sp_action_send_big_snapshot(uint16_t ch_le,
                                     uint8_t  series,
                                     const uint8_t ee1[8],
                                     const uint8_t ee2[8]);

/* Pre-open snapshot sent twice: first CH=0x0013 (series_0013), then CH=0x00FF
 * (series_00ff). The 24-byte payload combines ee1/ee2 with an internal section.
 * If ee1 or ee2 is NULL, its corresponding 8-byte block is zero-filled.
 */
sp_err_t sp_action_send_preopen_snapshot_dual(const uint8_t ee1[8],
                                              const uint8_t ee2[8],
                                              uint8_t series_0013,
                                              uint8_t series_00ff);

/* ======================== CTRL (0DEF) TX helpers for protocol v7 ======================== */

/* Short heartbeat ACK: 0DEF[peer][local] ch=0x13 len=0 */
sp_err_t sp_action_send_ctrl_ack_0013(void);

/* Terminal Open: 0DEF[peer][local] ch=0xA0 len=16 [tcu_serial][mt_serial] */
sp_err_t sp_action_send_terminal_open_ctrl(const uint8_t tcu_serial[8],
                                           const uint8_t mt_serial[8]);

/* Token exchange: 0DEF[peer][local] ch=0xFF len=16 [tcu_serial][mt_serial] */
sp_err_t sp_action_send_token_ctrl_ff(const uint8_t tcu_serial[8],
                                      const uint8_t mt_serial[8]);

/* Short ACK after prompt: 0DEF[peer][local] ch=0xA0 len=0 */
sp_err_t sp_action_send_short_ack_a0(void);
/* Short ACK after prompt with an explicit series, usually echoing the incoming ch=0x0A block series. */
sp_err_t sp_action_send_short_ack_a0_series(uint8_t series);
/* Short ACK for the service/PPP stream: 0DEF[peer][local] ch=0xFF len=0 */
sp_err_t sp_action_send_short_ack_ff(void);
sp_err_t sp_action_send_conn_accept(const uint8_t tcu_serial[8],
                                    const uint8_t mt_serial[8]);
sp_err_t sp_action_send_ctrl_ack_0031(void);
sp_err_t sp_action_send_token_echo_ctrl_ff(const uint8_t token16[16]);
#ifdef __cplusplus
}
#endif
