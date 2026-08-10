/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef TELNET_ROUTER_H
#define TELNET_ROUTER_H

#include "system/telnet_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELNET_ROUTE_NONE = 0,
    TELNET_ROUTE_BRIDGE,
    TELNET_ROUTE_TX485,
    TELNET_ROUTE_RS485_BRIDGE,
    TELNET_ROUTE__COUNT
} telnet_route_t;

void telnet_router_init(void);
void telnet_router_register(telnet_route_t route, telnet_server_rx_cb_t cb, void *user);
/* Disables the route and waits for any callback already in progress. */
void telnet_router_unregister(telnet_route_t route);
void telnet_router_set_active(telnet_route_t route);
telnet_route_t telnet_router_get_active(void);

#ifdef __cplusplus
}
#endif

#endif
