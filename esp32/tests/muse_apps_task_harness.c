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
#include "apps_alarm.h"
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_store.h"
#include "apps_task.h"
#include "apps_ui.h"
#include "apps_values.h"
#include "muse_apps.h"
#include "muse_apps_task_fakes.h"
#include "muse_input.h"
#include "muse_state.h"
#include "muse_voice.h"

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

/* heap_caps_*: the app task's own blocks (its record and its requests), which must be PSRAM, but for a small internal block, which is what
 * an atomic that no read-modify-write may reach in PSRAM needs (the store's load request: counted in internal_blocks). A case can make the
 * next ones fail (out of memory). */
static atomic_int caps_failures;
static atomic_uint internal_blocks;

void *heap_caps_malloc(size_t size, uint32_t caps) {
    bool small_internal = (caps & MALLOC_CAP_INTERNAL) != 0 && size <= 64;
    if ((caps & MALLOC_CAP_SPIRAM) == 0 && !small_internal) {
        error("heap_caps_malloc of %zu bytes not in PSRAM (caps 0x%x)", size, (unsigned)caps);
    }
    if (small_internal) {
        atomic_fetch_add(&internal_blocks, 1);
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

/* What heap_caps_get_free_size says of internal RAM: a case can make it short, which apps_io_submit doesn't ask about (R1: the
 * worker's stack is in PSRAM). */
static atomic_size_t internal_free = 200u * 1024;

size_t heap_caps_get_free_size(uint32_t caps) {
    return (caps & MALLOC_CAP_INTERNAL) != 0 ? atomic_load(&internal_free) : 4u * 1024 * 1024;
}

/* And its largest free block: a case can fragment it, which apps_io_submit doesn't ask about either. */
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

/* The time of day (apps_port_epoch_ms): unknown until a case says the clock is set (wall_valid), then a fixed day plus the monotonic
 * clock, so that a case that stops that clock knows every epoch the port reads. */
#define WALL_BASE_MS 1767225600000LL  /* 2026-01-01 */
static atomic_bool wall_valid;

bool apps_port_epoch_ms(int64_t *out) {
    if (!atomic_load(&wall_valid)) {
        return false;
    }
    *out = WALL_BASE_MS + apps_port_mono_ms();
    return true;
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
/* A case can hold the next worker that says it goes in that line (after it gave the worker's lock up, before it suspends itself), until it
 * lets it go. */
static atomic_bool hold_idle_line, idle_line_released;

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
        if (atomic_exchange(&hold_idle_line, false)) {
            event("io idle line held");
            while (!atomic_load(&idle_line_released)) {
                sleep_ms(1);
            }
        }
    }
    if (fake_esp_timer_on_task()) {
        error("a log line from the esp_timer task, which serves every timer of the gadget and never waits for the console: %s", msg);
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

/* An alarm's launch is taken, and nothing is ever said of it: the app task waits, in its slot, for what it can't have. */
bool muse_apps_launch_alarm(const char *id, uint32_t upto) {
    event("launch for alarms by id %s %u", id, (unsigned)upto);
    return true;
}
#endif

/* What an alarm does besides telling the app (apps_alarm.c): the screen wakes, and a chirp is asked of the voice task. Both come
 * from the esp_timer task, and neither may wait. */
void muse_input_wake(const char *why) {
    event("wake %s", why != NULL ? why : "(null)");
}

void muse_voice_request_chirp(void) {
    event("chirp");
}

/* The alarms are apps_alarm.c's, the real one, on the fake esp_timer (tests/muse_apps_fake_esp_timer.c), whose clock is this
 * harness's: a case that moves it makes the timers come due. */
apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    apps_alarm_result_t got = apps_alarm_arm(app, name, ms, sound, wake);
    apps_values_alarm_touched(app, name);  /* as apps_port_idf.c: the name's record follows (apps_values.h) */
    return got;
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    apps_alarm_cancel(app, name);
    apps_values_alarm_touched(app, name);
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    return apps_alarm_remaining(app, name);
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    return apps_alarm_list(app, names, remaining);
}

void apps_port_sound_chirp(void) {
    muse_voice_request_chirp();
}

void apps_port_sound_stop(void) {
    apps_alarm_ring_stop();
}

/* Free PSRAM: plenty, for the RAM store's room (the store class); or, once a case sets psram_total, that total less what the apps'
 * heap holds (C1's run admission). */
static _Atomic size_t psram_total;

size_t apps_port_psram_free(void) {
    size_t total = atomic_load(&psram_total), held = held_now(&port_held);
    return total == 0 ? (size_t)64 << 20 : held < total ? total - held : 0;
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
    bool suspended, deleted;  /* under tasks_mu: the I/O worker's park (vTaskSuspend), and its end (vTaskDeleteWithCaps) */
};

static struct fake_task app_tasks[16];  /* the app task, then each I/O worker made */
static int n_app_tasks;
static _Thread_local struct fake_task thread_task;  /* what a thread of the harness is to FreeRTOS */
static _Thread_local struct fake_task *current_task;
static pthread_mutex_t tasks_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tasks_cv = PTHREAD_COND_INITIALIZER;  /* a parked task's wait for its end */
static atomic_int io_workers_running;  /* I/O workers made and not parked yet */
static atomic_bool worker_create_fails;  /* no memory for a new I/O worker: its creation fails, as xTaskCreatePinnedToCoreWithCaps does */
static _Atomic(TickType_t) longest_receive;  /* the longest wait the app task asked of xQueueReceive */
static _Atomic(TickType_t) last_receive;     /* and the last: what its loop is waiting for now */
static atomic_uint zero_receives;            /* how many times it asked for no wait at all: it never blocked then */
static atomic_uint zero_delays;              /* how many times it asked vTaskDelay for no ticks: a bare yield, a loop that spins */

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

/* The app task's, and the I/O worker's (each with its stack in PSRAM). A case can have the worker's fail (worker_create_fails): no memory for
 * its stack or its TCB. */
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps) {
    bool worker = strcmp(name, "muse_apps_io") == 0;
    if (worker && atomic_load(&worker_create_fails)) {
        event("task %s: no memory", name);
        return pdFAIL;
    }
    event("task %s stack %u priority %u core %d caps 0x%x", name, (unsigned)stack, priority, core, caps);
    if (worker) {
        atomic_fetch_add(&io_workers_running, 1);
    }
    BaseType_t made = make_task(code, name, arg, priority, created);
    if (worker && made != pdPASS) {
        atomic_fetch_sub(&io_workers_running, 1);
    }
    return made;
}

/* Only a task's own suspension (vTaskSuspend(NULL)): the I/O worker's park. Its thread waits there until another task deletes it, and
 * ends. */
void vTaskSuspend(TaskHandle_t task) {
    struct fake_task *self = current_task;
    if (task != NULL || self == NULL) {
        error("vTaskSuspend of another task, or from a thread that isn't a task");
        return;
    }
    pthread_mutex_lock(&tasks_mu);
    self->suspended = true;
    pthread_mutex_unlock(&tasks_mu);
    if (strcmp(self->name, "muse_apps_io") == 0) {
        atomic_fetch_sub(&io_workers_running, 1);
    }
    event("task %s parked", self->name);
    pthread_mutex_lock(&tasks_mu);
    while (!self->deleted) {
        pthread_cond_wait(&tasks_cv, &tasks_mu);
    }
    pthread_mutex_unlock(&tasks_mu);
    pthread_exit(NULL);
}

eTaskState eTaskGetState(TaskHandle_t task) {
    pthread_mutex_lock(&tasks_mu);
    eTaskState state = task->deleted ? eDeleted : task->suspended ? eSuspended : task == current_task ? eRunning : eReady;
    pthread_mutex_unlock(&tasks_mu);
    return state;
}

/* Another task's end, once it has suspended itself (the I/O worker that parked, deleted by the task that makes the next): never the
 * caller's own (IDF would make a task in internal RAM for that, and abort without one), never a task that runs. */
void vTaskDeleteWithCaps(TaskHandle_t task) {
    if (task == NULL || task == current_task) {
        error("vTaskDeleteWithCaps of the calling task itself");
        return;
    }
    pthread_mutex_lock(&tasks_mu);
    bool parked = task->suspended && !task->deleted;
    task->deleted = task->deleted || parked;
    pthread_cond_broadcast(&tasks_cv);
    pthread_mutex_unlock(&tasks_mu);
    if (!parked) {
        error("vTaskDeleteWithCaps of a task that hasn't suspended itself");
        return;
    }
    event("task %s deleted", task->name);
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
    if (ticks == 0 && app_task_of_thread() != NULL) {
        atomic_fetch_add(&zero_delays, 1);
    }
    sleep_ms((int)ticks_to_ms(ticks));
}

/* ---- The store of packages (apps_store.h): a fake. Its mode picks the values' backend (RAM unless a case says otherwise); with the
 * console's cases its packages are the real RAM store's (apps_install.c), as the gadget's are without an apps partition. What the app
 * task asks of it, the tick and the quarantine's marker, is counted, and a flash backend's values and alarms' records are recorded. ---- */

static atomic_int store_mode = APPS_STORAGE_RAM;
static atomic_int store_ticks, store_mounts, marker_launches, marker_stops;
static char marker_id[APPS_ID_MAX + 1];  /* the app task's: the last launch's id */
static atomic_bool flash_refuses;        /* apps_nvs is full: the flash backend fails every save */
static apps_marker_t fake_marker;

apps_storage_t apps_store_mode(void) {
    return (apps_storage_t)atomic_load(&store_mode);
}

const char *apps_storage_name(apps_storage_t mode) {
    return mode == APPS_STORAGE_FLASH ? "flash" : mode == APPS_STORAGE_RAM ? "ram" : "none";
}

void apps_store_tick(int64_t now_ms) {
    (void)now_ms;
    atomic_fetch_add(&store_ticks, 1);
}

/* The worker's first step (apps_io_set_first): counted, not an event, so that the cases that read the events of a worker's start see
 * them as they were. */
void apps_store_mount_once(void) {
    atomic_fetch_add(&store_mounts, 1);
}

apps_marker_t *apps_store_marker(void) {
    return &fake_marker;
}

void apps_marker_launch(apps_marker_t *m, const char *id) {
    CHECK(m == &fake_marker, "the store's marker");
    snprintf(marker_id, sizeof marker_id, "%s", id);
    atomic_fetch_add(&marker_launches, 1);
}

void apps_marker_clean_stop(apps_marker_t *m) {
    CHECK(m == &fake_marker, "the store's marker");
    atomic_fetch_add(&marker_stops, 1);
}

bool apps_store_save_values(const char *id, const char *json, size_t len) {
    event("flash save %s %.*s", id, (int)len, json);
    return !atomic_load(&flash_refuses);
}

char *apps_store_load_values(const char *id, size_t *len) {
    (void)id;
    char *json = apps_port_realloc(NULL, 3);
    if (json != NULL) {
        memcpy(json, "{}", 3);
    }
    *len = json != NULL ? 2 : 0;
    return json;
}

/* A sync of the records, as an event: "records <id> <n>", each name the app touched as <name>@<when>/<s|-><w|-> (armed; when: ms from the
 * worker's now), <name>@- (armed with no clock) or <name>:gone, and " all" when every other record of the app goes too. */
void apps_store_sync_alarms(const char *id, const apps_store_name_t *names, int n, bool all) {
    char text[480];
    size_t used = (size_t)snprintf(text, sizeof text, "records %s %d", id, n);
    int64_t now = WALL_BASE_MS + apps_port_mono_ms();
    for (int i = 0; i < n && used < sizeof text; i++) {
        const apps_store_name_t *a = &names[i];
        if (a->state == APPS_STORE_ARMED) {
            used += (size_t)snprintf(text + used, sizeof text - used, " %s@%lld/%c%c", a->name, (long long)(a->epoch_ms - now),
                                     a->sound ? 's' : '-', a->wake ? 'w' : '-');
        } else {
            used += (size_t)snprintf(text + used, sizeof text - used, a->state == APPS_STORE_GONE ? " %s:gone" : " %s@-", a->name);
        }
    }
    if (all && used < sizeof text) {
        snprintf(text + used, sizeof text - used, " all");
    }
    event("%s", text);
}

/* The records the mount handed over (apps_store_take_records): the case's, taken once the store's mode isn't none. */
static apps_store_record_t handed[APPS_STORE_RECORDS_MAX];
static int n_handed;
static atomic_bool handed_taken;

static void hand_over(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    apps_store_record_t *r = &handed[n_handed++];
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->epoch_ms = epoch_ms;
    r->sound = sound;
    r->wake = wake;
}

int apps_store_take_records(apps_store_record_t *out, int max) {
    if (apps_store_mode() == APPS_STORAGE_NONE || atomic_exchange(&handed_taken, true)) {
        return 0;
    }
    int n = n_handed < max ? n_handed : max;
    memcpy(out, handed, (size_t)n * sizeof *out);
    return n;
}

#ifdef HARNESS_CONSOLE
bool apps_store_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n) {
    return apps_ram_commit(pkg, unchanged, err, n);
}

static char quarantined[APPS_ID_MAX + 1];  /* an app the store quarantined: its loads are refused as apps_store.c refuses them */

bool apps_store_load(const char *id, apps_pkg_t *pkg, char *err, size_t n) {
    if (strcmp(id, quarantined) == 0) {
        memset(pkg, 0, sizeof *pkg);
        snprintf(err, n, "%s was running during two restarts in a row; reinstall it to run it again", id);
        return false;
    }
    return apps_ram_load(id, pkg, err, n);
}

bool apps_store_has(const char *id) {
    return apps_ram_has(id);
}

int apps_store_list(char ids[][APPS_ID_MAX + 1], char names[][25], int max) {
    apps_ram_entry_t e[APPS_RAM_APPS];
    int n = apps_ram_list(e, max < APPS_RAM_APPS ? max : APPS_RAM_APPS);
    for (int i = 0; i < n; i++) {
        snprintf(ids[i], APPS_ID_MAX + 1, "%s", e[i].id);
        snprintf(names[i], 25, "%s", e[i].name);
    }
    return n;
}

bool apps_store_manifest(const char *id, apps_manifest_t *m) {
    return apps_ram_manifest(id, m);
}

uint32_t apps_store_free_kb(void) {
    return 0;
}

const char *apps_store_hint(void) {
    return apps_store_mode() == APPS_STORAGE_RAM ? APPS_RAM_HINT : NULL;
}
#else
void muse_apps_console_tick(int64_t now_ms) {
    (void)now_ms;  /* no console, no stage */
}

static char removed_app[APPS_ID_MAX + 1];  /* the store has every app but this one */

bool apps_store_has(const char *id) {
    return strcmp(id, removed_app) != 0;
}
#endif

/* ---- The values (apps_port.h): the real ones. apps_values.c is compiled into this file (included, not linked, so that a case can read
 * what the worker keeps: store_peek), and runs over the real I/O worker (apps_io.c): the app task's saves and loads are ops of the worker,
 * which alone touches the map. What a case can't read off the store itself it reads off the events: what the runtime asked of the port, in the
 * order the app task asked it, and the log lines the store and the worker write. apps_port_idf.c forwards the two calls as these do. ---- */

#include "apps_values.c"

char *apps_port_store_load(const char *app, size_t *len) {
    char *json = apps_values_load(app, len);
    apps_task_call_restart();  /* as apps_port_idf.c does: what the load waited for is the gadget's, not the app's */
    event(json != NULL ? "store load %s" : "store load failed %s", app);
    return json;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    bool taken = apps_values_save(app, json, len);
    event(taken ? "store save queued %s %s" : "store save refused %s %s", app, json);
    return taken;
}

/* How many saves the port holds for the worker, which hasn't taken them (apps_values.h). The app task's alone: a case reads it between rounds. */
static int store_pending_count(void) {
    int n = 0;
    for (size_t i = 0; i < ARRAY_LEN(s_pending); i++) {
        n += s_pending[i] != NULL ? 1 : 0;
    }
    return n;
}

/* The worker has done what it was given (the ops queued before this, whichever way they were nudged, and the jobs before it): a job
 * of its own, waited for. A worker that can't be made leaves nothing to wait for. */
static atomic_bool worker_synced;

static void sync_job(void *arg) {
    (void)arg;
    atomic_store(&worker_synced, true);
}

static void worker_sync(void) {
    atomic_store(&worker_synced, false);
    if (!apps_io_submit(sync_job, NULL)) {
        return;
    }
    for (int64_t until = real_ms() + 5000; !atomic_load(&worker_synced);) {
        CHECK(real_ms() < until, "the worker never ran the sync job");
        sleep_ms(1);
    }
}

/* What the worker's map holds for app, as a copy the case frees; NULL for none. The map is the worker's, so this reads it only after the
 * worker has run what it was given (store_peek) or while a case holds the worker in a job that isn't touching it (store_peek_now). */
static char *store_peek_now(const char *app) {
    for (const store_node_t *at = s_store; at != NULL; at = at->next) {
        if (strcmp(at->app, app) == 0) {
            return strdup(at->json);
        }
    }
    return NULL;
}

static char *store_peek(const char *app) {
    worker_sync();
    return store_peek_now(app);
}

/* The end of a case: once the worker has done what it was given, the map's blocks and the saves it never took go back (finish() checks
 * that nothing of the apps' heap is held). */
static void store_forget_all(void) {
    worker_sync();
    while (s_store != NULL) {
        store_node_t *next = s_store->next;
        apps_port_free(s_store);
        s_store = next;
    }
    s_store_n = 0;
}

/* ---- sys: the battery and the network are fixed, and show, keep_awake and exit are the real app task's (apps_port_idf.c forwards
 * them as these do); the idle timer's pokes are events with the time they came ---- */

apps_battery_t apps_port_battery(void) {
    return (apps_battery_t){.pct = 64, .charging = false, .usb = true};
}

bool apps_port_wifi(char *ssid, size_t cap) {
    snprintf(ssid, cap, "harness-net");
    return true;
}

void apps_port_show(void) {
    apps_task_show();
}

void apps_port_keep_awake(bool on) {
    event("sys keep_awake %d", on ? 1 : 0);
    apps_task_keep_awake(on);
}

void apps_port_exit(void) {
    apps_task_exit();
}

void muse_state_poke(void) {
    event("poke at %lld", (long long)apps_port_mono_ms());
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
static _Atomic(TickType_t) lvgl_longest_send, launch_longest_send, timer_longest_send;
static _Thread_local bool in_launch;  /* this thread is inside apps_task_launch (the case marks it) */
static const char *const EVENT_NAMES[] = {
    [0] = "?",
    [APPS_EV_TOUCH] = "TOUCH",
    [APPS_EV_RUN] = "RUN",
    [APPS_EV_STOP] = "STOP",
    [APPS_EV_COMPILE] = "COMPILE",
    [APPS_EV_MESSAGE] = "MESSAGE",
    [APPS_EV_SHOW] = "SHOW",
    [APPS_EV_HIDE] = "HIDE",
    [APPS_EV_ALARM] = "ALARM",
    [APPS_EV_WAKE] = "WAKE",
    [APPS_EV_ALARM_LAUNCH] = "ALARM_LAUNCH",
};
_Static_assert(ARRAY_LEN(EVENT_NAMES) == APPS_EV_ALARM_LAUNCH + 1, "a name for each event");

static void note_send(const void *item, size_t item_size, TickType_t wait) {
    _Atomic(TickType_t) *longest = on_lvgl_task()            ? &lvgl_longest_send
                                   : in_launch               ? &launch_longest_send
                                   : fake_esp_timer_on_task() ? &timer_longest_send
                                                              : NULL;
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
    if (app_task_of_thread() != NULL) {
        atomic_store(&last_receive, wait);
    }
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
    int number;  /* in the order made: muse_apps_start makes the record's (1), the requests' (2), the names' (3), the
                  * I/O worker's (4), then the values' (5) */
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

enum { MUTEX_RECORD = 1, MUTEX_REQUESTS = 2, MUTEX_NAMES = 3, MUTEX_IO = 4, MUTEX_VALUES = 5 };
static atomic_int n_mutexes;
static struct fake_mutex *_Atomic mutexes_made[32];  /* by number: a case can hold one as another task would */

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)(void *)buffer->opaque;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->held = false;
    m->number = atomic_fetch_add(&n_mutexes, 1) + 1;
    if (m->number < (int)ARRAY_LEN(mutexes_made)) {
        atomic_store(&mutexes_made[m->number], m);
    }
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
    int skip;  /* takes that match and go by before the gate holds one */
    bool armed;
    unsigned releases;
} gate = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static _Thread_local bool gated_thread;

