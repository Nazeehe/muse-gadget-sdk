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
 * "sleeps"; a paused one runs nothing, as LVGL paused on battery), and the case's own thread, the caller. The I/O
 * worker (apps_io.c, made on demand) is a fourth in the cases that submit jobs. Built with HARNESS_CONSOLE, the
 * console's launch job and the RAM store it loads from (apps_console.c, apps_install.c, with the real cJSON) are real
 * too, for the cases that need them; without it (Task 12's cases, which need no cJSON) muse_apps_launch and
 * muse_apps_console are stubs, and no app is in a store. Under the app
 * task is the real runtime (apps_rt, apps_ui, apps_vm, Lua); apps_lv is a fake that writes what is done to the screen
 * into one log of events, in the order it happens, and that keeps the image objects a pass makes and draws each from
 * the package's bytes at every frame, before the supervisor (LVGL's refresh may come first in a round of its timers),
 * until a reset takes them down: bytes freed while an object still points into them are an AddressSanitizer report.
 * The fake apps_lv also checks that what must run on the LVGL task does, and that a pass never meets a new table while
 * an old app's objects are still up.
 *
 * The port is the harness's: a counting allocator that logs the free of the packages and tables it watches, a UI lock
 * that checks its contract (no double take, no heap use while held), a console whose print of a given line blocks
 * until the case lets it go (a callback stuck in C), and a clock the case can stop and move. A print otherwise only
 * queues its line, as the device's port does (F3), and the drain echoes the queue holding the console, which a case can
 * hold as another task's long reply does.
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

#ifdef HARNESS_CONSOLE
#include "apps_install.h"
#endif
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_task.h"
#include "apps_ui.h"
#include "muse_apps.h"
#include "muse_apps_task_fakes.h"
#include "muse_input.h"

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

/* How long a number of ticks is, at the tick rate the fakes were built for (MUSE_FAKE_TICK_HZ: 1000, or 100 for the cases that
 * show what a coarse tick does). */
static int64_t ticks_to_ms(TickType_t ticks) {
    return (int64_t)ticks * 1000 / configTICK_RATE_HZ;
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
static atomic_bool events_full;  /* an event was dropped: no check of the log can be trusted, and the case fails */

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
    } else {
        atomic_store(&events_full, true);
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

/* A full log drops events, and then neither a wait nor an absence means anything: the case fails. */
static void log_trusted_at(int line) {
    if (atomic_load(&events_full)) {
        fail_at(line, "the event log is full (%d events): its checks can't be trusted", EVENTS_MAX);
    }
}

/* Waits up to ms of real time for an event that starts with prefix, at or after from, and returns its index. */
static int wait_event_at(int line, int from, const char *prefix, int ms) {
    int64_t until = real_ms() + ms;
    for (;;) {
        log_trusted_at(line);
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
    log_trusted_at(line);
    int a = at_from(from, first), b = at_from(from, then);
    if (a < 0 || b < 0 || a > b) {
        fail_at(line, "'%s' (%d) should come before '%s' (%d), both after %d", first, a, then, b, from);
    }
}
#define ORDERED(from, first, then) ordered_at(__LINE__, (from), (first), (then))

static void absent_at(int line, int from, const char *prefix) {
    log_trusted_at(line);
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
    bool room = false;
    pthread_mutex_lock(&heap_mu);
    for (size_t i = 0; i < ARRAY_LEN(watched) && !room; i++) {
        if (watched[i].ptr == NULL) {
            watched[i].ptr = ptr;
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(watched[i].label, sizeof watched[i].label, fmt, ap);
            va_end(ap);
            room = true;
        }
    }
    pthread_mutex_unlock(&heap_mu);
    if (!room) {
        error("more than %zu watched blocks: a free could go unseen", ARRAY_LEN(watched));
    }
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
static void forget_table(const void *ptr);

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
    if (strncmp(label, "table#", 6) == 0) {
        forget_table(ptr);
    }
    free(b);
}

static size_t held_now(size_t *counter) {
    pthread_mutex_lock(&heap_mu);
    size_t n = *counter;
    pthread_mutex_unlock(&heap_mu);
    return n;
}

/* heap_caps_*: the app task's own blocks (its record and its requests), which must be PSRAM. A case can make the next
 * ones fail (out of memory). */
static atomic_int caps_failures;

void *heap_caps_malloc(size_t size, uint32_t caps) {
    if ((caps & MALLOC_CAP_SPIRAM) == 0) {
        error("heap_caps_malloc of %zu bytes not in PSRAM (caps 0x%x)", size, (unsigned)caps);
    }
    if (atomic_load(&caps_failures) > 0) {
        atomic_fetch_sub(&caps_failures, 1);
        return NULL;
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

/* What heap_caps_get_free_size says of internal RAM (apps_io_submit's admission): a case can make it short. */
static atomic_size_t internal_free = 200u * 1024;

size_t heap_caps_get_free_size(uint32_t caps) {
    return (caps & MALLOC_CAP_INTERNAL) != 0 ? atomic_load(&internal_free) : 4u * 1024 * 1024;
}

/* And its largest free block (the admission of a new worker, P1): a case can fragment it. */
static atomic_size_t internal_largest = 200u * 1024;

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    return (caps & MALLOC_CAP_INTERNAL) != 0 ? atomic_load(&internal_largest) : 4u * 1024 * 1024;
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

static bool on_lvgl_task(void);
static struct fake_task *app_task_of_thread(void);

void apps_port_ui_lock(void) {
    if (ui_held_by_me()) {
        error("apps_port_ui_lock taken again by its holder");
        return;
    }
    pthread_mutex_lock(&ui_mu);
    if (ui_held) {
        /* Someone holds it: a case can wait for this instead of sleeping. */
        pthread_mutex_unlock(&ui_mu);
        event("ui_lock wait %s", app_task_of_thread() != NULL ? "app" : on_lvgl_task() ? "lvgl" : "case");
        pthread_mutex_lock(&ui_mu);
    }
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

/* The console. A print of block_line doesn't return until the case lets it go: a call stuck in C. Any other print only
 * queues its line ("print <id> <line>"), as the device's port does (F3); apps_port_log_drain, on the app task, echoes
 * the queue ("echo <id> <line>") with the device's locking (N1): console_mu, recursive like picolibc's stdio lock, held
 * once for the whole queue (flockfile) and taken again for each line (its printf and flush), never with nothing queued.
 * A case holds it as another task's long reply holds the console. */
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t print_cv = PTHREAD_COND_INITIALIZER;
static const char *block_line;
static bool print_blocked;
static pthread_mutex_t console_mu;

__attribute__((constructor)) static void console_mu_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&console_mu, &attr);
    pthread_mutexattr_destroy(&attr);
}

static pthread_mutex_t echo_mu = PTHREAD_MUTEX_INITIALIZER;
static char *echo_queue[64];
static int echo_queued;

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
    char text[300];
    snprintf(text, sizeof text, "%s %s", app_id, line);
    pthread_mutex_lock(&echo_mu);
    if (echo_queued < (int)ARRAY_LEN(echo_queue)) {
        echo_queue[echo_queued++] = strdup(text);
    }
    pthread_mutex_unlock(&echo_mu);
}

void apps_port_log_drain(void) {
    if (app_task_of_thread() == NULL) {
        error("apps_port_log_drain off the app task");
    }
    char *lines[ARRAY_LEN(echo_queue)];
    pthread_mutex_lock(&echo_mu);
    int n = echo_queued;
    memcpy(lines, echo_queue, (size_t)n * sizeof lines[0]);
    echo_queued = 0;
    pthread_mutex_unlock(&echo_mu);
    if (n == 0) {
        return;
    }
    event("drain %d", n);  /* then it waits for the console, once */
    pthread_mutex_lock(&console_mu);
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&console_mu);  /* the line's printf and flush: the holder takes it again at once */
        event("echo %s", lines[i]);
        pthread_mutex_unlock(&console_mu);
        free(lines[i]);
    }
    pthread_mutex_unlock(&console_mu);
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

/* When the I/O worker said it goes, and when its last job ended: both on the worker's own thread (T7). */
static _Atomic int64_t io_idle_ms, io_job_end_ms;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (strcmp(tag, "muse_apps") != 0) {
        error("a log line with the tag '%s'", tag);
    }
    if (strncmp(msg, "io: idle", 8) == 0) {
        atomic_store(&io_idle_ms, real_ms());
    }
    event("log %c %s", level, msg);
}

/* muse_input's console hooks, which muse_apps_start adds the apps' to (tests/test_muse_apps_console.py has the chain
 * and the console itself; built with HARNESS_CONSOLE, the real apps_console.c is linked here for its launch job). */
void muse_input_add_console_hook(bool (*hook)(const char *line)) {
    event("console hook %s", hook == muse_apps_console ? "muse_apps_console" : "?");
}

#ifndef HARNESS_CONSOLE
/* Without the console: no line is the apps', and no app is in a store, so a launch by id finds nothing to run (the
 * worker's job would say so in one line). */
bool muse_apps_console(const char *line) {
    (void)line;
    return false;
}

bool muse_apps_launch(const char *id, bool front) {
    event("launch by id %s %d", id, front ? 1 : 0);
    fake_esp_log('W', "muse_apps", "launch %s: no such app on the gadget", id);
    return true;
}
#endif

/* Free PSRAM, for the RAM store's room (the store class): plenty. */
size_t apps_port_psram_free(void) {
    return (size_t)64 << 20;
}

/* The end of a console reply (apps_port_idf.c's on the gadget). */
void apps_port_console_flush(void) {
    fflush(stdout);
}

/* ---- The fake FreeRTOS: tasks, queues and mutexes on pthreads ---- */

struct fake_task {
    pthread_t thread;
    TaskFunction_t code;
    void *arg;
    char name[24];
    UBaseType_t priority;
};

static struct fake_task app_tasks[16];  /* the app task, then each I/O worker made */
static int n_app_tasks;
static _Thread_local struct fake_task thread_task;  /* what a thread of the harness is to FreeRTOS */
static _Thread_local struct fake_task *current_task;
static pthread_mutex_t tasks_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(TickType_t) longest_receive;  /* the longest wait the app task asked of xQueueReceive */
static atomic_uint zero_receives;            /* how many times it asked for no wait at all: it never blocked then */

static struct fake_task *self_task(void) {
    return current_task != NULL ? current_task : &thread_task;
}

/* The app task, when this thread is it; NULL on the LVGL task, the I/O worker and the case's threads. */
static struct fake_task *app_task_of_thread(void) {
    return current_task != NULL && strcmp(current_task->name, "muse_apps") == 0 ? current_task : NULL;
}

static void *task_main(void *arg) {
    struct fake_task *task = arg;
    current_task = task;
    task->code(task->arg);
    return NULL;
}

static BaseType_t make_task(TaskFunction_t code, const char *name, void *arg, UBaseType_t priority, TaskHandle_t *created) {
    pthread_mutex_lock(&tasks_mu);
    struct fake_task *task = n_app_tasks < (int)ARRAY_LEN(app_tasks) ? &app_tasks[n_app_tasks++] : NULL;
    pthread_mutex_unlock(&tasks_mu);
    if (task == NULL) {
        return pdFAIL;
    }
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

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps) {
    event("task %s stack %u priority %u core %d caps 0x%x", name, (unsigned)stack, priority, core, caps);
    return make_task(code, name, arg, priority, created);
}

/* The I/O worker's: its stack in internal RAM, on either core. */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                   UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core) {
    char where[16];
    snprintf(where, sizeof where, core == tskNO_AFFINITY ? "any" : "%d", (int)core);
    event("task %s stack %u priority %u core %s internal", name, (unsigned)stack, priority, where);
    return make_task(code, name, arg, priority, created);
}

