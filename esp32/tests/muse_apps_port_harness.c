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
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define usable_size(p) malloc_size(p)
#else
#include <malloc.h>
#define usable_size(p) malloc_usable_size(p)
#endif

#include "apps_alarm.h"
#include "apps_echo.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_store.h"
#include "apps_task.h"
#include "apps_values.h"
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

/* What the heap says of PSRAM: free and the largest free block, which a case sets; and how often the port asked for the
 * largest block, which the gadget's heap works out by walking every block. */
static size_t psram_free_now = (size_t)3 << 20;
static size_t psram_largest = (size_t)3 << 20;
static atomic_int largest_asked;

size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return psram_free_now;
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    atomic_fetch_add(&largest_asked, 1);
    return psram_largest;
}

size_t heap_caps_get_allocated_size(void *ptr) {
    return usable_size(ptr);
}

/* The snapshot's blocks (N1): aligned as asked, counted as the port's other blocks are. */
static atomic_int aligned_asked;
static size_t aligned_last;

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    (void)caps;
    atomic_fetch_add(&aligned_asked, 1);
    aligned_last = alignment;
    if (alloc_fails()) {
        return NULL;
    }
    void *block = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    if (block != NULL) {
        atomic_fetch_add(&blocks_held, 1);
    }
    return block;
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
    char no_slot[32];  /* a name the table has no slot for: APPS_ALARM_REFUSED, whatever answer says */
} alarm_calls;

/* Every arming, in order: what the re-arm asked for. */
static struct {
    char app[32], name[32];
    int64_t ms;
    bool sound, wake;
} armed_log[16];
static int n_armed_log;

apps_alarm_result_t apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    snprintf(alarm_calls.app, sizeof alarm_calls.app, "%s", app);
    snprintf(alarm_calls.name, sizeof alarm_calls.name, "%s", name);
    alarm_calls.ms = ms;
    alarm_calls.sound = sound;
    alarm_calls.wake = wake;
    alarm_calls.arms++;
    if (n_armed_log < (int)(sizeof armed_log / sizeof armed_log[0])) {
        snprintf(armed_log[n_armed_log].app, sizeof armed_log[0].app, "%s", app);
        snprintf(armed_log[n_armed_log].name, sizeof armed_log[0].name, "%s", name);
        armed_log[n_armed_log].ms = ms;
        armed_log[n_armed_log].sound = sound;
        armed_log[n_armed_log].wake = wake;
        n_armed_log++;
    }
    return strcmp(name, alarm_calls.no_slot) == 0 ? APPS_ALARM_REFUSED : alarm_calls.answer;
}

/* The arming of app/name in the log, or NULL. */
static const char *armed_as(const char *app, const char *name, int64_t *ms, bool *sound, bool *wake) {
    for (int i = n_armed_log - 1; i >= 0; i--) {
        if (strcmp(armed_log[i].app, app) == 0 && strcmp(armed_log[i].name, name) == 0) {
            *ms = armed_log[i].ms;
            *sound = armed_log[i].sound;
            *wake = armed_log[i].wake;
            return armed_log[i].name;
        }
    }
    return NULL;
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

/* What the alarms' records read of an app's alarms (apps_values.c): the case sets them, for info_app ("" for any app). */
static apps_alarm_info_t listed_info[APPS_ALARMS_PER_APP];
static int n_listed_info, info_asks;
static char info_app[APPS_ID_MAX + 1];

int apps_alarm_list_info(const char *app, apps_alarm_info_t out[APPS_ALARMS_PER_APP]) {
    info_asks++;
    if (info_app[0] != '\0' && strcmp(app, info_app) != 0) {
        return 0;
    }
    memcpy(out, listed_info, sizeof listed_info);
    return n_listed_info;
}

/* The table has these of app now (name, the time left, sound, wake: up to four). */
static void table_has(const char *app, int n, const char *const *names, const int64_t *left, const bool *sound, const bool *wake) {
    snprintf(info_app, sizeof info_app, "%s", app);
    memset(listed_info, 0, sizeof listed_info);
    for (int i = 0; i < n; i++) {
        snprintf(listed_info[i].name, sizeof listed_info[i].name, "%s", names[i]);
        listed_info[i].remaining_ms = left[i];
        listed_info[i].sound = sound[i];
        listed_info[i].wake = wake[i];
    }
    n_listed_info = n;
}

/* ---- The store of packages (apps_store.h): the mode that picks the values' backend, and the flash backend, recorded ---- */

static apps_storage_t fake_mode = APPS_STORAGE_RAM;
static bool flash_answer = true;  /* what apps_nvs says to a save */
static char flash_app[APPS_ID_MAX + 1], flash_json[64];
static int flash_saves, flash_loads;

apps_storage_t apps_store_mode(void) {
    return fake_mode;
}

bool apps_store_save_values(const char *id, const char *json, size_t len) {
    flash_saves++;
    snprintf(flash_app, sizeof flash_app, "%s", id);
    snprintf(flash_json, sizeof flash_json, "%.*s", (int)len, json);
    return flash_answer;
}

char *apps_store_load_values(const char *id, size_t *len) {
    flash_loads++;
    const char *json = strcmp(id, flash_app) == 0 ? flash_json : "{}";
    *len = strlen(json);
    char *copy = apps_port_realloc(NULL, *len + 1);
    if (copy != NULL) {
        memcpy(copy, json, *len + 1);
    }
    return copy;
}

/* Every sync the worker was handed (apps_store_sync_alarms), in order. The fake does with the write budget what apps_store.c's sync does (its
 * own cases are tests/muse_apps_store_harness.c): an armed name's record takes APPS_VALUES_RECORD_COST, not forced, and one that doesn't fit
 * goes back to the app task (apps_values_record_waits); a drop costs nothing. */
typedef struct {
    char app[APPS_ID_MAX + 1];
    apps_store_name_t names[APPS_STORE_SYNC_MAX];
    int n;
    bool all;
    unsigned written, waits;  /* bit i: names[i]'s record written, or sent back to wait for the budget */
} sync_call_t;

static sync_call_t syncs[16];
static int n_syncs;

void apps_store_sync_alarms(const char *id, const apps_store_name_t *names, int n, bool all) {
    sync_call_t *s = n_syncs < (int)(sizeof syncs / sizeof syncs[0]) ? &syncs[n_syncs] : NULL;
    n_syncs++;
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof *s);
    snprintf(s->app, sizeof s->app, "%s", id);
    memcpy(s->names, names, (size_t)n * sizeof *names);
    s->n = n;
    s->all = all;
    for (int i = 0; i < n; i++) {
        if (names[i].state != APPS_STORE_ARMED) {
            continue;
        }
        if (apps_values_budget_take(APPS_VALUES_RECORD_COST, false)) {
            s->written |= 1u << i;
        } else {
            apps_values_record_waits(id, names[i].name);
            s->waits |= 1u << i;
        }
    }
}

/* name's entry in sync k (0: the first), or NULL. */
static const apps_store_name_t *synced_name(int k, const char *name) {
    for (int i = 0; k >= 0 && k < n_syncs && i < syncs[k].n; i++) {
        if (strcmp(syncs[k].names[i].name, name) == 0) {
            return &syncs[k].names[i];
        }
    }
    return NULL;
}

/* The records the mount handed over (apps_store_take_records): the case's, taken once the mode isn't none. */
static apps_store_record_t handed[APPS_STORE_RECORDS_MAX];
static int n_handed;
static bool handed_taken;

