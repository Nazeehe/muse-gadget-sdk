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
 * The launcher's icons (apps_icons.c), run on the host. Usage:
 *   muse_apps_icons_harness <case>    exits 0 when the case holds, and otherwise says why on stderr
 *   muse_apps_icons_harness --cases   lists the cases
 *
 * The main thread is the LVGL task: it alone calls apps_icons_*. The I/O worker is a thread of its own: a submit queues the job,
 * and run_jobs starts the worker, which runs every job queued (those queued meanwhile too) in order, and joins it; jobs_start and
 * jobs_join split that, for a case that acts while a job runs. The rest is fakes: the store's mode and its reader over files in
 * memory (the reader records whether the worker called it, takes its block from the apps' heap as the real one does, and stops
 * inside a read while a case keeps the gate shut), the apps' heap (a count of the blocks held, and a floor a case can close, which
 * refuses every new block) and the clock. Whatever a case leaves, the teardown drops, and every block must then be free.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps_icons.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_store.h"

static int failures;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__);                \
            fputc('\n', stderr);                         \
            failures++;                                  \
        }                                                \
    } while (0)

/* ---- The log (the worker writes it too) ---- */

#define LOG_MAX 200
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char logged[LOG_MAX][320];
static int n_logged;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    if (level == 'D') {
        return;
    }
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mu);
    if (n_logged < LOG_MAX) {
        snprintf(logged[n_logged++], sizeof logged[0], "%c %s: %s", level, tag, text);
    }
    pthread_mutex_unlock(&log_mu);
}

static int logged_count(const char *text) {
    int n = 0;
    pthread_mutex_lock(&log_mu);
    for (int i = 0; i < n_logged; i++) {
        n += strstr(logged[i], text) != NULL ? 1 : 0;
    }
    pthread_mutex_unlock(&log_mu);
    return n;
}

static void dump_log(void) {
    for (int i = 0; i < n_logged; i++) {
        fprintf(stderr, "  log %d: %s\n", i, logged[i]);
    }
}

/* ---- The apps' heap: counted, with a floor that refuses every new block while it is closed ---- */

static atomic_long blocks_held;
static atomic_int blocks_taken;  /* new blocks given, ever: what a load cost even if it freed it again */
static atomic_bool floor_closed;
static atomic_int floor_refusals;

void *apps_port_realloc(void *ptr, size_t n) {
    if (ptr == NULL && atomic_load(&floor_closed)) {
        atomic_fetch_add(&floor_refusals, 1);
        return NULL;
    }
    void *block = realloc(ptr, n);
    if (ptr == NULL && block != NULL) {
        atomic_fetch_add(&blocks_held, 1);
        atomic_fetch_add(&blocks_taken, 1);
    }
    return block;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        atomic_fetch_sub(&blocks_held, 1);
    }
    free(ptr);
}

/* ---- The clock ---- */

static atomic_llong now_ms = 100000;

int64_t apps_port_mono_ms(void) {
    return atomic_load(&now_ms);
}

/* ---- The store: its mode, and its reader over files in memory ---- */

static apps_storage_t store_mode = APPS_STORAGE_FLASH;  /* set by a case while no job runs */

apps_storage_t apps_store_mode(void) {
    return store_mode;
}

typedef struct {
    char id[APPS_ID_MAX + 1], path[APPS_PATH_MAX + 1];
    uint8_t *data;  /* malloc's: not the apps' heap */
    size_t len;
} file_t;

#define FILES_MAX 40
static file_t files[FILES_MAX];  /* changed by a case only while no job runs, or while the gate holds the worker past its copy */
static int n_files;

static _Thread_local bool on_worker;
static atomic_int reads, reads_off_worker;
static atomic_size_t read_max;
static pthread_mutex_t reads_mu = PTHREAD_MUTEX_INITIALIZER;
static char read_order[1024];  /* the ids read, in order, one space between */

/* The gate: while it is shut, a read that has its block stops until a case opens it. */
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static bool gate_shut, gate_entered;

static void gate_pass(void) {
    pthread_mutex_lock(&gate_mu);
    if (gate_shut) {
        gate_entered = true;
        pthread_cond_broadcast(&gate_cv);
        while (gate_shut) {
            pthread_cond_wait(&gate_cv, &gate_mu);
        }
    }
    pthread_mutex_unlock(&gate_mu);
}

static void gate_close(void) {
    pthread_mutex_lock(&gate_mu);
    gate_shut = true;
    gate_entered = false;
    pthread_mutex_unlock(&gate_mu);
}

/* Until a read is held at the gate: false, and the gate opened, when none comes within 10 s (no load was asked for). */
static bool gate_wait_entered(void) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    pthread_mutex_lock(&gate_mu);
    int waited = 0;
    while (!gate_entered && waited == 0) {
        waited = pthread_cond_timedwait(&gate_cv, &gate_mu, &until);
    }
    bool entered = gate_entered;
    if (!entered) {
        gate_shut = false;
    }
    pthread_mutex_unlock(&gate_mu);
    CHECK(entered, "no read came to the gate");
    return entered;
}

static void gate_open(void) {
    pthread_mutex_lock(&gate_mu);
    gate_shut = false;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
}

