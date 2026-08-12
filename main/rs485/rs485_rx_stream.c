/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Hardware-independent RS-485 byte stream decoder.
 */

#include "rs485/rs485_rx_stream.h"

#include <string.h>

static bool nmea_start_char_(uint8_t byte)
{
    return byte >= 0x21u && byte <= 0x7Eu;
}

static bool nmea_char_(uint8_t byte)
{
    return byte >= 0x20u && byte <= 0x7Eu;
}

static void emit_nmea_(rs485_rx_stream_t *stream,
                       rs485_rx_stream_emit_fn emit,
                       void *user)
{
    if (stream->nmea_len >= RS485_RX_STREAM_NMEA_MIN_LEN) {
        stream->nmea_line[stream->nmea_len] = '\0';
        if (emit) {
            emit(RS485_RX_STREAM_EVENT_NMEA,
                 stream->nmea_line, stream->nmea_len, user);
        }
    }
    stream->nmea_len = 0;
}

static void consume_hex_(rs485_rx_stream_t *stream,
                         uint8_t byte,
                         rs485_rx_stream_emit_fn emit,
                         void *user)
{
    char line[RS485_HEX_DUMP_LINE_CAPACITY];
    size_t line_len;

    if (!stream->hex_enabled) return;

    stream->hex_bytes[stream->hex_len++] = byte;
    if (stream->hex_len < RS485_HEX_DUMP_BYTES_PER_LINE) return;

    line_len = rs485_hex_dump_format(line, sizeof(line), stream->hex_offset,
                                     stream->hex_bytes, stream->hex_len);
    if (emit && line_len > 0) {
        emit(RS485_RX_STREAM_EVENT_HEX, line, line_len, user);
    }
    stream->hex_offset += (uint32_t)stream->hex_len;
    stream->hex_len = 0;
}

void rs485_rx_stream_init(rs485_rx_stream_t *stream, bool hex_enabled)
{
    rs485_rx_stream_reset(stream, hex_enabled);
}

void rs485_rx_stream_reset(rs485_rx_stream_t *stream, bool hex_enabled)
{
    if (!stream) return;
    memset(stream, 0, sizeof(*stream));
    stream->hex_enabled = hex_enabled;
}

void rs485_rx_stream_feed(rs485_rx_stream_t *stream,
                          const uint8_t *data,
                          size_t len,
                          rs485_rx_stream_emit_fn emit,
                          void *user)
{
    if (!stream || (!data && len > 0)) return;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = data[i];

        consume_hex_(stream, byte, emit, user);

        if (stream->nmea_len == 0 && !nmea_start_char_(byte)) {
            continue;
        }

        if (!nmea_char_(byte) ||
            stream->nmea_len >= RS485_RX_STREAM_NMEA_CAPACITY - 1u) {
            emit_nmea_(stream, emit, user);
            continue;
        }

        stream->nmea_line[stream->nmea_len++] = (char)byte;
    }
}
