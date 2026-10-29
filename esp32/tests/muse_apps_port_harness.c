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
 * The device's port of Muse apps (apps_port_idf.c), run on the host. Usage: muse_apps_port_harness <case>, one case per
 * test in tests/test_muse_apps_port.py; it exits 0 when the case holds, and otherwise prints why.
 *
 * Its ESP-IDF, FreeRTOS and PSA are fakes (the declarations of tests/muse_apps_task_fakes.h, defined here): a clock the
 * case sets, a log that keeps what the port logs, mutexes of pthreads, and tasks that are threads. The console is
 * stdout, sent to a file the case reads back; another thread can hold it (flockfile), as another task's long reply
 * holds the device's console.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apps_alarm.h"
#include "apps_echo.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_store.h"
#include "apps_task.h"
#include "muse_apps_task_fakes.h"
#include "muse_sntp.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#include "psa/crypto.h"

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL line %d: ", __LINE__);       \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
            failures++;                                        \
        }                                                      \
    } while (0)

/* ---- The clock: what the case sets ---- */

static int64_t now_us;
static void (*on_clock_read)(void);  /* a case's: something that happens as the port reads the clock */

int64_t esp_timer_get_time(void) {
    if (on_clock_read != NULL) {
        on_clock_read();
    }
    return now_us;
}

static void clock_at_ms(int64_t ms) {
    now_us = ms * 1000;
}

/* ---- The log: what the port logs at I, W and E ---- */

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char logged[16][400];
static int n_logged;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    if (level == 'D') {
        return;
    }
    char text[360];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mu);
    if (n_logged < (int)(sizeof logged / sizeof logged[0])) {
        snprintf(logged[n_logged++], sizeof logged[0], "%c %s: %s", level, tag, text);
    }
    pthread_mutex_unlock(&log_mu);
}

static void dump_log(void) {
    for (int i = 0; i < n_logged; i++) {
        fprintf(stderr, "  log %d: %s\n", i, logged[i]);
    }
}

/* ---- The heap ---- */

/* The blocks the port holds (made, not freed), so that a case can show that nothing leaks; and a case can make the next
 * allocation but some fail (the Nth next: 0 is the very next). */
static atomic_int blocks_held;
static int alloc_fail_in = -1;
static atomic_int internal_requests;  /* heap_caps_malloc calls for internal RAM, and the size of the last */
static atomic_size_t internal_size;

static int live_blocks(void) {
    return atomic_load(&blocks_held);
}

static bool alloc_fails(void) {
    if (alloc_fail_in < 0) {
        return false;
    }
    if (alloc_fail_in == 0) {
        alloc_fail_in = -1;
        return true;
    }
    alloc_fail_in--;
    return false;
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    if (ptr == NULL && alloc_fails()) {
        return NULL;
    }
    void *block = realloc(ptr, size);
    if (ptr == NULL && block != NULL) {
        atomic_fetch_add(&blocks_held, 1);
    }
    return block;
}

void *heap_caps_malloc(size_t size, uint32_t caps) {
    if ((caps & MALLOC_CAP_INTERNAL) != 0) {
        atomic_fetch_add(&internal_requests, 1);
        atomic_store(&internal_size, size);
    }
    if (alloc_fails()) {
        return NULL;
    }
    void *block = malloc(size);
    if (block != NULL) {
        atomic_fetch_add(&blocks_held, 1);
    }
    return block;
}

size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return (size_t)3 << 20;
}

void heap_caps_free(void *ptr) {
    if (ptr != NULL) {
        atomic_fetch_sub(&blocks_held, 1);
    }
    free(ptr);
}

/* ---- FreeRTOS: tasks are threads, a mutex is a pthread mutex and condition ---- */

struct fake_task {
    int number;
};

static struct fake_task tasks[3] = {{0}, {1}, {2}};
static _Thread_local struct fake_task *this_task = &tasks[0];

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return this_task;
}

struct fake_mutex {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool held;
    struct fake_task *holder;
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)(void *)buffer->opaque;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->held = false;
    m->holder = NULL;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    pthread_mutex_lock(&m->mu);
    if (m->held && m->holder == this_task) {
        pthread_mutex_unlock(&m->mu);
        CHECK(false, "the port took the mutex again from the task that holds it: FreeRTOS would block for good");
        return pdFALSE;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(wait / 1000);
    deadline.tv_nsec += (long)(wait % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (m->held) {
        if (wait == 0 ||
            (wait != portMAX_DELAY && pthread_cond_timedwait(&m->cv, &m->mu, &deadline) == ETIMEDOUT && m->held)) {
            pthread_mutex_unlock(&m->mu);
            return pdFALSE;
        }
        if (wait == portMAX_DELAY) {
            pthread_cond_wait(&m->cv, &m->mu);
        }
    }
    m->held = true;
    m->holder = this_task;
    pthread_mutex_unlock(&m->mu);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    pthread_mutex_lock(&m->mu);
    bool mine = m->held && m->holder == this_task;
    if (mine) {
        m->held = false;
        m->holder = NULL;
        pthread_cond_broadcast(&m->cv);
    }
    pthread_mutex_unlock(&m->mu);
    CHECK(mine, "the port gave the mutex from a task that doesn't hold it: FreeRTOS would assert");
    return mine ? pdTRUE : pdFALSE;
}

/* ---- PSA ---- */

psa_status_t psa_crypto_init(void) {
    return PSA_SUCCESS;
}

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length) {
    (void)alg;
    (void)input;
    (void)input_length;
    memset(hash, 0xab, hash_size < 32 ? hash_size : 32);
    *hash_length = 32;
    return PSA_SUCCESS;
}

/* ---- What the port asks of the UI and of the SNTP helper ---- */

static bool ui_is_dark, clock_is_set;

bool muse_ui_dark(void) {
    return ui_is_dark;
}

bool muse_time_valid(void) {
    return clock_is_set;
}

/* ---- What the port asks of the alarms (apps_alarm.c's own cases are tests/muse_apps_alarm_harness.c) and of the voice task ---- */

static struct {
    char app[32], name[32];
    int64_t ms;
    bool sound, wake;
    int arms, cancels, asks, lists, stops, chirps;
    apps_alarm_result_t answer;
} alarm_calls;

apps_alarm_result_t apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    snprintf(alarm_calls.app, sizeof alarm_calls.app, "%s", app);
    snprintf(alarm_calls.name, sizeof alarm_calls.name, "%s", name);
    alarm_calls.ms = ms;
    alarm_calls.sound = sound;
    alarm_calls.wake = wake;
    alarm_calls.arms++;
    return alarm_calls.answer;
}

void apps_alarm_cancel(const char *app, const char *name) {
    snprintf(alarm_calls.app, sizeof alarm_calls.app, "%s", app);
    snprintf(alarm_calls.name, sizeof alarm_calls.name, "%s", name);
    alarm_calls.cancels++;
}

int64_t apps_alarm_remaining(const char *app, const char *name) {
    snprintf(alarm_calls.app, sizeof alarm_calls.app, "%s", app);
    snprintf(alarm_calls.name, sizeof alarm_calls.name, "%s", name);
    alarm_calls.asks++;
    return 4321;
}

