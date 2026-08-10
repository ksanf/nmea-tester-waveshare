/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   FSM: Address Claim + online/session + glue L2↔(TLV|Transport).
 */

#include "fsm.h"
#include "bridge_can_config.h"
#include "l2_link.h"
#include "l3l4_transport.h"
#include "tlv_codec.h"
#include "l5_terminal.h"
#include "l5_service.h"
#include "fsm_actions.h"
#include "strategy_map.h"

#include <string.h>
#include <stdint.h>

#define TAG "FSM"

/* ===== Timing (ms) ===== */
#define T_CLAIM_MS              300u
#define T_ANN_RETRY_MS          3000u
#define T_KEEPALIVE_MS          2000u   /* generic FSM pump cadence */
#define T_HB13_MS               2000u   /* observed compatible-device keepalive cadence */
#define T_HB13_PROBE_MS         2000u   /* pre-promotion probing should stay continuous */
#define T_HB13_PREOPEN_MS        300u   /* kick pre-session HB13 shortly before Terminal Open */
#define T_OFFLINE_SILENCE_MS    8000u   /* 8s: disconnect if MT silent (was 5s — too tight) */
#define T_OFFLINE_SILENCE_PPP_MS 30000u /* PPP/session mode can be bursty; avoid premature drop */
#define T_CONN_REQ_RETRY_MS     12000u  /* sparse compatibility retry cadence; avoid flooding */
#define T_PROMPT_WAIT_MS        3500u   /* some MT revisions emit pre-session PPP before prompt */
#define T_TUNNEL_REOPEN_MS      5000u
#define T_TUNNEL_REOPEN_MAX     3u
#define T_TERM_DUP_WINDOW_MS    4000u   /* duplicate TERM_OUT suppression window */
/* If MT misses/omits TERM_OUT credit for one keypress, do not deadlock local input forever. */
#define T_TERM_INPUT_CREDIT_TIMEOUT_MS 1200u

#ifndef ANN_CAP_GAP_MS
#define ANN_CAP_GAP_MS          350u
#endif

/* ===== Address Claim ===== */
#define PF_EE_CLAIM             0xEEu
#define PF_EA_REQUEST           0xEAu
#define PF_FE_COMMANDED         0xFEu
#define PS_FED8                 0xD8u
#define SA_NULL                 0xFEu
#define SA_BROADCAST            0xFFu
#define CLAIM_MAX_TRIES         8u
#define SA_PREFERRED_DEFAULT    0x00u

/* ===== TLV / SIG ===== */
#define TLV_SIG_A 0x5F
#define TLV_SIG_B 0x99
#define TLV_SIG_C 0x04

#define SIG_BIG_A    0x5F
#define SIG_BIG_B    0x99
#define SIG_BIG_ANN  0x03
#define SIG_BIG_TERM 0x02

#define SIG_CTRL_A 0x5F
#define SIG_CTRL_B 0x99
#define SIG_CTRL_C 0x02

/* Channels (LE) */
#define CH_CONN_REQ_0013    0x0013u
#define CH_CONN_ACC_0031    0x0031u
#define CH_FC_REQ_0040      0x0040u
#define CH_TERM_OPEN_A0     0x00A0u
#define CH_TERM_OUT_000A    0x000Au
#define CH_TOKEN_00FF       0x00FFu

#define PPP_PROTO_LCP       0xC021u
#define PPP_PROTO_IPCP      0x8021u

typedef enum {
    S_BOOT = 0,
    S_CLAIM,
    S_WAIT_POLL,
    S_ONLINE,
    S_OFF
} st_t;

typedef struct {
    uint8_t  sa_local;
    uint8_t  sa_peer;

    uint8_t  tcu_name[8];
    uint8_t  claim_tries;
    bool     claim_sent;

    /* peer identity */
    uint8_t  mt_serial[8];
    bool     mt_serial_valid;

    /* handshake */
    bool     conn_req_sent;
    bool     conn_accept_received;
    uint32_t t_conn_req_sent;

    /* terminal */
    enum {
        TERM_IDLE = 0,
        TERM_WAIT_APP,
        TERM_WAIT_PROMPT,
        TERM_READY
    } term_phase;
    bool term_open_ack_short_sent;
    bool ppp_stream_seen;
    bool hb13_promoted;   /* after MT 0x0031 app=09 01, use 08 40 payload */
    bool hb13_seen_0701;  /* guard promotion: 09 01 is valid only after 07 01 */
    uint8_t hb13_probe_tries;
    bool ppp_lcp_req_sent;
    bool ppp_ipcp_req_sent;
    bool ppp_lcp_echo_req_sent;
    uint8_t ppp_lcp_id;
    uint8_t ppp_ipcp_id;
    bool app_seen_lcp;
    bool app_seen_ipcp;
    bool app_post_conn_seen;
    bool app_ctrl_ready;
    bool app_ipcp_after_open;
    bool term_input_credit;
    uint32_t term_input_sent_ms;
    bool term_last_valid;
    uint8_t term_last_series;
    uint16_t term_last_len;
    uint32_t term_last_hash;
    uint32_t term_last_ms;
    uint8_t wait_app_timeouts;
    uint8_t tunnel_reopen_tries;
    uint32_t t_tunnel_reopen_next;
    uint32_t t_next_hb13;

    /* token buffers (stable) */
    uint8_t  token_buf[16];

    /* announce scheduler */
    uint32_t t_ann_cycle_next;
    uint32_t t_ann_cap1_at;
    uint32_t t_ann_full_at;
    bool     ann_cap1_sent;
    bool     ann_full_sent;

    /* time */
    uint32_t t_start;
    uint32_t t_next_keepalive;
    uint32_t t_last_peer;
    uint32_t t_last_tick;

    /* TX actions */
    sp_fsm_actions_t act;

    /* callbacks */
    sp_l2_evt_cb_t ext_l2_evt;
    sp_tr_evt_cb_t ext_tr_evt;
    void          *ext_user;

    st_t st;
    bool inited;
} fsm_ctx_t;

static fsm_ctx_t g;

static inline uint32_t now_ms_(void)                      { return g.t_last_tick; }
static inline bool time_reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static inline void set_state_(st_t ns)
{
    if (g.st != ns) {
        HLOGI(TAG, "STATE: %d -> %d", (int)g.st, (int)ns);
        g.st = ns;
    }
}

static inline bool blk_has_sig_(const sp_app_block_t *blk, uint8_t a, uint8_t b, uint8_t c)
{
    return blk && blk->sig[0] == a && blk->sig[1] == b && blk->sig[2] == c;
}

static uint32_t term_hash_(const uint8_t *p, uint16_t len)
{
    if (!p || !len) return 0u;
    uint32_t h = 2166136261u; /* FNV-1a */
    const uint16_t n = (len > 96u) ? 96u : len;
    for (uint16_t i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    h ^= (uint32_t)len;
    return h;
}

static inline uint32_t offline_silence_ms_(void)
{
    return g.ppp_stream_seen ? T_OFFLINE_SILENCE_PPP_MS : T_OFFLINE_SILENCE_MS;
}

static inline void app_mark_proto_seen_(uint16_t proto)
{
    bool changed = false;
    if (proto == PPP_PROTO_LCP && !g.app_seen_lcp) {
        g.app_seen_lcp = true;
        changed = true;
    }
    if (proto == PPP_PROTO_IPCP && !g.app_seen_ipcp) {
        g.app_seen_ipcp = true;
        changed = true;
    }
    if (g.conn_accept_received && !g.app_post_conn_seen) {
        g.app_post_conn_seen = true;
        changed = true;
    }
    if (changed) {
        /* Progress on APP control plane: postpone WAIT_APP timeout fallback. */
        g.wait_app_timeouts = 0u;
    }
    if (!g.app_ctrl_ready &&
        g.conn_accept_received &&
        g.app_post_conn_seen &&
        g.app_seen_lcp &&
        g.app_seen_ipcp) {
        g.app_ctrl_ready = true;
        HLOGI(TAG, "APP control ready (post-ConnAccept LCP+IPCP)");
    }
}

static inline void send_terminal_open_(const char *reason)
{
    if (!g.conn_accept_received || !g.mt_serial_valid) return;
    if (!(g.term_phase == TERM_WAIT_APP || g.term_phase == TERM_WAIT_PROMPT)) return;

    /* Keep CH=0xA0 short ACK counter aligned with compatible-device behavior:
     * first OpenACK reply after each Terminal Open starts from cnt=0. */
    sp_actions_reset_short_ack_a0();
    sp_err_t er = sp_action_send_terminal_open_ctrl(g.tcu_name, g.mt_serial);
    (void)er;
    g.term_phase = TERM_WAIT_PROMPT;
    g.term_open_ack_short_sent = false;
    g.app_ipcp_after_open = false;
    g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
    g.wait_app_timeouts = 0u;
    g.tunnel_reopen_tries = 0u;
    HLOGI(TAG, "Terminal Open sent (%s, er=%d), waiting prompt", reason ? reason : "n/a", (int)er);
}

static inline void kick_pre_session_hb13_(const char *reason)
{
    if (!g.conn_accept_received || !g.app_ctrl_ready || !g.mt_serial_valid) return;
    if (g.term_phase != TERM_WAIT_APP && g.term_phase != TERM_WAIT_PROMPT) return;

    if (g.hb13_probe_tries == 0u) {
        const uint8_t hb0[2] = { 0x06u, 0x00u };
        (void)sp_action_send_service_0013_payload(hb0, (uint16_t)sizeof(hb0));
        g.hb13_probe_tries = 1u;
        g.t_next_hb13 = now_ms_() + T_HB13_PROBE_MS;
        HLOGI(TAG, "HB13 pre-session kick (%s)", reason ? reason : "n/a");
    }

    if (g.term_phase == TERM_WAIT_APP) {
        g.t_tunnel_reopen_next = now_ms_() + T_HB13_PREOPEN_MS;
    }
}

/* ===== PPP helpers (HDLC in CH=0xFF app-data) ===== */

static uint16_t ppp_fcs16_(const uint8_t *data, uint16_t len)
{
    uint16_t fcs = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        fcs ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            fcs = (fcs & 1u) ? (uint16_t)((fcs >> 1) ^ 0x8408u) : (uint16_t)(fcs >> 1);
        }
    }
    return (uint16_t)(~fcs);
}

