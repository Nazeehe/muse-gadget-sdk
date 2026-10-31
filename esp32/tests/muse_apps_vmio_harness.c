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
 * Checks the apps' `vm` module (apps_vmio.c) and the network worker (apps_net.c) together, over the real runtime. Usage:
 * muse_apps_vmio_harness <case>, one case per test of tests/test_muse_apps_vmio.py; exits 0 when the case holds.
 *
 * - The worker is real: apps_net.c's own thread, made by the FreeRTOS below (pthreads), with its queue and its lock. It calls the
 *   VM ops of a scriptable VM folder in memory (muse_apps_vm_ops below): files, a reader or writer that answers busy, ops held until
 *   a case lets them go or their deadline comes, a session that drops part way through a read, a VM that refuses a write, no session
 *   at all. Every op checks that it runs on the worker, and that no other op runs at the same time.
 * - The case's thread is the app task: a round takes the worker's results (one whose app has ended is freed and never runs, as
 *   apps_task.c's take_vm_results does), ticks the module with "in front and lit" as the case says, and runs one callback.
 * - The clock is the harness's and moves only when a case moves it; the worker's deadlines and the held ops' run on it. vTaskDelay
 *   doesn't move it: it notes the wait (the busy retries) and yields.
 * - The heap counts every byte through apps_port_realloc (the Lua heap's, the jobs', the worker's buffers and the decoder's), and
 *   every case ends with none held. A case can make it refuse every growth on the worker, growths on the worker from a size up, or
 *   one growth of a size on the app task's thread.
 * - The app's callbacks print what they get, and the case reads the app's log lines.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apps_net.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_vmio.h"
#include "muse_apps.h"
#include "muse_apps_task_fakes.h"

static atomic_int failures;

#define CHECK_AT(line, cond, ...)                                  \
    do {                                                           \
        if (!(cond)) {                                             \
            atomic_fetch_add(&failures, 1);                        \
            fprintf(stderr, "FAIL line %d: %s: ", (line), #cond);  \
            fprintf(stderr, __VA_ARGS__);                          \
            fputc('\n', stderr);                                   \
        }                                                          \
    } while (0)
#define CHECK(cond, ...) CHECK_AT(__LINE__, cond, __VA_ARGS__)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void die(const char *what) {
    fprintf(stderr, "harness: %s\n", what);
    exit(2);
}

static double real_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- What happened: the worker's and the VM's notes, in order ---- */

#define NOTES_MAX 4096
static pthread_mutex_t notes_mu = PTHREAD_MUTEX_INITIALIZER;
static char *notes[NOTES_MAX];
static int notes_n;

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&notes_mu);
    if (notes_n < NOTES_MAX) {
        notes[notes_n++] = strdup(line);
    }
    pthread_mutex_unlock(&notes_mu);
}

/* How many notes start with prefix. */
static int count_notes(const char *prefix) {
    int n = 0;
    pthread_mutex_lock(&notes_mu);
    for (int i = 0; i < notes_n; i++) {
        n += strncmp(notes[i], prefix, strlen(prefix)) == 0 ? 1 : 0;
    }
    pthread_mutex_unlock(&notes_mu);
    return n;
}

static void dump_notes(void) {
    pthread_mutex_lock(&notes_mu);
    for (int i = 0; i < notes_n; i++) {
        fprintf(stderr, "  note %d: %s\n", i, notes[i]);
    }
    pthread_mutex_unlock(&notes_mu);
}

/* ---- FreeRTOS, as much as apps_net.c uses: tasks are threads ---- */

static _Thread_local const char *thread_name = "app";  /* the case's thread plays the app task */

struct fake_task {
    pthread_t thread;
    char name[24];
    TaskFunction_t code;
    void *arg;
};

static struct fake_task tasks[4];
static atomic_int tasks_n;

static void *task_main(void *arg) {
    struct fake_task *task = arg;
    thread_name = task->name;
    task->code(task->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps) {
    note("task %s stack %u priority %u core %d caps 0x%x", name, (unsigned)stack, priority, core, caps);
    int i = atomic_fetch_add(&tasks_n, 1);
    if (i >= (int)ARRAY_LEN(tasks)) {
        return pdFAIL;
    }
    struct fake_task *task = &tasks[i];
    snprintf(task->name, sizeof task->name, "%s", name);
    task->code = code;
    task->arg = arg;
    if (created != NULL) {
        *created = task;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&task->thread, &attr, task_main, task);
    pthread_attr_destroy(&attr);
    return rc == 0 ? pdPASS : pdFAIL;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    for (int i = 0; i < atomic_load(&tasks_n) && i < (int)ARRAY_LEN(tasks); i++) {
        if (pthread_equal(tasks[i].thread, pthread_self())) {
            return &tasks[i];
        }
    }
    return NULL;
}

void vTaskDelay(const TickType_t ticks) {
    note("delay %u on %s", (unsigned)(ticks * 1000u / configTICK_RATE_HZ), thread_name);
    usleep(200);
}

struct fake_mutex {
    pthread_mutex_t mu;
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a mutex fits its static buffer");

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)buffer->opaque;
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
    (void)wait;
    pthread_mutex_lock(&mutex->mu);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    pthread_mutex_unlock(&mutex->mu);
    return pdTRUE;
}

struct fake_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char *items;
    size_t item_size, length, head, count;
};

QueueHandle_t xQueueCreateWithCaps(UBaseType_t length, UBaseType_t item_size, UBaseType_t caps) {
    note("queue %u x %u caps 0x%x", length, item_size, caps);
    struct fake_queue *q = calloc(1, sizeof *q);
    if (q == NULL) {
        die("out of memory");
    }
    q->items = calloc(length, item_size);
    q->item_size = item_size;
    q->length = length;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
    (void)wait;  /* apps_net.c never waits for room */
    pthread_mutex_lock(&q->mu);
    if (q->count == q->length) {
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
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) {
        if (wait != portMAX_DELAY) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;  /* only the worker receives, and it waits for ever */
        }
        pthread_cond_wait(&q->cv, &q->mu);
    }
    memcpy(item, q->items + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%c %s: %s\n", level, tag, line);
    note("log %c %s", level, line);
}

/* ---- The heap: every block counted, any thread ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static pthread_mutex_t heap_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t held;
static atomic_bool refuse_on_worker;          /* every growth on the worker is refused */
static atomic_size_t refuse_worker_at_least;  /* growths on the worker to this many bytes or more are refused (0: none) */
static atomic_size_t refuse_size_once;        /* the next growth of exactly this many bytes on the app's thread is refused (0: none) */

static bool on_worker(void) {
    return strcmp(thread_name, "muse_apps_net") == 0;
}

static bool refused(size_t n) {
    if (on_worker()) {
        size_t at_least = atomic_load(&refuse_worker_at_least);
        return atomic_load(&refuse_on_worker) || (at_least != 0 && n >= at_least);
    }
    size_t once = atomic_load(&refuse_size_once);
    if (once != 0 && n == once) {
        atomic_store(&refuse_size_once, 0);
        note("refused %zu on the app task", n);
        return true;
    }
    return false;
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (n == 0) {
        die("apps_port_realloc with n == 0");
    }
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old && refused(n)) {
        return NULL;
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    pthread_mutex_lock(&heap_mu);
    held = held - old + n;
    pthread_mutex_unlock(&heap_mu);
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        pthread_mutex_lock(&heap_mu);
        held -= b->size;
        pthread_mutex_unlock(&heap_mu);
        free(b);
    }
}

static size_t held_now(void) {
    pthread_mutex_lock(&heap_mu);
    size_t n = held;
    pthread_mutex_unlock(&heap_mu);
    return n;
}

/* ---- The rest of the port ---- */

static _Atomic int64_t clock_ms = 100000;
static _Atomic int64_t epoch_ms_now = 1700000000000LL;
static atomic_bool epoch_valid = true;

int64_t apps_port_mono_ms(void) {
    return atomic_load(&clock_ms);
}

bool apps_port_epoch_ms(int64_t *out) {
    if (!atomic_load(&epoch_valid)) {
        return false;
    }
    *out = atomic_load(&epoch_ms_now);
    return true;
}

bool apps_port_dark(void) {
    return false;  /* the module's "in front and lit" is the round's own (active) */
}

size_t apps_port_psram_free(void) {
    return 8u << 20;
}

void apps_port_ui_lock(void) {}
bool apps_port_ui_trylock(int ms) {
    (void)ms;
    return true;
}
void apps_port_ui_unlock(void) {}
void apps_port_init(void) {}
void apps_port_log_drain(void) {}
void apps_port_console_flush(void) {}

void apps_port_sha256(const void *data, size_t len, uint8_t out[32]) {
    (void)data;
    (void)len;
    memset(out, 0, 32);
}

apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    (void)app, (void)name, (void)ms, (void)sound, (void)wake;
    return APPS_ALARM_REFUSED;
}
void apps_port_alarm_cancel(const char *app, const char *name) {
    (void)app, (void)name;
}
int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    (void)app, (void)name;
    return -1;
}
int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    (void)app, (void)names, (void)remaining;
    return 0;
}
void apps_port_sound_chirp(void) {}
void apps_port_sound_stop(void) {}

char *apps_port_store_load(const char *app, size_t *len) {
    (void)app;
    char *doc = apps_port_realloc(NULL, 3);
    if (doc != NULL) {
        memcpy(doc, "{}", 3);
    }
    *len = doc != NULL ? 2 : 0;
    return doc;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    (void)app, (void)json, (void)len;
    return true;
}

apps_battery_t apps_port_battery(void) {
    return (apps_battery_t){.pct = 50};
}

bool apps_port_wifi(char *ssid, size_t cap) {
    snprintf(ssid, cap, "%s", "");
    return false;
}

void apps_port_show(void) {}
void apps_port_keep_awake(bool on) {
    (void)on;
}
void apps_port_exit(void) {}

/* The app's log: what print wrote, on the app task's thread. */
#define LOGGED_MAX 512
static char *logged[LOGGED_MAX];
static int logged_n;

void apps_port_log(const char *app_id, const char *line) {
    (void)app_id;
    if (logged_n < LOGGED_MAX) {
        logged[logged_n++] = strdup(line);
    }
}

static void forget_logged(void) {
    for (int i = 0; i < logged_n; i++) {
        free(logged[i]);
    }
    logged_n = 0;
}

static void expect_logged_at(int line, const char *const *want, int n) {
    bool same = logged_n == n;
    for (int i = 0; same && i < n; i++) {
        same = strcmp(logged[i], want[i]) == 0;
    }
    CHECK_AT(line, same, "logged %d lines (want %d)", logged_n, n);
    if (!same) {
        for (int i = 0; i < logged_n || i < n; i++) {
            fprintf(stderr, "  [%d] got  '%s'\n       want '%s'\n", i, i < logged_n ? logged[i] : "(none)", i < n ? want[i] : "(none)");
        }
    }
    forget_logged();
}
/* The lines logged since the last check, exactly; then forgotten. */
#define expect_logged(...)                                                      \
    do {                                                                        \
        const char *const want_[] = {__VA_ARGS__};                              \
        expect_logged_at(__LINE__, want_, (int)(sizeof want_ / sizeof want_[0])); \
    } while (0)
#define expect_no_logs() expect_logged_at(__LINE__, NULL, 0)

/* ---- The VM's files: muse_apps_vm_ops ---- */

#define VM_FILES 64
#define VM_PIECE 8192

typedef struct {
    bool used;
    char path[APPS_VM_PATH_MAX];
    uint8_t *data;
    size_t len;
} vm_file_rec_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    vm_file_rec_t files[VM_FILES];
    bool offline;               /* every op: the session is gone */
    int read_busy, write_busy;  /* the next ones answer busy */
    bool hold;                  /* ops wait while it is set, until it is let go or their deadline comes */
    size_t drop_after;          /* a read drops after this many bytes: SIZE_MAX for never */
    const char *write_code, *write_err;  /* the writes fail so, while set */
    int in_op, max_in_op, started, wrong_thread;
    int64_t last_deadline_us;
} vm = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .drop_after = SIZE_MAX};

static atomic_bool no_ops;  /* muse_apps_vm_ops() is NULL: a gadget without the VM's ops */

#define APP_DIR APPS_VM_DIR "app/"

static const char *rel_of(const char *path) {
    return strncmp(path, APP_DIR, strlen(APP_DIR)) == 0 ? path + strlen(APP_DIR) : path;
}

static vm_file_rec_t *vm_find(const char *path) {
    for (int i = 0; i < VM_FILES; i++) {
        if (vm.files[i].used && strcmp(vm.files[i].path, path) == 0) {
            return &vm.files[i];
        }
    }
    return NULL;
}

/* The file rel of the app's folder, with data[0, len) (mu held). */
static void vm_store(const char *path, const void *data, size_t len) {
    vm_file_rec_t *f = vm_find(path);
    for (int i = 0; f == NULL && i < VM_FILES; i++) {
        if (!vm.files[i].used) {
            f = &vm.files[i];
            f->used = true;
            snprintf(f->path, sizeof f->path, "%s", path);
        }
    }
    if (f == NULL) {
        die("the VM's folder is full");
    }
    free(f->data);
    f->data = malloc(len != 0 ? len : 1);
    if (f->data == NULL) {
        die("out of memory");
    }
    memcpy(f->data, data, len);
    f->len = len;
}

static void vm_put(const char *rel, const void *data, size_t len) {
    char path[APPS_VM_PATH_MAX];
    snprintf(path, sizeof path, "%s%s", APP_DIR, rel);
    pthread_mutex_lock(&vm.mu);
    vm_store(path, data, len);
    pthread_mutex_unlock(&vm.mu);
}

static void vm_put_text(const char *rel, const char *text) {
    vm_put(rel, text, strlen(text));
}

static void vm_remove(const char *rel) {
    char path[APPS_VM_PATH_MAX];
    snprintf(path, sizeof path, "%s%s", APP_DIR, rel);
    pthread_mutex_lock(&vm.mu);
    vm_file_rec_t *f = vm_find(path);
    if (f != NULL) {
        free(f->data);
        *f = (vm_file_rec_t){0};
    }
    pthread_mutex_unlock(&vm.mu);
}

/* The file's text, as written (a NUL-terminated copy the caller frees), or NULL when there is none. */
static char *vm_text(const char *rel) {
    char path[APPS_VM_PATH_MAX];
    snprintf(path, sizeof path, "%s%s", APP_DIR, rel);
    pthread_mutex_lock(&vm.mu);
    vm_file_rec_t *f = vm_find(path);
    char *text = NULL;
    if (f != NULL) {
        text = malloc(f->len + 1);
        if (text == NULL) {
            die("out of memory");
        }
        memcpy(text, f->data, f->len);
        text[f->len] = '\0';
    }
    pthread_mutex_unlock(&vm.mu);
    return text;
}

static void expect_file_at(int line, const char *rel, const char *want) {
    char *text = vm_text(rel);
    CHECK_AT(line, text != NULL && strcmp(text, want) == 0, "%s is '%s', want '%s'", rel, text != NULL ? text : "(none)", want);
    free(text);
}
#define expect_file(rel, want) expect_file_at(__LINE__, rel, want)

static void vm_set_hold(bool hold) {
    pthread_mutex_lock(&vm.mu);
    vm.hold = hold;
    pthread_cond_broadcast(&vm.cv);
    pthread_mutex_unlock(&vm.mu);
}

/* An op begins (mu held): on the worker alone, one at a time. */
static void op_begin(int64_t deadline_us) {
    vm.in_op++;
    vm.started++;
    if (vm.in_op > vm.max_in_op) {
        vm.max_in_op = vm.in_op;
    }
    if (!on_worker()) {
        vm.wrong_thread++;
    }
    vm.last_deadline_us = deadline_us;
    pthread_cond_broadcast(&vm.cv);
}

/* Waits while the ops are held (mu held): false when the deadline came first. */
static bool op_wait(int64_t deadline_us) {
    while (vm.hold) {
        if (apps_port_mono_ms() * 1000 >= deadline_us) {
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&vm.cv, &vm.mu, &ts);
    }
    return true;
}

static bool fail(const char **code, const char *c, char *err, size_t err_len, const char *why) {
    *code = c;
    snprintf(err, err_len, "%s", why);
    return false;
}

