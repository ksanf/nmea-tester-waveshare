#pragma once
#include <freertos/FreeRTOS.h>
typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
TimerHandle_t xTimerCreate(const char *, TickType_t, int, void *, TimerCallbackFunction_t);
int xTimerReset(TimerHandle_t, TickType_t);
int xTimerStop(TimerHandle_t, TickType_t);
