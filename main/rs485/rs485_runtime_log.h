/* Private bounded history. Allocate once: readers never race a free during mode changes. */
#pragma once
#include "rs485/rs485_runtime.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#define RS485_RUNTIME_LOG_CAPACITY 32u
typedef struct {
    portMUX_TYPE lock;
    rs485_runtime_event_t *events;
    uint32_t sequence, count;
} rs485_runtime_log_t;
#define RS485_RUNTIME_LOG_INIT { .lock = portMUX_INITIALIZER_UNLOCKED }
static inline bool rs485_runtime_log_init(rs485_runtime_log_t *log)
{
    if (log->events) return true;
    rs485_runtime_event_t *events = heap_caps_calloc(RS485_RUNTIME_LOG_CAPACITY,
        sizeof(*events), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!events) events = heap_caps_calloc(RS485_RUNTIME_LOG_CAPACITY,
        sizeof(*events), MALLOC_CAP_8BIT);
    if (!events) return false;
    portENTER_CRITICAL(&log->lock);
    log->events = events;
    portEXIT_CRITICAL(&log->lock);
    return true;
}
static inline void rs485_runtime_log_append(rs485_runtime_log_t *log, uint8_t kind,
                                            const void *data, size_t length)
{
    if (!data || !length) return;
    if (length > sizeof(((rs485_runtime_event_t *)0)->data)-1u)
        length = sizeof(((rs485_runtime_event_t *)0)->data)-1u;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time()/1000ULL);
    portENTER_CRITICAL(&log->lock);
    if (log->events) {
        const uint32_t sequence = ++log->sequence;
        rs485_runtime_event_t *evt = &log->events[(sequence-1u)%RS485_RUNTIME_LOG_CAPACITY];
        evt->sequence = sequence;
        evt->time_ms = now_ms;
        evt->kind = kind;
        evt->length = (uint16_t)length;
        memcpy(evt->data, data, length);
        evt->data[length] = 0;
        if (log->count < RS485_RUNTIME_LOG_CAPACITY) ++log->count;
    }
    portEXIT_CRITICAL(&log->lock);
}
static inline bool rs485_runtime_log_read(rs485_runtime_log_t *log, uint32_t *cursor,
                                        rs485_runtime_event_t *out, uint32_t *dropped)
{
    if (!cursor || !out) return false;
    if (dropped) *dropped = 0;
    bool found = false;
    portENTER_CRITICAL(&log->lock);
    if (log->events && log->count && *cursor != log->sequence) {
        uint32_t available = log->sequence - *cursor;
        if (available > log->count) {
            if (dropped) *dropped = available - log->count;
            *cursor = log->sequence - log->count;
        }
        const uint32_t wanted = *cursor + 1u;
        *out = log->events[(wanted-1u)%RS485_RUNTIME_LOG_CAPACITY];
        *cursor = wanted;
        found = true;
    }
    portEXIT_CRITICAL(&log->lock);
    return found;
}
