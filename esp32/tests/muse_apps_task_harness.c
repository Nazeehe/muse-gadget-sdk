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
 * The app task of Muse apps (apps_task.c), run for real on the host. Usage: muse_apps_task_harness <case>, one case per
 * test in tests/test_muse_apps_task.py; it exits 0 when the case holds, and otherwise prints why and every event.
 *
 * Three threads, as on the gadget: the app task (the fake FreeRTOS of tests/muse_apps_task_fakes.h, made of pthreads
 * here), an LVGL task that runs a frame every few ms (muse_apps_ui_supervise, then muse_apps_ui_tick unless the screen
 * "sleeps"; a paused one runs nothing, as LVGL paused on battery), and the case's own thread, the caller. Under the app
 * task is the real runtime (apps_rt, apps_ui, apps_vm, Lua); apps_lv is a fake that writes what is done to the screen
 * into one log of events, in the order it happens, and that keeps the image objects a pass makes and draws each from
 * the package's bytes at every frame, before the supervisor (LVGL's refresh may come first in a round of its timers),
 * until a reset takes them down: bytes freed while an object still points into them are an AddressSanitizer report.
 * The fake apps_lv also checks that what must run on the LVGL task does, and that a pass never meets a new table while
 * an old app's objects are still up.
 *
 * The port is the harness's: a counting allocator that logs the free of the packages and tables it watches, a UI lock
 * that checks its contract (no double take, no heap use while held), a console whose print of a given line blocks
 * until the case lets it go (a callback stuck in C), and a clock the case can stop and move.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_task.h"
#include "apps_ui.h"
#include "muse_apps.h"
#include "muse_apps_task_fakes.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define HUNG_MESSAGE "the app stopped responding; no app can run until its stuck call returns or the gadget restarts"

/* ---- Time ---- */

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static int64_t real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* An absolute CLOCK_REALTIME time ms from now, for pthread_cond_timedwait. */
static struct timespec deadline_in(int64_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* The monotonic clock the port gives (apps_port_mono_ms): real time, unless a case stopped it, and moved by what a case
 * adds. */
static pthread_mutex_t clock_mu = PTHREAD_MUTEX_INITIALIZER;
static bool clock_stopped;
static int64_t clock_at, clock_offset;

int64_t apps_port_mono_ms(void) {
    pthread_mutex_lock(&clock_mu);
    int64_t now = clock_stopped ? clock_at : real_ms() + clock_offset;
    pthread_mutex_unlock(&clock_mu);
    return now;
}

static void clock_stop_at(int64_t ms) {
    pthread_mutex_lock(&clock_mu);
    clock_stopped = true;
    clock_at = ms;
    pthread_mutex_unlock(&clock_mu);
}

/* It runs again, from where it stopped. */
static void clock_go(void) {
    pthread_mutex_lock(&clock_mu);
    if (clock_stopped) {
        clock_offset = clock_at - real_ms();
        clock_stopped = false;
    }
    pthread_mutex_unlock(&clock_mu);
}

static void clock_jump(int64_t ms) {
    pthread_mutex_lock(&clock_mu);
    if (clock_stopped) {
        clock_at += ms;
    } else {
        clock_offset += ms;
    }
    pthread_mutex_unlock(&clock_mu);
}

int64_t esp_timer_get_time(void) {
    return apps_port_mono_ms() * 1000;
}

/* ---- The events: one log of what every thread did, in order ---- */

#define EVENTS_MAX 16384
static pthread_mutex_t events_mu = PTHREAD_MUTEX_INITIALIZER;
static char *events[EVENTS_MAX];
static int n_events;

static void event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void event(const char *fmt, ...) {
    char text[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&events_mu);
    if (n_events < EVENTS_MAX) {
        events[n_events++] = strdup(text);
    }
    pthread_mutex_unlock(&events_mu);
}

/* Where the log is now: what a case looks for comes after it. */
static int mark(void) {
    pthread_mutex_lock(&events_mu);
    int n = n_events;
    pthread_mutex_unlock(&events_mu);
    return n;
}

/* The index of the first event at or after from that starts with prefix, or -1. */
static int at_from(int from, const char *prefix) {
    size_t len = strlen(prefix);
    int found = -1;
    pthread_mutex_lock(&events_mu);
    for (int i = from; i < n_events && found < 0; i++) {
        if (strncmp(events[i], prefix, len) == 0) {
            found = i;
        }
    }
    pthread_mutex_unlock(&events_mu);
    return found;
}

static int count_from(int from, const char *prefix) {
    size_t len = strlen(prefix);
    int n = 0;
    pthread_mutex_lock(&events_mu);
    for (int i = from; i < n_events; i++) {
        n += strncmp(events[i], prefix, len) == 0 ? 1 : 0;
    }
    pthread_mutex_unlock(&events_mu);
    return n;
}

static void dump_events(void) {
    pthread_mutex_lock(&events_mu);
    fprintf(stderr, "---- %d events ----\n", n_events);
    for (int i = 0; i < n_events; i++) {
        fprintf(stderr, "%5d %s\n", i, events[i]);
    }
    pthread_mutex_unlock(&events_mu);
}

/* ---- Failing ---- */

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void fail_at(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL line %d: ", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    dump_events();
    fflush(stderr);
    fflush(stdout);
    _exit(1);
}

#define CHECK(cond, ...)                   \
    do {                                   \
        if (!(cond)) {                     \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                  \
    } while (0)

/* What another thread got wrong: counted, and the case fails at its end (or at the next check of it). */
static pthread_mutex_t errors_mu = PTHREAD_MUTEX_INITIALIZER;
static int errors;
static char error_note[300];

static void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void error(const char *fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&errors_mu);
    if (errors++ == 0) {
        snprintf(error_note, sizeof error_note, "%s", text);
    }
    pthread_mutex_unlock(&errors_mu);
    event("ERROR %s", text);
}

/* Waits up to ms of real time for an event that starts with prefix, at or after from, and returns its index. */
static int wait_event_at(int line, int from, const char *prefix, int ms) {
    int64_t until = real_ms() + ms;
    for (;;) {
        int i = at_from(from, prefix);
        if (i >= 0) {
            return i;
        }
        if (real_ms() > until) {
            fail_at(line, "no event '%s' after %d within %d ms", prefix, from, ms);
        }
        sleep_ms(2);
    }
}
#define WAIT(from, prefix) wait_event_at(__LINE__, (from), (prefix), 5000)

static void ordered_at(int line, int from, const char *first, const char *then) {
    int a = at_from(from, first), b = at_from(from, then);
    if (a < 0 || b < 0 || a > b) {
        fail_at(line, "'%s' (%d) should come before '%s' (%d), both after %d", first, a, then, b, from);
    }
}
#define ORDERED(from, first, then) ordered_at(__LINE__, (from), (first), (then))

static void absent_at(int line, int from, const char *prefix) {
    int i = at_from(from, prefix);
    if (i >= 0) {
        fail_at(line, "'%s' at %d: there should be no '%s' after %d", events[i], i, prefix, from);
    }
}
#define ABSENT(from, prefix) absent_at(__LINE__, (from), (prefix))

/* ---- The port: the heap ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static pthread_mutex_t heap_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t port_held;  /* apps_port_realloc's bytes not given back */
static size_t caps_held;  /* heap_caps_*'s */

/* Blocks whose free is an event: the packages, their images and the tables. */
static struct {
    const void *ptr;
    char label[80];
} watched[256];

static void watch(const void *ptr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void watch(const void *ptr, const char *fmt, ...) {
    pthread_mutex_lock(&heap_mu);
    for (size_t i = 0; i < ARRAY_LEN(watched); i++) {
        if (watched[i].ptr == NULL) {
            watched[i].ptr = ptr;
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(watched[i].label, sizeof watched[i].label, fmt, ap);
            va_end(ap);
            break;
        }
    }
    pthread_mutex_unlock(&heap_mu);
}

/* The label of a watched block, or "?". */
static const char *watched_label(const void *ptr, char *out, size_t cap) {
    snprintf(out, cap, "?");
    pthread_mutex_lock(&heap_mu);
    for (size_t i = 0; i < ARRAY_LEN(watched); i++) {
        if (watched[i].ptr == ptr) {
            snprintf(out, cap, "%s", watched[i].label);
        }
    }
    pthread_mutex_unlock(&heap_mu);
    return out;
}

static bool ui_held_by_me(void);

void *apps_port_realloc(void *ptr, size_t n) {
    if (n == 0) {
        error("apps_port_realloc(%p, 0)", ptr);
    }
    if (ui_held_by_me()) {
        error("apps_port_realloc with the UI lock held");
    }
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    pthread_mutex_lock(&heap_mu);
    port_held = port_held - old + n;
    pthread_mutex_unlock(&heap_mu);
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (ui_held_by_me()) {
        error("apps_port_free with the UI lock held");
    }
    block_t *b = (block_t *)ptr - 1;
    char label[80] = "";
    pthread_mutex_lock(&heap_mu);
    port_held -= b->size;
    for (size_t i = 0; i < ARRAY_LEN(watched); i++) {
        if (watched[i].ptr == ptr) {
            snprintf(label, sizeof label, "%s", watched[i].label);
            watched[i].ptr = NULL;
        }
    }
    pthread_mutex_unlock(&heap_mu);
    if (label[0] != '\0') {
        event("free %s", label);
    }
    free(b);
}

static size_t held_now(size_t *counter) {
    pthread_mutex_lock(&heap_mu);
    size_t n = *counter;
    pthread_mutex_unlock(&heap_mu);
    return n;
}

/* heap_caps_*: the app task's own blocks (its record and its requests), which must be PSRAM. */
void *heap_caps_malloc(size_t size, uint32_t caps) {
    if ((caps & MALLOC_CAP_SPIRAM) == 0) {
        error("heap_caps_malloc of %zu bytes not in PSRAM (caps 0x%x)", size, (unsigned)caps);
    }
    block_t *b = malloc(sizeof *b + (size ? size : 1));
    if (b == NULL) {
        return NULL;
    }
    b->size = size;
    pthread_mutex_lock(&heap_mu);
    caps_held += size;
    pthread_mutex_unlock(&heap_mu);
    return b + 1;
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    void *p = heap_caps_malloc(n * size, caps);
    if (p != NULL) {
        memset(p, 0, n * size);
    }
    return p;
}

void heap_caps_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    pthread_mutex_lock(&heap_mu);
    caps_held -= b->size;
    pthread_mutex_unlock(&heap_mu);
    free(b);
}

size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 4u * 1024 * 1024;
}