/* Only a task's own end (vTaskDelete(NULL)), as the I/O worker's. */
void vTaskDelete(TaskHandle_t task) {
    if (task != NULL && task != current_task) {
        error("vTaskDelete of another task");
        return;
    }
    if (current_task == NULL) {
        error("vTaskDelete(NULL) from a thread that isn't a task");
        return;
    }
    event("task %s deleted", current_task->name);
    pthread_exit(NULL);
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

/* 40000 bytes for the app task (the status says so), 3000 for any other. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    const struct fake_task *t = task != NULL ? task : self_task();
    return strcmp(t->name, "muse_apps") == 0 ? 40000 : 3000;
}

void vTaskDelay(const TickType_t ticks) {
    sleep_ms((int)ticks_to_ms(ticks));
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
    struct timespec deadline = deadline_in(ticks_to_ms(wait));
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

/* What a post asked of the queue: the LVGL task and apps_task_launch never wait (C5, the rulings), whatever the
 * queue holds, so what they pass is recorded, not timed. */
static _Atomic(TickType_t) lvgl_longest_send, launch_longest_send;
static _Thread_local bool in_launch;  /* this thread is inside apps_task_launch (the case marks it) */
static const char *const EVENT_NAMES[] = {"?", "TOUCH", "RUN", "STOP", "COMPILE", "MESSAGE", "SHOW", "HIDE", "ALARM", "EXIT",
                                          "WAKE"};

static void note_send(const void *item, size_t item_size, TickType_t wait) {
    _Atomic(TickType_t) *longest = on_lvgl_task() ? &lvgl_longest_send : in_launch ? &launch_longest_send : NULL;
    if (longest != NULL && wait > atomic_load(longest)) {
        atomic_store(longest, wait);
    }
    if (item_size == sizeof(apps_event_t)) {
        uint8_t type = ((const apps_event_t *)item)->type;
        if (type != APPS_EV_TOUCH) {  /* touches come by the dozen: their count is checked instead */
            event("queue %s wait %u", type < ARRAY_LEN(EVENT_NAMES) ? EVENT_NAMES[type] : "?", (unsigned)wait);
        }
    }
}

UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) {
    pthread_mutex_lock(&q->mu);
    UBaseType_t spaces = (UBaseType_t)(q->length - q->count);
    pthread_mutex_unlock(&q->mu);
    return spaces;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    pthread_mutex_lock(&q->mu);
    UBaseType_t waiting = (UBaseType_t)q->count;
    pthread_mutex_unlock(&q->mu);
    return waiting;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    note_send(item, q->item_size, wait);
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
    if (app_task_of_thread() != NULL && wait > atomic_load(&longest_receive)) {
        atomic_store(&longest_receive, wait);
    }
    if (app_task_of_thread() != NULL && wait == 0) {
        atomic_fetch_add(&zero_receives, 1);
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
    int number;  /* in the order made: muse_apps_start makes the record's (1), the requests' (2), the names' (3), then
                  * the I/O worker's (4) */
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

enum { MUTEX_RECORD = 1, MUTEX_REQUESTS = 2, MUTEX_NAMES = 3, MUTEX_IO = 4 };
static atomic_int n_mutexes;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)(void *)buffer->opaque;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->held = false;
    m->number = atomic_fetch_add(&n_mutexes, 1) + 1;
    return m;
}

/* A gate on one mutex: the next take of it by a task (the app task, the I/O worker: by name) or by a thread of the case
 * that marked itself (NULL), after a given event, stops before taking it until the case opens the gate. It puts another task's step in between two of
 * the app task's, or between a caller's read and its lock, where only a race could otherwise put it. gate_open counts
 * releases, and a held thread waits for the count to pass the one it saw, so arming the gate again right after opening
 * it can't hold that thread a second time. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int number;
    char task[24];  /* "" for a thread of the case that marked itself */
    char after[80];
    int from;
    bool armed;
    unsigned releases;
} gate = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static _Thread_local bool gated_thread;

static void gate_arm(int number, const char *task, int from, const char *after) {
    pthread_mutex_lock(&gate.mu);
    gate.number = number;
    snprintf(gate.task, sizeof gate.task, "%s", task != NULL ? task : "");
    gate.from = from;
    snprintf(gate.after, sizeof gate.after, "%s", after != NULL ? after : "");
    gate.armed = true;
    pthread_mutex_unlock(&gate.mu);
}

/* Lets the thread the gate holds go on. */
static void gate_open(void) {
    pthread_mutex_lock(&gate.mu);
    gate.releases++;
    pthread_cond_broadcast(&gate.cv);
    pthread_mutex_unlock(&gate.mu);
}

