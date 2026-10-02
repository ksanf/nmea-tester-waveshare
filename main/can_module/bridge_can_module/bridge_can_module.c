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
#include <math.h>
#include <time.h>
#include <lvgl.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "nm2k_module.h"
#include "app/app_controller.h"

#include "bridge_can_module.h"
#include "bridge_can_config.h"
#include "protocol_handler.h"
#include "sailor_status_format.h"
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
#define BRIDGE_SIGNAL_COLOR_HEX 0x50E3A4

static lv_obj_t *scr = NULL;
static lv_obj_t *dd_rs_baud = NULL;
static lv_obj_t *lbl_serial;
static lv_obj_t *lbl_connection;
static lv_obj_t *network_labels[4];
static lv_obj_t *network_values[4];
static const char *const network_names[4] = { "Ocean", "Registration", "Protocol", "Channel" };
static lv_obj_t *lbl_signal;
static lv_obj_t *lbl_signal_age;
static lv_obj_t *signal_bars[5];
static lv_obj_t *lbl_latitude;
static lv_obj_t *lbl_longitude;
static lv_obj_t *lbl_position_age;
static lv_obj_t *lbl_position_utc;
static lv_obj_t *lbl_terminal;
static lv_obj_t *lbl_terminal_state;
static lv_obj_t *lbl_wifi;
static lv_obj_t *lbl_baud;
static lv_timer_t *status_timer = NULL;
static uint32_t s_theme_rev = 0;

static const uint32_t rs_baud_list[] = { 4800, 9600, 19200, 38400, 115200 };
static size_t rs_sel = 0;
static _Atomic(protocol_term_io_t) s_local_term_owner = PROTOCOL_TERM_IO_UART;
static atomic_bool s_web_terminal_enabled;
static atomic_uint s_rs_baud = ATOMIC_VAR_INIT(4800u);
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

static void pause_view_animations_(lv_obj_t *obj)
{
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_anim_del(obj, NULL);
    uint32_t count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < count; ++i) pause_view_animations_(lv_obj_get_child(obj, i));
}

void bridge_can_view_suspend(void)
{
    if (status_timer) { lv_timer_del(status_timer); status_timer = NULL; }
    if (dd_rs_baud && lv_obj_is_valid(dd_rs_baud)) lv_dropdown_close(dd_rs_baud);
    pause_view_animations_(scr);
}

static void bridge_can_delete_cb_(lv_event_t *e)
{
    (void)e;
    bridge_can_view_suspend();
    scr = NULL;
    dd_rs_baud = NULL;
    lbl_serial = lbl_connection = NULL;
    memset(network_labels, 0, sizeof(network_labels));
    memset(network_values, 0, sizeof(network_values));
    lbl_signal = lbl_signal_age = NULL;
    memset(signal_bars, 0, sizeof(signal_bars));
    lbl_latitude = lbl_longitude = NULL;
    lbl_position_age = lbl_position_utc = NULL;
    lbl_terminal = lbl_terminal_state = lbl_wifi = lbl_baud = NULL;
    status_timer = NULL;
}

void bridge_can_view_destroy(void)
{
    bridge_can_view_suspend();
    if (scr && lv_obj_is_valid(scr)) lv_obj_del(scr);
}

esp_err_t bridge_can_runtime_start(uint32_t rs_baud)
{
    if (nm2k_runtime_running()) return ESP_ERR_INVALID_STATE;
    bool use_selected_baud = (rs_baud == 0);
    if (use_selected_baud) {
        rs_baud = protocol_handler_is_running() ? protocol_handler_get_rs_baudrate() : rs485_get_baudrate();
        if (!rs_baud) rs_baud = s_rs_baud;
    }
    bool valid = false;
    for (size_t i = 0; i < sizeof(rs_baud_list) / sizeof(rs_baud_list[0]); ++i) {
        if (rs_baud == rs_baud_list[i]) valid = true;
    }
    if (!valid && use_selected_baud) rs_baud = s_rs_baud;
    else if (!valid) return ESP_ERR_INVALID_ARG;
    if (protocol_handler_is_running()) {
        if (!protocol_handler_is_started()) return ESP_ERR_INVALID_STATE;
        esp_err_t err = protocol_handler_get_rs_baudrate() == rs_baud ? ESP_OK : protocol_handler_set_rs_baudrate(rs_baud);
        if (err == ESP_OK) s_rs_baud = rs_baud;
        return err;
    }
    protocol_handler_cfg_t cfg = { .can_bitrate = 250000u, .rs_baudrate = rs_baud };
    if (!protocol_handler_init(&cfg) || !protocol_handler_start()) {
        (void)protocol_handler_deinit();
        return ESP_FAIL;
    }
    s_rs_baud = rs_baud;
    if (s_web_terminal_enabled) {
        s_local_term_owner = protocol_handler_get_term_io_owner();
        protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_WEB);
    }
    return ESP_OK;
}