static void gate_arm_after_skipping(int number, const char *task, int from, const char *after, int skip) {
    pthread_mutex_lock(&gate.mu);
    gate.number = number;
    snprintf(gate.task, sizeof gate.task, "%s", task != NULL ? task : "");
    gate.from = from;
    snprintf(gate.after, sizeof gate.after, "%s", after != NULL ? after : "");
    gate.skip = skip;
    gate.armed = true;
    pthread_mutex_unlock(&gate.mu);
}

static void gate_arm(int number, const char *task, int from, const char *after) {
    gate_arm_after_skipping(number, task, from, after, 0);
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
    if (mine && gate.skip > 0) {
        gate.skip--;
        mine = false;
    }
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

/* The longest wait the esp_timer task asked of a lock: the alarms' callbacks take the table with a short try (they serve every timer
 * of the gadget), never with a wait that has no end. */
static _Atomic(TickType_t) timer_longest_take;
static atomic_int alarm_mutex;  /* the alarm table's lock, once a case says which it is: each take of it by the app task is an event */

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    if (fake_esp_timer_on_task() && wait > atomic_load(&timer_longest_take)) {
        atomic_store(&timer_longest_take, wait);
    }
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
    if (m->number == atomic_load(&alarm_mutex) && current_task != NULL && strcmp(current_task->name, "muse_apps") == 0) {
        event("alarm take");
    }
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

bool apps_lv_launcher_shown(void) {
    lvgl_only("apps_lv_launcher_shown");
    return lv_launcher;
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

/* app.control snapshot's LVGL side (apps_snapshot.c, which has its own harness): the supervisor offers it every frame, the dark ones
 * included, on the LVGL task. */
static atomic_uint snapshot_offers, snapshot_offers_dark;

void apps_snapshot_take_pending(void) {
    lvgl_only("apps_snapshot_take_pending");
    atomic_fetch_add(&snapshot_offers, 1);
    if (atomic_load(&lvgl_asleep)) {
        atomic_fetch_add(&snapshot_offers_dark, 1);
    }
}

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

/* The manifest's "permissions" (apps_pkg.h's APPS_PERM_*) of a package a case made. */
static apps_pkg_t *with_perms(apps_pkg_t *p, uint8_t perms) {
    p->manifest.perms = perms;
    return p;
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
/* The queue's two syncs: the round of the second began after the first's ended, its timers (and the store's tick) included. */
#define ROUNDS()      \
    do {              \
        FIFO_SYNC();  \
        FIFO_SYNC();  \
    } while (0)

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

/* What a case needs before the app task starts. The records the mount hands over are taken at the first round that finds the store
 * mounted, so a case that hands some over starts with the mount still to run. */
static void preboot(const char *name) {
    if (strcmp(name, "handed_over_records_are_armed_again_when_the_clock_comes") == 0) {
        atomic_store(&store_mode, APPS_STORAGE_NONE);
    }
}

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
    CHECK(store_pending_count() == 0, "%d saves are still kept for a worker that hasn't taken them: the case ends with none", store_pending_count());
    store_forget_all();
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
    CHECK(atomic_load(&timer_longest_send) == 0, "the esp_timer task waited %u ticks to post: an alarm's callback never waits (C22)",
          (unsigned)atomic_load(&timer_longest_send));
    CHECK(atomic_load(&timer_longest_take) <= pdMS_TO_TICKS(20), "the esp_timer task waited %u ticks for a lock: a short try, 20 ms at most",
          (unsigned)atomic_load(&timer_longest_take));
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

/* C1: a run is admitted once the app it replaces has gone: free PSRAM (here a total less what the apps' heap holds) must then hold the new
 * app's heap (memory_kb) and APPS_RAM_RESERVE, its files being in PSRAM already. Short of that the run is refused in the RAM store's words:
 * nothing of it runs, the record says why, and its package goes at once. */
static void a_run_is_admitted_once_the_app_it_replaces_has_gone(void) {
    char err[300], want[200];
    CHECK(apps_task_run(make_pkg("one", "One", PICTURE, 2), true, 100, NULL, err, sizeof err), "run one: %s", err);
    WAIT(0, "object image one/a.bin");
    size_t with_one = held_now(&port_held);
    apps_pkg_t *two = make_pkg("two", "Two", PICTURE, 2);
    size_t two_bytes = held_now(&port_held) - with_one;
    atomic_store(&psram_total, two_bytes + 256 * 1024 + APPS_RAM_RESERVE);  /* short while one runs, exactly enough once it has gone */
    int from = mark();
    CHECK(apps_task_run(two, true, 100, NULL, err, sizeof err), "two is admitted once one has gone: %s", err);
    ORDERED(from, "log I stop one: replaced, peak ", "log I launch two (256 KB)");
    WAIT(from, "object image two/a.bin");
    size_t with_two = held_now(&port_held);
    apps_pkg_t *three = make_pkg("three", "Three", PICTURE, 2);
    size_t three_bytes = held_now(&port_held) - with_two;
    atomic_store(&psram_total, three_bytes + 256 * 1024 + APPS_RAM_RESERVE - 1);
    snprintf(want, sizeof want, "the gadget is short of memory for three: %u KB free, %u KB needed; restart it or install a smaller app",
             (unsigned)((256 * 1024 + APPS_RAM_RESERVE - 1) / 1024), (unsigned)((256 * 1024 + APPS_RAM_RESERVE) / 1024));
    apps_status_t *st = new_status();
    from = mark();
    CHECK(!apps_task_run(three, true, 100, st, err, sizeof err) && strcmp(err, want) == 0, "short by a byte: '%s'", err);
    CHECK(st->state == APPS_STATE_STOPPED && strcmp(st->id, "three") == 0 && strcmp(st->last_error, want) == 0, "the record: %s %s '%s'",
          apps_state_name(st->state), st->id, st->last_error);
    CHECK(at_from(from, "log I stop two: replaced, peak ") >= 0, "two ended first");
    WAIT(from, "free pkg three");
    CHECK(count_from(from, "log W launch three: no app: the gadget is short of memory for three") == 1, "the refusal is logged once");
    ABSENT(from, "print three");
    ABSENT(from, "set_ui table#");
    heap_caps_free(st);
    atomic_store(&psram_total, 0);
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
    WAIT(later, "free pkg pic");  /* the app task's, on its round after the reset: a thread of its own, not ordered with the card */
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

/* The gen the published table's widget at slot was made for, as touch_job reads it (0 too when no table is published). */
static uint16_t published_gen(int slot) {
    uint16_t gen = 0;
    if (apps_port_ui_trylock(100)) {
        pthread_mutex_lock(&lv_mu);
        if (lv_ui != NULL) {
            gen = apps_ui_slot(lv_ui, slot)->gen;
        }
        pthread_mutex_unlock(&lv_mu);
        apps_port_ui_unlock();
    }
    return gen;
}

/* M2: while a reset is outstanding the objects on screen are an app's that has ended (the new app's come only after the reset), and one
 * of them can carry the slot and gen of a widget of the new app: a touch then is dropped, never posted with the new app's app_gen. After
 * the reset, touches reach the new app as ever. */
static void a_touch_on_an_ended_apps_object_never_reaches_the_new_app(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("one", "One", BUSY, 1), true, 50, NULL, err, sizeof err), "run one: %s", err);
    WAIT(0, "set_ui table#1");
    uint16_t gen = published_gen(0);  /* a new table's first widget has the same: two's button will match it */
    pause_lvgl();  /* one's reset can't run: its button stays on the screen */
    CHECK(apps_task_run(make_pkg("two", "Two", BUSY, 1), true, 50, NULL, err, sizeof err), "run two: %s", err);
    int from = mark();
    lv_event_cb(0, gen, APPS_UI_TAP);  /* a tap on one's button, as apps_lv's callback sends it (the paused LVGL task's stand-in) */
    ROUNDS();
    ABSENT(from, "print two busy");
    atomic_store(&lvgl_paused, false);
    WAIT(from, "set_ui table#2");
    TAP(0);
    WAIT(from, "print two busy");
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

/* Task 25 (the Task 19 carry): a snapshot's drawing that held the core during a call (apps_rt_steal_ms, which the LVGL task calls
 * when it has drawn) isn't the call's time: neither the worst call's nor the duty cycle's, whose share of the second is cpu_pct.
 * A 400 ms call of which 300 ms were a drawing is a 100 ms call. On the stoppable clock. */
static void a_snapshot_is_not_the_calls_time(void) {
    char err[200];
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    const int64_t t0 = (apps_port_mono_ms() / 1000 + 3) * 1000;  /* a whole second, ahead */
    apps_status_t *st = new_status();
    clock_stop_at(t0);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 400);
    apps_rt_steal_ms(300);  /* the drawing, while the call was held up */
    release_print();
    FIFO_SYNC();  /* the call is over */
    clock_stop_at(t0 + 1001);
    FIFO_SYNC();  /* its second has closed */
    apps_task_status(st);
    CHECK(st->cpu_pct == 10 && st->worst_dispatch_ms == 100, "a 400 ms call, 300 of them drawn: cpu %u%%, worst %u ms",
          st->cpu_pct, st->worst_dispatch_ms);
    /* A drawing between calls is no call's. */
    apps_rt_steal_ms(500);
    clock_stop_at(t0 + 1100);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 1300);
    release_print();
    FIFO_SYNC();
    clock_stop_at(t0 + 2001);
    FIFO_SYNC();
    apps_task_status(st);
    CHECK(st->cpu_pct == 20 && st->worst_dispatch_ms == 200, "a 200 ms call after a drawing: cpu %u%%, worst %u ms", st->cpu_pct,
          st->worst_dispatch_ms);
    clock_go();
    heap_caps_free(st);
}

/* Ledger 69a: the supervisor's 3000 ms leave out the drawing too, as the worst call and the duty cycle do. A drawing that ended
 * before the call is nothing to it; one longer than the call so far (it straddled the start) makes it no call yet, never a wrapped
 * one; and a call 3400 ms old with 400 of them drawn is 3000 ms: not hung, until 3001. */
static void a_snapshot_is_not_hung_time(void) {
    const file_t files[] = {LUA("main.lua", APP_HANG)};
    char err[200];
    CHECK(apps_task_run(make_pkg("hangy", "Hangy", files, 1), true, 50, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "front 1");
    const int64_t t0 = (apps_port_mono_ms() / 1000 + 3) * 1000;
    clock_stop_at(t0);
    apps_rt_steal_ms(1000);  /* before the call */
    block_print("hang");
    int from = mark();
    TAP(0);
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 100);
    apps_rt_steal_ms(400);  /* more than the call has run */
    WAIT_FRAMES(5);
    ABSENT(from, "crash ");
    clock_stop_at(t0 + 3400);
    WAIT_FRAMES(10);
    ABSENT(from, "crash ");
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING, "3400 ms with 400 drawn isn't hung: %s", apps_state_name(st->state));
    clock_stop_at(t0 + 3401);
    WAIT(from, "crash Hangy | (stopped responding)");
    int back = mark();
    clock_go();
    release_print();
    WAIT(back, "priority muse_apps 2");
    WAIT(back, "free pkg hangy");
    heap_caps_free(st);
}

/* The supervisor offers app.control's snapshot to apps_snapshot.c every frame, while the screen sleeps too (E9: the request waits for
 * the first lit frame, which apps_snapshot.c tells for itself). */
static void the_snapshot_is_offered_every_frame(void) {
    unsigned frames_before = atomic_load(&frames), offers_before = atomic_load(&snapshot_offers);
    sleep_ms(60);
    unsigned frames_seen = atomic_load(&frames) - frames_before, offers = atomic_load(&snapshot_offers) - offers_before;
    CHECK(frames_seen >= 5 && offers + 1 >= frames_seen && offers <= frames_seen + 1, "%u offers in %u frames", offers, frames_seen);
    atomic_store(&lvgl_asleep, true);
    unsigned dark_before = atomic_load(&snapshot_offers_dark);
    sleep_ms(60);
    CHECK(atomic_load(&snapshot_offers_dark) - dark_before >= 5, "offered while asleep: %u",
          atomic_load(&snapshot_offers_dark) - dark_before);
    atomic_store(&lvgl_asleep, false);
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

/* apps_task_run_for_send (app.send's launch): by id and behind, and bound by what it is given. An app of that id that runs stays as it is
 * (its state is kept); a stopped one is started, behind; and a launch the app task doesn't answer within its bound fails with the wait's
 * words in about that long (the app task is held in a call), leaving the app that runs in place. */
static void run_for_send_is_by_id_behind_and_bounded(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("msg", "Msg", MSG, 1), false, 50, NULL, err, sizeof err), "run: %s", err);
    char *reply = apps_task_message("msg", "add", "{\"by\":2}", false, 0, err, sizeof err);
    CHECK(reply != NULL, "add: %s", err);
    apps_port_free(reply);
    /* The app runs: left in place, not started again. */
    CHECK(apps_task_run_for_send(make_pkg("msg", "Msg", MSG, 1), 3000, err, sizeof err), "resident: %s", err);
    reply = apps_task_message("msg", "add", "{\"by\":3}", false, 0, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"n\":5}") == 0, "its state is kept: %s", reply != NULL ? reply : err);
    apps_port_free(reply);
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && !st->front, "running, behind: %s %d", apps_state_name(st->state), st->front);
    /* A stopped app is started, behind, with no settle. */
    CHECK(apps_task_stop(err, sizeof err), "stop: %s", err);
    CHECK(apps_task_run_for_send(make_pkg("msg", "Msg", MSG, 1), 3000, err, sizeof err), "stopped: %s", err);
    reply = apps_task_message("msg", "add", "{\"by\":1}", false, 0, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"n\":1}") == 0, "started afresh: %s", reply != NULL ? reply : err);
    apps_port_free(reply);
    apps_task_status(st);
    CHECK(st->state == APPS_STATE_RUNNING && !st->front, "behind: %d", st->front);
    /* The bound: the app task is held in a handler, the launch gives up in its bound and says so. */
    block_print("stuck");
    CHECK(apps_task_message("msg", "stuck", NULL, false, 200, err, sizeof err) == NULL, "the caller gave up");
    WAIT_PRINT_BLOCKED();
    int64_t began = apps_port_mono_ms();
    CHECK(!apps_task_run_for_send(make_pkg("msg", "Msg", MSG, 1), 300, err, sizeof err) &&
              strcmp(err, "the app task didn't answer within 300 ms") == 0,
          "a launch held up: '%s'", err);
    int64_t took = apps_port_mono_ms() - began;
    CHECK(took >= 290 && took < 1500, "in about its bound: %lld ms", (long long)took);
    release_print();
    FIFO_SYNC();
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

/* ---- Task 17: the system's alarms, told by the app task's loop ---- */

/* The app of the alarm cases: a button (whose tap prints 'busy', which a case can make block), the alarm "t" that start() arms
 * 5000 ms ahead, and a message handler that does what a case asks of the alarm and sound modules. A fired alarm prints "rang <name>". */
static const char APP_ALARMY[] =
    "local app = {}\n"
    "function app.start()\n"
    "  print('start')\n"
    "  ui.button{text = 'Busy'}:on('tap', function() print('busy') end)\n"
    "  alarm.after('t', 5000)\n"
    "end\n"
    "function app.show() print('show') end\n"
    "function app.alarm(name)\n"
    "  print('rang ' .. name)\n"
    "  if name == 'crash' then error('alarm boom') end\n"
    "end\n"
    "function app.message(event, data)\n"
    "  if event == 'stop' then sound.stop() return {} end\n"
    "  if event == 'arm' then alarm.after(data.name, data.ms, data.options) return {} end\n"
    "  return nil\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t ALARMY[] = {LUA("main.lua", APP_ALARMY)};

/* The package, with the "alarm" permission, under the id of the case (the same app twice is two ids). */
static apps_pkg_t *alarmy_as(const char *id) {
    return with_perms(make_pkg(id, "Alarmy", ALARMY, 1), APPS_PERM_ALARM);
}

static apps_pkg_t *alarmy(void) {
    return alarmy_as("alarmy");
}

/* The app is told a message (its handler arms or silences). */
static void tell_alarmy(const char *what, const char *json) {
    char err[300];
    char *reply = apps_task_message("alarmy", what, json, false, 0, err, sizeof err);
    CHECK(reply != NULL, "message %s: %s", what, err);
    apps_port_free(reply);
}

/* An alarm of the app that runs: the esp_timer task marks it, rings, wakes the screen and posts, in that order and none of it
 * waiting (finish() checks the waits), and the app task fronts the layer and calls alarm(name), after which the alarm is gone from
 * its slot. The ring goes on every 1.5 s until the app says sound.stop(). */
static void an_alarm_of_the_running_app_fronts_it_and_calls_it(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print alarmy start");
    FIFO_SYNC();
    CHECK(apps_alarm_remaining("alarmy", "t") == 5000, "armed for 5000 ms: %lld", (long long)apps_alarm_remaining("alarmy", "t"));
    clock_jump(4999);
    fake_esp_timer_sync();
    FIFO_SYNC();
    ABSENT(from, "wake");
    ABSENT(from, "chirp");
    ABSENT(from, "front 1");
    FIFO_SYNC();  /* the loop is idle now, waiting for an event: what comes next is brought by the post, not by a timeout's second */
    int fired = mark();
    int64_t began = real_ms();
    clock_jump(1);
    fake_esp_timer_sync();  /* the callback has run */
    ORDERED(fired, "chirp", "wake alarm");
    ORDERED(fired, "wake alarm", "queue ALARM wait 0");
    WAIT(fired, "log I ring alarmy/t chirp 1");  /* the esp_timer task only recorded its lines: the app task's next round prints them */
    ORDERED(fired, "queue ALARM wait 0", "log I alarm alarmy/t fired (sound 1, wake 1)");
    ORDERED(fired, "log I alarm alarmy/t fired (sound 1, wake 1)", "log I ring alarmy/t chirp 1");
    WAIT(fired, "print alarmy rang t");
    int64_t told_after = real_ms() - began;
    CHECK(told_after < 300, "told %lld ms after it fired: a post brings the loop round at once (a timeout comes 1000 ms on)", (long long)told_after);
    ORDERED(fired, "log I ring alarmy/t chirp 1", "print alarmy rang t");
    WAIT(fired, "front 1");
    CHECK(apps_alarm_remaining("alarmy", "t") == -1 && !apps_alarm_pending(), "the alarm has been told, and left its slot");
    CHECK(count_from(fired, "print alarmy rang") == 1, "told once");
    /* The ring goes on until the app stops it. */
    clock_jump(1500);
    fake_esp_timer_sync();
    WAIT(fired, "log I ring alarmy/t chirp 2");  /* the ring chirps every 1.5 s, and the app task prints it on its next round */
    int stopped = mark();
    tell_alarmy("stop", "{}");
    CHECK(at_from(stopped, "log I ring stopped (sound.stop)") >= 0, "sound.stop() ended it");
    clock_jump(10000);
    fake_esp_timer_sync();
    CHECK(count_from(fired, "chirp") == 2, "no chirp after it: %d", count_from(fired, "chirp"));
    clock_go();
}

/* The esp_timer task's post is only a nudge, and the delivery is made on every round, whatever woke it: with the callback unset (as if the
 * post had been missed) and the loop idle, the first event that comes (a compile, here) delivers the alarm. FIFO_SYNC twice: the
 * first compile is answered while its round is still on, and the second can only be taken once that round, delivery included, is over.
 * A loop that delivered only when its wait timed out, or only on a timeout or its own event, would tell it a second later. */
