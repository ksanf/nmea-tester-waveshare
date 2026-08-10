/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   GT911 + LVGL, IRQ/polling by PIN_TOUCH_IRQ, cached sampling, RAW+CAL logs.
 */

#include "touch/touch_driver.h"
#include "config/config_pins.h"
#include "config/config_nmea_tester.h"
#include "system/board_waveshare.h"

#include <string.h>
#include <esp_check.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_TOUCH_DRIVER
#include "config_logs.h"
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch_gt911.h>
#include <nvs_flash.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdatomic.h>

#define TAG "touch_drv"

#define TOUCH_I2C_PORT         I2C_NUM_0

#define LOG_INTERVAL_MS        500
#define TOUCH_TIMER_PERIOD_MS  15  // LVGL timer period (ms)

#define TOUCH_IRQ_VALID (PIN_TOUCH_IRQ >= 0)
#define GT911_RESET_HOLD_MS    20
#define GT911_ADDR_LATCH_MS    10
#define GT911_RESET_BOOT_MS    120
#define I2C_PROBE_TIMEOUT_MS   20

/* ───── Log control ───────────────────────────────────────────────────
 * Enable through LOG_CFG_TOUCH_DRIVER in config_logs.h.
 * When disabled, this driver emits no logs.
 */
#if TOUCH_LOG_ENABLED
    #define TOUCH_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
    #define TOUCH_LOGI(...) do {} while (0)
#endif

static esp_lcd_touch_handle_t s_touch = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

static lv_indev_t *g_indev = NULL;
static lv_timer_t *s_lv_timer = NULL;

static SemaphoreHandle_t s_spi_mutex = NULL;
static SemaphoreHandle_t s_cache_mutex = NULL;

typedef struct {
    uint16_t raw_x;
    uint16_t raw_y;

    int32_t cal_x;
    int32_t cal_y;

    uint8_t points;
    uint16_t strength;

    esp_err_t rd_ret;
    esp_err_t gr_ret;

    bool pressed;
    uint64_t ts_ms;
} touch_sample_t;

static touch_sample_t s_sample = {0};

#if TOUCH_IRQ_VALID
static atomic_bool s_touch_pending = ATOMIC_VAR_INIT(false);
static bool s_isr_service_installed = false;

static inline bool touch_irq_active(void)
{
    // Active-low: 0 == pressed
    return gpio_get_level(PIN_TOUCH_IRQ) == 0;
}

static void IRAM_ATTR touch_gpio_isr(void *arg)
{
    (void)arg;
    // No SPI, no logs here.
    atomic_store_explicit(&s_touch_pending, true, memory_order_relaxed);
}
#endif

static inline uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline TickType_t ms_to_ticks_min1_(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static esp_err_t i2c_probe_addr_(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    esp_err_t ret = i2c_master_start(cmd);
    if (ret == ESP_OK) {
        ret = i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    }
    if (ret == ESP_OK) {
        ret = i2c_master_stop(cmd);
    }
    if (ret == ESP_OK) {
        ret = i2c_master_cmd_begin(TOUCH_I2C_PORT,
                                   cmd,
                                   ms_to_ticks_min1_(I2C_PROBE_TIMEOUT_MS));
    }

    i2c_cmd_link_delete(cmd);
    return ret;
}

static void touch_release_resources_(void)
{
    if (s_lv_timer) {
        lv_timer_del(s_lv_timer);
        s_lv_timer = NULL;
    }

    if (g_indev) {
        lv_indev_delete(g_indev);
        g_indev = NULL;
    }

#if TOUCH_IRQ_VALID
    if (s_isr_service_installed) {
        (void)gpio_isr_handler_remove(PIN_TOUCH_IRQ);
    }
    atomic_store_explicit(&s_touch_pending, false, memory_order_relaxed);
#endif

    if (s_touch) {
        esp_lcd_touch_del(s_touch);
        s_touch = NULL;
    }

    if (s_io_handle) {
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
    }

    if (s_cache_mutex) {
        vSemaphoreDelete(s_cache_mutex);
        s_cache_mutex = NULL;
    }

    if (s_spi_mutex) {
        vSemaphoreDelete(s_spi_mutex);
        s_spi_mutex = NULL;
    }

}

#if TOUCH_IRQ_VALID
static esp_err_t gt911_select_i2c_addr_(uint8_t dev_addr)
{
    uint32_t int_level;
    if (dev_addr == ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS) {
        int_level = 0;
    } else if (dev_addr == ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP) {
        int_level = 1;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t int_out = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "GT911 INT output config failed");

    ESP_RETURN_ON_ERROR(waveshare_touch_set_reset(false), TAG, "GT911 reset assert failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_TOUCH_IRQ, 0), TAG, "GT911 INT preselect failed");
    vTaskDelay(ms_to_ticks_min1_(GT911_RESET_HOLD_MS));

    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_TOUCH_IRQ, int_level), TAG, "GT911 INT address select failed");
    vTaskDelay(ms_to_ticks_min1_(GT911_ADDR_LATCH_MS));

    ESP_RETURN_ON_ERROR(waveshare_touch_set_reset(true), TAG, "GT911 reset release failed");
    vTaskDelay(ms_to_ticks_min1_(GT911_ADDR_LATCH_MS));

    const gpio_config_t int_in = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_in), TAG, "GT911 INT input release failed");
    vTaskDelay(ms_to_ticks_min1_(GT911_RESET_BOOT_MS));

    return ESP_OK;
}
#else
static esp_err_t gt911_select_i2c_addr_(uint8_t dev_addr)
{
    (void)dev_addr;
    ESP_RETURN_ON_ERROR(waveshare_touch_reset(), TAG, "GT911 reset failed");
    return ESP_OK;
}
#endif

