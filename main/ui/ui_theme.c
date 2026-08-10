/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared UI theme palette, persistence and helpers.
 */

#include "ui/ui_theme.h"
#include "ui/ui_colors.h"
#include "system/nvs_rw.h"

#define UI_THEME_NAMESPACE "ui"
#define UI_THEME_KEY       "theme"

static const ui_theme_palette_t s_palettes[UI_THEME_COUNT] = {
    [UI_THEME_METAL] = {
        .bg = UI_THEME_METAL_BG_HEX,
        .bg_grad = UI_THEME_METAL_BG_GRAD_HEX,
        .card = UI_THEME_METAL_CARD_HEX,
        .panel_bg = UI_THEME_METAL_PANEL_BG_HEX,
        .log_bg = UI_THEME_METAL_LOG_BG_HEX,
        .refresh = UI_THEME_METAL_REFRESH_HEX,
        .refresh_alert = UI_THEME_METAL_REFRESH_ALERT_HEX,
        .ais = UI_THEME_METAL_AIS_HEX,
        .group_active = UI_THEME_METAL_GROUP_ACTIVE_HEX,
        .nav_home = UI_THEME_METAL_NAV_HOME_HEX,
        .nav_settings = UI_THEME_METAL_NAV_SETTINGS_HEX,
        .can_bridge = UI_THEME_METAL_CAN_BRIDGE_HEX,
        .can_nm2k = UI_THEME_METAL_CAN_NM2K_HEX,
        .can_home = UI_THEME_METAL_CAN_HOME_HEX,
        .editor_tab_active = UI_THEME_METAL_EDITOR_TAB_ACTIVE_HEX,
        .bit_border = UI_THEME_METAL_BIT_BORDER_HEX,
        .bit_on = UI_THEME_METAL_BIT_ON_HEX,
        .bit_off = UI_THEME_METAL_BIT_OFF_HEX,
        .bit_idle = UI_THEME_METAL_BIT_IDLE_HEX,
        .border = UI_THEME_METAL_BORDER_HEX,
        .text = UI_THEME_METAL_TEXT_HEX,
        .title = UI_THEME_METAL_TITLE_HEX,
        .muted = UI_THEME_METAL_MUTED_HEX,
        .button = UI_THEME_METAL_BUTTON_HEX,
        .dropdown_bg = UI_THEME_METAL_DD_BG_HEX,
        .dropdown_text = UI_THEME_METAL_DD_TXT_HEX,
        .pause = UI_THEME_METAL_PAUSE_HEX,
        .pause_active = UI_THEME_METAL_PAUSE_ACTIVE_HEX,
    },
    [UI_THEME_COLOR] = {
        .bg = UI_THEME_COLOR_BG_HEX,
        .bg_grad = UI_THEME_COLOR_BG_GRAD_HEX,
        .card = UI_THEME_COLOR_CARD_HEX,
        .panel_bg = UI_THEME_COLOR_PANEL_BG_HEX,
        .log_bg = UI_THEME_COLOR_LOG_BG_HEX,
        .refresh = UI_THEME_COLOR_REFRESH_HEX,
        .refresh_alert = UI_THEME_COLOR_REFRESH_ALERT_HEX,
        .ais = UI_THEME_COLOR_AIS_HEX,
        .group_active = UI_THEME_COLOR_GROUP_ACTIVE_HEX,
        .nav_home = UI_THEME_COLOR_NAV_HOME_HEX,
        .nav_settings = UI_THEME_COLOR_NAV_SETTINGS_HEX,
        .can_bridge = UI_THEME_COLOR_CAN_BRIDGE_HEX,
        .can_nm2k = UI_THEME_COLOR_CAN_NM2K_HEX,
        .can_home = UI_THEME_COLOR_CAN_HOME_HEX,
        .editor_tab_active = UI_THEME_COLOR_EDITOR_TAB_ACTIVE_HEX,
        .bit_border = UI_THEME_COLOR_BIT_BORDER_HEX,
        .bit_on = UI_THEME_COLOR_BIT_ON_HEX,
        .bit_off = UI_THEME_COLOR_BIT_OFF_HEX,
        .bit_idle = UI_THEME_COLOR_BIT_IDLE_HEX,
        .border = UI_THEME_COLOR_BORDER_HEX,
        .text = UI_THEME_COLOR_TEXT_HEX,
        .title = UI_THEME_COLOR_TITLE_HEX,
        .muted = UI_THEME_COLOR_MUTED_HEX,
        .button = UI_THEME_COLOR_BUTTON_HEX,
        .dropdown_bg = UI_THEME_COLOR_DD_BG_HEX,
        .dropdown_text = UI_THEME_COLOR_DD_TXT_HEX,
        .pause = UI_THEME_COLOR_PAUSE_HEX,
        .pause_active = UI_THEME_COLOR_PAUSE_ACTIVE_HEX,
    },
    [UI_THEME_BW] = {
        .bg = UI_THEME_BW_BG_HEX,
        .bg_grad = UI_THEME_BW_BG_GRAD_HEX,
        .card = UI_THEME_BW_CARD_HEX,
        .panel_bg = UI_THEME_BW_PANEL_BG_HEX,
        .log_bg = UI_THEME_BW_LOG_BG_HEX,
        .refresh = UI_THEME_BW_REFRESH_HEX,
        .refresh_alert = UI_THEME_BW_REFRESH_ALERT_HEX,
        .ais = UI_THEME_BW_AIS_HEX,
        .group_active = UI_THEME_BW_GROUP_ACTIVE_HEX,
        .nav_home = UI_THEME_BW_NAV_HOME_HEX,
        .nav_settings = UI_THEME_BW_NAV_SETTINGS_HEX,
        .can_bridge = UI_THEME_BW_CAN_BRIDGE_HEX,
        .can_nm2k = UI_THEME_BW_CAN_NM2K_HEX,
        .can_home = UI_THEME_BW_CAN_HOME_HEX,
        .editor_tab_active = UI_THEME_BW_EDITOR_TAB_ACTIVE_HEX,
        .bit_border = UI_THEME_BW_BIT_BORDER_HEX,
        .bit_on = UI_THEME_BW_BIT_ON_HEX,
        .bit_off = UI_THEME_BW_BIT_OFF_HEX,
        .bit_idle = UI_THEME_BW_BIT_IDLE_HEX,
        .border = UI_THEME_BW_BORDER_HEX,
        .text = UI_THEME_BW_TEXT_HEX,
        .title = UI_THEME_BW_TITLE_HEX,
        .muted = UI_THEME_BW_MUTED_HEX,
        .button = UI_THEME_BW_BUTTON_HEX,
        .dropdown_bg = UI_THEME_BW_DD_BG_HEX,
        .dropdown_text = UI_THEME_BW_DD_TXT_HEX,
        .pause = UI_THEME_BW_PAUSE_HEX,
        .pause_active = UI_THEME_BW_PAUSE_ACTIVE_HEX,
    },
};

