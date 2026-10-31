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

#include "apps_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "apps_jpeg.h"
#include "apps_port.h"
#include "muse_apps.h"

/*
 * The network worker (apps_net.h has the contract). The app task alone submits, so the first submit makes the lock, the queue and the
 * worker with no race; the worker is made once and stays. The results' list is under the lock, which the worker holds only to append
 * and the app task only to take the whole list: neither waits for the other's progress. The list is plain state under a lock (PSRAM);
 * the lock is internal, as every lock is (S3 PSRAM has no CAS).
 */

static const char *TAG = "muse_apps";

#define NOT_SAID "the VM refused the request"  /* a failure the VM gave no words for */

typedef struct {
    apps_vm_job_t *head, *tail;
} job_list_t;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;   /* APPS_NET_JOBS of apps_vm_job_t *, in PSRAM */
static TaskHandle_t s_worker;
static void (*s_on_done)(void);
static bool (*s_alive)(uint32_t app_gen);
static EXT_RAM_BSS_ATTR job_list_t s_done;  /* under s_lock: the results the app task hasn't taken */

/* A read coming in: the bytes so far, in a block of apps_port_realloc that grows (to the job's max at most). */
typedef struct {
    apps_vm_job_t *job;
    uint8_t *buf;
    size_t len, cap;
    bool no_memory, over;
} sink_t;

static bool sink(void *user, const uint8_t *d, size_t n) {
    sink_t *s = user;
    if (n == 0) {
        return true;
    }
    if (n > s->job->max - s->len) {
        s->over = true;
        return false;
    }
    if (s->len + n > s->cap) {
        size_t cap = s->cap * 2;
        cap = cap < s->len + n ? s->len + n : cap;
        cap = cap > s->job->max ? s->job->max : cap;
        uint8_t *grown = apps_port_realloc(s->buf, cap);
        if (grown == NULL) {
            s->no_memory = true;  /* the floor's refusal: a clean error */
            return false;
        }
        s->buf = grown;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    return true;
}

/* A failed request, in the callback's words (apps_vmio.h). */
static void say_failure(apps_vm_job_t *job, const sink_t *s, const char *code, const char *why) {
    const char *rel = job->path + job->rel;
    code = code != NULL ? code : MUSE_APPS_VM_FAILED;
    if (s->no_memory || strcmp(code, MUSE_APPS_VM_OUT_OF_MEMORY) == 0) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NO_MEMORY);
    } else if (s->over || strcmp(code, MUSE_APPS_VM_TOO_LARGE) == 0) {
        snprintf(job->err, sizeof job->err, APPS_VM_MSG_TOO_LARGE, rel, (unsigned)(job->max / 1024));
    } else if (strcmp(code, MUSE_APPS_VM_NOT_FOUND) == 0) {
        snprintf(job->err, sizeof job->err, APPS_VM_MSG_NO_FILE, rel);
    } else if (strcmp(code, MUSE_APPS_VM_DROPPED) == 0) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NOT_CONNECTED);
    } else if (strcmp(code, MUSE_APPS_VM_TIMED_OUT) == 0) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_TIMED_OUT);
    } else if (strcmp(code, MUSE_APPS_VM_BUSY) == 0) {
        snprintf(job->err, sizeof job->err, "%s", job->op == APPS_VM_READ ? APPS_VM_MSG_READER_BUSY : APPS_VM_MSG_WRITER_BUSY);
    } else {
        snprintf(job->err, sizeof job->err, "%s", why[0] != '\0' ? why : NOT_SAID);  /* the VM's own words */
    }
}

