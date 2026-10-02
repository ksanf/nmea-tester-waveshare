/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester main screen with six primary modules.
 */

#include "lvgl.h"
#include "ui/screens/screen_ui.h"
#include "app/app_controller.h"
#include "web/web_server.h"
#include "ui/dialog_ui.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "config/config_nmea_tester.h"
#include "can_module/screen_can.h"  
#include "rs485/rs485_parser.h"
#include "rs485/rs485_simui.h"
#include "rs485_bridge/rs485_bridge.h"
#include "ui/screens/screen_settings.h"
#include "ui/screens/screen_wifi.h"
#include "system/udp_nmea_server.h"
#include "system/wifi_ap.h"
#include "system/telnet_server.h"
#include "wifi/wifi_manager.h"
#include "can_module/bridge_can_module/protocol_handler.h"
#include "system/telnet_router.h"
#include <esp_log.h>
#include <string.h>
#define CFG_LOG_MODULE LOG_CFG_SCREEN_UI
#include "config_logs.h"

/*─────────  Layout parameters  ─────────*/
LV_FONT_DECLARE(lv_font_montserrat_40)
LV_FONT_DECLARE(lv_font_montserrat_20)

#define GRID_COLS              3
#define GRID_ROWS              2
#define GRID_MARGIN_X         24
#define GRID_MARGIN_Y         40
#define GRID_GAP_X            16
#define GRID_GAP_Y            20

/* Full grid-cell dimensions and scaled button dimensions. */
#define BTN_CELL_W            ((LCD_WIDTH  - (2 * GRID_MARGIN_X) - ((GRID_COLS - 1) * GRID_GAP_X)) / GRID_COLS)
#define BTN_CELL_H            ((LCD_HEIGHT - (2 * GRID_MARGIN_Y) - ((GRID_ROWS - 1) * GRID_GAP_Y)) / GRID_ROWS)
#define MAIN_BTN_SCALE_NUM    9
#define MAIN_BTN_SCALE_DEN    10
#define BTN_W                 (BTN_CELL_W * MAIN_BTN_SCALE_NUM / MAIN_BTN_SCALE_DEN)
#define BTN_H                 (BTN_CELL_H * MAIN_BTN_SCALE_NUM / MAIN_BTN_SCALE_DEN)

/* Main-screen button font. */
#define MAIN_BTN_FONT         &lv_font_montserrat_40

/*─────────  Log tag and static state  ─────────*/
static const char *TAG = "screen_ui";

static lv_obj_t *scr_main = NULL;
static lv_obj_t *btn_wifi = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *lbl_wifi_address = NULL;
static lv_timer_t *wifi_status_timer = NULL;
static uint32_t s_theme_rev = 0;

/*─────────  Callback declarations  ─────────*/
static void cb_rs485(lv_event_t *e);
static void cb_tx485(lv_event_t *e);
static void cb_can(lv_event_t *e);
static void cb_settings(lv_event_t *e);
static void cb_rs485_bridge(lv_event_t *e);
static void cb_wifi(lv_event_t *e);

static void wifi_btn_refresh_(void);
static void wifi_status_timer_cb_(lv_timer_t *timer);
static void screen_main_visibility_cb_(lv_event_t *e);
static uint32_t main_btn_color_(int idx);
static void screen_main_delete_cb_(lv_event_t *e);
static void screen_main_destroy_(void);

/*─────────  Button table  ─────────*/
typedef struct {
    const char    *txt;
    uint32_t       color_hex;
    lv_event_cb_t  cb;
} btn_desc_t;

static const btn_desc_t BTNS[6] = {
    { "RX485",     UI_MAIN_BTN_RX485_HEX,       cb_rs485        },
    { "TX485",     UI_MAIN_BTN_TX485_HEX,       cb_tx485        },
    { "CAN",       UI_MAIN_BTN_CAN_HEX,         cb_can          },
    { "SETUP",     UI_MAIN_BTN_SETUP_HEX,       cb_settings     },
    { "RS485-PC",  UI_MAIN_BTN_RS485_BRIDGE_HEX, cb_rs485_bridge },
    { "WI-FI",     UI_MAIN_BTN_WIFI_OFF_HEX,    cb_wifi         }
};

/* --------------------------------------------------------------------------
 * Return true when the screen is valid and attached directly to the display.
 * -------------------------------------------------------------------------- */
static inline bool screen_alive(lv_obj_t *scr)
{
    return scr && lv_obj_is_valid(scr) && lv_obj_get_parent(scr) == NULL;
}

static void screen_main_delete_cb_(lv_event_t *e)
{
    (void)e;
    if (wifi_status_timer) {
        lv_timer_del(wifi_status_timer);
        wifi_status_timer = NULL;
    }
    scr_main = NULL;
    btn_wifi = NULL;
    lbl_wifi = NULL;
    lbl_wifi_address = NULL;
}

