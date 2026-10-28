/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apps_io.h"

#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * The I/O worker (apps_io.h has the contract). The lock is held by a submit while it decides and queues, and by the
 * worker while it decides to go: so a job is either queued while the worker is there (and the worker sees it before it
 * goes, as it looks at the queue under the lock), or it finds no worker and makes one. The worker never holds the lock
 * across a job, and a submit never waits for anything but the lock.
 */

static const char *TAG = "muse_apps";

/* What a new worker needs in one free internal block: its stack, its TCB (another block, which this one can hold too)
 * and the margin, so that making it never takes the last block that big. */
#define BLOCK_NEEDED (APPS_IO_STACK + sizeof(StaticTask_t) + APPS_IO_BLOCK_MARGIN)

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} job_t;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;   /* APPS_IO_JOBS of job_t, in PSRAM */
static TaskHandle_t s_worker;  /* under s_lock: the worker, or NULL when there is none */

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(APPS_IO_IDLE_MS)) == pdTRUE) {
            job.fn(job.arg);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_jobs) > 0) {
            xSemaphoreGive(s_lock);  /* one came in as the wait ran out */
            continue;
        }
        s_worker = NULL;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "io: idle, stack %u of %u free", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)APPS_IO_STACK);
        vTaskDelete(NULL);
    }
}

void apps_io_init(void) {
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_jobs = xQueueCreateWithCaps(APPS_IO_JOBS, sizeof(job_t), MALLOC_CAP_SPIRAM);
}

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    if (s_lock == NULL || s_jobs == NULL || fn == NULL) {
        ESP_LOGW(TAG, "io: not started: job refused");
        return false;
    }
    enum { TAKEN, SHORT, FULL, FRAGMENTED, NO_TASK } why = TAKEN;
    job_t job = {fn, arg};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), largest = 0;
    if (free_internal < APPS_IO_STACK + APPS_IO_HEADROOM) {
        why = SHORT;
    } else if (uxQueueMessagesWaiting(s_jobs) >= APPS_IO_JOBS) {
        why = FULL;
    } else if (s_worker == NULL && (largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) < BLOCK_NEEDED) {
        why = FRAGMENTED;  /* a worker that runs needs no new block */
    } else if (s_worker == NULL &&
               xTaskCreatePinnedToCore(worker, "muse_apps_io", APPS_IO_STACK, NULL, APPS_IO_PRIORITY, &s_worker,
                                       tskNO_AFFINITY) != pdPASS) {
        s_worker = NULL;
        why = NO_TASK;
    } else if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        why = FULL;  /* can't be: only submits fill the queue, under the lock, and there was room */
    }
    xSemaphoreGive(s_lock);
    switch (why) {
    case TAKEN:
        return true;
    case SHORT:
        ESP_LOGW(TAG, "io: %u bytes of internal RAM free, %u needed: job refused", (unsigned)free_internal,
                 (unsigned)(APPS_IO_STACK + APPS_IO_HEADROOM));
        return false;
    case FULL:
        ESP_LOGW(TAG, "io: %d jobs wait already: job refused", APPS_IO_JOBS);
        return false;
    case FRAGMENTED:
        ESP_LOGW(TAG, "io: the largest free internal block is %u bytes, %u needed: job refused", (unsigned)largest,
                 (unsigned)BLOCK_NEEDED);
        return false;
    default:
        ESP_LOGW(TAG, "io: no memory for the worker: job refused");
        return false;
    }
}
