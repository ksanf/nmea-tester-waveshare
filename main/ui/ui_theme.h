/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared UI theme palette, persistence and helpers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

typedef enum {
    UI_THEME_METAL = 0,
    UI_THEME_COLOR = 1,
    UI_THEME_BW = 2,
    UI_THEME_COUNT
} ui_theme_id_t;

typedef struct {
    uint32_t bg;
    uint32_t bg_grad;
    uint32_t card;
    uint32_t panel_bg;
    uint32_t log_bg;
    uint32_t refresh;
    uint32_t refresh_alert;
    uint32_t ais;
    uint32_t group_active;
    uint32_t nav_home;
    uint32_t nav_settings;
    uint32_t can_bridge;
    uint32_t can_nm2k;
    uint32_t can_home;
    uint32_t editor_tab_active;
    uint32_t bit_border;
    uint32_t bit_on;
    uint32_t bit_off;
    uint32_t bit_idle;
    uint32_t border;
    uint32_t text;
    uint32_t title;
    uint32_t muted;
    uint32_t button;
    uint32_t dropdown_bg;
    uint32_t dropdown_text;
    uint32_t pause;
    uint32_t pause_active;
} ui_theme_palette_t;

esp_err_t ui_theme_init(void);
ui_theme_id_t ui_theme_get_id(void);
uint32_t ui_theme_get_revision(void);
const ui_theme_palette_t *ui_theme_get(void);
esp_err_t ui_theme_set(ui_theme_id_t id, bool persist);
const char *ui_theme_options(void);

uint32_t ui_theme_accent_hex(uint32_t color_hex);
uint32_t ui_theme_form_text_hex(void);
uint32_t ui_theme_editor_bg_hex(void);
uint32_t ui_theme_editor_content_hex(void);
uint32_t ui_theme_refresh_hex(bool alert);
uint32_t ui_theme_ais_hex(void);
uint32_t ui_theme_group_active_hex(void);
uint32_t ui_theme_nav_home_hex(void);
uint32_t ui_theme_nav_settings_hex(void);
uint32_t ui_theme_can_bridge_hex(void);
uint32_t ui_theme_can_nm2k_hex(void);
uint32_t ui_theme_can_home_hex(void);
uint32_t ui_theme_editor_tab_active_hex(void);
uint32_t ui_theme_pause_hex(bool active);

void ui_theme_apply_screen(lv_obj_t *obj);
void ui_theme_apply_card(lv_obj_t *obj);
void ui_theme_apply_title(lv_obj_t *obj, const lv_font_t *font);
void ui_theme_apply_text(lv_obj_t *obj, const lv_font_t *font);
void ui_theme_apply_muted(lv_obj_t *obj, const lv_font_t *font);
void ui_theme_apply_dropdown(lv_obj_t *dd, const lv_font_t *font);
