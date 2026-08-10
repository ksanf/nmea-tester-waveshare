/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   PCF85063 RTC driver for Waveshare ESP32-S3-Touch-LCD-5.
 */

#include "system/rtc_driver.h"
#include "config_pins.h"
#include "system/board_waveshare.h"

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#define CFG_LOG_MODULE LOG_CFG_RTC_DRIVER
#include "config_logs.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static const char *TAG = "rtc_pcf85063";
static bool s_rtc_ready = false;
static bool s_rtc_time_valid = false;
static int s_last_second = -1;
static int64_t s_last_second_change_us = 0;

#define RTC_I2C_PORT        I2C_NUM_0
#define PCF85063_REG_CTRL1  0x00
#define PCF85063_REG_CTRL2  0x01
#define PCF85063_REG_OFFSET 0x02
#define PCF85063_REG_RAM    0x03
#define PCF85063_REG_SC     0x04
#define PCF85063_REG_MN     0x05
#define PCF85063_REG_HR     0x06
#define PCF85063_REG_DM     0x07
#define PCF85063_REG_DW     0x08
#define PCF85063_REG_MO     0x09
#define PCF85063_REG_YR     0x0A

#define PCF85063_SC_OS      0x80
#define PCF85063_CTRL1_EXT_TEST 0x80
#define PCF85063_CTRL1_STOP 0x20
#define PCF85063_CTRL1_12_24 0x02
#define PCF85063_CTRL1_CAP_SEL 0x01

#define RTC_I2C_TIMEOUT_MS  100
#define RTC_READ_RETRIES    3
#define RTC_RETRY_DELAY_MS  5
#define RTC_OSC_TIMEOUT_MS  2500
#define RTC_OSC_POLL_MS     100
#define RTC_TICK_TEST_MS    1200
#define RTC_STUCK_TIMEOUT_US 2500000LL

static uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

/* ─── I2C read with retry ─────────────────────────────────────────── */

static esp_err_t pcf85063_read_(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < RTC_READ_RETRIES; i++) {
        ret = i2c_master_write_read_device(RTC_I2C_PORT,
                                           RTC_PCF85063_I2C_ADDR,
                                           &reg, 1,
                                           buf, len,
                                           pdMS_TO_TICKS(RTC_I2C_TIMEOUT_MS));
        if (ret == ESP_OK) return ESP_OK;
        if (i < RTC_READ_RETRIES - 1) {
            vTaskDelay(pdMS_TO_TICKS(RTC_RETRY_DELAY_MS));
        }
    }
    ESP_LOGW(TAG, "I2C read reg=0x%02x len=%u failed after %d retries: %s (%d)",
             reg, (unsigned)len, RTC_READ_RETRIES, esp_err_to_name(ret), (int)ret);
    return ret;
}

/* ─── I2C write with retry ────────────────────────────────────────── */

static esp_err_t pcf85063_write_(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];

    if (!buf || len == 0 || len > sizeof(tmp) - 1U) return ESP_ERR_INVALID_ARG;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);

    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < RTC_READ_RETRIES; i++) {
        ret = i2c_master_write_to_device(RTC_I2C_PORT,
                                         RTC_PCF85063_I2C_ADDR,
                                         tmp, len + 1U,
                                         pdMS_TO_TICKS(RTC_I2C_TIMEOUT_MS));
        if (ret == ESP_OK) return ESP_OK;
        if (i < RTC_READ_RETRIES - 1) {
            vTaskDelay(pdMS_TO_TICKS(RTC_RETRY_DELAY_MS));
        }
    }
    ESP_LOGW(TAG, "I2C write reg=0x%02x len=%u failed after %d retries: %s (%d)",
             reg, (unsigned)len, RTC_READ_RETRIES, esp_err_to_name(ret), (int)ret);
    return ret;
}

/* ─── Dump all RTC registers ──────────────────────────────────────── */

static esp_err_t rtc_dump_regs(void)
{
    uint8_t buf[11];

    esp_err_t ret = pcf85063_read_(PCF85063_REG_CTRL1, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "REG DUMP unavailable: %s (%d)",
                 esp_err_to_name(ret), (int)ret);
        return ret;
    }

    ESP_LOGI(TAG, "REG DUMP: "
             "CTRL1=0x%02X CTRL2=0x%02X OFFSET=0x%02X RAM=0x%02X "
             "SC=0x%02X MN=0x%02X HR=0x%02X DM=0x%02X DW=0x%02X MO=0x%02X YR=0x%02X",
             buf[0x00], buf[0x01], buf[0x02], buf[0x03],
             buf[0x04], buf[0x05], buf[0x06], buf[0x07],
             buf[0x08], buf[0x09], buf[0x0A]);
    return ESP_OK;
}

