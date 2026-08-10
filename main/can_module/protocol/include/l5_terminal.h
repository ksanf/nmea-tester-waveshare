/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L5 PC-to-antenna terminal tunnel over L3L4, hiding CAN IDs from callers.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sailor_proto_common.h"   /* Types */
#include "l3l4_transport.h"        /* sp_app_block_t, sp_tr_send_app */
#include "strategy_map.h"

#ifdef __cplusplus
extern "C" { 
#endif

typedef struct { uint8_t reserved; } sp_term_config_t;

/* RX upward: raw APP data; is_echo is true when len==1. */
typedef void (*sp_term_rx_cb_t)(const uint8_t *data, uint16_t len, bool is_echo, void *user);

sp_err_t sp_term_init (const sp_term_config_t *cfg, sp_term_rx_cb_t on_rx, void *user);
void     sp_term_deinit(void);

/* TX: send a terminal data block, including len=1.
 * series_cnt becomes B3 in the L4 header.
 * The strategy supplies the ID/channel/signatures; L3L4 performs FP framing and FF padding.
 */
sp_err_t sp_term_send(const uint8_t *data, uint16_t len, uint8_t series_cnt);
sp_err_t sp_term_send_byte(uint8_t ch, uint8_t series_cnt);
void     sp_term_set_tx_channel(sp_channel_t ch);

/* RX entry from L3L4: pass parsed blocks whose channel is TERM_OUT here. */
void sp_term_on_app(sp_channel_t ch, uint8_t series_cnt,
                    const uint8_t *app_data, uint16_t app_len,
                    const sp_id_fields_t *id);

#ifdef __cplusplus
}
#endif
