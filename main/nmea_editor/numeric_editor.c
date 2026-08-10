/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "nmea_editor/numeric_editor.h"
#include "ui/ui_colors.h"
#include <string.h>
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_NUMERIC_EDITOR
#include "config_logs.h"

/* Enabled by LOG_CFG_NUMERIC_EDITOR in config_logs.h. */
#if NUM_EDIT_DEBUG
#define LOGI(fmt, ...) ESP_LOGI("NUM_EDIT", fmt, ##__VA_ARGS__)
#else
#define LOGI(fmt, ...)
#endif

/* -------- Global objects -------- */
static lv_obj_t *kb = NULL;   /* Single shared keyboard */
static lv_obj_t *proxy_ta = NULL;   /* Hidden textarea input sink */
static lv_obj_t *active_lbl = NULL;   /* Label currently being edited */
static lv_style_t underline_style; /* Cursor underline style */
static bool style_initialized = false; /* Style initialization flag */

/* -------- Underline-style initialization -------- */
static void init_underline_style(void) {
    if (style_initialized) return;
    lv_style_init(&underline_style);
    lv_style_set_border_width(&underline_style, 3); /* Line width */
    lv_style_set_border_side(&underline_style, LV_BORDER_SIDE_BOTTOM); /* Bottom edge only */
    lv_style_set_border_color(&underline_style, lv_color_hex(UI_NUM_EDIT_UNDERLINE_HEX));
    lv_style_set_border_opa(&underline_style, LV_OPA_COVER);
    lv_style_set_bg_opa(&underline_style, LV_OPA_TRANSP); /* Transparent background */
    lv_style_set_pad_all(&underline_style, 0); /* No padding */
    style_initialized = true;
}

/* -------- Scroll the label just above the keyboard -------- */
static void scroll_into_view(lv_obj_t *obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_coord_t kb_top = LV_VER_RES / 2;
    lv_coord_t target = kb_top - LV_VER_RES / 10;
    lv_coord_t dy = 0;

    if (a.y2 > kb_top) dy = -(a.y2 - target);
    else if (a.y1 < target) dy = (target - a.y1);

    if (dy) lv_obj_scroll_by(lv_obj_get_parent(obj), 0, dy, LV_ANIM_OFF);
}

/* -------- Editing context -------- */
typedef struct {
    char *raw;                  /* Unformatted buffer */
    const int8_t *map;          /* Display index to raw index */
    uint8_t disp_len;
    void (*fmt)(char *);        /* Raw value to display format */
    void (*commit)(void);
    char tmp[16];
    uint8_t cursor_pos;         /* Current cursor position */
    const field_limit_t *limits; /* Per-field limits */
    uint8_t num_fields;         /* Number of limited fields */
    lv_obj_t *cursor_underline; /* Character underline object */
    lv_timer_t *blink_timer;    /* Cursor blink timer */
} ctx_t;

static void on_label_delete(lv_event_t *e)
{
    ctx_t *c = lv_event_get_user_data(e);
    if (c && c->blink_timer) {
        lv_timer_del(c->blink_timer);
        c->blink_timer = NULL;
    }
    if (active_lbl == lv_event_get_target(e)) {
        num_edit_cancel();
    }
    if (c) {
        lv_obj_set_user_data(lv_event_get_target(e), NULL);
        lv_mem_free(c);
    }
}
/* -------- Helper macros -------- */
#define NEXT_EDITABLE(i, ctx) \
    while ((i) < (ctx)->disp_len && (ctx)->map[i] < 0) ++(i)
#define PREV_EDITABLE(i, ctx) \
    while ((i) > 0 && (ctx)->map[i] < 0) --(i)

/* -------- Compute 10^n without math.h -------- */
static int power10(int n) {
    int p = 1;
    for (int i = 0; i < n; i++) p *= 10;
    return p;
}