static ui_theme_id_t s_theme = UI_THEME_METAL;
static uint32_t s_theme_revision = 1;

static bool ui_theme_valid_(ui_theme_id_t id)
{
    return id >= 0 && id < UI_THEME_COUNT;
}

esp_err_t ui_theme_init(void)
{
    void *handle;
    uint8_t raw = (uint8_t)UI_THEME_METAL;
    esp_err_t err = nvs_rw_open_ro(UI_THEME_NAMESPACE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_theme = UI_THEME_METAL;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    err = nvs_rw_read_u8(handle, UI_THEME_KEY, &raw);
    nvs_rw_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_theme = UI_THEME_METAL;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    s_theme = ui_theme_valid_((ui_theme_id_t)raw) ? (ui_theme_id_t)raw : UI_THEME_METAL;
    return ESP_OK;
}

ui_theme_id_t ui_theme_get_id(void)
{
    return s_theme;
}

uint32_t ui_theme_get_revision(void)
{
    return s_theme_revision;
}

const ui_theme_palette_t *ui_theme_get(void)
{
    return &s_palettes[s_theme];
}

esp_err_t ui_theme_set(ui_theme_id_t id, bool persist)
{
    if (!ui_theme_valid_(id)) return ESP_ERR_INVALID_ARG;
    if (s_theme != id) {
        s_theme = id;
        s_theme_revision++;
    }
    if (!persist) return ESP_OK;

    void *handle;
    esp_err_t err = nvs_rw_open_rw(UI_THEME_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_rw_write_u8(handle, UI_THEME_KEY, (uint8_t)id);
    nvs_rw_close(handle);
    return err;
}

const char *ui_theme_options(void)
{
    return "Metal\nColor\nB/W";
}

uint32_t ui_theme_accent_hex(uint32_t color_hex)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? color_hex : ui_theme_get()->button;
}

uint32_t ui_theme_form_text_hex(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_COLOR_TEXT_BLACK_HEX : ui_theme_get()->text;
}

uint32_t ui_theme_editor_bg_hex(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_EDITOR_PARENT_HEX : ui_theme_get()->bg;
}

uint32_t ui_theme_editor_content_hex(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_EDITOR_PARENT_HEX : ui_theme_get()->card;
}

uint32_t ui_theme_refresh_hex(bool alert)
{
    const ui_theme_palette_t *th = ui_theme_get();
    return alert ? th->refresh_alert : th->refresh;
}

uint32_t ui_theme_ais_hex(void)
{
    return ui_theme_get()->ais;
}

uint32_t ui_theme_group_active_hex(void)
{
    return ui_theme_get()->group_active;
}

uint32_t ui_theme_nav_home_hex(void)
{
    return ui_theme_get()->nav_home;
}

uint32_t ui_theme_nav_settings_hex(void)
{
    return ui_theme_get()->nav_settings;
}

uint32_t ui_theme_can_bridge_hex(void)
{
    return ui_theme_get()->can_bridge;
}

uint32_t ui_theme_can_nm2k_hex(void)
{
    return ui_theme_get()->can_nm2k;
}

uint32_t ui_theme_can_home_hex(void)
{
    return ui_theme_get()->can_home;
}

uint32_t ui_theme_editor_tab_active_hex(void)
{
    return ui_theme_get()->editor_tab_active;
}

uint32_t ui_theme_pause_hex(bool active)
{
    const ui_theme_palette_t *th = ui_theme_get();
    return active ? th->pause_active : th->pause;
}

void ui_theme_apply_screen(lv_obj_t *obj)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(th->bg), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(th->bg_grad), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(obj, 0, 0);
    lv_obj_set_style_bg_grad_stop(obj, 255, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void ui_theme_apply_card(lv_obj_t *obj)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(th->card), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(th->border), 0);
}