/* ---- The port: the UI lock, the console, the rest ---- */

static pthread_mutex_t ui_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ui_cv = PTHREAD_COND_INITIALIZER;
static bool ui_held;
static pthread_t ui_holder;

static bool ui_held_by_me(void) {
    pthread_mutex_lock(&ui_mu);
    bool mine = ui_held && pthread_equal(ui_holder, pthread_self());
    pthread_mutex_unlock(&ui_mu);
    return mine;
}

void apps_port_ui_lock(void) {
    if (ui_held_by_me()) {
        error("apps_port_ui_lock taken again by its holder");
        return;
    }
    pthread_mutex_lock(&ui_mu);
    while (ui_held) {
        pthread_cond_wait(&ui_cv, &ui_mu);
    }
    ui_held = true;
    ui_holder = pthread_self();
    pthread_mutex_unlock(&ui_mu);
}

bool apps_port_ui_trylock(int ms) {
    if (ui_held_by_me()) {
        error("apps_port_ui_trylock by the lock's holder");
        return false;
    }
    struct timespec deadline = deadline_in(ms);
    pthread_mutex_lock(&ui_mu);
    while (ui_held) {
        if (pthread_cond_timedwait(&ui_cv, &ui_mu, &deadline) == ETIMEDOUT && ui_held) {
            pthread_mutex_unlock(&ui_mu);
            return false;
        }
    }
    ui_held = true;
    ui_holder = pthread_self();
    pthread_mutex_unlock(&ui_mu);
    return true;
}

void apps_port_ui_unlock(void) {
    pthread_mutex_lock(&ui_mu);
    bool mine = ui_held && pthread_equal(ui_holder, pthread_self());
    if (mine) {
        ui_held = false;
        pthread_cond_broadcast(&ui_cv);
    }
    pthread_mutex_unlock(&ui_mu);
    if (!mine) {
        error("apps_port_ui_unlock by a thread that doesn't hold the lock");
    }
}

void apps_port_init(void) {
    event("port init");
}

bool apps_port_epoch_ms(int64_t *out) {
    (void)out;
    return false;
}

void apps_port_sha256(const void *data, size_t len, uint8_t out[32]) {
    (void)data;
    (void)len;
    memset(out, 0, 32);
}

/* The console. A print of block_line doesn't return until the case lets it go: a call stuck in C. */
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t print_cv = PTHREAD_COND_INITIALIZER;
static const char *block_line;
static bool print_blocked;

void apps_port_log(const char *app_id, const char *line) {
    event("print %s %s", app_id, line);
    pthread_mutex_lock(&print_mu);
    while (block_line != NULL && strcmp(block_line, line) == 0) {
        print_blocked = true;
        pthread_cond_broadcast(&print_cv);
        pthread_cond_wait(&print_cv, &print_mu);
    }
    print_blocked = false;
    pthread_mutex_unlock(&print_mu);
}

static void block_print(const char *line) {
    pthread_mutex_lock(&print_mu);
    block_line = line;
    pthread_mutex_unlock(&print_mu);
}

static void release_print(void) {
    pthread_mutex_lock(&print_mu);
    block_line = NULL;
    pthread_cond_broadcast(&print_cv);
    pthread_mutex_unlock(&print_mu);
}

static void wait_print_blocked_at(int line) {
    struct timespec deadline = deadline_in(5000);
    pthread_mutex_lock(&print_mu);
    while (!print_blocked) {
        if (pthread_cond_timedwait(&print_cv, &print_mu, &deadline) == ETIMEDOUT && !print_blocked) {
            pthread_mutex_unlock(&print_mu);
            fail_at(line, "the app task never got stuck in its print");
        }
    }
    pthread_mutex_unlock(&print_mu);
}
#define WAIT_PRINT_BLOCKED() wait_print_blocked_at(__LINE__)

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (strcmp(tag, "muse_apps") != 0) {
        error("a log line with the tag '%s'", tag);
    }
    event("log %c %s", level, msg);
}

/* ---- The fake FreeRTOS: tasks, queues and mutexes on pthreads ---- */

struct fake_task {
    pthread_t thread;
    TaskFunction_t code;
    void *arg;
    char name[24];
    UBaseType_t priority;
};

static struct fake_task app_tasks[2];
static int n_app_tasks;
static _Thread_local struct fake_task thread_task;  /* what a thread of the harness is to FreeRTOS */
static _Thread_local struct fake_task *current_task;
static pthread_mutex_t tasks_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(TickType_t) longest_receive;  /* the longest wait the app task asked of xQueueReceive */

static struct fake_task *self_task(void) {
    return current_task != NULL ? current_task : &thread_task;
}

static void *task_main(void *arg) {
    struct fake_task *task = arg;
    current_task = task;
    task->code(task->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps) {
    event("task %s stack %u priority %u core %d caps 0x%x", name, (unsigned)stack, priority, core, caps);
    if (n_app_tasks >= (int)ARRAY_LEN(app_tasks)) {
        return pdFAIL;
    }
    struct fake_task *task = &app_tasks[n_app_tasks++];
    snprintf(task->name, sizeof task->name, "%s", name);
    task->code = code;
    task->arg = arg;
    task->priority = priority;
    if (created != NULL) {
        *created = task;  /* FreeRTOS hands out the handle before the task can run */
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);  /* Lua, and the sanitizers' redzones */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&task->thread, &attr, task_main, task);
    pthread_attr_destroy(&attr);
    return rc == 0 ? pdPASS : pdFAIL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return self_task();
}

void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority) {
    struct fake_task *t = task != NULL ? task : self_task();
    pthread_mutex_lock(&tasks_mu);
    t->priority = priority;
    pthread_mutex_unlock(&tasks_mu);
    event("priority %s %u", t->name[0] != '\0' ? t->name : "?", priority);
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    (void)task;
    return 40000;
}

void vTaskDelay(const TickType_t ticks) {
    sleep_ms((int)ticks);
}

struct fake_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char *items;
    size_t item_size, length, head, count;
};

QueueHandle_t xQueueCreateWithCaps(UBaseType_t length, UBaseType_t item_size, UBaseType_t caps) {
    event("queue %u x %u caps 0x%x", length, item_size, caps);
    struct fake_queue *q = calloc(1, sizeof *q);
    if (q == NULL) {
        return NULL;
    }
    q->items = calloc(length, item_size);
    q->item_size = item_size;
    q->length = length;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

void vQueueDeleteWithCaps(QueueHandle_t q) {
    if (q != NULL) {
        free(q->items);
        free(q);
    }
}

/* Waits on q's condition until ready() or the wait runs out; q->mu is held. */
static bool queue_wait(struct fake_queue *q, TickType_t wait, bool (*ready)(const struct fake_queue *)) {
    struct timespec deadline = deadline_in((int64_t)wait);
    while (!ready(q)) {
        if (wait == 0) {
            return false;
        }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&q->cv, &q->mu);
        } else if (pthread_cond_timedwait(&q->cv, &q->mu, &deadline) == ETIMEDOUT && !ready(q)) {
            return false;
        }
    }
    return true;
}

