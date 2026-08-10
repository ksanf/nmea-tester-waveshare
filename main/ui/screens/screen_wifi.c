/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   WiFi settings — AP/STA mode, scan, connect, NVS persist.
 */

#include "ui/screens/screen_wifi.h"
#include "ui/screens/screen_ui.h"
#include "ui/dialog_ui.h"
#include "ui/ui_theme.h"
#include "wifi/wifi_manager.h"
#include "system/telnet_router.h"
#include "system/telnet_server.h"
#include "system/udp_nmea_server.h"
#include "can_module/bridge_can_module/protocol_handler.h"

#include "config/config_nmea_tester.h"
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_SCREEN_SETTINGS
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_montserrat_18)
LV_FONT_DECLARE(lv_font_montserrat_20)
LV_FONT_DECLARE(lv_font_montserrat_22)
LV_FONT_DECLARE(lv_font_montserrat_26)
LV_FONT_DECLARE(lv_font_montserrat_28)

static const char *TAG = "screen_wifi";

/* Layout */
#define HOME_SZ        66
#define HOME_FONT      &lv_font_montserrat_28
#define TITLE_FONT     &lv_font_montserrat_22
#define LABEL_FONT     &lv_font_montserrat_18
#define BTN_FONT       &lv_font_montserrat_20
#define FIELD_LBL_W    80
#define FIELD_W        280
#define FIELD_H        48
#define ROW_H          60
#define MARGIN_X       20
#define LIST_W         (LCD_WIDTH - 2 * MARGIN_X)
#define ITEM_H         44
#define APPLY_W        300
#define APPLY_H        56
#define MODE_SW_W      140
#define MODE_SW_H      48
#define HEADER_CTRL_Y  76
#define STATUS_H       112
#define HEADER_GAP     12
#define KEYBOARD_H     230
#define BODY_MIN_H      96
#define CONNECT_SHEET_H 156
#define MAX_SCAN       20

typedef struct {
    char ssid[33];
    wifi_auth_mode_t authmode;
    int8_t rssi;
} scan_item_data_t;

/* ── Screen state ──────────────────────────────────────────────── */
static lv_obj_t *scr = NULL;
static lv_obj_t *ta_ssid  = NULL;
static lv_obj_t *ta_pass  = NULL;
static lv_obj_t *ta_ip    = NULL;
static lv_obj_t *ta_sta_pass = NULL;
static lv_obj_t *kb      = NULL;
static lv_obj_t *kb_panel = NULL;
static lv_obj_t *cont_ap = NULL;
static lv_obj_t *cont_sta = NULL;
static lv_obj_t *connect_sheet = NULL;
static lv_obj_t *list_cont = NULL;
static lv_obj_t *lbl_sta_status = NULL;
static lv_obj_t *btn_mode_ap = NULL;
static lv_obj_t *btn_mode_sta = NULL;
static lv_obj_t *btn_scan = NULL;
static lv_obj_t *btn_disconnect = NULL;
static lv_obj_t *lbl_selected_ssid = NULL;
static lv_obj_t *lbl_status_box = NULL;
static lv_obj_t *scroll_body = NULL;
static lv_timer_t *status_timer = NULL;
static lv_coord_t s_body_full_h = 0;
static uint32_t s_theme_rev = 0;

static char s_selected_ssid[33];
static wifi_ap_record_t s_scan_list[MAX_SCAN];
static uint16_t s_scan_count = 0;
static _Atomic(TaskHandle_t) s_scan_task = NULL;
static atomic_bool s_scan_busy = ATOMIC_VAR_INIT(false);
static atomic_bool s_scan_ready = ATOMIC_VAR_INIT(false);
static _Atomic uint16_t s_scan_ready_count = 0;

/* ── Helpers ──────────────────────────────────────────────────── */
static inline bool screen_alive_(lv_obj_t *obj)
{
    return obj && lv_obj_is_valid(obj) && lv_obj_get_parent(obj) == NULL;
}

static void wifi_screen_clear_refs_(void)
{
    scr = NULL; ta_ssid = NULL; ta_pass = NULL; ta_ip = NULL;
    ta_sta_pass = NULL; kb = NULL; kb_panel = NULL; cont_ap = NULL; cont_sta = NULL;
    connect_sheet = NULL;
    list_cont = NULL; lbl_sta_status = NULL;
    btn_mode_ap = NULL; btn_mode_sta = NULL; btn_scan = NULL; btn_disconnect = NULL;
    lbl_selected_ssid = NULL;
    lbl_status_box = NULL; scroll_body = NULL;
    s_body_full_h = 0;
}

