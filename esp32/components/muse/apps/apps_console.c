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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_port.h"
#include "apps_task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "muse_apps.h"

/*
 * The USB console's ">app.*" lines (muse_apps.h has the commands). muse_input's serial task calls the hook with each
 * whole line it reads, at most 1,023 bytes after the '>' (a longer one never comes here), on a stack of 3,584 bytes:
 * so nothing here keeps an array of more than 128 bytes in a frame, and everything else is on the heap (PSRAM on the
 * gadget). Senders send a file in pieces of at most 768 base64 characters, each after the last one's "@app.ack".
 *
 * Who owns what. The stage (the files put for one id, in PSRAM) is the serial task's, until a commit moves it into its
 * job; the job, on the I/O worker, reads the files out of it (taking each one's bytes), runs the install pipeline and
 * frees what is left. The stage's blocks hold at most STAGE_SLACK bytes beyond what was put, so it is never much more
 * than the 2 MB an app may be. The RAM store is the worker's (apps_install.h), so a launch and the list are jobs too.
 * Stop, status and send wait on the serial task, each bounded by the app task's own waits (APPS_TASK_WAIT_MS). The
 * launch job is also muse_apps_launch's, for any task (the launcher's pick comes through apps_task_launch): that one
 * runs the app where it was asked to, with no settle, by id (apps_task_run_by_id: an app of that id that runs by then
 * is fronted, not started again), and prints nothing. app.launch runs the store's copy afresh, as an install's run.
 *
 * Replies. Every one is one line: "@app.ack <path> <seq>", "@app {...}" or "@app.err {...}", the JSON made by cJSON
 * (texts travel escaped, tracebacks included) and written with one printf, so that no log line splits it, then
 * pushed out to the host (apps_port_console_flush: a line that ends on a 64-byte USB packet would otherwise wait for
 * the console's next output).
 */

#define STAGE_FILES (APPS_MAX_FILES + 1)  /* manifest.json, in slot 0, and as many files as a package has */
#define MIN_CAP 1024                      /* a staged file's first block */
#define STAGE_SLACK (STAGE_FILES * MIN_CAP)  /* what the stage's blocks may hold beyond the bytes put */
#define ERR_MAX 1024                      /* what apps_task_message says of a failure: a handler's error, a crash */

#define MSG_BAD_PATH "'%s' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 characters)"

typedef struct {
    char path[APPS_PATH_MAX + 1];  /* "" for a free slot */
    uint8_t *data;                 /* cap bytes of apps_port_realloc, len of them put */
    size_t len, cap;
    unsigned next;                 /* the seq that comes next */
} staged_t;

typedef struct {
    char id[APPS_ID_MAX + 1];
    staged_t files[STAGE_FILES];
    size_t app_bytes, image_bytes;  /* what is staged, manifest.json in the app's */
} stage_t;

/* A job for the I/O worker: a commit's (which has the stage) or a launch's. */
typedef struct {
    char id[APPS_ID_MAX + 1];
    bool run;        /* a commit's "run" */
    bool front;      /* a launch's place */
    bool print;      /* a launch the console asked for: it settles, and its result is printed */
    stage_t *stage;
    char why[192];   /* a launch's failure, said once (a frame keeps no array over 128 bytes) */
} job_t;

static const char *TAG = "muse_apps";
static stage_t *s_stage;  /* the serial task's */

/* ---- Replies ---- */

/* Whether op prints in JSON as it is: our own commands do. */
static bool plain(const char *op) {
    for (; *op != '\0'; op++) {
        if (*op < 'a' || *op > 'z') {
            return false;
        }
    }
    return true;
}

/* One reply line: prefix, then item's JSON (an object) with "op" first when op isn't NULL. */
static void print_reply(const char *prefix, const char *op, const cJSON *item) {
    char *text = item != NULL ? cJSON_PrintUnformatted(item) : NULL;
    if (text == NULL) {
        printf("@app.err {\"op\":\"%s\",\"message\":\"out of memory\"}\n", op != NULL && plain(op) ? op : "?");
    } else if (op != NULL) {
        printf("%s{\"op\":\"%s\"%s%s\n", prefix, op, text[1] == '}' ? "" : ",", text + 1);
    } else {
        printf("%s%s\n", prefix, text);
    }
    apps_port_console_flush();
    cJSON_free(text);
}