static bool ppp_unescape_slice_(const uint8_t *in, int start, int end,
                                uint8_t *out, uint16_t out_cap, uint16_t *out_len)
{
    if (!in || !out || !out_len) return false;
    if (start < 0 || end <= start + 1) return false;

    uint16_t wr = 0;
    bool esc = false;
    for (int i = start + 1; i < end; i++) {
        uint8_t b = in[i];
        if (esc) {
            b = (uint8_t)(b ^ 0x20u);
            esc = false;
        } else if (b == 0x7Du) {
            esc = true;
            continue;
        }
        if (wr >= out_cap) return false;
        out[wr++] = b;
    }
    if (esc || wr < 8u) return false;
    *out_len = wr;
    return true;
}

/* Some MT packets carry PPP payload with trailing 0x7E only (no leading flag),
 * e.g. "80 21 ... 7E". Decode bytes from start until first 0x7E. */
static bool ppp_unescape_until_flag_(const uint8_t *in, uint16_t in_len,
                                     uint8_t *out, uint16_t out_cap, uint16_t *out_len)
{
    if (!in || !out || !out_len || in_len < 3u) return false;

    uint16_t wr = 0u;
    bool esc = false;
    bool got_end = false;

    for (uint16_t i = 0; i < in_len; i++) {
        uint8_t b = in[i];
        if (b == 0x7Eu) {
            got_end = true;
            break;
        }
        if (esc) {
            b = (uint8_t)(b ^ 0x20u);
            esc = false;
        } else if (b == 0x7Du) {
            esc = true;
            continue;
        }
        if (wr >= out_cap) return false;
        out[wr++] = b;
    }

    if (!got_end || esc || wr < 8u) return false;
    *out_len = wr;
    return true;
}

static sp_err_t ppp_send_frame_(uint16_t proto, uint8_t code, uint8_t id,
                                const uint8_t *info, uint16_t info_len)
{
    uint8_t raw[320];
    uint16_t raw_len = 0;

    if (info_len > (uint16_t)(sizeof(raw) - 10u)) return SP_E_INVAL;

    raw[raw_len++] = 0xFFu;
    raw[raw_len++] = 0x03u;
    raw[raw_len++] = (uint8_t)((proto >> 8) & 0xFFu);
    raw[raw_len++] = (uint8_t)(proto & 0xFFu);
    raw[raw_len++] = code;
    raw[raw_len++] = id;
    raw[raw_len++] = (uint8_t)(((uint16_t)(info_len + 4u) >> 8) & 0xFFu);
    raw[raw_len++] = (uint8_t)((uint16_t)(info_len + 4u) & 0xFFu);
    if (info && info_len) {
        memcpy(&raw[raw_len], info, info_len);
        raw_len = (uint16_t)(raw_len + info_len);
    }

    uint16_t fcs = ppp_fcs16_(raw, raw_len);
    raw[raw_len++] = (uint8_t)(fcs & 0xFFu);        /* PPP FCS LE */
    raw[raw_len++] = (uint8_t)((fcs >> 8) & 0xFFu);

    uint8_t framed[520];
    uint16_t n = 0;
    framed[n++] = 0x7Eu;
    for (uint16_t i = 0; i < raw_len; i++) {
        const uint8_t b = raw[i];
        const bool need_esc = (b == 0x7Eu) || (b == 0x7Du) || (b < 0x20u);
        if (need_esc) {
            if ((uint16_t)(n + 2u) > (uint16_t)sizeof(framed)) return SP_E_NOMEM;
            framed[n++] = 0x7Du;
            framed[n++] = (uint8_t)(b ^ 0x20u);
        } else {
            if ((uint16_t)(n + 1u) > (uint16_t)sizeof(framed)) return SP_E_NOMEM;
            framed[n++] = b;
        }
    }
    if ((uint16_t)(n + 1u) > (uint16_t)sizeof(framed)) return SP_E_NOMEM;
    framed[n++] = 0x7Eu;

#if BRIDGE_CAN_LOG_HANDLER
    {
        const char *proto_s = (proto == PPP_PROTO_LCP) ? "LCP" :
                              (proto == PPP_PROTO_IPCP) ? "IPCP" : "UNK";
        const char *code_s = "Code";
        char info_hex[97];
        const uint16_t dump_len = (info_len > 24u) ? 24u : info_len;
        uint16_t w = 0u;

        if (proto == PPP_PROTO_LCP) {
            switch (code) {
            case 0x01u: code_s = "ConfReq"; break;
            case 0x02u: code_s = "ConfAck"; break;
            case 0x05u: code_s = "TermReq"; break;
            case 0x06u: code_s = "TermAck"; break;
            case 0x09u: code_s = "EchoReq"; break;
            case 0x0Au: code_s = "EchoRep"; break;
            default: break;
            }
        } else if (proto == PPP_PROTO_IPCP) {
            switch (code) {
            case 0x01u: code_s = "ConfReq"; break;
            case 0x02u: code_s = "ConfAck"; break;
            case 0x03u: code_s = "ConfNak"; break;
            case 0x04u: code_s = "ConfRej"; break;
            case 0x05u: code_s = "TermReq"; break;
            case 0x06u: code_s = "TermAck"; break;
            default: break;
            }
        }

        for (uint16_t i = 0; i < dump_len && (w + 2u) < sizeof(info_hex); i++) {
            static const char hx[] = "0123456789ABCDEF";
            const uint8_t b = info ? info[i] : 0u;
            info_hex[w++] = hx[(b >> 4) & 0x0Fu];
            info_hex[w++] = hx[b & 0x0Fu];
        }
        info_hex[w] = '\0';

        HLOGI(TAG, "PPP TX: %s %s id=0x%02X info_len=%u%s%s",
              proto_s, code_s, id, (unsigned)info_len,
              dump_len ? " info=" : "",
              dump_len ? info_hex : "");
    }
#endif

    return sp_service_send(framed, n, 0xFFu, NULL);
}

