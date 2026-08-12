/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/screens/screen_settings.h"
#include "ui/dialog_ui.h"
#include "ui/ui_theme.h"
#include "ui/screens/screen_ui.h"               // For screen_main_show()
#include "ui/screens/screen_init.h"
#include "system/telnet_router.h"
#include "ui/screens/screen_wifi.h"
#include "config/config_nmea_tester.h"
#include "nmea_editor/nmea_version.h"

#include <esp_app_desc.h>
#include <esp_idf_version.h>
LV_FONT_DECLARE(lv_font_montserrat_22)
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_SCREEN_SETTINGS
#include "config_logs.h"
#include "lvgl.h"

//------------------------------------------------------------------------------
// Style and layout constants
//------------------------------------------------------------------------------
#define BTN_W                  320
#define BTN_H                  66
#define BTN_SPACE              18
#define FONT_TITLE             &lv_font_montserrat_28  // Title font
#define FONT_BTN               &lv_font_montserrat_22  // Button label font
#define FONT_INFO              &lv_font_montserrat_18
#define ABOUT_FRAME_MARGIN     12
#define NAV_BTN_SZ             66
#define NAV_BTN_FONT           &lv_font_montserrat_28

// High-contrast button colors for the COLOR theme.
#define CLR_ABOUT              0xE63946   // Scarlet

static const char *TAG = "screen_settings";
static lv_obj_t *scr_settings = NULL;
static lv_obj_t *scr_about = NULL;
static lv_obj_t *dd_theme = NULL;
static lv_obj_t *dd_nmea_version = NULL;
static lv_obj_t *dd_orientation = NULL;
static uint32_t s_theme_rev = 0;
static void screen_settings_delete_cb_(lv_event_t *e);
static void screen_about_delete_cb_(lv_event_t *e);
static void screen_settings_destroy_(void);
static void screen_about_destroy_(void);
static void theme_rebuild_async_(void *user);
//------------------------------------------------------------------------------
// Event handlers
//------------------------------------------------------------------------------
static inline bool screen_alive(lv_obj_t *scr)
{
    return scr && lv_obj_is_valid(scr) && lv_obj_get_parent(scr) == NULL;
}

static void screen_settings_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr_settings && scr_settings != deleted) return;
    scr_settings = NULL;
    dd_theme = NULL;
    dd_nmea_version = NULL;
    dd_orientation = NULL;
}

static void screen_about_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr_about && scr_about != deleted) return;
    scr_about = NULL;
}

static void screen_settings_destroy_(void)
{
    lv_obj_t *old = scr_settings;
    if (!screen_alive(old)) return;
    scr_settings = NULL;
    dd_theme = NULL;
    dd_nmea_version = NULL;
    dd_orientation = NULL;
    lv_obj_del_async(old);
}

static void screen_about_destroy_(void)
{
    lv_obj_t *old = scr_about;
    if (!screen_alive(old)) return;
    scr_about = NULL;
    lv_obj_del_async(old);
}

static void theme_rebuild_async_(void *user)
{
    lv_obj_t *old = scr_settings;
    (void)user;

    scr_settings = NULL;
    dd_theme = NULL;
    dd_nmea_version = NULL;
    dd_orientation = NULL;
    screen_settings_show(NULL);
    if (old && lv_obj_is_valid(old)) {
        lv_obj_del_async(old);
    }
}

static void wifi_settings_cb_(lv_event_t *e)
{
    (void)e;
    screen_wifi_show(NULL);
}

static void about_back_cb_(lv_event_t *e)
{
    (void)e;
    screen_about_destroy_();
    screen_settings_show(NULL);
}

