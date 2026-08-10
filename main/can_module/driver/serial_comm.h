/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Serial transport abstraction for the bridge path.
 */

#ifndef SERIAL_COMM_H
#define SERIAL_COMM_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t serial_comm_init(uint32_t baud);
void serial_comm_deinit(void);
esp_err_t serial_comm_write(const char *data, size_t len);
esp_err_t serial_comm_read(uint8_t *buf, size_t *len);
size_t serial_comm_available(void);
esp_err_t serial_comm_flush_input(void);
esp_err_t serial_comm_set_baudrate(uint32_t baud);

#endif
