/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared memory placement configuration for PSRAM, SRAM, and IRAM.
 *          Every value can be overridden before including this file.
 *
 *          Memory pool        │ Size    │ Purpose
 *          ───────────────────┼─────────┼──────────────────────────
 *          SRAM (internal)     │ ~512 KB │ Code, stacks, ISRs, heap
 *          IRAM               │ ~128 KB │ Fast LVGL code and ISR handlers
 *          PSRAM (external)    │  ~8 MB  │ Framebuffers and other buffers
 *
 *          Important: sdkconfig options are shown only as comments here.
 *          Change them only through `idf.py menuconfig`.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_heap_caps.h"

/* ═══════════════════════════════════════════════════════════════════
 * 0. Helpers for explicit memory placement
 * ═══════════════════════════════════════════════════════════════════ */
#define MALLOC_SRAM(sz)     heap_caps_malloc((sz), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define MALLOC_PSRAM(sz)    heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#define MALLOC_DMA(sz)      heap_caps_malloc((sz), MALLOC_CAP_DMA)
#define CALLOC_SRAM(n,sz)   heap_caps_calloc((n), (sz), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define CALLOC_PSRAM(n,sz)  heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM)
#define CALLOC_DMA(n,sz)    heap_caps_calloc((n), (sz), MALLOC_CAP_DMA)

/* Select PSRAM when the flag is true, otherwise select SRAM. */
#define MALLOC_WHERE(psram, sz)     ((psram) ? MALLOC_PSRAM(sz)     : MALLOC_SRAM(sz))
#define CALLOC_WHERE(psram, n, sz)  ((psram) ? CALLOC_PSRAM(n, sz)  : CALLOC_SRAM(n, sz))

/* ═══════════════════════════════════════════════════════════════════
 * 1. sdkconfig malloc controls
 *    CONFIG_SPIRAM_USE_MALLOC=y  - malloc() may allocate from PSRAM
 *    CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 - allocations below 16K use SRAM
 *    CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768 - reserved SRAM
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * 2. LCD and LVGL memory
 * ═══════════════════════════════════════════════════════════════════ */

/* LVGL pool (sdkconfig: CONFIG_LV_MEM_SIZE_KILOBYTES). */
#ifndef LV_MEM_POOL_SIZE_KB
    #define LV_MEM_POOL_SIZE_KB             64
#endif

/* The mode selects framebuffer count: modes 1/4 use one, modes 2/3 use two. */
#ifndef LVGL_PORT_MODE
    #define LVGL_PORT_MODE                  2
#endif

#ifndef LVGL_DRAW_BUFFER_LINES
    #define LVGL_DRAW_BUFFER_LINES          16      /* Lines. */
#endif
#ifndef LVGL_BUFF_SPIRAM
    #define LVGL_BUFF_SPIRAM                false   /* LVGL draw buffer. */
#endif
#ifndef LCD_RGB_FB_IN_PSRAM
    #define LCD_RGB_FB_IN_PSRAM             true    /* RGB LCD framebuffers. */
#endif
#ifndef LCD_RGB_BOUNCE_BUFFER_LINES
    #define LCD_RGB_BOUNCE_BUFFER_LINES     10      /* internal SRAM bounce */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 3. RS-485 UART driver ring buffer
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef UART_RX_RING_BUF_SIZE
    #define UART_RX_RING_BUF_SIZE           1024    /* Bytes. */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 4. NMEA log (ui/nmea_log.c)
 *    ring[ROWS][COLS] = 32 x 256 = 8192 bytes
 *    bigbuf           = 32 x 256 = 8192 bytes
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef NMEA_LOG_RING_ROWS
    #define NMEA_LOG_RING_ROWS              32
#endif
#ifndef NMEA_LOG_RING_IN_PSRAM
    #define NMEA_LOG_RING_IN_PSRAM          true    /* ring[ROWS][COLS]: 8K, not latency-critical. */
#endif
#ifndef NMEA_LOG_BIGBUF_IN_PSRAM
    #define NMEA_LOG_BIGBUF_IN_PSRAM        true    /* 8K composition buffer. */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 5. RS485 engine (rs485/rs485_engine.c)
 *    s_lines_global[MAX_LINES_IN_GRP * NMEA_SENT_MAX] = 8 x 384 = 3072
 *    pkt is temporary and holds up to (cnt+1) x 384 bytes
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef RS485_ENGINE_LINES_IN_PSRAM
    #define RS485_ENGINE_LINES_IN_PSRAM     true
#endif
#ifndef RS485_ENGINE_PKT_IN_PSRAM
    #define RS485_ENGINE_PKT_IN_PSRAM       true    /* Temporary packet buffer. */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 6. RS485 simulator UI (rs485/rs485_simui.c)
 *    s_log_q queue: 24 x 387 = 9288 bytes (FreeRTOS always uses SRAM)
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * 7. AIS decoder
 *    s_list_buf  = 8192 bytes (aisdecoder_ui.c)
 *    s_targets   = 64 x ~92 = ~5888 bytes (aisdecoder_decode.c)
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef AIS_LIST_BUF_IN_PSRAM
    #define AIS_LIST_BUF_IN_PSRAM           true    /* s_list_buf: 8K UI data, not latency-critical. */
