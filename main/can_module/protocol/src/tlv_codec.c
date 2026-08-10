/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   TLV module for short L2 EF/EA/EE frames and nested EF-TLVs in large L3L4 EF blocks.
 */

#include "tlv_codec.h"
#include "sailor_proto_common.h"
#include "l2_link.h"            /* sp_l2_frame_t */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Define the macro locally when the header does not provide it. */
#ifndef SP_TLV_TAG_UNKNOWN
#define SP_TLV_TAG_UNKNOWN ((sp_tlv_tag_t)0xFF)
#endif

/* ===== Internal context ===== */
typedef struct {
    sp_tlv_rx_cb_t  cb;
    void           *user;
    sp_tlv_config_t cfg;

    /* EA/EE deduplication */
    TickType_t      last_tick_ea;
    TickType_t      last_tick_ee;
    uint8_t         last_ea[8];
    uint8_t         last_ee[8];
    uint8_t         last_ea_len;
    uint8_t         last_ee_len;

    /* 0x0C/0x10 cycle detector */
    bool            cycle_valid;
    uint8_t         cycle_last_idx;
    TickType_t      cycle_last_tick;
    uint32_t        cycle_period_ms;

    SemaphoreHandle_t lock;
    bool            inited;
} tlv_ctx_t;

static tlv_ctx_t g_tlv;

/* ===== Utilities ===== */
static inline TickType_t ms_to_ticks(uint32_t ms) {
    if (ms == 0) return 0;
    uint64_t t = ((uint64_t)ms * configTICK_RATE_HZ + 999u) / 1000u;
    if (t == 0) t = 1;
    return (TickType_t)t;
}

static inline uint32_t ticks_to_ms(TickType_t dt) {
    return (uint32_t)((dt * 1000u) / configTICK_RATE_HZ);
}

static inline uint32_t pgn_of_id_(const sp_id_fields_t *id) {
    return sp_pgn_of(id->dp, id->pf);
}

/* Short EF signature: 5F 99 04 */
#ifndef SP_EF_TLV_SIG_A
#define SP_EF_TLV_SIG_A 0x5F
#define SP_EF_TLV_SIG_B 0x99
#define SP_EF_TLV_SIG_C 0x04
#endif

static inline bool ef_tlv_sig_ok_(const uint8_t *d, uint8_t n) {
    return (n >= 6 &&
            d[0] == SP_EF_TLV_SIG_A &&
            d[1] == SP_EF_TLV_SIG_B &&
            d[2] == SP_EF_TLV_SIG_C);
}

static inline bool add_item_(sp_tlv_item_t *arr, uint8_t *io_n, uint8_t max_n,
                             sp_tlv_tag_t tag, uint8_t sub, sp_tlv_val_type_t vt, const void *vp, uint8_t vlen)
{
    if (*io_n >= max_n) return false;
    sp_tlv_item_t *it = &arr[*io_n];
    it->tag   = tag;
    it->sub   = sub;
    it->vtype = vt;
    switch (vt) {
        case SP_TLV_VAL_U8:    it->v.u8  = *(const uint8_t*)vp; break;
        case SP_TLV_VAL_U16BE: it->v.u16 = (uint16_t)(((const uint8_t*)vp)[0]<<8 | ((const uint8_t*)vp)[1]); break;
        case SP_TLV_VAL_U32BE: it->v.u32 = (uint32_t)(((const uint8_t*)vp)[0]<<24 | ((const uint8_t*)vp)[1]<<16 |
                                                      ((const uint8_t*)vp)[2]<<8  | ((const uint8_t*)vp)[3]); break;
        case SP_TLV_VAL_INDEX: it->v.u8  = *(const uint8_t*)vp; break;
        case SP_TLV_VAL_BYTES:
            it->v.bytes.p   = (const uint8_t*)vp;
            it->v.bytes.len = vlen;
            break;
    }
    (*io_n)++;
    return true;
}

