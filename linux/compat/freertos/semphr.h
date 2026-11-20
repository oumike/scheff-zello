#pragma once

#include "freertos/FreeRTOS.h"

typedef struct rtos_sem *SemaphoreHandle_t;

// A mutex is a binary semaphore that starts given.
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
