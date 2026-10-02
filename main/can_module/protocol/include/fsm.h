/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * TT6006 profile: address claim, discovery and independent NDP clients.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sailor_proto_common.h"
#include "sailor_telemetry.h"
#include "l2_link.h"
#include "l3l4_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SP_ST_BOOT = 0,     /* No address claimed yet. */
    SP_ST_CLAIM,        /* Waiting for address-claim arbitration. */
    SP_ST_WAIT_POLL,    /* Discovering the antenna or opening its services. */
    SP_ST_ONLINE,       /* Terminal NDP connection is established. */
    SP_ST_OFF          /* No source address could be claimed. */
} sp_state_t;

typedef struct {
    uint8_t local_sa;      /* Preferred source address; zero is valid. */
    uint8_t local_name[8]; /* Optional CAN NAME, LE64; all zero derives it from MAC. */
} sp_fsm_config_t;

sp_err_t sp_fsm_init(const sp_fsm_config_t *cfg);
void sp_fsm_deinit(void);
/* Call at least every 100 ms. Reads the monotonic clock after locking state. */
void sp_fsm_tick(void);
sp_state_t sp_fsm_get_state(void);
/* Current antenna source address, or 0xff if unavailable. */
uint8_t sp_fsm_get_peer_sa(void);
void sp_fsm_get_antenna_status(protocol_antenna_status_t *out);

/* Accept RX only when the consumer retained the entire message. */
typedef bool (*sp_fsm_term_rx_cb_t)(const uint8_t *data, uint16_t len, void *user);
void sp_fsm_set_term_rx(sp_fsm_term_rx_cb_t callback, void *user);
sp_err_t sp_fsm_term_send(const uint8_t *data, uint16_t len);
void sp_fsm_term_cancel_pending(void);
bool sp_fsm_term_input_can_send(void);

sp_err_t sp_fsm_bind_layers(const sp_l2_config_t *l2_cfg,
                             const sp_timing_t *tr_timing,
                             uint16_t tr_max_slots,
                             sp_l2_evt_cb_t on_l2_evt,
                             sp_tr_evt_cb_t on_tr_evt,
                             void *user);
bool sp_fsm_unbind_layers(void);

#ifdef __cplusplus
}
#endif
