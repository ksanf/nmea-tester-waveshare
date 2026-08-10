/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Buffer manager implementation for Sailor Inmarsat-C (TT-3027C).
 */

#include <esp_log.h>
#define CFG_LOG_MODULE LOG_CFG_BUFFER_MANAGER
#include "config_logs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>
#include "buffer_manager.h"
#include "config/memory_config.h"

static const char *TAG = "BUFFER";

/* --- Internal readiness flag --- */
static volatile bool s_inited = false;

static input_buffer_t  *input_buffer;
static output_buffer_t *output_buffer;

static bool buffers_ready_(void)
{
    return input_buffer && output_buffer;
}

static void buffers_free_(void)
{
    free(input_buffer);
    input_buffer = NULL;
    free(output_buffer);
    output_buffer = NULL;
}

static bool buffers_alloc_(void)
{
    input_buffer = CALLOC_WHERE(BRIDGE_CAN_BUFS_IN_PSRAM, 1, sizeof(*input_buffer));
    output_buffer = CALLOC_WHERE(BRIDGE_CAN_BUFS_IN_PSRAM, 1, sizeof(*output_buffer));
    if (!buffers_ready_()) {
        buffers_free_();
        return false;
    }
    return true;
}

bool buffer_manager_is_inited(void) { return s_inited; }

bool buffer_manager_init(void) {
    if (s_inited) return true;
    if (!buffers_alloc_()) {
        ESP_LOGE(TAG, "init: buffer alloc failed");
        return false;
    }

    /* input */
    input_buffer->len      = 0;
    input_buffer->has_line = false;
    input_buffer->is_free  = true;
    input_buffer->mutex    = xSemaphoreCreateMutex();
    if (!input_buffer->mutex) ESP_LOGE(TAG, "init: failed to create input mutex");

    /* output */
    output_buffer->head    = 0;
    output_buffer->count   = 0;
    output_buffer->is_full = false;
    for (int i = 0; i < OUTPUT_BUFFER_COUNT; i++) output_buffer->len[i] = 0;
    output_buffer->mutex   = xSemaphoreCreateMutex();
    if (!output_buffer->mutex) ESP_LOGE(TAG, "init: failed to create output mutex");

    if (!input_buffer->mutex || !output_buffer->mutex) {
        if (input_buffer->mutex) vSemaphoreDelete(input_buffer->mutex);
        if (output_buffer->mutex) vSemaphoreDelete(output_buffer->mutex);
        buffers_free_();
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "initialized");
    return true;
}

void buffer_manager_deinit(void) {
    /* Ordering matters: clear the flag before deleting the mutexes. */
    s_inited = false;

    if (input_buffer && input_buffer->mutex) {
        vSemaphoreDelete(input_buffer->mutex);
        input_buffer->mutex = NULL;
    }
    if (output_buffer && output_buffer->mutex) {
        vSemaphoreDelete(output_buffer->mutex);
        output_buffer->mutex = NULL;
    }

    buffers_free_();

    ESP_LOGI(TAG, "deinitialized");
}

/* --- Safe wrappers --- */

