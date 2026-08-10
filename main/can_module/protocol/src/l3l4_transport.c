/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L3+L4 transport: NMEA2000 Fast-Packet plus Sailor application blocks.
 */

#include "l3l4_transport.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ===== External L2 TX implemented by the lower layer ===== */
extern sp_err_t sp_l2_send(const sp_id_fields_t *id, const uint8_t *data, uint8_t dlc);

/* ===== Internal types ===== */
typedef struct {
    bool       in_use;
    uint32_t   pgn;
    uint8_t    sa, ps, sid;     /* sid: 0..7 */
    uint8_t    pri;             /* Priority from the source frame, used for events */
    uint16_t   total_len;       /* Payload length from Byte1 of the first frame, 1..223 */
    uint16_t   written;         /* Number of bytes copied into buf */
    uint8_t    next_idx;        /* Expected continuation index (1..31) */
    TickType_t last_tick;
    uint8_t   *buf;             /* Buffer of total_len bytes */
} fp_slot_t;

typedef struct {
    sp_tr_app_rx_cb_t on_app;
    sp_tr_evt_cb_t    on_evt;
    void             *user;

    uint16_t          max_slots;
    fp_slot_t        *slots;

    SemaphoreHandle_t lock;
    bool              inited;

    /* Timing */
    uint16_t          fp_block_timeout_ms;

    /* SID counter for automatic mode, cycling through 0..7 */
    uint8_t           sid_ctr;
} tr_ctx_t;

static tr_ctx_t g;

/* ===== Utilities ===== */
static inline TickType_t ms2ticks(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    return (t == 0) ? 1 : t;   /* At least one tick */
}

static inline uint32_t pgn_of(const sp_id_fields_t *id){
    /* J1939/N2K: for PDU1 (PF < 0xF0), PS=DA and the PGN excludes PS.
     * For PDU2 (PF >= 0xF0), PS=group and the PGN includes PS. */
    if (id->pf < 0xF0)
        return ((uint32_t)(id->dp & 1) << 16) | ((uint32_t)id->pf << 8);
    else
        return ((uint32_t)(id->dp & 1) << 16) | ((uint32_t)id->pf << 8) | id->ps;
}

static inline bool slot_match(const fp_slot_t *s, uint32_t pgn,
                              uint8_t sa, uint8_t ps, uint8_t sid, uint8_t pri){
    /* Important for Sailor traffic:
     * 15EF.. (pri=5) and 19EF.. (pri=6) can run in parallel with same SA/PS/SID.
     * If PRI is not part of the key, FP streams collide and trigger LOST_SEQ/TIMEOUT. */
    return s->in_use && s->pgn==pgn && s->sa==sa && s->ps==ps &&
           s->sid==sid && s->pri==pri;
}

static inline void parse_app_block_(const uint8_t *B, uint16_t L, sp_app_block_t *blk)
{
    *blk = (sp_app_block_t){0};
    blk->page_final = (L < SP_FP_MAX_PAYLOAD) ? 1u : 0u;

    if (L >= 6){
        blk->sig[0] = B[0];
        blk->sig[1] = B[1];
        blk->sig[2] = B[2];
        blk->series = B[3];
        blk->page_idx = B[4];
        blk->channel_le = (uint16_t)B[5]; /* Byte4 reserved, Byte5 channel */
    }

    if (L >= 8){
        const uint16_t declared_len = (uint16_t)(B[6] | ((uint16_t)B[7] << 8));
        const uint16_t avail_len = (uint16_t)(L - 8u);
        blk->app_len = declared_len;

        if ((uint32_t)8 + declared_len <= L) {
            blk->app_data = &B[8];
            return;
        }

        if (blk->channel_le == (uint16_t)SP_CH_TERM_OUT && avail_len > 0u) {
            blk->app_len = avail_len;
            blk->app_data = &B[8];
            return;
        }

        /* TT-3027 can emit TERM_OUT continuation pages where Byte4 is a chunk
         * index (01/02/...) and payload starts immediately at Byte5:
         *   5F 99 02 0F 01 6E ...
         *   5F 99 02 0F 02 20 ...
         * Treat these as TERM_OUT blocks so FSM can ACK every chunk. */
        if (blk->sig[0] == 0x5F && blk->sig[1] == 0x99 && blk->sig[2] == 0x02 &&
            B[4] != 0x00u && L > 5u)
        {
            blk->channel_le = (uint16_t)SP_CH_TERM_OUT;
            blk->app_len = (uint16_t)(L - 5u);
            blk->app_data = &B[5];
        }
    }
}