/* "@app.err {"op":op,["id":id,]["path":path,]"message":...}", the message made from fmt. op, id and path are shown as
 * they are, so callers pass them clean (apps_text_shown). */
static void fail(const char *op, const char *id, const char *path, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void fail(const char *op, const char *id, const char *path, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *message = n >= 0 ? apps_port_realloc(NULL, (size_t)n + 1) : NULL;
    if (message != NULL) {
        va_start(ap, fmt);
        vsnprintf(message, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    cJSON *r = message != NULL ? cJSON_CreateObject() : NULL;
    bool ok = r != NULL && cJSON_AddStringToObject(r, "op", op) != NULL &&
              (id == NULL || cJSON_AddStringToObject(r, "id", id) != NULL) &&
              (path == NULL || cJSON_AddStringToObject(r, "path", path) != NULL) &&
              cJSON_AddStringToObject(r, "message", message) != NULL;
    print_reply("@app.err ", ok ? NULL : op, ok ? r : NULL);
    cJSON_Delete(r);
    apps_port_free(message);
}

/* A result of apps_install (or a job's own object) as "@app {"op":op,...}". NULL is no memory for it. */
static void print_result(const char *op, const char *id, const cJSON *r) {
    if (r == NULL) {
        fail(op, id, NULL, "out of memory: no room for the result");
        return;
    }
    print_reply("@app ", op, r);
}

/* ---- Words ---- */

typedef struct {
    const char *p;
    size_t n;
} word_t;

/* The next word of *at, spaces before it skipped, and *at moved past it. */
static word_t next_word(const char **at) {
    const char *s = *at;
    while (*s == ' ') {
        s++;
    }
    const char *e = s;
    while (*e != '\0' && *e != ' ') {
        e++;
    }
    *at = e;
    return (word_t){s, (size_t)(e - s)};
}

/* What is left of the line, spaces before it skipped. */
static const char *rest(const char *at) {
    while (*at == ' ') {
        at++;
    }
    return at;
}

static bool is(word_t w, const char *s) {
    return w.n == strlen(s) && memcmp(w.p, s, w.n) == 0;
}

/* w into out (cap bytes with its NUL): false when it doesn't fit. */
static bool copy_word(word_t w, char *out, size_t cap) {
    if (w.n == 0 || w.n >= cap) {
        return false;
    }
    memcpy(out, w.p, w.n);
    out[w.n] = '\0';
    return true;
}

/* An app id from w, or a refusal for op. */
static bool take_id(const char *op, word_t w, const char *path, char *id) {
    if (copy_word(w, id, APPS_ID_MAX + 1) && apps_id_valid(id)) {
        return true;
    }
    char shown[APPS_SHOWN_MAX + 4];
    apps_text_shown(w.p, w.n, APPS_SHOWN_MAX, shown);
    fail(op, strcmp(op, "put") == 0 ? NULL : shown, path, APPS_MSG_BAD_ID, shown);
    return false;
}

/* A seq: a whole number of at most 9 digits. */
static bool take_seq(word_t w, unsigned *seq) {
    if (w.n == 0 || w.n > 9) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < w.n; i++) {
        if (w.p[i] < '0' || w.p[i] > '9') {
            return false;
        }
        v = v * 10 + (unsigned)(w.p[i] - '0');
    }
    *seq = v;
    return true;
}

/* ---- Base64 ---- */

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* 0 when every character of s[0, n) can be where it is ('=' only as the last one or two), else the position (from 1)
 * of the first that can't. */
static size_t b64_bad_at(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b64_value(s[i]) >= 0 || (s[i] == '=' && (i + 1 == n || (i + 2 == n && s[n - 1] == '=')))) {
            continue;
        }
        return i + 1;
    }
    return 0;
}

