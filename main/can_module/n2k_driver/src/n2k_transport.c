/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Single / Fast-Packet / BAM / RTS-CTS transport with PGN subscriptions.
 */

#include "n2k_transport.h"
#include "esp_log.h"
#include "config_logs.h"
#include "config/memory_config.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "n2k_transport";

/* ===== TP.CM control codes (PGN 60416) ===== */
#define TP_CM_RTS      0x10
#define TP_CM_CTS      0x11
#define TP_CM_EOM_ACK  0x13
#define TP_CM_BAM      0x20
#define TP_CM_ABORT    0xFF   /* J1939-21 defines ABORT as 0xFF. */
#define TP_CM_WAIT     0x12   /* Rarely used */

/* ===== Internal formatting helpers for logs ===== */
#if BRIDGE_CAN_LOG_N2K_TXRX
static inline void _hex(const uint8_t *d, size_t n, char *out, size_t cap) {
    size_t o=0;
    for (size_t i=0;i<n && o+3<cap;i++) o += (size_t)snprintf(out+o, cap-o, "%02X ", d[i]);
    if (o<cap) out[o]=0;
}
#endif

/* ===== Internal state ===== */

typedef struct {
    bool     used;
    uint8_t  sa;
    uint32_t pgn;
    uint8_t  seq;              /* 3-bit Fast-Packet sequence (0..7) */
    uint8_t  next_frame_idx;   /* Expected frame index (1..31) */
    uint16_t total_len;        /* From byte 1 of the first frame */
    uint16_t have;
    uint8_t  buf[233];
    uint64_t ts_last_us;
    uint8_t  priority;
    uint8_t  dst;
} fp_slot_t;

typedef struct {
    bool     used;
    uint8_t  sa;               /* Sender */
    uint8_t  dst;              /* Recipient (our SA for RTS) */
    uint32_t pgn;
    uint16_t total_len;
    uint8_t  total_pkts;       /* 1..255 */
    uint8_t  next_seq;         /* Expected TP.DT sequence (1..255) */
    uint16_t have;
    uint8_t  *buf;             /* malloc(min(total_len, 1785)) */
    uint64_t ts_last_us;
    uint8_t  priority;

    /* Current CTS window for an addressed RTS receiver */
    uint8_t  window_left;
} tp_slot_t;

typedef enum {
    RX_KIND_SINGLE = 0,
    RX_KIND_FP_FIRST,
    RX_KIND_FP_FOLLOW,
    RX_KIND_TP_CM,
    RX_KIND_TP_DT,
} rx_frame_kind_t;

/* CM events awaited by TX (RTS/CTS/EOM/ABORT) */
typedef struct {
    uint8_t  ctrl;    /* CTS/EOM/ABORT/WAIT */
    uint8_t  src;     /* Sender (receiver side) */
    uint8_t  dst;     /* Recipient (usually our SA) */
    uint32_t pgn;
    uint8_t  num_pkts;/* Number of packets permitted by CTS */
    uint8_t  next_seq;/* First sequence number requested by CTS */
    uint16_t total_len;/* Total EOM_ACK length as defined by the specification */
} tp_cm_evt_t;

/* Subscribers */
typedef struct {
    bool        used;
    uint32_t    pgn;
    n2k_rx_cb_t cb;
    void       *user;
} subscriber_t;

/* Configuration/resources */
static struct {
    uint8_t local_sa;
    uint8_t def_pri;
    uint8_t bam_gap_ms;
    uint8_t fp_slots_n;
    uint8_t tp_slots_n;

    QueueHandle_t rxq;       /* Complete messages */
    fp_slot_t *fp_slots;
    tp_slot_t *tp_slots;

    /* TX fast-packet sequence (0..7) */
    uint8_t fp_tx_seq;
    SemaphoreHandle_t tx_mutex;

    /* CM event queue used while TX waits */
    QueueHandle_t cm_evt_q;

    /* Subscriptions */
    subscriber_t subs[N2K_MAX_SUBSCRIBERS];

    n2k_transport_stats_t stats;
    bool inited;
} G;

static _Atomic(TaskHandle_t) s_rx_task = NULL;
static atomic_bool s_rx_stop = ATOMIC_VAR_INIT(false);
static atomic_bool s_tx_stop = ATOMIC_VAR_INIT(false);
static portMUX_TYPE s_subs_lock = portMUX_INITIALIZER_UNLOCKED;

#define N2K_RX_STOP_WAIT_MS  1000u
#define N2K_RX_STOP_POLL_MS    10u
#define N2K_TX_STOP_WAIT_MS   1000u

/* ===== Utilities ===== */
static inline uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static inline bool is_timeout(uint64_t start_us, uint32_t timeout_ms) {
    return (now_us() - start_us) > ((uint64_t)timeout_ms * 1000ULL);
}

static bool tp_size_valid_(uint16_t total_len, uint8_t total_pkts)
{
    if (total_len == 0 || total_len > N2K_MSG_MAX_DATA || total_pkts == 0) {
        return false;
    }
    return (uint16_t)((total_len + 6u) / 7u) == total_pkts;
}