int apps_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    snprintf(alarm_calls.app, sizeof alarm_calls.app, "%s", app);
    snprintf(names[0], 24, "first");
    snprintf(names[1], 24, "second");
    remaining[0] = 10;
    remaining[1] = 20;
    alarm_calls.lists++;
    return 2;
}

void apps_alarm_ring_stop(void) {
    alarm_calls.stops++;
}

void muse_voice_request_chirp(void) {
    alarm_calls.chirps++;
}

/* ---- The I/O worker's ops (apps_io.h): queued here, and run by the case, or by a caller that sleeps, in the order queued ---- */

typedef struct {
    void (*op)(void *arg);
    void *arg;
} queued_op_t;

static queued_op_t pending[APPS_IO_OPS];  /* the device's ring: a ninth op is refused there, and here */
static int n_pending, ops_ran, submit_calls;
static bool ops_refused;     /* the worker takes no more ops */
static bool worker_answers;  /* the worker runs what waits whenever the caller sleeps (vTaskDelay) */

bool apps_io_submit_op(void (*op)(void *arg), void *arg) {
    submit_calls++;
    if (ops_refused || n_pending == (int)(sizeof pending / sizeof pending[0])) {
        return false;
    }
    pending[n_pending++] = (queued_op_t){op, arg};
    return true;
}

/* The worker runs up to n of the ops that wait, one at a time, in the order they were queued. */
static void worker_runs(int n) {
    struct fake_task *caller = this_task;
    this_task = &tasks[1];  /* the worker is another task */
    while (n-- > 0 && n_pending > 0) {
        queued_op_t next = pending[0];
        memmove(pending, pending + 1, (size_t)(n_pending - 1) * sizeof pending[0]);
        n_pending--;
        next.op(next.arg);
        ops_ran++;
    }
    this_task = caller;
}

/* A sleep moves the clock by what it asks (nothing really waits), and the worker, if the case lets it, answers meanwhile. */
void vTaskDelay(const TickType_t ticks) {
    now_us += (int64_t)ticks * 1000000 / configTICK_RATE_HZ;
    if (worker_answers) {
        worker_runs(1 << 20);
    }
}

/* ---- What the port asks of the system's power and Wi-Fi, and of the app task ---- */

static muse_power_t fake_power;
static muse_wifi_status_t fake_wifi;
static int shows, exits, keep_awake_calls;
static bool keep_awake_last;

muse_power_t muse_state_power(void) {
    return fake_power;
}

void muse_wifi_status(muse_wifi_status_t *out) {
    *out = fake_wifi;
}

void apps_task_show(void) {
    shows++;
}

void apps_task_keep_awake(bool on) {
    keep_awake_calls++;
    keep_awake_last = on;
}

void apps_task_exit(void) {
    exits++;
}

static int restamps;
static int64_t restamp_at_ms;

void apps_task_call_restart(void) {
    restamps++;
    restamp_at_ms = now_us / 1000;
}

/* ---- The console: stdout, into a file ---- */

static char console_path[512];

static void console_start(void) {
    const char *dir = getenv("TMPDIR");
    snprintf(console_path, sizeof console_path, "%s/muse_port_console_XXXXXX", dir != NULL && dir[0] != '\0' ? dir : "/tmp");
    int fd = mkstemp(console_path);
    if (fd < 0 || freopen(console_path, "w", stdout) == NULL) {
        fprintf(stderr, "FAIL: no console file\n");
        exit(1);
    }
    close(fd);
}

/* What the console has had since console_start, as one string (the caller frees it). */
static char *console_text(void) {
    fflush(stdout);
    FILE *f = fopen(console_path, "r");
    if (f == NULL) {
        fprintf(stderr, "FAIL: the console file is gone\n");
        exit(1);
    }
    size_t cap = 1 << 16, n = 0;
    char *text = malloc(cap);
    if (text == NULL) {
        exit(1);
    }
    n = fread(text, 1, cap - 1, f);
    text[n] = '\0';
    fclose(f);
    return text;
}

static void console_end(void) {
    unlink(console_path);
}

/* apps_port_console_flush ends each line with fsync(fileno(stdout)). The console here is a file, which needs no sync, so
 * this fsync stands in for the system's (the port's calls bind to it): a case can act between two lines of a drain, at
 * the point where a drain that let the console go after each line would have let it go. */
static void (*between_lines)(void);

int fsync(int fd) {
    (void)fd;
    if (between_lines != NULL) {
        between_lines();
    }
    return 0;
}

/* ---- Cases ---- */

/* F8: the bucket's arithmetic (apps_echo.h). It starts full, earns APPS_ECHO_RATE bytes a second to the thousandth of
 * a byte and never holds more than the burst; a take it can't cover takes nothing; a clock that stands still or goes
 * back earns nothing. */
static void echo_bucket(void) {
    CHECK(APPS_ECHO_RATE == 1024 && APPS_ECHO_BURST == 1024, "the ruling's numbers: %u bytes a second, a %u-byte burst",
          APPS_ECHO_RATE, APPS_ECHO_BURST);
    apps_echo_t b = {0};
    CHECK(apps_echo_take(&b, 5000, 1024), "it starts full");
    CHECK(!apps_echo_take(&b, 5000, 1), "and the burst empties it");
    CHECK(apps_echo_take(&b, 5000, 0), "nothing costs nothing");
    CHECK(apps_echo_take(&b, 5125, 128), "125 ms earn 128 bytes");
    CHECK(!apps_echo_take(&b, 5125, 1), "and no more");
    CHECK(!apps_echo_take(&b, 5126, 2), "1 ms earns 1.024 bytes, not 2");
    CHECK(apps_echo_take(&b, 5126, 1), "but 1");
    CHECK(apps_echo_take(&b, 5167, 42), "the thousandths add up: 0.024 + 41.984 bytes");
    CHECK(!apps_echo_take(&b, 5167, 1), "0.008 bytes are left");
    CHECK(apps_echo_take(&b, 9000, 1024), "a quiet while fills it");
    CHECK(!apps_echo_take(&b, 9000, 1), "to the burst, and no more");
    CHECK(!apps_echo_take(&b, 8000, 1), "a clock that goes back earns nothing");
    CHECK(!apps_echo_take(&b, 9000, 1), "nor does it, coming back to where it was");
    CHECK(apps_echo_take(&b, 9001, 1), "time past that earns again");
    apps_echo_t c = {0};
    CHECK(!apps_echo_take(&c, 0, APPS_ECHO_BURST + 1), "a take above the burst never fits");
    CHECK(apps_echo_take(&c, 0, APPS_ECHO_BURST), "and a take that fails takes nothing");
    CHECK(apps_echo_take(&c, INT64_MAX / 2, APPS_ECHO_BURST), "a gap of any length fills it, and no more");
    CHECK(!apps_echo_take(&c, INT64_MAX / 2, 1), "a gap of any length fills it, and no more");
}

/* F8: the console echo takes at most 1,024 bytes a second, from a 1,024-byte burst. A line's bytes are what the console
 * sends: "@app.log <id> <line>" and CRLF. A line that doesn't fit isn't echoed and is counted; the next echoed line comes
 * after "@app.log <id> (<N> lines not echoed)", which takes its bytes from the bucket too. */