/* EA/EE deduplication */
static bool dedup_ok_(uint32_t pgn, const uint8_t *data, uint8_t len) {
    if (!g_tlv.cfg.dedup_ms) return true;
    TickType_t now = xTaskGetTickCount();
    TickType_t wnd = ms_to_ticks(g_tlv.cfg.dedup_ms);

    if (pgn == SP_PGN_EA) {
        if (g_tlv.last_ea_len == len && len <= 8 && memcmp(g_tlv.last_ea, data, len) == 0) {
            if ((now - g_tlv.last_tick_ea) < wnd) return false;
        }
        if (len <= 8) memcpy(g_tlv.last_ea, data, len);
        g_tlv.last_ea_len = len;
        g_tlv.last_tick_ea = now;
        return true;
    } else if (pgn == SP_PGN_EE) {
        if (g_tlv.last_ee_len == len && len <= 8 && memcmp(g_tlv.last_ee, data, len) == 0) {
            if ((now - g_tlv.last_tick_ee) < wnd) return false;
        }
        if (len <= 8) memcpy(g_tlv.last_ee, data, len);
        g_tlv.last_ee_len = len;
        g_tlv.last_tick_ee = now;
        return true;
    }
    return true;
}

/* 0x0C/0x10 cycle detector: period between adjacent indices */
static void cycle_update_(uint8_t idx) {
    TickType_t now = xTaskGetTickCount();
    if (!g_tlv.cycle_valid) {
        g_tlv.cycle_valid = true;
        g_tlv.cycle_last_idx  = idx;
        g_tlv.cycle_last_tick = now;
        g_tlv.cycle_period_ms = 0;
        return;
    }
    bool seq_ok = (idx == (uint8_t)(g_tlv.cycle_last_idx + 1)) ||
                  (g_tlv.cycle_last_idx == 0x0D && idx == 0x01);
    uint32_t dt_ms = ticks_to_ms(now - g_tlv.cycle_last_tick);
    if (seq_ok && dt_ms > 0) {
        if (g_tlv.cycle_period_ms == 0) g_tlv.cycle_period_ms = dt_ms;
        else g_tlv.cycle_period_ms = (g_tlv.cycle_period_ms * 3 + dt_ms) / 4;
        g_tlv.cycle_last_idx  = idx;
        g_tlv.cycle_last_tick = now;
    } else {
        g_tlv.cycle_valid = true;
        g_tlv.cycle_last_idx  = idx;
        g_tlv.cycle_last_tick = now;
        g_tlv.cycle_period_ms = 0;
    }
}

static void cycle_fill_info_(sp_tlv_cycle_info_t *ci, uint8_t tag, uint8_t sub, uint8_t idx) {
    if (!ci) return;
    if (!g_tlv.cfg.detect_cycle) { ci->has_cycle = false; return; }
    if (tag == 0x0C && sub == 0x10) {
        cycle_update_(idx);
        ci->has_cycle  = true;
        ci->idx        = idx;
        ci->period_ms  = g_tlv.cycle_period_ms;
    } else {
        ci->has_cycle = false;
    }
}

