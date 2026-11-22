#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t lock = malloc(sizeof(*lock));
    if (lock && pthread_mutex_init(lock, NULL) != 0) {
        free(lock);
        return NULL;
    }
    return lock;
}
static inline int xSemaphoreTake(SemaphoreHandle_t lock, int delay)
{
    (void)delay;
    return pthread_mutex_lock(lock) == 0;
}
static inline int xSemaphoreGive(SemaphoreHandle_t lock) { return pthread_mutex_unlock(lock) == 0; }