static void echo_lines(void) {
    console_start();
    apps_port_init();
    char x200[201];
    memset(x200, 'x', 200);
    x200[200] = '\0';
    char expect[4096] = "";
    char line[300];
    snprintf(line, sizeof line, "@app.log hello %s\n", x200);  /* 9 + 5 + 1 + 200 + 2 = 217 bytes */
    /* The burst: four lines (868 bytes) fit, the fifth and sixth don't. 156 bytes are left. */
    clock_at_ms(10000);
    for (int i = 0; i < 6; i++) {
        apps_port_log("hello", x200);
    }
    apps_port_log_drain();  /* the app task's, after the call */
    for (int i = 0; i < 4; i++) {
        strcat(expect, line);
    }
    /* 60 ms later, 156 + 61.44 bytes: the line (217) would fit, but not with its note (37 + 217): not echoed either. */
    clock_at_ms(10060);
    apps_port_log("hello", x200);
    apps_port_log_drain();
    /* 100 ms after the burst, 258.4 bytes: the note and the line (254) fit, and 4.4 bytes are left. */
    clock_at_ms(10100);
    apps_port_log("hello", x200);
    strcat(expect, "@app.log hello (3 lines not echoed)\n");
    strcat(expect, line);
    apps_port_log("hello", "bye");  /* 20 bytes: not echoed */
    apps_port_log_drain();
    /* A quiet second fills the bucket, and no more: the note and the line (37 + 21), then four long lines (868); the
     * fifth doesn't fit in the 98 bytes left. */
    clock_at_ms(12000);
    apps_port_log("hello", "back");
    strcat(expect, "@app.log hello (1 lines not echoed)\n@app.log hello back\n");
    for (int i = 0; i < 5; i++) {
        apps_port_log("hello", x200);
    }
    apps_port_log_drain();
    for (int i = 0; i < 4; i++) {
        strcat(expect, line);
    }
    apps_port_log_drain();  /* nothing more */
    char *got = console_text();
    CHECK(strcmp(got, expect) == 0, "the console:\n%s\nexpected:\n%s", got, expect);
    free(got);
    console_end();
}

/* Another task's long reply: it holds the console (stdout's lock) until the case lets it go. */
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static bool console_held, console_release;

static void *hold_the_console(void *arg) {
    (void)arg;
    flockfile(stdout);
    pthread_mutex_lock(&hold_mu);
    console_held = true;
    pthread_cond_broadcast(&hold_cv);
    while (!console_release) {
        pthread_cond_wait(&hold_cv, &hold_mu);
    }
    pthread_mutex_unlock(&hold_mu);
    funlockfile(stdout);
    return NULL;
}

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* F3: apps_port_log never waits for the console: it only queues (the app task is inside a call, its wall clock
 * running). With the console held by another thread it returns at once (a print that waited would never return, and
 * the alarm would end the case); the drain, after the call, prints the queue once the console is free. */
static void echo_queues_until_the_drain(void) {
    console_start();
    apps_port_init();
    clock_at_ms(20000);
    pthread_t holder;
    CHECK(pthread_create(&holder, NULL, hold_the_console, NULL) == 0, "no thread");
    pthread_mutex_lock(&hold_mu);
    while (!console_held) {
        pthread_cond_wait(&hold_cv, &hold_mu);
    }
    pthread_mutex_unlock(&hold_mu);
    alarm(5);
    int64_t t0 = wall_ms();
    apps_port_log_drain();  /* nothing queued: the app task's housekeeping never waits for the console */
    apps_port_log("hello", "one");
    apps_port_log("hello", "two");
    int64_t took = wall_ms() - t0;
    alarm(0);
    CHECK(took < 100, "an empty drain and two prints took %lld ms with the console held", (long long)took);
    pthread_mutex_lock(&hold_mu);
    console_release = true;
    pthread_cond_broadcast(&hold_cv);
    pthread_mutex_unlock(&hold_mu);
    pthread_join(holder, NULL);
    char *got = console_text();
    CHECK(strcmp(got, "") == 0, "printed before the drain: %s", got);
    free(got);
    apps_port_log_drain();
    got = console_text();
    CHECK(strcmp(got, "@app.log hello one\n@app.log hello two\n") == 0, "the drain printed:\n%s", got);
    free(got);
    console_end();
}

/* F3: the queue holds what the bucket lets a call echo, APPS_ECHO_QUEUE bytes as the console sends them (CRLF): a line
 * past that isn't echoed, counted as one past the bucket is. Three seconds of four long lines (217 bytes each) with no
 * drain: 1,953 bytes fit, the next three lines don't; after the drain, the next line comes after its note. */
static void echo_queue_is_bounded(void) {
    console_start();
    apps_port_init();
    CHECK(APPS_ECHO_QUEUE == APPS_ECHO_BURST + APPS_ECHO_RATE, "a burst and a second of the rate: %u", APPS_ECHO_QUEUE);
    char x200[201], line[300], expect[4096] = "";
    memset(x200, 'x', 200);
    x200[200] = '\0';
    snprintf(line, sizeof line, "@app.log hello %s\n", x200);
    for (int second = 0; second < 3; second++) {
        clock_at_ms(30000 + 1000 * second);
        for (int i = 0; i < 4; i++) {
            apps_port_log("hello", x200);
        }
    }
    for (int i = 0; i < 9; i++) {
        strcat(expect, line);
    }
    apps_port_log_drain();
    apps_port_log("hello", "after");
    apps_port_log_drain();
    strcat(expect, "@app.log hello (3 lines not echoed)\n@app.log hello after\n");
    char *got = console_text();
    CHECK(strcmp(got, expect) == 0, "the console:\n%s\nexpected:\n%s", got, expect);
    free(got);
    console_end();
}

/* Another task that keeps printing long lines (back-to-back status replies): it takes the console, holds it for
 * CONTEND_HOLD_MS, gives it back, and takes it again CONTEND_GAP_MS later, until the case stops it. Each time it has the
 * console it notes whether a drain was part-way through its lines. */
#define CONTEND_HOLD_MS 20
#define CONTEND_GAP_MS 5
#define DRAIN_LINES 8
#define OTHER_LINE "@app {\"op\":\"status\"} (another task's long reply)\n"

static atomic_bool contend_stop, contender_holding;
static atomic_int contender_lines, drained_lines, held_mid_drain;

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *contend_for_the_console(void *arg) {
    (void)arg;
    while (!atomic_load(&contend_stop)) {
        flockfile(stdout);
        int done = atomic_load(&drained_lines);
        if (done > 0 && done < DRAIN_LINES) {
            atomic_fetch_add(&held_mid_drain, 1);
        }
        fputs(OTHER_LINE, stdout);
        atomic_fetch_add(&contender_lines, 1);
        atomic_store(&contender_holding, true);
        sleep_ms(CONTEND_HOLD_MS);
        atomic_store(&contender_holding, false);
        funlockfile(stdout);
        sleep_ms(CONTEND_GAP_MS);
    }
    return NULL;
}