/* ===== Parse short EF TLVs (6..8 bytes) ===== */
static uint8_t parse_short_ef_tlv_(const uint8_t *d, uint8_t n,
                                   sp_tlv_item_t *items, uint8_t max_items,
                                   sp_tlv_cycle_info_t *ci_out)
{
    if (n < 6) return 0;
    uint8_t tag = d[3];
    uint8_t count = 0;

    if (tag == 0x0C) {
        /* 5F 99 04 0C SUB idx 00, usually 7 bytes */
        if (n < 7) goto unknown_as_bytes;
        uint8_t sub = d[4];
        uint8_t idx = d[5];
        add_item_(items, &count, max_items, (sp_tlv_tag_t)0x0C, sub, SP_TLV_VAL_INDEX, &idx, 1);
        if (ci_out) cycle_fill_info_(ci_out, 0x0C, sub, idx);
        return count;
    }
    else if (tag == 0x58) {
        uint8_t sub = d[4];
        if (sub == 0x02) {
            /* 5F 99 04 58 02 val, 6 bytes */
            if (n < 6) goto unknown_as_bytes;
            uint8_t val = d[5];
            add_item_(items, &count, max_items, (sp_tlv_tag_t)0x58, 0x02, SP_TLV_VAL_U8, &val, 1);
            return count;
        } else if (sub == 0x01) {
            /* 5F 99 04 58 01 xx [yy], 6..7 bytes; treat the tail as BYTES */
            if (n < 6) goto unknown_as_bytes;
            const uint8_t *p = &d[5];
            uint8_t len = (uint8_t)(n - 5);
            add_item_(items, &count, max_items, (sp_tlv_tag_t)0x58, 0x01, SP_TLV_VAL_BYTES, p, len);
            return count;
        } else {
            goto unknown_as_bytes;
        }
    }
    else if (tag == 0x8C) {
        /* 5F 99 04 8C XX XX [XX XX], either 6 bytes (u16be) or 8 bytes (u32be) */
        if (n >= 8) {
            add_item_(items, &count, max_items, (sp_tlv_tag_t)0x8C, 0x00, SP_TLV_VAL_U32BE, &d[4], 4);
            return count;
        } else if (n >= 6) {
            add_item_(items, &count, max_items, (sp_tlv_tag_t)0x8C, 0x00, SP_TLV_VAL_U16BE, &d[4], 2);
            return count;
        }
        goto unknown_as_bytes;
    }

unknown_as_bytes:
    /* Unknown tag: return everything after SIG as BYTES. */
    {
        const uint8_t *p = &d[3];
        uint8_t len = (uint8_t)(n - 3);
        if (len) add_item_(items, &count, max_items, SP_TLV_TAG_UNKNOWN, 0, SP_TLV_VAL_BYTES, p, len);
    }
    return count;
}

/* ===== Public API ===== */

sp_err_t sp_tlv_init(const sp_tlv_config_t *cfg, sp_tlv_rx_cb_t on_rx, void *user) {
    if (g_tlv.inited) return SP_E_STATE;
    if (!cfg) return SP_E_INVAL;

    memset(&g_tlv, 0, sizeof(g_tlv));
    g_tlv.cb   = on_rx;   /* NULL is allowed. */
    g_tlv.user = user;
    g_tlv.cfg  = *cfg;

    g_tlv.lock = xSemaphoreCreateMutex();
    if (!g_tlv.lock) return SP_E_NOMEM;

    g_tlv.inited = true;
    return SP_OK;
}

void sp_tlv_deinit(void) {
    if (!g_tlv.inited) return;
    if (g_tlv.lock) { vSemaphoreDelete(g_tlv.lock); g_tlv.lock = NULL; }
    memset(&g_tlv, 0, sizeof(g_tlv));
}