static bool vm_read(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                    int64_t deadline_us, const char **code, char *err, size_t err_len) {
    pthread_mutex_lock(&vm.mu);
    op_begin(deadline_us);
    note("vm read %s max %zu", rel_of(path), max);
    bool ok = false;
    if (vm.offline) {
        ok = fail(code, MUSE_APPS_VM_DROPPED, err, err_len, "could not send the request to the VM");
    } else if (vm.read_busy > 0) {
        vm.read_busy--;
        ok = fail(code, MUSE_APPS_VM_BUSY, err, err_len, "the VM file reader is busy (an image download)");
    } else if (!op_wait(deadline_us)) {
        ok = fail(code, MUSE_APPS_VM_TIMED_OUT, err, err_len, "download timed out");
    } else {
        vm_file_rec_t *f = vm_find(path);
        if (f == NULL) {
            ok = fail(code, MUSE_APPS_VM_NOT_FOUND, err, err_len, "VM answered HTTP 404");
        } else if (f->len > max) {
            char why[64];
            snprintf(why, sizeof why, "larger than %zu bytes", max);
            ok = fail(code, MUSE_APPS_VM_TOO_LARGE, err, err_len, why);
        } else {
            size_t len = f->len, drop = vm.drop_after;
            uint8_t *copy = malloc(len != 0 ? len : 1);
            if (copy == NULL) {
                die("out of memory");
            }
            memcpy(copy, f->data, len);
            pthread_mutex_unlock(&vm.mu);
            size_t at = 0;
            bool stopped = false;
            while (at < len && at < drop && !stopped) {
                size_t piece = len - at < VM_PIECE ? len - at : VM_PIECE;
                if (drop != SIZE_MAX && at + piece > drop) {
                    piece = drop - at;
                }
                stopped = !sink(user, copy + at, piece);
                at += piece;
            }
            free(copy);
            pthread_mutex_lock(&vm.mu);
            if (stopped) {
                char why[64];
                snprintf(why, sizeof why, "larger than %zu bytes", max);
                ok = fail(code, MUSE_APPS_VM_FAILED, err, err_len, why);  /* vm_file says a sink's stop so */
            } else if (at < len) {
                ok = fail(code, MUSE_APPS_VM_DROPPED, err, err_len, "the VM connection dropped");
            } else {
                ok = true;
            }
        }
    }
    vm.in_op--;
    pthread_mutex_unlock(&vm.mu);
    return ok;
}

static bool vm_write(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                     size_t err_len) {
    pthread_mutex_lock(&vm.mu);
    op_begin(deadline_us);
    note("vm write %s %.*s", rel_of(path), (int)(len < 200 ? len : 200), (const char *)data);
    bool ok = false;
    if (vm.offline) {
        ok = fail(code, MUSE_APPS_VM_DROPPED, err, err_len, "could not send the request to the VM");
    } else if (vm.write_busy > 0) {
        vm.write_busy--;
        ok = fail(code, MUSE_APPS_VM_BUSY, err, err_len, "the VM file writer is busy");
    } else if (!op_wait(deadline_us)) {
        ok = fail(code, MUSE_APPS_VM_TIMED_OUT, err, err_len, "write timed out");
    } else if (vm.write_code != NULL) {
        ok = fail(code, vm.write_code, err, err_len, vm.write_err);
    } else {
        vm_store(path, data, len);
        ok = true;
    }
    vm.in_op--;
    pthread_mutex_unlock(&vm.mu);
    return ok;
}

static const muse_apps_vm_ops_t VM_OPS = {vm_read, vm_write};

const muse_apps_vm_ops_t *muse_apps_vm_ops(void) {
    return atomic_load(&no_ops) ? NULL : &VM_OPS;
}

/* Waits (in real time) until n ops have begun. */
static void wait_ops_at(int line, int n) {
    double until = real_s() + 10;
    pthread_mutex_lock(&vm.mu);
    while (vm.started < n && real_s() < until) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&vm.cv, &vm.mu, &ts);
    }
    int started = vm.started;
    pthread_mutex_unlock(&vm.mu);
    CHECK_AT(line, started >= n, "%d ops began, want %d", started, n);
}
#define wait_ops(n) wait_ops_at(__LINE__, n)

/* ---- The app, and the app task's side of the worker ---- */

static apps_pkg_t *pkg;
static apps_rt_t *rt;
static uint32_t gen;            /* the current app's (the app task's T.gen) */
static atomic_uint live_gen;    /* the app whose requests are wanted (0: none), as apps_task.c's vm_alive answers */
static atomic_int submitted, taken, skipped, done_posts;
static bool active = true;      /* in front and lit, as the round tells the module */

bool apps_port_vm_submit(struct apps_vm_job *job) {
    job->app_gen = gen;
    if (!apps_net_submit(job)) {
        note("submit refused");
        return false;
    }
    atomic_fetch_add(&submitted, 1);
    return true;
}

static bool vm_alive(uint32_t app_gen) {
    bool alive = app_gen != 0 && atomic_load(&live_gen) == app_gen;
    if (!alive) {
        note("skipped a job of app %u", (unsigned)app_gen);
        atomic_fetch_add(&skipped, 1);
    }
    return alive;
}

static void vm_done(void) {
    atomic_fetch_add(&done_posts, 1);
}

static char *exact_copy(const char *src, size_t len) {
    char *p = malloc(len != 0 ? len : 1);
    if (p == NULL) {
        die("out of memory");
    }
    memcpy(p, src, len);
    return p;
}

/* What every app here starts with: a message handler that runs T[event], S(v) that shows a value as a callback got it, and cb(name)
 * and try(f, ...) that print it. */
#define APP_HEAD                                                                                             \
    "local app = {}\n"                                                                                       \
    "local T = {}\n"                                                                                         \
    "function app.message(event, data) return T[event](data) end\n"                                          \
    "local function S(v)\n"                                                                                  \
    "  if v == json.null then return 'null' end\n"                                                           \
    "  local t = type(v)\n"                                                                                  \
    "  if t == 'table' then return json.encode(v) end\n"                                                     \
    "  if t == 'userdata' then\n"                                                                            \
    "    local s = tostring(v)\n"                                                                            \
    "    if s:find('image#', 1, true) == 1 then local w, h = v:size() return s .. ' ' .. w .. 'x' .. h end\n" \
    "    return s\n"                                                                                         \
    "  end\n"                                                                                                \
    "  return tostring(v)\n"                                                                                 \
    "end\n"                                                                                                  \
    "local function cb(name) return function(a, b) print(name, S(a), S(b)) end end\n"                        \
    "local function try(f, ...) local ok, e = pcall(f, ...) print(ok, e) end\n"
#define APP(body) APP_HEAD body "return app\n"

static apps_pkg_t *make_pkg(const char *src, unsigned perms) {
    apps_pkg_t *p = calloc(1, sizeof *p);
    if (p == NULL) {
        die("out of memory");
    }
    snprintf(p->manifest.id, sizeof p->manifest.id, "app");
    snprintf(p->manifest.name, sizeof p->manifest.name, "App");
    snprintf(p->manifest.main, sizeof p->manifest.main, "main.lua");
    snprintf(p->manifest.files[0], sizeof p->manifest.files[0], "main.lua");
    p->manifest.file_count = 1;
    p->manifest.memory_kb = 512;
    p->manifest.perms = (uint8_t)perms;
    snprintf(p->files[0].path, sizeof p->files[0].path, "main.lua");
    p->files[0].len = strlen(src);
    p->files[0].data = (uint8_t *)exact_copy(src, p->files[0].len);
    p->count = 1;
    return p;
}

static void start_app_perms_at(int line, const char *src, unsigned perms) {
    gen++;
    pkg = make_pkg(src, perms);
    char err[256] = "";
    rt = apps_rt_new(pkg, err, sizeof err);
    CHECK_AT(line, rt != NULL, "apps_rt_new: %s", err);
    if (rt == NULL) {
        exit(1);
    }
    CHECK_AT(line, apps_rt_start(rt), "start: %s", apps_rt_last_error(rt));
    atomic_store(&live_gen, gen);
}
#define start_app(src) start_app_perms_at(__LINE__, src, APPS_PERM_VM)
#define start_app_perms(src, perms) start_app_perms_at(__LINE__, src, perms)

static void end_app(void) {
    atomic_store(&live_gen, 0);
    apps_rt_free(rt);
    rt = NULL;
    apps_pkg_free(pkg, NULL);
    free(pkg);
    pkg = NULL;
}

static bool running(void) {
    return rt != NULL && apps_rt_state(rt) == APPS_STATE_RUNNING;
}

/* message(event): the app's T[event](). */
static void call_at(int line, const char *event) {
    char err[APPS_ERROR_MAX] = "";
    char *reply = apps_rt_message(rt, event, NULL, 0, err, sizeof err);
    CHECK_AT(line, reply != NULL, "message %s: %s", event, err);
    apps_port_free(reply);
}
#define call(event) call_at(__LINE__, event)

/* apps_task.c's take_vm_results: each result, for its app if it still runs, or freed. */
static void take_results(void) {
    for (apps_vm_job_t *job = apps_net_take(); job != NULL;) {
        apps_vm_job_t *next = job->next;
        atomic_fetch_add(&taken, 1);
        if (running() && job->app_gen == gen && atomic_load(&live_gen) == gen) {
            apps_vmio_take(rt, job, apps_port_mono_ms());
        } else {
            note("dropped a result of app %u", (unsigned)job->app_gen);
            apps_vm_job_free(job);
        }
        job = next;
    }
}

/* One round of the app task's loop, as far as the module goes. */
static void round_once(void) {
    take_results();
    if (running()) {
        apps_vmio_tick(rt, apps_port_mono_ms(), active);
        if (apps_vmio_has_work(rt)) {
            (void)apps_vmio_run_one(rt);
        }
    }
}

