/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/dialog_ui.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "lvgl.h"

/*─────────────────────────────────────────────────────────*/
/*  Constants shared by all button styles.                 */
/*─────────────────────────────────────────────────────────*/
#define BTN_RADIUS_PX          6           /* Corner radius                 */
#define BTN_BORDER_PX          2           /* Border width                  */
#define BTN_PRESSED_DARKEN_DELTA_HEX 0x22

/* Lighten or darken every RGB component by delta. */
#define COLOR_DARKEN(hex,delta)  ( ((hex) > (delta))      ? ((hex) - (delta))      : 0 )
#define DARKEN24(hex,delta)      ( COLOR_DARKEN((hex) & 0xFF0000, (delta)<<16) |   \
                                   COLOR_DARKEN((hex) & 0x00FF00, (delta)<<8 ) |   \
                                   COLOR_DARKEN((hex) & 0x0000FF,  delta) )

/*─────────────────────────────────────────────────────────*/
static lv_color_t btn_text_color_(uint32_t bg_hex)
{
    uint8_t r = (uint8_t)((bg_hex >> 16) & 0xFF);
    uint8_t g = (uint8_t)((bg_hex >> 8) & 0xFF);
    uint8_t b = (uint8_t)(bg_hex & 0xFF);
    uint32_t luma = (uint32_t)r * 299u + (uint32_t)g * 587u + (uint32_t)b * 114u;
    return lv_color_hex((luma >= 128000u) ? UI_COLOR_BLACK_HEX : UI_COLOR_WHITE_HEX);
}

static void btn_apply_checked_style_(lv_obj_t *btn, uint32_t bg_hex)
{
    lv_color_t txt;

    if (!btn) return;
    txt = btn_text_color_(bg_hex);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, txt, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btn, txt, LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
}

/*─────────────────────────────────────────────────────────*/
/*  Public API                                             */
/*─────────────────────────────────────────────────────────*/

void dialog_ui_apply_screen_bg(lv_obj_t *screen)
{
    const ui_theme_palette_t *th = ui_theme_get();
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, lv_color_hex(th->bg), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(th->bg_grad), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(screen, 0, 0);
    lv_obj_set_style_bg_grad_stop(screen, 255, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

lv_obj_t *dialog_ui_create_button(lv_obj_t *parent,
                                  int16_t   x,
                                  int16_t   y,
                                  int16_t   w,
                                  int16_t   h,
                                  const char *txt,
                                  uint32_t  bg_hex,
                                  lv_event_cb_t cb,
                                  void      *user_data,
                                  const lv_font_t *font)
{
    /* 1. Create the LVGL button. */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_color_t text_color = btn_text_color_(bg_hex);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos (btn, x, y);
	
    /* 2. Apply LVGL properties directly without heap-allocated styles. */
    lv_obj_set_style_radius(btn, BTN_RADIUS_PX, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_DIALOG_BORDER_HEX), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, BTN_BORDER_PX, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, text_color, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_radius(btn, BTN_RADIUS_PX, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(DARKEN24(bg_hex, BTN_PRESSED_DARKEN_DELTA_HEX)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_DIALOG_BORDER_PRESSED_HEX),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, BTN_BORDER_PX, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, text_color, LV_PART_MAIN | LV_STATE_PRESSED);
	
    /* 3. Register the callback. */
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    /* 4. Create the embedded label. */
    lv_obj_t *lbl = lv_label_create(btn);
    if (font) lv_obj_set_style_text_font(lbl,font,0);
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
     
    return btn;
}

void dialog_ui_apply_checked_style(lv_obj_t *btn, uint32_t bg_hex)
{
    btn_apply_checked_style_(btn, bg_hex);
}
