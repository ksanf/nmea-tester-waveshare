/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Configuration for the Sailor CAN bridge (TT-3027C).
 */

#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "config_logs.h"

/* ---------- Core affinity ---------- */
#ifndef BRIDGE_CAN_CORE_BG
#define BRIDGE_CAN_CORE_BG 1
#endif
/* UART->CAN RX task can block on RS-485 bus lock/read timeouts.
 * Keep it off UI core so touch/HOME remains responsive under load. */
#ifndef BRIDGE_CAN_CORE_TERM
#define BRIDGE_CAN_CORE_TERM 0
#endif

/* ---------- Stack helpers ---------- */
#ifndef BRIDGE_CAN_STACK_BYTES_FROM_KB
/* ESP-IDF task stack sizes are specified in bytes, not FreeRTOS words. */
#define BRIDGE_CAN_STACK_BYTES_FROM_KB(kb) ((kb) * 1024)
#endif

/* ---------- Task priorities ---------- */
#ifndef BRIDGE_CAN_PRIO_TERMINAL
#define BRIDGE_CAN_PRIO_TERMINAL 6
#endif
#ifndef BRIDGE_CAN_PRIO_PUMP
#define BRIDGE_CAN_PRIO_PUMP 3
#endif

/* ---------- Task stacks (KB) ---------- */
#ifndef BRIDGE_CAN_STACK_TERMINAL_KB
#define BRIDGE_CAN_STACK_TERMINAL_KB 4
#endif
#ifndef BRIDGE_CAN_STACK_PUMP_KB
#define BRIDGE_CAN_STACK_PUMP_KB 4
#endif

#define BRIDGE_CAN_STACK_TERMINAL BRIDGE_CAN_STACK_BYTES_FROM_KB(BRIDGE_CAN_STACK_TERMINAL_KB)
#define BRIDGE_CAN_STACK_PUMP     BRIDGE_CAN_STACK_BYTES_FROM_KB(BRIDGE_CAN_STACK_PUMP_KB)

/* ---------- Queue lengths ---------- */
#ifndef BRIDGE_CAN_Q_TERM_EVT_LEN
#define BRIDGE_CAN_Q_TERM_EVT_LEN 128
#endif

/* ---------- Transport resources ---------- */
#ifndef BRIDGE_CAN_TR_MAX_SLOTS
#define BRIDGE_CAN_TR_MAX_SLOTS 16
#endif

/* ---------- L7 (terminal tunnel) ---------- */
#ifndef BRIDGE_CAN_L7_APPEND_CRLF
#define BRIDGE_CAN_L7_APPEND_CRLF 1
#endif

/* ---------- Unified logging macros ---------- */
#if BRIDGE_CAN_LOG_HANDLER
#define HLOGI(tag, fmt, ...) CFG_ESP_LOGI_RAW(tag, fmt, ##__VA_ARGS__)
#define HLOGW(tag, fmt, ...) CFG_ESP_LOGW_RAW(tag, fmt, ##__VA_ARGS__)
#define HLOGE(tag, fmt, ...) CFG_ESP_LOGE_RAW(tag, fmt, ##__VA_ARGS__)
#else
#define HLOGI(tag, fmt, ...) (void)0
#define HLOGW(tag, fmt, ...) (void)0
#define HLOGE(tag, fmt, ...) (void)0
#endif

#if BRIDGE_CAN_LOG_L3
#define L3LOGI(tag, fmt, ...) CFG_ESP_LOGI_RAW(tag, fmt, ##__VA_ARGS__)
#define L3LOGW(tag, fmt, ...) CFG_ESP_LOGW_RAW(tag, fmt, ##__VA_ARGS__)
#define L3LOGE(tag, fmt, ...) CFG_ESP_LOGE_RAW(tag, fmt, ##__VA_ARGS__)
#else
#define L3LOGI(tag, fmt, ...) (void)0
#define L3LOGW(tag, fmt, ...) (void)0
#define L3LOGE(tag, fmt, ...) (void)0
#endif

#if BRIDGE_CAN_LOG_L7
#define L7LOGI(tag, fmt, ...) CFG_ESP_LOGI_RAW(tag, fmt, ##__VA_ARGS__)
#define L7LOGW(tag, fmt, ...) CFG_ESP_LOGW_RAW(tag, fmt, ##__VA_ARGS__)
#else
#define L7LOGI(tag, fmt, ...) (void)0
#define L7LOGW(tag, fmt, ...) (void)0
#endif

#if BRIDGE_CAN_LOG_PCMT
#define PCMTLOGI(tag, fmt, ...) CFG_ESP_LOGI_RAW(tag, fmt, ##__VA_ARGS__)
#define PCMTLOGW(tag, fmt, ...) CFG_ESP_LOGW_RAW(tag, fmt, ##__VA_ARGS__)
#else
#define PCMTLOGI(tag, fmt, ...) (void)0
#define PCMTLOGW(tag, fmt, ...) (void)0
#endif