static esp_err_t tx_lock_(uint32_t timeout_ms)
{
    TickType_t wait_ticks;

    if (!G.inited || !G.tx_mutex ||
        atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    wait_ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : N2K_TP_TIMEOUT_MS);
    if (wait_ticks == 0) wait_ticks = 1;
    if (xSemaphoreTake(G.tx_mutex, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
        xSemaphoreGive(G.tx_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void tx_unlock_(void)
{
    xSemaphoreGive(G.tx_mutex);
}

/* ==== Subscriptions ==== */
static void notify_subs(const n2k_msg_t *m) {
    for (int i=0;i<N2K_MAX_SUBSCRIBERS;i++) {
        n2k_rx_cb_t cb = NULL;
        void *user = NULL;
        portENTER_CRITICAL(&s_subs_lock);
        if (G.subs[i].used &&
            (G.subs[i].pgn == m->pgn || G.subs[i].pgn == N2K_PGN_ANY)) {
            cb = G.subs[i].cb;
            user = G.subs[i].user;
        }
        portEXIT_CRITICAL(&s_subs_lock);
        if (cb) cb(m, user);
    }
}
esp_err_t n2k_transport_subscribe(uint32_t pgn, n2k_rx_cb_t cb, void *user) {
    if (!cb) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_subs_lock);
    for (int i=0;i<N2K_MAX_SUBSCRIBERS;i++) {
        if (G.subs[i].used &&
            G.subs[i].pgn == pgn &&
            G.subs[i].cb == cb &&
            G.subs[i].user == user) {
            portEXIT_CRITICAL(&s_subs_lock);
            return ESP_OK;
        }
    }

    for (int i=0;i<N2K_MAX_SUBSCRIBERS;i++) {
        if (!G.subs[i].used) {
            G.subs[i].used = true;
            G.subs[i].pgn  = pgn;
            G.subs[i].cb   = cb;
            G.subs[i].user = user;
            portEXIT_CRITICAL(&s_subs_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_subs_lock);
    return ESP_ERR_NO_MEM;
}

/* ==== Fast-Packet helpers ==== */
static bool pgn_is_single_only_(uint32_t pgn)
{
    /* Fast Packet has no type bit in the CAN frame. Known fixed-length PGNs must
     * be kept out of header heuristics because byte 0 is ordinary payload/SID. */
    switch (pgn) {
        case 59392u:  /* ISO Acknowledgement */
        case 59904u:  /* ISO Request */
        case 60928u:  /* ISO Address Claim */
        case 126992u: /* System Time */
        case 127245u: /* Rudder */
        case 127250u: /* Vessel Heading */
        case 127257u: /* Attitude */
        case 127488u: /* Engine Parameters, Rapid Update */
        case 127493u: /* Transmission Parameters, Dynamic */
        case 127505u: /* Fluid Level */
        case 127508u: /* Battery Status */
        case 129025u: /* Position, Rapid Update */
        case 129026u: /* COG/SOG, Rapid Update */
        case 129033u: /* Time & Date */
        case 129539u: /* GNSS DOPs */
        case 130306u: /* Wind Data */
        case 130310u: /* Environmental Parameters */
        case 130311u: /* Environmental Parameters (deprecated) */
            return true;
        default:
            return false;
    }
}

static bool frame_looks_like_fp_first_(const n2k_ll_frame_t *f)
{
    if (f->len != 8) return false;
    uint8_t frame_idx = (uint8_t)(f->data[0] & 0x1F);
    uint8_t total_len = f->data[1];
    return (frame_idx == 0) && (total_len > 6);
}

static bool frame_looks_like_fp_follow_(const n2k_ll_frame_t *f)
{
    if (f->len != 8) return false;
    uint8_t frame_idx = (uint8_t)(f->data[0] & 0x1F);
    return frame_idx >= 1 && frame_idx <= 31;
}

static bool frame_matches_active_fp_(const n2k_ll_frame_t *f)
{
    const uint8_t seq = (uint8_t)((f->data[0] >> 5) & 0x07);
    for (int i = 0; i < G.fp_slots_n; ++i) {
        if (G.fp_slots[i].used && G.fp_slots[i].sa == f->sa &&
            G.fp_slots[i].pgn == f->pgn && G.fp_slots[i].seq == seq) {
            return true;
        }
    }
    return false;
}

static rx_frame_kind_t classify_rx_frame_(const n2k_ll_frame_t *f)
{
    if (!f) return RX_KIND_SINGLE;
    if (f->pgn == 60416u) return RX_KIND_TP_CM;
    if (f->pgn == 60160u) return RX_KIND_TP_DT;
    if (pgn_is_single_only_(f->pgn)) return RX_KIND_SINGLE;
    if (frame_looks_like_fp_first_(f)) return RX_KIND_FP_FIRST;
    if (frame_looks_like_fp_follow_(f) && frame_matches_active_fp_(f)) {
        return RX_KIND_FP_FOLLOW;
    }
    return RX_KIND_SINGLE;
}

#if BRIDGE_CAN_LOG_N2K_TXRX
static void log_classify_(const n2k_ll_frame_t *f, const char *kind) {
    char h[64];

    if (!f || !kind) return;
    if (f->pgn != 127505u && f->pgn != 127245u) return;

    _hex(f->data, f->len < 16 ? f->len : 16, h, sizeof(h));
    ESP_LOGI(TAG,
             "CLASSIFY PGN=%u src=0x%02X dst=0x%02X len=%u kind=%s b0=0x%02X idx=%u seq=%u b1=0x%02X raw=%s",
             (unsigned)f->pgn,
             f->sa,
             f->dst,
             f->len,
             kind,
             f->data[0],
             (unsigned)(f->data[0] & 0x1F),
             (unsigned)((f->data[0] >> 5) & 0x07),
             f->data[1],
             h);
}
#endif

static int fp_find_free(void) {
    for (int i=0;i<G.fp_slots_n;i++) if (!G.fp_slots[i].used) return i;
    return -1;
}
static int fp_find(uint8_t sa, uint32_t pgn, uint8_t seq) {
    for (int i=0;i<G.fp_slots_n;i++) {
        if (G.fp_slots[i].used && G.fp_slots[i].sa==sa && G.fp_slots[i].pgn==pgn && G.fp_slots[i].seq==seq) return i;
    }
    return -1;
}
static void fp_collect_timeouts(void) {
    for (int i=0;i<G.fp_slots_n;i++) {
        if (G.fp_slots[i].used && is_timeout(G.fp_slots[i].ts_last_us, N2K_FP_TIMEOUT_MS)) {
            ESP_LOGW(TAG,"FP timeout: sa=0x%02X pgn=%u have=%u/%u",
                     G.fp_slots[i].sa,(unsigned)G.fp_slots[i].pgn,G.fp_slots[i].have,G.fp_slots[i].total_len);
            memset(&G.fp_slots[i],0,sizeof(G.fp_slots[i]));
            G.stats.rx_fast_packet_drop_to++;
        }
    }
}

/* ==== TP slots (BAM + RTS RX) ==== */
static int tp_find_free(void) { for(int i=0;i<G.tp_slots_n;i++) if(!G.tp_slots[i].used) return i; return -1; }
static int tp_find(uint8_t sa, uint32_t pgn) {
    for (int i=0;i<G.tp_slots_n;i++) if (G.tp_slots[i].used && G.tp_slots[i].sa==sa && G.tp_slots[i].pgn==pgn) return i;
    return -1;
}
static void tp_collect_timeouts(void) {
    for (int i=0;i<G.tp_slots_n;i++) {
        if (G.tp_slots[i].used && is_timeout(G.tp_slots[i].ts_last_us, N2K_TP_TIMEOUT_MS)) {
            ESP_LOGW(TAG,"TP timeout: sa=0x%02X pgn=%u have=%u/%u",
                     G.tp_slots[i].sa,(unsigned)G.tp_slots[i].pgn,G.tp_slots[i].have,G.tp_slots[i].total_len);
            if (G.tp_slots[i].buf) free(G.tp_slots[i].buf);
            /* Update BAM/RTS statistics using dst before clearing the slot. */
            if (G.tp_slots[i].dst == N2K_ADDR_GLOBAL) G.stats.rx_bam_drop_to++;
            else G.stats.rx_rts_drop_to++;
            memset(&G.tp_slots[i],0,sizeof(G.tp_slots[i]));
        }
    }
}

/* ==== deliver to app + subscribers ==== */
static void deliver_msg(uint32_t pgn, uint8_t pri, uint8_t src, uint8_t dst,
                        const uint8_t *data, uint16_t len, bool from_fp)
{
    n2k_msg_t m = {
        .pgn = pgn, .priority = pri, .src = src, .dst = dst,
        .len = len, .timestamp_us = now_us(), .from_fast_packet = from_fp
    };
    if (m.len > sizeof(m.data)) {
        ESP_LOGW(TAG, "RX len=%u exceeds max=%u, truncating",
                 (unsigned)m.len, (unsigned)sizeof(m.data));
        m.len = sizeof(m.data);
    }
    if (m.len && data) memcpy(m.data, data, m.len);

#if BRIDGE_CAN_LOG_N2K_TXRX
    {
        char h[64];
        size_t show = (m.len < 16) ? m.len : 16;
        _hex(m.data, show, h, sizeof(h));
        ESP_LOGI(TAG, "RX PGN=%u src=0x%02X dst=0x%02X len=%u%s %s",
                 (unsigned)m.pgn, m.src, m.dst, m.len, from_fp ? " FP" : "",
                 h);
    }
#endif

    notify_subs(&m);
    if (G.rxq && xQueueSend(G.rxq, &m, 0) != pdTRUE) {
        G.stats.rx_queue_drop++;
        ESP_LOGW(TAG, "RX queue full, drop pgn=%u len=%u",
                 (unsigned)m.pgn, (unsigned)m.len);
    }
}

/* ==== Transmission: Single / Fast-Packet / BAM ==== */

static inline esp_err_t send_sf(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                                const uint8_t *data, uint8_t len, uint32_t to_ms) {
#if BRIDGE_CAN_LOG_N2K_TXRX
    char h[64];
    size_t show = (len < 16) ? len : 16;
    _hex(data, show, h, sizeof(h));
    ESP_LOGI(TAG, "TX-SF PGN=%u pr=%u sa=0x%02X dst=0x%02X len=%u %s",
             (unsigned)pgn, priority, sa, dst, len, h);
#endif
    return n2k_ll_send(priority, pgn, sa, dst, data, len, to_ms);
}

esp_err_t n2k_send_single(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                          const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    if (!G.inited || atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > 8) return ESP_ERR_INVALID_SIZE;
    if (len > 0 && !data) return ESP_ERR_INVALID_ARG;
    esp_err_t r = send_sf(priority, pgn, sa, dst, data, len, timeout_ms);
    if (r == ESP_OK) G.stats.tx_single++;
    return r;
}

static esp_err_t send_fast_packet_unlocked_(uint8_t priority, uint32_t pgn,
                                            uint8_t sa, uint8_t dst,
                                            const uint8_t *data, uint16_t len,
                                            uint32_t timeout_ms)
{
    if (len <= 8) return send_sf(priority, pgn, sa, dst, data, (uint8_t)len, timeout_ms);
    if (len > 223) return ESP_ERR_INVALID_SIZE;
    if (!data) return ESP_ERR_INVALID_ARG;

    uint8_t seq = (G.fp_tx_seq++ & 0x07);
    uint16_t off = 0;
    uint8_t frame_idx = 0;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-FP start PGN=%u pr=%u sa=0x%02X dst=0x%02X len=%u seq=%u",
             (unsigned)pgn, priority, sa, dst, len, seq);
#endif

    /* frame 0 */
    {
        uint8_t pl[8] = {0};
        pl[0] = (uint8_t)((seq<<5) | 0);
        pl[1] = (uint8_t)len;
        uint8_t cpy = (len >= 6) ? 6 : (uint8_t)len;
        memcpy(&pl[2], &data[0], cpy);

#if BRIDGE_CAN_LOG_N2K_TXRX
        char h[64]; _hex(pl, 8, h, sizeof(h));
        ESP_LOGI(TAG, "TX-FP f0 %s", h);
#endif

        ESP_RETURN_ON_ERROR(send_sf(priority, pgn, sa, dst, pl, 8, timeout_ms), TAG, "FP f0 TX");
        off += cpy;
        frame_idx = 1;
    }
    /* frames 1.. */
    while (off < len && frame_idx <= 31) {
        if (atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t pl[8] = {0};
        pl[0] = (uint8_t)((seq<<5) | frame_idx);
        uint8_t remain = (uint8_t)(len - off);
        uint8_t cpy = (remain >= 7) ? 7 : remain;
        memcpy(&pl[1], &data[off], cpy);

#if BRIDGE_CAN_LOG_N2K_TXRX
        char h[64]; _hex(pl, 8, h, sizeof(h));
        ESP_LOGI(TAG, "TX-FP f%u %s", frame_idx, h);
#endif

        ESP_RETURN_ON_ERROR(send_sf(priority, pgn, sa, dst, pl, 8, timeout_ms), TAG, "FP fn TX");
        off += cpy;
        frame_idx++;
    }
    if (off != len) return ESP_FAIL;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-FP done PGN=%u len=%u", (unsigned)pgn, len);
#endif
    G.stats.tx_fast_packet++;
    return ESP_OK;
}

esp_err_t n2k_send_fast_packet(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                               const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    if (len <= 8) {
        return n2k_send_single(priority, pgn, sa, dst, data, (uint8_t)len, timeout_ms);
    }
    esp_err_t err = tx_lock_(timeout_ms);
    if (err != ESP_OK) return err;
    err = send_fast_packet_unlocked_(priority, pgn, sa, dst, data, len, timeout_ms);
    tx_unlock_();
    return err;
}

static esp_err_t send_bam_unlocked_(uint8_t priority, uint32_t pgn, uint8_t sa,
                                    const uint8_t *data, uint16_t len,
                                    uint32_t timeout_ms)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > N2K_MSG_MAX_DATA) return ESP_ERR_INVALID_SIZE;
    uint16_t pkts = (len + 6) / 7;
    if (pkts > 255) return ESP_ERR_INVALID_SIZE;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-BAM start PGN=%u pr=%u sa=0x%02X len=%u pkts=%u",
             (unsigned)pgn, priority, sa, len, pkts);
#endif

    /* CM BAM */
    uint8_t cm[8] = {0};
    cm[0]=TP_CM_BAM; cm[1]=len & 0xFF; cm[2]=(len>>8)&0xFF; cm[3]=(uint8_t)pkts; cm[4]=0xFF;
    cm[5]= (uint8_t)(pgn & 0xFF); cm[6]=(uint8_t)((pgn>>8)&0xFF); cm[7]=(uint8_t)((pgn>>16)&0xFF);

#if BRIDGE_CAN_LOG_N2K_TXRX
    { char h[64]; _hex(cm, 8, h, sizeof(h)); ESP_LOGI(TAG, "TX-BAM CM %s", h); }
#endif

    ESP_RETURN_ON_ERROR(n2k_ll_send(priority, 60416, sa, N2K_ADDR_GLOBAL, cm, 8, timeout_ms), TAG, "CM BAM TX");

    /* DT */
    uint16_t off=0; uint8_t seq=1;
    while (off < len && seq != 0) {
        if (atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t pl[8]={0};
        uint16_t remain = (uint16_t)(len - off);
        uint8_t cpy = (uint8_t)((remain >= 7u) ? 7u : remain);
        pl[0]=seq; memcpy(&pl[1], &data[off], cpy);

#if BRIDGE_CAN_LOG_N2K_TXRX
        char h[64]; _hex(pl, 8, h, sizeof(h));
        ESP_LOGI(TAG, "TX-BAM DT s%u %s", seq, h);
#endif

        ESP_RETURN_ON_ERROR(n2k_ll_send(priority, 60160, sa, N2K_ADDR_GLOBAL, pl, 8, timeout_ms), TAG, "DT BAM TX");
        off+=cpy; seq++; vTaskDelay(pdMS_TO_TICKS(G.bam_gap_ms));
    }
    if (off!=len) return ESP_FAIL;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-BAM done PGN=%u len=%u", (unsigned)pgn, len);
#endif

    G.stats.tx_bam++;
    return ESP_OK;
}

esp_err_t n2k_send_bam(uint8_t priority, uint32_t pgn, uint8_t sa,
                       const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    esp_err_t err = tx_lock_(timeout_ms);
    if (err != ESP_OK) return err;
    err = send_bam_unlocked_(priority, pgn, sa, data, len, timeout_ms);
    tx_unlock_();
    return err;
}

/* ==== RTS/CTS TX ==== */

static esp_err_t send_rtscts_unlocked_(uint8_t priority, uint32_t pgn,
                                       uint8_t sa, uint8_t dst,
                                       const uint8_t *data, uint16_t len,
                                       uint32_t timeout_ms)
{
    if (dst==N2K_ADDR_GLOBAL) return send_bam_unlocked_(priority, pgn, sa, data, len, timeout_ms);
    if (len <= 223) return send_fast_packet_unlocked_(priority, pgn, sa, dst, data, len, timeout_ms);
    if (!data) return ESP_ERR_INVALID_ARG;
    if (len > N2K_MSG_MAX_DATA) return ESP_ERR_INVALID_SIZE;

    uint16_t pkts = (uint16_t)((len + 6)/7);
    if (pkts>255) return ESP_ERR_INVALID_SIZE;
    uint16_t wnd = (N2K_TP_RTS_WINDOW > 0) ? N2K_TP_RTS_WINDOW : 16;
    if (wnd > 255) wnd = 255;
    uint8_t window = (uint8_t)wnd;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-RTS start PGN=%u pr=%u sa=0x%02X dst=0x%02X len=%u pkts=%u wnd=%u",
             (unsigned)pgn, priority, sa, dst, len, pkts, window);
#endif

    /* Send CM_RTS to dst */
    uint8_t rts[8]={0};
    rts[0]=TP_CM_RTS;
    rts[1]=len & 0xFF; rts[2]=(len>>8)&0xFF;
    rts[3]=(uint8_t)pkts;
    rts[4]=window;
    rts[5]=(uint8_t)(pgn & 0xFF);
    rts[6]=(uint8_t)((pgn>>8)&0xFF);
    rts[7]=(uint8_t)((pgn>>16)&0xFF);

#if BRIDGE_CAN_LOG_N2K_TXRX
    { char h[64]; _hex(rts, 8, h, sizeof(h)); ESP_LOGI(TAG, "TX-RTS CM %s", h); }
#endif

    xQueueReset(G.cm_evt_q);
    ESP_RETURN_ON_ERROR(n2k_ll_send(priority, 60416, sa, dst, rts, 8, timeout_ms), TAG, "CM RTS TX");

    uint16_t off = 0;
    uint8_t  next_seq = 1;
    uint64_t t0 = now_us();

    for (;;) {
        if (atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        /* Wait for CTS / EOM / ABORT from dst. */
        tp_cm_evt_t evt;
        uint32_t to = (timeout_ms>0)?timeout_ms:1000;
        if (to > 50u) to = 50u;
        if (xQueueReceive(G.cm_evt_q, &evt, pdMS_TO_TICKS(to)) != pdTRUE) {
            if (is_timeout(t0, N2K_TP_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "TX-RTS timeout waiting CM evt");
                return ESP_ERR_TIMEOUT;
            } else {
                continue;
            }
        }
        if (evt.dst != sa || evt.src != dst || evt.pgn != pgn) {
            /* Ignore events for other concurrent sessions. */
            continue;
        }

#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGI(TAG, "TX-RTS got CM ctrl=0x%02X from=0x%02X -> seq=%u k=%u total=%u",
                 evt.ctrl, evt.src, evt.next_seq, evt.num_pkts, evt.total_len);
#endif

        if (evt.ctrl == TP_CM_ABORT) {
            G.stats.tx_rtscts++; /* The attempt started but the receiver aborted it. */
            ESP_LOGW(TAG, "TX-RTS aborted by receiver");
            return ESP_FAIL;
        }
        if (evt.ctrl == TP_CM_EOM_ACK) {
            G.stats.tx_rtscts++;
#if BRIDGE_CAN_LOG_N2K_TXRX
            ESP_LOGI(TAG, "TX-RTS EOM-ACK ok off=%u len=%u", (unsigned)off, (unsigned)len);
#endif
            return (off >= len && evt.total_len == len) ? ESP_OK : ESP_FAIL;
        }
        if (evt.ctrl != TP_CM_CTS) {
            /* WAIT or another event: continue waiting. */
            continue;
        }

        /* CTS: evt.num_pkts, evt.next_seq */
        uint8_t k = evt.num_pkts ? evt.num_pkts : window;
        uint8_t seq = evt.next_seq;
        if (seq==0) seq = next_seq;
        const uint16_t requested_off = (uint16_t)(seq - 1u) * 7u;
        if (requested_off >= len) {
            ESP_LOGW(TAG, "TX-RTS invalid CTS seq=%u for len=%u", seq, len);
            return ESP_ERR_INVALID_RESPONSE;
        }
        off = requested_off;

        /* Send k packets starting at seq. */
        for (uint8_t i=0;i<k;i++) {
            if (atomic_load_explicit(&s_tx_stop, memory_order_acquire)) {
                return ESP_ERR_INVALID_STATE;
            }
            if (off >= len) break;
            uint8_t pl[8]={0};
            uint16_t remain = (uint16_t)(len - off);
            uint8_t cpy = (uint8_t)((remain >= 7u) ? 7u : remain);
            pl[0] = seq;
            memcpy(&pl[1], &data[off], cpy);

#if BRIDGE_CAN_LOG_N2K_TXRX
            char h[64]; _hex(pl, 8, h, sizeof(h));
            ESP_LOGI(TAG, "TX-RTS DT s%u %s", seq, h);
#endif

            ESP_RETURN_ON_ERROR(n2k_ll_send(priority, 60160, sa, dst, pl, 8, timeout_ms), TAG, "DT RTS TX");
            off += cpy;
            if (++seq==0) seq=1; /* Protect against wraparound to zero. */
            next_seq = seq;
        }
        /* Then wait for the next CTS or EOM_ACK. */
    }
}

esp_err_t n2k_send_rtscts(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                          const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    esp_err_t err = tx_lock_(timeout_ms);
    if (err != ESP_OK) return err;
    err = send_rtscts_unlocked_(priority, pgn, sa, dst, data, len, timeout_ms);
    tx_unlock_();
    return err;
}

/* ==== Automatic transport selection ==== */
esp_err_t n2k_send_auto(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                        const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "TX-AUTO pgn=%u pr=%u sa=0x%02X dst=0x%02X len=%u",
             (unsigned)pgn, priority, sa, dst, len);
#endif
    if (len <= 8)  return n2k_send_single(priority, pgn, sa, dst, data, (uint8_t)len, timeout_ms);

    esp_err_t err = tx_lock_(timeout_ms);
    if (err != ESP_OK) return err;
    if (len <= 223) {
        err = send_fast_packet_unlocked_(priority, pgn, sa, dst, data, len, timeout_ms);
    } else if (dst == N2K_ADDR_GLOBAL) {
        err = send_bam_unlocked_(priority, pgn, sa, data, len, timeout_ms);
    } else {
        err = send_rtscts_unlocked_(priority, pgn, sa, dst, data, len, timeout_ms);
    }
    tx_unlock_();
    return err;
}

/* ===== Reception/reassembly ===== */

static void deliver_single(const n2k_ll_frame_t *f) {
#if BRIDGE_CAN_LOG_N2K_TXRX
    char h[64]; _hex(f->data, f->len<16?f->len:16, h, sizeof(h));
    ESP_LOGI(TAG, "RX-SF PGN=%u src=0x%02X dst=0x%02X len=%u %s",
             (unsigned)f->pgn, f->sa, f->dst, f->len, h);
#endif
    deliver_msg(f->pgn, f->priority, f->sa, f->dst, f->data, f->len, false);
    G.stats.rx_single++;
}

static void process_fp_start(const n2k_ll_frame_t *f) {
    uint8_t seq = (uint8_t)((f->data[0]>>5)&0x07);
    uint16_t total = f->data[1];
    if (total < 7 || total > 223) return;

    int slot = fp_find_free();
    if (slot < 0) {
        ESP_LOGW(TAG,"No FP slots");
        return;
    }
    fp_slot_t *s=&G.fp_slots[slot];
    memset(s,0,sizeof(*s));
    s->used=true; s->sa=f->sa; s->pgn=f->pgn; s->seq=seq; s->next_frame_idx=1;
    s->total_len=total; s->have=0; s->priority=f->priority; s->dst=f->dst; s->ts_last_us=now_us();

    uint8_t cpy = (total>=6)?6:total;
    memcpy(&s->buf[0], &f->data[2], cpy);
    s->have += cpy;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "RX-FP start PGN=%u src=0x%02X dst=0x%02X total=%u seq=%u",
             (unsigned)f->pgn, f->sa, f->dst, total, seq);
#endif
}
static void process_fp_follow(const n2k_ll_frame_t *f) {
    uint8_t seq = (uint8_t)((f->data[0]>>5)&0x07);
    uint8_t idx = (uint8_t)(f->data[0] & 0x1F);

    int slot = fp_find(f->sa, f->pgn, seq);
    if (slot < 0) return;
    fp_slot_t *s=&G.fp_slots[slot];

    if (idx != s->next_frame_idx) {
        ESP_LOGW(TAG,"FP ooo: exp=%u got=%u",s->next_frame_idx,idx);
        memset(s,0,sizeof(*s));
        return;
    }

    s->ts_last_us = now_us();
    uint16_t remain = (s->total_len > s->have)?(s->total_len - s->have):0;
    uint8_t cpy = (remain>=7)?7:(uint8_t)remain;
    if (cpy) memcpy(&s->buf[s->have], &f->data[1], cpy);
    s->have += cpy; s->next_frame_idx++;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "RX-FP f%u have=%u/%u", idx, s->have, s->total_len);
