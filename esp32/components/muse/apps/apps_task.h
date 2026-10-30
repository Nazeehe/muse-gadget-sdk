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
 * Timers. The loop waits for an event until the app's earliest timer is due (apps_rt_next_deadline), and never more than a
 * second, which is also how long it waits while the screen is dark, when no timer runs (apps_port_dark): the first round after
 * the wake runs the timers that came due, each once. That round comes at once: the supervisor, on the LVGL task, posts a no-op
 * event (APPS_EV_WAKE) when it sees the screen go from dark to lit, so that an app in front doesn't show its old frame for up to
 * a second. A wait is rounded up to whole ticks and is at least one for any wait above 0 (a build whose tick is 10 ms would
 * otherwise spin for the last 9 ms). Each round, after the event it got, runs the timer that is due, one a round: a call of the
 * app like any other, bracketed for the supervisor and counted in the status, and an error in it ends the app as an error in a
 * handler does. The runtime's warnings that come after the run's start (the duty cycle's) are added to the record's as they
 * come, and its kill is a crash like any.
 *
 * Alarms. The system's alarms (apps_alarm.h) are delivered by this task, on every round of the loop, whatever woke it, and so at
 * least once a second: the esp_timer task that fires one only marks it (it keeps its slot until its app has been told), rings,
 * wakes the screen and posts APPS_EV_ALARM with no wait and no payload, once while the loop has not looked (a flood of firings
 * is one post, and the post leaves the queue's last 2 slots to requests). The one for the app that runs fronts it (request_front,
 * deferred past a voice turn like every front) and calls alarm(name) (apps_rt_alarm: a call of the app like any other, so the
 * duty cycle counts it and the supervisor watches it). One for an app that doesn't run starts it: the same launch as the
 * launcher's pick, but with an identity (the gen of the last of that app's firings that the launch is asked for: apps_alarm_wanted):
 * the I/O worker loads the last package installed with that id, from RAM now and from the store later, and runs it in front
 * (apps_task_run_for_alarm), and alarm(name) follows start(). The worker says what came of every launch it took, by a post
 * (APPS_EV_ALARM_LAUNCH, apps_task_alarm_launch_result): started, failed (the package didn't run), or missing (the store has no
 * such app). The alarm waits in its slot for that, and the app task never waits for the worker. A launch the worker refused is asked
 * for again each second, and given up after APPS_TASK_ALARM_REFUSED_MS; one it took is waited for, however long its jobs ahead of it
 * run, until APPS_TASK_ALARM_BACKSTOP_MS. A launch whose alarms are gone (told, cancelled or given up on) runs nothing: the worker
 * and the app task each ask apps_alarm_wanted before they load or replace anything. An app the store doesn't have is "alarm <name>
 * for missing app <id>" in the log, one that didn't run "alarm <name>: <id> did not start: dropped", and one that wasn't up in time
 * "alarm <name>: <id> did not start within <N> ms: dropped"; each drops only the firings the launch was for, and a ring they
 * started ends if nothing else could stop it. Launches go one app at a time, in the order the alarms fired. An app that its alarm
 * launched and that crashes in start() or in the alarm() it was launched for loses every alarm it has (apps_alarm_cancel_app: it
 * would arm its next in start() and be launched again for ever), and every app's end is told to the alarms
 * (apps_alarm_app_ended), whose ring ends with the app that was told it unless a firing with sound still waits. What the esp_timer
 * task and these calls record to log is printed here, in housekeeping and after each call (apps_alarm_log_drain).
 *
 * The store. An app's values are the runtime's cache (apps_rt.h), loaded when the runtime is made and saved through the port
 * (apps_port.h), whose work is the I/O worker's (X11). The loop saves what is due each round (apps_rt_store_tick), and its wait is
 * shortened to the time a save is due (apps_rt_next_deadline), dark or lit. The load, inside apps_rt_new, waits for the worker at most
 * APPS_STORE_WAIT_MS, inside the supervisor's 3 s. A launch is itself a job of the worker, which waits for this task, so that load can't
 * wait behind it: it is an op, which the worker runs between jobs and a job that waits here runs as it looks (apps_io_assist, called by
 * the waits of the callers). An app's end saves the rest (apps_rt_stop after stop(), the crash, apps_rt_free), the order of the ops
 * keeping the next launch's load after that save.
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
#include "apps_store.h"