static esp_err_t gt911_create_at_addr_(uint8_t dev_addr,
                                       esp_lcd_touch_handle_t *touch,
                                       esp_lcd_panel_io_handle_t *io_handle)
{
    if (!touch || !io_handle) return ESP_ERR_INVALID_ARG;

    *touch = NULL;
    *io_handle = NULL;

    ESP_RETURN_ON_ERROR(gt911_select_i2c_addr_(dev_addr), TAG, "GT911 address select failed");
    ESP_RETURN_ON_ERROR(i2c_probe_addr_(dev_addr), TAG, "GT911 address did not ACK");

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = dev_addr;

    esp_err_t ret = esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_I2C_PORT,
                                             &io_cfg,
                                             io_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_touch_io_gt911_config_t gt911_cfg = {
        .dev_addr = dev_addr,
    };

    const esp_lcd_touch_config_t tcfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = -1,
        .int_gpio_num = (TOUCH_IRQ_VALID ? PIN_TOUCH_IRQ : -1),
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = TOUCH_SWAP_XY,
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y
        },
        .driver_data = &gt911_cfg,
    };

    ret = esp_lcd_touch_new_i2c_gt911(*io_handle, &tcfg, touch);
    if (ret != ESP_OK) {
        (void)esp_lcd_panel_io_del(*io_handle);
        *io_handle = NULL;
        *touch = NULL;
    }

    return ret;
}

static esp_err_t touch_configure_irq_(void)
{
#if TOUCH_IRQ_VALID
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "touch IRQ config failed");

    if (!s_isr_service_installed) {
        esp_err_t ir = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (ir == ESP_OK || ir == ESP_ERR_INVALID_STATE) {
            s_isr_service_installed = true;
        } else {
            return ir;
        }
    }

    (void)gpio_isr_handler_remove(PIN_TOUCH_IRQ);
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_TOUCH_IRQ, touch_gpio_isr, NULL),
                        TAG, "touch IRQ handler add failed");

    if (touch_irq_active()) {
        atomic_store_explicit(&s_touch_pending, true, memory_order_relaxed);
    }
#endif
    return ESP_OK;
}

static bool cache_get(touch_sample_t *dst)
{
    if (!dst || !s_cache_mutex) return false;
    if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        *dst = s_sample;
        xSemaphoreGive(s_cache_mutex);
        return true;
    }
    return false;
}

static void cache_set(const touch_sample_t *src)
{
    if (!src || !s_cache_mutex) return;
    if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_sample = *src;
        xSemaphoreGive(s_cache_mutex);
    }
}

/**
 * @brief The only function that reads touch data from I2C.
 * @param force If true, reads regardless of IRQ/pending (used by calibrator via read_raw).
 */
static void touch_poll_once(bool force)
{
    if (!s_touch || !s_spi_mutex || !s_cache_mutex) return;

#if TOUCH_IRQ_VALID
    if (!force) {
        // Read only while touch is active/pending. If the release edge is missed,
        // still clear the cached state so LVGL cannot keep repeating a stale press.
        if (!atomic_load_explicit(&s_touch_pending, memory_order_relaxed) &&
            !touch_irq_active()) {
            touch_sample_t released;
            memset(&released, 0, sizeof(released));
            released.ts_ms = now_ms();
            cache_set(&released);
            return;
        }
    }
#endif

    uint16_t x = 0, y = 0;
    uint16_t strength = 0;
    uint8_t points = 0;
    esp_err_t rd = ESP_FAIL;
    bool got = false;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        rd = esp_lcd_touch_read_data(s_touch);
        if (rd == ESP_OK) {
            esp_lcd_touch_point_data_t pt = {0};
            uint8_t cnt = 0;
            if (esp_lcd_touch_get_data(s_touch, &pt, &cnt, 1) == ESP_OK && cnt > 0) {
                x = pt.x; y = pt.y; strength = pt.strength; points = cnt;
                got = true;
            }
        }
        xSemaphoreGive(s_spi_mutex);
    } else {
        return;
    }

    // Decide pressed based on points + range
    const bool in_range = (x < LCD_WIDTH && y < LCD_HEIGHT);
    const bool pressed = (points > 0) && in_range;

    int32_t cal_x = (int32_t)x;
    int32_t cal_y = (int32_t)y;

    touch_sample_t out;
    memset(&out, 0, sizeof(out));
    out.raw_x = x;
    out.raw_y = y;
    out.cal_x = cal_x;
    out.cal_y = cal_y;
    out.points = points;
    out.strength = strength;
    out.rd_ret = rd;
    out.gr_ret = got ? ESP_OK : ESP_ERR_NOT_FOUND;
    out.pressed = pressed;
    out.ts_ms = now_ms();

    cache_set(&out);

