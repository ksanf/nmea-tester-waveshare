/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   FSM TX actions: handshake, Terminal Open, token/ACK, flow-control ACK, announce/keepalive.
 */

#include "fsm_actions.h"

#include <string.h>

#include "bridge_can_config.h"
#include "strategy_map.h"
#include "l2_link.h"
#include "l3l4_transport.h"

#define TLV_A 0x5F
#define TLV_B 0x99
#define TLV_C 0x04

#define ANN_SERIES 0x08

/* CTRL/TERM/ANN signature used by this NDP v2 implementation */
#define SIG_A 0x5F
#define SIG_B 0x99
#define SIG_C 0x02

/* Module versioning */
#define SP_FSM_ACTIONS_VERSION     "6.0.0"
#define SP_FSM_ACTIONS_BUILD_DATE  __DATE__
#define SP_FSM_ACTIONS_BUILD_TIME  __TIME__

/* Channels (LE) */
#define CH_CONN_REQ_0013   0x0013u
#define CH_CONN_ACC_0031   0x0031u
#define CH_TOKEN_00FF      0x00FFu
#define CH_TERM_OPEN_A0    0x00A0u
#define CH_TERM_OUT_000A   0x000Au
#define CH_FC_ACK_0004     0x0004u

#define SP_DATA_SERIES_FIRST 0x08u
#define SP_DATA_SERIES_LAST  0x0Fu

typedef struct {
    uint8_t ctrl_series;   /* 0DEF: conn/open/token/short-ack */
    uint8_t data_series;   /* 19EF/15EF heartbeat/terminal streams, when sent */
    uint8_t bcast_series;  /* 0CEF short TLV broadcast counter (must not share DATA series) */
    uint8_t fc_series;     /* 0DEF flow-control ACK; timing-critical */
    uint8_t short_series_ff; /* dedicated short ACK counter for ch=0xFF */
    uint8_t short_series_13; /* dedicated short ACK counter for ch=0x13 */
    uint8_t short_series_31; /* dedicated short ACK counter for ch=0x31 */
    uint8_t short_series_a0; /* dedicated short ACK counter for ch=0xA0 */

    uint8_t tcu_name[8];
    bool    name_valid;
} act_ctx_t;

static act_ctx_t g;

static inline uint8_t data_series_next_(uint8_t s)
{
    if (s < SP_DATA_SERIES_FIRST || s >= SP_DATA_SERIES_LAST) return SP_DATA_SERIES_FIRST;
    return (uint8_t)(s + 1u);
}

sp_err_t sp_actions_init(void)
{
    memset(&g, 0, sizeof(g));
    /* Compatible devices start control/service series counters at 0x10.
     * Some devices ignore zero values, so start at 0x10.
     */
    g.ctrl_series = 0x10;
    g.data_series = SP_DATA_SERIES_FIRST;
    g.bcast_series = 0x10;
    g.fc_series   = 0x10;
    g.short_series_ff = 0x00;
    g.short_series_13 = 0x00;
    g.short_series_31 = 0x00;
    g.short_series_a0 = 0x00;
    /* Action-level logging is intentionally omitted from this translation unit. */
    return SP_OK;
}

void sp_actions_reset_short_ack_a0(void)
{
    g.short_series_a0 = 0x00;
}

void sp_actions_set_peer_sa(uint8_t sa)
{
    (void)sp_strat_set_peer_sa(sa);
}

void sp_actions_set_tcu_name(const uint8_t name[8])
{
    if (!name) return;
    memcpy(g.tcu_name, name, 8);
    g.name_valid = true;
}

const uint8_t* sp_actions_get_tcu_name(void)
{
    return g.name_valid ? g.tcu_name : NULL;
}

/* ===== Helpers ===== */