static void wifi_screen_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr && scr != deleted) return;
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    wifi_screen_clear_refs_();
}

static void wifi_screen_destroy_(void)
{
    lv_obj_t *old = scr;
    if (!screen_alive_(old)) return;
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    wifi_screen_clear_refs_();
    lv_obj_del_async(old);
}

/* ── Keyboard ─────────────────────────────────────────────────── */
static void kb_close(void)
{
    if (kb_panel && lv_obj_is_valid(kb_panel)) {
        lv_obj_del(kb_panel);
        kb_panel = NULL;
        kb = NULL;
    } else if (kb && lv_obj_is_valid(kb)) {
        lv_obj_del(kb);
        kb = NULL;
    }
    if (scroll_body && lv_obj_is_valid(scroll_body) && s_body_full_h > 0) {
        lv_obj_set_height(scroll_body, s_body_full_h);
    }
}

static bool obj_is_child_of_(lv_obj_t *obj, lv_obj_t *parent)
{
    while (obj) {
        if (obj == parent) return true;
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

static lv_coord_t textarea_y_in_body_(lv_obj_t *ta)
{
    lv_coord_t y = 0;
    lv_obj_t *obj = ta;

    while (obj && obj != scroll_body) {
        y += lv_obj_get_y(obj);
        obj = lv_obj_get_parent(obj);
    }
    return y;
}

static void scroll_textarea_above_keyboard_(lv_obj_t *ta)
{
    if (!ta || !scroll_body || !lv_obj_is_valid(scroll_body)) return;
    if (!obj_is_child_of_(ta, scroll_body)) return;

    lv_coord_t visible_h = LCD_HEIGHT - lv_obj_get_y(scroll_body) - KEYBOARD_H - 8;
    if (visible_h < BODY_MIN_H) visible_h = BODY_MIN_H;
    lv_obj_set_height(scroll_body, visible_h);
    lv_obj_update_layout(scroll_body);

    lv_coord_t y = textarea_y_in_body_(ta);
    lv_coord_t h = lv_obj_get_height(ta);
    lv_coord_t cur = lv_obj_get_scroll_y(scroll_body);
    lv_coord_t target = cur;

    if (y < cur + 8) {
        target = y - 8;
    } else if (y + h > cur + visible_h - 8) {
        target = y + h - visible_h + 8;
    }
    if (target < 0) target = 0;
    lv_obj_scroll_to_y(scroll_body, target, LV_ANIM_ON);
}

static void kb_event_cb_(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        kb_close();
    }
}

static void style_keyboard_(lv_obj_t *keyboard)
{
    const ui_theme_palette_t *th = ui_theme_get();

    lv_obj_set_style_bg_color(keyboard, lv_color_hex(th->panel_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keyboard, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard, 5, LV_PART_MAIN);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_22, LV_PART_MAIN);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(th->text), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(th->card), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(keyboard, lv_color_hex(th->border), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(ui_theme_accent_hex(0x2F80ED)),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
}

static void keyboard_open_for_(lv_obj_t *ta)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!ta || !scr || !lv_obj_is_valid(scr)) return;

    kb_close();

    kb_panel = lv_obj_create(scr);
    lv_obj_set_pos(kb_panel, 0, LCD_HEIGHT - KEYBOARD_H);
    lv_obj_set_size(kb_panel, LCD_WIDTH, KEYBOARD_H);
    lv_obj_set_style_bg_color(kb_panel, lv_color_hex(th->panel_bg), 0);
    lv_obj_set_style_bg_opa(kb_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb_panel, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_width(kb_panel, 2, 0);
    lv_obj_set_style_radius(kb_panel, 0, 0);
    lv_obj_set_style_pad_all(kb_panel, 4, 0);
    lv_obj_clear_flag(kb_panel, LV_OBJ_FLAG_SCROLLABLE);

    kb = lv_keyboard_create(kb_panel);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(kb, LCD_WIDTH - 8, KEYBOARD_H - 8);
    lv_obj_set_pos(kb, 4, 4);
    style_keyboard_(kb);
    lv_obj_add_event_cb(kb, kb_event_cb_, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event_cb_, LV_EVENT_CANCEL, NULL);
    lv_obj_move_foreground(kb_panel);

    scroll_textarea_above_keyboard_(ta);
}

static void ta_clicked_cb_(lv_event_t *e)
{
    keyboard_open_for_(lv_event_get_target(e));
}

static void connect_dialog_close_(void)
{
    kb_close();
    if (connect_sheet && lv_obj_is_valid(connect_sheet)) {
        lv_obj_del(connect_sheet);
    }
    connect_sheet = NULL;
    ta_sta_pass = NULL;
}

static uint32_t contrast_text_hex_(uint32_t bg_hex)
{
    uint8_t r = (uint8_t)((bg_hex >> 16) & 0xFF);
    uint8_t g = (uint8_t)((bg_hex >> 8) & 0xFF);
    uint8_t b = (uint8_t)(bg_hex & 0xFF);
    uint32_t luma = (uint32_t)r * 299u + (uint32_t)g * 587u + (uint32_t)b * 114u;
    return (luma >= 128000u) ? 0x000000 : 0xFFFFFF;
}

static void style_button_colors_(lv_obj_t *btn, uint32_t bg_hex)
{
    uint32_t txt_hex = contrast_text_hex_(bg_hex);

    if (!btn) return;
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(txt_hex), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(txt_hex), LV_PART_MAIN | LV_STATE_PRESSED);
}