static void on_about_pressed(lv_event_t *e)
{
    const ui_theme_palette_t *th = ui_theme_get();
    const esp_app_desc_t *app = esp_app_get_description();
    lv_coord_t hor;
    lv_coord_t ver;
    lv_obj_t *frame;
    lv_obj_t *title;
    lv_obj_t *info;
    static char info_buf[320];

    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);

    if (screen_alive(scr_about)) {
        lv_scr_load(scr_about);
        return;
    }

    hor = lv_disp_get_hor_res(NULL);
    ver = lv_disp_get_ver_res(NULL);

    scr_about = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_about, screen_about_delete_cb_, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(scr_about, hor, ver);
    ui_theme_apply_screen(scr_about);

    frame = lv_obj_create(scr_about);
    lv_obj_set_size(frame, hor - ABOUT_FRAME_MARGIN * 2, ver - ABOUT_FRAME_MARGIN * 2);
    lv_obj_align(frame, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(frame, 0, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(th->border), 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(scr_about);
    lv_label_set_text(title, "ABOUT");
    ui_theme_apply_title(title, FONT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    dialog_ui_create_button(scr_about, hor - NAV_BTN_SZ - 8, 3,
                            NAV_BTN_SZ, NAV_BTN_SZ, LV_SYMBOL_HOME,
                            ui_theme_get()->button, about_back_cb_,
                            NULL, NAV_BTN_FONT);

    snprintf(info_buf, sizeof(info_buf),
             "Device: NMEA Tester\n"
             "Version: %s\n"
             "Project: %s\n"
             "ESP-IDF: %s\n"
             "Author: S. Zhurba",
             app->version,
             app->project_name,
             esp_get_idf_version());

    info = lv_label_create(scr_about);
    lv_obj_set_width(info, hor - 64);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info, info_buf);
    ui_theme_apply_text(info, FONT_INFO);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 30, 78);

    lv_scr_load(scr_about);
}

static void settings_home_cb(lv_event_t *e) {
    (void)e;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    ui_theme_set(ui_theme_get_id(), true);  /* persist on exit */
    screen_settings_destroy_();
    screen_ui_show();
}

static void theme_changed_cb_(lv_event_t *e)
{
    uint16_t sel;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel == (uint16_t)ui_theme_get_id()) return;
    if (ui_theme_set((ui_theme_id_t)sel, false) != ESP_OK) return;  /* persist on exit */

    lv_async_call(theme_rebuild_async_, NULL);
}

static void nmea_version_changed_cb_(lv_event_t *e)
{
    uint16_t sel;
    esp_err_t err;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= NMEA_VERSION_COUNT) return;
    if (sel == (uint16_t)nmea_version_get()) return;

    err = nmea_version_set((nmea_version_t)sel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NMEA profile save failed: %s", esp_err_to_name(err));
        lv_dropdown_set_selected(lv_event_get_target(e),
                                 (uint16_t)nmea_version_get());
    }
}

static void orientation_changed_cb_(lv_event_t *e)
{
    lv_obj_t *dropdown;
    lv_indev_t *indev;
    uint16_t sel;
    bool rotated;
    esp_err_t err;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    dropdown = lv_event_get_target(e);
    sel = lv_dropdown_get_selected(dropdown);
    if (sel > 1U) return;

    rotated = sel == 1U;
    if (rotated == screen_orientation_is_rotated()) return;

    indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);

    err = screen_orientation_set_rotated(rotated, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Screen orientation save failed: %s", esp_err_to_name(err));
        lv_dropdown_set_selected(dropdown,
                                 screen_orientation_is_rotated() ? 1U : 0U);
    }
}

