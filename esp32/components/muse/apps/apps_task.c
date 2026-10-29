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

#include "apps_alarm.h"
#include "apps_io.h"
#include "apps_json.h"
#include "apps_lv.h"
#include "apps_port.h"
#include "apps_store.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "muse_apps.h"
#include "muse_input.h"
#include "muse_state.h"

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
 * - s_names_lock: what the LVGL task reads of the apps (the app that runs, the store's list for the launcher, a crash
 *   card to show), written by the app task and by the store's owner, the I/O worker;
 * - apps_port_ui_lock: the widget table (apps_ui.h), and the table's pointer in apps_lv (apps_lv_set_ui).
 * The order, where two are held: s_req_lock or s_rec_lock, then the UI lock.
 *
 * An app's end, the reset and the package's bytes: apps_task.h. The reset is a pair of counters: the app task asks
 * (reset_req), the LVGL task runs apps_lv_reset in muse_apps_ui_supervise, before any apply in the frame and while the
 * screen sleeps, and says so (reset_done). A table is published only while no reset is outstanding, so no pass meets
 * a new table before the old one's objects are gone. Only an app whose table was published asks for a reset when it
 * ends; its package, if the reset didn't come in time, is parked, and freed by the app task once reset_done reaches
 * it. So at most one package is parked: until its reset has run no table is published, and an app that ends with its
 * table unpublished frees its package at once.
 *
 * The layer's place. The app task asks (front_req, tagged with the asking app); the LVGL task carries it out in
 * muse_apps_ui_supervise, holds a front back during a voice turn and until the app's table is published, and decides
 * at the turn's end, after it has seen the frame's requests. What it knows of an app goes when another begins.
 *
 * The echo. An app's print only queues its line (apps_port_log); the app task prints the queue after every call and in
 * its housekeeping (apps_port_log_drain), never inside a call, so a console another task holds never counts against an
 * app's wall clock.
 *
 * The alarms. They are the system's (apps_alarm.h): the esp_timer task marks one fired, rings and wakes the screen, and posts
 * APPS_EV_ALARM with no payload and no wait (on_alarm_fired: once while the loop hasn't looked, and never into the last REQ_ROOM
 * slots); the app task delivers on every round, whatever woke it (deliver_alarms), so a post that finds the queue full loses
 * nothing: the alarm keeps its slot in the table, and the loop comes round at least once a second (C22). An alarm of the app that
 * runs fronts it (request_front, C11) and calls alarm(name); any other is a launch of that app (the I/O worker's job, C5:
 * muse_apps_launch_alarm) that the alarm waits for, in its slot, until the app runs, and then it is delivered as above, after
 * start(). A launch has an identity, the gen of the last firing it was asked for, and the worker reports each outcome with a post
 * (APPS_EV_ALARM_LAUNCH): one the worker refused is asked for again each second and given up after APPS_TASK_ALARM_REFUSED_MS, one it
 * took is waited for until APPS_TASK_ALARM_BACKSTOP_MS, and a launch whose alarms are gone runs nothing. Every app's end tells the
 * alarms (end_alarms): the ring ends with the app that was told it, and an app that its alarm launched and that crashed loses its
 * alarms. What they record to log, the app task prints (drain_logs: in housekeeping and after each call, never inside one).
 */

static const char *TAG = "muse_apps";

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 56 KB of PSRAM (the amended constraint): Task 4's review measured luaL_Buffer-heavy recursion at about 990 B per C
 * level on Xtensa. */
#define APPS_TASK_STACK 57344
#define APPS_TASK_PRIORITY 2       /* under the draw threads, at 3 */
#define APPS_TASK_HUNG_PRIORITY 1  /* a hung call's: whatever it spins on, the rest comes first */
#define APPS_TASK_CORE 1
#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define RECEIVE_MS 1000      /* C22: the loop comes round at least once a second */
#define POLL_MS 10           /* how often a task waiting on another looks again */
#define REQ_ROOM 2           /* slots of the queue a touch leaves to requests */
#define STATUS_LOCK_MS 200   /* apps_task_status's wait for the record */
#define LOCK_MS 1000         /* the app task's for the record and the names, held by others only for copies */
#define NAMES_LOCK_MS 2      /* the LVGL task's: it tries again next frame */
#define REFRESH_MS 1000      /* while an app runs, the record's log and dump are at most this old */
#define SETTLE_MAX_MS 60000
#define ALARM_RETRY_MS 1000  /* a launch the I/O worker refused is asked for again this much later */
#define KEEP_AWAKE_POKE_MS 1000  /* while an app keeps the screen awake and is in front, the idle timer is poked this often */
#define ALARM_PASSES 3       /* looks at the table in one round: another comes when telling an app ended it, or a launch was given up */
#define FIRST_LINE_MAX 160   /* a crash card's line, and a failed run's */
#define SHOWN_MAX 40         /* text an app supplied, as a message shows it (apps_ui.h) */
#define WHAT_MAX (APPS_PATH_MAX + 32)

#define NO_WARNED ((size_t)-1)

#define MSG_HUNG "the app stopped responding; no app can run until its stuck call returns or the gadget restarts"
#define MSG_BUSY "the app task is busy; try again"
#define MSG_DOWN "Muse apps aren't running"
#define MSG_SELF "the app task can't wait for itself"
#define MSG_ASLEEP "layout not checked: the screen was asleep"
#define MSG_EXITED "the app exited (sys.exit)"

static const char *const KINDS[] = {"label", "button", "box", "arc", "bar", "image"};
_Static_assert(ARRAY_LEN(KINDS) == APPS_W_IMAGE + 1, "a name for each kind of widget");

/* ---- Requests ---- */

typedef enum { REQ_RUN, REQ_LAUNCH, REQ_STOP, REQ_COMPILE, REQ_MESSAGE } req_op_t;

/* A caller's request (run, stop, compile, message), which it waits for, or a launch, which nobody waits for. One PSRAM
 * block; a compile's copy of the package's code follows it, or a message's event and data. */
typedef struct {
    uint8_t op;
    bool front;                 /* a run's and a launch's place; a message's show */
    int settle_ms;
    apps_pkg_t *pkg;            /* a run's: the request's until the app task takes it */
    char id[APPS_ID_MAX + 1];   /* a launch's and a message's */
    uint16_t memory_kb;         /* a compile's, and its copy of the .lua files, main first */
    int n_code;
    apps_code_t code[APPS_MAX_FILES];
    const char *event, *data;   /* a message's, in the block: NUL-terminated, data data_len bytes */
    size_t data_len;
    bool by_id;                 /* a run's: a launch by id, which fronts an app of pkg's id that runs (F10) */
    uint32_t alarm_upto;        /* a run's: the identity of a launch for alarms (apps_alarm_wanted), or 0 */
    apps_status_t *st;          /* the caller's buffers */
    char *err;
    size_t err_len;
    /* Under s_req_lock. */
    bool finished, abandoned, ok;
    char *reply;                /* a message's answer: the caller takes it; the block's until then */
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
    atomic_uint front_req;     /* app task: the last front or back request (front_word) */
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
        bool running;                       /* the app below runs: from when it is up (start() has returned) until
                                             * it ends */
        char id[APPS_ID_MAX + 1], name[25]; /* the last app launched, and the name it shows (the hung card's too) */
        int list_n;                         /* the store's apps, most recent first (apps_task_set_list) */
        apps_listed_t list[APPS_TASK_LIST_MAX];
        bool card;                          /* a crash card to show, once reset card_after has run */
        uint32_t card_after;
        char card_name[25], card_line[FIRST_LINE_MAX];
    } names;
    apps_listed_t rows[APPS_LV_LAUNCHER_ROWS];  /* the LVGL task's alone: the launcher's rows, copied out of names */
} record_t;