/* -------- Cursor blink callback -------- */
static void blink_callback(lv_timer_t *timer) {
    ctx_t *c = timer->user_data;
    if (!c || !c->cursor_underline || !lv_obj_is_valid(c->cursor_underline)) return;

    if (lv_obj_has_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------- Update the cursor underline position -------- */
static void update_cursor_underline(ctx_t *c, lv_obj_t *label) {
    if (!c->cursor_underline || !lv_obj_is_valid(c->cursor_underline)) return;

    if (c->cursor_pos >= c->disp_len || c->map[c->cursor_pos] < 0) {
        lv_obj_add_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Calculate the character position and width. */
    lv_point_t pos_start = {0, 0};
    lv_point_t pos_end = {0, 0};
    lv_label_get_letter_pos(label, c->cursor_pos, &pos_start);
    lv_label_get_letter_pos(label, c->cursor_pos + 1, &pos_end);

    lv_coord_t width = pos_end.x - pos_start.x + 2;
    lv_coord_t height = lv_font_get_line_height(lv_obj_get_style_text_font(label, 0));

    /* Set the underline size and position. */
    lv_obj_set_size(c->cursor_underline, width, height);
    lv_coord_t label_x = lv_obj_get_x(label);
    lv_coord_t label_y = lv_obj_get_y(label);
    lv_obj_set_pos(c->cursor_underline, label_x + pos_start.x + 3, label_y + pos_start.y + 1);

    /* Ensure the style is applied. */
    lv_obj_add_style(c->cursor_underline, &underline_style, 0);
    lv_obj_clear_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(c->cursor_underline);
}

/* -------- Check whether a digit can be accepted -------- */
static bool can_accept_digit(ctx_t *c, uint8_t cur, char digit) {
    int8_t raw_idx = c->map[cur];
    if (raw_idx < 0) return false;

    if (c->limits == NULL || c->num_fields == 0) return true;

    /* Find the field containing this raw index. */
    int field = -1;
    for (int f = 0; f < c->num_fields; f++) {
        if (raw_idx >= c->limits[f].start_raw && raw_idx < c->limits[f].start_raw + c->limits[f].digits) {
            field = f;
            break;
        }
    }
    if (field == -1) return true;

    field_limit_t lim = c->limits[field];
    int pos_in_field = raw_idx - lim.start_raw;
    int remaining = lim.digits - pos_in_field - 1;
    int p10_rem = power10(remaining);
    int p10_after = p10_rem * 10;

    /* Calculate the prefix formed by preceding field digits. */
    int prefix = 0;
    for (int i = 0; i < pos_in_field; i++) {
        char ch = c->raw[lim.start_raw + i];
        if (ch < '0' || ch > '9') ch = '0';
        prefix = prefix * 10 + (ch - '0');
    }

    int d = digit - '0';
    int min_possible = prefix * p10_after + d * p10_rem;
    int max_possible = prefix * p10_after + d * p10_rem + (p10_rem - 1);

    if (max_possible < lim.min_val || min_possible > lim.max_val) return false;
    return true;
}

/* -------- Reset state -------- */
void num_edit_cancel(void) {
    if (kb && lv_obj_is_valid(kb)) lv_obj_del(kb);
    if (proxy_ta && lv_obj_is_valid(proxy_ta)) {
        lv_obj_add_flag(proxy_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(proxy_ta, NULL);
    }
    kb = NULL;
    active_lbl = NULL;
}

/* -------- Backspace handling -------- */
static void handle_backspace(ctx_t *c, lv_obj_t *label) {
    uint8_t cur = c->cursor_pos;
    if (cur == 0) return;

    cur--;
    PREV_EDITABLE(cur, c);

    if (cur == 0 && c->map[cur] < 0) return;

    c->raw[c->map[cur]] = '0';
    c->fmt(c->tmp);

    c->cursor_pos = cur;

    static bool lock = false;
    if (lock) return;
    lock = true;
    lv_label_set_text(label, c->tmp);
    update_cursor_underline(c, label);
    lock = false;
    lv_obj_invalidate(label);
}

/* -------- Commit, hide the keyboard, and refresh the label -------- */
static void handle_commit_and_hide(ctx_t *c, lv_obj_t *label) {
    if (c->commit) c->commit();
    c->fmt(c->tmp);
    c->cursor_pos = 0;

    static bool lock = false;
    if (lock) return;
    lock = true;
    lv_label_set_text(label, c->tmp);
    if (c->cursor_underline) lv_obj_add_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
    lock = false;
    lv_obj_invalidate(label);

    if (c->blink_timer) {
        lv_timer_del(c->blink_timer);
        c->blink_timer = NULL;
    }

    if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* -------- Event handler -------- */
static void edit_cb(lv_event_t *e) {
    static bool lock = false;
    if (lock) return;
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_CLICKED && code != LV_EVENT_VALUE_CHANGED &&
        (!kb || lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN))) return;

    lv_obj_t *src = lv_event_get_target(e);
    lv_obj_t *label = (src == proxy_ta || src == kb) ? lv_obj_get_user_data(proxy_ta) : src;
    if (!label || !lv_obj_is_valid(label)) return;
    ctx_t *c = lv_obj_get_user_data(label);
    if (!c) return;

    /* 1. A label tap opens the keyboard. */
    if (code == LV_EVENT_CLICKED) {
        if (!kb) {
            kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(kb, LV_HOR_RES, LV_VER_RES / 2);
            lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, 0);
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_event_cb(kb, edit_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        if (!c->cursor_underline) {
            c->cursor_underline = lv_obj_create(lv_obj_get_parent(label));
            lv_obj_add_style(c->cursor_underline, &underline_style, 0);
            lv_obj_set_size(c->cursor_underline, 12, 20);
            lv_obj_add_flag(c->cursor_underline, LV_OBJ_FLAG_HIDDEN);
        }

        lv_keyboard_set_textarea(kb, proxy_ta);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_set_user_data(proxy_ta, label);

        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);

        scroll_into_view(label);
        c->fmt(c->tmp);
        c->cursor_pos = 0;
        NEXT_EDITABLE(c->cursor_pos, c);
        lock = true;
        lv_label_set_text(label, c->tmp);
        update_cursor_underline(c, label);
        lock = false;
        lv_obj_invalidate(label);
        active_lbl = label;

        if (!c->blink_timer) {
            c->blink_timer = lv_timer_create(blink_callback, 500, c);
        }

        return;
    }

    /* 2. Digits arrive through the proxy textarea. */
    if (code == LV_EVENT_INSERT && src == proxy_ta) {
        const char *t = lv_event_get_param(e);
        if (!t || !*t) return;

        if (*t >= '0' && *t <= '9') {
            uint8_t cur = c->cursor_pos;
            NEXT_EDITABLE(cur, c);
            if (cur < c->disp_len) {
                if (!can_accept_digit(c, cur, *t)) return;

                c->raw[c->map[cur]] = *t;
                c->fmt(c->tmp);

                uint8_t nxt = cur + 1;
                NEXT_EDITABLE(nxt, c);
                if (nxt < c->disp_len) {
                    c->cursor_pos = nxt;
                } else {
                    c->cursor_pos = cur;
                }

                lock = true;
                lv_label_set_text(label, c->tmp);
                update_cursor_underline(c, label);
                lock = false;
                lv_obj_invalidate(label);
            }
        } else if (*t == 127 || *t == '\b') {
            handle_backspace(c, label);
        } else if (*t == '\n' || *t == '\r') {
            handle_commit_and_hide(c, label);
        }
        lv_event_stop_processing(e);
        return;
    }

    /* 3. Handle Backspace and Enter key events. */
    if (code == LV_EVENT_KEY && src == proxy_ta) {
        const uint32_t *param = lv_event_get_param(e);
        if (!param) return;
        uint32_t k = *param;
        if (k == LV_KEY_BACKSPACE) {
            handle_backspace(c, label);
        } else if (k == LV_KEY_ENTER) {
            handle_commit_and_hide(c, label);
        }
        lv_event_stop_processing(e);
        return;
    }

    /* 4. Handle the OK button through VALUE_CHANGED. */
    if (code == LV_EVENT_VALUE_CHANGED && src == kb) {
        const uint32_t *param = lv_event_get_param(e);
        if (!param) return;
        uint32_t btn_id = *param;
        const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
        if (txt && strcmp(txt, LV_SYMBOL_OK) == 0) {
            handle_commit_and_hide(c, label);
        }
        lv_event_stop_processing(e);
        return;
    }

    /* 5. Commit and hide the keyboard on defocus. */
    if (code == LV_EVENT_DEFOCUSED) {
        handle_commit_and_hide(c, label);
        return;
    }
}

/* -------- API -------- */
void num_edit_init(void) {
    if (proxy_ta) return;

    init_underline_style();

    proxy_ta = lv_textarea_create(lv_layer_top());
    lv_obj_set_size(proxy_ta, 1, 1);
    lv_obj_add_flag(proxy_ta, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_opa(proxy_ta, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(proxy_ta, edit_cb, LV_EVENT_INSERT, NULL);
    lv_obj_add_event_cb(proxy_ta, edit_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(proxy_ta, edit_cb, LV_EVENT_DEFOCUSED, NULL);
}

void num_edit_bind(lv_obj_t *label,
                   char *raw, const int8_t *map, uint8_t len,
                   void (*fmt)(char *), void (*commit)(void),
                   const field_limit_t *limits, uint8_t num_fields) {
    ctx_t *c = lv_obj_get_user_data(label);
    if (!c) {
        c = lv_mem_alloc(sizeof *c);
        if (!c) return;
        lv_obj_set_user_data(label, c);
        lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(label, edit_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(label, edit_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(label, on_label_delete, LV_EVENT_DELETE, c);

        c->cursor_pos = 0;
        c->cursor_underline = NULL;
        c->blink_timer = NULL;
    }
    c->raw = raw;
    c->map = map;
    c->disp_len = len;
    c->fmt = fmt;
    c->commit = commit;
    c->limits = limits;
    c->num_fields = num_fields;
    c->fmt(c->tmp);
    lv_label_set_text(label, c->tmp);
}