static void ppp_handle_decoded_frame_(const uint8_t *dec, uint16_t dec_len)
{
    if (!dec || dec_len < 8u) return;

    /* Accept both classic PPP header (FF 03) and ACFC-compressed form. */
    uint16_t pos = 0u;
    if (dec_len >= 2u && dec[0] == 0xFFu && dec[1] == 0x03u) {
        pos = 2u;
    }
    if (pos >= dec_len) return;

    /* Protocol field may be compressed (PFC): one byte when low byte is odd. */
    uint16_t proto = 0u;
    if (dec[pos] & 0x01u) {
        proto = (uint16_t)dec[pos];
        pos = (uint16_t)(pos + 1u);
    } else {
        if ((uint16_t)(pos + 1u) >= dec_len) return;
        proto = (uint16_t)(((uint16_t)dec[pos] << 8) | dec[pos + 1u]);
        pos = (uint16_t)(pos + 2u);
    }

    if (proto != PPP_PROTO_LCP && proto != PPP_PROTO_IPCP) return;

    app_mark_proto_seen_(proto);
    if (proto == PPP_PROTO_IPCP &&
        g.term_phase == TERM_WAIT_PROMPT &&
        g.term_open_ack_short_sent) {
        g.app_ipcp_after_open = true;
    }

    if ((uint16_t)(pos + 4u + 2u) > dec_len) return; /* +2 FCS */

    const uint8_t code = dec[pos + 0u];
    const uint8_t id   = dec[pos + 1u];
    const uint16_t plen = (uint16_t)(((uint16_t)dec[pos + 2u] << 8) | dec[pos + 3u]);
    if (plen < 4u) return;

    const uint16_t info_len = (uint16_t)(plen - 4u);
    if ((uint16_t)(pos + 4u + info_len + 2u) > dec_len) return; /* +2 FCS */
    const uint8_t *info = &dec[pos + 4u];

#if BRIDGE_CAN_LOG_HANDLER
    {
        const char *proto_s = (proto == PPP_PROTO_LCP) ? "LCP" :
                              (proto == PPP_PROTO_IPCP) ? "IPCP" : "UNK";
        const char *code_s = "Code";
        char info_hex[97];
        char info_ascii[33];
        const uint16_t dump_len = (info_len > 24u) ? 24u : info_len;
        uint16_t w = 0u;
        uint16_t a = 0u;
        bool ascii_ok = (info_len > 0u);

        if (proto == PPP_PROTO_LCP) {
            switch (code) {
            case 0x01u: code_s = "ConfReq"; break;
            case 0x02u: code_s = "ConfAck"; break;
            case 0x05u: code_s = "TermReq"; break;
            case 0x06u: code_s = "TermAck"; break;
            case 0x09u: code_s = "EchoReq"; break;
            case 0x0Au: code_s = "EchoRep"; break;
            default: break;
            }
        } else if (proto == PPP_PROTO_IPCP) {
            switch (code) {
            case 0x01u: code_s = "ConfReq"; break;
            case 0x02u: code_s = "ConfAck"; break;
            case 0x03u: code_s = "ConfNak"; break;
            case 0x04u: code_s = "ConfRej"; break;
            case 0x05u: code_s = "TermReq"; break;
            case 0x06u: code_s = "TermAck"; break;
            default: break;
            }
        }

        for (uint16_t i = 0; i < dump_len && (w + 2u) < sizeof(info_hex); i++) {
            static const char hx[] = "0123456789ABCDEF";
            const uint8_t b = info[i];
            info_hex[w++] = hx[(b >> 4) & 0x0Fu];
            info_hex[w++] = hx[b & 0x0Fu];
        }
        info_hex[w] = '\0';

        for (uint16_t i = 0; i < info_len && a + 1u < sizeof(info_ascii); i++) {
            const uint8_t b = info[i];
            if ((b >= 0x20u && b <= 0x7Eu) || b == '\r' || b == '\n') {
                info_ascii[a++] = (b == '\r' || b == '\n') ? '.' : (char)b;
            } else {
                ascii_ok = false;
                break;
            }
        }
        info_ascii[a] = '\0';

        HLOGI(TAG, "PPP RX: %s %s id=0x%02X info_len=%u%s%s%s%s%s",
              proto_s, code_s, id, (unsigned)info_len,
              dump_len ? " info=" : "",
              dump_len ? info_hex : "",
              (ascii_ok && a > 0u) ? " txt=\"" : "",
              (ascii_ok && a > 0u) ? info_ascii : "",
              (ascii_ok && a > 0u) ? "\"" : "");
    }
#endif

    if (proto == PPP_PROTO_LCP) {
        if (code == 0x01u) { /* Configure-Request */
            if (!g.ppp_lcp_req_sent) {
                const uint8_t lcp_req[] = {
                    0x01u, 0x04u, 0x05u, 0xDCu,
                    0x02u, 0x06u, 0x00u, 0x00u, 0x00u, 0x00u,
                    0x05u, 0x06u, 0x65u, 0x53u, 0x17u, 0xDDu,
                    0x07u, 0x02u,
                    0x08u, 0x02u
                };
                HLOGI(TAG, "PPP auto: send LCP ConfReq id=0x%02X", g.ppp_lcp_id);
                (void)ppp_send_frame_(PPP_PROTO_LCP, 0x01u, g.ppp_lcp_id++, lcp_req, sizeof(lcp_req));
                g.ppp_lcp_req_sent = true;
            }
            HLOGI(TAG, "PPP auto: LCP ConfReq id=0x%02X -> ConfAck", id);
            (void)ppp_send_frame_(PPP_PROTO_LCP, 0x02u, id, info, info_len);
        } else if (code == 0x02u) { /* Configure-Ack */
            /* Compatibility behavior observed on supported terminals:
             * after MT ConfAck for our LCP ConfReq, TCU emits its own EchoReq
             * using Magic-Number 0x655317DD. Without this keepalive kick,
             * MT later reports peer-not-responding and withholds terminal flow. */
            if (!g.ppp_lcp_echo_req_sent) {
                const uint8_t echo_req[] = { 0x65u, 0x53u, 0x17u, 0xDDu };
                HLOGI(TAG, "PPP auto: send LCP EchoReq id=0x00");
                (void)ppp_send_frame_(PPP_PROTO_LCP, 0x09u, 0x00u, echo_req, sizeof(echo_req));
                g.ppp_lcp_echo_req_sent = true;
            }
        } else if (code == 0x09u) { /* Echo-Request */
            HLOGI(TAG, "PPP auto: LCP EchoReq id=0x%02X -> EchoReply", id);
            (void)ppp_send_frame_(PPP_PROTO_LCP, 0x0Au, id, info, info_len);
        } else if (code == 0x05u) { /* Terminate-Request */
            HLOGI(TAG, "PPP auto: LCP TermReq id=0x%02X -> TermAck", id);
            (void)ppp_send_frame_(PPP_PROTO_LCP, 0x06u, id, NULL, 0u);
        }
    } else if (proto == PPP_PROTO_IPCP && code == 0x01u) { /* Configure-Request */
        if (!g.ppp_ipcp_req_sent) {
            const uint8_t ipcp_req[] = { 0x03u, 0x06u, 0x00u, 0x00u, 0x00u, 0x00u };
            HLOGI(TAG, "PPP auto: send IPCP ConfReq id=0x%02X", g.ppp_ipcp_id);
            (void)ppp_send_frame_(PPP_PROTO_IPCP, 0x01u, g.ppp_ipcp_id++, ipcp_req, sizeof(ipcp_req));
            g.ppp_ipcp_req_sent = true;
        }

        uint8_t rej_opts[96];
        uint16_t rej_len = 0u;
        bool opt_malformed = false;

        for (uint16_t p = 0u; p < info_len; ) {
            if ((uint16_t)(p + 2u) > info_len) { opt_malformed = true; break; }
            const uint8_t opt_type = info[p + 0u];
            const uint8_t opt_len  = info[p + 1u];
            if (opt_len < 2u || (uint16_t)(p + opt_len) > info_len) {
                opt_malformed = true;
                break;
            }

            if (opt_type == 0x02u) { /* IP-Compression-Protocol */
                if ((uint16_t)(rej_len + opt_len) > (uint16_t)sizeof(rej_opts)) {
                    opt_malformed = true;
                    break;
                }
                memcpy(&rej_opts[rej_len], &info[p], opt_len);
                rej_len = (uint16_t)(rej_len + opt_len);
            }
            p = (uint16_t)(p + opt_len);
        }

        if (opt_malformed) {
            HLOGW(TAG, "PPP auto: IPCP ConfReq id=0x%02X malformed -> ConfRej(all)", id);
            (void)ppp_send_frame_(PPP_PROTO_IPCP, 0x04u, id, info, info_len); /* ConfRej */
        } else if (rej_len > 0u) {
            HLOGI(TAG, "PPP auto: IPCP ConfReq id=0x%02X -> ConfRej(IP-Comp)", id);
            (void)ppp_send_frame_(PPP_PROTO_IPCP, 0x04u, id, rej_opts, rej_len); /* ConfRej */
        } else {
            HLOGI(TAG, "PPP auto: IPCP ConfReq id=0x%02X -> ConfAck", id);
            (void)ppp_send_frame_(PPP_PROTO_IPCP, 0x02u, id, info, info_len);     /* ConfAck */
        }
    } else if (proto == PPP_PROTO_IPCP && code == 0x05u) { /* Terminate-Request */
        HLOGI(TAG, "PPP auto: IPCP TermReq id=0x%02X -> TermAck", id);
        (void)ppp_send_frame_(PPP_PROTO_IPCP, 0x06u, id, NULL, 0u);
    }
}

