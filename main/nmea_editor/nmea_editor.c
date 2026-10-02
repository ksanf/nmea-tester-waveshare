/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/nmea_editor.h"
#include "ui/dialog_ui.h"
#include "rs485/rs485_simui.h"
#include "nmea_editor/numeric_editor.h" 
#include "nmea_editor/nmea_tabs_gps.h"
#include "nmea_editor/nmea_tabs_gyro.h"
#include "nmea_editor/nmea_tabs_log.h"
#include "nmea_editor/nmea_tabs_echo.h"
#include "nmea_editor/nmea_tabs_weather.h"
#include "nmea_editor/nmea_templates.h"

#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "ui/screens/screen_ui.h"
#include "lvgl.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NMEA_EDITOR
#include "config_logs.h"
#include <stdint.h>
#include <string.h>

/* ---------- Geometry ---------- */
#define TAB_BTN_W   66
#define TAB_BTN_H   56
#define GAP          5
#define PADDING      5
#define TABBAR_W   (TAB_BTN_W + PADDING * 2)

/* ---------- Tab table ---------- */
enum { IDX_GPS, IDX_GYRO, IDX_LOG, IDX_ECHO, IDX_WX, IDX_COUNT };
typedef void (*tab_fn)(lv_obj_t *);

/* ---------- Internal state ---------- */
static lv_obj_t *editor_scr  = NULL;
static lv_obj_t *content     = NULL;
static lv_obj_t *tab_btns[IDX_COUNT] = {0};
static int       current_tab = -1;
static uint32_t  s_theme_rev = 0;
static int s_suspended_tab = -1;

static const struct {
    const char *name;
    uint32_t color;
    tab_fn create;
} TABS[IDX_COUNT] = {
    { "GPS",  UI_GROUP_GPS_HEX,  gps_tab_create    },
    { "GYRO", UI_GROUP_GYRO_HEX, gyro_tab_create   },
    { "LOG",  UI_GROUP_LOG_HEX,  log_tab_create    },
    { "ECHO", UI_GROUP_ECHO_HEX, echo_tab_create   },
    { "WX",   UI_GROUP_WX_HEX,   weather_tab_create}
};

/* ---------- Fonts ---------- */
LV_FONT_DECLARE(lv_font_montserrat_20)
LV_FONT_DECLARE(lv_font_montserrat_28)

/* ---------- Prototypes ---------- */
static void switch_tab(int idx);
static void cb_tab_btn(lv_event_t *e);
static void cb_back_btn(lv_event_t *e);
static void reset_state(void);
static uint32_t editor_tab_color_(int idx);
static void editor_apply_tab_checked_style_(lv_obj_t *btn);
static void editor_delete_cb_(lv_event_t *e);
static void editor_destroy_(void);
static lv_obj_t *editor_build_tabbar_(void);
static void editor_build_content_(void);
static void editor_update_tab_state_(int idx);

/* =============================================================== */
void nmea_editor_create(void)
{
    if (editor_scr && s_theme_rev != ui_theme_get_revision()) {
        editor_destroy_();
    }
    if (editor_scr) {              /* Reuse the existing screen. */
        nmea_editor_suspend(false);
        lv_scr_load(editor_scr);
        return;
    }

    /* Reset global state before creating the screen. */
    reset_state();

    /* Root screen container. */
    editor_scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(editor_scr, editor_delete_cb_, LV_EVENT_DELETE, NULL);
    lv_obj_clear_flag(editor_scr, LV_OBJ_FLAG_SCROLLABLE);
    dialog_ui_apply_screen_bg(editor_scr);

    editor_build_tabbar_();
    editor_build_content_();

    /* Initial tab. */
    switch_tab(IDX_GPS);
    s_theme_rev = ui_theme_get_revision();

    lv_scr_load(editor_scr);
}

static uint32_t editor_tab_color_(int idx)
{
    return ui_theme_accent_hex(TABS[idx].color);
}

static void editor_apply_tab_checked_style_(lv_obj_t *btn)
{
    if (!btn) return;
    dialog_ui_apply_checked_style(btn, ui_theme_editor_tab_active_hex());
}

/* ---------- callbacks ---------- */
static void cb_tab_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_tab(idx);
}