static void a_delivery_does_not_depend_on_the_post(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print alarmy start");
    FIFO_SYNC();
    apps_alarm_set_fired_cb(NULL);
    int fired = mark();
    int64_t began = real_ms();
    clock_jump(5000);
    fake_esp_timer_sync();
    ABSENT(fired, "queue ALARM");
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(at_from(fired, "print alarmy rang t") >= 0, "the round of an event of another kind didn't deliver it");
    CHECK(real_ms() - began < 500, "told %lld ms after it fired: well under the loop's second", (long long)(real_ms() - began));
    clock_go();
}

/* And the other way: the post missed and no event of any kind to bring the loop round, the loop's own wait (RECEIVE_MS, a second) runs out and the
 * round it makes delivers. A loop that delivered only on an event would leave the alarm in its slot for ever. */
static void a_firing_with_no_post_and_no_event_is_told_when_the_loops_second_ends(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print alarmy start");
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop is idle now: its round is over, and what wakes it next is an event or the end of its wait */
    apps_alarm_set_fired_cb(NULL);
    int fired = mark();
    int64_t began = real_ms();
    clock_jump(5000);
    fake_esp_timer_sync();
    ABSENT(fired, "queue ");  /* nothing was posted */
    WAIT(fired, "print alarmy rang t");
    CHECK(real_ms() - began < 2500, "told %lld ms after it fired: the end of the loop's second", (long long)(real_ms() - began));
    CHECK(count_from(fired, "print alarmy rang") == 1, "told once");
    clock_go();
}

/* A queue that is full loses nothing either: the esp_timer task leaves the last REQ_ROOM slots of the queue to requests (a post that would take
 * one isn't made: the queue is filled to there, and the callback's post isn't), and the alarm, which keeps its slot, is told when the loop
 * is round again. The app task is stuck in a call for as long as it takes the real clock to bring the alarm (the case's own, armed for 30 ms)
 * due: the clock isn't moved, as a call that took seconds is the duty cycle's. The queue is full when one more launch is refused, which is
 * checked, so that the case can't pass for want of a full queue, and the launch of the launcher's finds the room that is left. */
static void an_alarm_posted_to_a_full_queue_is_told(void) {
    char err[200];
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    const int room = 2;  /* REQ_ROOM, which the app task keeps to itself (apps_task.c) */
    for (int i = 0; i < APPS_QUEUE_LEN - room; i++) {
        launch("alarmy", false);
    }
    int fired = mark();
    CHECK(apps_alarm_arm("alarmy", "q", 30, true, true) == APPS_ALARM_ARMED, "arm");
    WAIT(fired, "chirp");  /* the callback has run (its lines are recorded, and printed by the app task, which is busy) */
    fake_esp_timer_sync();
    CHECK(count_from(fired, "queue ALARM") == 0, "the callback made no post into the slots that requests have: only %d are left", room);
    for (int i = 0; i < room; i++) {
        launch("alarmy", false);  /* a request finds them, and the queue is full */
    }
    int refused = mark();
    launch("alarmy", false);
    WAIT(refused, "log W launch alarmy: the app task is busy; try again");
    ABSENT(fired, "print alarmy rang");  /* the app task is stuck in its print */
    CHECK(count_from(fired, "queue ALARM") == 0, "and none was posted to a queue that is full either");
    CHECK(apps_alarm_pending(), "the alarm waits in its slot");
    release_print();
    WAIT(fired, "print alarmy rang q");
    CHECK(!apps_alarm_pending(), "told");
}

/* The posts of a flood of firings are one: while the app task is busy, 48 firings (six of each of eight apps, which is what an app's budget
 * allows) make one ALARM event, and the request that comes meanwhile (a launch of the launcher's) finds room. */
static void a_flood_of_firings_posts_one_event_and_leaves_room_for_requests(void) {
    _Static_assert(APPS_QUEUE_LEN * 3 == APPS_ALARMS_MAX * 6, "48 firings: 8 apps with 6 each, three times the queue");
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    apps_alarm_cancel("alarmy", "t");  /* start()'s: the table has eight slots, and the eight apps below want them all */
    block_print("busy");
    TAP(0);
    WAIT_PRINT_BLOCKED();
    static const char *const apps[APPS_ALARMS_MAX] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    int fired = mark();
    for (int round = 0; round < 6; round++) {
        for (int k = 0; k < APPS_ALARMS_MAX; k++) {
            CHECK(apps_alarm_arm(apps[k], "q", 1, false, false) == APPS_ALARM_ARMED, "arm %s", apps[k]);
        }
        clock_jump(1);
        fake_esp_timer_sync();
    }
    CHECK(count_from(fired, "queue ALARM wait 0") == 1, "%d ALARM events for 48 firings: one waits, and the rest are no use", count_from(fired, "queue ALARM wait 0"));
    int meanwhile = mark();
    launch("alarmy", false);
    ABSENT(meanwhile, "log W launch alarmy: the app task is busy");
    CHECK(at_from(meanwhile, "queue RUN wait 0") >= 0, "the launch is in the queue");
    for (int k = 0; k < APPS_ALARMS_MAX; k++) {
        apps_alarm_cancel_app(apps[k], "the case's end");  /* none of them is an app of the store */
    }
    release_print();
    FIFO_SYNC();
    CHECK(!apps_alarm_pending(), "nothing waits");
    /* The loop has looked since: the flag is clear, and the next firing posts again. */
    int next = mark();
    CHECK(apps_alarm_arm("alarmy", "again", 1, false, false) == APPS_ALARM_ARMED, "arm");
    clock_jump(1);
    fake_esp_timer_sync();
    CHECK(count_from(next, "queue ALARM wait 0") == 1, "a firing after the loop's look posts: %d", count_from(next, "queue ALARM wait 0"));
    WAIT(next, "print alarmy rang again");
    clock_go();
}

/* Alarms that come due together are told in the order they fired, whatever the order they were armed in. */
static void alarms_are_told_in_the_order_they_fired(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print alarmy start");
    tell_alarmy("arm", "{\"name\":\"b\",\"ms\":300}");
    tell_alarmy("arm", "{\"name\":\"a\",\"ms\":100,\"options\":{\"sound\":false}}");
    int fired = mark();
    clock_jump(5000);  /* all three of them are due */
    fake_esp_timer_sync();
    WAIT(fired, "print alarmy rang t");
    ORDERED(fired, "print alarmy rang a", "print alarmy rang b");
    ORDERED(fired, "print alarmy rang b", "print alarmy rang t");
    CHECK(count_from(fired, "print alarmy rang") == 3, "three, once each");
    CHECK(count_from(fired, "chirp") == 1, "the one without sound didn't chirp, and the last rang into the ring that went: %d chirps", count_from(fired, "chirp"));
    clock_go();
}

/* An alarm that fires during a voice turn is told at once (the app has its call), and the layer comes to the front when the turn is
 * over, as any request for the front does (C11). */
static void an_alarm_during_a_voice_turn_fronts_the_app_after_it(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    TURN(true);
    int from = mark();
    clock_jump(5000);
    fake_esp_timer_sync();
    WAIT(from, "print alarmy rang t");
    WAIT_FRAMES(5);
    ABSENT(from, "front 1");
    TURN(false);
    WAIT(from, "front 1");
    WAIT(from, "print alarmy show");
    clock_go();
}

/* An alarm that fires while the screen sleeps (LVGL paused, dark) is told at once, as the board needs it: the app task runs
 * whatever the screen does. The layer comes to the front when the screen is back. */
static void an_alarm_is_told_while_the_screen_sleeps(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    FIFO_SYNC();
    pause_lvgl();
    atomic_store(&lvgl_asleep, true);
    int from = mark();
    clock_jump(5000);
    fake_esp_timer_sync();
    WAIT(from, "wake alarm");
    WAIT(from, "print alarmy rang t");
    ABSENT(from, "front 1");  /* the LVGL task sleeps */
    atomic_store(&lvgl_paused, false);
    atomic_store(&lvgl_asleep, false);
    WAIT(from, "front 1");
    clock_go();
}

/* An alarm that another task cancels between the app task's copy of what has fired and its take of this one is not told: the take
 * says it is gone, and no one is told of what was cancelled (a ring it started ended with it). The app task's second take of the
 * table's lock after the callback's lines are printed (the copy is the first), the one that takes the alarm, is held until the case has
 * cancelled it. The table's lock is the one that arming a throwaway alarm made (and the case checks that it made exactly one), not
 * the last lock made, which would be a lock of someone else's as soon as another module made one lazily. */
static void an_alarm_cancelled_before_it_is_taken_is_not_told(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    int locks_before = atomic_load(&n_mutexes);
    CHECK(apps_alarm_arm("throwaway", "x", 1000, false, false) == APPS_ALARM_ARMED, "arm a throwaway");
    apps_alarm_cancel("throwaway", "x");
    int table_lock = atomic_load(&n_mutexes);
    CHECK(table_lock == locks_before + 1, "the first alarm made %d locks, not the table's one", table_lock - locks_before);
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print alarmy start");
    FIFO_SYNC();
    atomic_store(&alarm_mutex, table_lock);
    int from = mark();
    gate_arm_after_skipping(table_lock, "muse_apps", from, "log I ring alarmy/t chirp 1", 1);
    clock_jump(5000);
    fake_esp_timer_sync();
    char gate_text[16];
    snprintf(gate_text, sizeof gate_text, "gate %d", table_lock);
    WAIT(from, gate_text);  /* the copy was made (the first take), and the take of the alarm waits */
    ORDERED(from, "alarm take", gate_text);
    apps_alarm_cancel("alarmy", "t");
    gate_open();
    FIFO_SYNC();
    ABSENT(from, "print alarmy rang");
    CHECK(!apps_alarm_pending() && apps_alarm_remaining("alarmy", "t") == -1, "the alarm is gone");
    CHECK(at_from(from, "log I ring stopped (alarm cancelled)") >= 0, "and the ring it started with it");
    clock_go();
}

/* The app crashes in alarm(): its card comes as for any crash, and the ring ends, as no one is left to stop it. The error line is
 * main.lua's. */
static void a_crash_in_alarm_ends_the_ring(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    tell_alarmy("arm", "{\"name\":\"crash\",\"ms\":100}");
    int from = mark();
    clock_jump(100);
    fake_esp_timer_sync();
    WAIT(from, "crash Alarmy | main.lua:10: alarm boom");
    WAIT(from, "log I ring stopped (app crashed)");  /* the app that was told it is gone, and nothing else waits */
    int chirps = count_from(from, "chirp");
    clock_jump(4000);  /* two ticks of the ring, were it going; the app's own alarm, at 5000, is not due yet */
    fake_esp_timer_sync();
    CHECK(count_from(from, "chirp") == chirps && chirps == 1, "the ring ended with the app: %d chirps", count_from(from, "chirp"));
    clock_go();
}

/* D: a run for alarms asks apps_alarm_wanted before it replaces the app that runs: with no firing of its app left (they were told, cancelled or
 * given up on while the worker loaded it) it is refused, with the words that are no failure to log, and the app that runs goes on. */
static void a_run_for_alarms_that_are_gone_replaces_nothing(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    int from = mark();
    CHECK(!apps_task_run_for_alarm(make_pkg("alarmy", "Alarmy", ALARMY, 1), 12345, NULL, err, sizeof err), "no firing waits for it");
    CHECK(strcmp(err, APPS_TASK_ALARM_GONE) == 0, "it says '%s'", err);
    ABSENT(from, "log I stop busy");
    ABSENT(from, "log I launch");
    ABSENT(from, "print alarmy");
    char id[APPS_ID_MAX + 1];
    CHECK(apps_task_running(id, sizeof id) && strcmp(id, "busy") == 0, "busy goes on running: '%s'", id);
    clock_go();
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

/* The worker is held by a long job (a commit that runs, an install: the worker's jobs can take longer than any bound the alarm could put on
 * its own launch, which queues behind them): the case's "hold". Returns once the worker is in it. */
static void hold_the_worker(void) {
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
}

/* No worker can be made (no memory for its stack or its TCB: worker_create_fails), and none runs: each one that ran has parked. Every submit
 * is refused, with NO_TASK_LINE, until the case says otherwise. */
#define NO_TASK_LINE "log W io: no memory for the worker: job refused"

static void no_worker_can_be_made(void) {
    atomic_store(&worker_create_fails, true);
    for (int64_t until = real_ms() + 5000; atomic_load(&io_workers_running) > 0;) {
        CHECK(real_ms() < until, "a worker never parked");
        sleep_ms(5);
    }
}

/* apps_io: one worker, made when a job comes, runs the jobs in order and parks after 2 s without one; the next job's submit deletes it before
 * it makes the next worker. */
static void io_runs_jobs_in_order(void) {
    int from = mark();
    CHECK(apps_io_submit(io_job, "one") && apps_io_submit(io_job, "chain") && apps_io_submit(io_job, "three"), "submit");
    WAIT(from, "job chained on muse_apps_io priority 3");
    ORDERED(from, "job one on muse_apps_io priority 3", "job chain on muse_apps_io priority 3");
    ORDERED(from, "job chain on", "job three on");
    ORDERED(from, "job three on", "job chained on");
    CHECK(count_from(from, "task muse_apps_io") == 1 &&
              at_from(from, "task muse_apps_io stack 8192 priority 3 core 0 caps 0x404") >= 0,
          "one worker: 8 KB of PSRAM for its stack (R1), priority 3, on core 0 (F4: never in front of the app task or LVGL)");
    WAIT(from, "log I io: idle, stack 3000 of 8192 free");
    /* Measured on the worker's own thread: from its last job's end to its going (T7). */
    int64_t idle = atomic_load(&io_idle_ms) - atomic_load(&io_job_end_ms);
    CHECK(idle >= APPS_IO_IDLE_MS - 100, "it went after %lld ms without a job, not once idle for %d ms", (long long)idle,
          APPS_IO_IDLE_MS);
    WAIT(from, "task muse_apps_io parked");
    CHECK(count_from(from, "task muse_apps_io deleted") == 0, "parked, not deleted: nothing has deleted it yet");
    int again = mark();
    CHECK(apps_io_submit(io_job, "four"), "submit four");
    WAIT(again, "job four on muse_apps_io");
    CHECK(count_from(again, "task muse_apps_io stack") == 1, "a new worker for it");
    ORDERED(again, "task muse_apps_io deleted", "task muse_apps_io stack");  /* the parked one's stack and TCB go first */
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
    WAIT(from, "task muse_apps_io parked");
    CHECK(count_from(from, "task muse_apps_io parked") == 1, "one worker went");
}

/* Admission: with no worker and none that can be made (no memory for its stack in PSRAM or its TCB), or with APPS_IO_JOBS jobs waiting, a job
 * is refused, never runs, and one line says why. */
static void io_refuses_when_full_or_unmade(void) {
    int from = mark();
    atomic_store(&worker_create_fails, true);
    CHECK(!apps_io_submit(io_job, "unmade"), "refused when no worker can be made");
    WAIT(from, NO_TASK_LINE);
    atomic_store(&worker_create_fails, false);
    CHECK(apps_io_submit(io_job, "made"), "taken once one can be made");
    WAIT(from, "job made on muse_apps_io");
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
    ABSENT(from, "job unmade");
    ABSENT(from, "job over");
}

/* A worker on its way to park (it has said it goes, and may hold the log's lock as it does, but hasn't suspended itself) is never deleted: a
 * submit then makes a new worker beside it, and the new one deletes it once it has suspended itself, as the new one goes idle in turn. (The
 * fake's vTaskDeleteWithCaps fails the case for a task that hasn't suspended itself.) */
static void a_worker_on_its_way_to_park_is_never_deleted(void) {
    atomic_store(&idle_line_released, false);
    atomic_store(&hold_idle_line, true);
    int from = mark();
    CHECK(apps_io_submit(io_job, "one"), "a worker, made by a job");
    WAIT(from, "job one on muse_apps_io");
    WAIT(from, "io idle line held");  /* it has said it goes: parked in name, not suspended */
    int again = mark();
    CHECK(apps_io_submit(io_job, "two"), "a job while it is on its way");
    WAIT(again, "job two on muse_apps_io");
    CHECK(count_from(again, "task muse_apps_io stack") == 1 && count_from(again, "task muse_apps_io deleted") == 0,
          "a new worker beside it, the first not deleted: %d made, %d deleted", count_from(again, "task muse_apps_io stack"),
          count_from(again, "task muse_apps_io deleted"));
    atomic_store(&idle_line_released, true);
    int first = wait_event_at(__LINE__, again, "task muse_apps_io parked", 5000);  /* the first suspends itself */
    int deleted = wait_event_at(__LINE__, again, "task muse_apps_io deleted", 5000);  /* the second, idle 2 s on, deletes it */
    int second = wait_event_at(__LINE__, first + 1, "task muse_apps_io parked", 5000);  /* and parks */
    CHECK(first < deleted && deleted < second, "parked, deleted, parked: %d, %d, %d", first, deleted, second);
    CHECK(count_from(again, "task muse_apps_io deleted") == 1, "one deleted: %d", count_from(again, "task muse_apps_io deleted"));
}

/* R1: the worker's stack is in PSRAM (8 KB, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT: 0x404), so internal RAM decides nothing: with 1 KB of it
 * free, in a block of 1 KB, a job is taken and a worker made for it. Its end takes none either: it parks, and the next submit deletes it (IDF's
 * vTaskDeleteWithCaps, from another task: no cleanup task in internal RAM) before it makes the next worker. */
static void io_takes_jobs_whatever_internal_ram_says(void) {
    atomic_store(&internal_free, 1024);
    atomic_store(&internal_largest, 1024);
    int from = mark();
    CHECK(apps_io_submit(io_job, "low"), "taken with 1 KB of internal RAM free");
    WAIT(from, "job low on muse_apps_io");
    CHECK(at_from(from, "task muse_apps_io stack 8192 priority 3 core 0 caps 0x404") >= 0, "a worker with its 8 KB stack in PSRAM");
    WAIT(from, "task muse_apps_io parked");
    int again = mark();
    CHECK(apps_io_submit(io_job, "next"), "the next job, with 1 KB free still");
    WAIT(again, "job next on muse_apps_io");
    ORDERED(again, "task muse_apps_io deleted", "task muse_apps_io stack 8192 priority 3 core 0 caps 0x404");
    CHECK(count_from(again, "task muse_apps_io deleted") == 1 && count_from(again, "task muse_apps_io stack") == 1,
          "the parked worker deleted, then one made: %d deleted, %d made", count_from(again, "task muse_apps_io deleted"),
          count_from(again, "task muse_apps_io stack"));
    ABSENT(from, "log W io:");
    atomic_store(&internal_free, 200u * 1024);
    atomic_store(&internal_largest, 200u * 1024);
}

/* ---- Task 18: sys and the store, on the app task ---- */

/* The app's own message, said and answered (the reply is dropped). */
static void say(const char *id, const char *event, const char *json) {
    char err[300];
    char *reply = apps_task_message(id, event, json, false, 5000, err, sizeof err);
    CHECK(reply != NULL, "message %s to %s: %s", event, id, err);
    apps_port_free(reply);
}

/* sys.exit() (C10): the callback that called it runs to its end and its reply is given; then stop(), and nothing else of the app. */
static const char APP_EXITY[] =
    "local app = {}\n"
    "function app.start() print('start') end\n"
    "function app.message(event, data)\n"
    "  if event == 'quit' then\n"
    "    print('quit begins')\n"
    "    sys.exit()\n"
    "    print('quit ends')\n"
    "    return {bye = true}\n"
    "  end\n"
    "  if event == 'timers' then\n"
    "    timer.after(50, function() print('timer one') sys.exit() print('timer one ends') end)\n"
    "    timer.after(50, function() print('timer two') end)\n"
    "    return {}\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t EXITY[] = {LUA("main.lua", APP_EXITY)};

static void sys_exit_ends_the_app_after_the_callback(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("exity", "Exity", EXITY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    int from = mark();
    char *reply = apps_task_message("exity", "quit", NULL, false, 5000, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"bye\":true}") == 0, "the callback runs to its end and its reply is given: %s",
          reply != NULL ? reply : err);
    apps_port_free(reply);
    WAIT(from, "log I stop exity: exited");
    ORDERED(from, "print exity quit begins", "print exity quit ends");
    ORDERED(from, "print exity quit ends", "print exity stop");
    ORDERED(from, "print exity stop", "log I stop exity: exited");
    WAIT(from, "front 0");  /* the layer goes behind, as with any end */
    char id[APPS_ID_MAX + 1];
    CHECK(!apps_task_running(id, sizeof id), "nothing runs now: '%s'", id);
    CHECK(apps_task_message("exity", "quit", NULL, false, 0, err, sizeof err) == NULL && strcmp(err, "exity isn't running") == 0,
          "no app to tell: '%s'", err);
    CHECK(count_from(from, "print exity quit begins") == 1 && count_from(from, "print exity stop") == 1, "nothing ran twice");
}

/* Two timers are due together and the first exits: the second never runs (one timer a round, and the app is gone after the first). */
static void sys_exit_in_a_timer_runs_no_other_callback(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(make_pkg("exity", "Exity", EXITY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    say("exity", "timers", NULL);
    int from = mark();
    clock_jump(50);
    WAIT(from, "print exity timer one ends");
    WAIT(from, "log I stop exity: exited");
    ORDERED(from, "print exity timer one ends", "print exity stop");
    FIFO_SYNC();
    clock_jump(1000);
    FIFO_SYNC();
    ABSENT(from, "print exity timer two");
    clock_go();
}

/* sys.exit() before the run has settled, at main.lua's top level or in start(): the run fails with a message that says so (not "ok" with
 * a layout warning about a sleeping screen), the app is never started at the top level, and stop() runs after a start() that returned. */
static const char APP_EXIT_START[] =
    "local app = {}\n"
    "function app.start() print('start') sys.exit() print('start ends') end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const char APP_EXIT_TOP[] =
    "print('top')\n"
    "sys.exit()\n"
    "print('top ends')\n"
    "local app = {}\n"
    "function app.start() print('start') end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";

static void sys_exit_before_the_run_settles_fails_the_run(void) {
    char err[300];
    apps_status_t *st = new_status();
    const file_t in_start[] = {LUA("main.lua", APP_EXIT_START)};
    int from = mark();
    CHECK(!apps_task_run(make_pkg("quits", "Quits", in_start, 1), true, 150, st, err, sizeof err), "a run whose app exited didn't succeed");
    CHECK(strcmp(err, "the app exited (sys.exit)") == 0, "it says why: '%s'", err);
    CHECK(strstr(st->warnings, "layout not checked") == NULL, "no layout warning about a screen that wasn't asleep: '%s'", st->warnings);
    ORDERED(from, "print quits start", "print quits start ends");
    ORDERED(from, "print quits start ends", "print quits stop");
    WAIT(from, "log I stop quits: exited");
    const file_t at_top[] = {LUA("main.lua", APP_EXIT_TOP)};
    from = mark();
    CHECK(!apps_task_run(make_pkg("quitstop", "Quitstop", at_top, 1), true, 150, st, err, sizeof err), "a run whose main.lua exited didn't succeed");
    CHECK(strcmp(err, "the app exited (sys.exit)") == 0, "it says why: '%s'", err);
    WAIT(from, "print quitstop top ends");
    WAIT(from, "log I stop quitstop: exited");
    ABSENT(from, "print quitstop start");
    ABSENT(from, "print quitstop stop");  /* never started: nothing to stop */
    char id[APPS_ID_MAX + 1];
    CHECK(!apps_task_running(id, sizeof id), "nothing runs: '%s'", id);
    heap_caps_free(st);
}

/* sys.keep_awake and sys.show. */
static const char APP_AWAKE[] =
    "local app = {}\n"
    "function app.start() ui.label{text = 'Awake'} end\n"
    "function app.message(event, data)\n"
    "  if event == 'keep' then sys.keep_awake(data.on) return {} end\n"
    "  if event == 'show' then sys.show() return {} end\n"
    "  return nil\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t AWAKE[] = {LUA("main.lua", APP_AWAKE)};

/* While the app keeps the screen awake and its layer is in front, the loop pokes the idle timer, and no oftener than once a second: from the
 * first round at once, then each second of the clock. Not while the layer is behind (a voice turn), not once the app says false, and not
 * after the app ends, which clears it. */
static void keep_awake_pokes_the_idle_timer_while_in_front(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(with_perms(make_pkg("awake", "Awake", AWAKE, 1), APPS_PERM_KEEP_AWAKE), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "front 1");
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 0, "no poke before the app asks");
    say("awake", "keep", "{\"on\":true}");
    FIFO_SYNC();
    WAIT(from, "poke at");
    ORDERED(from, "sys keep_awake 1", "poke at");
    CHECK(count_from(from, "poke at") == 1, "one poke, at the first round");
    for (int i = 0; i < 3; i++) {
        FIFO_SYNC();
    }
    CHECK(count_from(from, "poke at") == 1, "no more with the clock where it was: %d", count_from(from, "poke at"));
    /* An app that says keep_awake(true) again and again, as a timer of 40 ms would, is still on: the call is no new start, and the pokes
     * are the loop's, once a second. */
    for (int i = 0; i < 3; i++) {
        say("awake", "keep", "{\"on\":true}");
        FIFO_SYNC();
    }
    CHECK(count_from(from, "poke at") == 1, "keep_awake(true) while it was on made %d pokes", count_from(from, "poke at"));
    clock_jump(999);
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 1, "none before a second: %d", count_from(from, "poke at"));
    clock_jump(1);
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 2, "the second poke, a second after the first: %d", count_from(from, "poke at"));
    int turn = mark();
    TURN(true);
    WAIT(turn, "front 0");
    clock_jump(5000);
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 2, "no poke while the layer is behind: %d", count_from(from, "poke at"));
    TURN(false);
    WAIT(turn, "front 1");
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 3, "the poke that was due, once the layer is back: %d", count_from(from, "poke at"));
    say("awake", "keep", "{\"on\":false}");
    clock_jump(5000);
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 3, "keep_awake(false) ends them: %d", count_from(from, "poke at"));
    say("awake", "keep", "{\"on\":true}");
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 4, "and true starts them again: %d", count_from(from, "poke at"));
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_jump(5000);
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 4, "an app's end clears it: %d", count_from(from, "poke at"));
    CHECK(apps_task_run(make_pkg("awake", "Awake", AWAKE, 1), true, 0, NULL, err, sizeof err), "run again: %s", err);
    clock_jump(5000);
    FIFO_SYNC();
    CHECK(count_from(from, "poke at") == 4, "the next run doesn't inherit it: %d", count_from(from, "poke at"));
    clock_go();
}

