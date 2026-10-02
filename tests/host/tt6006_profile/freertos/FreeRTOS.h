/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
void profile_test_critical_enter(portMUX_TYPE *lock);
void profile_test_critical_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(p) profile_test_critical_enter(p)
#define portEXIT_CRITICAL(p) profile_test_critical_exit(p)
