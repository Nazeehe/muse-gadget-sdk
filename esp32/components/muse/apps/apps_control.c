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

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apps_alarm.h"
#include "apps_install.h"
#include "apps_io.h"
#include "apps_json.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_store.h"
#include "apps_task.h"
#include "apps_ui.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_apps.h"
#include "sdkconfig.h"

/*
 * Link's app.control and app.api (muse_apps.h has the contract; main/apps_link.c the commands). The Noise session's task
 * calls these, and must never wait long: it delivers every frame of the session, the VM's answers to the guide's writes
 * among them.
 *
 * Where the work runs. Everything that touches the store runs on the I/O worker (X11), as one job. list and status are
 * synchronous (D20): the caller queues the job and waits for its answer for at most MUSE_APPS_CONTROL_WAIT_MS, polling.
 * A caller that gives up (the worker is in an install, which can take 90 s) abandons the job: one atomic, in a block of
 * internal RAM (no read-modify-write atomics on PSRAM on the S3), says which of the two came first, and the one that
 * comes second frees the block (and the job its answer). launch, stop, remove and the guide are asynchronous: the job
 * calls the caller's done once with the result, as Link's install does.
 *
 * The guide. The firmware carries the guide and the timer example (EMBED_FILES, under CONFIG_MUSE_APPS), and app.api's
 * job writes those bytes to MUSE_APPS_GUIDE_DIR on the VM through the VM ops (vm_file's writer: whole files, in 8 KB
 * pieces), one deadline for the three. The writer is not the reader, so a display.draw_file that holds the reader doesn't
 * stop it. Each send takes DMA RAM in the session's task, so the job logs the DMA heap's free and lowest marks before and
 * after the writes.
 */

static const char *TAG = "muse_apps";

#define POLL_MS 10
#define CODE_NOT_FOUND "not_found"
#define STORE_NOT_READY "storage not ready"  /* apps_store.c's words, for a store that isn't mounted yet */
#define CONTROL_BUSY "the gadget is busy with another app job (an install, a launch or the guide); try again in a few seconds"
#define NO_ROOM_RESULT "out of memory: no room for the result"

/* What app.api says the gadget is: the round screen (apps_ui.h's ui.W, ui.H and ui.SAFE), the api of manifest.json, the
 * memory_kb a manifest may ask for (apps_pkg.h: 64 to min(1024, the device's heap max), 256 or that max if less). */
#define API_LEVEL 1
#define SCREEN_PX 466
#define SAFE_PX 330
#define MEMORY_MIN_KB 64
#define MEMORY_MAX_KB (CONFIG_MUSE_APPS_HEAP_MAX_KB < 1024 ? CONFIG_MUSE_APPS_HEAP_MAX_KB : 1024)
#define MEMORY_DEFAULT_KB (MEMORY_MAX_KB < 256 ? MEMORY_MAX_KB : 256)

/* The ui module's fonts (apps_ui.c's apps_ui_fonts[], in its order: tests/test_muse_apps_console.py compares them). */
static const char *const FONTS[] = {"sans14", "sans16", "sans20", "sans28", "mono8", "mono16", "digits48", "digits96"};

/* The firmware's copies of the guide and the timer: IDF names each pair of symbols after the file's name. The asm labels
 * keep the names exactly, whatever prefix the C compiler gives its own. */
extern const uint8_t guide_start[] __asm__("_binary_apps_guide_md_start");
extern const uint8_t guide_end[] __asm__("_binary_apps_guide_md_end");
extern const uint8_t timer_manifest_start[] __asm__("_binary_manifest_json_start");
extern const uint8_t timer_manifest_end[] __asm__("_binary_manifest_json_end");
extern const uint8_t timer_main_start[] __asm__("_binary_main_lua_start");
extern const uint8_t timer_main_end[] __asm__("_binary_main_lua_end");

typedef struct {
    const char *path;  /* on the VM */
    const char *name;  /* in an inline answer's example_files (NULL: the guide itself) */
    const uint8_t *start, *end;
} guide_file_t;