static void slot_free(fp_slot_t *s){
    if (s->buf) { vPortFree(s->buf); s->buf = NULL; }
    memset(s, 0, sizeof(*s));
}

static fp_slot_t* slot_alloc_new(uint32_t pgn, uint8_t sa, uint8_t ps,
                                 uint8_t sid, uint16_t total_len, uint8_t pri){
    for (uint16_t i=0;i<g.max_slots;i++){
        fp_slot_t *s = &g.slots[i];
        if (!s->in_use){
            memset(s, 0, sizeof(*s));
            s->in_use    = true;
            s->pgn       = pgn; s->sa = sa; s->ps = ps;
            s->sid       = (uint8_t)(sid & SP_FP_SID_MASK);
            s->pri       = pri;
            s->total_len = total_len;
            s->next_idx  = 1;
            s->last_tick = xTaskGetTickCount();
            s->buf       = (uint8_t*)pvPortMalloc(total_len);
            if (!s->buf){ s->in_use=false; return NULL; }
            return s;
        }
    }
    return NULL;
}

static void sweep_timeouts(void){
    if (!g.lock) return;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t to  = ms2ticks(g.fp_block_timeout_ms ? g.fp_block_timeout_ms : 200);
    xSemaphoreTake(g.lock, portMAX_DELAY);
    for (uint16_t i=0;i<g.max_slots;i++){
        fp_slot_t *s = &g.slots[i];
        if (!s->in_use) continue;
        if ((now - s->last_tick) > to){
            sp_id_fields_t id = { .pri=s->pri,
                                  .dp=(uint8_t)((s->pgn>>16)&1),
                                  .pf=(uint8_t)((s->pgn>>8)&0xFF),
                                  .ps=s->ps, .sa=s->sa };
            uint8_t sid_copy = s->sid;
            slot_free(s);
            xSemaphoreGive(g.lock);
            if (g.on_evt) g.on_evt(SP_EVT_FP_TIMEOUT, &id, sid_copy, g.user);
            xSemaphoreTake(g.lock, portMAX_DELAY);
        }
    }
    xSemaphoreGive(g.lock);
}

/* ===== API ===== */
sp_err_t sp_tr_init(const sp_timing_t *timing,
                    uint16_t max_slots,
                    sp_tr_app_rx_cb_t on_app,
                    sp_tr_evt_cb_t on_evt,
                    void *user)
{
    if (g.inited) return SP_E_STATE;
    if (!on_app)  return SP_E_INVAL;

    memset(&g, 0, sizeof(g));
    g.max_slots = max_slots ? max_slots : 8;
    g.on_app    = on_app;
    g.on_evt    = on_evt;
    g.user      = user;
    g.sid_ctr   = 0;
    g.fp_block_timeout_ms = timing ? timing->fp_block_timeout_ms : 200;

    g.slots = (fp_slot_t*)pvPortMalloc(sizeof(fp_slot_t) * g.max_slots);
    if (!g.slots) return SP_E_NOMEM;
    memset(g.slots, 0, sizeof(fp_slot_t) * g.max_slots);

    g.lock = xSemaphoreCreateMutex();
    if (!g.lock){ vPortFree(g.slots); g.slots=NULL; return SP_E_NOMEM; }

    g.inited = true;
    return SP_OK;
}