static bool worker_idle(void) {
    return atomic_load(&submitted) == atomic_load(&taken) + atomic_load(&skipped);
}

/* Rounds until nothing is at the worker, nothing waits for a callback and a round sent nothing new: what can happen at this time
 * has happened. */
static void settle_at(int line) {
    double until = real_s() + 20;
    for (;;) {
        int before = atomic_load(&submitted);
        round_once();
        bool work = running() && apps_vmio_has_work(rt);
        if (!work && atomic_load(&submitted) == before && worker_idle()) {
            return;
        }
        if (real_s() > until) {
            CHECK_AT(line, false, "the vm module never settled: %d sent, %d taken, %d skipped", atomic_load(&submitted),
                     atomic_load(&taken), atomic_load(&skipped));
            dump_notes();
            exit(1);
        }
        if (!worker_idle()) {
            usleep(100);
        }
    }
}
#define settle() settle_at(__LINE__)

/* Time passes, then the rounds that follow. */
static void advance_at(int line, int64_t ms) {
    atomic_fetch_add(&clock_ms, ms);
    settle_at(line);
}
#define advance(ms) advance_at(__LINE__, ms)

/* Every case ends here: the app gone, every job back, every byte given back, and the ops were the worker's, one at a time. */
static void finish(void) {
    if (rt != NULL) {
        end_app();
    }
    double until = real_s() + 10;
    while ((!worker_idle() || held_now() != 0) && real_s() < until) {
        take_results();
        usleep(500);
    }
    take_results();
    CHECK(worker_idle(), "%d sent, %d taken, %d skipped", atomic_load(&submitted), atomic_load(&taken), atomic_load(&skipped));
    CHECK(held_now() == 0, "%zu bytes still held", held_now());
    pthread_mutex_lock(&vm.mu);
    CHECK(vm.max_in_op <= 1, "%d ops at once", vm.max_in_op);
    CHECK(vm.wrong_thread == 0, "%d ops off the worker", vm.wrong_thread);
    pthread_mutex_unlock(&vm.mu);
    forget_logged();
}

/* The line of main.lua that holds marker (1-based). */
static int line_of(const char *src, const char *marker) {
    const char *at = strstr(src, marker);
    if (at == NULL) {
        die("no marker in the source");
    }
    int line = 1;
    for (const char *p = src; p < at; p++) {
        line += *p == '\n' ? 1 : 0;
    }
    return line;
}

/* ---- Test files ---- */

/* The fake TJpgDec's format (tests/fake_tjpgd.h): "FJPG", the mode, the MCU's blocks, w, h, bytes to skip, then "END!"; pad bytes
 * after it, which the decoder never reads, make a file as long as a case wants. */
static void put_jpeg(const char *rel, int w, int h, char mode, unsigned skip, size_t pad) {
    size_t len = 14 + skip + 4 + pad;
    uint8_t *b = calloc(1, len);
    if (b == NULL) {
        die("out of memory");
    }
    memcpy(b, "FJPG", 4);
    b[4] = (uint8_t)mode;
    b[5] = 1;
    b[6] = 1;
    b[8] = (uint8_t)(w & 255);
    b[9] = (uint8_t)(w >> 8);
    b[10] = (uint8_t)(h & 255);
    b[11] = (uint8_t)(h >> 8);
    b[12] = (uint8_t)(skip & 255);
    b[13] = (uint8_t)(skip >> 8);
    memcpy(b + 14 + skip, "END!", 4);
    vm_put(rel, b, len);
    free(b);
}

/* A 2 x 1 RGB565 LVGL image. */
static const uint8_t SMALL_BIN[] = {0x19, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};

static void put_filled(const char *rel, size_t len, char fill, const char *head, const char *tail) {
    char *b = malloc(len);
    if (b == NULL) {
        die("out of memory");
    }
    memset(b, fill, len);
    memcpy(b, head, strlen(head));
    memcpy(b + len - strlen(tail), tail, strlen(tail));
    vm_put(rel, b, len);
    free(b);
}

/* ---- The cases: the brief's ---- */

static void read_json_text_jpg_bin(void) {
    vm_put_text("data.json", "{\"a\":[1,2,{\"b\":\"c\"}]}");
    vm_put_text("note.txt", "hello world");
    put_jpeg("pic.jpg", 200, 100, 'B', 0, 0);
    vm_put("small.bin", SMALL_BIN, sizeof SMALL_BIN);
    put_jpeg("circle.jpg", 600, 300, 'B', 0, 0);
    vm_put_text("bad.json", "{\"a\":");
    put_filled("big.json", APPS_VM_JSON_MAX + 1, 'a', "\"", "\"");
    put_filled("big.txt", APPS_VM_TXT_MAX + 1, 't', "", "");
    put_filled("edge.json", APPS_VM_JSON_MAX, 'a', "\"", "\"");
    put_filled("edge.txt", APPS_VM_TXT_MAX, 't', "", "");
    put_jpeg("big.jpg", 200, 100, 'B', 0, APPS_VM_JPEG_MAX + 1 - 18);
    put_filled("big.bin", APPS_VM_BIN_MAX + 1, 0, "", "");
    vm_put_text("bad.bin", "not an image");
    put_jpeg("prog.jpg", 200, 100, 'P', 0, 0);
    put_jpeg("pic.jpeg", 200, 100, 'B', 0, 0);
    put_jpeg("more.jpg", 20, 10, 'B', 0, 0);
    start_app(APP(
        "local held = {}\n"
        "local function keep(name) return function(v, e) held[#held + 1] = v print(name, S(v), S(e)) end end\n"
        "T.a = function()\n"
        "  vm.read('data.json', cb('data.json'))\n"
        "  vm.read('note.txt', cb('note.txt'))\n"
        "  vm.read('pic.jpg', keep('pic.jpg'))\n"
        "  vm.read('small.bin', keep('small.bin'))\n"
        "  vm.read('circle.jpg', {circle = true, max = 100}, keep('circle.jpg'))\n"
        "  vm.read('bad.json', cb('bad.json'))\n"
        "  vm.read('big.json', cb('big.json'))\n"
        "  vm.read('big.txt', cb('big.txt'))\n"
        "  vm.read('missing.json', cb('missing.json'))\n"
        "end\n"
        "T.b = function()\n"
        "  vm.read('edge.json', function(v, e) print('edge.json', type(v), #v, S(e)) end)\n"
        "  vm.read('edge.txt', function(v, e) print('edge.txt', #v, S(e)) end)\n"
        "  vm.read('big.jpg', cb('big.jpg'))\n"
        "  vm.read('big.bin', cb('big.bin'))\n"
        "  vm.read('bad.bin', cb('bad.bin'))\n"
        "  vm.read('prog.jpg', cb('prog.jpg'))\n"
        "  vm.read('pic.jpeg', nil, keep('pic.jpeg'))\n"
        "  vm.read('more.jpg', cb('more.jpg'))\n"
        "end\n"));
    call("a");
    settle();
    expect_logged("data.json\t{\"a\":[1,2,{\"b\":\"c\"}]}\tnil", "note.txt\thello world\tnil", "pic.jpg\timage#0 200x100\tnil",
                  "small.bin\timage#1 2x1\tnil", "circle.jpg\timage#2 75x37\tnil",
                  "bad.json\tnil\tbad.json is not valid JSON: unexpected end of input at byte 6",
                  "big.json\tnil\tbig.json is larger than 32 KB", "big.txt\tnil\tbig.txt is larger than 16 KB",
                  "missing.json\tnil\tno such file on the VM: missing.json");
    /* Each kind asks for its own most, of the right file. */
    CHECK(count_notes("vm read data.json max 32768") == 1, "data.json's read");
    CHECK(count_notes("vm read note.txt max 16384") == 1, "note.txt's read");
    CHECK(count_notes("vm read pic.jpg max 262144") == 1, "pic.jpg's read");
    CHECK(count_notes("vm read small.bin max 524288") == 1, "small.bin's read");
    call("b");
    settle();
    expect_logged("edge.json\tstring\t32766\tnil", "edge.txt\t16384\tnil", "big.jpg\tnil\tbig.jpg is larger than 256 KB",
                  "big.bin\tnil\tbig.bin is larger than 512 KB",
                  "bad.bin\tnil\timage: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)",
                  "prog.jpg\tnil\tunsupported JPEG: use baseline, not progressive", "pic.jpeg\timage#3 200x100\tnil",
                  "more.jpg\tnil\tno more than 4 images at once: free one with img:free()");
    finish();
}