#if TOUCH_IRQ_VALID
    // Keep polling while IRQ stays low, stop when released.
    atomic_store_explicit(&s_touch_pending, touch_irq_active(),
                          memory_order_relaxed);
#endif

    // Throttled logs RAW+CAL
    static uint64_t last_log_ms = 0;
    if (out.ts_ms - last_log_ms >= LOG_INTERVAL_MS) {
        last_log_ms = out.ts_ms;

        if (pressed) {
            TOUCH_LOGI(
                     "POLL: pressed rd=%s(%d) gr=%s(%d) points=%u str=%u RAW=(%u,%u) CAL=(%ld,%ld)",
                     esp_err_to_name(rd), (int)rd,
                     esp_err_to_name(out.gr_ret), (int)out.gr_ret,
                     points, strength, x, y, (long)cal_x, (long)cal_y);
        } else {
            TOUCH_LOGI(
                     "POLL: released rd=%s(%d) gr=%s(%d) points=%u str=%u RAW=(%u,%u)",
                     esp_err_to_name(rd), (int)rd,
                     esp_err_to_name(out.gr_ret), (int)out.gr_ret,
                     points, strength, x, y);
        }
    }
}

/* LVGL timer callback: runs in LVGL task context */
static void lv_timer_cb(lv_timer_t *t)
{
    (void)t;
    touch_poll_once(false);
}

/* LVGL input callback (consumer): reads cache only */
static void touch_read_cb(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (!data) return;

    touch_sample_t snap;
    if (!cache_get(&snap)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (!snap.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Protect against out-of-range after calibration
    if (snap.cal_x < 0 || snap.cal_y < 0 ||
        snap.cal_x >= (int32_t)LCD_WIDTH || snap.cal_y >= (int32_t)LCD_HEIGHT) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point.x = snap.cal_x;
    data->point.y = snap.cal_y;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* Public API: raw read for calibrator/menu. Forces a poll to avoid dependence on LVGL timers. */
bool touch_driver_read_raw(uint16_t *x, uint16_t *y)
{
    if (!x || !y) return false;

    touch_poll_once(true);

    touch_sample_t snap;
    if (!cache_get(&snap)) return false;
    if (!snap.pressed) return false;

    *x = snap.raw_x;
    *y = snap.raw_y;
    return true;
}

/* ────────────────────────────── Public API ────────────────────────────── */

esp_err_t nmea_touch_init(lv_disp_t *disp)
{
    if (!disp) {
        TOUCH_LOGI("nmea_touch_init: disp == NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS init failed");

    ESP_RETURN_ON_ERROR(waveshare_i2c_init(), TAG, "I2C init failed");

    const uint8_t gt911_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };

    for (size_t i = 0; i < sizeof(gt911_addrs) / sizeof(gt911_addrs[0]); ++i) {
        ret = gt911_create_at_addr_(gt911_addrs[i], &s_touch, &s_io_handle);
        if (ret == ESP_OK) {
            break;
        }
    }

    if (ret != ESP_OK) {
        touch_release_resources_();
        return ret;
    }

    s_spi_mutex = xSemaphoreCreateMutex();
    if (!s_spi_mutex) {
        touch_release_resources_();
        return ESP_ERR_NO_MEM;
    }

    s_cache_mutex = xSemaphoreCreateMutex();
    if (!s_cache_mutex) {
        touch_release_resources_();
        return ESP_ERR_NO_MEM;
    }

    ret = touch_configure_irq_();
    if (ret != ESP_OK) {
        touch_release_resources_();
        return ret;
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    indev_drv.disp = disp;
    indev_drv.long_press_time = UI_LONG_PRESS_TIME_MS;
    indev_drv.long_press_repeat_time = UI_LONG_PRESS_REPEAT_MS;
    g_indev = lv_indev_drv_register(&indev_drv);
    if (!g_indev) {
        touch_release_resources_();
        return ESP_FAIL;
    }

    s_lv_timer = lv_timer_create(lv_timer_cb, TOUCH_TIMER_PERIOD_MS, NULL);
    if (!s_lv_timer) {
        touch_release_resources_();
        return ESP_FAIL;
    }

    TOUCH_LOGI("Touch init OK. mode=%s IRQ=%d timer=%dms x_max=%d y_max=%d",
               TOUCH_IRQ_VALID ? "IRQ" : "POLL",
               (int)PIN_TOUCH_IRQ,
               (int)TOUCH_TIMER_PERIOD_MS,
               (int)TOUCH_X_MAX,
               (int)TOUCH_Y_MAX);

    return ESP_OK;
}

lv_indev_t *touch_driver_get_indev(void)
{
    return g_indev;
}

/* touch_calibration removed — GT911 is hardware-calibrated */