static const file_t *find_file(const char *id, const char *path) {
    for (int i = 0; i < n_files; i++) {
        if (strcmp(files[i].id, id) == 0 && strcmp(files[i].path, path) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

/* As apps_store.c's (apps_store.h): the file into a block of the apps' heap with a NUL after it, when it has at most max bytes. */
bool apps_store_read_file(const char *id, const char *path, size_t max, uint8_t **out, size_t *len, char *err, size_t n) {
    atomic_fetch_add(&reads, 1);
    if (!on_worker) {
        atomic_fetch_add(&reads_off_worker, 1);
    }
    atomic_store(&read_max, max);
    pthread_mutex_lock(&reads_mu);
    size_t at = strlen(read_order);
    snprintf(read_order + at, sizeof read_order - at, "%s%s", at > 0 ? " " : "", id);
    pthread_mutex_unlock(&reads_mu);
    *out = NULL;
    *len = 0;
    if (store_mode != APPS_STORAGE_FLASH) {
        snprintf(err, n, "storage not ready");
        return false;
    }
    const file_t *f = find_file(id, path);
    if (f == NULL) {
        snprintf(err, n, "No such file or directory");
        return false;
    }
    size_t size = f->len;
    if (size > max) {
        snprintf(err, n, "larger than %u KB", (unsigned)(max / 1024));
        return false;
    }
    uint8_t *block = apps_port_realloc(NULL, size + 1);
    if (block == NULL) {
        snprintf(err, n, "%s", APPS_STORE_MSG_NO_MEMORY);
        return false;
    }
    memcpy(block, f->data, size);
    block[size] = '\0';
    gate_pass();  /* from here on the file isn't looked at: a case may change it while the gate holds this read */
    *out = block;
    *len = size;
    return true;
}

/* ---- The I/O worker: a thread, which runs the jobs queued when a case starts it ---- */

#define JOBS 16
static pthread_mutex_t jobs_mu = PTHREAD_MUTEX_INITIALIZER;
static void (*job_fn[JOBS])(void *arg);
static void *job_arg[JOBS];
static int n_jobs;                   /* queued, not begun */
static int in_flight, max_in_flight; /* submitted, not ended */
static int submits;
static bool submit_refuses;
static pthread_t worker;
static bool worker_on;

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    pthread_mutex_lock(&jobs_mu);
    submits++;
    bool taken = !submit_refuses && n_jobs < JOBS;
    if (taken) {
        job_fn[n_jobs] = fn;
        job_arg[n_jobs] = arg;
        n_jobs++;
        in_flight++;
        max_in_flight = in_flight > max_in_flight ? in_flight : max_in_flight;
    }
    pthread_mutex_unlock(&jobs_mu);
    return taken;
}

static void *worker_main(void *arg) {
    (void)arg;
    on_worker = true;
    for (;;) {
        pthread_mutex_lock(&jobs_mu);
        if (n_jobs == 0) {
            pthread_mutex_unlock(&jobs_mu);
            return NULL;
        }
        void (*fn)(void *) = job_fn[0];
        void *a = job_arg[0];
        memmove(&job_fn[0], &job_fn[1], (size_t)(n_jobs - 1) * sizeof job_fn[0]);
        memmove(&job_arg[0], &job_arg[1], (size_t)(n_jobs - 1) * sizeof job_arg[0]);
        n_jobs--;
        pthread_mutex_unlock(&jobs_mu);
        fn(a);
        pthread_mutex_lock(&jobs_mu);
        in_flight--;
        pthread_mutex_unlock(&jobs_mu);
    }
}

static void jobs_start(void) {
    CHECK(!worker_on, "the worker is started twice");
    CHECK(pthread_create(&worker, NULL, worker_main, NULL) == 0, "no worker thread");
    worker_on = true;
}

static void jobs_join(void) {
    if (worker_on) {
        pthread_join(worker, NULL);
        worker_on = false;
    }
}

static void run_jobs(void) {
    jobs_start();
    jobs_join();
}

static int jobs_waiting(void) {
    pthread_mutex_lock(&jobs_mu);
    int n = n_jobs;
    pthread_mutex_unlock(&jobs_mu);
    return n;
}

/* Frames until the worker has nothing left: each runs the jobs queued, then polls (which adopts what they made and asks for the
 * next). Whether any poll said an icon came. */
static bool settle(void) {
    bool came = false;
    for (int i = 0; i < 32; i++) {
        run_jobs();
        came = apps_icons_poll() || came;
        if (jobs_waiting() == 0) {
            break;
        }
    }
    return came;
}

/* ---- Images and windows ---- */

#define CF_A8 0x0E
#define CF_RGB565 0x12
#define CF_RGB565A8 0x14
#define FLAG_PREMULTIPLIED 0x0001
#define FLAG_COMPRESSED 0x0008

static void put16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

/* An LVGL v9 .bin of w x h in cf, every byte after the header `fill`; *len its size. malloc'd. */
static uint8_t *image(unsigned cf, unsigned w, unsigned h, unsigned flags, uint8_t fill, size_t *len) {
    unsigned stride = cf == CF_A8 ? w : w * 2;
    size_t n = 12 + (size_t)stride * h + (cf == CF_RGB565A8 ? (size_t)(stride / 2) * h : 0);
    uint8_t *b = malloc(n);
    memset(b, fill, n);
    b[0] = 0x19;
    b[1] = (uint8_t)cf;
    put16(b + 2, flags);
    put16(b + 4, w);
    put16(b + 6, h);
    put16(b + 8, stride);
    put16(b + 10, 0);
    *len = n;
    return b;
}

/* data (malloc'd) becomes id's file at path, in place of one there. */
static void put_file(const char *id, const char *path, uint8_t *data, size_t len) {
    file_t *f = (file_t *)find_file(id, path);
    if (f == NULL) {
        CHECK(n_files < FILES_MAX, "the harness has room for %d files", FILES_MAX);
        if (n_files == FILES_MAX) {
            free(data);
            return;
        }
        f = &files[n_files++];
        snprintf(f->id, sizeof f->id, "%s", id);
        snprintf(f->path, sizeof f->path, "%s", path);
    } else {
        free(f->data);
    }
    f->data = data;
    f->len = len;
}

/* A good icon for id: px x px RGB565A8, premultiplied, at icon.bin. */
static void put_icon(const char *id, unsigned px, uint8_t fill) {
    size_t len;
    uint8_t *b = image(CF_RGB565A8, px, px, FLAG_PREMULTIPLIED, fill, &len);
    put_file(id, "icon.bin", b, len);
}

static void free_files(void) {
    for (int i = 0; i < n_files; i++) {
        free(files[i].data);
    }
    n_files = 0;
}

/* Whether id shows the icon put_icon(id, px, fill) made: the descriptor is the file's, as apps_lv.c makes an image's. */
static bool shows(const char *id, unsigned px, uint8_t fill) {
    const lv_image_dsc_t *d = apps_icons_get(id);
    const file_t *f = find_file(id, "icon.bin");
    if (d == NULL || f == NULL) {
        return false;
    }
    bool ok = d->header.magic == 0x19 && d->header.cf == CF_RGB565A8 && d->header.flags == FLAG_PREMULTIPLIED &&
              d->header.w == px && d->header.h == px && d->header.stride == px * 2 && d->data_size == f->len - 12 &&
              d->data != NULL && d->reserved == NULL && d->reserved_2 == NULL;
    if (ok) {
        ok = d->data[0] == fill && d->data[d->data_size - 1] == fill && memcmp(d->data - 12, f->data, 12) == 0;
    }
    if (!ok) {
        fprintf(stderr, "  %s: %ux%u cf 0x%02x flags 0x%x, %u bytes, first 0x%02x (want %ux%u, fill 0x%02x)\n", id,
                (unsigned)d->header.w, (unsigned)d->header.h, (unsigned)d->header.cf, (unsigned)d->header.flags,
                (unsigned)d->data_size, d->data != NULL ? d->data[0] : 0, px, px, fill);
    }
    return ok;
}

static const char *const ICON[] = {"icon.bin", "icon.bin", "icon.bin", "icon.bin", "icon.bin", "icon.bin"};

static const char *const APPS[] = {"aa", "bb", "cc", "dd", "ee", "ff", "gg", "hh", "ii", "jj", "kk", "ll"};
#define N_APPS 12

/* The window around APPS[c] of the first n apps, centre first, then its neighbours, nearest first and left before right. */
static int window_of(int c, int n, const char *ids[APPS_ICONS_WINDOW]) {
    int k = 0;
    ids[k++] = APPS[c];
    for (int d = 1; d <= APPS_ICONS_WINDOW / 2; d++) {
        if (c - d >= 0) {
            ids[k++] = APPS[c - d];
        }
        if (c + d < n) {
            ids[k++] = APPS[c + d];
        }
    }
    return k;
}

static void want_centre(int c, int n) {
    const char *ids[APPS_ICONS_WINDOW];
    int k = window_of(c, n, ids);
    apps_icons_want(ids, ICON, k);
}

static void want_one(const char *id) {
    const char *ids[] = {id};
    apps_icons_want(ids, ICON, 1);
}

static bool in_window(const char *id, int c, int n) {
    const char *ids[APPS_ICONS_WINDOW];
    int k = window_of(c, n, ids);
    for (int i = 0; i < k; i++) {
        if (strcmp(ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- The cases: the brief's ---- */

/* The window loads centre first, at most two at a time, and a scroll frees at once the icons that left it. */
static void window_loads_and_frees(void) {
    for (int i = 0; i < 7; i++) {
        put_icon(APPS[i], 8, (uint8_t)(i + 1));
    }
    const char *win[] = {"cc", "bb", "dd", "aa", "ee"};
    apps_icons_want(win, ICON, 5);
    CHECK(jobs_waiting() == APPS_ICONS_IN_FLIGHT, "two loads at once, not %d", jobs_waiting());
    CHECK(apps_icons_get("cc") == NULL, "nothing yet: tiles");
    run_jobs();
    CHECK(strcmp(read_order, "cc bb") == 0, "the centre first, then its neighbour: '%s'", read_order);
    CHECK(apps_icons_poll(), "two came");
    CHECK(shows("cc", 8, 3) && shows("bb", 8, 2), "the centre and its neighbour show");
    CHECK(apps_icons_get("dd") == NULL, "the next isn't loaded yet");
    CHECK(jobs_waiting() == 2, "the poll asked for the next two: %d", jobs_waiting());
    settle();
    CHECK(strcmp(read_order, "cc bb dd aa ee") == 0, "in the window's order: '%s'", read_order);
    CHECK(shows("cc", 8, 3) && shows("bb", 8, 2) && shows("dd", 8, 4) && shows("aa", 8, 1) && shows("ee", 8, 5), "all five show");
    CHECK(atomic_load(&blocks_held) == 5, "five blocks: %ld", atomic_load(&blocks_held));
    CHECK(max_in_flight <= APPS_ICONS_IN_FLIGHT, "never more than two in flight: %d", max_in_flight);

    /* Two to the right: ee is the centre; aa and bb leave and are freed at once, cc dd ee stay as they are. */
    const lv_image_dsc_t *dd_before = apps_icons_get("dd");
    const char *right[] = {"ee", "dd", "ff", "cc", "gg"};
    apps_icons_want(right, ICON, 5);
    CHECK(apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL, "the two that left are tiles");
    CHECK(atomic_load(&blocks_held) == 3, "and their blocks are freed at once: %ld held", atomic_load(&blocks_held));
    CHECK(apps_icons_get("dd") == dd_before && shows("dd", 8, 4), "what stays isn't loaded again");
    settle();
    CHECK(atomic_load(&reads) == 7, "only the two that came were read: %d reads", atomic_load(&reads));
    CHECK(shows("ff", 8, 6) && shows("gg", 8, 7), "the two that came show");
    CHECK(atomic_load(&blocks_held) == 5, "five blocks: %ld", atomic_load(&blocks_held));

    /* An app without an icon is a tile, and asks nothing of the worker. */
    const char *plain[] = {"hh"}, *none[] = {""};
    apps_icons_want(plain, none, 1);
    settle();
    CHECK(apps_icons_get("hh") == NULL && atomic_load(&reads) == 7, "no read for an app with no icon");
    CHECK(atomic_load(&blocks_held) == 0, "every icon left the window: %ld held", atomic_load(&blocks_held));
    apps_icons_want(NULL, NULL, 0);
    CHECK(!apps_icons_poll() && jobs_waiting() == 0, "an empty window asks nothing");
}

/* An icon is a tile until a poll on the LVGL task adopts it: the worker's end doesn't change what the LVGL task shows. */
static void get_is_null_until_loaded(void) {
    put_icon("aa", 16, 0x5a);
    want_one("aa");
    CHECK(apps_icons_get("aa") == NULL, "asked for, not loaded: a tile");
    CHECK(!apps_icons_poll(), "nothing came before the worker ran");
    CHECK(apps_icons_get("aa") == NULL, "still a tile");
    run_jobs();
    CHECK(atomic_load(&read_max) == APPS_ICON_MAX_BYTES, "the read is capped at an icon's most: %zu", atomic_load(&read_max));
    CHECK(apps_icons_get("aa") == NULL, "loaded, but a tile until a poll adopts it");
    CHECK(apps_icons_poll(), "it came");
    CHECK(shows("aa", 16, 0x5a), "the descriptor is the file's");
    CHECK(!apps_icons_poll(), "and came once");
    want_one("aa");
    settle();
    CHECK(atomic_load(&reads) == 1, "the same window again reads nothing: %d reads", atomic_load(&reads));
    CHECK(apps_icons_get("zz") == NULL && apps_icons_get(NULL) == NULL, "no other id has one");
}

/* Review Focus 3: the floor refuses the block, so the tile stays; nothing leaks, and nothing goes round the floor. A refusal is
 * a shortage that passes (a snapshot holds memory, an install's read): while the icon stays in the window it is asked for again
 * APPS_ICONS_RETRY_MS later, not at every frame, and shows once the floor lets it. */
static void refused_load_keeps_tile(void) {
    put_icon("aa", 8, 1);
    put_icon("bb", 8, 2);
    atomic_store(&floor_closed, true);
    const char *win[] = {"aa", "bb"};
    apps_icons_want(win, ICON, 2);
    CHECK(!settle(), "nothing came");
    CHECK(atomic_load(&floor_refusals) == 2, "each block was asked of the floor: %d", atomic_load(&floor_refusals));
    CHECK(apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL, "tiles");
    CHECK(atomic_load(&blocks_held) == 0, "nothing held: %ld", atomic_load(&blocks_held));
    CHECK(logged_count("W muse_apps: icons: aa: icon.bin: out of memory; its tile shows") == 1 &&
              logged_count("W muse_apps: icons: bb: icon.bin: out of memory; its tile shows") == 1,
          "one W line each");
    /* Not asked again before the wait, however many frames and wants come. */
    int before = atomic_load(&reads);
    for (int i = 0; i < 10; i++) {
        apps_icons_want(win, ICON, 2);
        settle();
    }
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS - 1);
    settle();
    CHECK(atomic_load(&reads) == before, "not asked again before the wait: %d more", atomic_load(&reads) - before);
    /* After it, asked again while it stays in the window: still refused, still tiles, nothing held. */
    atomic_fetch_add(&now_ms, 1);
    settle();
    CHECK(atomic_load(&reads) == before + 2, "both asked again after the wait: %d more", atomic_load(&reads) - before);
    CHECK(apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL && atomic_load(&blocks_held) == 0, "tiles, nothing held");
    CHECK(atomic_load(&floor_refusals) == 4, "the floor was asked again: %d", atomic_load(&floor_refusals));
    /* The floor opens: within the window, without a scroll, both show after the next wait. */
    atomic_store(&floor_closed, false);
    apps_icons_want(win, ICON, 2);
    settle();
    CHECK(apps_icons_get("aa") == NULL, "not before the wait");
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);
    CHECK(settle(), "they came");
    CHECK(shows("aa", 8, 1) && shows("bb", 8, 2), "both show, never having left the window");
    CHECK(atomic_load(&blocks_held) == 2, "two blocks: %ld", atomic_load(&blocks_held));
}

/* Review Focus 2: a drop_all (the list changed, the launcher closed, a voice turn) while a load is in flight. Its result is freed
 * when it ends, on the worker, and is never shown; a load asked for after the drop is the one that shows. */
static void list_change_drops_stale_icons(void) {
    put_icon("aa", 8, 0x11);
    put_icon("bb", 8, 0x12);

    /* Queued, not begun. */
    want_one("aa");
    CHECK(jobs_waiting() == 1, "aa is asked for");
    apps_icons_drop_all();
    run_jobs();
    CHECK(atomic_load(&reads) == 0, "the job read nothing, orphaned before it began (an_orphaned_load_reads_nothing): %d",
          atomic_load(&reads));
    CHECK(atomic_load(&blocks_held) == 0, "nothing held, with no poll: %ld", atomic_load(&blocks_held));
    CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL, "and it never shows");
    CHECK(jobs_waiting() == 0, "the drop forgot the window: nothing is asked for again");

    /* In the middle of its read: the worker has its block when the LVGL task drops. */
    want_one("aa");
    gate_close();
    jobs_start();
    gate_wait_entered();
    CHECK(atomic_load(&blocks_held) == 1, "the read has its block");
    apps_icons_drop_all();
    CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL, "a tile while it reads");
    gate_open();
    jobs_join();
    CHECK(atomic_load(&blocks_held) == 0, "freed as it ended: %ld", atomic_load(&blocks_held));
    CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL, "never shown");

    /* Ended, not yet adopted: the drop frees it itself. */
    want_one("aa");
    run_jobs();
    CHECK(atomic_load(&blocks_held) == 1, "loaded, waiting for a poll");
    apps_icons_drop_all();
    CHECK(atomic_load(&blocks_held) == 0, "the drop freed it: %ld", atomic_load(&blocks_held));
    CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL, "never shown");

    /* Shown: the drop frees them, and nothing is loaded again until the next want. */
    const char *both[] = {"aa", "bb"};
    apps_icons_want(both, ICON, 2);
    settle();
    CHECK(shows("aa", 8, 0x11) && shows("bb", 8, 0x12), "both show");
    int before = atomic_load(&reads);
    apps_icons_drop_all();
    CHECK(atomic_load(&blocks_held) == 0 && apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL, "both freed");
    settle();
    CHECK(atomic_load(&reads) == before, "nothing is read again");

    /* The list changed under a load: aa was reinstalled with a new icon while the old one was being read. The old result is freed;
     * the want after the drop asks again, and that is the icon that shows. */
    want_one("aa");
    gate_close();
    jobs_start();
    gate_wait_entered();  /* the old file is copied */
    apps_icons_drop_all();
    put_icon("aa", 12, 0x22);  /* the reinstall */
    want_one("aa");
    CHECK(jobs_waiting() == 1, "the new want asks again: the load in flight is the old list's");
    gate_open();
    jobs_join();  /* the old load, then the new one */
    CHECK(apps_icons_poll(), "the new one came");
    CHECK(shows("aa", 12, 0x22), "the reinstall's icon shows, never the old one");
    CHECK(atomic_load(&blocks_held) == 1, "the old block is gone: %ld held", atomic_load(&blocks_held));

    /* Two drops while one load is in flight (a list change, then the launcher's close), then a want before it ends: the orphan
     * stays its job's, and the new load gets a slot of its own. */
    apps_icons_drop_all();
    want_one("aa");
    gate_close();
    jobs_start();
    gate_wait_entered();
    apps_icons_drop_all();
    apps_icons_drop_all();
    put_icon("aa", 16, 0x33);
    want_one("aa");
    gate_open();
    jobs_join();
    CHECK(apps_icons_poll(), "the new one came");
    CHECK(shows("aa", 16, 0x33), "the last want's icon shows");
    CHECK(atomic_load(&blocks_held) == 1, "the orphan's block is gone: %ld held", atomic_load(&blocks_held));
}

