#pragma once

#include "freertos/FreeRTOS.h"

typedef struct rtos_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                                 UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}
// Only NULL (the calling task) is supported: it ends the thread.
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
// Threads get at least 256 KB of stack; report it as plenty.
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 256 * 1024;
}
static inline const char *pcTaskGetName(TaskHandle_t t)
{
    (void)t;
    return "task";
}
