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

#pragma once

/*
 * The app task of Muse apps: the one task that runs an app's Lua (apps_rt), and the services the rest of the
 * firmware asks of it. muse_apps_start() (muse_apps.h) makes it: "muse_apps", core 1, priority 2, a 56 KB stack in
 * PSRAM and a queue of 16 apps_event_t in PSRAM. One app at a time.
 *
 * Callers. apps_task_run, apps_task_compile, apps_task_stop and apps_task_message post a request and wait for its
 * answer, always with a bound: a run waits settle_ms + APPS_TASK_RUN_EXTRA_MS, a message its own timeout, the others
 * APPS_TASK_WAIT_MS, for room in the queue and the answer together. Touches leave the queue's last 2 slots to
 * requests, so taps can't keep a request out; a request that finds no room within its bound fails with "the app task
 * is busy; try again". A caller that gives up abandons the request, and the app task writes nothing to its buffers
 * after that: a run abandoned before its app has begun doesn't start it. They are for the tasks that install and drive
 * apps (the I/O worker, the console, Link), never the app task itself (a call from it fails at once) and never the
 * LVGL task, which posts and doesn't wait: apps_task_launch. apps_task_status reads the task's record under a short
 * lock and posts nothing.
 *
 * A hung app. A call into the app (a handler, main.lua's top level) that has run for more than APPS_TASK_HUNG_MS is
 * hung: the supervisor, on the LVGL task (muse_apps_ui_supervise), puts the layer behind, drops the app task to
 * priority 1, shows the card "<name> stopped responding" and marks the app HUNG. The device is never restarted.
 * While the call doesn't return the app task can't answer anything, so every call here returns at once with
 * "the app stopped responding; no app can run until its stuck call returns or the gadget restarts" (status reads
 * the record as ever). If the call does return, nothing of the app runs again: the task frees its state (as the
 * teardown below), goes back to priority 2 and serves calls again; the record keeps the state HUNG until the next
 * app. The supervisor runs in frame_tick, so it stops while LVGL is paused (asleep and dark on battery): a call
 * stuck in C then is marked HUNG on the first frame after the screen wakes. Until then the VM's 1 s backstop still
 * stops Lua that runs too long, and the callers' waits stay bounded: they end with their own timeout rather than
 * the hung message.
 *
 * An app's end (stop, a new run, sys.exit, a crash, a hang) is one order, the rulings': the pending request is
 * answered; the table is unpublished (apps_lv_set_ui(NULL) under the UI lock); the record takes the crash record,
 * the log and the widget dump; the runtime is freed; the LVGL task is asked to reset the layer (apps_lv_reset) and
 * the app task waits for it, at most APPS_TASK_RESET_MS; the package's bytes, which image objects draw from, are
 * freed only once that reset has run (they wait for a later one if it didn't come in time); and a crash card is
 * shown after the reset. A new app's table is published only once the old app's reset has run. Only an app whose
 * table was published asks for a reset: one that ends before its table is (a crash in main.lua or start(), or an
 * app replaced while its table still waited) frees its package at once, and its crash card waits for the reset
 * that is outstanding. So at most one package waits for a reset at a time: no table is published while a reset is
 * outstanding, so every app that ends in the meantime ends unpublished. A new app drops an earlier app's crash card
 * that isn't on screen yet.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_pkg.h"
#include "apps_rt.h"

/* The bounds of the waits. */
#define APPS_TASK_RUN_EXTRA_MS 7000  /* a run waits settle_ms and this */
#define APPS_TASK_WAIT_MS 2000       /* compile and stop */
#define APPS_TASK_HUNG_MS 3000       /* a call that has run longer is hung (strictly longer) */
#define APPS_TASK_RESET_MS 500       /* the app task's wait for the LVGL task's reset */
#define APPS_TASK_LAYOUT_MS 500      /* a run's wait for a pass of the apply step that has laid its widgets out */

/*
 * What the app task knows of the current app (or the last one: the record keeps it until the next run). It is
 * several KB, so it never lives on a stack: heap_caps_calloc(1, sizeof *st, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
 * and free it after.
 * - state: the runtime's (apps_rt.h), or APPS_STATE_HUNG. A run that made no runtime (a main.lua that doesn't
 *   compile) leaves STOPPED, with the reason in last_error.
 * - last_error: the crash record, cut on a character boundary; warnings: the manifest's, main.lua's and the run's
 *   own (overflow, "layout not checked: ..."), "; "-joined, each whole or dropped.
 * - log_tail: the whole log ring, oldest line first, "\n"-joined; ui_dump: apps_ui_dump's JSON array, and
 *   ui_truncated how many widgets it dropped for want of room (a result says "ui_truncated":N when it isn't 0).
 * - instructions: the VM's count (apps_vm_instructions); mem_kb, peak_kb: the Lua heap now and at most; limit_kb:
 *   the manifest's memory_kb; worst_dispatch_ms: the longest call into the app; cpu_pct: the share of the last
 *   whole second spent in calls; dropped_events: touches the queue had no room for; stack_free: the app task's
 *   stack high-water mark, in bytes; widgets: alive widgets; front: the layer is in front.
 * The log tail, the dump and the counters are refreshed after a run settles, when an app ends, and otherwise at
 * most once a second while the app runs; front and dropped_events are read when the status is.
 */