/* A file that isn't an icon (not square, past 160 px, past the bytes an icon may have, not an image, cut short, compressed,
 * missing) is a tile, with one W line, and isn't read again while it stays in the window; the others in the window show. */
/* Task 6a's fix round 1 (I1): apps_icons_keep, the launcher's hold for a launch. It keeps the ready icons it is given (the same
 * descriptors) and frees everything else at once: a ready icon it isn't given, a load that ended and wasn't adopted, a failed one; a load
 * in flight, queued or in the middle of its read, frees its block on the worker as it ends, never shown. An id given that isn't ready is
 * no exception. After it nothing is asked for, however many polls come. It starts a generation, as a drop does: an app wanted again
 * while its orphaned load still runs gets a load of its own, and its icon, not a tile. */
/* Final review, recommendation 3: a load orphaned before its job began (a drop, or the launcher's hold, while it waited in the worker's
 * queue) reads nothing and takes no block. Its job hands the slot back (DONE) at once, so the next want finds it free; nothing is said
 * of it. A pick's launch job, queued behind it, then waits for no read. */
static void an_orphaned_load_reads_nothing(void) {
    put_icon("aa", 8, 0x11);
    put_icon("bb", 8, 0x12);
    put_icon("cc", 8, 0x13);

    /* A drop: both loads queued, not begun. */
    const char *two[] = {"aa", "bb"};
    apps_icons_want(two, ICON, 2);
    CHECK(jobs_waiting() == 2, "aa and bb are asked for: %d", jobs_waiting());
    int reads0 = atomic_load(&reads), taken0 = atomic_load(&blocks_taken), said0 = logged_count("icons: ");
    apps_icons_drop_all();
    run_jobs();
    CHECK(atomic_load(&reads) == reads0, "an orphaned load reads nothing: %d reads", atomic_load(&reads) - reads0);
    CHECK(atomic_load(&blocks_taken) == taken0 && atomic_load(&blocks_held) == 0, "and takes no block: %d taken, %ld held",
          atomic_load(&blocks_taken) - taken0, atomic_load(&blocks_held));
    CHECK(logged_count("icons: ") == said0, "and says nothing");
    CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL, "never shown");
    /* Their slots came back: a want asks again, and the icons come. */
    apps_icons_want(two, ICON, 2);
    CHECK(jobs_waiting() == 2, "asked for again: %d", jobs_waiting());
    settle();
    CHECK(shows("aa", 8, 0x11) && shows("bb", 8, 0x12), "both show");

    /* The hold: aa and bb kept, cc's load queued and orphaned by the keep. */
    const char *three[] = {"aa", "bb", "cc"};
    apps_icons_want(three, ICON, 3);
    CHECK(jobs_waiting() == 1, "cc is asked for: %d", jobs_waiting());
    reads0 = atomic_load(&reads);
    taken0 = atomic_load(&blocks_taken);
    apps_icons_keep(two, 2);
    run_jobs();
    CHECK(atomic_load(&reads) == reads0 && atomic_load(&blocks_taken) == taken0, "cc's orphaned load reads nothing: %d reads, %d blocks",
          atomic_load(&reads) - reads0, atomic_load(&blocks_taken) - taken0);
    CHECK(atomic_load(&blocks_held) == 2 && shows("aa", 8, 0x11) && shows("bb", 8, 0x12), "the kept two stay: %ld held",
          atomic_load(&blocks_held));
    CHECK(!apps_icons_poll() && apps_icons_get("cc") == NULL, "cc never shows");

    /* A load already reading when it is orphaned still frees its own block (the hand-off's other half). */
    apps_icons_want(three, ICON, 3);
    gate_close();
    jobs_start();
    gate_wait_entered();
    apps_icons_drop_all();
    gate_open();
    jobs_join();
    CHECK(atomic_load(&blocks_held) == 0, "a read under way frees its block as it ends: %ld held", atomic_load(&blocks_held));
}