static void ppp_autoreply_on_app_(const uint8_t *app_data, uint16_t app_len)
{
    if (!app_data || app_len < 3u) return;

    bool parsed_any = false;
    int s = -1;
    for (uint16_t i = 0; i < app_len; i++) {
        if (app_data[i] != 0x7Eu) continue;
        if (s < 0) { s = (int)i; continue; }

        const int e = (int)i;
        uint8_t dec[320];
        uint16_t dec_len = 0;
        if (!ppp_unescape_slice_(app_data, s, e, dec, sizeof(dec), &dec_len)) {
            s = e;
            continue;
        }
        parsed_any = true;
        ppp_handle_decoded_frame_(dec, dec_len);

        s = e;
    }

    /* Fallback: trailing-flag only frame (no leading 0x7E). */
    if (!parsed_any) {
        uint8_t dec[320];
        uint16_t dec_len = 0;
        if (ppp_unescape_until_flag_(app_data, app_len, dec, sizeof(dec), &dec_len)) {
            ppp_handle_decoded_frame_(dec, dec_len);
        }
    }
}

/* ===== Address Claim helpers ===== */

static bool name_wins_(const uint8_t our[8], const uint8_t their[8])
{
    for (int i = 7; i >= 0; i--) {
        if (our[i] < their[i]) return true;
        if (our[i] > their[i]) return false;
    }
    return false;
}

static sp_err_t send_address_claim_(uint8_t sa)
{
    sp_id_fields_t id;
    id.pri = 3;
    id.dp  = 0;
    id.pf  = PF_EE_CLAIM;
    id.ps  = SA_BROADCAST;
    id.sa  = sa;
    return sp_l2_send(&id, g.tcu_name, 8);
}

static sp_err_t send_cannot_claim_(void)
{
    HLOGE(TAG, "Cannot Claim: all SA exhausted (tries=%u)", (unsigned)g.claim_tries);
    return send_address_claim_(SA_NULL);
}

static uint8_t next_sa_candidate_(uint8_t current)
{
    uint8_t next = (uint8_t)(current + 1u);
    if (next >= SA_NULL) next = SA_PREFERRED_DEFAULT;
    return next;
}

static void handle_ee_claim_(const sp_l2_frame_t *frm)
{
    if (frm->id.sa != g.sa_local) return;

    if (name_wins_(g.tcu_name, frm->data)) {
        HLOGI(TAG, "Contention SA=%02X: we win, re-claiming", g.sa_local);
        (void)send_address_claim_(g.sa_local);
        g.claim_sent = true;
        g.t_start    = now_ms_();
        set_state_(S_CLAIM);
        return;
    }

    if (g.claim_tries >= CLAIM_MAX_TRIES) {
        (void)send_cannot_claim_();
        g.sa_local = SA_NULL;
        (void)sp_strat_set_local_sa(SA_NULL);
        set_state_(S_OFF);
        return;
    }

    uint8_t new_sa = next_sa_candidate_(g.sa_local);
    HLOGI(TAG, "Contention SA=%02X: we lose, trying SA=%02X (try %u)",
          g.sa_local, new_sa, (unsigned)g.claim_tries);

    g.sa_local = new_sa;
    g.claim_tries++;
    (void)sp_strat_set_local_sa(g.sa_local);
    (void)send_address_claim_(g.sa_local);
    g.claim_sent = true;
    g.t_start    = now_ms_();
    set_state_(S_CLAIM);
}

static void handle_ea_request_(const sp_l2_frame_t *frm)
{
    if (frm->dlc < 3) return;
    uint32_t pgn = (uint32_t)frm->data[0]
                 | ((uint32_t)frm->data[1] << 8)
                 | ((uint32_t)frm->data[2] << 16);
    if (pgn == 0x00EE00UL) {
        HLOGI(TAG, "ISO Request EE00 from SA=%02X -> Claim", frm->id.sa);
        (void)send_address_claim_(g.sa_local);
    }
}

static void handle_commanded_address_app_(const uint8_t *app_data, uint16_t app_len)
{
    if (!app_data || app_len < 9) return;
    if (memcmp(app_data, g.tcu_name, 8) != 0) return;

    const uint8_t new_sa = app_data[8];
    if (new_sa == SA_NULL || new_sa == SA_BROADCAST) {
        HLOGW(TAG, "Commanded Address: invalid SA=0x%02X, ignoring", new_sa);
        return;
    }

    HLOGI(TAG, "Commanded Address: SA 0x%02X -> 0x%02X", g.sa_local, new_sa);
    g.sa_local    = new_sa;
    g.claim_tries = 0;
    (void)sp_strat_set_local_sa(g.sa_local);
    (void)send_address_claim_(g.sa_local);
    g.claim_sent = true;
    g.t_start    = now_ms_();
    set_state_(S_CLAIM);
}

/* ===== Mini-C token exchange helpers (DATA domain) ===== */

static sp_err_t send_token_reply_ff_swap_data_(const uint8_t token16[16], uint8_t incoming_series)
{
    if (!token16) return SP_E_INVAL;
    if (g.sa_peer == SA_BROADCAST) return SP_E_STATE;

    /* [MT][TCU] -> [TCU][MT] */
    memcpy(&g.token_buf[0], &token16[8], 8);
    memcpy(&g.token_buf[8], &token16[0], 8);

    sp_app_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.sig[0]     = SIG_CTRL_A;
    blk.sig[1]     = SIG_CTRL_B;
    blk.sig[2]     = SIG_CTRL_C;
    blk.series     = (uint8_t)(incoming_series + 1u);
    blk.channel_le = CH_TOKEN_00FF;
    blk.app_data   = g.token_buf;
    blk.app_len    = 16;

    sp_id_fields_t id;
    /* Compatibility mapping: DP=0 (0DEF), CH=0xFF. */
    sp_strat_build_id(SP_FLOW_CTRL_CH_00FF, &id);
    return sp_tr_send_app(&id, &blk, NULL);
}

/* ===== L2 RX mux ===== */

static inline bool is_small_tlv_ef_(const sp_l2_frame_t *f)
{
    if (!f || f->id.pf != SP_PF_EF || f->dlc < 3) return false;
    return (f->data[0] == TLV_SIG_A && f->data[1] == TLV_SIG_B && f->data[2] == TLV_SIG_C);
}

static void l2_rx_mux_(const sp_l2_frame_t *frm, void *ctx)
{
    (void)ctx;
    if (!frm) return;

    const uint8_t local_sa    = g.sa_local;
    const bool    from_peer   = (frm->id.sa != local_sa);
    const bool    to_us_or_bc = (frm->id.ps == SA_BROADCAST || frm->id.ps == local_sa);
    const bool    same_sa_other_name =
        (frm->id.pf == PF_EE_CLAIM &&
         frm->dlc >= 8 &&
         frm->id.sa == local_sa &&
         memcmp(frm->data, g.tcu_name, 8) != 0);

    if (frm->id.pf == PF_EE_CLAIM) {
        if ((from_peer || same_sa_other_name) && frm->dlc >= 8) {
            handle_ee_claim_(frm);
        }
        sp_tlv_on_l2_frame(frm);
        if ((from_peer || same_sa_other_name) && to_us_or_bc) {
            g.sa_peer = frm->id.sa;
            (void)sp_strat_set_peer_sa(g.sa_peer);
            g.t_last_peer = now_ms_();
        }
        return;
    }

    if (frm->id.pf == PF_EA_REQUEST) {
        if (from_peer && to_us_or_bc && frm->dlc >= 3) {
            handle_ea_request_(frm);
        }
        sp_tlv_on_l2_frame(frm);
        return;
    }

    if (from_peer && to_us_or_bc) {
        g.sa_peer = frm->id.sa;
        (void)sp_strat_set_peer_sa(g.sa_peer);
        g.t_last_peer = now_ms_();
    }

    if (frm->id.pf == SP_PF_EA || is_small_tlv_ef_(frm)) {
        sp_tlv_on_l2_frame(frm);
    } else if (frm->id.pf == SP_PF_EF || frm->id.pf == PF_FE_COMMANDED) {
        /* Some MT revisions use FE transport IDs (e.g. 15FE...) for terminal stream.
         * Feed both EF/FE FP streams into transport parser. */
        sp_tr_on_l2_frame(frm);
    }
}

static void l2_evt_fwd_(sp_event_t ev, void *ctx)
{
    (void)ctx;
    if (g.ext_l2_evt) g.ext_l2_evt(ev, g.ext_user);
}

/* ===== Transport callbacks ===== */

