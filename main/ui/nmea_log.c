/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "ui/nmea_log.h"
#include "config/memory_config.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "lvgl.h"
#include <inttypes.h>
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

static char *bigbuf = NULL;                  /* Module work buffer           */

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

static const char *log_pfx_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "606870";
        case UI_THEME_BW:    return "d0d0d0";
        default:             return "f0f0f0";
    }
}

static const char *log_talker_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "2f3943";
        case UI_THEME_BW:    return "ffffff";
        default:             return "ffff00";
    }
}

static const char *log_field_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "1f252b";
        case UI_THEME_BW:    return "ffffff";
        default:             return "00ff00";
    }
}

static const char *log_crc_hex_(void)
{
    switch (ui_theme_get_id()) {
        case UI_THEME_METAL: return "8e99a4";
        case UI_THEME_BW:    return "c0c0c0";
        default:             return "ff0000";
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

static bool append_raw_recolor_span_(char *dst, size_t dst_sz, size_t *dst_len,
                                     uint32_t color, const char *src, size_t src_len)
{
    char color_cmd[9];
    int color_cmd_len;

    if (!dst || !dst_len || !src || *dst_len >= dst_sz) return false;

    color_cmd_len = snprintf(color_cmd, sizeof(color_cmd),
                             "#%06" PRIX32 " ", color & UINT32_C(0xFFFFFF));
    if (color_cmd_len < 0 || (size_t)color_cmd_len >= sizeof(color_cmd) ||
        *dst_len + (size_t)color_cmd_len + 2u > dst_sz) {
        return false;
    }
    memcpy(dst + *dst_len, color_cmd, (size_t)color_cmd_len);
    *dst_len += (size_t)color_cmd_len;

    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '#') {
            const size_t escaped_len = 3u + (size_t)color_cmd_len;

            /* Close the span, emit ## as a literal #, then restore color. */
            if (*dst_len + escaped_len + 2u > dst_sz) return false;
            dst[(*dst_len)++] = '#';
            dst[(*dst_len)++] = '#';
            dst[(*dst_len)++] = '#';
            memcpy(dst + *dst_len, color_cmd, (size_t)color_cmd_len);
            *dst_len += (size_t)color_cmd_len;
        } else {
            /* Keep room for the closing recolor marker and NUL. */
            if (*dst_len + 3u > dst_sz) return false;
            dst[(*dst_len)++] = src[i];
        }
    }

    dst[(*dst_len)++] = '#';
    dst[*dst_len] = '\0';
    return true;
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

static void add_colored_nmea_(const char *body, char prefix)
{
    char pre[2] = {0};
    char talk[6] = {0};
    char fields[128] = {0};
    char crc[8] = {0};
    char line[COLS];
    const char *c_pfx = log_pfx_hex_();
    const char *c_talker = log_talker_hex_();
    const char *c_field = log_field_hex_();
    const char *c_crc = log_crc_hex_();
    const char *p_fields = strchr(body, ',');
    const char *p_crc = strchr(body, '*');
    size_t fields_len = 0;

    if (prefix != '\0') pre[0] = prefix;
    strncpy(talk, body, 5);

    if (p_fields) {
        const char *fields_start = p_fields + 1;
        if (p_crc && p_crc > fields_start) {
            fields_len = (size_t)(p_crc - fields_start);
        } else {
            fields_len = strlen(fields_start);
        }
        if (fields_len >= sizeof(fields)) fields_len = sizeof(fields) - 1;
        memcpy(fields, fields_start, fields_len);
        fields[fields_len] = '\0';
    }

    if (p_crc && strlen(p_crc) >= 3) {
        strncpy(crc, p_crc, 3);
    }

    /* Recolor commands are concatenated without visible separators.
     * Keep the leading field comma and checksum delimiter inside their
     * colored spans so rendered text remains byte-for-byte identical. */
    if (pre[0] != '\0') {
        snprintf(line, sizeof(line),
            "#%s %s##%s %s##%s ,%s##%s %s#",
            c_pfx, pre,
            c_talker, talk,
            c_field, fields,
            c_crc, crc);
    } else {
        snprintf(line, sizeof(line),
            "#%s %s##%s ,%s##%s %s#",
            c_talker, talk,
            c_field, fields,
            c_crc, crc);
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

    if (!label || !page || !buffers_ready_()) return;
    if (!s || !*s) return;

    if (is_nmea_like_(s, &body, &prefix)) {
        add_colored_nmea_(body, prefix);
        return;
    }

    if (sanitize_copy_(plain, sizeof(plain), s) == 0) return;
    buf_push(plain);
}

void nmea_log_add_plain(const char *text)
{
    char line[COLS];
    size_t len = 0;

    if (!label || !page || !buffers_ready_()) return;
    if (!text || !*text) return;

    len = sanitize_copy_(line, sizeof(line), text);
    if (len == 0) return;

    buf_push(line);
}

void nmea_log_add_hex_line(const char *text)
{
    char plain[COLS];
    char colored[COLS] = {0};
    const char *address_end;
    const char *ascii_start;
    size_t address_len;
    size_t colored_len = 0;
    size_t plain_len;

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

    if (!append_raw_recolor_span_(colored, sizeof(colored), &colored_len,
                                  UI_NMEA_HEX_ADDRESS_HEX, plain, address_len) ||
        !append_raw_recolor_span_(colored, sizeof(colored), &colored_len,
                                  UI_NMEA_HEX_DATA_HEX,
                                  plain + address_len,
                                  (size_t)(ascii_start - (plain + address_len))) ||
        !append_raw_recolor_span_(colored, sizeof(colored), &colored_len,
                                  UI_NMEA_HEX_ASCII_HEX, ascii_start,
                                  plain_len - (size_t)(ascii_start - plain))) {
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
