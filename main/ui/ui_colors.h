/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Shared UI color constants.
 */

#pragma once

/* Base colors */
#define UI_COLOR_BLACK_HEX                    0x000000
#define UI_COLOR_WHITE_HEX                    0xFFFFFF
#define UI_COLOR_TEXT_BLACK_HEX               0x000000U

/* ------------------------------------------------------------------------- */
/* METAL theme                                                               */
/* ------------------------------------------------------------------------- */
/* Screen background: gradient top */
#define UI_THEME_METAL_BG_HEX                 0xD7DBE0
/* Screen background: gradient bottom */
#define UI_THEME_METAL_BG_GRAD_HEX            0xC2C8CE
/* Card background */
#define UI_THEME_METAL_CARD_HEX               0xF3F5F7
/* Instrument-panel background */
#define UI_THEME_METAL_PANEL_BG_HEX           0xC7CBCF
/* Text-log background */
#define UI_THEME_METAL_LOG_BG_HEX             0xEDF1F4
/* REFRESH button */
#define UI_THEME_METAL_REFRESH_HEX            0xB8C0C8
/* REFRESH alert flash */
#define UI_THEME_METAL_REFRESH_ALERT_HEX      0x5F6B75
/* AIS DETECTED button */
#define UI_THEME_METAL_AIS_HEX                0x9099A1
/* Active TX485 group button */
#define UI_THEME_METAL_GROUP_ACTIVE_HEX       0x7C8893
/* HOME button */
#define UI_THEME_METAL_NAV_HOME_HEX           0x9AA3AB
/* SETTINGS button */
#define UI_THEME_METAL_NAV_SETTINGS_HEX       0x8E979F
/* Bridge CAN button */
#define UI_THEME_METAL_CAN_BRIDGE_HEX         0x9EA6AE
/* NM2K button */
#define UI_THEME_METAL_CAN_NM2K_HEX           0xB0B6BD
/* HOME button in the CAN submenu */
#define UI_THEME_METAL_CAN_HOME_HEX           0x8A949D
/* Active TX485 editor tab */
#define UI_THEME_METAL_EDITOR_TAB_ACTIVE_HEX  0x7C8893
/* Bit-indicator border */
#define UI_THEME_METAL_BIT_BORDER_HEX         0x7D8790
/* Bit value 1 */
#define UI_THEME_METAL_BIT_ON_HEX             0x2E3943
/* Bit value 0 */
#define UI_THEME_METAL_BIT_OFF_HEX            0xA8B0B7
/* Idle bit-indicator state */
#define UI_THEME_METAL_BIT_IDLE_HEX           0xD9DEE2
/* Shared border */
#define UI_THEME_METAL_BORDER_HEX             0x8E99A4
/* Primary text */
#define UI_THEME_METAL_TEXT_HEX               0x1F252B
/* Titles */
#define UI_THEME_METAL_TITLE_HEX              0x2F3943
/* Secondary text */
#define UI_THEME_METAL_MUTED_HEX              0x404A54
/* Default button */
#define UI_THEME_METAL_BUTTON_HEX             0xA7AFB8
/* Dropdown background */
#define UI_THEME_METAL_DD_BG_HEX              0xC7CDD3
/* Dropdown text */
#define UI_THEME_METAL_DD_TXT_HEX             0x1F252B
/* Pause button in the shared theme style */
#define UI_THEME_METAL_PAUSE_HEX              0xA8B0B7
/* Active pause button */
#define UI_THEME_METAL_PAUSE_ACTIVE_HEX       0x7C8893