#endif
#ifndef AIS_TARGETS_IN_PSRAM
    #define AIS_TARGETS_IN_PSRAM            true    /* s_targets ~6K */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 8. NM2K module (can_module/nm2k_module/nm2k_module.c)
 *    s_nodes[32]            ~6656 bytes
 *    s_nodes_snapshot[32]   ~6656 bytes
 *    s_traffic[24]          = 2880 bytes
 *    s_traffic_snapshot[24] = 2880 bytes
 *    s_devices_buf        = NM2K_DEVICES_BUF_SIZE
 *    s_traffic_buf        = NM2K_TRAFFIC_BUF_SIZE
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef NM2K_NODES_IN_PSRAM
    #define NM2K_NODES_IN_PSRAM             true    /* s_nodes plus snapshot: ~13K in PSRAM. */
#endif
#ifndef NM2K_TRAFFIC_IN_PSRAM
    #define NM2K_TRAFFIC_IN_PSRAM           true    /* s_traffic + snapshot — ~5.7K */
#endif
#ifndef NM2K_DEVICES_BUF_SIZE
    #define NM2K_DEVICES_BUF_SIZE           4096
#endif
#ifndef NM2K_TRAFFIC_BUF_SIZE
    #define NM2K_TRAFFIC_BUF_SIZE           8192
#endif
#ifndef NM2K_BUFS_IN_PSRAM
    #define NM2K_BUFS_IN_PSRAM              true    /* s_devices_buf + traffic_buf — ~12K */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 9. Bridge CAN (can_module/bridge_can_module/)
 *    input_buffer  ~1056 bytes (buffer_manager.c)
 *    output_buffer ~8224 bytes (buffer_manager.c)
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef BRIDGE_CAN_BUFS_IN_PSRAM
    #define BRIDGE_CAN_BUFS_IN_PSRAM        true    /* Input and output buffers: ~9K. */
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 10. N2K transport (can_module/n2k_driver/src/n2k_transport.c)
 *     G.fp_slots[8] ~1952 bytes
 *     G.tp_slots[4]   ~80 bytes
 *     s->buf payload is dynamic, up to 1785 bytes
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef N2K_SLOTS_IN_PSRAM
    #define N2K_SLOTS_IN_PSRAM              false
#endif
#ifndef N2K_PAYLOAD_IN_PSRAM
    #define N2K_PAYLOAD_IN_PSRAM            false
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 11. Telnet server (system/telnet_server.c)
 *     cp is a dynamic TX buffer
 *     s_tx_q is a FreeRTOS queue and always uses SRAM
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef TELNET_TX_BUF_IN_PSRAM
    #define TELNET_TX_BUF_IN_PSRAM          false
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 12. WiFi (system/wifi_ap.c)
 *     ap[WIFI_AP_SCAN_MAX_APS] stores access-point scan results
 * ═══════════════════════════════════════════════════════════════════ */
#ifndef WIFI_AP_SCAN_MAX_APS
    #define WIFI_AP_SCAN_MAX_APS            24
#endif
#ifndef WIFI_AP_SCAN_IN_PSRAM
    #define WIFI_AP_SCAN_IN_PSRAM           false
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 13. FreeRTOS (sdkconfig; change only through menuconfig)
 * ═══════════════════════════════════════════════════════════════════ */
// CONFIG_FREERTOS_IDLE_TASK_STACKSIZE      = 1536
// CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH   = 2048
// CONFIG_FREERTOS_ISR_STACKSIZE            = 1536
// CONFIG_ESP_MAIN_TASK_STACK_SIZE          = 6144
// CONFIG_LWIP_TCPIP_TASK_STACK_SIZE        = 3072

/* ═══════════════════════════════════════════════════════════════════
 * 14. PSRAM / GDMA / LCD (sdkconfig, menuconfig)
 * ═══════════════════════════════════════════════════════════════════ */
// CONFIG_SPIRAM_MODE_OCT  = y        // Octal at 80 MHz
// CONFIG_SPIRAM_SPEED_80M = y
// CONFIG_SPIRAM_XIP_FROM_PSRAM     = y   // Improves RGB LCD stability during flash operations
// CONFIG_SPIRAM_FETCH_INSTRUCTIONS = y
// CONFIG_SPIRAM_RODATA             = y
// CONFIG_GDMA_CTRL_FUNC_IN_IRAM    = y
// CONFIG_GDMA_ISR_HANDLER_IN_IRAM  = y
// CONFIG_LCD_RGB_ISR_IRAM_SAFE     = y
// CONFIG_ESP_WIFI_IRAM_OPT         = y
// CONFIG_ESP_WIFI_RX_IRAM_OPT      = y