static void tr_app_rx_cb_(const sp_id_fields_t *id, uint32_t pgn, uint8_t sid,
                          const sp_app_block_t *blk, void *user)
{
    (void)pgn; (void)user;
    if (!id || !blk) return;

    if (id->pf == PF_FE_COMMANDED && id->ps == PS_FED8) {
        handle_commanded_address_app_(blk->app_data, blk->app_len);
        return;
    }

    /* peer tracking */
    if (id->sa != g.sa_local && (id->ps == SA_BROADCAST || id->ps == g.sa_local)) {
        g.sa_peer     = id->sa;
        (void)sp_strat_set_peer_sa(g.sa_peer);
        g.t_last_peer = now_ms_();
    }

    /* Flow-control channel (0x40), based on observed compatibility behavior:
     *  - periodic short FC requests (app len=3) require short ACK on ch=0x04
     *  - explicit FC requests (app len=16) require 16-byte ACK [TCU][MT] on ch=0x04 */
    if (id->sa == g.sa_peer && blk->channel_le == CH_FC_REQ_0040) {
        (void)sp_action_send_ack_ch04((uint8_t)blk->app_len, g.tcu_name,
                                      g.mt_serial_valid ? g.mt_serial : NULL);
        return;
    }

    /* Session-level heartbeat ACK path from MT (DATA domain):
     * Compatible devices send MT->TCU 19EF ch=0x31 app {07 01}/{09 01}.
     * TCU replies with short ACK on ch=0x13 and keeps HB payload at 08 40. */
    if (id->sa == g.sa_peer &&
        id->dp == 1u && id->pri == 6u &&
        blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_TERM) &&
        blk->channel_le == CH_CONN_ACC_0031 &&
        blk->app_data && blk->app_len == 2)
    {
        const uint8_t a0 = blk->app_data[0];
        const uint8_t a1 = blk->app_data[1];
        if (a1 == 0x01u && (a0 == 0x07u || a0 == 0x09u)) {
            (void)sp_action_send_ctrl_ack_0013();
            if (a0 == 0x07u) {
                const uint8_t hb2[2] = { 0x08u, 0x40u };
                (void)sp_action_send_service_0013_payload(hb2, (uint16_t)sizeof(hb2));
                g.hb13_seen_0701 = true;
                /* Stay in post-probe mode after 07/01.
                 * Reverting probe_tries to zero causes another pre-open 06 00
                 * instead of proceeding to Terminal Open on the next window. */
            }
            if (a0 == 0x09u && g.hb13_seen_0701) {
                g.hb13_promoted = true;
            }
            g.t_next_hb13 = now_ms_() + T_HB13_MS;
            HLOGI(TAG, "HB13: RX 31 app=%02X %02X -> ACK13%s",
                  a0, a1, (a0 == 0x07u) ? " + 08 40" : "");
            return;
        }
    }

    /* ===== Mini-C token exchange from MT (observed counters: 0x10, then 0x12) ===== */
    if (id->sa == g.sa_peer &&
        blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_TERM) &&
        blk->channel_le == CH_TOKEN_00FF &&
        blk->app_len == 16 && blk->app_data &&
        !(blk->app_data[0] == 0x7Eu && blk->app_data[15] == 0x7Eu))
    {
        /* Guard: some PPP frames are exactly 16 bytes in APP payload
         * (e.g. 7E 80 21 ... 7E). They must go to PPP path below, not token logic. */
        /* MT payload is [MT][TCU] */
        memcpy(g.mt_serial, &blk->app_data[0], 8);
        g.mt_serial_valid = true;

        if (blk->series == 0x10u) {
            /* Step 5a (after Terminal Open) or initial SYS_CMD from MT.
             * Do not reset ConnReq/ConnAccept state here: frequent 0x10 during
             * pre-conn otherwise causes ConnReq flood and handshake churn. */
            (void)send_token_reply_ff_swap_data_(blk->app_data, blk->series);
            return;
        }

        if (blk->series == 0x12u) {
            /* Observed compatibility behavior: MT 0x12 -> TCU 0x12 with
             * swapped token payload [TCU][MT]. */
            if (g.term_phase == TERM_WAIT_PROMPT) {
                (void)send_token_reply_ff_swap_data_(blk->app_data, (uint8_t)(blk->series - 1u));
                return;
            }

            /* Default flow: MT 0x12 -> TCU 0x13 with [TCU][MT]. */
            (void)send_token_reply_ff_swap_data_(blk->app_data, blk->series);
            return;
        }

        /* Unknown series: be conservative, do nothing */
        return;
    }

    /* PPP/session stream on ch=0xFF after registration:
     * compatible devices send regular short ACK ch=0xFF from the TCU side. */
    if (id->sa == g.sa_peer &&
        blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_TERM) &&
        blk->channel_le == CH_TOKEN_00FF)
    {
        const bool has_payload = (blk->app_data && blk->app_len > 0);
        bool looks_ppp = false;
        if (has_payload) {
            for (uint16_t j = 0; j < blk->app_len; j++) {
                if (blk->app_data[j] == 0x7Eu) { looks_ppp = true; break; }
            }
        }

        /* During handshake keep CH=0xFF conservative.
         * IMPORTANT: before ConnAccept do not enter PPP mode, otherwise
         * ConnReq pump is suppressed by ppp_stream_seen and handshake stalls. */
        if (has_payload && g.term_phase != TERM_READY) {
            (void)sp_action_send_short_ack_ff();

            /* Pre-connection APP/PPP phase:
             * keep connReq pump alive, but already parse+reply PPP control. */
            if (!g.conn_accept_received || g.term_phase < TERM_WAIT_APP) {
                if (looks_ppp) {
                    ppp_autoreply_on_app_(blk->app_data, blk->app_len);
                    HLOGI(TAG, "PPP pre-conn: ACK+autoreply (series=0x%02X len=%u)",
                          (unsigned)blk->series, (unsigned)blk->app_len);
                }
                return;
            }

            if (g.term_phase == TERM_WAIT_APP) {
                /* APP-first phase: keep PPP control on L5, but do not push raw stream
                 * to terminal yet.
                 *
                 * IMPORTANT:
                 * Do not extend WAIT_APP timeout on pure PPP/IPCP keepalive traffic.
                 * TT-3027 can emit IPCP ConfReq every ~3s without ever reaching the
                 * stricter APP-ready condition, which would otherwise starve
                 * WAIT_APP timeout forever and block Terminal Open entirely.
                 *
                 * Only non-PPP app payload may postpone the timeout window here. */
                if (!looks_ppp) {
                    g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
                }
                g.ppp_stream_seen = true;
                sp_service_on_l4_app(blk->series, blk->app_data, blk->app_len);
                if (looks_ppp) {
                    ppp_autoreply_on_app_(blk->app_data, blk->app_len);
                    HLOGI(TAG, "PPP app-phase: ACK+autoreply (series=0x%02X len=%u)",
                          (unsigned)blk->series, (unsigned)blk->app_len);
                    if (!g.app_ctrl_ready) {
                        if (g.wait_app_timeouts < 0xFFu) g.wait_app_timeouts++;
                        /* Some runs never hit periodic FSM timeout path under sustained
                         * PPP/IPCP traffic. Mirror the fallback locally from the packet
                         * handler so Terminal Open still happens after several repeated
                         * PPP app-phase cycles. */
                        if (g.wait_app_timeouts >= 3u) {
                            HLOGW(TAG, "WAIT_APP PPP fallback: repeated PPP app-phase without APP ready");
                            send_terminal_open_("wait-app-ppp-fallback");
                            return;
                        }
                    }
                }
                if (g.app_ctrl_ready) {
                    kick_pre_session_hb13_("app-ready");
                }
                return;
            }

            /* In WAIT_PROMPT do not keep extending timeout on pure PPP traffic,
             * otherwise prompt reopen is starved forever. */
            if (g.term_phase == TERM_WAIT_PROMPT) {
                if (!looks_ppp) {
                    g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
                }
            } else {
                g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
            }

            /* After ConnAccept/OpenACK allow service stream to reach upper layer.
             * Keep terminal TX channel on classic TERM_IN (0xA0): switching TX to
             * SERVICE_APP here can stall interactive shell input on MT3027. */
            sp_service_on_l4_app(blk->series, blk->app_data, blk->app_len);
            if (!looks_ppp) {
                sp_term_on_app(SP_CH_SERVICE_APP, blk->series, blk->app_data, blk->app_len, id);
            }
            g.ppp_stream_seen = true;

            if (looks_ppp) {
                ppp_autoreply_on_app_(blk->app_data, blk->app_len);
                HLOGI(TAG, "PPP pre-session: ACK+autoreply (series=0x%02X len=%u)",
                      (unsigned)blk->series, (unsigned)blk->app_len);
            }

            /* Strict compatibility-sequence mode:
             * do not mark terminal READY from ch=0xFF/IPCP-only traffic.
             * READY is entered only on real terminal output ch=0x0A (prompt/data). */
            return;
        }

        if (has_payload && g.term_phase == TERM_READY)
        {
            (void)sp_action_send_short_ack_ff();
            sp_service_on_l4_app(blk->series, blk->app_data, blk->app_len);
            if (!looks_ppp) {
                sp_term_on_app(SP_CH_SERVICE_APP, blk->series, blk->app_data, blk->app_len, id);
            }
            if (looks_ppp) {
                ppp_autoreply_on_app_(blk->app_data, blk->app_len);
            }
            g.ppp_stream_seen = true;
        }
        return;
    }

    /* Terminal Open ACK (step 6a): ch=0x0A len=16.
     * This is not yet the prompt or READY state. In observed behavior, short ACK A0
     * is sent only after the first actual terminal output block (step 6b),
     * not in response to the Open ACK itself. */
    if (g.term_phase == TERM_WAIT_PROMPT &&
        id->sa == g.sa_peer &&
        blk->channel_le == CH_TERM_OUT_000A &&
        blk->app_len == 16)
    {
        g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
        HLOGI(TAG, "Terminal Open ACK (ch=0A plen=%u series=0x%02X), waiting prompt",
              (unsigned)blk->app_len, (unsigned)blk->series);
        return;
    }

    /* MT short ACK for our terminal input blocks.
     * Use ch=0x0A len=0 as credit to send the next UART->CAN A0 block. */
    if (id->sa == g.sa_peer &&
        blk->channel_le == CH_TERM_OUT_000A &&
        blk->app_len == 0)
    {
        g.term_input_credit = true;
        g.term_input_sent_ms = 0u;
        HLOGI(TAG, "Terminal input credit (ch=0A plen=0 series=0x%02X)",
              (unsigned)blk->series);
        return;
    }

    /* Prompt / terminal output stream.
     * Every observed MT-to-TCU terminal output block (ch=0x0A), not
     * only the first, is acknowledged with SHORT ACK ch=0xA0. */
    if ((g.term_phase == TERM_WAIT_PROMPT || g.term_phase == TERM_READY) &&
        id->sa == g.sa_peer && blk->channel_le == CH_TERM_OUT_000A &&
        blk->app_len > 0 && blk->app_len != 16)
    {
        const uint32_t now = now_ms_();
        const uint32_t h = term_hash_(blk->app_data, blk->app_len);
        const bool dup_block =
            g.term_last_valid &&
            g.term_last_series == blk->series &&
            g.term_last_len == blk->app_len &&
            g.term_last_hash == h &&
            (now - g.term_last_ms) <= T_TERM_DUP_WINDOW_MS;

        /* Long TERM_OUT is ACKed once per completed page,
         * not on every continuation chunk. Final page chunk is the short
         * FP block (wire_len < 223), exposed here as page_final=1. */
        if (blk->page_final) {
            const sp_err_t ack_er = sp_action_send_short_ack_a0();
            if (ack_er != SP_OK) {
                HLOGW(TAG, "TERM_OUT ACK A0 failed: in_sid=%u in_series=0x%02X page=%u len=%u dup=%d er=%d",
                      (unsigned)sid, (unsigned)blk->series, (unsigned)blk->page_idx,
                      (unsigned)blk->app_len, (int)dup_block, (int)ack_er);
            } else {
                HLOGI(TAG, "TERM_OUT ACK A0 ok: in_sid=%u in_series=0x%02X page=%u len=%u dup=%d",
                      (unsigned)sid, (unsigned)blk->series, (unsigned)blk->page_idx,
                      (unsigned)blk->app_len, (int)dup_block);
            }
        } else {
            HLOGI(TAG, "TERM_OUT page: in_sid=%u in_series=0x%02X page=%u len=%u dup=%d",
                  (unsigned)sid, (unsigned)blk->series, (unsigned)blk->page_idx,
                  (unsigned)blk->app_len, (int)dup_block);
        }
        g.term_open_ack_short_sent = true;
        const bool had_credit = g.term_input_credit;
        g.term_input_credit = true;
        g.term_input_sent_ms = 0u;
        if (!had_credit) {
            HLOGI(TAG, "Terminal input credit (via TERM_OUT data, series=0x%02X len=%u)",
                  (unsigned)blk->series, (unsigned)blk->app_len);
        }

        if (!dup_block && blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_TERM)) {
            sp_term_on_app(SP_CH_TERM_OUT, blk->series, blk->app_data, blk->app_len, id);
        }
        g.term_last_valid = true;
        g.term_last_series = blk->series;
        g.term_last_len = blk->app_len;
        g.term_last_hash = h;
        g.term_last_ms = now;

        if (g.term_phase != TERM_READY) {
            g.term_phase = TERM_READY;
            /* Classic prompt mode: keep UART->CAN on terminal input channel (0xA0). */
            sp_term_set_tx_channel(SP_CH_TERM_IN);
            HLOGI(TAG, "Terminal READY");
            g.tunnel_reopen_tries = 0u;
            g.t_tunnel_reopen_next = 0u;
            if (g.hb13_probe_tries == 0u) {
                /* Start heartbeat phase with 06 00; switch to 08 40 after MT 07 01. */
                const uint8_t hb0[2] = { 0x06u, 0x00u };
                (void)sp_action_send_service_0013_payload(hb0, (uint16_t)sizeof(hb0));
                g.hb13_probe_tries = 1u;
                g.t_next_hb13 = now_ms_() + T_HB13_PROBE_MS;
            }
        }
        return;
    }

    /* ConnAccept (ch=0x31, plen=16):
     * Enter the APP-first phase and allow PPP/L5 to start; send Terminal Open
     * when the application is ready. */
    if (!g.conn_accept_received &&
        blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_TERM) &&
        id->sa == g.sa_peer &&
        blk->channel_le == CH_CONN_ACC_0031 &&
        blk->app_data && blk->app_len >= 16)
    {
        const uint8_t *a0 = &blk->app_data[0];
        const uint8_t *a8 = &blk->app_data[8];
        const bool tcu_in_first  = (memcmp(a0, g.tcu_name, 8) == 0);
        const bool tcu_in_second = (memcmp(a8, g.tcu_name, 8) == 0);
        if (tcu_in_first || tcu_in_second) {
            const uint8_t *mt_src = tcu_in_first ? a8 : a0;
            memcpy(g.mt_serial, mt_src, 8);
            g.mt_serial_valid      = true;
            g.conn_accept_received = true;
            HLOGI(TAG, "ConnAccept OK from SA=%02X", g.sa_peer);

            /* Preserve PPP negotiation state across ConnAccept.
             * MT often proceeds with IPCP-only control after ConnAccept;
             * resetting LCP/IPCP markers here can block Terminal Open forever. */
            g.app_ipcp_after_open = false;

            g.term_phase = TERM_WAIT_APP;
            g.term_open_ack_short_sent = false;
            g.t_tunnel_reopen_next = now_ms_() + T_PROMPT_WAIT_MS;
            g.wait_app_timeouts = 0u;
            g.tunnel_reopen_tries = 0u;
            HLOGI(TAG, "APP phase started: waiting L5/PPP control before Terminal Open");
            if (g.app_ctrl_ready) {
                kick_pre_session_hb13_("app-ready-at-connaccept");
            }
            return;
        }
    }

    /* Announce discovery (bcast) */
    if (blk->channel_le == SP_CH_TLV_BCAST &&
        blk_has_sig_(blk, SIG_BIG_A, SIG_BIG_B, SIG_BIG_ANN) &&
        blk->app_data && blk->app_len)
    {
        sp_tlv_on_app_block(id, blk);
        if (id->sa != g.sa_local && blk->app_len >= 10) {
            sp_fsm_on_mt_serial(&blk->app_data[2]);
            sp_fsm_on_peer_sa(id->sa);
        }
        return;
    }
}