/* sys.show() in a message asks for the front through the one door of every front (C11): the layer that was behind comes up. */
static void sys_show_asks_for_the_front(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("awake", "Awake", AWAKE, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    FIFO_SYNC();
    WAIT_FRAMES(3);
    CHECK(!apps_lv_front(), "the layer starts behind");
    int from = mark();
    say("awake", "show", NULL);
    WAIT(from, "front 1");
    CHECK(apps_lv_front(), "and comes up");
}

/* The app of the store's cases: start() counts its runs, and a message sets, reads or exits. */
static const char APP_STORED[] =
    "local app = {}\n"
    "function app.start()\n"
    "  print('runs ' .. tostring(store.get('runs')))\n"
    "  store.set('runs', (store.get('runs') or 0) + 1)\n"
    "end\n"
    "function app.message(event, data)\n"
    "  if event == 'set' then store.set(data.k, data.v) return {} end\n"
    "  if event == 'get' then return {v = store.get(data.k)} end\n"
    "  if event == 'exit' then sys.exit() return {} end\n"
    "  return nil\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";
static const file_t STORED[] = {LUA("main.lua", APP_STORED)};

static apps_pkg_t *stored_pkg(void) {
    return with_perms(make_pkg("stored", "Stored", STORED, 1), APPS_PERM_STORE);
}

/* What the worker holds for "stored" has this key with this value. */
static void expect_stored_at(int line, const char *needle) {
    char *held = store_peek("stored");
    if (held == NULL || strstr(held, needle) == NULL) {
        fail_at(line, "the store holds '%s', not something with %s", held != NULL ? held : "(nothing)", needle);
    }
    free(held);
}
#define EXPECT_STORED(needle) expect_stored_at(__LINE__, (needle))

/* The app's values are saved by the loop, through the worker (X11), 2 s after the last change and not before: another change in the
 * second before pushes it back. The saves are the I/O worker's ops (the harness checks the thread). */
static void the_store_is_saved_through_the_worker_two_seconds_after_the_last_change(void) {
    char err[300];
    const int64_t t0 = apps_port_mono_ms();
    clock_stop_at(t0);
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");
    ORDERED(from, "store load stored", "print stored runs nil");  /* the values are read before start() runs */
    CHECK(atomic_load(&internal_blocks) == 1, "the load's request was %u small internal blocks: the house rule keeps an atomic out of PSRAM",
          atomic_load(&internal_blocks));
    clock_jump(1999);
    ROUNDS();
    ABSENT(from, "store save queued");
    say("stored", "set", "{\"k\":\"a\",\"v\":1}");  /* at t0 + 1999: the 2 s begin again */
    clock_jump(1);
    ROUNDS();
    ABSENT(from, "store save queued");
    clock_jump(1998);  /* t0 + 3998 */
    ROUNDS();
    ABSENT(from, "store save queued");
    clock_jump(1);
    ROUNDS();
    WAIT(from, "store save queued stored {");
    CHECK(count_from(from, "store save queued stored") == 1, "one save for the two changes: %d", count_from(from, "store save queued stored"));
    EXPECT_STORED("\"a\":1");
    EXPECT_STORED("\"runs\":1");
    ROUNDS();
    clock_jump(10000);
    ROUNDS();
    CHECK(count_from(from, "store save queued stored") == 1, "nothing changed since: no more saves");
    clock_go();
}

/* With nothing else to wake it, the loop wakes for the save: its wait ends when the store is due (apps_rt_next_deadline), not at its next round
 * of up to a second later. Read off the wait the loop asked of its queue (the clock stands still, so no real time is measured): a second while
 * the save is 2 s off, the 50 ms that are left once the clock is 1950 ms on, and the save is made when the last of them is over. */
static void the_loop_wakes_for_the_store_save(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");
    clock_jump(2000);  /* start()'s change is saved 2 s after the run: let it be, so that the case's own is the last */
    ROUNDS();
    WAIT(from, "store save queued stored {");
    from = mark();
    say("stored", "set", "{\"k\":\"late\",\"v\":true}");  /* the save is due 2000 ms on */
    ROUNDS();
    CHECK(atomic_load(&last_receive) == (TickType_t)((1000ull * configTICK_RATE_HZ + 999u) / 1000u),
          "the save is 2 s off and the loop waits %u ticks: its second", (unsigned)atomic_load(&last_receive));
    clock_jump(1950);
    ROUNDS();
    CHECK(atomic_load(&last_receive) == (TickType_t)((50ull * configTICK_RATE_HZ + 999u) / 1000u),
          "the save is 50 ms off and the loop waits %u ticks: for the save, not for its next round", (unsigned)atomic_load(&last_receive));
    ABSENT(from, "store save queued stored");
    clock_jump(50);
    ROUNDS();
    WAIT(from, "store save queued stored {");
    EXPECT_STORED("\"late\":true");
    clock_go();
}

/* A thread that lets the held worker go after a while (a case's hold: hold_the_worker). */
static void *let_the_worker_go_later(void *arg) {
    sleep_ms((int)(intptr_t)arg);
    event("worker released");
    atomic_store(&io_hold, false);
    return NULL;
}

/* The next run reads what the last one left, though the stop's save has to wait in the ring: the worker is in a job that doesn't look at its
 * ops, so the save waits behind it (nothing is saved while it does), and the new run's load is queued behind the save. The ops are in one order:
 * the load reads the save, not what was there before it (nothing). */
static void a_stop_saves_and_the_next_run_reads_it_though_the_save_waits_in_the_ring(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    say("stored", "set", "{\"k\":\"n\",\"v\":5}");
    hold_the_worker();
    int from = mark();
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    WAIT(from, "store save queued stored {");
    char *waiting = store_peek_now("stored");  /* the worker is held: it has run none of its ops */
    CHECK(waiting == NULL, "the save was written while the worker was held: '%s'", waiting != NULL ? waiting : "");
    free(waiting);
    pthread_t releaser;
    CHECK(pthread_create(&releaser, NULL, let_the_worker_go_later, (void *)(intptr_t)300) == 0, "no thread to let the worker go");
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run again: %s", err);
    pthread_join(releaser, NULL);
    WAIT(from, "print stored runs 1");  /* the second run read the first's: runs was 1 when it stopped */
    ORDERED(from, "store save queued stored", "worker released");
    ORDERED(from, "worker released", "store load stored");
    char *reply = apps_task_message("stored", "get", "{\"k\":\"n\"}", false, 5000, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"v\":5}") == 0, "the value came back: %s", reply != NULL ? reply : err);
    apps_port_free(reply);
    clock_go();
}

/* sys.exit() saves what is left as a stop does: the app task's end_app runs stop() (which saves) before the runtime goes. */
static void sys_exit_saves_the_store(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    say("stored", "set", "{\"k\":\"bye\",\"v\":\"yes\"}");
    int from = mark();
    say("stored", "exit", NULL);
    WAIT(from, "log I stop stored: exited");
    WAIT(from, "store save queued stored {");
    EXPECT_STORED("\"bye\":\"yes\"");
    clock_go();
}

/* The worker has parked (it goes after 2 s without a job or an op), and the gadget has no memory for a new one: what the store asks of the
 * worker is refused until memory is back. The run that began this made one for its load. */
static void no_worker_and_no_room_to_make_one(int from) {
    wait_event_at(__LINE__, from, "task muse_apps_io parked", 5000);
    no_worker_can_be_made();
}

/* A save the worker refuses isn't lost: the port keeps it in PSRAM, takes it as saved (the runtime has nothing to ask again), and offers it
 * again every APPS_VALUES_RETRY_MS, no oftener (a retry with no period would write a line at every round); the worker has it once it can. */
static void a_save_the_worker_refuses_is_kept_and_offered_every_two_seconds(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    no_worker_and_no_room_to_make_one(from);
    from = mark();
    clock_jump(2000);  /* start()'s change is due */
    ROUNDS();
    WAIT(from, "store save queued stored {");  /* the port has it */
    CHECK(count_from(from, NO_TASK_LINE) == 1, "offered once: %d", count_from(from, NO_TASK_LINE));
    CHECK(store_pending_count() == 1, "kept: %d waiting", store_pending_count());
    for (int i = 0; i < 5; i++) {
        FIFO_SYNC();
    }
    CHECK(count_from(from, NO_TASK_LINE) == 1, "not again before the next tick: %d", count_from(from, NO_TASK_LINE));
    clock_jump(1999);
    ROUNDS();
    CHECK(count_from(from, NO_TASK_LINE) == 1, "not 1999 ms on: %d", count_from(from, NO_TASK_LINE));
    clock_jump(1);
    ROUNDS();
    CHECK(count_from(from, NO_TASK_LINE) == 2, "again 2 s after: %d", count_from(from, NO_TASK_LINE));
    CHECK(store_pending_count() == 1, "still kept");
    ABSENT(from, "print stored store: the last changes");
    atomic_store(&worker_create_fails, false);
    clock_jump(2000);
    ROUNDS();
    EXPECT_STORED("\"runs\":1");  /* the worker has it, once there is one */
    CHECK(store_pending_count() == 0, "taken: %d still waiting", store_pending_count());
    CHECK(count_from(from, "store save queued stored") == 1, "the runtime asked once: %d", count_from(from, "store save queued stored"));
    clock_go();
}

/* The save at the app's end, which the worker refuses, is kept too: no log line says it is lost, and a restart that comes before the worker can
 * have it reads it (the load looks at what is kept first, and needs no worker for it). The next run's own final save takes its place, and the
 * worker has that. */
static void a_final_save_the_worker_refuses_is_kept_and_a_restart_reads_it(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    no_worker_and_no_room_to_make_one(from);
    from = mark();
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    CHECK(store_pending_count() == 1, "the final save is kept: %d waiting", store_pending_count());
    CHECK(count_from(from, "store save queued stored") == 1, "asked once at the end: %d", count_from(from, "store save queued stored"));
    ABSENT(from, "print stored store: the last changes couldn't be saved");
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run again, with the save still waiting: %s", err);
    WAIT(from, "print stored runs 1");  /* the first run's count: read from what is kept */
    ABSENT(from, "store load failed");
    CHECK(store_pending_count() == 1, "still kept: the restart only read it");
    atomic_store(&worker_create_fails, false);
    CHECK(stop_app(err, sizeof err), "stop: %s", err);  /* runs is 2 now: this save replaces the kept one, and the worker takes it */
    CHECK(store_pending_count() == 0, "the newer save went to the worker: %d kept", store_pending_count());
    EXPECT_STORED("\"runs\":2");
    clock_go();
}

/* An app that writes every second never has the 2 s of quiet its save waits for, and the loop saves it 10 s after the first write that wasn't saved
 * yet (APPS_STORE_MAX_WAIT_MS), with what it has written by then; the next write begins the next 10 s. */
static void a_writer_that_never_pauses_is_saved_at_ten_seconds(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");
    clock_jump(2000);  /* start()'s change is saved: the writer begins from a store with nothing waiting */
    ROUNDS();
    WAIT(from, "store save queued stored {");
    from = mark();
    for (int i = 1; i <= 10; i++) {
        char json[48];
        snprintf(json, sizeof json, "{\"k\":\"w\",\"v\":%d}", i);
        say("stored", "set", json);  /* at second i - 1 of the writer's */
        clock_jump(1000);
        ROUNDS();
        if (i < 10) {
            CHECK(count_from(from, "store save queued stored") == 0, "saved at second %d of a writer's 10 s", i);
        }
    }
    WAIT(from, "store save queued stored {");  /* 10 s after the first write, as the writer wrote its tenth */
    CHECK(count_from(from, "store save queued stored") == 1, "one save: %d", count_from(from, "store save queued stored"));
    EXPECT_STORED("\"w\":10");
    clock_go();
}

/* A load that has to wait for the worker polls in whole ticks: where a tick is 10 ms (the build at 100 Hz) 5 ms is no tick at all, and a delay of
 * none is a bare yield, the app task spinning at its priority for as long as the load waits (a second at the most). */
static void a_store_load_that_waits_polls_in_whole_ticks(void) {
    char err[300];
    hold_the_worker();
    pthread_t releaser;
    CHECK(pthread_create(&releaser, NULL, let_the_worker_go_later, (void *)(intptr_t)300) == 0, "no thread to let the worker go");
    int from = mark();
    unsigned before = atomic_load(&zero_delays);
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    pthread_join(releaser, NULL);
    WAIT(from, "print stored runs nil");
    CHECK(atomic_load(&zero_delays) == before, "the app task delayed for no ticks %u times while its load waited: a yield, not a wait",
          atomic_load(&zero_delays) - before);
}

/* The deadlock the assist exists to prevent, with no console: a job of the worker that runs an app and waits for the app task (a launch's, an
 * install's run). The app task, making the app, asks the worker for its values, which the job holds up; the job's wait runs that op as it
 * looks (apps_io_assist), so the app starts, and not after the load's bound has refused it. */
static atomic_bool store_job_done;

static void run_stored_job(void *arg) {
    (void)arg;
    char err[300];
    bool ok = apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err);
    event("job run %s %s", ok ? "ok" : "failed", ok ? "" : err);
    atomic_store(&store_job_done, true);
}

