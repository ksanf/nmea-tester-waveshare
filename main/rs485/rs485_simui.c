/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "rs485/rs485_simui.h"
#include "ui/dialog_ui.h"
#include "ui/instrument_panel.h"
#include "ui/nmea_log.h"
#include "rs485/rs485_engine.h"
#include "rs485/rs485_driver.h"
#include "rs485/rs485_transmit.h"
#include "ui/ui_baud_selector.h"
#include "nmea_editor/nmea_editor.h"
#include "nmea_editor/nmea_version.h"
#include "ui/screens/screen_ui.h"          /* main screen getter */
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "system/telnet_router.h"
#include "system/telnet_server.h"
#include "system/udp_nmea_server.h"
#include "system/wifi_ap.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_RS485_SIMUI
#include "config_logs.h"
#include "lvgl.h"
#include "config/config_nmea_tester.h"
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>


LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_22)
LV_FONT_DECLARE(lv_font_montserrat_28)

#define BTN_W        116
#define BTN_H         56
#define BTN_GAP        4
#define LOG_LINES      8
#define TOP_BAR_H     72
#define Y_OFFSET       2
#define PANEL_H       60
#define ENGINE_UI_INTERVAL_MS 200

/* Home and Settings button size and font. */
#define SIMUI_NAV_BTN_SZ    66
#define SIMUI_NAV_BTN_FONT  &lv_font_montserrat_28
/* Group button font. */
#define SIMUI_GRP_BTN_FONT  &lv_font_montserrat_22

static lv_obj_t *scr_simui = NULL;   /* Screen handle. */
static lv_obj_t *btn_grp[GRP_COUNT]; /* Toggle button handles. */
static lv_obj_t *lbl_net_status = NULL;
static lv_timer_t *tmr_net_status = NULL;
static lv_timer_t *tmr_engine_ui = NULL;
static lv_timer_t *tmr_log_drain = NULL;
static bool      rs485_ready = false;
static char      s_telnet_line[128];
static size_t    s_telnet_line_len = 0;
static char      s_udp_line[256];
static size_t    s_udp_line_len = 0;
static QueueHandle_t s_log_q = NULL;
static uint32_t s_theme_rev = 0;

#define SIMUI_LOG_Q_LEN 24

typedef struct {
    char line[NMEA_SENT_MAX + 3];
} simui_log_evt_t;

static const struct { const char *txt; uint32_t col; rs485_group_t grp; } BTN[GRP_COUNT] = {
    {"GPS", UI_GROUP_GPS_HEX, GRP_GPS},
    {"GYRO", UI_GROUP_GYRO_HEX, GRP_GYRO},
    {"LOG", UI_GROUP_LOG_HEX, GRP_LOG},
    {"ECHO", UI_GROUP_ECHO_HEX, GRP_ECHO},
    {"WX", UI_GROUP_WX_HEX, GRP_WX}
};

static uint32_t simui_group_color_(int idx);
static void simui_apply_group_checked_style_(lv_obj_t *btn);
static void scr_delete_cb(lv_event_t *e);
static void simui_update_net_status_(void);
static void simui_net_status_timer_cb_(lv_timer_t *tmr);
static void simui_engine_ui_timer_cb_(lv_timer_t *tmr);
static void simui_log_drain_timer_cb_(lv_timer_t *tmr);
static void simui_telnet_rx_cb_(const uint8_t *data, size_t len, void *user);
static void simui_udp_rx_cb_(const uint8_t *data, size_t len, void *user);
static void cb_toggle_grp(lv_event_t *e);
static void cb_btn_settings(lv_event_t *e);
static void cb_btn_back(lv_event_t *e);

static bool simui_prepare_port_(void)
{
    if (rs485_ready) return true;
    if (rs485_acquire(RS485_OWNER_TRANSMITTER, 0) != ESP_OK) {
        ESP_LOGE("simui", "RS-485 init failed");
        return false;
    }
    rs485_ready = true;
    ESP_LOGI("simui", "RS-485 transmitter runtime prepared");
    return true;
}

