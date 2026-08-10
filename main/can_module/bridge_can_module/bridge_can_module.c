/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   CAN Bridge UI and tasks for the Sailor Inmarsat-C terminal tunnel.
 */

#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <lvgl.h>
#include <stdlib.h>

#include "bridge_can_module.h"
#include "bridge_can_config.h"
#include "protocol_handler.h"
#include "rs485_driver.h"
#include "can_driver.h"
#include "dialog_ui.h"
#include "screen_ui.h"
#include "ui/ui_theme.h"
#include "config_nmea_tester.h"
#include "system/telnet_server.h"
#include "system/wifi_ap.h"

#define TAG "BRIDGE"

#define BRIDGE_TELNET_PORT    23

LV_FONT_DECLARE(lv_font_unscii_8);

static lv_obj_t *scr = NULL;
static lv_obj_t *dd_rs_baud = NULL;
static lv_obj_t *lbl_can_state = NULL;
static lv_obj_t *lbl_can_meta = NULL;
static lv_obj_t *lbl_pc_mode = NULL;
static lv_obj_t *lbl_pc_state = NULL;
static lv_timer_t *status_timer = NULL;
static uint32_t s_theme_rev = 0;

static const uint32_t rs_baud_list[] = { 4800, 9600, 19200, 38400, 115200 };
static const uint32_t can_baud_list[] = { 250000 };
static size_t rs_sel = 0;
static size_t can_sel = 0;
static volatile protocol_link_state_t g_link_state = PROTO_STATE_BOOT;
static void build_baud_opts_(char *buf, size_t buf_sz) {
    size_t pos = 0;
    if (!buf || buf_sz == 0) return;
    buf[0] = 0;
    for (size_t i = 0; i < sizeof(rs_baud_list) / sizeof(rs_baud_list[0]); ++i) {
        int n = snprintf(buf + pos, buf_sz - pos, "%lu Bd%s",
                         (unsigned long)rs_baud_list[i],
                         (i + 1u < sizeof(rs_baud_list) / sizeof(rs_baud_list[0])) ? "\n" : "");
        if (n < 0) break;
        if ((size_t)n >= buf_sz - pos) {
            pos = buf_sz - 1;
            break;
        }
        pos += (size_t)n;
    }
}

static const char* link_state_str(protocol_link_state_t st) {
    switch (st) {
        case PROTO_STATE_BOOT: return "BOOT";
        case PROTO_STATE_ERROR: return "ERROR";
        case PROTO_STATE_ONLINE: return "ONLINE";
        default: return "?";
    }
}

static void ui_status_from_handler(protocol_link_state_t st, void *user) {
    (void)user;
    g_link_state = st;
}

static void detach_ui_sinks_(void)
{
    protocol_handler_set_ui(NULL, ui_status_from_handler, NULL);
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
}

static void bridge_can_delete_cb_(lv_event_t *e)
{
    (void)e;
    scr = NULL;
    dd_rs_baud = NULL;
    lbl_can_state = NULL;
    lbl_can_meta = NULL;
    lbl_pc_mode = NULL;
    lbl_pc_state = NULL;
    status_timer = NULL;
}

static bool bridge_can_runtime_stop_(void)
{
    detach_ui_sinks_();
    return protocol_handler_deinit();
}

static void bridge_can_destroy_(void)
{
    if (!bridge_can_runtime_stop_()) {
        HLOGE(TAG, "Protocol runtime did not stop; screen kept open");
        return;
    }
    if (!scr || !lv_obj_is_valid(scr)) return;
    lv_obj_del(scr);
}

static void cb_rs_baud_changed(lv_event_t *e) {
    uint32_t sel;
    uint32_t previous_sel;
    esp_err_t err;
    sel = 4;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (protocol_handler_get_term_io_owner() == PROTOCOL_TERM_IO_WIFI) return;
    sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= (sizeof(rs_baud_list) / sizeof(rs_baud_list[0]))) sel = 0;
    if (sel == rs_sel) return;

    previous_sel = rs_sel;
    err = protocol_handler_set_rs_baudrate(rs_baud_list[sel]);
    if (err != ESP_OK) {
        HLOGW(TAG, "RS baud change failed: %s", esp_err_to_name(err));
        lv_dropdown_set_selected(lv_event_get_target(e), previous_sel);
        return;
    }
    rs_sel = sel;
    ui_status_from_handler(g_link_state, NULL);
}