static bool has_room(const struct fake_queue *q) {
    return q->count < q->length;
}

static bool has_item(const struct fake_queue *q) {
    return q->count > 0;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    pthread_mutex_lock(&q->mu);
    if (!queue_wait(q, wait, has_room)) {
        pthread_mutex_unlock(&q->mu);
        return pdFALSE;
    }
    memcpy(q->items + ((q->head + q->count) % q->length) * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
    if (current_task != NULL && wait > atomic_load(&longest_receive)) {
        atomic_store(&longest_receive, wait);
    }
    pthread_mutex_lock(&q->mu);
    if (!queue_wait(q, wait, has_item)) {
        pthread_mutex_unlock(&q->mu);
        return pdFALSE;
    }
    memcpy(item, q->items + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

struct fake_mutex {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool held;
    pthread_t holder;
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)(void *)buffer->opaque;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->held = false;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    struct timespec deadline = deadline_in((int64_t)wait);
    pthread_mutex_lock(&m->mu);
    if (m->held && pthread_equal(m->holder, pthread_self())) {
        pthread_mutex_unlock(&m->mu);
        error("a mutex taken again by its holder");
        return pdFALSE;
    }
    while (m->held) {
        if (wait == 0) {
            pthread_mutex_unlock(&m->mu);
            return pdFALSE;
        }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&m->cv, &m->mu);
        } else if (pthread_cond_timedwait(&m->cv, &m->mu, &deadline) == ETIMEDOUT && m->held) {
            pthread_mutex_unlock(&m->mu);
            return pdFALSE;
        }
    }
    m->held = true;
    m->holder = pthread_self();
    pthread_mutex_unlock(&m->mu);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    pthread_mutex_lock(&m->mu);
    bool mine = m->held && pthread_equal(m->holder, pthread_self());
    if (mine) {
        m->held = false;
        pthread_cond_broadcast(&m->cv);
    }
    pthread_mutex_unlock(&m->mu);
    if (!mine) {
        error("a mutex given by a thread that doesn't hold it");
        return pdFALSE;
    }
    return pdTRUE;
}

/* ---- The fake apps_lv ---- */

static pthread_t lvgl_thread;
static atomic_bool lvgl_up;
static bool building;  /* muse_ui's own task builds the overlays under the display lock, before the frame timer */

static bool on_lvgl_task(void) {
    return building || (atomic_load(&lvgl_up) && pthread_equal(pthread_self(), lvgl_thread));
}

static void lvgl_only(const char *fn) {
    if (!on_lvgl_task()) {
        error("%s called off the LVGL task", fn);
    }
}

static pthread_mutex_t lv_mu = PTHREAD_MUTEX_INITIALIZER;
static apps_ui_t *lv_ui;           /* the published table (apps_lv_set_ui, under the UI lock) */
static apps_ui_t *lv_tables[64];   /* a number for each table ever published: table#1, table#2 ... */
static int lv_n_tables;
static apps_lv_event_cb_t lv_event_cb;
static void (*lv_close_cb)(void);
static atomic_bool lv_front;
static bool lv_launcher, lv_crash;
static void (*lv_pick)(const char *id);
static atomic_uint lv_applies;
static int lv_callback_skips;
static int (*overflow_rule)(const apps_widget_t *w);  /* the overflow a pass writes back for a visible widget */

/* The image objects a pass made: each points into the package's bytes until a reset. */
static struct {
    int slot;
    const uint8_t *data;
    size_t len;
} lv_objects[APPS_UI_SLOTS];
static int lv_n_objects;
static apps_ui_t *lv_objects_table;  /* the table they were made for */
static volatile unsigned lv_drawn;   /* what the draws read, so they can't be left out */

static int table_number(apps_ui_t *ui) {
    for (int i = 0; i < lv_n_tables; i++) {
        if (lv_tables[i] == ui) {
            return i + 1;
        }
    }
    if (lv_n_tables == (int)ARRAY_LEN(lv_tables)) {
        return 0;
    }
    lv_tables[lv_n_tables++] = ui;
    return -lv_n_tables;  /* new: the caller watches it */
}

void apps_lv_build(lv_obj_t *screen) {
    lvgl_only("apps_lv_build");
    event("build %s", screen != NULL ? "screen" : "NULL");
}

void apps_lv_set_event_cb(apps_lv_event_cb_t cb) {
    lv_event_cb = cb;
}

void apps_lv_set_close_cb(void (*cb)(void)) {
    lv_close_cb = cb;
}

void apps_lv_set_ui(apps_ui_t *ui) {
    if (!ui_held_by_me()) {
        error("apps_lv_set_ui without the UI lock");
    }
    pthread_mutex_lock(&lv_mu);
    lv_ui = ui;
    int number = ui != NULL ? table_number(ui) : 0;
    pthread_mutex_unlock(&lv_mu);
    if (number < 0) {
        number = -number;
        watch(ui, "table#%d", number);
    }
    if (ui != NULL) {
        event("set_ui table#%d", number);
    } else {
        event("set_ui NULL");
    }
}

