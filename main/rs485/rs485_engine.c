/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 */

#include "rs485/rs485_simui.h"
#include "app/navigation_model.h"
#include "config/config_nmea_tester.h"
#include "config/memory_config.h"
#include "rs485/rs485_engine.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_regen.h"
#include "nmea_editor/nmea_wire.h"
#include "rs485/rs485_driver.h"
#include "ui/instrument_panel.h"
#include "ui/nmea_log.h"
#include "esp_log.h"
#define CFG_LOG_MODULE LOG_CFG_RS485_ENGINE
#include "config_logs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef void (*regen_fn)(char (*buf)[NMEA_SENT_MAX], uint8_t *cnt);

typedef struct {
    regen_fn regen;
    atomic_bool active;
    atomic_bool dirty;
    atomic_bool motion_reset;
    uint32_t next_ms;
    uint32_t motion_last_ms;
    bool motion_time_valid;
} group_t;

/* A full-size frame at 2400 baud may keep a write busy for almost 5 seconds. */
#define ENGINE_STOP_WAIT_MS 6000U
#define ENGINE_STOP_POLL_MS   10U

static _Atomic(TaskHandle_t) s_engine_task = NULL;
static atomic_bool s_engine_stop = ATOMIC_VAR_INIT(false);

static char (*s_lines_global)[NMEA_SENT_MAX] = NULL;
static bool stop_requested_(void)
{
    return atomic_load_explicit(&s_engine_stop, memory_order_acquire);
}

static bool lines_alloc_(void)
{
    if (s_lines_global) return true;
    s_lines_global = CALLOC_WHERE(RS485_ENGINE_LINES_IN_PSRAM, MAX_LINES_IN_GRP, sizeof(*s_lines_global));
    return s_lines_global != NULL;
}

static void lines_free_(void)
{
    free(s_lines_global);
    s_lines_global = NULL;
}

extern void regen_gps    (char (*)[NMEA_SENT_MAX], uint8_t *);
extern void regen_gyro   (char (*)[NMEA_SENT_MAX], uint8_t *);
extern void regen_log    (char (*)[NMEA_SENT_MAX], uint8_t *);
extern void regen_echo   (char (*)[NMEA_SENT_MAX], uint8_t *);
extern void regen_weather(char (*)[NMEA_SENT_MAX], uint8_t *);

static group_t G[GRP_COUNT] = {
    [GRP_GPS ] = { .regen = regen_gps    },
    [GRP_GYRO] = { .regen = regen_gyro   },
    [GRP_LOG ] = { .regen = regen_log    },
    [GRP_ECHO] = { .regen = regen_echo   },
    [GRP_WX  ] = { .regen = regen_weather},
};

static uint32_t period_ms(rs485_group_t grp)
{
    nmea_templates_snapshot_t templates;
    nmea_templates_snapshot_all(&templates);
    float hz =
        (grp == GRP_GPS ) ? templates.gps.rate_hz :
        (grp == GRP_GYRO) ? templates.gyro.rate_hz :
        (grp == GRP_LOG ) ? templates.log.rate_hz :
        (grp == GRP_ECHO) ? templates.echo.rate_hz : templates.weather.rate_hz;
    if (hz < 0.5f)  hz = 0.5f;
    if (hz > 10.0f) hz = 10.0f;
    return (uint32_t)(1000.0f / hz + 0.5f);
}

void rs485_engine_flush_ui(void)
{
    instrument_panel_flush();
}

static void dump_mem(void)
{
    uint32_t free_dram = esp_get_free_heap_size();
    uint32_t min_dram  = esp_get_minimum_free_heap_size();
    ESP_LOGI("MEM", "free=%u min=%u", free_dram, min_dram);
}

void rs485_engine_set_active(rs485_group_t grp, bool on)
{
    if ((unsigned)grp >= GRP_COUNT) return;

    if (on) {
        /* Publish dirty before active so the engine regenerates immediately. */
        atomic_store_explicit(&G[grp].dirty, true, memory_order_release);
        atomic_store_explicit(&G[grp].active, true, memory_order_release);
    } else {
        /* Make the engine stop before touching its pending UI snapshot. */
        atomic_store_explicit(&G[grp].active, false, memory_order_release);
        atomic_store_explicit(&G[grp].dirty, true, memory_order_release);
    }

    if (grp == GRP_GYRO) {
        atomic_store_explicit(&G[grp].motion_reset, true, memory_order_release);
    }


}

void rs485_engine_mark_dirty(rs485_group_t grp)
{
    if ((unsigned)grp < GRP_COUNT) {
        atomic_store_explicit(&G[grp].dirty, true, memory_order_release);
        if (grp == GRP_GYRO) {
            /* A newly edited heading or ROT starts a fresh time anchor. */
            atomic_store_explicit(&G[grp].motion_reset, true,
                                  memory_order_release);
        }
    }
}

