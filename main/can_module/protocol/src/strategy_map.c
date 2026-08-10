/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Sailor-CAN routing strategy implementation (SA cache, build_id, fill_app_header).
 */

#include "strategy_map.h"
#include <string.h>
#include <assert.h>

/* Address Claim selects the SA; it remains 0xFF until then. */
static uint8_t s_local_sa = 0xFF;
static uint8_t s_peer_sa  = 0xFF;

void sp_strat_set_local_sa(uint8_t sa){ s_local_sa = sa; }
void sp_strat_set_peer_sa (uint8_t sa){ s_peer_sa  = sa; }
uint8_t sp_strat_get_local_sa(void){ return s_local_sa; }
uint8_t sp_strat_get_peer_sa (void){ return (s_peer_sa<=0xFE)? s_peer_sa : 0xFF; }

void sp_strat_build_id(sp_flow_t flow, sp_id_fields_t *out_id)
{
    assert(out_id);
    const sp_strat_row_t *r = &SP_STRAT_TABLE[flow];
    out_id->pri = r->pri;
    out_id->dp  = r->dp;
    out_id->pf  = r->pf;
    out_id->sa  = s_local_sa;
    out_id->ps  = r->bcast ? 0xFF : s_peer_sa;
    assert(r->bcast || s_peer_sa != 0xFF);
}

bool sp_strat_fill_app_header(sp_flow_t flow, sp_app_block_t *blk)
{
    const sp_strat_row_t *r = &SP_STRAT_TABLE[flow];
    if (!blk) return false;
    if (r->channel_le == 0xFFFF) return false;
    blk->sig[0] = r->sig3[0];
    blk->sig[1] = r->sig3[1];
    blk->sig[2] = r->sig3[2];
    blk->channel_le = r->channel_le;
    return true;
}
