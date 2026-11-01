#pragma once

#include "FreeRTOS.h"
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *handle);
TickType_t xTaskGetTickCount(void);