static void tr_evt_fwd_(sp_event_t ev, const sp_id_fields_t *id, uint8_t sid, void *user)
{
    (void)user;
    if (g.ext_tr_evt) g.ext_tr_evt(ev, id, sid, g.ext_user);
}

/* ===== Announce scheduler ===== */

static inline void start_announce_cycle_now_(uint32_t now_ms)
{
    g.t_ann_cap1_at    = now_ms;
    g.t_ann_full_at    = now_ms + ANN_CAP_GAP_MS;
    g.ann_cap1_sent    = false;
    g.ann_full_sent    = false;
    g.t_ann_cycle_next = now_ms + T_ANN_RETRY_MS;
}

static inline void pump_announce_scheduler_(uint32_t now_ms)
{
    if (!g.ann_cap1_sent && g.t_ann_cap1_at && time_reached(now_ms, g.t_ann_cap1_at)) {
        (void)sp_action_send_announce_capabilities();
        g.ann_cap1_sent = true;
    }
    if (!g.ann_full_sent && g.t_ann_full_at && time_reached(now_ms, g.t_ann_full_at)) {
        (void)sp_action_send_announce_full();
        g.ann_full_sent = true;
    }
    if (g.ann_cap1_sent && g.ann_full_sent &&
        g.t_ann_cycle_next && time_reached(now_ms, g.t_ann_cycle_next) &&
        g.term_phase < TERM_WAIT_APP)   /* stop flooding after app/control phase starts */
    {
        start_announce_cycle_now_(now_ms);
    }
}

/* ===== Keepalive / handshake pump ===== */

