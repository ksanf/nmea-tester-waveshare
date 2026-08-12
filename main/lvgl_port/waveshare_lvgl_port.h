/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "config/lvgl_config.h"

/* Keep the Waveshare port and RGB panel on one project-wide configuration. */
#define CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_ENABLE  LVGL_AVOID_TEARING
#if LVGL_AVOID_TEARING
    #if LVGL_DIRECT_MODE && \
        ((LCD_RGB_FB_COUNT == 2) || \
         (LCD_RUNTIME_ROTATION_180 && (LCD_RGB_FB_COUNT == 3)))
        #define CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE 3
    #elif LVGL_FULL_REFRESH && (LCD_RGB_FB_COUNT == 2)
        #define CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE 1
    #elif LVGL_FULL_REFRESH && (LCD_RGB_FB_COUNT == 3)
        #define CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE 2
    #else
        #error "Selected LVGL mode is not supported by the Waveshare RGB port"
    #endif
#else
    #define CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE 0
#endif
#define CONFIG_EXAMPLE_LVGL_PORT_ROTATION_DEGREE    0
#define LVGL_PORT_RUNTIME_ROTATION_180               LCD_RUNTIME_ROTATION_180
#define CONFIG_EXAMPLE_LVGL_PORT_BUF_PSRAM          LVGL_BUFF_SPIRAM
#define CONFIG_EXAMPLE_LVGL_PORT_BUF_INTERNAL       (!LVGL_BUFF_SPIRAM)
#define CONFIG_EXAMPLE_LVGL_PORT_BUF_HEIGHT         LVGL_DRAW_BUFFER_LINES
#define CONFIG_EXAMPLE_LVGL_PORT_TICK               LVGL_TICK_PERIOD_MS
#define CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS  LVGL_TASK_MAX_SLEEP_MS
#define CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS  5
#define CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB (LVGL_TASK_STACK_SIZE / 1024)
#define CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY      LVGL_TASK_PRIORITY
#define CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE          LVGL_TASK_CORE

#include <stdbool.h>
#include <stdint.h>

#include "config/config_nmea_tester.h"

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_PANEL_USE_1024_600_LCD           ((LCD_WIDTH == 1024) && (LCD_HEIGHT == 600))
#define CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911 0

#define LVGL_PORT_H_RES             (LCD_WIDTH)
#define LVGL_PORT_V_RES             (LCD_HEIGHT)
#define LVGL_PORT_TICK_PERIOD_MS    (CONFIG_EXAMPLE_LVGL_PORT_TICK)

/**
 * LVGL timer handle task related parameters, can be adjusted by users
 *
 */
#define LVGL_PORT_TASK_MAX_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MAX_DELAY_MS)    // The maximum delay of the LVGL timer task, in milliseconds
#define LVGL_PORT_TASK_MIN_DELAY_MS (CONFIG_EXAMPLE_LVGL_PORT_TASK_MIN_DELAY_MS)    // The minimum delay of the LVGL timer task, in milliseconds
#define LVGL_PORT_TASK_STACK_SIZE   (CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB * 1024) // The stack size of the LVGL timer task, in bytes
#define LVGL_PORT_TASK_PRIORITY     (CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY)        // The priority of the LVGL timer task
#define LVGL_PORT_TASK_CORE         (CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE)            // The core of the LVGL timer task,
// `-1` means the don't specify the core
/**
 *
 * LVGL buffer related parameters, can be adjusted by users:
 *  (These parameters will be useless if the avoid tearing function is enabled)
 *
 *  - Memory type for buffer allocation:
 *      - MALLOC_CAP_SPIRAM: Allocate LVGL buffer in PSRAM
 *      - MALLOC_CAP_INTERNAL: Allocate LVGL buffer in SRAM
 *      (The SRAM is faster than PSRAM, but the PSRAM has a larger capacity)
 *
 */
#if CONFIG_EXAMPLE_LVGL_PORT_BUF_PSRAM
#define LVGL_PORT_BUFFER_MALLOC_CAPS    (MALLOC_CAP_SPIRAM)
#elif CONFIG_EXAMPLE_LVGL_PORT_BUF_INTERNAL
#define LVGL_PORT_BUFFER_MALLOC_CAPS    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
#define LVGL_PORT_BUFFER_HEIGHT         (CONFIG_EXAMPLE_LVGL_PORT_BUF_HEIGHT)

/**
 * Avoid tering related configurations, can be adjusted by users.
 *
 */