#endif

    if (s->have >= s->total_len) {
#if BRIDGE_CAN_LOG_N2K_TXRX
        ESP_LOGI(TAG, "RX-FP done PGN=%u len=%u", (unsigned)s->pgn, s->total_len);
#endif
        deliver_msg(s->pgn, s->priority, s->sa, s->dst, s->buf, s->total_len, true);
        G.stats.rx_fast_packet_ok++;
        memset(s,0,sizeof(*s));
    }
}

/* ==== CM handling (60416) ==== */
static void push_cm_event(uint8_t ctrl, uint8_t src, uint8_t dst, uint32_t pgn,
                          uint8_t num_pkts, uint8_t next_seq, uint16_t total_len)
{
    tp_cm_evt_t e = { .ctrl=ctrl, .src=src, .dst=dst, .pgn=pgn, .num_pkts=num_pkts, .next_seq=next_seq, .total_len=total_len };
    if (xQueueSend(G.cm_evt_q, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CM event queue full, drop ctrl=0x%02X pgn=%u",
                 ctrl, (unsigned)pgn);
    }
}

/* RTS/CTS server side: receive a large addressed message. */
static void handle_rts_server(uint8_t pri, uint8_t src, uint8_t dst, const uint8_t *d)
{
    /* d: [0]=RTS, [1..2]=len, [3]=packets, [4]=max_pkts_per_cts, [5..7]=PGN */
    uint16_t total = (uint16_t)d[1] | ((uint16_t)d[2]<<8);
    uint8_t  pkts  = d[3];
    uint32_t pgn   = (uint32_t)d[5] | ((uint32_t)d[6]<<8) | ((uint32_t)d[7]<<16);
    if (!tp_size_valid_(total, pkts)) {
        uint8_t ab[8]={TP_CM_ABORT, 0x02,0,0,0,
                       (uint8_t)(pgn&0xFF),(uint8_t)((pgn>>8)&0xFF),(uint8_t)((pgn>>16)&0xFF)};
        (void)n2k_ll_send(pri, 60416, dst, src, ab, 8, 10);
        ESP_LOGW(TAG, "RX-RTS invalid size total=%u pkts=%u", total, pkts);
        return;
    }

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "RX-RTS server src=0x%02X -> total=%u pkts=%u pgn=%u", src, total, pkts, (unsigned)pgn);
#endif

    int slot = tp_find(src, pgn);
    if (slot < 0) slot = tp_find_free();
    if (slot < 0) {
        uint8_t ab[8]={TP_CM_ABORT, 0x01,0,0,0, (uint8_t)(pgn&0xFF),(uint8_t)((pgn>>8)&0xFF),(uint8_t)((pgn>>16)&0xFF)};
        (void)n2k_ll_send(pri, 60416, dst, src, ab, 8, 10);
        ESP_LOGW(TAG, "RX-RTS server no slots -> ABORT");
        return;
    }
    tp_slot_t *s=&G.tp_slots[slot];
    if (s->used && s->buf) free(s->buf);
    memset(s,0,sizeof(*s));
    s->used=true; s->sa=src; s->dst=dst; s->pgn=pgn; s->total_len=total; s->total_pkts=pkts;
    s->next_seq=1; s->have=0; s->priority=pri; s->window_left=0; s->ts_last_us=now_us();

    s->buf = (uint8_t *)MALLOC_WHERE(N2K_PAYLOAD_IN_PSRAM, total);
    if (!s->buf) {
        uint8_t ab[8]={TP_CM_ABORT, 0x02,0,0,0, (uint8_t)(pgn&0xFF),(uint8_t)((pgn>>8)&0xFF),(uint8_t)((pgn>>16)&0xFF)};
        (void)n2k_ll_send(pri, 60416, dst, src, ab, 8, 10);
        ESP_LOGW(TAG, "RX-RTS server no mem -> ABORT");
        memset(s,0,sizeof(*s));
        return;
    }

    /* Send the first CTS. */
    uint8_t left_pkts = pkts;
    uint8_t k = (N2K_TP_RTS_WINDOW>0)?N2K_TP_RTS_WINDOW:16; if (k>left_pkts) k=left_pkts;
    uint8_t cts[8]={TP_CM_CTS, k, s->next_seq, 0xFF, 0xFF,
                    (uint8_t)(pgn&0xFF),(uint8_t)((pgn>>8)&0xFF),(uint8_t)((pgn>>16)&0xFF)};
    (void)n2k_ll_send(pri, 60416, dst, src, cts, 8, 10);
