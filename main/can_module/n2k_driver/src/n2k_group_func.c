/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "n2k_group_func.h"
#include "n2k_pgn.h"
#include "n2k_iso_ack.h"
#include "n2k_addr_claim.h"
#include <string.h>

static void on_126208(const n2k_msg_t *m, void *user) {
    (void)user;
    /* Minimum handling: send ISO Ack OK for PGN 126208 to the source address. */
    (void)n2k_iso_ack_send(6, n2k_addr_get_sa(), m->src, N2K_ACK, 126208);
}

esp_err_t n2k_group_func_init(void) {
    esp_err_t err;

    err = n2k_pgn_init();
    if (err != ESP_OK) return err;
    n2k_pgn_desc_t d = {
        .pgn = 126208,
        .policy = N2K_PGN_PASSIVE,
        .on_rx = on_126208,
        .encode = NULL,
        .user = NULL
    };
    return n2k_pgn_register(&d);
}