/* Between two lines of the drain: time enough for the other task to take the console, if the drain lets it go. */
static void a_line_was_drained(void) {
    atomic_fetch_add(&drained_lines, 1);
    sleep_ms(CONTEND_HOLD_MS + CONTEND_GAP_MS);
}

/* Task 13's N1: a drain holds the console once, for its whole queue. While another task keeps taking the console for
 * long lines, a drain of many lines waits once, behind the line in progress, then prints all of its lines with nothing
 * between them: the other task never has the console between the drain's first line and its last. (A drain that took
 * the console again for each line would wait behind one long line per line: on the gadget, the app task at priority 2
 * loses the console to the serial task at priority 5 every time.) */
static void echo_drain_holds_the_console(void) {
    console_start();
    apps_port_init();
    clock_at_ms(40000);
    char want[512] = "";
    for (int i = 1; i <= DRAIN_LINES; i++) {
        char text[16], line[64];
        snprintf(text, sizeof text, "line %d", i);
        apps_port_log("hello", text);
        snprintf(line, sizeof line, "@app.log hello %s\n", text);
        strcat(want, line);
    }
    pthread_t other;
    CHECK(pthread_create(&other, NULL, contend_for_the_console, NULL) == 0, "no thread");
    while (!atomic_load(&contender_holding)) {
        sleep_ms(1);
    }
    alarm(10);
    between_lines = a_line_was_drained;
    apps_port_log_drain();  /* behind the other task's line in progress */
    between_lines = NULL;
    alarm(0);
    atomic_store(&contend_stop, true);
    pthread_join(other, NULL);
    CHECK(atomic_load(&drained_lines) == DRAIN_LINES, "%d of %d lines drained", atomic_load(&drained_lines), DRAIN_LINES);
    CHECK(atomic_load(&held_mid_drain) == 0, "the other task had the console %d times between the drain's first line and "
          "its last", atomic_load(&held_mid_drain));
    CHECK(atomic_load(&contender_lines) >= 2, "the other task printed %d lines: it should have kept at it",
          atomic_load(&contender_lines));
    char *got = console_text();
    const char *first = strstr(got, "@app.log hello line 1\n");
    CHECK(first != NULL && strncmp(first, want, strlen(want)) == 0, "the drain's lines, one after another:\n%s", got);
    CHECK(first == got + strlen(OTHER_LINE) && strncmp(got, OTHER_LINE, strlen(OTHER_LINE)) == 0,
          "the drain waited once, behind the one line in progress:\n%s", got);
    free(got);
    console_end();
}

static void *give_from_another_task(void *arg) {
    (void)arg;
    this_task = &tasks[1];
    apps_port_ui_unlock();  /* a give by a task that doesn't hold it: skipped and logged */
    apps_port_ui_unlock();  /* the same kind again: skipped, not logged again */
    return NULL;
}

static void *take_from_another_task(void *arg) {
    this_task = &tasks[2];
    *(bool *)arg = apps_port_ui_trylock(1000);
    if (*(bool *)arg) {
        apps_port_ui_unlock();
    }
    return NULL;
}

/* F11: each kind of breach of the UI lock's contract is logged once, naming its kind, however many of another kind came
 * first: a re-take by its holder (lock or trylock), a give by a task that doesn't hold it, and heap use under the lock
 * (realloc or free). None blocks, and the lock still works for other tasks after them. */
static void breach_kinds(void) {
    apps_port_init();
    apps_port_ui_lock();
    apps_port_ui_lock();  /* a re-take by its holder: skipped, logged */
    CHECK(!apps_port_ui_trylock(10), "a re-try by its holder fails");  /* the same kind: not logged again */
    void *block = apps_port_realloc(NULL, 16);  /* heap use under the lock: logged */
    CHECK(block != NULL, "the realloc still happens");
    apps_port_free(block);  /* the same kind: not logged again */
    pthread_t other;
    CHECK(pthread_create(&other, NULL, give_from_another_task, NULL) == 0, "no thread");
    pthread_join(other, NULL);
    apps_port_ui_unlock();  /* its holder gives it back: no breach */
    bool took = false;
    CHECK(pthread_create(&other, NULL, take_from_another_task, &took) == 0, "no thread");
    pthread_join(other, NULL);
    CHECK(took, "another task takes the lock after the breaches");
    static const char *const want[] = {
        "E muse_apps: UI lock breach, re-take: taken again by its holder (each kind is logged once)",
        "E muse_apps: UI lock breach, heap use under the lock: the heap was used with the lock held (each kind is logged once)",
        "E muse_apps: UI lock breach, give by a non-holder: given back by a task that doesn't hold it (each kind is logged once)",
    };
    CHECK(n_logged == 3, "%d lines logged, not one for each of the 3 kinds", n_logged);
    for (int i = 0; i < 3 && i < n_logged; i++) {
        CHECK(strcmp(logged[i], want[i]) == 0, "log %d: '%s', not '%s'", i, logged[i], want[i]);
    }
}

/* The screen is dark when the UI says so, and the port keeps no flag of its own: it is read at each call. */
static void dark_follows_the_ui(void) {
    ui_is_dark = false;
    CHECK(!apps_port_dark(), "dark while the UI is lit");
    ui_is_dark = true;
    CHECK(apps_port_dark(), "lit while the UI is dark");
    ui_is_dark = false;
    CHECK(!apps_port_dark(), "dark again after the wake");
}

/* The wall clock counts when muse_time_valid() says so and not before, whatever the system clock reads (the host's reads
 * the real time, long after 2025): then it is the system clock, in milliseconds. */
static void the_wall_clock_counts_when_the_helper_says(void) {
    int64_t ms = 123;
    clock_is_set = false;
    CHECK(!apps_port_epoch_ms(&ms) && ms == 123, "an unset clock gave %lld", (long long)ms);
    clock_is_set = true;
    struct timespec before, after;
    clock_gettime(CLOCK_REALTIME, &before);
    CHECK(apps_port_epoch_ms(&ms), "a set clock gave nothing");
    clock_gettime(CLOCK_REALTIME, &after);
    int64_t low = (int64_t)before.tv_sec * 1000 + before.tv_nsec / 1000000;
    int64_t high = (int64_t)after.tv_sec * 1000 + after.tv_nsec / 1000000;
    CHECK(ms >= low - 1 && ms <= high + 1, "%lld is not between %lld and %lld", (long long)ms, (long long)low,
          (long long)high);
}

/* The alarms and the sound are other modules' (apps_alarm.c, the voice task): the port passes what it is given on, and what it is
 * told back, and a chirp is the voice task's request and a stop is the ring's. */
