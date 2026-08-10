/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    RS485_OWNER_NONE = 0,
    RS485_OWNER_PARSER,
    RS485_OWNER_TRANSMITTER,
    RS485_OWNER_NETWORK_BRIDGE,
    RS485_OWNER_CAN_BRIDGE,
} rs485_owner_t;

/**
 * @brief Acquire exclusive ownership of the RS-485 UART for an application mode.
 * @param owner Mode that will use the port.
 * @param baud Baud rate (0 selects RS485_BAUD_DEFAULT).
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if another mode owns the port.
 */
esp_err_t rs485_acquire(rs485_owner_t owner, uint32_t baud);

/**
 * @brief Release the UART. A different mode cannot uninstall the active driver.
 * @param owner Mode that previously acquired the port through rs485_acquire().
 */
esp_err_t rs485_release(rs485_owner_t owner);

/**
 * @brief Return the current owner of the RS-485 UART.
 */
rs485_owner_t rs485_get_owner(void);

/**
 * @brief Change the RS-485 UART baud rate at runtime.
 * @param baud New baud rate (0 returns ESP_ERR_INVALID_ARG).
 */
esp_err_t rs485_set_baudrate(uint32_t baud);

/**
 * @brief Return the current RS-485 UART baud rate.
 * @return Most recently applied baud rate.
 */
uint32_t rs485_get_baudrate(void);

/**
 * @brief  Transmit data over RS-485 (thread-safe).
 * @param  data  Data buffer.
 * @param  len   Data length in bytes.
 */
esp_err_t rs485_driver_write(const char *data, size_t len);

/**
 * @brief  Receive RS-485 data by polling (thread-safe).
 * @param[in]     buf  Receive buffer.
 * @param[in,out] len  [in] buffer capacity, [out] bytes actually read.
 */
esp_err_t rs485_driver_read(uint8_t *buf, size_t *len);

/**
 * @brief  Receive RS-485 data with an explicit timeout.
 * @param[in]     buf         Receive buffer.
 * @param[in,out] len         [in] buffer capacity, [out] bytes actually read.
 * @param[in]     timeout_ms  Time to wait for the first byte.
 * @return ESP_OK if at least one byte was read, or ESP_ERR_TIMEOUT if no data arrived.
 */
esp_err_t rs485_driver_read_timeout(uint8_t *buf, size_t *len, uint32_t timeout_ms);

/**
 * @brief  Read already-buffered bytes from the RX FIFO without blocking.
 * @param[in]     buf  Receive buffer.
 * @param[in,out] len  [in] buffer capacity, [out] bytes actually read.
 * @return ESP_OK if at least one byte was read, or ESP_ERR_TIMEOUT if no data is available.
 */
esp_err_t rs485_driver_read_nowait(uint8_t *buf, size_t *len);

/**
 * @brief  Return the number of bytes already buffered in the UART RX FIFO.
 * @return Available byte count, or 0 if no data is available or the bus is busy.
 */
size_t rs485_driver_available(void);