static void gate_check(const struct fake_mutex *m) {
    pthread_mutex_lock(&gate.mu);
    bool mine = gate.armed && m->number == gate.number &&
                (gate.task[0] != '\0' ? current_task != NULL && strcmp(current_task->name, gate.task) == 0 : gated_thread) &&
                (gate.after[0] == '\0' || at_from(gate.from, gate.after) >= 0);
    if (!mine) {
        pthread_mutex_unlock(&gate.mu);
        return;
    }
    gate.armed = false;  /* decided and disarmed at once: one thread, one hold */
    unsigned seen = gate.releases;
    pthread_mutex_unlock(&gate.mu);
    event("gate %d", m->number);
    pthread_mutex_lock(&gate.mu);
    while (gate.releases == seen) {
        pthread_cond_wait(&gate.cv, &gate.mu);
    }
    pthread_mutex_unlock(&gate.mu);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    gate_check(m);
    struct timespec deadline = deadline_in(ticks_to_ms(wait));
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

/* A freed table's number isn't given to a new table at the same address (an allocator with no quarantine reuses it). */
static void forget_table(const void *ptr) {
    pthread_mutex_lock(&lv_mu);
    for (int i = 0; i < lv_n_tables; i++) {
        if (lv_tables[i] == ptr) {
            lv_tables[i] = NULL;
        }
    }
    pthread_mutex_unlock(&lv_mu);
}

static int table_number(apps_ui_t *ui) {
    for (int i = 0; i < lv_n_tables; i++) {
        if (lv_tables[i] == ui) {
            return i + 1;
        }
    }
    if (lv_n_tables == (int)ARRAY_LEN(lv_tables)) {
        error("more than %zu tables published: they can't be told apart", ARRAY_LEN(lv_tables));
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
        w->rw = w->text != NULL && strcmp(w->text, "zero width") == 0 ? 0 : 66;
        w->rh = 26;
        /* The rule's value even for a hidden widget (stale, as if left from before it was hidden): the app task must
         * skip hidden and zero-width widgets itself. */
        w->overflow = (int16_t)(overflow_rule != NULL ? overflow_rule(w) : 0);
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
static atomic_uint frames, paused_rounds;

/* The screen is dark when the case says the LVGL task sleeps: it runs no apply step then, as the real one doesn't. */
bool apps_port_dark(void) {
    return atomic_load(&lvgl_asleep);
}
static atomic_bool last_tick;  /* what muse_apps_ui_tick returned at the last frame: frame_tick returns early on true */

static void *lvgl_main(void *arg) {
    (void)arg;
    lvgl_thread = pthread_self();
    atomic_store(&lvgl_up, true);
    while (!atomic_load(&lvgl_quit)) {
        pthread_mutex_lock(&frame_mu);
        if (job != NULL && !atomic_load(&lvgl_paused)) {  /* a paused LVGL task runs no event either */
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
                atomic_store(&last_tick, muse_apps_ui_tick());
            }
            atomic_fetch_add(&frames, 1);
        } else {
            atomic_fetch_add(&paused_rounds, 1);
        }
        sleep_ms(3);
    }
    return NULL;
}

/* LVGL pauses (as on battery, asleep and dark): once this returns, no frame is under way and none starts, and no job
 * runs (ON_LVGL fails while it is paused: on the gadget a touch wakes the screen before anything of it runs). */
static void pause_lvgl(void) {
    atomic_store(&lvgl_paused, true);
    unsigned seen = atomic_load(&paused_rounds);
    int64_t until = real_ms() + 5000;
    while (atomic_load(&paused_rounds) - seen < 2) {  /* a whole paused round after the one the flag may have met */
        CHECK(real_ms() < until, "the LVGL task never saw the pause");
        sleep_ms(1);
    }
}

/* Runs fn(arg) on the LVGL task, between frames, and waits for it. Never while it is paused (pause_lvgl). */
static void on_lvgl_at(int line, void (*fn)(void *), void *arg) {
    if (atomic_load(&lvgl_paused)) {
        fail_at(line, "a job for a paused LVGL task: the gadget runs nothing on it then; unpause it first");
    }
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

/* The queue is first in, first out: once a compile posted now has its answer, everything posted before it (a launch,
 * touches) has been handled, and the loop's housekeeping has run since. */
static void fifo_sync_at(int line) {
    static const char tiny[] = "return {}\n";
    const file_t files[] = {{"main.lua", tiny, sizeof tiny - 1}};
    apps_pkg_t *p = make_pkg("sync", "Sync", files, 1);
    char err[200];
    bool ok = apps_task_compile(p, err, sizeof err);
    free_pkg(p);
    if (!ok) {
        fail_at(line, "the sync compile failed: %s", err);
    }
}
#define FIFO_SYNC() fifo_sync_at(__LINE__)

/* apps_task_launch, with what it passes the queue recorded (it must never wait). */
static void launch(const char *id, bool front) {
    in_launch = true;
    apps_task_launch(id, front);
    in_launch = false;
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

/* apps_task_stop: true when the app stopped (or none ran), otherwise false with why in err (F3). */
static bool stop_app(char *err, size_t err_len) {
    return apps_task_stop(err, err_len);
}


/* What muse_ui does at boot: the layer under the display lock, the frame timer, then the app task. */
static size_t caps_after_start;

static void boot(void) {
    building = true;
    muse_apps_ui_build(SCREEN);
    building = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, lvgl_main, NULL) == 0, "no LVGL thread");
    pthread_detach(thread);  /* it runs until the case exits, like the app task */
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
    (void)stop_app(NULL, 0);
    int64_t until = real_ms() + 5000;
    while ((held_now(&port_held) != 0 || held_now(&caps_held) != caps_after_start) && real_ms() < until) {
        sleep_ms(5);
    }
    CHECK(held_now(&port_held) == 0, "%zu bytes of the apps' heap still held", held_now(&port_held));
    CHECK(held_now(&caps_held) == caps_after_start, "%zu bytes of the app task's own still held (%zu after start)",
          held_now(&caps_held), caps_after_start);
    CHECK(errors == 0, "%d errors, the first: %s", errors, error_note);
    CHECK(!atomic_load(&events_full), "the event log filled up (%d events): the case's checks can't be trusted", EVENTS_MAX);
    CHECK(atomic_load(&longest_receive) <= pdMS_TO_TICKS(1000), "the app task waited %u ticks for an event: at most a second, %u (C22)",
          (unsigned)atomic_load(&longest_receive), (unsigned)pdMS_TO_TICKS(1000));
    CHECK(atomic_load(&lvgl_longest_send) == 0, "the LVGL task waited %u ticks to post: it never waits",
          (unsigned)atomic_load(&lvgl_longest_send));
    CHECK(atomic_load(&launch_longest_send) == 0, "apps_task_launch waited %u ticks to post: it never waits (C5)",
          (unsigned)atomic_load(&launch_longest_send));
    atomic_store(&lvgl_quit, true);
}

static void task_shape(void) {
    char want[96];
    CHECK(at_from(0, "task muse_apps stack 57344 priority 2 core 1 caps 0x404") >= 0,
          "the app task: muse_apps, 56 KB, priority 2, core 1, PSRAM");
    snprintf(want, sizeof want, "queue 16 x %zu caps 0x400", sizeof(apps_event_t));
    CHECK(at_from(0, want) >= 0, "16 events in PSRAM: %s", want);
    CHECK(at_from(0, "log I up: stack 57344 in PSRAM") >= 0, "the boot line");
    CHECK(count_from(0, "console hook muse_apps_console") == 1, "the console's hook, added once");
    snprintf(want, sizeof want, "queue %d x %zu caps 0x400", APPS_IO_JOBS, 2 * sizeof(void *));
    CHECK(at_from(0, want) >= 0, "the I/O worker's queue of %d jobs in PSRAM: %s", APPS_IO_JOBS, want);
    ABSENT(0, "task muse_apps_io");  /* made when a job comes */
    ORDERED(0, "port init", "task muse_apps");
    ORDERED(0, "build screen", "task muse_apps");
    /* Idle, its loop still comes round at least once a second (C22: finish() checks the waits). */
    for (int64_t until = real_ms() + 5000; atomic_load(&longest_receive) == 0;) {
        CHECK(real_ms() < until, "the app task never waited for an event");
        sleep_ms(1);
    }
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
    /* show() comes from the LVGL task's fronting, which a stalled host can put after the settle: wait for it, then for a
     * refresh of the record (at most once a second while the app runs), rather than for the clock. */
    CHECK(strcmp(st->log_tail, "start") == 0 || strcmp(st->log_tail, "start\nshow") == 0, "the log: '%s'", st->log_tail);
    WAIT(from, "print hello show");
    clock_jump(1001);
    FIFO_SYNC();
    apps_task_status(st);
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
    CHECK(stop_app(err, sizeof err) && err[0] == '\0', "stop: '%s'", err);
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
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
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
    pause_lvgl();
    int from = mark();
    int64_t start = real_ms();
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    int64_t took = real_ms() - start;
    CHECK(took >= 500 - 20 && took < 2000, "the stop waited for the reset, at most 500 ms: %lld ms", (long long)took);
    CHECK(count_from(from, "log W the screen didn't take the app down within 500 ms") == 1, "the wait's end is logged");
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

/* A crash in start(): the run fails with it and the record keeps it. Its table was never published, so nothing points
 * into its package: it goes at once, with no reset asked and no wait (F1), and its card comes at the next frame. After
 * an app whose reset hasn't run yet (LVGL paused), the card waits for that reset. */
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
    WAIT(from, "free pkg crashy");
    WAIT(from, "crash Crashy | main.lua:4: boom");
    ABSENT(from, "reset");
    ABSENT(from, "log W the screen didn't take");
    ABSENT(from, "set_ui table");
    ABSENT(from, "front 1");
    CHECK(at_from(from, "log I stop crashy: crashed, peak ") >= 0, "the stop line");
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strcmp(st->id, "crashy") == 0, "the status: %s %s", apps_state_name(st->state), st->id);
    CHECK(apps_task_run(make_pkg("pic", "Picture", PICTURE, 2), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "object image pic/a.bin");
    pause_lvgl();
    int later = mark();
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", files, 1), true, 0, NULL, err, sizeof err), "a crash isn't a run");
    WAIT(later, "free pkg crashy");
    CHECK(count_from(later, "log W the screen didn't take the app down") == 1, "only the picture's end waited");
    ABSENT(later, "crash ");
    atomic_store(&lvgl_paused, false);
    int card = WAIT(later, "crash Crashy | main.lua:4: boom");
    int reset = WAIT(later, "reset");
    CHECK(reset < card, "the card after the reset it waits for: %d %d", reset, card);
    ORDERED(later, "reset", "free pkg pic");
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
    /* Its table was never published: the package goes at once, no reset is asked, and the card comes (F1). */
    WAIT(from, "free pkg top");
    WAIT(from, "crash Top | main.lua:2: top");
    ABSENT(from, "reset");
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
    WAIT(from, "queue RUN wait");  /* it has posted, and waits */
    CHECK(!atomic_load(&queued_done), "the run waits behind the stuck call");
    clock_stop_at(t0 + 3000);
    WAIT_FRAMES(10);
    ABSENT(from, "crash ");
    ABSENT(from, "priority muse_apps");
    ABSENT(from, "front 0");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING, "3000 ms isn't hung: %s", apps_state_name(st->state));
    clock_stop_at(t0 + 3001);
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
    /* Every call answers at once: none posts. */
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG, "the status: %s", apps_state_name(st->state));
    int asked = mark();
    CHECK(!apps_task_run(make_pkg("other", "Other", HELLO, 1), true, 0, st, err, sizeof err) && strcmp(err, HUNG_MESSAGE) == 0,
          "run: '%s'", err);
    CHECK(at_from(asked, "free pkg other") >= 0, "the package a refused run was given is freed");
    apps_pkg_t *p = make_pkg("other", "Other", HELLO, 1);
    CHECK(!apps_task_compile(p, err, sizeof err) && strcmp(err, HUNG_MESSAGE) == 0, "compile: '%s'", err);
    free_pkg(p);
    CHECK(!stop_app(err, sizeof err) && strcmp(err, HUNG_MESSAGE) == 0, "stop: '%s'", err);
    CHECK(count_from(asked, "log W stop: " HUNG_MESSAGE) == 1, "a refused stop says so, once");
    launch("hangy", true);
    ABSENT(asked, "queue ");
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

static char stop_caller_err[200];
static bool stop_caller_ok;

static void *stop_caller(void *arg) {
    (void)arg;
    stop_caller_ok = stop_app(stop_caller_err, sizeof stop_caller_err);
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
    clock_stop_at(t0 + 3001);
    int card = WAIT(from, "crash Slow | (stopped responding)");
    pthread_join(caller, NULL);
    CHECK(real_ms() - start < 1500, "the stop's caller returned once the app hung: %lld ms", (long long)(real_ms() - start));
    CHECK(!stop_caller_ok && strcmp(stop_caller_err, HUNG_MESSAGE) == 0, "the stop failed with the hung message: '%s'",
          stop_caller_err);
    CHECK(count_from(from, "log W stop: " HUNG_MESSAGE) == 1, "and logged it once");
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

/* C5: a launch posts and returns, even while the app task is busy in a call (the clock stopped: busy, not hung). What
 * it passes the queue is recorded and must be no wait (finish() checks it); the status posts nothing. */
static void a_launch_never_waits(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    CHECK(!apps_lv_front(), "behind");
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int from = mark();
    launch("busy", true);
    launch("nosuch", true);
    CHECK(count_from(from, "queue RUN wait 0") == 2, "both launches posted with no wait");
    apps_status_t *st = new_status();
    int asked = mark();
    apps_task_status(st);
    ABSENT(asked, "queue ");
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "busy") == 0, "the status: %s", apps_state_name(st->state));
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");
    release_print();
    clock_go();
    WAIT(from, "front 1");
    WAIT(from, "print busy show");
    WAIT(from, "log W launch nosuch: ");
    /* Nor does it wait for room in a queue full of requests: it is dropped, and the log says so. */
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int full = mark();
    for (int i = 0; i < 16; i++) {
        launch("busy", false);
    }
    ABSENT(full, "log W launch busy: ");
    launch("busy", false);
    CHECK(at_from(full, "log W launch busy: the app task is busy; try again") >= 0, "the 17th is dropped, and logged");
    release_print();
    clock_go();
    FIFO_SYNC();
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
    if (strcmp(w->text, "zero width") == 0) {
        return 4;  /* the fake lays it out 0 px wide: nothing of it is on screen */
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
    "  ui.label{text = 'zero width'}\n"
    "end\n"
    "app.strat = function() end\n"
    "return app\n";

/* Run step 7: after a pass has laid the widgets out, each visible one past the round edge is a warning, after the
 * manifest's and main.lua's. A hidden widget (the fake leaves a stale overflow on it) and one laid out 0 px wide are
 * not on screen, so they are not warned about. The text is a contract (Task 13 hands it to the developer as it is), so
 * it is compared whole. */
static void overflow_warnings(void) {
    overflow_rule = overflow_of;
    const file_t files[] = {LUA("main.lua", APP_WIDE)};
    apps_pkg_t *p = make_pkg("wide", "Wide", files, 1);
    snprintf(p->manifest.warnings, sizeof p->manifest.warnings, "manifest says hi");
    apps_status_t *st = new_status();
    char err[200];
    CHECK(apps_task_run(p, true, 50, st, err, sizeof err), "run: %s", err);
#define WIDE_WARNINGS                                                                     \
    "main.lua returned unknown handler 'strat' (did you mean 'start'?)"                   \
    "; label#0 'Time's up' extends 14 px past the round edge"                             \
    "; box#1 'box' extends 5 px past the round edge"                                      \
    "; label#4 'line one?line two' extends 3 px past the round edge"                      \
    "; label#5 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx...' extends 7 px past the round edge"
    CHECK(strcmp(st->warnings, "manifest says hi; " WIDE_WARNINGS) == 0, "warnings: '%s'", st->warnings);
    /* With no settle at all the run still waits for a pass that has laid the new widgets out. */
    CHECK(apps_task_run(make_pkg("wide", "Wide", files, 1), true, 0, st, err, sizeof err), "run: %s", err);
    CHECK(strcmp(st->warnings, WIDE_WARNINGS) == 0, "warnings with no settle: '%s'", st->warnings);
    heap_caps_free(st);
}

/* C8: the screen sleeps (the frames run the supervisor, not the apply step): a run waits at most 500 ms for
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
    CHECK(took >= 50 + 500 - 20 && took < 50 + 500 + 400, "it waited for a pass, at most 500 ms: %lld ms",
          (long long)took);
    CHECK(at_from(from, "set_ui table#1") >= 0, "published");
    from = mark();
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
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
    /* Main is compiled first, whatever the order the package holds its files in (the one order apphost's check uses too),
     * and the others as the package has them. */
    const file_t both_bad[] = {LUA("lib/util.lua", "return {\n"), LUA("main.lua", "x = = 1\n"), LUA("lib/b.lua", "y = = 2\n")};
    p = make_pkg("comp", "Comp", both_bad, 3);
    CHECK(!apps_task_compile(p, err, sizeof err) && strncmp(err, "main.lua:1:", 11) == 0, "main first: '%s'", err);
    free_pkg(p);
    const file_t modules_bad[] = {LUA("lib/util.lua", "return {\n"), LUA("main.lua", APP_HELLO), LUA("lib/b.lua", "y = = 2\n")};
    p = make_pkg("comp", "Comp", modules_bad, 3);
    CHECK(!apps_task_compile(p, err, sizeof err) && strncmp(err, "lib/util.lua:", 13) == 0, "then in the package's order: '%s'", err);
    free_pkg(p);
    /* A heap too small to make is the one message (apps_rt_compile's), with the manifest's memory_kb in it. */
    p = make_pkg("comp", "Comp", good, 2);
    p->manifest.memory_kb = 2;
    CHECK(!apps_task_compile(p, err, sizeof err) && strcmp(err, "out of memory: can't make the app's heap of 2 KB") == 0,
          "a heap of 2 KB: '%s'", err);
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

/* The app that runs is listed while it runs, here where no store lists it (F1: on the first row when the store's rows
 * don't have it); a pick fronts it. */
static void the_launcher_lists_the_running_app(void) {
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
    /* Stopped, and in no store: not listed. */
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    int stopped = mark();
    muse_apps_open_launcher();
    WAIT(stopped, "launcher 0");
}

/* A caller that gives up: its buffers are the case's, kept alive after it returns, so a late write by the app task
 * shows as changed bytes even without sanitizers (and as a use-after-free with them, once they are freed). */
static struct {
    apps_pkg_t *pkg;
    bool front;
    int settle_ms;
    char *err;
    apps_status_t *st;
    bool ok;
} late;

static void *late_caller(void *arg) {
    (void)arg;
    late.ok = apps_task_run(late.pkg, late.front, late.settle_ms, late.st, late.err, 200);
    return NULL;
}

/* An app that, once start() has returned, runs nothing until it is stopped: the clock can jump with no call under way. */
static const char APP_QUIET[] =
    "local app = {}\n"
    "function app.start() print('start') end\n"
    "return app\n";
static const file_t QUIET[] = {LUA("main.lua", APP_QUIET)};

/* Runs pkg from another thread; once wait_for is logged, moves the clock jump_ms on, which makes its caller give up, and
 * keeps a copy of the caller's buffers as it left them. The case makes pkg before it holds the UI lock, if it does. */
static void late_run_at(int line, apps_pkg_t *pkg, bool front, int settle_ms, int from, const char *wait_for, int64_t jump_ms,
                        char *err_copy, apps_status_t *st_copy) {
    late.pkg = pkg;
    late.front = front;
    late.settle_ms = settle_ms;
    pthread_t caller;
    if (pthread_create(&caller, NULL, late_caller, NULL) != 0) {
        fail_at(line, "no caller thread");
    }
    wait_event_at(line, from, wait_for, 5000);
    clock_jump(jump_ms);
    pthread_join(caller, NULL);
    memcpy(err_copy, late.err, 200);
    memcpy(st_copy, late.st, sizeof *st_copy);
}

static void a_late_answer_writes_nothing(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("one", "One", HELLO, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    late.err = malloc(200);
    late.st = new_status();
    char *err_copy = malloc(200);
    apps_status_t *st_copy = new_status();
    /* Given up before the old app has ended (the app task waits for the UI lock in its unpublish, which the case holds):
     * the run is dropped once the old app is gone (F2), and its answer writes nothing. */
    apps_pkg_t *two = make_pkg("two", "two", HELLO, 1);
    int from = mark();
    apps_port_ui_lock();
    late_run_at(__LINE__, two, true, 0, from, "ui_lock wait app", 7000 + 1000, err_copy, st_copy);
    CHECK(!late.ok && strcmp(late.err, "the app task didn't answer within 7000 ms") == 0, "the caller gave up: '%s'", late.err);
    CHECK(at_from(from, "print one stop") >= 0, "the old app's stop() ran first");
    apps_port_ui_unlock();
    WAIT(from, "free pkg two");
    FIFO_SYNC();  /* the dropped run has been answered */
    CHECK(memcmp(late.err, err_copy, 200) == 0 && memcmp(late.st, st_copy, sizeof *st_copy) == 0,
          "the app task wrote into the buffers of a caller that had given up");
    ABSENT(from, "log I launch two");
    ABSENT(from, "set_ui table#2");
    WAIT(from, "front 0");  /* the old app is gone and nothing took its place */
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_STOPPED && strcmp(st->id, "one") == 0, "the record: %s %s", apps_state_name(st->state), st->id);
    /* Given up once the new app has begun (its start() has returned and its table is up; the caller's deadline passes in
     * the settle): the run goes on, and its answer writes nothing. */
    from = mark();
    late_run_at(__LINE__, make_pkg("three", "three", QUIET, 1), false, 20000, from, "set_ui table#2", 20000 + 7000 + 1, err_copy,
                st_copy);
    CHECK(!late.ok && strcmp(late.err, "the app task didn't answer within 27000 ms") == 0, "the caller gave up: '%s'",
          late.err);
    FIFO_SYNC();  /* the settle is over and the run answered */
    CHECK(memcmp(late.err, err_copy, 200) == 0 && memcmp(late.st, st_copy, sizeof *st_copy) == 0,
          "the app task wrote into the buffers of a caller that had given up");
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && strcmp(st->id, "three") == 0, "the run went on: %s %s", apps_state_name(st->state),
          st->id);
    free(late.err);
    free(err_copy);
    heap_caps_free(late.st);
    heap_caps_free(st_copy);
    heap_caps_free(st);
}

/* Touches the queue has no room for are dropped, never waited for, and counted; they leave 2 slots for requests, so a
 * stop still gets in (F3). */
static void *stop_thread(void *arg) {
    *(bool *)arg = stop_app(NULL, 0);
    return NULL;
}

static void touches_past_a_full_queue_are_counted(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    for (int i = 0; i < 30; i++) {
        TAP(0);  /* the LVGL task's posts are recorded: finish() checks none waited */
    }
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->dropped_events == 30 - (16 - 2), "dropped: %u, not 16 (14 queued, 2 slots kept for requests)", st->dropped_events);
    int from = mark();
    bool stopped = false;
    pthread_t stopper;
    CHECK(pthread_create(&stopper, NULL, stop_thread, &stopped) == 0, "no stop thread");
    WAIT(from, "queue STOP wait");
    release_print();
    clock_go();
    pthread_join(stopper, NULL);
    CHECK(stopped, "the stop went through a queue full of touches");
    WAIT(from, "print busy stop");
    CHECK(count_from(from, "print busy busy") == 14, "the 14 queued touches ran before the stop: %d", count_from(from, "print busy busy"));
    heap_caps_free(st);
}

