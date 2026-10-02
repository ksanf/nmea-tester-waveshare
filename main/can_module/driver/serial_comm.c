/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "serial_comm.h"

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_SERIAL_COMM
#include "config_logs.h"
#include <driver/uart.h>

#include "rs485/rs485_driver.h"
#include "config/config_nmea_tester.h"

static const char *TAG = "serial_comm";

esp_err_t serial_comm_init(uint32_t baud)
{
    if (baud == 0) baud = RS485_BAUD_DEFAULT;
    esp_err_t err = rs485_acquire(RS485_OWNER_CAN_BRIDGE, baud);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "RS-485 init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS-485 initialized, baud: %u", baud);
    return ESP_OK;
}

esp_err_t serial_comm_deinit(void)
{
    esp_err_t err = rs485_release(RS485_OWNER_CAN_BRIDGE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RS-485 deinitialized");
    } else {
        ESP_LOGW(TAG, "RS-485 release failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t serial_comm_write(const char *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rs485_driver_write(data, len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "RS-485 write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t serial_comm_read(uint8_t *buf, size_t *len)
{
    if (!buf || !len || *len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = rs485_driver_read(buf, len);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "RS-485 read failed: %s", esp_err_to_name(err));
    }
    return err;
}

size_t serial_comm_available(void)
{
    return rs485_driver_available();
}

esp_err_t serial_comm_flush_input(void)
{
    return uart_flush_input(RS485_UART_PORT);
}

esp_err_t serial_comm_set_baudrate(uint32_t baud)
{
    return rs485_set_baudrate(baud);
}