bool buffer_manager_has_input(void) {
    if (!s_inited || !input_buffer || !input_buffer->mutex) return false;
    if (xSemaphoreTake(input_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        bool has_input = input_buffer->has_line;
        xSemaphoreGive(input_buffer->mutex);
        return has_input;
    }
    ESP_LOGW(TAG, "has_input: take mutex timeout");
    return false;
}

bool buffer_manager_has_output(void) {
    if (!s_inited || !output_buffer || !output_buffer->mutex) return false;
    if (xSemaphoreTake(output_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        bool has_output = output_buffer->count > 0;
        xSemaphoreGive(output_buffer->mutex);
        return has_output;
    }
    ESP_LOGW(TAG, "has_output: take mutex timeout");
    return false;
}

bool buffer_manager_put_input(const char *line, uint16_t len) {
    if (!s_inited || !input_buffer || !input_buffer->mutex) return false;
    if (!line || len==0 || len > INPUT_BUFFER_SIZE) {
        ESP_LOGW(TAG, "put_input: bad args len=%u", (unsigned)len);
        return false;
    }
    if (xSemaphoreTake(input_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        if (!input_buffer->is_free) {
            ESP_LOGW(TAG, "put_input: busy (drop)");
            xSemaphoreGive(input_buffer->mutex);
            return false;
        }
        memcpy(input_buffer->data, line, len);
        input_buffer->len      = len;
        input_buffer->has_line = true;
        input_buffer->is_free  = false;
        xSemaphoreGive(input_buffer->mutex);
        return true;
    }
    ESP_LOGW(TAG, "put_input: take mutex timeout");
    return false;
}

bool buffer_manager_get_input(char *line, size_t capacity, uint16_t *len) {
    if (!s_inited || !input_buffer || !input_buffer->mutex) return false;
    if (!line || !len || capacity == 0) return false;
    if (xSemaphoreTake(input_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        if (!input_buffer->has_line) { xSemaphoreGive(input_buffer->mutex); return false; }
        if (input_buffer->len > capacity) {
            ESP_LOGE(TAG, "get_input: destination too small (%u > %u)",
                     (unsigned)input_buffer->len, (unsigned)capacity);
            input_buffer->has_line = false;
            input_buffer->is_free = true;
            xSemaphoreGive(input_buffer->mutex);
            return false;
        }
        memcpy(line, input_buffer->data, input_buffer->len);
        *len = input_buffer->len;
        input_buffer->has_line = false;
        input_buffer->is_free  = true;
        xSemaphoreGive(input_buffer->mutex);
        return true;
    }
    ESP_LOGW(TAG, "get_input: take mutex timeout");
    return false;
}

bool buffer_manager_put_output(const char *line, uint16_t len) {
    if (!s_inited || !output_buffer || !output_buffer->mutex) return false;
    if (!line || len==0 || len > OUTPUT_BUFFER_SIZE) {
        ESP_LOGW(TAG, "put_output: bad args len=%u", (unsigned)len);
        return false;
    }
    if (xSemaphoreTake(output_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        if (output_buffer->is_full) {
            /* drop-oldest */
            uint8_t index = output_buffer->head;
            memcpy(output_buffer->data[index], line, len);
            output_buffer->len[index] = len;
            output_buffer->head = (output_buffer->head + 1) % OUTPUT_BUFFER_COUNT;
            xSemaphoreGive(output_buffer->mutex);
            ESP_LOGW(TAG, "put_output: ring full → drop-oldest len=%u", (unsigned)len);
            return true;
        }
        uint8_t index = (output_buffer->head + output_buffer->count) % OUTPUT_BUFFER_COUNT;
        memcpy(output_buffer->data[index], line, len);
        output_buffer->len[index] = len;
        output_buffer->count++;
        if (output_buffer->count == OUTPUT_BUFFER_COUNT) output_buffer->is_full = true;
        xSemaphoreGive(output_buffer->mutex);
        return true;
    }
    ESP_LOGW(TAG, "put_output: take mutex timeout");
    return false;
}

bool buffer_manager_get_output(char *line, size_t capacity, uint16_t *len) {
    if (!s_inited || !output_buffer || !output_buffer->mutex) return false;
    if (!line || !len || capacity == 0) return false;

    if (xSemaphoreTake(output_buffer->mutex, pdMS_TO_TICKS(BUFFER_TIMEOUT_MS)) == pdTRUE) {
        if (output_buffer->count == 0) { xSemaphoreGive(output_buffer->mutex); return false; }
        uint16_t item_len = output_buffer->len[output_buffer->head];
        if (item_len > capacity) {
            ESP_LOGE(TAG, "get_output: destination too small (%u > %u)",
                     (unsigned)item_len, (unsigned)capacity);
            output_buffer->head = (output_buffer->head + 1) % OUTPUT_BUFFER_COUNT;
            output_buffer->count--;
            output_buffer->is_full = false;
            xSemaphoreGive(output_buffer->mutex);
            return false;
        }
        memcpy(line, output_buffer->data[output_buffer->head], item_len);
        *len = item_len;
        output_buffer->head = (output_buffer->head + 1) % OUTPUT_BUFFER_COUNT;
        if (output_buffer->count > 0) output_buffer->count--;
        output_buffer->is_full = false;
        xSemaphoreGive(output_buffer->mutex);
        return true;
    }
    ESP_LOGW(TAG, "get_output: take mutex timeout");
    return false;
}