/* The bytes base64 s[0, n) (checked, n a multiple of 4) decodes to. */
static size_t b64_len(const char *s, size_t n) {
    size_t pads = n > 0 && s[n - 1] == '=' ? (n > 1 && s[n - 2] == '=' ? 2 : 1) : 0;
    return n / 4 * 3 - pads;
}

static void b64_decode(const char *s, size_t n, uint8_t *out) {
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        int pads = 0;
        for (int k = 0; k < 4; k++) {
            int d = b64_value(s[i + k]);
            pads += d < 0;
            v = (v << 6) | (uint32_t)(d < 0 ? 0 : d);
        }
        *out++ = (uint8_t)(v >> 16);
        if (pads < 2) {
            *out++ = (uint8_t)(v >> 8);
        }
        if (pads < 1) {
            *out++ = (uint8_t)v;
        }
    }
}

/* ---- The stage ---- */

static void stage_free(stage_t *stage) {
    if (stage == NULL) {
        return;
    }
    for (int i = 0; i < STAGE_FILES; i++) {
        apps_port_free(stage->files[i].data);
    }
    apps_port_free(stage);
}

static bool is_manifest(const char *path) {
    return strcmp(path, "manifest.json") == 0;
}

static staged_t *find_file(stage_t *stage, const char *path) {
    for (int i = 0; i < STAGE_FILES; i++) {
        if (strcmp(stage->files[i].path, path) == 0) {
            return &stage->files[i];
        }
    }
    return NULL;
}

/* A slot for path: manifest.json's own, or the first free one of the rest. NULL when they are all taken. */
static staged_t *new_file(stage_t *stage, const char *path) {
    int i = is_manifest(path) ? 0 : 1;
    for (; i < STAGE_FILES && stage->files[i].path[0] != '\0'; i++) {
        if (i == 0) {
            return NULL;  /* can't be: find_file finds manifest.json first */
        }
    }
    if (i == STAGE_FILES) {
        return NULL;
    }
    staged_t *f = &stage->files[i];
    snprintf(f->path, sizeof f->path, "%s", path);
    return f;
}

/* f's bytes leave the totals. */
static void unaccount(stage_t *stage, const staged_t *f) {
    stage->app_bytes -= f->len;
    if (apps_file_kind(f->path) == APPS_FILE_IMAGE) {
        stage->image_bytes -= f->len;
    }
}

/* f goes from the stage: the next piece of it must be seq 0. */
static void drop_file(stage_t *stage, staged_t *f) {
    unaccount(stage, f);
    apps_port_free(f->data);
    memset(f, 0, sizeof *f);
}

/*
 * The block f grows to for len bytes, at most max: doubling, so a file is copied a few times and not for each piece,
 * but the stage's blocks never hold more than STAGE_SLACK bytes beyond what was put, so the stage stays within the
 * 2 MB an app may be and that slack whatever the files are. The other files' blocks are fitted to their bytes first (a
 * sender sends one file after another, so they are done), and f gets what the slack leaves beyond len.
 */
static size_t grown_cap(stage_t *stage, const staged_t *f, size_t len, size_t max) {
    size_t spare = 0;
    for (int i = 0; i < STAGE_FILES; i++) {
        staged_t *g = &stage->files[i];
        if (g == f || g->cap <= g->len) {
            continue;
        }
        if (g->len == 0) {
            apps_port_free(g->data);
            g->data = NULL;
            g->cap = 0;
            continue;
        }
        uint8_t *fitted = apps_port_realloc(g->data, g->len);  /* smaller: in place */
        if (fitted != NULL) {
            g->data = fitted;
            g->cap = g->len;
        }
        spare += g->cap - g->len;
    }
    size_t room = spare < STAGE_SLACK ? STAGE_SLACK - spare : 0;
    size_t cap = f->cap * 2 > len ? f->cap * 2 : len;
    cap = cap < MIN_CAP ? MIN_CAP : cap > max ? max : cap;
    return cap - len > room ? len + room : cap;
}

/* The source of a commit's job (apps_install.h): a file's bytes leave the stage for the pipeline, fitted to their
 * length. */