/* ─── Probe chip ──────────────────────────────────────────────────── */

static esp_err_t rtc_probe(void)
{
    uint8_t ctrl1;
    return pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1);
}

static esp_err_t rtc_wait_for_oscillator_(uint8_t sec)
{
    sec &= (uint8_t)~PCF85063_SC_OS;

    for (int elapsed_ms = RTC_OSC_POLL_MS;
         elapsed_ms <= RTC_OSC_TIMEOUT_MS;
         elapsed_ms += RTC_OSC_POLL_MS) {
        ESP_RETURN_ON_ERROR(
            pcf85063_write_(PCF85063_REG_SC, &sec, 1),
            TAG, "OS flag clear failed");
        vTaskDelay(pdMS_TO_TICKS(RTC_OSC_POLL_MS));
        ESP_RETURN_ON_ERROR(
            pcf85063_read_(PCF85063_REG_SC, &sec, 1),
            TAG, "Seconds read during oscillator check failed");

        if ((sec & PCF85063_SC_OS) == 0) {
            ESP_LOGI(TAG, "Oscillator running; OS cleared after %d ms", elapsed_ms);
            return ESP_OK;
        }
        sec &= (uint8_t)~PCF85063_SC_OS;
    }

    ESP_LOGE(TAG, "Oscillator did not start within %d ms", RTC_OSC_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t rtc_verify_counter_running_(void)
{
    uint8_t ctrl1;
    uint8_t before;
    uint8_t after;

    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1),
        TAG, "CTRL1 read before tick test failed");
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_SC, &before, 1),
        TAG, "Seconds read before tick test failed");

    if ((ctrl1 & PCF85063_CTRL1_STOP) != 0) {
        ESP_LOGE(TAG, "Counter test failed: STOP is still set (CTRL1=0x%02X)", ctrl1);
        return ESP_ERR_INVALID_STATE;
    }
    if ((before & PCF85063_SC_OS) != 0) {
        ESP_LOGE(TAG, "Counter test failed: OS is set (SC=0x%02X)", before);
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelay(pdMS_TO_TICKS(RTC_TICK_TEST_MS));
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_SC, &after, 1),
        TAG, "Seconds read after tick test failed");

    if ((after & PCF85063_SC_OS) != 0) {
        ESP_LOGE(TAG, "Counter test failed: oscillator stopped (SC=0x%02X)", after);
        return ESP_ERR_INVALID_STATE;
    }
    if ((before & 0x7FU) == (after & 0x7FU)) {
        ESP_LOGE(TAG, "Counter test failed: seconds stuck at 0x%02X for %d ms",
                 before & 0x7FU, RTC_TICK_TEST_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Counter tick OK: %02u -> %02u in %d ms",
             (unsigned)bcd_to_dec(before & 0x7FU),
             (unsigned)bcd_to_dec(after & 0x7FU),
             RTC_TICK_TEST_MS);
    return ESP_OK;
}