static void keep_frees_all_but_the_kept(void) {
    for (int i = 0; i < 6; i++) {
        put_icon(APPS[i], 8, (uint8_t)(0x30 + i));
    }
    const char *four[] = {"aa", "bb", "cc", "dd"};
    apps_icons_want(four, ICON, 4);
    settle();
    CHECK(shows("aa", 8, 0x30) && shows("bb", 8, 0x31) && shows("cc", 8, 0x32) && shows("dd", 8, 0x33), "four show");
    const lv_image_dsc_t *a = apps_icons_get("aa"), *b = apps_icons_get("bb");

    /* ee in the middle of its read, ff queued behind it; dd left the window. */
    const char *with_two[] = {"aa", "bb", "cc", "ee", "ff"};
    apps_icons_want(with_two, ICON, 5);
    CHECK(jobs_waiting() == 2, "ee and ff are asked for: %d", jobs_waiting());
    gate_close();
    jobs_start();
    gate_wait_entered();
    CHECK(atomic_load(&blocks_held) == 4, "aa, bb, cc, and ee's read: %ld", atomic_load(&blocks_held));
    const char *keep[] = {"aa", "bb", "ee"};
    apps_icons_keep(keep, 3);
    CHECK(atomic_load(&blocks_held) == 3, "cc freed at once: %ld held", atomic_load(&blocks_held));
    CHECK(apps_icons_get("aa") == a && apps_icons_get("bb") == b && shows("aa", 8, 0x30) && shows("bb", 8, 0x31),
          "aa and bb keep their descriptors");
    CHECK(apps_icons_get("cc") == NULL && apps_icons_get("ee") == NULL, "cc is a tile, and ee (not ready) too");
    gate_open();
    jobs_join();
    CHECK(atomic_load(&blocks_held) == 2, "ee's and ff's loads freed their blocks on the worker: %ld held", atomic_load(&blocks_held));
    int before = atomic_load(&reads);
    for (int frame = 0; frame < 5; frame++) {
        run_jobs();
        CHECK(!apps_icons_poll(), "nothing comes");
    }
    CHECK(atomic_load(&reads) == before && jobs_waiting() == 0 && atomic_load(&blocks_held) == 2,
          "nothing asked for after it: %d reads, %d waiting, %ld held", atomic_load(&reads) - before, jobs_waiting(),
          atomic_load(&blocks_held));
    CHECK(apps_icons_get("ee") == NULL && apps_icons_get("ff") == NULL, "never shown");

    /* A load that ended, not adopted, and a failed one: freed too. gg has no file. */
    const char *three[] = {"aa", "bb", "cc", "gg"};
    apps_icons_want(three, ICON, 4);
    run_jobs();
    CHECK(atomic_load(&blocks_held) == 3, "cc's load ended, waiting for a poll: %ld held", atomic_load(&blocks_held));
    apps_icons_keep(keep, 2);
    CHECK(atomic_load(&blocks_held) == 2, "the ended load freed: %ld held", atomic_load(&blocks_held));
    const char *with_gg[] = {"aa", "bb", "gg"};
    apps_icons_want(with_gg, ICON, 3);
    settle();
    CHECK(apps_icons_get("gg") == NULL && logged_count("icons: gg: icon.bin: ") >= 1, "gg failed: a tile");
    apps_icons_keep(keep, 2);
    apps_icons_want(with_gg, ICON, 3);
    CHECK(jobs_waiting() == 1, "the failed one was forgotten: asked for again (%d)", jobs_waiting());
    settle();

    /* A want while an orphaned load of the same app still runs: a load of its own, and its icon. */
    const char *with_ee[] = {"aa", "bb", "ee"};
    apps_icons_want(with_ee, ICON, 3);
    gate_close();
    jobs_start();
    gate_wait_entered();  /* ee's read */
    apps_icons_keep(keep, 2);
    apps_icons_want(with_ee, ICON, 3);
    gate_open();
    jobs_join();
    settle();
    CHECK(shows("ee", 8, 0x34), "ee wanted again shows its icon");
    apps_icons_drop_all();
    run_jobs();
    CHECK(atomic_load(&blocks_held) == 0, "the drop frees the rest: %ld held", atomic_load(&blocks_held));
}

