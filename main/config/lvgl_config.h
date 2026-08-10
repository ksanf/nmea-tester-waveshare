/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared LVGL and RGB panel configuration.
 *          Select a mode with LVGL_PORT_MODE (1 through 4); all remaining
 *          values are derived automatically.
 *
 * ──────────────── Modes (LVGL_PORT_MODE) ────────────────
 * 1 - Single framebuffer with partial rendering; saves PSRAM.
 * 2 - Double framebuffer with direct mode and Waveshare anti-tearing.
 * 3 - Double framebuffer with full refresh and compatible anti-tearing.
 * 4 - Single framebuffer with two draw buffers and lightweight partial
 *     rendering without anti-tearing.
 */

#pragma once

#include "config_nmea_tester.h"   /* LCD_WIDTH, LCD_HEIGHT, PCLK, rotation */
#include "memory_config.h"        /* LVGL/LCD memory placement and sizes */

/* ═══════════════════════════════════════════════════════════════════
 * 1. Mode selection (default is defined in memory_config.h)
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef LVGL_PORT_MODE
    #define LVGL_PORT_MODE                  3
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 2. Memory settings (defaults come from memory_config.h)
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef LVGL_DRAW_BUFFER_LINES
    #define LVGL_DRAW_BUFFER_LINES          16  /* Draw-buffer lines when direct mode is disabled. */
#endif
#ifndef LVGL_BUFF_SPIRAM
    #define LVGL_BUFF_SPIRAM                0   /* LVGL draw buffer: 0=SRAM, 1=PSRAM. */
#endif
#ifndef LCD_RGB_BOUNCE_BUFFER_LINES
    #define LCD_RGB_BOUNCE_BUFFER_LINES     10  /* DRAM bounce buffer against RGB DMA drift */
#endif
#ifndef LCD_RGB_FB_IN_PSRAM
    #define LCD_RGB_FB_IN_PSRAM             1   /* RGB LCD framebuffers. */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 3. Settings derived for each mode
 * ═══════════════════════════════════════════════════════════════════ */
#if LVGL_PORT_MODE == 1
    /* ── Mode 1: single framebuffer with partial rendering ────── */
    #undef  LCD_RGB_FB_COUNT
    #define LCD_RGB_FB_COUNT                1
    #define LVGL_DIRECT_MODE                0
    #define LVGL_FULL_REFRESH               0
    #define LVGL_DOUBLE_BUFFER              0
    #define LVGL_AVOID_TEARING              0
    #define LVGL_BB_MODE                    0

#elif LVGL_PORT_MODE == 2
    /* ── Mode 2: double framebuffer, direct mode, anti-tearing ── */
    #undef  LCD_RGB_FB_COUNT
    #define LCD_RGB_FB_COUNT                2
    #define LVGL_DIRECT_MODE                1
    #define LVGL_FULL_REFRESH               0
    #define LVGL_DOUBLE_BUFFER              0
    #define LVGL_AVOID_TEARING              1
    #define LVGL_BB_MODE                    0

#elif LVGL_PORT_MODE == 3
    /* ── Mode 3: double framebuffer, full refresh, anti-tearing ─ */
    #undef  LCD_RGB_FB_COUNT
    #define LCD_RGB_FB_COUNT                2
    #define LVGL_DIRECT_MODE                0
    #define LVGL_FULL_REFRESH               1
    #define LVGL_DOUBLE_BUFFER              0
    #define LVGL_AVOID_TEARING              1
    #define LVGL_BB_MODE                    0

#elif LVGL_PORT_MODE == 4
    /* ── Mode 4: single framebuffer, two draw buffers, partial rendering ─ */
    #undef  LCD_RGB_FB_COUNT
    #define LCD_RGB_FB_COUNT                1
    #define LVGL_DIRECT_MODE                0
    #define LVGL_FULL_REFRESH               0
    #define LVGL_DOUBLE_BUFFER              1
    #define LVGL_AVOID_TEARING              0
    #define LVGL_BB_MODE                    0

#else
    #error "LVGL_PORT_MODE must be 1, 2, 3, or 4"
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 4. Derived values (change only when required)
 * ═══════════════════════════════════════════════════════════════════ */
/* Draw-buffer size in pixels; used only when direct mode is disabled. */
#define LVGL_DRAW_BUFFER_PIXELS             (LCD_WIDTH * LVGL_DRAW_BUFFER_LINES)

/* Bounce-buffer size in pixels; 0 disables it. */
#if LCD_RGB_BOUNCE_BUFFER_LINES > 0
    #define LCD_RGB_BOUNCE_BUFFER_PIXELS    (LCD_WIDTH * LCD_RGB_BOUNCE_BUFFER_LINES)
#else
    #define LCD_RGB_BOUNCE_BUFFER_PIXELS    0
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 5. LVGL task settings
 * ═══════════════════════════════════════════════════════════════════ */
#define LVGL_TICK_PERIOD_MS             20      /* LVGL tick period in ms. */
#define LVGL_TASK_STACK_SIZE            (6 * 1024) /* LVGL task stack in bytes. */
#define LVGL_TASK_PRIORITY              4       /* Task priority (0 through N). */
#define LVGL_TASK_CORE                  0       /* keep with RGB ISR/core     */
#define LVGL_TASK_MAX_SLEEP_MS          100     /* Maximum task sleep in ms. */

/* ═══════════════════════════════════════════════════════════════════
 * 6. Other panel settings
 * ═══════════════════════════════════════════════════════════════════ */
#define LCD_RGB_DATA_WIDTH              16      /* Bus bits per pixel. */
#define LCD_RGB_BITS_PER_PIXEL          16      /* RGB565                      */
#define LCD_RGB_SRAM_TRANS_ALIGN        4       /* SRAM transfer alignment. */
#define LCD_RGB_PSRAM_TRANS_ALIGN       64      /* PSRAM transfer alignment. */
#define LCD_RGB_PCLK_ACTIVE_NEG         1       /* Waveshare: data on falling PCLK edge */