/* The refusals every call returns, and the wait's: as text, in err. Link's app.send (apps_control.c) tells them from a handler's error. */
#define APPS_TASK_MSG_HUNG "the app stopped responding; no app can run until its stuck call returns or the gadget restarts"
#define APPS_TASK_MSG_BUSY "the app task is busy; try again"
#define APPS_TASK_MSG_DOWN "Muse apps aren't running"
#define APPS_TASK_MSG_NO_ANSWER "the app task didn't answer within"  /* then " <N> ms" */

/* The bounds of the waits. */
#define APPS_TASK_RUN_EXTRA_MS 7000  /* a run waits settle_ms and this */
#define APPS_TASK_SEND_LAUNCH_MS 3000  /* app.send's launch of a stored app: store load is the worker's, this is the load and start() (about 750 ms of instructions); with the 5 s reply it stays 2 s under the VM's 10 s */
#define APPS_TASK_WAIT_MS 2000       /* compile and stop */
#define APPS_TASK_HUNG_MS 3000       /* a call that has run longer is hung (strictly longer) */
#define APPS_BOOT_DEADLINE_MS 20000  /* I2: the boot's mount goes by then (apps_port_mono_ms) with no word from the Link (muse_apps.h) */
#define APPS_TASK_RESET_MS 500       /* the app task's wait for the LVGL task's reset */
#define APPS_TASK_LAYOUT_MS 500      /* a run's wait for a pass of the apply step that has laid its widgets out */
#define APPS_QUEUE_LEN 16            /* the queue's events: a touch or an alarm's post leaves the last 2 to requests */
#define APPS_TASK_ALARM_REFUSED_MS 10000   /* a launch for alarms that the I/O worker refused is asked for again each second this long, then given up */
#define APPS_TASK_ALARM_BACKSTOP_MS 120000  /* a launch the worker took must be reported by then (above its longest job: an install's 90 s) */
#define APPS_TASK_ALARM_GONE "its alarm is gone"  /* what a run for alarms says when they were told, cancelled or given up on first: not a failure */

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
 *   whole second (a window of the runtime's duty cycle, which counts the calls' time for both: apps_rt.h) that its calls
 *   took; dropped_events: touches the queue had no room for; stack_free: the app task's
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
    APPS_EV_ALARM,      /* an alarm fired (apps_alarm.h): brings the loop round, which delivers; no payload */
    APPS_EV_WAKE,       /* the LVGL task ran a reset the app task waits for, or the screen was lit: the round's housekeeping */
    APPS_EV_ALARM_LAUNCH,  /* the I/O worker's launch for an app's alarms is over: code the outcome (apps_alarm_launch_t), app_gen its identity */
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
 * 1. stops and frees the current app; 2. admits the new one (C1: free PSRAM, the old app gone and pkg's files in it
 * already, must hold memory_kb and APPS_RAM_RESERVE: apps_room, apps_pkg.h), then makes the runtime with memory_kb, the
 * package's images its assets, and main.lua run; 3. calls start(); 4. publishes its table and puts the layer in front (show() follows) or behind;
 * 5. runs its events for settle_ms; 6. waits at most APPS_TASK_LAYOUT_MS for a pass of the apply step and adds a
 * warning for each visible widget past the round edge: "label#3 'Time's up' extends 14 px past the round edge" (its
 * text, or its kind when it has none, as messages show text: 40 bytes, control bytes as '?'), or, if no pass came,
 * "layout not checked: the screen was asleep"; 7. fills st (may be NULL).
 * True when the app is running at the end. Otherwise err says why: the admission's refusal (APPS_MSG_SHORT_OF_MEMORY),
 * the runtime's own message (main.lua doesn't compile), "crashed loading main.lua: <first line>", "crashed in start(): <first line>", "crashed: <first line>"
 * (in the settle), or the waits' (hung, no answer in time, the queue full); st has the record as it is then.
 * pkg is a block of apps_port_realloc, as are its blobs and messages_json, and apps_task_run takes it, whatever
 * happens: it is freed (apps_pkg_free, apps_port_free) when the app ends, or at once when it doesn't run.
 */
bool apps_task_run(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len);

