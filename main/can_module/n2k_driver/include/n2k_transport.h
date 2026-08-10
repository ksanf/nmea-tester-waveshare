/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA2000/J1939 transport: Single, Fast-Packet, BAM, RTS/CTS, and PGN subscriptions.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "n2k_iso11783.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Default parameters ---- */
#ifndef N2K_RX_QUEUE_LEN
#define N2K_RX_QUEUE_LEN            8
#endif
#ifndef N2K_MSG_MAX_DATA
#define N2K_MSG_MAX_DATA         1785
#endif
#ifndef N2K_FP_REASM_SLOTS
#define N2K_FP_REASM_SLOTS          8
#endif
#ifndef N2K_TP_REASM_SLOTS
#define N2K_TP_REASM_SLOTS          4
#endif
#ifndef N2K_FP_TIMEOUT_MS
#define N2K_FP_TIMEOUT_MS         250
#endif
#ifndef N2K_TP_TIMEOUT_MS
#define N2K_TP_TIMEOUT_MS        1500
#endif
#ifndef N2K_TP_BAM_DT_GAP_MS
#define N2K_TP_BAM_DT_GAP_MS       50
#endif
#ifndef N2K_TP_RTS_WINDOW
#define N2K_TP_RTS_WINDOW          16   /* Packets per CTS */
#endif
#ifndef N2K_TP_EVENT_QUEUE_LEN
#define N2K_TP_EVENT_QUEUE_LEN     10   /* CM events used while TX waits */
#endif
#ifndef N2K_MAX_SUBSCRIBERS
#define N2K_MAX_SUBSCRIBERS        16
#endif
#ifndef N2K_RX_TASK_STACK
#define N2K_RX_TASK_STACK        6144
#endif
#ifndef N2K_RX_TASK_PRIO
#define N2K_RX_TASK_PRIO            5
#endif
#ifndef N2K_RX_TASK_CORE
#define N2K_RX_TASK_CORE            0
#endif
#ifndef N2K_PGN_ANY
#define N2K_PGN_ANY        0xFFFFFFFFu
#endif

/* ---- Complete reassembled message ---- */
typedef struct {
    uint32_t pgn;
    uint8_t  priority;
    uint8_t  src;
    uint8_t  dst;

    uint16_t len;              /* Full TP payload length for NMEA2000/J1939 */
    uint8_t  data[N2K_MSG_MAX_DATA];

    uint64_t timestamp_us;
    bool     from_fast_packet;
} n2k_msg_t;

/* ---- Initialization/deinitialization ---- */
typedef struct {
    uint8_t local_sa;           /* Local SA */
    uint8_t default_priority;   /* Usually 6 */
    uint8_t bam_dt_gap_ms;      /* BAM DT interval (minimum delay) */
    uint8_t fp_slots;           /* reassembly FP */
    uint8_t tp_slots;           /* reassembly BAM/RTS RX */
    uint8_t rx_queue_len;       /* Complete-message queue; 0 disables the app RX queue */
} n2k_transport_cfg_t;

esp_err_t n2k_transport_init(const n2k_transport_cfg_t *cfg);
esp_err_t n2k_transport_deinit(void);

/* Update the SA, for example after Address Claim. */
void      n2k_transport_set_local_sa(uint8_t sa);

/* ---- Receive complete messages ---- */
esp_err_t n2k_recv(n2k_msg_t *out, uint32_t timeout_ms);

/* ---- Transmission ---- */
esp_err_t n2k_send_auto(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                        const uint8_t *data, uint16_t len, uint32_t timeout_ms);

esp_err_t n2k_send_single(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                          const uint8_t *data, uint8_t len, uint32_t timeout_ms);

esp_err_t n2k_send_fast_packet(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                               const uint8_t *data, uint16_t len, uint32_t timeout_ms);

esp_err_t n2k_send_bam(uint8_t priority, uint32_t pgn, uint8_t sa,
                       const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* Addressed RTS/CTS transmission (when >223 bytes and dst != 0xFF). */
esp_err_t n2k_send_rtscts(uint8_t priority, uint32_t pgn, uint8_t sa, uint8_t dst,
                          const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* ---- Observer subscriptions ---- */
typedef void (*n2k_rx_cb_t)(const n2k_msg_t *msg, void *user);

/* Subscribe to a PGN; the callback receives a copy while the original remains in the shared queue. */
esp_err_t n2k_transport_subscribe(uint32_t pgn, n2k_rx_cb_t cb, void *user);

/* ---- Statistics ---- */
typedef struct {
    uint32_t rx_single;
    uint32_t rx_fast_packet_ok;
    uint32_t rx_fast_packet_drop_to;
    uint32_t rx_bam_ok;
    uint32_t rx_bam_drop_to;
    uint32_t rx_rts_ok;
    uint32_t rx_rts_drop_to;
    uint32_t rx_rts_abort;
    uint32_t rx_queue_drop;

    uint32_t tx_single;
    uint32_t tx_fast_packet;
    uint32_t tx_bam;
    uint32_t tx_rtscts;
} n2k_transport_stats_t;

void n2k_transport_get_stats(n2k_transport_stats_t *out);

#ifdef __cplusplus
}
#endif
