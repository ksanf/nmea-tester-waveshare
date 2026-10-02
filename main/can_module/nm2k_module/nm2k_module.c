/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NM2K monitor screen backed by internal NMEA2000 stack.
 */

#include "nm2k_module.h"
#include "nm2k_identity.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "protocol_handler.h"
#include "app/app_controller.h"

#include "config_nmea_tester.h"
#include "config/memory_config.h"
#include "driver/can_driver.h"
#include "freertos/FreeRTOS.h"
#include "nm2k_decoder.h"
#include "n2k.h"
#include "screen_can.h"
#include "screen_ui.h"
#include "ui/dialog_ui.h"
#include "ui/ui_theme.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "nm2k_module";

LV_FONT_DECLARE(lv_font_unscii_8);
LV_FONT_DECLARE(lv_font_montserrat_16);

#define NM2K_MARGIN           8
#define NM2K_HEADER_H        72
#define NM2K_STATUS_H        18
#define NM2K_CARD_GAP        12
#define NM2K_DEV_CARD_H     140
#define NM2K_TIMER_MS       500
#define NM2K_TRAFFIC_LINES   24
#define NM2K_LINE_LEN       120
#define NM2K_NODE_MAX        32
#define NM2K_NODE_PGNS        3

typedef nm2k_node_snapshot_t nm2k_node_t;

static lv_obj_t *scr = NULL;
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *dd_speed = NULL;
static lv_obj_t *page_devices = NULL;
static lv_obj_t *page_traffic = NULL;
static lv_obj_t *lbl_devices = NULL;
static lv_obj_t *lbl_traffic = NULL;
static lv_timer_t *ui_timer = NULL;
static atomic_bool stack_ready = ATOMIC_VAR_INIT(false);
static _Atomic(SemaphoreHandle_t) s_runtime_mutex;
static atomic_bool s_stack_operational;
static volatile bool s_devices_dirty = true;
static volatile bool s_traffic_dirty = true;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static nm2k_node_t *s_nodes = NULL;
static nm2k_node_t *s_nodes_snapshot = NULL;
static char (*s_traffic)[NM2K_LINE_LEN] = NULL;
static char (*s_traffic_snapshot)[NM2K_LINE_LEN] = NULL;
static char *s_devices_buf = NULL;
static char *s_traffic_buf = NULL;
static size_t s_traffic_count = 0;
static size_t s_traffic_head = 0;
static size_t s_traffic_snapshot_count = 0;
static size_t s_traffic_snapshot_head = 0;
static char s_last_traffic_base[NM2K_LINE_LEN];
static uint32_t s_last_rx_pgn = 0;
static uint8_t s_last_rx_src = 0;
static uint16_t s_last_rx_len = 0;
static atomic_uint s_can_speed = ATOMIC_VAR_INIT(250000);
static uint32_t s_rx_count;
static uint64_t s_traffic_next_seq;
static uint32_t s_traffic_dropped;
static uint32_t s_theme_rev = 0;

static inline bool screen_alive_(lv_obj_t *obj)
{
    return obj && lv_obj_is_valid(obj) && lv_obj_get_parent(obj) == NULL;
}

static bool runtime_lock_(TickType_t wait)
{
    if (!s_runtime_mutex) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (!created) return false;
        portENTER_CRITICAL(&s_lock);
        if (!s_runtime_mutex) { s_runtime_mutex = created; created = NULL; }
        portEXIT_CRITICAL(&s_lock);
        if (created) vSemaphoreDelete(created);
    }
    return xSemaphoreTake(s_runtime_mutex, wait) == pdTRUE;
}

static bool runtime_buffers_ready_(void)
{
    return s_nodes && s_traffic;
}

static bool buffers_ready_(void)
{
    return s_nodes && s_nodes_snapshot && s_traffic && s_traffic_snapshot &&
           s_devices_buf && s_traffic_buf;
}

static esp_err_t nm2k_stack_start_(void);
static void nm2k_delete_cb_(lv_event_t *e);