static void bad_icon_file_is_a_tile(void) {
    size_t len;
    uint8_t *b = image(CF_RGB565A8, 8, 4, 0, 1, &len);
    put_file("aa", "icon.bin", b, len);  /* not square */
    b = image(CF_RGB565, 161, 161, 0, 2, &len);
    put_file("bb", "icon.bin", b, len);  /* square, 161 px: under the bytes' cap, past the pixels' */
    b = image(CF_RGB565A8, 161, 161, 0, 3, &len);
    CHECK(len > APPS_ICON_MAX_BYTES, "the harness's big file is past the cap");
    put_file("cc", "icon.bin", b, len);  /* past APPS_ICON_MAX_BYTES: refused before any block */
    b = calloc(1, 100);
    put_file("dd", "icon.bin", b, 100);  /* not an image */
    b = image(CF_RGB565A8, 8, 8, 0, 5, &len);
    put_file("ee", "icon.bin", b, len - 1);  /* cut short */
    b = image(CF_RGB565A8, 8, 8, FLAG_COMPRESSED, 6, &len);
    put_file("ff", "icon.bin", b, len);  /* compressed */
    put_icon("hh", 8, 8);                /* a good one beside them */
    /* gg has none: missing */

    const char *first[] = {"aa", "bb", "cc", "dd", "hh"};
    apps_icons_want(first, ICON, 5);
    settle();
    CHECK(shows("hh", 8, 8), "the good one shows");
    for (int i = 0; i < 4; i++) {
        CHECK(apps_icons_get(first[i]) == NULL, "%s is a tile", first[i]);
    }
    const char *second[] = {"ee", "ff", "gg"};
    apps_icons_want(second, ICON, 3);
    settle();
    for (int i = 0; i < 3; i++) {
        CHECK(apps_icons_get(second[i]) == NULL, "%s is a tile", second[i]);
    }
    CHECK(atomic_load(&blocks_held) == 0, "no block is kept for a bad file: %ld", atomic_load(&blocks_held));
    CHECK(atomic_load(&floor_refusals) == 0, "the floor was never closed");
    CHECK(logged_count("icons: aa: icon.bin: icon: 8x4 must be square, at most 160x160") == 1, "aa's line");
    CHECK(logged_count("icons: bb: icon.bin: icon: 161x161 must be square, at most 160x160") == 1, "bb's line");
    CHECK(logged_count("icons: cc: icon.bin: larger than 75 KB") == 1, "cc's line");
    CHECK(logged_count("icons: dd: icon.bin: image: not an LVGL v9 image file") == 1, "dd's line");
    CHECK(logged_count("icons: ee: icon.bin: image: size") == 1, "ee's line");
    CHECK(logged_count("icons: ff: icon.bin: image: compressed images aren't supported") == 1, "ff's line");
    CHECK(logged_count("icons: gg: icon.bin: No such file or directory; its tile shows") == 1, "gg's line");
    int before = atomic_load(&reads);
    for (int i = 0; i < 5; i++) {
        atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);  /* unlike a refusal by the floor, waiting changes nothing */
        apps_icons_want(second, ICON, 3);
        settle();
    }
    CHECK(atomic_load(&reads) == before, "a bad file isn't read again while it stays in the window, however long");
}