static inline sp_err_t send_ctrl_app_(uint16_t ch_le, uint8_t series, const uint8_t *app, uint16_t app_len)
{
    sp_app_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.sig[0]     = SIG_A;
    blk.sig[1]     = SIG_B;
    blk.sig[2]     = SIG_C;
    blk.series     = series;
    blk.channel_le = ch_le;
    blk.app_data   = (uint8_t*)app;
    blk.app_len    = app_len;

    sp_id_fields_t id;

    /* Select the route strictly from the strategy map. */
    switch (ch_le) {
    case CH_CONN_REQ_0013: sp_strat_build_id(SP_FLOW_CTRL_CH_0013, &id); break;
    case CH_TERM_OPEN_A0:  sp_strat_build_id(SP_FLOW_CTRL_CH_00A0, &id); break;
    case CH_TOKEN_00FF:    sp_strat_build_id(SP_FLOW_CTRL_CH_00FF, &id); break;
    case CH_FC_ACK_0004:   sp_strat_build_id(SP_FLOW_CTRL_CH_0004, &id); break;
    case CH_CONN_ACC_0031:
        /* Normally unused by TCU; retained for tests. */
        sp_strat_build_id(SP_FLOW_CTRL_CH_0013, &id);
        break;
    case CH_TERM_OUT_000A:
        /* Compatible devices send Open ACK 0x0A as CTRL (0DEF) during opening. */
        sp_strat_build_id(SP_FLOW_CTRL_CH_00A0, &id);
        break;
    default:
        return SP_E_INVAL;
    }

    return sp_tr_send_app(&id, &blk, NULL);
}

static inline sp_err_t send_ctrl_short_(uint16_t ch_le, uint8_t series)
{
    sp_id_fields_t id;
    switch (ch_le) {
    case CH_CONN_REQ_0013: sp_strat_build_id(SP_FLOW_CTRL_CH_0013, &id); break;
    case CH_TERM_OPEN_A0:  sp_strat_build_id(SP_FLOW_CTRL_CH_00A0, &id); break;
    case CH_TOKEN_00FF:    sp_strat_build_id(SP_FLOW_CTRL_CH_00FF, &id); break;
    case CH_FC_ACK_0004:   sp_strat_build_id(SP_FLOW_CTRL_CH_0004, &id); break;
    case CH_CONN_ACC_0031: sp_strat_build_id(SP_FLOW_CTRL_CH_0013, &id); break;
    case CH_TERM_OUT_000A: sp_strat_build_id(SP_FLOW_CTRL_CH_00A0, &id); break;
    default:
        return SP_E_INVAL;
    }
    return sp_tr_send_short_ack(&id, series, ch_le);
}

static inline sp_err_t send_short_(sp_flow_t flow, const uint8_t *p, uint8_t n)
{
    sp_id_fields_t id;
    sp_strat_build_id(flow, &id);
    return sp_l2_send(&id, p, n);
}

static sp_err_t send_fp_(sp_flow_t flow, uint8_t series, const uint8_t *app, uint16_t app_len)
{
    if (!SP_STRAT_TABLE[flow].bcast && sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    sp_app_block_t blk = (sp_app_block_t){0};
    (void)sp_strat_fill_app_header(flow, &blk);
    blk.series   = series;
    blk.app_data = (uint8_t*)app;
    blk.app_len  = app_len;

    sp_id_fields_t id;
    sp_strat_build_id(flow, &id);
    return sp_tr_send_app(&id, &blk, NULL);
}

/* ===== Flow control ACK (CH=0x0004) ===== */
sp_err_t sp_action_send_ack_ch04(uint8_t incoming_app_len,
                                 const uint8_t *tcu_serial,
                                 const uint8_t *mt_serial)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    /* Some MT variants require a 16-byte payload; others accept SHORT. */
    if (incoming_app_len == 16 && tcu_serial && mt_serial) {
        uint8_t app[16];
        memcpy(&app[0], tcu_serial, 8);
        memcpy(&app[8], mt_serial,  8);
        return send_ctrl_app_(CH_FC_ACK_0004, g.fc_series++, app, sizeof(app));
    }

    return send_ctrl_short_(CH_FC_ACK_0004, g.fc_series++);
}

/* ===== Handshake ===== */
sp_err_t sp_action_send_conn_req(const uint8_t tcu_serial[8],
                                 const uint8_t mt_serial[8])
{
    if (!tcu_serial || !mt_serial) return SP_E_INVAL;
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    uint8_t app[16];
    memcpy(&app[0],  tcu_serial, 8);
    memcpy(&app[8],  mt_serial,  8);

    /* Observed compatibility behavior:
     * TCU ConnReq uses series=0x10 on each connection cycle.
     * Reusing rolling ctrl_series here shifts subsequent MT replies and
     * breaks parity with Terminal Open / OpenACK window. */
    return send_ctrl_app_(CH_CONN_REQ_0013, 0x10u, app, sizeof(app));
}

