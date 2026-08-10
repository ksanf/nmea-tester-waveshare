/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   UART<->CAN tunnel over the Sailor stack and FSM. Public handler API.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* size_t */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Link state exposed to the UI ---------- */
typedef enum {
    PROTO_STATE_BOOT   = 0,   /* startup/waiting for online */
    PROTO_STATE_ERROR  = 1,   /* error/recovery */
    PROTO_STATE_ONLINE = 2    /* link is active */
} protocol_link_state_t;

/* ---------- UI callbacks ---------- */
typedef void (*protocol_log_cb_t)(bool to_can, const char *line, void *user);
typedef void (*protocol_state_cb_t)(protocol_link_state_t state, void *user);

/* ---------- Configuration ---------- */
typedef struct {
    uint32_t rs_baudrate;   /* UART baud rate, e.g. 115200 */
    uint32_t can_bitrate;   /* CAN nominal bit rate, e.g. 250000 */
} protocol_handler_cfg_t;

typedef enum {
    PROTOCOL_TERM_IO_UART = 0,
    PROTOCOL_TERM_IO_WIFI = 1,
} protocol_term_io_t;

/* ---------- UI callbacks (log lines and state changes) ---------- */
void protocol_handler_set_ui(protocol_log_cb_t log_cb,
                             protocol_state_cb_t state_cb,
                             void *user);

/* ---------- Lifecycle ---------- */
bool protocol_handler_init(const protocol_handler_cfg_t *cfg);
bool protocol_handler_start(void);
bool protocol_handler_stop(void);
bool protocol_handler_deinit(void);
esp_err_t protocol_handler_set_rs_baudrate(uint32_t baud);
void protocol_handler_set_term_io_owner(protocol_term_io_t owner);
protocol_term_io_t protocol_handler_get_term_io_owner(void);
bool protocol_handler_is_term_ready(void);

/* ---------- External byte-stream transmission into the tunnel ---------- */
bool protocol_handler_send_ascii(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