static void set_button_busy_(lv_obj_t *btn, bool busy)
{
    if (!btn || !lv_obj_is_valid(btn)) return;
    if (busy) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void scan_controls_set_busy_(bool busy)
{
    set_button_busy_(btn_mode_ap, busy);
    set_button_busy_(btn_mode_sta, busy);
    set_button_busy_(btn_scan, busy);
    set_button_busy_(btn_disconnect, busy);
}

/* ── Mode switch ──────────────────────────────────────────────── */
static void update_mode_buttons_(void)
{
    const ui_theme_palette_t *th = ui_theme_get();
    wifi_mgr_mode_t mode = wifi_manager_get_mode();
    uint32_t on_color  = ui_theme_accent_hex(0x50C878);
    uint32_t off_color = th->border;
    uint32_t ap_color = (mode == WIFI_MGR_MODE_AP) ? on_color : off_color;
    uint32_t sta_color = (mode == WIFI_MGR_MODE_STA) ? on_color : off_color;

    style_button_colors_(btn_mode_ap, ap_color);
    style_button_colors_(btn_mode_sta, sta_color);

    if (mode == WIFI_MGR_MODE_AP) {
        if (cont_ap) lv_obj_clear_flag(cont_ap, LV_OBJ_FLAG_HIDDEN);
        if (cont_sta) lv_obj_add_flag(cont_sta, LV_OBJ_FLAG_HIDDEN);
        if (btn_scan) lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        if (btn_disconnect) lv_obj_add_flag(btn_disconnect, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (cont_ap) lv_obj_add_flag(cont_ap, LV_OBJ_FLAG_HIDDEN);
        if (cont_sta) lv_obj_clear_flag(cont_sta, LV_OBJ_FLAG_HIDDEN);
        if (btn_scan) lv_obj_clear_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        if (btn_disconnect) lv_obj_clear_flag(btn_disconnect, LV_OBJ_FLAG_HIDDEN);
    }
}

static esp_err_t wifi_services_sync_(void)
{
    esp_err_t err;

    if (!wifi_manager_is_started()) {
        protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_UART);
        (void)udp_nmea_server_stop();
        (void)telnet_server_stop();
        return ESP_OK;
    }

    err = telnet_server_start();
    if (err == ESP_OK) err = udp_nmea_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network service startup failed: %s", esp_err_to_name(err));
        (void)udp_nmea_server_stop();
        (void)telnet_server_stop();
        protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_UART);
        return err;
    }
    protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_WIFI);
    return ESP_OK;
}

static void mode_ap_cb_(lv_event_t *e)
{
    (void)e;
    connect_dialog_close_();
    if (wifi_manager_set_mode(WIFI_MGR_MODE_AP) == ESP_OK) {
        (void)wifi_services_sync_();
        update_mode_buttons_();
    }
}

static void mode_sta_cb_(lv_event_t *e)
{
    (void)e;
    connect_dialog_close_();
    if (wifi_manager_set_mode(WIFI_MGR_MODE_STA) == ESP_OK) {
        (void)wifi_services_sync_();
        update_mode_buttons_();
    }
}

/* ── AP Apply ─────────────────────────────────────────────────── */
static void ap_apply_cb_(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(ta_ssid);
    const char *pass = lv_textarea_get_text(ta_pass);
    const char *ip   = lv_textarea_get_text(ta_ip);
    if (wifi_manager_ap_set(ssid, pass, ip) == ESP_OK) {
        (void)wifi_manager_set_mode(WIFI_MGR_MODE_AP);
        (void)wifi_services_sync_();
        update_mode_buttons_();
        ESP_LOGI(TAG, "AP config saved: %s", ssid);
    }
}

