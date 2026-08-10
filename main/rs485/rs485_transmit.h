/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   RS-485 transmit module interface for NMEA Tester.
 */

#pragma once

/**
 * @brief Send an NMEA sentence to the RS-485 port.
 * @param msg NMEA sentence with a trailing \r\n.
 */
void rs485_transmit_send(const char *msg);