static void simui_build_screen_(void)
{
    scr_simui = lv_obj_create(NULL);
    lv_obj_clear_flag(scr_simui, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_apply_screen(scr_simui);
    lv_obj_add_event_cb(scr_simui, scr_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *bar = lv_obj_create(scr_simui);
    lv_obj_set_size(bar, LCD_WIDTH, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    ui_theme_apply_card(bar);

    lv_obj_t *baud_dd = ui_baud_selector_create(bar);
    lv_obj_align(baud_dd, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_update_layout(bar);
    const lv_coord_t status_x = lv_obj_get_x(baud_dd) + lv_obj_get_width(baud_dd) + 16;
    const lv_coord_t status_right_margin = 150;
    lv_coord_t status_w = LCD_WIDTH - status_x - status_right_margin;
    if (status_w < 80) status_w = 80;

    lbl_net_status = lv_label_create(scr_simui);
    lv_obj_set_width(lbl_net_status, status_w);
    lv_label_set_long_mode(lbl_net_status, LV_LABEL_LONG_CLIP);
    ui_theme_apply_text(lbl_net_status, &lv_font_montserrat_14);
    lv_obj_set_style_text_align(lbl_net_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(lbl_net_status, 0, 0);
    lv_obj_align(lbl_net_status, LV_ALIGN_TOP_LEFT, status_x, 8);

    dialog_ui_create_button(scr_simui,
            LCD_WIDTH - SIMUI_NAV_BTN_SZ - 8,
            (TOP_BAR_H - SIMUI_NAV_BTN_SZ) / 2,
            SIMUI_NAV_BTN_SZ, SIMUI_NAV_BTN_SZ,
            LV_SYMBOL_HOME, ui_theme_nav_home_hex(),
            cb_btn_back, NULL, SIMUI_NAV_BTN_FONT);

    dialog_ui_create_button(scr_simui,
            LCD_WIDTH - SIMUI_NAV_BTN_SZ - 8 - SIMUI_NAV_BTN_SZ - 8,
            (TOP_BAR_H - SIMUI_NAV_BTN_SZ) / 2,
            SIMUI_NAV_BTN_SZ, SIMUI_NAV_BTN_SZ,
            LV_SYMBOL_SETTINGS, ui_theme_nav_settings_hex(),
            cb_btn_settings, NULL, SIMUI_NAV_BTN_FONT);

    for (int i = 0; i < GRP_COUNT; ++i) {
        btn_grp[i] = dialog_ui_create_button(scr_simui,
            BTN_GAP + i * (BTN_W + BTN_GAP),
            TOP_BAR_H + BTN_GAP + Y_OFFSET,
            BTN_W, BTN_H,
            BTN[i].txt, simui_group_color_(i),
            cb_toggle_grp,
            (void*)(intptr_t)BTN[i].grp, SIMUI_GRP_BTN_FONT);
        simui_apply_group_checked_style_(btn_grp[i]);
    }

    const int panel_y = TOP_BAR_H + BTN_H + 2 * BTN_GAP + Y_OFFSET;
    const int log_y = panel_y + PANEL_H;
    const int log_h = LCD_HEIGHT - log_y;

    instrument_panel_init(scr_simui,
        2,
        panel_y,
        LCD_WIDTH,
        PANEL_H);

    nmea_log_init(scr_simui,
                  2,
                  log_y,
                  LCD_WIDTH - 4,
                  log_h);
}

static bool simui_start_runtime_(void)
{
    if (!s_log_q) {
        s_log_q = xQueueCreate(SIMUI_LOG_Q_LEN, sizeof(simui_log_evt_t));
        if (!s_log_q) {
            ESP_LOGE("simui", "Log queue create failed");
            return false;
        }
    }
    if (!rs485_engine_init()) return false;
    telnet_router_register(TELNET_ROUTE_TX485, simui_telnet_rx_cb_, NULL);
    telnet_router_set_active(TELNET_ROUTE_TX485);
    udp_nmea_server_set_rx_cb(simui_udp_rx_cb_, NULL);
    simui_update_net_status_();
    tmr_net_status = lv_timer_create(simui_net_status_timer_cb_, 500, NULL);
    tmr_engine_ui = lv_timer_create(simui_engine_ui_timer_cb_,
                                    ENGINE_UI_INTERVAL_MS, NULL);
    tmr_log_drain = lv_timer_create(simui_log_drain_timer_cb_, 50, NULL);
    if (!tmr_net_status || !tmr_engine_ui || !tmr_log_drain) {
        ESP_LOGE("simui", "UI timer create failed");
        return false;
    }
    return true;
}

static uint32_t simui_group_color_(int idx)
{
    return ui_theme_accent_hex(BTN[idx].col);
}

static void simui_apply_group_checked_style_(lv_obj_t *btn)
{
    if (!btn) return;
    if (ui_theme_get_id() == UI_THEME_COLOR) return;
    dialog_ui_apply_checked_style(btn, ui_theme_group_active_hex());
}

/* ───── Helper: reset all buttons and groups ─────────────────────────── */
static void simui_reset_buttons(void)
{
    for (int g = 0; g < GRP_COUNT; g++) {
        if (btn_grp[g]) lv_obj_clear_state(btn_grp[g], LV_STATE_CHECKED);
        rs485_engine_set_active(g, false);
    }
}

/**
 * @brief Clear all global pointers when the screen is deleted.
 */
static void scr_delete_cb(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr_simui && scr_simui != deleted) return;
    ESP_LOGI("simui", "Screen DELETE event — cleaning up globals");
    for (int g = 0; g < GRP_COUNT; g++) btn_grp[g] = NULL;
    lbl_net_status = NULL;
    tmr_net_status = NULL;
    tmr_engine_ui = NULL;
    tmr_log_drain = NULL;
    scr_simui = NULL;
}

static void simui_update_net_status_(void)
{
    char ap_ip[16];
    char telnet_ip[16];
    char udp_ip[16];
    uint16_t udp_port = 0;
    bool wifi_on;
    bool telnet_ok;
    bool udp_ok;
    uint32_t udp_age_ms;
    char text[128];

    if (!lbl_net_status || !lv_obj_is_valid(lbl_net_status)) {
        return;
    }

    wifi_on = wifi_ap_is_enabled();
    if (!wifi_on) {
        lv_obj_clear_flag(lbl_net_status, LV_OBJ_FLAG_HIDDEN);
        snprintf(text, sizeof(text), "NMEA %s\nWiFi OFF",
                 nmea_version_short_name());
        lv_label_set_text(lbl_net_status, text);
        return;
    }

    lv_obj_clear_flag(lbl_net_status, LV_OBJ_FLAG_HIDDEN);
    if (!wifi_ap_get_ip(ap_ip, sizeof(ap_ip))) {
        strlcpy(ap_ip, "?.?.?.?", sizeof(ap_ip));
    }
    telnet_ok = telnet_server_get_client_ip(telnet_ip, sizeof(telnet_ip));
    udp_ok = udp_nmea_server_get_last_peer(udp_ip, sizeof(udp_ip), &udp_port);
    udp_age_ms = udp_nmea_server_get_last_peer_age_ms();
    if (!udp_ok || udp_age_ms > 15000u) {
        udp_ok = false;
    }

    snprintf(text, sizeof(text),
             "NMEA %s | TELNET %s:%u %s%s\nUDP %s:%u %s%s%s",
             nmea_version_short_name(),
             ap_ip, (unsigned)WIFI_TELNET_PORT,
             telnet_ok ? "<- " : "(idle)",
             telnet_ok ? telnet_ip : "",
             ap_ip, (unsigned)WIFI_UDP_NMEA_PORT,
             udp_ok ? "<- " : "(idle)",
             udp_ok ? udp_ip : "",
             udp_ok ? ":" : "");

    if (udp_ok) {
        const size_t off = strlen(text);
        snprintf(text + off, sizeof(text) - off, "%u", (unsigned)udp_port);
    }

    lv_label_set_text(lbl_net_status, text);
}

static void simui_net_status_timer_cb_(lv_timer_t *tmr)
{
    (void)tmr;
    simui_update_net_status_();
}

static void simui_engine_ui_timer_cb_(lv_timer_t *tmr)
{
    (void)tmr;
    rs485_engine_flush_ui();
}

static void simui_log_drain_timer_cb_(lv_timer_t *tmr)
{
    simui_log_evt_t evt;
    (void)tmr;

    while (s_log_q && xQueueReceive(s_log_q, &evt, 0) == pdTRUE) {
        nmea_log_add_plain(evt.line);
    }
}

static void simui_telnet_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!data || len == 0) return;

    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];

        if (c == '\r' || c == '\n') {
            if (s_telnet_line_len > 0) {
                s_telnet_line[s_telnet_line_len] = 0;
                rs485_transmit_send(s_telnet_line);
                s_telnet_line_len = 0;
            }
            (void)telnet_server_send((const uint8_t *)"\r\n", 2);
            continue;
        }

        if (c == 0x08u || c == 0x7Fu) {
            if (s_telnet_line_len > 0) {
                s_telnet_line_len--;
                (void)telnet_server_send((const uint8_t *)"\b \b", 3);
            }
            continue;
        }

        if (c < 32 || c > 126) continue;
        if (s_telnet_line_len + 1 >= sizeof(s_telnet_line)) continue;

        s_telnet_line[s_telnet_line_len++] = (char)c;
        (void)telnet_server_send(&c, 1);
    }
}

