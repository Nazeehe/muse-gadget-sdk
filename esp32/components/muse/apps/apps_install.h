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
 * The one install pipeline of Muse apps, whatever the files come from: the USB console's stage (apps_console.c) now,
 * the VM from Task 22. Portable C: the files come in through a source, and the steps that need the app task and the
 * store through ops, so the host tests run it with fakes. It runs on the I/O worker (apps_io.h), whose stack is 6 KB:
 * everything big is on the heap (apps_port_realloc: PSRAM on the gadget).
 *
 * apps_install_run(id, run, src, ops), in order; the first step that fails ends it, and the result says why:
 * 1. id is an app id, or "app id '%s' must match ^[a-z][a-z0-9_]{1,23}$".
 * 2. manifest.json is read (at most APPS_MANIFEST_MAX bytes) and parsed for that id (apps_pkg.h: "manifest: ...").
 * 3. Every file the manifest lists, main first: its path is checked again (apps_path_valid) before it is read, then it
 *    is read with its kind's limit and checked, and hashed (apps_port_sha256):
 *      %s is larger than %u KB                          256 KB for .lua, 32 KB for .json, 512 KB for .bin
 *      the app is larger than 2 MB                      manifest.json and the files together
 *      files[%d] %s is a compiled chunk: install Lua source text     a .lua that starts with "\x1bLua" (%d counts
 *                                                       the package's files from 0, main first)
 *      %s: <apps_json_validate's message>               a .json that isn't JSON
 *      %s: image: ...                                   a .bin apps_image_check refuses
 *      images add up to more than 1.5 MB
 *    A source that can't read a file says so itself: "reading %s: %s".
 * 4. ops->compile. Lua's own message ("main.lua:2: unexpected symbol near 'x'") becomes "compile failed: " and
 *    apps_vm_excerpt's text of it for the file it names (the line before, the line and the line after, ">2|" on the
 *    line itself); a message that names none of the package's files is shown as it is, after "compile failed: ".
 * 5. ops->commit: the store keeps a copy of its own.
 * 6. With run, ops->run, with APPS_SETTLE_MS: it takes the package. A crash is
 *      installed but crashed in start(): <error>\n<traceback>\nlog:\n<the last 10 lines of the log>
 *    or "installed but crashed while loading <main>: ..." when main's top level failed, or "installed but crashed: ..."
 *    when the app crashed while it settled; any other failure is "installed but it didn't run: <why>".
 * 7. The result, which the caller owns (cJSON_Delete). It prints in at most APPS_RESULT_MAX bytes
 *    (cJSON_PrintUnformatted): what doesn't fit goes, the log's lines first, oldest first, every one of them before
 *    the dump's entries, which go whole from its end (then the last files, then the last warnings, which only a
 *    package of 30 or so long paths can need); an error message is cut at its end, on a character boundary.
 *      {"ok":true,"payload":{"id","version","storage","files":[{"path","bytes","sha256"}],"state","warnings":[...],
 *       "log":[...],"ui":[...],"mem_kb","peak_kb"}}
 *    with "ui_truncated":N when N widgets aren't in "ui" (apps_ui_dump's and the trim's), "files_truncated":N when
 *    files had to go, and "unchanged":true when the store found the package it already had. Without run, "state" is
 *    "installed", "warnings" the manifest's, and "log" and "ui" are empty. "ui" is the widget dump (apps_ui.h) as
 *    raw JSON (a cJSON_Raw item). On failure:
 *      {"ok":false,"error":{"code":<one of the codes below>,"message":<the message above>}}
 *
 * The package is the pipeline's from step 2 to step 6: its blocks are apps_port_realloc's, so that apps_task_run can
 * free it, and it is freed on every way out that doesn't give it to ops->run.
 *
 * The deadline. apps_install_run computes one absolute deadline, APPS_INSTALL_DEADLINE_MS from its start, in
 * microseconds of apps_port_mono_ms's clock, and passes it to every read.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_pkg.h"
#include "apps_task.h"
#include "cJSON.h"

#define APPS_RESULT_MAX 6144            /* bytes of a result, printed */
#define APPS_SETTLE_MS 2000             /* a run's settle after start() */
#define APPS_INSTALL_DEADLINE_MS 90000  /* the whole install's, from its start */

/* The error codes of a result. */
#define APPS_ERR_INVALID_PARAMS "invalid_params"    /* step 1 */
#define APPS_ERR_INVALID_PACKAGE "invalid_package"  /* steps 2 and 3 */
#define APPS_ERR_READ_FAILED "read_failed"          /* the source */
#define APPS_ERR_COMPILE_FAILED "compile_failed"    /* step 4 */
#define APPS_ERR_COMMIT_FAILED "commit_failed"      /* step 5 */
#define APPS_ERR_CRASHED "crashed"                  /* step 6: a crash */
#define APPS_ERR_NOT_RUNNING "not_running"          /* step 6: anything else */
#define APPS_ERR_OUT_OF_MEMORY "out_of_memory"      /* the pipeline's own blocks */
#define APPS_ERR_BUSY "busy"                        /* apps_install_try_begin, for its callers */

/* What a caller answers when apps_install_try_begin says no. */
#define APPS_INSTALL_BUSY "busy: another install is running"