void sp_tr_deinit(void){
    if (!g.inited) return;
    if (g.lock){
        xSemaphoreTake(g.lock, portMAX_DELAY);
        for (uint16_t i=0;i<g.max_slots;i++) slot_free(&g.slots[i]);
        xSemaphoreGive(g.lock);
        vSemaphoreDelete(g.lock); g.lock=NULL;
    }
    if (g.slots){ vPortFree(g.slots); g.slots=NULL; }
    memset(&g, 0, sizeof(g));
}

/* ===================== RX path: process every L2 frame ===================== */
void sp_tr_on_l2_frame(const sp_l2_frame_t *frm){
    if (!g.inited || !frm || frm->dlc==0) return;

    sweep_timeouts();

    const uint8_t *d = frm->data;
    const uint8_t seq = d[0];
    const uint8_t idx = (uint8_t)(seq & SP_FP_IDX_MASK);
    const uint8_t sid = (uint8_t)((seq >> SP_FP_SID_SHIFT) & SP_FP_SID_MASK);

    if (idx==0){
        if (frm->dlc < 2) return;
        const uint8_t total_len = d[1];
        if (total_len==0 || total_len>SP_FP_MAX_PAYLOAD){
            if (g.on_evt) g.on_evt(SP_EVT_FP_OVERLEN, &frm->id, sid, g.user);
            return;
        }

        const uint32_t pgn = pgn_of(&frm->id);
        const uint8_t  sa  = frm->id.sa;
        const uint8_t  ps  = frm->id.ps;
        const uint8_t  pri = frm->id.pri;

        xSemaphoreTake(g.lock, portMAX_DELAY);

        for (uint16_t i=0; i<g.max_slots; i++){
            if (slot_match(&g.slots[i], pgn, sa, ps, sid, pri)){
                /* Duplicate idx=0 from sender: keep in-flight block if header matches.
                 * This avoids false LOST_SEQ on occasional repeated first frame. */
                uint8_t first_avail = (frm->dlc>2) ? (uint8_t)(frm->dlc-2) : 0;
                if (first_avail > SP_FP_FIRST_DATA_BYTES) first_avail = SP_FP_FIRST_DATA_BYTES;
                if (first_avail > total_len) first_avail = total_len;
                if (g.slots[i].next_idx > 1 &&
                    g.slots[i].total_len == total_len &&
                    (first_avail == 0 ||
                     (g.slots[i].buf && memcmp(g.slots[i].buf, &d[2], first_avail) == 0)))
                {
                    g.slots[i].last_tick = xTaskGetTickCount();
                    xSemaphoreGive(g.lock);
                    return;
                }

                sp_id_fields_t eid = { .pri=g.slots[i].pri,
                                       .dp=(uint8_t)((pgn>>16)&1),
                                       .pf=(uint8_t)((pgn>>8)&0xFF),
                                       .ps=ps, .sa=sa };
                slot_free(&g.slots[i]);
                xSemaphoreGive(g.lock);
                if (g.on_evt) g.on_evt(SP_EVT_FP_LOST_SEQ, &eid, sid, g.user);
                xSemaphoreTake(g.lock, portMAX_DELAY);
                break;
            }
        }

        fp_slot_t *s = slot_alloc_new(pgn, sa, ps, sid, total_len, pri);
        if (!s){
            xSemaphoreGive(g.lock);
            if (g.on_evt) g.on_evt(SP_EVT_L2_RX_DROPPED, &frm->id, sid, g.user);
            return;
        }

        uint8_t avail = (frm->dlc>2) ? (uint8_t)(frm->dlc-2) : 0;
        if (avail>SP_FP_FIRST_DATA_BYTES) avail = SP_FP_FIRST_DATA_BYTES;
        if (avail>total_len) avail = total_len;
        if (avail) memcpy(s->buf, &d[2], avail);

        s->written   = avail;
        s->next_idx  = 1;
        s->last_tick = xTaskGetTickCount();

        if (s->written >= s->total_len){
            const uint8_t *B = s->buf;
            const uint16_t L = s->total_len;

            sp_app_block_t blk;
            parse_app_block_(B, L, &blk);

            uint8_t *buf = s->buf; s->buf=NULL; slot_free(s);
            xSemaphoreGive(g.lock);

            if (g.on_app &&
                blk.sig[0] == 0x5F && blk.sig[1] == 0x99 &&
                (blk.sig[2] == 0x02 || blk.sig[2] == 0x03) &&
                (blk.app_len == 0 || blk.app_data != NULL))
            {
                g.on_app(&frm->id, pgn, sid, &blk, g.user);
            }

            vPortFree(buf);
            return;
        }

        xSemaphoreGive(g.lock);
        return;
    }

    /* Continuation frames have idx >= 1. */
    const uint32_t pgn = pgn_of(&frm->id);
    const uint8_t  sa  = frm->id.sa;
    const uint8_t  ps  = frm->id.ps;

    xSemaphoreTake(g.lock, portMAX_DELAY);
    fp_slot_t *s = NULL;
    for (uint16_t i=0;i<g.max_slots;i++){
        if (slot_match(&g.slots[i], pgn, sa, ps, sid, frm->id.pri)){ s=&g.slots[i]; break; }
    }
    if (!s){ xSemaphoreGive(g.lock); return; }

    if (idx != s->next_idx){
        if (idx < s->next_idx){
            /* Late duplicate continuation frame: ignore without tearing down slot. */
            s->last_tick = xTaskGetTickCount();
            xSemaphoreGive(g.lock);
            return;
        }
        sp_id_fields_t id_err = frm->id;
        slot_free(s);
        xSemaphoreGive(g.lock);
        if (g.on_evt) g.on_evt(SP_EVT_FP_LOST_SEQ, &id_err, sid, g.user);
        return;
    }

    uint8_t avail = (frm->dlc>1) ? (uint8_t)(frm->dlc-1) : 0;
    if (avail>SP_FP_NEXT_DATA_BYTES) avail = SP_FP_NEXT_DATA_BYTES;
    uint16_t left = (s->total_len > s->written) ? (uint16_t)(s->total_len - s->written) : 0;
    if (avail>left) avail = (uint8_t)left;

    if (avail) memcpy(&s->buf[s->written], &d[1], avail);
    s->written  += avail;
    s->next_idx = (uint8_t)(s->next_idx + 1);
    s->last_tick = xTaskGetTickCount();

    const bool done = (s->written >= s->total_len);

    if (done){
        const uint8_t *B = s->buf;
        const uint16_t L = s->total_len;

        sp_app_block_t blk;
        parse_app_block_(B, L, &blk);

        sp_id_fields_t id_copy = frm->id;
        uint8_t *buf = s->buf; s->buf=NULL; slot_free(s);
        xSemaphoreGive(g.lock);

        if (g.on_app &&
            blk.sig[0] == 0x5F && blk.sig[1] == 0x99 &&
            (blk.sig[2] == 0x02 || blk.sig[2] == 0x03) &&
            (blk.app_len == 0 || blk.app_data != NULL))
        {
            g.on_app(&id_copy, pgn, sid, &blk, g.user);
        }
        vPortFree(buf);
    } else {
        xSemaphoreGive(g.lock);
    }
}

