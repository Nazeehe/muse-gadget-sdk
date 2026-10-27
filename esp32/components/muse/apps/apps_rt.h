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
 * What the app's code is given besides the sandbox's libraries (apps_vm.h) and `json` (apps_json.h):
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
 *   name and its path.
 *
 * States. A runtime is STOPPED when it is made (main.lua's top level has run, start() hasn't), RUNNING from
 * a successful start() until stop(), and CRASHED when a handler raises an error, is killed (CPU, wall clock
 * or a kill from another task: apps_vm.h) or runs out of memory. The one exception is message(): its own
 * error() fails that message and nothing else. A crash is final: a CRASHED runtime runs no app code again,
 * whatever it is asked, and its crash record (below) and log stay until apps_rt_free. To run the app again
 * make a new runtime. HUNG is the supervisor's (Task 12) to report: nothing here ever enters it.
 *
 * Crash record. apps_rt_last_error is the message of the error with the traceback of the app's frames, or
 * the kill's reason: "main.lua:12: attempt to index a nil value (local 'arc')\nstack traceback:\n\t…".
 * Like every text this module hands out, it is valid UTF-8 and has no control byte but "\n" and tab (the
 * bytes of an app's own error text are the app's, and they reach the console and Muse inside JSON), cut at
 * APPS_ERROR_MAX bytes. An error that fails a message alone isn't a crash: it is returned to the caller
 * without its traceback and last_error stays empty.
 *
 * The log. The last lines the app printed, in a ring of APPS_LOG_RING_BYTES bytes in which a line takes
 * its length plus one: the oldest lines go when a new one needs room. Every line is also given to
 * apps_port_log as it is printed.
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
 *     "\nstack traceback:"), as the app raised it: "main.lua:7: unknown event";
 *   - "data: <message>": data isn't JSON, with the decoder's own message ("data: unexpected end of input at
 *     byte 4");
 *   - "message: <message>": the reply can't be encoded, with the encoder's ("message: can't encode a function");
 *   - the runtime's own: "the app is not running" (STOPPED), "<main> has no message(event, data) handler";
 *   - the crash: last_error, when the handler was killed or the heap ran out, and also for a runtime that
 *     had crashed already. The runtime is CRASHED from then on.
 *   err is cut to err_len on a character boundary; it may be NULL.
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
 * Memory: a runtime is one block from apps_port_realloc (itself, with its log and crash record) and the VM's
 * heap, capped at the manifest's memory_kb. apps_rt_free gives all of it back, and runs no app code.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_pkg.h"

/* apps_vm.h's, which this header doesn't include: the firmware outside apps/ doesn't see Lua's headers. */
typedef struct apps_vm apps_vm_t;

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
 * NULL when that can't be done, and then nothing is held: the reason is in err (cut to err_len on a character
 * boundary; err may be NULL), a message of this header or the text of main.lua's own error (a syntax error
 * "main.lua:3: unexpected symbol near 'x'"; a runtime error with its traceback; a kill's reason). main.lua's
 * log is lost with it, but every line went through apps_port_log.
 */
apps_rt_t *apps_rt_new(const apps_pkg_t *pkg, char *err, size_t err_len);

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

/* Frees the runtime and its VM, and runs no app code (a crashed or abandoned app can be freed too). NULL is
 * ignored. */
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

/* The VM, for what the runtime doesn't wrap: apps_vm_kill, apps_vm_in_call, apps_vm_call_started_ms and the
 * memory counters (apps_vm.h). What is installed into its globals through it is there for the handlers, not for
 * main.lua's top level, which has run by the time the runtime exists. */
apps_vm_t *apps_rt_vm(apps_rt_t *rt);