static bool stage_read(void *ctx, const char *rel, size_t max, int64_t deadline_us, uint8_t **data, size_t *len,
                       char *err, size_t err_len) {
    (void)max;
    (void)deadline_us;  /* the bytes are here already */
    staged_t *f = find_file(ctx, rel);
    if (f == NULL || (f->data == NULL && f->len > 0)) {
        snprintf(err, err_len, "reading %s: it wasn't sent (app.put)", rel);
        return false;
    }
    if (f->data == NULL) {
        f->data = apps_port_realloc(NULL, 1);  /* an empty file is a block of one byte */
        f->cap = 1;
    } else if (f->len > 0 && f->len < f->cap) {
        uint8_t *fitted = apps_port_realloc(f->data, f->len);
        if (fitted != NULL) {
            f->data = fitted;
        }
    }
    if (f->data == NULL) {
        snprintf(err, err_len, "reading %s: out of memory", rel);
        return false;
    }
    *data = f->data;
    *len = f->len;
    f->data = NULL;
    f->cap = 0;
    return true;
}

/* ---- The commands ---- */

static void put(const char *args) {
    const char *at = args;
    word_t id_w = next_word(&at), path_w = next_word(&at), seq_w = next_word(&at);
    const char *b64 = rest(at);
    if (seq_w.n == 0) {
        fail("put", NULL, NULL, "usage: app.put <id> <path> <seq> <base64>");
        return;
    }
    char shown[APPS_SHOWN_MAX + 4], id[APPS_ID_MAX + 1], path[APPS_PATH_MAX + 1];
    apps_text_shown(path_w.p, path_w.n, APPS_SHOWN_MAX, shown);
    if (!take_id("put", id_w, shown, id)) {
        return;
    }
    if (!copy_word(path_w, path, sizeof path) || !apps_path_valid(path)) {
        fail("put", NULL, shown, MSG_BAD_PATH, shown);
        return;
    }
    apps_file_kind_t kind = apps_file_kind(path);
    if (kind == APPS_FILE_BAD) {
        fail("put", NULL, path, "'%s' has an unsupported type (use .lua, .json or .bin)", path);
        return;
    }
    unsigned seq;
    if (!take_seq(seq_w, &seq)) {
        fail("put", NULL, path, "seq must be a whole number");
        return;
    }
    size_t n = strlen(b64), bad = b64_bad_at(b64, n);
    if (bad > 0) {
        fail("put", NULL, path, "bad base64 at character %u", (unsigned)bad);
        return;
    }
    if (n % 4 != 0) {
        fail("put", NULL, path, "bad base64: %u characters isn't a multiple of 4", (unsigned)n);
        return;
    }
    if (s_stage == NULL || strcmp(s_stage->id, id) != 0) {
        stage_free(s_stage);  /* a new id: the old stage goes */
        s_stage = apps_port_realloc(NULL, sizeof *s_stage);
        if (s_stage == NULL) {
            fail("put", NULL, path, "out of memory: no room to stage the app");
            return;
        }
        memset(s_stage, 0, sizeof *s_stage);
        snprintf(s_stage->id, sizeof s_stage->id, "%s", id);
    }
    staged_t *f = find_file(s_stage, path);
    if (seq == 0 && f != NULL) {
        unaccount(s_stage, f);  /* sent again from the start */
        f->len = 0;
    } else if (seq == 0) {
        f = new_file(s_stage, path);
        if (f == NULL) {
            fail("put", NULL, path, "more than %d files besides manifest.json", APPS_MAX_FILES);
            return;
        }
    } else if (f == NULL || seq != f->next) {
        fail("put", NULL, path, "expected seq %u", f != NULL ? f->next : 0u);
        return;
    }
    /* The limits, as the bytes arrive: a file over one is dropped whole. */
    size_t bytes = b64_len(b64, n), max = is_manifest(path) ? APPS_MANIFEST_MAX : apps_file_max(kind);
    size_t len = f->len + bytes;
    if (len > max) {
        drop_file(s_stage, f);
        fail("put", NULL, path, "%s is larger than %u KB", path, (unsigned)(max / 1024));
        return;
    }
    if (s_stage->app_bytes + bytes > APPS_APP_MAX) {
        drop_file(s_stage, f);
        fail("put", NULL, path, "the app is larger than 2 MB");
        return;
    }
    if (kind == APPS_FILE_IMAGE && s_stage->image_bytes + bytes > APPS_IMAGES_TOTAL_MAX) {
        drop_file(s_stage, f);
        fail("put", NULL, path, "images add up to more than 1.5 MB");
        return;
    }
    if (len > f->cap) {
        size_t cap = grown_cap(s_stage, f, len, max);
        uint8_t *grown = apps_port_realloc(f->data, cap);
        if (grown == NULL) {
            drop_file(s_stage, f);
            fail("put", NULL, path, "out of memory: no room for %s", path);
            return;
        }
        f->data = grown;
        f->cap = cap;
    }
    if (bytes > 0) {
        b64_decode(b64, n, f->data + f->len);  /* straight into the stage (an empty piece may have no block yet) */
    }
    f->len = len;
    s_stage->app_bytes += bytes;
    if (kind == APPS_FILE_IMAGE) {
        s_stage->image_bytes += bytes;
    }
    f->next = seq + 1;
    printf("@app.ack %s %u\n", path, seq);
    apps_port_console_flush();
}

