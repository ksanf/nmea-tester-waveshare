/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   NMEA Tester RX-485 parser and bit indicator.
 */

#include "rs485/rs485_parser.h"
#include "rs485/rs485_driver.h"
#include "rs485/rs485_rx_stream.h"
#include "AISdecoder/aisdecoder.h"
#include "ui/nmea_log.h"
#include "ui/dialog_ui.h"
#include "ui/ui_baud_selector.h"
#include "ui/ui_colors.h"
#include "ui/ui_theme.h"
#include "ui/screens/screen_ui.h"
#include "config/config_nmea_tester.h"
#include "system/telnet_server.h"
#include "nmea_editor/nmea_templates.h"
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#define CFG_LOG_MODULE LOG_CFG_RS485_PARSER
#include "config_logs.h"
/* Additional includes. */
#include "ui/instrument_panel.h"

/* ─────────────── Defines ─────────────── */
#define TAG "rs485_parser"

#define POLL_PERIOD_MS    10
#define RX_WAIT_MS        25
#define FADE_OFF_MS      300

#define INDICATOR_SKIP     4
#define WIDTH_BAUDSEL     200
#define INDICATOR_X       WIDTH_BAUDSEL + 16
#define INDICATOR_Y       13
#define INDICATOR_W       18
#define INDICATOR_H       32
#define INDICATOR_SPACING  4
#define PANEL_H            60
#define AIS_BTN_W          136
#define AIS_BTN_H           50
#define AIS_BTN_X          (LCD_WIDTH - AIS_BTN_W - 8)
#define AIS_BTN_Y          (TOP_BAR_H + 5)
#define AIS_DETECT_HOLD_MS 15000U
#define BTN_W        88
#define BTN_H        50
#define BTN_GAP       4
#define LOG_LINES     8
#define TOP_BAR_H    72
/* Home and Refresh button size and font (previously 50). */
#define PARSER_NAV_BTN_SZ   66
#define PARSER_NAV_BTN_FONT &lv_font_montserrat_28
#define PARSER_HEX_BTN_FONT &lv_font_montserrat_16
#define Y_OFFSET     2
#define PANEL_IDLE_FLUSH_MS 1000U

/* ─────────────── Static ─────────────── */
static lv_obj_t *scr_rx       = NULL;
static lv_obj_t *refresh_btn = NULL;
static lv_obj_t *pause_btn   = NULL;
static lv_obj_t *hex_btn     = NULL;
static lv_obj_t *ais_btn = NULL;
static lv_obj_t *bit_rects[8] = {0};
static bool s_paused = false;

static lv_timer_t *poll_timer      = NULL;
static lv_timer_t *indicator_timer = NULL;
static QueueHandle_t parser_line_q = NULL;
static _Atomic(TaskHandle_t) parser_rx_task = NULL;
static atomic_bool parser_stop = ATOMIC_VAR_INIT(false);
static atomic_uint_fast32_t s_last_rx_ms = ATOMIC_VAR_INIT(0);
static atomic_uchar s_last_rx_byte = ATOMIC_VAR_INIT(0);
static uint32_t s_last_ais_ms = 0;
static uint32_t s_theme_rev = 0;
static uint32_t s_last_panel_flush_ms = 0;
static bool s_bit_indicator_valid = false;
static uint8_t s_bit_indicator_value = 0;
static bool s_port_acquired = false;
static atomic_bool s_hex_mode = ATOMIC_VAR_INIT(false);
static atomic_uint_fast32_t s_display_generation = ATOMIC_VAR_INIT(0);

#define PARSER_LINE_Q_LEN 16
#define PARSER_RX_STOP_WAIT_MS 500

typedef struct {
    rs485_rx_stream_event_kind_t kind;
    uint32_t generation;
    char line[RS485_RX_STREAM_NMEA_CAPACITY];
} parser_line_evt_t;

typedef struct {
    uint32_t generation;
} parser_stream_emit_ctx_t;