/* X11: the LVGL task never touches flash. Every read of the store, through every path (loads, scrolls, drops, refusals, a
 * gate), is the worker thread's; the teardown checks the same of every case. */
static void no_flash_on_the_lvgl_task(void) {
    for (int i = 0; i < N_APPS; i++) {
        if (i % 4 != 3) {
            put_icon(APPS[i], 8, (uint8_t)(i + 1));
        }
    }
    for (int c = 0; c < N_APPS; c++) {
        want_centre(c, N_APPS);
        (void)apps_icons_get(APPS[c]);
        (void)apps_icons_poll();
        run_jobs();
    }
    apps_icons_drop_all();
    atomic_store(&floor_closed, true);
    want_centre(5, N_APPS);
    settle();
    atomic_store(&floor_closed, false);
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);  /* the shortage's wait */
    apps_icons_drop_all();
    want_centre(2, N_APPS);
    gate_close();
    jobs_start();
    gate_wait_entered();
    apps_icons_drop_all();
    want_centre(9, N_APPS);
    (void)apps_icons_poll();
    gate_open();
    jobs_join();
    settle();
    CHECK(atomic_load(&reads) > N_APPS, "the store was read: %d times", atomic_load(&reads));
    CHECK(atomic_load(&reads_off_worker) == 0, "every read was the worker's: %d were the LVGL task's",
          atomic_load(&reads_off_worker));
}