static void paths_are_confined(void) {
    start_app(APP(
        "T.go = function()\n"
        "  local f = function() end\n"
        "  try(vm.read, '../x.json', f)\n"
        "  try(vm.read, '/etc/x.json', f)\n"
        "  try(vm.read, '.hidden.json', f)\n"
        "  try(vm.read, 'a/.b.json', f)\n"
        "  try(vm.read, 'a/../b.json', f)\n"
        "  try(vm.read, 'a//b.json', f)\n"
        "  try(vm.read, 'A.json', f)\n"
        "  try(vm.read, '', f)\n"
        "  try(vm.read, string.rep('a', 60) .. '.json', f)\n"
        "  try(vm.read, 5, f)\n"
        "  try(vm.read, 'x.lua', f)\n"
        "  try(vm.read, 'x', f)\n"
        "  try(vm.read, 'x.json')\n"
        "  try(vm.read, 'x.json', nil)\n"
        "  try(vm.read, 'x.jpg', {max = 0}, f)\n"
        "  try(vm.read, 'x.jpg', {max = 467}, f)\n"
        "  try(vm.read, 'x.jpg', {circle = 1}, f)\n"
        "  try(vm.read, 'x.jpg', {bogus = 1}, f)\n"
        "  try(vm.read, 'x.jpg', 5, f)\n"
        "  try(vm.write, 'outbox/../x.json', {})\n"
        "  try(vm.write, '../outbox/x.json', {})\n"
        "  try(vm.watch, '../state/rev', 2, f)\n"
        "  try(vm.watch, '/state/rev', 2, f)\n"
        "  try(vm.command, '../x', nil)\n"
        "end\n"));
    call("go");
    settle();
#define RULE " is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 characters)"
    expect_logged("false\tvm.read: path '../x.json'" RULE, "false\tvm.read: path '/etc/x.json'" RULE,
                  "false\tvm.read: path '.hidden.json'" RULE, "false\tvm.read: path 'a/.b.json'" RULE,
                  "false\tvm.read: path 'a/../b.json'" RULE, "false\tvm.read: path 'a//b.json'" RULE,
                  "false\tvm.read: path 'A.json'" RULE, "false\tvm.read: path ''" RULE,
                  "false\tvm.read: path 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.jso...'" RULE,
                  "false\tvm.read: path must be a string, got number",
                  "false\tvm.read: path must end in .json, .txt, .jpg, .jpeg or .bin",
                  "false\tvm.read: path must end in .json, .txt, .jpg, .jpeg or .bin",
                  "false\tvm.read: cb must be a function, got no value", "false\tvm.read: cb must be a function, got nil",
                  "false\tvm.read: max must be a whole number from 1 to 466",
                  "false\tvm.read: max must be a whole number from 1 to 466",
                  "false\tvm.read: circle must be true or false, got number",
                  "false\tvm.read: unknown option 'bogus' (valid: circle, max)",
                  "false\tvm.read: opts must be a table like {circle = true, max = 200}, got number",
                  "false\tvm.write: path 'outbox/../x.json'" RULE, "false\tvm.write: path '../outbox/x.json'" RULE,
                  "false\tvm.watch: path '../state/rev'" RULE, "false\tvm.watch: path '/state/rev'" RULE,
                  "false\tvm.command: name must be 1-32 characters of a-z, 0-9 and _");
#undef RULE
    CHECK(atomic_load(&submitted) == 0 && vm.started == 0, "%d requests, %d ops", atomic_load(&submitted), vm.started);
    advance(5000);
    CHECK(atomic_load(&submitted) == 0 && vm.started == 0, "none later either: %d requests", atomic_load(&submitted));
    expect_no_logs();
    finish();
}

static void write_only_outbox(void) {
    start_app(APP(
        "T.go = function()\n"
        "  vm.write('outbox/note.json', {a = 1}, cb('note'))\n"
        "  vm.write('outbox/n.json', nil)\n"
        "  vm.write('outbox/s.json', string.rep('x', 8190), cb('edge'))\n"
        "  try(vm.write, 'state/x.json', {})\n"
        "  try(vm.write, 'outbox', {})\n"
        "  try(vm.write, 'outboxes/x.json', {})\n"
        "  try(vm.write, 'outbox/big.json', string.rep('x', 8191))\n"
        "  try(vm.write, 'outbox/f.json', function() end)\n"
        "  try(vm.write, 'outbox/f.json', {}, 5)\n"
        "end\n"
        "T.refused = function() vm.write('outbox/r.json', 1, cb('refused')) end\n"
        "T.busy = function() vm.write('outbox/b.json', 2, cb('busy')) end\n"));
    call("go");
    expect_logged("false\tvm.write: only outbox/ can be written", "false\tvm.write: only outbox/ can be written",
                  "false\tvm.write: only outbox/ can be written", "false\tvm.write: value is larger than 8 KB as JSON",
                  "false\tvm.write: value can't be encoded: can't encode a function",
                  "false\tvm.write: cb must be a function, got number");
    settle();
    expect_logged("note\ttrue\tnil", "edge\ttrue\tnil");
    expect_file("outbox/note.json", "{\"a\":1}");
    expect_file("outbox/n.json", "null");
    char *s = vm_text("outbox/s.json");
    CHECK(s != NULL && strlen(s) == APPS_VM_WRITE_MAX && s[0] == '"' && s[APPS_VM_WRITE_MAX - 1] == '"', "8 KB written whole");
    free(s);
    CHECK(count_notes("vm write ") == 3, "three writes");

    /* What the VM says of a write it refuses, and a writer that stays busy. */
    vm.write_code = MUSE_APPS_VM_FAILED;
    vm.write_err = "VM answered HTTP 403: route not allowed";
    call("refused");
    settle();
    expect_logged("refused\tfalse\tVM answered HTTP 403: route not allowed");
    vm.write_code = NULL;
    vm.write_busy = APPS_VM_BUSY_RETRIES + 1;
    call("busy");
    settle();
    expect_logged("busy\tfalse\tthe VM file writer is busy");
    CHECK(count_notes("vm write outbox/b.json") == APPS_VM_BUSY_RETRIES + 1, "tried 4 times");
    finish();
}

static void command_acks_and_times_out(void) {
    const char *src = APP(
        "T.go = function() vm.command('play_pause', {x = 1}, cb('cmd')) end\n"
        "T.two = function() vm.command('skip', nil, cb('skip')) end\n"
        "T.bare = function() vm.command('next', 5, cb('bare')) end\n"
        "T.bad = function()\n"
        "  try(vm.command, 'Play', {})\n"
        "  try(vm.command, string.rep('a', 33), {})\n"
        "  try(vm.command, '', {})\n"
        "  try(vm.command, 5, {})\n"
        "  try(vm.command, 'ok', function() end)\n"
        "  try(vm.command, 'ok', string.rep('x', 5000))\n"
        "  try(vm.command, 'ok', {}, 5)\n"
        "  try(vm.command, string.rep('a', 32), {})\n"
        "end\n"
        "T.noclock = function() try(vm.command, 'ok', {}) end\n");
    start_app(src);
    call("bad");
    expect_logged("false\tvm.command: name must be 1-32 characters of a-z, 0-9 and _",
                  "false\tvm.command: name must be 1-32 characters of a-z, 0-9 and _",
                  "false\tvm.command: name must be 1-32 characters of a-z, 0-9 and _",
                  "false\tvm.command: name must be 1-32 characters of a-z, 0-9 and _",
                  "false\tvm.command: args can't be encoded: can't encode a function",
                  "false\tvm.command: args are larger than 4 KB as JSON", "false\tvm.command: cb must be a function, got number",
                  "true\tnil");
    settle();  /* the 32-character name was taken: its command is written, with no callback to wait for */
    CHECK(count_notes("vm write outbox/cmd-1700000000000.json") == 1, "the good one written");
    CHECK(count_notes("vm read state/acks.json") == 0, "a command with no callback reads no acks");
    atomic_store(&epoch_valid, false);
    call("noclock");
    expect_logged("false\tvm.command: the gadget's clock isn't set yet");
    atomic_store(&epoch_valid, true);

    /* An ack that comes on the third read. */
    call("go");
    settle();
    expect_file("outbox/cmd-1700000000001.json", "{\"seq\":1700000000001,\"name\":\"play_pause\",\"args\":{\"x\":1}}");
    CHECK(count_notes("vm read state/acks.json") == 0, "no read before a second has passed");
    advance(APPS_VM_ACK_POLL_MS - 1);
    CHECK(count_notes("vm read state/acks.json") == 0, "not before 1 s");
    advance(1);
    CHECK(count_notes("vm read state/acks.json") == 1, "the first read, which finds no file");
    vm_put_text("state/acks.json", "{\"acks\":[{\"seq\":5,\"result\":1}]}");
    advance(APPS_VM_ACK_POLL_MS);
    CHECK(count_notes("vm read state/acks.json") == 2, "the second, which doesn't list it");
    expect_no_logs();
    vm_put_text("state/acks.json", "{\"acks\":[{\"seq\":5},{\"seq\":1700000000001,\"result\":{\"playing\":true}}]}");
    advance(APPS_VM_ACK_POLL_MS);
    CHECK(count_notes("vm read state/acks.json") == 3, "the third");
    expect_logged("cmd\ttrue\t{\"playing\":true}");
    advance(5 * APPS_VM_ACK_POLL_MS);
    CHECK(count_notes("vm read state/acks.json") == 3, "no read once nothing waits");

    /* One that is never acked: 14 reads, then "timed out" at 15 s, and no read after. */
    call("two");
    settle();
    expect_file("outbox/cmd-1700000000002.json", "{\"seq\":1700000000002,\"name\":\"skip\",\"args\":null}");
    for (int i = 0; i < APPS_VM_ACK_MS / APPS_VM_ACK_POLL_MS - 1; i++) {
        advance(APPS_VM_ACK_POLL_MS);
    }
    CHECK(count_notes("vm read state/acks.json") == 3 + 14, "14 reads: %d", count_notes("vm read state/acks.json") - 3);
    expect_no_logs();
    advance(APPS_VM_ACK_POLL_MS - 1);
    expect_no_logs();
    advance(1);
    expect_logged("skip\tfalse\ttimed out");
    advance(3 * APPS_VM_ACK_POLL_MS);
    CHECK(count_notes("vm read state/acks.json") == 3 + 14, "no read after the time");

    /* An ack with no result: nil. */
    call("bare");
    settle();
    expect_file("outbox/cmd-1700000000003.json", "{\"seq\":1700000000003,\"name\":\"next\",\"args\":5}");
    vm_put_text("state/acks.json", "{\"acks\":[{\"seq\":1700000000003}]}");
    advance(APPS_VM_ACK_POLL_MS);
    expect_logged("bare\ttrue\tnil");
    finish();
}

