/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef UDP_NMEA_SERVER_H
#define UDP_NMEA_SERVER_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*udp_nmea_server_rx_cb_t)(const uint8_t *data, size_t len, void *user);

esp_err_t udp_nmea_server_start(void);
esp_err_t udp_nmea_server_stop(void);
bool udp_nmea_server_is_running(void);
/* Replaces the RX callback after the previous invocation has drained. */
void udp_nmea_server_set_rx_cb(udp_nmea_server_rx_cb_t cb, void *user);
bool udp_nmea_server_get_last_peer(char *buf, size_t buf_sz, uint16_t *port);
uint32_t udp_nmea_server_get_last_peer_age_ms(void);
esp_err_t udp_nmea_server_send_to_last_peer(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