static int object_of(int slot) {
    for (int i = 0; i < lv_n_objects; i++) {
        if (lv_objects[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

static void drop_object(int slot) {
    int i = object_of(slot);
    if (i >= 0) {
        lv_objects[i] = lv_objects[--lv_n_objects];
    }
}

bool apps_lv_apply(bool in_callback) {
    lvgl_only("apps_lv_apply");
    if (in_callback && lv_callback_skips < 3) {
        lv_callback_skips++;
        return false;
    }
    lv_callback_skips = 0;
    if (!apps_port_ui_trylock(2)) {
        return false;
    }
    pthread_mutex_lock(&lv_mu);
    apps_ui_t *ui = lv_ui;
    if (ui == NULL) {
        pthread_mutex_unlock(&lv_mu);
        apps_port_ui_unlock();
        return false;
    }
    if (lv_n_objects > 0 && lv_objects_table != ui) {
        error("a pass met a new table while the last app's objects were up: its reset hadn't run");
    }
    lv_objects_table = ui;
    if (apps_ui_take_clear(ui)) {
        lv_n_objects = 0;
    }
    (void)apps_ui_take_bg(ui, NULL);
    const uint8_t *made[APPS_UI_SLOTS];
    int n_made = 0;
    for (int slot = 0; slot < APPS_UI_SLOTS; slot++) {
        apps_widget_t *w = apps_ui_slot(ui, slot);
        if (!w->alive) {
            if ((w->dirty & APPS_DIRTY_DELETE) != 0) {
                drop_object(slot);
            }
            w->dirty = 0;
            continue;
        }
        if (w->kind == APPS_W_IMAGE && object_of(slot) < 0) {
            const apps_asset_t *a = apps_ui_asset(ui, w->image);
            if (a != NULL) {
                lv_objects[lv_n_objects].slot = slot;
                lv_objects[lv_n_objects].data = a->data;
                lv_objects[lv_n_objects].len = a->len;
                lv_n_objects++;
                made[n_made++] = a->data;
            }
        }
        w->dirty = 0;
        w->rx = 200;
        w->ry = 220;
        w->rw = 66;
        w->rh = 26;
        w->overflow = (int16_t)(overflow_rule != NULL && !w->hidden ? overflow_rule(w) : 0);
    }
    pthread_mutex_unlock(&lv_mu);
    apps_port_ui_unlock();
    for (int i = 0; i < n_made; i++) {
        char label[80];
        event("object %s", watched_label(made[i], label, sizeof label));
    }
    atomic_fetch_add(&lv_applies, 1);
    return true;
}

/* LVGL's refresh: every image object is drawn from the package's bytes. */
static void lv_draw(void) {
    pthread_mutex_lock(&lv_mu);
    unsigned sum = 0;
    for (int i = 0; i < lv_n_objects; i++) {
        for (size_t k = 0; k < lv_objects[i].len; k++) {
            sum += lv_objects[i].data[k];
        }
    }
    lv_drawn = sum;
    pthread_mutex_unlock(&lv_mu);
}

void apps_lv_reset(void) {
    lvgl_only("apps_lv_reset");
    pthread_mutex_lock(&lv_mu);
    lv_n_objects = 0;
    lv_objects_table = NULL;
    lv_callback_skips = 0;
    pthread_mutex_unlock(&lv_mu);
    event("reset");
}

void apps_lv_set_front(bool front) {
    lvgl_only("apps_lv_set_front");
    atomic_store(&lv_front, front);
    event("front %d", front ? 1 : 0);
}

bool apps_lv_front(void) {
    return atomic_load(&lv_front);
}

void apps_lv_stats(apps_lv_stats_t *out) {
    memset(out, 0, sizeof *out);
    out->applies = atomic_load(&lv_applies);
}

void apps_lv_show_launcher(const char *const *ids, const char *const *names, int n, void (*pick)(const char *id)) {
    lvgl_only("apps_lv_show_launcher");
    char text[400];
    int used = snprintf(text, sizeof text, "launcher %d", n);
    for (int i = 0; i < n && used < (int)sizeof text; i++) {
        used += snprintf(text + used, sizeof text - (size_t)used, " %s:%s", ids[i], names[i] != NULL ? names[i] : "(null)");
    }
    lv_launcher = true;
    lv_pick = pick;
    event("%s", text);
}

void apps_lv_show_crash(const char *name, const char *first_line) {
    lvgl_only("apps_lv_show_crash");
    lv_crash = true;
    event("crash %s | %s", name != NULL ? name : "(null)", first_line != NULL ? first_line : "(stopped responding)");
}

void apps_lv_hide_cards(void) {
    lvgl_only("apps_lv_hide_cards");
    if (lv_launcher || lv_crash) {
        event("hide cards");
    }
    lv_launcher = lv_crash = false;
}

bool apps_lv_cards_shown(void) {
    lvgl_only("apps_lv_cards_shown");
    return lv_launcher || lv_crash;
}

/* ---- The LVGL task ---- */

static pthread_mutex_t frame_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cv = PTHREAD_COND_INITIALIZER;
static void (*job)(void *);
static void *job_arg;
static bool job_done;
static atomic_bool lvgl_paused, lvgl_asleep, lvgl_quit;
static atomic_uint frames;

static void *lvgl_main(void *arg) {
    (void)arg;
    lvgl_thread = pthread_self();
    atomic_store(&lvgl_up, true);
    while (!atomic_load(&lvgl_quit)) {
        pthread_mutex_lock(&frame_mu);
        if (job != NULL) {
            void (*fn)(void *) = job;
            job = NULL;
            pthread_mutex_unlock(&frame_mu);
            fn(job_arg);  /* an LVGL event: a touch, a swipe, a voice turn */
            pthread_mutex_lock(&frame_mu);
            job_done = true;
            pthread_cond_broadcast(&frame_cv);
        }
        pthread_mutex_unlock(&frame_mu);
        if (!atomic_load(&lvgl_paused)) {
            lv_draw();
            muse_apps_ui_supervise();
            if (!atomic_load(&lvgl_asleep)) {
                (void)muse_apps_ui_tick();
            }
            atomic_fetch_add(&frames, 1);
        }
        sleep_ms(3);
    }
    return NULL;
}

/* Runs fn(arg) on the LVGL task, between frames, and waits for it. */
static void on_lvgl_at(int line, void (*fn)(void *), void *arg) {
    struct timespec deadline = deadline_in(5000);
    pthread_mutex_lock(&frame_mu);
    job = fn;
    job_arg = arg;
    job_done = false;
    while (!job_done) {
        if (pthread_cond_timedwait(&frame_cv, &frame_mu, &deadline) == ETIMEDOUT && !job_done) {
            pthread_mutex_unlock(&frame_mu);
            fail_at(line, "the LVGL task didn't run the job");
        }
    }
    pthread_mutex_unlock(&frame_mu);
}
#define ON_LVGL(fn, arg) on_lvgl_at(__LINE__, (fn), (arg))

static void wait_frames_at(int line, unsigned n) {
    unsigned start = atomic_load(&frames);
    int64_t until = real_ms() + 5000;
    while (atomic_load(&frames) - start < n) {
        if (real_ms() > until) {
            fail_at(line, "no %u frames in 5 s", n);
        }
        sleep_ms(2);
    }
}
#define WAIT_FRAMES(n) wait_frames_at(__LINE__, (n))

typedef struct {
    int slot;
    uint8_t event;
} touch_t;

/* A touch on the widget at slot, as apps_lv's callback sends it: its slot, the gen its object was made for. */
static void touch_job(void *arg) {
    const touch_t *t = arg;
    uint16_t gen = 0;
    if (apps_port_ui_trylock(100)) {
        pthread_mutex_lock(&lv_mu);
        if (lv_ui != NULL) {
            gen = apps_ui_slot(lv_ui, t->slot)->gen;
        }
        pthread_mutex_unlock(&lv_mu);
        apps_port_ui_unlock();
    }
    if (lv_event_cb != NULL) {
        lv_event_cb((uint16_t)t->slot, gen, t->event);
    }
}

static void tap_at(int line, int slot) {
    touch_t t = {slot, APPS_UI_TAP};
    on_lvgl_at(line, touch_job, &t);
}
#define TAP(slot) tap_at(__LINE__, (slot))

static void turn_job(void *arg) {
    muse_apps_ui_turn(*(const bool *)arg);
}

static void turn_at(int line, bool in_turn) {
    on_lvgl_at(line, turn_job, &in_turn);
}
#define TURN(in_turn) turn_at(__LINE__, (in_turn))

/* A right swipe on the layer: apps_lv's gesture handler calls the close callback, on the LVGL task. */
static void swipe_job(void *arg) {
    (void)arg;
    if (lv_close_cb != NULL) {
        lv_close_cb();
    }
}

static void pick_job(void *arg) {
    if (lv_pick != NULL) {
        lv_pick((const char *)arg);
    }
}

/* ---- Packages ---- */

typedef struct {
    const char *path;
    const void *data;
    size_t len;
} file_t;

#define LUA(path, src) {(path), (src), sizeof(src) - 1}
/* A valid LVGL v9 .bin, RGB565 2x1 (apps_image_check passes it). */
static const uint8_t IMG_A[] = {0x19, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
#define IMAGE(path) {(path), IMG_A, sizeof IMG_A}

/* A package as apps_task_run takes it: every block from apps_port_realloc. Its images and itself are watched. */
static apps_pkg_t *make_pkg(const char *id, const char *name, const file_t *files, int n) {
    apps_pkg_t *p = apps_port_realloc(NULL, sizeof *p);
    CHECK(p != NULL, "no memory for a package");
    memset(p, 0, sizeof *p);
    snprintf(p->manifest.id, sizeof p->manifest.id, "%s", id);
    snprintf(p->manifest.name, sizeof p->manifest.name, "%s", name);
    snprintf(p->manifest.version, sizeof p->manifest.version, "1.0.0");
    snprintf(p->manifest.main, sizeof p->manifest.main, "main.lua");
    p->manifest.memory_kb = 256;
    for (int i = 0; i < n; i++) {
        apps_blob_t *b = &p->files[i];
        snprintf(b->path, sizeof b->path, "%s", files[i].path);
        snprintf(p->manifest.files[i], sizeof p->manifest.files[i], "%s", files[i].path);
        b->data = apps_port_realloc(NULL, files[i].len > 0 ? files[i].len : 1);
        CHECK(b->data != NULL, "no memory for a file");
        memcpy(b->data, files[i].data, files[i].len);
        b->len = files[i].len;
        if (apps_file_kind(b->path) == APPS_FILE_IMAGE) {
            watch(b->data, "image %s/%s", id, b->path);
        }
    }
    p->count = n;
    p->manifest.file_count = (uint8_t)n;
    watch(p, "pkg %s", id);
    return p;
}

/* A package that stays the caller's (apps_task_compile). */
static void free_pkg(apps_pkg_t *p) {
    apps_pkg_free(p, apps_port_free);
    apps_port_free(p);
}

static apps_status_t *new_status(void) {
    apps_status_t *st = heap_caps_calloc(1, sizeof *st, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    CHECK(st != NULL, "no memory for a status");
    return st;
}

static const char APP_HELLO[] =
    "local app = {}\n"
    "function app.start() print('start') ui.label{text = 'Hello'} end\n"
    "function app.show() print('show') end\n"
    "function app.hide() print('hide') end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t HELLO[] = {LUA("main.lua", APP_HELLO)};

static const char APP_IMAGE[] =
    "local app = {}\n"
    "function app.start() ui.image{src = 'a.bin'} end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t PICTURE[] = {LUA("main.lua", APP_IMAGE), IMAGE("a.bin")};

/* A button whose tap prints 'busy' (a print a case can make block) and that shows and hides. */
static const char APP_BUSY[] =
    "local app = {}\n"
    "function app.start() ui.button{text = 'Busy'}:on('tap', function() print('busy') end) end\n"
    "function app.show() print('show') end\n"
    "function app.hide() print('hide') end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t BUSY[] = {LUA("main.lua", APP_BUSY)};

static lv_obj_t *const SCREEN = (lv_obj_t *)(uintptr_t)0x5c5e;  /* the fake apps_lv never reads it */

/* ---- The cases ---- */

/* What muse_ui does at boot: the layer under the display lock, the frame timer, then the app task. */
static size_t caps_after_start;

static void boot(void) {
    building = true;
    muse_apps_ui_build(SCREEN);
    building = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, lvgl_main, NULL) == 0, "no LVGL thread");
    while (!atomic_load(&lvgl_up)) {
        sleep_ms(1);
    }
    WAIT_FRAMES(3);  /* frames before the app task exists: the hooks do nothing yet */
    CHECK(muse_apps_start() == ESP_OK, "muse_apps_start");
    CHECK(muse_apps_start() == ESP_OK && count_from(0, "task ") == 1, "a second start makes nothing");
    WAIT(0, "log I up: stack ");
    caps_after_start = held_now(&caps_held);
}

/* The end of every case: whatever runs is stopped, the screen wakes, and nothing is left held. */
static void finish(void) {
    atomic_store(&lvgl_paused, false);
    atomic_store(&lvgl_asleep, false);
    release_print();
    clock_go();
    apps_task_stop();
    int64_t until = real_ms() + 5000;
    while ((held_now(&port_held) != 0 || held_now(&caps_held) != caps_after_start) && real_ms() < until) {
        sleep_ms(5);
    }
    CHECK(held_now(&port_held) == 0, "%zu bytes of the apps' heap still held", held_now(&port_held));
    CHECK(held_now(&caps_held) == caps_after_start, "%zu bytes of the app task's own still held (%zu after start)",
          held_now(&caps_held), caps_after_start);
    CHECK(errors == 0, "%d errors, the first: %s", errors, error_note);
    CHECK(atomic_load(&longest_receive) <= pdMS_TO_TICKS(1000), "the app task waited %u ticks for an event: at most 1000 (C22)",
          (unsigned)atomic_load(&longest_receive));
    atomic_store(&lvgl_quit, true);
}

static void task_shape(void) {
    char want[96];
    CHECK(at_from(0, "task muse_apps stack 57344 priority 2 core 1 caps 0x404") >= 0,
          "the app task: muse_apps, 56 KB, priority 2, core 1, PSRAM");
    snprintf(want, sizeof want, "queue 16 x %zu caps 0x400", sizeof(apps_event_t));
    CHECK(at_from(0, want) >= 0, "16 events in PSRAM: %s", want);
    CHECK(at_from(0, "log I up: stack 57344 in PSRAM") >= 0, "the boot line");
    ORDERED(0, "port init", "task muse_apps");
    ORDERED(0, "build screen", "task muse_apps");
    /* Idle, its loop still comes round at least once a second (C22: finish() checks the waits). */
    sleep_ms(1200);
    CHECK(atomic_load(&longest_receive) > 0, "the app task waits for events");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_STOPPED && st->id[0] == '\0' && !st->front, "no app yet");
    heap_caps_free(st);
}

static void run_and_status(void) {
    apps_status_t *st = new_status();
    char err[200];
    int from = mark();
    bool ok = apps_task_run(make_pkg("hello", "Hello", HELLO, 1), true, 150, st, err, sizeof err);
    CHECK(ok && err[0] == '\0', "the run: '%s'", err);
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "hello") == 0 && strcmp(st->name, "Hello") == 0 &&
              strcmp(st->version, "1.0.0") == 0,
          "who: %s %s %s %s", apps_state_name(st->state), st->id, st->name, st->version);
    CHECK(st->limit_kb == 256 && st->mem_kb > 0 && st->peak_kb >= st->mem_kb && st->instructions > 0, "memory %u/%u/%u, %llu instructions",
          st->mem_kb, st->peak_kb, st->limit_kb, (unsigned long long)st->instructions);
    CHECK(st->widgets == 1 && st->front && st->stack_free == 40000, "widgets %d front %d stack %u", st->widgets, st->front, st->stack_free);
    CHECK(strcmp(st->log_tail, "start\nshow") == 0, "the log: '%s'", st->log_tail);
    CHECK(strstr(st->ui_dump, "\"id\":\"label#0\"") != NULL && st->ui_truncated == 0, "the dump: %s", st->ui_dump);
    CHECK(st->warnings[0] == '\0' && st->last_error[0] == '\0', "no warnings: '%s', no error: '%s'", st->warnings, st->last_error);
    CHECK(at_from(from, "log I launch hello (256 KB)") >= 0, "the launch line");
    /* start(), the table published, the layer in front, show(). */
    ORDERED(from, "print hello start", "set_ui table#1");
    ORDERED(from, "set_ui table#1", "front 1");
    ORDERED(from, "front 1", "print hello show");
    /* The status reads the record and posts nothing. */
    apps_status_t *again = new_status();
    apps_task_status(again);
    CHECK(again->state == APPS_STATE_RUNNING && strcmp(again->id, "hello") == 0 && again->front, "status: %s %s %d",
          apps_state_name(again->state), again->id, again->front);
    /* Stop: stop() runs, the app goes, the layer goes behind. */
    from = mark();
    apps_task_stop();
    ORDERED(from, "print hello stop", "set_ui NULL");
    WAIT(from, "front 0");
    CHECK(at_from(from, "log I stop hello: stopped, peak ") >= 0, "the stop line");
    apps_task_status(again);
    CHECK(again->state == APPS_STATE_STOPPED && strcmp(again->id, "hello") == 0 && strcmp(again->log_tail, "start\nshow\nstop") == 0,
          "the record keeps the last app: %s %s '%s'", apps_state_name(again->state), again->id, again->log_tail);
    heap_caps_free(st);
    heap_caps_free(again);
}

/* The rulings' teardown order, with an image on the screen: unpublish, reset (and the runtime freed after the
 * unpublish), and only after the reset the image's bytes. The fake draws the image every frame until the reset. */
static void stop_tears_down_in_order(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("pic", "Picture", PICTURE, 2), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "object image pic/a.bin");
    WAIT_FRAMES(3);
    int from = mark();
    apps_task_stop();
    WAIT(from, "free pkg pic");
    ORDERED(from, "print pic stop", "set_ui NULL");
    ORDERED(from, "set_ui NULL", "free table#1");
    ORDERED(from, "set_ui NULL", "reset");
    ORDERED(from, "reset", "free image pic/a.bin");
    CHECK(at_from(from, "log I stop pic: stopped, peak ") >= 0, "the stop line");
    WAIT(from, "front 0");
    WAIT_FRAMES(5);
}