static void command_seq_increases_across_restarts(void) {
    const char *src = APP("T.go = function() vm.command('a', nil) end\n"
                          "T.two = function() vm.command('a', nil) vm.command('b', nil) end\n");
    start_app(src);
    call("two");  /* two in the same ms */
    settle();
    expect_file("outbox/cmd-1700000000000.json", "{\"seq\":1700000000000,\"name\":\"a\",\"args\":null}");
    expect_file("outbox/cmd-1700000000001.json", "{\"seq\":1700000000001,\"name\":\"b\",\"args\":null}");
    end_app();
    /* A restart, 5 ms later on the wall clock: the epoch is the base, so the new app's seq is past the old one's. */
    atomic_fetch_add(&epoch_ms_now, 5);
    start_app(src);
    call("go");
    settle();
    expect_file("outbox/cmd-1700000000005.json", "{\"seq\":1700000000005,\"name\":\"a\",\"args\":null}");
    /* The clock set back: one more than the last. */
    atomic_fetch_sub(&epoch_ms_now, 10000);
    call("go");
    settle();
    expect_file("outbox/cmd-1700000000006.json", "{\"seq\":1700000000006,\"name\":\"a\",\"args\":null}");
    CHECK(count_notes("vm write outbox/cmd-") == 4, "four commands");
    finish();
}

static void watch_fires_first_and_on_change_only(void) {
    vm_put_text("state/rev", "1");
    start_app(APP(
        "local w\n"
        "T.go = function() w = vm.watch('state/rev', 2, cb('rev')) print(tostring(w)) end\n"
        "T.cancel = function() w:cancel() w:cancel() print('cancelled') end\n"
        "T.bad = function()\n"
        "  local f = function() end\n"
        "  try(vm.watch, 'state/rev', 0, f)\n"
        "  try(vm.watch, 'state/rev', 61, f)\n"
        "  try(vm.watch, 'state/rev', 1.5, f)\n"
        "  try(vm.watch, 'state/rev', '2', f)\n"
        "  try(vm.watch, 'state/rev', 2)\n"
        "  try(vm.watch, 'state/rev')\n"
        "end\n"
        "T.four = function()\n"
        "  for i = 1, 3 do vm.watch('state/other', 60, function() end) end\n"
        "  try(vm.watch, 'state/rev', 60, function() end)\n"
        "end\n"
        "T.default = function() vm.watch('state/x', cb('x')) end\n"));
    call("go");
    expect_logged("watch#0");
    settle();
    expect_logged("rev\t1\tnil");
    CHECK(count_notes("vm read state/rev max 1024") == 1, "the first read, at once, of at most 1 KB");
    advance(1999);
    CHECK(count_notes("vm read state/rev") == 1, "every_s from the end of the last read");
    advance(1);
    CHECK(count_notes("vm read state/rev") == 2, "the second read");
    expect_no_logs();  /* the same text: quiet */
    vm_put_text("state/rev", "2");
    advance(2000);
    expect_logged("rev\t2\tnil");
    /* P15: an error once for each change of outcome. */
    vm_remove("state/rev");
    advance(2000);
    expect_logged("rev\tnil\tno such file on the VM: state/rev");
    advance(2000);
    expect_no_logs();
    vm.offline = true;
    advance(2000);
    expect_logged("rev\tnil\tthe VM is not connected");
    advance(2000);
    expect_no_logs();
    vm.offline = false;
    vm_put_text("state/rev", "2");
    advance(2000);
    expect_logged("rev\t2\tnil");  /* a success after an error calls back, the same text or not */
    put_filled("state/rev", APPS_VM_WATCH_MAX + 1, '9', "", "");
    advance(2000);
    expect_logged("rev\tnil\tstate/rev is larger than 1 KB");
    vm_put_text("state/rev", "3");
    advance(2000);
    expect_logged("rev\t3\tnil");
    CHECK(count_notes("vm read state/rev") == 10, "a read every 2 s: %d", count_notes("vm read state/rev"));

    call("bad");
    expect_logged("false\tvm.watch: every_s must be a whole number from 1 to 60",
                  "false\tvm.watch: every_s must be a whole number from 1 to 60",
                  "false\tvm.watch: every_s must be a whole number from 1 to 60",
                  "false\tvm.watch: every_s must be a whole number from 1 to 60", "false\tvm.watch: cb must be a function, got no value",
                  "false\tvm.watch: cb must be a function, got no value");
    call("four");
    expect_logged("false\tvm.watch: no more than 4 watches per app");
    settle();
    call("cancel");
    expect_logged("cancelled");
    vm_put_text("state/rev", "4");
    int reads = count_notes("vm read state/rev");
    advance(4000);
    CHECK(count_notes("vm read state/rev") == reads, "a cancelled watch reads no more");
    expect_no_logs();
    call("default");  /* the fourth again, with every_s left out: 2 s */
    settle();
    expect_logged("x\tnil\tno such file on the VM: state/x");
    advance(1999);
    CHECK(count_notes("vm read state/x") == 1, "every_s 2 by default");
    advance(1);
    CHECK(count_notes("vm read state/x") == 2, "every_s 2 by default");
    finish();
}

static void watch_pauses_when_behind_or_dark(void) {
    vm_put_text("state/rev", "1");
    vm_put_text("data.json", "[1]");
    start_app(APP("T.go = function() vm.watch('state/rev', 1, cb('rev')) end\n"
                  "T.read = function() vm.read('data.json', cb('data')) end\n"));
    call("go");
    settle();
    expect_logged("rev\t1\tnil");
    CHECK(count_notes("vm write outbox/active.json") == 1 && count_notes("vm read state/rev") == 1, "a heartbeat and a read");
    /* Behind, or dark (the app task's "in front and lit" is false either way): no request, and the heartbeat stops. */
    active = false;
    vm_put_text("state/rev", "2");
    advance(30000);
    CHECK(count_notes("vm write outbox/active.json") == 1 && count_notes("vm read state/rev") == 1,
          "nothing while behind or dark: %d heartbeats, %d reads", count_notes("vm write outbox/active.json"),
          count_notes("vm read state/rev"));
    expect_no_logs();
    /* P13: a read's callback runs whatever the state. */
    call("read");
    settle();
    expect_logged("data\t[1]\tnil");
    CHECK(count_notes("vm read state/rev") == 1, "still no watch read");
    /* In front and lit again: the heartbeat at once, and the watch that is overdue. */
    active = true;
    settle();
    expect_logged("rev\t2\tnil");
    CHECK(count_notes("vm write outbox/active.json") == 2 && count_notes("vm read state/rev") == 2, "both again");
    finish();
}

