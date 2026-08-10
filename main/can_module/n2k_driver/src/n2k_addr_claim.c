/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Address Claim handler (60928) + ISO Request 59904.
 */

#include "n2k_addr_claim.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_N2K_ADDR_CLAIM
#include "config_logs.h"
#include <string.h>
#include "esp_check.h"

static const char *TAG = "n2k_addr";

#define PGN_ADDRESS_CLAIM  60928u
#define PGN_ISO_REQUEST    59904u

typedef struct {
    uint64_t name;
    uint8_t  sa;
    uint8_t  pri;
    n2k_sa_changed_cb_t onchg;
    void    *onchg_user;
    bool     inited;
} S_t;

static S_t S;

/* Write NAME as 8 little-endian bytes. */
static void name_to_le(uint64_t name, uint8_t out[8]) {
    for (int i=0;i<8;i++) out[i] = (uint8_t)((name >> (8*i)) & 0xFF);
}

/* Send PGN 60928 (Address Claim). */
static esp_err_t send_addr_claim(void) {
    uint8_t pl[8]; name_to_le(S.name, pl);
    /* Broadcast PDU2, so dst=FF. */
    return n2k_send_single(S.pri, PGN_ADDRESS_CLAIM, S.sa, N2K_ADDR_GLOBAL, pl, 8, 10);
}

/* Compare NAME priority:
 * In J1939/NMEA2000, the lower numeric NAME wins.
 * Returns:
 *   <0 when our_name < other (we win),
 *    0 when they are equal,
 *   >0 when our_name > other (we lose).
 */
static int cmp_name(uint64_t our, uint64_t other) {
    if (our < other) return -1;
    if (our > other) return 1;
    return 0;
}

/* Find the next SA by incrementing with wraparound while skipping 0xFE/0xFF. */
static uint8_t next_sa(uint8_t cur) {
    uint8_t sa = cur;
    for (int i=0;i<253;i++) {
        sa = (uint8_t)((sa + 1) % 253);  /* 0..252 */
        if (sa == 0xFE || sa == 0xFF) continue;
        return sa;
    }
    return cur; /* fallback */
}

/* ----- Transport subscribers ----- */

/* Receive PGN 60928 from another device or our own echo. */
static void on_pgn_60928(const n2k_msg_t *m, void *user)
{
    (void)user;
    if (m->len < 8) return;
    /* Check for a conflict when the SA matches ours. */
    if (m->src == S.sa) {
        uint64_t other=0;
        for (int i=0;i<8;i++) other |= ((uint64_t)m->data[i]) << (8*i);

        int r = cmp_name(S.name, other);
        if (r == 0) {
            /* Same NAME: nothing to do. */
            return;
        } else if (r < 0) {
            /* Our NAME is lower, so we win and reaffirm the address. */
            (void)send_addr_claim();
        } else {
            /* We lost: select a new SA and repeat the claim. */
            uint8_t new_sa = next_sa(S.sa);
            ESP_LOGW(TAG, "Address conflict at SA=%u, we lose by NAME. Reclaim with SA=%u", S.sa, new_sa);
            S.sa = new_sa;
            n2k_transport_set_local_sa(S.sa);
            (void)send_addr_claim();
            if (S.onchg) S.onchg(S.sa, S.onchg_user);
        }
    }
}

/* Handle ISO Request (59904) and check whether it requests PGN 60928. */
static void on_pgn_59904(const n2k_msg_t *m, void *user)
{
    (void)user;
    if (m->len < 3) return;
    /* The PGN is little-endian in the payload. */
    uint32_t req_pgn = (uint32_t)m->data[0] | ((uint32_t)m->data[1]<<8) | ((uint32_t)m->data[2]<<16);

    /* The request must target us or the global address. */
    if (!(m->dst == S.sa || m->dst == N2K_ADDR_GLOBAL)) return;

    if (req_pgn == PGN_ADDRESS_CLAIM) {
        (void)send_addr_claim();
        /* ISO Ack (59392) is optional here; the standard permits omitting it. */
    }
}

/* ===== API ===== */

esp_err_t n2k_addr_init(const n2k_addr_cfg_t *cfg)
{
    esp_err_t err;

    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!S.inited) {
        memset(&S,0,sizeof(S));
        S.inited = true;
    }

    S.name = cfg->name;
    S.sa   = cfg->preferred_sa;
    if (S.sa == 0xFE || S.sa == 0xFF) S.sa = 0; /* Safe fallback. */
    S.pri  = cfg->priority ? cfg->priority : 6;

    n2k_transport_set_local_sa(S.sa);

    /* Subscribe to PGNs 60928 and 59904. */
    err = n2k_transport_subscribe(PGN_ADDRESS_CLAIM, on_pgn_60928, NULL);
    if (err != ESP_OK) return err;

    return n2k_transport_subscribe(PGN_ISO_REQUEST, on_pgn_59904, NULL);
}

esp_err_t n2k_addr_start_claim(void)
{
    if (!S.inited) return ESP_ERR_INVALID_STATE;
    return send_addr_claim();
}

uint8_t n2k_addr_get_sa(void) { return S.sa; }

esp_err_t n2k_addr_set_sa(uint8_t sa)
{
    if (!S.inited) return ESP_ERR_INVALID_STATE;
    if (sa==0xFE || sa==0xFF) return ESP_ERR_INVALID_ARG;
    S.sa = sa;
    n2k_transport_set_local_sa(S.sa);
    return send_addr_claim();
}

esp_err_t n2k_addr_set_on_change(n2k_sa_changed_cb_t cb, void *user)
{
    S.onchg = cb; S.onchg_user = user; return ESP_OK;
}