/*
 * apps_task_run for a launch by id (muse_apps_launch's job): the run is decided where it is used, on the app task. When
 * an app of pkg's id runs by the time the app task gets to the request, it stays as it is: it is fronted (front) or left
 * where it is, pkg is freed, and the call is true with st the record as it is then (no settle). Otherwise this is
 * apps_task_run. So two launches of one id in flight run it once, and the second fronts it.
 */
bool apps_task_run_by_id(apps_pkg_t *pkg, bool front, int settle_ms, apps_status_t *st, char *err, size_t err_len);

/*
 * Posts a launch of the app id and returns: it never waits, for the answer or for a frame, so the LVGL task (the
 * launcher's pick) can call it. The app task fronts the app that runs when it is that app (front) and leaves it where
 * it is otherwise. Any other id goes to muse_apps_launch (muse_apps.h): the I/O worker loads the store's copy and runs
 * it through apps_task_run_by_id, in front or behind; a refusal, or an id the store doesn't have, is one log line. A full
 * queue drops the launch, with a log line. The launcher lists the store's apps whether they run or not
 * (apps_task_set_list); Task 17's alarms and Task 20's flash store build on this.
 */
void apps_task_launch(const char *id, bool front);

/*
 * apps_task_run_by_id for Link's app.send (apps_control.c, on the I/O worker): behind, no settle, and the wait is at most bound_ms,
 * clamped to 1..APPS_TASK_SEND_LAUNCH_MS, instead of APPS_TASK_RUN_EXTRA_MS (the caller gives what is left of the send's deadline).
 * Otherwise as apps_task_run_by_id: an app of pkg's id that runs is left where it is, and pkg is taken whatever happens. A launch that
 * isn't answered in time fails with APPS_TASK_MSG_NO_ANSWER " <bound> ms".
 */
bool apps_task_run_for_send(apps_pkg_t *pkg, int bound_ms, char *err, size_t err_len);

/*
 * apps_task_run_by_id for a launch for an app's alarms (the I/O worker's, apps_console.c), with no settle and in front: upto is the
 * launch's identity, the gen of the last firing of the app that it was asked for (apps_alarm_wanted). When no firing of the app up
 * to it waits any more, nothing is replaced and nothing runs: false, with APPS_TASK_ALARM_GONE in err, which is no failure to
 * log. Otherwise as apps_task_run_by_id. The app is the one that its alarms may cancel (see above) if it crashes in start() or in
 * the alarm() that follows it.
 */
bool apps_task_run_for_alarm(apps_pkg_t *pkg, uint32_t upto, apps_status_t *st, char *err, size_t err_len);

/* What came of a launch for alarms that the worker took. */
typedef enum {
    APPS_ALARM_LAUNCH_STARTED = 1,  /* the app runs: its alarms are told by the loop */
    APPS_ALARM_LAUNCH_FAILED,       /* the app didn't run: it didn't compile, crashed in start(), or there was no memory */
    APPS_ALARM_LAUNCH_MISSING,      /* the store has no such app */
} apps_alarm_launch_t;

/*
 * The I/O worker says what came of the launch for alarms whose identity is upto (the worker has taken it, and reports each one it
 * ran, however it ended; one it skipped, as its alarms were gone, it doesn't): a post to the app task's queue, which the app task
 * answers in its next round. Started: the loop tells the alarms (nothing is needed). Failed: the firings the launch was for are
 * dropped, "alarm <name>: <id> did not start: dropped". Missing: they are dropped, "alarm <name> for missing app <id>"; a ring they
 * started ends if nothing else could stop it. A result for a launch the app task no longer waits for is ignored. It waits for room
 * in the queue like a request (at most APPS_TASK_WAIT_MS, in tries of 10 ms, and not at all once the app hangs): when none comes
 * the launch's backstop gives its alarms up. Any task but the app task.
 */
void apps_task_alarm_launch_result(uint32_t upto, apps_alarm_launch_t outcome);

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

/* The record's few words, for device.health and the heartbeat (muse_apps_health, muse_apps_brief): the app that is loaded (id, cap bytes:
 * from its launch to its end, a stuck one included; "" when none is), the record's state (the last app's once it has ended; HUNG while the
 * supervisor's mark is up), its Lua heap now (0 when none is loaded) and the last app's peak, in KB. Any task: the record's lock, held for
 * copies; STOPPED, "" and 0 when the task isn't up or the record stays busy. */
void apps_task_brief(char *id, size_t cap, apps_state_t *state, uint32_t *mem_kb, uint32_t *peak_kb);