#if BRIDGE_CAN_LOG_N2K_TXRX
    { char h[64]; _hex(cts, 8, h, sizeof(h)); ESP_LOGI(TAG, "RX-RTS server CTS %s", h); }
#endif
    s->window_left = k;
}

/* Handle an incoming CM message. */
static void process_tp_cm(const n2k_ll_frame_t *f)
{
    if (f->len != 8) return;
    const uint8_t *d=f->data;
    uint8_t ctrl=d[0];
    uint32_t pgn = (uint32_t)d[5] | ((uint32_t)d[6]<<8) | ((uint32_t)d[7]<<16);

#if BRIDGE_CAN_LOG_N2K_TXRX
    char h[64]; _hex(f->data, 8, h, sizeof(h));
    ESP_LOGI(TAG, "RX-CM ctrl=0x%02X pgn=%u src=0x%02X dst=0x%02X %s",
             ctrl, (unsigned)pgn, f->sa, f->dst, h);
#endif

    if (ctrl == TP_CM_BAM) {
        /* BAM announcement (broadcast) */
        uint16_t total = (uint16_t)d[1] | ((uint16_t)d[2]<<8);
        uint8_t  pkts  = d[3];
        if (!tp_size_valid_(total, pkts)) {
            ESP_LOGW(TAG, "RX-BAM invalid size total=%u pkts=%u", total, pkts);
            return;
        }

        int slot = tp_find(f->sa, pgn);
        if (slot < 0) slot = tp_find_free();
        if (slot < 0) {
            ESP_LOGW(TAG,"No TP slots for BAM");
            return;
        }

        tp_slot_t *s=&G.tp_slots[slot];
        if (s->used && s->buf) free(s->buf);
        memset(s,0,sizeof(*s));
        s->used=true; s->sa=f->sa; s->dst=N2K_ADDR_GLOBAL; s->pgn=pgn;
        s->total_len=total; s->total_pkts=pkts; s->next_seq=1; s->have=0;
        s->priority=f->priority; s->ts_last_us=now_us();

        s->buf = (uint8_t *)MALLOC_WHERE(N2K_PAYLOAD_IN_PSRAM, total);
        if (!s->buf) {
            ESP_LOGW(TAG,"No mem for BAM buf");
            memset(s,0,sizeof(*s));
            return;
        }
        return;
    }

    if (ctrl == TP_CM_RTS) {
        /* Accept an addressed RTS only when it targets us. */
        if (f->dst == G.local_sa) handle_rts_server(f->priority, f->sa, f->dst, d);
        return;
    }

    if (ctrl == TP_CM_CTS || ctrl == TP_CM_EOM_ACK || ctrl == TP_CM_ABORT || ctrl == TP_CM_WAIT) {
        /* These events belong to the TX side while we are transmitting. */
        if (f->dst == G.local_sa) {
            uint8_t num = (ctrl==TP_CM_CTS)? d[1] : 0;
            uint8_t seq = (ctrl==TP_CM_CTS)? d[2] : 0;
            uint16_t total = (ctrl==TP_CM_EOM_ACK)? ((uint16_t)d[1] | ((uint16_t)d[2]<<8)) : 0;
            push_cm_event(ctrl, f->sa, f->dst, pgn, num, seq, total);
        }
        return;
    }

    /* Ignore all other controls. */
}