/* ---- Fix round 1 ---- */

/* F1: with LVGL paused (battery sleep), only an app whose table was published can have objects up, so only its package
 * is parked; an app whose table never was is freed at once, with no reset asked and no wait. At most one package is
 * parked: a table is published only once no reset is outstanding. */
static void parking_is_bounded(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("a", "A", PICTURE, 2), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "object image a/a.bin");
    pause_lvgl();
    int from = mark();
    apps_status_t *st = new_status();
    const char *const next[] = {"b", "c", "d"};
    for (size_t i = 0; i < ARRAY_LEN(next); i++) {
        CHECK(apps_task_run(make_pkg(next[i], next[i], PICTURE, 2), true, 0, st, err, sizeof err), "run %s: %s", next[i], err);
        CHECK(strcmp(st->warnings, "layout not checked: the screen was asleep") == 0, "warnings: '%s'", st->warnings);
    }
    CHECK(count_from(from, "log W the screen didn't take the app down within 500 ms") == 1,
          "only a's end waited for a reset: %d waits", count_from(from, "log W the screen didn't take the app down"));
    WAIT(from, "free pkg b");
    WAIT(from, "free pkg c");
    ABSENT(from, "free pkg a");
    ABSENT(from, "free image a/a.bin");
    ABSENT(from, "set_ui table#2");
    ABSENT(from, "reset");
    /* The screen wakes: the one reset runs, then a's package goes and d's table is published. */
    int woke = mark();
    atomic_store(&lvgl_paused, false);
    int reset = WAIT(woke, "reset");
    int freed = WAIT(woke, "free pkg a");
    int published = WAIT(woke, "set_ui table#2");
    CHECK(reset < freed && reset < published, "reset %d, free %d, publish %d", reset, freed, published);
    WAIT(woke, "object image d/a.bin");
    WAIT_FRAMES(5);
    CHECK(count_from(woke, "reset") == 1, "one reset: %d", count_from(woke, "reset"));
    heap_caps_free(st);
}

/* F2: the old app's stop() hangs while a new run replaces it. The caller hears the HUNG message; when stop() returns,
 * the run it gave up on is dropped: the new app isn't started and its package goes. */
static void a_replace_whose_stop_hangs_drops_the_run(void) {
    const file_t slow[] = {LUA("main.lua", APP_SLOW_STOP)};
    char err[200];
    CHECK(apps_task_run(make_pkg("slow", "Slow", slow, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("stopping");
    int from = mark();
    late.pkg = make_pkg("next", "Next", HELLO, 1);
    late.front = true;
    late.settle_ms = 0;
    late.err = malloc(200);
    late.st = NULL;
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, late_caller, NULL) == 0, "no caller thread");
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 3001);
    WAIT(from, "crash Slow | (stopped responding)");
    pthread_join(caller, NULL);
    CHECK(!late.ok && strcmp(late.err, HUNG_MESSAGE) == 0, "the caller: '%s'", late.err);
    int back = mark();
    clock_go();
    release_print();
    WAIT(back, "priority muse_apps 2");
    WAIT(back, "free pkg next");
    ABSENT(from, "log I launch next");
    ABSENT(from, "print next");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG && strcmp(st->id, "slow") == 0, "the record: %s %s", apps_state_name(st->state), st->id);
    free(late.err);
    heap_caps_free(st);
}

/* F5 (a): a new app that runs behind (front=false) has its table published in the frame where a voice turn ends. The
 * turn's end is decided after the frame's front requests, for the app that is current: the new app is not fronted
 * and gets no show(), so its show() and hide() still follow the layer. The gate holds the app task just after the
 * publication, where, before the fix, the table was up and its back request not yet sent. */
static void a_back_request_in_the_turns_last_frame(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("first", "First", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print first show");
    int from = mark();
    TURN(true);
    WAIT(from, "print first hide");
    gate_arm(MUTEX_NAMES, "muse_apps", from, "set_ui table#2");
    late.pkg = make_pkg("second", "Second", HELLO, 1);
    late.front = false;
    late.settle_ms = 0;
    late.err = malloc(200);
    late.st = NULL;
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, late_caller, NULL) == 0, "no caller thread");
    WAIT(from, "gate 3");
    TURN(false);
    WAIT_FRAMES(5);
    gate_open();
    pthread_join(caller, NULL);
    CHECK(late.ok, "run: %s", late.err);
    WAIT_FRAMES(5);
    ABSENT(from, "print second show");
    CHECK(!apps_lv_front(), "the new app stays behind");
    /* Fronted later, it gets its show(). */
    int later = mark();
    launch("second", true);
    WAIT(later, "front 1");
    WAIT(later, "print second show");
    free(late.err);
}

/* F5 (b): a front asked during a voice turn by an app whose table isn't up yet (LVGL was paused, so the old app's reset
 * came late) is kept past the turn's end and carried out once the table is published, not dropped. */
static void a_front_asked_during_a_turn_waits_for_the_table(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("first", "First", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print first show");
    TURN(true);
    pause_lvgl();
    int from = mark();
    CHECK(apps_task_run(make_pkg("second", "Second", BUSY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    ABSENT(from, "set_ui table#2");
    apps_port_ui_lock();  /* the publication that follows the reset waits for the case */
    atomic_store(&lvgl_paused, false);
    WAIT(from, "reset");
    WAIT(from, "ui_lock wait app");
    WAIT_FRAMES(3);  /* frames in the turn have seen the front request */
    TURN(false);
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");  /* no table yet: the front waits for it */
    apps_port_ui_unlock();
    WAIT(from, "set_ui table#2");
    WAIT(from, "front 1");
    WAIT(from, "print second show");
}

/* F6: an app crashes while LVGL is paused, and its card waits; a new app that begins before it shows drops it, so the
 * old crash card doesn't come up over the new app when the screen wakes. */
static void a_new_app_drops_an_old_crash_card(void) {
    pause_lvgl();
    const file_t crashy[] = {LUA("main.lua", APP_START_CRASH)};
    char err[300];
    int from = mark();
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crashy, 1), true, 0, NULL, err, sizeof err), "a crash isn't a run");
    CHECK(apps_task_run(make_pkg("fixed", "Fixed", BUSY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    atomic_store(&lvgl_paused, false);
    WAIT(from, "front 1");
    WAIT(from, "print fixed show");
    WAIT_FRAMES(5);
    ABSENT(from, "crash ");
}

/* F6: the supervisor's card for a hung app, held back by a voice turn, goes when a new app begins: it doesn't come up
 * over the new app at the turn's end. */
static void a_new_app_drops_an_old_hung_card(void) {
    const file_t files[] = {LUA("main.lua", APP_HANG)};
    char err[200];
    CHECK(apps_task_run(make_pkg("hangy", "Hangy", files, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    TURN(true);
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("hang");
    int from = mark();
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 3001);
    WAIT(from, "priority muse_apps 1");
    WAIT_FRAMES(3);
    ABSENT(from, "crash ");  /* the turn holds the card back */
    clock_go();
    release_print();
    WAIT(from, "priority muse_apps 2");  /* the call came back, and the hung app is gone */
    CHECK(apps_task_run(make_pkg("fixed", "Fixed", BUSY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    TURN(false);
    WAIT(from, "print fixed show");
    WAIT_FRAMES(5);
    ABSENT(from, "crash ");
}

/* F9: cpu_pct is the share of the last whole second spent in calls: it falls to 0 when the app idles, and a call across
 * a second's end counts in both seconds. worst_dispatch_ms is the longest call. On the stoppable clock. */
static void cpu_pct_follows_idle_time(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    const int64_t t0 = (apps_port_mono_ms() / 1000 + 3) * 1000;  /* a whole second, ahead */
    apps_status_t *st = new_status();
    clock_stop_at(t0);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 400);
    release_print();
    FIFO_SYNC();  /* the 400 ms call is over */
    clock_stop_at(t0 + 1001);
    FIFO_SYNC();  /* its second has closed */
    apps_task_status(st);
    CHECK(st->cpu_pct == 40 && st->worst_dispatch_ms == 400, "a 400 ms call: cpu %u%%, worst %u ms", st->cpu_pct,
          st->worst_dispatch_ms);
    clock_stop_at(t0 + 3000);
    FIFO_SYNC();
    apps_task_status(st);
    CHECK(st->cpu_pct == 0 && st->worst_dispatch_ms == 400, "idle: cpu %u%%, worst %u ms", st->cpu_pct, st->worst_dispatch_ms);
    clock_stop_at(t0 + 3900);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 4300);
    release_print();
    FIFO_SYNC();
    apps_task_status(st);
    CHECK(st->cpu_pct == 10, "100 ms of the second that closed during the call: cpu %u%%", st->cpu_pct);
    clock_stop_at(t0 + 5400);
    FIFO_SYNC();
    apps_task_status(st);
    CHECK(st->cpu_pct == 30 && st->worst_dispatch_ms == 400, "300 ms of the next: cpu %u%%, worst %u ms", st->cpu_pct,
          st->worst_dispatch_ms);
    clock_go();
    heap_caps_free(st);
}

/* F10: a status read while touches are dropped and a new app begins never reports a count from before the app's base.
 * The gate holds the status call at the record's lock while the touches are dropped and the new app begins. */
static apps_status_t *gated_st;

static void *gated_status(void *arg) {
    (void)arg;
    gated_thread = true;
    apps_task_status(gated_st);
    return NULL;
}

static void dropped_is_read_with_its_base(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int from = mark();
    gated_st = new_status();
    gate_arm(MUTEX_RECORD, NULL, from, NULL);
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, gated_status, NULL) == 0, "no reader thread");
    WAIT(from, "gate 1");
    for (int i = 0; i < 5; i++) {
        TAP(0);
    }
    late.pkg = make_pkg("next", "Next", HELLO, 1);
    late.front = false;
    late.settle_ms = 0;
    late.err = malloc(200);
    late.st = NULL;
    pthread_t runner;
    CHECK(pthread_create(&runner, NULL, late_caller, NULL) == 0, "no runner thread");
    WAIT(from, "queue RUN wait");
    for (int i = 0; i < 15; i++) {
        TAP(0);  /* the queue fills, and the rest are dropped */
    }
    release_print();
    clock_go();
    WAIT(from, "log I launch next");
    gate_open();
    pthread_join(reader, NULL);
    pthread_join(runner, NULL);
    CHECK(late.ok, "run: %s", late.err);
    CHECK(strcmp(gated_st->id, "next") == 0 && gated_st->dropped_events == 0, "%s: %u dropped since it began", gated_st->id,
          gated_st->dropped_events);
    free(late.err);
    heap_caps_free(gated_st);
}

/* T1 (C11): an app behind the face is asked to the front during a voice turn: nothing is fronted during the turn, and
 * the turn's end fronts it and runs its show(). */
static void a_front_asked_during_a_turn_comes_after_it(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    CHECK(!apps_lv_front(), "behind before the turn");
    TURN(true);
    int from = mark();
    launch("busy", true);
    FIFO_SYNC();      /* the launch has been handled: its front request is out */
    WAIT_FRAMES(2);   /* and the LVGL task has seen it */
    ABSENT(from, "front 1");
    ABSENT(from, "print busy show");
    TURN(false);
    WAIT(from, "front 1");
    WAIT(from, "print busy show");
}

/* T9: a crash card that comes during a voice turn waits for its end. */
static void a_card_waits_for_the_turn_to_end(void) {
    TURN(true);
    const file_t crashy[] = {LUA("main.lua", APP_START_CRASH)};
    char err[300];
    int from = mark();
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crashy, 1), true, 0, NULL, err, sizeof err), "a crash isn't a run");
    WAIT_FRAMES(5);
    ABSENT(from, "crash ");
    TURN(false);
    WAIT(from, "crash Crashy | main.lua:4: boom");
}

/* T9: the app in front when a voice turn began has ended by its end: nothing is fronted. */
static void a_turn_after_the_app_ended_fronts_nothing(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print busy show");
    TURN(true);
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    int from = mark();
    TURN(false);
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");
    ABSENT(from, "queue SHOW");
}

/* F3: apps_task_stop says when it didn't stop the app, and why, with one W line each time: no answer within 2000 ms,
 * no room in the queue within 2000 ms, no memory for the request (the hung message: the hung cases). It is true when
 * the app stopped, and when none was running. While the app task is stuck in a call that prints, the clock stands
 * still (busy, not hung) until the case moves it past the stop's 2000 ms, short of the supervisor's 3000. */
static void stop_reports_failure(void) {
    char err[200] = "x";
    CHECK(stop_app(err, sizeof err) && err[0] == '\0', "no app running: '%s'", err);
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    int from = mark();
    /* No answer within 2000 ms. */
    int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    int asked = mark();
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, stop_caller, NULL) == 0, "no caller thread");
    WAIT(asked, "queue STOP wait");
    clock_stop_at(t0 + 2001);
    pthread_join(caller, NULL);
    CHECK(!stop_caller_ok && strcmp(stop_caller_err, "the app task didn't answer within 2000 ms") == 0, "no answer: '%s'",
          stop_caller_err);
    release_print();
    clock_go();
    WAIT(asked, "print busy stop");  /* the stop still happens when the app task gets to it */
    FIFO_SYNC();
    /* No room within 2000 ms: 14 touches and 2 launches fill the queue behind a stuck call. */
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    for (int i = 0; i < 16; i++) {
        TAP(0);
    }
    launch("busy", false);
    launch("busy", false);
    asked = mark();
    CHECK(pthread_create(&caller, NULL, stop_caller, NULL) == 0, "no caller thread");
    WAIT(asked, "queue STOP wait");
    clock_stop_at(t0 + 2001);
    pthread_join(caller, NULL);
    CHECK(!stop_caller_ok && strcmp(stop_caller_err, "the app task is busy; try again") == 0, "no room: '%s'",
          stop_caller_err);
    release_print();
    clock_go();
    FIFO_SYNC();
    ABSENT(asked, "print busy stop");
    /* No memory for the request. */
    atomic_store(&caps_failures, 1);
    CHECK(!stop_app(err, sizeof err) && strcmp(err, "out of memory: no room for the request") == 0, "no memory: '%s'", err);
    CHECK(atomic_load(&caps_failures) == 0, "the stop's request was the block that failed");
    /* And a stop that stops it. */
    CHECK(stop_app(err, sizeof err) && err[0] == '\0', "stop: '%s'", err);
    WAIT(asked, "print busy stop");
    CHECK(count_from(from, "log W stop: ") == 3, "a W line for each failure: %d", count_from(from, "log W stop: "));
    WAIT(from, "log W stop: the app task didn't answer within 2000 ms");
    WAIT(from, "log W stop: the app task is busy; try again");
    WAIT(from, "log W stop: out of memory: no room for the request");
}