/*
 * Where the files come from. read reads the app's file rel (a path apps_path_valid passed, or "manifest.json") into
 * *data, a block of apps_port_realloc whose owner the caller becomes, and its length into *len; *data is never NULL on
 * success (an empty file is a block of one byte). It needs to read no more than max + 1 bytes: a file longer than max
 * may come back with more than max bytes, and the pipeline refuses it. It gives up at deadline_us, an absolute time in
 * microseconds of apps_port_mono_ms's clock. False, with nothing to free, when it can't: err says why, as
 * "reading <rel>: <why>". origin names the source in the callers' log lines: "usb" or "vm".
 */
typedef struct {
    bool (*read)(void *ctx, const char *rel, size_t max, int64_t deadline_us, uint8_t **data, size_t *len, char *err,
                 size_t err_len);
    void *ctx;
    const char *origin;
} apps_source_t;

/*
 * The steps that need the gadget. compile checks every .lua file and runs nothing (apps_task_compile: Lua's own
 * message on failure). commit keeps a copy of the package in the store (*unchanged true when the store had it already).
 * run takes the package whatever happens, runs it in front and lets it settle for settle_ms, and fills st (C2:
 * apps_task_run(pkg, true, ...)). storage says where the store keeps apps: "ram" or "flash".
 */
typedef struct {
    bool (*compile)(const apps_pkg_t *pkg, char *err, size_t n);
    bool (*commit)(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n);
    bool (*run)(apps_pkg_t *pkg, int settle_ms, apps_status_t *st, char *err, size_t n);
    const char *(*storage)(void);
} apps_install_ops_t;

/* One install at a time, whatever it comes from (the console, Link): false while another holder has it. The holder
 * calls apps_install_end once its install has ended. Any task. */
bool apps_install_try_begin(void);
void apps_install_end(void);

/* The pipeline above. NULL only when there is no memory for even the result. */
cJSON *apps_install_run(const char *id, bool run, const apps_source_t *src, const apps_install_ops_t *ops);

/*
 * Runs an installed package (R13.5's >app.launch): ops->run takes pkg and runs it in front with APPS_SETTLE_MS, and the
 * result has the shape of an install's with run, within APPS_RESULT_MAX bytes: {"ok":true,"payload":{...}} with the
 * package's files as the store has them, or {"ok":false,"error":{...}} with the crash or the failure as step 6 has it,
 * less "installed but " ("crashed in start(): ...", or the run's own message). NULL only when there is no memory for
 * the result (pkg is taken all the same).
 */
cJSON *apps_install_launch(apps_pkg_t *pkg, const apps_install_ops_t *ops);

/*
 * st, the app task's record (apps_task_status), as the console's status shows it: {"state", "id", "name", "version",
 * "front", "last_error", "warnings":[...], "log":[...], "ui":[...] (raw, with "ui_truncated" as above),
 * "instructions" (the integer it is), "mem_kb", "peak_kb", "limit_kb", "worst_dispatch_ms", "cpu_pct",
 * "dropped_events", "stack_free", "widgets"}, trimmed as a result is, so that it prints in at most APPS_RESULT_MAX
 * bytes. NULL when there is no memory for it. It runs on the console's task too: its frames are small, and everything
 * it builds is on the heap.
 */
cJSON *apps_install_status(const apps_status_t *st);

/*
 * s[0, n), which someone typed, as a message shows it: valid UTF-8 as it is, each byte of a control character (under
 * 0x20, 0x7F) or of broken UTF-8 as '?', and a C1 control character (U+0080 to U+009F) as one '?'; cut at
 * APPS_SHOWN_MAX bytes on a character boundary, with "..." after the cut. out has room for APPS_SHOWN_MAX + 4 bytes.
 */
#define APPS_SHOWN_MAX 64
void apps_install_shown(const char *s, size_t n, char *out);

/*
 * The RAM store (C4): until Task 20's flash store, a committed package stays in PSRAM, one for each id and at most
 * APPS_RAM_APPS of them, until the gadget restarts. The store is the I/O worker's: every call below is made from a job
 * (apps_io_submit), never from another task, so it takes no lock. Each run gets a fresh copy (apps_ram_load), which the
 * app task frees when that app ends.
 */
#define APPS_RAM_APPS 8

typedef struct {
    char id[APPS_ID_MAX + 1], name[25], version[24];
} apps_ram_entry_t;

/* Keeps pkg (a block of apps_port_realloc, as all of its own are) in place of the package with its id. It takes pkg
 * whatever happens: false, with pkg freed, when APPS_RAM_APPS packages of other ids are kept already. */
bool apps_ram_put(apps_pkg_t *pkg);

/* A deep copy of id's package into out, every block of it from apps_port_realloc, for the caller to free (apps_pkg_free
 * with apps_port_free) or to give to apps_task_run. False, with out zeroed, when there is none or no memory for it. */
bool apps_ram_load(const char *id, apps_pkg_t *out);

/* Whether a package of that id is kept. */
bool apps_ram_has(const char *id);

/* Frees id's package. False when there was none. */
bool apps_ram_remove(const char *id);

/* Up to max of the kept apps, in the order they were first committed; returns how many. */
int apps_ram_list(apps_ram_entry_t *out, int max);

/* ops->commit for the RAM store: a deep copy of pkg, kept by apps_ram_put. *unchanged is always false. Fails with
 * "out of memory: no room to keep the app in RAM", or with "the gadget keeps at most 8 apps in RAM; restart it to make
 * room" when the store is full of other ids. */
bool apps_ram_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t err_len);
