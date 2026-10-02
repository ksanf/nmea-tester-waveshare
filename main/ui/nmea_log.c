/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/nmea_log.h"
#include "ui/nmea_log_markup.h"
#include "config/memory_config.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_NMEA_LOG
#include "config_logs.h"

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
/*  User-tunable constants                                                        */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
#define ROWS                NMEA_LOG_RING_ROWS
#define COLS                256    /* Maximum row length including markup   */
#define BIGBUF_SZ           (ROWS * COLS + ROWS + 1)
#define UPDATE_INTERVAL_MS  200    /* Batch update label every 200ms if dirty */
/* Theme colors expressed as hexadecimal values. */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
/*  LVGL font                                                                    */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_nmea_unscii_16)

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
/*  Static storage                                                               */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
static lv_obj_t *page  = NULL;               /* Scroll container             */
static lv_obj_t *label = NULL;               /* Shared label canvas          */

static char (*ring)[COLS] = NULL;            /* Row ring buffer              */
static uint16_t head = 0;                    /* Next row to write            */
static bool auto_scroll_enabled = true;
static bool dirty = false;                   /* Flag for batch update        */
static lv_timer_t *update_timer = NULL;      /* Timer for batch update       */

static char *bigbuf = NULL;                  /* Module work buffer      */
static uint16_t wrap_width_px = 0u;

static bool buffers_ready_(void)
{
    return ring && bigbuf;
}

static void buffers_free_(void)
{
    free(ring);
    ring = NULL;
    free(bigbuf);
    bigbuf = NULL;
}

static bool buffers_alloc_(void)
{
    ring = CALLOC_WHERE(NMEA_LOG_RING_IN_PSRAM, ROWS, sizeof(*ring));
    bigbuf = CALLOC_WHERE(NMEA_LOG_BIGBUF_IN_PSRAM, 1, BIGBUF_SZ);
    if (!buffers_ready_()) {
        buffers_free_();
        return false;
    }
    return true;
}

static uint32_t log_bg_hex_(void)
{
    return ui_theme_get()->log_bg;
}

static uint32_t log_text_hex_(void)
{
    return (ui_theme_get_id() == UI_THEME_COLOR) ? UI_NMEA_LOG_TEXT_HEX : ui_theme_get()->text;
}

static uint32_t log_pfx_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return UINT32_C(0x606870);
        case UI_THEME_BW:    return UINT32_C(0xD0D0D0);
        default:             return UINT32_C(0xF0F0F0);
    }
}

static uint32_t log_talker_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return UINT32_C(0x2F3943);
        case UI_THEME_BW:    return UINT32_C(0xFFFFFF);
        default:             return UINT32_C(0xFFFF00);
    }
}

static uint32_t log_field_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return UINT32_C(0x1F252B);
        case UI_THEME_BW:    return UINT32_C(0xFFFFFF);
        default:             return UINT32_C(0x00FF00);
    }
}

static uint32_t log_crc_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return UINT32_C(0x8E99A4);
        case UI_THEME_BW:    return UINT32_C(0xC0C0C0);
        default:             return UINT32_C(0xFF0000);
    }
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
/*  Helpers                                                                      */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
static inline void buf_push(const char *s)
{
    if (!buffers_ready_()) return;
    if (strlen(s) >= COLS - 1) ESP_LOGW("nmea_log", "String too long, truncated");
    strncpy(ring[head], s, COLS - 1);
    ring[head][COLS - 1] = '\0';
    head = (head + 1) % ROWS;
    dirty = true;  // Set dirty for batch
}

static size_t sanitize_copy_(char *dst, size_t dst_sz, const char *src)
{
    size_t i = 0;

    if (!dst || dst_sz == 0 || !src) return 0;

    while (src[i] && src[i] != '\r' && src[i] != '\n' && i < (dst_sz - 1)) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
        i++;
    }
    dst[i] = '\0';
    return i;
}

static void buf_push_plain_(const char *s)
{
    char escaped[COLS];

    if (!nmea_log_markup_escape_plain(escaped, sizeof(escaped), s)) {
        size_t dst_len = 0u;

        ESP_LOGW("nmea_log", "Plain line too long after escaping; truncated");
        while (*s && dst_len + ((*s == '#') ? 2u : 1u) < sizeof(escaped)) {
            if (*s == '#') escaped[dst_len++] = '#';
            escaped[dst_len++] = *s++;
        }
        escaped[dst_len] = '\0';
    }
    buf_push(escaped);
}

static uint16_t glyph_width_(unsigned char current,
                             unsigned char next,
                             void *context)
{
    const lv_font_t *font = (const lv_font_t *)context;
    lv_coord_t width = lv_font_get_glyph_width(font, current, next);

    return width > 0 ? (uint16_t)width : 0u;
}

