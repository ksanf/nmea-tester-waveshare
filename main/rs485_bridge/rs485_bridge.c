/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "rs485_bridge/rs485_bridge.h"
#include "rs485/rs485_runtime_log.h"
#include "app/app_controller.h"

#include "config/config_nmea_tester.h"
#include "rs485/rs485_driver.h"
#include "system/telnet_router.h"
#include "system/telnet_server.h"
#include "system/udp_nmea_server.h"
#include "system/wifi_ap.h"
#include "ui/dialog_ui.h"
#include "ui/ui_colors.h"
#include "ui/screens/screen_ui.h"
#include "ui/ui_baud_selector.h"
#include "ui/ui_theme.h"

#include <esp_err.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_RS485_BRIDGE
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#define BRIDGE_RX_BUF_SZ 256
#define BRIDGE_RX_WAIT_MS 20
#define BRIDGE_TASK_STOP_WAIT_MS 1000U
#define BRIDGE_NET_DRAIN_WAIT_MS 7000U
#define BRIDGE_STOP_POLL_MS 10U

static const char *TAG = "rs485_bridge";

static lv_obj_t *scr_bridge = NULL;
static lv_obj_t *dd_rs_baud = NULL;
static lv_obj_t *lbl_net = NULL;
static lv_obj_t *lbl_state = NULL;
static lv_obj_t *lbl_hint = NULL;
static lv_timer_t *tmr_status = NULL;
static _Atomic(TaskHandle_t) bridge_task = NULL;
static atomic_bool bridge_stop = ATOMIC_VAR_INIT(false);
static atomic_bool rs485_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_net_rx_enabled = ATOMIC_VAR_INIT(false);
static atomic_uint s_net_rx_inflight = ATOMIC_VAR_INIT(0);
static uint32_t s_theme_rev = 0;
static rs485_runtime_log_t s_runtime_log = RS485_RUNTIME_LOG_INIT;

static _Atomic uint32_t s_bytes_net_to_rs = 0;
static _Atomic uint32_t s_bytes_rs_to_net = 0;
static _Atomic uint32_t s_pkts_net_to_rs = 0;
static _Atomic uint32_t s_pkts_rs_to_net = 0;

static bool rs485_is_ready_(void)
{
    return atomic_load_explicit(&rs485_ready, memory_order_acquire);
}

static bool bridge_net_rx_begin_(void)
{
    if (!atomic_load_explicit(&s_net_rx_enabled, memory_order_acquire)) {
        return false;
    }

    atomic_fetch_add_explicit(&s_net_rx_inflight, 1u, memory_order_acq_rel);
    if (!atomic_load_explicit(&s_net_rx_enabled, memory_order_acquire) ||
        !rs485_is_ready_()) {
        atomic_fetch_sub_explicit(&s_net_rx_inflight, 1u, memory_order_release);
        return false;
    }
    return true;
}

static void bridge_net_rx_end_(void)
{
    atomic_fetch_sub_explicit(&s_net_rx_inflight, 1u, memory_order_release);
}

static bool bridge_wait_net_idle_(void)
{
    for (uint32_t waited_ms = 0;
         waited_ms < BRIDGE_NET_DRAIN_WAIT_MS;
         waited_ms += BRIDGE_STOP_POLL_MS) {
        if (atomic_load_explicit(&s_net_rx_inflight, memory_order_acquire) == 0u) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_STOP_POLL_MS));
    }
    return atomic_load_explicit(&s_net_rx_inflight, memory_order_acquire) == 0u;
}

static bool bridge_wait_task_stopped_(void)
{
    for (uint32_t waited_ms = 0;
         waited_ms < BRIDGE_TASK_STOP_WAIT_MS;
         waited_ms += BRIDGE_STOP_POLL_MS) {
        if (!atomic_load_explicit(&bridge_task, memory_order_acquire)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_STOP_POLL_MS));
    }
    return atomic_load_explicit(&bridge_task, memory_order_acquire) == NULL;
}

static inline bool screen_alive(lv_obj_t *scr)
{
    return scr && lv_obj_is_valid(scr) && lv_obj_get_parent(scr) == NULL;
}

