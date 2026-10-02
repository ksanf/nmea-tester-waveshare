/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Lifecycle calls are serialized by the protocol handler. Polling may block on
 * CAN/profile locks without delaying ESP_TIMER_TASK or LVGL's clock. */
typedef void (*protocol_poll_fn_t)(void);
bool protocol_poll_worker_start(protocol_poll_fn_t poll);
bool protocol_poll_worker_stop(uint32_t timeout_ms);
bool protocol_poll_worker_is_running(void);