/* TP.DT handler for BAM and RTS reception */
static void process_tp_dt(const n2k_ll_frame_t *f)
{
    if (f->len != 8) return;
    uint8_t seq = f->data[0];

#if BRIDGE_CAN_LOG_N2K_TXRX
    char h[64]; _hex(f->data, 8, h, sizeof(h));
    ESP_LOGI(TAG, "RX-DT s%u pgn=%u src=0x%02X dst=0x%02X %s",
             seq, (unsigned)f->pgn, f->sa, f->dst, h);
#endif

    for (int i=0;i<G.tp_slots_n;i++) {
        tp_slot_t *s=&G.tp_slots[i];
        if (!s->used) continue;
        if (s->sa != f->sa) continue;
        /* BAM uses dst=FF; RTS uses our SA. */
        if (s->dst != f->dst) continue;

        /* seq must match the expected value. */
        if (seq != s->next_seq) return;

        s->ts_last_us = now_us();

        uint16_t remain = (s->total_len > s->have) ? (s->total_len - s->have) : 0;
        uint8_t cpy = (remain>=7)?7:(uint8_t)remain;
        if (cpy) memcpy(&s->buf[s->have], &f->data[1], cpy);
        s->have += cpy;
        s->next_seq++;

        /* Track the receive window on the RTS server side. */
        if (s->dst == G.local_sa && s->window_left>0) {
            s->window_left--;
            /* When the window is exhausted with data remaining, request the next CTS batch. */
            if (s->window_left==0 && s->have < s->total_len) {
                uint8_t left_pkts = (uint8_t)(((s->total_len - s->have) + 6)/7);
                uint8_t k = (left_pkts > N2K_TP_RTS_WINDOW)? N2K_TP_RTS_WINDOW : left_pkts;
                uint8_t cts[8]={TP_CM_CTS, k, s->next_seq, 0xFF,0xFF,
                                (uint8_t)(s->pgn&0xFF),(uint8_t)((s->pgn>>8)&0xFF),(uint8_t)((s->pgn>>16)&0xFF)};
                (void)n2k_ll_send(s->priority, 60416, s->dst, s->sa, cts, 8, 10);
#if BRIDGE_CAN_LOG_N2K_TXRX
                char h2[64]; _hex(cts, 8, h2, sizeof(h2));
                ESP_LOGI(TAG, "RX-RTS server CTS-next %s", h2);
#endif
                s->window_left = k;
            }
        }

        /* Complete? */
        if (s->have >= s->total_len) {
            /* Send EOM_ACK for an addressed transfer. */
            if (s->dst == G.local_sa) {
                uint8_t eom[8]={TP_CM_EOM_ACK, (uint8_t)(s->total_len&0xFF), (uint8_t)((s->total_len>>8)&0xFF),
                                s->total_pkts, 0xFF,
                                (uint8_t)(s->pgn&0xFF),(uint8_t)((s->pgn>>8)&0xFF),(uint8_t)((s->pgn>>16)&0xFF)};
                (void)n2k_ll_send(s->priority, 60416, s->dst, s->sa, eom, 8, 10);
                G.stats.rx_rts_ok++;
#if BRIDGE_CAN_LOG_N2K_TXRX
                char h3[64]; _hex(eom, 8, h3, sizeof(h3));
                ESP_LOGI(TAG, "RX-RTS server EOM %s", h3);
#endif
            } else {
                G.stats.rx_bam_ok++;
            }

#if BRIDGE_CAN_LOG_N2K_TXRX
            ESP_LOGI(TAG, "RX-TP done pgn=%u len=%u", (unsigned)s->pgn, s->total_len);
#endif
            deliver_msg(s->pgn, s->priority, s->sa, s->dst, s->buf, s->total_len, false);
            free(s->buf); memset(s,0,sizeof(*s));
        }
        return;
    }
}