bool bridge_can_runtime_stop(void)
{
    bool stopped = !protocol_handler_is_running() || protocol_handler_deinit();
    if (stopped) bridge_can_set_web_terminal(false);
    return stopped;
}

bool bridge_can_runtime_running(void) { return protocol_handler_is_running(); }

void bridge_can_runtime_status(bridge_can_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running = protocol_handler_is_running();
    out->terminal_ready = protocol_handler_is_term_ready();
    out->state = protocol_handler_get_state();
    out->owner = protocol_handler_get_term_io_owner();
    out->can_bitrate = 250000u;
    out->rs_baudrate = protocol_handler_get_rs_baudrate();
    if (!out->rs_baudrate) out->rs_baudrate = s_rs_baud;
    protocol_handler_web_stats(&out->terminal_pending, &out->terminal_dropped);
    protocol_handler_get_antenna_status(&out->antenna);
}

void bridge_can_set_web_terminal(bool enabled)
{
    if (enabled) {
        if (!s_web_terminal_enabled) {
            s_local_term_owner = protocol_handler_get_term_io_owner();
            if (s_local_term_owner == PROTOCOL_TERM_IO_WEB) s_local_term_owner = PROTOCOL_TERM_IO_UART;
        }
        s_web_terminal_enabled = true;
        protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_WEB);
    } else {
        if (s_web_terminal_enabled) protocol_handler_set_term_io_owner(s_local_term_owner);
        s_web_terminal_enabled = false;
    }
}

esp_err_t bridge_can_terminal_write(const uint8_t *data, size_t len)
{
    return protocol_handler_web_write(data, len);
}

size_t bridge_can_terminal_drain(uint8_t *out, size_t cap)
{
    return protocol_handler_web_drain(out, cap);
}

static void cb_rs_baud_changed(lv_event_t *e) {
    uint32_t sel;
    uint32_t previous_sel;
    esp_err_t err;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    if (protocol_handler_get_term_io_owner() != PROTOCOL_TERM_IO_UART || app_controller_is_web()) return;
    sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= (sizeof(rs_baud_list) / sizeof(rs_baud_list[0]))) sel = 0;
    if (sel == rs_sel) return;

    previous_sel = rs_sel;
    err = app_controller_local_sailor_baud(rs_baud_list[sel]) ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        HLOGW(TAG, "RS baud change failed: %s", esp_err_to_name(err));
        lv_dropdown_set_selected(lv_event_get_target(e), previous_sel);
        return;
    }
    rs_sel = sel;

}

static lv_obj_t *status_label_(lv_obj_t *parent, const char *text,
                               const lv_font_t *font, int x, int y, bool muted)
{
    lv_obj_t *label = lv_label_create(parent);
    if (muted) ui_theme_apply_muted(label, font);
    else ui_theme_apply_text(label, font);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *create_status_card_(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui_theme_apply_card(card);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, LCD_HEIGHT < 400 ? 10 : 18, 0);
    return card;
}

/* Round the whole coordinate first so 59.9995 minutes carries into degrees. */
static bool coordinate_text_(char *buf, size_t cap, double value, bool longitude)
{
    if (!isfinite(value) || fabs(value) > (longitude ? 180.0 : 90.0)) {
        snprintf(buf, cap, "--");
        return false;
    }
    uint32_t units = (uint32_t)(fabs(value) * 60000.0 + 0.5);
    unsigned deg = units / 60000u;
    unsigned minutes = (units % 60000u) / 1000u;
    unsigned fraction = units % 1000u;
    char hemisphere = longitude ? (value < 0 ? 'W' : 'E') : (value < 0 ? 'S' : 'N');
    snprintf(buf, cap, longitude ? "%03u\xC2\xB0%02u.%03u' %c" : "%02u\xC2\xB0%02u.%03u' %c",
             deg, minutes, fraction, hemisphere);
    return true;
}

