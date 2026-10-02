/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include "FreeRTOS.h"
typedef struct { int locked; } profile_test_mutex_t;
typedef profile_test_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout);
int xSemaphoreGive(SemaphoreHandle_t mutex);