/* ===================== TX path: send an application block ===================== */
sp_err_t sp_tr_send_app(const sp_id_fields_t *id,
                        const sp_app_block_t *blk,
                        uint8_t *io_sid)
{
    if (!g.inited) return SP_E_STATE;
    if (!id || !blk) return SP_E_INVAL;
    if (blk->app_len && !blk->app_data) return SP_E_INVAL;

    /* Payload = 3(SIG)+1(SER)+1(RES)+1(CH)+2(LEN_LE)+app_len */
    const uint32_t total_len = 8u + (uint32_t)blk->app_len;
    if (total_len==0 || total_len>SP_FP_MAX_PAYLOAD) return SP_E_INVAL;

    uint8_t sid;
    if (!io_sid || *io_sid==0xFF){
        sid = (uint8_t)(g.sid_ctr & SP_FP_SID_MASK);
        g.sid_ctr = (uint8_t)((g.sid_ctr + 1) & SP_FP_SID_MASK);
    } else {
        sid = (uint8_t)(*io_sid & SP_FP_SID_MASK);
        *io_sid = (uint8_t)((*io_sid + 1) & SP_FP_SID_MASK);
    }

    /* Frame 0:
       Byte0=(SID<<5)|0
       Byte1=total_len
       Byte2..7=SIG3,SER,RES,CH */
    {
        uint8_t f[8];
        f[0] = (uint8_t)((sid << SP_FP_SID_SHIFT) | 0);
        f[1] = (uint8_t)(total_len & 0xFF);

        f[2] = blk->sig[0];
        f[3] = blk->sig[1];
        f[4] = blk->sig[2];
        f[5] = blk->series;
        f[6] = 0x00;                                /* RESERVED */
        f[7] = (uint8_t)(blk->channel_le & 0xFF);   /* CHANNEL (1 byte) */

        sp_err_t er = sp_l2_send(id, f, 8);
        if (er != SP_OK) return er;
    }

    uint8_t len_le[2] = { (uint8_t)(blk->app_len & 0xFF), (uint8_t)((blk->app_len >> 8) & 0xFF) };
    uint8_t idx = 1;
    uint8_t hdr_pos = 0;
    uint32_t app_pos = 0;

    while (hdr_pos < 2 || app_pos < blk->app_len){
        if (idx >= SP_FP_MAX_FRAMES) return SP_E_PROTO;

        uint8_t f[8];
        f[0] = (uint8_t)((sid << SP_FP_SID_SHIFT) | (idx & SP_FP_IDX_MASK));

        uint8_t m = 0;
        while (hdr_pos < 2 && m < 7) f[1 + m++] = len_le[hdr_pos++];
        while (app_pos < blk->app_len && m < 7) f[1 + m++] = blk->app_data[app_pos++];

        while (m < 7) f[1 + m++] = 0xFF;

        sp_err_t er = sp_l2_send(id, f, 8);
        if (er != SP_OK) return er;

        idx++;
    }

    return SP_OK;
}

