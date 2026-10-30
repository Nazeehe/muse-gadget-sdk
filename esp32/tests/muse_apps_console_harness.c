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
 * The USB console's ">app.*" protocol (apps_console.c), run for real on the host: muse_apps_console as muse_input's
 * hook calls it, with the real install pipeline and RAM store (apps_install.c, apps_pkg.c, apps_json.c, apps_vm.c and
 * the real cJSON) under it; and Link's app.control and app.api (apps_control.c) over the same fakes, the guide and the
 * timer embedded as the firmware embeds them. Usage: muse_apps_console_harness <case>, one case per test in
 * tests/test_muse_apps_console.py; it exits 0 when the case holds, and otherwise prints why.
 *
 * Fakes: apps_io_submit keeps the jobs and the case runs them (the I/O worker); the app task's calls (compile, run,
 * stop, status, message, and the store's two: the launcher's list and the app that runs) record what they were given
 * and answer what the case set. stdout, where the console prints
 * its replies with printf, is a file the harness reads back after each line (the fake printf sink); at the end the
 * whole of it goes to the real stdout, where the Python test checks that every reply is one line of valid JSON. The
 * port is a counting allocator that can refuse, and cJSON's allocations are counted too, so each case ends with
 * nothing held.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_port.h"
#include "apps_store.h"
#include "apps_task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_apps.h"
#include "muse_apps_exact_stack.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define KB 1024u

static const uint8_t ZEROS[512 * 1024];

/* ---- The output: stdout is a file, read back after each line ---- */

static int saved_stdout = -1, capture_fd = -1;
static off_t capture_taken;
static off_t flushed_at; /* where the output ended at the last apps_port_console_flush */
static int flushes;

static void capture_start(void) {
    char path[] = "/tmp/muse_apps_console_XXXXXX";
    capture_fd = mkstemp(path);
    if (capture_fd < 0) {
        fprintf(stderr, "no capture file: %s\n", strerror(errno));
        _exit(2);
    }
    unlink(path);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(capture_fd, STDOUT_FILENO);
}

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));

/* What the console printed since the last take, NUL-terminated (the caller frees it). Every reply ends with
 * apps_port_console_flush, so whatever was printed ends where the last flush saw the output end. */
static char *take_output(void) {
    fflush(stdout);
    off_t end = lseek(capture_fd, 0, SEEK_END);
    size_t n = (size_t)(end - capture_taken);
    char *text = malloc(n + 1);
    size_t got = 0;
    while (text != NULL && got < n) {
        ssize_t r = pread(capture_fd, text + got, n - got, capture_taken + (off_t)got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    if (text != NULL) {
        text[got] = '\0';
    }
    capture_taken += (off_t)got;
    if (got > 0 && end != flushed_at) {
        fail_at(__LINE__, "the console printed\n%s\nwithout apps_port_console_flush after it", text != NULL ? text : "");
    }
    return text;
}

/* Everything the console printed, to the real stdout, for the Python test. */
static void capture_end(void) {
    fflush(stdout);
    if (saved_stdout < 0) {
        return;
    }
    char buf[8192];
    off_t at = 0;
    for (;;) {
        ssize_t r = pread(capture_fd, buf, sizeof buf, at);
        if (r <= 0) {
            break;
        }
        ssize_t w = write(saved_stdout, buf, (size_t)r);
        (void)w;
        at += r;
    }
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
    capture_end();
    fflush(stderr);
    _exit(1);
}

#define CHECK(cond, ...)                    \
    do {                                    \
        if (!(cond)) {                      \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                   \
    } while (0)

/* The console's output since the last take is exactly want. */
static void expect_at(int line, const char *want) {
    char *got = take_output();
    if (got == NULL || strcmp(got, want) != 0) {
        fail_at(line, "the console printed\n%s\ninstead of\n%s", got != NULL ? got : "(null)", want);
    }
    free(got);
}
#define EXPECT(want) expect_at(__LINE__, (want))

/* The console printed one line since the last take, which starts with prefix and holds every one of parts (NULL-ended).
 */
static void expect_line_at(int line, const char *prefix, ...) {
    char *got = take_output();
    if (got == NULL || strncmp(got, prefix, strlen(prefix)) != 0 || strchr(got, '\n') != got + strlen(got) - 1) {
        fail_at(line, "the console printed\n%s\nnot one line starting %s", got != NULL ? got : "(null)", prefix);
    }
    va_list ap;
    va_start(ap, prefix);
    for (const char *part = va_arg(ap, const char *); part != NULL; part = va_arg(ap, const char *)) {
        if (strstr(got, part) == NULL) {
            va_end(ap);
            fail_at(line, "the console printed\n%s\nwithout %s", got, part);
        }
    }
    va_end(ap);
    free(got);
}
#define EXPECT_LINE(prefix, ...) expect_line_at(__LINE__, (prefix), __VA_ARGS__, (const char *)NULL)

/* ---- The port ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t port_held;      /* apps_port_realloc's bytes */
static size_t port_peak;      /* the most port_held has been since a case zeroed it */
static long port_growths;     /* the requests for a bigger block, since a case zeroed it */
static long port_refuse = -1; /* refuse the Nth growth request from now (0: the next), -1: never */

void *apps_port_realloc(void *ptr, size_t n) {
    CHECK(n > 0, "apps_port_realloc(%p, 0)", ptr);
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    port_growths += n > old;
    if (n > old && port_refuse >= 0 && port_refuse-- == 0) {
        return NULL;
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    port_held = port_held - old + n;
    port_peak = port_held > port_peak ? port_held : port_peak;
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        port_held -= b->size;
        free(b);
    }
}

static int64_t fake_ms = 1000;

int64_t apps_port_mono_ms(void) {
    return fake_ms;
}

bool apps_port_epoch_ms(int64_t *out) {
    (void)out;
    return false;
}

bool apps_port_dark(void) {
    return false;
}

/* The system's alarms and sound (apps_port.h): none here, as no case runs an app's alarm. */
apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    (void)app;
    (void)name;
    (void)ms;
    (void)sound;
    (void)wake;
    return APPS_ALARM_REFUSED;
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    (void)app;
    (void)name;
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    (void)app;
    (void)name;
    return -1;
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    (void)app;
    (void)names;
    (void)remaining;
    return 0;
}

void apps_port_sound_chirp(void) {}

void apps_port_sound_stop(void) {}

/* The store, the battery, the Wi-Fi and the front (apps_port.h): none here, as no case runs an app that uses them. */
char *apps_port_store_load(const char *app, size_t *len) {
    (void)app;
    *len = 0;
    return NULL;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    (void)app;
    (void)json;
    (void)len;
    return false;
}

apps_battery_t apps_port_battery(void) {
    return (apps_battery_t){.pct = -1, .charging = false, .usb = false};
}

bool apps_port_wifi(char *ssid, size_t cap) {
    if (cap > 0) {
        ssid[0] = '\0';
    }
    return false;
}

void apps_port_show(void) {}

void apps_port_keep_awake(bool on) {
    (void)on;
}

void apps_port_exit(void) {}

/* The store's room: plenty (tests/test_muse_apps_install.py has the budget). */
size_t apps_port_psram_free(void) {
    return (size_t)64 << 20;
}

void apps_port_log(const char *app_id, const char *line) {
    (void)app_id;
    (void)line;
}

/* The end of a reply: always right after its newline, with a whole line printed since the last one. */
void apps_port_console_flush(void) {
    fflush(stdout);
    off_t end = lseek(capture_fd, 0, SEEK_END);
    char last = 0;
    CHECK(end > flushed_at && pread(capture_fd, &last, 1, end - 1) == 1 && last == '\n',
          "apps_port_console_flush without a whole line printed since the last one");
    flushed_at = end;
    flushes++;
}

void apps_port_init(void) {}

void apps_port_ui_lock(void) {}

bool apps_port_ui_trylock(int ms) {
    (void)ms;
    return true;
}

void apps_port_ui_unlock(void) {}

/* apps_port_sha256 is tools/muse/apphost/sha256.c's. */

/* The console's log lines go to stderr; the last one and their count are kept for the cases, and the last 32 lines. */
static char last_log[300];
static int logs;
static char log_ring[32][300];

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(last_log, sizeof last_log, "%c %s: ", level, tag);
    vsnprintf(last_log + n, sizeof last_log - (size_t)n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", last_log);
    memcpy(log_ring[logs % 32], last_log, sizeof last_log);
    logs++;
}

/* The line of the last 32 logged that holds text, or NULL. */
static const char *logged(const char *text) {
    for (int i = logs - 1; i >= 0 && i >= logs - 32; i--) {
        if (strstr(log_ring[i % 32], text) != NULL) {
            return log_ring[i % 32];
        }
    }
    return NULL;
}

/* The internal RAM heap (esp_heap_caps.h): the blocks a synchronous wait of app.control takes for its one atomic (counted,
 * and refused when a case says so), and the DMA RAM the guide's writes log. */
static int internal_blocks;
static bool internal_refuses;
static size_t dma_free = 24315, dma_lowest = 12731;

void *heap_caps_malloc(size_t size, uint32_t caps) {
    CHECK(caps & MALLOC_CAP_INTERNAL, "only internal blocks here: caps 0x%x", (unsigned)caps);
    if (internal_refuses) {
        return NULL;
    }
    void *p = malloc(size);
    internal_blocks += p != NULL;
    return p;
}

void heap_caps_free(void *ptr) {
    if (ptr != NULL) {
        internal_blocks--;
        free(ptr);
    }
}

size_t heap_caps_get_free_size(uint32_t caps) {
    CHECK(caps == MALLOC_CAP_DMA, "the DMA RAM: caps 0x%x", (unsigned)caps);
    return dma_free;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    CHECK(caps == MALLOC_CAP_DMA, "the DMA RAM: caps 0x%x", (unsigned)caps);
    return dma_lowest;
}

/* vTaskDelay: the clock moves on by the ticks (10 ms each), and the worker runs what waits while the caller sleeps, unless a
 * case holds it in a long job (worker_busy). */
static bool worker_busy;
static int delays;
static void run_jobs(void);

void vTaskDelay(TickType_t ticks) {
    CHECK(ticks > 0, "a delay of no ticks spins");
    delays++;
    fake_ms += (int64_t)ticks * 10;
    if (!worker_busy) {
        run_jobs();
    }
}

/* cJSON's allocations, counted. */
static size_t cjson_held;
static long cjson_refuse = -1; /* refuse the Nth allocation from now (0: the next), -1: never */

static void *cjson_malloc(size_t n) {
    if (cjson_refuse >= 0 && cjson_refuse-- == 0) {
        return NULL;
    }
    block_t *b = malloc(sizeof *b + n);
    if (b == NULL) {
        return NULL;
    }
    b->size = n;
    cjson_held += n;
    return b + 1;
}

static void cjson_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        cjson_held -= b->size;
        free(b);
    }
}

/* ---- The fakes: the I/O worker ---- */

static struct {
    void (*fn)(void *);
    void *arg;
} jobs[16];
static int n_jobs;
static bool submit_refuses;

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    if (submit_refuses || n_jobs == (int)ARRAY_LEN(jobs)) {
        return false;
    }
    jobs[n_jobs].fn = fn;
    jobs[n_jobs].arg = arg;
    n_jobs++;
    return true;
}

void apps_io_init(void) {}

/* The worker: every job, in order (jobs a job submits too). */
static void run_jobs(void) {
    while (n_jobs > 0) {
        void (*fn)(void *) = jobs[0].fn;
        void *arg = jobs[0].arg;
        memmove(&jobs[0], &jobs[1], (size_t)(n_jobs - 1) * sizeof jobs[0]);
        n_jobs--;
        fn(arg);
    }
}

/* ---- The fakes: the store of packages (apps_store.h) ---- */

/* The RAM store's packages (apps_install.c), as on a gadget without the apps partition, in whatever mode a case gives. */
static apps_storage_t store_mode = APPS_STORAGE_RAM;
static uint32_t store_free_kb;

apps_storage_t apps_store_mode(void) {
    return store_mode;
}

const char *apps_storage_name(apps_storage_t mode) {
    return mode == APPS_STORAGE_FLASH ? "flash" : mode == APPS_STORAGE_RAM ? "ram" : "none";
}

bool apps_store_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n) {
    return apps_ram_commit(pkg, unchanged, err, n);
}

/* A refusal of the store's load, as a case sets it (the quarantine's, a damaged copy's), or NULL. */
static const char *store_load_refusal;

bool apps_store_load(const char *id, apps_pkg_t *pkg, char *err, size_t n) {
    if (store_load_refusal != NULL) {
        memset(pkg, 0, sizeof *pkg);
        snprintf(err, n, "%s", store_load_refusal);
        return false;
    }
    return apps_ram_load(id, pkg, err, n);
}

/* apps_store_remove over the RAM store, with apps_store.c's refusals: not ready, the app that runs, none of that id; or the
 * refusal a case sets (remove_refusal: a damaged folder the file system won't let go of). */
static int removes;
static const char *remove_refusal;
bool apps_task_running(char *id, size_t cap);

bool apps_store_remove(const char *id, char *err, size_t n) {
    removes++;
    char running[APPS_ID_MAX + 1];
    if (remove_refusal != NULL) {
        snprintf(err, n, "%s", remove_refusal);
        return false;
    }
    if (store_mode == APPS_STORAGE_NONE) {
        snprintf(err, n, "storage not ready");
        return false;
    }
    if (apps_task_running(running, sizeof running) && strcmp(running, id) == 0) {
        snprintf(err, n, "%s is running; stop it first", id);
        return false;
    }
    if (!apps_ram_remove(id)) {
        snprintf(err, n, "%s", APPS_STORE_NO_SUCH_APP);
        return false;
    }
    snprintf(err, n, "%s", "");
    return true;
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
    return store_free_kb;
}

/* What results and app.list add about where apps are kept: in RAM mode the reflash hint of a gadget without the partition, unless the case
 * gives the store another (store_hint: P7's, for a partition that couldn't be mounted). */
static const char *store_hint;

const char *apps_store_hint(void) {
    return store_mode != APPS_STORAGE_RAM ? NULL : store_hint != NULL ? store_hint : APPS_RAM_HINT;
}

/* ---- The fakes: the stage's lock (a FreeRTOS mutex) ---- */

/* This harness has one thread, the serial task's and the app task's both: a take that would wait finds a holder that can never give it
 * back, and fails the case. console_holds_the_stage makes the app task's try (wait 0) find the console at the stage. */
struct fake_mutex {
    bool held;
};
static bool console_holds_the_stage;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)buffer;
    m->held = false;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    if (wait == 0 && console_holds_the_stage) {
        return pdFALSE;
    }
    CHECK(!m->held, "a take of the stage's lock that would wait forever");
    m->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    CHECK(m->held, "a give of the stage's lock, which nobody holds");
    m->held = false;
    return pdTRUE;
}

/* ---- The fakes: the app task ---- */

/* The record's few words (apps_task_brief), as a case sets them. */
static struct {
    char id[APPS_ID_MAX + 1];
    apps_state_t state;
    uint32_t mem_kb, peak_kb;
} brief;

void apps_task_brief(char *id, size_t cap, apps_state_t *state, uint32_t *mem_kb, uint32_t *peak_kb) {
    snprintf(id, cap, "%s", brief.id);
    *state = brief.state;
    *mem_kb = brief.mem_kb;
    *peak_kb = brief.peak_kb;
}

/* What compile was given: each file's path and bytes. */
static struct {
    char path[APPS_PATH_MAX + 1];
    uint8_t *data;
    size_t len;
} compiled[APPS_MAX_FILES];
static int n_compiled, compiles;
static bool compile_ok = true;
static char compile_err[200];

static void forget_compiled(void) {
    for (int i = 0; i < n_compiled; i++) {
        free(compiled[i].data);
    }
    n_compiled = 0;
}

bool apps_task_compile(const apps_pkg_t *pkg, char *err, size_t err_len) {
    compiles++;
    forget_compiled();
    for (int i = 0; i < pkg->count; i++) {
        snprintf(compiled[i].path, sizeof compiled[i].path, "%s", pkg->files[i].path);
        compiled[i].data = malloc(pkg->files[i].len + 1);
        CHECK(compiled[i].data != NULL, "no memory");
        memcpy(compiled[i].data, pkg->files[i].data, pkg->files[i].len);
        compiled[i].len = pkg->files[i].len;
        n_compiled++;
    }
    snprintf(err, err_len, "%s", compile_ok ? "" : compile_err);
    return compile_ok;
}

