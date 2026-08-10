/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   L5 service/application exchange over L4 on the SERVICE_APP channel.
 */

#pragma once
#include "sailor_proto_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sp_service_rx_cb_t)(const uint8_t *data, uint16_t len, void *user);

sp_err_t sp_service_init(sp_service_rx_cb_t on_rx, void *user);
void     sp_service_deinit(void);

/* Downward: send a service message on the SERVICE_APP channel.
 * seq:
 *   - 0xFF: automatic internal rolling series 0x08..0x0F
 *   - otherwise: force this series in the APP header
 */
sp_err_t sp_service_send(const uint8_t *data, uint16_t len, uint8_t seq,
                         const sp_id_fields_t *id_for_l3);

/* Upward: deliver a parsed SERVICE_APP block from L4. */
void     sp_service_on_l4_app(uint8_t seq, const uint8_t *app_data, uint16_t app_len);

#ifdef __cplusplus
}
#endif