static void heartbeat_every_10_s_while_watching(void) {
    start_app(APP("local a, b\n"
                  "T.one = function() a = vm.watch('state/rev', 60, function() end) end\n"
                  "T.two = function() b = vm.watch('state/other', 60, function() end) end\n"
                  "T.stop = function() a:cancel() b:cancel() end\n"));
    advance(20000);
    CHECK(count_notes("vm write outbox/active.json") == 0, "no heartbeat without a watch");
    call("one");
    settle();
    CHECK(count_notes("vm write outbox/active.json") == 1, "one at once");
    expect_file("outbox/active.json", "{\"app\":\"app\",\"n\":1}");
    advance(APPS_VM_HEARTBEAT_MS - 1);
    CHECK(count_notes("vm write outbox/active.json") == 1, "not before 10 s");
    advance(1);
    CHECK(count_notes("vm write outbox/active.json") == 2, "then every 10 s");
    call("two");
    settle();
    CHECK(count_notes("vm write outbox/active.json") == 2, "a second watch doesn't write at once");
    advance(APPS_VM_HEARTBEAT_MS);
    CHECK(count_notes("vm write outbox/active.json") == 3, "the next");
    expect_file("outbox/active.json", "{\"app\":\"app\",\"n\":2}");
    /* A heartbeat the VM can't take is the next one's to say. */
    vm.offline = true;
    advance(APPS_VM_HEARTBEAT_MS);
    vm.offline = false;
    advance(APPS_VM_HEARTBEAT_MS);
    CHECK(count_notes("vm write outbox/active.json") == 5, "every 10 s regardless");
    expect_no_logs();
    call("stop");
    settle();
    advance(3 * APPS_VM_HEARTBEAT_MS);
    CHECK(count_notes("vm write outbox/active.json") == 5, "none once the last watch is cancelled");
    finish();
}

static void one_request_in_flight_and_queue_of_8(void) {
    const char *names = "abcdefghij";
    for (const char *n = names; *n != '\0'; n++) {
        char rel[16], text[8];
        snprintf(rel, sizeof rel, "%c.json", *n);
        snprintf(text, sizeof text, "\"%c\"", *n);
        vm_put_text(rel, text);
    }
    vm_put_text("state/rev", "1");
    start_app(APP("T.go = function()\n"
                  "  for _, n in ipairs({'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'}) do vm.read(n .. '.json', cb(n)) end\n"
                  "end\n"
                  "T.watch = function() vm.watch('state/rev', 1, cb('rev')) end\n"
                  "T.more = function() vm.read('a.json', cb('again')) end\n"));
    vm_set_hold(true);
    call("go");
    round_once();  /* the first goes to the VM, and the tenth's refusal is answered */
    wait_ops(1);
    expect_logged("j\tnil\ttoo many vm requests at once");
    call("watch");  /* P11: the watch's polls and the heartbeat take no place among the 8 */
    for (int i = 0; i < 20; i++) {
        round_once();
        usleep(500);
    }
    CHECK(vm.started == 1, "one at the VM at a time: %d began", vm.started);
    expect_no_logs();
    vm_set_hold(false);
    settle();
    expect_logged("a\ta\tnil", "rev\t1\tnil", "b\tb\tnil", "c\tc\tnil", "d\td\tnil", "e\te\tnil", "f\tf\tnil", "g\tg\tnil", "h\th\tnil",
                  "i\ti\tnil");
    CHECK(vm.max_in_op == 1, "%d at once", vm.max_in_op);
    CHECK(count_notes("vm write outbox/active.json") == 1, "the heartbeat went too");
    call("more");
    settle();
    expect_logged("again\ta\tnil");
    finish();
}

static void busy_reader_retries_then_fails(void) {
    vm_put_text("a.json", "1");
    vm_put_text("b.json", "2");
    start_app(APP("T.a = function() vm.read('a.json', cb('a')) end\n"
                  "T.b = function() vm.read('b.json', cb('b')) end\n"));
    vm.read_busy = APPS_VM_BUSY_RETRIES;
    call("a");
    settle();
    expect_logged("a\t1\tnil");
    CHECK(count_notes("vm read a.json") == APPS_VM_BUSY_RETRIES + 1, "busy 3 times, then read: %d", count_notes("vm read a.json"));
    CHECK(count_notes("delay 500 on muse_apps_net") == APPS_VM_BUSY_RETRIES, "500 ms between: %d",
          count_notes("delay 500 on muse_apps_net"));
    vm.read_busy = APPS_VM_BUSY_RETRIES + 1;
    call("b");
    settle();
    expect_logged("b\tnil\tthe VM file reader is busy");
    CHECK(count_notes("vm read b.json") == APPS_VM_BUSY_RETRIES + 1, "tried 4 times");
    CHECK(count_notes("delay 500 on muse_apps_net") == 2 * APPS_VM_BUSY_RETRIES, "3 waits again");
    finish();
}

static void vm_session_drop_mid_read(void) {
    put_jpeg("pic.jpg", 200, 100, 'B', 60000, 0);
    put_filled("data.json", 10000, ' ', "[", "]");
    start_app(APP("T.pic = function(name) vm.read('pic.jpg', cb('pic')) end\n"
                  "T.data = function() vm.read('data.json', cb('data')) end\n"));
    vm.drop_after = 30000;
    call("pic");
    settle();
    expect_logged("pic\tnil\tthe VM is not connected");
    vm.drop_after = 10;
    call("data");
    settle();
    expect_logged("data\tnil\tthe VM is not connected");
    vm.drop_after = SIZE_MAX;
    call("pic");
    settle();
    expect_logged("pic\timage#0 200x100\tnil");
    finish();
}

static void vm_result_after_the_app_ended_is_dropped(void) {
    put_jpeg("pic.jpg", 200, 100, 'B', 0, 0);
    vm_put_text("a.json", "1");
    vm_put_text("b.json", "2");
    const char *src = APP("T.pic = function() vm.read('pic.jpg', cb('pic')) end\n"
                          "T.a = function() vm.read('a.json', cb('a')) end\n"
                          "T.b = function() vm.read('b.json', cb('b')) end\n");
    /* At the VM when the app ends: the decoded picture comes back to nobody, and is freed. */
    start_app(src);
    vm_set_hold(true);
    call("pic");
    round_once();
    wait_ops(1);
    end_app();
    vm_set_hold(false);
    expect_no_logs();
    finish();
    CHECK(count_notes("dropped a result of app 1") == 1, "dropped by its generation");

    /* Back from the worker, not taken yet, when the app ends. */
    start_app(src);
    int posts = atomic_load(&done_posts);
    call("a");
    round_once();
    double until = real_s() + 10;
    while (atomic_load(&done_posts) == posts && real_s() < until) {
        usleep(200);
    }
    CHECK(atomic_load(&done_posts) == posts + 1, "the worker said it was done");
    end_app();
    expect_no_logs();
    finish();
    CHECK(count_notes("dropped a result of app 2") == 1, "dropped by its generation");

    /* Queued at the worker for an app that has ended by its turn: never run. */
    start_app(src);
    vm_set_hold(true);
    call("a");
    round_once();
    wait_ops(3);
    end_app();
    start_app(src);
    call("b");
    round_once();  /* queued behind the held one */
    end_app();
    vm_set_hold(false);
    expect_no_logs();  /* no callback ran for any of them */
    finish();
    CHECK(count_notes("skipped a job of app 4") == 1, "the worker skipped it");
    CHECK(count_notes("vm read b.json") == 0, "and made no request for it");
}

static void callback_error_crashes_like_a_timer(void) {
    vm_put_text("a.json", "1");
    vm_put_text("b.json", "2");
    const char *src = APP("T.w = function() vm.watch('a.json', 1, cb('w')) end\n"
                          "T.go = function()\n"
                          "  vm.read('a.json', function(v) error('boom') end)\n"
                          "  vm.read('b.json', cb('b'))\n"
                          "end\n"
                          "T.timer = function() timer.after(40, function() error('bang') end) end\n");
    start_app(src);
    call("w");
    settle();
    expect_logged("w\t1\tnil");
    call("go");
    settle();
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED, "crashed: %s", apps_state_name(apps_rt_state(rt)));
    char want[64];
    snprintf(want, sizeof want, "main.lua:%d: boom", line_of(src, "error('boom')"));
    CHECK(strncmp(apps_rt_last_error(rt), want, strlen(want)) == 0, "'%s', want '%s'", apps_rt_last_error(rt), want);
    int ops = vm.started;
    advance(5000);
    CHECK(vm.started == ops, "no request after the crash");
    expect_no_logs();  /* neither b's callback nor the watch's */
    end_app();
    /* A timer's error, for the record's form. */
    start_app(src);
    call("timer");
    atomic_fetch_add(&clock_ms, 40);
    (void)apps_rt_run_one_timer(rt, apps_port_mono_ms());
    snprintf(want, sizeof want, "main.lua:%d: bang", line_of(src, "error('bang')"));
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strncmp(apps_rt_last_error(rt), want, strlen(want)) == 0, "'%s'",
          apps_rt_last_error(rt));
    finish();
}

/* ---- The cases: the rulings' and the module's own ---- */

