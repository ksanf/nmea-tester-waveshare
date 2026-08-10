/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#ifndef CONFIG_PINS_H
#define CONFIG_PINS_H
#define PIN_POWER_SW  -1 // GPIO0 is used by the Waveshare RGB panel (G3)

// RS-485: onboard transceiver with automatic direction control
#define PIN_RS485_RE  -1
#define PIN_RS485_SE  -1
#define PIN_RS485_RX  43
#define PIN_RS485_TX  44
#define PIN_CGQ_EN    -1

// CAN
#define PIN_CAN_SE    -1
#define PIN_CAN_RX    16
#define PIN_CAN_TX    15

// Waveshare onboard RTC is PCF85063 on the shared I2C bus.
#define RTC_PCF85063_I2C_ADDR 0x51

// Active UART transport is always RS-485. USB/UART0 is reserved for flashing/debug only.
#define UART_PORT          UART_NUM_2
#define UART_TX_PIN        PIN_RS485_TX
#define UART_RX_PIN        PIN_RS485_RX
#define UART_DE_PIN        PIN_RS485_SE
#define UART_RE_PIN        PIN_RS485_RE
#define UART_POWER_PIN     PIN_CGQ_EN
// Shared I2C bus: CH422G IO expander, GT911 touch, PCF85063 RTC
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9

// RGB LCD
#define PIN_LCD_HSYNC 46
#define PIN_LCD_VSYNC 3
#define PIN_LCD_DE    5
#define PIN_LCD_PCLK  7
#define PIN_LCD_B0    14 // B3
#define PIN_LCD_B1    38 // B4
#define PIN_LCD_B2    18 // B5
#define PIN_LCD_B3    17 // B6
#define PIN_LCD_B4    10 // B7
#define PIN_LCD_G0    39 // G2
#define PIN_LCD_G1    0  // G3
#define PIN_LCD_G2    45 // G4
#define PIN_LCD_G3    48 // G5
#define PIN_LCD_G4    47 // G6
#define PIN_LCD_G5    21 // G7
#define PIN_LCD_R0    1  // R3
#define PIN_LCD_R1    2  // R4
#define PIN_LCD_R2    42 // R5
#define PIN_LCD_R3    41 // R6
#define PIN_LCD_R4    40 // R7

// GT911 touch
#define PIN_TOUCH_IRQ 4

// CH422G EXIO lines used by the board
#define PIN_EXIO_TP_RST   1
#define PIN_EXIO_LCD_DISP 2
#define PIN_EXIO_LCD_RST  3
#define PIN_EXIO_SD_CS    4
#endif
