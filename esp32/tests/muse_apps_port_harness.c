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
#include "apps_port.h"
#include "muse_apps_task_fakes.h"
#include "muse_sntp.h"
#include "muse_ui.h"
#include "muse_voice.h"
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

int64_t esp_timer_get_time(void) {
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

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(ptr, size);
}

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return (size_t)3 << 20;
}

void heap_caps_free(void *ptr) {
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
    bool answer;
} alarm_calls;

bool apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
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
    alarm_calls.answer = true;
    CHECK(apps_port_alarm_arm("clock", "wake_up", 90000, true, false) && alarm_calls.arms == 1, "arm");
    CHECK(strcmp(alarm_calls.app, "clock") == 0 && strcmp(alarm_calls.name, "wake_up") == 0 && alarm_calls.ms == 90000 &&
              alarm_calls.sound && !alarm_calls.wake,
          "arm passed %s/%s %lld %d %d", alarm_calls.app, alarm_calls.name, (long long)alarm_calls.ms, alarm_calls.sound,
          alarm_calls.wake);
    alarm_calls.answer = false;
    CHECK(!apps_port_alarm_arm("clock", "x", 1, false, true) && alarm_calls.arms == 2 && !alarm_calls.sound && alarm_calls.wake,
          "a refusal is told");
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
