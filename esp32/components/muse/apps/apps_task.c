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

#include "apps_task.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "apps_json.h"
#include "apps_lv.h"
#include "apps_port.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "muse_apps.h"

/*
 * The app task, and the glue muse_ui calls on the LVGL task (muse_apps.h). apps_task.h has the contract; this is how
 * the three kinds of task meet.
 *
 * Who touches what. The app task alone runs Lua and owns the runtime, the package and its own state (T below). The
 * LVGL task alone makes LVGL calls (apps_lv), and owns its own state (L). They meet in the record (record_t, in
 * PSRAM): word-sized atomics with one writer each (hung's two take turns), read and written with plain loads and
 * stores (no read-modify-write: PSRAM doesn't promise one), and the rest under a lock. The LVGL task never waits for
 * the app task: it posts with no wait and takes locks with a 2 ms try. The app task waits for the LVGL task only with
 * a bound (a reset, a pass of the apply step). The callers wait for the app task with a bound and poll, so a hung app
 * task can't hold them.
 *
 * The locks. None is held across an allocation, a call into the app, or a wait for another task's progress (a reset,
 * an answer); the record's and a request's are held while apps_ui_dump takes the UI lock, which the LVGL task holds
 * for one pass of the apply step at most.
 * - s_rec_lock: the record's status, taken by the app task to write it and by apps_task_status to copy it;
 * - s_req_lock: a request's last fields and the caller's buffers, which the app task writes only while the caller
 *   still waits; a caller that gives up marks the request abandoned under it, and the app task writes nothing more;
 * - s_names_lock: what the LVGL task reads of the app (its name, the resident app, a crash card to show);
 * - apps_port_ui_lock: the widget table (apps_ui.h), and the table's pointer in apps_lv (apps_lv_set_ui).
 * The order, where two are held: s_req_lock or s_rec_lock, then the UI lock.
 *
 * An app's end, the reset and the package's bytes: apps_task.h. The reset is a pair of counters: the app task asks
 * (reset_req), the LVGL task runs apps_lv_reset in muse_apps_ui_supervise, before any apply in the frame and while the
 * screen sleeps, and says so (reset_done). A table is published only while no reset is outstanding, so no pass meets
 * a new table before the old one's objects are gone. A package whose reset didn't come in time is parked, and freed
 * by the app task once reset_done reaches it.
 */

static const char *TAG = "muse_apps";

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 56 KB of PSRAM (the amended constraint): Task 4's review measured luaL_Buffer-heavy recursion at about 990 B per C
 * level on Xtensa. */
#define APPS_TASK_STACK 57344
#define APPS_TASK_PRIORITY 2       /* under the draw threads, at 3 */
#define APPS_TASK_HUNG_PRIORITY 1  /* a hung call's: whatever it spins on, the rest comes first */
#define APPS_TASK_CORE 1
#define APPS_QUEUE_LEN 16
#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define RECEIVE_MS 1000      /* C22: the loop comes round at least once a second */
#define POLL_MS 10           /* how often a task waiting on another looks again */
#define POST_MS 100          /* a caller's wait for room in the queue */
#define STATUS_LOCK_MS 200   /* apps_task_status's wait for the record */
#define LOCK_MS 1000         /* the app task's for the record and the names, held by others only for copies */
#define NAMES_LOCK_MS 2      /* the LVGL task's: it tries again next frame */
#define REFRESH_MS 1000      /* while an app runs, the record's log and dump are at most this old */
#define SETTLE_MAX_MS 60000
#define FIRST_LINE_MAX 160   /* a crash card's line, and a failed run's */
#define SHOWN_MAX 40         /* text an app supplied, as a message shows it (apps_ui.h) */
#define WHAT_MAX (APPS_PATH_MAX + 32)

#define MSG_HUNG "the app stopped responding; no app can run until its stuck call returns or the gadget restarts"
#define MSG_BUSY "the app task is busy; try again"
#define MSG_DOWN "Muse apps aren't running"
#define MSG_SELF "the app task can't wait for itself"
#define MSG_ASLEEP "layout not checked: the screen was asleep"

static const char *const KINDS[] = {"label", "button", "box", "arc", "bar", "image"};
_Static_assert(ARRAY_LEN(KINDS) == APPS_W_IMAGE + 1, "a name for each kind of widget");

/* ---- Requests ---- */

typedef enum { REQ_RUN, REQ_LAUNCH, REQ_STOP, REQ_COMPILE } req_op_t;

typedef struct {
    const char *path;
    const char *src;
    size_t len;
} code_t;

/* A caller's request (run, stop, compile), which it waits for, or a launch, which nobody waits for. One PSRAM block; a
 * compile's copy of the package's code follows it. */
typedef struct {
    uint8_t op;
    bool front;
    int settle_ms;
    apps_pkg_t *pkg;            /* a run's: the request's until the app task takes it */
    char id[APPS_ID_MAX + 1];   /* a launch's */
    uint16_t memory_kb;         /* a compile's, and its copy of the .lua files, main first */
    int n_code;
    code_t code[APPS_MAX_FILES];
    apps_status_t *st;          /* the caller's buffers */
    char *err;
    size_t err_len;
    /* Under s_req_lock. */
    bool finished, abandoned, ok;
    uint8_t holders;            /* the caller and the app task (a launch: the app task); the last to let go frees it */
} req_t;

/* ---- The record ---- */

typedef struct {
    atomic_uint in_call;       /* app task: a call into the app runs */
    atomic_uint call_started;  /* app task: when it began, apps_port_mono_ms's low 32 bits; stored before in_call */
    atomic_uint app_gen;       /* app task: the current app's number; touches, show and hide carry it */
    atomic_uint published;     /* app task: its table is published */
    atomic_uint waiting;       /* app task: it waits for a reset, with a table to publish or a package to free */
    atomic_uint reset_req;     /* app task: resets asked for */
    atomic_uint reset_done;    /* LVGL task: resets run */
    atomic_uint front_req;     /* app task: (requests << 1) | in front */
    /* The supervisor sets it, and the app task clears it once the hung app is gone: never at once, as the supervisor
     * sets it only during a call, and the app task acts on it only after the call has returned. */
    atomic_uint hung;
    atomic_uint dropped;       /* LVGL task: touches the queue had no room for, since boot */
    /* Under s_rec_lock; the app task, its only writer, reads them without it. */
    apps_status_t st;
    bool loaded;               /* an app is loaded: apps_task_status reads dropped_events live */
    uint32_t dropped_base;     /* dropped when it was launched */
    /* Under s_names_lock. */
    struct {
        bool resident;                      /* the app runs: the launcher lists it */
        char id[APPS_ID_MAX + 1], name[25];
        bool card;                          /* a crash card to show, once reset card_after has run */
        uint32_t card_after;
        char card_name[25], card_line[FIRST_LINE_MAX];
    } names;
} record_t;