/* T9, F7: muse_apps_ui_tick is true while the layer is in front or a card is shown (frame_tick then draws nothing under
 * it), and muse_apps_covering says the same to any task, at once for a change the LVGL task makes. */
static void the_tick_says_when_the_apps_cover(void) {
    WAIT_FRAMES(3);
    CHECK(!atomic_load(&last_tick) && !muse_apps_covering(), "nothing covers at first");
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    WAIT_FRAMES(3);
    CHECK(atomic_load(&last_tick) && muse_apps_covering(), "the layer in front covers");
    int from = mark();
    ON_LVGL(swipe_job, NULL);
    CHECK(!muse_apps_covering(), "a right swipe uncovers at once");
    WAIT(from, "front 0");
    WAIT_FRAMES(3);
    CHECK(!atomic_load(&last_tick), "and the tick says so");
    muse_apps_open_launcher();
    WAIT(from, "launcher 1 busy:Busy");
    WAIT_FRAMES(3);
    CHECK(atomic_load(&last_tick) && muse_apps_covering() && !apps_lv_front(), "the launcher covers");
    TURN(true);
    CHECK(!muse_apps_covering(), "a voice turn closes it, and uncovers at once");
    WAIT_FRAMES(3);
    CHECK(!atomic_load(&last_tick), "and the tick says so");
    TURN(false);
    const file_t crashy[] = {LUA("main.lua", APP_START_CRASH)};
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crashy, 1), true, 0, NULL, err, sizeof err), "a crash isn't a run");
    WAIT(from, "crash Crashy | ");
    WAIT_FRAMES(3);
    CHECK(atomic_load(&last_tick) && muse_apps_covering() && !apps_lv_front(), "a crash card covers, the layer behind");
}

/* N1: a voice turn begins after a new app (run behind) has begun, while the layer is still in front from the app it
 * replaced, and ends before the new app's table is published. The layer in front then held nothing of the new app, so
 * the turn's end fronts nothing, and a frame between the publication and the new app's back request gives it no show().
 * The gates hold the app task after start() (before the publication), then between the publication and the back
 * request. */
static void a_turn_during_a_new_apps_load_fronts_nothing(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("first", "First", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print first show");
    int from = mark();
    gate_arm(MUTEX_RECORD, "muse_apps", from, "print second start");
    late.pkg = make_pkg("second", "Second", HELLO, 1);
    late.front = false;
    late.settle_ms = 0;
    late.err = malloc(200);
    late.st = NULL;
    pthread_t caller;
    CHECK(pthread_create(&caller, NULL, late_caller, NULL) == 0, "no caller thread");
    WAIT(from, "gate 1");  /* start() has run; the table isn't published */
    WAIT_FRAMES(3);        /* the LVGL task has seen the new app */
    CHECK(apps_lv_front(), "the layer is still in front, from the app that was replaced");
    TURN(true);
    CHECK(!apps_lv_front(), "a turn puts the leftover layer behind");
    TURN(false);
    WAIT_FRAMES(3);        /* the turn's end has been decided */
    apps_port_ui_lock();   /* the publication waits for the case */
    gate_open();
    WAIT(from, "ui_lock wait app");
    gate_arm(MUTEX_NAMES, "muse_apps", from, "set_ui table#2");
    apps_port_ui_unlock();
    WAIT(from, "gate 3");  /* published; the back request not sent yet */
    WAIT_FRAMES(3);
    gate_open();
    pthread_join(caller, NULL);
    CHECK(late.ok, "run: %s", late.err);
    WAIT_FRAMES(5);
    FIFO_SYNC();
    FIFO_SYNC();  /* a SHOW posted in those frames has been handled */
    ABSENT(from, "front 1");
    ABSENT(from, "print second show");
    CHECK(!apps_lv_front(), "the new app stays behind");
    free(late.err);
}

/* ---- Task 13 ---- */

/* Kills `L.front_before_turn = false;` (Task 12's last review): an app in front with its table up comes back after a
 * turn with no request. */
static void a_turn_brings_back_the_app_that_was_in_front(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print busy show");
    int from = mark();
    TURN(true);
    WAIT(from, "print busy hide");
    WAIT_FRAMES(3);
    ABSENT(from, "front 1");
    int ended = mark();
    TURN(false);
    WAIT(ended, "front 1");
    WAIT(ended, "print busy show");
}

static const char APP_MSG[] =
    "local app = {}\n"
    "local n = 0\n"
    "function app.start() ui.label{text = 'Msg'} end\n"
    "function app.show() print('show') end\n"
    "function app.hide() print('hide') end\n"
    "function app.message(event, data)\n"
    "  if event == 'add' then n = n + data.by return {n = n} end\n"
    "  if event == 'fail' then error('unknown event') end\n"
    "  if event == 'stuck' then print('stuck') return {late = true} end\n"
    "  if event == 'spin' then while true do end end\n"
    "  return nil\n"
    "end\n"
    "return app\n";
static const file_t MSG[] = {LUA("main.lua", APP_MSG)};

/* apps_task_message: the reply, the app's own errors (which leave it running), the ids, and show. */
static void a_message_gets_its_reply(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("msg", "Msg", MSG, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    int from = mark();
    char *reply = apps_task_message("msg", "add", "{\"by\":2}", false, 0, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"n\":2}") == 0 && err[0] == '\0', "the reply: %s, '%s'",
          reply != NULL ? reply : "(null)", err);
    apps_port_free(reply);
    reply = apps_task_message("msg", "add", "{\"by\":3}", false, 2000, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"n\":5}") == 0, "the app keeps its state: %s", reply != NULL ? reply : err);
    apps_port_free(reply);
    /* No event is "", no data is nil, and no reply is null. */
    reply = apps_task_message("msg", NULL, NULL, false, 0, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "null") == 0, "nil: %s", reply != NULL ? reply : err);
    apps_port_free(reply);
    /* The app's own errors fail the message, and the app runs on. */
    CHECK(apps_task_message("msg", "fail", "", false, 0, err, sizeof err) == NULL &&
              strcmp(err, "main.lua:8: unknown event") == 0,
          "the handler's error: '%s'", err);
    CHECK(apps_task_message("msg", "add", "{bad", false, 0, err, sizeof err) == NULL && strncmp(err, "data: ", 6) == 0,
          "data that isn't JSON: '%s'", err);
    CHECK(apps_task_message("other", "add", "{}", false, 0, err, sizeof err) == NULL &&
              strcmp(err, "other isn't running") == 0,
          "another id: '%s'", err);
    CHECK(apps_task_message("Not an id", "add", "{}", false, 0, err, sizeof err) == NULL &&
              strcmp(err, "not an app id") == 0,
          "not an id: '%s'", err);
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && !st->front, "running, behind: %s %d", apps_state_name(st->state), st->front);
    /* With show, a message that failed leaves the layer where it is, and one that worked brings it to the front. */
    CHECK(apps_task_message("msg", "fail", NULL, true, 0, err, sizeof err) == NULL, "fail");
    FIFO_SYNC();
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");
    reply = apps_task_message("msg", "add", "{\"by\":0}", true, 0, err, sizeof err);
    CHECK(reply != NULL, "show: %s", err);
    apps_port_free(reply);
    WAIT(from, "front 1");
    WAIT(from, "print msg show");
    heap_caps_free(st);
}

/* A caller that gives up: the reply that comes later is the app task's to free (finish() checks that nothing is held).
 * A handler that never returns is killed, and the app ends as on any crash. */
static void a_message_after_its_caller_gave_up(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("msg", "Msg", MSG, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    int from = mark();
    block_print("stuck");
    CHECK(apps_task_message("msg", "stuck", NULL, false, 300, err, sizeof err) == NULL &&
              strcmp(err, "the app task didn't answer within 300 ms") == 0,
          "the caller gave up: '%s'", err);
    WAIT_PRINT_BLOCKED();
    release_print();
    FIFO_SYNC();
    WAIT(from, "print msg stuck");
    from = mark();
    CHECK(apps_task_message("msg", "spin", NULL, false, 5000, err, sizeof err) == NULL && err[0] != '\0',
          "killed: '%s'", err);
    WAIT(from, "log I stop msg: crashed");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strcmp(st->last_error, err) == 0, "the record: %s '%s'",
          apps_state_name(st->state), st->last_error);
    CHECK(apps_task_message("msg", "add", "{\"by\":1}", false, 0, err, sizeof err) == NULL &&
              strcmp(err, "msg isn't running") == 0,
          "after the crash: '%s'", err);
    heap_caps_free(st);
}

/* F3: the app task never waits on the console inside a call. A tap's print only queues its line; the drain after the
 * call echoes it, so a console another task holds delays the echo, never the call: the callback returns at once, and
 * the line comes once the console is free. The clock stands still through the call, then jumps past the supervisor's
 * bound while the drain waits for the console: a wait inside the call would be a hang there (and past any call's wall
 * clock); outside it, nothing happens. */
static void an_echo_waits_outside_the_call(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "echo busy show");  /* show()'s print, echoed after the call */
    pthread_mutex_lock(&console_mu);  /* another task's long reply holds the console */
    int from = mark();
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    TAP(0);
    WAIT(from, "print busy busy");
    WAIT(from, "drain 1");  /* the drain waits for the console */
    clock_stop_at(t0 + APPS_TASK_HUNG_MS + 1);
    WAIT_FRAMES(10);  /* the supervisor looks, and finds no call running */
    ABSENT(from, "crash ");
    ABSENT(from, "echo busy busy");
    pthread_mutex_unlock(&console_mu);
    clock_go();
    WAIT(from, "echo busy busy");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING, "the app runs on: %s", apps_state_name(st->state));
    ABSENT(from, "crash ");
    heap_caps_free(st);
}

/* ---- Timers (Task 16) ---- */

#define THROTTLED_TEXT "throttled: using more than 40% of the CPU"
#define KILLED_TEXT "stopped: used more than 40% of the CPU for 5 s"

/* A timer's callback is a call into the app like any other: it runs on the app task's loop (which wakes for the timer, not
 * at its next round a second away), is bracketed for the supervisor and the status, and its app ends as any app ends. */
