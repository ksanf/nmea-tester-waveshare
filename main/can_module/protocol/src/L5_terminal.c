/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "l5_terminal.h"
#include "l5_service.h"
#include <string.h>

typedef struct {
    sp_term_rx_cb_t cb;
    void           *user;
    bool            inited;
    sp_channel_t     tx_ch;
} term_ctx_t;

static term_ctx_t g;

sp_err_t sp_term_init(const sp_term_config_t *cfg, sp_term_rx_cb_t on_rx, void *user)
{
    (void)cfg;
    if (g.inited) return SP_E_STATE;
    if (!on_rx)   return SP_E_INVAL;
    memset(&g, 0, sizeof(g));
    g.cb   = on_rx;
    g.user = user;
    g.tx_ch = SP_CH_TERM_IN;
    g.inited = true;
    return SP_OK;
}

void sp_term_deinit(void){ memset(&g, 0, sizeof(g)); }

/* TX builds only the APP block; strategy supplies route/ID/channel and L3L4 adds FP/FF framing. */
sp_err_t sp_term_send(const uint8_t *data, uint16_t len, uint8_t series_cnt)
{
    if (!g.inited) return SP_E_STATE;
    if (!data || !len) return SP_E_INVAL;

    if (g.tx_ch == SP_CH_SERVICE_APP) {
        /* PPP/service mode uses L5.Service path (CH=0xFF). */
        (void)series_cnt;
        return sp_service_send(data, len, 0xFF, NULL);
    }

    /* Strategy metadata (sig3+channel): TERM_IN (legacy) or SERVICE_APP (PPP). */
    sp_app_block_t blk = {0};
    const sp_flow_t flow = SP_FLOW_TERM_IN;
    if (!sp_strat_fill_app_header(flow, &blk)) return SP_E_INVAL;
    blk.series  = series_cnt;
    blk.app_data = data;
    blk.app_len  = len;

    /* CAN ID from the strategy */
    sp_id_fields_t id; sp_strat_build_id(flow, &id);

    /* Transport manages SID when io_sid is NULL. */
    return sp_tr_send_app(&id, &blk, /*io_sid*/NULL);
}

sp_err_t sp_term_send_byte(uint8_t ch, uint8_t series_cnt)
{
    return sp_term_send(&ch, 1u, series_cnt);
}

void sp_term_set_tx_channel(sp_channel_t ch)
{
    if (!g.inited) return;
    if (ch == SP_CH_TERM_IN || ch == SP_CH_SERVICE_APP) {
        g.tx_ch = ch;
    }
}

/* RX from transport:
 * - TERM_OUT (0x0A): conventional terminal output
 * - SERVICE_APP (0xFF): PPP/HDLC stream used by some MT revisions */
void sp_term_on_app(sp_channel_t ch, uint8_t series_cnt,
                    const uint8_t *app_data, uint16_t app_len,
                    const sp_id_fields_t *id)
{
    (void)series_cnt; (void)id;
    if (!g.inited || !app_data || !app_len) return;
    if (ch != SP_CH_TERM_OUT && ch != SP_CH_SERVICE_APP) return;

    bool is_echo = (app_len == 1);
    if (g.cb) g.cb(app_data, app_len, is_echo, g.user);
}
