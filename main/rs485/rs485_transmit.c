/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Transmit NMEA messages over RS-485 for NMEA Tester.
 */

#include "rs485/rs485_transmit.h"
#include "config/config_nmea_tester.h"
#include "nmea_editor/nmea_wire.h"
#include "rs485/rs485_driver.h"
#include "rs485/rs485_simui.h"
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_RS485_TRANSMIT
#include "config_logs.h"

static const char *TAG = "rs485_transmit";

/**
 * @brief Send an NMEA sentence to the RS-485 port and display it in the log.
 * @param msg NMEA sentence including the trailing \r\n.
 */
void rs485_transmit_send(const char *msg)
{
    esp_err_t err = rs485_simui_runtime_send(msg);
    if (err != ESP_OK) ESP_LOGW(TAG, "RS485 TX failed: %s", esp_err_to_name(err));
}