static const char APP_TIMERS[] =
    "local app = {}\n"
    "local n = 0\n"
    "function app.start()\n"
    "  timer.after(60, function() print('after') end)\n"
    "  timer.every(100, function() n = n + 1 print('tick ' .. n) end)\n"
    "  timer.after(86400000, function() print('tomorrow') end)\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";

static void timers_run_on_the_app_task(void) {
    const file_t files[] = {LUA("main.lua", APP_TIMERS)};
    apps_status_t *st = new_status();
    char err[200];
    int from = mark();
    const int64_t began = real_ms();
    CHECK(apps_task_run(make_pkg("timers", "Timers", files, 1), false, 0, st, err, sizeof err), "run: %s", err);
    WAIT(from, "print timers after");
    int64_t took = real_ms() - began;
    CHECK(took < 700, "the timer due in 60 ms ran %lld ms after the launch: the loop wakes for a timer, it doesn't wait for its round",
          (long long)took);
    WAIT(from, "print timers tick 3");
    ORDERED(from, "print timers after", "print timers tick 1");
    ORDERED(from, "print timers tick 1", "print timers tick 2");
    ABSENT(from, "print timers tomorrow");
    /* Each callback is a call, which the status counts: the lines they printed (nothing else here prints) are in its log within
     * a second (the record is refreshed at most that often while the app runs), where start() and main.lua didn't put any. */
    for (int64_t until = real_ms() + 3000; real_ms() < until;) {
        apps_task_status(st);
        if (strstr(st->log_tail, "tick 3") != NULL) {
            break;
        }
        sleep_ms(20);
    }
    CHECK(st->state == APPS_STATE_RUNNING && strstr(st->log_tail, "after") != NULL && strstr(st->log_tail, "tick 3") != NULL,
          "the status: %s '%s'", apps_state_name(st->state), st->log_tail);
    FIFO_SYNC();
    CHECK(atomic_load(&longest_receive) > 0 && atomic_load(&longest_receive) <= pdMS_TO_TICKS(1000), "the longest wait: %u ticks",
          (unsigned)atomic_load(&longest_receive));
    /* Stopping the app takes its timers with it: nothing ticks after stop(). */
    CHECK(stop_app(err, sizeof err), "stop: '%s'", err);
    int stopped = WAIT(from, "print timers stop");
    sleep_ms(300);
    CHECK(at_from(stopped, "print timers tick") < 0, "a timer ran after the app was stopped");
    heap_caps_free(st);
}

/* A callback stuck in C is a hung call whichever way it began, so the supervisor must see timers' calls: the app is
 * marked HUNG after 3 s as for a handler, and nothing of it runs again. */
static const char APP_HANG_TIMER[] =
    "local app = {}\n"
    "function app.start() timer.after(50, function() print('hang') end) end\n"
    "return app\n";

