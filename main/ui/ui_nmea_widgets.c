/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/ui_nmea_widgets.h"
#include "nmea_editor/nmea_templates.h"
#include "ui/dialog_ui.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "config/config_nmea_tester.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lvgl.h"

/* ========================================================================= */
/*                              LOCAL MACROS                                 */
/* ========================================================================= */

/* Stepper acceleration ---------------------------------------------------- */
#define UI_STEPPER_ACCEL_FACTOR         10   /**< Step multiplier while held. */

/* Button and label geometry ----------------------------------------------- */
#define UI_BTN_SIZE_PX                  48   /**< Width and height of +/- buttons. */
#define UI_VALUE_OFFSET_X_PX            50   /**< Value-label offset from field X. */
#define UI_BTN_PLUS_OFFSET_X_PX        126   /**< Plus-button offset from field X. */
#define UI_VALUE_WIDTH_PX               72   /**< Value-label width. */
#define UI_VALUE_PAD_LEFT_PX             6   /**< Value-label left padding. */
/* ========================================================================= */
/*                             INTERNAL HELPERS                              */
/* ========================================================================= */

/* Apply shared long-press timing to every input device. ------------------- */
static void ensure_indev_repeat(void)
{
    static bool done = false;
    if(done) return;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        indev->driver->long_press_time = UI_LONG_PRESS_TIME_MS;
        indev->driver->long_press_repeat_time = UI_LONG_PRESS_REPEAT_MS;
        indev = lv_indev_get_next(indev);
    }
    done = true;
}

/* Vertical cursor relative to the current parent. ------------------------- */
static int cursor_y = 0;
static lv_obj_t *last_parent = NULL;
void ui_set_cursor_y(int new_y){ cursor_y = new_y; }

static inline void begin_parent(lv_obj_t *p)
{
    ensure_indev_repeat();
    if(p != last_parent){ cursor_y = 0; last_parent = p; }
}

static uint32_t ui_field_bg_(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_WIDGET_FIELD_BG_HEX : ui_theme_get()->card;
}

static uint32_t ui_text_hex_(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_COLOR_TEXT_BLACK_HEX : ui_theme_get()->text;
}

static void free_drop_ctx_cb_(lv_event_t *e)
{
    void *ctx = lv_event_get_user_data(e);
    free(ctx);
}

void ui_form_parent_apply(lv_obj_t *parent)
{
    if (!parent) return;
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(parent, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(parent, lv_color_hex(ui_theme_form_text_hex()), 0);
}

/* ========================================================================= */
/*                       ──   W I D G E T   F A C T O R Y   ──              */
/* ========================================================================= */
/* The builders below create an item, advance cursor_y, and return the item. */

/* ─────────────────────────  Text Field  ────────────────────────────────── */
lv_obj_t *ui_textfield_create(lv_obj_t *parent,const char *label,const char *def,
                              uint16_t w,uint16_t h)
{
    begin_parent(parent);

    /* Caption */
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab,label);
    lv_obj_set_pos(lab,0,cursor_y);
    lv_obj_set_width(lab,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab, lv_color_hex(ui_text_hex_()), 0);

    /* Input field */
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_text(ta,def?def:"");
    lv_obj_set_pos(ta,UI_FIELD_X_PX,cursor_y);
    lv_obj_set_size(ta,w,h);
    lv_obj_set_style_bg_color(ta,lv_color_hex(ui_field_bg_()),0);
    lv_obj_set_style_text_color(ta, lv_color_hex(ui_text_hex_()), 0);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return ta;
}

/* ─────────────────────────  Int Stepper  ──────────────────────────────── */
typedef struct { int32_t v,min,max,step; lv_obj_t *lbl; } int_ctx_t;

static void int_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;

    int8_t dir = (int8_t)(intptr_t)lv_event_get_user_data(e);
    int_ctx_t *c = lv_obj_get_user_data(lv_event_get_target(e));
    if (!c || !c->lbl) return;

    int32_t delta = c->step * dir;
    if(code == LV_EVENT_LONG_PRESSED_REPEAT) delta *= UI_STEPPER_ACCEL_FACTOR;

    int32_t nv = c->v + delta;
    if(nv < c->min) nv = c->min;
    if(nv > c->max) nv = c->max;
    c->v = nv;

    char b[16]; snprintf(b,sizeof b,"%ld",(long)c->v);
    lv_label_set_text(c->lbl,b);
     /* Trigger CLICKED so the external stepper callback stores the value. */
    lv_event_send(c->lbl, LV_EVENT_CLICKED, NULL);
}

/* Release an integer stepper context. */
static void free_int_ctx_cb(lv_event_t *e) {
    int_ctx_t *ctx = lv_obj_get_user_data(lv_event_get_target(e));
    if (ctx) free(ctx);
}

