#pragma once

// The FreeRTOS API the shared code uses, over pthreads: tasks are detached
// threads, a tick is a millisecond, queues and semaphores are a mutex and a
// condition variable each.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY        ((TickType_t)0xffffffffu)
#define configTICK_RATE_HZ   1000
#define portTICK_PERIOD_MS   1
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))
#define pdTICKS_TO_MS(t)     ((uint32_t)(t))
