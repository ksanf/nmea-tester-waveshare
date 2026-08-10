/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Main entry point for NMEA-Tester firmware.
 */

#include "config/config_nmea_tester.h"
#include "config/config_pins.h"
#include "ui/screens/screen_init.h"
#include "lvgl_port/waveshare_lvgl_port.h"
#include "ui/ui_theme.h"
#include "touch/touch_driver.h"
#include "ui/screens/screen_ui.h"
#include "rs485/rs485_parser.h"
#include "system/nmea_clock.h"
#include "system/telnet_server.h"
#include "system/telnet_router.h"
#include "system/udp_nmea_server.h"
#include "wifi/wifi_manager.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_version.h"
#include <driver/gpio.h> 
#include <nvs_flash.h>
#include <esp_err.h>
#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_APP_MAIN
#include "config_logs.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <esp_freertos_hooks.h>
#include <inttypes.h>
#include <stdatomic.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Module log tag */
static const char *TAG = "app_main";
#if LOG_CFG_MEM_MONITOR
static const char *MEM_TAG = "MEM";
#endif

#define INTERFACE_POWER_SETTLE_MS 50

static esp_err_t nvs_init_with_recovery_(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs recovery: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}
#if LOG_CFG_MEM_MONITOR
#define MEM_MONITOR_PERIOD_MS 5000
#define MEM_MONITOR_STACK     5120
#define MEM_MONITOR_CORE      1
#define MEM_MONITOR_PRIO      5

typedef struct {
    const char *name;
    TaskHandle_t handle;
} task_probe_t;

static _Atomic uint32_t s_idle_hits[2] = { 0, 0 };
static uint32_t s_idle_peak[2] = { 1, 1 };
#endif

/**
 * @brief Log current heap usage
 * @param stage Identifier string for logging context
 */
#if LOG_CFG_APP_MAIN
static void log_heap(const char *stage)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "[%s] Heap free: %u, largest block: %u",
             stage, info.total_free_bytes, info.largest_free_block);
}
#else
static inline void log_heap(const char *stage)
{
    (void)stage;
}
#endif

#if LOG_CFG_MEM_MONITOR
/* ─── Format byte counts for human-readable output ───────────────── */
static const char *fmt_bytes_(uint32_t bytes)
{
    static char b[4][16];
    static int idx = 0;
    char *buf = b[idx]; idx = (idx + 1) & 3;
    if (bytes >= 1024 * 1024) {
        snprintf(buf, 16, "%" PRIu32 ".%" PRIu32 "M",
                 bytes / (1024 * 1024), (bytes / 102400) % 10);
    } else if (bytes >= 1024) {
        snprintf(buf, 16, "%" PRIu32 "K", bytes / 1024);
    } else {
        snprintf(buf, 16, "%" PRIu32 "B", bytes);
    }
    return buf;
}

/* ─── Report a consistent memory-pool snapshot with heap_caps_get_info ─ */
static void log_pool_(const char *label, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    size_t used  = info.total_allocated_bytes;
    size_t free  = info.total_free_bytes;
    size_t total = used + free;
    if (total == 0) return;

    uint32_t pct = (uint32_t)((used * 100ULL) / total);
    CFG_LOGI(LOG_CFG_MEM_MONITOR, MEM_TAG,
             "%6s  total=%7s  used=%7s (%2" PRIu32 "%%)  free=%7s  minfree=%7s  largest=%7s",
             label,
             fmt_bytes_((uint32_t)total), fmt_bytes_((uint32_t)used), pct,
             fmt_bytes_((uint32_t)free), fmt_bytes_((uint32_t)info.minimum_free_bytes),
             fmt_bytes_((uint32_t)info.largest_free_block));
}

static inline uint32_t stack_hwm_bytes_(TaskHandle_t h)
{
    if (!h) return 0;
    return (uint32_t)uxTaskGetStackHighWaterMark2(h) * (uint32_t)sizeof(StackType_t);
}

static inline const char *task_core_str_(TaskHandle_t h)
{
    BaseType_t core;

    if (!h) return "NA";
    core = xTaskGetCoreID(h);
    if (core == 0) return "C0";
    if (core == 1) return "C1";
    return "NA";
}

static bool idle_hook_core0_(void)
{
    atomic_fetch_add_explicit(&s_idle_hits[0], 1u, memory_order_relaxed);
    return false;
}

static bool idle_hook_core1_(void)
{
    atomic_fetch_add_explicit(&s_idle_hits[1], 1u, memory_order_relaxed);
    return false;
}

static void log_idle_approx_(void)
{
    static uint32_t prev_hits[2] = { 0, 0 };
    uint32_t delta[2];
    uint32_t idle_pct[2];

    for (int i = 0; i < 2; ++i) {
        const uint32_t hits = atomic_load_explicit(&s_idle_hits[i],
                                                   memory_order_relaxed);
        delta[i] = hits - prev_hits[i];
        prev_hits[i] = hits;
        if (delta[i] > s_idle_peak[i]) {
            s_idle_peak[i] = delta[i];
        }
        idle_pct[i] = (s_idle_peak[i] > 0) ? (delta[i] * 100u) / s_idle_peak[i] : 0u;
    }

    CFG_LOGI(LOG_CFG_MEM_MONITOR, MEM_TAG,
             "IDLE C0=%" PRIu32 "%% (%" PRIu32 ") C1=%" PRIu32 "%% (%" PRIu32 ")",
             idle_pct[0], delta[0], idle_pct[1], delta[1]);
}