static esp_err_t rtc_track_seconds_(uint8_t raw_sec)
{
    const int second = bcd_to_dec(raw_sec & 0x7FU);
    const int64_t now_us = esp_timer_get_time();

    if (second != s_last_second) {
        s_last_second = second;
        s_last_second_change_us = now_us;
        return ESP_OK;
    }

    if (s_last_second_change_us != 0 &&
        now_us - s_last_second_change_us >= RTC_STUCK_TIMEOUT_US) {
        uint8_t ctrl1 = 0xFF;
        esp_err_t ctrl_ret = pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1);
        ESP_LOGE(TAG, "Runtime check failed: seconds stuck at %02d, CTRL1=%s0x%02X STOP=%s",
                 second,
                 ctrl_ret == ESP_OK ? "" : "unreadable/",
                 ctrl1,
                 ctrl_ret == ESP_OK && (ctrl1 & PCF85063_CTRL1_STOP) ? "YES" : "no");
        s_last_second_change_us = now_us;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/* ─── Public API ──────────────────────────────────────────────────── */

esp_err_t app_rtc_init(void)
{
    if (s_rtc_ready) return ESP_OK;

    ESP_RETURN_ON_ERROR(waveshare_i2c_init(), TAG, "I2C init failed");

    esp_err_t ret = rtc_probe();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 not responding at address 0x%02X: %s (%d)",
                 RTC_PCF85063_I2C_ADDR, esp_err_to_name(ret), (int)ret);
        return ret;
    }
    ESP_LOGI(TAG, "PCF85063 found at I2C addr 0x%02X", RTC_PCF85063_I2C_ADDR);

    uint8_t ctrl1;
    uint8_t sec;
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1),
        TAG, "Initial CTRL1 read failed");
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_SC, &sec, 1),
        TAG, "Initial seconds read failed");
    (void)rtc_dump_regs();

    const uint8_t entry_ctrl1 = ctrl1;
    const bool os_flag = (sec & PCF85063_SC_OS) != 0;
    const bool stopped = (ctrl1 & PCF85063_CTRL1_STOP) != 0;
    const bool unsupported_mode =
        (ctrl1 & (PCF85063_CTRL1_EXT_TEST | PCF85063_CTRL1_12_24)) != 0;

    ESP_LOGI(TAG, "State on entry: CTRL1=0x%02X OS=%s STOP=%s mode=%s CAP_SEL=%s",
             ctrl1,
             os_flag ? "SET" : "clear",
             stopped ? "YES" : "no",
             (ctrl1 & PCF85063_CTRL1_12_24) ? "12h" : "24h",
             (ctrl1 & PCF85063_CTRL1_CAP_SEL) ? "12.5pF" : "7pF");

    /* Known production state: oscillator source, counter running, 24-hour
     * mode, correction IRQ off and 7 pF internal load. Keeping corrupted
     * CTRL1 bits (especially EXT_TEST or STOP) can freeze the clock. */
    ctrl1 = 0x00;
    ESP_RETURN_ON_ERROR(
        pcf85063_write_(PCF85063_REG_CTRL1, &ctrl1, 1),
        TAG, "CTRL1 write failed");
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1),
        TAG, "CTRL1 readback failed");
    if (ctrl1 != 0x00) {
        ESP_LOGE(TAG, "CTRL1 verify failed: wrote 0x00, read 0x%02X", ctrl1);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "CTRL1 normalized: 0x%02X -> 0x%02X (STOP cleared)",
             entry_ctrl1, ctrl1);

    if (os_flag) {
        ESP_LOGW(TAG, "OS flag is set; checking crystal startup for up to %d ms",
                 RTC_OSC_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(
            rtc_wait_for_oscillator_(sec),
            TAG, "Oscillator startup check failed");
    }

    uint8_t ctrl2 = 0x00;
    ESP_RETURN_ON_ERROR(
        pcf85063_write_(PCF85063_REG_CTRL2, &ctrl2, 1),
        TAG, "CTRL2 write failed");

    ESP_RETURN_ON_ERROR(
        rtc_verify_counter_running_(),
        TAG, "RTC counter is not running");

    s_rtc_ready = true;
    s_rtc_time_valid = !os_flag && !stopped && !unsupported_mode;
    s_last_second = -1;
    s_last_second_change_us = 0;

    ESP_LOGI(TAG, "RTC health: I2C=OK oscillator=RUNNING counter=RUNNING time=%s",
             s_rtc_time_valid ? "VALID" : "NEEDS_SYNC");
    if (!s_rtc_time_valid) {
        ESP_LOGW(TAG, "Stored RTC time is not trusted (OS=%s STOP=%s CTRL1=0x%02X)",
                 os_flag ? "SET" : "clear", stopped ? "YES" : "no", entry_ctrl1);
    }
    (void)rtc_dump_regs();
    return ESP_OK;
}