static uint32_t speed_from_sel_(uint16_t sel)
{
    switch (sel) {
        case 1: return 500000u;
        case 2: return 125000u;
        default: return 250000u;
    }
}

static uint16_t speed_sel_(uint32_t speed)
{
    switch (speed) {
        case 500000u: return 1u;
        case 125000u: return 2u;
        default: return 0u;
    }
}

static void runtime_clear_(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_nodes) memset(s_nodes, 0, sizeof(*s_nodes) * NM2K_NODE_MAX);
    if (s_traffic) memset(s_traffic, 0, sizeof(*s_traffic) * NM2K_TRAFFIC_LINES);
    s_traffic_count = 0;
    s_traffic_head = 0;
    s_last_traffic_base[0] = 0;
    s_last_rx_pgn = 0;
    s_last_rx_src = 0;
    s_last_rx_len = 0;
    s_rx_count = 0;
    s_devices_dirty = true;
    s_traffic_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}

static void buffers_free_(void)
{
    /* The RX task is joined and the runtime mutex excludes snapshots. */
    free(s_nodes); s_nodes = NULL;
    free(s_traffic); s_traffic = NULL;
}

static esp_err_t buffers_alloc_(void)
{
    if (runtime_buffers_ready_()) return ESP_OK;
    s_nodes = CALLOC_WHERE(NM2K_NODES_IN_PSRAM, NM2K_NODE_MAX, sizeof(*s_nodes));
    s_traffic = CALLOC_WHERE(NM2K_TRAFFIC_IN_PSRAM, NM2K_TRAFFIC_LINES, sizeof(*s_traffic));
    if (!runtime_buffers_ready_()) { buffers_free_(); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static void view_buffers_free_(void)
{
    free(s_nodes_snapshot); s_nodes_snapshot = NULL;
    free(s_traffic_snapshot); s_traffic_snapshot = NULL;
    free(s_devices_buf); s_devices_buf = NULL;
    free(s_traffic_buf); s_traffic_buf = NULL;
}

static bool view_buffers_alloc_(void)
{
    if (s_nodes_snapshot && s_traffic_snapshot && s_devices_buf && s_traffic_buf) return true;
    view_buffers_free_();
    s_nodes_snapshot = CALLOC_WHERE(NM2K_NODES_IN_PSRAM, NM2K_NODE_MAX, sizeof(*s_nodes_snapshot));
    s_traffic_snapshot = CALLOC_WHERE(NM2K_TRAFFIC_IN_PSRAM, NM2K_TRAFFIC_LINES, sizeof(*s_traffic_snapshot));
    s_devices_buf = CALLOC_WHERE(NM2K_BUFS_IN_PSRAM, 1, NM2K_DEVICES_BUF_SIZE);
    s_traffic_buf = CALLOC_WHERE(NM2K_BUFS_IN_PSRAM, 1, NM2K_TRAFFIC_BUF_SIZE);
    if (s_nodes_snapshot && s_traffic_snapshot && s_devices_buf && s_traffic_buf) return true;
    view_buffers_free_();
    return false;
}

static void traffic_push_(const char *line)
{
    size_t idx;

    if (!line || !s_traffic) return;
    idx = s_traffic_head % NM2K_TRAFFIC_LINES;
    snprintf(s_traffic[idx], sizeof(s_traffic[idx]), "%s", line);
    s_traffic_head = (s_traffic_head + 1u) % NM2K_TRAFFIC_LINES;
    if (s_traffic_count < NM2K_TRAFFIC_LINES) s_traffic_count++;
    else s_traffic_dropped++;
    s_traffic_next_seq++;
}

static nm2k_node_t *node_get_(uint8_t sa, bool create)
{
    int free_idx = -1;

    for (int i = 0; i < NM2K_NODE_MAX; ++i) {
        if (s_nodes[i].used && s_nodes[i].sa == sa) return &s_nodes[i];
        if (!s_nodes[i].used && free_idx < 0) free_idx = i;
    }
    if (!create || free_idx < 0) return NULL;

    memset(&s_nodes[free_idx], 0, sizeof(s_nodes[free_idx]));
    s_nodes[free_idx].used = true;
    s_nodes[free_idx].sa = sa;
    return &s_nodes[free_idx];
}

static void parse_product_strings_126996_(nm2k_node_t *n, const uint8_t *data, uint16_t len)
{
    const uint8_t *p = data;
    const uint8_t *e = data + len;
    uint8_t raw_model, raw_sw, raw_hw, raw_serial;
    uint8_t l_model, l_serial;

    if (!n || !data || len < 5) return;
    p += 5;

    if (p >= e) return;
    raw_model = *p++;
    if ((size_t)(e - p) < raw_model) return;
    l_model = raw_model;
    if (l_model >= sizeof(n->model)) l_model = sizeof(n->model) - 1;
    memcpy(n->model, p, l_model);
    n->model[l_model] = 0;
    p += raw_model;

    if (p >= e) return;
    raw_sw = *p++;
    if ((size_t)(e - p) < raw_sw) return;
    p += raw_sw;

    if (p >= e) return;
    raw_hw = *p++;
    if ((size_t)(e - p) < raw_hw) return;
    p += raw_hw;

    if (p >= e) return;
    raw_serial = *p++;
    if ((size_t)(e - p) < raw_serial) return;
    l_serial = raw_serial;
    if (l_serial >= sizeof(n->serial)) l_serial = sizeof(n->serial) - 1;
    memcpy(n->serial, p, l_serial);
    n->serial[l_serial] = 0;
}

static void node_update_(const n2k_msg_t *m)
{
    nm2k_node_t *n;

    if (!m || !s_nodes) return;
    n = node_get_(m->src, true);
    if (!n) return;

    n->last_seen_us = m->timestamp_us;
    n->last_pgn = m->pgn;
    n->last_len = m->len;
    n->rx_count++;

    if (n->seen_pgns[0] != m->pgn) {
        int pos = -1;
        for (int i = 1; i < NM2K_NODE_PGNS; ++i) {
            if (n->seen_pgns[i] == m->pgn) {
                pos = i;
                break;
            }
        }
        if (pos < 0) pos = NM2K_NODE_PGNS - 1;
        for (int i = pos; i > 0; --i) n->seen_pgns[i] = n->seen_pgns[i - 1];
        n->seen_pgns[0] = m->pgn;
    }

    if (m->pgn == 60928u && m->len >= 8) {
        uint64_t name = 0;
        for (int i = 0; i < 8; ++i) name |= ((uint64_t)m->data[i]) << (8 * i);
        n->name = name;
    } else if (m->pgn == 126996u) {
        parse_product_strings_126996_(n, m->data, m->len);
    }
}

static void traffic_format_(const n2k_msg_t *m, char *line, size_t cap)
{
    char hex[48];
    size_t off = 0;
    size_t preview;

    if (!m || !line || cap == 0) return;
    if (nm2k_decode_message(m, line, cap)) return;
    preview = (m->len < 8u) ? m->len : 8u;
    hex[0] = 0;
    for (size_t i = 0; i < preview && off + 4 < sizeof(hex); ++i) {
        off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%02X ", m->data[i]);
    }
    snprintf(line, cap, "%06" PRIu32 " %02X>%02X %4u %s%s",
             m->pgn,
             m->src,
             m->dst,
             (unsigned)m->len,
             hex,
             (m->len > preview) ? "..." : "");
}

static void refresh_devices_view_(void)
{
    size_t off = 0;
    uint64_t now = (uint64_t)esp_timer_get_time();

    if (!lbl_devices || !page_devices || !runtime_lock_(0)) return;
    if (!buffers_ready_()) { xSemaphoreGive(s_runtime_mutex); return; }

    portENTER_CRITICAL(&s_lock);
    memcpy(s_nodes_snapshot, s_nodes, sizeof(*s_nodes_snapshot) * NM2K_NODE_MAX);
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_runtime_mutex);

    off += (size_t)snprintf(s_devices_buf + off, 4096 - off,
                            "SA NAME32   LAST   P1     P2     AGE  RX MODEL\n");
    for (int i = 0; i < NM2K_NODE_MAX && off + 96 < 4096u; ++i) {
        uint32_t age_ms;
        const nm2k_node_t *n = &s_nodes_snapshot[i];

        if (!n->used) continue;
        age_ms = (uint32_t)((now - n->last_seen_us) / 1000ULL);
        off += (size_t)snprintf(s_devices_buf + off, 4096 - off,
                                "%02X %08" PRIX32 " %06" PRIu32 " %06" PRIu32 " %06" PRIu32 " %4" PRIu32 " %3" PRIu32 " %.12s\n",
                                n->sa,
                                (uint32_t)(n->name & 0xFFFFFFFFu),
                                n->last_pgn,
                                n->seen_pgns[1],
                                n->seen_pgns[2],
                                age_ms,
                                n->rx_count,
                                n->model[0] ? n->model : (n->serial[0] ? n->serial : "-"));
    }

    lv_label_set_text(lbl_devices, s_devices_buf);
}

static void refresh_traffic_view_(void)
{
    size_t off = 0;
    size_t first;

    if (!lbl_traffic || !page_traffic || !runtime_lock_(0)) return;
    if (!buffers_ready_()) { xSemaphoreGive(s_runtime_mutex); return; }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_traffic_snapshot, s_traffic, sizeof(*s_traffic_snapshot) * NM2K_TRAFFIC_LINES);
    s_traffic_snapshot_count = s_traffic_count;
    s_traffic_snapshot_head = s_traffic_head;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_runtime_mutex);

    if (s_traffic_snapshot_count == 0) {
        lv_label_set_text(lbl_traffic, "-- waiting for NMEA2000 traffic --");
        return;
    }

    first = (s_traffic_snapshot_head + NM2K_TRAFFIC_LINES - s_traffic_snapshot_count) % NM2K_TRAFFIC_LINES;
    for (size_t i = 0; i < s_traffic_snapshot_count && off + NM2K_LINE_LEN + 2 < 8192u; ++i) {
        size_t idx = (first + i) % NM2K_TRAFFIC_LINES;
        off += (size_t)snprintf(s_traffic_buf + off, 8192 - off, "%s\n", s_traffic_snapshot[idx]);
    }

    lv_label_set_text(lbl_traffic, s_traffic_buf);
}