static void log_stack_group_(const task_probe_t *tasks, size_t count)
{
    char line[320];
    size_t off = 0;

    for (size_t i = 0; i < count; ++i) {
        TaskHandle_t h = tasks[i].handle ? tasks[i].handle : xTaskGetHandle(tasks[i].name);
        int n = snprintf(line + off, sizeof(line) - off, "%s%s@%s=%" PRIu32 "B",
                         i ? " " : "",
                         tasks[i].name,
                         task_core_str_(h),
                         stack_hwm_bytes_(h));
        if (n < 0 || (size_t)n >= sizeof(line) - off) break;
        off += (size_t)n;
    }

    if (off > 0) {
        CFG_LOGI(LOG_CFG_MEM_MONITOR, MEM_TAG, "%s", line);
    }
}

static void mem_monitor_task_(void *arg)
{
    (void)arg;

    const task_probe_t group_a[] = {
        { "main", NULL },
        { "nmea_clock", NULL },
        { "nmeaEngine", NULL },
        { "rs485_prx", NULL },
        { "telnet_srv", NULL },
        { "udp_nmea", NULL },
    };
    const task_probe_t group_b[] = {
        { "pipe", NULL },
        { "term_rx", NULL },
        { "rs485_bridge", NULL },
        { "n2k_rx", NULL },
        { "can_watchdog", NULL },
        { "IDLE0", NULL },
        { "IDLE1", NULL },
    };

    while (true) {
        CFG_LOGI(LOG_CFG_MEM_MONITOR, MEM_TAG,
                 "══════ Memory Monitor ══════");
        log_pool_("DRAM",  MALLOC_CAP_INTERNAL);
        log_pool_("PSRAM", MALLOC_CAP_SPIRAM);
        log_pool_("DMA",   MALLOC_CAP_DMA);
        /* Idle load and task stacks. */
        log_idle_approx_();
        log_stack_group_(group_a, sizeof(group_a) / sizeof(group_a[0]));
        log_stack_group_(group_b, sizeof(group_b) / sizeof(group_b[0]));
        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_PERIOD_MS));
    }
}
#endif

static void interface_power_init_(void)
{
#if PIN_CGQ_EN >= 0
    const gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_CGQ_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    vTaskDelay(pdMS_TO_TICKS(INTERFACE_POWER_SETTLE_MS));
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    /* Keep shared interface drivers rail enabled from boot to avoid
     * brownout-inducing inrush when opening parser/TX/CAN screens later. */
    gpio_set_level(PIN_CGQ_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(INTERFACE_POWER_SETTLE_MS));
#endif
}

void app_main(void)
{
    ESP_LOGW(TAG, "Starting NMEA-Tester ...");
    interface_power_init_();

    log_heap("boot");

    // 1) Initialize NVS-backed settings
    ESP_ERROR_CHECK(nvs_init_with_recovery_());
    log_heap("after nvs_init");
    ESP_ERROR_CHECK(ui_theme_init());
    log_heap("after ui_theme_init");
    ESP_ERROR_CHECK(nmea_version_init());
    log_heap("after nmea_version_init");
    ESP_ERROR_CHECK(nmea_templates_init());
    log_heap("after nmea_templates_init");

    // 2) Initialize clock
    nmea_clock_init();
    log_heap("after nmea_clock_init");

    // 3) Initialize display and LVGL
    screen_init();
    log_heap("after screen_init");

    // 4) Initialize touch input
    esp_err_t touch_err = ESP_ERR_TIMEOUT;
    if (lvgl_port_lock(-1)) {
        touch_err = nmea_touch_init(screen_get_display());
        lvgl_port_unlock();
    }
    if (touch_err != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed: %s, continuing without touch",
                 esp_err_to_name(touch_err));
    }
    log_heap("after touch_init");

    // 5) Build main UI (Parser, Transmitter, CAN)
    if (lvgl_port_lock(-1)) {
        screen_ui_init();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for main screen init");
    }
    log_heap("after screen_ui_init");

    telnet_router_init();
    esp_err_t net_err = wifi_manager_init();
    if (net_err == ESP_OK && wifi_manager_is_started()) {
        esp_err_t srv_err = telnet_server_start();
        if (srv_err != ESP_OK) {
            ESP_LOGW(TAG, "Telnet server start failed: %s", esp_err_to_name(srv_err));
        } else {
            srv_err = udp_nmea_server_start();
        }
        if (srv_err != ESP_OK) {
            ESP_LOGW(TAG, "Network service startup failed: %s", esp_err_to_name(srv_err));
            (void)udp_nmea_server_stop();
            (void)telnet_server_stop();
            (void)wifi_manager_deinit();
        }
    } else if (net_err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi disabled, network services not started");
    } else {
        ESP_LOGE(TAG, "WiFi init failed, continuing without network services: %s",
                 esp_err_to_name(net_err));
    }
    log_heap("after wifi_init");
    if (lvgl_port_lock(-1)) {
        screen_ui_show();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for final screen refresh");
    }

#if LOG_CFG_MEM_MONITOR
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_hook_core0_, 0));
    ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(idle_hook_core1_, 1));
    if (xTaskCreatePinnedToCore(mem_monitor_task_, "mem_mon",
                                MEM_MONITOR_STACK, NULL, MEM_MONITOR_PRIO,
                                NULL, MEM_MONITOR_CORE) != pdPASS) {
        CFG_LOGE(LOG_CFG_MEM_MONITOR, MEM_TAG, "Memory monitor task create failed");
    }
#endif

    ESP_LOGW(TAG, "System initialized and running.");
    // Prevent main task from exiting
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