static lv_obj_t *create_status_card_(lv_obj_t *parent, const char *title,
                                     lv_coord_t x, lv_coord_t y,
                                     lv_coord_t w, lv_coord_t h)
{
    const ui_theme_palette_t *th = ui_theme_get();
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(th->card), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title);
    ui_theme_apply_title(lbl, &lv_font_montserrat_16);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

static void bridge_status_refresh_(void)
{
    char client_ip[16] = {0};
    char ap_ip[16] = {0};
    bool wifi_on = wifi_ap_is_enabled();
    bool telnet_client = telnet_server_client_connected();
    bool have_client_ip = telnet_server_get_client_ip(client_ip, sizeof(client_ip));
    bool have_ap_ip = wifi_ap_get_ip(ap_ip, sizeof(ap_ip));
    protocol_term_io_t owner = protocol_handler_get_term_io_owner();
    bool term_ready = protocol_handler_is_term_ready();

    if (lbl_can_state) {
        lv_label_set_text_fmt(lbl_can_state, "FSM: %s", link_state_str(g_link_state));
    }
    if (lbl_can_meta) {
        lv_label_set_text_fmt(lbl_can_meta, "CAN %luk\nTunnel %s",
                              (unsigned long)(can_baud_list[can_sel] / 1000),
                              term_ready ? "READY" : "WAIT");
    }
    if (lbl_pc_mode) {
        if (owner == PROTOCOL_TERM_IO_WIFI) {
            lv_label_set_text(lbl_pc_mode, "PC: TELNET");
        } else {
            lv_label_set_text_fmt(lbl_pc_mode, "PC: RS-485 %lu",
                                  (unsigned long)rs_baud_list[rs_sel]);
        }
    }
    if (lbl_pc_state) {
        if (owner == PROTOCOL_TERM_IO_WIFI) {
            if (!wifi_on) {
                lv_label_set_text(lbl_pc_state, "AP OFF");
            } else if (telnet_client) {
                lv_label_set_text_fmt(lbl_pc_state, "AP %s:%u\nClient: %s",
                                      have_ap_ip ? ap_ip : "?.?.?.?",
                                      (unsigned)BRIDGE_TELNET_PORT,
                                      have_client_ip ? client_ip : "connected");
            } else {
                lv_label_set_text_fmt(lbl_pc_state, "AP %s:%u\nClient: none",
                                      have_ap_ip ? ap_ip : "?.?.?.?",
                                      (unsigned)BRIDGE_TELNET_PORT);
            }
        } else {
            lv_label_set_text(lbl_pc_state, "Echo: ON\nMode: single-char");
        }
    }
    if (dd_rs_baud) {
        if (owner == PROTOCOL_TERM_IO_WIFI) {
            lv_obj_add_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void bridge_status_timer_cb_(lv_timer_t *timer)
{
    (void)timer;
    bridge_status_refresh_();
}

static void cb_home(lv_event_t *e) {
    (void)e;

    if (!bridge_can_runtime_stop_()) {
        HLOGE(TAG, "Protocol runtime did not stop; staying on Bridge CAN screen");
        return;
    }
    screen_ui_show();
    if (scr && lv_obj_is_valid(scr)) lv_obj_del(scr);
}

static lv_obj_t* build_screen(lv_obj_t *parent) {
    const ui_theme_palette_t *th = ui_theme_get();
    char baud_opts[80];
    lv_obj_t *card_can;
    lv_obj_t *card_pc;
    int card_y = 92;
    int card_w = (LCD_WIDTH - 28) / 2;
    int card_h = 136;

    (void)parent;
    scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, bridge_can_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr);

    dialog_ui_create_button(scr, LCD_WIDTH - 66 - 8, 3, 66, 66, LV_SYMBOL_HOME,
                           th->button, cb_home, NULL, &lv_font_montserrat_28);

    {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "Bridge CAN");
        ui_theme_apply_title(lbl, &lv_font_montserrat_20);
        lv_obj_set_pos(lbl, 8, 12);
    }

    build_baud_opts_(baud_opts, sizeof(baud_opts));
    dd_rs_baud = lv_dropdown_create(scr);
    lv_dropdown_set_options(dd_rs_baud, baud_opts);
    lv_dropdown_set_selected(dd_rs_baud, rs_sel);
    ui_theme_apply_dropdown(dd_rs_baud, &lv_font_montserrat_20);
    lv_obj_set_size(dd_rs_baud, 176, 44);
    lv_obj_align(dd_rs_baud, LV_ALIGN_TOP_RIGHT, -66 - 8 - 12, 8);
    lv_obj_add_event_cb(dd_rs_baud, cb_rs_baud_changed, LV_EVENT_VALUE_CHANGED, NULL);

    card_can = create_status_card_(scr, "CAN", 8, card_y, card_w, card_h);
    card_pc = create_status_card_(scr, "PC", 16 + card_w, card_y, card_w, card_h);

    lbl_can_state = lv_label_create(card_can);
    ui_theme_apply_text(lbl_can_state, &lv_font_montserrat_26);
    lv_obj_align(lbl_can_state, LV_ALIGN_TOP_LEFT, 0, 32);

    lbl_can_meta = lv_label_create(card_can);
    ui_theme_apply_muted(lbl_can_meta, &lv_font_montserrat_20);
    lv_obj_align(lbl_can_meta, LV_ALIGN_TOP_LEFT, 0, 80);

    lbl_pc_mode = lv_label_create(card_pc);
    ui_theme_apply_text(lbl_pc_mode, &lv_font_montserrat_26);
    lv_obj_align(lbl_pc_mode, LV_ALIGN_TOP_LEFT, 0, 32);

    lbl_pc_state = lv_label_create(card_pc);
    ui_theme_apply_muted(lbl_pc_state, &lv_font_montserrat_20);
    lv_obj_align(lbl_pc_state, LV_ALIGN_TOP_LEFT, 0, 80);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint,
                      "RS-485: single-char + echo\n"
                      "TELNET: multi-char + no echo");
    ui_theme_apply_muted(hint, &lv_font_montserrat_18);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    return scr;
}

