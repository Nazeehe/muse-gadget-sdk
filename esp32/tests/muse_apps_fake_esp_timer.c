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
 * The esp_timer that apps_alarm.c calls, on the host (tests/muse_apps_task_fakes.h declares it; the harness of the alarms
 * and the app task's link it). It follows ESP-IDF v6.0.1's esp_timer where apps_alarm.c leans on it:
 *   - one thread, the "esp_timer task", runs every callback, one at a time, the earliest due first (ties in the order they were
 *     armed): a callback that waits holds up every other timer, which is why the alarms' never do;
 *   - a one-shot is no longer armed when its callback runs, so the callback can start it again; a periodic timer's next time
 *     is its last plus its period, set before the callback, so one that came late runs again at once until it has caught up;
 *   - starting a timer that is armed is ESP_ERR_INVALID_STATE, and so is stopping one that isn't;
 *   - timers are made and never freed here (apps_alarm.c never deletes one).
 * The clock is the harness's own esp_timer_get_time, polled every millisecond of real time: a harness that holds it still and
 * moves it sees the callbacks run in the steps it takes, and fake_esp_timer_sync tells when they have.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "muse_apps_task_fakes.h"

#define TIMERS_MAX 32

struct fake_esp_timer {
    esp_timer_cb_t callback;
    void *arg;
    char name[24];
    bool armed, periodic;
    int64_t due_us, period_us;
    uint64_t order;  /* the arming's number: of two timers due together, the lower runs first */
};

static struct fake_esp_timer timers[TIMERS_MAX];
static int n_timers;
static uint64_t last_order;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t synced_cv = PTHREAD_COND_INITIALIZER;
static pthread_t task_self;      /* the esp_timer task: set by that thread, first thing */
static atomic_bool task_up;      /* a timer was made, so the thread was too */
static atomic_bool task_self_set;
static uint64_t sync_asked, sync_done;
static esp_timer_handle_t call_pending;  /* fake_esp_timer_call's timer, until its callback has run */
static bool behind_next_stop;            /* fake_esp_timer_start_behind_next_stop's */
static int timers_limit = TIMERS_MAX;    /* fake_esp_timer_set_limit's: how many timers esp_timer_create makes */

static void fail(const char *what) {
    fprintf(stderr, "FAIL the fake esp_timer: %s\n", what);
    fflush(stderr);
    _exit(1);
}

static void sleep_1ms(void) {
    struct timespec ts = {0, 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

/* The armed timer that is due now and runs first, or NULL (mu held). */
static struct fake_esp_timer *next_due(int64_t now_us) {
    struct fake_esp_timer *best = NULL;
    for (int i = 0; i < n_timers; i++) {
        struct fake_esp_timer *t = &timers[i];
        if (t->armed && t->due_us <= now_us &&
            (best == NULL || t->due_us < best->due_us || (t->due_us == best->due_us && t->order < best->order))) {
            best = t;
        }
    }
    return best;
}

static void *task_main(void *arg) {
    (void)arg;
    task_self = pthread_self();
    atomic_store(&task_self_set, true);
    pthread_mutex_lock(&mu);
    for (;;) {
        esp_timer_handle_t forced = call_pending;
        struct fake_esp_timer *t = forced != NULL ? forced : next_due(esp_timer_get_time());
        if (t != NULL) {
            esp_timer_cb_t callback = t->callback;
            void *cb_arg = t->arg;
            if (forced != NULL) {
                /* the timer's own state is left as it is: this call was already on its way */
            } else if (t->periodic) {
                t->due_us += t->period_us;
            } else {
                t->armed = false;  /* before the callback, as esp_timer does: it may start the timer again */
            }
            pthread_mutex_unlock(&mu);
            callback(cb_arg);
            pthread_mutex_lock(&mu);
            if (forced != NULL) {
                call_pending = NULL;
                pthread_cond_broadcast(&synced_cv);
            }
            continue;
        }
        sync_done = sync_asked;  /* nothing is due: everything asked for before has run */
        pthread_cond_broadcast(&synced_cv);
        pthread_mutex_unlock(&mu);
        sleep_1ms();
        pthread_mutex_lock(&mu);
    }
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    if (args == NULL || args->callback == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&mu);
    if (n_timers >= timers_limit) {
        pthread_mutex_unlock(&mu);
        return ESP_ERR_NO_MEM;  /* the esp_timer task's own memory is internal RAM, which can run out */
    }
    struct fake_esp_timer *t = &timers[n_timers++];
    memset(t, 0, sizeof *t);
    t->callback = args->callback;
    t->arg = args->arg;
    snprintf(t->name, sizeof t->name, "%s", args->name != NULL ? args->name : "");
    *out = t;
    bool first = !atomic_load(&task_up);
    atomic_store(&task_up, true);
    pthread_mutex_unlock(&mu);
    if (first) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, task_main, NULL) != 0) {
            fail("no thread for the esp_timer task");
        }
        pthread_attr_destroy(&attr);
    }
    return ESP_OK;
}

