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

#include <assert.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
/* The worker's stack is in PSRAM, and the worker writes flash (the FAT, apps_nvs). That is safe only while a flash write leaves the cache
 * on, which IDF v6.0.1 does when code and rodata run from PSRAM: both options make SPI_FLASH_CACHE_NO_DISABLE true
 * (components/spi_flash/spi_flash_os_func_app.c:32), so spi1_start takes a mutex and nothing else, and the internal-stack assert of the
 * cache-disabling path (esp_task_stack_is_sane_cache_disabled, cache_utils.c:126) is never reached (ruling X18). Without them a flash
 * write from this task would assert. */
#if !CONFIG_SPIRAM_FETCH_INSTRUCTIONS || !CONFIG_SPIRAM_RODATA
#error "apps_io.c: the I/O worker's stack is in PSRAM, so flash writes must leave the cache on: set CONFIG_SPIRAM_FETCH_INSTRUCTIONS and CONFIG_SPIRAM_RODATA (X18)"
#endif
#endif

/*
 * The I/O worker (apps_io.h has the contract). The lock is held by a submit while it decides and queues, and by the
 * worker while it decides to go: so a job is either queued while the worker is there (and the worker sees it before it
 * goes, as it looks at the queue under the lock), or it finds no worker and makes one. The worker never holds the lock
 * across a job, and a submit never waits for anything but the lock.
 *
 * The ops are a ring of APPS_IO_OPS under the same lock. One submitter (the app task, which registered itself: s_op_task) pushes; the worker's thread alone pops and runs
 * them, outside the lock: after each job, before it decides to go (it doesn't go with one waiting), and in apps_io_assist. A worker that
 * waits for a job would not see a new op, so a push queues a job to wake it (drain_job), one at a time: s_ops_nudged says one is on its
 * way, and the job clears it before it looks, so an op that comes after the look queues the next. When that job can't be queued and
 * no worker is there, nothing could ever run the op, and it is taken back; with a worker there it stays, for the end of its job.
 *
 * A worker that goes idle parks: s_parked has it, suspended, until the next submit that makes a worker deletes it (reap_locked: the
 * stack and the TCB are freed by vTaskDeleteWithCaps, from that task, as IDF advises). Only a worker that has suspended itself is
 * deleted, never one still on its way (it may hold the log's lock while it says its line); one that is still on its way stays parked for
 * the next look, and a worker that would park while another hasn't been deleted stays the worker a while longer, so at most one waits.
 */

static const char *TAG = "muse_apps";

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} job_t;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;   /* APPS_IO_JOBS of job_t, in PSRAM */
static TaskHandle_t s_worker;  /* under s_lock: the worker, or NULL when there is none */
static TaskHandle_t s_parked;  /* under s_lock: a worker that went idle, suspended or about to be, for a submit to delete */
static job_t s_ops[APPS_IO_OPS];  /* under s_lock: the ops waiting, a ring from s_ops_head */
static unsigned s_ops_head, s_ops_n;
static bool s_ops_nudged;         /* under s_lock: a drain job is queued, and has not looked yet */
static TaskHandle_t s_op_task;    /* the task that submits ops: set by it, before it submits any */
static void (*s_first)(void);     /* under s_lock: the step a new worker runs once, before anything else (apps_io_set_first) */

/* Runs the ops that are waiting, in order. The worker's thread alone calls it, and no lock is held while an op runs. */
static void drain_ops(void) {
    for (;;) {
        job_t op = {NULL, NULL};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool have = s_ops_n > 0;
        if (have) {
            op = s_ops[s_ops_head];
            s_ops_head = (s_ops_head + 1) % APPS_IO_OPS;
            s_ops_n--;
        }
        xSemaphoreGive(s_lock);
        if (!have) {
            return;
        }
        op.fn(op.arg);
    }
}

/* The parked worker deleted, its stack and TCB freed, once it has suspended itself; s_lock held, by a task that isn't it. */
static void reap_locked(void) {
    if (s_parked != NULL && eTaskGetState(s_parked) == eSuspended) {
        vTaskDeleteWithCaps(s_parked);
        s_parked = NULL;
    }
}

/* What an op's submit queues to bring a waiting worker to it: the flag goes before the look, so that an op pushed after the look
 * queues the next one. */
static void drain_job(void *arg) {
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ops_nudged = false;
    xSemaphoreGive(s_lock);
    drain_ops();
}