static void refresh_status_(void)
{
    uint32_t rx_count;
    uint8_t sa = n2k_addr_get_sa();
    uint32_t last_pgn;
    uint8_t last_src;
    uint16_t last_len;
    int devices = 0;

    if (!lbl_status || !runtime_lock_(0)) return;
    if (!stack_ready) {
        xSemaphoreGive(s_runtime_mutex);
        lv_label_set_text(lbl_status, "NM2K stopped");
        return;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_nodes) {
        for (int i = 0; i < NM2K_NODE_MAX; ++i) if (s_nodes[i].used) devices++;
    }
    last_pgn = s_last_rx_pgn;
    last_src = s_last_rx_src;
    last_len = s_last_rx_len;
    rx_count = s_rx_count;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_runtime_mutex);

    if (last_pgn != 0u) {
        lv_label_set_text_fmt(lbl_status,
                              "SA=%02X DEV=%d RX=%" PRIu32 " LAST=%06" PRIu32 " SRC=%02X LEN=%u",
                              sa,
                              devices,
                              rx_count,
                              last_pgn,
                              last_src,
                              (unsigned)last_len);
    } else {
        lv_label_set_text_fmt(lbl_status,
                              "SA=%02X DEV=%d RX=%" PRIu32,
                              sa,
                              devices,
                              rx_count);
    }
}

