/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Hardware-independent RS-485 byte stream decoder.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rs485/rs485_hex_dump.h"

#define RS485_RX_STREAM_NMEA_MIN_LEN  6u
#define RS485_RX_STREAM_NMEA_CAPACITY 256u

typedef enum {
    RS485_RX_STREAM_EVENT_NMEA = 0,
    RS485_RX_STREAM_EVENT_HEX,
} rs485_rx_stream_event_kind_t;

typedef void (*rs485_rx_stream_emit_fn)(rs485_rx_stream_event_kind_t kind,
                                        const char *line,
                                        size_t len,
                                        void *user);

typedef struct {
    char nmea_line[RS485_RX_STREAM_NMEA_CAPACITY];
    size_t nmea_len;
    uint8_t hex_bytes[RS485_HEX_DUMP_BYTES_PER_LINE];
    size_t hex_len;
    uint32_t hex_offset;
    bool hex_enabled;
} rs485_rx_stream_t;

void rs485_rx_stream_init(rs485_rx_stream_t *stream, bool hex_enabled);
void rs485_rx_stream_reset(rs485_rx_stream_t *stream, bool hex_enabled);
void rs485_rx_stream_feed(rs485_rx_stream_t *stream,
                          const uint8_t *data,
                          size_t len,
                          rs485_rx_stream_emit_fn emit,
                          void *user);