static void alarms_and_sound_go_to_their_owners(void) {
    alarm_calls.answer = APPS_ALARM_ARMED;
    CHECK(apps_port_alarm_arm("clock", "wake_up", 90000, true, false) == APPS_ALARM_ARMED && alarm_calls.arms == 1, "arm");
    CHECK(strcmp(alarm_calls.app, "clock") == 0 && strcmp(alarm_calls.name, "wake_up") == 0 && alarm_calls.ms == 90000 &&
              alarm_calls.sound && !alarm_calls.wake,
          "arm passed %s/%s %lld %d %d", alarm_calls.app, alarm_calls.name, (long long)alarm_calls.ms, alarm_calls.sound,
          alarm_calls.wake);
    alarm_calls.answer = APPS_ALARM_REFUSED;
    CHECK(apps_port_alarm_arm("clock", "x", 1, false, true) == APPS_ALARM_REFUSED && alarm_calls.arms == 2 && !alarm_calls.sound &&
              alarm_calls.wake,
          "a refusal is told");
    alarm_calls.answer = APPS_ALARM_NO_MEMORY;  /* and so is the other kind of refusal (G): the binding says which */
    CHECK(apps_port_alarm_arm("clock", "y", 1, true, true) == APPS_ALARM_NO_MEMORY && alarm_calls.arms == 3, "a want of memory is told"); 
    apps_port_alarm_cancel("clock", "wake_up");
    CHECK(alarm_calls.cancels == 1 && strcmp(alarm_calls.name, "wake_up") == 0, "cancel");
    CHECK(apps_port_alarm_remaining("clock", "t") == 4321 && alarm_calls.asks == 1 && strcmp(alarm_calls.name, "t") == 0, "remaining");
    char names[4][24] = {{0}};
    int64_t left[4] = {0};
    CHECK(apps_port_alarm_list("clock", names, left) == 2 && strcmp(names[1], "second") == 0 && left[0] == 10 && left[1] == 20 &&
              strcmp(alarm_calls.app, "clock") == 0,
          "list");
    CHECK(alarm_calls.chirps == 0 && alarm_calls.stops == 0, "nothing sounded yet");
    apps_port_sound_chirp();
    CHECK(alarm_calls.chirps == 1 && alarm_calls.stops == 0, "one chirp is one request to the voice task");
    apps_port_sound_stop();
    CHECK(alarm_calls.stops == 1 && alarm_calls.chirps == 1, "stop is the ring's");
}


/* ---- The store (apps_port.h): the I/O worker's map of each app's object ---- */

/* Loads app's object and checks it: want, whole, with a NUL after its bytes, in a block the caller frees. */
static void expect_load(const char *app, const char *want, const char *what) {
    size_t len = 99;
    char *got = apps_port_store_load(app, &len);
    CHECK(got != NULL, "%s: the load of %s gave nothing", what, app);
    if (got == NULL) {
        return;
    }
    CHECK(len == strlen(want) && memcmp(got, want, len) == 0 && got[len] == '\0',
          "%s: the load of %s gave %zu bytes '%.60s', not '%.60s' with a NUL after", what, app, len, got, want);
    apps_port_free(got);
}

/* An app with nothing saved reads "{}". A save only queues: the caller doesn't wait for the worker, and its own bytes are its
 * own again at once. The worker writes it, and a load, which sleeps in 10 ms steps until the worker answers, reads the object
 * whole. A second save replaces the first, an app's object is its own, and nothing is held but one node for each app. */
static void store_round_trip_through_the_worker(void) {
    worker_answers = true;
    int before = live_blocks();
    expect_load("clock", "{}", "nothing saved");
    CHECK(ops_ran == 1 && n_logged == 0, "a load of an app with nothing is one op and no log: %d ops, %d lines", ops_ran, n_logged);
    char buf[] = "{\"a\":1}";
    CHECK(apps_port_store_save("clock", buf, strlen(buf)), "the save is queued");
    CHECK(n_pending == 1 && ops_ran == 1, "a save only queues: %d waiting, %d run", n_pending, ops_ran);
    memset(buf, 'X', sizeof buf - 1);  /* the caller's bytes are copied before the save returns */
    expect_load("clock", "{\"a\":1}", "the saved object");
    CHECK(n_pending == 0 && ops_ran == 3, "the load's wait ran the save and then the load: %d waiting, %d run", n_pending, ops_ran);
    CHECK(apps_port_store_save("clock", "{\"a\":2,\"b\":\"x\"}", 15), "the second save is queued");
    CHECK(apps_port_store_save("timer", "{}", 2), "another app's save is queued");
    expect_load("clock", "{\"a\":2,\"b\":\"x\"}", "the second save is the object");
    expect_load("timer", "{}", "another app's object is its own");
    expect_load("notes", "{}", "an app that never saved");
    CHECK(live_blocks() - before == 2, "%d blocks held, not the 2 nodes of the 2 apps", live_blocks() - before);
    CHECK(n_logged == 0, "%d lines logged for what went right", n_logged);
}

/* The worker runs the ops in the order the calls were made: the last of three saves, queued ahead of a load, is what it reads. */
static void store_ops_run_in_the_order_of_the_calls(void) {
    worker_answers = true;
    CHECK(apps_port_store_save("clock", "{\"n\":1}", 7) && apps_port_store_save("clock", "{\"n\":2}", 7) &&
              apps_port_store_save("clock", "{\"n\":3}", 7),
          "three saves are queued");
    CHECK(n_pending == 3 && ops_ran == 0, "all three wait: %d waiting, %d run", n_pending, ops_ran);
    expect_load("clock", "{\"n\":3}", "the load comes after the saves");
    CHECK(n_pending == 0 && ops_ran == 4, "%d waiting, %d run", n_pending, ops_ran);
}

/* A load that the worker doesn't answer waits APPS_STORE_WAIT_MS and no longer (to the poll's 10 ms), gives nothing, and says so in one
 * log line that names the app and no values. The request it leaves behind is the worker's to free: when it does come to the op, it
 * frees the request and its answer, and the store works as before. */
static void store_load_gives_up_when_the_worker_is_silent(void) {
    worker_answers = false;
    clock_at_ms(5000);
    int before = live_blocks();
    size_t len = 77;
    char *got = apps_port_store_load("clock", &len);
    int64_t waited = now_us / 1000 - 5000;
    CHECK(got == NULL && len == 0, "a silent worker gave %p and length %zu", (void *)got, len);
    CHECK(waited >= APPS_STORE_WAIT_MS && waited <= APPS_STORE_WAIT_MS + 10, "waited %lld ms for a bound of %d", (long long)waited,
          APPS_STORE_WAIT_MS);
    CHECK(n_logged == 1 && strcmp(logged[0], "W muse_apps: store: no answer from the I/O worker for clock within 1000 ms: launch refused") == 0,
          "%d lines logged, the first '%s'", n_logged, n_logged > 0 ? logged[0] : "");
    CHECK(n_pending == 1 && live_blocks() - before == 1, "the op waits with its request: %d waiting, %d blocks held", n_pending,
          live_blocks() - before);
    worker_runs(1);  /* the worker comes to it at last */
    CHECK(live_blocks() == before, "%d blocks held after the late answer: the request and its answer are freed", live_blocks() - before);
    worker_answers = true;
    expect_load("clock", "{}", "after the late answer");
}

static int64_t answer_from_us;

/* The worker answers right after the loader has read the clock and found the bound passed, before it marks the request abandoned. */
static void answer_as_the_bound_passes(void) {
    if (now_us >= answer_from_us) {
        on_clock_read = NULL;
        worker_runs(1);
    }
}

/* The answer that lands as the wait runs out is kept, not lost to the abandon: the mark that was made first decides, and the
 * request is freed once, by the loader. */