/* ------------------------------------------------------------------------- */
/* COLOR theme                                                               */
/* ------------------------------------------------------------------------- */
/* Screen background: bright-blue gradient top */
#define UI_THEME_COLOR_BG_HEX                 0x1A6AD6
/* Screen background: dark-blue gradient bottom */
#define UI_THEME_COLOR_BG_GRAD_HEX            0x0B2D75
/* Card background */
#define UI_THEME_COLOR_CARD_HEX               0x263238
/* Instrument-panel background */
#define UI_THEME_COLOR_PANEL_BG_HEX           0x000080
/* Text-log background */
#define UI_THEME_COLOR_LOG_BG_HEX             0x000000
/* REFRESH button */
#define UI_THEME_COLOR_REFRESH_HEX            0x40C4B4
/* REFRESH alert flash */
#define UI_THEME_COLOR_REFRESH_ALERT_HEX      0xFF0000
/* AIS DETECTED button */
#define UI_THEME_COLOR_AIS_HEX                0xE45C2B
/* Active TX485 group button */
#define UI_THEME_COLOR_GROUP_ACTIVE_HEX       0x607D8B
/* HOME button */
#define UI_THEME_COLOR_NAV_HOME_HEX           UI_CAN_BTN_HOME_HEX
/* SETTINGS button */
#define UI_THEME_COLOR_NAV_SETTINGS_HEX       UI_EDITOR_BACK_HEX
/* Bridge CAN button */
#define UI_THEME_COLOR_CAN_BRIDGE_HEX         UI_CAN_BTN_BRIDGE_HEX
/* NM2K button */
#define UI_THEME_COLOR_CAN_NM2K_HEX           UI_CAN_BTN_NM2K_HEX
/* HOME button in the CAN submenu */
#define UI_THEME_COLOR_CAN_HOME_HEX           UI_CAN_BTN_HOME_HEX
/* Active TX485 editor tab */
#define UI_THEME_COLOR_EDITOR_TAB_ACTIVE_HEX  0x607D8B
/* Bit-indicator border */
#define UI_THEME_COLOR_BIT_BORDER_HEX         0x0000FF
/* Bit value 1 */
#define UI_THEME_COLOR_BIT_ON_HEX             0xFF0000
/* Bit value 0 */
#define UI_THEME_COLOR_BIT_OFF_HEX            0x00FF00
/* Idle bit-indicator state */
#define UI_THEME_COLOR_BIT_IDLE_HEX           0x808080
/* Shared border */
#define UI_THEME_COLOR_BORDER_HEX             0x546E7A
/* Primary text */
#define UI_THEME_COLOR_TEXT_HEX               0xF6FBFF
/* Titles */
#define UI_THEME_COLOR_TITLE_HEX              0xFFC107
/* Secondary text */
#define UI_THEME_COLOR_MUTED_HEX              0xCBE2F5
/* Default button */
#define UI_THEME_COLOR_BUTTON_HEX             0xFFBF00
/* Dropdown background */
#define UI_THEME_COLOR_DD_BG_HEX              0x263238
/* Dropdown text */
#define UI_THEME_COLOR_DD_TXT_HEX             0xFFC107
/* Pause button */
#define UI_THEME_COLOR_PAUSE_HEX              0x00CC66
/* Active pause button */
#define UI_THEME_COLOR_PAUSE_ACTIVE_HEX       0xFF6600

/* ------------------------------------------------------------------------- */
/* B/W theme                                                                 */
/* ------------------------------------------------------------------------- */
/* Screen background: identical top and bottom, no gradient */
#define UI_THEME_BW_BG_HEX                    0x000000
#define UI_THEME_BW_BG_GRAD_HEX               0x000000
/* Card background */
#define UI_THEME_BW_CARD_HEX                  0x111111
/* Instrument-panel background */
#define UI_THEME_BW_PANEL_BG_HEX              0x2A2A2A
/* Text-log background */
#define UI_THEME_BW_LOG_BG_HEX                0x151515
/* REFRESH button */
#define UI_THEME_BW_REFRESH_HEX               0x404040
/* REFRESH alert flash */
#define UI_THEME_BW_REFRESH_ALERT_HEX         0xE0E0E0
/* AIS DETECTED button */
#define UI_THEME_BW_AIS_HEX                   0x5A5A5A
/* Active TX485 group button */
#define UI_THEME_BW_GROUP_ACTIVE_HEX          0x707070
/* HOME button */
#define UI_THEME_BW_NAV_HOME_HEX              0x5E5E5E
/* SETTINGS button */
#define UI_THEME_BW_NAV_SETTINGS_HEX          0x6C6C6C
/* Bridge CAN button */
#define UI_THEME_BW_CAN_BRIDGE_HEX            0x505050
/* NM2K button */
#define UI_THEME_BW_CAN_NM2K_HEX              0x676767
/* HOME button in the CAN submenu */
#define UI_THEME_BW_CAN_HOME_HEX              0x5E5E5E
/* Active TX485 editor tab */
#define UI_THEME_BW_EDITOR_TAB_ACTIVE_HEX     0x707070
/* Bit-indicator border */
#define UI_THEME_BW_BIT_BORDER_HEX            0xCFCFCF
/* Bit value 1 */
#define UI_THEME_BW_BIT_ON_HEX                0xFFFFFF
/* Bit value 0 */
#define UI_THEME_BW_BIT_OFF_HEX               0x8E8E8E
/* Idle bit-indicator state */
#define UI_THEME_BW_BIT_IDLE_HEX              0x2F2F2F
/* Shared border */
#define UI_THEME_BW_BORDER_HEX                0xD8D8D8
/* Primary text */
#define UI_THEME_BW_TEXT_HEX                  0xFFFFFF
/* Titles */
#define UI_THEME_BW_TITLE_HEX                 0xFFFFFF
/* Secondary text */
#define UI_THEME_BW_MUTED_HEX                 0xD0D0D0
/* Default button */
#define UI_THEME_BW_BUTTON_HEX                0xE0E0E0
/* Dropdown background */
#define UI_THEME_BW_DD_BG_HEX                 0x1C1C1C
/* Dropdown text */
#define UI_THEME_BW_DD_TXT_HEX                0xFFFFFF
/* Pause button */
#define UI_THEME_BW_PAUSE_HEX                 0xE0E0E0
/* Active pause button */
#define UI_THEME_BW_PAUSE_ACTIVE_HEX          0x808080