static void simui_udp_rx_cb_(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!data || len == 0) return;

    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];

        if (c == '\r' || c == '\n') {
            if (s_udp_line_len > 0) {
                s_udp_line[s_udp_line_len] = 0;
                rs485_transmit_send(s_udp_line);
                s_udp_line_len = 0;
            }
            continue;
        }

        if (c < 32 || c > 126) continue;
        if (s_udp_line_len + 1 >= sizeof(s_udp_line)) {
            s_udp_line[s_udp_line_len] = 0;
            rs485_transmit_send(s_udp_line);
            s_udp_line_len = 0;
        }

        s_udp_line[s_udp_line_len++] = (char)c;
    }

    if (s_udp_line_len > 0) {
        s_udp_line[s_udp_line_len] = 0;
        rs485_transmit_send(s_udp_line);
        s_udp_line_len = 0;
    }
}

void rs485_simui_log_tx(const char *msg)
{
    simui_log_evt_t evt;
    size_t len;

    if (!msg || !*msg || !s_log_q) return;
    if (!scr_simui || !lv_obj_is_valid(scr_simui)) return;

    len = strnlen(msg, sizeof(evt.line) - 1);
    memcpy(evt.line, msg, len);
    evt.line[len] = '\0';
    (void)xQueueSend(s_log_q, &evt, 0);
}