static void store_load_answered_at_the_deadline(void) {
    worker_answers = false;
    clock_at_ms(5000);
    int before = live_blocks();
    answer_from_us = (5000 + APPS_STORE_WAIT_MS) * 1000;
    on_clock_read = answer_as_the_bound_passes;
    expect_load("clock", "{}", "an answer at the deadline");
    CHECK(on_clock_read == NULL, "the answer was made at the deadline: the hook ran");
    CHECK(n_pending == 0 && live_blocks() == before, "%d waiting, %d blocks held", n_pending, live_blocks() - before);
    CHECK(n_logged == 0, "the load that was answered logged %d lines", n_logged);
}

/* What the port refuses or can't do, it says with nothing held: a worker that takes no op (a load fails at once, without a wait; a
 * save is kept for it, and says true), no memory for a copy or a request or an answer, an id or an object the runtime never makes.
 * APPS_STORE_MAX_BYTES (16 KB) is the most an object can be, to the byte. */
static void store_refusals(void) {
    worker_answers = true;
    clock_at_ms(5000);
    int before = live_blocks();
    size_t len = 55;
    ops_refused = true;
    char *got = apps_port_store_load("clock", &len);
    CHECK(got == NULL && len == 0 && now_us == 5000000, "a load the worker refused gave %p, length %zu, after %lld us", (void *)got, len,
          (long long)(now_us - 5000000));
    CHECK(live_blocks() == before && n_pending == 0, "%d blocks held, %d waiting after the refused load", live_blocks() - before, n_pending);
    CHECK(apps_port_store_save("clock", "{}", 2), "a save the worker refused is kept, and says true");
    CHECK(live_blocks() - before == 1 && n_pending == 0, "one block kept, nothing queued: %d blocks, %d waiting", live_blocks() - before, n_pending);
    ops_refused = false;
    apps_store_tick(100000);  /* the worker takes it now */
    worker_runs(1);
    CHECK(n_pending == 0 && live_blocks() - before == 1, "the worker's map holds it: %d blocks, %d waiting", live_blocks() - before, n_pending);
    before = live_blocks();  /* what the map holds isn't what the cases below count */
    alloc_fail_in = 0;
    CHECK(!apps_port_store_save("clock", "{}", 2) && n_pending == 0, "a save with no memory for its copy says false and queues nothing");
    alloc_fail_in = 0;
    got = apps_port_store_load("clock", &len);
    CHECK(got == NULL && len == 0 && n_pending == 0, "a load with no memory for its request gave %p and queued %d", (void *)got, n_pending);
    alloc_fail_in = 1;  /* the request is made, the worker's copy of the answer isn't */
    got = apps_port_store_load("clock", &len);
    CHECK(got == NULL && len == 0, "a load whose answer had no memory gave %p, length %zu", (void *)got, len);
    CHECK(live_blocks() == before && n_pending == 0, "%d blocks held after the no-memory cases", live_blocks() - before);
    char id[APPS_ID_MAX + 2];
    memset(id, 'a', sizeof id - 1);
    id[sizeof id - 1] = '\0';  /* one character more than an app id can have */
    got = apps_port_store_load(id, &len);
    CHECK(got == NULL && len == 0 && n_pending == 0, "a load of an id that long gave %p", (void *)got);
    CHECK(!apps_port_store_save(id, "{}", 2) && n_pending == 0, "a save for an id that long says false");
    id[APPS_ID_MAX] = '\0';  /* the longest there is */
    CHECK(apps_port_store_save(id, "{}", 2), "a save for the longest id is queued");
    worker_runs(1);
    char *big = malloc(APPS_STORE_MAX_BYTES + 2);
    CHECK(big != NULL, "no memory for the test");
    if (big == NULL) {
        return;
    }
    memset(big, 'x', APPS_STORE_MAX_BYTES + 1);
    big[APPS_STORE_MAX_BYTES + 1] = '\0';
    n_logged = 0;
    CHECK(!apps_port_store_save("clock", big, APPS_STORE_MAX_BYTES + 1), "one byte over the most says false");
    CHECK(n_logged == 1 && strstr(logged[0], "refused 16385 bytes of values for clock") != NULL, "the refusal is logged once, without the bytes: %d lines, '%s'",
          n_logged, n_logged > 0 ? logged[0] : "");
    CHECK(apps_port_store_save("clock", big, APPS_STORE_MAX_BYTES), "the most, exactly, is queued");
    big[APPS_STORE_MAX_BYTES] = '\0';
    expect_load("clock", big, "the most, exactly");
    free(big);
}

/* The map keeps STORE_APPS = 16 apps. A save for an app that is there replaces it and makes it the newest saved; one for a 17th app
 * drops the one saved longest ago (with a log line that names it, and no values), whatever was loaded since. */
static void store_keeps_16_apps_and_drops_the_oldest_saved(void) {
    worker_answers = true;
    int before = live_blocks();
    char app[16], object[24];
    for (int i = 0; i < 16; i++) {
        snprintf(app, sizeof app, "app%02d", i);
        snprintf(object, sizeof object, "{\"n\":%d}", i);
        CHECK(apps_port_store_save(app, object, strlen(object)), "save %d is queued", i);
        worker_runs(1);  /* the worker keeps up: the real queue holds 8 ops (APPS_IO_OPS) */
    }
    expect_load("app00", "{\"n\":0}", "the first of 16 is kept");  /* a load makes none newer */
    CHECK(n_logged == 0 && live_blocks() - before == 16, "%d lines logged and %d blocks held after 16 apps", n_logged, live_blocks() - before);
    CHECK(apps_port_store_save("app00", "{\"n\":100}", 9), "app00 saves again: it is the newest saved");
    worker_runs(1);
    CHECK(apps_port_store_save("app16", "{\"n\":16}", 8), "a 17th app saves");
    worker_runs(1);
    expect_load("app16", "{\"n\":16}", "the 17th");
    CHECK(n_logged == 1 && strcmp(logged[0], "W muse_apps: store: dropped the values of app01 to make room for another app's") == 0,
          "%d lines logged, the first '%s'", n_logged, n_logged > 0 ? logged[0] : "");
    expect_load("app01", "{}", "app01, saved longest ago, is dropped");
    expect_load("app00", "{\"n\":100}", "app00, saved again, is kept");
    for (int i = 2; i < 16; i++) {
        snprintf(app, sizeof app, "app%02d", i);
        snprintf(object, sizeof object, "{\"n\":%d}", i);
        expect_load(app, object, "the rest are kept");
    }
    CHECK(live_blocks() - before == 16, "%d blocks held, not the 16 nodes", live_blocks() - before);
}