/* ------------------------------------------------------------------------- */
/* Main screen                                                               */
/* ------------------------------------------------------------------------- */
#define UI_MAIN_BTN_RX485_HEX                 0x00CC66
#define UI_MAIN_BTN_TX485_HEX                 0xFF8800
#define UI_MAIN_BTN_CAN_HEX                   0x4488FF
#define UI_MAIN_BTN_SETUP_HEX                 0xAAAAAA
#define UI_MAIN_BTN_RS485_BRIDGE_HEX          0xB67DFF
#define UI_MAIN_BTN_WIFI_ON_HEX               0x40C878
#define UI_MAIN_BTN_WIFI_OFF_HEX              0x7A7A7A
#define UI_MAIN_BTN_WIFI_ON_PRESSED_HEX       0x2EA65C
#define UI_MAIN_BTN_WIFI_OFF_PRESSED_HEX      0x565656

/* ------------------------------------------------------------------------- */
/* Group and editor buttons                                                  */
/* ------------------------------------------------------------------------- */
#define UI_GROUP_GPS_HEX                      0xC8E6FF
#define UI_GROUP_GYRO_HEX                     0xFFE0B2
#define UI_GROUP_LOG_HEX                      0xC5E1A5
#define UI_GROUP_ECHO_HEX                     0xB3E5FC
#define UI_GROUP_WX_HEX                       0xFFCCBC
#define UI_EDITOR_BACK_HEX                    0x90A4AE
#define UI_EDITOR_PARENT_HEX                  0x455A64

/* ------------------------------------------------------------------------- */
/* Shared widgets                                                            */
/* ------------------------------------------------------------------------- */
#define UI_WIDGET_FIELD_BG_HEX                0xE3E3E3
#define UI_DIALOG_BORDER_HEX                  0x8C8C8C
#define UI_DIALOG_BORDER_PRESSED_HEX          0x404040
#define UI_NMEA_LOG_TEXT_HEX                  0xFFFFFF
#define UI_NMEA_HEX_ADDRESS_HEX               0xFFFFFF
#define UI_NMEA_HEX_DATA_HEX                  0x00FF66
#define UI_NMEA_HEX_ASCII_HEX                 0xFFD54A
#define UI_NUM_EDIT_UNDERLINE_HEX             0x000000

/* ------------------------------------------------------------------------- */
/* CAN submenu                                                               */
/* ------------------------------------------------------------------------- */
#define UI_CAN_BTN_BRIDGE_HEX                 0x0094C8
#define UI_CAN_BTN_NM2K_HEX                   0xC89400
#define UI_CAN_BTN_HOME_HEX                   0xD3A333

/* ------------------------------------------------------------------------- */
/* RS485 bridge                                                              */
/* ------------------------------------------------------------------------- */
#define UI_RS485_BRIDGE_BG_HEX                0xF3F3F1
#define UI_RS485_BRIDGE_TITLE_HEX             0x23272D
#define UI_RS485_BRIDGE_CARD_TOP_HEX          0x4A4F57
#define UI_RS485_BRIDGE_CARD_BOTTOM_HEX       0x2F343B
#define UI_RS485_BRIDGE_CARD_BORDER_HEX       0x6D727A
#define UI_RS485_BRIDGE_CARD_CAPTION_HEX      0x9BB7C7
#define UI_RS485_BRIDGE_CARD_TEXT_HEX         0xEAF4FA
