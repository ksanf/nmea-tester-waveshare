/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef TELNET_SERVER_H
#define TELNET_SERVER_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*telnet_server_rx_cb_t)(const uint8_t *data, size_t len, void *user);

esp_err_t telnet_server_start(void);
esp_err_t telnet_server_stop(void);
bool telnet_server_is_running(void);
bool telnet_server_client_connected(void);
bool telnet_server_get_client_ip(char *buf, size_t buf_sz);
esp_err_t telnet_server_send(const uint8_t *data, size_t len);
/* Replaces the RX callback after the previous invocation has drained. */
void telnet_server_set_rx_cb(telnet_server_rx_cb_t cb, void *user);
void telnet_server_flush_tx(void);

#ifdef __cplusplus
}
#endif

#endif