static void cb_back_btn(lv_event_t *e)
{
    esp_err_t err;

    err = nmea_templates_save_now();
    if (err != ESP_OK) {
        ESP_LOGW("nmea_editor", "template save failed: %s", esp_err_to_name(err));
    }

    if (rs485_simui_get_screen()) lv_scr_load(rs485_simui_get_screen());
    editor_destroy_();
}

/* ---------- Tab switching ---------- */
static void switch_tab(int idx)
{
    if (idx < 0 || idx >= IDX_COUNT || !content) return;
    
    /* Keep a populated tab when it is selected again. */
    if (idx == current_tab && lv_obj_get_child_cnt(content) > 0) return;

    if (current_tab == IDX_GPS && idx != IDX_GPS) num_edit_cancel();

    editor_update_tab_state_(idx);

    lv_obj_clean(content);
    TABS[idx].create(content);

    current_tab = idx;
    ESP_LOGI("nmea_editor", "Tab -> %s", TABS[idx].name);
}

/* ---------- Global-state reset ---------- */
static void reset_state(void)
{
    num_edit_cancel();  /* Clear the keyboard and numeric-editor state. */

    current_tab = -1;
    s_suspended_tab = -1;
    editor_scr  = NULL;
    content     = NULL;
    memset(tab_btns, 0, sizeof(tab_btns));
}

static void editor_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (editor_scr && editor_scr != deleted) return;
    reset_state();
}

static void editor_destroy_(void)
{
    lv_obj_t *old = editor_scr;

    if (!old || !lv_obj_is_valid(old)) {
        reset_state();
        return;
    }
    reset_state();
    lv_obj_del_async(old);
}

static lv_obj_t *editor_build_tabbar_(void)
{
    lv_obj_t *tabbar = lv_obj_create(editor_scr);
    lv_obj_set_size(tabbar, TABBAR_W, LV_VER_RES);
    lv_obj_align(tabbar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(tabbar, PADDING, 0);
    lv_obj_set_style_pad_row(tabbar, GAP, 0);
    lv_obj_set_flex_flow(tabbar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tabbar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tabbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(tabbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(tabbar, lv_color_hex(ui_theme_get()->border), 0);

    for (int i = 0; i < IDX_COUNT; ++i) {
        tab_btns[i] = dialog_ui_create_button(tabbar, 0, 0,
                     TAB_BTN_W, TAB_BTN_H,
                     TABS[i].name, editor_tab_color_(i),
                     cb_tab_btn, (void*)(intptr_t)i,
                     &lv_font_montserrat_20);
        editor_apply_tab_checked_style_(tab_btns[i]);
    }

    dialog_ui_create_button(tabbar, 0, 0,
        TAB_BTN_W, TAB_BTN_H, LV_SYMBOL_LEFT,
        ui_theme_nav_home_hex(),
        cb_back_btn, NULL,
        &lv_font_montserrat_28);

    return tabbar;
}

static void editor_build_content_(void)
{
    content = lv_obj_create(editor_scr);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, TABBAR_W, 0);
    lv_obj_set_size(content, LV_HOR_RES - TABBAR_W, LV_VER_RES);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(content, lv_color_hex(ui_theme_get()->border), 0);
}

static void editor_update_tab_state_(int idx)
{
    for (int i = 0; i < IDX_COUNT; ++i) {
        if (tab_btns[i]) lv_obj_clear_state(tab_btns[i], LV_STATE_CHECKED);
    }
    if (tab_btns[idx]) lv_obj_add_state(tab_btns[idx], LV_STATE_CHECKED);
}

void nmea_editor_suspend(bool suspended)
{
    if (!editor_scr || !content) return;
    if (suspended) {
        if (s_suspended_tab >= 0) return;
        s_suspended_tab = current_tab >= 0 ? current_tab : IDX_GPS;
        (void)num_edit_finish();
        /* Deleting the tab's labels releases every cursor/blink timer, even
         * those belonging to a previously focused numeric field. */
        lv_obj_clean(content);
        current_tab = -1;
    } else if (s_suspended_tab >= 0) {
        const int tab = s_suspended_tab;
        s_suspended_tab = -1;
        switch_tab(tab);
    }
}