static void bridge_status_refresh_(void)
{
    if (!scr) return;
    const ui_theme_palette_t *th = ui_theme_get();
    bridge_can_status_t status;
    bridge_can_runtime_status(&status);
    const protocol_antenna_status_t *a = &status.antenna;
    const bool compact = LCD_HEIGHT < 400;
    const bool signal_fresh = a->online && a->signal_valid;
    const uint32_t accent = BRIDGE_SIGNAL_COLOR_HEX;
    char text[80];

    lv_label_set_text_fmt(lbl_serial, "S/N  %s", a->identity_valid && a->serial[0] ? a->serial : "--");
    lv_label_set_text(lbl_connection, a->online ? "Antenna online" : a->identity_valid ? "Antenna offline" : "Waiting for antenna");
    lv_obj_set_style_text_color(lbl_connection, lv_color_hex(a->online ? th->text : th->muted), 0);
    sailor_network_text_t network;
    sailor_status_format_network(a, &network);
    const sailor_status_value_t *fields[] = {
        &network.ocean, &network.registration, &network.protocol, &network.channel
    };
    for (unsigned i = 0; i < 4u; ++i) {
        const bool stale = fields[i]->known && !fields[i]->fresh;
        lv_label_set_text_fmt(network_labels[i], "%s%s", stale ? "Stale: " : "", network_names[i]);
        lv_label_set_text(network_values[i], fields[i]->known ? fields[i]->text : "--");
        lv_obj_set_style_text_color(network_values[i], lv_color_hex(fields[i]->fresh ? th->text : th->muted), 0);
    }
    if (signal_fresh) snprintf(text, sizeof(text), "%u", (unsigned)a->cn0_dbhz);
    else snprintf(text, sizeof(text), "--");
    lv_label_set_text(lbl_signal, text);
    lv_obj_set_style_text_color(lbl_signal, lv_color_hex(signal_fresh ? accent : th->muted), 0);
    lv_label_set_text(lbl_signal_age, signal_fresh ? "Fresh" : a->signal_age_ms != UINT32_MAX ? "Stale" : "Waiting");
    unsigned bars = signal_fresh ? a->signal_bars : 0u;
    if (bars > 5u) bars = 5u;
    const lv_color_t idle_bar = lv_color_mix(lv_color_hex(th->text), lv_color_hex(th->card), LV_OPA_20);
    for (unsigned i = 0; i < 5u; ++i) {
        lv_obj_set_style_bg_color(signal_bars[i], i < bars ? lv_color_hex(accent) : idle_bar, 0);
        lv_obj_set_style_bg_opa(signal_bars[i], LV_OPA_COVER, 0);
    }

    bool position_valid = a->position_valid;
    if (position_valid) {
        bool lat_ok = coordinate_text_(text, sizeof(text), a->latitude, false);
        lv_label_set_text(lbl_latitude, text);
        bool lon_ok = coordinate_text_(text, sizeof(text), a->longitude, true);
        lv_label_set_text(lbl_longitude, text);
        position_valid = lat_ok && lon_ok;
    }
    if (!position_valid) {
        lv_label_set_text(lbl_latitude, "--");
        lv_label_set_text(lbl_longitude, "--");
    }
    lv_label_set_text(lbl_position_age, !position_valid ? "Waiting" :
                      a->online && a->position_fresh ? "Fresh" : "Stale");
    if (position_valid && a->position_utc) {
        time_t timestamp = (time_t)a->position_utc;
        struct tm utc;
        if (gmtime_r(&timestamp, &utc)) {
            if (compact) snprintf(text, sizeof(text), "UTC %02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
            else snprintf(text, sizeof(text), "UTC  %04d-%02d-%02d  %02d:%02d:%02d",
                          utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
        } else snprintf(text, sizeof(text), "UTC --");
    } else snprintf(text, sizeof(text), "UTC --");
    lv_label_set_text(lbl_position_utc, text);

    const char *route = status.owner == PROTOCOL_TERM_IO_WEB ? "Browser" :
                        status.owner == PROTOCOL_TERM_IO_WIFI ? "Telnet" : "RS-485";
    lv_label_set_text_fmt(lbl_terminal, "%s terminal", route);
    lv_label_set_text(lbl_terminal_state, status.terminal_ready ? "Ready" : "Connecting");
    for (size_t i = 0; i < sizeof(rs_baud_list) / sizeof(rs_baud_list[0]); ++i) {
        if (rs_baud_list[i] == status.rs_baudrate) { rs_sel = i; break; }
    }
    lv_dropdown_set_selected(dd_rs_baud, rs_sel);
    if (status.owner != PROTOCOL_TERM_IO_UART) {
        lv_obj_add_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_baud, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(dd_rs_baud, LV_OBJ_FLAG_HIDDEN);
        if (compact) lv_obj_add_flag(lbl_baud, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(lbl_baud, LV_OBJ_FLAG_HIDDEN);
    }

    char ap_ip[16] = {0}, client_ip[16] = {0};
    if (!wifi_ap_is_enabled()) {
        lv_label_set_text(lbl_wifi, "Wi-Fi off");
    } else {
        bool have_ip = wifi_ap_get_ip(ap_ip, sizeof(ap_ip));
        bool client = telnet_server_client_connected();
        bool have_client = client && telnet_server_get_client_ip(client_ip, sizeof(client_ip));
        if (compact) {
            lv_label_set_text_fmt(lbl_wifi, "%s\n%s", have_ip ? ap_ip : "Getting IP...", client ? "Client connected" : "No client");
        } else {
            lv_label_set_text_fmt(lbl_wifi, "Wi-Fi  %s:%u\n%s%s", have_ip ? ap_ip : "Getting IP...",
                                  (unsigned)BRIDGE_TELNET_PORT,
                                  client ? "Client  " : "No terminal client", client ? (have_client ? client_ip : "connected") : "");
        }
    }
}

static void bridge_status_timer_cb_(lv_timer_t *timer)
{
    (void)timer;
    if (scr && lv_scr_act() == scr) bridge_status_refresh_();
}

static void cb_home(lv_event_t *e)
{
    (void)e;
    (void)app_controller_local_stop();
}

static lv_obj_t *build_screen(lv_obj_t *parent)
{
    const ui_theme_palette_t *th = ui_theme_get();
    const bool compact = LCD_HEIGHT < 400;
    const int margin = compact ? 10 : 16;
    const int gap = compact ? 12 : 16;
    const int top = compact ? 70 : 92;
    const int bottom_h = compact ? 54 : 96;
    const int bottom_y = LCD_HEIGHT - margin - bottom_h;
    const int card_h = bottom_y - gap - top;
    const int signal_w = compact ? 142 : 240;
    const int position_x = margin + signal_w + gap;
    char baud_opts[80];
    (void)parent;

    scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, bridge_can_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr);
    lv_obj_t *title = status_label_(scr, "SAILOR", &lv_font_montserrat_22, margin, compact ? 8 : 12, false);
    ui_theme_apply_title(title, &lv_font_montserrat_22);
    lbl_serial = status_label_(scr, "S/N  --", compact ? &lv_font_montserrat_16 : &lv_font_montserrat_22,
                               margin, compact ? 34 : 46, false);
    lv_obj_set_width(lbl_serial, compact ? 162 : 240);
    lv_label_set_long_mode(lbl_serial, LV_LABEL_LONG_DOT);
    lbl_connection = status_label_(scr, "Waiting for antenna", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                                   margin, compact ? 53 : 74, true);
    lv_obj_set_width(lbl_connection, compact ? 162 : 240);
    lv_label_set_long_mode(lbl_connection, LV_LABEL_LONG_DOT);
    const int network_x = compact ? 182 : 272;
    const int network_end = LCD_WIDTH - margin - (compact ? 44 : 56) - gap;
    const int network_column = (network_end - network_x) / 2;
    for (unsigned i = 0; i < 4u; ++i) {
        const int x = network_x + (int)(i % 2u) * network_column;
        const int y = (compact ? 4 : 8) + (int)(i / 2u) * (compact ? 30 : 38);
        network_labels[i] = status_label_(scr, network_names[i], &lv_font_montserrat_12, x, y, true);
        network_values[i] = status_label_(scr, "--", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16,
                                          x, y + (compact ? 15 : 16), false);
        lv_obj_set_width(network_labels[i], network_column - 8);
        lv_obj_set_width(network_values[i], network_column - 8);
        lv_label_set_long_mode(network_labels[i], LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(network_values[i], LV_LABEL_LONG_DOT);
    }
    dialog_ui_create_button(scr, LCD_WIDTH - margin - (compact ? 44 : 56), compact ? 7 : 12,
                           compact ? 44 : 56, compact ? 44 : 56, LV_SYMBOL_HOME,
                           th->button, cb_home, NULL, &lv_font_montserrat_26);

    lv_obj_t *signal = create_status_card_(scr, margin, top, signal_w, card_h);
    status_label_(signal, "SIGNAL C/N0", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0, 0, true);
    lbl_signal = status_label_(signal, "--", compact ? &lv_font_montserrat_40 : &lv_font_montserrat_48, 0, compact ? 23 : 40, false);
    status_label_(signal, "dBHz", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_18, compact ? 75 : 105, compact ? 48 : 65, true);
    const int bar_w = compact ? 14 : 25, bar_gap = compact ? 9 : 14;
    const int bar_bottom = compact ? 123 : 195;
    for (unsigned i = 0; i < 5; ++i) {
        signal_bars[i] = lv_obj_create(signal);
        lv_obj_remove_style_all(signal_bars[i]);
        const int h = (compact ? 15 : 20) + (int)i * (compact ? 7 : 12);
        lv_obj_set_size(signal_bars[i], bar_w, h);
        lv_obj_set_pos(signal_bars[i], (int)i * (bar_w + bar_gap), bar_bottom - h);
        lv_obj_set_style_bg_color(signal_bars[i], lv_color_hex(th->panel_bg), 0);
        lv_obj_set_style_bg_opa(signal_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(signal_bars[i], 3, 0);
        lv_obj_clear_flag(signal_bars[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lbl_signal_age = status_label_(signal, "Waiting", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                                   0, compact ? 136 : 201, true);

    lv_obj_t *position = create_status_card_(scr, position_x, top, LCD_WIDTH - margin - position_x, card_h);
    status_label_(position, "POSITION", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0, 0, true);
    lbl_position_age = status_label_(position, "Waiting", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0, 0, true);
    lv_obj_align(lbl_position_age, LV_ALIGN_TOP_RIGHT, 0, 0);
    status_label_(position, "Latitude", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0, compact ? 27 : 40, true);
    lbl_latitude = status_label_(position, "--", compact ? &lv_font_montserrat_26 : &lv_font_montserrat_40, 0, compact ? 44 : 61, false);
    status_label_(position, "Longitude", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16, 0, compact ? 86 : 121, true);
    lbl_longitude = status_label_(position, "--", compact ? &lv_font_montserrat_26 : &lv_font_montserrat_40, 0, compact ? 103 : 142, false);
    lbl_position_utc = status_label_(position, "UTC --", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                                     0, compact ? 136 : 201, true);

    lv_obj_t *terminal = create_status_card_(scr, margin, bottom_y, LCD_WIDTH - 2 * margin, bottom_h);
    if (!compact) status_label_(terminal, "TERMINAL BRIDGE", &lv_font_montserrat_12, 0, -3, true);
    lbl_terminal = status_label_(terminal, "RS-485 terminal", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_18, 0, compact ? -1 : 18, false);
    lbl_terminal_state = status_label_(terminal, "Connecting", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0, compact ? 15 : 40, true);
    const int baud_x = compact ? 151 : 230;
    lbl_baud = status_label_(terminal, "RS-485 baud", &lv_font_montserrat_12, baud_x, -3, true);
    if (compact) lv_obj_add_flag(lbl_baud, LV_OBJ_FLAG_HIDDEN);
    build_baud_opts_(baud_opts, sizeof(baud_opts));
    dd_rs_baud = lv_dropdown_create(terminal);
    lv_dropdown_set_options(dd_rs_baud, baud_opts);
    lv_dropdown_set_selected(dd_rs_baud, rs_sel);
    ui_theme_apply_dropdown(dd_rs_baud, compact ? &lv_font_montserrat_14 : &lv_font_montserrat_18);
    lv_obj_set_size(dd_rs_baud, compact ? 122 : 160, compact ? 32 : 42);
    lv_obj_set_pos(dd_rs_baud, baud_x, compact ? -1 : 18);
    lv_obj_add_event_cb(dd_rs_baud, cb_rs_baud_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_wifi = status_label_(terminal, "Wi-Fi off", compact ? &lv_font_montserrat_12 : &lv_font_montserrat_16,
                             compact ? 298 : 430, compact ? -1 : 15, true);
    return scr;
}

lv_obj_t *bridge_can_view_show(lv_obj_t *parent)
{
    if (scr && lv_obj_is_valid(scr) && s_theme_rev != ui_theme_get_revision()) bridge_can_view_destroy();
    if (!scr || !lv_obj_is_valid(scr)) {
        build_screen(parent);
        s_theme_rev = ui_theme_get_revision();
    }
    lv_scr_load(scr);
    if (!status_timer) status_timer = lv_timer_create(bridge_status_timer_cb_, 250, NULL);
    bridge_status_refresh_();
    return scr;
}

lv_obj_t *bridge_can_create(lv_obj_t *parent)
{
    return bridge_can_view_show(parent);
}

void bridge_can_module_start(lv_obj_t *parent) {
    (void)parent;
    (void)app_controller_local_start(APP_MODE_SAILOR);
}