static bool is_nmea_like_(const char *s, const char **body_out, char *prefix_out)
{
    const char *body = s;
    char prefix = '\0';

    if (!s || !*s) return false;

    if (*body == '$' || *body == '!') {
        prefix = *body;
        body++;
    }

    if (strlen(body) < 6) return false;
    if (body[5] != ',') return false;

    for (int i = 0; i < 5; i++) {
        unsigned char c = (unsigned char)body[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }

    if (body_out) *body_out = body;
    if (prefix_out) *prefix_out = prefix;
    return true;
}

static void add_colored_nmea_(const char *plain, const char *body, char prefix)
{
    char line[COLS];
    const char *fields = body + 5;
    const char *crc = strchr(fields, '*');
    nmea_log_markup_span_t spans[4];
    size_t span_count = 0u;

    if (prefix != '\0') {
        spans[span_count++] = (nmea_log_markup_span_t) {
            .text = plain, .length = 1u, .color = log_pfx_hex_()
        };
    }
    spans[span_count++] = (nmea_log_markup_span_t) {
        .text = body, .length = 5u, .color = log_talker_hex_()
    };
    spans[span_count++] = (nmea_log_markup_span_t) {
        .text = fields,
        .length = crc ? (size_t)(crc - fields) : strlen(fields),
        .color = log_field_hex_()
    };
    if (crc) {
        spans[span_count++] = (nmea_log_markup_span_t) {
            .text = crc, .length = strlen(crc), .color = log_crc_hex_()
        };
    }

    if (!nmea_log_markup_format(line, sizeof(line), spans, span_count,
                                wrap_width_px, 0, glyph_width_,
                                (void *)&lv_font_montserrat_16)) {
        ESP_LOGW("nmea_log", "Colored NMEA line too long; using plain text");
        buf_push_plain_(plain);
        return;
    }
    buf_push(line);
}

/* Compose the complete label text in bigbuf. */
static void compose(void)
{
    if (!buffers_ready_()) return;
    bigbuf[0] = '\0';
    uint16_t idx = head;
    size_t len = 0;
    int added = 0;

    for (uint16_t i = 0; i < ROWS; ++i) {
        if (ring[idx][0]) {
            if (added > 0) {
                if (len + 1u >= BIGBUF_SZ) break;
                bigbuf[len++] = '\n';
                bigbuf[len] = '\0';
            }
            const size_t row_len = strnlen(ring[idx], COLS - 1u);
            const size_t copy_len = (row_len < BIGBUF_SZ - len - 1u)
                                      ? row_len : BIGBUF_SZ - len - 1u;
            memcpy(bigbuf + len, ring[idx], copy_len);
            len += copy_len;
            bigbuf[len] = '\0';
            added++;
        }
        idx = (idx + 1) % ROWS;
    }

    // Add a final empty line.
    if (len + 1u < BIGBUF_SZ) {
        bigbuf[len++] = '\n';
        bigbuf[len] = '\0';
    }
}

/* Batch update callback */
static void update_log_cb(lv_timer_t *timer)
{
    /* Widgets may already have been deleted. */
    if (!label || !page) return;
    if (!buffers_ready_()) return;
    if (!dirty) return;

    compose();
    lv_label_set_text(label, bigbuf);
    dirty = false;

    if (auto_scroll_enabled) {
        lv_obj_update_layout(label);  // Ensure height updated
        int label_h = lv_obj_get_height(label);
        int page_h = lv_obj_get_height(page);
        if (label_h > page_h) {
            lv_obj_scroll_to_y(page, label_h - page_h, LV_ANIM_OFF);
        }
    }
}

/**
 * @brief Clear global references when the parent container is deleted.
 */
static void free_log_cb(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (page && page != deleted) return;
    ESP_LOGI("nmea_log", "DELETE event — cleaning up");

    if (update_timer) {
        lv_timer_del(update_timer);
        update_timer = NULL;
    }

    /* LVGL is already destroying the objects; only clear references. */
    label = NULL;
    page  = NULL;
    dirty = false;
    head = 0;
    buffers_free_();
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
/*  Public API                                                                   */
/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
void nmea_log_init(lv_obj_t *parent, int x, int y, int w, int h)
{
    buffers_free_();
    if (!buffers_alloc_()) {
        ESP_LOGE("nmea_log", "buffer alloc failed");
        return;
    }

    /* Container */
    page = lv_obj_create(parent);
    lv_obj_set_pos(page, x, y);
    lv_obj_set_size(page, w, h);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, lv_color_hex(log_bg_hex_()), 0);
    if (ui_theme_get_id() != UI_THEME_COLOR) {
        lv_obj_set_style_border_color(page, lv_color_hex(ui_theme_get()->border), 0);
    }

    /* Label canvas */
    label = lv_label_create(page);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, w - 16);
    wrap_width_px = (uint16_t)((w > 24) ? (w - 24) : w);
    lv_label_set_recolor(label, true);
    lv_obj_set_style_text_color(label, lv_color_hex(log_text_hex_()), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_text(label, "");

    /* Reset the ring buffer. */
    memset(ring, 0, sizeof(*ring) * ROWS);
    head = 0;
    dirty = false;

    /* Timer for batch update */
    update_timer = lv_timer_create(update_log_cb, UPDATE_INTERVAL_MS, NULL);
    if (!update_timer) {
        ESP_LOGE("nmea_log", "update timer create failed");
    }

    /* Clear state when the page is deleted. */
    lv_obj_add_event_cb(page, free_log_cb, LV_EVENT_DELETE, NULL);

    ESP_LOGI("nmea_log", "initialized (%dx%d)", w, h);
}

/**
 * Add a recolored row with minimal overhead.
 */
void nmea_log_add(const char *s)
{
    const char *body = NULL;
    char prefix = '\0';
    char plain[COLS];
    size_t plain_len;

    if (!label || !page || !buffers_ready_()) return;
    if (!s || !*s) return;

    plain_len = sanitize_copy_(plain, sizeof(plain), s);
    if (plain_len == 0u) return;

    if (is_nmea_like_(plain, &body, &prefix)) {
        add_colored_nmea_(plain, body, prefix);
        return;
    }

    buf_push_plain_(plain);
}

void nmea_log_add_plain(const char *text)
{
    char line[COLS];
    size_t len = 0;

    if (!label || !page || !buffers_ready_()) return;
    if (!text || !*text) return;

    len = sanitize_copy_(line, sizeof(line), text);
    if (len == 0) return;

    buf_push_plain_(line);
}

void nmea_log_add_hex_line(const char *text)
{
    char plain[COLS];
    char colored[COLS] = {0};
    const char *address_end;
    const char *ascii_start;
    size_t address_len;
    size_t plain_len;
    nmea_log_markup_span_t spans[3];

    if (!label || !page || !buffers_ready_()) return;
    if (!text || !*text) return;

    plain_len = sanitize_copy_(plain, sizeof(plain), text);
    if (plain_len == 0) return;

    /* Metal and B/W themes intentionally remain single-color. */
    if (ui_theme_get_id() != UI_THEME_COLOR) {
        buf_push(plain);
        return;
    }

    address_end = strstr(plain, ": ");
    if (!address_end) {
        buf_push(plain);
        return;
    }
    address_len = (size_t)(address_end - plain) + 2u;
    ascii_start = strchr(plain + address_len, '|');
    if (!ascii_start) {
        buf_push(plain);
        return;
    }

    spans[0] = (nmea_log_markup_span_t) {
        .text = plain, .length = address_len, .color = UI_NMEA_HEX_ADDRESS_HEX
    };
    spans[1] = (nmea_log_markup_span_t) {
        .text = plain + address_len,
        .length = (size_t)(ascii_start - (plain + address_len)),
        .color = UI_NMEA_HEX_DATA_HEX
    };
    spans[2] = (nmea_log_markup_span_t) {
        .text = ascii_start,
        .length = plain_len - (size_t)(ascii_start - plain),
        .color = UI_NMEA_HEX_ASCII_HEX
    };

    if (!nmea_log_markup_format(colored, sizeof(colored), spans, 3u,
                                wrap_width_px, -2, glyph_width_,
                                (void *)&lv_font_nmea_unscii_16)) {
        buf_push(plain);
        return;
    }

    buf_push(colored);
}

void nmea_log_clear(void)
{
    if (!buffers_ready_()) return;

    memset(ring, 0, sizeof(*ring) * ROWS);
    bigbuf[0] = '\0';
    head = 0;
    dirty = false;

    if (label) {
        lv_label_set_text(label, "");
    }
    if (page) {
        lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
    }
}

void nmea_log_set_raw_mode(bool enabled)
{
    const bool colorized = enabled && ui_theme_get_id() == UI_THEME_COLOR;

    if (!label) return;

    lv_label_set_recolor(label, !enabled || colorized);
    lv_obj_set_style_text_font(label,
                               enabled ? &lv_font_nmea_unscii_16 : &lv_font_montserrat_16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, enabled ? -2 : 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, enabled ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label,
        lv_color_hex(colorized
                         ? UI_NMEA_HEX_DATA_HEX
                         : log_text_hex_()),
        LV_PART_MAIN);
}

void nmea_log_view_suspend(bool suspended)
{
    if (!update_timer) return;
    if (suspended) lv_timer_pause(update_timer);
    else {
        lv_timer_resume(update_timer);
        update_log_cb(update_timer);
    }
}
