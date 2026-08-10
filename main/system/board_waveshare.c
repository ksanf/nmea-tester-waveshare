/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief Waveshare board I/O and CH422G control.
 */

#include "system/board_waveshare.h"
#include "config/config_pins.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_ws";

#define WAVESHARE_I2C_PORT     I2C_NUM_0
#define WAVESHARE_I2C_FREQ_HZ  400000

#define CH422G_REG_MODE        0x24
#define CH422G_REG_OUT         0x38
#define CH422G_REG_OUT_UPPER   0x23
#define CH422G_MODE_OUTPUT     0x01

static uint16_t s_exio_state = 0;
static bool s_i2c_ready = false;

static esp_err_t ch422g_write_reg_(uint8_t reg, uint8_t value)
{
    return i2c_master_write_to_device(WAVESHARE_I2C_PORT, reg, &value, 1, pdMS_TO_TICKS(100));
}

static esp_err_t ch422g_flush_(void)
{
    ESP_RETURN_ON_ERROR(ch422g_write_reg_(CH422G_REG_OUT, (uint8_t)(s_exio_state & 0xff)),
                        TAG, "CH422G lower output write failed");
    ESP_RETURN_ON_ERROR(ch422g_write_reg_(CH422G_REG_OUT_UPPER, (uint8_t)(s_exio_state >> 8)),
                        TAG, "CH422G upper output write failed");
    return ESP_OK;
}

static esp_err_t ch422g_set_pin_(uint8_t pin, bool level)
{
    if (pin > 15) return ESP_ERR_INVALID_ARG;

    if (level) {
        s_exio_state |= (uint16_t)(1U << pin);
    } else {
        s_exio_state &= (uint16_t)~(1U << pin);
    }

    return ch422g_flush_();
}

esp_err_t waveshare_i2c_init(void)
{
    if (s_i2c_ready) return ESP_OK;

    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = PIN_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = WAVESHARE_I2C_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(WAVESHARE_I2C_PORT, &i2c_conf),
                        TAG, "I2C config failed");
    esp_err_t ret = i2c_driver_install(WAVESHARE_I2C_PORT, i2c_conf.mode, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "I2C install failed");
    }

    s_i2c_ready = true;
    return ESP_OK;
}

esp_err_t waveshare_lcd_set_backlight(bool on)
{
    return ch422g_set_pin_(PIN_EXIO_LCD_DISP, on);
}

esp_err_t waveshare_lcd_reset(void)
{
    ESP_RETURN_ON_ERROR(ch422g_set_pin_(PIN_EXIO_LCD_RST, false), TAG, "LCD reset low failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set_pin_(PIN_EXIO_LCD_RST, true), TAG, "LCD reset high failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t waveshare_touch_set_reset(bool released)
{
    return ch422g_set_pin_(PIN_EXIO_TP_RST, released);
}

esp_err_t waveshare_touch_reset(void)
{
    ESP_RETURN_ON_ERROR(waveshare_touch_set_reset(false), TAG, "TP reset low failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(waveshare_touch_set_reset(true), TAG, "TP reset high failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

esp_err_t waveshare_board_init(void)
{
    ESP_RETURN_ON_ERROR(waveshare_i2c_init(), TAG, "I2C init failed");

    s_exio_state = 0;
    s_exio_state |= (uint16_t)(1U << PIN_EXIO_TP_RST);
    s_exio_state |= (uint16_t)(1U << PIN_EXIO_LCD_RST);
    s_exio_state |= (uint16_t)(1U << PIN_EXIO_SD_CS);
    ESP_RETURN_ON_ERROR(ch422g_flush_(), TAG, "CH422G initial output write failed");
    ESP_RETURN_ON_ERROR(ch422g_write_reg_(CH422G_REG_MODE, CH422G_MODE_OUTPUT),
                        TAG, "CH422G mode write failed");

    ESP_RETURN_ON_ERROR(waveshare_lcd_set_backlight(false), TAG, "Backlight off failed");
    ESP_RETURN_ON_ERROR(waveshare_touch_reset(), TAG, "Touch reset failed");
    ESP_RETURN_ON_ERROR(waveshare_lcd_reset(), TAG, "LCD reset failed");

    ESP_LOGI(TAG, "Waveshare board IO initialized on I2C%d SDA=%d SCL=%d",
             (int)WAVESHARE_I2C_PORT, PIN_I2C_SDA, PIN_I2C_SCL);
    return ESP_OK;
}