lv_obj_t *ui_int_stepper_create(lv_obj_t *p,const char *l,int32_t v,
                                int32_t mn,int32_t mx,int32_t st)
{
    begin_parent(p);

    /* Caption */
    lv_obj_t *lab = lv_label_create(p);
    lv_label_set_text(lab,l);
    lv_obj_set_pos(lab,0,cursor_y);
    lv_obj_set_width(lab,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab, lv_color_hex(ui_text_hex_()), 0);

    /* Minus button */
    lv_obj_t *btn_minus = dialog_ui_create_button(p,UI_FIELD_X_PX,cursor_y,
                                  UI_BTN_SIZE_PX,UI_BTN_SIZE_PX,"-",
                                  ui_field_bg_(),int_btn_cb,
                                  (void*)(intptr_t)-1,NULL);

    /* Value */
    lv_obj_t *lbl_val = lv_label_create(p);
    char b[16]; snprintf(b,sizeof b,"%ld",(long)v); lv_label_set_text(lbl_val,b);
    lv_obj_set_pos(lbl_val,UI_FIELD_X_PX + UI_VALUE_OFFSET_X_PX,cursor_y+4);
    lv_obj_set_width(lbl_val,UI_VALUE_WIDTH_PX);
    lv_obj_set_style_text_align(lbl_val,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(ui_text_hex_()), 0);

    /* Plus button */
    lv_obj_t *btn_plus = dialog_ui_create_button(p,UI_FIELD_X_PX + UI_BTN_PLUS_OFFSET_X_PX,
                                  cursor_y,UI_BTN_SIZE_PX,UI_BTN_SIZE_PX,"+",
                                  ui_field_bg_(),int_btn_cb,
                                  (void*)(intptr_t)+1,NULL);

    /* Context */
    int_ctx_t *ctx = malloc(sizeof *ctx);
    if (!ctx) {
        lv_obj_add_state(btn_minus, LV_STATE_DISABLED);
        lv_obj_add_state(btn_plus, LV_STATE_DISABLED);
        cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
        return lbl_val;
    }
    ctx->v = v; ctx->min = mn; ctx->max = mx; ctx->step = st; ctx->lbl = lbl_val;
    lv_obj_set_user_data(btn_minus,ctx);
    lv_obj_set_user_data(btn_plus ,ctx);

    /* Long-press handling */
    lv_obj_add_event_cb(btn_minus,int_btn_cb,LV_EVENT_LONG_PRESSED_REPEAT,
                        (void*)(intptr_t)-1);
    lv_obj_add_event_cb(btn_plus ,int_btn_cb,LV_EVENT_LONG_PRESSED_REPEAT,
                        (void*)(intptr_t)+1);

    /* Free the shared context from one button's delete event only. */
    lv_obj_add_event_cb(btn_plus, free_int_ctx_cb, LV_EVENT_DELETE, NULL);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return lbl_val;
}

/* ───────────────────────── Float Stepper ─────────────────────────────── */
typedef struct { float v, step; lv_obj_t *lbl; } float_ctx_t;

static void float_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;

    int8_t dir = (int8_t)(intptr_t)lv_event_get_user_data(e);
    float_ctx_t *c = lv_obj_get_user_data(lv_event_get_target(e));
    if (!c || !c->lbl) return;

    float delta = c->step * dir;
    if(code == LV_EVENT_LONG_PRESSED_REPEAT) delta *= (float)UI_STEPPER_ACCEL_FACTOR;

    c->v += delta;
    char b[16]; snprintf(b,sizeof b,"%.1f",c->v);
    lv_label_set_text(c->lbl,b);
    lv_event_send(c->lbl, LV_EVENT_CLICKED, NULL);  
}

/* Release a floating-point stepper context. */
static void free_float_ctx_cb(lv_event_t *e) {
    float_ctx_t *ctx = lv_obj_get_user_data(lv_event_get_target(e));
    if (ctx) free(ctx);
}