static void nm2k_on_rx_(const n2k_msg_t *msg, void *user)
{
    char line[NM2K_LINE_LEN];
    (void)user;

    if (!msg || !runtime_buffers_ready_()) return;
    traffic_format_(msg, line, sizeof(line));

    portENTER_CRITICAL(&s_lock);
    node_update_(msg);
    s_last_rx_pgn = msg->pgn;
    s_last_rx_src = msg->src;
    s_last_rx_len = msg->len;
    s_rx_count++;
    if (strcmp(line, s_last_traffic_base) != 0) {
        snprintf(s_last_traffic_base, sizeof(s_last_traffic_base), "%s", line);
        traffic_push_(line);
        s_traffic_dirty = true;
    }
    s_devices_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}

static void nm2k_timer_cb_(lv_timer_t *t)
{
    bool devices_dirty;
    bool traffic_dirty;
    static uint8_t device_ticks = 0;
    (void)t;

    portENTER_CRITICAL(&s_lock);
    devices_dirty = s_devices_dirty;
    traffic_dirty = s_traffic_dirty;
    s_devices_dirty = false;
    s_traffic_dirty = false;
    portEXIT_CRITICAL(&s_lock);

    if (traffic_dirty) {
        refresh_traffic_view_();
    }
    device_ticks++;
    if (devices_dirty || device_ticks >= 4u) {
        refresh_devices_view_();
        device_ticks = 0;
    }
    if (dd_speed) lv_dropdown_set_selected(dd_speed, speed_sel_(s_can_speed));
    refresh_status_();
}