typedef struct {
    apps_state_t state;
    char id[APPS_ID_MAX + 1], name[25], version[24];
    char last_error[768], warnings[512];
    char log_tail[APPS_LOG_RING_BYTES + 1];
    char ui_dump[16384];
    int ui_truncated;
    uint64_t instructions;
    uint32_t mem_kb, peak_kb, limit_kb, worst_dispatch_ms, dropped_events, stack_free;
    uint8_t cpu_pct;
    int widgets;
    bool front;
} apps_status_t;

/* What the app task's queue carries. Touches, show and hide come from the LVGL task and carry the app_gen of the app
 * they were meant for (one that has ended since drops them); requests carry req. Later tasks add types. */
typedef enum {
    APPS_EV_TOUCH = 1,  /* a widget event: slot, gen, code (one APPS_UI_* bit) */
    APPS_EV_RUN,        /* a run or a launch */
    APPS_EV_STOP,
    APPS_EV_COMPILE,
    APPS_EV_MESSAGE,    /* apps_task_message */
    APPS_EV_SHOW,       /* the layer came to the front: show() */
    APPS_EV_HIDE,       /* it went behind: hide() */
    APPS_EV_ALARM,      /* Task 17 */
    APPS_EV_EXIT,       /* sys.exit() (Task 18): the app ends once the current callback returns */
    APPS_EV_WAKE,       /* the LVGL task ran a reset the app task waits for */
} apps_event_type_t;

typedef struct {
    uint8_t type, code;
    uint16_t slot, gen;
    uint32_t app_gen;
    void *req;
} apps_event_t;

/*
 * Compiles every .lua file of pkg (main first) in a scratch VM with the manifest's memory_kb: nothing runs. pkg
 * stays the caller's (its code is copied before the call returns). False with Lua's own message in err: "main.lua:3:
 * unexpected symbol near 'x'", or "<main> is missing from the package".
 */
bool apps_task_compile(const apps_pkg_t *pkg, char *err, size_t err_len);

/*
 * Runs pkg, in front or behind, in place of the current app (which is stopped first), and lets it settle:
 * 1. stops and frees the current app; 2. makes the runtime with memory_kb, the package's images its assets, and
 * main.lua run; 3. calls start(); 4. publishes its table and puts the layer in front (show() follows) or behind;
 * 5. runs its events for settle_ms; 6. waits at most APPS_TASK_LAYOUT_MS for a pass of the apply step and adds a
 * warning for each visible widget past the round edge: "label#3 'Time's up' extends 14 px past the round edge" (its
 * text, or its kind when it has none, as messages show text: 40 bytes, control bytes as '?'), or, if no pass came,
 * "layout not checked: the screen was asleep"; 7. fills st (may be NULL).
 * True when the app is running at the end. Otherwise err says why: the runtime's own message (main.lua doesn't
 * compile), "crashed loading main.lua: <first line>", "crashed in start(): <first line>", "crashed: <first line>"
 * (in the settle), or the waits' (hung, no answer in time, the queue full); st has the record as it is then.
 * pkg is a block of apps_port_realloc, as are its blobs and messages_json, and apps_task_run takes it, whatever
 * happens: it is freed (apps_pkg_free, apps_port_free) when the app ends, or at once when it doesn't run.
 */
bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len);

/*
 * Posts a launch of the app id and returns: it never waits, for the answer or for a frame, so the LVGL task (the
 * launcher's pick) can call it. The app task fronts the resident app when it is that app (front) and leaves it
 * where it is otherwise (an app that isn't loaded logs "launch <id>: no such app on the gadget"; the console's
 * app.launch runs a stored app through the I/O worker instead, and Task 20's store adds stored apps here). A full
 * queue drops it, with a log line.
 */
void apps_task_launch(const char *id, bool front);

/*
 * Stops the current app (its stop() runs) and puts the layer behind. Waits at most APPS_TASK_WAIT_MS. True once the
 * app has stopped, or when none was running. Otherwise false, with why in err (may be NULL), and a W log line: a
 * refusal ("Muse apps aren't running", from the app task, or the hung message), no memory, no room in the queue
 * within the bound, or no answer within it. Task 13's >app.stop reports the result.
 */
bool apps_task_stop(char *err, size_t err_len);

/* A copy of the record (never NULL st). It posts nothing, and waits only for the record's lock, which is held for
 * copies. */
void apps_task_status(apps_status_t *st);

/*
 * Sends message(event, data_json) to the app id, which must be the one running (Task 24 adds starting a stored app
 * that isn't), and returns its reply: JSON text in a block of apps_port_realloc that the caller frees with
 * apps_port_free ("null" when the handler returned nothing). data_json is JSON text, or NULL or "" for nil. With show,
 * a message that succeeded asks for the layer in front (after the voice turn, if one is on). Waits at most timeout_ms
 * (APPS_TASK_WAIT_MS when it is 0 or less), for room in the queue and the answer together.
 * NULL, with why in err (may be NULL), cut to err_len:
 * - the app's own: its handler's error, which leaves it running ("main.lua:7: unknown event"), "data: <message>" for
 *   data that isn't JSON, "message: <message>" for a reply that can't be encoded, "<main> has no message(event, data)
 *   handler", or the crash record when the handler was killed or ran out of memory (the app has ended, as any crash);
 * - "<id> isn't running" when no app of that id runs, and "not an app id" for an id that can't be one;
 * - the waits' and the refusals', as for the calls above ("the app task didn't answer within N ms", ...).
 */
char *apps_task_message(const char *id, const char *event, const char *data_json, bool show, int timeout_ms, char *err,
                        size_t err_len);
