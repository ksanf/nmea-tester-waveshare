/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_log.h"

/*
 * Central project logging configuration.
 * Each switch below controls diagnostics for one project area.
 *
 * Rules:
 *  - Regular .c files define CFG_LOG_MODULE before including this header;
 *    direct ESP_LOG* calls are then gated by that file's switch.
 *  - Bridge/Sailor macros use the compatible BRIDGE_CAN_LOG_* switches below.
 *  - Legacy local macros TOUCH_LOG_ENABLED and NUM_EDIT_DEBUG are mapped here too.
 */

/* Raw ESP-IDF macros let bridge code bypass a local CFG_LOG_MODULE mapping. */
#ifndef CFG_ESP_LOGE_RAW
#define CFG_ESP_LOGE_RAW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define CFG_ESP_LOGW_RAW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define CFG_ESP_LOGI_RAW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define CFG_ESP_LOGD_RAW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define CFG_ESP_LOGV_RAW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)
#endif

/* ───────── AISdecoder ───────── */
/* File: main/AISdecoder/aisdecoder_parse.c
 * Detect AIVDM/AIVDO and assemble multi-fragment AIS sentences. */
#define LOG_CFG_AISDEC_PARSE              0
/* File: main/AISdecoder/aisdecoder_decode.c
 * Decode AIS payloads and update the target table. */
#define LOG_CFG_AISDEC_DECODE             0
/* File: main/AISdecoder/aisdecoder_ui.c
 * Update the AIS screen, list layout, and raw AIS sentence counters. */
#define LOG_CFG_AISDEC_UI                 0

/* ───────── App / UI ───────── */
/* File: main/app_main.c
 * General application boot, system, and heap logs. */
#define LOG_CFG_APP_MAIN                  0
/* File: main/app_main.c
 * Periodic memory and key-task stack monitor. */
#define LOG_CFG_MEM_MONITOR               0
/* File: main/ui/screens/screen_init.c
 * LVGL/display initialization and startup heap snapshots. */
#define LOG_CFG_SCREEN_INIT               0
/* File: main/ui/screens/screen_ui.c
 * Main-menu navigation and screen transitions. */
#define LOG_CFG_SCREEN_UI                 0
/* File: main/ui/screens/screen_settings.c
 * Settings screen and service-action selection logs. */
#define LOG_CFG_SCREEN_SETTINGS           0
/* File: main/can_module/screen_can.c
 * CAN module selection and navigation away from the CAN screen. */
#define LOG_CFG_SCREEN_CAN                0
/* File: main/ui/instrument_panel.c
 * Template updates from the parser and instrument panel. */
#define LOG_CFG_INSTRUMENT_PANEL          0
/* File: main/ui/nmea_log.c
 * NMEA, AIS, and plain-text log widget diagnostics. */
#define LOG_CFG_NMEA_LOG                  0

/* ───────── Touch ───────── */
/* File: main/touch/touch_driver.c
 * GT911 initialization, IRQ handling, and polling. */
#define LOG_CFG_TOUCH_DRIVER              0
/* ───────── RS-485 runtime ───────── */
/* File: main/rs485/rs485_driver.c
 * Low-level RS-485 UART driver, baud rate, and read/write errors. */
#define LOG_CFG_RS485_DRIVER              0
/* File: main/rs485/rs485_parser.c
 * Parser screen, RX task, template refresh, and screen lifecycle. */
#define LOG_CFG_RS485_PARSER              0
/* File: main/rs485/rs485_simui.c
 * TX485 screen, UI lifecycle, manual input, and queues. */
#define LOG_CFG_RS485_SIMUI               0
/* File: main/rs485/rs485_engine.c
 * NMEA traffic generation, memory, and group regeneration. */
#define LOG_CFG_RS485_ENGINE              0
/* File: main/rs485/rs485_transmit.c
 * Manual sentence submission to the RS-485 transmit path. */
#define LOG_CFG_RS485_TRANSMIT            0
/* File: main/rs485_bridge/rs485_bridge.c
 * Bidirectional Wi-Fi-to-RS-485 bridge screen and runtime counters. */
#define LOG_CFG_RS485_BRIDGE              0

/* ───────── System / Wi-Fi ───────── */
/* File: main/system/telnet_server.c
 * Telnet server lifecycle and socket, connection, and disconnection errors. */
#define LOG_CFG_TELNET_SERVER             0
/* File: main/system/udp_nmea_server.c
 * UDP server lifecycle, bind/receive/send errors, and start/stop events. */
#define LOG_CFG_UDP_NMEA_SERVER           0
/* File: main/wifi/wifi_manager.c
 * AP/STA lifecycle, radio settings, scanning, and connection events. */
#define LOG_CFG_WIFI_AP                   0
/* File: main/system/nmea_clock.c
 * Synchronize system time from the RTC and templates. */
#define LOG_CFG_NMEA_CLOCK                1
/* File: main/system/rtc_driver.c
 * I2C, oscillator, and counter diagnostics for the external PCF85063 RTC. */
#define LOG_CFG_RTC_DRIVER                1
/* ───────── NMEA editor / templates ───────── */
/* File: main/nmea_editor/nmea_editor.c
 * Template editor tab switching. */
#define LOG_CFG_NMEA_EDITOR               0
/* File: main/nmea_editor/nmea_tabs_echo.c
 * ECHO tab readiness logs. */
#define LOG_CFG_NMEA_TABS_ECHO            0
/* File: main/nmea_editor/nmea_tabs_gyro.c
 * GYRO tab readiness logs. */
#define LOG_CFG_NMEA_TABS_GYRO            0
/* File: main/nmea_editor/nmea_tabs_log.c
 * LOG tab readiness logs. */