/* The LVGL task doesn't run (LVGL paused on battery): the stop waits for the reset with a bound, parks the image bytes
 * instead of freeing them, and a new app's table waits for that reset. A later reset frees them. */
static void a_late_reset_parks_the_images(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("one", "One", PICTURE, 2), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "object image one/a.bin");
    atomic_store(&lvgl_paused, true);
    sleep_ms(20);  /* a frame that had begun is over */
    int from = mark();
    int64_t start = real_ms();
    apps_task_stop();
    int64_t took = real_ms() - start;
    CHECK(took >= APPS_TASK_RESET_MS - 20 && took < APPS_TASK_WAIT_MS, "the stop waited for the reset, at most %d ms: %lld ms",
          APPS_TASK_RESET_MS, (long long)took);
    ORDERED(from, "print one stop", "set_ui NULL");
    CHECK(at_from(from, "free table#1") >= 0, "the runtime is freed, its table unpublished");
    ABSENT(from, "reset");
    ABSENT(from, "free image one/a.bin");
    ABSENT(from, "free pkg one");
    /* A new app meanwhile runs, but its table isn't published before the old app's reset. */
    apps_status_t *st = new_status();
    CHECK(apps_task_run(make_pkg("two", "Two", PICTURE, 2), true, 50, st, err, sizeof err), "run: %s", err);
    CHECK(strcmp(st->warnings, "layout not checked: the screen was asleep") == 0, "warnings: '%s'", st->warnings);
    ABSENT(from, "set_ui table#2");
    ABSENT(from, "free image one/a.bin");
    heap_caps_free(st);
    /* The screen wakes: its first frame draws the old objects (the bytes are still there), then resets; only then is the
     * new table published and the old bytes freed. */
    int woke = mark();
    atomic_store(&lvgl_paused, false);
    int reset = WAIT(woke, "reset");
    int published = WAIT(woke, "set_ui table#2");
    int freed = WAIT(woke, "free image one/a.bin");
    CHECK(reset < published && reset < freed, "reset %d, publish %d, free %d", reset, published, freed);
    WAIT(woke, "object image two/a.bin");
    /* The back of the stop and the front of the run came while the LVGL task slept: the last one counts, and the layer,
     * in front all along, stays there. */
    CHECK(apps_lv_front(), "the new app is in front");
}