static atomic_bool s_up;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static record_t *s_rec;
static StaticSemaphore_t s_rec_lock_buf, s_req_lock_buf, s_names_lock_buf;
static SemaphoreHandle_t s_rec_lock, s_req_lock, s_names_lock;
static atomic_bool s_launcher_wanted;  /* any task asks; the LVGL task shows it */
static atomic_bool s_alarm_posted;     /* the esp_timer task posted APPS_EV_ALARM and the loop hasn't looked at the table since */

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
    bool was_published;      /* its table was published: objects may draw from its package */
    req_t *pending;          /* the run that waits for its settle */
    int64_t settle_until;
    char what[WHAT_MAX];     /* what a crash now is, to the pending run */
    int64_t call_began;
    uint8_t cpu_pct;         /* what the runtime last said of the share of a window its calls took (apps_rt_cpu_pct) */
    uint32_t worst_ms;
    bool dirty;              /* the app ran since the record was refreshed */
    int64_t refresh_at;
    size_t warned;           /* the bytes of apps_rt_warnings that the record has taken; NO_WARNED until the run has taken
                              * main.lua's own, after start() */
    parked_t *parked;
    /* The launch for alarms in flight (one app at a time): its app ("" for none), its identity (the gen of the last firing it was asked
     * for, which apps_alarm_wanted answers for), when it was first asked for, when a refused one may be asked for again, and whether the
     * worker took it (queued, since alarm_queued_at): then only its result, or the backstop, ends the wait. */
    char alarm_app[APPS_ID_MAX + 1];
    uint32_t alarm_upto;
    int64_t alarm_since, alarm_retry, alarm_queued_at;
    bool alarm_queued;
    bool run_alarm;           /* the app that runs was launched for its alarms, and no delivery round has come since it began (P1) */
    bool exit_asked;          /* sys.exit() was called in the call that runs: the app ends when it returns (call_end) */
    bool keep_awake;          /* sys.keep_awake(true): while the layer is in front, the idle timer is poked each second */
    int64_t poke_at;          /* the earliest the next poke may be (apps_port_mono_ms) */
} T;

/* ---- The LVGL task's own ---- */

static struct {
    bool in_turn;
    bool dark;               /* the screen was dark at the last frame (apps_port_dark) */
    bool turn_ended;         /* the next supervise decides the layer's place */
    uint32_t gen;            /* the app (app_gen) the two below are about */
    bool front_before_turn;  /* it was in front when the voice turn began */
    bool want_front;         /* it asked for the front: carried out out of a turn, once its table is published */
    uint32_t front_seen;
    bool hung_card;          /* the supervisor's card, still to show (a voice turn holds it back) */
    uint32_t hung_gen;       /* the app it is about */
    char hung_name[25];
} L;

static atomic_bool s_covering;  /* the LVGL task's: the layer is in front or a card is shown */

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

/* What was recorded to be logged, printed by the app task outside any call (its wall clock must not run for the console, which
 * another task's long reply can hold): the alarms' (what the esp_timer task did, and the calls that changed the table), then the
 * apps' echo. */