static esp_err_t start(esp_timer_handle_t t, uint64_t after_us, bool periodic) {
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&mu);
    if (t->armed) {
        pthread_mutex_unlock(&mu);
        return ESP_ERR_INVALID_STATE;
    }
    t->armed = true;
    t->periodic = periodic;
    t->period_us = (int64_t)after_us;
    t->due_us = esp_timer_get_time() + (int64_t)after_us;
    t->order = ++last_order;
    pthread_mutex_unlock(&mu);
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
    pthread_mutex_lock(&mu);
    bool was = t->armed;
    t->armed = false;
    if (behind_next_stop && was) {
        behind_next_stop = false;
        t->armed = true;  /* another task's start, in the caller's gap: a retry's, 10 ms on */
        t->periodic = false;
        t->due_us = esp_timer_get_time() + 10000;
        t->order = ++last_order;
    }
    pthread_mutex_unlock(&mu);
    return was ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool fake_esp_timer_on_task(void) {
    return atomic_load(&task_self_set) && pthread_equal(pthread_self(), task_self);
}

/* Waits on synced_cv until done() or 5 s have gone (mu held). */
static void wait_until(bool (*done)(void), const char *what) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    while (!done()) {
        if (pthread_cond_timedwait(&synced_cv, &mu, &deadline) == ETIMEDOUT && !done()) {
            pthread_mutex_unlock(&mu);
            fail(what);
        }
    }
}

static uint64_t sync_want;

static bool synced(void) {
    return sync_done >= sync_want;
}

void fake_esp_timer_sync(void) {
    if (fake_esp_timer_on_task()) {
        fail("fake_esp_timer_sync from a callback");
    }
    if (!atomic_load(&task_up)) {
        return;  /* no timer was ever made: nothing can be due */
    }
    pthread_mutex_lock(&mu);
    sync_want = ++sync_asked;
    wait_until(synced, "the esp_timer task never went idle: a callback is stuck, or timers keep coming due");
    pthread_mutex_unlock(&mu);
}

static bool call_made(void) {
    return call_pending == NULL;
}

void fake_esp_timer_call(const char *name, uintptr_t arg) {
    pthread_mutex_lock(&mu);
    esp_timer_handle_t found = NULL;
    for (int i = 0; i < n_timers && found == NULL; i++) {
        found = strcmp(timers[i].name, name) == 0 && (uintptr_t)timers[i].arg == arg ? &timers[i] : NULL;
    }
    if (found == NULL) {
        pthread_mutex_unlock(&mu);
        fail("fake_esp_timer_call: no such timer");
    }
    wait_until(call_made, "a call is still pending");  /* one at a time */
    call_pending = found;
    wait_until(call_made, "the esp_timer task never ran the call");
    pthread_mutex_unlock(&mu);
}

int64_t fake_esp_timer_next_due_us(void) {
    int64_t next = INT64_MAX;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n_timers; i++) {
        if (timers[i].armed && timers[i].due_us < next) {
            next = timers[i].due_us;
        }
    }
    pthread_mutex_unlock(&mu);
    return next;
}

void fake_esp_timer_set_limit(int max_timers) {
    pthread_mutex_lock(&mu);
    timers_limit = max_timers < TIMERS_MAX ? max_timers : TIMERS_MAX;
    pthread_mutex_unlock(&mu);
}

void fake_esp_timer_start_behind_next_stop(void) {
    pthread_mutex_lock(&mu);
    behind_next_stop = true;
    pthread_mutex_unlock(&mu);
}