//------------------------------------------------------------------------------
// Display the Settings screen
//------------------------------------------------------------------------------
void screen_settings_show(lv_obj_t *parent) {
    lv_obj_t *old;
    (void)parent;
    telnet_router_set_active(TELNET_ROUTE_NONE);
    if (screen_alive(scr_settings) && s_theme_rev != ui_theme_get_revision()) {
        old = scr_settings;
        scr_settings = NULL;
        dd_theme = NULL;
        dd_nmea_version = NULL;
        dd_orientation = NULL;
        lv_obj_del(old);
    }
    if (screen_alive(scr_settings)) {
        lv_scr_load(scr_settings);
        return;
    }

    lv_coord_t hor = lv_disp_get_hor_res(NULL);
    lv_coord_t ver = lv_disp_get_ver_res(NULL);

    scr_settings = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_settings, screen_settings_delete_cb_, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(scr_settings, hor, ver);
    ui_theme_apply_screen(scr_settings);
    s_theme_rev = ui_theme_get_revision();

    // Title label
    lv_obj_t *title = lv_label_create(scr_settings);
    lv_label_set_text(title, "SETTINGS");
    ui_theme_apply_title(title, FONT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    // Home button uses the standard size and color shared by all screens.
    dialog_ui_create_button(scr_settings, hor - NAV_BTN_SZ - 8, 3,
                            NAV_BTN_SZ, NAV_BTN_SZ,
                            LV_SYMBOL_HOME, ui_theme_get()->button,
                            settings_home_cb, NULL, NAV_BTN_FONT);

    lv_obj_t *body = lv_obj_create(scr_settings);
    lv_obj_set_pos(body, 0, 74);
    lv_obj_set_size(body, hor, ver - 74);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    const bool two_columns = hor >= (BTN_W * 2 + BTN_SPACE * 3);
    const lv_coord_t left_x = two_columns
                            ? (hor - (BTN_W * 2 + BTN_SPACE)) / 2
                            : (hor - BTN_W) / 2;
    const lv_coord_t right_x = left_x + BTN_W + BTN_SPACE;
    const lv_coord_t theme_y = 12;
    const lv_coord_t nmea_y = theme_y + 96;
    const lv_coord_t orientation_y = nmea_y + 96;
    const lv_coord_t action_x = two_columns ? right_x : left_x;
    const lv_coord_t wifi_y = two_columns ? theme_y + 28
                                          : orientation_y + 96;
    const lv_coord_t about_y = wifi_y + BTN_H + BTN_SPACE;

    if (two_columns) lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // Theme selector
    {
        lv_obj_t *lbl_theme = lv_label_create(body);
        lv_label_set_text(lbl_theme, "Theme");
        ui_theme_apply_text(lbl_theme, &lv_font_montserrat_18);
        lv_obj_align(lbl_theme, LV_ALIGN_TOP_LEFT, left_x, theme_y);

        dd_theme = lv_dropdown_create(body);
        lv_dropdown_set_options(dd_theme, ui_theme_options());
        lv_dropdown_set_selected(dd_theme, (uint16_t)ui_theme_get_id());
        ui_theme_apply_dropdown(dd_theme, &lv_font_montserrat_22);
        lv_obj_set_size(dd_theme, BTN_W, 52);
        lv_obj_align(dd_theme, LV_ALIGN_TOP_LEFT, left_x, theme_y + 28);
        lv_obj_add_event_cb(dd_theme, theme_changed_cb_, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // NMEA 0183 transmitter format profile
    {
        lv_obj_t *lbl_nmea = lv_label_create(body);
        lv_label_set_text(lbl_nmea, "Transmitter NMEA version");
        ui_theme_apply_text(lbl_nmea, &lv_font_montserrat_18);
        lv_obj_align(lbl_nmea, LV_ALIGN_TOP_LEFT, left_x, nmea_y);

        dd_nmea_version = lv_dropdown_create(body);
        lv_dropdown_set_options(dd_nmea_version, nmea_version_options());
        lv_dropdown_set_selected(dd_nmea_version,
                                 (uint16_t)nmea_version_get());
        ui_theme_apply_dropdown(dd_nmea_version, &lv_font_montserrat_22);
        lv_obj_set_size(dd_nmea_version, BTN_W, 52);
        lv_obj_align(dd_nmea_version, LV_ALIGN_TOP_LEFT, left_x, nmea_y + 28);
        lv_obj_add_event_cb(dd_nmea_version, nmea_version_changed_cb_,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Display and touch orientation
    {
        lv_obj_t *lbl_orientation = lv_label_create(body);
        lv_label_set_text(lbl_orientation, "Display / touch orientation");
        ui_theme_apply_text(lbl_orientation, &lv_font_montserrat_18);
        lv_obj_align(lbl_orientation, LV_ALIGN_TOP_LEFT,
                     left_x, orientation_y);

        dd_orientation = lv_dropdown_create(body);
        lv_dropdown_set_options(dd_orientation,
                                "Normal (0 deg)\nRotated (180 deg)");
        lv_dropdown_set_selected(dd_orientation,
                                 screen_orientation_is_rotated() ? 1U : 0U);
        ui_theme_apply_dropdown(dd_orientation, &lv_font_montserrat_22);
        lv_obj_set_size(dd_orientation, BTN_W, 52);
        lv_obj_align(dd_orientation, LV_ALIGN_TOP_LEFT,
                     left_x, orientation_y + 28);
        lv_obj_add_event_cb(dd_orientation, orientation_changed_cb_,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    // WiFi
    dialog_ui_create_button(body, action_x, wifi_y,
        BTN_W, BTN_H,
        "  " LV_SYMBOL_WIFI "  WiFi Settings",
        ui_theme_accent_hex(0x457B9D),
        wifi_settings_cb_,
        NULL, FONT_BTN);

    // About
    dialog_ui_create_button(body, action_x, about_y,
        BTN_W, BTN_H,
        "  " LV_SYMBOL_EYE_OPEN "  About",
        ui_theme_accent_hex(CLR_ABOUT),
        on_about_pressed,
        NULL, FONT_BTN);
    lv_scr_load(scr_settings);
    ESP_LOGI(TAG, "Settings screen loaded");
}