static lv_obj_t *create_status_card_(lv_obj_t *parent, const char *title,
                                     lv_coord_t x, lv_coord_t y,
                                     lv_coord_t w, lv_coord_t h)
{
    const ui_theme_palette_t *th = ui_theme_get();
    const bool color_mode = (ui_theme_get_id() == UI_THEME_COLOR);
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (color_mode) {
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_RS485_BRIDGE_CARD_TOP_HEX), 0);
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(UI_RS485_BRIDGE_CARD_BOTTOM_HEX), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_RS485_BRIDGE_CARD_BORDER_HEX), 0);
    } else {
        lv_obj_set_style_bg_color(card, lv_color_hex(th->card), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(th->border), 0);
    }
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    if (color_mode) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_RS485_BRIDGE_CARD_CAPTION_HEX), 0);
    } else {
        ui_theme_apply_title(lbl, &lv_font_montserrat_16);
    }
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

static lv_obj_t *create_hint_card_(lv_obj_t *parent, const char *title,
                                   const char *text,
                                   lv_coord_t x, lv_coord_t y,
                                   lv_coord_t w, lv_coord_t h)
{
    const bool color_mode = (ui_theme_get_id() == UI_THEME_COLOR);
    lv_obj_t *card = create_status_card_(parent, title, x, y, w, h);
    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_set_width(lbl, w - 20);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    if (color_mode) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_RS485_BRIDGE_CARD_TEXT_HEX), 0);
    } else {
        ui_theme_apply_text(lbl, &lv_font_montserrat_22);
    }
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 12);
    return card;
}

static void rs485_bridge_status_update_(void)
{
    char ap_ip[16];
    char telnet_ip[16];
    char udp_ip[16];
    uint16_t udp_port = 0;
    bool wifi_on;
    bool telnet_ok;
    bool udp_ok;
    uint32_t udp_age_ms;
    char net_text[128];
    char state_text[128];

    if (!scr_bridge || !screen_alive(scr_bridge)) return;
    ui_baud_selector_sync(dd_rs_baud);

    wifi_on = wifi_ap_is_enabled();
    if (!wifi_on) {
        if (lbl_hint && lv_obj_is_valid(lbl_hint)) {
            lv_obj_clear_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_hint, "Enable Wi-Fi on the main screen");
        }
        if (dd_rs_baud && lv_obj_is_valid(dd_rs_baud)) {
            lv_obj_add_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
        }
        if (lbl_net && lv_obj_is_valid(lbl_net)) lv_obj_add_flag(lbl_net, LV_OBJ_FLAG_HIDDEN);
        if (lbl_state && lv_obj_is_valid(lbl_state)) lv_obj_add_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (lbl_hint && lv_obj_is_valid(lbl_hint)) lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
    if (dd_rs_baud && lv_obj_is_valid(dd_rs_baud)) lv_obj_clear_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
    if (lbl_net && lv_obj_is_valid(lbl_net)) lv_obj_clear_flag(lbl_net, LV_OBJ_FLAG_HIDDEN);
    if (lbl_state && lv_obj_is_valid(lbl_state)) lv_obj_clear_flag(lbl_state, LV_OBJ_FLAG_HIDDEN);

    if (!wifi_ap_get_ip(ap_ip, sizeof(ap_ip))) {
        strlcpy(ap_ip, "?.?.?.?", sizeof(ap_ip));
    }
    telnet_ok = telnet_server_get_client_ip(telnet_ip, sizeof(telnet_ip));
    udp_ok = udp_nmea_server_get_last_peer(udp_ip, sizeof(udp_ip), &udp_port);
    udp_age_ms = udp_nmea_server_get_last_peer_age_ms();
    if (!udp_ok || udp_age_ms > 15000u) udp_ok = false;

    snprintf(net_text, sizeof(net_text),
             "TELNET %s:%u %s%s\nUDP %s:%u %s%s%s",
             ap_ip, (unsigned)WIFI_TELNET_PORT,
             telnet_ok ? "<- " : "(idle)",
             telnet_ok ? telnet_ip : "",
             ap_ip, (unsigned)WIFI_UDP_NMEA_PORT,
             udp_ok ? "<- " : "(idle)",
             udp_ok ? udp_ip : "",
             udp_ok ? ":" : "");
    if (udp_ok) {
        size_t off = strlen(net_text);
        snprintf(net_text + off, sizeof(net_text) - off, "%u", (unsigned)udp_port);
    }

    snprintf(state_text, sizeof(state_text),
             "RS485 %lu\nNET->RS  %" PRIu32 " B / %" PRIu32 " pkt\nRS->NET  %" PRIu32 " B / %" PRIu32 " pkt",
             (unsigned long)rs485_get_baudrate(),
             (uint32_t)atomic_load(&s_bytes_net_to_rs),
             (uint32_t)atomic_load(&s_pkts_net_to_rs),
             (uint32_t)atomic_load(&s_bytes_rs_to_net),
             (uint32_t)atomic_load(&s_pkts_rs_to_net));

    lv_label_set_text(lbl_net, net_text);
    lv_label_set_text(lbl_state, state_text);
}

