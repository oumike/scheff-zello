#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define xQueueCreateWithCaps(len, size, caps) xQueueCreate(len, size)
#define xTaskCreateWithCaps(fn, name, stack, arg, prio, out, caps) xTaskCreate(fn, name, stack, arg, prio, out)