static void a_new_run_replaces_the_app(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("one", "One", PICTURE, 2), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "object image one/a.bin");
    int from = mark();
    CHECK(apps_task_run(make_pkg("two", "Two", PICTURE, 2), true, 100, NULL, err, sizeof err), "run: %s", err);
    ORDERED(from, "print one stop", "set_ui NULL");
    ORDERED(from, "set_ui NULL", "reset");
    ORDERED(from, "reset", "set_ui table#2");
    ORDERED(from, "reset", "free image one/a.bin");
    ORDERED(from, "log I stop one: replaced, peak ", "log I launch two (256 KB)");
    WAIT(from, "object image two/a.bin");
}

static const char APP_START_CRASH[] =
    "local app = {}\n"
    "function app.start()\n"
    "  print('starting')\n"
    "  error('boom')\n"
    "end\n"
    "return app\n";

static void a_crash_in_start_is_contained(void) {
    const file_t files[] = {LUA("main.lua", APP_START_CRASH)};
    apps_status_t *st = new_status();
    char err[300];
    int from = mark();
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", files, 1), true, 100, st, err, sizeof err), "a crash isn't a run");
    CHECK(strcmp(err, "crashed in start(): main.lua:4: boom") == 0, "err: '%s'", err);
    CHECK(st->state == APPS_STATE_CRASHED && strncmp(st->last_error, "main.lua:4: boom\nstack traceback:", 33) == 0,
          "the record: %s '%s'", apps_state_name(st->state), st->last_error);
    CHECK(strcmp(st->log_tail, "starting") == 0, "the log: '%s'", st->log_tail);
    int reset = WAIT(from, "reset");
    int card = WAIT(from, "crash Crashy | main.lua:4: boom");
    CHECK(reset < card, "the card after the reset: %d %d", reset, card);
    ORDERED(from, "reset", "free pkg crashy");
    ABSENT(from, "set_ui table");
    ABSENT(from, "front 1");
    CHECK(at_from(from, "log I stop crashy: crashed, peak ") >= 0, "the stop line");
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strcmp(st->id, "crashy") == 0, "the status: %s %s", apps_state_name(st->state), st->id);
    heap_caps_free(st);
}

static const char APP_TAP_CRASH[] =
    "local app = {}\n"
    "function app.start()\n"
    "  ui.image{src = 'a.bin'}\n"
    "  ui.button{text = 'Go'}:on('tap', function() print('tapped') error('bad tap') end)\n"
    "end\n"
    "return app\n";

static void a_crash_in_a_callback_is_contained(void) {
    const file_t files[] = {LUA("main.lua", APP_TAP_CRASH), IMAGE("a.bin")};
    char err[200];
    CHECK(apps_task_run(make_pkg("clicky", "Clicky", files, 2), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "object image clicky/a.bin");
    WAIT(0, "front 1");
    int from = mark();
    TAP(1);
    int card = WAIT(from, "crash Clicky | main.lua:4: bad tap");
    ORDERED(from, "print clicky tapped", "set_ui NULL");
    ORDERED(from, "set_ui NULL", "reset");
    ORDERED(from, "reset", "free image clicky/a.bin");
    CHECK(at_from(from, "reset") < card, "the card after the reset");
    WAIT(from, "front 0");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strncmp(st->last_error, "main.lua:4: bad tap", 19) == 0 &&
              strcmp(st->log_tail, "tapped") == 0,
          "the record: %s '%s' '%s'", apps_state_name(st->state), st->last_error, st->log_tail);
    heap_caps_free(st);
}

static void a_crash_at_the_top_level_is_contained(void) {
    static const char top[] = "print('loading')\nerror('top')\n";
    const file_t files[] = {LUA("main.lua", top)};
    apps_status_t *st = new_status();
    char err[300];
    int from = mark();
    CHECK(!apps_task_run(make_pkg("top", "Top", files, 1), true, 100, st, err, sizeof err), "a crash isn't a run");
    CHECK(strcmp(err, "crashed loading main.lua: main.lua:2: top") == 0, "err: '%s'", err);
    CHECK(st->state == APPS_STATE_CRASHED && strcmp(st->log_tail, "loading") == 0, "the record: %s '%s'", apps_state_name(st->state),
          st->log_tail);
    int reset = WAIT(from, "reset");
    CHECK(reset < WAIT(from, "crash Top | main.lua:2: top"), "the card after the reset");
    ORDERED(from, "reset", "free pkg top");
    heap_caps_free(st);
}

static void a_main_that_does_not_compile_fails_the_run(void) {
    static const char bad[] = "local x = = 1\n";
    const file_t files[] = {LUA("main.lua", bad)};
    apps_status_t *st = new_status();
    char err[300];
    int from = mark();
    CHECK(!apps_task_run(make_pkg("syntax", "Syntax", files, 1), true, 100, st, err, sizeof err), "no app to run");
    CHECK(strncmp(err, "main.lua:1:", 11) == 0, "Lua's message: '%s'", err);
    CHECK(st->state == APPS_STATE_STOPPED && strcmp(st->last_error, err) == 0 && strcmp(st->id, "syntax") == 0,
          "the record: %s '%s' %s", apps_state_name(st->state), st->last_error, st->id);
    /* Nothing was published, so nothing points into the package: it is freed at once, with no reset and no card. */
    WAIT(from, "free pkg syntax");
    WAIT_FRAMES(5);
    ABSENT(from, "set_ui table");
    ABSENT(from, "reset");
    ABSENT(from, "crash ");
    heap_caps_free(st);
}

static const char APP_HANG[] =
    "local app = {}\n"
    "function app.start() ui.button{text = 'Hang'}:on('tap', function() print('hang') end) end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";

/* A run posted while the app task is stuck: it waits in the queue, and its caller in apps_task_run. */
static apps_pkg_t *queued_pkg;
static bool queued_ok;
static char queued_err[200];
static atomic_bool queued_done;

static void *queued_caller(void *arg) {
    (void)arg;
    char err[200];
    apps_status_t *st = new_status();
    queued_ok = apps_task_run(queued_pkg, true, 0, st, err, sizeof err);
    snprintf(queued_err, sizeof queued_err, "%s", err);
    heap_caps_free(st);
    atomic_store(&queued_done, true);
    return NULL;
}

/* A callback stuck in C (its print doesn't return). The supervisor's 3000 ms rule, across the clock's 32-bit wrap;
 * a caller already waiting gives up when the app hangs; the calls that answer at once while it hangs; and the app's
 * state freed when the call comes back. */