static inline void pump_keepalive_(uint32_t now_ms)
{
    if (!time_reached(now_ms, g.t_next_keepalive)) return;

    if (g.act.send_bcast_ef_small)   (void)g.act.send_bcast_ef_small();
    /* address claim only when peer unknown — periodic claim triggers MT NAME exchange restart */
    if (g.act.send_bcast_ee_status && g.sa_peer == SA_BROADCAST)
        (void)g.act.send_bcast_ee_status();
    /* ISO req EE00 only when peer unknown — triggers MT address claim/token cycle */
    if (g.act.send_bcast_ea_pointer && g.sa_peer == SA_BROADCAST)
        (void)g.act.send_bcast_ea_pointer();

    /* ConnReq: sparse retry while waiting ConnAccept.
     * Avoid frequent resends during pre-conn PPP/token churn. */
    if (g.sa_peer != SA_BROADCAST && g.mt_serial_valid &&
        !g.conn_accept_received && !g.ppp_stream_seen &&
        g.term_phase < TERM_WAIT_APP)
    {
        const bool need_first = !g.conn_req_sent;
        const bool need_retry = g.conn_req_sent &&
                                g.t_conn_req_sent != 0u &&
                                time_reached(now_ms, g.t_conn_req_sent + T_CONN_REQ_RETRY_MS);
        if (need_first || need_retry) {
            if (sp_action_send_conn_req(g.tcu_name, g.mt_serial) == SP_OK) {
                g.conn_req_sent   = true;
                g.t_conn_req_sent = now_ms;
                HLOGI(TAG, "ConnReq sent (0x13%s)", need_retry ? ", retry" : "");
            }
        }
    }

    /* CH=0x13 heartbeat strategy based on observed device behavior:
     *  - Do not spam autonomously before session is ready.
     *  - Use 08 40 as steady keepalive payload (~2s cadence). */
    if (g.sa_peer != SA_BROADCAST &&
        g.conn_accept_received &&
        g.app_ctrl_ready &&
        g.term_phase >= TERM_WAIT_APP &&
        g.t_next_hb13 != 0u &&
        time_reached(now_ms, g.t_next_hb13))
    {
        /* Some MT revisions never send explicit 0x09/0x01 promotion.
         * After first 0x07/0x01 we must keep steady 0x08 0x40 heartbeat,
         * otherwise APP/session side times out. */
        if (g.hb13_promoted || g.hb13_seen_0701) {
            const uint8_t hb_post[2] = { 0x08u, 0x40u };
            (void)sp_action_send_service_0013_payload(hb_post, (uint16_t)sizeof(hb_post));
            g.t_next_hb13 = now_ms + T_HB13_MS;
        } else if (!g.hb13_seen_0701) {
            const uint8_t hb_pre[2] = { 0x06u, 0x00u };
            (void)sp_action_send_service_0013_payload(hb_pre, (uint16_t)sizeof(hb_pre));
            if (g.hb13_probe_tries < 0xFFu) g.hb13_probe_tries++;
            g.t_next_hb13 = now_ms + T_HB13_PROBE_MS;
        }
    }

    g.t_next_keepalive = now_ms + T_KEEPALIVE_MS;
}

static inline void reset_session_(uint32_t now_ms)
{
    g.t_next_keepalive     = now_ms + T_KEEPALIVE_MS;
    g.t_last_peer          = now_ms;

    g.conn_req_sent        = false;
    g.conn_accept_received = false;
    g.t_conn_req_sent      = 0;

    g.term_phase           = TERM_IDLE;
    g.term_open_ack_short_sent = false;
    g.ppp_stream_seen      = false;
    g.hb13_promoted        = false;
    g.hb13_seen_0701       = false;
    g.hb13_probe_tries     = 0u;
    g.ppp_lcp_req_sent     = false;
    g.ppp_ipcp_req_sent    = false;
    g.ppp_lcp_echo_req_sent = false;
    g.ppp_lcp_id           = 1u;
    g.ppp_ipcp_id          = 1u;
    g.app_seen_lcp         = false;
    g.app_seen_ipcp        = false;
    g.app_post_conn_seen   = false;
    g.app_ctrl_ready       = false;
    g.app_ipcp_after_open  = false;
    g.term_input_credit    = false;
    g.term_input_sent_ms   = 0u;
    g.term_last_valid      = false;
    g.wait_app_timeouts    = 0u;
    g.tunnel_reopen_tries  = 0u;
    g.t_tunnel_reopen_next = 0u;
    g.t_next_hb13          = 0u;
    g.mt_serial_valid      = false;
    sp_term_set_tx_channel(SP_CH_TERM_IN);
}

/* ===== PUBLIC API ===== */

sp_err_t sp_fsm_init(const sp_fsm_config_t *cfg, const sp_fsm_actions_t *act)
{
    if (g.inited) return SP_OK;
    if (!cfg || !act) return SP_E_INVAL;

    memset(&g, 0, sizeof(g));

    g.sa_local = (cfg->local_sa != SA_BROADCAST) ? cfg->local_sa : SA_PREFERRED_DEFAULT;
    g.sa_peer  = (cfg->peer_sa  != 0x00) ? cfg->peer_sa  : SA_BROADCAST;

    bool name_set = false;
    for (int i = 0; i < 8; i++) { if (cfg->tcu_serial[i]) { name_set = true; break; } }
    if (name_set) {
        memcpy(g.tcu_name, cfg->tcu_serial, 8);
    } else {
        static const uint8_t def_name[8] = BRIDGE_CAN_FALLBACK_TCU_NAME_BYTES;
        memcpy(g.tcu_name, def_name, 8);
    }

    g.claim_tries          = 0;
    g.claim_sent           = false;

    g.mt_serial_valid      = false;
    g.conn_req_sent        = false;
    g.conn_accept_received = false;
    g.term_phase           = TERM_IDLE;
    g.term_open_ack_short_sent = false;
    g.ppp_stream_seen      = false;
    g.hb13_promoted        = false;
    g.hb13_seen_0701       = false;
    g.hb13_probe_tries     = 0u;
    g.ppp_lcp_req_sent     = false;
    g.ppp_ipcp_req_sent    = false;
    g.ppp_lcp_echo_req_sent = false;
    g.ppp_lcp_id           = 1u;
    g.ppp_ipcp_id          = 1u;
    g.app_seen_lcp         = false;
    g.app_seen_ipcp        = false;
    g.app_post_conn_seen   = false;
    g.app_ctrl_ready       = false;
    g.app_ipcp_after_open  = false;
    g.term_input_credit    = false;
    g.term_input_sent_ms   = 0u;
    g.term_last_valid      = false;
    g.wait_app_timeouts    = 0u;
    g.tunnel_reopen_tries  = 0u;
    g.t_tunnel_reopen_next = 0u;
    g.t_next_hb13          = 0u;
    sp_term_set_tx_channel(SP_CH_TERM_IN);

    g.act                  = *act;
    g.st                   = S_BOOT;
    g.inited               = true;

    (void)sp_strat_set_local_sa(g.sa_local);
    if (g.sa_peer != SA_BROADCAST) (void)sp_strat_set_peer_sa(g.sa_peer);

    sp_actions_set_tcu_name(g.tcu_name);

    HLOGI(TAG, "Init: SA=%02X peer=%02X NAME=%02X%02X%02X%02X%02X%02X%02X%02X",
          g.sa_local, g.sa_peer,
          g.tcu_name[0], g.tcu_name[1], g.tcu_name[2], g.tcu_name[3],
          g.tcu_name[4], g.tcu_name[5], g.tcu_name[6], g.tcu_name[7]);

    return SP_OK;
}

void sp_fsm_deinit(void)
{
    memset(&g, 0, sizeof(g));
}