static void a_hung_timer_callback_is_abandoned(void) {
    const file_t files[] = {LUA("main.lua", APP_HANG_TIMER)};
    char err[200];
    block_print("hang");
    int from = mark();
    CHECK(apps_task_run(make_pkg("hangt", "Hangt", files, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT_PRINT_BLOCKED();
    WAIT_FRAMES(5);
    ABSENT(from, "crash ");  /* a call isn't hung until it has run for more than 3 s */
    clock_jump(APPS_TASK_HUNG_MS + 1);
    int card = WAIT(from, "crash Hangt | (stopped responding)");
    CHECK(at_from(from, "priority muse_apps 1") >= 0, "the app task drops to priority 1");
    CHECK(at_from(from, "log W Hangt stopped responding") >= 0, "the log says so");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_HUNG, "the status: %s", apps_state_name(st->state));
    int back = mark();
    release_print();
    WAIT(back, "priority muse_apps 2");
    ORDERED(back, "set_ui NULL", "reset");
    CHECK(at_from(back, "log I stop hangt: hung, peak ") >= 0, "the stop line");
    /* The callback was stuck for three seconds of the clock: its call, when it returns, is booked as three windows of 100% of
     * the CPU, and the runtime says what it counted (the throttle's warning, once: its own line, not the app's). Nothing of the
     * app's own runs. */
    CHECK(card >= 0 && count_from(back, "print hangt") == count_from(back, "print hangt " THROTTLED_TEXT),
          "nothing of the app runs again");
    heap_caps_free(st);
}

/* Timers that are due when the loop comes round again (two that were due together: one runs a round) never make it wait for
 * nothing: a task at priority 2 that went straight back to its work would never let the idle task run. */
static const char APP_TWINS[] =
    "local app = {}\n"
    "function app.start()\n"
    "  timer.after(1000, function() print('one') end)\n"
    "  timer.after(1000, function() print('two') end)\n"
    "end\n"
    "return app\n";

static void due_timers_never_make_the_loop_spin(void) {
    const file_t files[] = {LUA("main.lua", APP_TWINS)};
    char err[200];
    CHECK(apps_task_run(make_pkg("twins", "Twins", files, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    FIFO_SYNC();  /* the run has settled: nothing but a timer can ask for a wait of 0 from here */
    unsigned zeros = atomic_load(&zero_receives);
    int from = mark();
    clock_jump(1000);  /* both are due */
    FIFO_SYNC();
    WAIT(from, "print twins one");
    WAIT(from, "print twins two");
    ORDERED(from, "print twins one", "print twins two");  /* the one armed first */
    FIFO_SYNC();
    CHECK(atomic_load(&zero_receives) == zeros, "the loop asked for a wait of 0 %u times with a timer due",
          atomic_load(&zero_receives) - zeros);
    clock_go();
}

/* The loop never waits longer than a second, whatever its earliest timer: an app whose only timer is 5 s away (a day-long
 * one is the same) has the loop come round once a second, as the record's refresh, the parked package and the log's drain
 * need it to (C22; finish() checks the longest wait of every case, and this one says so too). */
static const char APP_FAR[] =
    "local app = {}\n"
    "function app.start() timer.after(5000, function() print('far') end) end\n"
    "return app\n";

static void a_lone_far_timer_never_makes_the_loop_wait_long(void) {
    const file_t files[] = {LUA("main.lua", APP_FAR)};
    char err[200];
    CHECK(apps_task_run(make_pkg("far", "Far", files, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    FIFO_SYNC();  /* the loop has been round, and waits for the timer */
    sleep_ms(50);
    CHECK(atomic_load(&longest_receive) > 0 && atomic_load(&longest_receive) <= pdMS_TO_TICKS(1000), "the longest wait: %u ticks, not a second's %u",
          (unsigned)atomic_load(&longest_receive), (unsigned)pdMS_TO_TICKS(1000));
}

/* A run that waits for its settle gets its answer then, not when the timer of the app (5 s away) is due: the loop wakes at the
 * earlier of the two. */
static void a_far_timer_does_not_delay_a_settle(void) {
    const file_t files[] = {LUA("main.lua", APP_FAR)};
    apps_status_t *st = new_status();
    char err[200];
    int64_t began = real_ms();
    CHECK(apps_task_run(make_pkg("far", "Far", files, 1), false, 300, st, err, sizeof err), "run: %s", err);
    int64_t took = real_ms() - began;
    CHECK(took >= 300 && took < 2000, "a run with a settle of 300 ms answered after %lld ms", (long long)took);
    CHECK(st->state == APPS_STATE_RUNNING, "the status: %s", apps_state_name(st->state));
    heap_caps_free(st);
}

/* A timer that is due within a tick is waited for, never spun on: a wait of 5 ms is a tick (1 at 1000 Hz, which is 5 of them;
 * 10 ms at 100 Hz, where pdMS_TO_TICKS(5) is 0 and the loop used to go straight round for those 5 ms: the idle task starves).
 * The clock stands still, and the timer is 5 ms away. */
static void a_timer_due_within_a_tick_is_waited_for_not_spun_on(void) {
    const file_t files[] = {LUA("main.lua", APP_TWINS)};
    char err[200];
    clock_stop_at(apps_port_mono_ms());  /* before the run: the timers are armed at this time, and are due exactly 1000 ms on */
    CHECK(apps_task_run(make_pkg("twins", "Twins", files, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    FIFO_SYNC();
    unsigned zeros = atomic_load(&zero_receives);
    int from = mark();
    clock_jump(995);  /* both are due in 5 ms */
    FIFO_SYNC();
    sleep_ms(300);  /* real time: the loop goes round every tick it waits */
    ABSENT(from, "print twins");
    CHECK(atomic_load(&zero_receives) == zeros, "the loop asked for a wait of 0 %u times with a timer 5 ms away",
          atomic_load(&zero_receives) - zeros);
    clock_jump(5);
    WAIT(from, "print twins one");
    WAIT(from, "print twins two");
    clock_go();
}

/* While the screen is dark no timer runs; after the wake each overdue timer runs once, and the next comes a whole period
 * after that, not the ten that were missed. The clock stands still between the case's steps, and each FIFO_SYNC makes the
 * loop come round, as the wake does within its second. */
static const char APP_TICKER[] =
    "local app = {}\n"
    "local n = 0\n"
    "function app.start() timer.every(1000, function() n = n + 1 print('tick ' .. n) end) end\n"
    "return app\n";

static void dark_pauses_the_timers_and_the_wake_runs_each_once(void) {
    const file_t files[] = {LUA("main.lua", APP_TICKER)};
    char err[200];
    CHECK(apps_task_run(make_pkg("ticker", "Ticker", files, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    clock_jump(1000);
    FIFO_SYNC();
    WAIT(from, "print ticker tick 1");
    atomic_store(&lvgl_asleep, true);  /* the screen goes dark */
    int dark = mark();
    clock_jump(10000);  /* ten periods */
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop has been round, with a timer overdue */
    ABSENT(dark, "print ticker");
    atomic_store(&lvgl_asleep, false);
    FIFO_SYNC();
    WAIT(dark, "print ticker tick 2");
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(count_from(dark, "print ticker tick") == 1, "%d ticks at the wake, one is due", count_from(dark, "print ticker tick"));
    clock_jump(999);
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(count_from(dark, "print ticker tick") == 1, "the next one comes a whole period after the wake's");
    clock_jump(1);
    FIFO_SYNC();
    WAIT(dark, "print ticker tick 3");
    clock_go();
}

/* The screen wakes and the timers that came due while it was dark run at once, not when the loop next comes round (up to a
 * second, which on the watch is the old minute on screen for that long): the LVGL task's supervisor sees the screen go from
 * dark to lit and posts a no-op event, which wakes the loop. The loop was sent into a fresh wait of a second just before the
 * wake. */
static void the_wake_runs_the_overdue_timers_at_once(void) {
    const file_t files[] = {LUA("main.lua", APP_TICKER)};
    char err[200];
    CHECK(apps_task_run(make_pkg("ticker", "Ticker", files, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    clock_jump(1000);
    FIFO_SYNC();
    WAIT(from, "print ticker tick 1");
    WAIT_FRAMES(5);
    ABSENT(from, "queue WAKE");        /* frames of a screen that is lit post nothing */
    atomic_store(&lvgl_asleep, true);  /* the screen goes dark */
    WAIT_FRAMES(3);                    /* and the supervisor has seen it */
    ABSENT(from, "queue WAKE");        /* nor does the edge into the dark */
    clock_jump(10000);                 /* the timer is overdue ten times over */
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop has been round, and now waits a second for an event: a dark screen has no timer to wait for */
    int dark = mark();
    ABSENT(dark, "queue WAKE");
    ABSENT(dark, "print ticker");
    int64_t woke = real_ms();
    atomic_store(&lvgl_asleep, false);
    WAIT(dark, "queue WAKE wait 0");  /* the supervisor's no-op event, posted with no wait */
    WAIT(dark, "print ticker tick 2");
    int64_t took = real_ms() - woke;
    CHECK(took < 400, "the overdue timer ran %lld ms after the wake: the loop waited out its second", (long long)took);
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(count_from(dark, "print ticker tick") == 1, "%d ticks at the wake, one is due", count_from(dark, "print ticker tick"));
    clock_go();
}

/* An error in a timer's callback crashes the app as one in a widget's handler does: the table comes down, the card is
 * shown after the reset, and the record has the error and its traceback. */
static const char APP_TIMER_BOOM[] =
    "local app = {}\n"
    "function app.start()\n"
    "  ui.image{src = 'a.bin'}\n"
    "  timer.after(50, function() print('boom soon') error('timer boom') end)\n"
    "end\n"
    "return app\n";

static void a_timer_error_crashes_the_app_and_shows_the_card(void) {
    const file_t files[] = {LUA("main.lua", APP_TIMER_BOOM), IMAGE("a.bin")};
    char err[200];
    int from = mark();
    CHECK(apps_task_run(make_pkg("boomer", "Boomer", files, 2), true, 0, NULL, err, sizeof err), "run: %s", err);
    int card = WAIT(from, "crash Boomer | main.lua:4: timer boom");
    ORDERED(from, "print boomer boom soon", "set_ui NULL");
    ORDERED(from, "set_ui NULL", "reset");
    ORDERED(from, "reset", "free image boomer/a.bin");
    CHECK(at_from(from, "reset") < card, "the card after the reset");
    WAIT(from, "front 0");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strncmp(st->last_error, "main.lua:4: timer boom\nstack traceback:", 39) == 0 &&
              strcmp(st->log_tail, "boom soon") == 0,
          "the record: %s '%s' '%s'", apps_state_name(st->state), st->last_error, st->log_tail);
    heap_caps_free(st);
}

/* The duty cycle, which every call counts toward (a message here, not a timer: the case fills each second of the clock it
 * holds still with three calls that burn the rate between them). The third hot second in a row is a warning in the status
 * and the log, once; the fifth ends the app with the reason, and a card. */
static const char APP_BURNER[] =
    "local app = {}\n"
    "function app.start() ui.label{text = 'Busy'} end\n"
    "function app.message(event, data) for i = 1, data do end return true end\n"
    "return app\n";

static void the_duty_cycle_warns_the_status_then_kills(void) {
    const file_t files[] = {LUA("main.lua", APP_BURNER)};
    apps_status_t *st = new_status();
    char err[300], data[32];
    int from = mark();
    CHECK(apps_task_run(make_pkg("burner", "Burner", files, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    clock_stop_at(apps_port_mono_ms());
    snprintf(data, sizeof data, "%u", APPS_DUTY_INSTR_PER_S / 3 + 5000);  /* three of them are over the rate */
    for (int window = 1; window <= 5; window++) {
        for (int call = 0; call < 3; call++) {
            char *reply = apps_task_message("burner", "burn", data, false, 0, err, sizeof err);
            CHECK(reply != NULL, "window %d, call %d: %s", window, call + 1, err);
            apps_port_free(reply);
            if (call == 0) {  /* this call has closed the window before it */
                apps_task_status(st);
                bool warned = strstr(st->warnings, THROTTLED_TEXT) != NULL;
                CHECK(warned == (window >= 4), "window %d: the warning is %s", window, warned ? "there" : "not there");
            }
        }
        clock_jump(1000);
    }
    CHECK(count_from(from, "print burner " THROTTLED_TEXT) == 1, "the log line, once: %d",
          count_from(from, "print burner " THROTTLED_TEXT));
    apps_task_status(st);
    CHECK(strcmp(st->warnings, THROTTLED_TEXT) == 0, "the status warns once: '%s'", st->warnings);
    ABSENT(from, "crash ");
    /* The sixth window's first call closes the fifth: the app is killed. The loop's own pass may have closed it first (it reads
     * the clock after the fifth call's reply, and the clock was jumped by then: nothing orders the two), and the app has crashed:
     * the call then finds it gone. Either way the card comes, and the record has the reason. */
    char *reply = apps_task_message("burner", "burn", data, false, 0, err, sizeof err);
    CHECK(reply == NULL && (strcmp(err, KILLED_TEXT) == 0 || strstr(err, "isn't running") != NULL), "the kill: '%s'", err);
    WAIT(from, "crash Burner | " KILLED_TEXT);
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_CRASHED && strcmp(st->last_error, KILLED_TEXT) == 0, "the record: %s '%s'",
          apps_state_name(st->state), st->last_error);
    clock_go();
    heap_caps_free(st);
}

/* A warning that the duty cycle raises later joins the ones the launch gave, with one "; " between (the runtime's own text starts
 * with "; " when it follows another, which the status strips before it adds its own). Here it is the loop's pass that raises
 * it, the second after the third hot window has ended with no call since (no timer is due, so the pass makes no call: the
 * status takes the warning from a pass that made none). */
static const char APP_BURNER_WARNED[] =
    "local app = {bogus = function() end}\n"
    "function app.start() ui.label{text = 'Busy'} end\n"
    "function app.message(event, data) for i = 1, data do end return true end\n"
    "return app\n";

#define LAUNCH_WARNING "main.lua returned unknown handler 'bogus'"

static void a_throttle_warning_from_a_pass_joins_the_launch_warnings(void) {
    const file_t files[] = {LUA("main.lua", APP_BURNER_WARNED)};
    apps_status_t *st = new_status();
    char err[300], data[32];
    CHECK(apps_task_run(make_pkg("burner", "Burner", files, 1), true, 0, st, err, sizeof err), "run: %s", err);
    CHECK(strcmp(st->warnings, LAUNCH_WARNING) == 0, "the launch's warning: '%s'", st->warnings);
    clock_stop_at(apps_port_mono_ms());
    snprintf(data, sizeof data, "%u", APPS_DUTY_INSTR_PER_S / 3 + 5000);  /* three of them are over the rate */
    for (int window = 1; window <= 3; window++) {
        for (int call = 0; call < 3; call++) {
            char *reply = apps_task_message("burner", "burn", data, false, 0, err, sizeof err);
            CHECK(reply != NULL, "window %d, call %d: %s", window, call + 1, err);
            apps_port_free(reply);
        }
        clock_jump(1000);
    }
    FIFO_SYNC();
    FIFO_SYNC();  /* the pass of the first has finished: nothing but it has closed the third window */
    apps_task_status(st);
    CHECK(strcmp(st->warnings, LAUNCH_WARNING "; " THROTTLED_TEXT) == 0, "the status: '%s'", st->warnings);
    clock_go();
    heap_caps_free(st);
}

/* The status's cpu_pct and the duty cycle count the same time: a call that takes 450 ms of each of three seconds (a print that
 * blocks, on the stoppable clock) is 45% of the last, and is hot, by time alone (it runs a few hundred instructions): the third
 * hot second makes a warning. */
static void cpu_pct_and_the_duty_cycle_agree(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    const int64_t t0 = (apps_port_mono_ms() / 1000 + 3) * 1000;  /* a whole second, ahead */
    apps_status_t *st = new_status();
    for (int second = 0; second < 3; second++) {
        clock_stop_at(t0 + 1000 * second);
        block_print("busy");
        TAP(0);
        WAIT_PRINT_BLOCKED();
        clock_stop_at(t0 + 1000 * second + 450);
        release_print();
        FIFO_SYNC();
        apps_task_status(st);
        CHECK(strstr(st->warnings, THROTTLED_TEXT) == NULL, "second %d: warned '%s'", second, st->warnings);
    }
    clock_stop_at(t0 + 3001);  /* the third second has closed */
    FIFO_SYNC();
    FIFO_SYNC();
    apps_task_status(st);
    CHECK(st->cpu_pct == 45, "cpu %u%%", st->cpu_pct);
    CHECK(strcmp(st->warnings, THROTTLED_TEXT) == 0, "three seconds of 45%%: the status warns once: '%s'", st->warnings);
    clock_go();
    heap_caps_free(st);
}

/* The I/O worker's jobs: "hold" waits while io_hold is set, "chain" submits "chained". */
static atomic_bool io_hold, io_held;

static void io_job(void *arg) {
    const char *label = arg;
    const struct fake_task *t = self_task();
    event("job %s on %s priority %u", label, t->name, t->priority);
    if (strcmp(label, "hold") == 0) {
        atomic_store(&io_held, true);
        while (atomic_load(&io_hold)) {
            sleep_ms(1);
        }
    }
    if (strcmp(label, "chain") == 0) {
        CHECK(apps_io_submit(io_job, "chained"), "a job submits another");
    }
    atomic_store(&io_job_end_ms, real_ms());
}

/* apps_io: one worker, made when a job comes, runs the jobs in order and goes after 2 s without one. */
static void io_runs_jobs_in_order(void) {
    int from = mark();
    CHECK(apps_io_submit(io_job, "one") && apps_io_submit(io_job, "chain") && apps_io_submit(io_job, "three"), "submit");
    WAIT(from, "job chained on muse_apps_io priority 3");
    ORDERED(from, "job one on muse_apps_io priority 3", "job chain on muse_apps_io priority 3");
    ORDERED(from, "job chain on", "job three on");
    ORDERED(from, "job three on", "job chained on");
    CHECK(count_from(from, "task muse_apps_io") == 1 &&
              at_from(from, "task muse_apps_io stack 6144 priority 3 core 0 internal") >= 0,
          "one worker: 6 KB of internal RAM, priority 3, on core 0 (F4: never in front of the app task or LVGL)");
    WAIT(from, "log I io: idle, stack 3000 of 6144 free");
    /* Measured on the worker's own thread: from its last job's end to its going (T7). */
    int64_t idle = atomic_load(&io_idle_ms) - atomic_load(&io_job_end_ms);
    CHECK(idle >= APPS_IO_IDLE_MS - 100, "it went after %lld ms without a job, not once idle for %d ms", (long long)idle,
          APPS_IO_IDLE_MS);
    WAIT(from, "task muse_apps_io deleted");
    int again = mark();
    CHECK(apps_io_submit(io_job, "four"), "submit four");
    WAIT(again, "job four on muse_apps_io");
    CHECK(count_from(again, "task muse_apps_io stack") == 1, "a new worker for it");
}

/* T3: a job submitted after the worker's 2 s wait ran out, but before it decided to go, is run by that worker: it looks
 * at the queue again under the lock the submit took. The gate holds the worker just before it takes the lock (its wait
 * has run out), the case submits, then lets it go. */
static void io_takes_a_job_that_came_as_it_timed_out(void) {
    int from = mark();
    CHECK(apps_io_submit(io_job, "one"), "submit one");
    WAIT(from, "job one on muse_apps_io");
    gate_arm(MUTEX_IO, "muse_apps_io", from, "job one on");
    WAIT(from, "gate 4");  /* its wait ran out: it is about to decide */
    CHECK(apps_io_submit(io_job, "late"), "submit late");
    gate_open();
    WAIT(from, "job late on muse_apps_io");
    CHECK(count_from(from, "task muse_apps_io stack") == 1, "the same worker ran it: %d made",
          count_from(from, "task muse_apps_io stack"));
    WAIT(from, "log I io: idle");
    ORDERED(from, "job late on", "log I io: idle");
    WAIT(from, "task muse_apps_io deleted");
    CHECK(count_from(from, "task muse_apps_io deleted") == 1, "one worker went");
}

/* Admission: internal RAM short of the worker's stack and 16 KB, or APPS_IO_JOBS jobs waiting, refuse a job, which
 * never runs. */
static void io_refuses_when_short(void) {
    int from = mark();
    atomic_store(&internal_free, APPS_IO_STACK + APPS_IO_HEADROOM - 1);
    CHECK(!apps_io_submit(io_job, "short"), "refused without the room");
    WAIT(from, "log W io: 22527 bytes of internal RAM free, 22528 needed: job refused");
    atomic_store(&internal_free, APPS_IO_STACK + APPS_IO_HEADROOM);
    CHECK(apps_io_submit(io_job, "enough"), "taken with just the room");
    WAIT(from, "job enough on muse_apps_io");
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    for (int i = 0; i < APPS_IO_JOBS; i++) {
        CHECK(apps_io_submit(io_job, "queued"), "job %d of %d waiting", i + 1, APPS_IO_JOBS);
    }
    int full = mark();
    CHECK(!apps_io_submit(io_job, "over"), "one more is refused");
    WAIT(full, "log W io: 8 jobs wait already: job refused");
    atomic_store(&io_hold, false);
    for (int64_t until = real_ms() + 5000; count_from(from, "job queued on") < APPS_IO_JOBS;) {
        CHECK(real_ms() < until, "the waiting jobs never ran");
        sleep_ms(1);
    }
    ABSENT(from, "job short");
    ABSENT(from, "job over");
    atomic_store(&internal_free, 200u * 1024);
}

/* P1: a new worker's stack, its TCB and APPS_IO_BLOCK_MARGIN must fit in the largest free internal block. With internal
 * RAM enough in all but no block that big, a job is refused, never runs, and no worker is made. A worker that runs
 * already needs no new block, so a job for it is taken. */
static void io_refuses_when_fragmented(void) {
    const size_t need = APPS_IO_STACK + sizeof(StaticTask_t) + APPS_IO_BLOCK_MARGIN;
    CHECK(need == 7268, "the board's sum, with the S3's 100-byte TCB: %zu", need);
    char want[160];
    int from = mark();
    atomic_store(&internal_largest, need - 1);
    CHECK(!apps_io_submit(io_job, "fragmented"), "refused without a block for the worker");
    snprintf(want, sizeof want, "log W io: the largest free internal block is %zu bytes, %zu needed: job refused",
             need - 1, need);
    WAIT(from, want);
    CHECK(count_from(from, "task muse_apps_io") == 0, "no worker was made");
    atomic_store(&internal_largest, need);
    CHECK(apps_io_submit(io_job, "fits"), "taken with just the block");
    WAIT(from, "job fits on muse_apps_io");
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    atomic_store(&internal_largest, 1024);
    CHECK(apps_io_submit(io_job, "behind"), "a job for the worker that runs is taken");
    atomic_store(&io_hold, false);
    WAIT(from, "job behind on muse_apps_io");
    ABSENT(from, "job fragmented");
    atomic_store(&internal_largest, 200u * 1024);
}

#ifdef HARNESS_CONSOLE
/* ---- The cases that need the console's launch job and the RAM store (HARNESS_CONSOLE) ---- */

/* The RAM store is the worker's alone: a case puts a package in it, or empties it, with a job it waits for. */
static atomic_bool store_done;

static void store_job(void *arg) {
    apps_pkg_t *pkg = arg;
    bool unchanged = false;
    char err[200];
    CHECK(apps_ram_commit(pkg, &unchanged, err, sizeof err), "the store: %s", err);
    apps_pkg_free(pkg, apps_port_free);
    apps_port_free(pkg);
    atomic_store(&store_done, true);
}

static void empty_store_job(void *arg) {
    (void)arg;
    apps_ram_entry_t e[APPS_RAM_APPS];
    int n = apps_ram_list(e, APPS_RAM_APPS);
    for (int i = 0; i < n; i++) {
        CHECK(apps_ram_remove(e[i].id), "%s wasn't in the store", e[i].id);
    }
    atomic_store(&store_done, true);
}

static void on_worker(void (*job)(void *), void *arg) {
    atomic_store(&store_done, false);
    CHECK(apps_io_submit(job, arg), "the store's job wasn't taken");
    for (int64_t until = real_ms() + 5000; !atomic_load(&store_done);) {
        CHECK(real_ms() < until, "the store's job never ran");
        sleep_ms(1);
    }
}

/* An install without run: the store keeps a copy (its job publishes the launcher's list). */
static void store(apps_pkg_t *pkg) {
    on_worker(store_job, pkg);
}

/* The launcher, opened now, shows exactly want ("launcher <n> <id>:<name> ..."). */
static void expect_launcher_at(int line, const char *want) {
    int from = mark();
    muse_apps_open_launcher();
    int i = wait_event_at(line, from, "launcher ", 5000);
    pthread_mutex_lock(&events_mu);
    bool same = strcmp(events[i], want) == 0;
    pthread_mutex_unlock(&events_mu);
    if (!same) {
        fail_at(line, "'%s' instead of '%s'", events[i], want);
    }
}
#define EXPECT_LAUNCHER(want) expect_launcher_at(__LINE__, (want))

/* busy installed and run, then stopped: in the store and not running. */
static void stopped_in_store(void) {
    char err[200];
    store(make_pkg("busy", "Busy", BUSY, 1));
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print busy show");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    WAIT(0, "log I stop busy: stopped");
}

/* P2: the launcher lists the stored app after it stopped, and the pick runs it again in front. apps_task_launch hands
 * an app that isn't running to the I/O worker (muse_apps_launch), whose job loads the store's copy and runs it through
 * apps_task_run_by_id: the launch's post (no wait) comes before that run's (a caller's, in 10 ms tries). */
static void a_pick_runs_a_stored_app_that_stopped(void) {
    stopped_in_store();
    EXPECT_LAUNCHER("launcher 1 busy:Busy");
    int picked = mark();
    ON_LVGL(pick_job, "busy");
    WAIT(picked, "log I launch busy (256 KB)");
    WAIT(picked, "front 1");
    WAIT(picked, "print busy show");
    ORDERED(picked, "queue RUN wait 0", "queue RUN wait 10");
    char err[200];
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    on_worker(empty_store_job, NULL);
}

/* P2: a launch the I/O worker can't take is dropped with one log line (apps_io_submit's), and nothing runs. */
static void a_refused_launch_logs_once(void) {
    stopped_in_store();
    int from = mark();
    atomic_store(&internal_free, APPS_IO_STACK + APPS_IO_HEADROOM - 1);
    launch("busy", true);
    WAIT(from, "log W io: 22527 bytes of internal RAM free, 22528 needed: job refused");
    WAIT_FRAMES(10);
    CHECK(count_from(from, "log W") == 1, "one log line for the refused launch, not %d", count_from(from, "log W"));
    ABSENT(from, "log I launch busy");
    atomic_store(&internal_free, 200u * 1024);
    on_worker(empty_store_job, NULL);
}

/* F1: the launcher lists the store's apps, most recent first (an install, with or without run, or a run makes an app
 * the most recent), up to its four rows; the app that runs comes first when the rows don't have it. A pick of the app
 * that runs fronts it; a pick of any other launches it through the worker. */
static void the_launcher_lists_the_store(void) {
    char err[200];
    EXPECT_LAUNCHER("launcher 0");
    store(make_pkg("hello", "Hello", HELLO, 1));
    EXPECT_LAUNCHER("launcher 1 hello:Hello");
    store(make_pkg("busy", "Busy", BUSY, 1));
    EXPECT_LAUNCHER("launcher 2 busy:Busy hello:Hello");
    /* A pick of an app that doesn't run: the worker loads it (the most recent now) and runs it in front. */
    int picked = mark();
    ON_LVGL(pick_job, "hello");
    WAIT(picked, "log I launch hello (256 KB)");
    WAIT(picked, "front 1");
    EXPECT_LAUNCHER("launcher 2 hello:Hello busy:Busy");
    /* A pick of the app that runs fronts it: nothing is launched. */
    int swiped = mark();
    ON_LVGL(swipe_job, NULL);
    WAIT(swiped, "front 0");
    int again = mark();
    ON_LVGL(pick_job, "hello");
    WAIT(again, "front 1");
    WAIT(again, "print hello show");
    ABSENT(again, "log I launch");
    /* Four installs push hello out of the four rows: while it runs, it comes first. */
    static const char *const more[][2] = {{"q1", "Q1"}, {"q2", "Q2"}, {"q3", "Q3"}, {"q4", "Q4"}};
    for (size_t i = 0; i < ARRAY_LEN(more); i++) {
        store(make_pkg(more[i][0], more[i][1], QUIET, 1));
    }
    EXPECT_LAUNCHER("launcher 4 hello:Hello q4:Q4 q3:Q3 q2:Q2");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    EXPECT_LAUNCHER("launcher 4 q4:Q4 q3:Q3 q2:Q2 q1:Q1");
    on_worker(empty_store_job, NULL);
    EXPECT_LAUNCHER("launcher 0");
}

/* T4: an app stays listed for as long as the store has it, however it ended: an install without run, a crash in start(),
 * a main.lua that gives no app (no_app), and a hang once its stuck call has come back. */
static void the_launcher_keeps_apps_that_ended(void) {
    char err[300];
    store(make_pkg("quiet", "Quiet", QUIET, 1));
    EXPECT_LAUNCHER("launcher 1 quiet:Quiet");
    const file_t crash[] = {LUA("main.lua", APP_START_CRASH)};
    store(make_pkg("crashy", "Crashy", crash, 1));
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crash, 1), true, 0, NULL, err, sizeof err), "a crash isn't a run");
    WAIT(0, "crash Crashy | main.lua:4: boom");
    EXPECT_LAUNCHER("launcher 2 crashy:Crashy quiet:Quiet");
    static const char bad[] = "local x = = 1\n";
    const file_t syntax[] = {LUA("main.lua", bad)};
    store(make_pkg("syntax", "Syntax", syntax, 1));
    CHECK(!apps_task_run(make_pkg("syntax", "Syntax", syntax, 1), true, 0, NULL, err, sizeof err), "no app to run");
    WAIT(0, "log W launch syntax: no app: ");
    EXPECT_LAUNCHER("launcher 3 syntax:Syntax crashy:Crashy quiet:Quiet");
    const file_t hang[] = {LUA("main.lua", APP_HANG)};
    store(make_pkg("hangy", "Hangy", hang, 1));
    int from = mark();
    CHECK(apps_task_run(make_pkg("hangy", "Hangy", hang, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "front 1");
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    block_print("hang");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 3001);
    WAIT(from, "crash Hangy | (stopped responding)");
    clock_go();
    release_print();
    WAIT(from, "priority muse_apps 2");
    WAIT(from, "log I stop hangy: hung, peak ");
    EXPECT_LAUNCHER("launcher 4 hangy:Hangy syntax:Syntax crashy:Crashy quiet:Quiet");
    on_worker(empty_store_job, NULL);
}

/* F10: a launch by id fronts the app if that id runs by the time the run happens, decided on the app task. Two launches
 * of busy reach the app task while the worker is busy, so both become jobs; the first runs busy (behind), the second
 * finds it running and fronts it: one launch, no restart. */
static void two_launches_of_one_id_run_it_once(void) {
    store(make_pkg("busy", "Busy", BUSY, 1));
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    int from = mark();
    launch("busy", false);
    launch("busy", true);
    FIFO_SYNC();  /* both launches were handled: busy ran in neither, so each became a job */
    atomic_store(&io_hold, false);
    WAIT(from, "log I launch busy (256 KB)");
    WAIT(from, "front 1");
    WAIT(from, "print busy show");
    WAIT_FRAMES(10);
    CHECK(count_from(from, "log I launch busy") == 1, "busy was launched %d times", count_from(from, "log I launch busy"));
    ABSENT(from, "log I stop busy: replaced");
    ABSENT(from, "print busy stop");
    char err[200];
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    on_worker(empty_store_job, NULL);
}
#endif

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
        {"the_launcher_lists_the_running_app", the_launcher_lists_the_running_app},
        {"a_late_answer_writes_nothing", a_late_answer_writes_nothing},
        {"touches_past_a_full_queue_are_counted", touches_past_a_full_queue_are_counted},
        {"parking_is_bounded", parking_is_bounded},
        {"a_replace_whose_stop_hangs_drops_the_run", a_replace_whose_stop_hangs_drops_the_run},
        {"a_back_request_in_the_turns_last_frame", a_back_request_in_the_turns_last_frame},
        {"a_front_asked_during_a_turn_waits_for_the_table", a_front_asked_during_a_turn_waits_for_the_table},
        {"a_new_app_drops_an_old_crash_card", a_new_app_drops_an_old_crash_card},
        {"a_new_app_drops_an_old_hung_card", a_new_app_drops_an_old_hung_card},
        {"cpu_pct_follows_idle_time", cpu_pct_follows_idle_time},
        {"dropped_is_read_with_its_base", dropped_is_read_with_its_base},
        {"a_front_asked_during_a_turn_comes_after_it", a_front_asked_during_a_turn_comes_after_it},
        {"a_card_waits_for_the_turn_to_end", a_card_waits_for_the_turn_to_end},
        {"a_turn_after_the_app_ended_fronts_nothing", a_turn_after_the_app_ended_fronts_nothing},
        {"stop_reports_failure", stop_reports_failure},
        {"the_tick_says_when_the_apps_cover", the_tick_says_when_the_apps_cover},
        {"a_turn_during_a_new_apps_load_fronts_nothing", a_turn_during_a_new_apps_load_fronts_nothing},
        {"a_turn_brings_back_the_app_that_was_in_front", a_turn_brings_back_the_app_that_was_in_front},
        {"a_message_gets_its_reply", a_message_gets_its_reply},
        {"a_message_after_its_caller_gave_up", a_message_after_its_caller_gave_up},
        {"io_runs_jobs_in_order", io_runs_jobs_in_order},
        {"io_refuses_when_short", io_refuses_when_short},
        {"io_refuses_when_fragmented", io_refuses_when_fragmented},
        {"io_takes_a_job_that_came_as_it_timed_out", io_takes_a_job_that_came_as_it_timed_out},
        {"an_echo_waits_outside_the_call", an_echo_waits_outside_the_call},
        {"timers_run_on_the_app_task", timers_run_on_the_app_task},
        {"a_lone_far_timer_never_makes_the_loop_wait_long", a_lone_far_timer_never_makes_the_loop_wait_long},
        {"a_far_timer_does_not_delay_a_settle", a_far_timer_does_not_delay_a_settle},
        {"a_timer_due_within_a_tick_is_waited_for_not_spun_on", a_timer_due_within_a_tick_is_waited_for_not_spun_on},
        {"the_wake_runs_the_overdue_timers_at_once", the_wake_runs_the_overdue_timers_at_once},
        {"a_hung_timer_callback_is_abandoned", a_hung_timer_callback_is_abandoned},
        {"due_timers_never_make_the_loop_spin", due_timers_never_make_the_loop_spin},
        {"dark_pauses_the_timers_and_the_wake_runs_each_once", dark_pauses_the_timers_and_the_wake_runs_each_once},
        {"a_timer_error_crashes_the_app_and_shows_the_card", a_timer_error_crashes_the_app_and_shows_the_card},
        {"the_duty_cycle_warns_the_status_then_kills", the_duty_cycle_warns_the_status_then_kills},
        {"a_throttle_warning_from_a_pass_joins_the_launch_warnings", a_throttle_warning_from_a_pass_joins_the_launch_warnings},
        {"cpu_pct_and_the_duty_cycle_agree", cpu_pct_and_the_duty_cycle_agree},
#ifdef HARNESS_CONSOLE
        {"a_pick_runs_a_stored_app_that_stopped", a_pick_runs_a_stored_app_that_stopped},
        {"a_refused_launch_logs_once", a_refused_launch_logs_once},
        {"the_launcher_lists_the_store", the_launcher_lists_the_store},
        {"the_launcher_keeps_apps_that_ended", the_launcher_keeps_apps_that_ended},
        {"two_launches_of_one_id_run_it_once", two_launches_of_one_id_run_it_once},
#endif
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