esp_err_t rtc_get_time_tm(struct tm *tm)
{
    uint8_t buf[7];

    if (!tm) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(app_rtc_init(), TAG, "RTC init failed");
    if (!s_rtc_time_valid) {
        ESP_LOGW(TAG, "RTC counter runs, but stored time needs synchronization");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = pcf85063_read_(PCF85063_REG_SC, buf, sizeof(buf));
    if (ret != ESP_OK) {
        /* Log the error code and clear the ready flag so the next call
         * reinitializes the chip. */
        ESP_LOGE(TAG, "Time read failed: %s (%d), will re-init on next cycle",
                 esp_err_to_name(ret), (int)ret);
        s_rtc_ready = false;
        return ret;
    }

    if (buf[0] & PCF85063_SC_OS) {
        ESP_LOGW(TAG, "Oscillator stop flag set — RTC lost power, time invalid");
        ESP_LOGI(TAG, "Raw regs: SC=0x%02X MN=0x%02X HR=0x%02X DM=0x%02X DW=0x%02X MO=0x%02X YR=0x%02X",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
        s_rtc_time_valid = false;
        return ESP_FAIL;
    }

    memset(tm, 0, sizeof(*tm));
    tm->tm_sec  = bcd_to_dec(buf[PCF85063_REG_SC - PCF85063_REG_SC] & 0x7F);
    tm->tm_min  = bcd_to_dec(buf[PCF85063_REG_MN - PCF85063_REG_SC] & 0x7F);
    tm->tm_hour = bcd_to_dec(buf[PCF85063_REG_HR - PCF85063_REG_SC] & 0x3F);
    tm->tm_mday = bcd_to_dec(buf[PCF85063_REG_DM - PCF85063_REG_SC] & 0x3F);
    tm->tm_wday = bcd_to_dec(buf[PCF85063_REG_DW - PCF85063_REG_SC] & 0x07);
    tm->tm_mon  = bcd_to_dec(buf[PCF85063_REG_MO - PCF85063_REG_SC] & 0x1F) - 1;
    tm->tm_year = bcd_to_dec(buf[PCF85063_REG_YR - PCF85063_REG_SC]) + 100;
    tm->tm_isdst = 0;

    /* Validate the value. */
    if (tm->tm_sec > 59 || tm->tm_min > 59 || tm->tm_hour > 23 ||
        tm->tm_mday < 1 || tm->tm_mday > 31 ||
        tm->tm_mon < 0 || tm->tm_mon > 11 ||
        tm->tm_wday < 0 || tm->tm_wday > 6) {
        ESP_LOGW(TAG, "Time validation failed: %02d:%02d:%02d %02d/%02d/%02d wday=%d",
                 tm->tm_hour, tm->tm_min, tm->tm_sec,
                 tm->tm_mday, tm->tm_mon + 1, tm->tm_year, tm->tm_wday);
        return ESP_FAIL;
    }

    ret = rtc_track_seconds_(buf[0]);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGD(TAG, "RTC read OK: %02d:%02d:%02d %02d/%02d/%04d",
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);

    return ESP_OK;
}

esp_err_t rtc_set_time_epoch(time_t epoch)
{
    struct tm tm;
    uint8_t buf[7];
    uint8_t verify[7];
    uint8_t ctrl1;

    if (epoch == (time_t)-1) return ESP_ERR_INVALID_ARG;
    gmtime_r(&epoch, &tm);
    if (tm.tm_year < 100 || tm.tm_year > 199) return ESP_ERR_INVALID_ARG;

    buf[PCF85063_REG_SC - PCF85063_REG_SC] = dec_to_bcd((uint8_t)tm.tm_sec);
    buf[PCF85063_REG_MN - PCF85063_REG_SC] = dec_to_bcd((uint8_t)tm.tm_min);
    buf[PCF85063_REG_HR - PCF85063_REG_SC] = dec_to_bcd((uint8_t)tm.tm_hour);
    buf[PCF85063_REG_DM - PCF85063_REG_SC] = dec_to_bcd((uint8_t)tm.tm_mday);
    buf[PCF85063_REG_DW - PCF85063_REG_SC] = dec_to_bcd((uint8_t)tm.tm_wday);
    buf[PCF85063_REG_MO - PCF85063_REG_SC] = dec_to_bcd((uint8_t)(tm.tm_mon + 1));
    buf[PCF85063_REG_YR - PCF85063_REG_SC] = dec_to_bcd((uint8_t)(tm.tm_year % 100));

    ESP_RETURN_ON_ERROR(app_rtc_init(), TAG, "RTC init failed");

    ESP_LOGI(TAG, "Setting RTC: %02d:%02d:%02d %02d/%02d/%04d",
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);

    ESP_RETURN_ON_ERROR(
        pcf85063_write_(PCF85063_REG_SC, buf, sizeof(buf)),
        TAG, "Time write failed");
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_CTRL1, &ctrl1, 1),
        TAG, "CTRL1 read after time write failed");
    ESP_RETURN_ON_ERROR(
        pcf85063_read_(PCF85063_REG_SC, verify, sizeof(verify)),
        TAG, "Time readback failed");

    if ((ctrl1 & PCF85063_CTRL1_STOP) != 0 ||
        (verify[0] & PCF85063_SC_OS) != 0) {
        ESP_LOGE(TAG, "Time write verify failed: CTRL1=0x%02X SC=0x%02X",
                 ctrl1, verify[0]);
        s_rtc_time_valid = false;
        return ESP_ERR_INVALID_STATE;
    }

    s_rtc_time_valid = true;
    s_last_second = -1;
    s_last_second_change_us = 0;
    ESP_LOGI(TAG, "RTC synchronized and verified: raw SC=%02X MN=%02X HR=%02X "
             "DM=%02X DW=%02X MO=%02X YR=%02X",
             verify[0], verify[1], verify[2], verify[3],
             verify[4], verify[5], verify[6]);
    return ESP_OK;
}
