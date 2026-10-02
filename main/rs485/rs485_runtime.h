/* Screen-independent RS-485 services. Lifecycle calls are serialized by the app controller. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RS485_EVENT_NMEA = 1, RS485_EVENT_HEX, RS485_EVENT_TX,
    RS485_EVENT_BRIDGE_RX, RS485_EVENT_BRIDGE_TX
} rs485_runtime_event_kind_t;
typedef struct {
    uint32_t sequence;
    uint32_t time_ms;
    uint8_t kind;
    uint16_t length;
    char data[384];
} rs485_runtime_event_t;

typedef struct {
    bool running, hex_mode, paused;
    uint32_t baud, last_rx_ms, last_ais_ms, bytes_received, lines_received;
    uint8_t last_byte;
} rs485_parser_status_t;
bool rs485_parser_runtime_start(void);
bool rs485_parser_runtime_stop(void);
bool rs485_parser_runtime_running(void);
esp_err_t rs485_parser_runtime_set_baud(uint32_t baud);
void rs485_parser_runtime_status(rs485_parser_status_t *out);
void rs485_parser_runtime_set_hex(bool enabled);
void rs485_parser_runtime_set_paused(bool paused);
void rs485_parser_runtime_refresh(void);
bool rs485_parser_runtime_read(uint32_t *cursor, rs485_runtime_event_t *out, uint32_t *dropped);

bool rs485_simui_runtime_start(void);
bool rs485_simui_runtime_stop(void);
bool rs485_simui_runtime_running(void);
esp_err_t rs485_simui_runtime_set_baud(uint32_t baud);
esp_err_t rs485_simui_runtime_send(const char *line);
bool rs485_simui_runtime_read(uint32_t *cursor, rs485_runtime_event_t *out, uint32_t *dropped);

typedef struct {
    bool running;
    uint32_t baud, bytes_to_uart, bytes_from_uart, packets_to_uart, packets_from_uart;
} rs485_bridge_status_t;
bool rs485_bridge_runtime_start(void);
bool rs485_bridge_runtime_stop(void);
bool rs485_bridge_runtime_running(void);
esp_err_t rs485_bridge_runtime_set_baud(uint32_t baud);
esp_err_t rs485_bridge_runtime_write(const uint8_t *data, size_t length);
void rs485_bridge_runtime_status(rs485_bridge_status_t *out);
bool rs485_bridge_runtime_read(uint32_t *cursor, rs485_runtime_event_t *out, uint32_t *dropped);