static void a_job_that_runs_a_store_app_does_not_wait_for_its_load(void) {
    int from = mark();
    CHECK(apps_io_submit(run_stored_job, NULL), "submit the job");
    WAIT(from, "job run ");
    CHECK(at_from(from, "job run ok") >= 0, "the job's run: %s", events[at_from(from, "job run ")]);
    WAIT(from, "print stored runs nil");
    ABSENT(from, "log W store: no answer");  /* a load that waited for its job would have refused the launch at its bound */
    CHECK(count_from(from, "store load stored") == 1, "one load: %d", count_from(from, "store load stored"));
}

/* The store's load waits for the worker (at most a second) inside the call that makes the app, which the supervisor holds to APPS_TASK_HUNG_MS:
 * that wait is the gadget's, not the app's, and the port begins the call again once the load is done (apps_task_call_restart). The load waits 900
 * ms of the clock behind a held worker, then main.lua's top level is stuck in its print: the supervisor counts the 3000 ms from the end of the
 * load and not from the call's start, 3900 ms ago, and the app is not hung until the 3001st. */
static const char APP_BLOCKER[] = "print('blocked')\nreturn {}\n";
static const file_t BLOCKER[] = {LUA("main.lua", APP_BLOCKER)};
static apps_pkg_t *blocker_pkg;
static atomic_bool blocker_done;

static void *blocker_runner(void *arg) {
    (void)arg;
    char err[200];
    (void)apps_task_run(blocker_pkg, false, 0, NULL, err, sizeof err);
    atomic_store(&blocker_done, true);
    return NULL;
}

static void the_wait_for_the_stores_load_is_not_charged_to_the_app(void) {
    const int64_t t0 = (int64_t)5 << 32;
    clock_stop_at(t0);
    hold_the_worker();
    block_print("blocked");
    blocker_pkg = with_perms(make_pkg("blocker", "Blocker", BLOCKER, 1), APPS_PERM_STORE);
    int from = mark();
    unsigned requests = atomic_load(&internal_blocks);
    pthread_t runner;
    CHECK(pthread_create(&runner, NULL, blocker_runner, NULL) == 0, "no runner thread");
    for (int64_t until = real_ms() + 5000; atomic_load(&internal_blocks) == requests;) {
        CHECK(real_ms() < until, "the app task never began its load");
        sleep_ms(1);
    }
    sleep_ms(50);  /* it has made its request, and is polling for the answer */
    clock_jump(900);
    atomic_store(&io_hold, false);  /* the worker goes on, and serves the load: 900 ms of the clock after the call began */
    WAIT_PRINT_BLOCKED();
    clock_stop_at(t0 + 900 + APPS_TASK_HUNG_MS);
    WAIT_FRAMES(10);
    CHECK(count_from(from, "log W") == 0 && count_from(from, "crash ") == 0, "the app is hung at %d ms from the end of its load, and 3900 from the call's start: %s",
          APPS_TASK_HUNG_MS, count_from(from, "log W") > 0 ? events[at_from(from, "log W")] : "a card");
    clock_stop_at(t0 + 900 + APPS_TASK_HUNG_MS + 1);
    int warned = WAIT(from, "log W ");
    CHECK(strstr(events[warned], "stopped responding") != NULL, "the supervisor says: '%s'", events[warned]);
    release_print();
    clock_go();
    pthread_join(runner, NULL);
    WAIT(from, "priority muse_apps 2");
}

/* The worker's ops (apps_io.h): in the order queued; a worker that isn't there is made for the first; and while a job holds the worker they
 * wait for its end, then run before the next job. */
static void io_op(void *arg) {
    event("op %s on %s", (const char *)arg, self_task()->name);
}

static void io_ops_run_in_order_between_jobs(void) {
    apps_io_register_op_task();  /* this thread is the one that submits ops (apps_io.h) */
    int from = mark();
    CHECK(apps_io_submit_op(io_op, "a") && apps_io_submit_op(io_op, "b"), "two ops, with no worker");
    WAIT(from, "op b on muse_apps_io");
    ORDERED(from, "op a on muse_apps_io", "op b on muse_apps_io");
    CHECK(count_from(from, "task muse_apps_io stack") == 1, "one worker was made for them");
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    int held = mark();
    CHECK(apps_io_submit_op(io_op, "c") && apps_io_submit_op(io_op, "d") && apps_io_submit(io_job, "after"), "two ops and a job behind the held one");
    sleep_ms(100);
    ABSENT(held, "op c");  /* the job holds the worker, and doesn't look */
    atomic_store(&io_hold, false);
    WAIT(held, "job after on muse_apps_io");
    ORDERED(held, "op c on muse_apps_io", "op d on muse_apps_io");
    ORDERED(held, "op d on muse_apps_io", "job after on muse_apps_io");
}

/* A job that waits (a launch waiting for the app task) runs the ops that wait as it looks (apps_io_assist): while it still holds the worker.
 * Another task's assist runs nothing. */
static void assisting_job(void *arg) {
    (void)arg;
    atomic_store(&io_held, true);
    while (atomic_load(&io_hold)) {
        apps_io_assist();
        sleep_ms(1);
    }
    event("job assisting ends");
}

static void io_ops_run_while_a_job_that_waits_assists(void) {
    apps_io_register_op_task();  /* this thread is the one that submits ops (apps_io.h) */
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    int from = mark();
    CHECK(apps_io_submit(assisting_job, NULL), "submit the job that waits");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    CHECK(apps_io_submit_op(io_op, "x") && apps_io_submit_op(io_op, "y"), "two ops");
    WAIT(from, "op y on muse_apps_io");
    ORDERED(from, "op x on muse_apps_io", "op y on muse_apps_io");
    ABSENT(from, "job assisting ends");  /* while the job still holds the worker */
    atomic_store(&io_hold, false);
    WAIT(from, "job assisting ends");
    /* A caller that isn't the worker, asking to assist, runs nothing: the ops are the worker's. */
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    int others = mark();
    CHECK(apps_io_submit_op(io_op, "z"), "an op behind the held job");
    apps_io_assist();
    sleep_ms(50);
    ABSENT(others, "op z");
    atomic_store(&io_hold, false);
    WAIT(others, "op z on muse_apps_io");
}

/* APPS_IO_OPS ops can wait; one more is refused with one log line, and never runs. */
static void io_ops_are_bounded(void) {
    apps_io_register_op_task();  /* this thread is the one that submits ops (apps_io.h) */
    atomic_store(&io_held, false);
    atomic_store(&io_hold, true);
    CHECK(apps_io_submit(io_job, "hold"), "submit hold");
    for (int64_t until = real_ms() + 5000; !atomic_load(&io_held);) {
        CHECK(real_ms() < until, "the job never ran");
        sleep_ms(1);
    }
    static const char *const LABELS[] = {"o1", "o2", "o3", "o4", "o5", "o6", "o7", "o8"};
    _Static_assert(ARRAY_LEN(LABELS) == APPS_IO_OPS, "a label for each op the worker holds");
    int from = mark();
    for (size_t i = 0; i < ARRAY_LEN(LABELS); i++) {
        CHECK(apps_io_submit_op(io_op, (void *)LABELS[i]), "op %zu of %d", i + 1, APPS_IO_OPS);
    }
    CHECK(!apps_io_submit_op(io_op, "over"), "one more is refused");
    WAIT(from, "log W io: 8 ops wait already: op refused");
    atomic_store(&io_hold, false);
    WAIT(from, "op o8 on muse_apps_io");
    for (size_t i = 1; i < ARRAY_LEN(LABELS); i++) {
        char first[24], then[24];
        snprintf(first, sizeof first, "op %s on", LABELS[i - 1]);
        snprintf(then, sizeof then, "op %s on", LABELS[i]);
        ORDERED(from, first, then);
    }
    ABSENT(from, "op over");
}

/* With no worker and none that can be made, an op is refused (nothing could ever run it): the nudge's own refusal says why, and the op is
 * taken back, so it doesn't run when memory comes back. */
static void io_op_with_no_worker_and_no_room_is_refused(void) {
    apps_io_register_op_task();  /* this thread is the one that submits ops (apps_io.h) */
    int from = mark();
    atomic_store(&worker_create_fails, true);
    CHECK(!apps_io_submit_op(io_op, "unmade"), "refused when no worker can be made");
    WAIT(from, NO_TASK_LINE);
    atomic_store(&worker_create_fails, false);
    CHECK(apps_io_submit_op(io_op, "made"), "taken once one can be made");
    WAIT(from, "op made on muse_apps_io");
    ABSENT(from, "op unmade");
}

/* With a worker that runs, an op is queued even when the nudge that would wake it is refused (its queue is full): the worker looks at the ops
 * when its job ends, before the jobs that wait. (A worker that waits for a job has room in its queue, so its nudge is never refused.) */
static void io_op_for_a_worker_that_runs_waits_for_its_job(void) {
    apps_io_register_op_task();  /* this thread is the one that submits ops (apps_io.h) */
    hold_the_worker();
    for (int i = 0; i < APPS_IO_JOBS; i++) {
        CHECK(apps_io_submit(io_job, "queued"), "job %d of %d waiting", i + 1, APPS_IO_JOBS);
    }
    int from = mark();
    CHECK(apps_io_submit_op(io_op, "late"), "taken: the worker is there and will look");
    WAIT(from, "log W io: 8 jobs wait already: job refused");
    atomic_store(&io_hold, false);
    int64_t released = real_ms();
    WAIT(from, "op late on muse_apps_io");
    CHECK(real_ms() - released < 500, "the op ran %lld ms after the job ended: as it ended, not at the worker's idle look 2 s on",
          (long long)(real_ms() - released));
    ORDERED(from, "op late on muse_apps_io", "job queued on muse_apps_io");
    for (int64_t until = real_ms() + 5000; count_from(from, "job queued on") < APPS_IO_JOBS;) {
        CHECK(real_ms() < until, "the waiting jobs never ran");
        sleep_ms(1);
    }
}

/* Only the app task submits ops (apps_io.h): the ring has one pusher, and the take-back of a refused op relies on it. The registered task's op
 * is taken; another task's asserts, and the process ends (the Python test reads that off the exit status). */
static void *submit_from_elsewhere(void *arg) {
    (void)arg;
    event("submitting from another thread");
    apps_io_submit_op(io_op, "intruder");
    event("the submit came back");
    return NULL;
}

static void io_op_from_another_task_asserts(void) {
    int from = mark();
    apps_io_register_op_task();
    CHECK(apps_io_submit_op(io_op, "mine"), "the registered task's op was refused");
    WAIT(from, "op mine on muse_apps_io");
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, submit_from_elsewhere, NULL) == 0, "no thread");
    pthread_join(thread, NULL);
    fail_at(__LINE__, "an op from a task that isn't the submitter was taken: it should have asserted");
}

/* A worker that ran only ops (the drain jobs that bring it to them aren't work of its own) says it goes at debug level: a save every 2 s
 * would otherwise write a line each. One that ran a job says it, as it did; so does the boot's first worker, which mounted the store
 * before its op (apps_io_set_first: the mount is work of its own, and its stack is the one to watch), and no later worker mounts again. */
static void the_idle_line_of_a_worker_that_ran_only_ops_is_debug(void) {
    apps_io_register_op_task();
    int from = mark();
    CHECK(atomic_load(&store_mounts) == 0, "no worker yet, no mount: %d", atomic_load(&store_mounts));
    CHECK(apps_io_submit_op(io_op, "first"), "an op, with no worker");
    WAIT(from, "op first on muse_apps_io");
    wait_event_at(__LINE__, from, "task muse_apps_io parked", 5000);
    CHECK(atomic_load(&store_mounts) == 1, "the first worker mounts the store: %d mounts", atomic_load(&store_mounts));
    CHECK(count_from(from, "log I io: idle, stack") == 1 && count_from(from, "log D io: idle") == 0,
          "the idle line of the worker that mounted: %d at info, %d at debug", count_from(from, "log I io: idle, stack"),
          count_from(from, "log D io: idle"));
    from = mark();
    CHECK(apps_io_submit_op(io_op, "only"), "an op, with no worker");
    WAIT(from, "op only on muse_apps_io");
    wait_event_at(__LINE__, from, "task muse_apps_io parked", 5000);
    CHECK(atomic_load(&store_mounts) == 1, "a worker made again doesn't mount again: %d mounts", atomic_load(&store_mounts));
    CHECK(count_from(from, "log D io: idle, stack") == 1 && count_from(from, "log I io: idle") == 0,
          "the idle line of a worker that ran only ops: %d at debug, %d at info", count_from(from, "log D io: idle, stack"),
          count_from(from, "log I io: idle"));
    from = mark();
    CHECK(apps_io_submit(io_job, "one"), "a job");
    CHECK(apps_io_submit_op(io_op, "with"), "an op");
    wait_event_at(__LINE__, from, "task muse_apps_io parked", 5000);
    CHECK(count_from(from, "log I io: idle, stack") == 1 && count_from(from, "log D io: idle") == 0,
          "the idle line of a worker that ran a job: %d at info, %d at debug", count_from(from, "log I io: idle, stack"),
          count_from(from, "log D io: idle"));
}

/* A store that can't be read refuses the launch (an app never starts with an empty store: a later save would replace the real one): with
 * no worker, and none that can be made, the load is refused at once and the run fails with a message that says why. */
static void a_store_that_cannot_be_read_refuses_the_launch(void) {
    char err[300];
    no_worker_can_be_made();
    int from = mark();
    CHECK(!apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "no app without its values");
    CHECK(strstr(err, "can't read the app's saved values") != NULL, "it says why: '%s'", err);
    ABSENT(from, "print stored runs");
    ABSENT(from, "store load stored");
    atomic_store(&worker_create_fails, false);
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run once the memory is back: %s", err);
    WAIT(from, "print stored runs nil");
}

/* A worker that is held by a job that doesn't look at the ops: the load waits APPS_STORE_WAIT_MS and the launch is refused, inside the
 * supervisor's 3 s. The request it leaves is the worker's: it frees it, and the answer it never gave, when it gets to the op (finish() checks
 * too that nothing is held). */
static void a_load_the_worker_cannot_serve_refuses_the_launch_after_a_second(void) {
    char err[300];
    hold_the_worker();
    int from = mark();
    size_t apps_heap = held_now(&port_held), own = held_now(&caps_held);
    int64_t began = real_ms();
    CHECK(!apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "no app without its values");
    int64_t took = real_ms() - began;
    CHECK(took >= APPS_STORE_WAIT_MS && took < APPS_TASK_HUNG_MS, "the launch was refused after %lld ms: the load's bound is %d", (long long)took,
          APPS_STORE_WAIT_MS);
    CHECK(strstr(err, "can't read the app's saved values") != NULL, "it says why: '%s'", err);
    WAIT(from, "log W store: no answer from the I/O worker for stored within 1000 ms: launch refused");
    ABSENT(from, "print stored runs");
    CHECK(held_now(&port_held) + held_now(&caps_held) > apps_heap + own, "the request is still held, for the worker to free");
    atomic_store(&io_hold, false);
    worker_sync();  /* the worker gets to the op, finds nobody waiting, and frees the request */
    CHECK(held_now(&port_held) == apps_heap && held_now(&caps_held) == own, "%zu and %zu bytes held after the worker's turn, not %zu and %zu",
          held_now(&port_held), held_now(&caps_held), apps_heap, own);
    CHECK(count_from(from, "log W launch stored: no app: ") == 1, "one log line for the refused launch: %d", count_from(from, "log W launch stored: no app: "));
}

/* ---- Task 20: the store's mode, the quarantine's marker, the values' backend, the alarms' records and the heartbeat's brief ---- */

/* The launcher says "Loading apps..." (APPS_LV_LOADING rows) until the store's mount has ended (the mode is none until then), and is shown
 * again, with the list, when the list comes while it is up (the mount's end gives one). A list that comes while it is closed opens nothing. */
static void the_launcher_says_loading_until_the_store_is_mounted(void) {
    static const apps_listed_t listed[] = {{"hello", "Hello"}};
    atomic_store(&store_mode, APPS_STORAGE_NONE);
    int from = mark();
    muse_apps_open_launcher();
    WAIT(from, "launcher -1");
    WAIT_FRAMES(5);
    CHECK(count_from(from, "launcher ") == 1, "shown once while it loads: %d", count_from(from, "launcher "));
    from = mark();
    atomic_store(&store_mode, APPS_STORAGE_FLASH);  /* the mount publishes its mode, then its list (apps_store.c) */
    apps_task_set_list(listed, 1);
    WAIT(from, "launcher 1 hello:Hello");
    WAIT_FRAMES(5);
    CHECK(count_from(from, "launcher ") == 1, "shown again once, with the list: %d", count_from(from, "launcher "));
    TURN(true);  /* a voice turn closes the cards */
    TURN(false);
    from = mark();
    apps_task_set_list(listed, 1);
    WAIT_FRAMES(5);
    ABSENT(from, "launcher");
}

/* The quarantine's marker (apps_store.h): every launch marks it with the app's id before any of the app runs (begin_app), and every end that
 * isn't a reset clears it: a stop, a crash, a main.lua that gives no app. A reset in between leaves it marked, for the next boot to count. */
static void a_launch_marks_the_quarantine_and_every_end_clears_it(void) {
    char err[300];
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    CHECK(atomic_load(&marker_launches) == 1 && strcmp(marker_id, "hello") == 0 && atomic_load(&marker_stops) == 0,
          "marked for hello, not cleared while it runs: %d launches ('%s'), %d clears", atomic_load(&marker_launches), marker_id,
          atomic_load(&marker_stops));
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    FIFO_SYNC();
    CHECK(atomic_load(&marker_stops) == 1, "a stop clears it: %d", atomic_load(&marker_stops));
    const file_t crash[] = {LUA("main.lua", APP_START_CRASH)};
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crash, 1), false, 0, NULL, err, sizeof err), "a crash isn't a run");
    FIFO_SYNC();
    CHECK(atomic_load(&marker_launches) == 2 && strcmp(marker_id, "crashy") == 0 && atomic_load(&marker_stops) == 2,
          "a crash in start(): marked, then cleared: %d launches ('%s'), %d clears", atomic_load(&marker_launches), marker_id,
          atomic_load(&marker_stops));
    static const char bad[] = "local x = = 1\n";
    const file_t syntax[] = {LUA("main.lua", bad)};
    CHECK(!apps_task_run(make_pkg("syntax", "Syntax", syntax, 1), false, 0, NULL, err, sizeof err), "no app isn't a run");
    FIFO_SYNC();
    CHECK(atomic_load(&marker_launches) == 3 && strcmp(marker_id, "syntax") == 0 && atomic_load(&marker_stops) == 3,
          "no app: marked, then cleared: %d launches ('%s'), %d clears", atomic_load(&marker_launches), marker_id,
          atomic_load(&marker_stops));
}

/* The heartbeat's brief (muse_apps_brief): the app that is loaded, the record's state and the Lua heap now; once the app has ended, no
 * resident and no heap, with the state it ended in. */
static void the_brief_names_the_loaded_app(void) {
    char resident[APPS_ID_MAX + 1];
    const char *state = NULL;
    unsigned lua_kb = 99;
    muse_apps_brief(resident, sizeof resident, &state, &lua_kb);
    CHECK(strcmp(resident, "") == 0 && strcmp(state, "stopped") == 0 && lua_kb == 0, "before any app: '%s' %s %uK", resident, state, lua_kb);
    char err[300];
    CHECK(apps_task_run(make_pkg("hello", "Hello", HELLO, 1), false, 0, NULL, err, sizeof err), "run: %s", err);
    muse_apps_brief(resident, sizeof resident, &state, &lua_kb);
    CHECK(strcmp(resident, "hello") == 0 && strcmp(state, "running") == 0 && lua_kb > 0, "running: '%s' %s %uK", resident, state, lua_kb);
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    FIFO_SYNC();
    muse_apps_brief(resident, sizeof resident, &state, &lua_kb);
    CHECK(strcmp(resident, "") == 0 && strcmp(state, "stopped") == 0 && lua_kb == 0, "stopped: '%s' %s %uK", resident, state, lua_kb);
    const file_t crash[] = {LUA("main.lua", APP_START_CRASH)};
    CHECK(!apps_task_run(make_pkg("crashy", "Crashy", crash, 1), false, 0, NULL, err, sizeof err), "a crash isn't a run");
    FIFO_SYNC();
    muse_apps_brief(resident, sizeof resident, &state, &lua_kb);
    CHECK(strcmp(resident, "") == 0 && strcmp(state, "crashed") == 0 && lua_kb == 0, "crashed: '%s' %s %uK", resident, state, lua_kb);
}