/* No LVGL operation below: the controller may call these with the display asleep. */
static bool runtime_stop_locked_(void)
{
    if (stack_ready) {
        s_stack_operational = false;
        esp_err_t err = n2k_transport_deinit();
        if (err != ESP_OK) { ESP_LOGE(TAG, "N2K stop failed: %s", esp_err_to_name(err)); return false; }
        err = can_driver_deinit();
        if (err != ESP_OK) { ESP_LOGE(TAG, "CAN stop failed: %s", esp_err_to_name(err)); return false; }
        stack_ready = false;
    }
    buffers_free_();
    return true;
}

esp_err_t nm2k_runtime_start(uint32_t speed)
{
    if (speed == 0) speed = s_can_speed;
    if (speed != 125000u && speed != 250000u && speed != 500000u) return ESP_ERR_INVALID_ARG;
    if (!runtime_lock_(portMAX_DELAY)) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    if (protocol_handler_is_running()) { err = ESP_ERR_INVALID_STATE; goto done; }
    if (stack_ready && s_stack_operational && speed == s_can_speed) goto done;
    if (!runtime_stop_locked_()) { err = ESP_ERR_TIMEOUT; goto done; }
    s_can_speed = speed;
    err = buffers_alloc_();
    if (err != ESP_OK) goto done;
    runtime_clear_();
    err = nm2k_stack_start_();
    if (err != ESP_OK && !stack_ready) buffers_free_();
done:
    xSemaphoreGive(s_runtime_mutex);
    return err;
}

bool nm2k_runtime_stop(void)
{
    if (!runtime_lock_(portMAX_DELAY)) return false;
    bool ok = runtime_stop_locked_();
    xSemaphoreGive(s_runtime_mutex);
    return ok;
}

bool nm2k_runtime_running(void) { return stack_ready; }
uint32_t nm2k_runtime_speed(void) { return s_can_speed; }

bool nm2k_runtime_snapshot(nm2k_snapshot_t *out)
{
    if (!out || !runtime_lock_(portMAX_DELAY)) return false;
    memset(out, 0, sizeof(*out));
    out->running = stack_ready;
    out->speed = s_can_speed;
    out->local_sa = stack_ready ? n2k_addr_get_sa() : 0xFFu;
    portENTER_CRITICAL(&s_lock);
    out->rx_count = s_rx_count;
    out->last_pgn = s_last_rx_pgn;
    out->last_src = s_last_rx_src;
    out->last_len = s_last_rx_len;
    out->next_seq = s_traffic_next_seq;
    out->traffic_dropped = s_traffic_dropped;
    if (s_nodes) {
        for (size_t i = 0; i < NM2K_NODE_MAX; ++i) {
            if (s_nodes[i].used) out->nodes[out->node_count++] = s_nodes[i];
        }
    }
    if (s_traffic) {
        out->traffic_count = s_traffic_count;
        size_t first = (s_traffic_head + NM2K_TRAFFIC_LINES - s_traffic_count) % NM2K_TRAFFIC_LINES;
        for (size_t i = 0; i < s_traffic_count; ++i) {
            memcpy(out->traffic[i], s_traffic[(first + i) % NM2K_TRAFFIC_LINES], NM2K_LINE_LEN);
        }
    }
    out->first_seq = out->next_seq - out->traffic_count;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_runtime_mutex);
    return true;
}