/* The load's request is a small block of internal RAM, which no read-modify-write may reach in PSRAM, and it is freed once, by whoever finishes last. */
static void store_load_request_is_a_small_internal_block(void) {
    worker_answers = true;
    int before = live_blocks();
    int requests = atomic_load(&internal_requests);
    expect_load("clock", "{}", "a load");
    CHECK(atomic_load(&internal_requests) - requests == 1 && atomic_load(&internal_size) <= 64, "%d internal requests, the last of %zu bytes",
          atomic_load(&internal_requests) - requests, atomic_load(&internal_size));
    CHECK(live_blocks() == before, "%d blocks held after the load", live_blocks() - before);
    /* A worker that never answers: the request is the worker's to free when it comes to it. */
    worker_answers = false;
    clock_at_ms(5000);
    requests = atomic_load(&internal_requests);
    CHECK(apps_port_store_load("clock", &(size_t){0}) == NULL, "a silent worker");
    CHECK(atomic_load(&internal_requests) - requests == 1 && live_blocks() - before == 1, "the abandoned request is held: %d blocks", live_blocks() - before);
    worker_runs(1);
    CHECK(live_blocks() == before, "%d blocks held after the late answer", live_blocks() - before);
    n_logged = 0;
}

/* A save the worker refuses isn't lost: the port keeps it in PSRAM, says true (it has it), reads it back to a load before the worker has it,
 * replaces it with a newer save of the app, and offers it again at the tick, APPS_STORE_RETRY_MS after the refusal and no sooner. */
static void store_a_save_the_worker_refuses_is_kept(void) {
    worker_answers = false;
    clock_at_ms(10000);
    int before = live_blocks();
    ops_refused = true;
    CHECK(apps_port_store_save("clock", "{\"a\":1}", 7), "a refused save is kept: it says true");
    CHECK(n_pending == 0 && live_blocks() - before == 1, "nothing queued, one block kept: %d waiting, %d blocks", n_pending, live_blocks() - before);
    int calls = submit_calls;
    expect_load("clock", "{\"a\":1}", "a load reads what is kept");
    CHECK(submit_calls == calls && n_pending == 0, "the load asked the worker for nothing: %d submits", submit_calls - calls);
    CHECK(apps_port_store_save("clock", "{\"a\":2}", 7), "a newer save");
    CHECK(live_blocks() - before == 1, "it took the place of the first: %d blocks", live_blocks() - before);
    expect_load("clock", "{\"a\":2}", "the newer is what is kept");
    calls = submit_calls;
    apps_store_tick(11999);
    CHECK(submit_calls == calls, "offered again before the retry time: %d submits", submit_calls - calls);
    apps_store_tick(12000);
    CHECK(submit_calls - calls == 1 && n_pending == 0 && live_blocks() - before == 1, "offered at 12000 and refused again: %d submits, %d blocks",
          submit_calls - calls, live_blocks() - before);
    calls = submit_calls;
    apps_store_tick(13999);
    CHECK(submit_calls == calls, "offered 1999 ms after the second refusal: %d submits", submit_calls - calls);
    ops_refused = false;
    apps_store_tick(14000);
    CHECK(submit_calls - calls == 1 && n_pending == 1, "offered at 14000 and taken: %d submits, %d queued", submit_calls - calls, n_pending);
    worker_answers = true;
    worker_runs(1);
    CHECK(n_pending == 0 && live_blocks() - before == 1, "the worker has it, and the port keeps nothing: %d blocks", live_blocks() - before);
    calls = submit_calls;
    apps_store_tick(99999);
    CHECK(submit_calls == calls, "nothing is kept: no submit at the tick");
    expect_load("clock", "{\"a\":2}", "the worker's map has the newer");
    CHECK(n_logged == 0, "%d lines logged for what was kept", n_logged);
}

/* Each app's save is kept in a slot of its own, and there are as many as the map has apps (16): a 17th is refused (the runtime says so in the
 * log, and loses what it had). A tick offers them in turn and stops at the first refusal; the ring holds 8, so the rest wait for the next tick. */
static void store_kept_saves_are_one_for_each_app_and_bounded(void) {
    worker_answers = false;
    clock_at_ms(10000);
    int before = live_blocks();
    char app[16], object[24];
    ops_refused = true;
    for (int i = 0; i < 16; i++) {
        snprintf(app, sizeof app, "app%02d", i);
        snprintf(object, sizeof object, "{\"n\":%d}", i);
        CHECK(apps_port_store_save(app, object, strlen(object)), "save %d is kept", i);
    }
    CHECK(live_blocks() - before == 16, "%d blocks kept, wanted 16", live_blocks() - before);
    CHECK(!apps_port_store_save("app16", "{}", 2), "a 17th app's save can't be kept");
    CHECK(apps_port_store_save("app03", "{\"n\":33}", 8) && live_blocks() - before == 16, "a second save of an app takes its place");
    CHECK(live_blocks() - before == 16, "%d blocks kept after the 17th was refused", live_blocks() - before);
    ops_refused = false;
    apps_store_tick(12000);
    CHECK(n_pending == APPS_IO_OPS, "a tick offers them until the ring is full: %d queued of %d", n_pending, APPS_IO_OPS);
    worker_answers = true;
    worker_runs(1 << 20);
    CHECK(n_pending == 0, "the worker ran them");
    apps_store_tick(12001);
    CHECK(n_pending == 0, "the next offer waits for its time (%d ms): %d queued", APPS_STORE_RETRY_MS, n_pending);
    apps_store_tick(14000);
    CHECK(n_pending == 8, "the rest are offered at the next: %d queued", n_pending);
    worker_runs(1 << 20);
    expect_load("app00", "{\"n\":0}", "the first");
    expect_load("app03", "{\"n\":33}", "the one that was saved twice");
    expect_load("app15", "{\"n\":15}", "the last");
    CHECK(live_blocks() - before == 16, "the map's 16 nodes are all that is held: %d", live_blocks() - before);
}

/* The order of one app's saves is kept: one that the worker has taken is applied before a later one that waited, and a load that comes between
 * them is given the later (what is kept is the newer). */
static void store_a_kept_save_comes_after_the_one_the_worker_took(void) {
    worker_answers = false;
    clock_at_ms(10000);
    CHECK(apps_port_store_save("clock", "{\"n\":1}", 7), "the first is taken");
    CHECK(n_pending == 1, "queued");
    ops_refused = true;
    CHECK(apps_port_store_save("clock", "{\"n\":2}", 7), "the second is kept");
    worker_runs(1);  /* the worker applies the first */
    expect_load("clock", "{\"n\":2}", "a load between them is given the kept one");
    ops_refused = false;
    apps_store_tick(12000);
    worker_runs(1);
    worker_answers = true;
    expect_load("clock", "{\"n\":2}", "the later is the object");
    CHECK(n_pending == 0, "nothing waits");
}

/* The port begins the app task's call again once it has waited for the worker (apps_task_call_restart): what it waits for is the gadget's, not
 * the app's, and the supervisor must not count it. Once for a load, answered, silent or refused, after it has waited. */
static void store_load_begins_the_call_again(void) {
    worker_answers = true;
    clock_at_ms(5000);
    restamps = 0;
    expect_load("clock", "{}", "a load");
    CHECK(restamps == 1, "%d restarts for a load that was answered", restamps);
    worker_answers = false;
    clock_at_ms(5000);
    restamps = 0;
    CHECK(apps_port_store_load("clock", &(size_t){0}) == NULL, "a silent worker");
    CHECK(restamps == 1 && restamp_at_ms >= 5000 + APPS_STORE_WAIT_MS, "%d restarts, at %lld: after the wait of %d ms", restamps,
          (long long)restamp_at_ms, APPS_STORE_WAIT_MS);
    worker_runs(1);
    ops_refused = true;
    restamps = 0;
    CHECK(apps_port_store_load("clock", &(size_t){0}) == NULL, "a refused load");
    CHECK(restamps == 1, "%d restarts for a load that was refused", restamps);
    ops_refused = false;
    n_logged = 0;
}

