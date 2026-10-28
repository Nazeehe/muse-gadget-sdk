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
 * stdout, sent to a file the case reads back.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apps_echo.h"
#include "apps_port.h"
#include "muse_apps_task_fakes.h"
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
    for (int i = 0; i < 4; i++) {
        strcat(expect, line);
    }
    /* 60 ms later, 156 + 61.44 bytes: the line (217) would fit, but not with its note (37 + 217): not echoed either. */
    clock_at_ms(10060);
    apps_port_log("hello", x200);
    /* 100 ms after the burst, 258.4 bytes: the note and the line (254) fit, and 4.4 bytes are left. */
    clock_at_ms(10100);
    apps_port_log("hello", x200);
    strcat(expect, "@app.log hello (3 lines not echoed)\n");
    strcat(expect, line);
    apps_port_log("hello", "bye");  /* 20 bytes: not echoed */
    /* A quiet second fills the bucket, and no more: the note and the line (37 + 21), then four long lines (868); the
     * fifth doesn't fit in the 98 bytes left. */
    clock_at_ms(12000);
    apps_port_log("hello", "back");
    strcat(expect, "@app.log hello (1 lines not echoed)\n@app.log hello back\n");
    for (int i = 0; i < 5; i++) {
        apps_port_log("hello", x200);
    }
    for (int i = 0; i < 4; i++) {
        strcat(expect, line);
    }
    char *got = console_text();
    CHECK(strcmp(got, expect) == 0, "the console:\n%s\nexpected:\n%s", got, expect);
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

static const struct {
    const char *name;
    void (*run)(void);
} CASES[] = {
    {"echo_bucket", echo_bucket},
    {"echo_lines", echo_lines},
    {"breach_kinds", breach_kinds},
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
