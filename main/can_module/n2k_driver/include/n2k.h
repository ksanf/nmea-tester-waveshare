/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* ───────── Stack version / build flags ───────── */
#ifndef N2K_STACK_VERSION_MAJOR
#define N2K_STACK_VERSION_MAJOR 1
#endif
#ifndef N2K_STACK_VERSION_MINOR
#define N2K_STACK_VERSION_MINOR 1
#endif
#ifndef N2K_STACK_VERSION_PATCH
#define N2K_STACK_VERSION_PATCH 0
#endif

#define N2K_STACK_VERSION  ((N2K_STACK_VERSION_MAJOR*10000) + (N2K_STACK_VERSION_MINOR*100) + (N2K_STACK_VERSION_PATCH))

/* ───────── Minimum compile-time checks ───────── */
#if !defined(ESP_PLATFORM)
#  warning "This N2K stack targets ESP-IDF. You're compiling outside ESP-IDF?"
#endif

/* Optionally validate the IDF major/minor version when available. */
#ifdef IDF_VER
/* Example: ESP-IDF 5.x */
#endif

/* ───────── CORE DEPENDENCIES ───────── */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

/* ───────── INTERNAL STACK INTERFACES (re-exported) ───────── */
/* L2: parse/build 29-bit IDs, PGNs, and low-level frames */
#include "n2k_iso11783.h"

/* Transport: Single / Fast-Packet / BAM / RTS-CTS, subscriptions, complete-message queue */
#include "n2k_transport.h"

/* Address Claim 60928 and responses to ISO Request (59904) for 60928 */
#include "n2k_addr_claim.h"

/* PGN registry: on_rx/encode and policy (RESPOND_TO_REQUEST/...) */
#include "n2k_pgn.h"

/* ISO Acknowledgment (59392) helper */
#include "n2k_iso_ack.h"

/* Automatic Product (126996) and Config/Installation (126998) responses */
#include "n2k_product.h"

/* Group Function (126208): basic reception with ISO-ACK */
#include "n2k_group_func.h"

/* Bus device directory (SA/NAME/last_seen) */
#include "n2k_device_list.h"


/* ───────── Shared convenience constants (re-export/aliases) ───────── */
#ifndef N2K_ADDR_GLOBAL
#define N2K_ADDR_GLOBAL   0xFFu
#endif
#ifndef N2K_ADDR_NULL
#define N2K_ADDR_NULL     0xFEu
#endif

/* Standard PGNs available to the application */
#ifndef N2K_PGN_ISO_ACK
#define N2K_PGN_ISO_ACK           59392u
#define N2K_PGN_ISO_REQUEST       59904u
#define N2K_PGN_TP_DT             60160u
#define N2K_PGN_TP_CM             60416u
#define N2K_PGN_ADDRESS_CLAIM     60928u
#define N2K_PGN_GROUP_FUNCTION   126208u
#define N2K_PGN_PRODUCT_INFO     126996u
#define N2K_PGN_CONFIG_INFO      126998u
#define N2K_PGN_PROP_FAST        126720u
#define N2K_PGN_PROP_A            61184u
#define N2K_PGN_PROP_B_BASE       65280u
#endif

/* ───────── CONVENIENCE HELPERS (inline) ───────── */

/** Send an ACK/NAK for a PGN to the message source. */
static inline esp_err_t n2k_ack_to(const n2k_msg_t *m, n2k_ack_type_t t) {
    if (!m) return ESP_ERR_INVALID_ARG;
    return n2k_iso_ack_send(6, n2k_addr_get_sa(), m->src, t, m->pgn);
}

/** Initialize the base stack in one call.
 *  Parameters:
 *   - can_init:      the caller must invoke can_driver_init() first
 *   - tcfg:          transport configuration (NULL selects sensible defaults)
 *   - acfg:          Address Claim configuration (NAME and preferred SA are required)
 *   - prod/inst1/2:  optionally register 126996/126998
 *   - enable_group:  enable the 126208 handler (ISO-ACK for Group Function)
 *   - devlist_ms:    >0 enables the device directory with an offline timeout
 */
static inline esp_err_t n2k_stack_init_all(const n2k_transport_cfg_t *tcfg,
                                           const n2k_addr_cfg_t *acfg,
                                           const n2k_product_info_t *prod,
                                           const char *inst1,
                                           const char *inst2,
                                           bool enable_group,
                                           uint32_t devlist_ms)
{
    if (!acfg) return ESP_ERR_INVALID_ARG;

    n2k_transport_cfg_t _tc = {
        .local_sa = acfg->preferred_sa ? acfg->preferred_sa : 0x25,
        .default_priority = 6,
        .bam_dt_gap_ms = 50,
        .fp_slots = 8,
        .tp_slots = 4,
        .rx_queue_len = 16
    };
    if (tcfg) _tc = *tcfg;

    esp_err_t er;
    er = n2k_transport_init(&_tc);          if (er != ESP_OK) return er;
    er = n2k_addr_init(acfg);               if (er != ESP_OK) goto fail;
    er = n2k_addr_start_claim();            if (er != ESP_OK) goto fail;

    if (prod) {
        er = n2k_product_init(prod, inst1, inst2);
        if (er != ESP_OK) goto fail;
    }
    if (enable_group) {
        er = n2k_group_func_init();         if (er != ESP_OK) goto fail;
    }
    if (devlist_ms) {
        er = n2k_devlist_init(devlist_ms);  if (er != ESP_OK) goto fail;
    }
    return ESP_OK;

fail:
    (void)n2k_transport_deinit();
    return er;
}

/* Proprietary transmit shortcuts retained for application convenience. */
static inline esp_err_t n2k_prop_send_A(uint8_t pri, uint8_t sa, uint8_t dst,
                                        const uint8_t *data, uint16_t len, uint32_t to_ms) {
    return n2k_send_auto(pri, N2K_PGN_PROP_A, sa, dst, data, len, to_ms);
}
static inline esp_err_t n2k_prop_send_B(uint8_t pri, uint8_t sa, uint8_t ge,
                                        const uint8_t *data, uint16_t len, uint32_t to_ms) {
    return n2k_send_auto(pri, (uint32_t)N2K_PGN_PROP_B_BASE + ge, sa, N2K_ADDR_GLOBAL, data, len, to_ms);
}
static inline esp_err_t n2k_prop_send_126720(uint8_t pri, uint8_t sa, uint8_t dst,
                                             const uint8_t *data, uint16_t len, uint32_t to_ms) {
    return n2k_send_auto(pri, N2K_PGN_PROP_FAST, sa, dst, data, len, to_ms);
}

#ifdef __cplusplus
} /* extern "C" */
#endif