/* sys.battery()'s numbers are muse_state's, a percentage clamped to 0 to 100 and -1 for none. sys.wifi() is connected only when the
 * station is joined, with that network's name, cut to the room the caller has (and never past the NUL a full-length name lacks), and
 * "" when it isn't, whatever name the status still holds. Nothing of it is logged. */
static void battery_and_wifi_are_the_systems(void) {
    fake_power = (muse_power_t){.battery_pct = 76, .battery_mv = 3900, .charging = true, .usb = true};
    apps_battery_t b = apps_port_battery();
    CHECK(b.pct == 76 && b.charging && b.usb, "battery %d %d %d", b.pct, b.charging, b.usb);
    fake_power = (muse_power_t){.battery_pct = -1};
    b = apps_port_battery();
    CHECK(b.pct == -1 && !b.charging && !b.usb, "no battery: %d %d %d", b.pct, b.charging, b.usb);
    fake_power = (muse_power_t){.battery_pct = 0, .charging = false, .usb = false};
    CHECK(apps_port_battery().pct == 0, "an empty battery is 0, not none");
    fake_power = (muse_power_t){.battery_pct = 250};
    CHECK(apps_port_battery().pct == 100, "a percentage over 100 is 100");
    fake_power = (muse_power_t){.battery_pct = -40};
    CHECK(apps_port_battery().pct == -1, "a negative percentage is none");

    char ssid[40];
    memset(&fake_wifi, 0, sizeof fake_wifi);
    fake_wifi.state = MUSE_WIFI_CONNECTED;
    snprintf(fake_wifi.ssid, sizeof fake_wifi.ssid, "muse-lab");
    memset(ssid, 'z', sizeof ssid);
    CHECK(apps_port_wifi(ssid, sizeof ssid) && strcmp(ssid, "muse-lab") == 0, "connected: '%s'", ssid);
    memset(ssid, 'z', sizeof ssid);
    CHECK(apps_port_wifi(ssid, 4) && strcmp(ssid, "mus") == 0 && ssid[4] == 'z', "cut to 4 bytes with the NUL, and nothing past them: '%s'", ssid);
    memset(ssid, 'z', sizeof ssid);
    CHECK(apps_port_wifi(ssid, 0) && ssid[0] == 'z', "no room: connected, nothing written");
    memset(fake_wifi.ssid, 'x', sizeof fake_wifi.ssid);  /* a name that fills the field, with no NUL: bytes from the air */
    memset(ssid, 'z', sizeof ssid);
    CHECK(apps_port_wifi(ssid, sizeof ssid) && strlen(ssid) == MUSE_SSID_MAX, "a full-length name is cut at its field: %zu bytes", strlen(ssid));
    static const muse_wifi_state_t NOT_JOINED[] = {MUSE_WIFI_OFF, MUSE_WIFI_NO_NETWORK, MUSE_WIFI_CONNECTING, MUSE_WIFI_FAILED, MUSE_WIFI_NOT_NEARBY};
    for (size_t i = 0; i < sizeof NOT_JOINED / sizeof NOT_JOINED[0]; i++) {
        fake_wifi.state = NOT_JOINED[i];
        snprintf(fake_wifi.ssid, sizeof fake_wifi.ssid, "muse-lab");  /* the status still holds the name it is trying */
        memset(ssid, 'z', sizeof ssid);
        CHECK(!apps_port_wifi(ssid, sizeof ssid) && ssid[0] == '\0', "state %d is not connected, and gives no name: '%s'", (int)NOT_JOINED[i], ssid);
    }
    CHECK(n_logged == 0, "%d lines logged for battery and Wi-Fi", n_logged);
}

/* sys.show, sys.keep_awake and sys.exit are the app task's: each goes there once, as called. */
static void show_keep_awake_and_exit_go_to_the_app_task(void) {
    CHECK(shows == 0 && exits == 0 && keep_awake_calls == 0, "nothing asked yet");
    apps_port_show();
    CHECK(shows == 1 && exits == 0 && keep_awake_calls == 0, "show: %d %d %d", shows, exits, keep_awake_calls);
    apps_port_keep_awake(true);
    CHECK(keep_awake_calls == 1 && keep_awake_last && shows == 1 && exits == 0, "keep_awake(true)");
    apps_port_keep_awake(false);
    CHECK(keep_awake_calls == 2 && !keep_awake_last, "keep_awake(false)");
    apps_port_exit();
    CHECK(exits == 1 && shows == 1 && keep_awake_calls == 2, "exit: %d %d %d", shows, exits, keep_awake_calls);
}

static const struct {
    const char *name;
    void (*run)(void);
} CASES[] = {
    {"echo_bucket", echo_bucket},
    {"echo_lines", echo_lines},
    {"echo_queues_until_the_drain", echo_queues_until_the_drain},
    {"echo_queue_is_bounded", echo_queue_is_bounded},
    {"echo_drain_holds_the_console", echo_drain_holds_the_console},
    {"breach_kinds", breach_kinds},
    {"dark_follows_the_ui", dark_follows_the_ui},
    {"the_wall_clock_counts_when_the_helper_says", the_wall_clock_counts_when_the_helper_says},
    {"alarms_and_sound_go_to_their_owners", alarms_and_sound_go_to_their_owners},
    {"store_round_trip_through_the_worker", store_round_trip_through_the_worker},
    {"store_ops_run_in_the_order_of_the_calls", store_ops_run_in_the_order_of_the_calls},
    {"store_load_gives_up_when_the_worker_is_silent", store_load_gives_up_when_the_worker_is_silent},
    {"store_load_answered_at_the_deadline", store_load_answered_at_the_deadline},
    {"store_refusals", store_refusals},
    {"store_keeps_16_apps_and_drops_the_oldest_saved", store_keeps_16_apps_and_drops_the_oldest_saved},
    {"store_load_request_is_a_small_internal_block", store_load_request_is_a_small_internal_block},
    {"store_a_save_the_worker_refuses_is_kept", store_a_save_the_worker_refuses_is_kept},
    {"store_kept_saves_are_one_for_each_app_and_bounded", store_kept_saves_are_one_for_each_app_and_bounded},
    {"store_a_kept_save_comes_after_the_one_the_worker_took", store_a_kept_save_comes_after_the_one_the_worker_took},
    {"store_load_begins_the_call_again", store_load_begins_the_call_again},
    {"battery_and_wifi_are_the_systems", battery_and_wifi_are_the_systems},
    {"show_keep_awake_and_exit_go_to_the_app_task", show_keep_awake_and_exit_go_to_the_app_task},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (strcmp(argv[1], CASES[i].name) == 0) {
            CASES[i].run();
            if (failures > 0) {
                dump_log();
                return 1;
            }
            return 0;
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