static void rs485_bridge_status_timer_cb_(lv_timer_t *tmr)
{
    (void)tmr;
    rs485_bridge_status_update_();
}

static void rs485_bridge_forward_to_net_(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;

    if (telnet_server_client_connected()) {
        (void)telnet_server_send(data, len);
    }
    (void)udp_nmea_server_send_to_last_peer(data, len);
    rs485_runtime_log_append(&s_runtime_log, RS485_EVENT_BRIDGE_RX, data, len);
    atomic_fetch_add(&s_bytes_rs_to_net, (uint32_t)len);
    atomic_fetch_add(&s_pkts_rs_to_net, 1u);
}

static esp_err_t rs485_bridge_send_to_rs_(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !rs485_is_ready_()) return ESP_ERR_INVALID_STATE;
    esp_err_t result = rs485_driver_write((const char *)data, len);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "RS485 write failed: len=%u", (unsigned)len);
        return result;
    }
    atomic_fetch_add(&s_bytes_net_to_rs, (uint32_t)len);
    atomic_fetch_add(&s_pkts_net_to_rs, 1u);
    rs485_runtime_log_append(&s_runtime_log, RS485_EVENT_BRIDGE_TX, data, len);
    return ESP_OK;
}

static void rs485_bridge_telnet_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!data || len == 0 || !bridge_net_rx_begin_()) return;
    rs485_bridge_send_to_rs_(data, len);
    bridge_net_rx_end_();
}

static void rs485_bridge_udp_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!data || len == 0 || !bridge_net_rx_begin_()) return;
    rs485_bridge_send_to_rs_(data, len);
    bridge_net_rx_end_();
}