/* ── Scan ─────────────────────────────────────────────────────── */
static void scan_refresh_list_(void);

static bool auth_needs_password_(wifi_auth_mode_t authmode)
{
    return authmode != WIFI_AUTH_OPEN;
}

static const char *auth_label_(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "secured";
    }
}

static void sta_connect_to_(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;

    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
    if (lbl_selected_ssid) {
        lv_label_set_text_fmt(lbl_selected_ssid, "Selected: %s", s_selected_ssid);
    }

    esp_err_t err = wifi_manager_sta_set(s_selected_ssid, pass ? pass : "");
    if (err == ESP_OK) {
        err = wifi_manager_set_mode(WIFI_MGR_MODE_STA);
    }

    if (err == ESP_OK) {
        err = wifi_services_sync_();
    }

    if (err == ESP_OK) {
        update_mode_buttons_();
        if (lbl_sta_status) {
            lv_label_set_text_fmt(lbl_sta_status,
                                  wifi_manager_is_started()
                                      ? "Connecting to %s..."
                                      : "Saved %s (Wi-Fi is off)",
                                  s_selected_ssid);
        }
        if (wifi_manager_is_started()) {
            ESP_LOGI(TAG, "STA connecting to %s", s_selected_ssid);
        } else {
            ESP_LOGI(TAG, "STA saved while Wi-Fi is off: %s", s_selected_ssid);
        }
    } else {
        if (lbl_sta_status) {
            lv_label_set_text_fmt(lbl_sta_status, "Failed to start STA: 0x%x", (unsigned)err);
        }
        ESP_LOGE(TAG, "STA connect start failed for %s: %s", s_selected_ssid, esp_err_to_name(err));
    }
}

static void connect_password_cb_(lv_event_t *e)
{
    (void)e;
    char pass[65] = {0};
    if (ta_sta_pass && lv_obj_is_valid(ta_sta_pass)) {
        strlcpy(pass, lv_textarea_get_text(ta_sta_pass), sizeof(pass));
    }
    connect_dialog_close_();
    sta_connect_to_(s_selected_ssid, pass);
}

static void connect_cancel_cb_(lv_event_t *e)
{
    (void)e;
    connect_dialog_close_();
}

