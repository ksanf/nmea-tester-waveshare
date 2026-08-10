/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "AISdecoder/aisdecoder_internal.h"

#include "config/config_nmea_tester.h"
#include "config/memory_config.h"
#include "system/telnet_router.h"
#include "ui/dialog_ui.h"
#include "ui/screens/screen_ui.h"
#include "ui/ui_theme.h"
#include "lvgl.h"

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_AISDEC_UI
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nmea_unscii_16)

#define AISDEC_BACK_W               52
#define AISDEC_BACK_H               28
#define AISDEC_BACK_X               (LCD_WIDTH - AISDEC_BACK_W - 6)
#define AISDEC_BACK_Y               4
#define AISDEC_TICKER_Y             (AISDEC_BACK_Y + AISDEC_BACK_H + 4)
#define AISDEC_TICKER_H             22
#define AISDEC_MARGIN               8
#define AISDEC_LIST_Y               (AISDEC_TICKER_Y + AISDEC_TICKER_H + 6)
#define AISDEC_LINE_Q_LEN           16
#define AISDEC_TEXT_BUFSZ           (16U * 1024U)
#define AISDEC_REFRESH_MS          500U

typedef struct {
    char line[AISDEC_MAX_LINE];
} ais_line_evt_t;

static lv_obj_t *scr_ais = NULL;
static lv_obj_t *lbl_ticker = NULL;
static lv_obj_t *lbl_header = NULL;
static lv_obj_t *page_list = NULL;
static lv_obj_t *lbl_list = NULL;
static lv_timer_t *tmr_drain = NULL;
static QueueHandle_t s_line_q = NULL;
static uint32_t s_ais_lines_seen = 0;
static char s_last_line[AISDEC_MAX_LINE];
static char *s_list_buf = NULL;
static aisdecoder_target_t *s_render_targets = NULL;
static lv_obj_t *s_scr_prev = NULL;
static uint32_t s_theme_rev = 0;
static const char *TAG = "aisdec_ui";

static bool ui_buffers_alloc_(void)
{
    if (!s_list_buf) {
        s_list_buf = CALLOC_WHERE(AIS_LIST_BUF_IN_PSRAM, 1, AISDEC_TEXT_BUFSZ);
    }
    if (!s_render_targets) {
        s_render_targets = CALLOC_WHERE(AIS_TARGETS_IN_PSRAM,
                                        AISDEC_MAX_TARGETS,
                                        sizeof(*s_render_targets));
    }
    if (s_list_buf && s_render_targets) return true;

    free(s_render_targets);
    s_render_targets = NULL;
    free(s_list_buf);
    s_list_buf = NULL;
    return false;
}

static void ui_buffers_free_(void)
{
    free(s_render_targets);
    s_render_targets = NULL;
    free(s_list_buf);
    s_list_buf = NULL;
}

static inline bool screen_alive_(lv_obj_t *scr)
{
    return scr && lv_obj_is_valid(scr) && lv_obj_get_parent(scr) == NULL;
}

static lv_obj_t *create_card_(lv_obj_t *parent,
                              lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h,
                              uint32_t top_hex,
                              uint32_t bottom_hex,
                              uint32_t border_hex)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(top_hex), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(bottom_hex), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(border_hex), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    return card;
}

static void sync_header_x_(void)
{
    if (!lbl_header || !page_list) return;
    lv_obj_set_x(lbl_header, -lv_obj_get_scroll_x(page_list));
}

static void format_lat_(float lat, char *dst, size_t dst_sz)
{
    char hemi = 'N';

    if (lat < -90.0f || lat > 90.0f) {
        strlcpy(dst, "--", dst_sz);
        return;
    }
    if (lat < 0.0f) {
        hemi = 'S';
        lat = -lat;
    }
    snprintf(dst, dst_sz, "%.2f%c", lat, hemi);
}

static void format_lon_(float lon, char *dst, size_t dst_sz)
{
    char hemi = 'E';

    if (lon < -180.0f || lon > 180.0f) {
        strlcpy(dst, "--", dst_sz);
        return;
    }
    if (lon < 0.0f) {
        hemi = 'W';
        lon = -lon;
    }
    snprintf(dst, dst_sz, "%.2f%c", lon, hemi);
}