/* A drop that races the worker, wherever it lands in the load (before the read, inside it, after the check, as the result is
 * posted): the block is freed by whichever side loses, so once the worker has ended nothing is held, with no poll after the drop
 * (the launcher has closed: nothing polls). Many rounds, each a fresh race. */
static void a_drop_racing_the_worker_frees_every_block(void) {
    put_icon("aa", 8, 1);
    for (int round = 0; round < 3000 && failures == 0; round++) {
        want_one("aa");
        jobs_start();
        for (int spin = 0; spin < round % 64; spin++) {
            atomic_signal_fence(memory_order_seq_cst);  /* a little later each round, across the job's whole run */
        }
        apps_icons_drop_all();
        jobs_join();
        CHECK(atomic_load(&blocks_held) == 0, "round %d: %ld blocks held after the drop, with no poll", round,
              atomic_load(&blocks_held));
        CHECK(!apps_icons_poll() && apps_icons_get("aa") == NULL, "round %d: never shown", round);
    }
}

/* ---- The cases: past the brief ---- */

/* The worker refused the job (its queue full): the slot isn't lost, and the load is asked for again APPS_ICONS_RETRY_MS later, not
 * at every frame (each refusal is a W line on the console, which the LVGL task would wait for). */
static void a_refused_submit_is_tried_again_later(void) {
    put_icon("aa", 8, 1);
    submit_refuses = true;
    want_one("aa");
    CHECK(submits == 1 && jobs_waiting() == 0, "asked once, refused: %d", submits);
    for (int i = 0; i < 5; i++) {
        want_one("aa");
        (void)apps_icons_poll();
    }
    CHECK(submits == 1, "not asked again at once: %d", submits);
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS - 1);
    (void)apps_icons_poll();
    CHECK(submits == 1, "not before the wait: %d", submits);
    atomic_fetch_add(&now_ms, 1);
    (void)apps_icons_poll();
    CHECK(submits == 2, "asked again after it: %d", submits);
    submit_refuses = false;
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);
    CHECK(!apps_icons_poll() && submits == 3 && jobs_waiting() == 1, "taken: %d", submits);
    CHECK(settle(), "it came");
    CHECK(shows("aa", 8, 1), "it shows");
    /* Refusals lose no slot: a full window still loads every icon. */
    for (int i = 0; i < 5; i++) {
        put_icon(APPS[i + 1], 8, (uint8_t)(i + 2));
    }
    submit_refuses = true;
    for (int i = 0; i < 20; i++) {
        atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);
        want_centre(3, 6);
        (void)apps_icons_poll();
    }
    submit_refuses = false;
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);
    settle();
    CHECK(shows("dd", 8, 4) && shows("cc", 8, 3) && shows("ee", 8, 5) && shows("bb", 8, 2) && shows("ff", 8, 6),
          "every icon of the window");
}

/* A fling: windows pass faster than the worker reads. The loads in flight (at most two) finish, and those no longer wanted are
 * freed; nothing else is queued until they have, so only the window the fling ends on is loaded after them. */
static void a_fling_keeps_the_worker_to_two_loads(void) {
    for (int i = 0; i < N_APPS; i++) {
        put_icon(APPS[i], 8, (uint8_t)(i + 1));
    }
    for (int c = 0; c < N_APPS; c++) {
        want_centre(c, N_APPS);
    }
    CHECK(jobs_waiting() == APPS_ICONS_IN_FLIGHT, "the first window's two, and nothing more: %d", jobs_waiting());
    settle();
    CHECK(max_in_flight <= APPS_ICONS_IN_FLIGHT, "at most two in flight: %d", max_in_flight);
    CHECK(strcmp(read_order, "aa bb ll kk jj") == 0, "the first two, then the last window: '%s'", read_order);
    CHECK(shows("ll", 8, 12) && shows("kk", 8, 11) && shows("jj", 8, 10), "the window the fling ended on");
    CHECK(apps_icons_get("aa") == NULL && apps_icons_get("bb") == NULL, "the passed ones are tiles");
    CHECK(atomic_load(&blocks_held) == 3, "three blocks: %ld", atomic_load(&blocks_held));
}

/* RAM mode (no apps partition) and before the mount, there is no FAT: every app is its tile, and the worker is asked nothing. */
static void ram_mode_is_a_tile(void) {
    put_icon("aa", 8, 1);
    store_mode = APPS_STORAGE_RAM;
    want_one("aa");
    settle();
    CHECK(submits == 0 && atomic_load(&reads) == 0 && apps_icons_get("aa") == NULL, "RAM: a tile, nothing asked");
    store_mode = APPS_STORAGE_NONE;
    want_one("aa");
    settle();
    CHECK(submits == 0 && atomic_load(&reads) == 0 && apps_icons_get("aa") == NULL, "before the mount: a tile, nothing asked");
    store_mode = APPS_STORAGE_FLASH;
    want_one("aa");
    settle();
    CHECK(shows("aa", 8, 1), "on flash it loads");
}

/* What a caller might pass: NULL ids and icons, a duplicate, ids and paths that aren't valid, more than a window, an app with no
 * icon. Only what is valid is asked for, once. */