static bool install_run(apps_pkg_t *p, int settle_ms, apps_status_t *st, char *err, size_t n) {
    return apps_task_run(p, true, settle_ms, st, err, n);  /* C2: an install with a run fronts the app */
}

static const char *ram_storage(void) {
    return "ram";
}

/* Until Task 20's store, a package stays in PSRAM (apps_install.h's RAM store). */
static const apps_install_ops_t USB_OPS = {apps_task_compile, apps_ram_commit, install_run, ram_storage};

static void commit_job(void *arg) {
    job_t *job = arg;
    apps_source_t src = {stage_read, job->stage, "usb"};
    cJSON *r = apps_install_run(job->id, job->run, &src, &USB_OPS);
    stage_free(job->stage);
    apps_install_end();  /* before the result: a commit sent once it is read isn't busy */
    print_result("commit", job->id, r);
    cJSON_Delete(r);
    apps_port_free(job);
}

static void commit(const char *args) {
    const char *at = args;
    word_t id_w = next_word(&at), run_w = next_word(&at), more = next_word(&at);
    if (id_w.n == 0 || (run_w.n > 0 && !is(run_w, "run")) || more.n > 0) {
        fail("commit", NULL, NULL, "usage: app.commit <id> [run]");
        return;
    }
    char id[APPS_ID_MAX + 1];
    if (!take_id("commit", id_w, NULL, id)) {
        return;
    }
    /* One install at a time, first: the same commit sent again while its job runs is busy, though its stage went into
     * that job. */
    if (!apps_install_try_begin()) {
        fail("commit", id, NULL, APPS_INSTALL_BUSY);
        return;
    }
    bool staged = false;
    if (s_stage != NULL && strcmp(s_stage->id, id) == 0) {
        for (int i = 0; i < STAGE_FILES; i++) {
            staged = staged || s_stage->files[i].path[0] != '\0';
        }
    }
    if (!staged) {
        apps_install_end();
        fail("commit", id, NULL, "nothing is staged for %s: send its files with app.put first", id);
        return;
    }
    job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job == NULL) {
        apps_install_end();
        fail("commit", id, NULL, "out of memory: no room for the install");
        return;
    }
    memset(job, 0, sizeof *job);
    snprintf(job->id, sizeof job->id, "%s", id);
    job->run = run_w.n > 0;
    job->stage = s_stage;
    s_stage = NULL;  /* the job's from here: a put after this starts a new stage */
    if (!apps_io_submit(commit_job, job)) {
        s_stage = job->stage;  /* kept for another try */
        apps_port_free(job);
        apps_install_end();
        fail("commit", id, NULL, APPS_IO_REFUSED);
    }
}

/* The store's copy of job->id, run. The console's (R13.5) runs it through apps_task_run in front, settles and prints its
 * result; anyone else's (muse_apps_launch) runs it by id where it was asked to, with no settle, and says only a failure,
 * in one log line. */