static void render_target_list_(void)
{
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    size_t count;
    lv_coord_t scroll_x = 0;
    lv_coord_t scroll_y = 0;
    size_t len = 0;

    if (!page_list || !lbl_list || !s_list_buf || !s_render_targets) return;

    scroll_x = lv_obj_get_scroll_x(page_list);
    scroll_y = lv_obj_get_scroll_y(page_list);
    count = aisdecoder_decode_collect(s_render_targets, AISDEC_MAX_TARGETS, now_ms);

    ESP_LOGI(TAG, "render targets: count=%u ais_lines=%lu last=%s",
             (unsigned)count, (unsigned long)s_ais_lines_seen,
             s_last_line[0] ? s_last_line : "(none)");

    s_list_buf[0] = '\0';

    if (count == 0) {
        strlcpy(s_list_buf, "-- waiting for AIS decode --", AISDEC_TEXT_BUFSZ);
    } else {
        for (size_t i = 0; i < count && i < AISDEC_MAX_TARGETS; i++) {
            char lat[16];
            char lon[16];
            const char *name = s_render_targets[i].name[0] ? s_render_targets[i].name : "--";
            const char *call = s_render_targets[i].call[0] ? s_render_targets[i].call : "--";

            format_lat_(s_render_targets[i].lat, lat, sizeof(lat));
            format_lon_(s_render_targets[i].lon, lon, sizeof(lon));

            len += snprintf(s_list_buf + len, AISDEC_TEXT_BUFSZ - len,
                            "%-10.10s %-7.7s %09" PRIu32 " %-7.7s %-8.8s %4.1f %4.0f %3u%s",
                            name, call, s_render_targets[i].mmsi, lat, lon,
                            s_render_targets[i].sog, s_render_targets[i].cog,
                            s_render_targets[i].heading,
                            (i + 1 < count) ? "\n" : "");
            if (len >= AISDEC_TEXT_BUFSZ) {
                len = AISDEC_TEXT_BUFSZ - 1;
                s_list_buf[len] = '\0';
                break;
            }
        }
    }

    lv_label_set_text(lbl_list, s_list_buf);
    lv_obj_update_layout(lbl_list);
    lv_obj_update_layout(page_list);
    ESP_LOGI(TAG, "list geom: x=%d y=%d w=%d h=%d page_w=%d page_h=%d sx=%d sy=%d text_len=%u",
             (int)lv_obj_get_x(lbl_list),
             (int)lv_obj_get_y(lbl_list),
             (int)lv_obj_get_width(lbl_list),
             (int)lv_obj_get_height(lbl_list),
             (int)lv_obj_get_width(page_list),
             (int)lv_obj_get_height(page_list),
             (int)scroll_x,
             (int)scroll_y,
             (unsigned)strlen(s_list_buf));
    lv_obj_scroll_to_x(page_list, scroll_x, LV_ANIM_OFF);
    lv_obj_scroll_to_y(page_list, scroll_y, LV_ANIM_OFF);
    sync_header_x_();
}

static void list_scroll_cb_(lv_event_t *e)
{
    (void)e;
    sync_header_x_();
}

static void drain_timer_cb_(lv_timer_t *tmr)
{
    ais_line_evt_t evt;
    (void)tmr;

    bool updated = false;
    while (s_line_q && xQueueReceive(s_line_q, &evt, 0) == pdTRUE) {
        strlcpy(s_last_line, evt.line, sizeof(s_last_line));
        s_ais_lines_seen++;
        updated = true;
    }
    if (updated) {
        if (lbl_ticker && lv_obj_is_valid(lbl_ticker)) {
            lv_label_set_text(lbl_ticker, s_last_line);
        }
        render_target_list_();
    }
}

static void scr_delete_cb_(lv_event_t *e)
{
    lv_obj_t *deleted = lv_event_get_target(e);
    if (scr_ais && scr_ais != deleted) return;
    lbl_ticker = NULL;
    lbl_header = NULL;
    page_list = NULL;
    lbl_list = NULL;
    if (tmr_drain) {
        lv_timer_del(tmr_drain);
        tmr_drain = NULL;
    }
    scr_ais = NULL;
    s_scr_prev = NULL;
    ui_buffers_free_();
    aisdecoder_decode_deinit();
    if (s_line_q) {
        vQueueDelete(s_line_q);
        s_line_q = NULL;
    }
}

static void destroy_(void)
{
    lv_obj_t *old;

    if (!scr_ais) return;
    old = scr_ais;
    scr_ais = NULL;

    telnet_router_set_active(TELNET_ROUTE_NONE);
    if (tmr_drain) {
        lv_timer_del(tmr_drain);
        tmr_drain = NULL;
    }
    if (s_line_q) {
        vQueueDelete(s_line_q);
        s_line_q = NULL;
    }
    if (s_scr_prev && screen_alive_(s_scr_prev)) {
        lv_scr_load(s_scr_prev);
    } else {
        lv_scr_load(screen_ui_get_main());
    }
    aisdecoder_decode_deinit();
    ui_buffers_free_();
    lbl_ticker = NULL;
    lbl_header = NULL;
    page_list = NULL;
    lbl_list = NULL;
    lv_obj_del_async(old);
}

static void back_cb_(lv_event_t *e)
{
    (void)e;
    destroy_();
}

