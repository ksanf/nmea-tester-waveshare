/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   FSM for handshake/announce/online states, layer binding, and routing.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sailor_proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== States ============================== */
typedef enum {
    SP_ST_BOOT = 0,     /* Initial state; Claim has not been sent yet */
    SP_ST_CLAIM,        /* Waiting for Address Claim confirmation     */
    SP_ST_WAIT_POLL,    /* Waiting for communication with the antenna */
    SP_ST_ONLINE,       /* Terminal session is active                 */
    SP_ST_OFF           /* Disabled / Cannot Claim                    */
} sp_state_t;

/* ============================== FSM configuration ============================== */
typedef struct {
    sp_timing_t timing;        /* Protocol timing in ms; reserved             */
    uint8_t     local_sa;      /* Preferred SA (0 selects default 0x03)       */
    uint8_t     peer_sa;       /* Peer SA (0 enables automatic discovery)     */
    uint8_t     tcu_serial[8]; /* TCU NAME (8-byte LE64) for Address Claim;
                                * all zeros select the firmware default       */
} sp_fsm_config_t;

/* ============================== Actions (TX) ============================== */
typedef struct {
    /* Short broadcasts (L2, without FF padding): */
    sp_err_t (*send_bcast_ef_small)(void);
    sp_err_t (*send_bcast_ee_status)(void);
    sp_err_t (*send_bcast_ea_pointer)(void);

    /* Large EF frames through L3L4 (legacy API): */
    sp_err_t (*send_big_ef_announce)(uint8_t seq_hint);

    /* Addressed service traffic: */
    sp_err_t (*send_dialog_req)(void);

    /* Two-phase ANNOUNCE: */
    sp_err_t (*send_bcast_announce_cap1)(void);
    sp_err_t (*send_bcast_announce_full)(void);

    /* Service keepalives: */
    sp_err_t (*send_service_keepalive_0013)(void);
    sp_err_t (*send_service_keepalive_0004)(void);

    /* Terminal KA6: */
    sp_err_t (*send_term_keepalive6)(uint8_t ka_idx_mod8, uint8_t ps_sel);

    /* Large snapshot (EE data): */
    sp_err_t (*send_big_snapshot)(uint16_t ch_le,
                                  uint8_t  series,
                                  const uint8_t ee_local[8],
                                  const uint8_t ee_peer[8]);
} sp_fsm_actions_t;

/* ============================== API FSM ============================== */
sp_err_t   sp_fsm_init       (const sp_fsm_config_t *cfg, const sp_fsm_actions_t *act);
void       sp_fsm_deinit     (void);

/**
 * sp_fsm_tick: FSM tick using monotonic time in milliseconds.
 * Must be called regularly (100 ms or less is recommended).
 * The first call in S_BOOT immediately sends Address Claim.
 */
void       sp_fsm_tick       (uint32_t now_ms);

/* External events: */
void       sp_fsm_on_peer_sa  (uint8_t sa);
void       sp_fsm_on_timeout  (void);
void       sp_fsm_set_state   (sp_state_t st);
sp_state_t sp_fsm_get_state   (void);

/**
 * sp_fsm_on_mt_serial: provide the FSM with the 8-byte MT serial number.
 * It comes from the NDP v3 broadcast presence/discovery message
 * (SIG=5F 99 03, ch=0x0000). EE00 (Address Claim) contains NAME rather than
 * mt_serial.
 */
void       sp_fsm_on_mt_serial(const uint8_t serial[8]);

/** Current peer SA, or 0xFF when unknown. */
uint8_t    sp_fsm_get_peer_sa (void);

/**
 * Terminal input pacing:
 * allow next TCU->MT ch=0xA0 block only when MT has ACKed the previous one
 * with short ctrl ACK on ch=0x0A.
 */
bool       sp_fsm_term_input_can_send(void);
void       sp_fsm_term_input_mark_sent(void);

/**
 * sp_fsm_build_term_id: build the EF ID for the terminal tunnel (TCU to MT).
 * Requires a known peer SA and FSM state SP_ST_ONLINE.
 */
void       sp_fsm_build_term_id(sp_id_fields_t *id);

/* ============================== Layer binding ============================== */
#include "l2_link.h"
#include "l3l4_transport.h"
#include "tlv_codec.h"

sp_err_t   sp_fsm_bind_layers (const sp_l2_config_t  *l2_cfg,
                               const sp_timing_t     *tr_timing,
                               uint16_t               tr_max_slots,
                               const sp_tlv_config_t *tlv_cfg,
                               sp_l2_evt_cb_t         on_l2_evt,
                               sp_tr_evt_cb_t         on_tr_evt,
                               void                  *user);

void       sp_fsm_unbind_layers(void);

#ifdef __cplusplus
}
#endif