/*───────────────────────────────────────────────*/
/*  Callbacks                                    */
/*───────────────────────────────────────────────*/
static void cb_toggle_grp(lv_event_t *e)
{
    rs485_group_t g  = (rs485_group_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn    = lv_event_get_target(e);
    bool on = !lv_obj_has_state(btn, LV_STATE_CHECKED);
    rs485_engine_set_active(g, on);
    if (on) lv_obj_add_state(btn, LV_STATE_CHECKED);
    else    lv_obj_clear_state(btn, LV_STATE_CHECKED);
    ESP_LOGI("simui", "group %d %s", (int)g, on ? "ON" : "OFF");
}

static void cb_btn_settings(lv_event_t *e) {
  	(void)e;
	 simui_reset_buttons();      /* Clear checks and disable all groups. */
   	 nmea_editor_create();
}

static void simui_destroy(void)
{
    if (!scr_simui) return;

    /* Stop new batches first, then wait for any in-flight UART write. */
    simui_reset_buttons();
    if (!rs485_engine_deinit()) {
        ESP_LOGE("simui", "TX engine is still stopping; screen kept open");
        return;
    }

    if (tmr_net_status) {
        lv_timer_del(tmr_net_status);
        tmr_net_status = NULL;
    }
    if (tmr_log_drain) {
        lv_timer_del(tmr_log_drain);
        tmr_log_drain = NULL;
    }
    if (tmr_engine_ui) {
        lv_timer_del(tmr_engine_ui);
        tmr_engine_ui = NULL;
    }

    telnet_router_unregister(TELNET_ROUTE_TX485);
    udp_nmea_server_set_rx_cb(NULL, NULL);
    s_telnet_line_len = 0;
    s_udp_line_len = 0;
    if (s_log_q) {
        vQueueDelete(s_log_q);
        s_log_q = NULL;
    }

    /* Release RS-485 only after the engine no longer uses the UART. */
    esp_err_t release_err = rs485_release(RS485_OWNER_TRANSMITTER);
    if (release_err != ESP_OK) {
        ESP_LOGW("simui", "RS-485 release failed: %s", esp_err_to_name(release_err));
    }
    rs485_ready = false;

    /* Load the main screen before deleting the current one. */
    screen_ui_show();

    /* scr_delete_cb finishes clearing widget pointers on LV_EVENT_DELETE. */
    lv_obj_t *old = scr_simui;
    scr_simui = NULL;
    lv_obj_del_async(old);
}

static void cb_btn_back(lv_event_t *e)
{
    (void)e;
    simui_destroy();
}

/*───────────────────────────────────────────────*/
/*  Public API                                   */
/*───────────────────────────────────────────────*/
lv_obj_t *rs485_simui_create(lv_obj_t *parent)
{
    (void)parent;
    if (scr_simui && s_theme_rev != ui_theme_get_revision()) {
        simui_destroy();
    }
    if (scr_simui) { lv_scr_load(scr_simui); return scr_simui; }

    if (!simui_prepare_port_()) return NULL;
    simui_build_screen_();
    if (!simui_start_runtime_()) {
        simui_destroy();
        return NULL;
    }

    lv_scr_load(scr_simui);
    simui_reset_buttons();
    s_theme_rev = ui_theme_get_revision();
    return scr_simui;
}

lv_obj_t *rs485_simui_get_screen(void) { return scr_simui; }

void rs485_simui_flush_log(void)
{
    if (tmr_log_drain) {
        simui_log_drain_timer_cb_(tmr_log_drain);
    }
}
