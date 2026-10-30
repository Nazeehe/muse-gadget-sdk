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
 * The one install pipeline of Muse apps, whatever the files come from: the USB console's stage (apps_console.c), and
 * Muse's VM (apps_install_from_vm, below: Link's app.install). Portable C: the files come in through a source, and the
 * steps that need the app task and the store through ops, so the host tests run it with fakes. It runs on the I/O worker
 * (apps_io.h), whose stack is 8 KB: everything big is on the heap (apps_port_realloc: PSRAM on the gadget).
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
 *    A source that can't read a file says so itself ("reading %s: %s"), and may give the result's code (read_failed
 *    otherwise). One that ran out of memory (out_of_memory) while the app being installed runs adds "; <id> is running,
 *    and an update needs room for two copies of it: stop it, then install again" (C1: the old copy goes only when the
 *    new one runs).
 * 4. ops->compile. Lua's own message ("main.lua:2: unexpected symbol near 'x'") becomes "compile failed: " and
 *    apps_vm_excerpt's text of it for the file it names (the line before, the line and the line after, ">2|" on the
 *    line itself); a message that names none of the package's files is shown as it is, after "compile failed: ".
 *    The text is clean (apps_text_clean): Lua's message and the lines are the package's own bytes, and a '?' stands for
 *    each control byte but the newlines between the lines and tab, each C1 character and each byte that isn't UTF-8.
 *    apps_pkg_compile_message makes that text, and apphost's check calls it too (apps_pkg_check_source says the
 *    compiled chunk of step 3 and apps_rt_compile compiles, for the app task and for apphost's check alike).
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
 *    files had to go, "unchanged":true when the store found the package it already had, and "hint" when the store has one
 *    (ops->hint, in RAM mode: APPS_RAM_HINT when the gadget has no apps partition, apps_store.h's APPS_STORE_NO_FAT_HINT
 *    when it has one that couldn't be mounted). Without run, "state" is
 *    "installed", "warnings" the manifest's, and "log" and "ui" are empty. "ui" is the widget dump (apps_ui.h) as
 *    raw JSON (a cJSON_Raw item). On failure:
 *      {"ok":false,"error":{"code":<one of the codes below>,"message":<the message above>}}
 *
 * The package is the pipeline's from step 2 to step 6: its blocks are apps_port_realloc's, so that apps_task_run can
 * free it, and it is freed on every way out that doesn't give it to ops->run. It keeps manifest.json's bytes
 * (manifest_json), which the store writes as they came.
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
#define APPS_ERR_BUSY "busy"                        /* apps_install_try_begin, for its callers; a VM reader someone has */
#define APPS_ERR_UNAVAILABLE "unavailable"          /* for callers: what the gadget can't do now (no VM ops) */

/* What a caller answers when apps_install_try_begin says no. */
#define APPS_INSTALL_BUSY "busy: another install is running"

/* What a result adds when the apps are kept in RAM (no apps partition: apps_store.h). */
#define APPS_RAM_HINT "reflash over USB once to keep apps"

/*
 * Where the files come from. read reads the app's file rel (a path apps_path_valid passed, or "manifest.json") into
 * *data, a block of apps_port_realloc whose owner the caller becomes, and its length into *len; *data is never NULL on
 * success (an empty file is a block of one byte). It needs to read no more than max + 1 bytes: a file longer than max
 * may come back with more than max bytes, and the pipeline refuses it. It gives up at deadline_us, an absolute time in
 * microseconds of apps_port_mono_ms's clock. False, with nothing to free, when it can't: err says why, as
 * "reading <rel>: <why>", and *code (NULL when it is called) may say the result's code, read_failed when it stays NULL.
 * origin names the source in the callers' log lines: "usb" or "vm".
 */
typedef struct {
    bool (*read)(void *ctx, const char *rel, size_t max, int64_t deadline_us, uint8_t **data, size_t *len,
                 const char **code, char *err, size_t err_len);
    void *ctx;
    const char *origin;
} apps_source_t;

/*
 * The steps that need the gadget. compile checks every .lua file and runs nothing (apps_task_compile: Lua's own
 * message on failure). commit keeps a copy of the package in the store (*unchanged true when the store had it already).
 * run takes the package whatever happens, runs it in front and lets it settle for settle_ms, and fills st (C2:
 * apps_task_run(pkg, true, ...)). storage says where the store keeps apps: "ram" or "flash"; hint what a result adds about
 * that, NULL for nothing (apps_store_hint).
 */
typedef struct {
    bool (*compile)(const apps_pkg_t *pkg, char *err, size_t n);
    bool (*commit)(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n);
    bool (*run)(apps_pkg_t *pkg, int settle_ms, apps_status_t *st, char *err, size_t n);
    const char *(*storage)(void);
    const char *(*hint)(void);
} apps_install_ops_t;

/* One install at a time, whatever it comes from (the console, Link): false while another holder has it. The holder
 * calls apps_install_end once its install has ended. Any task. */
bool apps_install_try_begin(void);
void apps_install_end(void);

/* The pipeline above. NULL only when there is no memory for even the result. */
cJSON *apps_install_run(const char *id, bool run, const apps_source_t *src, const apps_install_ops_t *ops);

/*
 * An install from Muse's VM (Task 22's app.install), for the I/O worker's job: apps_install_run(id, run, <the VM>, ops),
 * whose source reads each file of the app through vm->read (muse_apps.h) from "workspace/apps/<id>/<rel>", a path made
 * here from the id and a path the manifest's parse passed, into a block of apps_port_realloc that grows as the file comes,
 * never past the file's limit. What a read that fails gives the result:
 *   install timed out after 90 s (reading <rel>)    read_failed: the reader said its deadline came, or the install's
 *                                                   deadline (APPS_INSTALL_DEADLINE_MS) had passed when the read failed
 *                                                   or was due to start (a read is never started after it)
 *   <the reader's words>                            busy: someone else has the reader (a display.draw_file)
 *   reading <rel>: <the reader's words>             out_of_memory (the reader's no memory, or the block's), or read_failed
 *   <rel> is larger than <N> KB                     invalid_package: more than the file's limit came, as the pipeline says
 * Logged (tag muse_apps): each file read, "install <id> from vm: <rel> <bytes> bytes in <ms> ms", and the install once it has
 * ended, "install <id> from vm done: ok|failed (<files> files, <ms> ms)", the files the ones read whole (manifest.json among
 * them) and the time the whole install's (compile, commit and run included). NULL only when there is no memory for even the
 * result.
 */
#define APPS_VM_DIR "workspace/apps/"  /* where Muse writes an app's folder on its VM, relative to its home */

struct muse_apps_vm_ops;
cJSON *apps_install_from_vm(const char *id, bool run, const struct muse_apps_vm_ops *vm, const apps_install_ops_t *ops);

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

/* What a message shows of text someone typed (an id, a path, a command): apps_text_shown (apps_pkg.h) with this max,
 * into room for APPS_SHOWN_MAX + 4 bytes. */
#define APPS_SHOWN_MAX 64

/*
 * The RAM store (C4): on a gadget without the apps partition (apps_store.h's RAM mode), a committed package stays in
 * PSRAM, one for each id, at most APPS_RAM_APPS of them, until the gadget restarts or the store drops it. Its copies don't
 * keep manifest.json's bytes (manifest_json is NULL in them): nothing reads them back from RAM. The store is the I/O worker's: every call
 * below is made from a job (apps_io_submit), never from another task, so it takes no lock. Each run gets a fresh copy
 * (apps_ram_load), which the app task frees when that app ends: an app that runs is in PSRAM twice (C4, E26).
 *
 * Recency. The packages are kept most recent first: a commit (an install, with or without run) and a load (a run) make
 * theirs the most recent. After every change (a commit, a load, a drop, a remove) the store gives the launcher its list
 * (apps_task_set_list), so the LVGL task never asks the store (X11).
 *
 * Room. Before a copy (a commit's, a load's), free PSRAM (apps_port_psram_free) after it must still hold the app's heap
 * (its memory_kb) and APPS_RAM_RESERVE (apps_room, apps_pkg.h). Short of that, the store drops its oldest packages, one at
 * a time and looking again after each, never the app that runs (apps_task_running) nor the one being copied, each with one
 * W log line, "ram: dropped <id> to make room"; short still, the copy is refused with APPS_MSG_SHORT_OF_MEMORY, "the gadget
 * is short of memory for <id>: <F> KB free, <N> KB needed; restart it or install a smaller app" (F what is free then, N
 * the copy, the heap and the reserve, rounded up). A commit of a new id into a full store drops the oldest the same way.
 * A load beside another app that is loaded (N3) is admitted by apps_room_beside instead, with what that app's end frees
 * (apps_task_footprint): the copy and the reserve now, and the copy, the heap and the reserve once it has gone, the drops
 * as above; short still, APPS_MSG_SHORT_ONCE_ENDED or APPS_MSG_SHORT_BESIDE, and the other runs on.
 */
#define APPS_RAM_APPS 8

typedef struct {
    char id[APPS_ID_MAX + 1], name[25], version[24], icon[APPS_PATH_MAX + 1];
} apps_ram_entry_t;

/* A deep copy of id's package into out (a caller's apps_pkg_t), every block of it from apps_port_realloc, for the caller
 * to free (apps_pkg_free with apps_port_free) or to give to apps_task_run; id becomes the most recent. False, with out
 * zeroed and why in err ("no such app on the gadget", the store's refusal for room above, "out of memory: no room to load
 * the app"), when there is none, no room or no memory for it. */
bool apps_ram_load(const char *id, apps_pkg_t *out, char *err, size_t err_len);

/* Whether a package of that id is kept. */
bool apps_ram_has(const char *id);

/* Frees id's package. False when there was none. */
bool apps_ram_remove(const char *id);

/* Up to max of the kept apps, most recent first; returns how many. */
int apps_ram_list(apps_ram_entry_t *out, int max);

/* ops->commit for the RAM store: a deep copy of pkg, kept in place of its id's (pkg stays the caller's); its id becomes
 * the most recent. *unchanged is always false. Fails with the store's refusal for room above, or with "out of memory: no
 * room to keep the app in RAM"; either way the store's copy of that id, if any, stays. */
bool apps_ram_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t err_len);

/* A copy of id's manifest into m (overwritten), its messages_json a block of apps_port_realloc (apps_manifest_clear(m,
 * apps_port_free)): apps_store_manifest in RAM mode. False, with m zeroed, when there is none or no memory. Recency is
 * unchanged. */
bool apps_ram_manifest(const char *id, apps_manifest_t *m);