static void screen_main_destroy_(void)
{
    if (!screen_alive(scr_main)) return;
    lv_obj_del(scr_main);
}

/*────────────────  CALLBACKS  ───────────*/
static void cb_rs485(lv_event_t *e)
{
    (void)e;
    if (!app_controller_local_start(APP_MODE_RX485)) {
        ESP_LOGW(TAG, "RX start request rejected");
    }
}

static void cb_tx485(lv_event_t *e)
{
    (void)e;
    if (!app_controller_local_start(APP_MODE_TX485)) {
        ESP_LOGW(TAG, "TX start request rejected");
    }
}

static void cb_can(lv_event_t *e)
{
    lv_obj_t *scr_can;
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    scr_can = screen_can_show(scr_main);
    if (!scr_can) {
        ESP_LOGE(TAG, "CAN screen create failed");
        return;
    }
    lv_scr_load(scr_can);
}

static void cb_settings(lv_event_t *e)
{
    /* Add stop or pause handling here if future modules require it. */
    telnet_router_set_active(TELNET_ROUTE_NONE);
    screen_settings_show(scr_main);
}

static void wifi_btn_refresh_(void)
{
    if (!btn_wifi || !lv_obj_is_valid(btn_wifi) ||
        !lbl_wifi || !lv_obj_is_valid(lbl_wifi) ||
        !lbl_wifi_address || !lv_obj_is_valid(lbl_wifi_address)) return;

    const bool enabled = wifi_manager_is_started();
    const char *title = enabled ? "WI-FI ON" : "WI-FI OFF";
    const char *address = "";
    char ip[16] = "";

    if (enabled) {
        if (wifi_manager_get_mode() == WIFI_MGR_MODE_AP) {
            address = wifi_manager_get_ip_str(ip, sizeof(ip)) ? ip : "Starting...";
        } else {
            /* The netif may retain its old address while STA reconnects. */
            switch (wifi_manager_sta_state()) {
            case WIFI_MGR_CONNECTED:
                address = wifi_manager_get_ip_str(ip, sizeof(ip)) ? ip : "Getting IP...";
                break;
            case WIFI_MGR_CONNECTING:
                address = "Connecting...";
                break;
            default:
                address = "No connection";
                break;
            }
        }
    }

    /* Polling must not repaint an unchanged button every second. */
    if (strcmp(lv_label_get_text(lbl_wifi), title) == 0 &&
        strcmp(lv_label_get_text(lbl_wifi_address), address) == 0) return;

    const ui_theme_palette_t *th = ui_theme_get();
    const bool color_mode = (ui_theme_get_id() == UI_THEME_COLOR);
    lv_obj_set_style_bg_color(btn_wifi,
                              lv_color_hex(color_mode
                                               ? (enabled ? UI_MAIN_BTN_WIFI_ON_HEX : UI_MAIN_BTN_WIFI_OFF_HEX)
                                               : (enabled ? th->button : th->border)),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn_wifi, enabled ? LV_OPA_COVER : LV_OPA_70,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn_wifi,
                              lv_color_hex(color_mode
                                               ? (enabled ? UI_MAIN_BTN_WIFI_ON_PRESSED_HEX : UI_MAIN_BTN_WIFI_OFF_PRESSED_HEX)
                                               : th->card),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    lv_label_set_text(lbl_wifi, title);
    lv_obj_align(lbl_wifi, LV_ALIGN_CENTER, 0, enabled ? -18 : 0);
    lv_label_set_text(lbl_wifi_address, address);
    if (enabled) {
        lv_obj_clear_flag(lbl_wifi_address, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_wifi_address, LV_ALIGN_CENTER, 0, 28);
    } else {
        lv_obj_add_flag(lbl_wifi_address, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_status_timer_cb_(lv_timer_t *timer)
{
    (void)timer;
    if (screen_alive(scr_main) && lv_scr_act() == scr_main) {
        wifi_btn_refresh_();
    }
}

static void screen_main_visibility_cb_(lv_event_t *e)
{
    if (!wifi_status_timer) return;
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        wifi_btn_refresh_();
        lv_timer_resume(wifi_status_timer);
        lv_timer_reset(wifi_status_timer);
    } else if (lv_event_get_code(e) == LV_EVENT_SCREEN_UNLOADED) {
        lv_timer_pause(wifi_status_timer);
    }
}

static uint32_t main_btn_color_(int idx)
{
    return ui_theme_accent_hex(BTNS[idx].color_hex);
}

static void cb_wifi(lv_event_t *e)
{
    esp_err_t err;
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);

    if (wifi_manager_is_started()) {
        protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_UART);
        err = udp_nmea_server_stop();
        if (err == ESP_OK) err = telnet_server_stop();
        if (err == ESP_OK) err = wifi_manager_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi shutdown failed: %s", esp_err_to_name(err));
        }
    } else {
        err = wifi_ap_start();
        if (err == ESP_OK) err = telnet_server_start();
        if (err == ESP_OK) err = udp_nmea_server_start();
        if (err == ESP_OK) {
            esp_err_t web_err = web_server_start();
            if (web_err != ESP_OK) {
                ESP_LOGW(TAG, "Web server start failed: %s", esp_err_to_name(web_err));
            }
            protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_WIFI);
        } else {
            ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
            (void)udp_nmea_server_stop();
            (void)telnet_server_stop();
            (void)wifi_manager_deinit();
            protocol_handler_set_term_io_owner(PROTOCOL_TERM_IO_UART);
        }
    }

    wifi_btn_refresh_();
}