lv_obj_t *ui_float_stepper_create(lv_obj_t *p,const char *l,float v,float st)
{
    begin_parent(p);

    /* Caption */
    lv_obj_t *lab = lv_label_create(p);
    lv_label_set_text(lab,l);
    lv_obj_set_pos(lab,0,cursor_y);
    lv_obj_set_width(lab,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab, lv_color_hex(ui_text_hex_()), 0);

    /* Minus button */
    lv_obj_t *btn_minus = dialog_ui_create_button(p,UI_FIELD_X_PX,cursor_y,
                                  UI_BTN_SIZE_PX,UI_BTN_SIZE_PX,"-",
                                  ui_field_bg_(),float_btn_cb,
                                  (void*)(intptr_t)-1,NULL);

    /* Value */
    lv_obj_t *lbl_val = lv_label_create(p);
    char b[16]; snprintf(b,sizeof b,"%.1f",v); lv_label_set_text(lbl_val,b);
    lv_obj_set_pos(lbl_val,UI_FIELD_X_PX + UI_VALUE_OFFSET_X_PX,cursor_y+4);
    lv_obj_set_width(lbl_val,UI_VALUE_WIDTH_PX);
    lv_obj_set_style_text_align(lbl_val,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(ui_text_hex_()), 0);

    /* Plus button */
    lv_obj_t *btn_plus = dialog_ui_create_button(p,UI_FIELD_X_PX + UI_BTN_PLUS_OFFSET_X_PX,
                                  cursor_y,UI_BTN_SIZE_PX,UI_BTN_SIZE_PX,"+",
                                  ui_field_bg_(),float_btn_cb,
                                  (void*)(intptr_t)+1,NULL);

    /* Context */
    float_ctx_t *ctx = malloc(sizeof *ctx);
    if (!ctx) {
        lv_obj_add_state(btn_minus, LV_STATE_DISABLED);
        lv_obj_add_state(btn_plus, LV_STATE_DISABLED);
        cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
        return lbl_val;
    }
    ctx->v = v; ctx->step = st; ctx->lbl = lbl_val;
    lv_obj_set_user_data(btn_minus,ctx);
    lv_obj_set_user_data(btn_plus ,ctx);

    /* Long-press handling */
    lv_obj_add_event_cb(btn_minus,float_btn_cb,LV_EVENT_LONG_PRESSED_REPEAT,
                        (void*)(intptr_t)-1);
    lv_obj_add_event_cb(btn_plus ,float_btn_cb,LV_EVENT_LONG_PRESSED_REPEAT,
                        (void*)(intptr_t)+1);

    /* Free the shared context from one button's delete event only. */
    lv_obj_add_event_cb(btn_plus, free_float_ctx_cb, LV_EVENT_DELETE, NULL);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return lbl_val;
}

/* ───────────────────────── Dropdown ─────────────────────────────────── */
lv_obj_t *ui_dropdown_create(lv_obj_t *p,const char *l,const char **o,
                             uint8_t n,uint8_t sel)
{
    begin_parent(p);

    lv_obj_t *lab = lv_label_create(p);
    lv_label_set_text(lab,l);
    lv_obj_set_pos(lab,0,cursor_y);
    lv_obj_set_width(lab,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab, lv_color_hex(ui_text_hex_()), 0);

    lv_obj_t *dd = lv_dropdown_create(p);
    lv_obj_set_pos(dd,UI_FIELD_X_PX,cursor_y);
    lv_obj_set_width(dd,160);
    ui_theme_apply_dropdown(dd, &lv_font_montserrat_20);

    char buf[128] = {0};  // Compact buffer; the option count is small.
    size_t len = 0;
    for(uint8_t i=0; i<n; i++){
        if (len >= sizeof(buf) - 1u) break;
        int written = snprintf(buf + len, sizeof(buf) - len, "%s%s",
                               o[i] ? o[i] : "", (i + 1u < n) ? "\n" : "");
        if (written < 0) break;
        if ((size_t)written >= sizeof(buf) - len) {
            len = sizeof(buf) - 1u;
            break;
        }
        len += (size_t)written;
    }
    lv_dropdown_set_options(dd,buf);
    lv_dropdown_set_selected(dd,sel);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return dd;
}

/* ───────────────────────── Checkbox ─────────────────────────────────── */
lv_obj_t *ui_checkbox_create(lv_obj_t *p,const char *l,bool chk)
{
    begin_parent(p);
    lv_obj_t *cb = lv_checkbox_create(p);
    lv_checkbox_set_text(cb,l);
    if(chk) lv_obj_add_state(cb,LV_STATE_CHECKED);
    lv_obj_set_pos(cb,0,cursor_y);
    lv_obj_set_style_text_color(cb, lv_color_hex(ui_text_hex_()), 0);
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_22, 0);
    lv_obj_set_style_width(cb, 40, LV_PART_INDICATOR);
    lv_obj_set_style_height(cb, 40, LV_PART_INDICATOR);
    lv_obj_set_style_pad_column(cb, 12, 0);
    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return cb;
}