#define LVGL_PORT_AVOID_TEAR_ENABLE     (CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_ENABLE) // Set to 1 to enable
#if LVGL_PORT_AVOID_TEAR_ENABLE
/**
 * Set the avoid tearing mode:
 *      - 0: Disable avoid tearing function
 *      - 1: LCD double-buffer & LVGL full-refresh
 *      - 2: LCD triple-buffer & LVGL full-refresh
 *      - 3: LCD double-buffer & LVGL direct-mode (recommended)
 *
 */
#define LVGL_PORT_AVOID_TEAR_MODE       (CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE)

/**
 * Set the rotation degree of the LCD panel when the avoid tearing function is enabled:
 *      - 0: 0 degree
 *      - 90: 90 degree
 *      - 180: 180 degree
 *      - 270: 270 degree
 *
 */
#define EXAMPLE_LVGL_PORT_ROTATION_DEGREE  (CONFIG_EXAMPLE_LVGL_PORT_ROTATION_DEGREE)
#define LVGL_PORT_ROTATION_COPY_ENABLE     \
    ((EXAMPLE_LVGL_PORT_ROTATION_DEGREE != 0) || \
     (LVGL_PORT_DIRECT_MODE && LVGL_PORT_RUNTIME_ROTATION_180))

/**
 * Below configurations are automatically set according to the above configurations, users do not need to modify them.
 *
 */
#if LVGL_PORT_AVOID_TEAR_MODE == 1
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (2)
#define LVGL_PORT_FULL_REFRESH          (1)
#elif LVGL_PORT_AVOID_TEAR_MODE == 2
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (3)
#define LVGL_PORT_FULL_REFRESH          (1)
#elif LVGL_PORT_AVOID_TEAR_MODE == 3
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (2)
#define LVGL_PORT_DIRECT_MODE           (1)
#endif /* LVGL_PORT_AVOID_TEAR_MODE */

#if !LVGL_PORT_ROTATION_COPY_ENABLE
#define EXAMPLE_LVGL_PORT_ROTATION_0    (1)
#else
#if EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 90
#define EXAMPLE_LVGL_PORT_ROTATION_90   (1)
#elif EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 180
#define EXAMPLE_LVGL_PORT_ROTATION_180  (1)
#elif EXAMPLE_LVGL_PORT_ROTATION_DEGREE == 270
#define EXAMPLE_LVGL_PORT_ROTATION_270  (1)
#endif
#ifdef LVGL_PORT_LCD_RGB_BUFFER_NUMS
#undef LVGL_PORT_LCD_RGB_BUFFER_NUMS
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (3)
#endif
#endif /* EXAMPLE_LVGL_PORT_ROTATION_DEGREE */
#else
#define LVGL_PORT_LCD_RGB_BUFFER_NUMS   (1)
#define LVGL_PORT_FULL_REFRESH          (0)
#define LVGL_PORT_DIRECT_MODE           (0)
#endif /* LVGL_PORT_AVOID_TEAR_ENABLE */

#if LVGL_PORT_LCD_RGB_BUFFER_NUMS != LCD_RGB_FB_COUNT
#error "LVGL and RGB panel framebuffer counts differ"
#endif
#if LVGL_PORT_FULL_REFRESH != LVGL_FULL_REFRESH
#error "LVGL full-refresh configuration differs between port and panel"
#endif
#if LVGL_PORT_DIRECT_MODE != LVGL_DIRECT_MODE
#error "LVGL direct-mode configuration differs between port and panel"
#endif

/**
 * @brief Initialize LVGL port
 *
 * @param[in] lcd_handle: LCD panel handle
 * @param[in] tp_handle: Touch panel handle
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - Others: Fail
 */
esp_err_t lvgl_port_init(esp_lcd_panel_handle_t lcd_handle, esp_lcd_touch_handle_t tp_handle);

/**
 * @brief Select normal or 180-degree display orientation.
 *
 * The caller must hold the LVGL lock after lvgl_port_init() has completed.
 * LVGL applies the matching pointer-coordinate transform automatically.
 */
esp_err_t lvgl_port_set_rotation_180(bool enabled);

/**
 * @brief Take LVGL mutex
 *
 * @param[in] timeout_ms: Timeout in [ms]. 0 will block indefinitely.
 *
 * @return
 *      - true:  Mutex was taken
 *      - false: Mutex was NOT taken
 */
bool lvgl_port_lock(int timeout_ms);

/**
 * @brief Give LVGL mutex
 *
 */
void lvgl_port_unlock(void);

/**
 * @brief Notifies the LVGL task when the transmission of the RGB frame buffer is completed.
 *
 * @return
 *      - true:  The tasks need to be re-scheduled
 *      - false: The tasks don't need to be re-scheduled
 */
bool lvgl_port_notify_rgb_vsync(void);

#ifdef __cplusplus
}
#endif