static void launch_job(void *arg) {
    job_t *job = arg;
    apps_pkg_t *pkg = apps_port_realloc(NULL, sizeof *pkg);
    snprintf(job->why, sizeof job->why, "out of memory: no room to load the app");
    if (pkg != NULL && apps_ram_load(job->id, pkg, job->why, sizeof job->why)) {
        if (job->print) {
            cJSON *r = apps_install_launch(pkg, &USB_OPS);  /* it takes the copy: the app task frees it */
            print_result("launch", job->id, r);
            cJSON_Delete(r);
        } else if (!apps_task_run_by_id(pkg, job->front, 0, NULL, job->why, sizeof job->why)) {  /* it takes it too */
            ESP_LOGW(TAG, "launch %s: %s", job->id, job->why);
        }
    } else {
        apps_port_free(pkg);
        if (job->print) {
            fail("launch", job->id, NULL, "%s", job->why);
        } else {
            ESP_LOGW(TAG, "launch %s: %s", job->id, job->why);
        }
        if (!apps_ram_has(job->id)) {
            apps_task_launch_missing(job->id);  /* the alarms that fired for it and wait for this launch are given up */
        }
    }
    apps_port_free(job);
}

/* A launch job for id, or NULL when there is no memory for it. */
static job_t *new_launch(const char *id, bool front, bool print) {
    job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job != NULL) {
        memset(job, 0, sizeof *job);
        snprintf(job->id, sizeof job->id, "%s", id);
        job->front = front;
        job->print = print;
    }
    return job;
}

static void launch(const char *args) {
    const char *at = args;
    word_t id_w = next_word(&at), more = next_word(&at);
    if (id_w.n == 0 || more.n > 0) {
        fail("launch", NULL, NULL, "usage: app.launch <id>");
        return;
    }
    char id[APPS_ID_MAX + 1];
    if (!take_id("launch", id_w, NULL, id)) {
        return;
    }
    job_t *job = new_launch(id, true, true);
    if (job == NULL) {
        fail("launch", id, NULL, "out of memory: no room for the launch");
    } else if (!apps_io_submit(launch_job, job)) {
        apps_port_free(job);
        fail("launch", id, NULL, APPS_IO_REFUSED);
    }
}

bool muse_apps_launch(const char *id, bool front) {
    if (id == NULL || !apps_id_valid(id)) {
        ESP_LOGW(TAG, "launch: not an app id");
        return false;
    }
    job_t *job = new_launch(id, front, false);
    if (job == NULL) {
        ESP_LOGW(TAG, "launch %s: out of memory: no room for the launch", id);
        return false;
    }
    if (!apps_io_submit(launch_job, job)) {  /* it says why, in one log line */
        apps_port_free(job);
        return false;
    }
    return true;
}

static bool no_args(const char *op, const char *args) {
    const char *at = args;
    if (next_word(&at).n == 0) {
        return true;
    }
    fail(op, NULL, NULL, "usage: app.%s", op);
    return false;
}

static void stop(const char *args) {
    if (!no_args("stop", args)) {
        return;
    }
    char err[128];
    if (apps_task_stop(err, sizeof err)) {
        printf("@app {\"op\":\"stop\",\"ok\":true}\n");
        apps_port_console_flush();
    } else {
        fail("stop", NULL, NULL, "%s", err);
    }
}

static void status(const char *args) {
    if (!no_args("status", args)) {
        return;
    }
    apps_status_t *st = apps_port_realloc(NULL, sizeof *st);  /* several KB: never on a stack (X12) */
    if (st == NULL) {
        fail("status", NULL, NULL, "out of memory: no room for the status");
        return;
    }
    apps_task_status(st);
    cJSON *r = apps_install_status(st);
    apps_port_free(st);
    print_result("status", NULL, r);
    cJSON_Delete(r);
}

