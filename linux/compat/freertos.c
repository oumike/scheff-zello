// FreeRTOS over pthreads (see freertos/FreeRTOS.h).

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static void deadline_after(struct timespec *ts, TickType_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

// Waits on cv until ready() or the wait runs out; called with m held.
static bool wait_until(pthread_cond_t *cv, pthread_mutex_t *m, TickType_t wait, bool (*ready)(void *), void *arg)
{
    if (ready(arg)) return true;
    if (wait == 0) return false;
    if (wait == portMAX_DELAY) {
        while (!ready(arg)) pthread_cond_wait(cv, m);
        return true;
    }
    struct timespec ts;
    deadline_after(&ts, wait);
    while (!ready(arg)) {
        if (pthread_cond_timedwait(cv, m, &ts) == ETIMEDOUT) return ready(arg);
    }
    return true;
}

// ---- tasks -----------------------------------------------------------------

typedef struct {
    TaskFunction_t fn;
    void *arg;
} task_start_t;

static void *task_trampoline(void *p)
{
    task_start_t s = *(task_start_t *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    (void)name;
    (void)prio;
    task_start_t *s = malloc(sizeof(*s));
    if (!s) return pdFAIL;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // ESP stack sizes are in bytes and sized for the chip; give Linux threads
    // room (libc and curl use more stack than ESP-IDF).
    size_t sz = stack < 256 * 1024 ? 256 * 1024 : stack;
    pthread_attr_setstacksize(&attr, sz);
    pthread_t th;
    int rc = pthread_create(&th, &attr, task_trampoline, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(s);
        return pdFAIL;
    }
    if (out) *out = (TaskHandle_t)(uintptr_t)1;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) pthread_exit(NULL);
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec ts = {.tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

// ---- queues ----------------------------------------------------------------

struct rtos_queue {
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
    UBaseType_t len, size, head, count;
    unsigned char *buf;
};

static bool q_has_item(void *p) { return ((struct rtos_queue *)p)->count > 0; }
static bool q_has_room(void *p)
{
    struct rtos_queue *q = p;
    return q->count < q->len;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct rtos_queue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->buf = calloc(length ? length : 1, item_size ? item_size : 1);
    if (!q->buf) {
        free(q);
        return NULL;
    }
    q->len = length;
    q->size = item_size;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->m);
    bool ok = wait_until(&q->not_full, &q->m, wait, q_has_room, q);
    if (ok) {
        memcpy(q->buf + ((q->head + q->count) % q->len) * q->size, item, q->size);
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->m);
    bool ok = wait_until(&q->not_empty, &q->m, wait, q_has_item, q);
    if (ok) {
        memcpy(item, q->buf + q->head * q->size, q->size);
        q->head = (q->head + 1) % q->len;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    q->head = q->count = 0;
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    UBaseType_t n = q->count;
    pthread_mutex_unlock(&q->m);
    return n;
}

void vQueueDelete(QueueHandle_t q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
    free(q);
}

// ---- semaphores ------------------------------------------------------------

struct rtos_sem {
    pthread_mutex_t m;
    pthread_cond_t cv;
    UBaseType_t count, max;
};

static bool s_available(void *p) { return ((struct rtos_sem *)p)->count > 0; }

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial)
{
    struct rtos_sem *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->max = max;
    s->count = initial;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return xSemaphoreCreateCounting(1, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xSemaphoreCreateCounting(1, 0); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    pthread_mutex_lock(&s->m);
    bool ok = wait_until(&s->cv, &s->m, wait, s_available, s);
    if (ok) s->count--;
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    bool ok = s->count < s->max;
    if (ok) {
        s->count++;
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->cv);
    free(s);
}