/* What run was given, and what it answers. */
static int runs, run_settle;
static bool run_front, run_ok = true;
static char run_id[APPS_ID_MAX + 1], run_err[300];
static const char *run_log = "hello from start";
static const char *run_error_record = "";

/* A launch by id's run (F10): counted, then run as apps_task_run (the app task's choice is tests/test_muse_apps_task.py's). */
static int runs_by_id;

bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len);

/* app.send's launch: counted apart, run behind with no settle (the bound it asks for is apps_task.c's). */
static int send_launches;

bool apps_task_run_for_send(apps_pkg_t *pkg, char *err, size_t err_len) {
    send_launches++;
    return apps_task_run(pkg, false, 0, NULL, err, err_len);
}

bool apps_task_run_by_id(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len) {
    runs_by_id++;
    return apps_task_run(pkg, front, settle_ms, st, err, err_len);
}

bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len) {
    runs++;
    run_front = front;
    run_settle = settle_ms;
    snprintf(run_id, sizeof run_id, "%s", pkg->manifest.id);
    if (st != NULL) {
        memset(st, 0, sizeof *st);
        st->state = run_ok ? APPS_STATE_RUNNING : APPS_STATE_CRASHED;
        snprintf(st->id, sizeof st->id, "%s", pkg->manifest.id);
        snprintf(st->name, sizeof st->name, "%s", pkg->manifest.name);
        snprintf(st->version, sizeof st->version, "%s", pkg->manifest.version);
        snprintf(st->last_error, sizeof st->last_error, "%s", run_error_record);
        snprintf(st->warnings, sizeof st->warnings, "%s", pkg->manifest.warnings);
        snprintf(st->log_tail, sizeof st->log_tail, "%s", run_log);
        snprintf(st->ui_dump, sizeof st->ui_dump, "[{\"id\":\"label#0\",\"kind\":\"label\",\"text\":\"Hi\",\"x\":200,"
                                                  "\"y\":220,\"w\":66,\"h\":26,\"hidden\":false,\"overflow\":0}]");
        st->mem_kb = 18;
        st->peak_kb = 21;
        st->limit_kb = pkg->manifest.memory_kb;
        st->widgets = 1;
        st->front = front;
    }
    apps_pkg_free(pkg, apps_port_free);
    apps_port_free(pkg);
    snprintf(err, err_len, "%s", run_ok ? "" : run_err);
    return run_ok;
}

static bool stop_ok = true;
static const char *stop_err = "";
static int stops;

bool apps_task_stop(char *err, size_t err_len) {
    stops++;
    snprintf(err, err_len, "%s", stop_ok ? "" : stop_err);
    return stop_ok;
}

/* A launch for an app's alarms (muse_apps_launch_alarm): the job asks whether any firing it was for still waits (alarm_wanted, which a case
 * sets), runs the app through apps_task_run_for_alarm (counted, with the identity it was given; it answers as apps_task_run does, or says
 * why not as alarm_run_err does) and tells the app task what came of it (the results, in order: "started", "failed", "missing"). */
static bool alarm_wanted = true;
static int wanted_calls;
static uint32_t wanted_upto;
static char wanted_id[APPS_ID_MAX + 1];

bool apps_alarm_wanted(const char *app, uint32_t upto) {
    wanted_calls++;
    wanted_upto = upto;
    snprintf(wanted_id, sizeof wanted_id, "%s", app);
    return alarm_wanted;
}

static int alarm_runs;
static uint32_t alarm_run_upto;
static const char *alarm_run_err;  /* NULL: as apps_task_run says */

bool apps_task_run_for_alarm(apps_pkg_t *pkg, uint32_t upto, apps_status_t *st, char *err, size_t err_len) {
    alarm_runs++;
    alarm_run_upto = upto;
    if (alarm_run_err != NULL) {
        snprintf(run_id, sizeof run_id, "%s", pkg->manifest.id);
        apps_pkg_free(pkg, apps_port_free);
        apps_port_free(pkg);
        snprintf(err, err_len, "%s", alarm_run_err);
        return false;
    }
    return apps_task_run(pkg, true, 0, st, err, err_len);
}

static int results;
static uint32_t result_upto[8];
static const char *result_name[8];

void apps_task_alarm_launch_result(uint32_t upto, apps_alarm_launch_t outcome) {
    if (results < 8) {
        result_upto[results] = upto;
        result_name[results] = outcome == APPS_ALARM_LAUNCH_STARTED ? "started"
                               : outcome == APPS_ALARM_LAUNCH_FAILED ? "failed"
                               : outcome == APPS_ALARM_LAUNCH_MISSING ? "missing" : "?";
    }
    results++;
}

static apps_status_t status_record; /* what apps_task_status copies */

/* The store's calls into the app task: the list it gives the launcher, and the app that runs (none here). */
static char listed[1024];

void apps_task_set_list(const apps_listed_t *apps, int n) {
    size_t at = 0;
    listed[0] = '\0';
    for (int i = 0; i < n; i++) {
        at += (size_t)snprintf(listed + at, sizeof listed - at, "%s%s:%s", i > 0 ? " " : "", apps[i].id, apps[i].name);
    }
}

/* The app that runs, as a case sets it ("" for none). */
static char running_app[APPS_ID_MAX + 1];

bool apps_task_running(char *id, size_t cap) {
    snprintf(id, cap, "%s", running_app);
    return running_app[0] != '\0';
}

void apps_task_status(apps_status_t *st) {
    memcpy(st, &status_record, sizeof *st);
}

static char message_id[64], message_event[256], message_data[512];
static bool message_data_null, message_show;
static int message_timeout, messages;
static const char *message_reply = "{\"left\":3}";
static const char *message_err = "";

char *apps_task_message(const char *id, const char *event, const char *data_json, bool show, int timeout_ms, char *err,
                        size_t err_len) {
    messages++;
    snprintf(message_id, sizeof message_id, "%s", id);
    snprintf(message_event, sizeof message_event, "%s", event);
    message_data_null = data_json == NULL;
    snprintf(message_data, sizeof message_data, "%s", data_json != NULL ? data_json : "");
    message_show = show;
    message_timeout = timeout_ms;
    if (message_reply == NULL) {
        snprintf(err, err_len, "%s", message_err);
        return NULL;
    }
    size_t n = strlen(message_reply) + 1;
    char *reply = apps_port_realloc(NULL, n);
    CHECK(reply != NULL, "no memory for the reply");
    memcpy(reply, message_reply, n);
    snprintf(err, err_len, "%s", "");
    return reply;
}

/* ---- Lines ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* data[0, len) as base64, NUL-terminated (the caller frees it). */
static char *base64(const uint8_t *data, size_t len) {
    char *out = malloc((len + 2) / 3 * 4 + 1);
    CHECK(out != NULL, "no memory");
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)data[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= data[i + 2];
        }
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

/* One console line, as muse_input hands it to the hook (without the '>'). */
static bool line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool line(const char *fmt, ...) {
    char text[1100];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    CHECK(n > 0 && n < 1024, "a console line is at most 1023 bytes: %d", n);
    return muse_apps_console(text);
}

/* app.put of one chunk. */
static bool put(const char *id, const char *path, unsigned seq, const void *data, size_t len) {
    char *b64 = base64(data, len);
    bool taken = line("app.put %s %s %u %s", id, path, seq, b64);
    free(b64);
    return taken;
}

/* A whole file, in chunks of 576 bytes (768 base64 characters): true when every chunk was acknowledged. */
static bool put_file(const char *id, const char *path, const void *data, size_t len) {
    const uint8_t *p = data;
    unsigned seq = 0;
    size_t at = 0;
    do {
        size_t n = len - at < 576 ? len - at : 576;
        CHECK(put(id, path, seq, p + at, n), "put not taken");
        char *got = take_output();
        char want[128];
        snprintf(want, sizeof want, "@app.ack %s %u\n", path, seq);
        bool acked = got != NULL && strcmp(got, want) == 0;
        if (!acked) {
            fprintf(stderr, "put_file %s seq %u: %s", path, seq, got != NULL ? got : "(null)");
        }
        free(got);
        if (!acked) {
            return false;
        }
        at += n;
        seq++;
    } while (at < len);
    return true;
}

static void put_text(const char *id, const char *path, const char *text) {
    CHECK(put_file(id, path, text, strlen(text)), "%s/%s wasn't staged", id, path);
}

/* A valid LVGL v9 image, RGB565, w x h. */
static uint8_t *make_image(unsigned w, unsigned h, size_t *len) {
    unsigned stride = w * 2;
    *len = 12 + (size_t)stride * h;
    uint8_t *img = calloc(1, *len);
    CHECK(img != NULL, "no memory");
    img[0] = 0x19;
    img[1] = 0x12;
    img[4] = (uint8_t)w;
    img[5] = (uint8_t)(w >> 8);
    img[6] = (uint8_t)h;
    img[7] = (uint8_t)(h >> 8);
    img[8] = (uint8_t)stride;
    img[9] = (uint8_t)(stride >> 8);
    for (size_t i = 12; i < *len; i++) {
        img[i] = (uint8_t)(i * 7);
    }
    return img;
}

static const char MANIFEST_HELLO[] = "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\",\"api\":1}";
static const char MAIN_HELLO[] = "local app = {}\nfunction app.start() ui.label{text = 'Hi'} end\nreturn app\n";

/* hello, staged: manifest.json and main.lua. */
static void stage_hello(void) {
    put_text("hello", "manifest.json", MANIFEST_HELLO);
    put_text("hello", "main.lua", MAIN_HELLO);
}

/* hello, installed in the RAM store (no run). */
static void install_hello(void) {
    stage_hello();
    CHECK(line("app.commit hello"), "commit not taken");
    EXPECT("");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", "\"storage\":\"ram\"");
}

/* ---- The end of every case ---- */

static cJSON *done_result;  /* the done callback's (below) */
static void forget_writes(void);

/* Nothing held once whatever is staged has gone through a commit and the RAM store is empty. */
static void finish(void) {
    submit_refuses = false;
    port_refuse = -1;
    worker_busy = false;
    internal_refuses = false;
    store_load_refusal = NULL;
    store_mode = APPS_STORAGE_RAM;
    running_app[0] = '\0';
    run_jobs();
    cJSON_Delete(done_result);
    done_result = NULL;
    forget_writes();
    CHECK(internal_blocks == 0, "%d blocks of internal RAM still held", internal_blocks);
    /* A stage for an id of its own (a new id drops the old stage), then a commit, whose job frees it. */
    CHECK(put("zz_end", "manifest.json", 0, "{}", 2), "the last put");
    CHECK(line("app.commit zz_end"), "the last commit");
    run_jobs();
    (void)take_output();
    apps_ram_entry_t e[APPS_RAM_APPS];
    int n = apps_ram_list(e, APPS_RAM_APPS);
    for (int i = 0; i < n; i++) {
        CHECK(apps_ram_remove(e[i].id), "remove %s", e[i].id);
    }
    forget_compiled();
    CHECK(apps_install_try_begin(), "the install's hold was let go");
    apps_install_end();
    CHECK(port_held == 0, "%zu bytes of the port's heap still held", port_held);
    CHECK(cjson_held == 0, "%zu bytes of cJSON's still held", cjson_held);
}

/* ---- The cases ---- */

static void put_sequence_and_ack(void) {
    CHECK(put("hello", "main.lua", 0, "local a = 1\n", 12), "put not taken");
    EXPECT("@app.ack main.lua 0\n");
    CHECK(put("hello", "main.lua", 1, "local b = 2\n", 12), "put not taken");
    EXPECT("@app.ack main.lua 1\n");
    CHECK(put("hello", "main.lua", 2, "return {}\n", 10), "put not taken");
    EXPECT("@app.ack main.lua 2\n");
    /* Another file in between, then seq 0 starts main.lua again. */
    put_text("hello", "manifest.json", MANIFEST_HELLO);
    CHECK(put("hello", "main.lua", 0, "return {", 8), "put not taken");
    EXPECT("@app.ack main.lua 0\n");
    CHECK(put("hello", "main.lua", 1, "}\n", 2), "put not taken");
    EXPECT("@app.ack main.lua 1\n");
    /* An empty chunk is a chunk. */
    CHECK(line("app.put hello main.lua 2 "), "put not taken");
    EXPECT("@app.ack main.lua 2\n");
    CHECK(line("app.commit hello"), "commit not taken");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", "\"bytes\":10,");
    CHECK(n_compiled == 1 && strcmp(compiled[0].path, "main.lua") == 0 && compiled[0].len == 10 &&
              memcmp(compiled[0].data, "return {}\n", 10) == 0,
          "compile saw main.lua as %.*s", n_compiled > 0 ? (int)compiled[0].len : 0,
          n_compiled > 0 ? (const char *)compiled[0].data : "");
}

