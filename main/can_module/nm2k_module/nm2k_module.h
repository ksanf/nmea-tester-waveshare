/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NM2K monitor screen backed by internal NMEA2000 stack.
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <lvgl.h>

#define NM2K_SNAPSHOT_NODES 32
#define NM2K_SNAPSHOT_TRAFFIC 24
#define NM2K_SNAPSHOT_LINE_LEN 120
#define NM2K_SNAPSHOT_NODE_PGNS 3

typedef struct {
    bool used;
    uint8_t sa;
    uint64_t name;
    uint64_t last_seen_us;
    uint32_t last_pgn;
    uint16_t last_len;
    uint32_t rx_count;
    uint32_t seen_pgns[NM2K_SNAPSHOT_NODE_PGNS];
    char model[33];
    char serial[21];
} nm2k_node_snapshot_t;

typedef struct {
    bool running;
    uint32_t speed;
    uint8_t local_sa;
    uint32_t rx_count;
    uint32_t last_pgn;
    uint8_t last_src;
    uint16_t last_len;
    size_t node_count;
    nm2k_node_snapshot_t nodes[NM2K_SNAPSHOT_NODES];
    size_t traffic_count;
    uint64_t first_seq;
    uint64_t next_seq;
    uint32_t traffic_dropped;
    char traffic[NM2K_SNAPSHOT_TRAFFIC][NM2K_SNAPSHOT_LINE_LEN];
} nm2k_snapshot_t;

/* Runtime calls do not access LVGL. Serialize lifecycle changes through the app
 * controller; snapshots may be read concurrently. speed=0 preserves selection. */
esp_err_t nm2k_runtime_start(uint32_t speed);
bool nm2k_runtime_stop(void);
bool nm2k_runtime_running(void);
uint32_t nm2k_runtime_speed(void);
bool nm2k_runtime_snapshot(nm2k_snapshot_t *out);

/* View calls require the LVGL lock and never start or stop the runtime. */
void nm2k_view_suspend(void);
lv_obj_t *nm2k_view_show(lv_obj_t *parent);
void nm2k_view_destroy(void);
lv_obj_t *nm2k_create(lv_obj_t *parent);
void nm2k_module_start(lv_obj_t *parent);