static void hand_over(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    apps_store_record_t *r = &handed[n_handed++];
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->name, sizeof r->name, "%s", name);
    r->epoch_ms = epoch_ms;
    r->sound = sound;
    r->wake = wake;
}

int apps_store_take_records(apps_store_record_t *out, int max) {
    if (handed_taken || fake_mode == APPS_STORAGE_NONE) {
        return 0;
    }
    handed_taken = true;
    int n = n_handed < max ? n_handed : max;
    memcpy(out, handed, (size_t)n * sizeof *out);
    return n;
}

static char removed_app[APPS_ID_MAX + 1];  /* an app removed since the mount: the store doesn't have it */

bool apps_store_has(const char *id) {
    return strcmp(id, removed_app) != 0;
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

/* A sleep moves the clock by what it asks (nothing really waits), and the worker, if the case lets it, answers meanwhile; then a
 * case's hook runs (another task's work that comes while the caller sleeps: a snapshot that lets go of its blocks). */
static int delays;
static void (*on_delay)(void);

void vTaskDelay(const TickType_t ticks) {
    delays++;
    now_us += (int64_t)ticks * 1000000 / configTICK_RATE_HZ;
    if (worker_answers) {
        worker_runs(1 << 20);
    }
    if (on_delay != NULL) {
        on_delay();
    }
}

/* The app task (apps_task_current): none of the harness's tasks unless a case names one. */
static struct fake_task *fake_app_task;

bool apps_task_current(void) {
    return fake_app_task != NULL && this_task == fake_app_task;
}

/* The time others took from the app task's core (apps_rt_steal_ms): one total, as the runtime keeps it. */
static atomic_uint stolen_total;
static int steals;

void apps_rt_steal_ms(uint32_t ms) {
    atomic_fetch_add(&stolen_total, ms);
    steals++;
}

uint32_t apps_rt_stolen_ms(void) {
    return atomic_load(&stolen_total);
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

/*
 * C1: the floor. Growth that would leave less than APPS_RAM_RESERVE of PSRAM free is refused (NULL, the block as it was), and
 * so is a block of APPS_FLOOR_BIG_BLOCK or more that would leave the largest free block under it; a shrink, or growth within
 * the block it has, never is, however short PSRAM is (vm_alloc takes a refused shrink for running out). A smaller request
 * never asks for the largest block, which the gadget works out by walking the heap.
 */
static void psram_floor(void) {
    apps_port_init();
    int held = live_blocks();  /* the echo's queue */
    psram_free_now = psram_largest = (size_t)3 << 20;
    char *a = apps_port_realloc(NULL, 1000);
    CHECK(a != NULL, "a block with room");
    memset(a, 'x', 1000);
    size_t have = heap_caps_get_allocated_size(a);
    psram_free_now = APPS_RAM_RESERVE + 1000;
    char *b = apps_port_realloc(a, have + 1001);
    CHECK(b == NULL, "growth of 1001 bytes left less than the reserve");
    if (b != NULL) {
        a = b;
    } else {
        CHECK(a[0] == 'x' && a[999] == 'x' && heap_caps_get_allocated_size(a) == have, "the refused block is as it was");
    }
    b = apps_port_realloc(a, have + 1000);
    CHECK(b != NULL, "growth down to the reserve itself was refused");
    a = b != NULL ? b : a;
    psram_free_now = APPS_RAM_RESERVE + 99;
    void *refused = apps_port_realloc(NULL, 100);
    CHECK(refused == NULL, "a new block of 100 bytes left less than the reserve");
    apps_port_free(refused);
    void *fits = apps_port_realloc(NULL, 99);
    CHECK(fits != NULL, "a new block down to the reserve was refused");
    apps_port_free(fits);

    psram_free_now = psram_largest = 0;
    b = apps_port_realloc(a, heap_caps_get_allocated_size(a));
    CHECK(b != NULL, "growth within the block was refused");
    a = b != NULL ? b : a;
    b = apps_port_realloc(a, 10);
    CHECK(b != NULL, "a shrink was refused");
    a = b != NULL ? b : a;
    apps_port_free(a);

    psram_free_now = (size_t)3 << 20;
    psram_largest = APPS_RAM_RESERVE + 64 * 1024 - 1;
    int asked = atomic_load(&largest_asked);
    refused = apps_port_realloc(NULL, 64 * 1024);
    CHECK(refused == NULL, "a block of 64 KB left the largest free block under the reserve");
    apps_port_free(refused);
    CHECK(atomic_load(&largest_asked) == asked + 1, "the big block asked for the largest free block once");
    psram_largest = APPS_RAM_RESERVE + 64 * 1024;
    void *big = apps_port_realloc(NULL, 64 * 1024);
    CHECK(big != NULL, "a block of 64 KB that leaves the reserve whole was refused");
    apps_port_free(big);
    psram_largest = 0;
    asked = atomic_load(&largest_asked);
    void *small = apps_port_realloc(NULL, APPS_FLOOR_BIG_BLOCK - 1);
    CHECK(small != NULL, "a block under APPS_FLOOR_BIG_BLOCK was held to the largest free block");
    CHECK(atomic_load(&largest_asked) == asked, "a block under APPS_FLOOR_BIG_BLOCK walked the heap");
    apps_port_free(small);
    psram_free_now = psram_largest = (size_t)3 << 20;
    CHECK(live_blocks() == held, "%d blocks held", live_blocks() - held);
}

/*
 * N1: a snapshot's blocks (apps_port_snapshot_alloc, _shrink, _free) go through the same floor as the apps' heap, both of its
 * clauses, 16-byte aligned (the JPEG encoder's input), and are counted as the snapshot's while held (apps_port_snapshot_bytes). A
 * shrink is never refused, and none of them ever waits. apps_port_room is the floor for a new block (the encoder's buffers ask it).
 */
static void snapshot_blocks_go_through_the_floor(void) {
    apps_port_init();
    int held = live_blocks();
    psram_free_now = psram_largest = (size_t)3 << 20;
    CHECK(apps_port_snapshot_bytes() == 0, "%zu bytes held before any snapshot", apps_port_snapshot_bytes());
    CHECK(apps_port_psram_largest() == psram_largest, "the largest free block: %zu", apps_port_psram_largest());
    uint16_t *px = apps_port_snapshot_alloc(434312);
    CHECK(px != NULL && ((uintptr_t)px & 15) == 0 && aligned_last == 16, "the screen's block, aligned to 16: %p (%zu)", (void *)px,
          aligned_last);
    size_t had = px != NULL ? heap_caps_get_allocated_size(px) : 0;
    CHECK(apps_port_snapshot_bytes() == had, "counted: %zu, not %zu", apps_port_snapshot_bytes(), had);
    int before = delays;
    psram_free_now = APPS_RAM_RESERVE + 131072 - 1;
    void *out = apps_port_snapshot_alloc(131072);
    CHECK(out == NULL, "a snapshot block that left less than the reserve free");
    psram_free_now = (size_t)3 << 20;
    psram_largest = APPS_RAM_RESERVE + 131072 - 1;
    out = apps_port_snapshot_alloc(131072);
    CHECK(out == NULL, "a snapshot block that left the largest free block under the reserve");
    psram_largest = (size_t)3 << 20;
    CHECK(delays == before && steals == 0, "a snapshot's refusal waited (%d sleeps, %d steals)", delays - before, steals);
    CHECK(apps_port_snapshot_bytes() == had, "the refusals counted something: %zu", apps_port_snapshot_bytes());
    psram_free_now = APPS_RAM_RESERVE + 99;
    CHECK(!apps_port_room(100) && apps_port_room(99), "apps_port_room is the floor for a new block");
    psram_free_now = psram_largest = 0;
    uint16_t *small = apps_port_snapshot_shrink(px, 108578);
    CHECK(small != NULL, "a shrink was refused");
    small = small != NULL ? small : px;
    CHECK(apps_port_snapshot_bytes() == heap_caps_get_allocated_size(small), "the shrink recounted: %zu, not %zu",
          apps_port_snapshot_bytes(), heap_caps_get_allocated_size(small));
    psram_free_now = psram_largest = (size_t)3 << 20;
    apps_port_snapshot_free(small);
    apps_port_snapshot_free(NULL);
    CHECK(apps_port_snapshot_bytes() == 0, "%zu bytes counted after the free", apps_port_snapshot_bytes());
    CHECK(live_blocks() == held, "%d blocks held", live_blocks() - held);
}

/* A snapshot in flight for the wait's cases: the LVGL task takes the screen's block, and the hook (on_delay, as the app task sleeps)
 * lets it go, on the worker, once the clock reaches release_at_ms; the free total follows by hand (the heap here is the case's). */
static void *snap_block;
static size_t snap_size;
static int64_t release_at_ms;
static uint32_t steal_on_next_delay;  /* a drawing that steals the core while the app task sleeps */

static void snapshot_takes(size_t n) {
    struct fake_task *caller = this_task;
    this_task = &tasks[2];  /* the LVGL task */
    snap_block = apps_port_snapshot_alloc(n);
    this_task = caller;
    CHECK(snap_block != NULL, "the snapshot's block of %zu bytes", n);
    snap_size = n;
    psram_free_now -= n;
}

static void snapshot_lets_go(void) {
    if (steal_on_next_delay != 0) {
        apps_rt_steal_ms(steal_on_next_delay);
        steal_on_next_delay = 0;
    }
    if (snap_block != NULL && now_us / 1000 >= release_at_ms) {
        struct fake_task *caller = this_task;
        this_task = &tasks[1];  /* the snapshot's job, on the I/O worker */
        apps_port_snapshot_free(snap_block);
        this_task = caller;
        snap_block = NULL;
        psram_free_now += snap_size;
    }
}

static void wait_case_begins(void) {
    apps_port_init();
    fake_app_task = &tasks[0];
    on_delay = snapshot_lets_go;
    clock_at_ms(1000);
    psram_largest = (size_t)3 << 20;
}

/*
 * The ruling: a snapshot never kills the running app. A growth of the app task's that the floor refuses while a snapshot holds blocks,
 * and that would fit were they free, waits (polls of APPS_PORT_SNAPSHOT_POLL_MS) until the snapshot lets go, then is tried once more;
 * the wait is the core's, not the app's (apps_rt_steal_ms, less what others stole meanwhile, so a drawing isn't counted twice), and the
 * snapshot's job reads how long the app waited (apps_port_snapshot_waits).
 */
static void an_app_that_grows_during_a_snapshot_waits_then_succeeds(void) {
    wait_case_begins();
    int held = live_blocks();
    psram_free_now = APPS_RAM_RESERVE + 600 * 1024;
    snapshot_takes(434312);
    release_at_ms = 1050;
    steal_on_next_delay = 5;
    uint32_t stolen = apps_rt_stolen_ms();
    char *a = apps_port_realloc(NULL, 200 * 1024);
    CHECK(a != NULL, "the app's growth was refused: it should have waited for the snapshot");
    CHECK(snap_block == NULL && now_us / 1000 == 1050, "it waited until the snapshot let go, at %lld ms", (long long)(now_us / 1000));
    CHECK(apps_rt_stolen_ms() - stolen == 50, "%u ms stolen, not the 50 ms waited (the drawing's 5 counted once)",
          apps_rt_stolen_ms() - stolen);
    uint32_t ms = 0;
    CHECK(apps_port_snapshot_waits(&ms) == 1 && ms == 50, "the waits: %u ms", ms);
    CHECK(apps_port_snapshot_waits(&ms) == 0 && ms == 0, "read once: %u ms", ms);
    /* A block of the app's that grows: the same. */
    psram_free_now = APPS_RAM_RESERVE + 600 * 1024;
    snapshot_takes(434312);
    release_at_ms = now_us / 1000 + 20;
    size_t have = heap_caps_get_allocated_size(a);
    char *b = apps_port_realloc(a, have + 190 * 1024);  /* more than is free over the reserve */
    CHECK(b != NULL, "the growth of a block was refused during the snapshot");
    a = b != NULL ? b : a;
    CHECK(apps_port_snapshot_waits(&ms) == 1 && ms == 20, "the second wait: %u ms", ms);
    /* The floor passes but the heap has no block that big (a fragmented PSRAM): it waits until the snapshot is done. */
    psram_free_now = APPS_RAM_RESERVE + 900 * 1024;
    snapshot_takes(434312);
    release_at_ms = now_us / 1000 + 40;
    alloc_fail_in = 0;
    char *c = apps_port_realloc(NULL, 100 * 1024);
    CHECK(c != NULL && snap_block == NULL, "a growth the heap refused during the snapshot didn't wait for it");
    CHECK(apps_port_snapshot_waits(&ms) == 1 && ms == 40, "the heap's wait: %u ms", ms);
    apps_port_free(c);
    apps_port_free(a);
    on_delay = NULL;
    fake_app_task = NULL;
    psram_free_now = (size_t)3 << 20;
    CHECK(apps_port_snapshot_bytes() == 0 && live_blocks() == held, "%zu snapshot bytes, %d blocks held",
          apps_port_snapshot_bytes(), live_blocks() - held);
}

/* Never a wait for the snapshot's own tasks (its job on the I/O worker, the LVGL task's drawing) or any task but the app task: their
 * refusal is at once. Nor for the app task when no snapshot holds anything, or when the growth wouldn't fit even with the snapshot's
 * blocks free. */
static void only_the_app_task_waits_and_only_when_it_helps(void) {
    wait_case_begins();
    int held = live_blocks();
    psram_free_now = APPS_RAM_RESERVE + 600 * 1024;
    snapshot_takes(434312);
    release_at_ms = INT64_MAX;
    static struct fake_task *const others[] = {&tasks[1], &tasks[2]};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        this_task = others[i];
        int before = delays;
        void *p = apps_port_realloc(NULL, 200 * 1024);
        CHECK(p == NULL && delays == before && steals == 0, "task %d waited (%d sleeps) or was given the block", others[i]->number,
              delays - before);
        apps_port_free(p);
    }
    this_task = &tasks[0];
    int before = delays;
    void *p = apps_port_realloc(NULL, 700 * 1024);  /* over the snapshot's bytes and what is free together */
    CHECK(p == NULL && delays == before, "a growth that couldn't fit with the snapshot gone waited (%d sleeps)", delays - before);
    apps_port_free(p);
    snapshot_lets_go();
    release_at_ms = 0;
    snapshot_lets_go();
    CHECK(snap_block == NULL && apps_port_snapshot_bytes() == 0, "the snapshot let go");
    psram_free_now = APPS_RAM_RESERVE + 100 * 1024;
    p = apps_port_realloc(NULL, 200 * 1024);
    CHECK(p == NULL && delays == before, "a refusal with no snapshot in flight waited (%d sleeps)", delays - before);
    apps_port_free(p);
    uint32_t ms;
    CHECK(apps_port_snapshot_waits(&ms) == 0, "no wait counted");
    on_delay = NULL;
    fake_app_task = NULL;
    psram_free_now = (size_t)3 << 20;
    CHECK(live_blocks() == held, "%d blocks held", live_blocks() - held);
}

/* The wait is bounded: APPS_PORT_SNAPSHOT_WAIT_MS at most, all of it stolen, then one more try, refused (the app runs out of memory as
 * ever); and that bound is the snapshot's, so a later refusal during the same snapshot doesn't wait again. The next snapshot gets its
 * own. */
static void the_wait_is_bounded_once_a_snapshot(void) {
    wait_case_begins();
    int held = live_blocks();
    psram_free_now = APPS_RAM_RESERVE + 600 * 1024;
    snapshot_takes(434312);
    release_at_ms = INT64_MAX;
    uint32_t stolen = apps_rt_stolen_ms();
    int before = delays;
    void *p = apps_port_realloc(NULL, 200 * 1024);
    CHECK(p == NULL, "the growth got a block with the snapshot still holding its own");
    apps_port_free(p);
    CHECK(now_us / 1000 == 1000 + APPS_PORT_SNAPSHOT_WAIT_MS && delays - before == APPS_PORT_SNAPSHOT_WAIT_MS / APPS_PORT_SNAPSHOT_POLL_MS,
          "waited until %lld ms in %d sleeps", (long long)(now_us / 1000), delays - before);
    CHECK(apps_rt_stolen_ms() - stolen == APPS_PORT_SNAPSHOT_WAIT_MS, "%u ms stolen", apps_rt_stolen_ms() - stolen);
    before = delays;
    p = apps_port_realloc(NULL, 200 * 1024);
    CHECK(p == NULL && delays == before, "a second refusal in the same snapshot waited (%d sleeps)", delays - before);
    apps_port_free(p);
    uint32_t ms;
    CHECK(apps_port_snapshot_waits(&ms) == 1 && ms == APPS_PORT_SNAPSHOT_WAIT_MS, "the waits: %u ms", ms);
    release_at_ms = 0;
    snapshot_lets_go();
    snapshot_takes(434312);  /* the next snapshot */
    release_at_ms = now_us / 1000 + 30;
    p = apps_port_realloc(NULL, 200 * 1024);
    CHECK(p != NULL, "the next snapshot's wait was refused");
    apps_port_free(p);
    CHECK(apps_port_snapshot_waits(&ms) == 1 && ms == 30, "the next wait: %u ms", ms);
    on_delay = NULL;
    fake_app_task = NULL;
    psram_free_now = (size_t)3 << 20;
    CHECK(apps_port_snapshot_bytes() == 0 && live_blocks() == held, "%zu snapshot bytes, %d blocks held",
          apps_port_snapshot_bytes(), live_blocks() - held);
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
    apps_values_tick(100000);  /* the worker takes it now */
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

/* The map keeps APPS_VALUES_RAM_APPS = 16 apps. A save for an app that is there replaces it and makes it the newest saved; one for a 17th app
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
 * replaces it with a newer save of the app, and offers it again at the tick, APPS_VALUES_RETRY_MS after the refusal and no sooner. */
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
    apps_values_tick(11999);
    CHECK(submit_calls == calls, "offered again before the retry time: %d submits", submit_calls - calls);
    apps_values_tick(12000);
    CHECK(submit_calls - calls == 1 && n_pending == 0 && live_blocks() - before == 1, "offered at 12000 and refused again: %d submits, %d blocks",
          submit_calls - calls, live_blocks() - before);
    calls = submit_calls;
    apps_values_tick(13999);
    CHECK(submit_calls == calls, "offered 1999 ms after the second refusal: %d submits", submit_calls - calls);
    ops_refused = false;
    apps_values_tick(14000);
    CHECK(submit_calls - calls == 1 && n_pending == 1, "offered at 14000 and taken: %d submits, %d queued", submit_calls - calls, n_pending);
    worker_answers = true;
    worker_runs(1);
    CHECK(n_pending == 0 && live_blocks() - before == 1, "the worker has it, and the port keeps nothing: %d blocks", live_blocks() - before);
    calls = submit_calls;
    apps_values_tick(99999);
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
    apps_values_tick(12000);
    CHECK(n_pending == APPS_IO_OPS, "a tick offers them until the ring is full: %d queued of %d", n_pending, APPS_IO_OPS);
    worker_answers = true;
    worker_runs(1 << 20);
    CHECK(n_pending == 0, "the worker ran them");
    apps_values_tick(12001);
    CHECK(n_pending == 0, "the next offer waits for its time (%d ms): %d queued", APPS_VALUES_RETRY_MS, n_pending);
    apps_values_tick(14000);
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
    apps_values_tick(12000);
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

/* ---- Task 20: the flash backend, its failures, the alarms' records ---- */

/* The time of day in epoch ms, as apps_port_epoch_ms reads it: a case brackets the port's reads with two of these. */
static int64_t epoch_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Whether a line the port logged has needle. */
static bool log_has(const char *needle) {
    for (int i = 0; i < n_logged; i++) {
        if (strstr(logged[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

/* On a gadget with the apps partition (the store's mode FLASH) the same ops write and read apps_nvs (apps_store_save_values,
 * apps_store_load_values) instead of the RAM map: in the order of the calls, a load after the save it follows, and nothing kept in RAM. */
static void values_go_to_apps_nvs_on_flash(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    int before = live_blocks();
    CHECK(apps_port_store_save("timer", "{\"s\":420}", 9), "queued");
    CHECK(n_pending == 1 && flash_saves == 0, "the save waits for the worker");
    worker_runs(1);
    CHECK(flash_saves == 1 && strcmp(flash_app, "timer") == 0 && strcmp(flash_json, "{\"s\":420}") == 0, "written: %s %s", flash_app,
          flash_json);
    CHECK(live_blocks() == before, "nothing is kept in RAM: %d blocks", live_blocks() - before);
    expect_load("timer", "{\"s\":420}", "read back from apps_nvs");
    CHECK(flash_loads == 1, "one read");
    expect_load("clock", "{}", "an app with nothing");
    CHECK(live_blocks() == before && n_logged == 0, "%d blocks, %d lines", live_blocks() - before, n_logged);
}

/* apps_nvs full (or a write error): the save isn't offered again, and the failure is said once a streak: apps_values_take_failure
 * hands the app over once, a second failure says nothing more, a success ends the streak and the next failure is said again. The
 * gadget's log has one line for each failure, with no values. */
static void a_backend_failure_is_said_once_a_streak(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    char app[APPS_ID_MAX + 1] = "";
    CHECK(!apps_values_take_failure(app, sizeof app), "nothing to say yet");
    flash_answer = false;
    CHECK(apps_port_store_save("timer", "{\"a\":1}", 7), "taken (the failure is the backend's, not a refusal)");
    worker_runs(1);
    CHECK(apps_values_take_failure(app, sizeof app) && strcmp(app, "timer") == 0, "said, for timer: '%s'", app);
    CHECK(!apps_values_take_failure(app, sizeof app), "once");
    CHECK(apps_port_store_save("timer", "{\"a\":2}", 7), "taken again");
    worker_runs(1);
    CHECK(!apps_values_take_failure(app, sizeof app), "the streak goes on: nothing more to say");
    int calls = submit_calls;
    apps_values_tick(1 << 30);
    CHECK(submit_calls == calls, "a failed save isn't offered again");
    flash_answer = true;
    CHECK(apps_port_store_save("timer", "{\"a\":3}", 7), "a save that succeeds");
    worker_runs(1);
    CHECK(!apps_values_take_failure(app, sizeof app), "nothing to say: the streak is over");
    flash_answer = false;
    CHECK(apps_port_store_save("clock", "{\"b\":1}", 7), "another failure");
    worker_runs(1);
    CHECK(apps_values_take_failure(app, sizeof app) && strcmp(app, "clock") == 0, "said again, for clock: '%s'", app);
    CHECK(n_logged == 3 && strstr(logged[0], "store: apps_nvs refused 7 bytes of values for timer") != NULL &&
              strstr(logged[2], "for clock") != NULL,
          "%d lines: '%s'", n_logged, n_logged > 0 ? logged[0] : "");
    flash_answer = true;
    n_logged = 0;
}

/* The alarms' records follow the names an app touched (P1): a burst of touches (an arming, a cancel, the same arming again) is one op,
 * APPS_VALUES_ALARM_DELAY_MS after the first, that hands the worker each touched name with what the table has of it, and no other name: done
 * armed (its time as an epoch, its sound and wake), old gone; other, armed but untouched, isn't in it. Without a valid clock an armed name has
 * no time. A refused op is asked for again APPS_VALUES_RETRY_MS later, and RAM mode hands nothing (nothing outlives a reset there). */
static void alarm_records_follow_the_table(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_at_ms(10000);
    clock_is_set = true;
    alarm_calls.answer = APPS_ALARM_ARMED;
    static const char *const done_and_other[] = {"done", "other"};
    table_has("timer", 2, done_and_other, (const int64_t[]){120000, 50000}, (const bool[]){true, false}, (const bool[]){true, true});
    int before = live_blocks();
    (void)apps_port_alarm_arm("timer", "done", 120000, true, true);
    apps_port_alarm_cancel("timer", "old");
    (void)apps_port_alarm_arm("timer", "done", 120000, true, true);
    apps_values_tick(10999);
    CHECK(n_pending == 0 && info_asks == 0, "nothing before the delay: %d ops", n_pending);
    int64_t low = epoch_now_ms();
    apps_values_tick(11000);
    int64_t high = epoch_now_ms();
    CHECK(n_pending == 1 && info_asks == 1, "one op for the burst: %d ops, %d asks", n_pending, info_asks);
    worker_runs(1);
    const apps_store_name_t *done = synced_name(0, "done"), *old = synced_name(0, "old");
    CHECK(n_syncs == 1 && strcmp(syncs[0].app, "timer") == 0 && syncs[0].n == 2 && !syncs[0].all, "timer's two touched names: %d syncs, %d names",
          n_syncs, syncs[0].n);
    CHECK(done != NULL && done->state == APPS_STORE_ARMED && done->epoch_ms >= low + 120000 && done->epoch_ms <= high + 120000 && done->sound &&
              done->wake,
          "done armed, at %lld (the clock %lld to %lld)", done != NULL ? (long long)done->epoch_ms : -1LL, (long long)low, (long long)high);
    CHECK(old != NULL && old->state == APPS_STORE_GONE, "old gone");
    CHECK(synced_name(0, "other") == NULL, "other, armed but untouched, isn't in it");
    CHECK(live_blocks() == before, "%d blocks held", live_blocks() - before);
    apps_values_tick(20000);
    CHECK(n_pending == 0, "nothing more without a change");
    /* Without a valid clock: nap, armed, has no time; done, cancelled, is gone. */
    clock_is_set = false;
    static const char *const just_nap[] = {"nap"};
    table_has("timer", 1, just_nap, (const int64_t[]){30000}, (const bool[]){false}, (const bool[]){true});
    (void)apps_port_alarm_arm("timer", "nap", 30000, false, true);
    apps_port_alarm_cancel("timer", "done");
    apps_values_tick(21000);
    worker_runs(1);
    const apps_store_name_t *nap = synced_name(1, "nap");
    done = synced_name(1, "done");
    CHECK(n_syncs == 2 && syncs[1].n == 2 && nap != NULL && nap->state == APPS_STORE_ARMED_NO_CLOCK && done != NULL &&
              done->state == APPS_STORE_GONE,
          "no clock: nap with no time, done gone: %d syncs, %d names", n_syncs, syncs[1].n);
    /* A refusal: asked again 2 s later. */
    apps_port_alarm_cancel("timer", "x");
    ops_refused = true;
    apps_values_tick(30000);
    CHECK(n_pending == 0 && live_blocks() == before, "refused: nothing queued or held");
    ops_refused = false;
    apps_values_tick(30000 + APPS_VALUES_RETRY_MS - 1);
    CHECK(n_pending == 0, "not before the retry");
    apps_values_tick(30000 + APPS_VALUES_RETRY_MS);
    CHECK(n_pending == 1, "asked again");
    worker_runs(1);
    /* RAM mode: nothing outlives a reset, so no op. */
    fake_mode = APPS_STORAGE_RAM;
    (void)apps_port_alarm_arm("timer", "done", 1000, false, false);
    apps_values_tick(60000);
    CHECK(n_pending == 0 && n_syncs == 3, "RAM mode: no op");
    fake_mode = APPS_STORAGE_FLASH;
    apps_values_tick(70000);
    CHECK(n_pending == 0, "and the mark is gone");
}

/* RAM mode's remove forgets the app's values (apps_values_forget, the worker's): its load reads {} again, and another app's stay. */
static void the_ram_map_forgets_a_removed_app(void) {
    worker_answers = true;
    int before = live_blocks();
    CHECK(apps_port_store_save("timer", "{\"a\":1}", 7) && apps_port_store_save("clock", "{\"b\":2}", 7), "saved");
    worker_runs(2);
    CHECK(live_blocks() - before == 2, "two nodes");
    apps_values_forget("timer");
    CHECK(live_blocks() - before == 1, "timer's node went");
    expect_load("timer", "{}", "forgotten");
    expect_load("clock", "{\"b\":2}", "the other's stays");
    apps_values_forget("nobody");
    CHECK(live_blocks() - before == 1, "forgetting an app with nothing changes nothing");
}

/* K: a failure streak is each app's: clock's first failure is said while timer's streak goes on, and each app once. */
static void each_apps_failure_is_said(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    flash_answer = false;
    char app[APPS_ID_MAX + 1] = "";
    CHECK(apps_port_store_save("timer", "{\"a\":1}", 7), "taken");
    worker_runs(1);
    CHECK(apps_port_store_save("clock", "{\"b\":1}", 7), "taken");
    worker_runs(1);
    CHECK(apps_port_store_save("timer", "{\"a\":2}", 7), "taken");
    worker_runs(1);
    CHECK(apps_values_take_failure(app, sizeof app) && strcmp(app, "timer") == 0, "timer's: '%s'", app);
    CHECK(apps_values_take_failure(app, sizeof app) && strcmp(app, "clock") == 0, "and clock's, while timer's streak goes on: '%s'", app);
    CHECK(!apps_values_take_failure(app, sizeof app), "each once");
    flash_answer = true;
    n_logged = 0;
}

/* P3: the write budget. A 16,000-byte value changed every 2 s for 30 minutes: the saves take the 48 KB the budget starts with, then one
 * fits each time it has filled again (about 24 bytes a second), the newest value each time, so apps_nvs sees at most 5 of them (48 KB and
 * 1800 s of 24 B/s are about 91 KB); the budget's hold is said once; and the app's end writes the last value whatever the budget says. */
static void the_budget_holds_back_a_big_value(void) {
    fake_mode = APPS_STORAGE_FLASH;
    enum { LEN = 16000 };
    char *json = malloc(LEN);
    CHECK(json != NULL, "no memory");
    if (json == NULL) {
        return;
    }
    memset(json, 'x', LEN);
    json[LEN - 2] = '"';
    json[LEN - 1] = '}';
    int before = live_blocks();
    for (int n = 0; n < 900; n++) {
        int64_t now = 100000 + (int64_t)n * 2000;
        clock_at_ms(now);
        apps_values_tick(now);
        worker_runs(8);
        char head[16];
        snprintf(head, sizeof head, "{\"n\":%04d,\"x\":\"", n);
        memcpy(json, head, strlen(head));
        CHECK(apps_port_store_save("big", json, LEN), "save %d taken", n);
        worker_runs(8);
    }
    int in_time = flash_saves;
    CHECK(in_time >= 3 && in_time <= 5, "%d saves in 30 minutes: at most 5", in_time);
    char app[APPS_ID_MAX + 1] = "";
    CHECK(apps_values_take_slowdown(app, sizeof app) && strcmp(app, "big") == 0, "the hold is said: '%s'", app);
    CHECK(!apps_values_take_slowdown(app, sizeof app), "once");
    CHECK(strncmp(flash_json, "{\"n\":0899,", 10) != 0, "the last value waits: %.12s", flash_json);
    apps_values_flush("big");
    worker_runs(8);
    CHECK(flash_saves == in_time + 1 && strncmp(flash_json, "{\"n\":0899,", 10) == 0, "the end wrote the last value: %d saves, %.12s",
          flash_saves, flash_json);
    CHECK(live_blocks() == before, "%d blocks held", live_blocks() - before);
    free(json);
}

/* P3: a small value (100 bytes) changed once a minute for 30 minutes is never held back: each save is written as it is made. */
static void a_small_value_is_never_held_back(void) {
    fake_mode = APPS_STORAGE_FLASH;
    char json[100];
    memset(json, 'y', sizeof json);
    json[sizeof json - 2] = '"';
    json[sizeof json - 1] = '}';
    for (int n = 0; n < 30; n++) {
        int64_t now = 100000 + (int64_t)n * 60000;
        clock_at_ms(now);
        apps_values_tick(now);
        char head[16];
        snprintf(head, sizeof head, "{\"n\":%02d,\"y\":\"", n);
        memcpy(json, head, strlen(head));
        CHECK(apps_port_store_save("small", json, sizeof json), "save %d taken", n);
        worker_runs(8);
        CHECK(flash_saves == n + 1, "save %d written as it was made: %d saves", n, flash_saves);
    }
    char app[APPS_ID_MAX + 1] = "";
    CHECK(!apps_values_take_slowdown(app, sizeof app), "never held back: '%s'", app);
}

/* P1: the records the mount handed over wait for the clock, and an alarm the app arms meanwhile under another name syncs that name alone:
 * the others' names aren't in the sync, so nothing drops their records for not being in the table. When the clock comes each untouched record
 * is armed again for the time it has left, with its sound and wake, and the app's alarm, synced without a clock, gets its record then. */
static void untouched_records_wait_for_the_clock(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_is_set = false;
    alarm_calls.answer = APPS_ALARM_ARMED;
    clock_at_ms(10000);
    int64_t epoch = epoch_now_ms();
    hand_over("timer", "a", epoch + 60000, true, true);
    hand_over("timer", "b", epoch + 120000, false, true);
    int before = live_blocks();
    static const char *const just_c[] = {"c"};
    table_has("timer", 1, just_c, (const int64_t[]){30000}, (const bool[]){true}, (const bool[]){false});
    (void)apps_port_alarm_arm("timer", "c", 30000, true, false);
    apps_values_tick(10000 + APPS_VALUES_ALARM_DELAY_MS);
    worker_runs(1);
    const apps_store_name_t *c = synced_name(0, "c");
    CHECK(n_syncs == 1 && strcmp(syncs[0].app, "timer") == 0 && syncs[0].n == 1 && !syncs[0].all && c != NULL &&
              c->state == APPS_STORE_ARMED_NO_CLOCK,
          "c synced alone, with no time (a's and b's names aren't in it): %d syncs, %d names", n_syncs, syncs[0].n);
    CHECK(alarm_calls.arms == 1, "nothing armed again without the clock: %d armings", alarm_calls.arms);
    /* The clock comes. */
    clock_is_set = true;
    clock_at_ms(20000);
    int64_t low = epoch_now_ms();
    apps_values_tick(20000);
    int64_t high = epoch_now_ms();
    int64_t ms = 0;
    bool sound = false, wake = false;
    CHECK(armed_as("timer", "a", &ms, &sound, &wake) != NULL && ms >= epoch + 60000 - high && ms <= epoch + 60000 - low && sound && wake,
          "a armed again for what it has left: %lld ms (%lld to %lld), sound %d, wake %d", (long long)ms, (long long)(epoch + 60000 - high),
          (long long)(epoch + 60000 - low), sound, wake);
    CHECK(armed_as("timer", "b", &ms, &sound, &wake) != NULL && ms >= epoch + 120000 - high && ms <= epoch + 120000 - low && !sound && wake,
          "b too: %lld ms, sound %d, wake %d", (long long)ms, sound, wake);
    CHECK(alarm_calls.arms == 3 && log_has("I muse_apps: store: alarm timer/a armed again: ") &&
              log_has("I muse_apps: store: alarm timer/b armed again: "),
          "two re-arms, each with its line: %d armings", alarm_calls.arms);
    /* c gets its record: it was synced without the clock. */
    static const char *const all[] = {"c", "a", "b"};
    table_has("timer", 3, all, (const int64_t[]){20000, 50000, 110000}, (const bool[]){true, true, false}, (const bool[]){false, true, true});
    low = epoch_now_ms();
    apps_values_tick(20000 + APPS_VALUES_ALARM_DELAY_MS);
    high = epoch_now_ms();
    worker_runs(1);
    c = synced_name(1, "c");
    CHECK(n_syncs == 2 && syncs[1].n == 1 && c != NULL && c->state == APPS_STORE_ARMED && c->epoch_ms >= low + 20000 &&
              c->epoch_ms <= high + 20000 && c->sound && !c->wake,
          "c's record, with its time, and c's alone (a and b, armed again from their records, aren't touched): %d syncs, %d names", n_syncs,
          syncs[1].n);
    CHECK(live_blocks() == before, "%d blocks held", live_blocks() - before);
}

/* P1: a name the app touches before the clock comes is the app's from then on: the record the mount handed over for it is not armed again.
 * Armed again by the app without a clock, it is in the sync with no time (its old record goes: apps_store.h), and gets its new record, with
 * the new deadline, once the clock comes. Cancelled while it waited, it is in the sync gone (its record goes), and nothing arms it. */
static void a_touched_name_is_not_armed_again(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_is_set = false;
    alarm_calls.answer = APPS_ALARM_ARMED;
    clock_at_ms(10000);
    int64_t epoch = epoch_now_ms();
    hand_over("timer", "a", epoch + 60000, true, true);
    hand_over("timer", "b", epoch + 90000, true, true);
    static const char *const just_a[] = {"a"};
    table_has("timer", 1, just_a, (const int64_t[]){45000}, (const bool[]){true}, (const bool[]){true});
    (void)apps_port_alarm_arm("timer", "a", 45000, true, true);  /* the app arms a again: a new countdown */
    apps_port_alarm_cancel("timer", "b");                        /* and cancels b, which waited for the clock */
    apps_values_tick(10000 + APPS_VALUES_ALARM_DELAY_MS);
    worker_runs(1);
    const apps_store_name_t *a = synced_name(0, "a"), *b = synced_name(0, "b");
    CHECK(n_syncs == 1 && syncs[0].n == 2 && a != NULL && a->state == APPS_STORE_ARMED_NO_CLOCK && b != NULL && b->state == APPS_STORE_GONE,
          "a with no time (its old record goes), b gone (its record goes): %d syncs, %d names", n_syncs, syncs[0].n);
    clock_is_set = true;
    clock_at_ms(20000);
    table_has("timer", 1, just_a, (const int64_t[]){35000}, (const bool[]){true}, (const bool[]){true});
    apps_values_tick(20000);
    CHECK(alarm_calls.arms == 1 && n_armed_log == 1, "nothing armed again: the app's own arming is the only one: %d", alarm_calls.arms);
    int64_t low = epoch_now_ms();
    apps_values_tick(20000 + APPS_VALUES_ALARM_DELAY_MS);
    int64_t high = epoch_now_ms();
    worker_runs(1);
    a = synced_name(1, "a");
    CHECK(n_syncs == 2 && syncs[1].n == 1 && a != NULL && a->state == APPS_STORE_ARMED && a->epoch_ms >= low + 35000 &&
              a->epoch_ms <= high + 35000,
          "a's new record, with the new deadline: %d syncs, %lld (%lld to %lld)", n_syncs, a != NULL ? (long long)a->epoch_ms : -1LL,
          (long long)(low + 35000), (long long)(high + 35000));
    CHECK(!log_has("armed again"), "no line of a re-arm");
}

/* P1: a re-arm the alarm table has no memory for is tried again APPS_VALUES_REARM_RETRY_MS later, and its line says so; one it has no slot
 * for is dropped, record and all (its name is synced, gone), with its line; a time past the week of APPS_ALARM_MAX_MS is cut to it, and one
 * that is due fires at once (1 ms). A record of an app removed since the mount arms nothing. */
static void rearm_retries_drops_and_clamps(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_is_set = true;
    clock_at_ms(10000);
    int64_t epoch = epoch_now_ms();
    hand_over("timer", "due", epoch - 5000, true, true);
    hand_over("timer", "far", epoch + 3 * APPS_ALARM_MAX_MS, true, false);
    hand_over("clock", "full", epoch + 60000, false, true);
    hand_over("ghost", "boo", epoch + 60000, true, true);
    snprintf(removed_app, sizeof removed_app, "ghost");
    snprintf(alarm_calls.no_slot, sizeof alarm_calls.no_slot, "full");
    alarm_calls.answer = APPS_ALARM_NO_MEMORY;
    apps_values_tick(10000);
    CHECK(alarm_calls.arms == 1 && strcmp(armed_log[0].name, "due") == 0, "the first re-arm got no memory, and the rest wait: %d armings",
          alarm_calls.arms);
    CHECK(log_has("W muse_apps: store: alarm timer/due: no memory to arm it again; it is tried again in 2 s"), "said, with when");
    alarm_calls.answer = APPS_ALARM_ARMED;
    apps_values_tick(10000 + APPS_VALUES_REARM_RETRY_MS - 1);
    CHECK(alarm_calls.arms == 1, "not before the retry: %d armings", alarm_calls.arms);
    apps_values_tick(10000 + APPS_VALUES_REARM_RETRY_MS);
    CHECK(alarm_calls.arms == 4, "due, far and full asked for, boo not: %d armings", alarm_calls.arms);
    int64_t ms = 0;
    bool sound = false, wake = false;
    CHECK(armed_as("timer", "due", &ms, &sound, &wake) != NULL && ms == 1, "a record that is due fires at once: %lld ms", (long long)ms);
    CHECK(armed_as("timer", "far", &ms, &sound, &wake) != NULL && ms == APPS_ALARM_MAX_MS && sound && !wake, "a far one is cut to the week: %lld",
          (long long)ms);
    CHECK(armed_as("ghost", "boo", &ms, &sound, &wake) == NULL, "nothing for an app removed since the mount");
    CHECK(log_has("W muse_apps: store: alarm clock/full couldn't be armed again (the gadget has no slot for it): dropped"), "the drop is said");
    table_has("clock", 0, NULL, NULL, NULL, NULL);
    apps_values_tick(10000 + APPS_VALUES_REARM_RETRY_MS + APPS_VALUES_ALARM_DELAY_MS);
    worker_runs(1);
    const apps_store_name_t *full = synced_name(0, "full");
    CHECK(n_syncs == 1 && strcmp(syncs[0].app, "clock") == 0 && syncs[0].n == 1 && full != NULL && full->state == APPS_STORE_GONE,
          "clock's full is synced gone: its record goes: %d syncs, %s, %d names", n_syncs, syncs[0].app, syncs[0].n);
    apps_values_tick(60000);
    CHECK(alarm_calls.arms == 4 && n_syncs == 1, "nothing more: %d armings, %d syncs", alarm_calls.arms, n_syncs);
}

/* #1: a record the budget can't take holds nothing up. With the budget overdrawn (a store-heavy app's end), timer's re-armed done is handed to
 * the worker at once all the same (its old record goes there at once: apps_store.c), comes back to wait, and is synced again when the budget
 * has room for its record, not before; meanwhile another app's cancel is synced at its own time (a drop needs no budget). */
static void a_record_that_waits_for_the_budget_holds_no_other_sync(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_is_set = true;
    alarm_calls.answer = APPS_ALARM_ARMED;
    clock_at_ms(10000);
    (void)apps_values_budget_take(APPS_VALUES_BUDGET_BYTES + 16 * 1024, true);  /* an app's end overdrew it by 16 KB */
    static const char *const just_done[] = {"done"};
    table_has("timer", 1, just_done, (const int64_t[]){120000}, (const bool[]){true}, (const bool[]){true});
    (void)apps_port_alarm_arm("timer", "done", 120000, true, true);
    apps_values_tick(10000 + APPS_VALUES_ALARM_DELAY_MS);
    CHECK(n_pending == 1, "done's sync is handed over at once, whatever the budget: %d ops", n_pending);
    worker_runs(1);
    CHECK(n_syncs == 1 && syncs[0].n == 1 && syncs[0].waits == 1u && syncs[0].written == 0, "done had to wait for the budget: %d syncs", n_syncs);
    /* The next round takes the worker's note: done waits for as long as the budget needs to have room for its record. */
    clock_at_ms(12000);
    int64_t wait = apps_values_budget_wait_ms(APPS_VALUES_RECORD_COST);
    CHECK(wait > 600000, "about 11 minutes: %lld ms", (long long)wait);
    apps_values_tick(12000);
    CHECK(n_pending == 0, "nothing while it waits: %d ops", n_pending);
    /* Another app's cancel goes through at its own time. */
    apps_port_alarm_cancel("clock", "nap");
    apps_values_tick(12000 + APPS_VALUES_ALARM_DELAY_MS);
    CHECK(n_pending == 1, "clock's sync, while timer's waits: %d ops", n_pending);
    worker_runs(1);
    const apps_store_name_t *nap = synced_name(1, "nap");
    CHECK(n_syncs == 2 && strcmp(syncs[1].app, "clock") == 0 && syncs[1].n == 1 && nap != NULL && nap->state == APPS_STORE_GONE,
          "nap's record goes, with no budget: %d syncs", n_syncs);
    /* done again, once the budget has room. */
    clock_at_ms(12000 + wait - 1);
    apps_values_tick(12000 + wait - 1);
    CHECK(n_pending == 0, "not before the budget has room: %d ops", n_pending);
    clock_at_ms(12000 + wait);
    apps_values_tick(12000 + wait);
    CHECK(n_pending == 1, "synced again once it has: %d ops", n_pending);
    worker_runs(1);
    const apps_store_name_t *done = synced_name(2, "done");
    CHECK(n_syncs == 3 && syncs[2].n == 1 && done != NULL && done->state == APPS_STORE_ARMED && syncs[2].written == 1u,
          "done's new record is written: %d syncs", n_syncs);
    apps_values_tick(12000 + wait + 60000);
    CHECK(n_pending == 0 && n_syncs == 3, "and nothing more: %d ops, %d syncs", n_pending, n_syncs);
}

/* #2: a remove (or a quarantine) of an app, on the worker, has the records the mount handed over for it forgotten at the next round
 * (apps_values_forget_records): the app installed again under that id before the clock comes is armed nothing of the old one's, and nothing of
 * it is synced. Another app's records are armed again. */
static void a_removed_apps_records_are_not_armed_again(void) {
    fake_mode = APPS_STORAGE_FLASH;
    worker_answers = true;
    clock_is_set = false;
    alarm_calls.answer = APPS_ALARM_ARMED;
    clock_at_ms(10000);
    int64_t epoch = epoch_now_ms();
    hand_over("timer", "a", epoch + 60000, true, true);
    hand_over("timer", "b", epoch + 90000, false, true);
    hand_over("clock", "c", epoch + 60000, true, false);
    apps_values_tick(10000);
    CHECK(log_has("I muse_apps: store: 3 alarm records wait for the clock"), "they wait for the clock");
    apps_values_forget_records("timer");  /* the worker's remove of timer; the store has a timer again (a reinstall), and the clock comes */
    clock_is_set = true;
    clock_at_ms(20000);
    apps_values_tick(20000);
    int64_t ms = 0;
    bool sound = false, wake = false;
    CHECK(armed_as("timer", "a", &ms, &sound, &wake) == NULL && armed_as("timer", "b", &ms, &sound, &wake) == NULL,
          "nothing of the removed timer's is armed");
    CHECK(armed_as("clock", "c", &ms, &sound, &wake) != NULL && alarm_calls.arms == 1, "clock's is: %d armings", alarm_calls.arms);
    CHECK(!log_has("I muse_apps: store: alarm timer/"), "no line of a re-arm of timer's");
    apps_values_tick(30000);
    CHECK(n_pending == 0 && n_syncs == 0, "and no sync: nothing of timer's was touched: %d ops, %d syncs", n_pending, n_syncs);
}

/* #4: a save is charged when the worker is handed it, and one the worker refuses gets its cost back, so the save that replaces it pays once.
 * The tick's offer of a kept save is charged the same way, and refunded when the worker refuses it again. */
static void a_refused_save_is_charged_once(void) {
    fake_mode = APPS_STORAGE_FLASH;
    clock_at_ms(10000);  /* the clock stands still: the budget doesn't fill in this case */
    enum { LEN = 16000 };
    char *json = malloc(LEN);
    CHECK(json != NULL, "no memory");
    if (json == NULL) {
        return;
    }
    memset(json, 'x', LEN);
    memcpy(json, "{\"x\":\"", 6);
    json[LEN - 2] = '"';
    json[LEN - 1] = '}';
    const uint32_t cost = (uint32_t)(strlen("big") + 1 + LEN + APPS_VALUES_ENTRY_COST);
    ops_refused = true;
    CHECK(apps_port_store_save("big", json, LEN), "a refused save is kept: it says true");
    CHECK(n_pending == 0 && apps_values_budget_wait_ms(APPS_VALUES_BUDGET_BYTES) == 0, "and costs nothing: the budget is full");
    apps_values_tick(10000 + APPS_VALUES_RETRY_MS);
    CHECK(n_pending == 0 && apps_values_budget_wait_ms(APPS_VALUES_BUDGET_BYTES) == 0, "the tick's offer, refused too, costs nothing");
    ops_refused = false;
    json[6] = 'y';
    CHECK(apps_port_store_save("big", json, LEN), "the newer save");
    CHECK(n_pending == 1, "the worker has it: %d ops", n_pending);
    worker_runs(1);
    CHECK(flash_saves == 1 && flash_app[0] != '\0', "written once: %d saves", flash_saves);
    CHECK(apps_values_budget_wait_ms(APPS_VALUES_BUDGET_BYTES - cost) == 0 && apps_values_budget_wait_ms(APPS_VALUES_BUDGET_BYTES - cost + 1) > 0,
          "charged once: the budget holds %u bytes less", (unsigned)cost);
    free(json);
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
    {"psram_floor", psram_floor},
    {"snapshot_blocks_go_through_the_floor", snapshot_blocks_go_through_the_floor},
    {"an_app_that_grows_during_a_snapshot_waits_then_succeeds", an_app_that_grows_during_a_snapshot_waits_then_succeeds},
    {"only_the_app_task_waits_and_only_when_it_helps", only_the_app_task_waits_and_only_when_it_helps},
    {"the_wait_is_bounded_once_a_snapshot", the_wait_is_bounded_once_a_snapshot},
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
    {"values_go_to_apps_nvs_on_flash", values_go_to_apps_nvs_on_flash},
    {"a_backend_failure_is_said_once_a_streak", a_backend_failure_is_said_once_a_streak},
    {"alarm_records_follow_the_table", alarm_records_follow_the_table},
    {"the_ram_map_forgets_a_removed_app", the_ram_map_forgets_a_removed_app},
    {"each_apps_failure_is_said", each_apps_failure_is_said},
    {"the_budget_holds_back_a_big_value", the_budget_holds_back_a_big_value},
    {"a_small_value_is_never_held_back", a_small_value_is_never_held_back},
    {"untouched_records_wait_for_the_clock", untouched_records_wait_for_the_clock},
    {"a_touched_name_is_not_armed_again", a_touched_name_is_not_armed_again},
    {"rearm_retries_drops_and_clamps", rearm_retries_drops_and_clamps},
    {"a_record_that_waits_for_the_budget_holds_no_other_sync", a_record_that_waits_for_the_budget_holds_no_other_sync},
    {"a_removed_apps_records_are_not_armed_again", a_removed_apps_records_are_not_armed_again},
    {"a_refused_save_is_charged_once", a_refused_save_is_charged_once},
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