static atomic_bool s_up;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static record_t *s_rec;
static StaticSemaphore_t s_rec_lock_buf, s_req_lock_buf, s_names_lock_buf;
static SemaphoreHandle_t s_rec_lock, s_req_lock, s_names_lock;
static atomic_bool s_launcher_wanted;  /* any task asks; the LVGL task shows it */

/* ---- The app task's own ---- */

typedef struct parked {
    struct parked *next;
    apps_pkg_t *pkg;
    uint32_t reset;  /* freed once this reset has run */
} parked_t;

static struct {
    apps_rt_t *rt;
    apps_pkg_t *pkg;
    uint32_t gen;
    bool shown;              /* show() came last, not hide() */
    bool abandoned;          /* it hung: none of its code runs again */
    bool publish;            /* its table waits for a reset to be published */
    req_t *pending;          /* the run that waits for its settle */
    int64_t settle_until;
    char what[WHAT_MAX];     /* what a crash now is, to the pending run */
    int64_t call_began;
    int64_t window;          /* the second the busy time is counted in */
    int64_t window_busy;
    uint8_t cpu_pct;
    uint32_t worst_ms;
    bool dirty;              /* the app ran since the record was refreshed */
    int64_t refresh_at;
    parked_t *parked;
} T;

/* ---- The LVGL task's own ---- */

static struct {
    bool in_turn, front_before_turn, want_front;
    uint32_t front_seen;
    bool hung_card;          /* the supervisor's card, still to show (a voice turn holds it back) */
    char hung_name[25];
} L;

/* ---- Small things ---- */

/* How much of s, which is longer than n bytes, to keep so as to cut at most n bytes on a character boundary. */
static size_t utf8_cut(const char *s, size_t n) {
    for (int i = 0; i < 3 && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80; i++) {
        n--;
    }
    return n;
}