sp_err_t sp_action_send_conn_accept(const uint8_t tcu_serial[8],
                                    const uint8_t mt_serial[8])
{
    /* TCU normally does not send accept; retained for test benches and simulation. */
    if (!tcu_serial || !mt_serial) return SP_E_INVAL;
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    uint8_t app[16];
    memcpy(&app[0],  mt_serial,  8);
    memcpy(&app[8],  tcu_serial, 8);

    return send_ctrl_app_(CH_CONN_ACC_0031, g.ctrl_series++, app, sizeof(app));
}

sp_err_t sp_action_send_ctrl_ack_0013(void)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    uint8_t cnt = (uint8_t)(g.short_series_13 & 0x07u);
    g.short_series_13 = (uint8_t)((g.short_series_13 + 1u) & 0x07u);
    return send_ctrl_short_(CH_CONN_REQ_0013, cnt);
}

sp_err_t sp_action_send_ctrl_ack_0031(void)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    uint8_t cnt = (uint8_t)(g.short_series_31 & 0x07u);
    g.short_series_31 = (uint8_t)((g.short_series_31 + 1u) & 0x07u);
    return send_ctrl_short_(CH_CONN_ACC_0031, cnt);
}

/* ===== Terminal Open ===== */
sp_err_t sp_action_send_terminal_open_ctrl(const uint8_t tcu_serial[8],
                                           const uint8_t mt_serial[8])
{
    if (!tcu_serial || !mt_serial) return SP_E_INVAL;
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    uint8_t app[16];
    memcpy(&app[0], tcu_serial, 8);
    memcpy(&app[8], mt_serial,  8);

    /* Observed compatibility behavior:
     * Terminal Open uses series=0x10, MT replies with OpenACK series=0x11.
     * A rolling series here yields 0x11/0x12 and MT withholds prompt. */
    return send_ctrl_app_(CH_TERM_OPEN_A0, 0x10u, app, sizeof(app));
}

/* Byte-for-byte token echo retained for diagnostics. */
sp_err_t sp_action_send_token_echo_ctrl_ff(const uint8_t token16[16])
{
    if (!token16) return SP_E_INVAL;
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    return send_ctrl_app_(CH_TOKEN_00FF, g.ctrl_series++, token16, 16);
}

/* BACKWARD COMPATIBILITY: legacy interface, not a token echo.
 * Sends payload=[TCU][MT] on CH=0x00FF.
 * TT-3027C tunnel opening normally requires the FSM token exchange, so this
 * function is not recommended.
 */
sp_err_t sp_action_send_token_ctrl_ff(const uint8_t tcu_serial[8],
                                      const uint8_t mt_serial[8])
{
    if (!tcu_serial || !mt_serial) return SP_E_INVAL;
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;

    uint8_t app[16];
    memcpy(&app[0],  tcu_serial, 8);
    memcpy(&app[8],  mt_serial,  8);
    return send_ctrl_app_(CH_TOKEN_00FF, g.ctrl_series++, app, sizeof(app));
}

/* Short ACK A0 after the prompt */
sp_err_t sp_action_send_short_ack_a0(void)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    uint8_t cnt = (uint8_t)(g.short_series_a0 & 0x07u);
    g.short_series_a0 = (uint8_t)((g.short_series_a0 + 1u) & 0x07u);
    return send_ctrl_short_(CH_TERM_OPEN_A0, cnt);
}

sp_err_t sp_action_send_short_ack_a0_series(uint8_t series)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    /* Keep legacy counter roughly in sync for callers that still use auto mode. */
    g.short_series_a0 = (uint8_t)((series + 1u) & 0x07u);
    return send_ctrl_short_(CH_TERM_OPEN_A0, series);
}

/* Short ACK FF for the service/PPP stream */
sp_err_t sp_action_send_short_ack_ff(void)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    uint8_t cnt = (uint8_t)(g.short_series_ff & 0x07u);
    g.short_series_ff = (uint8_t)((g.short_series_ff + 1u) & 0x07u);
    return send_ctrl_short_(CH_TOKEN_00FF, cnt);
}

/* ===== SHORT FRAMES (L2) ===== */
sp_err_t sp_action_send_bcast_ef_small(void)
{
    uint8_t f[6] = { TLV_A, TLV_B, TLV_C, 0x58, 0x02, g.bcast_series++ };
    return send_short_(SP_FLOW_TLV_SHORT, f, sizeof(f));
}

sp_err_t sp_action_send_bcast_ee_status(void)
{
    if (!g.name_valid) return SP_E_STATE;
    return send_short_(SP_FLOW_EE_STATUS, g.tcu_name, 8);
}