static void worker(void *arg) {
    (void)arg;
    bool ran_job = false;  /* a job of its own: the drain jobs that bring it to the ops are not */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    void (*first)(void) = s_first;
    s_first = NULL;  /* once: a worker made again later finds it done */
    xSemaphoreGive(s_lock);
    if (first != NULL) {
        first();  /* before any job or op: they were queued for a worker that does this first */
        ran_job = true;
    }
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(APPS_IO_IDLE_MS)) == pdTRUE) {
            ran_job = ran_job || job.fn != drain_job;
            job.fn(job.arg);
            drain_ops();  /* the ops that came while it ran */
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (uxQueueMessagesWaiting(s_jobs) > 0 || s_ops_n > 0) {
            xSemaphoreGive(s_lock);  /* one came in as the wait ran out */
            drain_ops();
            continue;
        }
        reap_locked();  /* the worker before this one, which parked long ago */
        if (s_parked != NULL) {
            xSemaphoreGive(s_lock);  /* it hasn't suspended itself yet: this one stays the worker for another wait */
            continue;
        }
        s_worker = NULL;
        s_parked = xTaskGetCurrentTaskHandle();
        xSemaphoreGive(s_lock);
        /* A worker that only ran ops is made for most saves of an app that keeps a store, which come 2 s or more apart, and says so each time:
         * that is a debug line. One that ran a job (an install, a launch) says it as it did. */
        if (ran_job) {
            ESP_LOGI(TAG, "io: idle, stack %u of %u free", (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)APPS_IO_STACK);
        } else {
            ESP_LOGD(TAG, "io: idle, stack %u of %u free", (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)APPS_IO_STACK);
        }
        vTaskSuspend(NULL);  /* parked, for good: the next submit that makes a worker deletes this one (reap_locked) */
    }
}

void apps_io_register_op_task(void) {
    s_op_task = xTaskGetCurrentTaskHandle();
}

void apps_io_set_first(void (*fn)(void)) {
    if (s_lock == NULL) {
        return;  /* apps_io_init first */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_first = fn;
    xSemaphoreGive(s_lock);
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
    enum { TAKEN, FULL, NO_TASK } why = TAKEN;
    job_t job = {fn, arg};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (uxQueueMessagesWaiting(s_jobs) >= APPS_IO_JOBS) {
        why = FULL;
    } else if (s_worker == NULL) {
        reap_locked();  /* the worker that parked: its stack and TCB go before a new one's are taken */
        if (xTaskCreatePinnedToCoreWithCaps(worker, "muse_apps_io", APPS_IO_STACK, NULL, APPS_IO_PRIORITY, &s_worker, APPS_IO_CORE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_worker = NULL;
            why = NO_TASK;
        }
    }
    if (why == TAKEN && xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        why = FULL;  /* can't be: only submits fill the queue, under the lock, and there was room */
    }
    xSemaphoreGive(s_lock);
    switch (why) {
    case TAKEN:
        return true;
    case FULL:
        ESP_LOGW(TAG, "io: %d jobs wait already: job refused", APPS_IO_JOBS);
        return false;
    default:
        ESP_LOGW(TAG, "io: no memory for the worker: job refused");
        return false;
    }
}

bool apps_io_submit_op(void (*op)(void *arg), void *arg) {
    assert(s_op_task != NULL && xTaskGetCurrentTaskHandle() == s_op_task);  /* the ring has one pusher: the app task (apps_io.h) */
    if (s_lock == NULL || s_jobs == NULL || op == NULL) {
        ESP_LOGW(TAG, "io: not started: op refused");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ops_n >= APPS_IO_OPS) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "io: %d ops wait already: op refused", APPS_IO_OPS);
        return false;
    }
    s_ops[(s_ops_head + s_ops_n) % APPS_IO_OPS] = (job_t){op, arg};
    s_ops_n++;
    bool nudge = !s_ops_nudged;
    s_ops_nudged = true;
    xSemaphoreGive(s_lock);
    if (!nudge || apps_io_submit(drain_job, NULL)) {
        return true;
    }
    /* No job could be queued to bring the worker (apps_io_submit said why). With a worker there the op waits for the end of its job,
     * or its idle look; with none, nothing would ever run it, and it is taken back: it is the last in the ring, and nothing pops
     * but the worker's thread. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ops_nudged = false;
    bool taken_back = s_worker == NULL && s_ops_n > 0;
    if (taken_back) {
        s_ops_n--;
    }
    xSemaphoreGive(s_lock);
    return !taken_back;
}

void apps_io_assist(void) {
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool mine = s_worker != NULL && xTaskGetCurrentTaskHandle() == s_worker;
    bool waiting = s_ops_n > 0;
    xSemaphoreGive(s_lock);
    if (mine && waiting) {
        drain_ops();
    }
}