static void put_out_of_order(void) {
    CHECK(put("hello", "main.lua", 0, "aaa", 3), "put");
    EXPECT("@app.ack main.lua 0\n");
    CHECK(put("hello", "main.lua", 1, "bbb", 3), "put");
    EXPECT("@app.ack main.lua 1\n");
    CHECK(put("hello", "main.lua", 3, "ddd", 3), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"expected seq 2\"}\n");
    CHECK(put("hello", "main.lua", 1, "bbb", 3), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"expected seq 2\"}\n");
    /* The file is as it was: seq 2 goes on from it. */
    CHECK(put("hello", "main.lua", 2, "ccc", 3), "put");
    EXPECT("@app.ack main.lua 2\n");
    /* A file not begun takes seq 0 first. */
    CHECK(put("hello", "util.lua", 1, "x", 1), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"util.lua\",\"message\":\"expected seq 0\"}\n");
    CHECK(line("app.put hello main.lua x QUJD"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"seq must be a whole number\"}\n");
    CHECK(line("app.put hello main.lua -1 QUJD"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"seq must be a whole number\"}\n");
    put_text("hello", "manifest.json", MANIFEST_HELLO);
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", "\"bytes\":9,");
    CHECK(n_compiled == 1 && compiled[0].len == 9 && memcmp(compiled[0].data, "aaabbbccc", 9) == 0, "main.lua as staged");
}

static void put_bad_base64(void) {
    CHECK(put("hello", "main.lua", 0, "abc", 3), "put");
    EXPECT("@app.ack main.lua 0\n");
    CHECK(line("app.put hello main.lua 1 QUJ*"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"bad base64 at character 4\"}\n");
    CHECK(line("app.put hello main.lua 1 QU=D"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"bad base64 at character 3\"}\n");
    CHECK(line("app.put hello main.lua 1 Q==="), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"bad base64 at character 2\"}\n");
    CHECK(line("app.put hello main.lua 1 QUJ"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"bad base64: 3 characters isn't a multiple of "
           "4\"}\n");
    CHECK(line("app.put hello main.lua 1 QUJD RA=="), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"bad base64 at character 5\"}\n");
    /* None of them changed the file: seq 1 still comes next. */
    CHECK(line("app.put hello main.lua 1 ZGVm"), "put");
    EXPECT("@app.ack main.lua 1\n");
    put_text("hello", "manifest.json", MANIFEST_HELLO);
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", NULL);
    CHECK(n_compiled == 1 && compiled[0].len == 6 && memcmp(compiled[0].data, "abcdef", 6) == 0, "main.lua as staged");
}

static void put_over_kind_limit(void) {
    size_t len = 257 * KB;
    uint8_t *big = malloc(len);
    CHECK(big != NULL, "no memory");
    memset(big, '-', len);
    unsigned seq = 0;
    size_t at = 0;
    bool refused = false;
    for (; at < len && !refused; at += 576, seq++) {
        size_t n = len - at < 576 ? len - at : 576;
        CHECK(put("hello", "main.lua", seq, big + at, n), "put");
        char *got = take_output();
        char want[64];
        snprintf(want, sizeof want, "@app.ack main.lua %u\n", seq);
        if (strcmp(got, want) != 0) {
            CHECK(strcmp(got, "@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"main.lua is larger than 256 "
                              "KB\"}\n") == 0,
                  "seq %u: %s", seq, got);
            refused = true;
        }
        free(got);
    }
    /* Refused as the bytes arrived: on the chunk that went past 256 KB, not at the commit. */
    CHECK(refused && at == 455 * 576 + 576, "refused at %zu bytes", at);
    /* The file is gone: the next chunk has nothing to follow. */
    CHECK(put("hello", "main.lua", seq, big, 576), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"expected seq 0\"}\n");
    free(big);
    /* A manifest.json of 8 KB and a .json of 32 KB are within their limits. */
    char *text = malloc(33 * KB);
    CHECK(text != NULL, "no memory");
    memset(text, ' ', 33 * KB);
    CHECK(put_file("hello", "manifest.json", text, 8 * KB), "an 8 KB manifest");
    CHECK(put_file("hello", "data.json", text, 32 * KB), "a 32 KB .json");
    free(text);
    CHECK(put_file("hello", "main.lua", "return {}", 9), "main.lua");
}

/* A put_file that failed printed its error to stderr; this one checks the message too. */
static void put_refused_at(int at_line, const char *id, const char *path, size_t len, const char *message) {
    uint8_t *data = malloc(len);
    CHECK(data != NULL, "no memory");
    memset(data, 'x', len);
    unsigned seq = 0;
    for (size_t at = 0; at < len; at += 576, seq++) {
        size_t n = len - at < 576 ? len - at : 576;
        CHECK(put(id, path, seq, data + at, n), "put");
        char *got = take_output();
        if (strncmp(got, "@app.ack ", 9) != 0) {
            char want[256];
            snprintf(want, sizeof want, "@app.err {\"op\":\"put\",\"path\":\"%s\",\"message\":\"%s\"}\n", path, message);
            if (strcmp(got, want) != 0) {
                fail_at(at_line, "%s seq %u: %s instead of %s", path, seq, got, want);
            }
            free(got);
            free(data);
            return;
        }
        free(got);
    }
    fail_at(at_line, "%s: every chunk of %zu bytes was taken", path, len);
}
#define PUT_REFUSED(id, path, len, message) put_refused_at(__LINE__, (id), (path), (len), (message))

static void limits_as_bytes_arrive(void) {
    /* Paths and ids are checked before anything is staged. */
    CHECK(line("app.put hello ../x.lua 0 QUJD"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"../x.lua\",\"message\":\"'../x.lua' is not an allowed path (a-z 0-9 _ "
           ". / -, no '..', at most 64 characters)\"}\n");
    CHECK(line("app.put hello notes.txt 0 QUJD"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"notes.txt\",\"message\":\"'notes.txt' has an unsupported type (use "
           ".lua, .json or .bin)\"}\n");
    CHECK(line("app.put Hello main.lua 0 QUJD"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"app id 'Hello' must match "
           "^[a-z][a-z0-9_]{1,23}$\"}\n");
    CHECK(line("app.put hello"), "put");
    EXPECT("@app.err {\"op\":\"put\",\"message\":\"usage: app.put <id> <path> <seq> <base64>\"}\n");
    /* manifest.json's own limit, 8 KB, and a .json's, 32 KB. */
    PUT_REFUSED("hello", "manifest.json", 8 * KB + 1, "manifest.json is larger than 8 KB");
    PUT_REFUSED("hello", "data.json", 32 * KB + 1, "data.json is larger than 32 KB");
    /* Images: 1.5 MB in all. */
    PUT_REFUSED("pics", "a.bin", 513 * KB, "a.bin is larger than 512 KB");
    CHECK(put_file("pics", "a.bin", ZEROS, 512 * KB), "a.bin");
    CHECK(put_file("pics", "b.bin", ZEROS, 512 * KB), "b.bin");
    CHECK(put_file("pics", "c.bin", ZEROS, 512 * KB), "c.bin");
    PUT_REFUSED("pics", "d.bin", 1, "images add up to more than 1.5 MB");
    /* The app: 2 MB in all, manifest.json with the rest. */
    CHECK(put_file("pics", "manifest.json", "{}", 2), "manifest.json");
    CHECK(put_file("pics", "d.lua", ZEROS, 256 * KB), "d.lua");
    PUT_REFUSED("pics", "e.lua", 256 * KB, "the app is larger than 2 MB");
    /* A file sent again from seq 0 counts once. */
    CHECK(put_file("pics", "d.lua", ZEROS, 256 * KB), "d.lua again");
    /* 32 files and manifest.json, no more. */
    for (int i = 0; i < 32; i++) {
        char path[16];
        snprintf(path, sizeof path, "f%02d.lua", i);
        CHECK(put_file("many", path, "x", 1), "%s", path);
    }
    CHECK(put("many", "f32.lua", 0, "x", 1), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"f32.lua\",\"message\":\"more than 32 files besides "
           "manifest.json\"}\n");
    CHECK(put_file("many", "manifest.json", "{}", 2), "manifest.json keeps its place");
}

static const char MANIFEST_FULL[] =
    "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"2.1.0\",\"api\":1,"
    "\"files\":[\"lib/util.lua\",\"data.json\",\"logo.bin\"]}";

static void commit_submits_staged_files(void) {
    size_t img_len;
    uint8_t *img = make_image(40, 30, &img_len);
    static const char util[] = "return {twice = function(x) return 2 * x end}\n";
    static const char data[] = "{\"steps\":[1,2,3]}";
    put_text("hello", "manifest.json", MANIFEST_FULL);
    put_text("hello", "main.lua", MAIN_HELLO);
    put_text("hello", "lib/util.lua", util);
    put_text("hello", "data.json", data);
    CHECK(put_file("hello", "logo.bin", img, img_len), "logo.bin");
    put_text("hello", "extra.lua", "-- not in the manifest\n");
    /* The commit only submits its job: no reply until the job has run. */
    CHECK(line("app.commit hello run"), "commit not taken");
    EXPECT("");
    CHECK(n_jobs == 1, "one job: %d", n_jobs);
    run_jobs();
    /* The pipeline read the staged files as they were put (and only the listed ones), compiled, kept and ran them. */
    CHECK(n_compiled == 4, "compile saw %d files", n_compiled);
    const char *want_paths[] = {"main.lua", "lib/util.lua", "data.json", "logo.bin"};
    const void *want_data[] = {MAIN_HELLO, util, data, img};
    size_t want_len[] = {strlen(MAIN_HELLO), strlen(util), strlen(data), img_len};
    for (int i = 0; i < 4; i++) {
        CHECK(strcmp(compiled[i].path, want_paths[i]) == 0 && compiled[i].len == want_len[i] &&
                  memcmp(compiled[i].data, want_data[i], want_len[i]) == 0,
              "file %d: %s, %zu bytes", i, compiled[i].path, compiled[i].len);
    }
    CHECK(runs == 1 && run_front && run_settle == APPS_SETTLE_MS && strcmp(run_id, "hello") == 0,
          "run: %d, front %d, settle %d, %s", runs, run_front, run_settle, run_id);
    char want_bytes[32];
    snprintf(want_bytes, sizeof want_bytes, "\"bytes\":%zu,", img_len);
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,\"payload\":{\"id\":\"hello\",\"version\":\"2.1.0\",", "\"storage\":\"ram\"",
                "\"path\":\"logo.bin\"", want_bytes, "\"state\":\"running\"", "\"log\":[\"hello from start\"]",
                "\"ui\":[{\"id\":\"label#0\"", "\"mem_kb\":18", "\"peak_kb\":21");
    /* The stage went into the job and the job freed it: what is held is the RAM store's copy, and nothing once it goes. */
    CHECK(apps_ram_has("hello") && port_held > 0, "kept in RAM");
    CHECK(apps_ram_remove("hello") && port_held == 0, "the stage was freed: %zu bytes held besides the store's", port_held);
    /* Nothing is staged any more; a put after the commit starts a new stage. */
    CHECK(line("app.commit hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"nothing is staged for hello: send its files with "
           "app.put first\"}\n");
    stage_hello();
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", "\"state\":\"installed\"", "\"log\":[]", "\"ui\":[]");
    CHECK(runs == 1 && n_compiled == 1, "no run without 'run'; compile saw %d files", n_compiled);
    free(img);
}

static void commit_refusals(void) {
    CHECK(line("app.commit"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"message\":\"usage: app.commit <id> [run]\"}\n");
    CHECK(line("app.commit hello now"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"message\":\"usage: app.commit <id> [run]\"}\n");
    CHECK(line("app.commit Hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"Hello\",\"message\":\"app id 'Hello' must match "
           "^[a-z][a-z0-9_]{1,23}$\"}\n");
    CHECK(line("app.commit three"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"three\",\"message\":\"nothing is staged for three: send its files "
           "with app.put first\"}\n");
    /* Short of memory: refused, and the stage stays for another try. */
    stage_hello();
    submit_refuses = true;
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"the gadget is short of memory right now; try "
           "again in a few seconds\"}\n");
    submit_refuses = false;
    /* One install at a time: a commit while another's job waits is busy, and keeps its stage. */
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("");
    put_text("other", "manifest.json", "{\"id\":\"other\",\"name\":\"Other\",\"api\":1}");
    put_text("other", "main.lua", "return {}\n");
    CHECK(line("app.commit other"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"other\",\"message\":\"busy: another install is running\"}\n");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,\"payload\":{\"id\":\"hello\"", NULL);
    CHECK(line("app.commit other"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,\"payload\":{\"id\":\"other\"", NULL);
    /* The pipeline's own failure is the result's. */
    put_text("bad", "manifest.json", "{\"id\":\"bad\",\"name\":\"Bad\",\"api\":2}");
    CHECK(line("app.commit bad"), "commit");
    run_jobs();
    EXPECT("@app {\"op\":\"commit\",\"ok\":false,\"error\":{\"code\":\"invalid_package\",\"message\":\"manifest: this "
           "gadget supports api 1 (manifest says api 2)\"}}\n");
}

static void a_new_id_discards_the_stage(void) {
    size_t before = port_held;
    stage_hello();
    size_t staged = port_held;
    CHECK(staged > before, "staged");
    put_text("other", "main.lua", "return {}\n");
    CHECK(port_held < staged, "hello's stage was freed: %zu, %zu", staged, port_held);
    CHECK(line("app.commit hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"nothing is staged for hello: send its files with "
           "app.put first\"}\n");
}

static void launch_runs_an_installed_app(void) {
    CHECK(line("app.launch"), "launch");
    EXPECT("@app.err {\"op\":\"launch\",\"message\":\"usage: app.launch <id>\"}\n");
    CHECK(line("app.launch nosuch"), "launch");
    EXPECT("");
    run_jobs();
    EXPECT("@app.err {\"op\":\"launch\",\"id\":\"nosuch\",\"message\":\"no such app on the gadget\"}\n");
    install_hello();
    CHECK(runs == 0, "installed, not run");
    CHECK(line("app.launch hello"), "launch");
    EXPECT("");
    run_jobs();
    CHECK(runs == 1 && run_front && run_settle == APPS_SETTLE_MS && strcmp(run_id, "hello") == 0, "run: %d %d %d %s",
          runs, run_front, run_settle, run_id);
    EXPECT_LINE("@app {\"op\":\"launch\",\"ok\":true,\"payload\":{\"id\":\"hello\",\"version\":\"1.0.0\",",
                "\"storage\":\"ram\"", "\"path\":\"main.lua\"", "\"state\":\"running\"", "\"log\":[\"hello from start\"]");
    /* A crash, without "installed but ". */
    run_ok = false;
    snprintf(run_err, sizeof run_err, "crashed in start(): main.lua:2: boom");
    run_error_record = "main.lua:2: boom\nstack traceback:\n\tmain.lua:2: in function 'start'";
    run_log = "one\ntwo";
    CHECK(line("app.launch hello"), "launch");
    run_jobs();
    EXPECT("@app {\"op\":\"launch\",\"ok\":false,\"error\":{\"code\":\"crashed\",\"message\":\"crashed in start(): "
           "main.lua:2: boom\\nstack traceback:\\n\\tmain.lua:2: in function 'start'\\nlog:\\none\\ntwo\"}}\n");
    /* The store still has it: a run takes a copy. */
    CHECK(apps_ram_has("hello"), "still installed");
    CHECK(runs_by_id == 0, "app.launch runs the store's copy afresh, whatever runs");
    /* Short of memory. */
    submit_refuses = true;
    CHECK(line("app.launch hello"), "launch");
    EXPECT("@app.err {\"op\":\"launch\",\"id\":\"hello\",\"message\":\"the gadget is short of memory right now; try "
           "again in a few seconds\"}\n");
    submit_refuses = false;
}

static void stop_reports_the_task(void) {
    CHECK(line("app.stop"), "stop");
    EXPECT("@app {\"op\":\"stop\",\"ok\":true}\n");
    stop_ok = false;
    stop_err = "the app task didn't answer within 2000 ms";
    CHECK(line("app.stop"), "stop");
    EXPECT("@app.err {\"op\":\"stop\",\"message\":\"the app task didn't answer within 2000 ms\"}\n");
    CHECK(line("app.stop now"), "stop");
    EXPECT("@app.err {\"op\":\"stop\",\"message\":\"usage: app.stop\"}\n");
    CHECK(stops == 2, "stops: %d", stops);
}

/* P2: muse_apps_launch, from any task: app.launch's job with its place and nothing printed. The job runs the store's copy
 * through apps_task_run, in front or behind, with no settle; an id the store doesn't have is one log line. A refused
 * submit adds no line of its own (apps_io_submit logs why, once). */
static void launch_by_id_prints_nothing(void) {
    install_hello();
    int before = runs;
    CHECK(muse_apps_launch("hello", false), "queued");
    EXPECT("");
    run_jobs();
    EXPECT("");
    CHECK(runs == before + 1 && !run_front && run_settle == 0 && strcmp(run_id, "hello") == 0,
          "run behind with no settle: runs %d front %d settle %d id %s", runs - before, run_front, run_settle, run_id);
    CHECK(runs_by_id == 1, "a launch by id runs by id (F10: the app task fronts the app if it runs by then): %d",
          runs_by_id);
    CHECK(muse_apps_launch("hello", true), "queued");
    run_jobs();
    EXPECT("");
    CHECK(runs == before + 2 && run_front, "run in front: runs %d front %d", runs - before, run_front);
    int logged = logs;
    CHECK(muse_apps_launch("nosuch", true), "queued");
    run_jobs();
    EXPECT("");
    CHECK(runs == before + 2, "nothing ran");
    CHECK(logs == logged + 1 && strcmp(last_log, "W muse_apps: launch nosuch: no such app on the gadget") == 0,
          "one log line: '%s'", last_log);
    CHECK(results == 0 && wanted_calls == 0, "a launch that isn't for alarms tells the app task nothing: %d results, %d questions", results,
          wanted_calls);
    run_ok = false;
    snprintf(run_err, sizeof run_err, "crashed in start(): main.lua:2: boom");
    CHECK(muse_apps_launch("hello", true), "queued");
    run_jobs();
    EXPECT("");
    CHECK(strcmp(last_log, "W muse_apps: launch hello: crashed in start(): main.lua:2: boom") == 0, "'%s'", last_log);
    run_ok = true;
    logged = logs;
    submit_refuses = true;
    CHECK(!muse_apps_launch("hello", true), "refused");
    submit_refuses = false;
    CHECK(!muse_apps_launch("Not An Id", true), "not an id");
    CHECK(runs == before + 3 && logs == logged + 1, "a refused submit logs nothing here; a bad id logs once (%d)",
          logs - logged);
    CHECK(runs_by_id == 3, "every launch by id ran by id: %d", runs_by_id);
    EXPECT("");
}

/* A launch for an app's alarms (muse_apps_launch_alarm: the app task's, with the identity apps_alarm_wanted answers for) is app.launch's job in
 * front with nothing printed, and it tells the app task what came of it, whichever way it went: started (the run came back true), failed (the
 * run did not, with the line "launch <id>: <why>"; a store that has the app but can't load it now), missing (the store has no such app, with
 * the line the others have). Before it loads anything it asks whether any firing it was for still waits: when none does nothing is loaded,
 * run, said or told. A run that says its alarms are gone (they went while the app was loaded) is quiet too, and tells nothing. */
static void launch_for_alarms_tells_the_app_task_what_came_of_it(void) {
    install_hello();
    int before = alarm_runs;
    CHECK(muse_apps_launch_alarm("hello", 7), "queued");
    EXPECT("");
    run_jobs();
    EXPECT("");
    CHECK(wanted_calls == 1 && wanted_upto == 7 && strcmp(wanted_id, "hello") == 0, "asked first: %d calls, %u, %s", wanted_calls, wanted_upto,
          wanted_id);
    CHECK(alarm_runs == before + 1 && alarm_run_upto == 7 && run_front && run_settle == 0 && strcmp(run_id, "hello") == 0,
          "run in front with its identity: runs %d upto %u front %d", alarm_runs - before, alarm_run_upto, run_front);
    CHECK(runs_by_id == 0, "it isn't a launch by id of the launcher's kind: %d", runs_by_id);
    CHECK(results == 1 && result_upto[0] == 7 && strcmp(result_name[0], "started") == 0, "started: %d results, %s", results, result_name[0]);
    int logged = logs;
    /* The run did not come up: failed, with the line, and the app task is told. */
    run_ok = false;
    snprintf(run_err, sizeof run_err, "crashed in start(): main.lua:2: boom");
    CHECK(muse_apps_launch_alarm("hello", 9), "queued");
    run_jobs();
    CHECK(results == 2 && result_upto[1] == 9 && strcmp(result_name[1], "failed") == 0, "failed: %d results", results);
    CHECK(logs == logged + 1 && strcmp(last_log, "W muse_apps: launch hello: crashed in start(): main.lua:2: boom") == 0, "'%s'", last_log);
    run_ok = true;
    /* The store has no such app: missing, with the line. */
    logged = logs;
    CHECK(muse_apps_launch_alarm("nosuch", 11), "queued");
    run_jobs();
    CHECK(results == 3 && result_upto[2] == 11 && strcmp(result_name[2], "missing") == 0, "missing: %d results", results);
    CHECK(logs == logged + 1 && strcmp(last_log, "W muse_apps: launch nosuch: no such app on the gadget") == 0, "'%s'", last_log);
    /* No firing waits any more: nothing is loaded, run, said or told. */
    alarm_wanted = false;
    int runs_then = alarm_runs, results_then = results;
    logged = logs;
    CHECK(muse_apps_launch_alarm("hello", 13), "queued");
    CHECK(muse_apps_launch_alarm("nosuch", 15), "queued");
    run_jobs();
    CHECK(alarm_runs == runs_then && results == results_then && logs == logged, "skipped: %d runs, %d results, %d lines", alarm_runs - runs_then,
          results - results_then, logs - logged);
    CHECK(wanted_upto == 15 && strcmp(wanted_id, "nosuch") == 0, "each was asked about: %u, %s", wanted_upto, wanted_id);
    alarm_wanted = true;
    /* Its alarms went while the app was loaded: the run says so, and that is no failure to log or to tell. */
    alarm_run_err = APPS_TASK_ALARM_GONE;
    logged = logs;
    CHECK(muse_apps_launch_alarm("hello", 17), "queued");
    run_jobs();
    CHECK(alarm_run_upto == 17 && results == results_then && logs == logged, "quiet: %d results, %d lines", results - results_then, logs - logged);
    alarm_run_err = NULL;
    /* Not a launch: no identity, a bad id, and a queue that refuses (apps_io_submit says why, once). */
    CHECK(!muse_apps_launch_alarm("hello", 0), "no identity");
    logged = logs;
    CHECK(!muse_apps_launch_alarm("Not An Id", 19), "not an id");
    CHECK(logs == logged + 1, "a bad id logs once (%d)", logs - logged);
    submit_refuses = true;
    CHECK(!muse_apps_launch_alarm("hello", 21), "refused");
    submit_refuses = false;
    run_jobs();
    CHECK(results == results_then && alarm_runs == runs_then + 1, "nothing of those ran: %d results, %d runs", results - results_then,
          alarm_runs - runs_then);
    EXPECT("");
}

/* Each kind of reply goes out to the host as it ends (apps_port_console_flush after its newline): an acknowledgement,
 * a refusal, a result from the serial task, stop's own line and a result from the I/O worker. take_output checks that
 * nothing was printed after the last flush, after every line of every case. */
static void replies_end_with_a_push(void) {
    int before = flushes;
    CHECK(put("hello", "main.lua", 0, "return {}\n", 10), "put not taken");
    EXPECT("@app.ack main.lua 0\n");
    CHECK(line("app.put hello main.lua 5 AAAA"), "put not taken");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"expected seq 1\"}\n");
    CHECK(line("app.status"), "status not taken");
    EXPECT_LINE("@app {\"op\":\"status\",", "\"state\":");
    CHECK(line("app.stop"), "stop not taken");
    EXPECT("@app {\"op\":\"stop\",\"ok\":true}\n");
    CHECK(line("app.list"), "list not taken");
    EXPECT("");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"list\",", "\"storage\":\"ram\"");
    CHECK(flushes - before == 5, "%d flushes for 5 replies", flushes - before);
}

/* A record as full as it gets: 40 lines of 200 bytes (the whole 8 KB ring) and a dump of 128 widgets. */
static void fill_record(apps_status_t *st, int dropped) {
    memset(st, 0, sizeof *st);
    st->state = APPS_STATE_RUNNING;
    snprintf(st->id, sizeof st->id, "hello");
    snprintf(st->name, sizeof st->name, "Hello \"quoted\"");
    snprintf(st->version, sizeof st->version, "1.0.0");
    snprintf(st->warnings, sizeof st->warnings, "unknown key 'colour' ignored; label#3 'x' extends 4 px past the round edge");
    /* 40 lines of 200 bytes: "line NN " and 192 'z'. */
    size_t at = 0;
    for (int i = 0; i < 40; i++) {
        at += (size_t)snprintf(st->log_tail + at, sizeof st->log_tail - at, "%sline %02d ", i > 0 ? "\n" : "", i);
        memset(st->log_tail + at, 'z', 192);
        at += 192;
        st->log_tail[at] = '\0';
    }
    at = (size_t)snprintf(st->ui_dump, sizeof st->ui_dump, "[");
    for (int i = 0; i < 128 && at < sizeof st->ui_dump - 200; i++) {
        at += (size_t)snprintf(st->ui_dump + at, sizeof st->ui_dump - at,
                               "%s{\"id\":\"label#%d\",\"kind\":\"label\",\"text\":\"Item %d\",\"x\":%d,\"y\":%d,\"w\":null,"
                               "\"h\":null,\"hidden\":false,\"overflow\":0}",
                               i > 0 ? "," : "", i, i, i, i * 2);
    }
    snprintf(st->ui_dump + at, sizeof st->ui_dump - at, "]");
    st->ui_truncated = dropped;
    st->instructions = 123456789012ull;
    st->mem_kb = 40;
    st->peak_kb = 52;
    st->limit_kb = 256;
    st->worst_dispatch_ms = 31;
    st->dropped_events = 2;
    st->stack_free = 41234;
    st->cpu_pct = 7;
    st->widgets = 128;
    st->front = true;
}

static void status_prints_the_record(void) {
    memset(&status_record, 0, sizeof status_record);
    status_record.state = APPS_STATE_RUNNING;
    snprintf(status_record.id, sizeof status_record.id, "hello");
    snprintf(status_record.name, sizeof status_record.name, "Hello");
    snprintf(status_record.version, sizeof status_record.version, "1.0.0");
    snprintf(status_record.log_tail, sizeof status_record.log_tail, "start\nshow");
    snprintf(status_record.ui_dump, sizeof status_record.ui_dump, "[]");
    status_record.instructions = 4321;
    status_record.stack_free = 41000;
    status_record.mem_kb = 17;
    status_record.peak_kb = 19;
    status_record.limit_kb = 256;
    status_record.cpu_pct = 3;
    status_record.widgets = 1;
    CHECK(line("app.status"), "status");
    EXPECT("@app {\"op\":\"status\",\"state\":\"running\",\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\","
           "\"front\":false,\"last_error\":\"\",\"warnings\":[],\"log\":[\"start\",\"show\"],\"ui\":[],"
           "\"instructions\":4321,\"mem_kb\":17,\"peak_kb\":19,\"limit_kb\":256,\"worst_dispatch_ms\":0,\"cpu_pct\":3,"
           "\"dropped_events\":0,\"stack_free\":41000,\"widgets\":1}\n");
    /* A full log fits the console's 6 KB with a small dump: the oldest lines go. */
    fill_record(&status_record, 0);
    snprintf(status_record.ui_dump, sizeof status_record.ui_dump, "[]");
    CHECK(line("app.status"), "status");
    char *got = take_output();
    CHECK(got != NULL && strncmp(got, "@app {\"op\":\"status\",", 19) == 0, "%s", got);
    /* The status is at most APPS_RESULT_MAX bytes, and the console's "op" comes with it. */
    size_t len = strlen(got) - strlen("@app ") - 1 - strlen("\"op\":\"status\",");
    CHECK(len <= APPS_RESULT_MAX, "the status is %zu bytes", len);
    CHECK(strstr(got, "\"line 39 ") != NULL && strstr(got, "\"line 00 ") == NULL && strstr(got, "ui_truncated") == NULL,
          "the newest lines are kept: %s", got);
    free(got);
    /* The fullest record: every log line goes before the dump's last widgets do. */
    fill_record(&status_record, 5);
    CHECK(line("app.status"), "status");
    got = take_output();
    len = strlen(got) - strlen("@app ") - 1 - strlen("\"op\":\"status\",");
    CHECK(len <= APPS_RESULT_MAX, "the status is %zu bytes", len);
    CHECK(strstr(got, "\"log\":[],\"ui\":[{\"id\":\"label#0\"") != NULL && strstr(got, "\"ui_truncated\":") != NULL &&
              strstr(got, "\"instructions\":123456789012,") != NULL && strstr(got, "\"stack_free\":41234,") != NULL &&
              strstr(got, "\"name\":\"Hello \\\"quoted\\\"\"") != NULL,
          "%s", got);
    free(got);
    CHECK(line("app.status now"), "status");
    EXPECT("@app.err {\"op\":\"status\",\"message\":\"usage: app.status\"}\n");
}

static void list_shows_installed_apps(void) {
    CHECK(line("app.list"), "list");
    EXPECT("");
    run_jobs();
    EXPECT("@app {\"op\":\"list\",\"storage\":\"ram\",\"hint\":\"" APPS_RAM_HINT "\",\"apps\":[]}\n");
    install_hello();
    put_text("timer", "manifest.json", "{\"id\":\"timer\",\"name\":\"Timer\",\"version\":\"0.3\",\"api\":1}");
    put_text("timer", "main.lua", "return {}\n");
    CHECK(line("app.commit timer"), "commit");
    run_jobs();
    (void)take_output();
    CHECK(line("app.list"), "list");
    run_jobs();
    /* Most recent first, as the launcher lists them (F1). */
    EXPECT("@app {\"op\":\"list\",\"storage\":\"ram\",\"hint\":\"" APPS_RAM_HINT "\",\"apps\":[{\"id\":\"timer\",\"name\":"
           "\"Timer\",\"version\":\"0.3\"},{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\"}]}\n");
    CHECK(strcmp(listed, "timer:Timer hello:Hello") == 0, "the launcher's list: %s", listed);
    /* A store on the apps partition says so, with no hint. */
    store_mode = APPS_STORAGE_FLASH;
    CHECK(line("app.list"), "list");
    run_jobs();
    EXPECT("@app {\"op\":\"list\",\"storage\":\"flash\",\"apps\":[{\"id\":\"timer\",\"name\":\"Timer\",\"version\":"
           "\"0.3\"},{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\"}]}\n");
    store_mode = APPS_STORAGE_RAM;
    CHECK(line("app.list all"), "list");
    EXPECT("@app.err {\"op\":\"list\",\"message\":\"usage: app.list\"}\n");
}

/* P7: a partition that is there and couldn't be mounted keeps the apps in RAM for the boot, and app.list says the store's own hint for
 * that, which doesn't say to reflash (a reflash wouldn't help). */
static void list_says_the_stores_hint(void) {
    store_mode = APPS_STORAGE_RAM;
    store_hint = "the gadget couldn't open its app storage; restart it to try again";
    CHECK(line("app.list"), "list");
    run_jobs();
    EXPECT("@app {\"op\":\"list\",\"storage\":\"ram\",\"hint\":\"the gadget couldn't open its app storage; restart it to try again\","
           "\"apps\":[]}\n");
    store_hint = NULL;
}

/* device.health's "apps" (muse_apps_health): the record's few words and the store's mode and room. resident is null with no app loaded. */
static void health_says_the_app_and_the_store(void) {
    static const struct {
        const char *id;
        apps_state_t state;
        uint32_t mem_kb, peak_kb;
        apps_storage_t mode;
        uint32_t free_kb;
        const char *want;
    } cases[] = {
        {"timer", APPS_STATE_RUNNING, 180, 220, APPS_STORAGE_FLASH, 5824,
         "{\"resident\":\"timer\",\"state\":\"running\",\"lua_kb\":180,\"peak_kb\":220,\"storage\":\"flash\",\"storage_free_kb\":5824}"},
        {"spin", APPS_STATE_HUNG, 300, 310, APPS_STORAGE_FLASH, 5824,
         "{\"resident\":\"spin\",\"state\":\"hung\",\"lua_kb\":300,\"peak_kb\":310,\"storage\":\"flash\",\"storage_free_kb\":5824}"},
        {"", APPS_STATE_CRASHED, 0, 220, APPS_STORAGE_RAM, 0,
         "{\"resident\":null,\"state\":\"crashed\",\"lua_kb\":0,\"peak_kb\":220,\"storage\":\"ram\",\"storage_free_kb\":0}"},
        {"", APPS_STATE_STOPPED, 0, 0, APPS_STORAGE_NONE, 0,
         "{\"resident\":null,\"state\":\"stopped\",\"lua_kb\":0,\"peak_kb\":0,\"storage\":\"none\",\"storage_free_kb\":0}"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        snprintf(brief.id, sizeof brief.id, "%s", cases[i].id);
        brief.state = cases[i].state;
        brief.mem_kb = cases[i].mem_kb;
        brief.peak_kb = cases[i].peak_kb;
        store_mode = cases[i].mode;
        store_free_kb = cases[i].free_kb;
        cJSON *h = muse_apps_health();
        CHECK(h != NULL, "no health");
        char *text = cJSON_PrintUnformatted(h);
        CHECK(text != NULL && strcmp(text, cases[i].want) == 0, "health %s instead of %s", text != NULL ? text : "(null)", cases[i].want);
        cJSON_free(text);
        cJSON_Delete(h);
    }
    store_mode = APPS_STORAGE_RAM;
    /* Out of memory: NULL, and nothing held. */
    cjson_refuse = 2;
    CHECK(muse_apps_health() == NULL, "health without memory");
    cjson_refuse = -1;
}

/* A stage that has had no put for MUSE_APPS_STAGE_IDLE_MS is freed by the app task's rounds (muse_apps_console_tick), with one log line,
 * and a commit then finds nothing; a round before that, or one that finds the console at the stage, leaves it, and a put since starts
 * the time again. A round before any stage was made does nothing. */
static void an_idle_stage_is_freed(void) {
    muse_apps_console_tick(fake_ms + MUSE_APPS_STAGE_IDLE_MS);  /* no stage was ever made: no lock yet */
    size_t before = port_held;
    int logs_before = logs;
    stage_hello();
    size_t staged = port_held;
    CHECK(staged > before, "staged");
    fake_ms += MUSE_APPS_STAGE_IDLE_MS - 1;
    muse_apps_console_tick(fake_ms);
    CHECK(port_held == staged && logs == logs_before, "a minute short of idle: left");
    put_text("hello", "main.lua", MAIN_HELLO);  /* a put (main.lua again, from seq 0): the time starts again */
    staged = port_held;
    fake_ms += MUSE_APPS_STAGE_IDLE_MS - 1;
    muse_apps_console_tick(fake_ms);
    CHECK(port_held == staged && logs == logs_before, "idle counts from the last put: left");
    fake_ms += 1;
    console_holds_the_stage = true;
    muse_apps_console_tick(fake_ms);
    console_holds_the_stage = false;
    CHECK(port_held == staged && logs == logs_before, "the console is at it: left for the next round");
    muse_apps_console_tick(fake_ms);
    CHECK(port_held == before, "freed: %zu bytes held, %zu before", port_held, before);
    CHECK(logs == logs_before + 1 &&
              strcmp(last_log, "W muse_apps: console: the files put for hello had no commit for 10 minutes: freed") == 0,
          "one log line: %d, %s", logs - logs_before, last_log);
    CHECK(line("app.commit hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"nothing is staged for hello: send its files with "
           "app.put first\"}\n");
    muse_apps_console_tick(fake_ms + 2 * MUSE_APPS_STAGE_IDLE_MS);  /* nothing staged: nothing to say */
    CHECK(logs == logs_before + 1, "said once");
}

static void send_returns_the_reply(void) {
    CHECK(line("app.send hello tick {\"n\": 1}"), "send");
    EXPECT("@app {\"op\":\"send\",\"id\":\"hello\",\"event\":\"tick\",\"reply\":{\"left\":3}}\n");
    CHECK(strcmp(message_id, "hello") == 0 && strcmp(message_event, "tick") == 0 && !message_data_null &&
              strcmp(message_data, "{\"n\": 1}") == 0 && !message_show && message_timeout == APPS_TASK_WAIT_MS,
          "message: %s %s %s %d %d", message_id, message_event, message_data, message_show, message_timeout);
    CHECK(line("app.send hello reset"), "send");
    EXPECT("@app {\"op\":\"send\",\"id\":\"hello\",\"event\":\"reset\",\"reply\":{\"left\":3}}\n");
    CHECK(message_data_null, "no data is nil");
    message_reply = NULL;
    message_err = "main.lua:7: unknown event\nstack";
    CHECK(line("app.send hello bogus"), "send");
    EXPECT("@app.err {\"op\":\"send\",\"id\":\"hello\",\"message\":\"main.lua:7: unknown event\\nstack\"}\n");
    /* A reply the console won't print. */
    char *huge = malloc(7001);
    CHECK(huge != NULL, "no memory");
    memset(huge, ' ', 7000);
    huge[0] = '"';
    huge[6999] = '"';
    huge[7000] = '\0';
    message_reply = huge;
    CHECK(line("app.send hello dump"), "send");
    EXPECT("@app.err {\"op\":\"send\",\"id\":\"hello\",\"message\":\"the reply is 7000 bytes: the console shows at most "
           "6144\"}\n");
    free(huge);
    message_reply = "{}";
    CHECK(line("app.send hello"), "send");
    EXPECT("@app.err {\"op\":\"send\",\"message\":\"usage: app.send <id> <event> [json]\"}\n");
    CHECK(line("app.send Hello x"), "send");
    EXPECT("@app.err {\"op\":\"send\",\"id\":\"Hello\",\"message\":\"app id 'Hello' must match "
           "^[a-z][a-z0-9_]{1,23}$\"}\n");
}

static void foreign_and_unknown_lines(void) {
    CHECK(!muse_apps_console("status"), "not ours");
    CHECK(!muse_apps_console("sdk_token=x"), "not ours");
    CHECK(!muse_apps_console("app"), "not ours");
    CHECK(!muse_apps_console("apps.put x"), "not ours");
    EXPECT("");
    CHECK(line("app.bogus x"), "ours");
    EXPECT("@app.err {\"op\":\"bogus\",\"message\":\"unknown command app.bogus (app.put, app.commit, app.launch, "
           "app.stop, app.status, app.list, app.send)\"}\n");
    /* What someone typed is shown cleaned: valid UTF-8 kept, control bytes and broken bytes as '?', 64 bytes at most. */
    CHECK(line("app.\x01\xff\xc3\xa9t\xc3"), "ours");
    EXPECT("@app.err {\"op\":\"??\xc3\xa9t?\",\"message\":\"unknown command app.??\xc3\xa9t? (app.put, app.commit, "
           "app.launch, app.stop, app.status, app.list, app.send)\"}\n");
    /* A path too long to be one: shown cut at 64 bytes. */
    char path[80], shown_path[80], want[512];
    snprintf(path, sizeof path, "an/%.71s.lua", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    snprintf(shown_path, sizeof shown_path, "%.64s...", path);
    CHECK(line("app.put hello %s 0 QUJD", path), "ours");
    snprintf(want, sizeof want, "@app.err {\"op\":\"put\",\"path\":\"%s\",\"message\":\"'%s' is not an allowed path "
             "(a-z 0-9 _ . / -, no '..', at most 64 characters)\"}\n", shown_path, shown_path);
    EXPECT(want);
}

static void stage_out_of_memory(void) {
    size_t before = port_held;
    port_refuse = 0; /* the stage itself */
    CHECK(put("hello", "main.lua", 0, "abc", 3), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"out of memory: no room to stage the app\"}\n");
    CHECK(port_held == before, "nothing held");
    port_refuse = 1; /* the file's bytes, after the stage */
    CHECK(put("hello", "main.lua", 0, "abc", 3), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"out of memory: no room for main.lua\"}\n");
    port_refuse = -1;
    CHECK(put("hello", "main.lua", 1, "abc", 3), "put");
    EXPECT("@app.err {\"op\":\"put\",\"path\":\"main.lua\",\"message\":\"expected seq 0\"}\n");
    CHECK(put("hello", "main.lua", 0, "abc", 3), "put");
    EXPECT("@app.ack main.lua 0\n");
    /* The job's own: no room for the install. */
    put_text("hello", "manifest.json", MANIFEST_HELLO);
    port_refuse = 0;
    CHECK(line("app.commit hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"out of memory: no room for the install\"}\n");
    port_refuse = -1;
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", NULL);
}

/* F5: one install at a time comes first. The same commit sent again while its job waits is busy (not "nothing is
 * staged", though its stage went into the job), and a commit with nothing staged gives the hold back. */
static void a_repeated_commit_is_busy(void) {
    stage_hello();
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("");
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"busy: another install is running\"}\n");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,\"payload\":{\"id\":\"hello\"", NULL);
    CHECK(runs == 1, "one install ran: %d", runs);
    CHECK(line("app.commit hello"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"nothing is staged for hello: send its files with "
           "app.put first\"}\n");
    CHECK(apps_install_try_begin(), "nothing staged gave the hold back");
    apps_install_end();
}

/* ---- Link's app.install (Task 22): muse_apps_install, its job and its refusals ---- */

/* The VM's files: hello's two, as Muse would have written them to workspace/apps/hello/. */
static int vm_reads;

static bool fake_vm_read(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                         int64_t deadline_us, const char **code, char *err, size_t err_len) {
    (void)max;
    (void)deadline_us;
    vm_reads++;
    const char *text = strcmp(path, "workspace/apps/hello/manifest.json") == 0 ? MANIFEST_HELLO
                       : strcmp(path, "workspace/apps/hello/main.lua") == 0    ? MAIN_HELLO
                                                                               : NULL;
    if (text == NULL) {
        *code = MUSE_APPS_VM_FAILED;
        snprintf(err, err_len, "VM answered HTTP 404");
        return false;
    }
    return sink(user, (const uint8_t *)text, strlen(text));
}

/* The VM's writes (app.api's guide): each one's path, bytes and deadline kept; the write numbered write_fails_at (from 1)
 * fails with write_code and write_why, and every one after it succeeds. A write lowers the DMA RAM's lowest mark, as the
 * session's copies of its sends can. */
static int vm_writes, write_fails_at;
static const char *write_code = MUSE_APPS_VM_FAILED, *write_why = "VM answered HTTP 403: route not allowed for this credential's scopes";
static struct {
    char path[96];
    char *data;
    size_t len;
    int64_t deadline_us;
} written[8];

static bool fake_vm_write(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                          size_t err_len) {
    int i = vm_writes++;
    CHECK(i < (int)ARRAY_LEN(written), "too many writes");
    snprintf(written[i].path, sizeof written[i].path, "%s", path);
    written[i].data = malloc(len + 1);
    CHECK(written[i].data != NULL, "no memory");
    memcpy(written[i].data, data, len);
    written[i].data[len] = '\0';
    written[i].len = len;
    written[i].deadline_us = deadline_us;
    dma_lowest -= 512;
    if (i + 1 == write_fails_at) {
        *code = write_code;
        snprintf(err, err_len, "%s", write_why);
        return false;
    }
    return true;
}

static void forget_writes(void) {
    for (int i = 0; i < vm_writes; i++) {
        free(written[i].data);
    }
    vm_writes = 0;
}

static const muse_apps_vm_ops_t FAKE_VM = {fake_vm_read, fake_vm_write};

/* The done callback: how often it ran, with what, and whether the install's hold was let go before it. */
static int dones;
static void *done_ctx;
static cJSON *done_result;
static bool done_held;

static void on_done(void *ctx, cJSON *result) {
    dones++;
    done_ctx = ctx;
    cJSON_Delete(done_result);
    done_result = result;
    done_held = !apps_install_try_begin();
    if (!done_held) {
        apps_install_end();
    }
}

/* The result the callback was given: ok, and a string at payload.key or error.key. */
static const char *done_text(const char *object, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(done_result, object), key));
}

/* app.install's job: queued from the Noise session's task without waiting, run on the I/O worker as one job, which reads
 * workspace/apps/<id>/ through the VM's ops, keeps the app (the RAM store) and runs it in front with the settle, lets the hold go,
 * and calls done once with the result, which done owns. */
static void a_link_install_is_one_job(void) {
    muse_apps_set_vm_ops(&FAKE_VM);
    int tag;
    const char *code = NULL, *err = NULL;
    CHECK(muse_apps_install("hello", true, on_done, &tag, &code, &err), "not queued: %s: %s", code, err);
    CHECK(n_jobs == 1 && dones == 0 && vm_reads == 0, "one job, not run yet: %d jobs, %d dones", n_jobs, dones);
    run_jobs();
    CHECK(dones == 1 && done_ctx == &tag && vm_reads == 2 && vm_writes == 0, "done once, with its ctx: %d dones, %d reads, %d writes",
          dones, vm_reads, vm_writes);
    CHECK(!done_held, "the hold was let go before done");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(done_result, "ok")), "not ok: %s", done_text("error", "message"));
    CHECK(strcmp(done_text("payload", "id"), "hello") == 0 && strcmp(done_text("payload", "storage"), "ram") == 0 &&
              strcmp(done_text("payload", "state"), "running") == 0 &&
              strcmp(done_text("payload", "hint"), APPS_RAM_HINT) == 0,
          "the payload");
    CHECK(compiles == 1 && runs == 1 && run_front && run_settle == APPS_SETTLE_MS && strcmp(run_id, "hello") == 0,
          "compiled, then run in front with the settle");
    CHECK(apps_ram_has("hello"), "kept in the store");
    CHECK(strstr(last_log, "install hello from vm done: ok (2 files, ") != NULL, "the last line: %s", last_log);
    cJSON_Delete(done_result);
    done_result = NULL;
    /* Without run: kept, not run. */
    CHECK(muse_apps_install("hello", false, on_done, NULL, &code, &err), "not queued: %s: %s", code, err);
    run_jobs();
    CHECK(dones == 2 && runs == 1 && strcmp(done_text("payload", "state"), "installed") == 0, "installed, not run");
    cJSON_Delete(done_result);
    done_result = NULL;
    /* A file that isn't on the VM: the result says so, through done all the same. */
    muse_apps_set_vm_ops(&FAKE_VM);
    CHECK(muse_apps_install("nope", true, on_done, NULL, &code, &err), "not queued: %s: %s", code, err);
    run_jobs();
    CHECK(dones == 3 && strcmp(done_text("error", "code"), "read_failed") == 0 &&
              strcmp(done_text("error", "message"), "reading manifest.json: VM answered HTTP 404") == 0,
          "the failure");
    CHECK(strstr(last_log, "install nope from vm done: failed (0 files, ") != NULL, "the last line: %s", last_log);
    cJSON_Delete(done_result);
    done_result = NULL;
    (void)take_output();  /* nothing: a Link install prints no reply on the console */
}

/* D14: one install at a time, whatever it comes from. A second app.install while one is queued or running is busy, at once; so is
 * the console's commit while a Link install runs, and a Link install while a commit's job runs. Neither refusal touches the other's
 * job, and once both are done an install is taken again. */
static void a_second_install_is_busy(void) {
    muse_apps_set_vm_ops(&FAKE_VM);
    const char *code = NULL, *err = NULL;
    CHECK(muse_apps_install("hello", true, on_done, NULL, &code, &err), "not queued: %s: %s", code, err);
    code = err = NULL;
    CHECK(!muse_apps_install("hello", true, on_done, NULL, &code, &err), "a second install was queued");
    CHECK(code != NULL && strcmp(code, "busy") == 0 && err != NULL && strcmp(err, APPS_INSTALL_BUSY) == 0, "%s: %s", code, err);
    stage_hello();
    (void)take_output();
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("@app.err {\"op\":\"commit\",\"id\":\"hello\",\"message\":\"busy: another install is running\"}\n");
    CHECK(n_jobs == 1, "only the first install's job: %d", n_jobs);
    run_jobs();
    CHECK(dones == 1 && runs == 1, "the first install ran, once: %d dones, %d runs", dones, runs);
    cJSON_Delete(done_result);
    done_result = NULL;
    /* The console's commit (its stage is still there) waits for the worker: a Link install is busy meanwhile. */
    CHECK(line("app.commit hello run"), "commit");
    EXPECT("");
    code = err = NULL;
    CHECK(!muse_apps_install("hello", true, on_done, NULL, &code, &err), "an install was queued behind a commit");
    CHECK(code != NULL && strcmp(code, "busy") == 0 && strcmp(err, APPS_INSTALL_BUSY) == 0, "%s: %s", code, err);
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,\"payload\":{\"id\":\"hello\"", NULL);
    CHECK(dones == 1 && runs == 2, "the commit ran, and no Link install: %d dones, %d runs", dones, runs);
    CHECK(muse_apps_install("hello", false, on_done, NULL, &code, &err), "not taken after both: %s: %s", code, err);
    run_jobs();
    CHECK(dones == 2, "done: %d", dones);
    cJSON_Delete(done_result);
    done_result = NULL;
}

/* What muse_apps_install refuses, at once and with nothing queued, done never called and the hold let go: no VM ops (unavailable),
 * not an app id or no done (invalid_params), no memory for the job or a worker that can't take it (out_of_memory). And the id's
 * message the handler says, as an install says it. */
static void link_install_refusals(void) {
    const char *code = NULL, *err = NULL;
    muse_apps_set_vm_ops(NULL);
    CHECK(!muse_apps_install("hello", true, on_done, NULL, &code, &err) && strcmp(code, "unavailable") == 0, "no VM: %s: %s", code,
          err);
    muse_apps_set_vm_ops(&FAKE_VM);
    CHECK(!muse_apps_install("Hello", true, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "a bad id: %s", code);
    CHECK(!muse_apps_install(NULL, true, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no id: %s", code);
    CHECK(!muse_apps_install("hello", true, NULL, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no done: %s", code);
    submit_refuses = true;
    CHECK(!muse_apps_install("hello", true, on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0 &&
              strcmp(err, APPS_IO_REFUSED) == 0,
          "the worker refused: %s: %s", code, err);
    submit_refuses = false;
    port_refuse = 0;
    CHECK(!muse_apps_install("hello", true, on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0 &&
              strcmp(err, "out of memory: no room for the install") == 0,
          "no memory for the job: %s: %s", code, err);
    port_refuse = -1;
    CHECK(n_jobs == 0 && dones == 0 && vm_reads == 0, "nothing queued, done never called");
    CHECK(apps_install_try_begin(), "the hold was let go");
    apps_install_end();
    char msg[160];
    CHECK(muse_apps_id_valid("timer", msg, sizeof msg), "timer is an id");
    CHECK(!muse_apps_id_valid("Timer\x01", msg, sizeof msg) && strcmp(msg, "app id 'Timer?' must match ^[a-z][a-z0-9_]{1,23}$") == 0,
          "%s", msg);
    CHECK(!muse_apps_id_valid(NULL, msg, sizeof msg) && strcmp(msg, "app id '' must match ^[a-z][a-z0-9_]{1,23}$") == 0, "%s", msg);
    CHECK(!muse_apps_id_valid("x", msg, 8) && strcmp(msg, "app id ") == 0, "cut to its room: '%s'", msg);
}

/* ---- Link's app.control and app.api (Task 23): apps_control.c ---- */

/* r printed and deleted: the text, which the caller frees. */
static char *result_text(cJSON *r) {
    CHECK(r != NULL, "no result");
    char *text = cJSON_PrintUnformatted(r);
    CHECK(text != NULL, "no memory to print");
    char *copy = strdup(text);
    cJSON_free(text);
    cJSON_Delete(r);
    return copy;
}

/* r (deleted here) prints as want. */
static void expect_result_at(int line, cJSON *r, const char *want) {
    char *got = result_text(r);
    if (strcmp(got, want) != 0) {
        fail_at(line, "the result was\n%s\ninstead of\n%s", got, want);
    }
    free(got);
}
#define EXPECT_RESULT(r, want) expect_result_at(__LINE__, (r), (want))

/* What r has at the path a.b ("payload.state"), as text: a string as it is, anything else as it prints; "(none)" when there is
 * nothing there. */
static const char *at_path(const cJSON *r, const char *path) {
    static char out[512];
    char key[64];
    const cJSON *at = r;
    for (const char *p = path; at != NULL && *p != '\0';) {
        size_t n = strcspn(p, ".");
        snprintf(key, sizeof key, "%.*s", (int)n, p);
        at = cJSON_GetObjectItemCaseSensitive(at, key);
        p += n + (p[n] == '.');
    }
    if (at == NULL) {
        return "(none)";
    }
    if (cJSON_IsString(at)) {
        snprintf(out, sizeof out, "%s", at->valuestring);
    } else {
        char *text = cJSON_PrintUnformatted(at);
        snprintf(out, sizeof out, "%s", text != NULL ? text : "?");
        cJSON_free(text);
    }
    return out;
}
#define CHECK_AT(r, path, want) \
    CHECK(strcmp(at_path((r), (path)), (want)) == 0, "%s is %s, not %s", (path), at_path((r), (path)), (want))

/* payload.apps[i] of a list's result. */
static const cJSON *listed_app(const cJSON *r, int i) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "payload"), "apps"), i);
}

/* A file of components/muse/apps/ as it is on disk (the caller frees it), and its length. */
static char *apps_file(const char *rel, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", APPS_SOURCE_DIR, rel);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "can't open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    CHECK(text != NULL && fread(text, 1, (size_t)n, f) == (size_t)n, "can't read %s", path);
    fclose(f);
    text[n] = '\0';
    *len = (size_t)n;
    return text;
}

#define NOT_FOUND(id) "no app '" id "' on this gadget (app.control list shows the installed ones)"
#define CONTROL_BUSY "the gadget is busy with another app job (an install, a launch or the guide); try again in a few seconds"

static const char MANIFEST_TIMER[] = "{\"id\":\"timer\",\"name\":\"Timer\",\"version\":\"0.3\",\"api\":1,\"messages\":{"
                                     "\"start\": \"Start a countdown.\", \"status\": \"Replies {state, remaining}.\"}}";

/* hello, then timer (with its messages), installed in the RAM store: lists have timer first, the most recent. */
static void install_two(void) {
    install_hello();
    put_text("timer", "manifest.json", MANIFEST_TIMER);
    put_text("timer", "main.lua", "return {}\n");
    CHECK(line("app.commit timer"), "commit");
    run_jobs();
    (void)take_output();
}

/* The record says hello crashed, with lines lines in its log ("line 00", ...). */
static void record_hello_crashed(int lines) {
    memset(&status_record, 0, sizeof status_record);
    status_record.state = APPS_STATE_CRASHED;
    snprintf(status_record.id, sizeof status_record.id, "hello");
    snprintf(status_record.name, sizeof status_record.name, "Hello");
    snprintf(status_record.version, sizeof status_record.version, "1.0.0");
    snprintf(status_record.last_error, sizeof status_record.last_error,
             "main.lua:1: boom\nstack traceback:\n\tmain.lua:1: in main chunk");
    snprintf(status_record.ui_dump, sizeof status_record.ui_dump, "[]");
    size_t at = 0;
    for (int i = 0; i < lines; i++) {
        at += (size_t)snprintf(status_record.log_tail + at, sizeof status_record.log_tail - at, "%sline %02d", i > 0 ? "\n" : "",
                               i);
    }
    status_record.limit_kb = 256;
}

/*
 * app.control list, status and logs: synchronous on the caller's task, each a job of the I/O worker (the store is the worker's,
 * X11) that the caller waits for. list is the store's apps, most recent first, each with its version, its state (the record's
 * for the record's app, stopped for the rest), its last error and its messages. status is the record's (apps_install_status)
 * for the record's app, and a stopped one by its manifest for any other, with the store's storage and hint, its log the last 50
 * lines (D26); logs is those lines alone. An id the gadget doesn't have is not_found; one that can't be an id is refused at once.
 */
static void control_list_and_status(void) {
    int before = delays;
    EXPECT_RESULT(muse_apps_list(), "{\"ok\":true,\"payload\":{\"apps\":[]}}");
    CHECK(n_jobs == 0 && delays > before, "the list was the worker's job, waited for");
    install_two();
    record_hello_crashed(3);
    EXPECT_RESULT(muse_apps_list(),
                  "{\"ok\":true,\"payload\":{\"apps\":[{\"id\":\"timer\",\"name\":\"Timer\",\"version\":\"0.3\",\"state\":\"stopped\","
                  "\"last_error\":\"\",\"messages\":{\"start\":\"Start a countdown.\",\"status\":\"Replies {state, remaining}.\"}},"
                  "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\",\"state\":\"crashed\",\"last_error\":\"main.lua:1: "
                  "boom\\nstack traceback:\\n\\tmain.lua:1: in main chunk\",\"messages\":{}}]}}");
    /* A hung app is hung. */
    status_record.state = APPS_STATE_HUNG;
    cJSON *r = muse_apps_list();
    CHECK(strcmp(at_path(listed_app(r, 1), "state"), "hung") == 0 && strcmp(at_path(listed_app(r, 0), "state"), "stopped") == 0,
          "hello hung, timer stopped");
    cJSON_Delete(r);
    status_record.state = APPS_STATE_CRASHED;

    /* status: the record's app, with the store's words. */
    r = muse_apps_status("hello", false);
    CHECK_AT(r, "ok", "true");
    CHECK_AT(r, "payload.state", "crashed");
    CHECK_AT(r, "payload.id", "hello");
    CHECK_AT(r, "payload.version", "1.0.0");
    CHECK_AT(r, "payload.last_error", "main.lua:1: boom\nstack traceback:\n\tmain.lua:1: in main chunk");
    CHECK_AT(r, "payload.log", "[\"line 00\",\"line 01\",\"line 02\"]");
    CHECK_AT(r, "payload.limit_kb", "256");
    CHECK_AT(r, "payload.cpu_pct", "0");
    CHECK_AT(r, "payload.storage", "ram");
    CHECK_AT(r, "payload.hint", APPS_RAM_HINT);
    cJSON_Delete(r);
    /* An installed app that isn't the record's: stopped, by its manifest, with nothing else to say. */
    r = muse_apps_status("timer", false);
    CHECK_AT(r, "payload.state", "stopped");
    CHECK_AT(r, "payload.id", "timer");
    CHECK_AT(r, "payload.name", "Timer");
    CHECK_AT(r, "payload.version", "0.3");
    CHECK_AT(r, "payload.front", "false");
    CHECK_AT(r, "payload.last_error", "");
    CHECK_AT(r, "payload.log", "[]");
    CHECK_AT(r, "payload.ui", "[]");
    CHECK_AT(r, "payload.limit_kb", "256");
    cJSON_Delete(r);
    store_mode = APPS_STORAGE_FLASH;
    r = muse_apps_status("timer", false);
    CHECK_AT(r, "payload.storage", "flash");
    CHECK_AT(r, "payload.hint", "(none)");
    cJSON_Delete(r);
    store_mode = APPS_STORAGE_RAM;
    /* Ids the gadget doesn't have, and ids that can't be. */
    EXPECT_RESULT(muse_apps_status("nosuch", false),
                  "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"" NOT_FOUND("nosuch") "\"}}");
    EXPECT_RESULT(muse_apps_status("nosuch", true),
                  "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"" NOT_FOUND("nosuch") "\"}}");
    EXPECT_RESULT(muse_apps_status("Bad", false), "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"app id 'Bad' "
                                                  "must match ^[a-z][a-z0-9_]{1,23}$\"}}");
    EXPECT_RESULT(muse_apps_status(NULL, false), "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"app id '' "
                                                 "must match ^[a-z][a-z0-9_]{1,23}$\"}}");
    store_mode = APPS_STORAGE_NONE;
    EXPECT_RESULT(muse_apps_status("hello", false),
                  "{\"ok\":false,\"error\":{\"code\":\"unavailable\",\"message\":\"storage not ready\"}}");
    store_mode = APPS_STORAGE_RAM;

    /* The last 50 lines, in status and in logs (D26), of a log of 60. */
    record_hello_crashed(60);
    char want[1200];
    size_t at = (size_t)snprintf(want, sizeof want, "{\"ok\":true,\"payload\":{\"id\":\"hello\",\"log\":[");
    for (int i = 10; i < 60; i++) {
        at += (size_t)snprintf(want + at, sizeof want - at, "%s\"line %02d\"", i > 10 ? "," : "", i);
    }
    snprintf(want + at, sizeof want - at, "]}}");
    EXPECT_RESULT(muse_apps_status("hello", true), want);
    r = muse_apps_status("hello", false);
    const cJSON *log = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "payload"), "log");
    CHECK(cJSON_GetArraySize(log) == 50 && strcmp(cJSON_GetArrayItem(log, 0)->valuestring, "line 10") == 0 &&
              strcmp(cJSON_GetArrayItem(log, 49)->valuestring, "line 59") == 0,
          "status: the last 50 lines");
    cJSON_Delete(r);
    EXPECT_RESULT(muse_apps_status("timer", true), "{\"ok\":true,\"payload\":{\"id\":\"timer\",\"log\":[]}}");
    CHECK(n_jobs == 0 && internal_blocks == 0, "nothing left waiting");
    memset(&status_record, 0, sizeof status_record);
}

/*
 * A synchronous wait is bounded (D20: MUSE_APPS_CONTROL_WAIT_MS, 500): with the worker held in a long job, list says busy once
 * the wait is over, and the job, when it runs, finds nobody waiting and frees what it made and its request (its one block of
 * internal RAM, for the atomic). A worker that can't take the job, and no memory for the request, refuse at once.
 */
static void control_list_waits_at_most_500_ms(void) {
    install_hello();
    size_t held = port_held, cjson = cjson_held;
    int blocks = internal_blocks;
    worker_busy = true;
    int64_t t0 = fake_ms;
    EXPECT_RESULT(muse_apps_list(), "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"" CONTROL_BUSY "\"}}");
    CHECK(fake_ms - t0 >= MUSE_APPS_CONTROL_WAIT_MS && fake_ms - t0 <= MUSE_APPS_CONTROL_WAIT_MS + 10, "waited %lld ms",
          (long long)(fake_ms - t0));
    CHECK(n_jobs == 1 && internal_blocks == blocks + 1, "the job waits still, with its request");
    t0 = fake_ms;
    EXPECT_RESULT(muse_apps_status("hello", true), "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"" CONTROL_BUSY "\"}}");
    CHECK(fake_ms - t0 == MUSE_APPS_CONTROL_WAIT_MS && n_jobs == 2, "status waits as long: %lld ms", (long long)(fake_ms - t0));
    worker_busy = false;
    run_jobs();
    CHECK(internal_blocks == blocks && port_held == held && cjson_held == cjson, "the late jobs freed everything: %d blocks, %zu, %zu",
          internal_blocks - blocks, port_held - held, cjson_held - cjson);
    /* Refused at once. */
    submit_refuses = true;
    t0 = fake_ms;
    EXPECT_RESULT(muse_apps_list(), "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"" APPS_IO_REFUSED "\"}}");
    submit_refuses = false;
    internal_refuses = true;
    EXPECT_RESULT(muse_apps_status("hello", false), "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"out of "
                                                    "memory: no room for the request\"}}");
    internal_refuses = false;
    CHECK(fake_ms == t0 && n_jobs == 0 && internal_blocks == blocks, "no wait, nothing queued, nothing held");
}

/*
 * launch, stop and remove: queued like an install (D20), done once on the worker. launch loads the store's copy and runs it by id
 * in front with the settle (an app of that id that runs is fronted, not started again: apps_task_run_by_id), and answers the
 * status; a store that refuses the load (the quarantine) says why, and a run that crashed says so as a launch does (apps_install.h).
 * stop stops the app if it is the one that runs, and answers {} either way; remove is the store's (Task 20), which refuses the app
 * that runs.
 */
static void control_launch_stop_remove(void) {
    install_two();
    const char *code = NULL, *err = NULL;
    int tag;
    /* launch */
    CHECK(muse_apps_launch_async("hello", on_done, &tag, &code, &err), "not queued: %s: %s", code, err);
    CHECK(n_jobs == 1 && dones == 0, "queued, not run");
    memset(&status_record, 0, sizeof status_record);
    status_record.state = APPS_STATE_RUNNING;
    snprintf(status_record.id, sizeof status_record.id, "hello");
    snprintf(status_record.ui_dump, sizeof status_record.ui_dump, "[]");
    status_record.front = true;
    run_jobs();
    CHECK(dones == 1 && done_ctx == &tag, "done once, with its ctx");
    CHECK(runs_by_id == 1 && run_front && run_settle == APPS_SETTLE_MS && strcmp(run_id, "hello") == 0, "run by id, in front, settled");
    CHECK_AT(done_result, "ok", "true");
    CHECK_AT(done_result, "payload.state", "running");
    CHECK_AT(done_result, "payload.id", "hello");
    CHECK_AT(done_result, "payload.front", "true");
    CHECK_AT(done_result, "payload.storage", "ram");
    CHECK_AT(done_result, "payload.hint", APPS_RAM_HINT);
    /* Not on the gadget; refused by the store (the quarantine); crashed in start(); no store yet. */
    CHECK(muse_apps_launch_async("nosuch", on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK_AT(done_result, "error.code", "not_found");
    CHECK_AT(done_result, "error.message", NOT_FOUND("nosuch"));
    store_load_refusal = "hello was running during two restarts in a row; reinstall it to run it again";
    CHECK(muse_apps_launch_async("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    store_load_refusal = NULL;
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "hello was running during two restarts in a row; reinstall it to run it again");
    run_ok = false;
    snprintf(run_err, sizeof run_err, "crashed in start(): main.lua:2: boom");
    run_error_record = "main.lua:2: boom\nstack traceback:\n\tmain.lua:2: in function 'start'";
    CHECK(muse_apps_launch_async("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    run_ok = true;
    run_error_record = "";
    static const char crashed[] = "crashed in start(): main.lua:2: boom\nstack traceback:\n\tmain.lua:2: in function 'start'\nlog:\n";
    CHECK_AT(done_result, "error.code", "crashed");
    CHECK(strncmp(at_path(done_result, "error.message"), crashed, strlen(crashed)) == 0,
          "a launch's words, with no \"installed but\": %s", at_path(done_result, "error.message"));
    store_mode = APPS_STORAGE_NONE;
    CHECK(muse_apps_launch_async("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    store_mode = APPS_STORAGE_RAM;
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "storage not ready");
    CHECK(runs_by_id == 2, "two runs: the first launch and the crash");

    /* stop: the app that runs, if it is that one. */
    snprintf(running_app, sizeof running_app, "hello");
    int before = stops;
    CHECK(muse_apps_stop("hello", on_done, NULL, &code, &err), "queued");
    CHECK(stops == before, "nothing stopped before the worker runs it");
    run_jobs();
    CHECK(stops == before + 1, "stopped");
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{}}");
    done_result = NULL;
    CHECK(muse_apps_stop("timer", on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK(stops == before + 1, "timer doesn't run: nothing stopped");
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{}}");
    done_result = NULL;
    stop_ok = false;
    stop_err = "the app stopped responding; no app can run until its stuck call returns or the gadget restarts";
    CHECK(muse_apps_stop("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    stop_ok = true;
    CHECK_AT(done_result, "error.code", "busy");
    CHECK_AT(done_result, "error.message",
             "the app stopped responding; no app can run until its stuck call returns or the gadget restarts");

    /* remove: not the app that runs; then timer goes; then it isn't there; then no store. */
    CHECK(muse_apps_remove("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK_AT(done_result, "error.code", "busy");
    CHECK_AT(done_result, "error.message", "hello is running; stop it first");
    CHECK(apps_ram_has("hello"), "hello stays");
    running_app[0] = '\0';
    CHECK(muse_apps_remove("timer", on_done, NULL, &code, &err), "queued");
    run_jobs();
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{\"removed\":\"timer\"}}");
    done_result = NULL;
    CHECK(!apps_ram_has("timer") && apps_ram_has("hello"), "timer removed, hello kept");
    CHECK(muse_apps_remove("timer", on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK_AT(done_result, "error.code", "not_found");
    CHECK_AT(done_result, "error.message", NOT_FOUND("timer"));
    store_mode = APPS_STORAGE_NONE;
    CHECK(muse_apps_remove("hello", on_done, NULL, &code, &err), "queued");
    run_jobs();
    store_mode = APPS_STORAGE_RAM;
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "storage not ready");
    /* A folder the store doesn't list (its manifest.json is damaged) can still be removed; one whose remove fails is the
     * store's refusal, unavailable, not not_found. */
    remove_refusal = "couldn't remove ghost: I/O error";
    CHECK(muse_apps_remove("ghost", on_done, NULL, &code, &err), "queued");
    run_jobs();
    remove_refusal = NULL;
    CHECK(!apps_ram_has("ghost"), "ghost isn't listed");
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "couldn't remove ghost: I/O error");
    cJSON_Delete(done_result);
    done_result = NULL;

    /* Refused at once, nothing queued and done never called. */
    int calls = dones;
    CHECK(!muse_apps_launch_async("Bad", on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0 &&
              strcmp(err, "app id must match ^[a-z][a-z0-9_]{1,23}$") == 0,
          "a bad id: %s: %s", code, err);
    CHECK(!muse_apps_remove("hello", NULL, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no done: %s", code);
    CHECK(!muse_apps_stop(NULL, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no id: %s", code);
    submit_refuses = true;
    CHECK(!muse_apps_launch_async("hello", on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0 &&
              strcmp(err, APPS_IO_REFUSED) == 0,
          "the worker refused: %s: %s", code, err);
    submit_refuses = false;
    port_refuse = 0;
    CHECK(!muse_apps_remove("hello", on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0, "no memory: %s", code);
    port_refuse = -1;
    CHECK(n_jobs == 0 && dones == calls, "nothing queued, done never called");
    memset(&status_record, 0, sizeof status_record);
}

/*
 * app.send's job (Task 24), on the I/O worker: the message goes to the app that runs, at once; to any other installed app it goes
 * after the store's copy is loaded and run behind (apps_task_run_by_id, no settle: the launch has its own bound), which makes it the
 * most recent. The reply is the payload's "reply" (the handler's JSON as it is, null for none); show goes to apps_task_message, with
 * the 5 s reply bound. What can't be done is a result with a code: not installed (MUSE_APPS_NOT_FOUND), the store's refusals
 * (unavailable), a launch that failed (crashed, not_running), no reply (timed_out, MUSE_APPS_SEND_NO_REPLY), the handler's own error
 * as it is (app_error), the app task's refusals. Refused at once: a bad id, no done, no event, too much data, no memory.
 */
static void control_send(void) {
    install_two();
    const char *code = NULL, *err = NULL;
    int tag;
    /* The app that runs: the message, and nothing launched. */
    snprintf(running_app, sizeof running_app, "timer");
    message_reply = "{\"state\":\"running\",\"remaining\":300}";
    CHECK(muse_apps_send("timer", "start", "{\"seconds\":300}", false, on_done, &tag, &code, &err), "not queued: %s: %s", code, err);
    CHECK(n_jobs == 1 && dones == 0 && messages == 0, "queued, nothing run before the worker");
    run_jobs();
    CHECK(dones == 1 && done_ctx == &tag, "done once, with its ctx");
    CHECK(runs_by_id == 0 && messages == 1, "no launch: %d runs, %d messages", runs_by_id, messages);
    CHECK(strcmp(message_id, "timer") == 0 && strcmp(message_event, "start") == 0 && !message_data_null &&
              strcmp(message_data, "{\"seconds\":300}") == 0 && !message_show && message_timeout == MUSE_APPS_SEND_REPLY_MS,
          "message %s %s %s show %d timeout %d", message_id, message_event, message_data, message_show, message_timeout);
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{\"reply\":{\"state\":\"running\",\"remaining\":300}}}");
    done_result = NULL;
    CHECK(logged("I muse_apps: app.send timer start {\"seconds\":300}") != NULL, "the job logs its event and data: '%s'", last_log);
    /* show, no data, and a handler that returned nothing. */
    message_reply = "null";
    CHECK(muse_apps_send("timer", "status", NULL, true, on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK(message_show && message_data_null && message_timeout == 5000, "show, no data");
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{\"reply\":null}}");
    done_result = NULL;

    /* An installed app that isn't running: loaded, run behind with no settle, and then sent to. */
    running_app[0] = '\0';
    message_reply = "{\"state\":\"running\",\"remaining\":300}";
    int before_runs = runs_by_id, before_messages = messages, before_sends = send_launches;
    CHECK(muse_apps_send("hello", "start", "{\"seconds\":300}", true, on_done, &tag, &code, &err), "queued");
    run_jobs();
    CHECK(send_launches == before_sends + 1 && runs_by_id == before_runs && strcmp(run_id, "hello") == 0 && !run_front && run_settle == 0,
          "hello launched by app.send's launch, behind with no settle: %d, front %d, settle %d", send_launches, run_front, run_settle);
    CHECK(messages == before_messages + 1 && strcmp(message_id, "hello") == 0 && message_show, "then the message, with show");
    CHECK_AT(done_result, "ok", "true");
    CHECK_AT(done_result, "payload.reply.state", "running");
    char ids[2][APPS_ID_MAX + 1], names[2][25];
    CHECK(apps_store_list(ids, names, 2) == 2 && strcmp(ids[0], "hello") == 0, "the load made hello the most recent: %s", ids[0]);
    cJSON_Delete(done_result);
    done_result = NULL;

    /* Not installed: the exact words, and nothing launched or sent. */
    before_runs = send_launches;
    before_messages = messages;
    CHECK(muse_apps_send("nosuch", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK_AT(done_result, "error.code", "not_found");
    CHECK_AT(done_result, "error.message", NOT_FOUND("nosuch"));
    CHECK(logged("I muse_apps: app.send nosuch start: not_found") != NULL, "a refusal logs its code: '%s'", last_log);
    CHECK(send_launches == before_runs && messages == before_messages, "nothing launched, nothing sent");
    /* No store yet; the store's refusal (the quarantine). */
    store_mode = APPS_STORAGE_NONE;
    CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    store_mode = APPS_STORAGE_RAM;
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "storage not ready");
    store_load_refusal = "timer was running during two restarts in a row; reinstall it to run it again";
    CHECK(muse_apps_send("timer", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    store_load_refusal = NULL;
    CHECK_AT(done_result, "error.code", "unavailable");
    CHECK_AT(done_result, "error.message", "timer was running during two restarts in a row; reinstall it to run it again");
    CHECK(send_launches == before_runs && messages == before_messages, "a quarantined app isn't launched or sent to");

    /* The app crashes in start() on its launch: no message goes. */
    run_ok = false;
    snprintf(run_err, sizeof run_err, "crashed in start(): main.lua:2: boom");
    CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK_AT(done_result, "error.code", "crashed");
    CHECK_AT(done_result, "error.message", "'hello' didn't start: crashed in start(): main.lua:2: boom");
    snprintf(run_err, sizeof run_err, "main.lua:3: unexpected symbol near 'x'");
    CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    run_ok = true;
    CHECK_AT(done_result, "error.code", "not_running");
    CHECK_AT(done_result, "error.message", "'hello' didn't start: main.lua:3: unexpected symbol near 'x'");
    CHECK(messages == before_messages, "no message to an app that didn't start");

    /* The message's failures. The handler's text is as it is, and the app keeps running. */
    static const struct {
        const char *text, *code, *message;
    } failures[] = {
        {"main.lua:142: unknown event 'nope'; use start, pause", "app_error", "main.lua:142: unknown event 'nope'; use start, pause"},
        {"main.lua:7: boom\nstack traceback:\n\tmain.lua:7: in function 'message'", "app_error",
         "main.lua:7: boom\\nstack traceback:\\n\\tmain.lua:7: in function 'message'"},
        {"data: unexpected character", "app_error", "data: unexpected character"},
        {"the app task didn't answer within 5000 ms", "timed_out", MUSE_APPS_SEND_NO_REPLY},
        {"hello isn't running", "not_running", "hello isn't running"},
        {"the app task is busy; try again", "busy", "the app task is busy; try again"},
        {"the app stopped responding; no app can run until its stuck call returns or the gadget restarts", "unavailable",
         "the app stopped responding; no app can run until its stuck call returns or the gadget restarts"},
        {"Muse apps aren't running", "unavailable", "Muse apps aren't running"},
        {"out of memory: no room for the request", "out_of_memory", "out of memory: no room for the request"},
    };
    snprintf(running_app, sizeof running_app, "hello");
    message_reply = NULL;
    for (size_t i = 0; i < ARRAY_LEN(failures); i++) {
        message_err = failures[i].text;
        CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
        run_jobs();
        CHECK_AT(done_result, "ok", "false");
        CHECK(strcmp(at_path(done_result, "error.code"), failures[i].code) == 0, "'%s' is %s, not %s", failures[i].text,
              at_path(done_result, "error.code"), failures[i].code);
        char want[400];
        snprintf(want, sizeof want, "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", failures[i].code, failures[i].message);
        EXPECT_RESULT(done_result, want);
        done_result = NULL;
    }
    message_reply = "{}";
    message_err = "";
    running_app[0] = '\0';

    /* A reply that isn't JSON text is still sent: as a string. */
    snprintf(running_app, sizeof running_app, "hello");
    message_reply = "not json";
    CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
    run_jobs();
    EXPECT_RESULT(done_result, "{\"ok\":true,\"payload\":{\"reply\":\"not json\"}}");
    done_result = NULL;
    message_reply = "{}";
    running_app[0] = '\0';

    /* No memory for the result, whichever allocation of it fails: done is still called, once, with the result or the lack of memory
     * (NULL when there is none for even that; Link's done says so), and nothing is left held (the harness checks at its end). */
    snprintf(running_app, sizeof running_app, "hello");
    message_reply = "{\"state\":\"running\"}";
    for (long refused = 0; refused < 8; refused++) {
        int calls = dones;
        CHECK(muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err), "queued");
        cjson_refuse = refused;
        run_jobs();
        cjson_refuse = -1;
        CHECK(dones == calls + 1, "done called once (allocation %ld refused)", refused);
        const char *ok = at_path(done_result, "ok");
        CHECK(done_result == NULL || strcmp(ok, "true") == 0 ||
                  strcmp(at_path(done_result, "error.code"), "out_of_memory") == 0,
              "a result or the lack of memory (allocation %ld refused)", refused);
    }
    cJSON_Delete(done_result);
    done_result = NULL;
    message_reply = "{}";
    running_app[0] = '\0';

    /* Refused at once: nothing queued and done never called. */
    int calls = dones;
    CHECK(!muse_apps_send("Bad", "start", NULL, false, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "a bad id");
    CHECK(!muse_apps_send(NULL, "start", NULL, false, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no id");
    CHECK(!muse_apps_send("hello", "", NULL, false, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no event");
    CHECK(!muse_apps_send("hello", NULL, NULL, false, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "null event");
    CHECK(!muse_apps_send("hello", "start", NULL, false, NULL, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no done");
    char *big = malloc(MUSE_APPS_SEND_DATA_MAX + 2);
    CHECK(big != NULL, "no memory");
    memset(big, ' ', MUSE_APPS_SEND_DATA_MAX + 1);
    big[MUSE_APPS_SEND_DATA_MAX + 1] = '\0';
    CHECK(!muse_apps_send("hello", "start", big, false, on_done, NULL, &code, &err) && strcmp(code, "invalid_params") == 0 &&
              strcmp(err, "data is larger than 4 KB") == 0,
          "4097 bytes of data: %s", err);
    free(big);
    submit_refuses = true;
    CHECK(!muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0 &&
              strcmp(err, APPS_IO_REFUSED) == 0,
          "the worker refused: %s: %s", code, err);
    submit_refuses = false;
    port_refuse = 0;
    CHECK(!muse_apps_send("hello", "start", NULL, false, on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0, "no memory");
    port_refuse = -1;
    CHECK(n_jobs == 0 && dones == calls, "nothing queued, done never called");
}

/* The wrapper main/ validates data through: one JSON value, an object at the top (D25). */
static void json_object_valid(void) {
    char err[96];
    static const char *const objects[] = {"{}", "{\"seconds\":300}", " \t\r\n{\"a\":[1,{\"b\":null}]}\n ", "{\"a\":\"\\u00e9\"}"};
    for (size_t i = 0; i < ARRAY_LEN(objects); i++) {
        CHECK(muse_apps_json_object_valid(objects[i], strlen(objects[i]), err, sizeof err), "'%s': %s", objects[i], err);
    }
    static const char *const refused[] = {"", " ", "[1,2]", "[]", "5", "true", "null", "\"x\"", "{", "{\"a\":}", "{} {}", "{}x", "{'a':1}",
                                          "{\"a\":1,}", "[{\"a\":1}]"};
    for (size_t i = 0; i < ARRAY_LEN(refused); i++) {
        err[0] = '\0';
        CHECK(!muse_apps_json_object_valid(refused[i], strlen(refused[i]), err, sizeof err), "'%s' isn't an object", refused[i]);
        CHECK(err[0] != '\0', "'%s' says why", refused[i]);
    }
    /* Only len bytes are read: an object followed by more. */
    CHECK(muse_apps_json_object_valid("{}x", 2, err, sizeof err), "two bytes of '{}x'");
    CHECK(!muse_apps_json_object_valid("{}x", 3, err, sizeof err), "three");
    /* Nesting over the decoder's depth (16) is refused, as the app would refuse it. */
    char deep[256];
    size_t n = 0;
    for (int i = 0; i < 20; i++) {
        n += (size_t)snprintf(deep + n, sizeof deep - n, "{\"a\":");
    }
    n += (size_t)snprintf(deep + n, sizeof deep - n, "1");
    for (int i = 0; i < 20 && n + 1 < sizeof deep; i++) {
        deep[n++] = '}';
    }
    deep[n] = '\0';
    CHECK(!muse_apps_json_object_valid(deep, n, err, sizeof err), "20 levels deep");
}

/*
 * app.api (D2, and D3 as the user decided it): one job of the I/O worker writes the guide and the timer example (the firmware's
 * own bytes: EMBED_FILES) to workspace/apps/_guide/ on the VM, logging the DMA RAM's free and lowest marks before and after, and
 * answers with the paths and what the gadget is: api, screen, fonts, limits, storage and the installed apps. When the VM refuses a
 * write, the answer carries the guide and the example inline instead, and says why; a write that fails otherwise (busy, out of
 * memory, timed out) fails app.api with a code, to be asked again.
 */
static void guide_written_or_inline(void) {
    const char *code = NULL, *err = NULL;
    muse_apps_set_vm_ops(NULL);
    CHECK(!muse_apps_write_guide(on_done, NULL, &code, &err) && strcmp(code, "unavailable") == 0 &&
              strcmp(err, "the gadget can't write files on the VM") == 0,
          "no VM: %s: %s", code, err);
    muse_apps_set_vm_ops(&FAKE_VM);
    CHECK(!muse_apps_write_guide(NULL, NULL, &code, &err) && strcmp(code, "invalid_params") == 0, "no done: %s", code);
    install_two();
    size_t guide_len, manifest_len, main_len;
    char *guide = apps_file("apps_guide.md", &guide_len);
    char *manifest = apps_file("examples/timer/manifest.json", &manifest_len);
    char *main_lua = apps_file("examples/timer/main.lua", &main_len);

    /* Written. */
    int tag;
    CHECK(muse_apps_write_guide(on_done, &tag, &code, &err), "not queued: %s: %s", code, err);
    CHECK(n_jobs == 1 && vm_writes == 0, "queued, nothing written yet");
    int64_t started = fake_ms;
    size_t lowest = dma_lowest;
    run_jobs();
    CHECK(dones == 1 && done_ctx == &tag, "done once, with its ctx");
    CHECK(vm_writes == 3, "three files: %d", vm_writes);
    static const char *const paths[] = {"workspace/apps/_guide/README.md", "workspace/apps/_guide/timer/manifest.json",
                                        "workspace/apps/_guide/timer/main.lua"};
    const char *const bytes[] = {guide, manifest, main_lua};
    const size_t lens[] = {guide_len, manifest_len, main_len};
    for (int i = 0; i < 3; i++) {
        CHECK(strcmp(written[i].path, paths[i]) == 0, "write %d: %s", i, written[i].path);
        CHECK(written[i].len == lens[i] && memcmp(written[i].data, bytes[i], lens[i]) == 0, "write %d: the firmware's bytes of %s", i,
              paths[i]);
        CHECK(written[i].deadline_us > started * 1000 && written[i].deadline_us <= (started + MUSE_APPS_GUIDE_WRITE_MS) * 1000,
              "write %d: the deadline %lld", i, (long long)written[i].deadline_us);
    }
    char want[160];
    snprintf(want, sizeof want, "DMA free %zu, lowest %zu", dma_free, lowest);
    const char *at = logged("app.api: writing the guide and the timer to workspace/apps/_guide/");
    CHECK(at != NULL && strstr(at, want) != NULL, "the DMA reading before: %s", at != NULL ? at : last_log);
    snprintf(want, sizeof want, "DMA free %zu, lowest %zu", dma_free, lowest - 3 * 512);
    at = logged("app.api: the guide is on the VM");
    CHECK(at != NULL && strstr(at, want) != NULL, "the DMA reading after: %s", at != NULL ? at : last_log);
    CHECK_AT(done_result, "ok", "true");
    CHECK_AT(done_result, "payload.delivered", "vm");
    CHECK_AT(done_result, "payload.guide", "workspace/apps/_guide/README.md");
    CHECK_AT(done_result, "payload.example", "workspace/apps/_guide/timer/");
    CHECK_AT(done_result, "payload.api", "1");
    CHECK_AT(done_result, "payload.screen", "{\"w\":466,\"h\":466,\"round\":true,\"safe\":330}");
    CHECK_AT(done_result, "payload.fonts", "[\"sans14\",\"sans16\",\"sans20\",\"sans28\",\"mono8\",\"mono16\",\"digits48\",\"digits96\"]");
    CHECK_AT(done_result, "payload.limits.files", "32");
    CHECK_AT(done_result, "payload.limits.lua_kb", "256");
    CHECK_AT(done_result, "payload.limits.json_kb", "32");
    CHECK_AT(done_result, "payload.limits.image_kb", "512");
    CHECK_AT(done_result, "payload.limits.images_kb", "1536");
    CHECK_AT(done_result, "payload.limits.app_kb", "2048");
    CHECK_AT(done_result, "payload.limits.memory_kb", "{\"min\":64,\"default\":256,\"max\":1024}");
    CHECK_AT(done_result, "payload.limits.widgets", "128");
    CHECK_AT(done_result, "payload.limits.timers", "16");
    CHECK_AT(done_result, "payload.limits.alarms", "4");
    CHECK_AT(done_result, "payload.limits.store_kb", "16");
    CHECK_AT(done_result, "payload.storage", "ram");
    CHECK_AT(done_result, "payload.hint", APPS_RAM_HINT);
    CHECK_AT(done_result, "payload.apps", "[{\"id\":\"timer\",\"name\":\"Timer\",\"version\":\"0.3\",\"state\":\"stopped\"},"
                                          "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0.0\",\"state\":\"stopped\"}]");
    CHECK_AT(done_result, "payload.guide_text", "(none)");
    CHECK_AT(done_result, "payload.write_error", "(none)");
    forget_writes();

    /* The VM refuses the second file: inline, and why. */
    write_fails_at = 2;
    CHECK(muse_apps_write_guide(on_done, NULL, &code, &err), "queued");
    run_jobs();
    CHECK(vm_writes == 2, "nothing written after the refusal: %d", vm_writes);
    CHECK_AT(done_result, "ok", "true");
    CHECK_AT(done_result, "payload.delivered", "inline");
    CHECK_AT(done_result, "payload.write_error", "VM answered HTTP 403: route not allowed for this credential's scopes");
    CHECK_AT(done_result, "payload.guide", "(none)");
    CHECK_AT(done_result, "payload.example", "(none)");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(done_result, "payload");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "guide_text");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(payload, "example_files");
    CHECK(cJSON_IsString(text) && strcmp(text->valuestring, guide) == 0, "the guide inline");
    CHECK(cJSON_GetArraySize(files) == 2 &&
              strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(files, "manifest.json")), manifest) == 0 &&
              strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(files, "main.lua")), main_lua) == 0,
          "the timer inline");
    CHECK_AT(done_result, "payload.api", "1");
    CHECK_AT(done_result, "payload.storage", "ram");
    CHECK(logged("app.api: the VM didn't take workspace/apps/_guide/timer/manifest.json: VM answered HTTP 403") != NULL &&
              strstr(logged("app.api: the VM didn't take"), "; the guide goes inline (DMA free ") != NULL,
          "the refusal's line: %s", last_log);
    forget_writes();

    /* Busy, out of memory, timed out, and a session that is gone (nothing will read an answer, so no inline payload is made
     * for it): app.api fails with a code, says to call it again, and the log line names why. */
    static const struct {
        const char *code, *why, *want_code;
    } failures[] = {
        {MUSE_APPS_VM_BUSY, "the VM file writer is busy", "busy"},
        {MUSE_APPS_VM_OUT_OF_MEMORY, "out of memory", "out_of_memory"},
        {MUSE_APPS_VM_TIMED_OUT, "write timed out", "unavailable"},
        {MUSE_APPS_VM_DROPPED, "the VM connection dropped", "unavailable"},
        {MUSE_APPS_VM_DROPPED, "could not send the request to the VM", "unavailable"},
    };
    for (size_t i = 0; i < ARRAY_LEN(failures); i++) {
        write_fails_at = 1;
        write_code = failures[i].code;
        write_why = failures[i].why;
        int lines = logs;
        CHECK(muse_apps_write_guide(on_done, NULL, &code, &err), "queued");
        run_jobs();
        CHECK(vm_writes == 1, "one write tried: %d", vm_writes);
        CHECK_AT(done_result, "error.code", failures[i].want_code);
        char message[160];
        snprintf(message, sizeof message, "couldn't write the guide to the VM: %s; call app.api again", failures[i].why);
        CHECK_AT(done_result, "error.message", message);
        CHECK_AT(done_result, "payload", "(none)");
        snprintf(message, sizeof message, "app.api: couldn't write workspace/apps/_guide/README.md: %s (DMA free ", failures[i].why);
        CHECK(logged(message) != NULL && logs == lines + 2, "the failure's line: %s", last_log);
        forget_writes();
    }
    write_fails_at = 0;
    write_code = MUSE_APPS_VM_FAILED;
    write_why = "VM answered HTTP 403: route not allowed for this credential's scopes";
    /* Refused at once. */
    submit_refuses = true;
    CHECK(!muse_apps_write_guide(on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0 && strcmp(err, APPS_IO_REFUSED) == 0,
          "the worker refused: %s: %s", code, err);
    submit_refuses = false;
    port_refuse = 0;
    CHECK(!muse_apps_write_guide(on_done, NULL, &code, &err) && strcmp(code, "out_of_memory") == 0, "no memory: %s", code);
    port_refuse = -1;
    CHECK(n_jobs == 0 && vm_writes == 0, "nothing queued");
    cJSON_Delete(done_result);
    done_result = NULL;
    free(guide);
    free(manifest);
    free(main_lua);
}

/* F6: the stage holds what was put and a small slack beyond it (with its own block, under 64 KB), so at most 2 MB and
 * 64 KB whatever the files' sizes: checked after every piece. The reviewer's in-limit set, five 257 KB images and five
 * 129 KB modules (1.88 MB) put in 576-byte pieces, made 3.75 MB of blocks when each grew by doubling up to its kind's
 * limit. */
static void the_stage_is_bounded(void) {
    port_peak = port_held;
    port_growths = 0;
    size_t before = port_held, content = 0, worst = 0;
    for (int i = 0; i < 10; i++) {
        char path[16];
        size_t len = i < 5 ? 257 * KB : 129 * KB;
        snprintf(path, sizeof path, i < 5 ? "img%d.bin" : "mod%d.lua", i);
        unsigned seq = 0;
        for (size_t at = 0; at < len; at += 576, seq++) {
            size_t n = len - at < 576 ? len - at : 576;
            CHECK(put("big", path, seq, ZEROS, n), "put not taken");
            char *got = take_output();
            char want[64];
            snprintf(want, sizeof want, "@app.ack %s %u\n", path, seq);
            CHECK(strcmp(got, want) == 0, "%s seq %u: %s", path, seq, got);
            free(got);
            content += n;
            size_t over = port_held - before > content ? port_held - before - content : 0;
            worst = over > worst ? over : worst;
        }
    }
    fprintf(stderr, "the_stage_is_bounded: %zu bytes put, %zu held at most, %zu more than put at worst, %ld growths\n",
            content, port_peak - before, worst, port_growths);
    CHECK(worst < 64 * KB, "the stage held %zu bytes more than was put", worst);
    CHECK(port_peak - before <= APPS_APP_MAX + 64 * KB, "the stage took %zu bytes at most for %zu put: over 2 MB and 64 KB",
          port_peak - before, content);
    /* And a block still grows by a good step, not a piece at a time: 3,431 pieces, a few hundred growths at most. */
    CHECK(port_growths < 400, "%ld growths for %zu bytes in 576-byte pieces", port_growths, content);
}

/* T5 and F8: the stage's own reads. A file the manifest lists that was never put, or no manifest.json, fails the read
 * with "it wasn't sent", the stage freed and the hold let go. An empty file (seq 0 with no base64: its first piece adds
 * nothing to no block) installs as a file of 0 bytes. */
static void empty_and_missing_files(void) {
    static const char manifest[] = "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"lib.lua\"]}";
    put_text("hello", "manifest.json", manifest);
    put_text("hello", "main.lua", MAIN_HELLO);
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT("@app {\"op\":\"commit\",\"ok\":false,\"error\":{\"code\":\"read_failed\",\"message\":\"reading lib.lua: it "
           "wasn't sent (app.put)\"}}\n");
    CHECK(port_held == 0, "the stage was freed: %zu bytes held", port_held);
    CHECK(apps_install_try_begin(), "the hold was let go");
    apps_install_end();
    put_text("hello", "main.lua", MAIN_HELLO);
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT("@app {\"op\":\"commit\",\"ok\":false,\"error\":{\"code\":\"read_failed\",\"message\":\"reading "
           "manifest.json: it wasn't sent (app.put)\"}}\n");
    CHECK(port_held == 0, "the stage was freed: %zu bytes held", port_held);
    put_text("hello", "manifest.json", manifest);
    put_text("hello", "main.lua", MAIN_HELLO);
    CHECK(line("app.put hello lib.lua 0 "), "put");
    EXPECT("@app.ack lib.lua 0\n");
    CHECK(line("app.commit hello"), "commit");
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", "{\"path\":\"lib.lua\",\"bytes\":0,\"sha256\":"
                "\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}");
    CHECK(n_compiled == 2 && strcmp(compiled[1].path, "lib.lua") == 0 && compiled[1].len == 0, "compile saw lib.lua empty");
}

/* R13.1: the hook on exactly the console task's 3,584 bytes, through a put, a commit, a status and a send. */
#define SERIAL_STACK 3584

/* The lines, made beforehand on the heap as muse_input's serial task has them: only the hook runs on the stack. */
static char *stack_lines[6];

static void through_the_console(void *arg) {
    (void)arg;
    for (int i = 0; stack_lines[i] != NULL; i++) {
        (void)muse_apps_console(stack_lines[i]);
    }
}

static char *heap_line(const char *fmt, const char *b64) {
    size_t n = strlen(fmt) + (b64 != NULL ? strlen(b64) : 0) + 1;
    char *text = malloc(n);
    CHECK(text != NULL, "no memory");
    snprintf(text, n, fmt, b64 != NULL ? b64 : "");
    return text;
}

static void console_stack(void) {
    fill_record(&status_record, 0);
    char *main_b64 = base64((const uint8_t *)MAIN_HELLO, strlen(MAIN_HELLO));
    char *manifest_b64 = base64((const uint8_t *)MANIFEST_HELLO, strlen(MANIFEST_HELLO));
    stack_lines[0] = heap_line("app.put hello main.lua 0 %s", main_b64);
    stack_lines[1] = heap_line("app.put hello manifest.json 0 %s", manifest_b64);
    stack_lines[2] = heap_line("app.commit hello run%s", NULL);
    stack_lines[3] = heap_line("app.status%s", NULL);
    stack_lines[4] = heap_line("app.send hello tick {\"n\":1}%s", NULL);
    stack_lines[5] = NULL;
    free(main_b64);
    free(manifest_b64);
    int exact = 0;
    size_t used = on_exact_stack(through_the_console, NULL, SERIAL_STACK, &exact);
    for (int i = 0; stack_lines[i] != NULL; i++) {
        free(stack_lines[i]);
    }
    fprintf(stderr, "console_stack: %zu of %d bytes\n", used, SERIAL_STACK);
    CHECK(used > 0 && exact, "the hook ran on exactly %d bytes (used %zu, exact %d)", SERIAL_STACK, used, exact);
    char *got = take_output();
    CHECK(strstr(got, "@app.ack main.lua 0\n@app.ack manifest.json 0\n@app {\"op\":\"status\",") != NULL &&
              strstr(got, "\n@app {\"op\":\"send\",") != NULL,
          "the replies on the small stack: %s", got);
    free(got);
    run_jobs();
    EXPECT_LINE("@app {\"op\":\"commit\",\"ok\":true,", NULL);
    /* The harness's own line, for the Python test: after the console's, and not one for the console to push. */
    printf("@stack {\"used\":%zu,\"of\":%d}\n", used, SERIAL_STACK);
    fflush(stdout);
    capture_taken = flushed_at = lseek(capture_fd, 0, SEEK_END);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"put_sequence_and_ack", put_sequence_and_ack},
        {"put_out_of_order", put_out_of_order},
        {"put_bad_base64", put_bad_base64},
        {"put_over_kind_limit", put_over_kind_limit},
        {"limits_as_bytes_arrive", limits_as_bytes_arrive},
        {"commit_submits_staged_files", commit_submits_staged_files},
        {"commit_refusals", commit_refusals},
        {"a_new_id_discards_the_stage", a_new_id_discards_the_stage},
        {"launch_runs_an_installed_app", launch_runs_an_installed_app},
        {"stop_reports_the_task", stop_reports_the_task},
        {"replies_end_with_a_push", replies_end_with_a_push},
        {"launch_by_id_prints_nothing", launch_by_id_prints_nothing},
        {"launch_for_alarms_tells_the_app_task_what_came_of_it", launch_for_alarms_tells_the_app_task_what_came_of_it},
        {"status_prints_the_record", status_prints_the_record},
        {"list_shows_installed_apps", list_shows_installed_apps},
        {"list_says_the_stores_hint", list_says_the_stores_hint},
        {"health_says_the_app_and_the_store", health_says_the_app_and_the_store},
        {"an_idle_stage_is_freed", an_idle_stage_is_freed},
        {"send_returns_the_reply", send_returns_the_reply},
        {"foreign_and_unknown_lines", foreign_and_unknown_lines},
        {"stage_out_of_memory", stage_out_of_memory},
        {"a_repeated_commit_is_busy", a_repeated_commit_is_busy},
        {"a_link_install_is_one_job", a_link_install_is_one_job},
        {"a_second_install_is_busy", a_second_install_is_busy},
        {"link_install_refusals", link_install_refusals},
        {"control_list_and_status", control_list_and_status},
        {"control_list_waits_at_most_500_ms", control_list_waits_at_most_500_ms},
        {"control_launch_stop_remove", control_launch_stop_remove},
        {"control_send", control_send},
        {"json_object_valid", json_object_valid},
        {"guide_written_or_inline", guide_written_or_inline},
        {"the_stage_is_bounded", the_stage_is_bounded},
        {"empty_and_missing_files", empty_and_missing_files},
        {"console_stack", console_stack},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    cJSON_Hooks hooks = {cjson_malloc, cjson_free};
    cJSON_InitHooks(&hooks);
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            capture_start();
            cases[i].run();
            finish();
            capture_end();
            fflush(stderr);
            _exit(0);
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
