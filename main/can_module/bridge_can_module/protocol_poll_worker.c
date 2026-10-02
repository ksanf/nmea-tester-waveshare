/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#include "protocol_poll_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define POLL_PERIOD_MS 20u
#define POLL_STACK_BYTES 4096u
#define POLL_PRIORITY 5u
static atomic_bool running = ATOMIC_VAR_INIT(false);
static atomic_bool stop_requested = ATOMIC_VAR_INIT(false);
static protocol_poll_fn_t poll_callback;

static TickType_t ticks_at_least_one(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static void poll_task(void *arg)
{
    (void)arg;
    /* Creation can schedule us before start() has published the running flag.
     * One startup notification is the only notification: no periodic backlog. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!atomic_load_explicit(&stop_requested, memory_order_acquire)) {
        TickType_t started = xTaskGetTickCount();
        poll_callback();
        if (atomic_load_explicit(&stop_requested, memory_order_acquire)) break;
        TickType_t elapsed = xTaskGetTickCount() - started;
        TickType_t period = ticks_at_least_one(POLL_PERIOD_MS);
        /* Missed periods are coalesced. A slow CAN transaction must not create a
         * catch-up burst that starves other tasks after the bus recovers. */
        vTaskDelay(elapsed < period ? period - elapsed : 1);
    }
    /* No profile resources/callbacks are touched after this publication. stop()
     * may now release them; only the FreeRTOS task itself remains to be reaped. */
    atomic_store_explicit(&running, false, memory_order_release);
    vTaskDelete(NULL);
}

bool protocol_poll_worker_start(protocol_poll_fn_t poll)
{
    if (!poll) return false;
    if (atomic_load_explicit(&running, memory_order_acquire))
        return !atomic_load_explicit(&stop_requested, memory_order_acquire);
    poll_callback = poll;
    atomic_store_explicit(&stop_requested, false, memory_order_release);
    TaskHandle_t task = NULL;
    if (xTaskCreate(poll_task, "can_fsm", POLL_STACK_BYTES, NULL,
                    POLL_PRIORITY, &task) != pdPASS) return false;
    atomic_store_explicit(&running, true, memory_order_release);
    xTaskNotifyGive(task);
    return true;
}

bool protocol_poll_worker_stop(uint32_t timeout_ms)
{
    atomic_store_explicit(&stop_requested, true, memory_order_release);
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= timeout) return false;
        vTaskDelay(ticks_at_least_one(10));
    }
    return true;
}

bool protocol_poll_worker_is_running(void)
{
    return atomic_load_explicit(&running, memory_order_acquire);
}
