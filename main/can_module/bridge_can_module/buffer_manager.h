/*
 * Copyright (c) 2026 S. Zhurba
 * SPDX-License-Identifier: MIT
 *
 * @brief   Buffer management for console input/output.
 */

#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_BUFFER_SIZE 1024
#define OUTPUT_BUFFER_SIZE 1024
#define OUTPUT_BUFFER_COUNT 8
#define BUFFER_TIMEOUT_MS 1000

typedef struct {
    char data[INPUT_BUFFER_SIZE];
    uint16_t len;
    bool has_line;
    bool is_free;
    SemaphoreHandle_t mutex;
} input_buffer_t;

typedef struct {
    char data[OUTPUT_BUFFER_COUNT][OUTPUT_BUFFER_SIZE];
    uint16_t len[OUTPUT_BUFFER_COUNT];
    uint8_t head;
    uint8_t count;
    bool is_full;
    SemaphoreHandle_t mutex;
} output_buffer_t;

bool buffer_manager_init(void);
void buffer_manager_deinit(void);
bool buffer_manager_put_input(const char *line, uint16_t len);
bool buffer_manager_get_input(char *line, size_t capacity, uint16_t *len);
bool buffer_manager_put_output(const char *line, uint16_t len);
bool buffer_manager_get_output(char *line, size_t capacity, uint16_t *len);
bool buffer_manager_has_input(void);
bool buffer_manager_has_output(void);
/* True after buffer_manager_init() completes while its mutexes remain valid. */
bool buffer_manager_is_inited(void);

#endif