/* Values on flash (apps_nvs): the app's values are loaded from it and saved to it, by the worker. A save it refuses (full) is said once a
 * streak, in a log line and in the app's log while the app runs; the next save it takes ends the streak, and a later refusal is said again. */
static void a_full_apps_nvs_is_said_once_a_streak(void) {
    char err[300];
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    atomic_store(&flash_refuses, true);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");  /* apps_nvs had nothing for it: "{}" */
    clock_jump(2000);  /* start()'s change is due */
    ROUNDS();
    WAIT(from, "flash save stored {\"runs\":1}");
    WAIT(from, "log W store: apps_nvs refused 10 bytes of values for stored");
    worker_sync();  /* the op has said how it went */
    ROUNDS();
    WAIT(from, "log W stored: " APPS_VALUES_FULL);
    WAIT(from, "print stored " APPS_VALUES_FULL);
    say("stored", "set", "{\"k\":\"a\",\"v\":1}");
    clock_jump(2000);
    ROUNDS();
    worker_sync();
    ROUNDS();
    CHECK(count_from(from, "flash save stored") == 2, "the next save was offered: %d", count_from(from, "flash save stored"));
    CHECK(count_from(from, "log W stored: store:") == 1 && count_from(from, "print stored store:") == 1, "said once a streak: %d, %d",
          count_from(from, "log W stored: store:"), count_from(from, "print stored store:"));
    atomic_store(&flash_refuses, false);
    say("stored", "set", "{\"k\":\"a\",\"v\":2}");
    clock_jump(2000);
    ROUNDS();
    worker_sync();  /* taken: the streak ends */
    atomic_store(&flash_refuses, true);
    say("stored", "set", "{\"k\":\"a\",\"v\":3}");
    clock_jump(2000);
    ROUNDS();
    worker_sync();
    ROUNDS();
    CHECK(count_from(from, "flash save stored") == 4, "four saves: %d", count_from(from, "flash save stored"));
    CHECK(count_from(from, "log W stored: store:") == 2 && count_from(from, "print stored store:") == 2, "a new streak is said again: %d, %d",
          count_from(from, "log W stored: store:"), count_from(from, "print stored store:"));
    CHECK(store_pending_count() == 0, "a refusal of the backend isn't kept for the worker: %d", store_pending_count());
    atomic_store(&flash_refuses, false);
    clock_go();
}

/* The alarms' records follow the alarm table (apps_values.h) where the store is on flash: an arming (the port says which name, as
 * apps_port_idf.c does) or a firing that is told touches a name, and the round APPS_VALUES_ALARM_DELAY_MS after the first touch hands the
 * store the names touched, each with what the table has of it then, as an op of the worker: no other name. Where the store is in RAM nothing
 * is handed: nothing outlives a reset there. */
static void the_alarms_records_follow_the_table_on_flash(void) {
    char err[300];
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);  /* start() arms t, 5000 ms ahead */
    WAIT(from, "print alarmy start");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS - 1);
    ROUNDS();
    worker_sync();
    ABSENT(from, "records");
    clock_jump(1);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 1 t@-") == 1 && count_from(from, "records") == 1, "t's record, once (no clock yet): %d",
          count_from(from, "records"));
    from = mark();
    tell_alarmy("arm", "{\"name\":\"u\",\"ms\":60000}");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 1 u@-") == 1 && count_from(from, "records") == 1, "u's alone: t wasn't touched again: %d",
          count_from(from, "records"));
    from = mark();
    clock_jump(5000 - 2 * APPS_VALUES_ALARM_DELAY_MS);  /* t fires */
    fake_esp_timer_sync();
    WAIT(from, "print alarmy rang t");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 1 t:gone") == 1 && count_from(from, "records") == 1, "t's record went once it was told: %d",
          count_from(from, "records"));
    tell_alarmy("stop", "{}");  /* the ring */
    /* In RAM: the marks go, and nothing is handed, then or later. */
    atomic_store(&store_mode, APPS_STORAGE_RAM);
    from = mark();
    tell_alarmy("arm", "{\"name\":\"v\",\"ms\":60000}");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    ABSENT(from, "records");
    clock_go();
}

/* ---- Task 20, fix round 1 ---- */

/* V: the worker mounts the store before it runs anything of the boot's (apps_io_set_first): the first op it runs finds the mount done. And
 * the app task's rounds tick the store (its mount's retries, the order's write): from its start, and again at every round. */
static atomic_int mounts_seen_by_op = -1;

static void mount_seen_op(void *arg) {
    (void)arg;
    atomic_store(&mounts_seen_by_op, atomic_load(&store_mounts));
    event("op saw the mount");
}

static void the_store_is_mounted_first_and_ticked_every_round(void) {
    int ticks = atomic_load(&store_ticks);
    CHECK(ticks >= 1, "the task's start ticked the store: %d ticks", ticks);
    ROUNDS();
    int later = atomic_load(&store_ticks);
    CHECK(later > ticks, "a round ticks it again: %d, then %d", ticks, later);
    ROUNDS();
    CHECK(atomic_load(&store_ticks) > later, "and the next: %d, then %d", later, atomic_load(&store_ticks));
    apps_io_register_op_task();
    int from = mark();
    CHECK(atomic_load(&store_mounts) == 0, "no worker yet, no mount: %d", atomic_load(&store_mounts));
    CHECK(apps_io_submit_op(mount_seen_op, NULL), "the boot's first op");
    WAIT(from, "op saw the mount");
    CHECK(atomic_load(&mounts_seen_by_op) == 1, "the op ran after the mount: it saw %d mounts", atomic_load(&mounts_seen_by_op));
    wait_event_at(__LINE__, from, "task muse_apps_io parked", 5000);
}

/* R: a list that comes while the launcher is up is shown, even when the frame that first looks can't have the names (another task holds
 * them past its short try): the change is taken only with the names, so a later frame shows it. */
static void a_list_change_survives_a_frame_without_the_names(void) {
    static const apps_listed_t one[] = {{"hello", "Hello"}};
    static const apps_listed_t two[] = {{"timer", "Timer"}, {"hello", "Hello"}};
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    apps_task_set_list(one, 1);
    int from = mark();
    muse_apps_open_launcher();
    WAIT(from, "launcher 1 hello:Hello");
    pause_lvgl();
    from = mark();
    apps_task_set_list(two, 2);  /* the mount's end, an install: the change is flagged, and no frame has looked */
    struct fake_mutex *names = atomic_load(&mutexes_made[MUTEX_NAMES]);
    CHECK(names != NULL && xSemaphoreTake(names, portMAX_DELAY) == pdTRUE, "the case holds the names, as a task on the other core would");
    atomic_store(&lvgl_paused, false);
    WAIT_FRAMES(3);  /* frames that look for the names, and can't have them */
    ABSENT(from, "launcher");
    xSemaphoreGive(names);
    WAIT(from, "launcher 2 timer:Timer hello:Hello");
    WAIT_FRAMES(3);
    CHECK(count_from(from, "launcher ") == 1, "shown once: %d", count_from(from, "launcher "));
}

/* P1 with the real alarm table: the records the mount handed over wait for the clock. The app's own arming of one of their names (start()
 * arms t) makes it the app's: its old record goes at the sync, with no time, and the other's isn't in the sync at all. When the clock comes the
 * untouched record is armed again for the time it has left, t is not, and t gets its new record with its own deadline. The mount runs after
 * the task has started (preboot leaves the store's mode none), as on the gadget. */
static void handed_over_records_are_armed_again_when_the_clock_comes(void) {
    char err[300];
    clock_stop_at(apps_port_mono_ms());
    int64_t wall = WALL_BASE_MS + apps_port_mono_ms();
    hand_over("alarmy", "wake", wall + 600000, true, true);
    hand_over("alarmy", "t", wall + 900000, false, false);
    atomic_store(&store_mode, APPS_STORAGE_FLASH);  /* the mount has run: the next round takes the records */
    int from = mark();
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);  /* start() arms t, 5000 ms ahead */
    WAIT(from, "print alarmy start");
    WAIT(from, "log I store: 2 alarm records wait for the clock");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 1 t@-") == 1 && count_from(from, "records") == 1,
          "t synced with no time (its old record goes), wake not in it: %d syncs", count_from(from, "records"));
    CHECK(apps_alarm_remaining("alarmy", "wake") == -1, "nothing armed again without the clock");
    from = mark();
    atomic_store(&wall_valid, true);  /* the clock comes */
    ROUNDS();
    WAIT(from, "log I store: alarm alarmy/wake armed again: 599000 ms left");
    CHECK(apps_alarm_remaining("alarmy", "wake") == 599000, "wake armed again for the time it has left: %lld",
          (long long)apps_alarm_remaining("alarmy", "wake"));
    CHECK(apps_alarm_remaining("alarmy", "t") == 4000, "t is the app's: %lld", (long long)apps_alarm_remaining("alarmy", "t"));
    ABSENT(from, "log I store: alarm alarmy/t armed again");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 1 t@3000/sw") == 1 && count_from(from, "records") == 1,
          "t's new record, with its own deadline, and t's alone: %d syncs", count_from(from, "records"));
    clock_go();
}

/* I: the budget's cut-off on the firing path (the esp_timer task) cancels every alarm of the app, and the records follow through
 * apps_alarm_take_cancelled, on the app task's round: t, the app's only alarm, whose record was written, has none after its 7th firing in
 * the minute is cut off, though nothing else marked the app since (and the cut-off left it no slot to count). */
