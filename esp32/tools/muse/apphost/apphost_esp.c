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

/*
 * esp_timer, the lock and the log, as apps_alarm.c uses them, on apphost's fake clock (apphost_esp.h says why and how). One thread, so
 * "the esp_timer task" is apphost_esp_run_timers, which apphost.c calls after each step of the clock: a timer that came due in the step
 * runs then, with the clock at the step's end, as a task that was a moment late would run it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apphost_esp.h"
#include "apps_port.h"

#define TIMERS_MAX 32  /* the alarm table has 8 and the ring 1 */

struct apphost_timer {
    esp_timer_cb_t callback;
    void *arg;
    bool armed, periodic;
    int64_t due_us, period_us;
    uint64_t order;  /* the number of its start: of two timers due together, the lower runs first */
};

static struct apphost_timer timers[TIMERS_MAX];
static int n_timers;
static uint64_t last_order;

int64_t esp_timer_get_time(void) {
    return apps_port_mono_ms() * 1000;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    if (args == NULL || args->callback == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n_timers >= TIMERS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    struct apphost_timer *t = &timers[n_timers++];
    memset(t, 0, sizeof *t);
    t->callback = args->callback;
    t->arg = args->arg;
    *out = t;
    return ESP_OK;
}

static esp_err_t start(esp_timer_handle_t t, uint64_t after_us, bool periodic) {
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    t->armed = true;
    t->periodic = periodic;
    t->period_us = (int64_t)after_us;
    t->due_us = esp_timer_get_time() + (int64_t)after_us;
    t->order = ++last_order;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us) {
    return start(t, timeout_us, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us) {
    return period_us == 0 ? ESP_ERR_INVALID_ARG : start(t, period_us, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool was = t->armed;
    t->armed = false;
    return was ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void apphost_esp_run_timers(void) {
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        struct apphost_timer *best = NULL;
        for (int i = 0; i < n_timers; i++) {
            struct apphost_timer *t = &timers[i];
            if (t->armed && t->due_us <= now_us &&
                (best == NULL || t->due_us < best->due_us || (t->due_us == best->due_us && t->order < best->order))) {
                best = t;
            }
        }
        if (best == NULL) {
            return;
        }
        if (best->periodic) {
            best->due_us += best->period_us;
        } else {
            best->armed = false;  /* before the callback, as esp_timer does: it may start the timer again */
        }
        best->callback(best->arg);
    }
}

/* ---- The lock ---- */

struct apphost_mutex {
    bool held;
};
_Static_assert(sizeof(struct apphost_mutex) <= sizeof(StaticSemaphore_t), "the flag fits in its buffer");

/* A bug of the table's, and the run ends: apphost's tests have an empty stderr, so none goes unseen. */
static void violation(const char *what) {
    fprintf(stderr, "apphost: %s\n", what);
    abort();
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct apphost_mutex *m = (struct apphost_mutex *)(void *)buffer;
    m->held = false;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    if (m->held) {
        if (wait == portMAX_DELAY) {
            violation("the alarm table's lock was taken again by its holder");
        }
        return pdFALSE;  /* a try that waits for a lock no one will give back */
    }
    m->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    if (!m->held) {
        violation("the alarm table's lock was given back and wasn't held");
    }
    m->held = false;
    return pdTRUE;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;  /* nothing waits on one thread */
}

/* ---- The log ---- */

static apphost_esp_log_fn log_fn;

void apphost_esp_set_log(apphost_esp_log_fn fn) {
    log_fn = fn;
}

void apphost_esp_log(char level, const char *tag, const char *fmt, ...) {
    (void)tag;
    if (log_fn == NULL) {
        return;
    }
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log_fn(level, text);
}