/* ─────────────── Forward declarations ─────────────── */
static void   nav_back_cb(lv_event_t *e);
static void   update_bit_indicator(uint8_t b);
static void   poll_cb(lv_timer_t *t);
static void   indicator_timer_cb(lv_timer_t *t);
static void   scr_delete_cb(lv_event_t *e);
static void   parser_rx_task_(void *arg);
static bool   parser_start_runtime_(void);
static bool   parser_stop_runtime_(void);
static void   cb_btn_ais(lv_event_t *e);
static void   cb_btn_pause(lv_event_t *e);
static void   cb_btn_hex(lv_event_t *e);
static void   update_ais_button_(void);
static lv_color_t parser_bit_color_(int state);
static lv_color_t parser_refresh_color_(bool alert);
static bool   parser_prepare_port_(void);
static void   parser_build_screen_(void);
static bool   parser_start_ui_timers_(void);
static void   parser_destroy_screen_(void);
static void   parser_forward_line_to_telnet_(const char *line, size_t len);

// Animate the button flash.
static void anim_refresh_color_cb(void *var, int32_t v) {
    lv_obj_t *btn = (lv_obj_t *)var;
    lv_color_t color;
    color = lv_color_mix(parser_refresh_color_(false), parser_refresh_color_(true), v);
    lv_obj_set_style_bg_color(btn, color, 0);
}
static void cb_btn_refresh(lv_event_t *e) {
    esp_err_t err;

    ESP_LOGI(TAG, "Refresh button pressed");
    refresh_template();
    err = nmea_templates_save_now();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "template save failed: %s", esp_err_to_name(err));
    }
    // Start the flash animation.
    if (refresh_btn) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, refresh_btn);
        lv_anim_set_values(&a, 0, 255); // From turquoise (0) to red (255).
        lv_anim_set_time(&a, 1000); // One second per cycle (three flashes take three seconds).
        lv_anim_set_repeat_count(&a, 3); // Three flashes (0->255->0 is one cycle plus two repeats).
        lv_anim_set_exec_cb(&a, anim_refresh_color_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/* ─────────────── Cleanup when the screen is deleted ─────────────── */
/**
 * @brief LVGL callback invoked when scr_rx is deleted.
 *        Clears all global pointers to screen widgets.
 */
static void scr_delete_cb(lv_event_t *e)
{
    lv_event_code_t code = e ? lv_event_get_code(e) : LV_EVENT_ALL;
    bool runtime_stopped;

    CFG_LOGW(LOG_CFG_RS485_PARSER, TAG,
             "scr_delete code=%d telnet=%d poll=%d ind=%d task=%d q=%d",
             (int)code,
             telnet_server_client_connected() ? 1 : 0,
             poll_timer ? 1 : 0,
             indicator_timer ? 1 : 0,
             atomic_load_explicit(&parser_rx_task, memory_order_acquire) ? 1 : 0,
             parser_line_q ? 1 : 0);
    ESP_LOGI(TAG, "Screen delete event — cleaning up globals");

    runtime_stopped = parser_stop_runtime_();
    if (s_port_acquired && runtime_stopped) {
        if (rs485_release(RS485_OWNER_PARSER) == ESP_OK) {
            s_port_acquired = false;
        }
    } else if (s_port_acquired) {
        ESP_LOGE(TAG, "Parser task did not stop; keeping RS-485 ownership");
    }

    /* Delete timers before clearing widgets so their callbacks cannot run. */
    if (poll_timer) {
        lv_timer_del(poll_timer);
        poll_timer = NULL;
    }
    if (indicator_timer) {
        lv_timer_del(indicator_timer);
        indicator_timer = NULL;
    }

    /* Clear widget pointers; LVGL has already destroyed the objects. */
    for (int i = 0; i < 8; i++) bit_rects[i] = NULL;
    refresh_btn = NULL;
    pause_btn = NULL;
    hex_btn = NULL;
    ais_btn = NULL;
    scr_rx = NULL;

    /* Reset the state machine. */
    atomic_store_explicit(&s_last_rx_byte, 0, memory_order_relaxed);
    atomic_store_explicit(&s_last_rx_ms, 0, memory_order_release);
    s_last_ais_ms = 0;
    s_last_panel_flush_ms = 0;
    s_bit_indicator_valid = false;
    s_bit_indicator_value = 0;
    s_paused = false;
    atomic_store_explicit(&s_hex_mode, false, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_display_generation, 1u, memory_order_acq_rel);
}

static bool parser_start_runtime_(void)
{
    TaskHandle_t created_task = NULL;

    if (!parser_line_q) {
        parser_line_q = xQueueCreate(PARSER_LINE_Q_LEN, sizeof(parser_line_evt_t));
        if (!parser_line_q) {
            ESP_LOGE(TAG, "Failed to create parser queue");
            return false;
        }
    }

    if (!atomic_load_explicit(&parser_rx_task, memory_order_acquire)) {
        atomic_store_explicit(&parser_stop, false, memory_order_release);
        if (xTaskCreatePinnedToCore(parser_rx_task_, "rs485_prx", 4096, NULL, 4,
                                    &created_task, tskNO_AFFINITY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create parser RX task");
            vQueueDelete(parser_line_q);
            parser_line_q = NULL;
            return false;
        }
        atomic_store_explicit(&parser_rx_task, created_task, memory_order_release);
    }

    return true;
}

static bool parser_stop_runtime_(void)
{
    if (atomic_load_explicit(&parser_rx_task, memory_order_acquire)) {
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PARSER_RX_STOP_WAIT_MS);
        atomic_store_explicit(&parser_stop, true, memory_order_release);
        while (atomic_load_explicit(&parser_rx_task, memory_order_acquire)) {
            if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!atomic_load_explicit(&parser_rx_task, memory_order_acquire) && parser_line_q) {
        vQueueDelete(parser_line_q);
        parser_line_q = NULL;
    }
    return !atomic_load_explicit(&parser_rx_task, memory_order_acquire);
}

static void parser_queue_line_(rs485_rx_stream_event_kind_t kind, uint32_t generation,
                               const char *line, size_t len)
{
    parser_line_evt_t evt;

    if (!parser_line_q || !line || len == 0) return;
    if (len >= sizeof(evt.line)) len = sizeof(evt.line) - 1u;

    evt.kind = kind;
    evt.generation = generation;
    memcpy(evt.line, line, len);
    evt.line[len] = '\0';
    (void)xQueueSend(parser_line_q, &evt, 0);
}

static void parser_stream_emit_(rs485_rx_stream_event_kind_t kind,
                                const char *line,
                                size_t len,
                                void *user)
{
    const parser_stream_emit_ctx_t *ctx = user;

    if (!ctx) return;
    parser_queue_line_(kind, ctx->generation, line, len);
    if (kind == RS485_RX_STREAM_EVENT_NMEA && telnet_server_client_connected()) {
        parser_forward_line_to_telnet_(line, len);
    }
}

static void parser_rx_task_(void *arg)
{
    uint8_t buf[128];
    rs485_rx_stream_t stream;
    parser_stream_emit_ctx_t emit_ctx;
    uint32_t active_generation =
        (uint32_t)atomic_load_explicit(&s_display_generation, memory_order_acquire);
    int local_ind_cnt = INDICATOR_SKIP;
    uint32_t last_diag_ms = 0;

    (void)arg;
    rs485_rx_stream_init(
        &stream, atomic_load_explicit(&s_hex_mode, memory_order_acquire));

    while (!atomic_load_explicit(&parser_stop, memory_order_acquire)) {
        size_t n = sizeof(buf);
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000ULL);

        if (rs485_driver_read_timeout(buf, &n, RX_WAIT_MS) != ESP_OK || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
            continue;
        }

        uint32_t t1 = (uint32_t)(esp_timer_get_time() / 1000ULL);
        const uint32_t generation =
            (uint32_t)atomic_load_explicit(&s_display_generation, memory_order_acquire);
        const bool hex_mode = atomic_load_explicit(&s_hex_mode, memory_order_acquire);

        if (generation != active_generation) {
            rs485_rx_stream_reset(&stream, hex_mode);
            active_generation = generation;
        }

        if (t1 - last_diag_ms >= 5000) {
            ESP_LOGI(TAG, "RX diag: read %u bytes in %" PRIu32 " ms, local_len=%u, q_spaces=%d",
                     (unsigned)n, t1 - t0, (unsigned)stream.nmea_len,
                     parser_line_q ? (int)uxQueueSpacesAvailable(parser_line_q) : -1);
            last_diag_ms = t1;
        }

        for (size_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];

            if (++local_ind_cnt >= INDICATOR_SKIP) {
                local_ind_cnt = 0;
                atomic_store_explicit(&s_last_rx_byte, c, memory_order_relaxed);
                atomic_store_explicit(
                    &s_last_rx_ms,
                    (uint32_t)(esp_timer_get_time() / 1000ULL),
                    memory_order_release);
            }
        }

        emit_ctx.generation = active_generation;
        rs485_rx_stream_feed(&stream, buf, n, parser_stream_emit_, &emit_ctx);
    }

    atomic_store_explicit(&parser_rx_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

static void parser_forward_line_to_telnet_(const char *line, size_t len)
{
    uint8_t frame[RS485_RX_STREAM_NMEA_CAPACITY + 2u];

    if (!line || len == 0 || len > RS485_RX_STREAM_NMEA_CAPACITY) return;
    memcpy(frame, line, len);
    frame[len] = '\r';
    frame[len + 1] = '\n';
    (void)telnet_server_send(frame, len + 2u);
}

static bool parser_prepare_port_(void)
{
    if (rs485_acquire(RS485_OWNER_PARSER, rs485_get_baudrate()) != ESP_OK) {
        ESP_LOGE(TAG, "RS-485 init failed");
        return false;
    }
    s_port_acquired = true;
    return true;
}

static void parser_build_screen_(void)
{
    atomic_store_explicit(&s_hex_mode, false, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_display_generation, 1u, memory_order_acq_rel);

    scr_rx = lv_obj_create(NULL);
    lv_obj_clear_flag(scr_rx, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_apply_screen(scr_rx);
    lv_obj_add_event_cb(scr_rx, scr_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *bar = lv_obj_create(scr_rx);
    lv_obj_set_size(bar, LCD_WIDTH, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_gap(bar, 0, 0);
    ui_theme_apply_card(bar);

    lv_obj_t *baud_dd = ui_baud_selector_create(bar);
    lv_obj_set_width(baud_dd, WIDTH_BAUDSEL);
    lv_obj_align(baud_dd, LV_ALIGN_LEFT_MID, 0, 2);

    instrument_panel_init(scr_rx, 0, TOP_BAR_H, LCD_WIDTH, PANEL_H);
    nmea_log_init(scr_rx, 0, TOP_BAR_H + PANEL_H, LCD_WIDTH, LCD_HEIGHT - TOP_BAR_H - PANEL_H);
    instrument_panel_flush();
    s_last_panel_flush_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    for (int i = 0; i < 8; i++) {
        bit_rects[i] = lv_obj_create(bar);
        lv_obj_set_size(bit_rects[i], INDICATOR_W, INDICATOR_H);
        lv_obj_set_style_radius(bit_rects[i], 3, 0);
        lv_obj_set_style_border_width(bit_rects[i], 2, 0);
        lv_obj_set_style_border_color(bit_rects[i], lv_color_hex(ui_theme_get()->bit_border), 0);
        lv_obj_set_style_bg_color(bit_rects[i], parser_bit_color_(0), 0);
        lv_obj_align(bit_rects[i], LV_ALIGN_TOP_LEFT,
                     INDICATOR_X + i * (INDICATOR_W + INDICATOR_SPACING), INDICATOR_Y);
    }

    dialog_ui_create_button(scr_rx,
                            LCD_WIDTH - PARSER_NAV_BTN_SZ - 8,
                            (TOP_BAR_H - PARSER_NAV_BTN_SZ) / 2,
                            PARSER_NAV_BTN_SZ, PARSER_NAV_BTN_SZ,
                            LV_SYMBOL_HOME, ui_theme_nav_home_hex(),
                            nav_back_cb, NULL, PARSER_NAV_BTN_FONT);
    refresh_btn = dialog_ui_create_button(scr_rx,
                                          LCD_WIDTH - PARSER_NAV_BTN_SZ - 8 - PARSER_NAV_BTN_SZ - 8,
                                          (TOP_BAR_H - PARSER_NAV_BTN_SZ) / 2,
                                          PARSER_NAV_BTN_SZ, PARSER_NAV_BTN_SZ,
                                          LV_SYMBOL_REFRESH, ui_theme_refresh_hex(false),
                                          cb_btn_refresh, NULL, PARSER_NAV_BTN_FONT);
    pause_btn = dialog_ui_create_button(scr_rx,
                                        LCD_WIDTH - PARSER_NAV_BTN_SZ - 8 - PARSER_NAV_BTN_SZ - 8 - PARSER_NAV_BTN_SZ - 8,
                                        (TOP_BAR_H - PARSER_NAV_BTN_SZ) / 2,
                                        PARSER_NAV_BTN_SZ, PARSER_NAV_BTN_SZ,
                                        LV_SYMBOL_PAUSE, ui_theme_pause_hex(false),
                                        cb_btn_pause, NULL, PARSER_NAV_BTN_FONT);
    hex_btn = dialog_ui_create_button(scr_rx,
                                      LCD_WIDTH - PARSER_NAV_BTN_SZ - 8 - PARSER_NAV_BTN_SZ - 8 -
                                          PARSER_NAV_BTN_SZ - 8 - PARSER_NAV_BTN_SZ - 8,
                                      (TOP_BAR_H - PARSER_NAV_BTN_SZ) / 2,
                                      PARSER_NAV_BTN_SZ, PARSER_NAV_BTN_SZ,
                                      "HEX", ui_theme_pause_hex(false),
                                      cb_btn_hex, NULL, PARSER_HEX_BTN_FONT);
    lv_obj_add_flag(hex_btn, LV_OBJ_FLAG_CHECKABLE);
    dialog_ui_apply_checked_style(hex_btn, ui_theme_pause_hex(true));
    ais_btn = dialog_ui_create_button(scr_rx, AIS_BTN_X, AIS_BTN_Y, AIS_BTN_W, AIS_BTN_H,
                                      "AIS DETECTED", ui_theme_ais_hex(),
                                      cb_btn_ais, NULL, &lv_font_montserrat_16);
    lv_obj_add_flag(ais_btn, LV_OBJ_FLAG_HIDDEN);
    s_bit_indicator_valid = false;
}

static bool parser_start_ui_timers_(void)
{
    poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
    indicator_timer = lv_timer_create(indicator_timer_cb, 20, NULL);
    return poll_timer && indicator_timer;
}

static void parser_destroy_screen_(void)
{
    if (!scr_rx || !lv_obj_is_valid(scr_rx)) return;
    CFG_LOGW(LOG_CFG_RS485_PARSER, TAG,
             "parser_destroy_screen telnet=%d task=%d q=%d",
             telnet_server_client_connected() ? 1 : 0,
             atomic_load_explicit(&parser_rx_task, memory_order_acquire) ? 1 : 0,
             parser_line_q ? 1 : 0);
    lv_obj_del(scr_rx);
}

/* ─────────────── UI creation ─────────────── */
lv_obj_t *rs485_parser_create(lv_obj_t *scr_main)
{
    if (scr_rx && s_theme_rev != ui_theme_get_revision()) {
        CFG_LOGW(LOG_CFG_RS485_PARSER, TAG,
                 "parser_recreate_due_theme old_rev=%" PRIu32 " new_rev=%" PRIu32,
                 s_theme_rev, ui_theme_get_revision());
        parser_destroy_screen_();
    }
    if (scr_rx) {
        CFG_LOGI(LOG_CFG_RS485_PARSER, TAG,
                 "parser_reuse_screen telnet=%d", telnet_server_client_connected() ? 1 : 0);
        lv_scr_load(scr_rx);
        return scr_rx;
    }

    (void)scr_main;

    if (!parser_prepare_port_()) return NULL;

    parser_build_screen_();

    if (!parser_start_runtime_()) {
        parser_destroy_screen_();
        return NULL;
    }
    if (!parser_start_ui_timers_()) {
        parser_destroy_screen_();
        return NULL;
    }

    ESP_LOGI(TAG, "Parser screen created with bit indicator and NMEA log.");
    CFG_LOGI(LOG_CFG_RS485_PARSER, TAG,
             "parser_create_ok telnet=%d task=%d q=%d",
             telnet_server_client_connected() ? 1 : 0,
             atomic_load_explicit(&parser_rx_task, memory_order_acquire) ? 1 : 0,
             parser_line_q ? 1 : 0);
    s_theme_rev = ui_theme_get_revision();
    lv_scr_load(scr_rx);
    return scr_rx;
}

/* ─────────────── Poll UART + FSM ─────────────── */
static uint32_t s_poll_diag_ms = 0;
static void poll_cb(lv_timer_t *t)
{
    parser_line_evt_t evt;
    int count = 0;
    bool panel_dirty = false;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000ULL);
    const bool hex_mode = atomic_load_explicit(&s_hex_mode, memory_order_acquire);
    const uint32_t generation =
        (uint32_t)atomic_load_explicit(&s_display_generation, memory_order_acquire);

    (void)t;
    if (!scr_rx) return;

    while (parser_line_q && xQueueReceive(parser_line_q, &evt, 0) == pdTRUE) {
        if (evt.generation != generation) {
            continue;
        }

        if (evt.kind == RS485_RX_STREAM_EVENT_HEX) {
            if (!s_paused && hex_mode) {
                nmea_log_add_hex_line(evt.line);
            }
        } else {
            if (!s_paused) {
                if (!hex_mode) {
                    nmea_log_add(evt.line);
                }
                instrument_panel_process(evt.line);
                panel_dirty = true;
            }
            if (aisdecoder_is_ais_sentence(evt.line)) {
                s_last_ais_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                aisdecoder_feed_nmea_line(evt.line);
            }
        }
        count++;
    }

    uint32_t t1 = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!s_paused && (panel_dirty || (t1 - s_last_panel_flush_ms) >= PANEL_IDLE_FLUSH_MS)) {
        instrument_panel_flush();
        s_last_panel_flush_ms = t1;
    }
    if (t1 - s_poll_diag_ms >= 5000) {
        ESP_LOGI(TAG, "poll diag: %d lines in %" PRIu32 "ms (q_left=%d)",
                 count, t1 - t0,
                 parser_line_q ? (int)uxQueueMessagesWaiting(parser_line_q) : -1);
        s_poll_diag_ms = t1;
    }

    update_ais_button_();
}

/* ─────────────── Indicator fade-out ─────────────── */
static void indicator_timer_cb(lv_timer_t *t)
{
    const uint32_t last_rx_ms =
        (uint32_t)atomic_load_explicit(&s_last_rx_ms, memory_order_acquire);

    (void)t;
    if (!scr_rx) return;  /* guard */
    if (last_rx_ms == 0) {
        update_bit_indicator(0x00);
        return;
    }
    if (((uint32_t)(esp_timer_get_time() / 1000ULL) - last_rx_ms) > FADE_OFF_MS) {
        update_bit_indicator(0x00);
    } else {
        update_bit_indicator(atomic_load_explicit(&s_last_rx_byte, memory_order_relaxed));
    }
}

/* ─────────────── Update bit indicator ─────────────── */
static void update_bit_indicator(uint8_t b)
{
    if (s_bit_indicator_valid && s_bit_indicator_value == b) {
        return;
    }
    s_bit_indicator_valid = true;
    s_bit_indicator_value = b;

    for (int i = 0; i < 8; i++) {
        if (!bit_rects[i]) continue;  /* guard */
        lv_color_t c = (b == 0) ? parser_bit_color_(0)
            : (((b >> (7 - i)) & 1)
               ? parser_bit_color_(1)
               : parser_bit_color_(2));
        lv_obj_set_style_bg_color(bit_rects[i], c, 0);
    }
}

static lv_color_t parser_bit_color_(int state)
{
    const ui_theme_palette_t *th = ui_theme_get();

    switch (state) {
        case 1: return lv_color_hex(th->bit_on);
        case 2: return lv_color_hex(th->bit_off);
        default: return lv_color_hex(th->bit_idle);
    }
}

static lv_color_t parser_refresh_color_(bool alert)
{
    const ui_theme_palette_t *th = ui_theme_get();
    return lv_color_hex(alert ? th->refresh_alert : th->refresh);
}

/* ─────────────── Navigation back ─────────────── */
static void nav_back_cb(lv_event_t *e)
{
    CFG_LOGW(LOG_CFG_RS485_PARSER, TAG,
             "parser_nav_back telnet=%d task=%d q=%d",
             telnet_server_client_connected() ? 1 : 0,
             atomic_load_explicit(&parser_rx_task, memory_order_acquire) ? 1 : 0,
             parser_line_q ? 1 : 0);
    /*
     * Load the main screen first, then delete the current screen asynchronously.
     * scr_delete_cb finishes cleaning up runtime state and pointers.
     */
    screen_ui_show();

    if (scr_rx) {
        lv_obj_t *old = scr_rx;
        scr_rx = NULL;
        lv_obj_del_async(old);
    }

    ESP_LOGI(TAG, "Returned to main screen.");
}

static void cb_btn_ais(lv_event_t *e)
{
    (void)e;
    if (!scr_rx) return;
    (void)aisdecoder_create(scr_rx);
}

static void update_ais_button_(void)
{
    uint32_t now_ms;
    bool show;

    if (!ais_btn || !lv_obj_is_valid(ais_btn)) return;

    now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    show = (s_last_ais_ms != 0) && ((now_ms - s_last_ais_ms) <= AIS_DETECT_HOLD_MS);

    if (show) {
        lv_obj_clear_flag(ais_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ais_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ─────────────── Pause button ─────────────── */
static void cb_btn_pause(lv_event_t *e)
{
    (void)e;
    s_paused = !s_paused;

    if (!pause_btn) return;

    if (s_paused) {
        lv_obj_set_style_bg_color(pause_btn, lv_color_hex(ui_theme_pause_hex(true)), 0);
        lv_label_set_text(lv_obj_get_child(pause_btn, 0), LV_SYMBOL_PLAY);
        ESP_LOGI(TAG, "Output PAUSED");
    } else {
        lv_obj_set_style_bg_color(pause_btn, lv_color_hex(ui_theme_pause_hex(false)), 0);
        lv_label_set_text(lv_obj_get_child(pause_btn, 0), LV_SYMBOL_PAUSE);
        ESP_LOGI(TAG, "Output RESUMED");
    }
}

static void cb_btn_hex(lv_event_t *e)
{
    (void)e;
    const bool enabled = !atomic_load_explicit(&s_hex_mode, memory_order_acquire);

    atomic_store_explicit(&s_hex_mode, enabled, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_display_generation, 1u, memory_order_acq_rel);
    nmea_log_set_raw_mode(enabled);
    nmea_log_clear();

    if (hex_btn) {
        if (enabled) {
            lv_obj_add_state(hex_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(hex_btn, LV_STATE_CHECKED);
        }
    }
    ESP_LOGI(TAG, "HEX output %s", enabled ? "ENABLED" : "DISABLED");
}

/**
 * @brief Resume the parser, recreating its timers if they were deleted.
 */
void rs485_parser_resume(void)
{
    if (!scr_rx) return;
    (void)parser_start_runtime_();

    /* Restart poll timer if deleted */
    if (!poll_timer) {
        poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
        ESP_LOGI(TAG, "Poll timer restarted");
    }

    /* Restart indicator timer if deleted */
    if (!indicator_timer) {
        indicator_timer = lv_timer_create(indicator_timer_cb, 5, NULL);
        ESP_LOGI(TAG, "Indicator timer restarted");
    }
}

void rs485_parser_flush_lines(void)
{
    if (poll_timer) {
        poll_cb(poll_timer);
    }
}