static void connect_dialog_open_(const char *ssid, wifi_auth_mode_t authmode, int8_t rssi)
{
    const ui_theme_palette_t *th = ui_theme_get();
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};
    lv_coord_t sheet_w = LCD_WIDTH - 2 * MARGIN_X;
    lv_coord_t sheet_y = LCD_HEIGHT - KEYBOARD_H - CONNECT_SHEET_H - 8;

    if (!ssid || !ssid[0]) return;

    connect_dialog_close_();
    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));
    if (lbl_selected_ssid) {
        lv_label_set_text_fmt(lbl_selected_ssid, "Selected: %s", s_selected_ssid);
    }

    if (!auth_needs_password_(authmode)) {
        sta_connect_to_(s_selected_ssid, "");
        return;
    }

    if (sheet_y < 76) sheet_y = 76;
    connect_sheet = lv_obj_create(scr);
    lv_obj_set_pos(connect_sheet, MARGIN_X, sheet_y);
    lv_obj_set_size(connect_sheet, sheet_w, CONNECT_SHEET_H);
    lv_obj_set_style_bg_color(connect_sheet, lv_color_hex(th->card), 0);
    lv_obj_set_style_bg_opa(connect_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(connect_sheet, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_width(connect_sheet, 2, 0);
    lv_obj_set_style_radius(connect_sheet, 8, 0);
    lv_obj_set_style_pad_all(connect_sheet, 12, 0);
    lv_obj_clear_flag(connect_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(connect_sheet);
    lv_label_set_text_fmt(title, "Connect to %s", s_selected_ssid);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, sheet_w - 28);
    lv_obj_set_style_text_color(title, lv_color_hex(th->title), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 2, 0);

    lv_obj_t *meta = lv_label_create(connect_sheet);
    lv_label_set_text_fmt(meta, "%s, %d dBm", auth_label_(authmode), (int)rssi);
    lv_obj_set_style_text_color(meta, lv_color_hex(th->muted), 0);
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(meta, 2, 30);

    lv_obj_t *lbl_pass = lv_label_create(connect_sheet);
    lv_label_set_text(lbl_pass, "Password:");
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(th->text), 0);
    lv_obj_set_style_text_font(lbl_pass, LABEL_FONT, 0);
    lv_obj_set_pos(lbl_pass, 2, 67);

    ta_sta_pass = lv_textarea_create(connect_sheet);
    lv_textarea_set_one_line(ta_sta_pass, true);
    lv_textarea_set_max_length(ta_sta_pass, 63);
    lv_textarea_set_password_mode(ta_sta_pass, true);
    lv_obj_set_size(ta_sta_pass, sheet_w - 260, FIELD_H);
    lv_obj_set_pos(ta_sta_pass, 118, 55);
    lv_obj_set_style_text_font(ta_sta_pass, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(ta_sta_pass, ta_clicked_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta_sta_pass, ta_clicked_cb_, LV_EVENT_FOCUSED, NULL);

    wifi_manager_sta_get(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
    if (strcmp(saved_ssid, s_selected_ssid) == 0 && saved_pass[0]) {
        lv_textarea_set_text(ta_sta_pass, saved_pass);
    }

    lv_obj_t *btn_cancel = lv_btn_create(connect_sheet);
    lv_obj_set_size(btn_cancel, 120, 44);
    lv_obj_set_pos(btn_cancel, sheet_w - 260, 98);
    style_button_colors_(btn_cancel, th->border);
    lv_obj_add_event_cb(btn_cancel, connect_cancel_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t *btn_connect = lv_btn_create(connect_sheet);
    lv_obj_set_size(btn_connect, 120, 44);
    lv_obj_set_pos(btn_connect, sheet_w - 132, 98);
    style_button_colors_(btn_connect, ui_theme_accent_hex(0x006B54));
    lv_obj_add_event_cb(btn_connect, connect_password_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_connect = lv_label_create(btn_connect);
    lv_label_set_text(lbl_connect, "Connect");
    lv_obj_set_style_text_font(lbl_connect, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_connect);

    lv_obj_move_foreground(connect_sheet);
    keyboard_open_for_(ta_sta_pass);
}

static void scan_task_(void *arg)
{
    (void)arg;

    /* scan_cb_ publishes our handle before releasing this task. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint16_t count = wifi_manager_scan(s_scan_list, MAX_SCAN);

    atomic_store_explicit(&s_scan_ready_count, count, memory_order_relaxed);
    atomic_store_explicit(&s_scan_ready, true, memory_order_release);
    atomic_store_explicit(&s_scan_busy, false, memory_order_release);
    atomic_store_explicit(&s_scan_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

static void scan_cb_(lv_event_t *e)
{
    TaskHandle_t created_task = NULL;

    (void)e;
    if (atomic_load_explicit(&s_scan_busy, memory_order_acquire) ||
        atomic_load_explicit(&s_scan_task, memory_order_acquire)) {
        if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
            lv_label_set_text(lbl_status_box, "Scanning...");
        }
        return;
    }

    connect_dialog_close_();
    s_scan_count = 0;
    atomic_store_explicit(&s_scan_ready, false, memory_order_release);
    atomic_store_explicit(&s_scan_busy, true, memory_order_release);
    scan_controls_set_busy_(true);
    scan_refresh_list_();
    if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
        lv_label_set_text(lbl_status_box, "Scanning...");
    }

    if (xTaskCreate(scan_task_, "wifi_scan", 4096, NULL, 4, &created_task) != pdPASS) {
        atomic_store_explicit(&s_scan_busy, false, memory_order_release);
        scan_controls_set_busy_(false);
        if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
            lv_label_set_text(lbl_status_box, "Scan: task start failed");
        }
        return;
    }
    atomic_store_explicit(&s_scan_task, created_task, memory_order_release);
    xTaskNotifyGive(created_task);
}

static void scan_item_cb_(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    scan_item_data_t *item = lv_obj_get_user_data(btn);
    if (!item) return;
    connect_dialog_open_(item->ssid, item->authmode, item->rssi);
}

static void scan_item_delete_cb_(lv_event_t *e)
{
    scan_item_data_t *item = lv_event_get_user_data(e);
    free(item);
}

static void scan_refresh_list_(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);

    for (uint16_t i = 0; i < s_scan_count; ++i) {
        char label[96];
        int rssi = s_scan_list[i].rssi;

        snprintf(label, sizeof(label), "%s  %d dBm  %s",
                 (const char *)s_scan_list[i].ssid, rssi,
                 auth_label_(s_scan_list[i].authmode));

        uint32_t list_btn_bg = ui_theme_accent_hex(0x006B54);
        uint32_t list_txt = contrast_text_hex_(list_btn_bg);
        lv_obj_t *btn = lv_btn_create(list_cont);
        lv_obj_set_size(btn, LIST_W - 8, ITEM_H - 4);
        lv_obj_set_pos(btn, 0, (lv_coord_t)(i * ITEM_H + 2));
        style_button_colors_(btn, list_btn_bg);
        lv_obj_add_event_cb(btn, scan_item_cb_, LV_EVENT_CLICKED, NULL);

        scan_item_data_t *item = malloc(sizeof(*item));
        if (item) {
            strlcpy(item->ssid, (const char *)s_scan_list[i].ssid, sizeof(item->ssid));
            item->authmode = s_scan_list[i].authmode;
            item->rssi = (int8_t)s_scan_list[i].rssi;
            lv_obj_set_user_data(btn, item);
            lv_obj_add_event_cb(btn, scan_item_delete_cb_, LV_EVENT_DELETE, item);
        }

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LIST_W - 36);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(list_txt), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
    }
}

static void status_timer_cb_(lv_timer_t *timer)
{
    (void)timer;
    char ip[24] = "";

    if (atomic_exchange_explicit(&s_scan_ready, false, memory_order_acq_rel)) {
        s_scan_count = atomic_load_explicit(&s_scan_ready_count,
                                            memory_order_relaxed);
        scan_refresh_list_();
        scan_controls_set_busy_(false);
        if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
            if (s_scan_count > 0) {
                lv_label_set_text_fmt(lbl_status_box, "Scan: %u networks found", (unsigned)s_scan_count);
            } else {
                lv_label_set_text(lbl_status_box, "Scan: no networks found");
            }
        }
        return;
    }

    if (atomic_load_explicit(&s_scan_busy, memory_order_acquire)) {
        scan_controls_set_busy_(true);
        if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
            lv_label_set_text(lbl_status_box, "Scanning...");
        }
        return;
    }

    if (lbl_status_box && lv_obj_is_valid(lbl_status_box)) {
        if (!wifi_manager_is_started()) {
            lv_label_set_text(lbl_status_box, "WiFi: Off");
        } else if (wifi_manager_get_mode() == WIFI_MGR_MODE_AP) {
            wifi_manager_get_ip_str(ip, sizeof(ip));
            lv_label_set_text_fmt(lbl_status_box, "Mode: AP\nIP: %s", ip[0] ? ip : "starting...");
        } else {
            switch (wifi_manager_sta_state()) {
            case WIFI_MGR_CONNECTED:
                wifi_manager_get_ip_str(ip, sizeof(ip));
                lv_label_set_text_fmt(lbl_status_box, "Mode: STA\nIP: %s\nNet: %s",
                    ip[0] ? ip : "connected",
                    s_selected_ssid[0] ? s_selected_ssid : "?");
                break;
            case WIFI_MGR_CONNECTING:
                lv_label_set_text_fmt(lbl_status_box, "Mode: STA\nConnecting...");
                break;
            default:
                lv_label_set_text(lbl_status_box, "Mode: STA\nDisconnected");
                break;
            }
        }
    }

    if (lbl_sta_status && lv_obj_is_valid(lbl_sta_status) && wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
        switch (wifi_manager_sta_state()) {
        case WIFI_MGR_CONNECTED:
            lv_label_set_text(lbl_sta_status, ip[0] ? ip : "Connected"); break;
        case WIFI_MGR_CONNECTING:
            lv_label_set_text(lbl_sta_status, "Connecting..."); break;
        default: break;
        }
    }
}

/* ── Disconnect ────────────────────────────────────────────────── */
static void disconnect_cb_(lv_event_t *e)
{
    (void)e;
    wifi_manager_sta_disconnect();
    if (lbl_status_box && lv_obj_is_valid(lbl_status_box))
        lv_label_set_text(lbl_status_box, "Mode: STA\nDisconnected");
    if (lbl_sta_status && lv_obj_is_valid(lbl_sta_status))
        lv_label_set_text(lbl_sta_status, "Disconnected");
    ESP_LOGI(TAG, "STA disconnected by user");
}

/* ── Home ─────────────────────────────────────────────────────── */
static void home_cb_(lv_event_t *e)
{
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    connect_dialog_close_();
    wifi_screen_destroy_();
    screen_ui_show();
}

/* ── Build textarea row ────────────────────────────────────────── */
static lv_obj_t *add_field_(lv_obj_t *parent, lv_coord_t y,
                            const char *label, const char *init_text,
                            lv_coord_t field_w)
{
    const ui_theme_palette_t *th = ui_theme_get();

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text), 0);
    lv_obj_set_style_text_font(lbl, LABEL_FONT, 0);
    lv_obj_set_pos(lbl, 0, y + 10);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (label[0] == 'I') ? 15 : 63);
    lv_obj_set_size(ta, field_w, FIELD_H);
    lv_obj_set_pos(ta, FIELD_LBL_W, y);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    if (init_text && init_text[0]) lv_textarea_set_text(ta, init_text);
    lv_obj_add_event_cb(ta, ta_clicked_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, ta_clicked_cb_, LV_EVENT_FOCUSED, NULL);

    return ta;
}

/* ── Build screen ─────────────────────────────────────────────── */
static void build_screen_(void)
{
    char buf[64];
    lv_coord_t ap_h, sta_h;

    scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, wifi_screen_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr);

    /* Home button */
    dialog_ui_create_button(scr, LCD_WIDTH - HOME_SZ - 8, 3,
                            HOME_SZ, HOME_SZ, LV_SYMBOL_HOME,
                            ui_theme_can_home_hex(), home_cb_,
                            NULL, HOME_FONT);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi Settings");
    ui_theme_apply_title(title, TITLE_FONT);
    lv_obj_set_pos(title, 8, 12);

    /* Mode selector row */
    lv_coord_t mode_y = HEADER_CTRL_Y;
    lv_coord_t mode_x = LCD_WIDTH - MARGIN_X - 2 * MODE_SW_W - HEADER_GAP;

    btn_mode_ap = lv_btn_create(scr);
    lv_obj_set_size(btn_mode_ap, MODE_SW_W, MODE_SW_H);
    lv_obj_set_pos(btn_mode_ap, mode_x, mode_y);
    lv_obj_add_event_cb(btn_mode_ap, mode_ap_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ap = lv_label_create(btn_mode_ap);
    lv_label_set_text(lbl_ap, "AP");
    lv_obj_set_style_text_font(lbl_ap, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_ap);

    btn_mode_sta = lv_btn_create(scr);
    lv_obj_set_size(btn_mode_sta, MODE_SW_W, MODE_SW_H);
    lv_obj_set_pos(btn_mode_sta, mode_x + MODE_SW_W + HEADER_GAP, mode_y);
    lv_obj_add_event_cb(btn_mode_sta, mode_sta_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_sta = lv_label_create(btn_mode_sta);
    lv_label_set_text(lbl_sta, "STA");
    lv_obj_set_style_text_font(lbl_sta, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_sta);

    btn_scan = lv_btn_create(scr);
    lv_obj_set_size(btn_scan, MODE_SW_W, MODE_SW_H);
    lv_obj_set_pos(btn_scan, mode_x, mode_y + MODE_SW_H + HEADER_GAP);
    style_button_colors_(btn_scan, ui_theme_accent_hex(0x2F80ED));
    lv_obj_add_event_cb(btn_scan, scan_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_scan = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_font(lbl_scan, BTN_FONT, 0);
    lv_obj_center(lbl_scan);

    btn_disconnect = lv_btn_create(scr);
    lv_obj_set_size(btn_disconnect, MODE_SW_W, MODE_SW_H);
    lv_obj_set_pos(btn_disconnect, mode_x + MODE_SW_W + HEADER_GAP, mode_y + MODE_SW_H + HEADER_GAP);
    style_button_colors_(btn_disconnect, ui_theme_accent_hex(0x8E3B46));
    lv_obj_add_event_cb(btn_disconnect, disconnect_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_disc = lv_label_create(btn_disconnect);
    lv_label_set_text(lbl_disc, "Disconnect");
    lv_obj_set_style_text_font(lbl_disc, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_disc);

    /* Status panel */
    {
        const ui_theme_palette_t *th = ui_theme_get();
        lv_coord_t status_w = mode_x - MARGIN_X - HEADER_GAP;
        lv_obj_t *sp = lv_obj_create(scr);
        lv_obj_set_pos(sp, MARGIN_X, mode_y);
        lv_obj_set_size(sp, status_w, STATUS_H);
        lv_obj_set_style_bg_color(sp, lv_color_hex(th->card), 0);
        lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sp, lv_color_hex(th->border), 0);
        lv_obj_set_style_border_width(sp, 1, 0);
        lv_obj_set_style_radius(sp, 6, 0);
        lv_obj_set_style_pad_all(sp, 6, 0);
        lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
        lbl_status_box = lv_label_create(sp);
        lv_label_set_long_mode(lbl_status_box, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_status_box, status_w - 16);
        lv_obj_set_style_text_color(lbl_status_box, lv_color_hex(th->text), 0);
        lv_obj_set_style_text_font(lbl_status_box, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl_status_box, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* Scrollable body for panels */
    lv_coord_t body_y = mode_y + STATUS_H + HEADER_GAP;
    lv_coord_t body_h = LCD_HEIGHT - body_y - 8;
    s_body_full_h = body_h;

    scroll_body = lv_obj_create(scr);
    lv_obj_set_pos(scroll_body, 0, body_y);
    lv_obj_set_size(scroll_body, LCD_WIDTH, body_h);
    lv_obj_set_scroll_dir(scroll_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(scroll_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_body, 0, 0);
    lv_obj_set_style_pad_all(scroll_body, MARGIN_X, 0);

    /* ═══ AP panel ═══ */
    ap_h = 3 * ROW_H + APPLY_H + 30;
    cont_ap = lv_obj_create(scroll_body);
    lv_obj_set_size(cont_ap, LCD_WIDTH - 2 * MARGIN_X, ap_h);
    lv_obj_set_style_bg_opa(cont_ap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_ap, 0, 0);
    lv_obj_clear_flag(cont_ap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont_ap, 0, 0);

    ta_ssid = add_field_(cont_ap, 0,         "SSID:", NULL, FIELD_W);
    ta_pass = add_field_(cont_ap, ROW_H,     "Pass:", NULL, FIELD_W);
    ta_ip   = add_field_(cont_ap, 2*ROW_H,    "IP:",   NULL, 180);

    lv_obj_t *btn_ap_apply = lv_btn_create(cont_ap);
    lv_obj_set_size(btn_ap_apply, APPLY_W, APPLY_H);
    lv_obj_set_pos(btn_ap_apply, (LCD_WIDTH - 2*MARGIN_X - APPLY_W)/2, 3*ROW_H + 8);
    style_button_colors_(btn_ap_apply, ui_theme_accent_hex(0x2F80ED));
    lv_obj_add_event_cb(btn_ap_apply, ap_apply_cb_, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ap_apply = lv_label_create(btn_ap_apply);
    lv_label_set_text(lbl_ap_apply, "Apply");
    lv_obj_set_style_text_font(lbl_ap_apply, BTN_FONT, 0);
    lv_obj_center(lbl_ap_apply);

    /* ═══ STA panel ═══ */
    sta_h = body_h - 2 * MARGIN_X;
    if (sta_h < 120) sta_h = 120;
    cont_sta = lv_obj_create(scroll_body);
    lv_obj_set_size(cont_sta, LCD_WIDTH - 2 * MARGIN_X, sta_h);
    lv_obj_set_style_bg_opa(cont_sta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_sta, 0, 0);
    lv_obj_clear_flag(cont_sta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont_sta, 0, 0);

    /* Network list */
    list_cont = lv_obj_create(cont_sta);
    lv_obj_set_pos(list_cont, 0, 0);
    lv_obj_set_size(list_cont, LIST_W, sta_h);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(list_cont, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);

    /* Load saved values */
    wifi_manager_ap_get(buf, sizeof(buf), NULL, 0, NULL, 0);
    if (buf[0] && ta_ssid) lv_textarea_set_text(ta_ssid, buf);
    wifi_manager_ap_get(NULL, 0, buf, sizeof(buf), NULL, 0);
    if (buf[0] && ta_pass) lv_textarea_set_text(ta_pass, buf);
    wifi_manager_ap_get(NULL, 0, NULL, 0, buf, sizeof(buf));
    if (buf[0] && ta_ip) lv_textarea_set_text(ta_ip, buf);

    wifi_manager_sta_get(buf, sizeof(buf), NULL, 0);
    if (buf[0]) {
        strlcpy(s_selected_ssid, buf, sizeof(s_selected_ssid));
    }

    update_mode_buttons_();
    status_timer = lv_timer_create(status_timer_cb_, 1000, NULL);
    status_timer_cb_(status_timer);
}

/* ── Public ───────────────────────────────────────────────────── */
void screen_wifi_show(lv_obj_t *parent)
{
    (void)parent;
    telnet_router_set_active(TELNET_ROUTE_NONE);


    if (screen_alive_(scr) && s_theme_rev != ui_theme_get_revision()) {
        wifi_screen_destroy_();
    }

    if (screen_alive_(scr)) {
        lv_scr_load(scr);
        return;
    }

    s_selected_ssid[0] = 0;
    s_scan_count = 0;
    kb = NULL;
    build_screen_();
    s_theme_rev = ui_theme_get_revision();
    lv_scr_load(scr);
    ESP_LOGI(TAG, "WiFi screen loaded");
}