void sp_fsm_tick(uint32_t now_ms)
{
    if (!g.inited) return;
    g.t_last_tick = now_ms;

    pump_announce_scheduler_(now_ms);

    switch (g.st) {
    case S_BOOT:
        HLOGI(TAG, "S_BOOT: Address Claim SA=%02X", g.sa_local);
        (void)send_address_claim_(g.sa_local);
        g.claim_sent  = true;
        g.claim_tries = 0;
        g.t_start     = now_ms;
        set_state_(S_CLAIM);
        break;

    case S_CLAIM:
        if (g.sa_local == SA_NULL) {
            set_state_(S_OFF);
            break;
        }
        if (g.claim_sent && time_reached(now_ms, g.t_start + T_CLAIM_MS)) {
            HLOGI(TAG, "S_CLAIM: SA=%02X claimed OK", g.sa_local);
            start_announce_cycle_now_(now_ms);
            reset_session_(now_ms);
            set_state_(S_WAIT_POLL);
        }
        break;

    case S_WAIT_POLL:
        pump_keepalive_(now_ms);
        /* Only restart announce cycle before app/control phase starts */
        if (g.term_phase < TERM_WAIT_APP &&
            (!g.t_last_peer || (now_ms - g.t_last_peer) > offline_silence_ms_())) {
            if (time_reached(now_ms, g.t_ann_cycle_next)) {
                start_announce_cycle_now_(now_ms);
            }
        }
        /* Consider the link ONLINE after ConnAccept or when the terminal is ready. */
        if (g.term_phase == TERM_READY || g.conn_accept_received) {
            set_state_(S_ONLINE);
        }
        break;

    case S_ONLINE:
        pump_keepalive_(now_ms);
        if (g.conn_accept_received &&
            g.term_phase == TERM_WAIT_APP &&
            g.mt_serial_valid &&
            g.t_tunnel_reopen_next != 0u &&
            time_reached(now_ms, g.t_tunnel_reopen_next))
        {
            if (g.app_ctrl_ready) {
                if (g.hb13_probe_tries == 0u) {
                    kick_pre_session_hb13_("wait-app-timeout");
                } else {
                    send_terminal_open_("app-ready-timeout");
                }
            } else if (g.wait_app_timeouts < 3u) {
                g.wait_app_timeouts++;
                g.t_tunnel_reopen_next = now_ms + T_PROMPT_WAIT_MS;
                HLOGW(TAG, "WAIT_APP timeout: still waiting APP control (%u/3, lcp=%d ipcp=%d)",
                      (unsigned)g.wait_app_timeouts,
                      (int)g.app_seen_lcp,
                      (int)g.app_seen_ipcp);
            } else {
                HLOGW(TAG, "WAIT_APP timeout: fallback to Terminal Open");
                send_terminal_open_("wait-app-timeout-fallback");
            }
        }
        if (g.conn_accept_received &&
            g.term_phase == TERM_WAIT_PROMPT &&
            g.mt_serial_valid &&
            g.t_tunnel_reopen_next != 0u &&
            time_reached(now_ms, g.t_tunnel_reopen_next))
        {
            if (g.tunnel_reopen_tries < T_TUNNEL_REOPEN_MAX) {
                sp_err_t er = sp_action_send_terminal_open_ctrl(g.tcu_name, g.mt_serial);
                (void)er;
                g.term_open_ack_short_sent = false;
                g.tunnel_reopen_tries++;
                g.t_tunnel_reopen_next = now_ms + T_TUNNEL_REOPEN_MS;
                HLOGW(TAG, "WAIT_PROMPT timeout: reopen #%u (er=%d)",
                      (unsigned)g.tunnel_reopen_tries, (int)er);
            } else {
                HLOGW(TAG, "WAIT_PROMPT timeout: fallback to WAIT_POLL/reset");
                set_state_(S_WAIT_POLL);
                reset_session_(now_ms);
                break;
            }
        }
        if (!g.t_last_peer || (now_ms - g.t_last_peer) > offline_silence_ms_()) {
            if (g.conn_accept_received && g.mt_serial_valid &&
                g.tunnel_reopen_tries < T_TUNNEL_REOPEN_MAX &&
                (g.t_tunnel_reopen_next == 0u || time_reached(now_ms, g.t_tunnel_reopen_next)))
            {
                sp_err_t er = sp_action_send_terminal_open_ctrl(g.tcu_name, g.mt_serial);
                (void)er;
                g.term_phase = TERM_WAIT_PROMPT;
                g.term_open_ack_short_sent = false;
                g.tunnel_reopen_tries++;
                g.t_tunnel_reopen_next = now_ms + T_TUNNEL_REOPEN_MS;
                g.t_last_peer = now_ms; /* give MT a grace window for response */
                HLOGW(TAG, "ONLINE silence: soft tunnel reopen #%u (er=%d)",
                      (unsigned)g.tunnel_reopen_tries, (int)er);
            } else {
                HLOGW(TAG, "ONLINE silence: fallback to WAIT_POLL/reset");
                set_state_(S_WAIT_POLL);
                reset_session_(now_ms);
            }
        }
        break;

    case S_OFF:
    default:
        break;
    }
}

void sp_fsm_on_peer_sa(uint8_t sa)
{
    g.sa_peer = sa;
    (void)sp_strat_set_peer_sa(g.sa_peer);
    g.t_last_peer = now_ms_();
}

void sp_fsm_on_mt_serial(const uint8_t serial[8])
{
    if (!serial) return;
    memcpy(g.mt_serial, serial, 8);
    g.mt_serial_valid = true;
}

void sp_fsm_on_timeout(void) { }

void sp_fsm_set_state(sp_state_t s)
{
    switch (s) {
    case SP_ST_BOOT:      set_state_(S_BOOT);      break;
    case SP_ST_CLAIM:     set_state_(S_CLAIM);     break;
    case SP_ST_WAIT_POLL: set_state_(S_WAIT_POLL); break;
    case SP_ST_ONLINE:    set_state_(S_ONLINE);    break;
    default:              set_state_(S_OFF);       break;
    }
}

sp_state_t sp_fsm_get_state(void)
{
    switch (g.st) {
    case S_BOOT:      return SP_ST_BOOT;
    case S_CLAIM:     return SP_ST_CLAIM;
    case S_WAIT_POLL: return SP_ST_WAIT_POLL;
    case S_ONLINE:    return SP_ST_ONLINE;
    default:          return SP_ST_OFF;
    }
}

uint8_t sp_fsm_get_peer_sa(void)
{
    return (g.sa_peer != 0) ? g.sa_peer : SA_BROADCAST;
}

bool sp_fsm_term_input_can_send(void)
{
    static uint32_t s_last_wait_log_ms = 0u;
    if (!g.inited || g.st != S_ONLINE || g.term_phase != TERM_READY) return false;
    if (g.term_input_credit) return true;

    const uint32_t now = now_ms_();
    if (g.term_input_sent_ms != 0u &&
        time_reached(now, g.term_input_sent_ms + T_TERM_INPUT_CREDIT_TIMEOUT_MS)) {
        HLOGW(TAG, "TERM input credit timeout: allow send (elapsed=%u ms)",
              (unsigned)(now - g.term_input_sent_ms));
        return true; /* anti-deadlock fallback if MT skipped one input credit */
    }

    if (g.term_input_sent_ms != 0u &&
        (s_last_wait_log_ms == 0u || time_reached(now, s_last_wait_log_ms + 500u))) {
#if BRIDGE_CAN_LOG_HANDLER
        const uint32_t elapsed = now - g.term_input_sent_ms;
        HLOGI(TAG, "TERM input wait-credit: elapsed=%u ms left=%u ms",
              (unsigned)elapsed,
              (unsigned)((elapsed < T_TERM_INPUT_CREDIT_TIMEOUT_MS)
                         ? (T_TERM_INPUT_CREDIT_TIMEOUT_MS - elapsed) : 0u));
#endif
        s_last_wait_log_ms = now;
    }

    return false;
}

void sp_fsm_term_input_mark_sent(void)
{
    if (!g.inited) return;
    if (g.st != S_ONLINE || g.term_phase != TERM_READY) return;
    g.term_input_credit = false;
    g.term_input_sent_ms = now_ms_();
}

void sp_fsm_build_term_id(sp_id_fields_t *id)
{
    if (!id) return;
    (void)sp_strat_build_id(SP_FLOW_TERM_IN, id);
}

sp_err_t sp_fsm_bind_layers(const sp_l2_config_t  *l2_cfg,
                            const sp_timing_t     *tr_timing,
                            uint16_t               tr_max_slots,
                            const sp_tlv_config_t *tlv_cfg,
                            sp_l2_evt_cb_t         on_l2_evt,
                            sp_tr_evt_cb_t         on_tr_evt,
                            void                  *user)
{
    if (!l2_cfg || !tlv_cfg) return SP_E_INVAL;

    (void)sp_tlv_init(tlv_cfg, NULL, NULL);

    uint16_t slots = tr_max_slots ? tr_max_slots : 8u;
    sp_err_t er = sp_tr_init(tr_timing, slots, tr_app_rx_cb_, tr_evt_fwd_, NULL);
    if (er != SP_OK) return er;

    er = sp_l2_init(l2_cfg, l2_rx_mux_, l2_evt_fwd_, NULL);
    if (er != SP_OK) return er;

    g.ext_l2_evt = on_l2_evt;
    g.ext_tr_evt = on_tr_evt;
    g.ext_user   = user;

    HLOGI(TAG, "Bind: L2/TLV/TR ready (slots=%u)", (unsigned)slots);
    return SP_OK;
}

void sp_fsm_unbind_layers(void)
{
    /* Stop the only producer before releasing the decoders it calls. */
    sp_l2_deinit();
    sp_tlv_deinit();
    sp_tr_deinit();
    HLOGI(TAG, "Unbind: L2/TLV/TR stopped");
}
