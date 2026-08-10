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
    char frame[NMEA_SENT_MAX + 3];
    size_t frame_len;
    esp_err_t err;

    frame_len = nmea_wire_frame_build(frame, sizeof(frame), msg);
    if (frame_len == 0) {
        ESP_LOGW(TAG, "Invalid or oversized NMEA TX frame");
        return;
    }

    err = rs485_driver_write(frame, frame_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RS485 TX failed len=%u: %s",
                 (unsigned)frame_len, esp_err_to_name(err));
        return;
    }

    rs485_simui_log_tx(frame);
}