static void pause_view_animations_(lv_obj_t *obj)
{
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_anim_del(obj, NULL);
    uint32_t count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < count; ++i) pause_view_animations_(lv_obj_get_child(obj, i));
}

void nm2k_view_suspend(void)
{
    if (ui_timer) { lv_timer_del(ui_timer); ui_timer = NULL; }
    if (dd_speed && lv_obj_is_valid(dd_speed)) lv_dropdown_close(dd_speed);
    pause_view_animations_(scr);
}

static void nm2k_delete_cb_(lv_event_t *e)
{
    (void)e;
    nm2k_view_suspend();
    scr = NULL;
    lbl_status = NULL;
    dd_speed = NULL;
    page_devices = NULL;
    page_traffic = NULL;
    lbl_devices = NULL;
    lbl_traffic = NULL;
    view_buffers_free_();
}

void nm2k_view_destroy(void)
{
    nm2k_view_suspend();
    if (screen_alive_(scr)) lv_obj_del(scr);
    else view_buffers_free_();
}

static void nm2k_home_cb_(lv_event_t *e)
{
    (void)e;

    (void)app_controller_local_stop();
}

static void nm2k_speed_cb_(lv_event_t *e)
{
    uint32_t speed;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || app_controller_is_web()) return;
    speed = speed_from_sel_(lv_dropdown_get_selected(lv_event_get_target(e)));
    if (speed == s_can_speed) return;
    if (!app_controller_local_n2k_speed(speed)) {
        lv_dropdown_set_selected(lv_event_get_target(e), speed_sel_(s_can_speed));
    }
}

static void text_area_style_(lv_obj_t **page_out, lv_obj_t **label_out,
                             lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                             const char *title)
{
    const ui_theme_palette_t *th = ui_theme_get();
    lv_obj_t *lbl;
    lv_obj_t *page;
    lv_obj_t *label;

    lbl = lv_label_create(scr);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->title), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(lbl, x, y);

    page = lv_obj_create(scr);
    lv_obj_set_pos(page, x, y + 26);
    lv_obj_set_size(page, w, h - 26);
    lv_obj_set_scroll_dir(page, LV_DIR_HOR | LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, lv_color_hex(th->card), 0);
    lv_obj_set_style_border_color(page, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_width(page, 1, 0);
    lv_obj_set_style_radius(page, 8, 0);

    label = lv_label_create(page);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, w - 16);
    lv_obj_set_style_text_color(label, lv_color_hex(th->text), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_text(label, "");

    *page_out = page;
    *label_out = label;
}

static esp_err_t nm2k_stack_start_(void)
{
    nm2k_identity_t identity;
    esp_err_t err = nm2k_identity_read(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "N2K device identity unavailable: %s", esp_err_to_name(err));
        return err;
    }
    const can_config_t cc = {
        .nom_speed = s_can_speed,
        .listen_only = false,
    };
    const n2k_transport_cfg_t tc = {
        .local_sa = 0x25,
        .default_priority = 6,
        .bam_dt_gap_ms = 50,
        .fp_slots = 8,
        .tp_slots = 4,
        .rx_queue_len = 0,
    };
    const n2k_addr_cfg_t ac = {
        .name = identity.name,
        .preferred_sa = 0x25,
        .priority = 6,
    };
    n2k_product_info_t prod = {
        .manufacturer_code = 2046,
        .unique_id = identity.identity_number,
        .model_id = "NMEA TESTER NM2K",
        .sw_version = "1.0",
        .hw_version = "ESP32",
    };
    _Static_assert(sizeof(prod.serial_code) >= sizeof(identity.serial),
                   "N2K product serial must contain the complete MAC identity");
    memcpy(prod.serial_code, identity.serial, sizeof(identity.serial));

    err = can_driver_init(&cc);
    if (err != ESP_OK) return err;
    stack_ready = true; /* Ownership persists if a later cleanup cannot join RX. */
    s_stack_operational = false;

    err = n2k_stack_init_all(&tc, &ac, &prod, "ESP32 monitor", "NMEA Tester", true, 5000);
    if (err != ESP_OK) {
        (void)runtime_stop_locked_();
        return err;
    }
    err = n2k_transport_subscribe(N2K_PGN_ANY, nm2k_on_rx_, NULL);
    if (err != ESP_OK) {
        (void)runtime_stop_locked_();
        return err;
    }
    s_stack_operational = true;
    return ESP_OK;
}