static void vm_needs_its_permission(void) {
    const char *src = APP("T.go = function()\n"
                          "  local f = function() end\n"
                          "  try(vm.read, 'a.json', f)\n"
                          "  try(vm.write, 'outbox/a.json', 1)\n"
                          "  try(vm.command, 'a', nil)\n"
                          "  try(vm.watch, 'state/rev', 2, f)\n"
                          "  print(sys.has('vm'), type(vm))\n"
                          "end\n");
    start_app_perms(src, APPS_PERM_STORE);
    call("go");
    expect_logged("false\tvm: add \"vm\" to \"permissions\" in manifest.json", "false\tvm: add \"vm\" to \"permissions\" in manifest.json",
                  "false\tvm: add \"vm\" to \"permissions\" in manifest.json", "false\tvm: add \"vm\" to \"permissions\" in manifest.json",
                  "true\ttable");
    settle();
    CHECK(vm.started == 0, "no request");
    end_app();
    vm_put_text("a.json", "7");
    start_app(src);
    call("go");
    expect_logged("true\tnil", "true\tnil", "true\tnil", "true\twatch#0", "true\ttable");
    finish();
}

static void a_request_times_out_after_10_s(void) {
    vm_put_text("a.json", "1");
    start_app(APP("T.a = function() vm.read('a.json', cb('a')) end\n"
                  "T.w = function() vm.write('outbox/w.json', 1, cb('w')) end\n"));
    vm_set_hold(true);
    int64_t now = apps_port_mono_ms();
    call("a");
    round_once();
    wait_ops(1);
    CHECK(vm.last_deadline_us == (now + APPS_VM_REQUEST_MS) * 1000, "the deadline: %lld", (long long)vm.last_deadline_us);
    atomic_fetch_add(&clock_ms, APPS_VM_REQUEST_MS - 1);
    for (int i = 0; i < 20; i++) {
        round_once();
        usleep(500);
    }
    expect_no_logs();
    advance(1);
    expect_logged("a\tnil\ttimed out");
    call("w");
    round_once();
    wait_ops(2);
    advance(APPS_VM_REQUEST_MS);
    expect_logged("w\tfalse\ttimed out");
    vm_set_hold(false);
    finish();
}

static void a_refused_buffer_is_a_clean_error(void) {
    vm_put_text("data.json", "{\"a\":1}");
    put_jpeg("pic.jpg", 200, 100, 'B', 0, 0);
    start_app(APP("T.data = function() vm.read('data.json', cb('data')) end\n"
                  "T.pic = function() vm.read('pic.jpg', cb('pic')) end\n"
                  "T.job = function() vm.read('data.json', cb('job')) end\n"));
    atomic_store(&refuse_on_worker, true);  /* the read's buffer */
    call("data");
    settle();
    expect_logged("data\tnil\tthe gadget is short of memory");
    atomic_store(&refuse_on_worker, false);
    atomic_store(&refuse_worker_at_least, 12 + 200 * 100 * 2);  /* the decoded picture's block */
    call("pic");
    settle();
    expect_logged("pic\tnil\tthe gadget is short of memory");
    atomic_store(&refuse_worker_at_least, 0);
    int ops = vm.started;
    atomic_store(&refuse_size_once, sizeof(apps_vm_job_t));  /* the request's own block, at the call */
    call("job");
    settle();
    expect_logged("job\tnil\tthe gadget is short of memory");
    CHECK(count_notes("refused ") == 1 && vm.started == ops, "refused at the call, and no request made");
    call("pic");  /* and all is well after */
    settle();
    expect_logged("pic\timage#0 200x100\tnil");
    finish();
}

static void a_stop_releases_every_callback(void) {
    vm_put_text("state/rev", "1");
    vm_put_text("data.json", "1");
    start_app(APP("T.go = function()\n"
                  "  vm.watch('state/rev', 1, cb('rev'))\n"
                  "  vm.command('a', nil, cb('cmd'))\n"
                  "  vm.read('data.json', cb('data'))\n"
                  "end\n"
                  "T.more = function() vm.read('data.json', cb('more')) vm.write('outbox/x.json', 1, cb('x')) end\n"
                  "function app.stop() print('stopped') end\n"));
    call("go");
    settle();
    expect_logged("rev\t1\tnil", "data\t1\tnil");  /* the command waits for its ack */
    vm_set_hold(true);
    int ops = vm.started;
    call("more");
    round_once();
    wait_ops(ops + 1);
    apps_rt_stop(rt);
    expect_logged("stopped");
    vm_set_hold(false);
    for (int i = 0; i < 20; i++) {
        advance(1000);
    }
    CHECK(vm.started == ops + 1, "no request after the stop but the one at the VM: %d", vm.started - ops);
    expect_no_logs();  /* no callback: not the read's, the write's, the watch's or the command's time */
    finish();
}

static void no_vm_ops_is_not_connected(void) {
    start_app(APP("T.go = function()\n"
                  "  vm.read('a.json', cb('a'))\n"
                  "  vm.write('outbox/w.json', 1, cb('w'))\n"
                  "  vm.command('c', nil, cb('cmd'))\n"
                  "  vm.watch('state/rev', 1, cb('rev'))\n"
                  "end\n"));
    atomic_store(&no_ops, true);
    call("go");
    settle();
    expect_logged("rev\tnil\tthe VM is not connected", "a\tnil\tthe VM is not connected", "w\tfalse\tthe VM is not connected",
                  "cmd\tfalse\tthe VM is not connected");
    advance(5000);
    expect_no_logs();  /* the watch says it once */
    atomic_store(&no_ops, false);
    vm.offline = true;  /* ops, but no session */
    call("go");
    settle();
    expect_logged("rev\tnil\tthe VM is not connected", "a\tnil\tthe VM is not connected", "w\tfalse\tthe VM is not connected",
                  "cmd\tfalse\tthe VM is not connected");
    vm.offline = false;
    finish();
}

static void the_watch_handle_is_protected(void) {
    start_app(APP("T.go = function()\n"
                  "  local w = vm.watch('state/rev', 2, function() end)\n"
                  "  print(type(w), getmetatable(w), tostring(w))\n"
                  "  try(function() return w.foo end)\n"
                  "  try(function() w.x = 1 end)\n"
                  "  try(w.cancel, 5)\n"
                  "  try(w.cancel)\n"
                  "  print(rawequal(w, w), w == w)\n"
                  "  w:cancel()\n"
                  "  print(tostring(w))\n"
                  "end\n"));
    call("go");
    /* A metamethod's error has the line of the app's code that touched the handle, as an image's has (apps_rt.c). */
    expect_logged("userdata\tmuse.watch\twatch#0", "false\tmain.lua:20: watch: unknown method 'foo' (valid: cancel)",
                  "false\tmain.lua:21: watch: a watch has no fields: h:cancel() stops it",
                  "false\twatch: cancel needs a watch: call it as h:cancel()",
                  "false\twatch: cancel needs a watch: call it as h:cancel()", "true\ttrue", "watch#0");
    settle();
    CHECK(count_notes("vm read state/rev") == 0, "cancelled before its first read");
    finish();
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"read_json_text_jpg_bin", read_json_text_jpg_bin},
        {"paths_are_confined", paths_are_confined},
        {"write_only_outbox", write_only_outbox},
        {"command_acks_and_times_out", command_acks_and_times_out},
        {"command_seq_increases_across_restarts", command_seq_increases_across_restarts},
        {"watch_fires_first_and_on_change_only", watch_fires_first_and_on_change_only},
        {"watch_pauses_when_behind_or_dark", watch_pauses_when_behind_or_dark},
        {"heartbeat_every_10_s_while_watching", heartbeat_every_10_s_while_watching},
        {"one_request_in_flight_and_queue_of_8", one_request_in_flight_and_queue_of_8},
        {"busy_reader_retries_then_fails", busy_reader_retries_then_fails},
        {"vm_session_drop_mid_read", vm_session_drop_mid_read},
        {"vm_result_after_the_app_ended_is_dropped", vm_result_after_the_app_ended_is_dropped},
        {"callback_error_crashes_like_a_timer", callback_error_crashes_like_a_timer},
        {"vm_needs_its_permission", vm_needs_its_permission},
        {"a_request_times_out_after_10_s", a_request_times_out_after_10_s},
        {"a_refused_buffer_is_a_clean_error", a_refused_buffer_is_a_clean_error},
        {"a_stop_releases_every_callback", a_stop_releases_every_callback},
        {"no_vm_ops_is_not_connected", no_vm_ops_is_not_connected},
        {"the_watch_handle_is_protected", the_watch_handle_is_protected},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    apps_net_init(vm_done, vm_alive);
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            if (atomic_load(&failures) != 0) {
                dump_notes();
            }
            return atomic_load(&failures) == 0 ? 0 : 1;
        }
    }
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
