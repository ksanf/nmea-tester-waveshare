/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "n2k_product.h"
#include "n2k_pgn.h"
#include "n2k_addr_claim.h"
#include "n2k_transport.h"
#include <string.h>

/* ----------- Helpers for building PGNs 126996 / 126998 ----------- */

/* Experimental compact response for PGN 126996.
 * This project-specific representation is useful for the built-in monitor but
 * is not the fixed-length NMEA 2000 Product Information payload.
 */
static n2k_product_info_t G_info;
static char G_inst1[41], G_inst2[41];

static int enc_126996(uint8_t *out, uint16_t cap, void *user) {
    (void)user;
    /* Build the project-specific compact, length-prefixed payload. */
    uint8_t *p=out; uint8_t *e=out+cap;
    if (p+2 > e) return -1;
    /* Manufacturer code LSB first */
    *p++ = (uint8_t)(G_info.manufacturer_code & 0xFF);
    *p++ = (uint8_t)((G_info.manufacturer_code>>8) & 0xFF);
    /* Unique ID (3 least-significant bytes) */
    if (p+3 > e) return -1;
    *p++ = (uint8_t)(G_info.unique_id & 0xFF);
    *p++ = (uint8_t)((G_info.unique_id>>8) & 0xFF);
    *p++ = (uint8_t)((G_info.unique_id>>16) & 0xFF);

    /* Length-prefixed Model, SW, HW, and Serial strings: 0..32/20/20/20 bytes. */
    const char *S1=G_info.model_id;      uint8_t L1=strnlen(S1,32);
    const char *S2=G_info.sw_version;    uint8_t L2=strnlen(S2,20);
    const char *S3=G_info.hw_version;    uint8_t L3=strnlen(S3,20);
    const char *S4=G_info.serial_code;   uint8_t L4=strnlen(S4,20);

    #define PUT_STR(S,L) do{ if (p+1+L > e) return -1; *p++=(L); memcpy(p,(S),L); p+=L; }while(0)

    PUT_STR(S1,L1);
    PUT_STR(S2,L2);
    PUT_STR(S3,L3);
    PUT_STR(S4,L4);

    return (int)(p-out);
}

/* Experimental compact response for PGN 126998 with two length-prefixed strings. */
static int enc_126998(uint8_t *out, uint16_t cap, void *user) {
    (void)user;
    uint8_t *p=out,*e=out+cap;
    uint8_t L1=strnlen(G_inst1,40);
    uint8_t L2=strnlen(G_inst2,40);
    if (p+1+L1+1+L2 > e) return -1;
    *p++=L1; memcpy(p,G_inst1,L1); p+=L1;
    *p++=L2; memcpy(p,G_inst2,L2); p+=L2;
    return (int)(p-out);
}

/* These PGNs do not need an RX handler because we only answer requests,
   but retain the option to intercept incoming messages if needed later. */
static void rx_noop(const n2k_msg_t *m, void *u){ (void)m; (void)u; }

esp_err_t n2k_product_init(const n2k_product_info_t *info,
                           const char *inst_info1, const char *inst_info2)
{
    esp_err_t err;

    if (!info) return ESP_ERR_INVALID_ARG;
    memset(&G_info,0,sizeof(G_info));
    G_info = *info;
    memset(G_inst1,0,sizeof(G_inst1));
    memset(G_inst2,0,sizeof(G_inst2));
    if (inst_info1) strncpy(G_inst1, inst_info1, sizeof(G_inst1)-1);
    if (inst_info2) strncpy(G_inst2, inst_info2, sizeof(G_inst2)-1);

    err = n2k_pgn_init();
    if (err != ESP_OK) return err;

    n2k_pgn_desc_t d1 = {
        .pgn = 126996,
        .policy = N2K_PGN_RESPOND_TO_REQUEST,
        .on_rx = rx_noop,
        .encode = enc_126996,
        .user = NULL
    };
    n2k_pgn_desc_t d2 = {
        .pgn = 126998,
        .policy = N2K_PGN_RESPOND_TO_REQUEST,
        .on_rx = rx_noop,
        .encode = enc_126998,
        .user = NULL
    };
    err = n2k_pgn_register(&d1);
    if (err != ESP_OK) return err;
    return n2k_pgn_register(&d2);
}