lv_obj_t *aisdecoder_create(lv_obj_t *parent)
{
    lv_obj_t *ticker_card;
    lv_obj_t *list_card;
    lv_obj_t *title;
    lv_coord_t list_card_w;
    lv_coord_t list_h;
    lv_coord_t header_y;
    lv_coord_t header_h;
    lv_coord_t content_y;
    lv_coord_t content_h;

    if (scr_ais && s_theme_rev != ui_theme_get_revision()) {
        destroy_();
    }
    if (scr_ais) {
        s_theme_rev = ui_theme_get_revision();
        lv_scr_load(scr_ais);
        return scr_ais;
    }

    s_scr_prev = parent;
    ui_buffers_free_();
    if (!ui_buffers_alloc_()) {
        ESP_LOGE(TAG, "UI buffer alloc failed");
        return parent;
    }
    telnet_router_set_active(TELNET_ROUTE_NONE);
    aisdecoder_decode_reset();

    scr_ais = lv_obj_create(NULL);
    lv_obj_clear_flag(scr_ais, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_apply_screen(scr_ais);
    lv_obj_set_style_border_width(scr_ais, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scr_ais, scr_delete_cb_, LV_EVENT_DELETE, NULL);

    title = lv_label_create(scr_ais);
    lv_label_set_text(title, "AIS DECODER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(ui_theme_get()->title), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    dialog_ui_create_button(scr_ais,
                            AISDEC_BACK_X, AISDEC_BACK_Y, AISDEC_BACK_W, AISDEC_BACK_H,
                            LV_SYMBOL_LEFT, COLOR_BUTTON_HOME,
                            back_cb_, NULL, &lv_font_montserrat_22);

    uint32_t card_hex = ui_theme_get()->card;
    uint32_t border_hex = ui_theme_get()->border;
    ticker_card = create_card_(scr_ais,
                               AISDEC_MARGIN, AISDEC_TICKER_Y,
                               LCD_WIDTH - (AISDEC_MARGIN * 2), AISDEC_TICKER_H,
                               card_hex, card_hex, border_hex);
    lbl_ticker = lv_label_create(ticker_card);
    lv_obj_set_width(lbl_ticker, LCD_WIDTH - (AISDEC_MARGIN * 2) - 16);
    lv_label_set_long_mode(lbl_ticker, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl_ticker, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ticker, lv_color_hex(AISDEC_COLOR_BAR_TEXT), 0);
    lv_label_set_text(lbl_ticker, "Waiting for AIS sentences (!AIVDM / !AIVDO)...");
    lv_obj_align(lbl_ticker, LV_ALIGN_LEFT_MID, 0, 0);

    list_card_w = LCD_WIDTH - (AISDEC_MARGIN * 2);
    list_h = LCD_HEIGHT - AISDEC_LIST_Y - AISDEC_MARGIN;
    list_card = create_card_(scr_ais,
                             AISDEC_MARGIN, AISDEC_LIST_Y,
                             list_card_w, list_h,
                             card_hex, card_hex, border_hex);

    header_y = 0;
    header_h = 18;
    lbl_header = lv_label_create(list_card);
    lv_label_set_text(lbl_header, "SHIP       CALL    MMSI      LAT     LON      SOG   COG HDT");
    lv_obj_set_style_text_font(lbl_header, &lv_font_nmea_unscii_16, 0);
    lv_obj_set_style_text_color(lbl_header, lv_color_hex(ui_theme_get()->muted), 0);
    lv_obj_set_pos(lbl_header, 0, header_y);

    content_y = header_y + header_h + 4;
    content_h = list_h - content_y;
    lv_obj_update_layout(list_card);
    page_list = lv_obj_create(list_card);
    lv_obj_set_pos(page_list, 0, content_y);
    lv_obj_set_size(page_list, list_card_w - 16, content_h);
    lv_obj_set_scroll_dir(page_list, LV_DIR_HOR | LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(page_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_list, 0, 0);
    lv_obj_set_style_pad_all(page_list, 0, 0);
    lv_obj_add_event_cb(page_list, list_scroll_cb_, LV_EVENT_SCROLL, NULL);

    lbl_list = lv_label_create(page_list);
    lv_label_set_long_mode(lbl_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_list, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(lbl_list, &lv_font_nmea_unscii_16, 0);
    lv_obj_set_style_text_line_space(lbl_list, 2, 0);
    lv_obj_set_style_text_color(lbl_list, lv_color_hex(AISDEC_COLOR_LIST_TEXT), 0);
    lv_label_set_text(lbl_list, "-- waiting for AIS decode --");

    render_target_list_();

    s_line_q = xQueueCreate(AISDEC_LINE_Q_LEN, sizeof(ais_line_evt_t));
    if (!s_line_q) {
        ESP_LOGE(TAG, "line queue alloc failed");
        destroy_();
        return parent;
    }
    tmr_drain = lv_timer_create(drain_timer_cb_, AISDEC_REFRESH_MS, NULL);
    if (!tmr_drain) {
        ESP_LOGE(TAG, "drain timer alloc failed");
        destroy_();
        return parent;
    }

    s_theme_rev = ui_theme_get_revision();
    lv_scr_load(scr_ais);
    return scr_ais;
}

lv_obj_t *aisdecoder_get_screen(void)
{
    return scr_ais;
}

bool aisdecoder_is_ais_sentence(const char *line)
{
    return aisdecoder_parse_is_sentence(line);
}

void aisdecoder_feed_nmea_line(const char *line)
{
    ais_line_evt_t evt;

    if (!line || !*line) return;
    if (!scr_ais || !screen_alive_(scr_ais)) return;

    aisdecoder_parse_feed_line(line);

    if (!s_line_q) return;
    strlcpy(evt.line, line, sizeof(evt.line));
    (void)xQueueSend(s_line_q, &evt, 0);
}