static void rs485_bridge_task_(void *arg)
{
    (void)arg;

    while (!atomic_load_explicit(&bridge_stop, memory_order_acquire)) {
        uint8_t buf[BRIDGE_RX_BUF_SZ];
        size_t len = sizeof(buf);

        if (rs485_driver_read_timeout(buf, &len, BRIDGE_RX_WAIT_MS) == ESP_OK && len > 0) {
            rs485_bridge_forward_to_net_(buf, len);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    atomic_store_explicit(&bridge_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

bool rs485_bridge_runtime_stop(void)
{
    if (!rs485_is_ready_() && !atomic_load(&bridge_task)) return true;
    atomic_store_explicit(&s_net_rx_enabled, false, memory_order_release);
    telnet_router_unregister(TELNET_ROUTE_RS485_BRIDGE);
    udp_nmea_server_set_rx_cb(NULL, NULL);
    if (!bridge_wait_net_idle_()) return false;
    atomic_store_explicit(&bridge_stop, true, memory_order_release);
    if (!bridge_wait_task_stopped_()) return false;
    if (rs485_is_ready_()) {
        if (rs485_release(RS485_OWNER_NETWORK_BRIDGE) != ESP_OK) return false;
        atomic_store_explicit(&rs485_ready, false, memory_order_release);
    }
    return true;
}

void rs485_bridge_view_destroy(void)
{
    if (scr_bridge && lv_obj_is_valid(scr_bridge)) lv_obj_del(scr_bridge);
}

static void rs485_bridge_scr_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr_bridge && scr_bridge != deleted) return;
    dd_rs_baud = NULL;
    lbl_net = NULL;
    lbl_state = NULL;
    lbl_hint = NULL;
    if (tmr_status) { lv_timer_del(tmr_status); tmr_status = NULL; }
    scr_bridge = NULL;
}

static void rs485_bridge_home_cb_(lv_event_t *e)
{
    (void)e;
    (void)app_controller_local_stop();
}

lv_obj_t *rs485_bridge_view_show(lv_obj_t *parent)
{
    (void)parent;
    const ui_theme_palette_t *th = ui_theme_get();
    const bool color_mode = (ui_theme_get_id() == UI_THEME_COLOR);

    if (screen_alive(scr_bridge) && s_theme_rev != ui_theme_get_revision()) {
        rs485_bridge_view_destroy();
    }

    if (scr_bridge) {
        rs485_bridge_view_suspend(false);
        ui_baud_selector_sync(dd_rs_baud);
        lv_scr_load(scr_bridge);
        return scr_bridge;
    }

    scr_bridge = lv_obj_create(NULL);
    lv_obj_clear_flag(scr_bridge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_bridge, rs485_bridge_scr_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr_bridge);
    lv_obj_set_style_border_width(scr_bridge, 0, LV_PART_MAIN);
    s_theme_rev = ui_theme_get_revision();

    lv_obj_t *title = lv_label_create(scr_bridge);
    lv_label_set_text(title, "RS485-BRIDGE");
    if (color_mode) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(UI_RS485_BRIDGE_TITLE_HEX), 0);
    } else {
        ui_theme_apply_title(title, &lv_font_montserrat_26);
    }
    lv_obj_align(title, LV_ALIGN_TOP_MID, 48, 12);

    dd_rs_baud = ui_baud_selector_create(scr_bridge);
    lv_obj_set_size(dd_rs_baud, 148, 42);
    lv_obj_align(dd_rs_baud, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_text_font(dd_rs_baud, &lv_font_montserrat_16, 0);

    dialog_ui_create_button(scr_bridge,
                            LCD_WIDTH - 66 - 8, 4, 66, 66,
                            LV_SYMBOL_HOME, color_mode ? COLOR_BUTTON_HOME : th->button,
                            rs485_bridge_home_cb_, NULL, &lv_font_montserrat_28);

    if (wifi_ap_is_enabled() && rs485_is_ready_()) {
        lv_obj_t *card_net;
        lv_obj_t *card_state;
        int card_x = 8;
        int card_w = LCD_WIDTH - 16;
        int card_gap = 10;
        int card_y = 68;
        int card_net_h = 102;
        int card_state_h = 128;

        card_net = create_status_card_(scr_bridge, "NETWORK",
                                       card_x, card_y, card_w, card_net_h);
        card_state = create_status_card_(scr_bridge, "BRIDGE",
                                         card_x, card_y + card_net_h + card_gap, card_w, card_state_h);

        lbl_net = lv_label_create(card_net);
        lv_obj_set_width(lbl_net, card_w - 20);
        lv_label_set_long_mode(lbl_net, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lbl_net, &lv_font_montserrat_16, 0);
        if (color_mode) {
            lv_obj_set_style_text_color(lbl_net, lv_color_hex(UI_RS485_BRIDGE_CARD_TEXT_HEX), 0);
        } else {
            ui_theme_apply_text(lbl_net, &lv_font_montserrat_16);
        }
        lv_obj_align(lbl_net, LV_ALIGN_TOP_LEFT, 0, 32);

        lbl_state = lv_label_create(card_state);
        lv_obj_set_width(lbl_state, card_w - 20);
        lv_label_set_long_mode(lbl_state, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_16, 0);
        if (color_mode) {
            lv_obj_set_style_text_color(lbl_state, lv_color_hex(UI_RS485_BRIDGE_CARD_TEXT_HEX), 0);
        } else {
            ui_theme_apply_text(lbl_state, &lv_font_montserrat_16);
        }
        lv_obj_align(lbl_state, LV_ALIGN_TOP_LEFT, 0, 32);

        tmr_status = lv_timer_create(rs485_bridge_status_timer_cb_, 500, NULL);
        if (!tmr_status) {
            ESP_LOGW(TAG, "Status timer allocation failed");
        }
        if (atomic_load_explicit(&bridge_task, memory_order_acquire)) {
            rs485_bridge_status_update_();
        }
    } else {
        const char *hint = wifi_ap_is_enabled()
                               ? "RS-485 is busy or unavailable"
                               : "Enable Wi-Fi on the main screen";
        lv_obj_t *card_hint = create_hint_card_(scr_bridge, "NETWORK",
                                                hint,
                                                8, 86, LCD_WIDTH - 16, 158);
        (void)card_hint;
        lbl_hint = lv_label_create(scr_bridge);
        lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
    }

    lv_scr_load(scr_bridge);
    return scr_bridge;
}

lv_obj_t *rs485_bridge_get_screen(void)
{
    return scr_bridge;
}


bool rs485_bridge_runtime_running(void)
{
    return rs485_is_ready_() && atomic_load_explicit(&bridge_task, memory_order_acquire) != NULL;
}

bool rs485_bridge_runtime_start(void)
{
    if (rs485_bridge_runtime_running()) return true;
    if (!rs485_runtime_log_init(&s_runtime_log)) return false;
    if (rs485_acquire(RS485_OWNER_NETWORK_BRIDGE, rs485_get_baudrate()) != ESP_OK) return false;
    atomic_store_explicit(&rs485_ready, true, memory_order_release);
    atomic_store(&s_bytes_net_to_rs, 0);
    atomic_store(&s_bytes_rs_to_net, 0);
    atomic_store(&s_pkts_net_to_rs, 0);
    atomic_store(&s_pkts_rs_to_net, 0);
    TaskHandle_t task = NULL;
    atomic_store_explicit(&bridge_stop, false, memory_order_release);
    if (xTaskCreatePinnedToCore(rs485_bridge_task_, "rs485_bridge", 4096,
                                NULL, 4, &task, tskNO_AFFINITY) != pdPASS) {
        if (rs485_release(RS485_OWNER_NETWORK_BRIDGE) == ESP_OK) atomic_store(&rs485_ready, false);
        return false;
    }
    atomic_store_explicit(&bridge_task, task, memory_order_release);
    telnet_router_register(TELNET_ROUTE_RS485_BRIDGE, rs485_bridge_telnet_rx_cb_, NULL);
    telnet_router_set_active(TELNET_ROUTE_RS485_BRIDGE);
    udp_nmea_server_set_rx_cb(rs485_bridge_udp_rx_cb_, NULL);
    atomic_store_explicit(&s_net_rx_enabled, true, memory_order_release);
    return true;
}

esp_err_t rs485_bridge_runtime_set_baud(uint32_t baud)
{
    return rs485_bridge_runtime_running() ? rs485_set_baudrate(baud) : ESP_ERR_INVALID_STATE;
}

esp_err_t rs485_bridge_runtime_write(const uint8_t *data, size_t length)
{
    if (!data || !length || length > BRIDGE_RX_BUF_SZ) return ESP_ERR_INVALID_ARG;
    if (!bridge_net_rx_begin_()) return ESP_ERR_INVALID_STATE;
    esp_err_t result = rs485_bridge_send_to_rs_(data, length);
    bridge_net_rx_end_();
    return result;
}

void rs485_bridge_runtime_status(rs485_bridge_status_t *out)
{
    if (!out) return;
    *out = (rs485_bridge_status_t){
        .running = rs485_bridge_runtime_running(), .baud = rs485_get_baudrate(),
        .bytes_to_uart = atomic_load(&s_bytes_net_to_rs), .bytes_from_uart = atomic_load(&s_bytes_rs_to_net),
        .packets_to_uart = atomic_load(&s_pkts_net_to_rs), .packets_from_uart = atomic_load(&s_pkts_rs_to_net)
    };
}

bool rs485_bridge_runtime_read(uint32_t *cursor, rs485_runtime_event_t *out, uint32_t *dropped)
{
    return rs485_runtime_log_read(&s_runtime_log, cursor, out, dropped);
}

void rs485_bridge_view_suspend(bool suspended)
{
    if (tmr_status) { if (suspended) lv_timer_pause(tmr_status); else lv_timer_resume(tmr_status); }
}

lv_obj_t *rs485_bridge_create(lv_obj_t *parent)
{
    if (!rs485_bridge_runtime_start()) return NULL;
    return rs485_bridge_view_show(parent);
}
