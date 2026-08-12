/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_NMEA_TESTER_H
#define CONFIG_NMEA_TESTER_H

/* ─── LCD/Touch panel profile ───────────────────────────────────────── */
#define PANEL_PROFILE_4IN_480X320  1
#define PANEL_PROFILE_35IN_480X320 2
#define PANEL_PROFILE_WAVESHARE_5_800X480 3
#define PANEL_PROFILE_WAVESHARE_5B_1024X600 4

/* Active hardware profile.
 * 4.0" original module: display uses swap_xy, touch uses swapped axes.
 * 3.5" replacement module: display/touch work without swap_xy.
 * Waveshare 5: 800x480, Waveshare 5B: 1024x600. */
#define PANEL_PROFILE PANEL_PROFILE_WAVESHARE_5_800X480

#if PANEL_PROFILE == PANEL_PROFILE_4IN_480X320
    #define LCD_WIDTH 480
    #define LCD_HEIGHT 320

    #define LCD_ROT_SWAP_XY  1
    #define LCD_ROT_MIRROR_X 0
    #define LCD_ROT_MIRROR_Y 1

    #define TOUCH_X_MAX      LCD_HEIGHT
    #define TOUCH_Y_MAX      LCD_WIDTH
    #define TOUCH_SWAP_XY    1
    #define TOUCH_MIRROR_X   0
    #define TOUCH_MIRROR_Y   0
#elif PANEL_PROFILE == PANEL_PROFILE_35IN_480X320
    #define LCD_WIDTH 480
    #define LCD_HEIGHT 320

    #define LCD_ROT_SWAP_XY  1
    #define LCD_ROT_MIRROR_X 1
    #define LCD_ROT_MIRROR_Y 0

    #define TOUCH_X_MAX      LCD_WIDTH
    #define TOUCH_Y_MAX      LCD_HEIGHT
    #define TOUCH_SWAP_XY    0
    #define TOUCH_MIRROR_X   1
    #define TOUCH_MIRROR_Y   1
#elif PANEL_PROFILE == PANEL_PROFILE_WAVESHARE_5_800X480
    #define LCD_WIDTH 800
    #define LCD_HEIGHT 480
    #define LCD_RGB_PIXEL_CLOCK_HZ (16 * 1000 * 1000)

    #define LCD_ROT_SWAP_XY  0
    #define LCD_ROT_MIRROR_X 0
    #define LCD_ROT_MIRROR_Y 0

    #define TOUCH_X_MAX      LCD_WIDTH
    #define TOUCH_Y_MAX      LCD_HEIGHT
    #define TOUCH_SWAP_XY    0
    #define TOUCH_MIRROR_X   0
    #define TOUCH_MIRROR_Y   0
#elif PANEL_PROFILE == PANEL_PROFILE_WAVESHARE_5B_1024X600
    #define LCD_WIDTH 1024
    #define LCD_HEIGHT 600
    #define LCD_RGB_PIXEL_CLOCK_HZ (21 * 1000 * 1000)

    #define LCD_ROT_SWAP_XY  0
    #define LCD_ROT_MIRROR_X 0
    #define LCD_ROT_MIRROR_Y 0

    #define TOUCH_X_MAX      LCD_WIDTH
    #define TOUCH_Y_MAX      LCD_HEIGHT
    #define TOUCH_SWAP_XY    0
    #define TOUCH_MIRROR_X   0
    #define TOUCH_MIRROR_Y   0
#else
    #error "Unsupported PANEL_PROFILE"
#endif

/* Keep one logical LVGL framebuffer and two RGB output framebuffers so the
 * display can switch between 0 and 180 degrees at runtime in direct mode. */
#define LCD_RUNTIME_ROTATION_180 1

#define LCD_DRAW_LINES 16
#define RS485_UART_PORT UART_NUM_2
#define RS485_BAUD_DEFAULT 4800
#define WIFI_AP_ENABLED 1
#define WIFI_AP_DEFAULT_ENABLED 0
#define WIFI_AP_SSID "nmeatester"
/* WPA2/WPA3 SoftAP requires at least 8 characters.
 * If you need exactly "1234", the AP must be open. */
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONNECTIONS 1
#define WIFI_AP_B_ONLY 1
#define WIFI_AP_INACTIVE_TIMEOUT_SEC 600
#define WIFI_TELNET_ENABLED 1
#define WIFI_TELNET_PORT 23
#define WIFI_UDP_NMEA_ENABLED 1
#define WIFI_UDP_NMEA_PORT 10110

/* Public NMEA 2000 identity placeholders.
 * Assign a registered manufacturer code and a unique device ID/serial before
 * deploying more than one unit on the same network. */
#ifndef NM2K_MANUFACTURER_CODE
#define NM2K_MANUFACTURER_CODE 2046u
#endif
#ifndef NM2K_DEVICE_UNIQUE_ID
#define NM2K_DEVICE_UNIQUE_ID 1u
#endif
#ifndef NM2K_DEVICE_SERIAL
#define NM2K_DEVICE_SERIAL "NM2K0001"
#endif

#define FONT_WIDTH 8
#define FONT_HEIGHT 8
#define LINE_SPACING 2

/* Terminal tunnel TX mode:
 * 0 -> send UART->CAN as single-byte A0 blocks
 * 1 -> coalesce short bursts before CAN send */
#define TERM_INPUT_TX_MODE_CHAR  0
#define TERM_INPUT_TX_MODE_BURST 1
#define TERM_INPUT_TX_MODE       TERM_INPUT_TX_MODE_CHAR


#define COLOR_BUTTON_HOME 0xFFBF00
#define COLOR_BUTTON_SETTINGS 0x50C878
/* Main text area. */
#define TEXT_WINDOW_X 0
#define TEXT_WINDOW_Y 100
#define TEXT_WINDOW_W LCD_WIDTH
#define TEXT_WINDOW_H (LCD_HEIGHT - TEXT_WINDOW_Y)
#define TEXT_FG_COLOR GFX_COLOR_WHITE
#define TEXT_BG_COLOR GFX_COLOR_BLACK

#define TOUCH_POLL_STACK_SIZE 4096 /* Touch polling task stack size. */
#define UI_LONG_PRESS_TIME_MS 600
#define UI_LONG_PRESS_REPEAT_MS 250
/* ─── Time source for the NMEA clock ─────────────────────────────────
 * 0 -> use time()/SNTP (CPU RTC)
 * 1 -> use the external hardware RTC (rtc_get_time_tm())
 */
#define NMEA_CLOCK_FROM_RTC   1
/* ───── public constants ─────────────────────────────────────────────── */
#define NMEA0183_WIRE_MAX      82     /* '$' ... checksum + CR/LF        */
#define NMEA_SENT_MAX        384     /* Maximum sentence buffer length. */
#define MAX_LINES_IN_GRP      8     /* Maximum sentences in one group. */
/* ─────────────── Macros and constants ─────────────── */
#define MAX_LNS        MAX_LINES_IN_GRP
#define FEET_PER_M     3.28084f
#define FATHOM_PER_M   0.54680665f
#define TASK_STEP_MS           10
#define LVGL_TICK_MS           10

#endif