static void a_budget_cut_off_drops_the_records(void) {
    char err[300];
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    atomic_store(&wall_valid, true);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(alarmy(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print alarmy start");
    for (int k = 1; k <= 6; k++) {  /* six firings of t within the minute, each told */
        tell_alarmy("arm", "{\"name\":\"t\",\"ms\":1000}");
        clock_jump(1000);
        fake_esp_timer_sync();
        wait_event_at(__LINE__, from, "print alarmy rang t", 5000);
        from = mark();
    }
    tell_alarmy("arm", "{\"name\":\"t\",\"ms\":3000}");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    WAIT(from, "records alarmy 1 t@2000/sw");  /* t's record, from the arming */
    from = mark();
    clock_jump(2000);  /* t's 7th firing in the minute: the budget cancels it */
    fake_esp_timer_sync();
    WAIT(from, "log W alarm: alarmy fired more than 6 alarms in a minute; its alarms are cancelled");
    clock_jump(APPS_VALUES_ALARM_DELAY_MS);
    ROUNDS();
    worker_sync();
    CHECK(count_from(from, "records alarmy 0 all") == 1, "every record of the app goes, from the cut-off alone: %d",
          count_from(from, "records alarmy 0"));
    ABSENT(from, "print alarmy rang");
    tell_alarmy("stop", "{}");
    clock_go();
}

/* P3: a save the write budget can't take waits, and is said once a streak, in the app's log and the gadget's; the app's end writes the
 * newest value whatever the budget says. */
static void a_save_the_budget_holds_back_is_said_and_written_at_the_end(void) {
    char err[300];
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    clock_stop_at(apps_port_mono_ms());
    (void)apps_values_budget_take(APPS_VALUES_BUDGET_BYTES + 100000, true);  /* others' saves have spent it, and more */
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");
    clock_jump(2000);  /* start()'s change is due */
    ROUNDS();
    WAIT(from, "log W stored: " APPS_VALUES_SLOWER);
    WAIT(from, "print stored " APPS_VALUES_SLOWER);
    say("stored", "set", "{\"k\":\"a\",\"v\":1}");
    clock_jump(2000);
    ROUNDS();
    worker_sync();
    ABSENT(from, "flash save stored");
    CHECK(count_from(from, "log W stored: " APPS_VALUES_SLOWER) == 1 && count_from(from, "print stored " APPS_VALUES_SLOWER) == 1,
          "said once a streak: %d, %d", count_from(from, "log W stored: " APPS_VALUES_SLOWER),
          count_from(from, "print stored " APPS_VALUES_SLOWER));
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    worker_sync();
    int at = at_from(from, "flash save stored ");
    CHECK(at >= 0 && count_from(from, "flash save stored ") == 1, "the end wrote it, once");
    if (at >= 0) {
        pthread_mutex_lock(&events_mu);
        bool newest = strstr(events[at], "\"a\":1") != NULL && strstr(events[at], "\"runs\":1") != NULL;
        pthread_mutex_unlock(&events_mu);
        CHECK(newest, "the newest value");
    }
    clock_go();
}

/* K: the failure streak is each app's: the second app's first failure is said, in its own log, while the first's streak goes on. */
static void each_apps_failure_is_said_in_its_own_log(void) {
    char err[300];
    atomic_store(&store_mode, APPS_STORAGE_FLASH);
    atomic_store(&flash_refuses, true);
    clock_stop_at(apps_port_mono_ms());
    int from = mark();
    CHECK(apps_task_run(stored_pkg(), false, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print stored runs nil");
    clock_jump(2000);
    ROUNDS();
    worker_sync();
    ROUNDS();
    WAIT(from, "print stored " APPS_VALUES_FULL);
    apps_pkg_t *other = with_perms(make_pkg("other", "Other", STORED, 1), APPS_PERM_STORE);
    CHECK(apps_task_run(other, false, 0, NULL, err, sizeof err), "run the other: %s", err);  /* stored's streak goes on: its stop failed too */
    WAIT(from, "print other runs nil");
    clock_jump(2000);
    ROUNDS();
    worker_sync();
    ROUNDS();
    WAIT(from, "log W other: " APPS_VALUES_FULL);
    WAIT(from, "print other " APPS_VALUES_FULL);
    CHECK(count_from(from, "log W stored: " APPS_VALUES_FULL) == 1, "stored's said once: %d", count_from(from, "log W stored: " APPS_VALUES_FULL));
    atomic_store(&flash_refuses, false);
    clock_go();
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
    no_worker_can_be_made();
    launch("busy", true);
    WAIT(from, NO_TASK_LINE);
    WAIT_FRAMES(10);
    CHECK(count_from(from, "log W") == 1, "one log line for the refused launch, not %d", count_from(from, "log W"));
    ABSENT(from, "log I launch busy");
    atomic_store(&worker_create_fails, false);
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

/* ---- Task 17: an alarm for an app that doesn't run (the store, and the worker's launch) ---- */

/* An alarm outlives its app's stop, and when it fires the app is started from the store, in front, and alarm(name) comes after
 * start(): start() of this app arms its alarm "t" again (as an alarm clock sets its next), which must not cost the one that
 * fired: it is told, and the new one is armed beside it. */
static void an_alarm_starts_a_stopped_app_and_calls_it_after_start(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    CHECK(apps_task_run(alarmy(), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(0, "print alarmy start");
    int stopping = mark();
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    WAIT(0, "print alarmy stop");
    WAIT(stopping, "front 0");  /* the layer has gone behind (a frame of the LVGL task's): the alarm's front is one of its own */
    CHECK(apps_alarm_remaining("alarmy", "t") == 5000, "the alarm outlives its app: %lld", (long long)apps_alarm_remaining("alarmy", "t"));
    int from = mark();
    clock_jump(5000);
    fake_esp_timer_sync();
    WAIT(from, "log I launch alarmy (256 KB)");
    int started = WAIT(from, "print alarmy start");
    int rang = WAIT(from, "print alarmy rang t");
    CHECK(started < rang, "alarm(name) comes after start() (%d, %d)", started, rang);
    WAIT(from, "front 1");
    CHECK(!apps_alarm_pending(), "told");
    CHECK(apps_alarm_remaining("alarmy", "t") == 5000, "start()'s new alarm is armed: %lld", (long long)apps_alarm_remaining("alarmy", "t"));
    CHECK(count_from(from, "print alarmy rang") == 1, "told once");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* The alarm of an app that isn't the one that runs replaces the one that runs: one app at a time, and an alarm is the user's. */
static void an_alarm_replaces_the_app_that_runs_with_its_own(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    store(make_pkg("busy", "Busy", BUSY, 1));
    CHECK(apps_task_run(make_pkg("busy", "Busy", BUSY, 1), true, 0, NULL, err, sizeof err), "run: %s", err);
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I stop busy: replaced");
    WAIT(from, "log I launch alarmy (256 KB)");
    int rang = WAIT(from, "print alarmy rang t");
    ORDERED(from, "log I stop busy: replaced", "log I launch alarmy (256 KB)");
    CHECK(at_from(from, "print alarmy start") < rang, "start(), then alarm(name)");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* An alarm of an app the store doesn't have is dropped with one line, after the launch's own, and the ring it started ends:
 * nothing will start the app, and with no app there is no one to stop the ring. */
static void an_alarm_for_a_missing_app_is_dropped_and_its_ring_ends(void) {
    clock_stop_at(apps_port_mono_ms());
    CHECK(apps_alarm_arm("ghost", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log W alarm t for missing app ghost");
    ORDERED(from, "log W launch ghost: no such app on the gadget", "log W alarm t for missing app ghost");
    WAIT(from, "log I ring stopped (alarm dropped)");
    CHECK(!apps_alarm_pending() && apps_alarm_remaining("ghost", "t") == -1, "dropped, and out of the table");
    int chirps = count_from(from, "chirp");
    clock_jump(10000);
    fake_esp_timer_sync();
    FIFO_SYNC();
    CHECK(count_from(from, "chirp") == chirps && chirps == 1, "no chirp after the drop: %d", count_from(from, "chirp"));
    CHECK(count_from(from, "log W alarm t for missing app ghost") == 1, "said once");
    clock_go();
}

/* W: an alarm of an app the store quarantined: its launch is refused with the quarantine's words, its firing is dropped, and nothing of the
 * app runs. */
static void an_alarm_for_a_quarantined_app_launches_nothing(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    snprintf(quarantined, sizeof quarantined, "alarmy");
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log W launch alarmy: alarmy was running during two restarts in a row; reinstall it to run it again");
    WAIT(from, "log W alarm t: alarmy did not start: dropped");
    ORDERED(from, "log W launch alarmy: alarmy was running", "log W alarm t: alarmy did not start: dropped");
    FIFO_SYNC();
    ABSENT(from, "print alarmy start");
    CHECK(!apps_alarm_pending() && apps_alarm_remaining("alarmy", "t") == -1, "dropped, and out of the table");
    quarantined[0] = '\0';
    on_worker(empty_store_job, NULL);
    clock_go();
}

/* D: a launch that waits behind a long job of the worker is not given up (the worker has it, and says what came of it, by a post, when
 * it gets to it): past the 10 s that a launch the worker refused gets, however long, the app starts then and is told. */
static void an_alarm_launch_behind_a_long_job_is_waited_for(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I ring alarmy/t chirp 1");
    FIFO_SYNC();  /* the loop has asked for the launch, which waits behind the worker's job */
    ABSENT(from, "log W alarm t: ");
    /* The queue is full now (the launch and the fillers), so a second ask would be refused and logged. The loop doesn't make one: the
     * launch it asked for is queued, and only a launch the worker refused is asked for again. */
    for (int i = 1; i < APPS_IO_JOBS; i++) {
        CHECK(apps_io_submit(io_job, "filler"), "submit a filler");
    }
    for (int i = 0; i < 20; i++) {
        clock_jump(1000);  /* twice the 10 s that a refused launch gets */
        FIFO_SYNC();
    }
    ABSENT(from, "log W io:");
    ABSENT(from, "log W alarm t: ");
    CHECK(apps_alarm_pending(), "the alarm waits in its slot");
    CHECK(count_from(from, "log I ring stopped") == 0, "and the ring goes on");
    atomic_store(&io_hold, false);
    WAIT(from, "log I launch alarmy (256 KB)");
    int started = WAIT(from, "print alarmy start");
    int rang = WAIT(from, "print alarmy rang t");
    CHECK(started < rang, "alarm(name) comes after start() (%d, %d)", started, rang);
    WAIT(from, "queue ALARM_LAUNCH");  /* the worker said it was started */
    CHECK(count_from(from, "print alarmy rang") == 1, "told once");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* D: the backstop above the longest job of the worker (120 s, above the 90 s of an install's deadline): a launch that is never reported is
 * given up then, with a line that says why, and when the worker does get to it, its alarm is gone and it runs nothing: no app comes to the
 * front for an alarm that was given up on. (The ring it started has ended by its own minute long before: the backstop's two are the
 * alarm's, not the ring's.) */
static void an_unreported_alarm_launch_is_given_up_and_runs_nothing(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I ring alarmy/t chirp 1");
    FIFO_SYNC();
    clock_jump(APPS_TASK_ALARM_BACKSTOP_MS - 1000);
    FIFO_SYNC();
    WAIT(from, "log I ring stopped (timeout)");  /* its own minute */
    ABSENT(from, "log W alarm t: ");
    CHECK(apps_alarm_pending(), "the alarm waits still");
    clock_jump(2000);
    FIFO_SYNC();
    WAIT(from, "log W alarm t: alarmy did not start within 120000 ms: dropped");
    CHECK(!apps_alarm_pending(), "dropped");
    atomic_store(&io_hold, false);
    on_worker(empty_store_job, NULL);  /* behind the launch: when it has run, the launch has been through the worker */
    ABSENT(from, "log I launch alarmy");
    ABSENT(from, "print alarmy start");
    FIFO_SYNC();
    ABSENT(from, "print alarmy rang");
    CHECK(count_from(from, "print alarmy") == 0, "nothing of the app ran");
    clock_go();
}

/* D: an alarm that is cancelled while its launch waits for the worker is not launched for: the job finds nothing that wants it. */
static void an_alarm_cancelled_while_its_launch_waits_launches_nothing(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I ring alarmy/t chirp 1");
    FIFO_SYNC();
    apps_alarm_cancel("alarmy", "t");
    WAIT(from, "log I ring stopped (alarm cancelled)");
    atomic_store(&io_hold, false);
    on_worker(empty_store_job, NULL);
    ABSENT(from, "log I launch alarmy");
    ABSENT(from, "print alarmy start");
    FIFO_SYNC();
    CHECK(count_from(from, "print alarmy") == 0, "nothing of the app ran");
    clock_go();
}

/* D: a launch the worker refused is asked for again each second, and given up after 10 s: with the line that says why, and the ring it
 * started ending. */
static void a_refused_alarm_launch_is_given_up_after_ten_seconds(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    no_worker_can_be_made();
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, NO_TASK_LINE);
    for (int i = 0; i < 9; i++) {
        clock_jump(1000);
        FIFO_SYNC();
    }
    ABSENT(from, "log W alarm t: ");
    CHECK(count_from(from, "log W io:") == 10, "asked for each second of the 9 s and the first: %d", count_from(from, "log W io:"));
    clock_jump(1001);
    FIFO_SYNC();
    WAIT(from, "log W alarm t: alarmy did not start within 10000 ms: dropped");
    WAIT(from, "log I ring stopped (alarm dropped)");
    CHECK(!apps_alarm_pending(), "dropped");
    atomic_store(&worker_create_fails, false);
    clock_jump(5000);
    FIFO_SYNC();
    ABSENT(from, "log I launch alarmy");  /* nothing is left to launch for */
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* Giving up on a launch ends the pass, and the loop looks again at once: the alarm of another app that waited behind it is asked for in that same
 * round (the one that the post of its own firing brought), and not in the one a second on. The worker refuses every launch (no memory for a worker),
 * so that an ask shows as its line. */
static void a_launch_given_up_lets_the_next_apps_be_asked_for_in_the_same_round(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy_as("alarmy"));
    store(alarmy_as("second"));
    CHECK(apps_alarm_arm("alarmy", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm");
    no_worker_can_be_made();
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, NO_TASK_LINE);
    for (int i = 0; i < 9; i++) {
        clock_jump(1000);
        FIFO_SYNC();
    }
    int asked = count_from(from, "log W io:");
    CHECK(asked == 10, "alarmy's launch was asked for in each second of the 9 s, and the first: %d", asked);
    CHECK(apps_alarm_arm("second", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm second's");
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop is idle: what brings its next round is the post of second's firing, or the end of its wait a second on */
    clock_jump(1001);  /* alarmy's 10 s are over, and second's alarm fires */
    fake_esp_timer_sync();
    int64_t began = real_ms();
    WAIT(from, "log W alarm t: alarmy did not start within 10000 ms: dropped");
    while (count_from(from, "log W io:") == asked && real_ms() - began < 600) {
        sleep_ms(5);
    }
    CHECK(count_from(from, "log W io:") == asked + 1, "second's launch was asked for %d times, once in the round that gave up alarmy's (within 600 ms)",
          count_from(from, "log W io:") - asked);
    atomic_store(&worker_create_fails, false);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* The same at the backstop: alarmy's launch waits behind a long job of the worker for the 120 s, and is given up in the round that second's firing
 * brings; second's launch is asked for in that round. The worker's queue is full, so that the ask shows as the refusal's line. */
static void a_launch_given_up_at_the_backstop_lets_the_next_apps_be_asked_for_in_the_same_round(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy_as("alarmy"));
    store(alarmy_as("second"));
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I alarm alarmy/t fired (sound 0, wake 0)");
    FIFO_SYNC();  /* the launch is asked for, and waits behind the worker's job */
    for (int i = 1; i < APPS_IO_JOBS; i++) {
        CHECK(apps_io_submit(io_job, "filler"), "submit a filler");  /* the launch and these fill the queue */
    }
    ABSENT(from, "log W io:");
    CHECK(apps_alarm_arm("second", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm second's");
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop is idle: what brings its next round is the post of second's firing, or the end of its wait a second on */
    clock_jump(APPS_TASK_ALARM_BACKSTOP_MS + 1);  /* alarmy's 120 s are over, and second's alarm fires */
    fake_esp_timer_sync();
    int64_t began = real_ms();
    WAIT(from, "log W alarm t: alarmy did not start within 120000 ms: dropped");
    while (count_from(from, "log W io: 8 jobs wait already: job refused") == 0 && real_ms() - began < 600) {
        sleep_ms(5);
    }
    CHECK(count_from(from, "log W io: 8 jobs wait already: job refused") == 1,
          "second's launch was asked for (and refused, the queue being full) %d times, once in the round that gave up alarmy's (within 600 ms)",
          count_from(from, "log W io: 8 jobs wait already: job refused"));
    atomic_store(&io_hold, false);
    for (int64_t until = real_ms() + 5000; count_from(from, "job filler") < APPS_IO_JOBS - 1;) {
        CHECK(real_ms() < until, "the fillers never ran");
        sleep_ms(5);
    }
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* D: a launch that fails (the package doesn't run) drops the firings it was asked for, and no others: the one that fired after it was asked
 * is launched for in its turn, which fails too, and each says so. (A package that doesn't compile: the worker loads it, and the app task
 * can't make a runtime.) */
static const char APP_BROKEN[] = "this is not lua\n";
static const file_t BROKEN[] = {LUA("main.lua", APP_BROKEN)};

static void a_failed_alarm_launch_drops_only_the_firings_it_was_for(void) {
    clock_stop_at(apps_port_mono_ms());
    store(with_perms(make_pkg("failing", "Failing", BROKEN, 1), APPS_PERM_ALARM));
    hold_the_worker();
    CHECK(apps_alarm_arm("failing", "a1", 1000, false, false) == APPS_ALARM_ARMED, "arm a1");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I alarm failing/a1 fired (sound 0, wake 0)");
    FIFO_SYNC();  /* the launch for a1 is asked for, and waits behind the job */
    CHECK(apps_alarm_arm("failing", "a2", 1, false, false) == APPS_ALARM_ARMED, "arm a2");
    clock_jump(1);
    fake_esp_timer_sync();
    WAIT(from, "log I alarm failing/a2 fired (sound 0, wake 0)");
    FIFO_SYNC();  /* the round with both waiting: the launch in flight is for a1 alone, and isn't asked for again */
    atomic_store(&io_hold, false);
    WAIT(from, "log W alarm a1: failing did not start: dropped");
    WAIT(from, "log W alarm a2: failing did not start: dropped");
    ORDERED(from, "log W alarm a1: failing did not start: dropped", "log W alarm a2: failing did not start: dropped");
    CHECK(count_from(from, "log W launch failing: no app: ") == 2, "two launches, one for each: %d", count_from(from, "log W launch failing: no app: "));
    CHECK(count_from(from, "queue ALARM_LAUNCH") == 2, "and the worker told the app task of each");
    CHECK(!apps_alarm_pending(), "nothing waits");
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* D: a launch is for every firing of its app that waited when it was asked for: two that fired together are one launch, and a failed one drops
 * both (the post of the esp_timer task is off, so that the loop sees them together, as it does when it was busy when they fired). */
static void a_failed_alarm_launch_drops_every_firing_it_was_asked_for(void) {
    clock_stop_at(apps_port_mono_ms());
    store(with_perms(make_pkg("failing", "Failing", BROKEN, 1), APPS_PERM_ALARM));
    apps_alarm_set_fired_cb(NULL);
    CHECK(apps_alarm_arm("failing", "a1", 1000, false, false) == APPS_ALARM_ARMED && apps_alarm_arm("failing", "a2", 1000, false, false) == APPS_ALARM_ARMED,
          "arm two");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    FIFO_SYNC();
    FIFO_SYNC();
    WAIT(from, "log W alarm a1: failing did not start: dropped");
    WAIT(from, "log W alarm a2: failing did not start: dropped");
    CHECK(count_from(from, "log W launch failing: no app: ") == 1, "one launch for both: %d", count_from(from, "log W launch failing: no app: "));
    CHECK(count_from(from, "queue ALARM_LAUNCH") == 1, "and one report: %d", count_from(from, "queue ALARM_LAUNCH"));
    CHECK(!apps_alarm_pending(), "nothing waits");
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* D: a launch's result is for that launch: one with another identity (a launch that was given up on, or that began since) says nothing to the one
 * in flight, and the right one does. */
static void a_result_for_another_launch_is_ignored(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I ring alarmy/t chirp 1");
    FIFO_SYNC();  /* the launch is asked for, and waits behind the job */
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 1, "the alarm waits");
    apps_task_alarm_launch_result(f.gen + 100, APPS_ALARM_LAUNCH_FAILED);
    apps_task_alarm_launch_result(f.gen + 1, APPS_ALARM_LAUNCH_MISSING);
    FIFO_SYNC();
    ABSENT(from, "log W alarm t");
    CHECK(apps_alarm_pending(), "the alarm waits still");
    apps_task_alarm_launch_result(f.gen, APPS_ALARM_LAUNCH_FAILED);
    FIFO_SYNC();
    WAIT(from, "log W alarm t: alarmy did not start: dropped");
    WAIT(from, "log I ring stopped (alarm dropped)");
    CHECK(!apps_alarm_pending(), "dropped");
    atomic_store(&io_hold, false);
    on_worker(empty_store_job, NULL);  /* the job that waited finds nothing that wants it */
    ABSENT(from, "log I launch alarmy");
    clock_go();
}

/* P1: an app that its alarm launched and that crashes (in start(), or in the alarm(name) it was launched for) loses every alarm it has, and
 * is not launched again: start() arms an alarm of 50 ms and a far one before it raises (or before alarm() does), which is what made the
 * launch a loop. The crash's own card and line come as for any crash, the cancel says why, and the ring that alarm began ends. */
static const char APP_LOOPY_START[] =
    "local app = {}\n"
    "function app.start()\n"
    "  print('start')\n"
    "  alarm.after('t', 50)\n"
    "  alarm.after('far', 100000)\n"
    "  error('boom in start')\n"
    "end\n"
    "return app\n";
static const file_t LOOPY_START[] = {LUA("main.lua", APP_LOOPY_START)};

static const char APP_LOOPY_ALARM[] =
    "local app = {}\n"
    "function app.start()\n"
    "  print('start')\n"
    "  alarm.after('t', 50)\n"
    "  alarm.after('far', 100000)\n"
    "end\n"
    "function app.alarm(name)\n"
    "  print('rang ' .. name)\n"
    "  error('boom in alarm')\n"
    "end\n"
    "return app\n";
static const file_t LOOPY_ALARM[] = {LUA("main.lua", APP_LOOPY_ALARM)};

static void loopy_is_launched_once(const file_t *files, const char *crash_line, const char *ring_reason) {
    clock_stop_at(apps_port_mono_ms());
    store(with_perms(make_pkg("loopy", "Loopy", files, 1), APPS_PERM_ALARM));
    CHECK(apps_alarm_arm("loopy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, crash_line);
    WAIT(from, "log W alarm: loopy crashed when its alarm launched it; its alarms are cancelled");
    char stopped[80];
    snprintf(stopped, sizeof stopped, "log I ring stopped (%s)", ring_reason);
    WAIT(from, stopped);
    clock_jump(200000);  /* past its own alarm of 50 ms, and the far one */
    fake_esp_timer_sync();
    FIFO_SYNC();
    FIFO_SYNC();
    CHECK(count_from(from, "print loopy start") == 1, "launched %d times", count_from(from, "print loopy start"));
    CHECK(count_from(from, "log I launch loopy") == 1, "and %d launch lines", count_from(from, "log I launch loopy"));
    CHECK(!apps_alarm_pending(), "nothing waits");
    char names[4][24];
    int64_t left[4];
    CHECK(apps_alarm_remaining("loopy", "t") == -1 && apps_alarm_remaining("loopy", "far") == -1 && apps_alarm_list("loopy", names, left) == 0,
          "the table holds none of its alarms");
    clock_go();
    on_worker(empty_store_job, NULL);
}

static void a_crash_in_start_of_an_alarm_launch_cancels_the_alarms(void) {
    loopy_is_launched_once(LOOPY_START, "crash Loopy | main.lua:6: boom in start", "alarms cancelled");
}

static void a_crash_in_the_launching_alarm_cancels_the_alarms(void) {
    loopy_is_launched_once(LOOPY_ALARM, "crash Loopy | main.lua:9: boom in alarm", "app crashed");
}

/* P1's window is the first delivery round after the launch: an app that its alarm launched, and that crashes in an alarm() of a later round, is
 * only an app that crashed: its other alarms stay armed (start()'s own among them), and no line says they were cancelled. */
static void a_crash_after_the_launch_costs_the_app_nothing(void) {
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    CHECK(apps_alarm_arm("alarmy", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "print alarmy rang t");  /* launched for it, and told; start() armed "t" again, 5000 ms on */
    FIFO_SYNC();
    FIFO_SYNC();
    tell_alarmy("arm", "{\"name\":\"crash\",\"ms\":100}");
    clock_jump(100);
    fake_esp_timer_sync();
    WAIT(from, "crash Alarmy | main.lua:10: alarm boom");
    ABSENT(from, "log W alarm: alarmy");
    CHECK(apps_alarm_remaining("alarmy", "t") > 0, "start()'s alarm is still armed: %lld", (long long)apps_alarm_remaining("alarmy", "t"));
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* The loop looks at the table again when telling an alarm ended the app (a crash in alarm(name)): the copy it holds may name firings that the
 * crash took with it, and a launch asked for one of those would hold the next app's back, with the worker saying nothing of a launch it
 * skipped: the next app would wait for the rounds that come a second apart. Here the handler cancels its other firing before it raises, and
 * another app's alarm waits behind the two: it is asked for in the round that told the crash. (apps_alarm_set_fired_cb(NULL): no post, so the
 * loop finds the three together, at the round of the one event the case brings.) */
static const char APP_CANCELS[] =
    "local app = {}\n"
    "function app.start() print('start') end\n"
    "function app.alarm(name)\n"
    "  print('rang ' .. name)\n"
    "  alarm.cancel('u')\n"
    "  error('boom in alarm')\n"
    "end\n"
    "return app\n";
static const file_t CANCELS[] = {LUA("main.lua", APP_CANCELS)};

static void a_crash_in_alarm_makes_the_loop_look_again_for_the_next_app(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy_as("second"));
    int from = mark();
    CHECK(apps_task_run(with_perms(make_pkg("cancels", "Cancels", CANCELS, 1), APPS_PERM_ALARM), true, 0, NULL, err, sizeof err), "run: %s", err);
    WAIT(from, "print cancels start");
    FIFO_SYNC();
    apps_alarm_set_fired_cb(NULL);
    CHECK(apps_alarm_arm("cancels", "t", 1000, false, false) == APPS_ALARM_ARMED && apps_alarm_arm("cancels", "u", 1000, false, false) == APPS_ALARM_ARMED &&
              apps_alarm_arm("second", "t", 1000, false, false) == APPS_ALARM_ARMED,
          "arm three");
    FIFO_SYNC();
    FIFO_SYNC();  /* the loop is idle: what wakes it next is the event of the case, or the end of its wait a second on */
    from = mark();
    int64_t began = real_ms();
    clock_jump(1000);
    fake_esp_timer_sync();
    CHECK(apps_alarm_pending(), "the three wait in their slots");
    FIFO_SYNC();
    WAIT(from, "log I stop cancels: crashed");
    WAIT(from, "print second start");
    CHECK(real_ms() - began < 800, "second's app started %lld ms after: its launch was asked for in the round that told the crash, and not in one a second on",
          (long long)(real_ms() - began));
    WAIT(from, "print second rang t");
    CHECK(count_from(from, "print cancels rang") == 1, "the crashed app was told once");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* A launch the I/O worker refuses (no memory for a worker) is one line in the log and is asked for again a second later, not at
 * every round of the loop (which is what a retry with no period would be): once the memory is back, the app starts and is told. */
static void a_refused_alarm_launch_is_asked_for_again(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy());
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED, "arm");
    no_worker_can_be_made();
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, NO_TASK_LINE);
    for (int i = 0; i < 5; i++) {
        FIFO_SYNC();  /* rounds of the loop, with the alarm waiting: it doesn't ask again at each one */
    }
    CHECK(count_from(from, "log W io:") == 1, "asked %d times in a second of the clock that didn't move", count_from(from, "log W io:"));
    ABSENT(from, "log I launch alarmy");
    atomic_store(&worker_create_fails, false);
    clock_jump(1000);  /* ALARM_RETRY_MS: the time to ask again */
    FIFO_SYNC();
    WAIT(from, "log I launch alarmy (256 KB)");
    WAIT(from, "print alarmy rang t");
    CHECK(count_from(from, "log W") == 1, "the refusal was the only warning");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* Alarms of two apps that aren't running, due together: the first to fire starts its app and is told, and the other app's launch
 * comes on a later round of the loop and replaces it (one app at a time), then that one is told: neither alarm is lost. */
static void alarms_of_two_stopped_apps_are_told_one_after_the_other(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy_as("alarmy"));
    store(alarmy_as("second"));
    CHECK(apps_alarm_arm("alarmy", "t", 1000, true, true) == APPS_ALARM_ARMED && apps_alarm_arm("second", "t", 2000, true, true) == APPS_ALARM_ARMED, "arm two");
    int from = mark();
    clock_jump(5000);
    fake_esp_timer_sync();
    WAIT(from, "print alarmy rang t");
    WAIT(from, "print second rang t");
    ORDERED(from, "print alarmy start", "print alarmy rang t");
    ORDERED(from, "print alarmy rang t", "log I stop alarmy: replaced");
    ORDERED(from, "log I stop alarmy: replaced", "print second start");
    ORDERED(from, "print second start", "print second rang t");
    CHECK(count_from(from, "print alarmy rang") == 1 && count_from(from, "print second rang") == 1, "once each");
    CHECK(!apps_alarm_pending(), "both told");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}

/* One launch is in flight at a time, and the loop keeps it in mind for as long as a firing of its app waits. When the alarm it was asked for is
 * cancelled (and the worker, held by a long job, has not got to it yet), another app's alarm that fires does not wait behind it for a report that
 * never comes (the worker skips a launch that nothing wants, and says nothing): the stale launch is forgotten, and the next round asks for the
 * other app's. */
static void a_launch_whose_alarm_went_does_not_hold_back_the_next_apps(void) {
    char err[200];
    clock_stop_at(apps_port_mono_ms());
    store(alarmy_as("alarmy"));
    store(alarmy_as("second"));
    hold_the_worker();
    CHECK(apps_alarm_arm("alarmy", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log I alarm alarmy/t fired (sound 0, wake 0)");
    FIFO_SYNC();  /* the launch for alarmy is asked for, and waits behind the worker's job */
    apps_alarm_cancel("alarmy", "t");
    CHECK(apps_alarm_arm("second", "t", 1, false, false) == APPS_ALARM_ARMED, "arm second's");
    clock_jump(1);
    fake_esp_timer_sync();
    WAIT(from, "log I alarm second/t fired (sound 0, wake 0)");
    FIFO_SYNC();  /* a round finds the launch in flight with no firing of its app waiting: it is forgotten */
    FIFO_SYNC();  /* and a later one asks for second's, once the first of these has been answered and its round is over */
    atomic_store(&io_hold, false);
    WAIT(from, "print second rang t");  /* the worker skipped alarmy's launch and ran second's */
    ORDERED(from, "print second start", "print second rang t");
    WAIT(from, "queue ALARM_LAUNCH");  /* the worker says second's was started */
    CHECK(count_from(from, "print alarmy") == 0 && count_from(from, "log I launch alarmy") == 0, "nothing of alarmy ran");
    CHECK(count_from(from, "queue ALARM_LAUNCH") == 1, "one report, second's, and none for the launch that was skipped: %d", count_from(from, "queue ALARM_LAUNCH"));
    CHECK(!apps_alarm_pending(), "second's alarm was told");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    clock_go();
    on_worker(empty_store_job, NULL);
}
/* ---- Task 18: the store and a launch that is a job of the worker ---- */

/* A launch is a job of the I/O worker, which waits for the app task; the app task, making the app, asks the worker for the app's values
 * (an op). The job's waits run the ops that wait (apps_io_assist), so the load is served as the job looks and the app starts inside the load's
 * bound; without that the load would wait for the job and the job for the load, until the load's bound refused the launch. */
static void a_launch_of_a_store_app_does_not_wait_for_its_own_load(void) {
    char err[200];
    store(stored_pkg());
    int from = mark();
    launch("stored", true);
    WAIT(from, "print stored runs nil");  /* a load that waited for its job would have refused the launch at its bound, and this would never come */
    ABSENT(from, "log W store: no answer");
    CHECK(count_from(from, "store load stored") == 1, "one load: %d", count_from(from, "store load stored"));
    ORDERED(from, "log I launch stored", "store load stored");
    CHECK(stop_app(err, sizeof err), "stop: %s", err);
    on_worker(empty_store_job, NULL);
}

/* An app that its alarm launched and that calls sys.exit() in start(), before the alarm that launched it was told: the run fails ("the app
 * exited"), the worker says the launch failed, and the alarm is dropped with the line every launch that didn't run gets. Nothing launches it
 * again and again for the alarm that waits (the same rule as a crash in start()). */
static void an_alarm_launch_of_an_app_that_exits_in_start_is_given_up(void) {
    clock_stop_at(apps_port_mono_ms());
    const file_t in_start[] = {LUA("main.lua", APP_EXIT_START)};
    store(with_perms(make_pkg("quitter", "Quitter", in_start, 1), APPS_PERM_ALARM));
    CHECK(apps_alarm_arm("quitter", "t", 1000, false, false) == APPS_ALARM_ARMED, "arm");
    int from = mark();
    clock_jump(1000);
    fake_esp_timer_sync();
    WAIT(from, "log W alarm t: quitter did not start: dropped");
    WAIT(from, "log W launch quitter: the app exited (sys.exit)");
    for (int i = 0; i < 5; i++) {
        FIFO_SYNC();
        clock_jump(1000);
    }
    FIFO_SYNC();
    CHECK(count_from(from, "log I launch quitter") == 1 && count_from(from, "print quitter start ends") == 1, "launched once, not again and again: %d launches, %d starts",
          count_from(from, "log I launch quitter"), count_from(from, "print quitter start ends"));
    CHECK(!apps_alarm_pending(), "nothing waits");
    clock_go();
    on_worker(empty_store_job, NULL);
}


/* A big Lua heap: a 400 KB string it keeps (and, until a collection, the buffer it was made in). */
static const char APP_BIG[] =
    "local app = {}\n"
    "local hold\n"
    "function app.start() hold = string.rep('x', 400 * 1024) ui.label{text = 'Big'} end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";

/* The text of event i. */
static void event_text(int i, char *out, size_t cap) {
    pthread_mutex_lock(&events_mu);
    snprintf(out, cap, "%s", events[i]);
    pthread_mutex_unlock(&events_mu);
}

/* N3: a launch beside a big app counts what the big app's end frees (its Lua heap as the VM counts it, and its package): hello, short of
 * its heap while big runs and with room once big has gone, is loaded, replaces big and runs. huge, whose heap doesn't fit even once big
 * has gone, is refused up front, in words that say so, and big runs on. */
static void a_launch_beside_a_big_app_counts_what_its_end_frees(void) {
    char err[300], text[400], running[APPS_ID_MAX + 1];
    const file_t big_files[] = {LUA("main.lua", APP_BIG)};
    store(make_pkg("hello", "Hello", HELLO, 1));
    apps_pkg_t *huge = make_pkg("huge", "Huge", HELLO, 1);
    huge->manifest.memory_kb = 4096;
    store(huge);
    apps_pkg_t *big = make_pkg("big", "Big", big_files, 1);
    big->manifest.memory_kb = 1024;
    CHECK(apps_task_run(big, true, 50, NULL, err, sizeof err), "run big: %s", err);
    apps_status_t *st = new_status();
    apps_task_status(st);
    CHECK(st->mem_kb >= 400, "big's heap: %u KB", (unsigned)st->mem_kb);
    /* 200 KB short of hello's copy, heap and reserve while big runs; big's end frees more than that. */
    size_t need = (sizeof APP_HELLO - 1) + 256 * 1024 + APPS_RAM_RESERVE;
    atomic_store(&psram_total, held_now(&port_held) + need - 200 * 1024);
    int from = mark();
    launch("hello", true);
    WAIT(from, "log I stop big: replaced, peak ");
    WAIT(from, "log I launch hello (256 KB)");
    WAIT(from, "print hello start");
    ABSENT(from, "log W launch hello");
    /* huge beside big: short even once big has gone. */
    atomic_store(&psram_total, 0);
    big = make_pkg("big", "Big", big_files, 1);
    big->manifest.memory_kb = 1024;
    CHECK(apps_task_run(big, true, 50, NULL, err, sizeof err), "run big again: %s", err);
    atomic_store(&psram_total, held_now(&port_held) + need - 200 * 1024);
    from = mark();
    launch("huge", true);
    int i = WAIT(from, "log W launch huge: the gadget is short of memory for huge: ");
    event_text(i, text, sizeof text);
    CHECK(strstr(text, " KB free once big ends, ") != NULL && strstr(text, " KB needed; restart the gadget or install a smaller app") != NULL,
          "the refusal: %s", text);
    ROUNDS();
    CHECK(apps_task_running(running, sizeof running) && strcmp(running, "big") == 0, "big runs on: '%s'", running);
    ABSENT(from, "log I stop big");
    ABSENT(from, "log I launch huge");
    atomic_store(&psram_total, 0);
    heap_caps_free(st);
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
        {"a_run_is_admitted_once_the_app_it_replaces_has_gone", a_run_is_admitted_once_the_app_it_replaces_has_gone},
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
        {"a_touch_on_an_ended_apps_object_never_reaches_the_new_app", a_touch_on_an_ended_apps_object_never_reaches_the_new_app},
        {"parking_is_bounded", parking_is_bounded},
        {"a_replace_whose_stop_hangs_drops_the_run", a_replace_whose_stop_hangs_drops_the_run},
        {"a_back_request_in_the_turns_last_frame", a_back_request_in_the_turns_last_frame},
        {"a_front_asked_during_a_turn_waits_for_the_table", a_front_asked_during_a_turn_waits_for_the_table},
        {"a_new_app_drops_an_old_crash_card", a_new_app_drops_an_old_crash_card},
        {"a_new_app_drops_an_old_hung_card", a_new_app_drops_an_old_hung_card},
        {"cpu_pct_follows_idle_time", cpu_pct_follows_idle_time},
        {"a_snapshot_is_not_the_calls_time", a_snapshot_is_not_the_calls_time},
        {"a_snapshot_is_not_hung_time", a_snapshot_is_not_hung_time},
        {"the_snapshot_is_offered_every_frame", the_snapshot_is_offered_every_frame},
        {"dropped_is_read_with_its_base", dropped_is_read_with_its_base},
        {"a_front_asked_during_a_turn_comes_after_it", a_front_asked_during_a_turn_comes_after_it},
        {"a_card_waits_for_the_turn_to_end", a_card_waits_for_the_turn_to_end},
        {"a_turn_after_the_app_ended_fronts_nothing", a_turn_after_the_app_ended_fronts_nothing},
        {"stop_reports_failure", stop_reports_failure},
        {"the_tick_says_when_the_apps_cover", the_tick_says_when_the_apps_cover},
        {"a_turn_during_a_new_apps_load_fronts_nothing", a_turn_during_a_new_apps_load_fronts_nothing},
        {"a_turn_brings_back_the_app_that_was_in_front", a_turn_brings_back_the_app_that_was_in_front},
        {"a_message_gets_its_reply", a_message_gets_its_reply},
        {"run_for_send_is_by_id_behind_and_bounded", run_for_send_is_by_id_behind_and_bounded},
        {"a_message_after_its_caller_gave_up", a_message_after_its_caller_gave_up},
        {"io_runs_jobs_in_order", io_runs_jobs_in_order},
        {"io_refuses_when_full_or_unmade", io_refuses_when_full_or_unmade},
        {"io_takes_jobs_whatever_internal_ram_says", io_takes_jobs_whatever_internal_ram_says},
        {"a_worker_on_its_way_to_park_is_never_deleted", a_worker_on_its_way_to_park_is_never_deleted},
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
        {"an_alarm_of_the_running_app_fronts_it_and_calls_it", an_alarm_of_the_running_app_fronts_it_and_calls_it},
        {"a_delivery_does_not_depend_on_the_post", a_delivery_does_not_depend_on_the_post},
        {"a_firing_with_no_post_and_no_event_is_told_when_the_loops_second_ends", a_firing_with_no_post_and_no_event_is_told_when_the_loops_second_ends},
        {"an_alarm_posted_to_a_full_queue_is_told", an_alarm_posted_to_a_full_queue_is_told},
        {"a_flood_of_firings_posts_one_event_and_leaves_room_for_requests", a_flood_of_firings_posts_one_event_and_leaves_room_for_requests},
        {"alarms_are_told_in_the_order_they_fired", alarms_are_told_in_the_order_they_fired},
        {"an_alarm_during_a_voice_turn_fronts_the_app_after_it", an_alarm_during_a_voice_turn_fronts_the_app_after_it},
        {"an_alarm_is_told_while_the_screen_sleeps", an_alarm_is_told_while_the_screen_sleeps},
        {"a_crash_in_alarm_ends_the_ring", a_crash_in_alarm_ends_the_ring},
        {"a_run_for_alarms_that_are_gone_replaces_nothing", a_run_for_alarms_that_are_gone_replaces_nothing},
        {"an_alarm_cancelled_before_it_is_taken_is_not_told", an_alarm_cancelled_before_it_is_taken_is_not_told},
        {"sys_exit_ends_the_app_after_the_callback", sys_exit_ends_the_app_after_the_callback},
        {"sys_exit_in_a_timer_runs_no_other_callback", sys_exit_in_a_timer_runs_no_other_callback},
        {"sys_exit_before_the_run_settles_fails_the_run", sys_exit_before_the_run_settles_fails_the_run},
        {"keep_awake_pokes_the_idle_timer_while_in_front", keep_awake_pokes_the_idle_timer_while_in_front},
        {"sys_show_asks_for_the_front", sys_show_asks_for_the_front},
        {"the_store_is_saved_through_the_worker_two_seconds_after_the_last_change", the_store_is_saved_through_the_worker_two_seconds_after_the_last_change},
        {"the_loop_wakes_for_the_store_save", the_loop_wakes_for_the_store_save},
        {"a_stop_saves_and_the_next_run_reads_it_though_the_save_waits_in_the_ring", a_stop_saves_and_the_next_run_reads_it_though_the_save_waits_in_the_ring},
        {"sys_exit_saves_the_store", sys_exit_saves_the_store},
        {"a_save_the_worker_refuses_is_kept_and_offered_every_two_seconds", a_save_the_worker_refuses_is_kept_and_offered_every_two_seconds},
        {"a_final_save_the_worker_refuses_is_kept_and_a_restart_reads_it", a_final_save_the_worker_refuses_is_kept_and_a_restart_reads_it},
        {"a_writer_that_never_pauses_is_saved_at_ten_seconds", a_writer_that_never_pauses_is_saved_at_ten_seconds},
        {"a_store_load_that_waits_polls_in_whole_ticks", a_store_load_that_waits_polls_in_whole_ticks},
        {"a_job_that_runs_a_store_app_does_not_wait_for_its_load", a_job_that_runs_a_store_app_does_not_wait_for_its_load},
        {"the_wait_for_the_stores_load_is_not_charged_to_the_app", the_wait_for_the_stores_load_is_not_charged_to_the_app},
        {"a_store_that_cannot_be_read_refuses_the_launch", a_store_that_cannot_be_read_refuses_the_launch},
        {"a_load_the_worker_cannot_serve_refuses_the_launch_after_a_second", a_load_the_worker_cannot_serve_refuses_the_launch_after_a_second},
        {"io_ops_run_in_order_between_jobs", io_ops_run_in_order_between_jobs},
        {"io_ops_run_while_a_job_that_waits_assists", io_ops_run_while_a_job_that_waits_assists},
        {"io_ops_are_bounded", io_ops_are_bounded},
        {"io_op_with_no_worker_and_no_room_is_refused", io_op_with_no_worker_and_no_room_is_refused},
        {"io_op_for_a_worker_that_runs_waits_for_its_job", io_op_for_a_worker_that_runs_waits_for_its_job},
        {"io_op_from_another_task_asserts", io_op_from_another_task_asserts},
        {"the_idle_line_of_a_worker_that_ran_only_ops_is_debug", the_idle_line_of_a_worker_that_ran_only_ops_is_debug},
        {"the_launcher_says_loading_until_the_store_is_mounted", the_launcher_says_loading_until_the_store_is_mounted},
        {"a_launch_marks_the_quarantine_and_every_end_clears_it", a_launch_marks_the_quarantine_and_every_end_clears_it},
        {"the_brief_names_the_loaded_app", the_brief_names_the_loaded_app},
        {"a_full_apps_nvs_is_said_once_a_streak", a_full_apps_nvs_is_said_once_a_streak},
        {"the_alarms_records_follow_the_table_on_flash", the_alarms_records_follow_the_table_on_flash},
        {"the_store_is_mounted_first_and_ticked_every_round", the_store_is_mounted_first_and_ticked_every_round},
        {"a_list_change_survives_a_frame_without_the_names", a_list_change_survives_a_frame_without_the_names},
        {"handed_over_records_are_armed_again_when_the_clock_comes", handed_over_records_are_armed_again_when_the_clock_comes},
        {"a_budget_cut_off_drops_the_records", a_budget_cut_off_drops_the_records},
        {"a_save_the_budget_holds_back_is_said_and_written_at_the_end", a_save_the_budget_holds_back_is_said_and_written_at_the_end},
        {"each_apps_failure_is_said_in_its_own_log", each_apps_failure_is_said_in_its_own_log},
#ifdef HARNESS_CONSOLE
        {"a_pick_runs_a_stored_app_that_stopped", a_pick_runs_a_stored_app_that_stopped},
        {"a_refused_launch_logs_once", a_refused_launch_logs_once},
        {"the_launcher_lists_the_store", the_launcher_lists_the_store},
        {"the_launcher_keeps_apps_that_ended", the_launcher_keeps_apps_that_ended},
        {"two_launches_of_one_id_run_it_once", two_launches_of_one_id_run_it_once},
        {"an_alarm_starts_a_stopped_app_and_calls_it_after_start", an_alarm_starts_a_stopped_app_and_calls_it_after_start},
        {"an_alarm_replaces_the_app_that_runs_with_its_own", an_alarm_replaces_the_app_that_runs_with_its_own},
        {"an_alarm_for_a_missing_app_is_dropped_and_its_ring_ends", an_alarm_for_a_missing_app_is_dropped_and_its_ring_ends},
        {"an_alarm_for_a_quarantined_app_launches_nothing", an_alarm_for_a_quarantined_app_launches_nothing},
        {"an_alarm_launch_behind_a_long_job_is_waited_for", an_alarm_launch_behind_a_long_job_is_waited_for},
        {"an_unreported_alarm_launch_is_given_up_and_runs_nothing", an_unreported_alarm_launch_is_given_up_and_runs_nothing},
        {"an_alarm_cancelled_while_its_launch_waits_launches_nothing", an_alarm_cancelled_while_its_launch_waits_launches_nothing},
        {"a_refused_alarm_launch_is_asked_for_again", a_refused_alarm_launch_is_asked_for_again},
        {"a_refused_alarm_launch_is_given_up_after_ten_seconds", a_refused_alarm_launch_is_given_up_after_ten_seconds},
        {"a_failed_alarm_launch_drops_only_the_firings_it_was_for", a_failed_alarm_launch_drops_only_the_firings_it_was_for},
        {"a_failed_alarm_launch_drops_every_firing_it_was_asked_for", a_failed_alarm_launch_drops_every_firing_it_was_asked_for},
        {"a_result_for_another_launch_is_ignored", a_result_for_another_launch_is_ignored},
        {"a_crash_in_start_of_an_alarm_launch_cancels_the_alarms", a_crash_in_start_of_an_alarm_launch_cancels_the_alarms},
        {"a_crash_in_the_launching_alarm_cancels_the_alarms", a_crash_in_the_launching_alarm_cancels_the_alarms},
        {"a_crash_after_the_launch_costs_the_app_nothing", a_crash_after_the_launch_costs_the_app_nothing},
        {"alarms_of_two_stopped_apps_are_told_one_after_the_other", alarms_of_two_stopped_apps_are_told_one_after_the_other},
        {"a_launch_whose_alarm_went_does_not_hold_back_the_next_apps", a_launch_whose_alarm_went_does_not_hold_back_the_next_apps},
        {"a_crash_in_alarm_makes_the_loop_look_again_for_the_next_app", a_crash_in_alarm_makes_the_loop_look_again_for_the_next_app},
        {"a_launch_given_up_lets_the_next_apps_be_asked_for_in_the_same_round", a_launch_given_up_lets_the_next_apps_be_asked_for_in_the_same_round},
        {"a_launch_given_up_at_the_backstop_lets_the_next_apps_be_asked_for_in_the_same_round", a_launch_given_up_at_the_backstop_lets_the_next_apps_be_asked_for_in_the_same_round},
        {"a_launch_of_a_store_app_does_not_wait_for_its_own_load", a_launch_of_a_store_app_does_not_wait_for_its_own_load},
        {"an_alarm_launch_of_an_app_that_exits_in_start_is_given_up", an_alarm_launch_of_an_app_that_exits_in_start_is_given_up},
        {"a_launch_beside_a_big_app_counts_what_its_end_frees", a_launch_beside_a_big_app_counts_what_its_end_frees},
#endif
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            preboot(argv[1]);
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