/* Main RX loop */
static void rx_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "transport rx task started");
    while (!atomic_load_explicit(&s_rx_stop, memory_order_acquire)) {
        n2k_ll_frame_t f;
        esp_err_t r = n2k_ll_receive(&f, 20);
        if (atomic_load_explicit(&s_rx_stop, memory_order_acquire)) break;
        if (r == ESP_OK) {
            switch (classify_rx_frame_(&f)) {
            case RX_KIND_FP_FIRST:
#if BRIDGE_CAN_LOG_N2K_TXRX
                log_classify_(&f, "fp_first");
#endif
                process_fp_start(&f);
                break;
            case RX_KIND_FP_FOLLOW:
#if BRIDGE_CAN_LOG_N2K_TXRX
                log_classify_(&f, "fp_follow");
#endif
                process_fp_follow(&f);
                break;
            case RX_KIND_TP_CM:
                process_tp_cm(&f);
                break;
            case RX_KIND_TP_DT:
                process_tp_dt(&f);
                break;
            case RX_KIND_SINGLE:
            default:
#if BRIDGE_CAN_LOG_N2K_TXRX
                log_classify_(&f, "single");
#endif
                deliver_single(&f);
                break;
            }
        }
        fp_collect_timeouts();
        tp_collect_timeouts();
    }
    atomic_store_explicit(&s_rx_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

/* ===== API ===== */

esp_err_t n2k_transport_init(const n2k_transport_cfg_t *cfg)
{
    if (G.inited) return ESP_ERR_INVALID_STATE;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(&G,0,sizeof(G));

    G.local_sa   = cfg->local_sa;
    G.def_pri    = cfg->default_priority ? cfg->default_priority : 6;
    G.bam_gap_ms = cfg->bam_dt_gap_ms ? cfg->bam_dt_gap_ms : N2K_TP_BAM_DT_GAP_MS;
    G.fp_slots_n = cfg->fp_slots ? cfg->fp_slots : N2K_FP_REASM_SLOTS;
    G.tp_slots_n = cfg->tp_slots ? cfg->tp_slots : N2K_TP_REASM_SLOTS;

    G.tx_mutex = xSemaphoreCreateMutex();
    if (!G.tx_mutex) return ESP_ERR_NO_MEM;

    if (cfg->rx_queue_len) {
        G.rxq = xQueueCreate(cfg->rx_queue_len, sizeof(n2k_msg_t));
        if (!G.rxq) { vSemaphoreDelete(G.tx_mutex); return ESP_ERR_NO_MEM; }
    }

    G.cm_evt_q = xQueueCreate(N2K_TP_EVENT_QUEUE_LEN, sizeof(tp_cm_evt_t));
    if (!G.cm_evt_q) {
        if (G.rxq) vQueueDelete(G.rxq);
        vSemaphoreDelete(G.tx_mutex);
        return ESP_ERR_NO_MEM;
    }

    G.fp_slots = (fp_slot_t *)CALLOC_WHERE(N2K_SLOTS_IN_PSRAM, G.fp_slots_n, sizeof(fp_slot_t));
    G.tp_slots = (tp_slot_t *)CALLOC_WHERE(N2K_SLOTS_IN_PSRAM, G.tp_slots_n, sizeof(tp_slot_t));
    if (!G.fp_slots || !G.tp_slots) {
        if (G.fp_slots) free(G.fp_slots);
        if (G.tp_slots) free(G.tp_slots);
        vQueueDelete(G.cm_evt_q);
        if (G.rxq) vQueueDelete(G.rxq);
        vSemaphoreDelete(G.tx_mutex);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t created_task = NULL;
    atomic_store_explicit(&s_rx_stop, false, memory_order_release);
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "n2k_rx",
                                           N2K_RX_TASK_STACK, NULL, N2K_RX_TASK_PRIO,
                                           &created_task, N2K_RX_TASK_CORE);
    if (ok != pdPASS) {
        free(G.fp_slots); free(G.tp_slots);
        vQueueDelete(G.cm_evt_q);
        if (G.rxq) vQueueDelete(G.rxq);
        vSemaphoreDelete(G.tx_mutex);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_rx_task, created_task, memory_order_release);

    atomic_store_explicit(&s_tx_stop, false, memory_order_release);
    G.inited = true;

#if BRIDGE_CAN_LOG_N2K_TXRX
    ESP_LOGI(TAG, "init SA=0x%02X pri=%u fp_slots=%u tp_slots=%u bam_gap=%ums",
             G.local_sa, G.def_pri, G.fp_slots_n, G.tp_slots_n, G.bam_gap_ms);
#endif

    return ESP_OK;
}

esp_err_t n2k_transport_deinit(void)
{
    SemaphoreHandle_t tx_mutex;

    if (!G.inited) return ESP_OK;
    if (xTaskGetCurrentTaskHandle() ==
        atomic_load_explicit(&s_rx_task, memory_order_acquire)) {
        ESP_LOGE(TAG, "deinit called from RX callback");
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_tx_stop, true, memory_order_release);
    if (atomic_load_explicit(&s_rx_task, memory_order_acquire)) {
        atomic_store_explicit(&s_rx_stop, true, memory_order_release);
        for (uint32_t waited_ms = 0;
             waited_ms < N2K_RX_STOP_WAIT_MS &&
                 atomic_load_explicit(&s_rx_task, memory_order_acquire);
             waited_ms += N2K_RX_STOP_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(N2K_RX_STOP_POLL_MS));
        }
        if (atomic_load_explicit(&s_rx_task, memory_order_acquire)) {
            ESP_LOGE(TAG, "RX task stop timed out; resources kept alive");
            return ESP_ERR_TIMEOUT;
        }
    }
    tx_mutex = G.tx_mutex;
    if (!tx_mutex ||
        xSemaphoreTake(tx_mutex, pdMS_TO_TICKS(N2K_TX_STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "TX stop timed out; resources kept alive");
        return ESP_ERR_TIMEOUT;
    }
    if (G.fp_slots) { free(G.fp_slots); G.fp_slots=NULL; }
    if (G.tp_slots) {
        for (int i=0;i<G.tp_slots_n;i++) if (G.tp_slots[i].used && G.tp_slots[i].buf) free(G.tp_slots[i].buf);
        free(G.tp_slots); G.tp_slots=NULL;
    }
    if (G.cm_evt_q) { vQueueDelete(G.cm_evt_q); G.cm_evt_q=NULL; }
    if (G.rxq) { vQueueDelete(G.rxq); G.rxq=NULL; }
    G.tx_mutex = NULL;
    xSemaphoreGive(tx_mutex);
    vSemaphoreDelete(tx_mutex);
    memset(&G,0,sizeof(G));
    atomic_store_explicit(&s_rx_stop, false, memory_order_release);
    atomic_store_explicit(&s_tx_stop, false, memory_order_release);
    ESP_LOGI(TAG, "deinit");
    return ESP_OK;
}

void n2k_transport_set_local_sa(uint8_t sa) { G.local_sa = sa; }

esp_err_t n2k_recv(n2k_msg_t *out, uint32_t timeout_ms)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!G.inited) return ESP_ERR_INVALID_STATE;
    if (!G.rxq) return ESP_ERR_NOT_SUPPORTED;
    if (xQueueReceive(G.rxq, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

void n2k_transport_get_stats(n2k_transport_stats_t *out)
{
    if (!out) return;
    *out = G.stats;
}
