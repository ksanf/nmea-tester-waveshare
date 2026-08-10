/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Unified Sailor-CAN routing strategy: PF/DP/PRI/channels/signatures.
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "sailor_proto_common.h"
#include "l3l4_transport.h"

/* ===== Flows ===== */
typedef enum {
    /* --- Short flows sent directly through L2 --- */
    SP_FLOW_TLV_SHORT = 0,      /* 0CEF: EF short 5F 99 04 … (bcast, DP=0) */
    SP_FLOW_EA_POINTER,         /* 0CEA: EA 00 EE 00 (bcast, DP=0) */
    SP_FLOW_EE_STATUS,          /* 0CEE: EE00 NAME (bcast, DP=0) */

    /* --- Discovery/presence NDPv3 (Fast-Packet) --- */
    SP_FLOW_ANNOUNCE_LARGE_TLV_BCAST, /* 0DEFFF (DP=1, PF=EF, PS=FF): SIG=5F 99 03, bcast */

    /* --- CTRL (0DEF, DP=0, pri=3) --- */
    SP_FLOW_CTRL_CH_0013,       /* CH=0x0013 (conn/ACK short) */
    SP_FLOW_CTRL_CH_00A0,       /* CH=0x00A0 (Terminal Open + short ACK) */
    SP_FLOW_CTRL_CH_00FF,       /* CH=0x00FF (token) */
    SP_FLOW_CTRL_CH_0004,       /* CH=0x0004 (flow-control ACK) */

    /* --- DATA (DP=1) --- */
    SP_FLOW_DATA_CH_0013,       /* 19EF: heartbeat data CH=0x0013 (pri=6) */
    SP_FLOW_TERM_IN,            /* 19EF: terminal input CH=0x00A0 (pri=6) */
    SP_FLOW_TERM_OUT,           /* 15EF: terminal output CH=0x000A (pri=5) */
    SP_FLOW_PPP_DATA            /* 19EF: PPP/token stream CH=0x00FF (pri=6) */
} sp_flow_t;

typedef struct {
    uint8_t  pf;
    uint8_t  dp;
    uint8_t  pri;
    bool     bcast;
    uint8_t  sig3[3];
    uint16_t channel_le; /* Little-endian; 0xFFFF for short flows */
} sp_strat_row_t;

static const sp_strat_row_t SP_STRAT_TABLE[] = {
/* flow                               pf    dp  pri  bcast   sig3              channel  */
[SP_FLOW_TLV_SHORT]                = {0xEF, 0,  3,   true,  {0x00,0x00,0x00}, 0xFFFF},
[SP_FLOW_EA_POINTER]               = {0xEA, 0,  3,   true,  {0x00,0x00,0x00}, 0xFFFF},
[SP_FLOW_EE_STATUS]                = {0xEE, 0,  3,   true,  {0x00,0x00,0x00}, 0xFFFF},

/* Discovery/presence: 0DEFFF => DP=1 */
[SP_FLOW_ANNOUNCE_LARGE_TLV_BCAST] = {0xEF, 1,  3,   true,  {0x5F,0x99,0x03}, 0x0000},

/* CTRL (0DEF): DP=1, pri=3; compatible devices use DP=1 for unicast CTRL and DATA frames. */
[SP_FLOW_CTRL_CH_0013]             = {0xEF, 1,  3,   false, {0x5F,0x99,0x02}, 0x0013},
[SP_FLOW_CTRL_CH_00A0]             = {0xEF, 1,  3,   false, {0x5F,0x99,0x02}, 0x00A0},
[SP_FLOW_CTRL_CH_00FF]             = {0xEF, 1,  3,   false, {0x5F,0x99,0x02}, 0x00FF},
[SP_FLOW_CTRL_CH_0004]             = {0xEF, 1,  3,   false, {0x5F,0x99,0x02}, 0x0004},

/* DATA: DP=1 */
[SP_FLOW_DATA_CH_0013]             = {0xEF, 1,  6,   false, {0x5F,0x99,0x02}, 0x0013},
[SP_FLOW_TERM_IN]                  = {0xEF, 1,  6,   false, {0x5F,0x99,0x02}, 0x00A0},
[SP_FLOW_TERM_OUT]                 = {0xEF, 1,  5,   false, {0x5F,0x99,0x02}, 0x000A},
[SP_FLOW_PPP_DATA]                 = {0xEF, 1,  6,   false, {0x5F,0x99,0x02}, 0x00FF},
};

void sp_strat_set_local_sa(uint8_t sa);
void sp_strat_set_peer_sa (uint8_t sa);
uint8_t sp_strat_get_local_sa(void);
uint8_t sp_strat_get_peer_sa (void);

void sp_strat_build_id(sp_flow_t flow, sp_id_fields_t *out_id);
bool sp_strat_fill_app_header(sp_flow_t flow, sp_app_block_t *blk_meta);

#ifdef __cplusplus
}
#endif