void ui_theme_apply_title(lv_obj_t *obj, const lv_font_t *font)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(th->title), 0);
    if (font) lv_obj_set_style_text_font(obj, font, 0);
}

void ui_theme_apply_text(lv_obj_t *obj, const lv_font_t *font)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(th->text), 0);
    if (font) lv_obj_set_style_text_font(obj, font, 0);
}

void ui_theme_apply_muted(lv_obj_t *obj, const lv_font_t *font)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!obj) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(th->muted), 0);
    if (font) lv_obj_set_style_text_font(obj, font, 0);
}

void ui_theme_apply_dropdown(lv_obj_t *dd, const lv_font_t *font)
{
    const ui_theme_palette_t *th = ui_theme_get();
    lv_obj_t *list;

    if (!dd) return;
    lv_obj_set_style_bg_color(dd, lv_color_hex(th->dropdown_bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(dd, lv_color_hex(th->dropdown_text), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dd, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(dd, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (font) lv_obj_set_style_text_font(dd, font, LV_PART_MAIN | LV_STATE_DEFAULT);

    list = lv_dropdown_get_list(dd);
    if (!list) return;
    lv_obj_set_style_bg_color(list, lv_color_hex(th->dropdown_bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(list, lv_color_hex(th->dropdown_text), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(list, lv_color_hex(th->card), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(list, lv_color_hex(th->text), LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (font) {
        lv_obj_set_style_text_font(list, font, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(list, font, LV_PART_SELECTED | LV_STATE_DEFAULT);
    }
}