static void a_hung_call_is_abandoned(void) {
    const file_t files[] = {LUA("main.lua", APP_HANG)};
    char err[200];
    CHECK(apps_task_run(make_pkg("hangy", "Hangy", files, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    const int64_t t0 = ((int64_t)3 << 32) - 1000;  /* 1 s before the low 32 bits wrap */
    clock_stop_at(t0);
    block_print("hang");
    int from = mark();
    TAP(0);
    WAIT_PRINT_BLOCKED();
    WAIT_FRAMES(5);  /* the supervisor looks before the wrap too */
    ABSENT(from, "crash ");
    queued_pkg = make_pkg("queued", "Queued", HELLO, 1);
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, queued_caller, NULL) == 0, "no caller thread");
    sleep_ms(50);  /* it has posted, and waits */
    CHECK(!atomic_load(&queued_done), "the run waits behind the stuck call");
    clock_stop_at(t0 + APPS_TASK_HUNG_MS);
    WAIT_FRAMES(10);
    ABSENT(from, "crash ");
    ABSENT(from, "priority muse_apps");
    ABSENT(from, "front 0");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING, "3000 ms isn't hung: %s", apps_state_name(st->state));
    clock_stop_at(t0 + APPS_TASK_HUNG_MS + 1);
    int card = WAIT(from, "crash Hangy | (stopped responding)");
    CHECK(at_from(from, "front 0") >= 0 && at_from(from, "front 0") < card, "the layer goes behind");
    CHECK(at_from(from, "priority muse_apps 1") >= 0, "the app task drops to priority 1");
    CHECK(at_from(from, "log W Hangy stopped responding") >= 0, "the log says so");
    /* The waiting run gives up at once, long before its own deadline (the clock stands still). */
    for (int64_t until = real_ms() + 1500; !atomic_load(&queued_done) && real_ms() < until;) {
        sleep_ms(2);
    }
    CHECK(atomic_load(&queued_done), "the waiting run gave up when the app hung");
    pthread_join(caller, NULL);
    CHECK(!queued_ok && strcmp(queued_err, HUNG_MESSAGE) == 0, "the waiting run: '%s'", queued_err);
    /* Every call answers at once, and none posts. */
    int64_t start = real_ms();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG, "the status: %s", apps_state_name(st->state));
    int asked = mark();
    CHECK(!apps_task_run(make_pkg("other", "Other", HELLO, 1), true, 0, st, err, sizeof err) && strcmp(err, HUNG_MESSAGE) == 0,
          "run: '%s'", err);
    CHECK(at_from(asked, "free pkg other") >= 0, "the package a refused run was given is freed");
    apps_pkg_t *p = make_pkg("other", "Other", HELLO, 1);
    CHECK(!apps_task_compile(p, err, sizeof err) && strcmp(err, HUNG_MESSAGE) == 0, "compile: '%s'", err);
    free_pkg(p);
    apps_task_stop();
    apps_task_launch("hangy", true);
    CHECK(real_ms() - start < 500, "they answered at once: %lld ms", (long long)(real_ms() - start));
    ABSENT(from, "front 1");
    ABSENT(from, "print other");
    /* The call comes back: nothing of the app runs again (no stop()), its state goes in the teardown's order, and the task
     * is back at priority 2 and serves calls. */
    int back = mark();
    clock_go();
    release_print();
    WAIT(back, "priority muse_apps 2");
    ORDERED(back, "set_ui NULL", "reset");
    ORDERED(back, "reset", "free pkg hangy");
    CHECK(at_from(back, "log I stop hangy: hung, peak ") >= 0, "the stop line");
    ABSENT(from, "print hangy stop");
    /* The run its caller gave up on is dropped when the task gets to it: its package goes, nothing of it runs. */
    WAIT(back, "free pkg queued");
    ABSENT(from, "log I launch queued");
    ABSENT(from, "print queued");
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG && strcmp(st->id, "hangy") == 0, "the record keeps the hang: %s %s",
          apps_state_name(st->state), st->id);
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), true, 50, st, err, sizeof err), "a run after it: %s", err);
    CHECK(st->state == APPS_STATE_RUNNING, "running");
    heap_caps_free(st);
}

static const char APP_SLOW_STOP[] =
    "local app = {}\n"
    "function app.start() ui.label{text = 'Bye'} end\n"
    "function app.stop() print('stopping') end\n"
    "return app\n";

static void *stop_caller(void *arg) {
    (void)arg;
    apps_task_stop();
    return NULL;
}

/* stop() itself hangs: the stop's caller returns once the supervisor has marked it, and when stop() comes back the app
 * goes as a hung one (stop() doesn't run twice) and the task recovers. */
static void a_hung_stop_is_abandoned(void) {
    const file_t files[] = {LUA("main.lua", APP_SLOW_STOP)};
    char err[200];
    CHECK(apps_task_run(make_pkg("slow", "Slow", files, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("stopping");
    int from = mark();
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, stop_caller, NULL) == 0, "no caller thread");
    WAIT_PRINT_BLOCKED();
    int64_t start = real_ms();
    clock_stop_at(t0 + APPS_TASK_HUNG_MS + 1);
    int card = WAIT(from, "crash Slow | (stopped responding)");
    pthread_join(caller, NULL);
    CHECK(real_ms() - start < 1500, "the stop's caller returned once the app hung: %lld ms", (long long)(real_ms() - start));
    CHECK(at_from(from, "front 0") >= 0 && at_from(from, "front 0") < card, "the layer goes behind");
    CHECK(at_from(from, "priority muse_apps 1") >= 0, "the app task drops to priority 1");
    ABSENT(from, "set_ui NULL");  /* nothing is taken from the app while its stop() runs */
    int back = mark();
    clock_go();
    release_print();
    WAIT(back, "priority muse_apps 2");
    ORDERED(back, "set_ui NULL", "reset");
    ORDERED(back, "reset", "free pkg slow");
    CHECK(at_from(back, "log I stop slow: hung, peak ") >= 0, "the stop line says hung");
    CHECK(count_from(from, "print slow stopping") == 1, "stop() ran once");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG && strcmp(st->id, "slow") == 0, "the record keeps the hang: %s %s",
          apps_state_name(st->state), st->id);
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), true, 50, st, err, sizeof err), "a run after it: %s", err);
    CHECK(st->state == APPS_STATE_RUNNING, "running");
    heap_caps_free(st);
}

/* C5: a launch posts and returns, even while the app task is busy in a call (the clock stopped: busy, not hung). The
 * status doesn't wait either. */
static void a_launch_never_waits(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    CHECK(!apps_lv_front(), "behind");
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int from = mark();
    int64_t start = real_ms();
    apps_task_launch("busy", true);
    apps_task_launch("nosuch", true);
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(real_ms() - start < 200, "a launch and a status don't wait: %lld ms", (long long)(real_ms() - start));
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "busy") == 0, "the status: %s", apps_state_name(st->state));
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");
    release_print();
    clock_go();
    WAIT(from, "front 1");
    WAIT(from, "print busy show");
    WAIT(from, "log W launch nosuch: ");
    /* Nor does it wait for room in a full queue: it is dropped, and the log says so. */
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    for (int i = 0; i < 20; i++) {
        TAP(0);
    }
    int full = mark();
    start = real_ms();
    apps_task_launch("busy", true);
    CHECK(real_ms() - start < 200, "a launch into a full queue doesn't wait: %lld ms", (long long)(real_ms() - start));
    CHECK(at_from(full, "log W launch busy: the app task is busy; try again") >= 0, "the log says it was dropped");
    release_print();
    clock_go();
    heap_caps_free(st);
}

static int overflow_of(const apps_widget_t *w) {
    if (w->kind == APPS_W_BOX) {
        return 5;
    }
    if (w->text == NULL) {
        return 0;
    }
    if (strcmp(w->text, "Time's up") == 0) {
        return 14;
    }
    if (strcmp(w->text, "hidden") == 0) {
        return 9;
    }
    if (strcmp(w->text, "line one\nline two") == 0) {
        return 3;
    }
    return strncmp(w->text, "xxxx", 4) == 0 ? 7 : 0;
}

static const char APP_WIDE[] =
    "local app = {}\n"
    "function app.start()\n"
    "  ui.label{text = \"Time's up\"}\n"
    "  ui.box{w = 300, h = 300}\n"
    "  ui.label{text = 'fits'}\n"
    "  ui.label{text = 'hidden', hidden = true}\n"
    "  ui.label{text = 'line one\\nline two'}\n"
    "  ui.label{text = string.rep('x', 60)}\n"
    "end\n"
    "app.strat = function() end\n"
    "return app\n";

/* Run step 7: after a pass has laid the widgets out, each visible one past the round edge is a warning, after the
 * manifest's and main.lua's. */
static void overflow_warnings(void) {
    overflow_rule = overflow_of;
    const file_t files[] = {LUA("main.lua", APP_WIDE)};
    apps_pkg_t *p = make_pkg("wide", "Wide", files, 1);
    snprintf(p->manifest.warnings, sizeof p->manifest.warnings, "manifest says hi");
    apps_status_t *st = new_status();
    char err[200];
    CHECK(apps_task_run(p, true, 50, st, err, sizeof err), "run: %s", err);
    static const char overflow[] =
        "; label#0 'Time's up' extends 14 px past the round edge"
        "; box#1 'box' extends 5 px past the round edge"
        "; label#4 'line one?line two' extends 3 px past the round edge"
        "; label#5 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx...' extends 7 px past the round edge";
    size_t len = strlen(st->warnings);
    CHECK(strncmp(st->warnings, "manifest says hi; main.lua returned unknown handler 'strat'", 59) == 0 &&
              len > strlen(overflow) && strcmp(st->warnings + len - strlen(overflow), overflow) == 0,
          "warnings: '%s'", st->warnings);
    /* With no settle at all the run still waits for a pass that has laid the new widgets out. */
    CHECK(apps_task_run(make_pkg("wide", "Wide", files, 1), true, 0, st, err, sizeof err), "run: %s", err);
    len = strlen(st->warnings);
    CHECK(len > strlen(overflow) && strcmp(st->warnings + len - strlen(overflow), overflow) == 0, "warnings with no settle: '%s'",
          st->warnings);
    heap_caps_free(st);
}

/* C8: the screen sleeps (the frames run the supervisor, not the apply step): a run waits at most APPS_TASK_LAYOUT_MS for
 * a pass and says so. The reset still runs, so a stop frees the package at once. */
