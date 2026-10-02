#pragma once
#include "freertos/FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *tick, TickType_t period);
int xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack,
                          void *arg, unsigned priority, void *handle, int core);
