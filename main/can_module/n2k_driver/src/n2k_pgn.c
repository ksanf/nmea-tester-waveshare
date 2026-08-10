/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "n2k_pgn.h"
#include "n2k_iso11783.h"
#include "n2k_addr_claim.h"
#include "config/memory_config.h"
#include <string.h>
#include <stdlib.h>

#define MAX_REG_PGNS  24

static struct {
    n2k_pgn_desc_t reg[MAX_REG_PGNS];
    int count;
    bool inited;
} G;

static int find_index(uint32_t pgn) {
    for (int i = 0; i < G.count; i++) if (G.reg[i].pgn == pgn) return i;
    return -1;
}

static const n2k_pgn_desc_t* find(uint32_t pgn) {
    int idx = find_index(pgn);
    if (idx >= 0) return &G.reg[idx];
    return NULL;
}

/* Entry point for incoming messages, registered as a transport subscriber. */
static void on_rx_any(const n2k_msg_t *m, void *user) {
    (void)user;
    const n2k_pgn_desc_t *d = find(m->pgn);
    if (d && d->on_rx) d->on_rx(m, d->user);
}

/* For ISO Request 59904, find a registered PGN with policy=RESPOND_TO_REQUEST. */
static void on_rx_59904(const n2k_msg_t *m, void *user) {
    (void)user;
    if (m->len < 3) return;
    uint32_t req_pgn = (uint32_t)m->data[0] | ((uint32_t)m->data[1]<<8) | ((uint32_t)m->data[2]<<16);
    const n2k_pgn_desc_t *d = find(req_pgn);
    if (!d || d->policy != N2K_PGN_RESPOND_TO_REQUEST || !d->encode) return;

    uint8_t *buf = (uint8_t *)MALLOC_WHERE(N2K_PAYLOAD_IN_PSRAM, N2K_MSG_MAX_DATA);
    if (!buf) return;
    int n = d->encode(buf, N2K_MSG_MAX_DATA, d->user);
    if (n <= 0 || n > N2K_MSG_MAX_DATA) {
        free(buf);
        return;
    }
    uint8_t sa = n2k_addr_get_sa();
    uint8_t dst = (m->dst == N2K_ADDR_GLOBAL) ? N2K_ADDR_GLOBAL : m->src; /* Addressed reply for an addressed request. */
    (void)n2k_send_auto(6, d->pgn, sa, dst, buf, (uint16_t)n, 20);
    free(buf);
}

esp_err_t n2k_pgn_init(void) {
    esp_err_t err;

    if (!G.inited) {
        memset(&G,0,sizeof(G));
        G.inited = true;
    }

    err = n2k_transport_subscribe(N2K_PGN_ANY, on_rx_any, NULL);
    if (err != ESP_OK) return err;

    err = n2k_transport_subscribe(59904, on_rx_59904, NULL);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t n2k_pgn_register(const n2k_pgn_desc_t *desc) {
    int idx;

    if (!G.inited) return ESP_ERR_INVALID_STATE;
    if (!desc) return ESP_ERR_INVALID_ARG;

    idx = find_index(desc->pgn);
    if (idx >= 0) {
        G.reg[idx] = *desc;
        return ESP_OK;
    }

    if (G.count >= MAX_REG_PGNS) return ESP_ERR_NO_MEM;
    G.reg[G.count++] = *desc;
    G.inited = true;
    return ESP_OK;
}

esp_err_t n2k_pgn_send_encoded(uint8_t priority, uint8_t sa, uint8_t dst,
                               const n2k_pgn_desc_t *d)
{
    if (!d || !d->encode) return ESP_ERR_INVALID_ARG;
    uint8_t *buf = (uint8_t *)MALLOC_WHERE(N2K_PAYLOAD_IN_PSRAM, N2K_MSG_MAX_DATA);
    if (!buf) return ESP_ERR_NO_MEM;
    int n = d->encode(buf, N2K_MSG_MAX_DATA, d->user);
    if (n <= 0 || n > N2K_MSG_MAX_DATA) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = n2k_send_auto(priority, d->pgn, sa, dst, buf, (uint16_t)n, 20);
    free(buf);
    return err;
}