static void engine_task(void *arg)
{
    char    (*lines)[NMEA_SENT_MAX] = s_lines_global;
    char      frame[NMEA_SENT_MAX + 3];
    uint8_t  cnt;
    const uint32_t LOG_PERIOD_MS = 5000;
    uint32_t next_log_ms = 0;

    ESP_LOGI("ENGINE", "Task started");

    while (!stop_requested_()) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        for (int g = 0; g < GRP_COUNT; g++) {
            if (stop_requested_()) break;

            group_t *gp = &G[g];
            if (!atomic_load_explicit(&gp->active, memory_order_acquire)) continue;

            const bool dirty = atomic_exchange_explicit(
                &gp->dirty, false, memory_order_acq_rel);
            if (dirty || (int32_t)(now_ms - gp->next_ms) >= 0) {
                if (g == GRP_GYRO) {
                    if (atomic_exchange_explicit(&gp->motion_reset, false,
                                                 memory_order_acq_rel)) {
                        gp->motion_time_valid = false;
                    }
                    if (gp->motion_time_valid) {
                        nmea_templates_gyro_advance(now_ms - gp->motion_last_ms);
                    }
                    gp->motion_last_ms = now_ms;
                    gp->motion_time_valid = true;
                }
                gp->regen(lines, &cnt);
                gp->next_ms = now_ms + period_ms(g);

                ESP_LOGD("ENGINE", "regen grp=%d cnt=%u first=\"%s\"",
                         g, (unsigned)cnt, cnt ? lines[0] : "");

                /* --- 1. RS-485 always runs at the configured group rate. -- */
                for (uint8_t i = 0; i < cnt; i++) {
                    if (stop_requested_() ||
                        !atomic_load_explicit(&gp->active, memory_order_acquire)) {
                        break;
                    }
                    const size_t frame_len =
                        nmea_wire_frame_build(frame, sizeof(frame), lines[i]);
                    if (frame_len == 0) {
                        ESP_LOGW("ENGINE", "invalid tx frame grp=%d line=%u",
                                 g, (unsigned)i);
                        continue;
                    }
                    if (rs485_driver_write(frame, frame_len) != ESP_OK) {
                        ESP_LOGW("ENGINE", "tx failed grp=%d line=%u len=%u",
                                 g, (unsigned)i, (unsigned)frame_len);
                    } else {
                        navigation_model_process(lines[i]);
                        rs485_simui_log_tx(frame);
                    }
                }


            }
        }

        if (!stop_requested_() && (int32_t)(now_ms - next_log_ms) >= 0) {
            dump_mem();
            next_log_ms = now_ms + LOG_PERIOD_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_STEP_MS));
    }

    ESP_LOGI("ENGINE", "Task exiting gracefully");

    /* Release lets deinit safely free resources after observing NULL. */
    atomic_store_explicit(&s_engine_task, NULL, memory_order_release);
    vTaskDelete(NULL);
}

bool rs485_engine_init(void)
{
    BaseType_t rc;
    TaskHandle_t task = NULL;

    if (atomic_load_explicit(&s_engine_task, memory_order_acquire)) return true;
    if (!lines_alloc_()) {
        ESP_LOGE("ENGINE", "line buffer alloc failed");
        return false;
    }
    atomic_store_explicit(&s_engine_stop, false, memory_order_release);
    rc = xTaskCreatePinnedToCore(engine_task, "nmeaEngine", 5120, NULL, 3, &task, 1);
    if (rc != pdPASS) {
        ESP_LOGE("ENGINE", "task create failed");
        lines_free_();
        return false;
    }
    atomic_store_explicit(&s_engine_task, task, memory_order_release);
    return true;
}

bool rs485_engine_deinit(void)
{
    if (!atomic_load_explicit(&s_engine_task, memory_order_acquire)) {
        lines_free_();
        return true;
    }

    atomic_store_explicit(&s_engine_stop, true, memory_order_release);

    /*
     * Never force-delete this task: it may be waiting for UART completion while
     * holding the shared RS-485 bus mutex. The timeout covers one maximum-size
     * write at the minimum supported baudrate.
     */
    for (uint32_t waited_ms = 0;
         waited_ms < ENGINE_STOP_WAIT_MS &&
             atomic_load_explicit(&s_engine_task, memory_order_acquire);
         waited_ms += ENGINE_STOP_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(ENGINE_STOP_POLL_MS));
    }

    if (atomic_load_explicit(&s_engine_task, memory_order_acquire)) {
        ESP_LOGE("ENGINE", "Task stop timed out; resources kept alive");
        return false;
    }

    lines_free_();

    for (int i = 0; i < GRP_COUNT; i++) {
        atomic_store_explicit(&G[i].active, false, memory_order_release);
        atomic_store_explicit(&G[i].dirty, false, memory_order_release);
        atomic_store_explicit(&G[i].motion_reset, false, memory_order_release);
        G[i].next_ms = 0;
        G[i].motion_last_ms = 0;
        G[i].motion_time_valid = false;
    }

    ESP_LOGI("ENGINE", "Deinitialized");
    return true;
}


bool rs485_engine_running(void)
{
    return atomic_load_explicit(&s_engine_task, memory_order_acquire) != NULL;
}

bool rs485_engine_group_active(rs485_group_t grp)
{
    return (unsigned)grp < GRP_COUNT && atomic_load_explicit(&G[grp].active, memory_order_acquire);
}
