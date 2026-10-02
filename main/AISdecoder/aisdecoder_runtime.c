/* Copyright (c) 2026 S.Zhurba. SPDX-License-Identifier: MIT */
#include "AISdecoder/aisdecoder_runtime.h"
#include "AISdecoder/aisdecoder_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/semphr.h"
#include <stdatomic.h>
static _Atomic(SemaphoreHandle_t) s_mutex = NULL;
/* Allocate outside a critical section; publish one mutex for the entire
 * application lifetime. A racing initializer disposes only its own candidate. */
static bool lock_(void)
{
    SemaphoreHandle_t mutex = atomic_load_explicit(&s_mutex, memory_order_acquire);
    if (!mutex) {
        SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
        if (!candidate) return false;
        SemaphoreHandle_t expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&s_mutex, &expected, candidate,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            mutex = candidate;
        } else {
            vSemaphoreDelete(candidate);
            mutex = expected;
        }
    }
    return xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}
static void unlock_(void)
{
    xSemaphoreGive(atomic_load_explicit(&s_mutex, memory_order_acquire));
}
#else
#include <pthread.h>
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool lock_(void) { return pthread_mutex_lock(&s_mutex) == 0; }
static void unlock_(void) { (void)pthread_mutex_unlock(&s_mutex); }
#endif

static aisdecoder_runtime_status_t s_status;

bool aisdecoder_runtime_start(void)
{
    bool ready;
    if (!lock_()) return false;
    if (!s_status.active) {
        aisdecoder_parse_reset();
        memset(&s_status, 0, sizeof(s_status));
        if (aisdecoder_decode_init()) {
            aisdecoder_decode_reset();
            s_status.active = true;
        }
    }
    ready = s_status.active;
    unlock_();
    return ready;
}

void aisdecoder_runtime_stop(void)
{
    if (!lock_()) return;
    s_status.active = false;
    aisdecoder_parse_reset();
    aisdecoder_decode_deinit();
    memset(&s_status, 0, sizeof(s_status));
    unlock_();
}

void aisdecoder_runtime_feed(const char *line)
{
    if (!aisdecoder_parse_is_sentence(line) || !lock_()) return;
    if (s_status.active) {
        s_status.lines_seen++;
        s_status.last_rx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        strlcpy(s_status.last_line, line, sizeof(s_status.last_line));
        aisdecoder_parse_feed_line(line);
        s_status.decoded_updates = aisdecoder_decode_updates();
    }
    unlock_();
}

size_t aisdecoder_runtime_snapshot(aisdecoder_target_t *out, size_t max_targets,
                                  uint32_t *decoded_updates)
{
    size_t count = 0;
    if (decoded_updates) *decoded_updates = 0;
    if (!lock_()) return 0;
    if (s_status.active) {
        const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        count = aisdecoder_decode_collect(out, max_targets, now_ms);
    }
    if (decoded_updates) *decoded_updates = s_status.decoded_updates;
    unlock_();
    return count;
}

void aisdecoder_runtime_status(aisdecoder_runtime_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lock_()) return;
    *out = s_status;
    unlock_();
}

bool aisdecoder_is_ais_sentence(const char *line)
{
    return aisdecoder_parse_is_sentence(line);
}

void aisdecoder_feed_nmea_line(const char *line)
{
    aisdecoder_runtime_feed(line);
}