/* ─────────────────────── Value Label (readonly) ─────────────────────── */
lv_obj_t *ui_value_label_create(lv_obj_t *p,const char *title,const char *value,uint16_t w)
{
    begin_parent(p);

    /* Caption */
    lv_obj_t *lab_title = lv_label_create(p);
    lv_label_set_text(lab_title,title);
    lv_obj_set_pos(lab_title,0,cursor_y);
    lv_obj_set_width(lab_title,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab_title,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab_title, lv_color_hex(ui_text_hex_()), 0);

    /* Value */
    lv_obj_t *lab_val = lv_label_create(p);
    lv_label_set_text(lab_val,value?value:"");
    lv_obj_set_pos(lab_val,UI_FIELD_X_PX,cursor_y);
    lv_obj_set_size(lab_val,w,UI_ROW_H_PX);

    lv_obj_set_style_bg_color(lab_val,lv_color_hex(ui_field_bg_()),0);
    lv_obj_set_style_bg_opa  (lab_val,LV_OPA_COVER,0);
    lv_obj_set_style_text_color(lab_val, lv_color_hex(ui_text_hex_()), 0);
    lv_obj_add_flag(lab_val,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_pad_left(lab_val,UI_VALUE_PAD_LEFT_PX,0);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return lab_val;
}

/* ─────────────────── Lat / Lon (value + dropdown) ───────────────────── */
lv_obj_t *ui_latlon_create(lv_obj_t *p,const char *title,const char *value,
                           const char *dir,uint16_t w)
{
    begin_parent(p);

    /* Caption */
    lv_obj_t *lab_title = lv_label_create(p);
    lv_label_set_text(lab_title,title);
    lv_obj_set_pos(lab_title,0,cursor_y);
    lv_obj_set_width(lab_title,UI_LABEL_W_PX);
    lv_obj_set_style_text_align(lab_title,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_set_style_text_color(lab_title, lv_color_hex(ui_text_hex_()), 0);

    /* Value */
    lv_obj_t *lab_val = lv_label_create(p);
    lv_label_set_text(lab_val,value);
    lv_obj_set_pos(lab_val,UI_FIELD_X_PX,cursor_y);
    lv_obj_set_size(lab_val,w,UI_ROW_H_PX);

    lv_obj_set_style_bg_color(lab_val,lv_color_hex(ui_field_bg_()),0);
    lv_obj_set_style_bg_opa  (lab_val,LV_OPA_COVER,0);
    lv_obj_set_style_text_color(lab_val, lv_color_hex(ui_text_hex_()), 0);
    lv_obj_add_flag(lab_val,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_pad_left(lab_val,UI_VALUE_PAD_LEFT_PX,0);

    /* Direction dropdown with options derived from dir. */
    const char *dirs[2];
    uint8_t n_dirs = 2;
    uint8_t sel = 0;
    if (strstr(title, "Latitude")) {  // Latitude: N/S
        dirs[0] = "N";
        dirs[1] = "S";
        sel = (strcmp(dir, "S") == 0) ? 1 : 0;
    } else {  // Longitude: E/W
        dirs[0] = "E";
        dirs[1] = "W";
        sel = (strcmp(dir, "W") == 0) ? 1 : 0;
    }

    lv_obj_t *dd = ui_dropdown_create(p,"",dirs,n_dirs,sel);
    lv_obj_set_pos(dd,UI_FIELD_X_PX + w + 4,cursor_y - UI_ROW_H_PX - UI_ROW_GAP_PX);

    cursor_y += UI_ROW_H_PX + UI_ROW_GAP_PX;
    return lab_val; /* Used by num_edit_bind. */
}

/* Shared handler for all dropdowns. */
void ui_dropdown_generic_cb(lv_event_t *e)
{
    ui_drop_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx || !ctx->dst) return;

    if(ctx->is_str2) {               /* Two-letter talker ID */
        char tmp[4] = {0};           /* Spare byte plus NUL */
        lv_dropdown_get_selected_str(lv_event_get_target(e),
                                     tmp, sizeof(tmp));
        /* Copy at most two characters plus NUL. */
        strncpy((char*)ctx->dst, tmp, 2);
        ((char*)ctx->dst)[2] = '\0';
    } else {                         /* Single character */
        char tmp[2] = {0};
        lv_dropdown_get_selected_str(lv_event_get_target(e),
                                     tmp, sizeof(tmp));
        *((char*)ctx->dst) = tmp[0];
    }

    nmea_mark_dirty(ctx->grp_id);
}

bool ui_dropdown_bind(lv_obj_t *dd, void *dst, uint8_t is_str2, int grp_id)
{
    ui_drop_ctx_t *ctx;

    if (!dd) return false;
    ctx = malloc(sizeof(*ctx));
    if (!ctx) return false;

    *ctx = (ui_drop_ctx_t){
        .dst = dst,
        .is_str2 = is_str2,
        .grp_id = grp_id,
    };

    lv_obj_add_event_cb(dd, ui_dropdown_generic_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(dd, free_drop_ctx_cb_, LV_EVENT_DELETE, ctx);
    return true;
}
