/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   RGB LCD + LVGL init. Uses proven Waveshare demo port.
 */

#include "ui/screens/screen_init.h"
#include "config/config_pins.h"
#include "config/config_nmea_tester.h"
#include "config/lvgl_config.h"
#include "system/board_waveshare.h"

#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_SCREEN_INIT
#include "config_logs.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "lvgl_port/waveshare_lvgl_port.h"

#include "esp_heap_caps.h"
#include "lvgl.h"

/* ─── Waveshare panel config ─────────────────────────────────────── */
/* LCD_RGB_PIXEL_CLOCK_HZ from config_nmea_tester.h */
#define EXAMPLE_LCD_H_RES               LCD_WIDTH
#define EXAMPLE_LCD_V_RES               LCD_HEIGHT
#define EXAMPLE_PIN_NUM_RGB_DATA0       PIN_LCD_B0
#define EXAMPLE_PIN_NUM_RGB_DATA1       PIN_LCD_B1
#define EXAMPLE_PIN_NUM_RGB_DATA2       PIN_LCD_B2
#define EXAMPLE_PIN_NUM_RGB_DATA3       PIN_LCD_B3
#define EXAMPLE_PIN_NUM_RGB_DATA4       PIN_LCD_B4
#define EXAMPLE_PIN_NUM_RGB_DATA5       PIN_LCD_G0
#define EXAMPLE_PIN_NUM_RGB_DATA6       PIN_LCD_G1
#define EXAMPLE_PIN_NUM_RGB_DATA7       PIN_LCD_G2
#define EXAMPLE_PIN_NUM_RGB_DATA8       PIN_LCD_G3
#define EXAMPLE_PIN_NUM_RGB_DATA9       PIN_LCD_G4
#define EXAMPLE_PIN_NUM_RGB_DATA10      PIN_LCD_G5
#define EXAMPLE_PIN_NUM_RGB_DATA11      PIN_LCD_R0
#define EXAMPLE_PIN_NUM_RGB_DATA12      PIN_LCD_R1
#define EXAMPLE_PIN_NUM_RGB_DATA13      PIN_LCD_R2
#define EXAMPLE_PIN_NUM_RGB_DATA14      PIN_LCD_R3
#define EXAMPLE_PIN_NUM_RGB_DATA15      PIN_LCD_R4
#define EXAMPLE_LCD_IO_RGB_VSYNC        PIN_LCD_VSYNC
#define EXAMPLE_LCD_IO_RGB_HSYNC        PIN_LCD_HSYNC
#define EXAMPLE_LCD_IO_RGB_DE           PIN_LCD_DE
#define EXAMPLE_LCD_IO_RGB_PCLK         PIN_LCD_PCLK
#define EXAMPLE_LCD_IO_RGB_DISP         -1

static const char *TAG = "SCREEN_INIT";

/* ─── VSYNC shim matching the reference demo ─────────────────────── */
IRAM_ATTR static bool rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel,
                                              const esp_lcd_rgb_panel_event_data_t *edata,
                                              void *user_ctx)
{
    return lvgl_port_notify_rgb_vsync();
}

static void log_meminfo(const char *stage)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "[%s] INTERNAL free=%zu largest=%zu",
             stage, info.total_free_bytes, info.largest_free_block);
}

void screen_init(void)
{
    ESP_LOGI(TAG, "=== SCREEN_INIT START (demo port) ===");
    log_meminfo("START");

    ESP_ERROR_CHECK(waveshare_board_init());
    log_meminfo("board_init");

    /* ── RGB panel ── */
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = LCD_RGB_DATA_WIDTH,
        .bits_per_pixel = LCD_RGB_BITS_PER_PIXEL,
        .num_fbs = LCD_RGB_FB_COUNT,
        .psram_trans_align = LCD_RGB_PSRAM_TRANS_ALIGN,
        .sram_trans_align = LCD_RGB_SRAM_TRANS_ALIGN,
        .bounce_buffer_size_px = LCD_RGB_BOUNCE_BUFFER_PIXELS,
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,
        .data_gpio_nums = {
            EXAMPLE_PIN_NUM_RGB_DATA0,  EXAMPLE_PIN_NUM_RGB_DATA1,
            EXAMPLE_PIN_NUM_RGB_DATA2,  EXAMPLE_PIN_NUM_RGB_DATA3,
            EXAMPLE_PIN_NUM_RGB_DATA4,  EXAMPLE_PIN_NUM_RGB_DATA5,
            EXAMPLE_PIN_NUM_RGB_DATA6,  EXAMPLE_PIN_NUM_RGB_DATA7,
            EXAMPLE_PIN_NUM_RGB_DATA8,  EXAMPLE_PIN_NUM_RGB_DATA9,
            EXAMPLE_PIN_NUM_RGB_DATA10, EXAMPLE_PIN_NUM_RGB_DATA11,
            EXAMPLE_PIN_NUM_RGB_DATA12, EXAMPLE_PIN_NUM_RGB_DATA13,
            EXAMPLE_PIN_NUM_RGB_DATA14, EXAMPLE_PIN_NUM_RGB_DATA15,
        },
        .timings = {
            .pclk_hz = LCD_RGB_PIXEL_CLOCK_HZ,
            .h_res = EXAMPLE_LCD_H_RES,
            .v_res = EXAMPLE_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = LCD_RGB_PCLK_ACTIVE_NEG,
        },
        .flags.fb_in_psram = LCD_RGB_FB_IN_PSRAM,
    };

    ESP_LOGI(TAG, "RGB panel %dx%d @ %d Hz, fb=%d bounce=%d",
             LCD_WIDTH, LCD_HEIGHT, LCD_RGB_PIXEL_CLOCK_HZ,
             (int)LCD_RGB_FB_COUNT, (int)LCD_RGB_BOUNCE_BUFFER_LINES);

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_cfg, &panel));
    log_meminfo("rgb_panel_new");

    ESP_ERROR_CHECK(waveshare_lcd_reset());
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    log_meminfo("panel_init");

    /* ── VSYNC callback: install before LVGL task can flush. ── */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = rgb_lcd_on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL));

    /* ── LVGL via demo port; touch is registered by nmea_touch_init(). ── */
    ESP_LOGI(TAG, "Init LVGL (demo port)...");
    ESP_ERROR_CHECK(lvgl_port_init(panel, NULL));
    log_meminfo("lvgl_port_init");

    ESP_ERROR_CHECK(waveshare_lcd_set_backlight(true));
    ESP_LOGI(TAG, "Display ready.");
    log_meminfo("DONE");
}

lv_disp_t *screen_get_display(void) {
    return lv_disp_get_default();
}
