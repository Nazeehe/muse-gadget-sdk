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
 * One running Muse app: its Lua VM (apps_vm), the handlers main.lua returned, the messages it answers, a
 * log and a crash record. Portable C: the platform comes in through apps_port.h, and every function here
 * runs on the task that runs the VM (the app task); nothing is thread-safe.
 *
 * The app. main.lua returns a table, and the keys start, stop, show, hide, message and alarm of it are
 * the app's handlers. All are optional, and a missing one does nothing. They are read once, when main.lua
 * returns: what the app does to the table afterwards isn't seen. Any other key is a warning. Every
 * handler call is one apps_vm_call, with APPS_BUDGET_START for main.lua's own top level and for start(),
 * and APPS_BUDGET_CALL for the rest.
 *
 * What the app's code is given besides the sandbox's libraries (apps_vm.h), `json` (apps_json.h), `ui`
 * (apps_ui.h; main.lua's top level can already draw, so ui.image finds the package's images from the start) and
 * `time`, `os` and `timer` (apps_svc.h; main.lua's top level can already arm a timer, which runs once start() has):
 * - print(...): the tostring of each argument, joined with tabs, as the lines of the app's log. The text
 *   is split at each "\n". In each line, a byte under 0x20 but tab, 0x7F, a C1 control character
 *   (U+0080 to U+009F) and anything that isn't valid UTF-8 (RFC 3629) become '?', one for each byte, or
 *   for the whole character in the C1 case, and a line is cut at APPS_LOG_LINE_MAX bytes, on a character
 *   boundary. Why: a line goes to the serial console as "@app.log <id> <line>", where a newline in it
 *   could forge a protocol line, and where an escape sequence would run in the terminal that shows it, and
 *   to Muse inside a JSON string. print() is one empty line. Each line costs 250 VM instructions of
 *   the call's budget, and every 16 bytes print reads one more (C work that no count hook sees), so a call
 *   that prints without end, or prints a long string over and over, is killed.
 * - require(name): loads one of the app's own .lua files, as text, in the same sandbox, once: the value
 *   its chunk returns (true when it returns nothing) is what every later require of it gives. name is a
 *   path of manifest.json's "files" with or without the ".lua" ("util", "util.lua", "lib/util"); main.lua
 *   isn't one of them. Nothing else loads: no other path, no package.path. The chunk is called with the
 *   name and its path. A module that doesn't compile is an error of the require, one line ("require: " and
 *   Lua's message, with the module's path), and the next require compiles it again. It compiles on the thread
 *   that called require, so how deeply nested it may be is the C-call room that code has left: a coroutine deep
 *   in C calls has less than the main thread, and a module nested past the room fails as "require: C stack
 *   overflow".
 *
 * States. A runtime is STOPPED when it is made (main.lua's top level has run, start() hasn't), RUNNING from
 * a successful start() until stop(), and CRASHED when a handler raises an error, is killed (CPU, wall clock
 * or a kill from another task: apps_vm.h) or runs out of memory. The one exception is message(): its own
 * error() fails that message and nothing else. main.lua's top level is the app's first run and can crash
 * it in the same ways: apps_rt_new then returns a CRASHED runtime (below). A crash is final: a CRASHED runtime
 * runs no app code again, whatever it is asked, and its crash record (below) and log stay until apps_rt_free.
 * To run the app again make a new runtime. HUNG is the supervisor's (Task 12) to report: nothing here ever
 * enters it.
 *
 * Crash record. apps_rt_last_error is the message of the error with the traceback of the app's frames, or
 * the kill's reason: "main.lua:12: attempt to index a nil value (local 'arc')\nstack traceback:\n\t…".
 * Like every text this module hands out, it is valid UTF-8 and has no control byte but "\n" and tab (the
 * bytes of an app's own error text are the app's, and they reach the console and Muse inside JSON): anything
 * else becomes '?' as it does in print's lines, a C1 control character included, one '?' for the character.
 * It is cut at APPS_ERROR_MAX bytes. An error that fails a message alone isn't a crash: it is returned to the
 * caller without its traceback and last_error stays empty.
 *
 * The log. The last lines the app printed, main.lua's top level's included, in a ring of APPS_LOG_RING_BYTES
 * bytes in which a line takes its length plus one: the oldest lines go when a new one needs room. Every line
 * is also given to apps_port_log as it is printed.
 *
 * Messages. apps_rt_message(rt, event, data) calls message(event, data) and returns what it returned, as
 * JSON, in one apps_vm_call: decoding data, the handler and encoding the reply all run in the protected
 * call, so a heap that refuses memory at any of them is the app running out of memory (a crash) and never
 * an abort.
 * - data is JSON text, decoded as json.decode decodes it (null is json.null); NULL or empty is nil.
 * - A nil reply (or none) is the text "null". The block is from apps_port_realloc: the caller frees it
 *   with apps_port_free.
 * - Failure is NULL with the reason in err, and the app keeps running unless it says it crashed:
 *   - the handler's error: its message, without the traceback (the text before the first
 *     "\nstack traceback:"), cleaned as the crash record is (above, so "\n" and tab stay and a stray
 *     control byte is a '?'): "main.lua:7: unknown event";
 *   - "data: <message>": data isn't JSON, with the decoder's own message ("data: unexpected end of input at
 *     byte 4");
 *   - "message: <message>": the reply can't be encoded, with the encoder's ("message: can't encode a function");
 *   - the runtime's own: "the app is not running" (STOPPED), "<main> has no message(event, data) handler";
 *   - the crash: last_error, when the handler was killed or the heap ran out, and also for a runtime that
 *     had crashed already. The runtime is CRASHED from then on.
 *   err is cut to err_len on a character boundary; it may be NULL.
 *
 * Timers. timer.after(ms, fn) and timer.every(ms, fn) arm one of at most APPS_TIMERS_MAX timers, ms from APPS_TIMER_MIN_MS
 * to APPS_TIMER_MAX_MS, on the monotonic clock (apps_port_mono_ms: SNTP setting the wall clock moves none of them). The loop
 * that serves the app (the app task's, apphost's `advance`) asks apps_rt_next_deadline when to come back, and runs the timers
 * that are due, each as one apps_vm_call of APPS_BUDGET_CALL: apps_rt_run_one_timer for one, apps_rt_run_timers for a pass.
 * - Only a RUNNING app's timers run, and none while the screen is dark (apps_port_dark): nothing is lost, the first pass after
 *   the wake runs every timer that came due once, and an every timer's next run is a period after that pass, not the runs
 *   that were missed. The earliest due runs first, and of two that were due together the one armed first.
 * - A one-shot's slot is free before its callback runs, so the callback can arm another at the limit. A callback may cancel
 *   itself, or another timer, also one that is due in the same pass (which then doesn't run). A handle that names a timer
 *   that has fired (a one-shot) or was cancelled cancels nothing, the one that took its slot included.
 * - Every timer is gone when stop() has returned, and when the app crashes. A callback that fails is a crash, as the failure
 *   of any handler nobody waits for is (a widget's, show, hide, alarm): last_error is the record. Only message() fails alone,
 *   as it has a caller to tell.
 *
 * The duty cycle. The VM's instructions are counted in windows of one second of the monotonic clock, over every call into the
 * app (a callback, a handler, a message, main.lua's top level). A window of more than APPS_DUTY_INSTR_PER_S is hot, any other
 * is cool, and a second in which the app ran nothing is cool. Windows are closed when a call begins and by every pass of the
 * timers (apps_rt_has_work, apps_rt_run_one_timer), which the loop that serves the app makes at least once a second, so a
 * warning and a kill come when the second ends, with or without a call to carry them.
 * - After APPS_DUTY_THROTTLE_WINDOWS hot windows in a row the app is throttled: every timer's next run moves 100 ms later, so
 *   does each one armed or rescheduled while it lasts, and the warning "throttled: using more than 40% of the CPU" is added to
 *   apps_rt_warnings and to the log, once for the runtime. A cool window ends the throttle and the count.
 * - After APPS_DUTY_KILL_WINDOWS hot windows in a row the VM is killed (apps_vm_kill) and the app has crashed, with the
 *   reason "stopped: used more than 40% of the CPU for 5 s" as its record: at the next call into it, or at the next step of
 *   a pass, which makes one to say so.
 *
 * Messages (the runtime's own; <main> is the manifest's main file, "main.lua" unless it says otherwise):
 *   <main> must return a table of handlers: local app = {} … return app
 *   <main> returned '%s' as a %s; handlers must be functions         a handler key holding a non-function
 *   <main> returned unknown handler '%s' (did you mean '%s'?)        a warning; the suggestion (apps_suggest)
 *                                                                    only when a handler's name is near
 *   <main> has no message(event, data) handler
 *   require: '%s' is not one of this app's files (manifest.json "files": [...])
 *   the app is not running
 * A warning is added for every key of the returned table that isn't a handler (a key that isn't text is shown
 * by its value, '1' or '2.5' or 'true', or by its type, '<table>'). They are joined with "; " in
 * apps_rt_warnings, and one that doesn't fit whole in APPS_WARNINGS_MAX bytes is dropped, never cut (as
 * apps_manifest_t's are). A loop over the table's keys is charged to main.lua's budget by the size of the
 * table.
 *
 * Memory: a runtime is one block from apps_port_realloc (itself, with its log and crash record), the VM's
 * heap, capped at the manifest's memory_kb, and the widget table (apps_ui.h) with its text. apps_rt_free gives
 * all of it back, and runs no app code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_pkg.h"

/* apps_vm.h's and apps_ui.h's, which this header doesn't include: the firmware outside apps/ doesn't see Lua's
 * headers. */
typedef struct apps_vm apps_vm_t;
typedef struct apps_ui apps_ui_t;

typedef enum { APPS_STATE_STOPPED, APPS_STATE_RUNNING, APPS_STATE_CRASHED, APPS_STATE_HUNG } apps_state_t;

/* "stopped", "running", "crashed" or "hung"; "?" for anything else. */
const char *apps_state_name(apps_state_t state);

/* One line of the log is cut here; the ring holds APPS_LOG_RING_BYTES (a line costs its length plus one). */
#define APPS_LOG_LINE_MAX 200
#define APPS_LOG_RING_BYTES 8192
/* A crash record (message and traceback) holds this many bytes, with its NUL, and the warnings that many. */
#define APPS_ERROR_MAX 2048
#define APPS_WARNINGS_MAX 512

typedef struct apps_rt apps_rt_t;

/*
 * Makes the runtime for pkg: a VM with the manifest's memory_kb of heap, `json`, print and require, and
 * main.lua run to the table it returns (APPS_BUDGET_START), whose handlers are then known. pkg stays the
 * caller's and must outlive the runtime: the runtime reads the bytes of its .lua files from it, for require.
 * The runtime is STOPPED: start() hasn't run.
 *
 * Once the VM exists, main.lua's top level is the app's first run, and when it fails the app has crashed
 * before it was made: a runtime error, a kill, running out of memory, a main.lua that returns no table, a
 * handler that isn't a function. The result is then a runtime in APPS_STATE_CRASHED, with err "": the crash
 * record is apps_rt_last_error's, the lines main.lua printed before it failed are in the log
 * (apps_rt_log_tail), and apps_rt_warnings is empty (the table, if main.lua got that far, wasn't read to its
 * end). Nothing runs in it, as in any crashed runtime, and apps_rt_free frees it.
 *
 * NULL when there is no app to make, and then nothing is held: the reason is in err (cut to err_len on a
 * character boundary; err may be NULL), and no log, as nothing ran. That is: no package; a main file that
 * isn't in it or isn't a .lua file, or no room for the VM or the app's screen (messages of this header); or a
 * main.lua that doesn't compile, with Lua's own message ("main.lua:3: unexpected symbol near 'x'"), or the
 * parser's for source nested past the C-call room ("C stack overflow"), which has no position and no
 * traceback. Every line main.lua printed went through apps_port_log in either case.
 */
apps_rt_t *apps_rt_new(const apps_pkg_t *pkg, char *err, size_t err_len);

/* One Lua file to compile: its path, which names the chunk in Lua's messages ("util.lua:7: ..."), and its source. */
typedef struct {
    const char *path;
    const char *src;
    size_t len;
} apps_code_t;

/*
 * The install's compile step, in one place for the app task (the install's, on the gadget) and for apphost (check, on a
 * laptop): code[0, n) each loaded, as a top-level chunk and not run, in a scratch VM of the app's own kind (memory_kb of
 * heap, the sandbox) that is gone before this returns. The caller picks the order (apps_pkg_compile_order); the first
 * file that fails ends it. True when every file compiles. Otherwise err (cut to err_len on a character boundary; err may be
 * NULL) has Lua's raw message for that file ("util.lua:7: unexpected symbol near '='"; apps_pkg_compile_message words it),
 * or, when the VM can't be made, "out of memory: can't make the app's heap of %u KB": the same words apps_rt_new says of a
 * heap it can't make.
 */
bool apps_rt_compile(const apps_code_t *code, int n, unsigned memory_kb, char *err, size_t err_len);

/*
 * start(), with APPS_BUDGET_START. True when the app is RUNNING after it: start() returned, or the app has no
 * start, or it was RUNNING already (then nothing runs). False, with nothing run, when the runtime is CRASHED;
 * false, with the runtime CRASHED, when start() fails.
 */
bool apps_rt_start(apps_rt_t *rt);

/* show() when shown, hide() when not, with APPS_BUDGET_CALL, for a RUNNING app; nothing otherwise. */
void apps_rt_show(apps_rt_t *rt, bool shown);

/* alarm(name), with APPS_BUDGET_CALL, for a RUNNING app: true when it returned or the app has no alarm; false,
 * with nothing run, when the app isn't RUNNING, and false, with the runtime CRASHED, when it fails. */
bool apps_rt_alarm(apps_rt_t *rt, const char *name);

/* message(event, data), above. NULL event is "". */
char *apps_rt_message(apps_rt_t *rt, const char *event, const char *data_json, size_t data_len, char *err,
                      size_t err_len);

/* stop(), with APPS_BUDGET_CALL, for a RUNNING app: the runtime is STOPPED after it, or CRASHED if stop() fails.
 * Nothing runs for an app that isn't RUNNING. A STOPPED runtime can be started again: apps_rt_start. */
void apps_rt_stop(apps_rt_t *rt);

/* Frees the runtime, its VM and its widget table, and runs no app code (a crashed or abandoned app can be freed
 * too). NULL is ignored. The LVGL task reads the table under the UI lock and this frees it without one
 * (apps_ui_free): the apply step must have let go of apps_rt_ui(rt) before this is called (Task 12 clears its
 * pointer to it under the lock first), or it reads freed memory. */
void apps_rt_free(apps_rt_t *rt);

apps_state_t apps_rt_state(const apps_rt_t *rt);

/* The crash record while the runtime is CRASHED, else "". Valid until the runtime is freed. */
const char *apps_rt_last_error(const apps_rt_t *rt);

/* The warnings of main.lua's table, "; "-joined, or "". The manifest's are in the manifest. */
const char *apps_rt_warnings(const apps_rt_t *rt);

/*
 * The last `lines` lines of the log, oldest first, joined with "\n" (none after the last), into out (cap bytes
 * with the NUL). When they don't fit, the oldest go, whole; a newest line that alone doesn't fit is cut on a
 * character boundary. Returns the bytes written, without the NUL. lines of 0 or less, or an empty log, is "".
 * APPS_LOG_RING_BYTES + 1 bytes always hold the whole log.
 */
size_t apps_rt_log_tail(const apps_rt_t *rt, int lines, char *out, size_t cap);

/*
 * The app's widgets: the shadow table that ui.* fills and the LVGL task's apply step reads (apps_ui.h). The
 * runtime owns it, from apps_rt_new (so main.lua's top level can draw) to apps_rt_free, and a crash leaves it as
 * it was: what becomes of the app's last screen is for the supervisor and the LVGL task to decide, and
 * apps_rt_free takes the table with the runtime. The package's .bin files (those that pass apps_image_check) are
 * its images, in the package's order, which is how ui.image{src = "logo.bin"} finds them.
 */
apps_ui_t *apps_rt_ui(apps_rt_t *rt);

/*
 * A touch on a widget, from the LVGL task through the app's queue: the handler the app gave w:on for that event
 * runs with (w, event) and APPS_BUDGET_CALL, for a RUNNING app. slot and gen are what the event carried when
 * the apply step posted it, and event one APPS_UI_* bit. A widget that has been deleted since (its gen has moved
 * on, or the slot is empty), or has no handler for the event, runs nothing, and says nothing. A handler that
 * fails crashes the app, like any other.
 */
void apps_rt_touch(apps_rt_t *rt, uint16_t slot, uint16_t gen, uint8_t event);

/* ---- Timers and the duty cycle (above) ---- */

#define APPS_TIMERS_MAX 16
#define APPS_TIMER_MIN_MS 40
#define APPS_TIMER_MAX_MS 86400000  /* a day */

/* The most instructions a window of one second may hold and not be hot: the plan's number, about 40% of 10 M a second, and
 * Task 27 sets it from its own measurement. The 1.75C's VM runs 2.5 M a second of the cheapest instruction (a loop that only
 * counts; Task 16 measured it on the board, with the count hook on), so until Task 27 lowers this no window on that board is
 * hot, and the throttle and the kill don't come: 40% of 2.5 M is about 1 M. */
#define APPS_DUTY_INSTR_PER_S 4000000u
#define APPS_DUTY_THROTTLE_WINDOWS 3
#define APPS_DUTY_KILL_WINDOWS 5

/* The monotonic time the earliest timer is due at, for the wait of the loop that serves the app: INT64_MAX when there is no
 * timer, the app isn't RUNNING, or the screen is dark (the loop looks again within its second). Any time at or before now is
 * due. */
int64_t apps_rt_next_deadline(const apps_rt_t *rt);

/* Whether apps_rt_run_one_timer(rt, now_ms) would make a call: a timer is due, or the app was killed and no call has found
 * out. It closes the duty cycle's windows that have ended by now_ms first, so a loop that makes no call otherwise (the app
 * task's rounds) asks this once a round, and brackets the step with its own bookkeeping only when it says true. */
bool apps_rt_has_work(apps_rt_t *rt, int64_t now_ms);

/* One step of a pass at now_ms (it closes the duty windows first, as above). The earliest timer due at now_ms runs, as one
 * call of APPS_BUDGET_CALL, and true comes back. An every timer's next run is now_ms + its period (a throttled app's: 100 ms
 * more), so no timer is due twice in a pass, and what a callback arms is a period away. A killed app (the duty cycle's kill,
 * or another task's) that no call has found out is the step's call: a call that does nothing and records the crash. False,
 * with nothing run, when no timer is due, the app isn't RUNNING, or the screen is dark. A callback that fails has crashed the
 * app: the caller checks apps_rt_state, as after any call, before the next step. */
bool apps_rt_run_one_timer(apps_rt_t *rt, int64_t now_ms);

/* A whole pass: each timer due at now_ms once, earliest first (apps_rt_run_one_timer until it says false, or the app crashes). */
void apps_rt_run_timers(apps_rt_t *rt, int64_t now_ms);

/* What the timer module (apps_svc.c) is built on; the Lua side owns its handles. Arms a timer for the function at registry
 * reference fn_ref, ms from now (APPS_TIMER_MIN_MS to APPS_TIMER_MAX_MS: the module has checked), and every ms after each run
 * when `every`. Returns its slot, with its life in *gen (never 0), or -1 when APPS_TIMERS_MAX are armed. A handle is a slot
 * and the gen the timer was armed with. */
int apps_rt_timer_add(apps_rt_t *rt, int fn_ref, int64_t ms, bool every, uint32_t *gen);

/* Disarms the timer a handle (slot, gen) names, if it is still armed: true, with the reference of its function in *fn_ref,
 * which the caller gives back to the registry (luaL_unref, on the thread that called: not the main thread's while a coroutine
 * runs). False for a timer that has fired (a one-shot), was cancelled, or went with the app's end, and for a slot out of
 * range. */
bool apps_rt_timer_take(apps_rt_t *rt, int slot, uint32_t gen, int *fn_ref);

/* How many timers are armed. */
int apps_rt_timer_count(const apps_rt_t *rt);

/* The VM, for what the runtime doesn't wrap: apps_vm_kill, apps_vm_in_call, apps_vm_call_started_ms and the
 * memory counters (apps_vm.h). What is installed into its globals through it is there for the handlers, not for
 * main.lua's top level, which has run by the time the runtime exists. */
apps_vm_t *apps_rt_vm(apps_rt_t *rt);
