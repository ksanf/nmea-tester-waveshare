/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   CAN Bridge UI and tasks for the Sailor Inmarsat-C terminal tunnel.
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <lvgl.h>
#include "protocol_handler.h"

typedef struct {
    bool running;
    bool terminal_ready;
    protocol_link_state_t state;
    protocol_term_io_t owner;
    uint32_t can_bitrate;
    uint32_t rs_baudrate;
    size_t terminal_pending;
    uint32_t terminal_dropped;
    protocol_antenna_status_t antenna;
} bridge_can_status_t;

/* No LVGL access. Lifecycle is serialized by the app controller. */
esp_err_t bridge_can_runtime_start(uint32_t rs_baud);
bool bridge_can_runtime_stop(void);
bool bridge_can_runtime_running(void);
void bridge_can_runtime_status(bridge_can_status_t *out);
void bridge_can_set_web_terminal(bool enabled);
esp_err_t bridge_can_terminal_write(const uint8_t *data, size_t len);
size_t bridge_can_terminal_drain(uint8_t *out, size_t cap);

/* LVGL lock required. These never change the protocol runtime. */
void bridge_can_view_suspend(void);
lv_obj_t *bridge_can_view_show(lv_obj_t *parent);
void bridge_can_view_destroy(void);
lv_obj_t *bridge_can_create(lv_obj_t *parent);
void bridge_can_module_start(lv_obj_t *parent);