static const guide_file_t GUIDE_FILES[] = {
    {MUSE_APPS_GUIDE_DIR "README.md", NULL, guide_start, guide_end},
    {MUSE_APPS_GUIDE_DIR "timer/manifest.json", "manifest.json", timer_manifest_start, timer_manifest_end},
    {MUSE_APPS_GUIDE_DIR "timer/main.lua", "main.lua", timer_main_start, timer_main_end},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- Results ---- */

/* {"ok":false,"error":{"code","message"}}; NULL without memory. */
static cJSON *failure(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON *e = NULL;
    bool ok = r != NULL && cJSON_AddFalseToObject(r, "ok") != NULL && (e = cJSON_AddObjectToObject(r, "error")) != NULL &&
              cJSON_AddStringToObject(e, "code", code) != NULL && cJSON_AddStringToObject(e, "message", message) != NULL;
    if (!ok) {
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

/* {"ok":true,"payload":payload}, payload taken; NULL (payload freed) without memory. */
static cJSON *success(cJSON *payload) {
    cJSON *r = payload != NULL ? cJSON_CreateObject() : NULL;
    if (r != NULL && cJSON_AddTrueToObject(r, "ok") != NULL && cJSON_AddItemToObject(r, "payload", payload)) {
        return r;
    }
    cJSON_Delete(r);
    cJSON_Delete(payload);
    return NULL;
}

/* item added to o as key, or freed: false then. */
static bool add_item(cJSON *o, const char *key, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToObject(o, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static cJSON *not_found(const char *id) {
    char msg[sizeof MUSE_APPS_NOT_FOUND + APPS_ID_MAX];
    snprintf(msg, sizeof msg, MUSE_APPS_NOT_FOUND, id);
    return failure(CODE_NOT_FOUND, msg);
}

/* A refusal in the store's or the app task's words: out_of_memory for a lack of memory, else code. They give their refusals as
 * text only, with no code, and their lack-of-memory texts vary ("out of memory: no room to load the app", "... for the request"),
 * so this one reads the start of the text. */
static cJSON *refusal(const char *code, const char *why) {
    return failure(strncmp(why, "out of memory", 13) == 0 ? APPS_ERR_OUT_OF_MEMORY : code, why);
}

/* On the worker: true when the store is mounted and has id; otherwise *r is the refusal. */
static bool installed(const char *id, cJSON **r) {
    if (apps_store_mode() == APPS_STORAGE_NONE) {
        *r = failure(APPS_ERR_UNAVAILABLE, STORE_NOT_READY);
        return false;
    }
    if (!apps_store_has(id)) {
        *r = not_found(id);
        return false;
    }
    return true;
}

/* ---- status and the list (the worker's side) ---- */

/* log, lines joined by "\n", cut in place to its last n lines. */
static void keep_last_lines(char *log, int n) {
    size_t len = strlen(log);
    int seen = 0;
    for (size_t i = len; i > 0; i--) {
        if (log[i - 1] == '\n' && ++seen == n) {
            memmove(log, log + i, len - i + 1);
            return;
        }
    }
}

/* The lines of log as an array of strings; NULL without memory. */
static cJSON *log_lines(const char *log) {
    cJSON *lines = cJSON_CreateArray();
    for (const char *p = log; lines != NULL && *p != '\0';) {
        size_t n = strcspn(p, "\n");
        char *text = apps_port_realloc(NULL, n + 1);
        cJSON *s = NULL;
        if (text != NULL) {
            memcpy(text, p, n);
            text[n] = '\0';
            s = cJSON_CreateString(text);
            apps_port_free(text);
        }
        if (s == NULL || !cJSON_AddItemToArray(lines, s)) {
            cJSON_Delete(s);
            cJSON_Delete(lines);
            return NULL;
        }
        p += n + (p[n] == '\n');
    }
    return lines;
}

/* st (changed: its log cut to its last lines) as status shows it, or with logs as logs does. */
static cJSON *status_payload(apps_status_t *st, bool logs) {
    keep_last_lines(st->log_tail, MUSE_APPS_LOG_LINES);
    if (logs) {
        cJSON *p = cJSON_CreateObject();
        if (p == NULL || cJSON_AddStringToObject(p, "id", st->id) == NULL || !add_item(p, "log", log_lines(st->log_tail))) {
            cJSON_Delete(p);
            return NULL;
        }
        return success(p);
    }
    cJSON *s = apps_install_status(st);
    const char *hint = apps_store_hint();
    if (s == NULL || cJSON_AddStringToObject(s, "storage", apps_store_storage()) == NULL ||
        (hint != NULL && cJSON_AddStringToObject(s, "hint", hint) == NULL)) {
        cJSON_Delete(s);
        return NULL;
    }
    return success(s);
}

/* st made a stopped app id, as its manifest has it (m is scratch). */
static void stopped_record(const char *id, apps_status_t *st, apps_manifest_t *m) {
    bool known = apps_store_manifest(id, m);
    memset(st, 0, sizeof *st);
    st->state = APPS_STATE_STOPPED;
    snprintf(st->id, sizeof st->id, "%s", id);
    snprintf(st->name, sizeof st->name, "%s", known ? m->name : id);
    snprintf(st->version, sizeof st->version, "%s", known ? m->version : "");
    snprintf(st->ui_dump, sizeof st->ui_dump, "[]");
    st->limit_kb = known ? m->memory_kb : 0;
    apps_manifest_clear(m, apps_port_free);
}

static cJSON *status_result(const char *id, bool logs) {
    cJSON *r = NULL;
    if (!installed(id, &r)) {
        return r;
    }
    apps_status_t *st = apps_port_realloc(NULL, sizeof *st);  /* several KB: never on a stack (X12) */
    apps_manifest_t *m = st != NULL ? apps_port_realloc(NULL, sizeof *m) : NULL;
    if (m == NULL) {
        apps_port_free(st);
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the status");
    }
    apps_task_status(st);
    if (strcmp(st->id, id) != 0) {  /* not the record's app: it isn't running, and has said nothing since it ran */
        stopped_record(id, st, m);
    }
    r = status_payload(st, logs);
    apps_port_free(m);
    apps_port_free(st);
    return r;
}

/* The store's apps, its order, and the record: what the list and app.api read. */
typedef struct {
    char ids[APPS_STORE_APPS_MAX][APPS_ID_MAX + 1];
    char names[APPS_STORE_APPS_MAX][25];
    apps_manifest_t m;
    apps_status_t st;
} catalog_t;

/* The store's apps as an array: each with its id, name, version and state, and with full its last error and messages.
 * NULL without memory. c is scratch. */
static cJSON *apps_array(catalog_t *c, bool full) {
    apps_task_status(&c->st);
    int n = apps_store_list(c->ids, c->names, APPS_STORE_APPS_MAX);
    cJSON *apps = cJSON_CreateArray();
    bool ok = apps != NULL;
    for (int i = 0; ok && i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        if (a == NULL || !cJSON_AddItemToArray(apps, a)) {
            cJSON_Delete(a);
            ok = false;
            break;
        }
        bool known = apps_store_manifest(c->ids[i], &c->m);
        bool record = strcmp(c->st.id, c->ids[i]) == 0;
        ok = cJSON_AddStringToObject(a, "id", c->ids[i]) != NULL && cJSON_AddStringToObject(a, "name", c->names[i]) != NULL &&
             cJSON_AddStringToObject(a, "version", known ? c->m.version : "") != NULL &&
             cJSON_AddStringToObject(a, "state", apps_state_name(record ? c->st.state : APPS_STATE_STOPPED)) != NULL;
        if (ok && full) {
            ok = cJSON_AddStringToObject(a, "last_error", record ? c->st.last_error : "") != NULL &&
                 (c->m.messages_json != NULL ? cJSON_AddRawToObject(a, "messages", c->m.messages_json)
                                             : cJSON_AddObjectToObject(a, "messages")) != NULL;
        }
        apps_manifest_clear(&c->m, apps_port_free);
    }
    if (!ok) {
        cJSON_Delete(apps);
        return NULL;
    }
    return apps;
}

static cJSON *list_result(void) {
    catalog_t *c = apps_port_realloc(NULL, sizeof *c);  /* tens of KB: the record's among them */
    if (c == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the list");
    }
    memset(c, 0, sizeof *c);
    cJSON *apps = apps_array(c, true);
    apps_port_free(c);
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        cJSON_Delete(apps);
        return NULL;
    }
    if (!add_item(p, "apps", apps)) {
        cJSON_Delete(p);
        return NULL;
    }
    return success(p);
}

/* ---- The synchronous wait ---- */

enum { WAITING, ANSWERED, ABANDONED };

/* A list's or a status's request: in internal RAM, for its atomic. */
typedef struct {
    _Atomic int state;
    cJSON *result;  /* the job's, set before it says ANSWERED */
    bool logs;
    char id[APPS_ID_MAX + 1];  /* "" for the list */
} sync_t;

static void sync_job(void *arg) {
    sync_t *s = arg;
    cJSON *r = s->id[0] != '\0' ? status_result(s->id, s->logs) : list_result();
    s->result = r != NULL ? r : failure(APPS_ERR_OUT_OF_MEMORY, NO_ROOM_RESULT);
    int expected = WAITING;
    if (!atomic_compare_exchange_strong(&s->state, &expected, ANSWERED)) {  /* the caller gave up: nobody will read it */
        cJSON_Delete(s->result);
        heap_caps_free(s);
    }
}

/* The job for id ("" for the list), waited for at most MUSE_APPS_CONTROL_WAIT_MS. */
static cJSON *sync_call(const char *id, bool logs) {
    sync_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the request");
    }
    memset(s, 0, sizeof *s);
    atomic_init(&s->state, WAITING);
    snprintf(s->id, sizeof s->id, "%s", id);
    s->logs = logs;
    if (!apps_io_submit(sync_job, s)) {  /* it says why, in one log line */
        heap_caps_free(s);
        return failure(APPS_ERR_OUT_OF_MEMORY, APPS_IO_REFUSED);
    }
    const int64_t until = apps_port_mono_ms() + MUSE_APPS_CONTROL_WAIT_MS;
    while (atomic_load(&s->state) != ANSWERED) {
        if (apps_port_mono_ms() >= until) {
            int expected = WAITING;
            if (atomic_compare_exchange_strong(&s->state, &expected, ABANDONED)) {
                return failure(APPS_ERR_BUSY, CONTROL_BUSY);  /* the job frees the request */
            }
            break;  /* it answered just now */
        }
        TickType_t ticks = pdMS_TO_TICKS(POLL_MS);
        vTaskDelay(ticks > 0 ? ticks : 1);
    }
    cJSON *r = s->result;
    heap_caps_free(s);
    return r;
}

cJSON *muse_apps_list(void) {
    return sync_call("", false);
}

cJSON *muse_apps_status(const char *id, bool logs) {
    char msg[128];
    if (!muse_apps_id_valid(id, msg, sizeof msg)) {
        return failure(APPS_ERR_INVALID_PARAMS, msg);
    }
    return sync_call(id, logs);
}

/* ---- launch, stop and remove (the worker's side) ---- */

static bool run_in_front(apps_pkg_t *pkg, int settle_ms, apps_status_t *st, char *err, size_t n) {
    return apps_task_run_by_id(pkg, true, settle_ms, st, err, n);
}

/* A launch runs the store's copy and nothing else of the pipeline (apps_install_launch). */
static const apps_install_ops_t LAUNCH_OPS = {NULL, NULL, run_in_front, apps_store_storage, apps_store_hint};

static cJSON *launch_result(const char *id) {
    cJSON *r = NULL;
    if (!installed(id, &r)) {
        return r;
    }
    apps_pkg_t *pkg = apps_port_realloc(NULL, sizeof *pkg);
    if (pkg == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room to load the app");
    }
    char why[256];
    if (!apps_store_load(id, pkg, why, sizeof why)) {  /* the quarantine, a damaged copy, no room */
        apps_port_free(pkg);
        return refusal(APPS_ERR_UNAVAILABLE, why);
    }
    r = apps_install_launch(pkg, &LAUNCH_OPS);  /* it takes the copy: the app task frees it */
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"))) {
        return r;  /* crashed or not_running, in a launch's words */
    }
    cJSON_Delete(r);
    apps_status_t *st = apps_port_realloc(NULL, sizeof *st);
    if (st == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the status");
    }
    apps_task_status(st);
    r = status_payload(st, false);
    apps_port_free(st);
    return r;
}

static cJSON *stop_result(const char *id) {
    char running[APPS_ID_MAX + 1], why[128];
    if (apps_task_running(running, sizeof running) && strcmp(running, id) == 0 && !apps_task_stop(why, sizeof why)) {
        return refusal(APPS_ERR_BUSY, why);
    }
    return success(cJSON_CreateObject());
}

/* The store's remove, which can also take a folder it doesn't list (its manifest.json damaged). Its refusals: not mounted,
 * the app that runs, no folder of that id (APPS_STORE_NO_SUCH_APP: not_found), or the file system's (unavailable). */
static cJSON *remove_result(const char *id) {
    char running[APPS_ID_MAX + 1], why[192];
    if (apps_store_remove(id, why, sizeof why)) {
        cJSON *p = cJSON_CreateObject();
        if (p == NULL || cJSON_AddStringToObject(p, "removed", id) == NULL) {
            cJSON_Delete(p);
            return NULL;
        }
        return success(p);
    }
    if (apps_store_mode() == APPS_STORAGE_NONE) {
        return failure(APPS_ERR_UNAVAILABLE, why);
    }
    if (apps_task_running(running, sizeof running) && strcmp(running, id) == 0) {
        return failure(APPS_ERR_BUSY, why);
    }
    return strcmp(why, APPS_STORE_NO_SUCH_APP) == 0 ? not_found(id) : refusal(APPS_ERR_UNAVAILABLE, why);
}

/* ---- The guide (the worker's side) ---- */

static void log_dma(const char *what, const char *more) {
    ESP_LOGI(TAG, "app.api: %s%s (DMA free %u, lowest %u)", what, more, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
}

/* f's bytes as a string of its own (the caller frees it); NULL without memory. */
static char *file_text(const guide_file_t *f) {
    size_t n = (size_t)(f->end - f->start);
    char *text = apps_port_realloc(NULL, n + 1);
    if (text != NULL) {
        memcpy(text, f->start, n);
        text[n] = '\0';
    }
    return text;
}

/* The guide and the timer, inline: guide_text and example_files. */
static bool add_inline(cJSON *p) {
    cJSON *files = cJSON_AddObjectToObject(p, "example_files");
    bool ok = files != NULL;
    for (size_t i = 0; ok && i < ARRAY_LEN(GUIDE_FILES); i++) {
        const guide_file_t *f = &GUIDE_FILES[i];
        char *text = file_text(f);
        ok = text != NULL && (f->name != NULL ? cJSON_AddStringToObject(files, f->name, text)
                                              : cJSON_AddStringToObject(p, "guide_text", text)) != NULL;
        apps_port_free(text);
    }
    return ok;
}

/* {keys[i]: values[i], ...}; NULL without memory. */
static cJSON *number_object(const char *const *keys, const double *values, size_t n) {
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; o != NULL && i < n; i++) {
        if (cJSON_AddNumberToObject(o, keys[i], values[i]) == NULL) {
            cJSON_Delete(o);
            return NULL;
        }
    }
    return o;
}

/* What the gadget is, for an app's author: api, screen, fonts, limits, storage (and hint), the installed apps. */
static bool add_facts(cJSON *p) {
    static const char *const screen_keys[] = {"w", "h"};
    const double screen[] = {SCREEN_PX, SCREEN_PX};
    static const char *const limit_keys[] = {"files", "lua_kb", "json_kb", "image_kb", "images_kb", "app_kb", "widgets",
                                             "timers", "alarms", "store_kb"};
    const double limits[] = {APPS_MAX_FILES, APPS_LUA_MAX / 1024, APPS_JSON_FILE_MAX / 1024, APPS_IMAGE_MAX / 1024,
                             APPS_IMAGES_TOTAL_MAX / 1024, APPS_APP_MAX / 1024, APPS_UI_SLOTS, APPS_TIMERS_MAX,
                             APPS_ALARMS_PER_APP, APPS_STORE_MAX_BYTES / 1024};
    static const char *const memory_keys[] = {"min", "default", "max"};
    const double memory[] = {MEMORY_MIN_KB, MEMORY_DEFAULT_KB, MEMORY_MAX_KB};
    _Static_assert(ARRAY_LEN(limit_keys) == ARRAY_LEN(limits), "a value for each limit");

    cJSON *s = number_object(screen_keys, screen, ARRAY_LEN(screen));
    if (s != NULL && (cJSON_AddTrueToObject(s, "round") == NULL || cJSON_AddNumberToObject(s, "safe", SAFE_PX) == NULL)) {
        cJSON_Delete(s);
        s = NULL;
    }
    cJSON *l = number_object(limit_keys, limits, ARRAY_LEN(limits));
    if (cJSON_AddNumberToObject(p, "api", API_LEVEL) == NULL || !add_item(p, "screen", s) ||
        !add_item(p, "fonts", cJSON_CreateStringArray(FONTS, (int)ARRAY_LEN(FONTS))) ||
        (l != NULL && !add_item(l, "memory_kb", number_object(memory_keys, memory, ARRAY_LEN(memory))))) {
        cJSON_Delete(l);
        return false;
    }
    if (!add_item(p, "limits", l)) {
        return false;
    }
    const char *hint = apps_store_hint();
    if (cJSON_AddStringToObject(p, "storage", apps_store_storage()) == NULL ||
        (hint != NULL && cJSON_AddStringToObject(p, "hint", hint) == NULL)) {
        return false;
    }
    catalog_t *c = apps_port_realloc(NULL, sizeof *c);
    if (c == NULL) {
        return false;
    }
    memset(c, 0, sizeof *c);
    cJSON *apps = apps_array(c, false);
    apps_port_free(c);
    return add_item(p, "apps", apps);
}

static cJSON *guide_result(const muse_apps_vm_ops_t *vm) {
    const int64_t started = apps_port_mono_ms();
    const int64_t deadline_us = (started + MUSE_APPS_GUIDE_WRITE_MS) * 1000;
    log_dma("writing the guide and the timer to " MUSE_APPS_GUIDE_DIR, "");
    char why[192] = "";
    const char *code = NULL;
    size_t done = 0, bytes = 0;
    for (; done < ARRAY_LEN(GUIDE_FILES); done++) {
        const guide_file_t *f = &GUIDE_FILES[done];
        size_t len = (size_t)(f->end - f->start);
        code = MUSE_APPS_VM_FAILED;
        if (!vm->write(f->path, f->start, len, deadline_us, &code, why, sizeof why)) {
            break;
        }
        bytes += len;
    }
    bool written = done == ARRAY_LEN(GUIDE_FILES);
    char more[288];
    if (written) {
        snprintf(more, sizeof more, ", %u bytes in %u ms", (unsigned)bytes, (unsigned)(apps_port_mono_ms() - started));
        log_dma("the guide is on the VM", more);
    } else if (code == NULL || strcmp(code, MUSE_APPS_VM_FAILED) == 0) {
        /* The VM answered and didn't take the file (D3: a refusal): the answer carries the files instead. A session that is
         * gone (MUSE_APPS_VM_DROPPED) is a failure below: nothing would read an answer, so none is made of the guide. */
        snprintf(more, sizeof more, " %s: %s; the guide goes inline", GUIDE_FILES[done].path, why);
        log_dma("the VM didn't take", more);
    } else {
        snprintf(more, sizeof more, " %s: %s", GUIDE_FILES[done].path, why);
        log_dma("couldn't write", more);
        char msg[256];
        snprintf(msg, sizeof msg, "couldn't write the guide to the VM: %s; call app.api again", why);
        return failure(strcmp(code, MUSE_APPS_VM_BUSY) == 0            ? APPS_ERR_BUSY
                       : strcmp(code, MUSE_APPS_VM_OUT_OF_MEMORY) == 0 ? APPS_ERR_OUT_OF_MEMORY
                                                                       : APPS_ERR_UNAVAILABLE,
                       msg);
    }
    cJSON *p = cJSON_CreateObject();
    bool ok = p != NULL && cJSON_AddStringToObject(p, "delivered", written ? "vm" : "inline") != NULL;
    if (ok && written) {
        ok = cJSON_AddStringToObject(p, "guide", GUIDE_FILES[0].path) != NULL &&
             cJSON_AddStringToObject(p, "example", MUSE_APPS_GUIDE_DIR "timer/") != NULL;
    } else if (ok) {
        ok = cJSON_AddStringToObject(p, "write_error", why) != NULL;
    }
    ok = ok && add_facts(p) && (written || add_inline(p));
    if (!ok) {
        cJSON_Delete(p);
        return NULL;
    }
    return success(p);
}

/* ---- The asynchronous calls ---- */

typedef enum { DO_LAUNCH, DO_STOP, DO_REMOVE, DO_GUIDE } act_t;

/* A queued call: what the job needs, in PSRAM. */
typedef struct {
    act_t act;
    char id[APPS_ID_MAX + 1];
    const muse_apps_vm_ops_t *vm;
    muse_apps_done_cb done;
    void *ctx;
} job_t;

static void job_run(void *arg) {
    job_t *job = arg;
    cJSON *r = job->act == DO_LAUNCH   ? launch_result(job->id)
               : job->act == DO_STOP   ? stop_result(job->id)
               : job->act == DO_REMOVE ? remove_result(job->id)
                                       : guide_result(job->vm);
    job->done(job->ctx, r != NULL ? r : failure(APPS_ERR_OUT_OF_MEMORY, NO_ROOM_RESULT));
    apps_port_free(job);
}

static bool queue_job(act_t act, const char *id, const muse_apps_vm_ops_t *vm, muse_apps_done_cb done, void *ctx,
                      const char **code, const char **err) {
    const char *unused;
    code = code != NULL ? code : &unused;
    err = err != NULL ? err : &unused;
    if (done == NULL) {
        *code = APPS_ERR_INVALID_PARAMS;
        *err = "no callback for the result";
        return false;
    }
    if (act != DO_GUIDE && (id == NULL || !apps_id_valid(id))) {
        *code = APPS_ERR_INVALID_PARAMS;
        *err = "app id must match ^[a-z][a-z0-9_]{1,23}$";
        return false;
    }
    job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job == NULL) {
        *code = APPS_ERR_OUT_OF_MEMORY;
        *err = "out of memory: no room for the command";
        return false;
    }
    memset(job, 0, sizeof *job);
    job->act = act;
    snprintf(job->id, sizeof job->id, "%s", id != NULL ? id : "");
    job->vm = vm;
    job->done = done;
    job->ctx = ctx;
    if (!apps_io_submit(job_run, job)) {  /* it says why, in one log line */
        apps_port_free(job);
        *code = APPS_ERR_OUT_OF_MEMORY;
        *err = APPS_IO_REFUSED;
        return false;
    }
    return true;
}

bool muse_apps_launch_async(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    return queue_job(DO_LAUNCH, id, NULL, done, ctx, code, err);
}

bool muse_apps_stop(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    return queue_job(DO_STOP, id, NULL, done, ctx, code, err);
}

bool muse_apps_remove(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    return queue_job(DO_REMOVE, id, NULL, done, ctx, code, err);
}

/* ---- app.send (Task 24) ---- */

bool muse_apps_json_object_valid(const char *s, size_t len, char *err, size_t err_len) {
    if (!apps_json_validate(s, len, err, err_len)) {
        return false;
    }
    size_t at = 0;
    while (at < len && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) {
        at++;
    }
    if (at == len || s[at] != '{') {  /* one valid value: its first character says what it is */
        snprintf(err, err_len, "the top level must be an object");
        return false;
    }
    return true;
}

/* A queued app.send: what the job needs, event and data after the struct in the same block (data "" for none). */
typedef struct {
    char id[APPS_ID_MAX + 1];
    bool show;
    muse_apps_done_cb done;
    void *ctx;
    const char *event, *data;
} send_job_t;

/* What the app task said of a message that got no reply (apps_task_message's err), as a result's code and message. A handler's own
 * error is not any of the app task's words, so it is app_error and goes as it is. */
static cJSON *send_failure(const char *why) {
    size_t n = strlen(why);
    if (strncmp(why, APPS_TASK_MSG_NO_ANSWER, sizeof APPS_TASK_MSG_NO_ANSWER - 1) == 0) {
        return failure("timed_out", MUSE_APPS_SEND_NO_REPLY);
    }
    if (strcmp(why, APPS_TASK_MSG_BUSY) == 0) {
        return failure(APPS_ERR_BUSY, why);
    }
    if (strcmp(why, APPS_TASK_MSG_HUNG) == 0 || strcmp(why, APPS_TASK_MSG_DOWN) == 0) {
        return failure(APPS_ERR_UNAVAILABLE, why);
    }
    static const char NOT_RUNNING[] = " isn't running";
    if (n > sizeof NOT_RUNNING - 1 && strcmp(why + n - (sizeof NOT_RUNNING - 1), NOT_RUNNING) == 0) {
        return failure(APPS_ERR_NOT_RUNNING, why);
    }
    return refusal("app_error", why);
}

/* The launch an app.send needs when id isn't the app that runs: the store's copy of id, run behind. The refusal as a result, or NULL
 * once it runs. The launch has its own bound (APPS_TASK_SEND_LAUNCH_MS: 3 s for the load and start(), no settle), apart from the reply's, which
 * only starts once the message is in the app task's queue. */
static cJSON *send_launch(const char *id) {
    cJSON *r = NULL;
    if (!installed(id, &r)) {
        return r;
    }
    apps_pkg_t *pkg = apps_port_realloc(NULL, sizeof *pkg);
    if (pkg == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room to load the app");
    }
    char why[256];
    if (!apps_store_load(id, pkg, why, sizeof why)) {  /* the quarantine, a damaged copy, no room; the load is the order's bump */
        apps_port_free(pkg);
        return refusal(APPS_ERR_UNAVAILABLE, why);
    }
    if (!apps_task_run_for_send(pkg, why, sizeof why)) {  /* behind, no settle; it takes the copy */
        char msg[sizeof why + APPS_ID_MAX + 32];
        snprintf(msg, sizeof msg, "'%s' didn't start: %s", id, why);
        return failure(strncmp(why, "crashed", 7) == 0 ? APPS_ERR_CRASHED : APPS_ERR_NOT_RUNNING, msg);
    }
    return NULL;
}

/* On the worker: the message, after the launch it needs, as a result. */
static cJSON *send_result(const send_job_t *job) {
    char running[APPS_ID_MAX + 1];
    if (!apps_task_running(running, sizeof running) || strcmp(running, job->id) != 0) {
        cJSON *r = send_launch(job->id);
        if (r != NULL) {
            return r;
        }
    }
    char *why = apps_port_realloc(NULL, APPS_ERROR_MAX);
    if (why == NULL) {
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the message");
    }
    char *reply = apps_task_message(job->id, job->event, job->data[0] != '\0' ? job->data : NULL, job->show,
                                    MUSE_APPS_SEND_REPLY_MS, why, APPS_ERROR_MAX);
    cJSON *r;
    if (reply == NULL) {
        r = send_failure(why);
    } else {
        /* The handler's value is JSON text (apps_rt_message encoded it); a reply that somehow isn't goes as a string. */
        cJSON *value = cJSON_Parse(reply);
        if (value == NULL) {
            value = cJSON_CreateString(reply);
        }
        cJSON *p = value != NULL ? cJSON_CreateObject() : NULL;
        if (p != NULL && add_item(p, "reply", value)) {
            r = success(p);
        } else {
            if (p == NULL) {
                cJSON_Delete(value);
            }
            cJSON_Delete(p);
            r = NULL;
        }
    }
    apps_port_free(reply);
    apps_port_free(why);
    return r;
}

/* in, cut to 64 bytes with control bytes as '?', for the log (the event and data are Muse's text). */
static void log_text(const char *in, char *out, size_t out_len) {
    size_t n = 0;
    for (; in[n] != '\0' && n < 64 && n + 1 < out_len; n++) {
        out[n] = (unsigned char)in[n] < 0x20 || in[n] == 0x7f ? '?' : in[n];
    }
    out[n] = '\0';
}

static void send_run(void *arg) {
    send_job_t *job = arg;
    char event[72], data[72];
    log_text(job->event, event, sizeof event);
    log_text(job->data, data, sizeof data);
    ESP_LOGI(TAG, "app.send %s %s%s%s", job->id, event, data[0] != '\0' ? " " : "", data);
    cJSON *r = send_result(job);
    if (r != NULL && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"))) {
        ESP_LOGI(TAG, "app.send %s %s: %s", job->id, event,
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "error"), "code")));
    }
    job->done(job->ctx, r != NULL ? r : failure(APPS_ERR_OUT_OF_MEMORY, NO_ROOM_RESULT));
    apps_port_free(job);
}