#define LOG_CFG_NMEA_TABS_LOG             0
/* File: main/nmea_editor/nmea_tabs_weather.c
 * WEATHER tab readiness logs. */
#define LOG_CFG_NMEA_TABS_WEATHER         0
/* File: main/nmea_editor/nmea_templates.c
 * Template NVS load/save operations and debounced saves. */
#define LOG_CFG_NMEA_TEMPLATES            0
/* File: main/nmea_editor/numeric_editor.c
 * Numeric editor diagnostics. */
#define LOG_CFG_NUMERIC_EDITOR            0

/* ───────── CAN / Sailor bridge ───────── */
/* File: main/can_module/bridge_can_module/buffer_manager.c
 * Buffer manager ring, queue, and mutex diagnostics. */
#define LOG_CFG_BUFFER_MANAGER            0
/* File: main/can_module/driver/serial_comm.c
 * serial_comm wrapper around the RS-485 driver. */
#define LOG_CFG_SERIAL_COMM               0
/* File: main/can_module/driver/can_driver.c
 * Basic direct ESP_LOG output from the CAN/TWAI driver. */
#define LOG_CFG_CAN_DRIVER                0
/* File: main/can_module/n2k_driver/src/n2k_addr_claim.c
 * NMEA 2000 address-claim conflict logs. */
#define LOG_CFG_N2K_ADDR_CLAIM            0
/* File: main/can_module/n2k_driver/src/n2k_iso11783.c
 * ISO11783 send-path errors. */
#define LOG_CFG_N2K_ISO11783              0
/* File: main/can_module/nm2k_module/nm2k_decoder*.c
 * Human-readable PGN decoder activity for the NM2K screen. */
#define LOG_CFG_NM2K_DECODER              0

/* Bridge/Sailor category flags used by bridge_can_config.h and low-level transport. */
/* Files: main/can_module/bridge_can_module/protocol_handler.c
 * Raw PIPE text and direct terminal-tunnel dumps. */
#define LOG_CFG_BRIDGE_PIPE               0
/* Files: main/can_module/protocol/src/l2_link.c,
 *        main/can_module/n2k_driver/src/n2k_transport.c
 * Low-level N2K/CAN TX/RX transport dump. */
#define LOG_CFG_BRIDGE_N2K_TXRX          0
/* Files: main/can_module/driver/can_driver.c
 * Additional categorized CAN driver RX dump. */
#define LOG_CFG_BRIDGE_CANDRV            0
/* Files: protocol_handler.c
 * L3 layer/control event trace. */
#define LOG_CFG_BRIDGE_L3                0
/* Files: protocol_handler.c
 * L7 terminal-tunnel frames and text blocks. */
#define LOG_CFG_BRIDGE_L7                0
/* Files: protocol_handler.c, fsm.c, l2_link.c, bridge_can_module.c
 * General Sailor bridge/FSM lifecycle and state logs. */
#define LOG_CFG_BRIDGE_HANDLER           0
/* Files: protocol_handler.c
 * PC<->MT queue/dequeue/credit trace. */
#define LOG_CFG_BRIDGE_PCMT              0

/* ───────── Compatibility with legacy local macros ───────── */
#define TOUCH_LOG_ENABLED                LOG_CFG_TOUCH_DRIVER
#define NUM_EDIT_DEBUG                   LOG_CFG_NUMERIC_EDITOR

#define BRIDGE_CAN_LOG_PIPE              LOG_CFG_BRIDGE_PIPE
#define BRIDGE_CAN_LOG_N2K_TXRX          LOG_CFG_BRIDGE_N2K_TXRX
#define BRIDGE_CAN_LOG_CANDRV            LOG_CFG_BRIDGE_CANDRV
#define BRIDGE_CAN_LOG_L3                LOG_CFG_BRIDGE_L3
#define BRIDGE_CAN_LOG_L7                LOG_CFG_BRIDGE_L7
#define BRIDGE_CAN_LOG_HANDLER           LOG_CFG_BRIDGE_HANDLER
#define BRIDGE_CAN_LOG_PCMT              LOG_CFG_BRIDGE_PCMT

/* ───────── Generic helper macros ───────── */
#define CFG_LOGE(flag, tag, fmt, ...) do { if (flag) CFG_ESP_LOGE_RAW(tag, fmt, ##__VA_ARGS__); } while (0)
#define CFG_LOGW(flag, tag, fmt, ...) do { if (flag) CFG_ESP_LOGW_RAW(tag, fmt, ##__VA_ARGS__); } while (0)
#define CFG_LOGI(flag, tag, fmt, ...) do { if (flag) CFG_ESP_LOGI_RAW(tag, fmt, ##__VA_ARGS__); } while (0)
#define CFG_LOGD(flag, tag, fmt, ...) do { if (flag) CFG_ESP_LOGD_RAW(tag, fmt, ##__VA_ARGS__); } while (0)
#define CFG_LOGV(flag, tag, fmt, ...) do { if (flag) CFG_ESP_LOGV_RAW(tag, fmt, ##__VA_ARGS__); } while (0)

/* Regular .c files define CFG_LOG_MODULE before this include to gate local ESP_LOG* calls. */
#ifdef CFG_LOG_MODULE
#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGD
#undef ESP_LOGV
#define ESP_LOGE(tag, fmt, ...) CFG_LOGE(CFG_LOG_MODULE, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) CFG_LOGW(CFG_LOG_MODULE, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) CFG_LOGI(CFG_LOG_MODULE, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) CFG_LOGD(CFG_LOG_MODULE, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) CFG_LOGV(CFG_LOG_MODULE, tag, fmt, ##__VA_ARGS__)
#endif