lv_obj_t *bridge_can_create(lv_obj_t *parent) {
    protocol_handler_cfg_t cfg;

    HLOGI(TAG, "Bridge CAN UI v1.0.0 started, free heap=%u", esp_get_free_heap_size());
    if (scr && s_theme_rev != ui_theme_get_revision()) {
        bridge_can_destroy_();
    }
    if (scr && lv_obj_is_valid(scr)) {
        lv_scr_load(scr);
        bridge_status_refresh_();
        return scr;
    }
    for (size_t i = 0; i < sizeof(rs_baud_list) / sizeof(rs_baud_list[0]); ++i) {
        if (rs_baud_list[i] == rs485_get_baudrate()) {
            rs_sel = i;
            break;
        }
    }
    build_screen(parent);
    s_theme_rev = ui_theme_get_revision();

    cfg = (protocol_handler_cfg_t) {
        .can_bitrate = can_baud_list[can_sel],
        .rs_baudrate = rs_baud_list[rs_sel]
    };
    protocol_handler_set_ui(NULL, ui_status_from_handler, NULL);
    if (!protocol_handler_init(&cfg) || !protocol_handler_start()) {
        HLOGE(TAG, "protocol handler bring-up failed");
        detach_ui_sinks_();
        protocol_handler_deinit();
    }
    lv_scr_load(scr);
    status_timer = lv_timer_create(bridge_status_timer_cb_, 250, NULL);
    bridge_status_refresh_();
    ui_status_from_handler(g_link_state, NULL);
    return scr;
}

void bridge_can_module_start(lv_obj_t *parent) {
    (void)bridge_can_create(parent);
}
