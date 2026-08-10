/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "n2k_device_list.h"
#include "n2k_transport.h"
#include "esp_timer.h"
#include <string.h>

#define MAX_DEV 32
static struct {
    n2k_device_t dev[MAX_DEV];
    uint32_t offline_ms;
    bool inited;
} G;

static inline uint64_t now_us(){ return (uint64_t)esp_timer_get_time(); }

static int find_sa(uint8_t sa){
    for (int i=0;i<MAX_DEV;i++) if (G.dev[i].used && G.dev[i].sa==sa) return i;
    return -1;
}
static int find_free(){
    for (int i=0;i<MAX_DEV;i++) if (!G.dev[i].used) return i;
    return -1;
}

static void touch_sa(uint8_t sa) {
    int i=find_sa(sa);
    if (i<0) { i=find_free(); if (i<0) return; G.dev[i].used=true; G.dev[i].sa=sa; G.dev[i].name=0; }
    G.dev[i].last_seen_us = now_us();
}

static void on_any(const n2k_msg_t *m, void *u) { (void)u; touch_sa(m->src); }

static void on_60928(const n2k_msg_t *m, void *u) {
    (void)u; if (m->len<8) return;
    uint64_t name=0; for (int i=0;i<8;i++) name |= ((uint64_t)m->data[i]) << (8*i);
    int i=find_sa(m->src); if (i<0) { i=find_free(); if (i<0) return; G.dev[i].used=true; G.dev[i].sa=m->src; }
    G.dev[i].name = name; G.dev[i].last_seen_us = now_us();
}

esp_err_t n2k_devlist_init(uint32_t offline_ms)
{
    esp_err_t err;

    if (!G.inited) {
        memset(&G,0,sizeof(G));
        G.inited=true;
    }
    G.offline_ms = offline_ms ? offline_ms : 5000;

    err = n2k_transport_subscribe(60928, on_60928, NULL);
    if (err != ESP_OK) return err;

    return n2k_transport_subscribe(N2K_PGN_ANY, on_any, NULL);
}

int n2k_devlist_get(n2k_device_t *out, int max)
{
    if (!G.inited || !out || max<=0) return 0;
    int n=0; uint64_t t=now_us();
    for (int i=0;i<MAX_DEV && n<max;i++) {
        if (!G.dev[i].used) continue;
        uint64_t dt_ms = (t - G.dev[i].last_seen_us)/1000ULL;
        if (dt_ms > G.offline_ms) continue;
        out[n++] = G.dev[i];
    }
    return n;
}
