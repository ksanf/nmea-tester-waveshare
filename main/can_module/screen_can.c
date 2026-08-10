/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   CAN submodule selection screen: Bridge / NM2K with a Home button.
 */

#include "config_nmea_tester.h"
#include "screen_can.h"
#include "dialog_ui.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "screen_ui.h"
#include "bridge_can_module.h"
#include "nm2k_module.h"
#include "screen_ui.h"
#include "system/telnet_router.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_SCREEN_CAN
#include "config_logs.h"
#include "lvgl.h"
#include <stdlib.h>

// Logging tag
static const char *TAG = "screen_can";

LV_FONT_DECLARE(lv_font_montserrat_28)

/* Module selection buttons */
#define BTN_W       240
#define BTN_H        96
#define GAP_Y        36
#define CAN_BTN_FONT &lv_font_montserrat_28

/* Home button */
#define CAN_HOME_SZ  66
#define CAN_HOME_FONT &lv_font_montserrat_28
// Screen
static lv_obj_t *scr = NULL;
static uint32_t s_theme_rev = 0;

static inline bool screen_alive_(lv_obj_t *obj)
{
    return obj && lv_obj_is_valid(obj) && lv_obj_get_parent(obj) == NULL;
}

static void screen_can_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr && scr != deleted) return;
    scr = NULL;
}

static void screen_can_destroy_(void)
{
    if (!screen_alive_(scr)) return;
    lv_obj_del_async(scr);
    scr = NULL;
}

/**
 * @brief Bridge CAN button callback
 */
static void on_btn_bridge(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Bridge CAN selected");
    bridge_can_module_start(NULL);
    screen_can_destroy_();
}

/**
 * @brief NM2K button callback
 */
static void on_btn_nm2k(lv_event_t *e)
{
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    ESP_LOGI(TAG, "NM2K selected");
    nm2k_module_start(NULL);
    screen_can_destroy_();
}

/**
 * @brief Home button callback
 */
static void on_btn_home(lv_event_t *e) {
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    ESP_LOGI(TAG, "Returning to main menu");
    screen_ui_show();
    screen_can_destroy_();
}

/**
 * @brief Show the CAN submodule selection screen
 * @param parent Parent LVGL object (main menu)
 * @return Pointer to the created LVGL screen
 */
lv_obj_t *screen_can_show(lv_obj_t *parent) {
    telnet_router_set_active(TELNET_ROUTE_NONE);
    ESP_LOGI(TAG, "Showing CAN screen, version 1.2.1-prod");

    (void)parent;

    if (screen_alive_(scr) && s_theme_rev != ui_theme_get_revision()) {
        screen_can_destroy_();
    }

    if (screen_alive_(scr)) {
        lv_scr_load(scr);
        return scr;
    }

    scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, screen_can_delete_cb_, LV_EVENT_DELETE, NULL);
    dialog_ui_apply_screen_bg(scr);
    s_theme_rev = ui_theme_get_revision();

    int screen_w = LCD_WIDTH;
    int y_start = (LCD_HEIGHT - 2 * BTN_H - GAP_Y) / 2;

    // Bridge CAN button
    dialog_ui_create_button(scr,
                            (screen_w - BTN_W) / 2,
                            y_start,
                            BTN_W, BTN_H,
                            "Bridge CAN",
                            ui_theme_can_bridge_hex(),
                            on_btn_bridge,
                            NULL, CAN_BTN_FONT);

    // NM2K button
    dialog_ui_create_button(scr,
                            (screen_w - BTN_W) / 2,
                            y_start + BTN_H + GAP_Y,
                            BTN_W, BTN_H,
                            "NM2K",
                            ui_theme_can_nm2k_hex(),
                            on_btn_nm2k,
                            NULL, CAN_BTN_FONT);

    // Home button
    dialog_ui_create_button(scr,
                            screen_w - CAN_HOME_SZ - 8,
                            4,
                            CAN_HOME_SZ, CAN_HOME_SZ,
                            LV_SYMBOL_HOME,
                            ui_theme_can_home_hex(),
                            on_btn_home,
                            NULL, CAN_HOME_FONT);

    lv_scr_load(scr);
    return scr;
}