/* An app the launcher lists: its id, the name it shows (its manifest's name, or its id) and its icon's path (the manifest's, or ""). */
typedef struct {
    char id[APPS_ID_MAX + 1], name[25], icon[APPS_PATH_MAX + 1];  /* icon: the manifest's path, "" for none */
} apps_listed_t;

#define APPS_TASK_LIST_MAX APPS_STORE_APPS_MAX  /* the store's apps the launcher is told of: every one (the RAM store keeps 8) */

/*
 * The launcher's list (cached data for the LVGL task, X11): the store's apps, most recent first, n of them (at most
 * APPS_TASK_LIST_MAX; more are left out). The store's owner, the I/O worker, gives it after every change of the store
 * (apps_install.h). It is copied under the names lock, which the caller waits for (held only for copies; a W log line
 * says so in the unlikely case it stays busy for a second, and the list waits for the next change). When the launcher
 * opens, the LVGL task shows the first APPS_LV_LAUNCHER_ROWS of it (the old card; the list holds every app) (taking the lock with its 2 ms try), the app that
 * runs on the first row when they don't have it, so the launcher can always bring it back. A pick of the app that runs
 * fronts it; a pick of any other launches it (apps_task_launch).
 */
void apps_task_set_list(const apps_listed_t *apps, int n);

/* The app that runs, from when it is up (its start() has returned) until it ends (a stop, a crash, an exit, a hang once
 * its call has come back), into id (cap bytes): true; false, with id "", when none does. Any task: it waits only for the
 * names lock (held only for copies). The store never drops it. */
bool apps_task_running(char *id, size_t cap);

/* N3: the app that is loaded (from its launch to its end, as apps_task_brief's) and what its end frees, for a load beside it
 * (apps_room_beside): id into id (cap bytes), and into *bytes its Lua heap as the VM counted it at the record's last refresh
 * (apps_vm_mem_used: after its run settled, then at most once a second) and its package's blocks (its files, manifest.json's
 * bytes, its messages and the apps_pkg_t). True when one is loaded; false, with "" and 0, when none is, the task isn't up or the
 * record stays busy for STATUS_LOCK_MS. Any task: the record's lock, held only for copies. */
bool apps_task_footprint(char *id, size_t cap, size_t *bytes);

/*
 * Sends message(event, data_json) to the app id, which must be the one running (Link's app.send starts a stored app that
 * isn't first: muse_apps_send, apps_control.c, on the I/O worker), and returns its reply: JSON text in a block of apps_port_realloc that the caller frees with
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

/*
 * What sys asks of the app task, from inside a call of the app (apps_port.h's apps_port_show, apps_port_keep_awake and apps_port_exit
 * are these). Only the app task calls them: they touch the app task's own state.
 * - apps_task_show: the layer is asked to the front, through the door of every front (request_front: held back during a voice turn).
 * - apps_task_keep_awake: while on, and the layer is in front, the loop's housekeeping pokes the idle timer (muse_state_poke) about once a
 *   second (never oftener, and the loop comes round at least each second, so never more than 2 s apart), so the screen isn't let go dark
 *   for being idle. It is off again when the app ends.
 * - apps_task_call_restart: the call that is running begins again now, for the supervisor's APPS_TASK_HUNG_MS and the status's worst call
 *   alike: the port calls it once its wait for the I/O worker, for the app's store, is over (apps_port.h), so that a wait that is the gadget's
 *   is not counted against the app. Nothing outside a call.
 * - apps_task_exit: the app ends, as END_EXITED, once the call it was asked in has returned (call_end): that callback runs to its end,
 *   its reply is given, stop() runs, the store is saved, and no other callback of the app runs after it, a timer due in the same round
 *   included. At main.lua's top level it means the app is never started; in start(), that it ends after it. A run that hasn't settled
 *   by then (those two, or a callback in the first moments of its settle) is answered as failed, "the app exited (sys.exit)", so that
 *   the developer's run says what happened and an alarm's launch of the app is given up on as for any app that didn't run.
 */
void apps_task_show(void);
void apps_task_keep_awake(bool on);
void apps_task_call_restart(void);
void apps_task_exit(void);

/* Whether the caller is the app task (the port's wait for a snapshot's blocks is the app task's alone: apps_port.h). Any task. */
bool apps_task_current(void);