/* ===================== TX: SHORT ACK (6-byte NDP, one CAN frame) ===================== */
static sp_err_t tr_send_short_ack_sid_(const sp_id_fields_t *id, uint8_t sid, uint8_t cnt, uint16_t ch)
{
    if (!g.inited) return SP_E_STATE;
    if (!id) return SP_E_INVAL;
    sid &= SP_FP_SID_MASK;

    /* total_len=6: 5F 99 02 cnt 00 ch_lo */
    uint8_t f[8];
    f[0] = (uint8_t)((sid << SP_FP_SID_SHIFT) | 0);
    f[1] = 0x06;
    f[2] = 0x5F;
    f[3] = 0x99;
    f[4] = 0x02;
    f[5] = cnt;
    f[6] = 0x00;                         /* reserved */
    f[7] = (uint8_t)(ch & 0xFF);         /* channel (1 byte) */

    return sp_l2_send(id, f, 8);
}

sp_err_t sp_tr_send_short_ack(const sp_id_fields_t *id, uint8_t cnt, uint16_t ch)
{
    if (!g.inited) return SP_E_STATE;
    if (!id) return SP_E_INVAL;

    uint8_t sid = (uint8_t)(g.sid_ctr & SP_FP_SID_MASK);
    g.sid_ctr = (uint8_t)((g.sid_ctr + 1) & SP_FP_SID_MASK);
    return tr_send_short_ack_sid_(id, sid, cnt, ch);
}