/* Runs one job: its request, with the deadline from now and the busy retries within it, then a JPEG's decode. */
static void run(apps_vm_job_t *job) {
    const muse_apps_vm_ops_t *ops = muse_apps_vm_ops();
    const bool read = job->op == APPS_VM_READ;
    const int64_t deadline_ms = apps_port_mono_ms() + APPS_VM_REQUEST_MS;
    job->ok = false;
    job->err[0] = '\0';
    if (ops == NULL || (read ? ops->read == NULL : ops->write == NULL)) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NOT_CONNECTED);
        return;
    }
    sink_t s = {.job = job};
    const char *code = NULL;
    char why[APPS_VM_ERR_MAX];
    bool ok = false;
    for (int attempt = 0;; attempt++) {
        code = NULL;
        why[0] = '\0';
        if (read) {
            apps_port_free(s.buf);
            s = (sink_t){.job = job};
            ok = ops->read(job->path, job->max, sink, &s, deadline_ms * 1000, &code, why, sizeof why);
        } else {
            ok = ops->write(job->path, job->data, job->len, deadline_ms * 1000, &code, why, sizeof why);
        }
        bool busy = !ok && code != NULL && strcmp(code, MUSE_APPS_VM_BUSY) == 0;
        if (!busy || attempt >= APPS_VM_BUSY_RETRIES) {
            break;
        }
        if (apps_port_mono_ms() + APPS_VM_BUSY_WAIT_MS >= deadline_ms) {
            code = MUSE_APPS_VM_TIMED_OUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APPS_VM_BUSY_WAIT_MS));  /* the reader is another's (an install, a picture): try again */
    }
    if (!ok) {
        apps_port_free(s.buf);
        say_failure(job, &s, code, why);
        return;
    }
    if (!read) {
        job->ok = true;
        return;
    }
    if (s.cap > s.len && s.len > 0) {
        s.buf = apps_port_realloc(s.buf, s.len);  /* a shrink never fails */
    }
    if (job->as == APPS_VM_AS_JPEG) {
        uint8_t *block = NULL;
        size_t block_len = 0;
        bool decoded = apps_jpeg_decode(s.buf, s.len, job->max_px, job->circle, &block, &block_len, NULL, NULL, job->err,
                                        sizeof job->err);
        apps_port_free(s.buf);
        if (!decoded) {
            return;  /* the decoder's own words */
        }
        job->data = block;
        job->len = block_len;
    } else {
        job->data = s.buf;
        job->len = s.len;
    }
    job->ok = true;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        apps_vm_job_t *job = NULL;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }
        if (s_alive != NULL && !s_alive(job->app_gen)) {
            apps_vm_job_free(job);  /* its app has ended: no request for it */
            continue;
        }
        run(job);
        job->next = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_done.tail != NULL) {
            s_done.tail->next = job;
        } else {
            s_done.head = job;
        }
        s_done.tail = job;
        xSemaphoreGive(s_lock);
        if (s_on_done != NULL) {
            s_on_done();
        }
    }
}

void apps_net_init(void (*done)(void), bool (*alive)(uint32_t app_gen)) {
    s_on_done = done;
    s_alive = alive;
}

bool apps_net_submit(apps_vm_job_t *job) {
    if (job == NULL) {
        return false;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    if (s_jobs == NULL) {
        s_jobs = xQueueCreateWithCaps(APPS_NET_JOBS, sizeof(apps_vm_job_t *), MALLOC_CAP_SPIRAM);
    }
    if (s_jobs != NULL && s_worker == NULL &&
        xTaskCreatePinnedToCoreWithCaps(worker, "muse_apps_net", APPS_NET_STACK, NULL, APPS_NET_PRIORITY, &s_worker, APPS_NET_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_worker = NULL;
    }
    if (s_jobs == NULL || s_worker == NULL) {
        ESP_LOGW(TAG, "vm: no memory for the network worker: request refused");
        return false;
    }
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "vm: %d requests wait already: request refused", APPS_NET_JOBS);
        return false;
    }
    return true;
}

apps_vm_job_t *apps_net_take(void) {
    if (s_lock == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apps_vm_job_t *head = s_done.head;
    s_done.head = s_done.tail = NULL;
    xSemaphoreGive(s_lock);
    return head;
}