static void list_job(void *arg) {
    (void)arg;
    apps_ram_entry_t *e = apps_port_realloc(NULL, APPS_RAM_APPS * sizeof *e);
    int n = e != NULL ? apps_ram_list(e, APPS_RAM_APPS) : 0;
    cJSON *r = e != NULL ? cJSON_CreateObject() : NULL;
    cJSON *apps = r != NULL && cJSON_AddStringToObject(r, "storage", "ram") != NULL ? cJSON_AddArrayToObject(r, "apps")
                                                                                   : NULL;
    bool ok = apps != NULL;
    for (int i = 0; ok && i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        if (a == NULL || !cJSON_AddItemToArray(apps, a)) {
            cJSON_Delete(a);
            ok = false;
            break;
        }
        ok = cJSON_AddStringToObject(a, "id", e[i].id) != NULL && cJSON_AddStringToObject(a, "name", e[i].name) != NULL &&
             cJSON_AddStringToObject(a, "version", e[i].version) != NULL;
    }
    print_result("list", NULL, ok ? r : NULL);
    cJSON_Delete(r);
    apps_port_free(e);
}

static void list(const char *args) {
    if (no_args("list", args) && !apps_io_submit(list_job, NULL)) {
        fail("list", NULL, NULL, APPS_IO_REFUSED);
    }
}

static void send(const char *args) {
    const char *at = args;
    word_t id_w = next_word(&at), event_w = next_word(&at);
    const char *data = rest(at);
    if (event_w.n == 0) {
        fail("send", NULL, NULL, "usage: app.send <id> <event> [json]");
        return;
    }
    char id[APPS_ID_MAX + 1];
    if (!take_id("send", id_w, NULL, id)) {
        return;
    }
    char *event = apps_port_realloc(NULL, event_w.n + 1);
    char *err = event != NULL ? apps_port_realloc(NULL, ERR_MAX) : NULL;
    if (err == NULL) {
        apps_port_free(event);
        fail("send", id, NULL, "out of memory: no room for the message");
        return;
    }
    memcpy(event, event_w.p, event_w.n);
    event[event_w.n] = '\0';
    char *reply = apps_task_message(id, event, data[0] != '\0' ? data : NULL, false, APPS_TASK_WAIT_MS, err, ERR_MAX);
    size_t len = reply != NULL ? strlen(reply) : 0;
    if (reply == NULL) {
        fail("send", id, NULL, "%s", err);
    } else if (len > APPS_RESULT_MAX) {
        fail("send", id, NULL, "the reply is %u bytes: the console shows at most %d", (unsigned)len, APPS_RESULT_MAX);
    } else {
        char shown[APPS_SHOWN_MAX + 4];
        apps_text_shown(event_w.p, event_w.n, APPS_SHOWN_MAX, shown);
        cJSON *r = cJSON_CreateObject();
        bool ok = r != NULL && cJSON_AddStringToObject(r, "id", id) != NULL &&
                  cJSON_AddStringToObject(r, "event", shown) != NULL && cJSON_AddRawToObject(r, "reply", reply) != NULL;
        print_result("send", id, ok ? r : NULL);
        cJSON_Delete(r);
    }
    apps_port_free(reply);
    apps_port_free(err);
    apps_port_free(event);
}

bool muse_apps_console(const char *line) {
    if (line == NULL || strncmp(line, "app.", 4) != 0) {
        return false;
    }
    const char *args = line + 4;
    word_t cmd = {args, strcspn(args, " ")};
    args += cmd.n;
    if (is(cmd, "put")) {
        put(args);
    } else if (is(cmd, "commit")) {
        commit(args);
    } else if (is(cmd, "launch")) {
        launch(args);
    } else if (is(cmd, "stop")) {
        stop(args);
    } else if (is(cmd, "status")) {
        status(args);
    } else if (is(cmd, "list")) {
        list(args);
    } else if (is(cmd, "send")) {
        send(args);
    } else {
        char shown[APPS_SHOWN_MAX + 4];
        apps_text_shown(cmd.p, cmd.n, APPS_SHOWN_MAX, shown);
        fail(shown, NULL, NULL,
             "unknown command app.%s (app.put, app.commit, app.launch, app.stop, app.status, app.list, app.send)",
             shown);
    }
    return true;
}