/* RX from L2: short single-frame EF/EA/EE without FF padding */
void sp_tlv_on_l2_frame(const sp_l2_frame_t *frm) {
    if (!g_tlv.inited || !frm || frm->dlc == 0) return;

    const uint8_t *d = frm->data;
    uint8_t n = frm->dlc;
    uint32_t pgn = pgn_of_id_(&frm->id);

    /* EA/EE deduplication */
    if ((pgn == SP_PGN_EA || pgn == SP_PGN_EE) && !dedup_ok_(pgn, d, n)) {
        return;
    }

    sp_tlv_item_t items[8];
    uint8_t n_items = 0;
    sp_tlv_cycle_info_t ci = (sp_tlv_cycle_info_t){ .has_cycle = false, .idx = 0, .period_ms = 0 };

    if (frm->id.pf == SP_PF_EF) {
        if (ef_tlv_sig_ok_(d, n)) {
            /* Try the actual length, then 8/7/6-byte alternatives. */
            if (n_items == 0) n_items = parse_short_ef_tlv_(d, n, items, 8, &ci);
            if (n_items == 0 && n >= 8) n_items = parse_short_ef_tlv_(d, 8, items, 8, &ci);
            if (n_items == 0 && n >= 7) n_items = parse_short_ef_tlv_(d, 7, items, 8, &ci);
            if (n_items == 0 && n >= 6) n_items = parse_short_ef_tlv_(d, 6, items, 8, &ci);
        } else {
            /* Treat EF without the signature as BYTES. */
            add_item_(items, &n_items, 8, SP_TLV_TAG_UNKNOWN, 0, SP_TLV_VAL_BYTES, d, n);
        }
    } else if (frm->id.pf == SP_PF_EA) {
        add_item_(items, &n_items, 8, (sp_tlv_tag_t)0xEA, 0, SP_TLV_VAL_BYTES, d, n);
    } else if (frm->id.pf == SP_PF_EE) {
        if (n >= 8) {
            add_item_(items, &n_items, 8, (sp_tlv_tag_t)0xE1, 0, SP_TLV_VAL_U16BE, &d[1], 2);
            add_item_(items, &n_items, 8, (sp_tlv_tag_t)0xE2, 0, SP_TLV_VAL_U16BE, &d[3], 2);
            add_item_(items, &n_items, 8, (sp_tlv_tag_t)0xE3, 0, SP_TLV_VAL_U16BE, &d[5], 2);
            add_item_(items, &n_items, 8, (sp_tlv_tag_t)0xEF, 0, SP_TLV_VAL_U8,     &d[7], 1);
        } else {
            add_item_(items, &n_items, 8, SP_TLV_TAG_UNKNOWN, 0, SP_TLV_VAL_BYTES, d, n);
        }
    } else {
        return;
    }

    if (n_items == 0) return;

    if (g_tlv.cb) {
        g_tlv.cb(pgn, &frm->id, items, n_items, &ci, g_tlv.user);
    }
}

/* RX from L3L4: for a large EF block on the TLV channel, parse app_data into short EF records.
 * This function must only be called by the transport when ch==SP_CH_TLV_BCAST.
 */
void sp_tlv_on_app_block(const sp_id_fields_t *id, const sp_app_block_t *blk)
{
    if (!g_tlv.inited || !id || !blk || !blk->app_data || blk->app_len==0) return;
    if (blk->channel_le != SP_CH_TLV_BCAST) return;

    const uint8_t *p = blk->app_data;
    uint16_t left = blk->app_len;

    sp_tlv_item_t items[8];
    uint8_t n_items = 0;
    sp_tlv_cycle_info_t ci = (sp_tlv_cycle_info_t){ .has_cycle=false, .idx=0, .period_ms=0 };

    while (left >= 6 && n_items < 8) {
        if (left >= 6 && p[0] == SP_EF_TLV_SIG_A && p[1] == SP_EF_TLV_SIG_B && p[2] == SP_EF_TLV_SIG_C) {
            if (left >= 7) {
                uint8_t used = parse_short_ef_tlv_(p, 7, items + n_items, (uint8_t)(8 - n_items), &ci);
                if (used > 0) { p += 7; left -= 7; n_items += used; continue; }
            }
            {
                uint8_t used = parse_short_ef_tlv_(p, 6, items + n_items, (uint8_t)(8 - n_items), &ci);
                if (used > 0) { p += 6; left -= 6; n_items += used; continue; }
            }
        }
        /* Slide one byte at a time to tolerate noise or inserted data. */
        p++; left--;
    }

    if (n_items == 0) return;

    if (g_tlv.cb) {
        uint32_t pgn = sp_pgn_of(id->dp, id->pf);
        g_tlv.cb(pgn, id, items, n_items, &ci, g_tlv.user);
    }
}
