#pragma once
#include <stdint.h>
#include <pthread.h>
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux) ((void)pthread_mutex_lock(mux))
#define portEXIT_CRITICAL(mux) ((void)pthread_mutex_unlock(mux))
#define pdMS_TO_TICKS(ms) (ms)
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1U