static void cb_rs485_bridge(lv_event_t *e)
{
    (void)e;
    if (!app_controller_local_start(APP_MODE_RS485_BRIDGE)) {
        ESP_LOGW(TAG, "RS485 bridge start request rejected");
    }
}

/*─────────  Public getter  ─────────*/
lv_obj_t *screen_ui_get_main(void) { return scr_main; }

/*────────────────  INIT  ──────────────────────*/
void screen_ui_init(void)
{
    telnet_router_set_active(TELNET_ROUTE_NONE);
    if (screen_alive(scr_main) && s_theme_rev != ui_theme_get_revision()) {
        screen_main_destroy_();
    }
    /* Root screen object. */
    if (!screen_alive(scr_main)) {
        scr_main = lv_obj_create(NULL);
        lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(scr_main, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(scr_main, screen_main_delete_cb_, LV_EVENT_DELETE, NULL);
        lv_obj_add_event_cb(scr_main, screen_main_visibility_cb_, LV_EVENT_SCREEN_LOADED, NULL);
        lv_obj_add_event_cb(scr_main, screen_main_visibility_cb_, LV_EVENT_SCREEN_UNLOADED, NULL);
        dialog_ui_apply_screen_bg(scr_main);
        lv_obj_set_style_border_width(scr_main, 0, LV_PART_MAIN);

        for (int i = 0; i < 6; ++i) {
            const int row = i / GRID_COLS;
            const int col = i % GRID_COLS;
            /* Position grid cells evenly across the screen. */
            int cell_x = GRID_MARGIN_X + col * (BTN_CELL_W + GRID_GAP_X);
            int cell_y = GRID_MARGIN_Y + row * (BTN_CELL_H + GRID_GAP_Y);
            /* Center the button within its grid cell. */
            int btn_x = cell_x + (BTN_CELL_W - BTN_W) / 2;
            int btn_y = cell_y + (BTN_CELL_H - BTN_H) / 2;
            lv_obj_t *btn = dialog_ui_create_button(scr_main,
                                                    btn_x, btn_y,
                                                    BTN_W, BTN_H,
                                                    BTNS[i].txt,
                                                    main_btn_color_(i),
                                                    BTNS[i].cb,
                                                    NULL, MAIN_BTN_FONT);
            if (i == 5) {
                btn_wifi = btn;
                lbl_wifi = lv_obj_get_child(btn_wifi, 0);
                lbl_wifi_address = lv_label_create(btn_wifi);
                lv_obj_set_style_text_font(lbl_wifi_address, &lv_font_montserrat_20, 0);
                lv_obj_set_style_text_align(lbl_wifi_address, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(lbl_wifi_address, lv_pct(100));
                lv_label_set_long_mode(lbl_wifi_address, LV_LABEL_LONG_CLIP);
                lv_label_set_text(lbl_wifi_address, "");
                lv_obj_add_flag(lbl_wifi_address, LV_OBJ_FLAG_HIDDEN);
            }
        }
        wifi_status_timer = lv_timer_create(wifi_status_timer_cb_, 1000, NULL);
        if (wifi_status_timer) lv_timer_pause(wifi_status_timer);
        s_theme_rev = ui_theme_get_revision();
    }
    wifi_btn_refresh_();
    lv_scr_load(scr_main);
    ESP_LOGI(TAG, "Main menu loaded");
}
void screen_ui_show(void)
{
    telnet_router_set_active(TELNET_ROUTE_NONE);
    if (screen_alive(scr_main) && s_theme_rev != ui_theme_get_revision()) {
        screen_main_destroy_();
    }
    if (!screen_alive(scr_main)) {
        screen_ui_init();
    }
    wifi_btn_refresh_();
    lv_scr_load(scr_main);
}