bool muse_apps_send(const char *id, const char *event, const char *data_json, bool show, muse_apps_done_cb done, void *ctx,
                    const char **code, const char **err) {
    const char *unused;
    code = code != NULL ? code : &unused;
    err = err != NULL ? err : &unused;
    *code = APPS_ERR_INVALID_PARAMS;
    if (done == NULL) {
        *err = "no callback for the result";
        return false;
    }
    if (id == NULL || !apps_id_valid(id)) {
        *err = "app id must match ^[a-z][a-z0-9_]{1,23}$";
        return false;
    }
    if (event == NULL || event[0] == '\0') {
        *err = "event is required";
        return false;
    }
    size_t event_len = strlen(event), data_len = data_json != NULL ? strlen(data_json) : 0;
    if (data_len > MUSE_APPS_SEND_DATA_MAX) {
        *err = "data is larger than 4 KB";
        return false;
    }
    send_job_t *job = apps_port_realloc(NULL, sizeof *job + event_len + 1 + data_len + 1);
    if (job == NULL) {
        *code = APPS_ERR_OUT_OF_MEMORY;
        *err = "out of memory: no room for the command";
        return false;
    }
    memset(job, 0, sizeof *job);
    char *at = (char *)(job + 1);
    memcpy(at, event, event_len + 1);
    job->event = at;
    at += event_len + 1;
    if (data_len > 0) {
        memcpy(at, data_json, data_len);
    }
    at[data_len] = '\0';
    job->data = at;
    snprintf(job->id, sizeof job->id, "%s", id);
    job->show = show;
    job->done = done;
    job->ctx = ctx;
    if (!apps_io_submit(send_run, job)) {  /* it says why, in one log line */
        apps_port_free(job);
        *code = APPS_ERR_OUT_OF_MEMORY;
        *err = APPS_IO_REFUSED;
        return false;
    }
    return true;
}

bool muse_apps_write_guide(muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    const muse_apps_vm_ops_t *vm = muse_apps_vm_ops();
    if (done != NULL && (vm == NULL || vm->write == NULL)) {
        if (code != NULL) {
            *code = APPS_ERR_UNAVAILABLE;
        }
        if (err != NULL) {
            *err = "the gadget can't write files on the VM";
        }
        return false;
    }
    return queue_job(DO_GUIDE, NULL, vm, done, ctx, code, err);
}