static lv_obj_t *build_screen_(void)
{
    lv_coord_t body_y;
    lv_coord_t card_w;
    lv_coord_t traffic_y;
    lv_coord_t traffic_h;

    scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, nm2k_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr);

    dialog_ui_create_button(scr, LCD_WIDTH - 66 - 8, 3, 66, 66, LV_SYMBOL_HOME,
                            ui_theme_can_home_hex(), nm2k_home_cb_, NULL, &lv_font_montserrat_28);

    {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "NMEA2000");
        ui_theme_apply_title(lbl, &lv_font_montserrat_20);
        lv_obj_set_pos(lbl, 8, 12);
    }

    dd_speed = lv_dropdown_create(scr);
    lv_dropdown_set_options(dd_speed, "250k\n500k\n125k");
    lv_dropdown_set_selected(dd_speed, speed_sel_(s_can_speed));
    ui_theme_apply_dropdown(dd_speed, &lv_font_montserrat_20);
    lv_obj_set_size(dd_speed, 150, 44);
    lv_obj_set_pos(dd_speed, LCD_WIDTH - 66 - 8 - 12 - 150, 8);
    lv_obj_add_event_cb(dd_speed, nm2k_speed_cb_, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_status = lv_label_create(scr);
    lv_obj_set_pos(lbl_status, NM2K_MARGIN, NM2K_HEADER_H - 14);
    ui_theme_apply_muted(lbl_status, &lv_font_montserrat_14);

    body_y = NM2K_HEADER_H;
    card_w = LCD_WIDTH - 2 * NM2K_MARGIN;

    text_area_style_(&page_devices, &lbl_devices,
                     NM2K_MARGIN, body_y, card_w, NM2K_DEV_CARD_H, "DEVICES");

    traffic_y = body_y + NM2K_DEV_CARD_H + NM2K_CARD_GAP;
    traffic_h = LCD_HEIGHT - traffic_y - NM2K_MARGIN;

    text_area_style_(&page_traffic, &lbl_traffic,
                     NM2K_MARGIN, traffic_y, card_w, traffic_h, "TRAFFIC");
    lv_label_set_text(lbl_devices, "-- waiting for address claims / product info --");
    lv_label_set_text(lbl_traffic, "-- waiting for NMEA2000 traffic --");

    return scr;
}

lv_obj_t *nm2k_view_show(lv_obj_t *parent)
{
    (void)parent;
    if (screen_alive_(scr) && s_theme_rev != ui_theme_get_revision()) nm2k_view_destroy();
    if (!screen_alive_(scr)) {
        build_screen_();
        s_theme_rev = ui_theme_get_revision();
    }
    if (!view_buffers_alloc_()) {
        lv_label_set_text(lbl_status, "NM2K view allocation failed");
    } else {
        lv_dropdown_set_selected(dd_speed, speed_sel_(s_can_speed));
        refresh_devices_view_();
        refresh_traffic_view_();
        refresh_status_();
        if (!ui_timer) ui_timer = lv_timer_create(nm2k_timer_cb_, NM2K_TIMER_MS, NULL);
    }
    lv_scr_load(scr);
    return scr;
}

lv_obj_t *nm2k_create(lv_obj_t *parent)
{
    return nm2k_view_show(parent);
}

void nm2k_module_start(lv_obj_t *parent)
{
    (void)parent;
    (void)app_controller_local_start(APP_MODE_N2K);
}