static void odd_windows(void) {
    put_icon("aa", 8, 1);
    put_icon("bb", 8, 2);
    put_icon("cc", 8, 3);
    const char *ids[] = {NULL, "aa", "aa", "Bad", "bb", "cc"};
    const char *icons[] = {"icon.bin", "icon.bin", "other.bin", "icon.bin", "../x.bin", "icon.bin"};
    apps_icons_want(ids, icons, 6);
    settle();
    CHECK(strcmp(read_order, "aa") == 0, "aa alone: the rest are NULL, a duplicate, not valid or past the window: '%s'", read_order);
    CHECK(shows("aa", 8, 1) && apps_icons_get("bb") == NULL && apps_icons_get("cc") == NULL, "aa shows");
    apps_icons_want(ids, NULL, 3);
    CHECK(apps_icons_get("aa") == NULL && atomic_load(&blocks_held) == 0, "no icons: nothing wanted");
    apps_icons_want(NULL, icons, 3);
    apps_icons_want(ids, icons, -1);
    const char *long_id[] = {"abcdefghijklmnopqrstuvwxyz"};
    const char *long_path[] = {"a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z/z/z/z/z/icon.bin"};
    apps_icons_want(long_id, ICON, 1);
    const char *bb[] = {"bb"};
    apps_icons_want(bb, long_path, 1);
    const char *empty[] = {""};
    apps_icons_want(bb, empty, 1);
    settle();
    CHECK(strcmp(read_order, "aa") == 0, "nothing else was read: '%s'", read_order);
    CHECK(atomic_load(&blocks_held) == 0, "nothing held");
}

/* A long run of calls in a pseudo-random order, the floor and the worker's queue refusing now and then: the loads in flight and
 * the blocks held stay bounded, an icon shown is always its own app's and in the window, and a last window loads in full. */
static uint32_t lcg = 12345;
static uint32_t next_rand(void) {
    lcg = lcg * 1103515245u + 12345u;
    return lcg >> 8;
}

static void any_order_of_calls_keeps_the_bounds(void) {
    for (int i = 0; i < N_APPS; i++) {
        if (i % 5 != 4) {
            put_icon(APPS[i], 8, (uint8_t)(i + 1));
        }
    }
    size_t len;
    uint8_t *bad = image(CF_RGB565A8, 8, 6, 0, 4, &len);
    put_file("dd", "icon.bin", bad, len);  /* a bad one: not square */
    int c = 0;
    for (int step = 0; step < 4000; step++) {
        uint32_t r = next_rand();
        switch (r % 12) {
            case 0: case 1: case 2: case 3:
                c = (int)(next_rand() % N_APPS);
                want_centre(c, N_APPS);
                break;
            case 4: case 5:
                (void)apps_icons_poll();
                break;
            case 6: case 7: case 8:
                run_jobs();
                break;
            case 9:
                apps_icons_drop_all();
                c = -1;
                break;
            case 10:
                atomic_store(&floor_closed, next_rand() % 4 == 0);
                break;
            default:
                submit_refuses = next_rand() % 4 == 0;
                atomic_fetch_add(&now_ms, 400);
                break;
        }
        CHECK(atomic_load(&blocks_held) <= APPS_ICONS_SLOTS, "step %d: %ld blocks held", step, atomic_load(&blocks_held));
        CHECK(in_flight <= APPS_ICONS_IN_FLIGHT, "step %d: %d loads in flight", step, in_flight);
        for (int i = 0; i < N_APPS; i++) {
            const lv_image_dsc_t *d = apps_icons_get(APPS[i]);
            if (d != NULL) {
                CHECK(c >= 0 && in_window(APPS[i], c, N_APPS), "step %d: %s shows outside the window", step, APPS[i]);
                CHECK(d->data[0] == (uint8_t)(i + 1), "step %d: %s shows another app's icon (0x%02x)", step, APPS[i], d->data[0]);
            }
        }
        if (failures > 0) {
            return;
        }
    }
    atomic_store(&floor_closed, false);
    submit_refuses = false;
    apps_icons_drop_all();
    atomic_fetch_add(&now_ms, APPS_ICONS_RETRY_MS);
    want_centre(6, N_APPS);
    settle();
    CHECK(shows("gg", 8, 7) && shows("ff", 8, 6) && shows("hh", 8, 8) && shows("ii", 8, 9), "the last window loads in full");
    CHECK(apps_icons_get("ee") == NULL, "its app with no icon is a tile");
}

typedef struct {
    const char *name;
    void (*fn)(void);
} case_t;

static const case_t CASES[] = {
    {"window_loads_and_frees", window_loads_and_frees},
    {"get_is_null_until_loaded", get_is_null_until_loaded},
    {"refused_load_keeps_tile", refused_load_keeps_tile},
    {"list_change_drops_stale_icons", list_change_drops_stale_icons},
    {"an_orphaned_load_reads_nothing", an_orphaned_load_reads_nothing},
    {"keep_frees_all_but_the_kept", keep_frees_all_but_the_kept},
    {"bad_icon_file_is_a_tile", bad_icon_file_is_a_tile},
    {"no_flash_on_the_lvgl_task", no_flash_on_the_lvgl_task},
    {"a_drop_racing_the_worker_frees_every_block", a_drop_racing_the_worker_frees_every_block},
    {"a_refused_submit_is_tried_again_later", a_refused_submit_is_tried_again_later},
    {"a_fling_keeps_the_worker_to_two_loads", a_fling_keeps_the_worker_to_two_loads},
    {"ram_mode_is_a_tile", ram_mode_is_a_tile},
    {"odd_windows", odd_windows},
    {"any_order_of_calls_keeps_the_bounds", any_order_of_calls_keeps_the_bounds},
};

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--cases") == 0) {
        for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
            printf("%s\n", CASES[i].name);
        }
        return 0;
    }
    const case_t *c = NULL;
    for (size_t i = 0; argc == 2 && c == NULL && i < sizeof CASES / sizeof CASES[0]; i++) {
        c = strcmp(CASES[i].name, argv[1]) == 0 ? &CASES[i] : NULL;
    }
    if (c == NULL) {
        fprintf(stderr, "usage: %s <case> | --cases\n", argv[0]);
        return 2;
    }
    c->fn();
    /* The teardown: whatever the case left, the launcher's close lets go of, and the loads in flight free their own blocks. */
    gate_open();
    jobs_join();
    apps_icons_drop_all();
    run_jobs();
    (void)apps_icons_poll();
    CHECK(atomic_load(&blocks_held) == 0, "%ld blocks of the apps' heap still held after the drop", atomic_load(&blocks_held));
    CHECK(atomic_load(&reads_off_worker) == 0, "X11: %d reads of the store on the LVGL task", atomic_load(&reads_off_worker));
    CHECK(jobs_waiting() == 0, "the drop asked for more: %d jobs", jobs_waiting());
    free_files();
    if (failures > 0) {
        dump_log();
        return 1;
    }
    return 0;
}