static void the_layout_waits_for_the_screen(void) {
    atomic_store(&lvgl_asleep, true);
    apps_status_t *st = new_status();
    char err[200];
    int from = mark();
    int64_t start = real_ms();
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), true, 50, st, err, sizeof err), "run: %s", err);
    int64_t took = real_ms() - start;
    CHECK(strcmp(st->warnings, "layout not checked: the screen was asleep") == 0, "warnings: '%s'", st->warnings);
    CHECK(took >= 50 + APPS_TASK_LAYOUT_MS - 20 && took < 50 + APPS_TASK_LAYOUT_MS + 1500, "it waited for a pass, bounded: %lld ms",
          (long long)took);
    CHECK(at_from(from, "set_ui table#1") >= 0, "published");
    from = mark();
    apps_task_stop();
    ORDERED(from, "set_ui NULL", "reset");
    ORDERED(from, "reset", "free pkg hello");
    atomic_store(&lvgl_asleep, false);
    heap_caps_free(st);
}

static void compile_checks_every_file(void) {
    char err[200];
    const file_t good[] = {LUA("main.lua", APP_HELLO), LUA("lib/util.lua", "return {}\n")};
    apps_pkg_t *p = make_pkg("comp", "Comp", good, 2);
    CHECK(apps_task_compile(p, err, sizeof err) && err[0] == '\0', "a good package: '%s'", err);
    free_pkg(p);  /* the caller's: what the compile used was copied */
    const file_t bad[] = {LUA("main.lua", APP_HELLO), LUA("lib/util.lua", "return {\n")};
    p = make_pkg("comp", "Comp", bad, 2);
    CHECK(!apps_task_compile(p, err, sizeof err) && strncmp(err, "lib/util.lua:", 13) == 0, "a bad module: '%s'", err);
    free_pkg(p);
    const file_t no_main[] = {LUA("lib/util.lua", "return {}\n")};
    p = make_pkg("comp", "Comp", no_main, 1);
    CHECK(!apps_task_compile(p, err, sizeof err) && strcmp(err, "main.lua is missing from the package") == 0, "no main: '%s'", err);
    free_pkg(p);
    ABSENT(0, "print comp");  /* nothing ran */
    /* A compile leaves a running app as it was. */
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    p = make_pkg("comp", "Comp", good, 2);
    CHECK(apps_task_compile(p, err, sizeof err), "compile: '%s'", err);
    free_pkg(p);
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "hello") == 0, "still running: %s %s", apps_state_name(st->state), st->id);
    heap_caps_free(st);
}

/* C11: a voice turn hides an app in front and its end brings it back; a front request during the turn waits for its
 * end. */
static void a_voice_turn_hides_the_app(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print busy show");
    int from = mark();
    TURN(true);
    WAIT(from, "front 0");
    WAIT(from, "print busy hide");
    apps_task_launch("busy", true);
    WAIT_FRAMES(10);
    ABSENT(from, "front 1");
    int ended = mark();
    TURN(false);
    WAIT(ended, "front 1");
    WAIT(ended, "print busy show");
    /* An app behind when the turn begins stays behind after it. */
    ON_LVGL(swipe_job, NULL);
    WAIT(ended, "print busy hide");
    int behind = mark();
    TURN(true);
    TURN(false);
    WAIT_FRAMES(10);
    ABSENT(behind, "front 1");
    ABSENT(behind, "print busy show");
}

/* The close callback (a right swipe) only backgrounds: hide() runs, nothing is torn down. The launcher brings it back. */
static void a_right_swipe_only_backgrounds(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 100, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print busy show");
    int from = mark();
    ON_LVGL(swipe_job, NULL);
    WAIT(from, "front 0");
    WAIT(from, "print busy hide");
    WAIT_FRAMES(5);
    ABSENT(from, "set_ui NULL");
    ABSENT(from, "reset");
    ABSENT(from, "print busy stop");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && !st->front, "running behind: %s %d", apps_state_name(st->state), st->front);
    heap_caps_free(st);
    int opened = mark();
    muse_apps_open_launcher();
    WAIT(opened, "launcher 1 busy:Busy");
    ON_LVGL(pick_job, "busy");
    WAIT(opened, "front 1");
    WAIT(opened, "print busy show");
}

static void the_launcher_lists_the_resident_app(void) {
    int from = mark();
    muse_apps_open_launcher();  /* from this task: the LVGL task shows it */
    WAIT(from, "launcher 0");
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    from = mark();
    muse_apps_open_launcher();
    WAIT(from, "launcher 1 busy:Busy");
    ON_LVGL(pick_job, "busy");
    WAIT(from, "front 1");
    WAIT(from, "print busy show");
    /* A voice turn holds the launcher back until it ends. */
    TURN(true);
    int during = mark();
    muse_apps_open_launcher();
    WAIT_FRAMES(10);
    ABSENT(during, "launcher");
    TURN(false);
    WAIT(during, "launcher 1 busy:Busy");
}

/* The caller of a run gives up while the app task is held up outside any call (another thread holds the UI lock, and
 * the clock moves past the wait): the answer that comes later writes nothing to the caller's buffers, which are gone. */
static apps_pkg_t *late_pkg;
static bool late_ok;
static char late_err[200];

static void *late_caller(void *arg) {
    (void)arg;
    char *err = malloc(200);
    apps_status_t *st = new_status();
    late_ok = apps_task_run(late_pkg, true, 0, st, err, 200);
    snprintf(late_err, sizeof late_err, "%s", err);
    free(err);
    heap_caps_free(st);
    return NULL;
}

static void a_late_answer_writes_nothing(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("one", "One", HELLO, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    late_pkg = make_pkg("two", "Two", HELLO, 1);
    int from = mark();
    apps_port_ui_lock();
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, late_caller, NULL) == 0, "no caller thread");
    WAIT(from, "print one stop");  /* the run has begun: the old app's stop() ran, its unpublish waits for the lock */
    sleep_ms(20);
    clock_jump(APPS_TASK_RUN_EXTRA_MS + 1000);
    pthread_join(caller, NULL);
    CHECK(!late_ok && strcmp(late_err, "the app task didn't answer within 7000 ms") == 0, "the caller gave up: '%s'", late_err);
    apps_port_ui_unlock();
    WAIT(from, "set_ui table#2");
    WAIT(from, "print two show");  /* the layer was in front, and stays: the new app is shown */
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "two") == 0, "the run went on: %s %s", apps_state_name(st->state), st->id);
    heap_caps_free(st);
}

/* Touches the queue has no room for are dropped, never waited for, and counted. */
static void touches_past_a_full_queue_are_counted(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int64_t start = real_ms();
    for (int i = 0; i < 30; i++) {
        TAP(0);
    }
    CHECK(real_ms() - start < 2000, "30 touches in %lld ms", (long long)(real_ms() - start));
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->dropped_events >= 30 - 16, "dropped: %u", st->dropped_events);
    int from = mark();
    release_print();
    clock_go();
    WAIT(from, "print busy busy");
    heap_caps_free(st);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"task_shape", task_shape},
        {"run_and_status", run_and_status},
        {"stop_tears_down_in_order", stop_tears_down_in_order},
        {"a_late_reset_parks_the_images", a_late_reset_parks_the_images},
        {"a_new_run_replaces_the_app", a_new_run_replaces_the_app},
        {"a_crash_in_start_is_contained", a_crash_in_start_is_contained},
        {"a_crash_in_a_callback_is_contained", a_crash_in_a_callback_is_contained},
        {"a_crash_at_the_top_level_is_contained", a_crash_at_the_top_level_is_contained},
        {"a_main_that_does_not_compile_fails_the_run", a_main_that_does_not_compile_fails_the_run},
        {"a_hung_call_is_abandoned", a_hung_call_is_abandoned},
        {"a_hung_stop_is_abandoned", a_hung_stop_is_abandoned},
        {"a_launch_never_waits", a_launch_never_waits},
        {"overflow_warnings", overflow_warnings},
        {"the_layout_waits_for_the_screen", the_layout_waits_for_the_screen},
        {"compile_checks_every_file", compile_checks_every_file},
        {"a_voice_turn_hides_the_app", a_voice_turn_hides_the_app},
        {"a_right_swipe_only_backgrounds", a_right_swipe_only_backgrounds},
        {"the_launcher_lists_the_resident_app", the_launcher_lists_the_resident_app},
        {"a_late_answer_writes_nothing", a_late_answer_writes_nothing},
        {"touches_past_a_full_queue_are_counted", touches_past_a_full_queue_are_counted},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            boot();
            cases[i].run();
            finish();
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
