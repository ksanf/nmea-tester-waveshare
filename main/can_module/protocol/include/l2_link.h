/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L2: parse/build 29-bit IDs, route traffic, and transmit frames up to 8 bytes.
 */

#pragma once
#include "sailor_proto_common.h"
#include "can_driver.h"   /* L1 driver */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sp_l2_rx_cb_t)(const sp_l2_frame_t *frm, void *user);
typedef void (*sp_l2_evt_cb_t)(sp_event_t evt, void *user);

typedef struct {
    uint8_t  local_sa;         /* Local SA */
    bool     use_hw_filters;   /* Configure EF/EA/EE hardware filters when true */
} sp_l2_config_t;

sp_err_t sp_l2_init (const sp_l2_config_t *cfg, sp_l2_rx_cb_t on_rx, sp_l2_evt_cb_t on_evt, void *user);
/* False retains the complete runtime if its RX producer did not stop. */
bool     sp_l2_deinit(void);

/* Transmit an L2 frame (dlc <= 8). The ID is built from fields. */
sp_err_t sp_l2_send(const sp_id_fields_t *id, const uint8_t *data, uint8_t dlc);

/* Convenience helper to pack an ID and transmit immediately. */
static inline sp_err_t sp_l2_send_quick(uint8_t pri, uint8_t dp, uint8_t pf, uint8_t ps, uint8_t sa,
                                        const uint8_t *data, uint8_t dlc) {
    sp_id_fields_t id = {pri, dp, pf, ps, sa};
    return sp_l2_send(&id, data, dlc);
}

#ifdef __cplusplus
}
#endif
