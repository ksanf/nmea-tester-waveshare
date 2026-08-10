/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "system/telnet_router.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define ROUTE_DRAIN_POLL_MS 10U

typedef struct {
    telnet_server_rx_cb_t cb;
    void *user;
    atomic_uint inflight;
} telnet_route_slot_t;

static bool s_router_inited = false;
static telnet_route_t s_active_route = TELNET_ROUTE_NONE;
static telnet_route_slot_t s_slots[TELNET_ROUTE__COUNT];
static portMUX_TYPE s_router_lock = portMUX_INITIALIZER_UNLOCKED;

static void telnet_router_wait_idle_(telnet_route_t route)
{
    while (atomic_load_explicit(&s_slots[route].inflight,
                                memory_order_acquire) != 0u) {
        vTaskDelay(pdMS_TO_TICKS(ROUTE_DRAIN_POLL_MS));
    }
}

static void telnet_router_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    telnet_server_rx_cb_t cb = NULL;
    void *cb_user = NULL;
    telnet_route_t route;
    (void)user;

    portENTER_CRITICAL(&s_router_lock);
    route = s_active_route;
    if (route > TELNET_ROUTE_NONE && route < TELNET_ROUTE__COUNT) {
        cb = s_slots[route].cb;
        cb_user = s_slots[route].user;
        if (cb) {
            atomic_fetch_add_explicit(&s_slots[route].inflight, 1u,
                                      memory_order_acq_rel);
        }
    }
    portEXIT_CRITICAL(&s_router_lock);

    if (!cb) return;
    cb(data, len, cb_user);
    atomic_fetch_sub_explicit(&s_slots[route].inflight, 1u,
                              memory_order_release);
}

void telnet_router_init(void)
{
    if (s_router_inited) return;

    for (int i = 0; i < TELNET_ROUTE__COUNT; ++i) {
        s_slots[i].cb = NULL;
        s_slots[i].user = NULL;
        atomic_store_explicit(&s_slots[i].inflight, 0u, memory_order_relaxed);
    }
    s_active_route = TELNET_ROUTE_NONE;
    telnet_server_set_rx_cb(telnet_router_rx_cb_, NULL);
    s_router_inited = true;
}

void telnet_router_register(telnet_route_t route, telnet_server_rx_cb_t cb, void *user)
{
    if (!s_router_inited) telnet_router_init();
    if (route <= TELNET_ROUTE_NONE || route >= TELNET_ROUTE__COUNT) return;

    telnet_router_unregister(route);
    portENTER_CRITICAL(&s_router_lock);
    s_slots[route].cb = cb;
    s_slots[route].user = user;
    portEXIT_CRITICAL(&s_router_lock);
}

void telnet_router_unregister(telnet_route_t route)
{
    if (route <= TELNET_ROUTE_NONE || route >= TELNET_ROUTE__COUNT) return;

    portENTER_CRITICAL(&s_router_lock);
    if (s_active_route == route) {
        s_active_route = TELNET_ROUTE_NONE;
    }
    s_slots[route].cb = NULL;
    s_slots[route].user = NULL;
    portEXIT_CRITICAL(&s_router_lock);

    telnet_router_wait_idle_(route);
}

void telnet_router_set_active(telnet_route_t route)
{
    portENTER_CRITICAL(&s_router_lock);
    if (route < TELNET_ROUTE_NONE || route >= TELNET_ROUTE__COUNT) {
        s_active_route = TELNET_ROUTE_NONE;
        portEXIT_CRITICAL(&s_router_lock);
        return;
    }
    if (route != TELNET_ROUTE_NONE && !s_slots[route].cb) {
        s_active_route = TELNET_ROUTE_NONE;
        portEXIT_CRITICAL(&s_router_lock);
        return;
    }
    s_active_route = route;
    portEXIT_CRITICAL(&s_router_lock);
}

telnet_route_t telnet_router_get_active(void)
{
    telnet_route_t route;

    portENTER_CRITICAL(&s_router_lock);
    route = s_active_route;
    portEXIT_CRITICAL(&s_router_lock);
    return route;
}