/* text (NULL: "") into out, cap bytes with the NUL, cut on a character boundary. NULL out or no room: nothing. */
static void put_text(char *out, size_t cap, const char *text) {
    if (out == NULL || cap == 0) {
        return;
    }
    text = text != NULL ? text : "";
    size_t n = strlen(text);
    if (n >= cap) {
        n = utf8_cut(text, cap - 1);
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

/* text's first line, cut to fit out. */
static void first_line(const char *text, char *out, size_t cap) {
    text = text != NULL ? text : "";
    size_t n = strcspn(text, "\n");
    if (n >= cap) {
        n = utf8_cut(text, cap - 1);
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

/* item joins out's "; "-separated list (cap bytes), whole or not at all. */
static void add_warning(char *out, size_t cap, const char *item) {
    if (item == NULL || item[0] == '\0') {
        return;
    }
    size_t used = strlen(out), len = strlen(item), sep = used > 0 ? 2 : 0;
    if (used + sep + len >= cap) {
        return;
    }
    memcpy(out + used, "; ", sep);
    memcpy(out + used + sep, item, len + 1);
}

/* s, which the app supplied, as a message shows it (apps_ui.h): valid UTF-8 as it is, a control byte or anything else
 * as '?', and cut at SHOWN_MAX bytes (a character is never split) with "..." after the cut. */
static void shown(char out[SHOWN_MAX + 4], const char *s) {
    const unsigned char *p = (const unsigned char *)s, *end = p + strlen(s);
    size_t used = 0;
    while (p < end) {
        size_t n = 1;
        bool keep = *p >= 0x20 && *p != 0x7F;
        if (*p >= 0x80) {
            int k = apps_json_utf8_len(p, end);
            keep = k > 0;
            n = keep ? (size_t)k : 1;
        }
        size_t take = keep ? n : 1;
        if (used + take > SHOWN_MAX) {
            memcpy(out + used, "...", 3);
            used += 3;
            break;
        }
        if (keep) {
            memcpy(out + used, p, n);
        } else {
            out[used] = '?';
        }
        used += take;
        p += n;
    }
    out[used] = '\0';
}

static uint32_t kb(size_t bytes) {
    return (uint32_t)((bytes + 1023) / 1024);
}

static uint32_t now32(void) {
    return (uint32_t)apps_port_mono_ms();
}

/* The LVGL task has run reset n, or a later one. */
static bool reset_ran(uint32_t n) {
    return (int32_t)(atomic_load(&s_rec->reset_done) - n) >= 0;
}

static void free_pkg(apps_pkg_t *pkg) {
    if (pkg != NULL) {
        apps_pkg_free(pkg, apps_port_free);
        apps_port_free(pkg);
    }
}

static bool rec_take(int ms) {
    return xSemaphoreTake(s_rec_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void rec_give(void) {
    xSemaphoreGive(s_rec_lock);
}

static bool names_take(int ms) {
    return xSemaphoreTake(s_names_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static void names_give(void) {
    xSemaphoreGive(s_names_lock);
}

/* Held only for flags and copies into a caller's buffers, never across a wait for another task. */
static void req_take(void) {
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
}

static void req_give(void) {
    xSemaphoreGive(s_req_lock);
}

/* ---- Requests: both sides ---- */

static req_t *new_req(uint8_t op, size_t extra) {
    req_t *r = heap_caps_calloc(1, sizeof *r + extra, CAPS_PSRAM);
    if (r != NULL) {
        r->op = op;
        r->holders = 2;
    }
    return r;
}

static void free_req(req_t *r) {
    free_pkg(r->pkg);
    heap_caps_free(r);
}

/* A holder lets go of r; the last frees it. */
static void release(req_t *r) {
    req_take();
    bool last = --r->holders == 0;
    req_give();
    if (last) {
        free_req(r);
    }
}

/* Its caller still waits for it. */
static bool wanted(req_t *r) {
    req_take();
    bool still = !r->abandoned;
    req_give();
    return still;
}

/* ---- The record ---- */

/* The current app's status from its runtime, into st: the record's (its lock held) or a caller's copy (the request's
 * lock held). What the launch set (who the app is, its limit, its warnings) stays. */
static void fill_status(apps_status_t *st) {
    apps_vm_t *vm = apps_rt_vm(T.rt);
    apps_ui_t *ui = apps_rt_ui(T.rt);
    st->state = T.abandoned ? APPS_STATE_HUNG : apps_rt_state(T.rt);
    put_text(st->last_error, sizeof st->last_error, apps_rt_last_error(T.rt));
    apps_rt_log_tail(T.rt, INT_MAX, st->log_tail, sizeof st->log_tail);
    int dropped = 0;
    apps_ui_dump(ui, st->ui_dump, sizeof st->ui_dump, &dropped);
    st->ui_truncated = dropped;
    st->instructions = apps_vm_instructions(vm);
    st->mem_kb = kb(apps_vm_mem_used(vm));
    st->peak_kb = kb(apps_vm_mem_peak(vm));
    st->worst_dispatch_ms = T.worst_ms;
    st->cpu_pct = T.cpu_pct;
    st->stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    st->widgets = apps_ui_count(ui);
    st->front = apps_lv_front();
    st->dropped_events = atomic_load(&s_rec->dropped) - s_rec->dropped_base;
}

/* After a run settles, when an app ends, and at most once a second while it runs. */
static void refresh_record(void) {
    if (T.rt == NULL) {
        return;
    }
    if (rec_take(LOCK_MS)) {
        fill_status(&s_rec->st);
        rec_give();
    }
    T.dirty = false;
    T.refresh_at = apps_port_mono_ms() + REFRESH_MS;
}

/* The app task answers r: if its caller still waits, it gets ok, err and the status; then the app task lets go. */
static void complete(req_t *r, bool ok, const char *err) {
    if (r == NULL) {
        return;
    }
    req_take();
    if (!r->abandoned) {
        r->ok = ok;
        put_text(r->err, r->err_len, err);
        if (r->st != NULL) {
            memcpy(r->st, &s_rec->st, sizeof *r->st);
            if (T.rt != NULL && T.dirty) {
                fill_status(r->st);  /* the record is older than what has happened since */
            }
            r->st->front = apps_lv_front();
        }
    }
    r->finished = true;
    bool last = --r->holders == 0;
    req_give();
    if (last) {
        free_req(r);
    }
}

static void set_resident(bool resident) {
    if (names_take(LOCK_MS)) {
        s_rec->names.resident = resident;
        names_give();
    }
}

/* The crash card, for the LVGL task to show once reset `after` has run, so that the reset can't take it down. */
static void post_card(const char *line, uint32_t after) {
    if (names_take(LOCK_MS)) {
        s_rec->names.card = true;
        s_rec->names.card_after = after;
        const char *name = s_rec->st.name[0] != '\0' ? s_rec->st.name : s_rec->st.id;
        put_text(s_rec->names.card_name, sizeof s_rec->names.card_name, name);
        put_text(s_rec->names.card_line, sizeof s_rec->names.card_line, line);
        names_give();
    }
}

/* ---- The table, the reset and the package ---- */

/* waiting asks the LVGL task to wake the app task after a reset. It is set before the app task looks at reset_done, so
 * a reset that runs between the look and the store can't go unnoticed until the loop's next round. */
static void update_waiting(void) {
    atomic_store(&s_rec->waiting, T.publish || T.parked != NULL ? 1u : 0u);
}

/* The current app's table is published once no reset is outstanding: no pass meets it before the last app's objects are
 * gone (apps_lv_reset would take its objects down with them). */
static void try_publish(void) {
    if (!T.publish || T.rt == NULL) {
        return;
    }
    atomic_store(&s_rec->waiting, 1);
    if (reset_ran(atomic_load(&s_rec->reset_req))) {
        apps_port_ui_lock();
        apps_lv_set_ui(apps_rt_ui(T.rt));
        apps_port_ui_unlock();
        atomic_store(&s_rec->published, 1);
        T.publish = false;
    }
    update_waiting();
}

static void unpublish(void) {
    T.publish = false;
    apps_port_ui_lock();
    apps_lv_set_ui(NULL);
    apps_port_ui_unlock();
    atomic_store(&s_rec->published, 0);
    update_waiting();
}

static uint32_t ask_reset(void) {
    uint32_t n = atomic_load(&s_rec->reset_req) + 1;
    atomic_store(&s_rec->reset_req, n);
    return n;
}

static bool wait_reset(uint32_t n) {
    int64_t until = apps_port_mono_ms() + APPS_TASK_RESET_MS;
    while (!reset_ran(n)) {
        if (apps_port_mono_ms() >= until) {
            ESP_LOGW(TAG, "the screen didn't take the app down within %d ms: its package waits for that",
                     APPS_TASK_RESET_MS);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return true;
}

/* Image objects draw from the package's bytes until the reset has run: free it then, never before. */
static void release_pkg(apps_pkg_t *pkg, uint32_t reset) {
    if (pkg == NULL) {
        return;
    }
    atomic_store(&s_rec->waiting, 1);
    if (reset_ran(reset)) {
        free_pkg(pkg);
    } else {
        parked_t *p = heap_caps_malloc(sizeof *p, CAPS_PSRAM);
        if (p != NULL) {
            p->pkg = pkg;
            p->reset = reset;
            p->next = T.parked;
            T.parked = p;
        } else {
            /* Kept for good: never freed early. */
            ESP_LOGE(TAG, "no memory to park %s's package: it is kept", pkg->manifest.id);
        }
    }
    update_waiting();
}

static void free_parked(void) {
    for (parked_t **link = &T.parked; *link != NULL;) {
        parked_t *p = *link;
        if (reset_ran(p->reset)) {
            *link = p->next;
            free_pkg(p->pkg);
            heap_caps_free(p);
        } else {
            link = &p->next;
        }
    }
    update_waiting();
}

/* Every front and back goes through here (C11): the LVGL task carries it out, or defers a front past a voice turn. */
static void request_front(bool front) {
    uint32_t n = (atomic_load(&s_rec->front_req) >> 1) + 1;
    atomic_store(&s_rec->front_req, (n << 1) | (front ? 1u : 0u));
}

/* ---- An app's end ---- */

typedef enum { END_STOPPED, END_REPLACED, END_EXITED, END_CRASHED, END_HUNG } end_t;
static const char *const END_NAMES[] = {"stopped", "replaced", "exited", "crashed", "hung"};

static void finish_run(void);

static void call_begin(void) {
    T.call_began = apps_port_mono_ms();
    atomic_store(&s_rec->call_started, (uint32_t)T.call_began);
    atomic_store(&s_rec->in_call, 1);
}

/* The call is over: the counters. */
static void call_finish(void) {
    atomic_store(&s_rec->in_call, 0);
    int64_t now = apps_port_mono_ms();
    int64_t took = now - T.call_began;
    if (took > (int64_t)T.worst_ms) {
        T.worst_ms = took > UINT32_MAX ? UINT32_MAX : (uint32_t)took;
    }
    if (now / 1000 != T.window) {
        T.cpu_pct = now / 1000 == T.window + 1 ? (uint8_t)(T.window_busy >= 1000 ? 100 : T.window_busy / 10) : 0;
        T.window = now / 1000;
        T.window_busy = 0;
    }
    T.window_busy += took;
    T.dirty = true;
}

/* The hung app is gone: the task goes back to its priority and serves calls again. */
static void recover(void) {
    vTaskPrioritySet(NULL, APPS_TASK_PRIORITY);
    atomic_store(&s_rec->hung, 0);
}

/*
 * The current app ends, in the rulings' order (apps_task.h). stop() runs first for an app asked to end: its last word,
 * before anything is taken from it. Then:
 * 1. the pending run is answered (with its settle's result, or the crash, or the hang);
 * 2. the table is unpublished, so no pass reads it from here on;
 * 4. the record takes the crash record, the log, the dump and the counters;
 * 5. the runtime is freed, and its table with it;
 * 3. the LVGL task takes the app's objects down (apps_lv_reset), and the app task waits for it, bounded;
 * 6. the package's bytes, which image objects drew from, go once that reset has run, parked until it does;
 * 7. a crash card, which the LVGL task shows once that reset has run.
 * The ruling allows 4 and 5 before 3, never 6 or 7. A new run (END_REPLACED) decides the layer's place; any other end
 * puts it behind.
 */
static void end_app(end_t how) {
    if (T.rt == NULL && T.pkg == NULL) {
        if (how == END_HUNG) {
            recover();
        }
        return;
    }
    if (how != END_CRASHED && how != END_HUNG && T.pending != NULL) {
        finish_run();  /* the run settles before its app ends */
    }
    if (how <= END_EXITED && T.rt != NULL && !T.abandoned && apps_rt_state(T.rt) == APPS_STATE_RUNNING) {
        call_begin();
        apps_rt_stop(T.rt);
        call_finish();
        if (atomic_load(&s_rec->hung)) {
            T.abandoned = true;  /* stop() hung, and has come back since */
            how = END_HUNG;
        }
    }
    char line[FIRST_LINE_MAX] = "";
    if (how == END_CRASHED) {
        first_line(apps_rt_last_error(T.rt), line, sizeof line);
    }
    if (T.pending != NULL) {
        char err[WHAT_MAX + FIRST_LINE_MAX + 2];
        snprintf(err, sizeof err, "%s: %s", T.what, line);
        complete(T.pending, false, how == END_HUNG ? MSG_HUNG : err);
        T.pending = NULL;
    }
    unpublish();
    refresh_record();
    if (rec_take(LOCK_MS)) {
        s_rec->loaded = false;
        rec_give();
    }
    ESP_LOGI(TAG, "stop %s: %s, peak %u KB, stack %u free", s_rec->st.id, END_NAMES[how], (unsigned)s_rec->st.peak_kb,
             (unsigned)s_rec->st.stack_free);
    set_resident(false);
    apps_rt_free(T.rt);
    T.rt = NULL;
    uint32_t reset = ask_reset();
    (void)wait_reset(reset);
    release_pkg(T.pkg, reset);
    T.pkg = NULL;
    if (how == END_CRASHED) {
        post_card(line, reset);
    }
    if (how != END_REPLACED) {
        request_front(false);
    }
    if (how == END_HUNG) {
        recover();
    }
}

/* The supervisor marked a call hung and it has come back (or it was over by the time the mark was seen): nothing of the
 * app runs again. */
static void abandon(void) {
    T.abandoned = true;
    end_app(END_HUNG);
}

/* After a call into the app: whether it is still there. A hang the supervisor saw abandons it, a crash contains it. */
static bool call_end(void) {
    call_finish();
    if (atomic_load(&s_rec->hung)) {
        abandon();
        return false;
    }
    if (T.rt != NULL && apps_rt_state(T.rt) == APPS_STATE_CRASHED) {
        end_app(END_CRASHED);
        return false;
    }
    return true;
}

/* ---- A run ---- */

static void begin_app(apps_pkg_t *pkg) {
    const apps_manifest_t *m = &pkg->manifest;
    T.pkg = pkg;
    T.rt = NULL;
    T.shown = T.abandoned = T.publish = T.dirty = false;
    T.worst_ms = 0;
    T.cpu_pct = 0;
    T.window_busy = 0;
    T.gen++;
    atomic_store(&s_rec->app_gen, T.gen);
    if (rec_take(LOCK_MS)) {
        memset(&s_rec->st, 0, sizeof s_rec->st);
        put_text(s_rec->st.id, sizeof s_rec->st.id, m->id);
        put_text(s_rec->st.name, sizeof s_rec->st.name, m->name);
        put_text(s_rec->st.version, sizeof s_rec->st.version, m->version);
        put_text(s_rec->st.warnings, sizeof s_rec->st.warnings, m->warnings);
        s_rec->st.limit_kb = m->memory_kb;
        s_rec->st.state = APPS_STATE_STOPPED;
        s_rec->loaded = true;
        s_rec->dropped_base = atomic_load(&s_rec->dropped);
        rec_give();
    }
    if (names_take(LOCK_MS)) {
        s_rec->names.resident = false;
        put_text(s_rec->names.id, sizeof s_rec->names.id, m->id);
        put_text(s_rec->names.name, sizeof s_rec->names.name, m->name[0] != '\0' ? m->name : m->id);
        names_give();
    }
    ESP_LOGI(TAG, "launch %s (%u KB)", m->id, (unsigned)m->memory_kb);
}

/* main.lua gave no app (it doesn't compile, or there is no room for it): nothing ran to keep, nothing was published,
 * so nothing points into the package and it goes at once. */
static void no_app(const char *err) {
    if (rec_take(LOCK_MS)) {
        s_rec->st.state = APPS_STATE_STOPPED;
        put_text(s_rec->st.last_error, sizeof s_rec->st.last_error, err);
        s_rec->loaded = false;
        rec_give();
    }
    ESP_LOGW(TAG, "launch %s: no app: %s", T.pkg->manifest.id, err);
    free_pkg(T.pkg);
    T.pkg = NULL;
    complete(T.pending, false, err);
    T.pending = NULL;
    request_front(false);
}

/* A pass of the apply step that has laid the app's widgets out: the first pass to end may have begun before the app's
 * last change, the second can't have. False when none came within APPS_TASK_LAYOUT_MS (the screen sleeps) or there is
 * no table to lay out. */
static bool wait_layout(void) {
    if (!atomic_load(&s_rec->published)) {
        return false;
    }
    apps_lv_stats_t stats;
    apps_lv_stats(&stats);
    uint32_t want = stats.applies + 2;
    int64_t until = apps_port_mono_ms() + APPS_TASK_LAYOUT_MS;
    for (;;) {
        apps_lv_stats(&stats);
        if ((int32_t)(stats.applies - want) >= 0) {
            return true;
        }
        if (apps_port_mono_ms() >= until) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* Run step 7: each visible widget past the round edge (Task 11's rule, written back by the apply step under the UI
 * lock) is a warning. The text is the app task's own field, read without the lock. */
static void add_overflow_warnings(char *out, size_t cap) {
    apps_ui_t *ui = apps_rt_ui(T.rt);
    struct {
        uint8_t slot;
        int16_t px;
    } over[APPS_UI_SLOTS];
    int n = 0;
    apps_port_ui_lock();
    for (int slot = 0; slot < APPS_UI_SLOTS; slot++) {
        const apps_widget_t *w = apps_ui_slot(ui, slot);
        if (w != NULL && w->alive && !w->hidden && w->rw > 0 && w->overflow > 0) {
            over[n].slot = (uint8_t)slot;
            over[n].px = w->overflow;
            n++;
        }
    }
    apps_port_ui_unlock();
    for (int i = 0; i < n; i++) {
        const apps_widget_t *w = apps_ui_slot(ui, over[i].slot);
        const char *kind = (size_t)w->kind < ARRAY_LEN(KINDS) ? KINDS[w->kind] : "widget";
        char text[SHOWN_MAX + 4], item[SHOWN_MAX + 96];
        shown(text, w->text != NULL && w->text[0] != '\0' ? w->text : kind);
        snprintf(item, sizeof item, "%s#%d '%s' extends %d px past the round edge", kind, over[i].slot, text,
                 over[i].px);
        add_warning(out, cap, item);
    }
}

/* The pending run has settled. */
static void finish_run(void) {
    req_t *r = T.pending;
    T.pending = NULL;
    if (r == NULL) {
        return;
    }
    char warnings[sizeof s_rec->st.warnings];
    memcpy(warnings, s_rec->st.warnings, sizeof warnings);
    if (wait_layout()) {
        add_overflow_warnings(warnings, sizeof warnings);
    } else {
        add_warning(warnings, sizeof warnings, MSG_ASLEEP);
    }
    if (rec_take(LOCK_MS)) {
        memcpy(s_rec->st.warnings, warnings, sizeof warnings);
        rec_give();
    }
    refresh_record();
    bool ok = T.rt != NULL && apps_rt_state(T.rt) == APPS_STATE_RUNNING;
    complete(r, ok, ok ? NULL : "the app isn't running");
}

/*
 * A run, on the app task: apps_task_run's, with r waiting for the settle, and (Task 13 on) a launch's and the alarm
 * path's, with r NULL. It owns pkg. The current app ends first; the new one's table is published after start() once no
 * reset is outstanding; then the layer goes in front or behind.
 */
static void run_locked(apps_pkg_t *pkg, bool front, req_t *r, int settle_ms) {
    end_app(END_REPLACED);
    begin_app(pkg);
    T.pending = r;
    snprintf(T.what, sizeof T.what, "crashed loading %s", pkg->manifest.main);
    char err[512];
    call_begin();
    T.rt = apps_rt_new(pkg, err, sizeof err);
    if (!call_end()) {
        return;  /* hung, or crashed at main.lua's top level (Task 8's F2): handled as in start() */
    }
    if (T.rt == NULL) {
        no_app(err);
        return;
    }
    put_text(T.what, sizeof T.what, "crashed in start()");
    call_begin();
    (void)apps_rt_start(T.rt);
    if (!call_end()) {
        return;
    }
    put_text(T.what, sizeof T.what, "crashed");
    if (rec_take(LOCK_MS)) {
        add_warning(s_rec->st.warnings, sizeof s_rec->st.warnings, apps_rt_warnings(T.rt));
        rec_give();
    }
    T.publish = true;
    try_publish();
    set_resident(true);
    request_front(front);
    refresh_record();
    T.settle_until = apps_port_mono_ms() + settle_ms;  /* its events run for settle_ms from here (the loop's rounds) */
}

/* ---- The app task's loop ---- */

static bool app_runs(uint32_t gen) {
    return T.rt != NULL && !T.abandoned && gen == T.gen && apps_rt_state(T.rt) == APPS_STATE_RUNNING;
}

/* Until Task 13's RAM store, the only app there is to launch is the one that runs. */
static void launch(const req_t *r) {
    if (app_runs(T.gen) && strcmp(T.pkg->manifest.id, r->id) == 0) {
        if (r->front) {
            request_front(true);
        }
    } else {
        ESP_LOGW(TAG, "launch %s: no such app on the gadget", r->id);
    }
}

static void on_run(req_t *r) {
    if (r->op == REQ_LAUNCH) {
        launch(r);
        release(r);
        return;
    }
    apps_pkg_t *pkg = r->pkg;
    r->pkg = NULL;  /* the app task's now */
    if (!wanted(r)) {
        free_pkg(pkg);  /* its caller gave up before it began */
        complete(r, false, NULL);
        return;
    }
    run_locked(pkg, r->front, r, r->settle_ms);
}

static bool compile(const req_t *r, char *err, size_t err_len) {
    apps_vm_config_t cfg = {
        .mem_limit = (size_t)r->memory_kb * 1024u,
        .realloc_fn = apps_port_realloc,
        .free_fn = apps_port_free,
        .now_ms = apps_port_mono_ms,
    };
    apps_vm_t *vm = apps_vm_new(&cfg);
    if (vm == NULL) {
        snprintf(err, err_len, "out of memory: can't make the app's heap of %u KB", (unsigned)r->memory_kb);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < r->n_code && ok; i++) {
        ok = apps_vm_load(vm, r->code[i].path, r->code[i].src, r->code[i].len, err, err_len);
        if (ok) {
            lua_pop(apps_vm_lua(vm), 1);
        }
    }
    apps_vm_free(vm);
    return ok;
}

static void on_compile(req_t *r) {
    char err[512] = "";
    bool ok = wanted(r) && compile(r, err, sizeof err);
    complete(r, ok, ok ? NULL : err);
}

static void handle(const apps_event_t *ev) {
    switch (ev->type) {
    case APPS_EV_TOUCH:
        if (app_runs(ev->app_gen)) {
            call_begin();
            apps_rt_touch(T.rt, ev->slot, ev->gen, ev->code);
            (void)call_end();
        }
        break;
    case APPS_EV_SHOW:
    case APPS_EV_HIDE: {
        bool show = ev->type == APPS_EV_SHOW;
        if (app_runs(ev->app_gen) && T.shown != show) {
            T.shown = show;
            call_begin();
            apps_rt_show(T.rt, show);
            (void)call_end();
        }
        break;
    }
    case APPS_EV_RUN:
        on_run(ev->req);
        break;
    case APPS_EV_STOP:
        end_app(END_STOPPED);
        complete(ev->req, true, NULL);
        break;
    case APPS_EV_COMPILE:
        on_compile(ev->req);
        break;
    case APPS_EV_EXIT:
        if (T.rt != NULL && ev->app_gen == T.gen) {
            end_app(END_EXITED);
        }
        break;
    default:
        break;  /* WAKE: the round's housekeeping is the work. MESSAGE and ALARM arrive with Tasks 13 and 17. */
    }
}

/* Every round of the loop, whatever woke it. */
static void housekeeping(void) {
    if (atomic_load(&s_rec->hung)) {
        abandon();  /* marked when its call had already come back */
    }
    free_parked();
    try_publish();
    if (T.pending != NULL && apps_port_mono_ms() >= T.settle_until) {
        finish_run();
    }
    if (T.rt != NULL && T.dirty && apps_port_mono_ms() >= T.refresh_at) {
        refresh_record();
    }
}

static int receive_ms(void) {
    if (T.pending == NULL) {
        return RECEIVE_MS;
    }
    int64_t left = T.settle_until - apps_port_mono_ms();
    return left <= 0 ? 0 : left < RECEIVE_MS ? (int)left : RECEIVE_MS;
}

static void apps_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "up: stack %u in PSRAM", (unsigned)APPS_TASK_STACK);
    T.window = apps_port_mono_ms() / 1000;
    for (;;) {
        apps_event_t ev;
        bool got = xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(receive_ms())) == pdTRUE;
        housekeeping();
        if (got) {
            handle(&ev);
        }
    }
}

/* ---- The LVGL task ---- */

static void post_from_lvgl(uint8_t type) {
    apps_event_t ev = {.type = type, .app_gen = atomic_load(&s_rec->app_gen)};
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "the app task's queue is full: %s() is lost", type == APPS_EV_SHOW ? "show" : "hide");
    }
}

/* A touch on a widget, from apps_lv's callback: posted with no wait, and counted when there is no room. */
static void on_widget_event(uint16_t slot, uint16_t gen, uint8_t event) {
    if (!atomic_load(&s_up)) {
        return;
    }
    apps_event_t ev = {
        .type = APPS_EV_TOUCH, .code = event, .slot = slot, .gen = gen, .app_gen = atomic_load(&s_rec->app_gen),
    };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        atomic_store(&s_rec->dropped, atomic_load(&s_rec->dropped) + 1);
    }
}

/* A right swipe on the layer, inside LVGL's gesture event. It only backgrounds (Task 11's ruling): no teardown, and no
 * wait for the app task, which may be waiting for this task. */
static void on_close(void) {
    if (!atomic_load(&s_up)) {
        return;
    }
    apps_lv_set_front(false);
    post_from_lvgl(APPS_EV_HIDE);
}

/* The launcher's pick, inside the row's tap. */
static void on_pick(const char *id) {
    apps_task_launch(id, true);
}

static void front_now(void) {
    apps_lv_hide_cards();  /* opening an app closes the launcher (C12), and a card would cover it */
    if (!apps_lv_front()) {
        apps_lv_set_front(true);
    }
    post_from_lvgl(APPS_EV_SHOW);
}

/* The supervisor: a call that has run for more than APPS_TASK_HUNG_MS is hung. */
static void watch_for_hang(void) {
    if (atomic_load(&s_rec->hung) || !atomic_load(&s_rec->in_call)) {
        return;
    }
    uint32_t started = atomic_load(&s_rec->call_started);  /* read after in_call: this call's start, or a later one's */
    if ((uint32_t)(now32() - started) <= APPS_TASK_HUNG_MS) {
        return;
    }
    atomic_store(&s_rec->hung, 1);
    apps_lv_set_front(false);
    L.want_front = L.front_before_turn = false;
    vTaskPrioritySet(s_task, APPS_TASK_HUNG_PRIORITY);
    put_text(L.hung_name, sizeof L.hung_name, "The app");
    if (names_take(NAMES_LOCK_MS)) {
        if (s_rec->names.name[0] != '\0') {
            put_text(L.hung_name, sizeof L.hung_name, s_rec->names.name);
        }
        names_give();
    }
    ESP_LOGW(TAG, "%s stopped responding", L.hung_name);
    L.hung_card = true;
}

static void run_reset(void) {
    uint32_t asked = atomic_load(&s_rec->reset_req);
    if (asked == atomic_load(&s_rec->reset_done)) {
        return;
    }
    apps_lv_reset();
    atomic_store(&s_rec->reset_done, asked);
    if (atomic_load(&s_rec->waiting)) {
        apps_event_t ev = {.type = APPS_EV_WAKE};
        (void)xQueueSend(s_queue, &ev, 0);  /* with the queue full the app task is awake anyway */
    }
}

static void follow_front(void) {
    uint32_t asked = atomic_load(&s_rec->front_req);
    if (asked == L.front_seen) {
        return;
    }
    L.front_seen = asked;
    if ((asked & 1u) == 0) {
        if (apps_lv_front()) {
            apps_lv_set_front(false);
        }
        L.want_front = L.front_before_turn = false;
    } else if (atomic_load(&s_rec->hung)) {
        /* a hung app stays behind */
    } else if (L.in_turn) {
        L.want_front = true;
    } else {
        front_now();
    }
}

/* The cards the app task and other tasks asked for: never during a voice turn. */
static void show_cards(void) {
    if (L.hung_card) {
        L.hung_card = false;
        apps_lv_show_crash(L.hung_name, NULL);
    }
    bool launcher = atomic_load(&s_launcher_wanted);
    bool card = false, resident = false;
    char name[25] = "", line[FIRST_LINE_MAX] = "", id[APPS_ID_MAX + 1] = "", app[25] = "";
    if (!names_take(NAMES_LOCK_MS)) {
        return;  /* next frame */
    }
    if (s_rec->names.card && reset_ran(s_rec->names.card_after)) {
        card = true;
        s_rec->names.card = false;
        put_text(name, sizeof name, s_rec->names.card_name);
        put_text(line, sizeof line, s_rec->names.card_line);
    }
    if (launcher) {
        resident = s_rec->names.resident && !atomic_load(&s_rec->hung);
        put_text(id, sizeof id, s_rec->names.id);
        put_text(app, sizeof app, s_rec->names.name);
    }
    names_give();
    if (card) {
        apps_lv_show_crash(name, line);
    }
    if (launcher) {
        atomic_store(&s_launcher_wanted, false);
        const char *ids[] = {id};
        const char *names[] = {app};
        apps_lv_show_launcher(ids, names, resident ? 1 : 0, on_pick);
    }
}

void muse_apps_ui_build(lv_obj_t *screen) {
    apps_lv_build(screen);
    apps_lv_set_event_cb(on_widget_event);
    apps_lv_set_close_cb(on_close);
}

void muse_apps_ui_supervise(void) {
    if (!atomic_load(&s_up)) {
        return;
    }
    watch_for_hang();
    run_reset();
    follow_front();
    if (!L.in_turn) {
        show_cards();
    }
}

bool muse_apps_ui_tick(void) {
    if (!atomic_load(&s_up)) {
        return false;
    }
    if (!atomic_load(&s_rec->hung)) {
        bool published = atomic_load(&s_rec->published) != 0;
        (void)apps_lv_apply(published && atomic_load(&s_rec->in_call) != 0);
    }
    return apps_lv_front() || apps_lv_cards_shown();
}

void muse_apps_ui_turn(bool in_turn) {
    if (!atomic_load(&s_up) || in_turn == L.in_turn) {
        return;
    }
    L.in_turn = in_turn;
    if (in_turn) {
        L.front_before_turn = apps_lv_front();
        L.want_front = false;
        apps_lv_hide_cards();
        if (L.front_before_turn) {
            apps_lv_set_front(false);
            post_from_lvgl(APPS_EV_HIDE);
        }
        return;
    }
    bool again = (L.want_front || L.front_before_turn) && atomic_load(&s_rec->published) && !atomic_load(&s_rec->hung);
    L.want_front = L.front_before_turn = false;
    if (again) {
        front_now();
    }
}

void muse_apps_open_launcher(void) {
    atomic_store(&s_launcher_wanted, true);
}

bool muse_apps_front(void) {
    return apps_lv_front();
}

/* ---- The callers ---- */

/* Why a call can't be made now, or NULL. A caller that waits can't be the app task, which would wait for itself. */
static const char *refusal(bool waits) {
    if (!atomic_load(&s_up)) {
        return MSG_DOWN;
    }
    if (waits && xTaskGetCurrentTaskHandle() == s_task) {
        return MSG_SELF;
    }
    if (atomic_load(&s_rec->hung)) {
        return MSG_HUNG;
    }
    return NULL;
}

static bool post_req(uint8_t type, req_t *r, int wait_ms) {
    apps_event_t ev = {.type = type, .req = r};
    return xQueueSend(s_queue, &ev, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

/* Waits for r's answer, at most ms, and not at all once the app hangs. True when it came (it is in the caller's
 * buffers). Otherwise r is abandoned, so the app task writes nothing more to them, and err says why. */
static bool await(req_t *r, int ms, char *err, size_t err_len) {
    int64_t until = apps_port_mono_ms() + ms;
    for (;;) {
        bool hung = atomic_load(&s_rec->hung) != 0;
        bool late = apps_port_mono_ms() >= until;
        req_take();
        bool finished = r->finished;
        if (!finished && (hung || late)) {
            r->abandoned = true;
        }
        req_give();
        if (finished) {
            return true;
        }
        if (hung) {
            put_text(err, err_len, MSG_HUNG);
            return false;
        }
        if (late) {
            char text[64];
            snprintf(text, sizeof text, "the app task didn't answer within %d ms", ms);
            put_text(err, err_len, text);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len) {
    put_text(err, err_len, "");
    if (pkg == NULL) {
        put_text(err, err_len, "no package to run");
        return false;
    }
    const char *why = refusal(true);
    req_t *r = why == NULL ? new_req(REQ_RUN, 0) : NULL;
    if (why == NULL && r == NULL) {
        why = "out of memory: no room for the request";
    }
    if (why != NULL) {
        free_pkg(pkg);
        put_text(err, err_len, why);
        if (st != NULL) {
            apps_task_status(st);
        }
        return false;
    }
    r->pkg = pkg;
    r->front = front;
    r->settle_ms = settle_ms < 0 ? 0 : settle_ms > SETTLE_MAX_MS ? SETTLE_MAX_MS : settle_ms;
    r->st = st;
    r->err = err;
    r->err_len = err_len;
    int wait_ms = r->settle_ms + APPS_TASK_RUN_EXTRA_MS;
    if (!post_req(APPS_EV_RUN, r, POST_MS)) {
        free_req(r);
        put_text(err, err_len, MSG_BUSY);
        if (st != NULL) {
            apps_task_status(st);
        }
        return false;
    }
    bool answered = await(r, wait_ms, err, err_len);
    bool ok = answered && r->ok;
    if (!answered && st != NULL) {
        apps_task_status(st);
    }
    release(r);
    return ok;
}

static void add_code(req_t *r, char **at, const apps_blob_t *b) {
    code_t *c = &r->code[r->n_code++];
    size_t n = strlen(b->path) + 1;
    memcpy(*at, b->path, n);
    c->path = *at;
    *at += n;
    if (b->len > 0) {
        memcpy(*at, b->data, b->len);
    }
    c->src = *at;
    c->len = b->len;
    *at += b->len;
}

bool apps_task_compile(const apps_pkg_t *pkg, char *err, size_t err_len) {
    put_text(err, err_len, "");
    if (pkg == NULL) {
        put_text(err, err_len, "no package to compile");
        return false;
    }
    const apps_blob_t *main = NULL;
    bool listed = false;
    size_t bytes = 0;
    for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
        const apps_blob_t *b = &pkg->files[i];
        bool is_main = strcmp(b->path, pkg->manifest.main) == 0;
        listed = listed || is_main;
        if (apps_file_kind(b->path) == APPS_FILE_LUA) {
            bytes += strlen(b->path) + 1 + b->len;
            main = is_main ? b : main;
        }
    }
    if (main == NULL) {
        char text[APPS_PATH_MAX + 40];
        snprintf(text, sizeof text, listed ? "%s is not a .lua file" : "%s is missing from the package",
                 pkg->manifest.main);
        put_text(err, err_len, text);
        return false;
    }
    const char *why = refusal(true);
    req_t *r = why == NULL ? new_req(REQ_COMPILE, bytes) : NULL;
    if (why == NULL && r == NULL) {
        why = "out of memory: no room to copy the app's code";
    }
    if (why != NULL) {
        put_text(err, err_len, why);
        return false;
    }
    r->memory_kb = pkg->manifest.memory_kb;
    char *at = (char *)(r + 1);
    add_code(r, &at, main);
    for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
        const apps_blob_t *b = &pkg->files[i];
        if (b != main && apps_file_kind(b->path) == APPS_FILE_LUA) {
            add_code(r, &at, b);
        }
    }
    r->err = err;
    r->err_len = err_len;
    if (!post_req(APPS_EV_COMPILE, r, POST_MS)) {
        free_req(r);
        put_text(err, err_len, MSG_BUSY);
        return false;
    }
    bool ok = await(r, APPS_TASK_WAIT_MS, err, err_len) && r->ok;
    release(r);
    return ok;
}

void apps_task_launch(const char *id, bool front) {
    if (id == NULL || !apps_id_valid(id)) {
        ESP_LOGW(TAG, "launch: not an app id");
        return;
    }
    const char *why = refusal(false);
    req_t *r = why == NULL ? new_req(REQ_LAUNCH, 0) : NULL;
    if (why != NULL || r == NULL) {
        ESP_LOGW(TAG, "launch %s: %s", id, why != NULL ? why : "out of memory");
        return;
    }
    r->holders = 1;
    r->front = front;
    put_text(r->id, sizeof r->id, id);
    if (!post_req(APPS_EV_RUN, r, 0)) {
        ESP_LOGW(TAG, "launch %s: %s", id, MSG_BUSY);
        free_req(r);
    }
}

void apps_task_stop(void) {
    if (refusal(true) != NULL) {
        return;
    }
    req_t *r = new_req(REQ_STOP, 0);
    if (r == NULL) {
        return;
    }
    if (!post_req(APPS_EV_STOP, r, POST_MS)) {
        free_req(r);
        return;
    }
    (void)await(r, APPS_TASK_WAIT_MS, NULL, 0);
    release(r);
}

void apps_task_status(apps_status_t *st) {
    if (st == NULL) {
        return;
    }
    if (!atomic_load(&s_up)) {
        memset(st, 0, sizeof *st);
        return;
    }
    uint32_t dropped = atomic_load(&s_rec->dropped);
    if (rec_take(STATUS_LOCK_MS)) {
        memcpy(st, &s_rec->st, sizeof *st);
        if (s_rec->loaded) {
            st->dropped_events = dropped - s_rec->dropped_base;
        }
        rec_give();
    } else {
        memset(st, 0, sizeof *st);  /* the record was busy for that long: what is read without it */
    }
    st->front = apps_lv_front();
    if (atomic_load(&s_rec->hung)) {
        st->state = APPS_STATE_HUNG;
    }
}

esp_err_t muse_apps_start(void) {
    if (s_task != NULL) {
        return ESP_OK;
    }
    apps_port_init();
    s_rec_lock = xSemaphoreCreateMutexStatic(&s_rec_lock_buf);
    s_req_lock = xSemaphoreCreateMutexStatic(&s_req_lock_buf);
    s_names_lock = xSemaphoreCreateMutexStatic(&s_names_lock_buf);
    s_rec = heap_caps_calloc(1, sizeof *s_rec, CAPS_PSRAM);
    s_queue = xQueueCreateWithCaps(APPS_QUEUE_LEN, sizeof(apps_event_t), MALLOC_CAP_SPIRAM);
    if (s_rec != NULL && s_queue != NULL &&
        xTaskCreatePinnedToCoreWithCaps(apps_task, "muse_apps", APPS_TASK_STACK, NULL, APPS_TASK_PRIORITY, &s_task,
                                        APPS_TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        atomic_store(&s_up, true);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "no memory for the app task");
    if (s_queue != NULL) {
        vQueueDeleteWithCaps(s_queue);
    }
    heap_caps_free(s_rec);
    s_queue = NULL;
    s_rec = NULL;
    s_task = NULL;
    return ESP_ERR_NO_MEM;
}