sp_err_t sp_action_send_bcast_ea_pointer(void)
{
    const uint8_t f[3] = { 0x00, 0xEE, 0x00 };
    return send_short_(SP_FLOW_EA_POINTER, f, sizeof(f));
}

/* ===== LARGE FRAMES: ANNOUNCE ===== */
static sp_err_t send_announce_app_(uint8_t series, const uint8_t *app, uint16_t app_len)
{
    if (!app || !app_len) return SP_E_INVAL;
    return send_fp_(SP_FLOW_ANNOUNCE_LARGE_TLV_BCAST, series ? series : ANN_SERIES, app, app_len);
}

sp_err_t sp_action_send_announce_capabilities(void)
{
    static const uint8_t app1[1] = { 0x01 };
    return send_announce_app_(ANN_SERIES, app1, sizeof(app1));
}

sp_err_t sp_action_send_announce_full(void)
{
    if (!g.name_valid) return SP_E_STATE;

    static const uint8_t mid_fixed[8]  = { 0x03, 0x00, 0x00, 0x00, 0x01, 0x07, 0x03, 0x00 };
    static const uint8_t tail_fixed[6] = { 0x04, 0x00, 0x0F, 0x05, 0x00, 0x04 };

    uint8_t app[0x38];
    memset(app, 0, sizeof(app));

    app[0] = 0x02; /* pkt_type = full announce */
    app[1] = 0x01; /* device_class = TCU */

    memcpy(&app[2],  g.tcu_name, 8);
    memcpy(&app[10], mid_fixed, 8);

    const char *sn = BRIDGE_CAN_DEVICE_SERIAL;
    size_t sn_len = strlen(sn);
    if (sn_len > 10) sn_len = 10;
    memcpy(&app[18], sn, sn_len);

    memcpy(&app[50], tail_fixed, 6);

    return send_announce_app_(ANN_SERIES, app, sizeof(app));
}

/* ===== ADDRESSED service channels ===== */
sp_err_t sp_action_send_service_keepalive_0013(void)
{
    const uint8_t s = g.data_series;
    g.data_series = data_series_next_(g.data_series);
    return send_fp_(SP_FLOW_CTRL_CH_0013, s, NULL, 0);
}

sp_err_t sp_action_send_service_keepalive_0004(void)
{
    const uint8_t s = g.data_series;
    g.data_series = data_series_next_(g.data_series);
    return send_fp_(SP_FLOW_CTRL_CH_0004, s, NULL, 0);
}

sp_err_t sp_action_send_service_0013_payload(const uint8_t *app, uint16_t app_len)
{
    if (!app && app_len) return SP_E_INVAL;
    const uint8_t s = g.data_series;
    g.data_series = data_series_next_(g.data_series);
    return send_fp_(SP_FLOW_DATA_CH_0013, s, app, app_len);
}

sp_err_t sp_action_send_service_0004_payload(const uint8_t *app, uint16_t app_len)
{
    if (!app && app_len) return SP_E_INVAL;
    const uint8_t s = g.data_series;
    g.data_series = data_series_next_(g.data_series);
    return send_fp_(SP_FLOW_CTRL_CH_0004, s, app, app_len);
}

sp_err_t sp_action_send_dialog_req(void)
{
    if (sp_strat_get_peer_sa() == 0xFF) return SP_E_STATE;
    return sp_action_send_service_keepalive_0013();
}

sp_err_t sp_action_send_preopen_snapshot_dual(const uint8_t ee1[8], const uint8_t ee2[8],
                                              uint8_t series_0013, uint8_t series_00ff)
{
    /* Retained unchanged for legacy diagnostics. */
    (void)ee1; (void)ee2; (void)series_0013; (void)series_00ff;
    return SP_OK;
}

sp_err_t sp_action_send_big_ef_announce(uint8_t seq_hint)
{
    (void)seq_hint;
    return sp_action_send_announce_full();
}

sp_err_t sp_action_send_big_snapshot(uint16_t ch_le, uint8_t series,
                                     const uint8_t ee1[8], const uint8_t ee2[8])
{
    (void)ch_le; (void)series; (void)ee1; (void)ee2;
    /* Not used in the current TCU-emulation path. */
    return SP_E_INVAL;
}

sp_err_t sp_action_send_term_keepalive6(uint8_t cnt, uint8_t ch_lo)
{
    (void)cnt; (void)ch_lo;
    return sp_action_send_bcast_ef_small();
}