static void drain_logs(void) {
    apps_alarm_log_drain();
    apps_port_log_drain();
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
    apps_port_free(r->reply);
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

/* The app task answers r: if its caller still waits, it gets ok, err, the status and a message's reply (which the app
 * task frees otherwise); then the app task lets go. */
static void complete_with(req_t *r, bool ok, const char *err, char *reply) {
    if (r == NULL) {
        apps_port_free(reply);
        return;
    }
    req_take();
    bool waits = !r->abandoned;
    if (waits) {
        r->ok = ok;
        r->reply = reply;
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
    if (!waits) {
        apps_port_free(reply);
    }
    if (last) {
        free_req(r);
    }
}

static void complete(req_t *r, bool ok, const char *err) {
    complete_with(r, ok, err, NULL);
}

/* The app task's word on the app that runs, for the launcher and the store (apps_task_running). */
static void set_running(bool running) {
    if (names_take(LOCK_MS)) {
        s_rec->names.running = running;
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
        T.was_published = true;
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

/* A front or back request is one word, so the LVGL task reads it whole: a count (only a change matters), the low bits
 * of the asking app's app_gen (the LVGL task drops a request of an app that has ended since), and front. */
#define FRONT_GEN_BITS 16
#define FRONT_GEN_MASK ((1u << FRONT_GEN_BITS) - 1u)

static uint32_t front_word(uint32_t count, uint32_t gen, bool front) {
    return (count << (FRONT_GEN_BITS + 1)) | ((gen & FRONT_GEN_MASK) << 1) | (front ? 1u : 0u);
}

/* Every front and back of the current app goes through here (C11): the LVGL task carries it out, holding a front back
 * during a voice turn and until the app's table is published. */
static void request_front(bool front) {
    uint32_t count = (atomic_load(&s_rec->front_req) >> (FRONT_GEN_BITS + 1)) + 1;
    atomic_store(&s_rec->front_req, front_word(count, T.gen, front));
}

/* ---- An app's end ---- */

typedef enum { END_STOPPED, END_REPLACED, END_EXITED, END_CRASHED, END_HUNG } end_t;
static const char *const END_NAMES[] = {"stopped", "replaced", "exited", "crashed", "hung"};

static void finish_run(void);

/* What the app's end means to the system's alarms (apps_alarm.h), the app being the one in the record. An app that its alarm launched
 * and that crashed (or hung) in start() or in the alarm() it was launched for loses every alarm it has (P1): it would arm its next in
 * start() before it raised, and the alarm would launch it again, for ever. Then the end is told, with how, so that a ring ends with the
 * app that was told it unless a firing with sound still waits. The window of the first closes with the app (and at the end of the
 * first delivery round after the run, whichever comes first). */
static void end_alarms(end_t how) {
    const char *id = s_rec->st.id;
    if (T.run_alarm && (how == END_CRASHED || how == END_HUNG)) {
        apps_alarm_cancel_app(id, how == END_HUNG ? "hung when its alarm launched it" : "crashed when its alarm launched it");
    }
    T.run_alarm = false;
    char why[24];
    snprintf(why, sizeof why, "app %s", END_NAMES[how]);
    apps_alarm_app_ended(id, why);
}

/* The time of the app's calls is counted by its runtime, in the windows of the duty cycle (whole seconds of apps_port_mono_ms:
 * apps_rt.h), where the same booking decides what is hot: cpu_pct is the share of the last whole window that its calls took. The
 * loop asks every round, at least once a second (C22), so an idle app's cpu_pct falls to 0 (and the windows that have ended are
 * closed, so the duty cycle's warning and kill come on time); the record takes a change at once. */
static void roll_window(void) {
    if (T.rt == NULL || T.abandoned) {
        return;
    }
    uint8_t pct = apps_rt_cpu_pct(T.rt, apps_port_mono_ms());
    if (pct != T.cpu_pct) {
        T.cpu_pct = pct;
        if (rec_take(LOCK_MS)) {
            s_rec->st.cpu_pct = pct;
            rec_give();
        }
    }
}

static void call_begin(void) {
    T.call_began = apps_port_mono_ms();
    atomic_store(&s_rec->call_started, (uint32_t)T.call_began);
    atomic_store(&s_rec->in_call, 1);
}

/* The call is over: the counters (its time is the runtime's to book). */
static void call_finish(void) {
    atomic_store(&s_rec->in_call, 0);
    int64_t took = apps_port_mono_ms() - T.call_began;
    if (took > (int64_t)T.worst_ms) {
        T.worst_ms = took > UINT32_MAX ? UINT32_MAX : (uint32_t)took;
    }
    T.dirty = true;
    drain_logs();  /* the call's echo, and the alarms' lines, now that its wall clock has stopped */
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
 * The ruling allows 4 and 5 before 3, never 6 or 7. Only an app whose table was published can have objects up: any
 * other skips 3, its package goes at once in 6, and its card waits for the reset that is outstanding, if one is. A new
 * run (END_REPLACED) decides the layer's place; any other end puts it behind.
 */
static void end_app(end_t how) {
    if (T.rt == NULL && T.pkg == NULL) {
        if (how == END_HUNG) {
            recover();
        }
        return;
    }
    if (how != END_CRASHED && how != END_HUNG && how != END_EXITED && T.pending != NULL) {
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
        /* An app that called sys.exit() before its run settled (at main.lua's top level, in start(), or in the settle's first
         * moments) didn't give a running app: the run says so, and an alarm's launch of it is given up on like any that didn't run. */
        complete(T.pending, false, how == END_HUNG ? MSG_HUNG : how == END_EXITED ? MSG_EXITED : err);
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
    end_alarms(how);
    set_running(false);  /* the launcher lists it still while the store has it */
    apps_rt_free(T.rt);  /* saves the store if a change is left in it (stop() has saved it when the app stopped) */
    T.rt = NULL;
    T.exit_asked = T.keep_awake = false;
    uint32_t reset;
    if (T.was_published) {
        reset = ask_reset();
        (void)wait_reset(reset);
        release_pkg(T.pkg, reset);
    } else {
        reset = atomic_load(&s_rec->reset_req);  /* the outstanding one, if any: a card mustn't come before it */
        free_pkg(T.pkg);
    }
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

/* The warnings the runtime adds to a RUNNING app after the run began (the duty cycle's: they can come with a call or with a
 * pass of the timers, which makes none), into the record's. The warnings main.lua's table gave are the run's (run_locked). */
static void merge_warnings(void) {
    if (T.rt == NULL || T.warned == NO_WARNED || apps_rt_state(T.rt) != APPS_STATE_RUNNING) {
        return;
    }
    const char *all = apps_rt_warnings(T.rt);
    size_t have = strlen(all);
    if (have > T.warned && rec_take(LOCK_MS)) {
        const char *fresh = all + T.warned;
        add_warning(s_rec->st.warnings, sizeof s_rec->st.warnings, strncmp(fresh, "; ", 2) == 0 ? fresh + 2 : fresh);
        rec_give();
        T.warned = have;
    }
}

/* After a call into the app: whether it is still there. A hang the supervisor saw abandons it, a crash contains it. */
static bool call_end(void) {
    call_finish();
    if (atomic_load(&s_rec->hung)) {
        abandon();
        return false;
    }
    merge_warnings();
    if (T.rt != NULL && apps_rt_state(T.rt) == APPS_STATE_CRASHED) {
        end_app(END_CRASHED);
        return false;
    }
    if (T.exit_asked && T.rt != NULL) {
        end_app(END_EXITED);  /* sys.exit(): the callback has run to its end, and nothing else of the app runs after it */
        return false;
    }
    return true;
}

/* ---- A run ---- */

static void begin_app(apps_pkg_t *pkg) {
    const apps_manifest_t *m = &pkg->manifest;
    T.pkg = pkg;
    T.rt = NULL;
    T.shown = T.abandoned = T.publish = T.was_published = T.dirty = false;
    T.warned = NO_WARNED;
    T.worst_ms = 0;
    T.cpu_pct = 0;
    T.exit_asked = T.keep_awake = false;
    T.poke_at = 0;
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
        s_rec->names.running = false;  /* running once it is up */
        s_rec->names.card = false;  /* an earlier app's crash card not shown yet would come up over this one */
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
    set_running(false);  /* the store keeps it: another launch may find the room */
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
 * reset is outstanding; then the layer goes in front or behind. alarm: the app is launched for its alarms, whose delivery round
 * (the one that follows this event) is the window in which its crash costs it its alarms (end_alarms).
 */
static void run_locked(apps_pkg_t *pkg, bool front, req_t *r, int settle_ms, bool alarm) {
    end_app(END_REPLACED);
    if (r != NULL && !wanted(r)) {
        /* Its caller gave up while the old app ended (its stop() hung, or the waits ran long): the run is dropped, as from
         * the queue, and the layer goes behind with nothing on it. */
        free_pkg(pkg);
        complete(r, false, NULL);
        request_front(false);
        return;
    }
    begin_app(pkg);
    T.run_alarm = alarm;
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
    T.warned = strlen(apps_rt_warnings(T.rt));  /* later ones are merge_warnings' */
    T.publish = true;
    try_publish();
    set_running(true);  /* up: the launcher lists it first if it must, and the store never drops it */
    request_front(front);
    refresh_record();
    T.settle_until = apps_port_mono_ms() + settle_ms;  /* its events run for settle_ms from here (the loop's rounds) */
}

/* ---- The app task's loop ---- */

static bool app_runs(uint32_t gen) {
    return T.rt != NULL && !T.abandoned && gen == T.gen && apps_rt_state(T.rt) == APPS_STATE_RUNNING;
}

/* A launch of the app id: the app that runs is fronted here (front); any other goes to the I/O worker (muse_apps_launch),
 * whose job runs the store's copy through apps_task_run_by_id. True when it is done or queued; false for a launch the worker
 * refused, which is logged there, once. The launcher's pick and the alarms' share it. */
static bool launch_id(const char *id, bool front) {
    if (app_runs(T.gen) && strcmp(T.pkg->manifest.id, id) == 0) {
        if (front) {
            request_front(true);
        }
        return true;
    }
    return muse_apps_launch(id, front);
}

/* The launcher's pick, or any apps_task_launch. */
static void launch(const req_t *r) {
    (void)launch_id(r->id, r->front);
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
    if (r->alarm_upto != 0 && !apps_alarm_wanted(pkg->manifest.id, r->alarm_upto)) {
        /* The alarms it was launched for were told, cancelled or given up on while it waited for the worker (or the worker loaded it):
         * nothing is replaced for them, and no app comes to the front. */
        free_pkg(pkg);
        complete(r, false, APPS_TASK_ALARM_GONE);
        return;
    }
    if (r->by_id && app_runs(T.gen) && strcmp(T.pkg->manifest.id, pkg->manifest.id) == 0) {
        /* A launch by id of the app that runs by now (F10): fronted (or left where it is), not started again. */
        free_pkg(pkg);
        if (r->front) {
            request_front(true);
        }
        complete(r, true, NULL);
        return;
    }
    run_locked(pkg, r->front, r, r->settle_ms, r->alarm_upto != 0);
}

static void on_compile(req_t *r) {
    char err[512] = "";
    bool ok = wanted(r) && apps_rt_compile(r->code, r->n_code, r->memory_kb, err, sizeof err);
    complete(r, ok, ok ? NULL : err);
}

/* message(event, data) to the app that runs, if it is r's: its reply, or why not. A handler that was killed or ran out
 * of memory crashed the app, which ends as on any crash; its own error leaves it running. */
static void on_message(req_t *r) {
    if (!wanted(r)) {
        complete(r, false, NULL);
        return;
    }
    char err[APPS_ERROR_MAX] = "";
    if (!app_runs(T.gen) || strcmp(T.pkg->manifest.id, r->id) != 0) {
        snprintf(err, sizeof err, "%s isn't running", r->id);
        complete(r, false, err);
        return;
    }
    call_begin();
    char *reply = apps_rt_message(T.rt, r->event, r->data, r->data_len, err, sizeof err);
    bool alive = call_end();
    if (reply != NULL && alive && r->front) {
        request_front(true);  /* after the voice turn, if one is on: the LVGL task decides */
    }
    complete_with(r, reply != NULL, reply != NULL ? NULL : err, reply);
}

static void on_alarm_launch(uint8_t outcome, uint32_t upto);

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
    case APPS_EV_MESSAGE:
        on_message(ev->req);
        break;
    case APPS_EV_ALARM_LAUNCH:
        on_alarm_launch(ev->code, ev->app_gen);
        break;
    default:
        break;  /* WAKE: the round's housekeeping is the work. ALARM too: deliver_alarms, every round, is the delivery. */
    }
}

/* ---- Alarms ---- */

/* The alarm f is told to the app that runs (it checked): the alarm leaves its slot, the layer is asked to the front (after the
 * voice turn, if one is on), and alarm(name) is called, which is a call of the app like any other. A crash in it ends the app as
 * any crash does, and end_app tells the alarms (a ring ends with the app that was told it). */
static void tell_alarm(const apps_alarm_fired_t *f) {
    if (!apps_alarm_take(f, true)) {
        return;  /* cancelled, replaced or given up on since it was copied: nothing to tell */
    }
    request_front(true);
    call_begin();
    (void)apps_rt_alarm(T.rt, f->name);
    (void)call_end();
}

/* Whether gen is later than upto (both are firings' gens, which wrap: compared as a difference). */
static bool gen_after(uint32_t gen, uint32_t upto) {
    return (int32_t)(gen - upto) > 0;
}

static void alarm_launch_clear(void) {
    T.alarm_app[0] = '\0';
    T.alarm_upto = 0;
    T.alarm_queued = false;
}

typedef enum { DROP_MISSING, DROP_FAILED, DROP_REFUSED, DROP_BACKSTOP } drop_t;

/* The firings of app that a launch was asked for (gens up to upto) are given up on: taken and not delivered, one log line each,
 * and a ring they started ends if nothing else could stop it (apps_alarm_take). A firing later than the launch's is not its own. */
static void drop_alarms(const char *app, uint32_t upto, drop_t why) {
    apps_alarm_fired_t due[APPS_ALARMS_MAX];
    int n = apps_alarm_fired(due, APPS_ALARMS_MAX);
    for (int i = 0; i < n; i++) {
        if (strcmp(due[i].app, app) != 0 || gen_after(due[i].gen, upto) || !apps_alarm_take(&due[i], false)) {
            continue;
        }
        switch (why) {
        case DROP_MISSING:
            ESP_LOGW(TAG, "alarm %s for missing app %s", due[i].name, app);
            break;
        case DROP_FAILED:
            ESP_LOGW(TAG, "alarm %s: %s did not start: dropped", due[i].name, app);
            break;
        case DROP_REFUSED:
            ESP_LOGW(TAG, "alarm %s: %s did not start within %d ms: dropped", due[i].name, app, APPS_TASK_ALARM_REFUSED_MS);
            break;
        case DROP_BACKSTOP:
            ESP_LOGW(TAG, "alarm %s: %s did not start within %d ms: dropped", due[i].name, app, APPS_TASK_ALARM_BACKSTOP_MS);
            break;
        }
    }
}

/* The worker's report on the launch with identity upto (APPS_EV_ALARM_LAUNCH). One for a launch that isn't the one in flight (it was
 * given up on, or its alarms went, and another began) says nothing to this task. */
static void on_alarm_launch(uint8_t outcome, uint32_t upto) {
    if (T.alarm_app[0] == '\0' || T.alarm_upto != upto) {
        return;
    }
    char app[APPS_ID_MAX + 1];
    snprintf(app, sizeof app, "%s", T.alarm_app);
    alarm_launch_clear();
    if (outcome == APPS_ALARM_LAUNCH_MISSING) {
        drop_alarms(app, upto, DROP_MISSING);
    } else if (outcome == APPS_ALARM_LAUNCH_FAILED) {
        drop_alarms(app, upto, DROP_FAILED);
    }  /* started: the app runs, and the next delivery tells it (it may have, already) */
}

/* A launch for the alarms of due[first]'s app begins: its identity is the latest firing of that app in the copy. */
static void begin_launch(const apps_alarm_fired_t *due, int n, int first, int64_t now) {
    put_text(T.alarm_app, sizeof T.alarm_app, due[first].app);
    T.alarm_upto = due[first].gen;
    for (int j = first + 1; j < n; j++) {
        if (strcmp(due[j].app, T.alarm_app) == 0 && gen_after(due[j].gen, T.alarm_upto)) {
            T.alarm_upto = due[j].gen;
        }
    }
    T.alarm_since = now;
    T.alarm_retry = 0;
    T.alarm_queued = false;
}

/* One look at the firings, earliest first. Those of the app that runs are told. For the others a launch is asked for, one app at a
 * time (the first firing whose app doesn't run begins it; the other apps wait their turn, all of them in their slots): the I/O worker
 * loads the app and runs it in front (muse_apps_launch_alarm), and its report ends the wait. A launch the worker refused is asked
 * for again ALARM_RETRY_MS after, and given up on after APPS_TASK_ALARM_REFUSED_MS; one it took is waited for until the backstop.
 * True when the table should be looked at again: telling a firing ended the app (what that cancelled is not asked for), or a launch
 * was given up (the next firing begins its own). */
static bool deliver_pass(void) {
    apps_alarm_fired_t due[APPS_ALARMS_MAX];
    int n = apps_alarm_fired(due, APPS_ALARMS_MAX);
    int64_t now = apps_port_mono_ms();
    bool waiting = false;  /* a firing of the app being launched for is still in its slot */
    for (int i = 0; i < n; i++) {
        if (app_runs(T.gen) && strcmp(T.pkg->manifest.id, due[i].app) == 0) {
            uint32_t gen = T.gen;
            tell_alarm(&due[i]);
            if (!app_runs(gen)) {
                return true;
            }
            continue;
        }
        if (T.alarm_app[0] != '\0' && strcmp(T.alarm_app, due[i].app) != 0) {
            continue;  /* another app's launch goes first */
        }
        if (T.alarm_app[0] == '\0') {
            begin_launch(due, n, i, now);
        }
        waiting = true;
        if (T.alarm_queued) {
            if (now - T.alarm_queued_at > APPS_TASK_ALARM_BACKSTOP_MS) {
                drop_alarms(T.alarm_app, T.alarm_upto, DROP_BACKSTOP);
                alarm_launch_clear();
                return true;
            }
        } else if (now - T.alarm_since > APPS_TASK_ALARM_REFUSED_MS) {
            drop_alarms(T.alarm_app, T.alarm_upto, DROP_REFUSED);
            alarm_launch_clear();
            return true;
        } else if (now >= T.alarm_retry) {  /* the first time at once, and again no sooner than ALARM_RETRY_MS after a refusal */
            T.alarm_queued = muse_apps_launch_alarm(T.alarm_app, T.alarm_upto);
            T.alarm_queued_at = now;
            T.alarm_retry = now + ALARM_RETRY_MS;
        }
    }
    if (!waiting) {
        alarm_launch_clear();
    }
    return false;
}

/*
 * Every round: the fired alarms (apps_alarm_fired, which copies and changes nothing, after a lock-free look at the pending word)
 * are looked at as deliver_pass says, at most ALARM_PASSES times. The flag that the esp_timer task's post sets is cleared first,
 * before the table is read, so a firing that comes after the look posts again, and one that came before it is seen by the look.
 */
static void deliver_alarms(void) {
    atomic_store(&s_alarm_posted, false);
    for (int pass = 0; pass < ALARM_PASSES; pass++) {
        if (!apps_alarm_pending()) {
            alarm_launch_clear();
            return;
        }
        if (!deliver_pass()) {
            return;
        }
    }
}

/* The esp_timer task's (apps_alarm_set_fired_cb): an alarm fired. It posts, with no wait and no payload, to bring the loop round
 * at once, and only one is on its way at a time (a flood of firings is one post: the loop's next look finds them all). It leaves the
 * queue's last REQ_ROOM slots to requests, as a touch does; a post that doesn't go in loses nothing, as the alarm stays in its slot
 * for the round that comes within a second, and the next firing may post again. */
static void on_alarm_fired(const char *app, const char *name) {
    (void)app;
    (void)name;
    if (atomic_exchange(&s_alarm_posted, true)) {
        return;
    }
    apps_event_t ev = {.type = APPS_EV_ALARM};
    if (!(uxQueueSpacesAvailable(s_queue) > REQ_ROOM && xQueueSend(s_queue, &ev, 0) == pdTRUE)) {
        atomic_store(&s_alarm_posted, false);
    }
}

/* Every round of the loop, whatever woke it. */
static void housekeeping(void) {
    if (atomic_load(&s_rec->hung)) {
        abandon();  /* marked when its call had already come back */
    }
    roll_window();
    free_parked();
    try_publish();
    if (T.pending != NULL && apps_port_mono_ms() >= T.settle_until) {
        finish_run();
    }
    if (T.rt != NULL && T.dirty && apps_port_mono_ms() >= T.refresh_at) {
        refresh_record();
    }
    if (T.keep_awake && T.rt != NULL && !T.abandoned && apps_port_mono_ms() >= T.poke_at && apps_lv_front()) {
        muse_state_poke();  /* sys.keep_awake(true): the screen isn't let go dark for idling while the app is in front */
        T.poke_at = apps_port_mono_ms() + KEEP_AWAKE_POKE_MS;
    }
    apps_store_tick(apps_port_mono_ms());  /* the saves the worker hasn't taken yet, with or without an app (apps_store.h) */
    drain_logs();
}

/* How long the loop waits for an event: until the pending run settles or the app's earliest timer is due, and never more
 * than RECEIVE_MS (C22), which is also how long it waits while the screen is dark and no timer can run. A timer that is due
 * already still waits a millisecond, which wait_ticks makes a tick: a task that came straight back would never let the idle task
 * run. 0 only for a run whose settle is over. */
static int receive_ms(void) {
    int64_t now = apps_port_mono_ms();
    int64_t wait = RECEIVE_MS;
    if (T.pending != NULL) {
        int64_t left = T.settle_until - now;
        wait = left <= 0 ? 0 : left < RECEIVE_MS ? left : RECEIVE_MS;
    }
    if (T.rt != NULL && !T.abandoned) {
        int64_t due = apps_rt_next_deadline(T.rt);
        if (due != INT64_MAX) {
            int64_t until = due - now < 1 ? 1 : due - now;
            wait = until < wait ? until : wait;
        }
    }
    return (int)wait;
}

/* ms in ticks, rounded up, and at least one for a wait above 0. pdMS_TO_TICKS rounds down, so where a tick is longer than a
 * millisecond (ESP-IDF's default is 100 Hz: 10 ms; the board's is 1000) a wait of 1 to 9 ms would be no wait at all, and the loop
 * would go straight round for those milliseconds, a task at priority 2 that never lets the idle task run. 0 is a settle that
 * is over. */
static TickType_t wait_ticks(int ms) {
    return ms <= 0 ? 0 : (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999u) / 1000u);
}

/* The timer that is due runs, as a call of the app like any other: bracketed for the supervisor and the status, and an
 * error in it ends the app as an error in a handler does (the card, the record). One a round, so that a touch or a stop
 * waits behind one callback and not behind all of them; the next one that is due comes a tick later (receive_ms). Every round
 * asks, which also closes the duty cycle's second (apps_rt_has_work), so its warning and its kill come on time. */
static void run_timers(void) {
    if (T.rt == NULL || T.abandoned) {
        return;
    }
    int64_t now = apps_port_mono_ms();
    apps_rt_store_tick(T.rt, now);  /* a save of the store that is due: no call of the app, so no bracket (the port only queues it) */
    if (apps_rt_has_work(T.rt, now)) {
        call_begin();
        (void)apps_rt_run_one_timer(T.rt, now);
        (void)call_end();
    } else {
        merge_warnings();  /* a pass that made no call can still have warned */
    }
}

static void apps_task(void *arg) {
    (void)arg;
    apps_io_register_op_task();  /* the store's loads and saves are ops of the I/O worker, and only this task submits them (apps_io.h) */
    ESP_LOGI(TAG, "up: stack %u in PSRAM", (unsigned)APPS_TASK_STACK);
    for (;;) {
        apps_event_t ev;
        bool got = xQueueReceive(s_queue, &ev, wait_ticks(receive_ms())) == pdTRUE;
        housekeeping();
        if (got) {
            handle(&ev);
        }
        deliver_alarms();
        T.run_alarm = false;  /* the delivery round that followed a run for alarms is over: a crash from here on is only a crash */
        run_timers();
    }
}

/* ---- The LVGL task ---- */

static void post_from_lvgl(uint8_t type, uint32_t app_gen) {
    apps_event_t ev = {.type = type, .app_gen = app_gen};
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "the app task's queue is full: %s() is lost", type == APPS_EV_SHOW ? "show" : "hide");
    }
}

/* The layer is in front or a card is shown: muse_apps_covering's flag, set at each change this file makes and at every
 * frame (a card's own tap closes it between frames). */
static bool update_covering(void) {
    bool covering = apps_lv_front() || apps_lv_cards_shown();
    atomic_store(&s_covering, covering);
    return covering;
}

/* A touch on a widget, from apps_lv's callback: posted with no wait, and counted when there is no room. It leaves the
 * last REQ_ROOM slots to requests, so a stop gets in however fast the taps come. */
static void on_widget_event(uint16_t slot, uint16_t gen, uint8_t event) {
    if (!atomic_load(&s_up)) {
        return;
    }
    apps_event_t ev = {
        .type = APPS_EV_TOUCH, .code = event, .slot = slot, .gen = gen, .app_gen = atomic_load(&s_rec->app_gen),
    };
    if (uxQueueSpacesAvailable(s_queue) <= REQ_ROOM || xQueueSend(s_queue, &ev, 0) != pdTRUE) {
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
    post_from_lvgl(APPS_EV_HIDE, atomic_load(&s_rec->app_gen));
    update_covering();
}

/* The launcher's pick, inside the row's tap. */
static void on_pick(const char *id) {
    apps_task_launch(id, true);
}

/* Fronts L's app: show() goes to that app alone. */
static void front_now(void) {
    apps_lv_hide_cards();  /* opening an app closes the launcher (C12), and a card would cover it */
    if (!apps_lv_front()) {
        apps_lv_set_front(true);
    }
    post_from_lvgl(APPS_EV_SHOW, L.gen);
}

/* The screen was dark and is lit: the timers that came due while it slept are overdue, and the app task's loop, which waits a
 * second for an event while the screen is dark (no timer can run, so no deadline shortens its wait), is told at once, with a
 * no-op event. Without it the loop notices the wake at its next round, up to that second, and an app in front shows its
 * frame from before the sleep (a clock, its old minute) all that time. Posted with no wait, as everything the LVGL task
 * posts: with the queue full the app task is awake anyway. Seen at a frame, whichever way the screen is: update_sleep clears
 * the flag later in the frame than the supervisor runs, so it is the next frame's that sees the edge. */
static void note_wake(void) {
    bool dark = apps_port_dark();
    if (L.dark && !dark) {
        apps_event_t ev = {.type = APPS_EV_WAKE};
        (void)xQueueSend(s_queue, &ev, 0);
    }
    L.dark = dark;
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
    L.hung_gen = atomic_load(&s_rec->app_gen);  /* the app task, stuck in the call, can't move on from it */
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

/* The app task's last request, when it is new: a back is carried out at once, a front is noted for carry_out_front. What
 * L knew of an app goes when another has begun, and a request of an app that has ended since is dropped. */
static void follow_front(void) {
    uint32_t asked = atomic_load(&s_rec->front_req);
    uint32_t gen = atomic_load(&s_rec->app_gen);  /* read after: the asking app's, or a later one's */
    if (gen != L.gen) {
        L.gen = gen;
        L.want_front = L.front_before_turn = false;
    }
    if (asked == L.front_seen) {
        return;
    }
    L.front_seen = asked;
    if (((asked >> 1) & FRONT_GEN_MASK) != (gen & FRONT_GEN_MASK)) {
        return;
    }
    if ((asked & 1u) == 0) {
        if (apps_lv_front()) {
            apps_lv_set_front(false);
        }
        L.want_front = L.front_before_turn = false;
    } else if (!atomic_load(&s_rec->hung)) {
        L.want_front = true;  /* a hung app stays behind */
    }
}

/* Out of a voice turn, after follow_front has seen the frame's requests: a turn's end fronts the app again if it was in
 * front when the turn began, and a front the app asked for is carried out once its table is published. */
static void carry_out_front(void) {
    if (L.turn_ended) {
        L.turn_ended = false;
        L.want_front = L.want_front || L.front_before_turn;
        L.front_before_turn = false;
    }
    if (L.want_front && atomic_load(&s_rec->published) && !atomic_load(&s_rec->hung) &&
        atomic_load(&s_rec->app_gen) == L.gen) {  /* read after published: that table is L's app's */
        L.want_front = false;
        front_now();
    }
}

/*
 * The launcher's rows, into s_rec->rows (the names lock held): the store's apps, most recent first, as many as there
 * are rows, with the app that runs on the first row when they don't have it (the store never drops it, but a run that
 * didn't come from the store, as on the host, isn't in it), so the launcher can always bring it back.
 */
static int launcher_rows(void) {
    const int n = s_rec->names.list_n;
    const apps_listed_t *list = s_rec->names.list;
    bool listed = !s_rec->names.running;
    for (int i = 0; i < n && i < APPS_LV_LAUNCHER_ROWS && !listed; i++) {
        listed = strcmp(list[i].id, s_rec->names.id) == 0;
    }
    int rows = 0;
    if (!listed) {
        put_text(s_rec->rows[0].id, sizeof s_rec->rows[0].id, s_rec->names.id);
        put_text(s_rec->rows[0].name, sizeof s_rec->rows[0].name, s_rec->names.name);
        rows = 1;
    }
    for (int i = 0; i < n && rows < APPS_LV_LAUNCHER_ROWS; i++) {
        if (listed || strcmp(list[i].id, s_rec->names.id) != 0) {
            s_rec->rows[rows++] = list[i];
        }
    }
    return rows;
}

/* The cards the app task and other tasks asked for: never during a voice turn. */
static void show_cards(void) {
    if (L.hung_card) {
        L.hung_card = false;
        if (L.hung_gen == L.gen) {  /* an app that began since drops it, as it does a crash card */
            apps_lv_show_crash(L.hung_name, NULL);
        }
    }
    bool launcher = atomic_load(&s_launcher_wanted);
    bool card = false;
    int rows = 0;
    char name[25] = "", line[FIRST_LINE_MAX] = "";
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
        rows = launcher_rows();
    }
    names_give();
    if (card) {
        apps_lv_show_crash(name, line);
    }
    if (launcher) {
        atomic_store(&s_launcher_wanted, false);
        const char *ids[APPS_LV_LAUNCHER_ROWS], *names[APPS_LV_LAUNCHER_ROWS];
        for (int i = 0; i < rows; i++) {
            ids[i] = s_rec->rows[i].id;
            names[i] = s_rec->rows[i].name;
        }
        apps_lv_show_launcher(ids, names, rows, on_pick);  /* it copies them */
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
        carry_out_front();
        show_cards();
    }
    note_wake();
    update_covering();
}

bool muse_apps_ui_tick(void) {
    if (!atomic_load(&s_up)) {
        return false;
    }
    if (!atomic_load(&s_rec->hung)) {
        bool published = atomic_load(&s_rec->published) != 0;
        (void)apps_lv_apply(published && atomic_load(&s_rec->in_call) != 0);
    }
    return update_covering();
}

void muse_apps_ui_turn(bool in_turn) {
    if (!atomic_load(&s_up) || in_turn == L.in_turn) {
        return;
    }
    L.in_turn = in_turn;
    if (in_turn) {
        L.turn_ended = false;
        bool front = apps_lv_front();
        /* The app was in front only if its table was up: after a new run the layer stays in front, with nothing of the
         * new app on it, until the app's own request decides. */
        L.front_before_turn = front && atomic_load(&s_rec->published);
        apps_lv_hide_cards();
        if (front) {
            apps_lv_set_front(false);
            post_from_lvgl(APPS_EV_HIDE, atomic_load(&s_rec->app_gen));
        }
    } else {
        L.turn_ended = true;  /* muse_apps_ui_supervise decides, once it has seen the frame's requests */
    }
    update_covering();
}

void muse_apps_open_launcher(void) {
    atomic_store(&s_launcher_wanted, true);
}

bool muse_apps_front(void) {
    return apps_lv_front();
}

bool muse_apps_covering(void) {
    return atomic_load(&s_covering);
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

/* Posts r, waiting for room in the queue until `until` (apps_port_mono_ms), and no longer once the app hangs: NULL once
 * it is in the queue, otherwise why not. Touches leave REQ_ROOM slots to requests. */
static const char *post_req(uint8_t type, req_t *r, int64_t until) {
    apps_event_t ev = {.type = type, .req = r};
    for (;;) {
        if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            return NULL;
        }
        if (atomic_load(&s_rec->hung)) {
            return MSG_HUNG;
        }
        if (apps_port_mono_ms() >= until) {
            return MSG_BUSY;
        }
    }
}

/* Waits for r's answer until `until`, the end of the caller's bound of ms, and not at all once the app hangs. True when
 * it came (it is in the caller's buffers). Otherwise r is abandoned, so the app task writes nothing more to them, and
 * err says why. */
static bool await(req_t *r, int64_t until, int ms, char *err, size_t err_len) {
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
        apps_io_assist();  /* a job of the I/O worker that waits here runs the ops the app task may be waiting for (the app's store) */
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static bool run_request(apps_pkg_t *pkg, bool front, bool by_id, uint32_t alarm_upto, int settle_ms, apps_status_t *st,
                        char *err, size_t err_len) {
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
    r->by_id = by_id;
    r->alarm_upto = alarm_upto;
    r->settle_ms = settle_ms < 0 ? 0 : settle_ms > SETTLE_MAX_MS ? SETTLE_MAX_MS : settle_ms;
    r->st = st;
    r->err = err;
    r->err_len = err_len;
    int wait_ms = r->settle_ms + APPS_TASK_RUN_EXTRA_MS;
    int64_t until = apps_port_mono_ms() + wait_ms;
    const char *busy = post_req(APPS_EV_RUN, r, until);
    if (busy != NULL) {
        free_req(r);
        put_text(err, err_len, busy);
        if (st != NULL) {
            apps_task_status(st);
        }
        return false;
    }
    bool answered = await(r, until, wait_ms, err, err_len);
    bool ok = answered && r->ok;
    if (!answered && st != NULL) {
        apps_task_status(st);
    }
    release(r);
    return ok;
}

bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len) {
    return run_request(pkg, front, false, 0, settle_ms, st, err, err_len);
}

bool apps_task_run_by_id(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len) {
    return run_request(pkg, front, true, 0, settle_ms, st, err, err_len);
}

bool apps_task_run_for_alarm(apps_pkg_t *pkg, uint32_t upto, apps_status_t *st, char *err, size_t err_len) {
    return run_request(pkg, true, true, upto, 0, st, err, err_len);
}

void apps_task_alarm_launch_result(uint32_t upto, apps_alarm_launch_t outcome) {
    if (!atomic_load(&s_up) || upto == 0 || xTaskGetCurrentTaskHandle() == s_task) {
        return;
    }
    apps_event_t ev = {.type = APPS_EV_ALARM_LAUNCH, .code = (uint8_t)outcome, .app_gen = upto};
    int64_t until = apps_port_mono_ms() + APPS_TASK_WAIT_MS;
    while (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(POLL_MS)) != pdTRUE) {
        if (atomic_load(&s_rec->hung) || apps_port_mono_ms() >= until) {
            ESP_LOGW(TAG, "the app task's queue is full: a launch's result is lost (its alarms are given up on later)");
            return;
        }
    }
}

static void add_code(req_t *r, char **at, const apps_blob_t *b) {
    apps_code_t *c = &r->code[r->n_code++];
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
    const apps_blob_t *order[APPS_MAX_FILES];
    int n = apps_pkg_compile_order(pkg, order);
    size_t bytes = 0;
    for (int i = 0; i < n; i++) {
        bytes += strlen(order[i]->path) + 1 + order[i]->len;
    }
    if (n == 0 || strcmp(order[0]->path, pkg->manifest.main) != 0) {  /* main is first when the package has it as Lua */
        bool listed = false;
        for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
            listed = listed || strcmp(pkg->files[i].path, pkg->manifest.main) == 0;
        }
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
    for (int i = 0; i < n; i++) {
        add_code(r, &at, order[i]);
    }
    r->err = err;
    r->err_len = err_len;
    int64_t until = apps_port_mono_ms() + APPS_TASK_WAIT_MS;
    const char *busy = post_req(APPS_EV_COMPILE, r, until);
    if (busy != NULL) {
        free_req(r);
        put_text(err, err_len, busy);
        return false;
    }
    bool ok = await(r, until, APPS_TASK_WAIT_MS, err, err_len) && r->ok;
    release(r);
    return ok;
}

void apps_task_set_list(const apps_listed_t *apps, int n) {
    if (!atomic_load(&s_up)) {
        return;
    }
    n = apps == NULL || n < 0 ? 0 : n > APPS_TASK_LIST_MAX ? APPS_TASK_LIST_MAX : n;
    if (!names_take(LOCK_MS)) {
        ESP_LOGW(TAG, "the launcher's list wasn't updated: the names were busy");
        return;
    }
    for (int i = 0; i < n; i++) {
        put_text(s_rec->names.list[i].id, sizeof s_rec->names.list[i].id, apps[i].id);
        put_text(s_rec->names.list[i].name, sizeof s_rec->names.list[i].name, apps[i].name);
    }
    s_rec->names.list_n = n;
    names_give();
}

bool apps_task_running(char *id, size_t cap) {
    bool running = false;
    put_text(id, cap, "");
    if (atomic_load(&s_up) && names_take(LOCK_MS)) {
        running = s_rec->names.running;
        if (running) {
            put_text(id, cap, s_rec->names.id);
        }
        names_give();
    }
    return running;
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
    apps_event_t ev = {.type = APPS_EV_RUN, .req = r};
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "launch %s: %s", id, MSG_BUSY);
        free_req(r);
    }
}

bool apps_task_stop(char *err, size_t err_len) {
    char why[128] = "";
    const char *refused = refusal(true);
    req_t *r = refused == NULL ? new_req(REQ_STOP, 0) : NULL;
    bool ok = false;
    if (refused != NULL || r == NULL) {
        put_text(why, sizeof why, refused != NULL ? refused : "out of memory: no room for the request");
    } else {
        r->err = why;  /* the caller's own buffer, which outlives its wait */
        r->err_len = sizeof why;
        int64_t until = apps_port_mono_ms() + APPS_TASK_WAIT_MS;
        const char *busy = post_req(APPS_EV_STOP, r, until);
        if (busy != NULL) {
            free_req(r);
            put_text(why, sizeof why, busy);
        } else {
            ok = await(r, until, APPS_TASK_WAIT_MS, why, sizeof why) && r->ok;
            release(r);
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "stop: %s", why);
    }
    put_text(err, err_len, why);
    return ok;
}

char *apps_task_message(const char *id, const char *event, const char *data_json, bool show, int timeout_ms, char *err,
                        size_t err_len) {
    put_text(err, err_len, "");
    if (id == NULL || !apps_id_valid(id)) {
        put_text(err, err_len, "not an app id");
        return NULL;
    }
    event = event != NULL ? event : "";
    size_t event_len = strlen(event), data_len = data_json != NULL ? strlen(data_json) : 0;
    int wait_ms = timeout_ms > 0 ? timeout_ms : APPS_TASK_WAIT_MS;
    const char *why = refusal(true);
    req_t *r = why == NULL ? new_req(REQ_MESSAGE, event_len + 1 + data_len + 1) : NULL;
    if (why == NULL && r == NULL) {
        why = "out of memory: no room for the request";
    }
    if (why != NULL) {
        put_text(err, err_len, why);
        return NULL;
    }
    put_text(r->id, sizeof r->id, id);
    char *at = (char *)(r + 1);
    memcpy(at, event, event_len + 1);
    r->event = at;
    at += event_len + 1;
    if (data_len > 0) {
        memcpy(at, data_json, data_len);
    }
    at[data_len] = '\0';
    r->data = at;
    r->data_len = data_len;
    r->front = show;
    r->err = err;
    r->err_len = err_len;
    int64_t until = apps_port_mono_ms() + wait_ms;
    const char *busy = post_req(APPS_EV_MESSAGE, r, until);
    if (busy != NULL) {
        free_req(r);
        put_text(err, err_len, busy);
        return NULL;
    }
    char *reply = NULL;
    if (await(r, until, wait_ms, err, err_len) && r->ok) {
        reply = r->reply;  /* finished: the app task writes nothing more */
        r->reply = NULL;
    }
    release(r);
    return reply;
}

void apps_task_status(apps_status_t *st) {
    if (st == NULL) {
        return;
    }
    if (!atomic_load(&s_up)) {
        memset(st, 0, sizeof *st);
        return;
    }
    if (rec_take(STATUS_LOCK_MS)) {
        memcpy(st, &s_rec->st, sizeof *st);
        if (s_rec->loaded) {
            /* Read under the lock, as its base is: dropped only grows, so the difference can't wrap. */
            st->dropped_events = atomic_load(&s_rec->dropped) - s_rec->dropped_base;
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

void apps_task_show(void) {
    request_front(true);  /* the one door of every front: the LVGL task holds it back during a voice turn (C11) */
}

void apps_task_keep_awake(bool on) {
    if (on && !T.keep_awake) {
        T.poke_at = 0;  /* the first poke at once, at the next round: only for a change from off to on, so that the rate is the loop's */
    }
    T.keep_awake = on;
}

void apps_task_call_restart(void) {
    if (s_rec != NULL && atomic_load(&s_rec->in_call)) {
        T.call_began = apps_port_mono_ms();  /* the call's time, for the supervisor and for the status's worst call, begins here */
        atomic_store(&s_rec->call_started, (uint32_t)T.call_began);
    }
}

void apps_task_exit(void) {
    T.exit_asked = true;
}

esp_err_t muse_apps_start(void) {
    static bool console_added;
    if (s_task != NULL) {
        return ESP_OK;
    }
    apps_port_init();
    s_rec_lock = xSemaphoreCreateMutexStatic(&s_rec_lock_buf);
    s_req_lock = xSemaphoreCreateMutexStatic(&s_req_lock_buf);
    s_names_lock = xSemaphoreCreateMutexStatic(&s_names_lock_buf);
    apps_io_init();
    if (!console_added) {
        /* Even if the task can't be made below: the console then says that Muse apps aren't running. */
        muse_input_add_console_hook(muse_apps_console);
        console_added = true;
    }
    s_rec = heap_caps_calloc(1, sizeof *s_rec, CAPS_PSRAM);
    s_queue = xQueueCreateWithCaps(APPS_QUEUE_LEN, sizeof(apps_event_t), MALLOC_CAP_SPIRAM);
    if (s_rec != NULL && s_queue != NULL &&
        xTaskCreatePinnedToCoreWithCaps(apps_task, "muse_apps", APPS_TASK_STACK, NULL, APPS_TASK_PRIORITY, &s_task,
                                        APPS_TASK_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        atomic_store(&s_up, true);
        apps_alarm_set_fired_cb(on_alarm_fired);  /* no alarm can be armed before an app runs */
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
