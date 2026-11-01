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
 * The apps' `vm` module (connected apps Task 3): an app's folder on Muse's VM, workspace/apps/<id>/, read, watched and written
 * while the app runs. A hook on the VM polls a connector and writes cache files there; the app reads them, watches a small rev
 * file for changes, and sends commands through outbox/, which the hook applies and acks in state/acks.json. Portable C, like the
 * runtime it belongs to (apps_rt.h): every function here runs on the app task, and the requests go to the platform through
 * apps_port_vm_submit (apps_port.h), which on the gadget hands them to the network worker (apps_net.h), the only task that waits
 * on the VM.
 *
 * The calls (apps_vm_funcs[], with the "vm" permission, APPS_PERM_VM; sys.has("vm") is true). A path is relative to the app's
 * folder and follows the package's path rules (apps_path_valid: a-z 0-9 _ . / -, at most 64 characters, no ".." and no part that
 * starts with a dot). A callback always comes later, on the app task, never during the call: as one call of the app with
 * APPS_BUDGET_CALL (apps_rt_callback), whose error crashes the app as a timer's does.
 * - vm.read(path, [opts,] cb): cb(value) or cb(nil, err). The kind is the ending: .json a decoded value (APPS_VM_JSON_MAX), .txt a
 *   string (APPS_VM_TXT_MAX), .jpg and .jpeg a muse.image decoded on the worker (APPS_VM_JPEG_MAX; opts.circle and opts.max, 1 to
 *   APPS_RT_IMAGE_SIDE_MAX px, as apps_jpeg.h has them), .bin a muse.image as apps_image_check takes it (APPS_VM_BIN_MAX).
 * - vm.write(path, value, [cb]): value as JSON, at most APPS_VM_WRITE_MAX bytes, to a path in outbox/; cb(true) or cb(false, err).
 * - vm.command(name, args, [cb]): writes outbox/cmd-<seq>.json, {"seq":<seq>,"name":<name>,"args":<args>}, where seq is
 *   max(the last seq + 1, the wall clock in epoch ms), then reads state/acks.json every APPS_VM_ACK_POLL_MS until it lists seq:
 *   cb(true, result), the entry's "result" (nil when it has none); cb(false, "timed out") after APPS_VM_ACK_MS. name is 1 to
 *   APPS_VM_NAME_MAX characters of a-z, 0-9 and _; args a table, nil or any value json.encode takes, at most APPS_VM_ARGS_MAX bytes.
 *   Without cb the command is done once it is written.
 * - vm.watch(path, [every_s,] cb): reads path (at most APPS_VM_WATCH_MAX bytes) every every_s seconds (APPS_VM_EVERY_MIN_S to
 *   APPS_VM_EVERY_MAX_S, APPS_VM_EVERY_DEFAULT_S if left out), counted from the end of its last read, while the app is in front and
 *   the screen is lit (P12). cb(text) on the first read and whenever the text changes; cb(nil, err) when a read fails, once for each
 *   change of outcome, so the same error again is quiet and a success after it calls back (P15). Returns a handle (APPS_WATCH_MT,
 *   protected, X17) with h:cancel(); tostring(h) is "watch#<n>". At most APPS_VM_WATCHES_MAX an app.
 *
 * The heartbeat. While the app holds a watch and is in front with the screen lit, outbox/active.json is written,
 * {"app":"<id>","n":<watches>}: at once, then every APPS_VM_HEARTBEAT_MS, until the app goes behind, the screen goes dark or the last
 * watch is cancelled.
 *
 * The acks (P3): state/acks.json is {"acks":[{"seq":<int>,"result":<any JSON>}, ...]}, newest last, holding at most the last 16
 * applied commands. A command whose seq is in the list is acked, and its result (nil if absent) goes to its callback. A file that
 * isn't there or doesn't read as that is a poll that found nothing: the command waits for the next poll or its time.
 *
 * Limits (P11). The app's own calls (read, write, command) wait their turn: one at the VM at a time, and at most APPS_VM_QUEUE more,
 * a command awaiting its ack among them; one past that gets "too many vm requests at once" through its callback. The polls of the
 * watches and of the acks and the heartbeat's writes are the module's: they share the one turn at the VM, one at a time, and
 * coalesce (one pending a watch, one acks read for every command that waits, one heartbeat), so they never take a place among the
 * 8 and the 8 never refuse them. The turns alternate (R3a), so a request of the app's waits behind one poll at most, and the
 * requests never starve the polls. Every request has APPS_VM_REQUEST_MS on the worker, and a busy file reader or writer is tried again
 * APPS_VM_BUSY_RETRIES times, APPS_VM_BUSY_WAIT_MS apart.
 *
 * Errors (P10). A mistake in a call's own arguments is raised at the call, "vm.<name>: ...", and makes no request: a bad path or
 * ending, a write outside outbox/ (APPS_VM_MSG_OUTBOX), a value that won't encode or encodes too large, a bad command name or args,
 * every_s out of range or a fifth watch, opts.max out of range, a missing callback where one is needed (read, watch), and the clock
 * not set (APPS_VM_MSG_NO_CLOCK, vm.command). What depends on the VM or the queue comes through the callback: the messages below,
 * and the JPEG decoder's and the runtime images' (apps_jpeg.h, apps_rt.h).
 *
 * Results while behind or dark (P13). A read's, a write's and a command's callback runs when its result comes, in front or behind,
 * lit or dark, and acks are polled until they come or time out, whatever the state; only the watches and the heartbeat pause.
 *
 * Where the bytes become values (P14, P2). The worker returns bytes. JSON is decoded into Lua on the app task (so "not valid JSON"
 * is said there), a JPEG is decoded on the worker into an image block that the callback's call adds (apps_rt_image_add), and a .bin
 * is checked by that add. Every buffer for VM bytes comes from apps_port_realloc, through the apps' floor: a refusal is
 * APPS_VM_MSG_NO_MEMORY.
 *
 * Memory. The module's books are part of the runtime's block (apps_vmio_size); each request is a job (apps_vm_job_t, a block of
 * apps_port_realloc), and the callbacks are in a table of the app's registry, released when the app stops or crashes
 * (apps_vmio_release), after which no callback runs and no request is made.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_pkg.h"
#include "apps_rt.h"

#define APPS_VM_QUEUE 8              /* an app's own requests waiting, beside the one at the VM */
#define APPS_VM_WATCHES_MAX 4        /* watches an app may hold */
#define APPS_VM_REQUEST_MS 10000     /* a request's deadline on the worker */
#define APPS_VM_ACK_MS 15000         /* a command's wait for its ack */
#define APPS_VM_ACK_POLL_MS 1000     /* how often state/acks.json is read while a command waits */
#define APPS_VM_HEARTBEAT_MS 10000   /* how often outbox/active.json is written while watching */
#define APPS_VM_BUSY_RETRIES 3       /* a busy file reader or writer is tried again this many times */
#define APPS_VM_BUSY_WAIT_MS 500     /* this far apart */
#define APPS_VM_WATCH_MAX 1024       /* a watched file's bytes */
#define APPS_VM_WRITE_MAX 8192       /* a written value's JSON */
#define APPS_VM_ARGS_MAX 4096        /* a command's args' JSON */
#define APPS_VM_NAME_MAX 32          /* a command's name */
#define APPS_VM_EVERY_MIN_S 1        /* a watch's period, s */
#define APPS_VM_EVERY_MAX_S 60
#define APPS_VM_EVERY_DEFAULT_S 2
#define APPS_VM_JSON_MAX (32 * 1024)
#define APPS_VM_TXT_MAX (16 * 1024)
#define APPS_VM_JPEG_MAX (256 * 1024)
#define APPS_VM_BIN_MAX APPS_IMAGE_MAX

/* The watch handle's metatable. */
#define APPS_WATCH_MT "muse.watch"

/* What the callbacks get (the brief's words, which the guide's Common errors quote). */
#define APPS_VM_MSG_NOT_CONNECTED "the VM is not connected"
#define APPS_VM_MSG_NO_FILE "no such file on the VM: %s"           /* the app's path */
#define APPS_VM_MSG_TOO_LARGE "%s is larger than %u KB"
#define APPS_VM_MSG_NOT_JSON "%s is not valid JSON: %s"            /* the path, the decoder's message */
#define APPS_VM_MSG_NO_MEMORY "the gadget is short of memory"
#define APPS_VM_MSG_TIMED_OUT "timed out"
#define APPS_VM_MSG_READER_BUSY "the VM file reader is busy"
#define APPS_VM_MSG_WRITER_BUSY "the VM file writer is busy"
#define APPS_VM_MSG_TOO_MANY "too many vm requests at once"
/* What the calls raise. */
#define APPS_VM_MSG_OUTBOX "only outbox/ can be written"
#define APPS_VM_MSG_NO_CLOCK "the gadget's clock isn't set yet"

/* A full path on the VM: APPS_VM_DIR, the id, '/', the app's path and the NUL. */
#define APPS_VM_PATH_MAX (sizeof APPS_VM_DIR + APPS_ID_MAX + 1 + APPS_PATH_MAX)
#define APPS_VM_ERR_MAX 192

/* What the worker does, and what it makes of a read. */
typedef enum { APPS_VM_READ = 1, APPS_VM_WRITE } apps_vm_op_t;
typedef enum { APPS_VM_AS_BYTES = 0, APPS_VM_AS_JPEG } apps_vm_as_t;

/*
 * One request and, once the worker has run it, its result: a block of apps_port_realloc that the module makes on the app task, the
 * worker runs and hands back (apps_net.h), and the app task gives to apps_vmio_take, or frees (apps_vm_job_free) when its app has
 * ended. data is the job's own block of apps_port_realloc: a write's bytes going out, a read's bytes coming back (NULL for an empty
 * file), or a JPEG's decoded image block.
 */
typedef struct apps_vm_job {
    struct apps_vm_job *next;      /* the worker's list of results */
    uint32_t app_gen;              /* the app it is for: the port's submit stamps it */
    uint32_t req;                  /* the module's: the request's id, or a watch's life */
    uint32_t epoch;                /* the module's: the books it was made from (apps_vmio_release begins new ones) */
    uint8_t kind;                  /* the module's: whose request it is */
    uint8_t slot;                  /* a watch's slot */
    uint8_t op;                    /* apps_vm_op_t */
    uint8_t as;                    /* a read's apps_vm_as_t */
    bool circle;                   /* a JPEG's: the corners outside the circle transparent (apps_jpeg.h) */
    uint16_t max_px;               /* a JPEG's longest side */
    uint32_t max;                  /* a read's most bytes */
    uint8_t rel;                   /* where the app's own path starts in path: messages show it */
    char path[APPS_VM_PATH_MAX];   /* workspace/apps/<id>/<path> */
    uint8_t *data;
    size_t len;
    bool ok;                       /* the result: done, or err says why */
    char err[APPS_VM_ERR_MAX];
} apps_vm_job_t;

/* Frees a job and its data. NULL is ignored. Any task. */
void apps_vm_job_free(apps_vm_job_t *job);

/* The module's books, which live in the runtime's block (apps_rt.c): their size, and their start, for the runtime rt (its id is the
 * folder's name). */
typedef struct apps_vmio apps_vmio_t;
size_t apps_vmio_size(void);
void apps_vmio_init(apps_vmio_t *v, apps_rt_t *rt);

/* Installs the global `vm` and the watch handle's metatable. apps_svc_open's protected call runs it, with the other modules: a heap
 * that refuses raises there, and the VM is out of memory. */
void apps_vmio_open(lua_State *L, apps_rt_t *rt);

/* The app stopped or crashed: every callback is released (the registry's table goes), and every request, watch and result is
 * dropped; a job at the worker comes back to apps_vmio_take, which frees it. No callback runs and no request is made after it, until
 * a stopped runtime is started again. luaL_unref allocates nothing, so it is safe outside a call. */
void apps_vmio_release(apps_vmio_t *v, lua_State *L);

/* The runtime is freed: the blocks the books hold (the VM goes with the runtime, so the registry is left alone). */
void apps_vmio_free(apps_vmio_t *v);

/* The app task, outside a call, for a RUNNING app whose job it is (apps_task.c drops a job whose app has ended, by its app_gen): the
 * worker's result. It runs no app code: a result whose callback is due waits for apps_vmio_run_one. Takes the job. */
void apps_vmio_take(apps_rt_t *rt, apps_vm_job_t *job, int64_t now_ms);

/* The app task, each round, outside a call, for a RUNNING app (P12): active is "in front and lit". The commands that time out, the
 * heartbeat's start and stop, and, when nothing is at the worker, the next request. Turns alternate (R3a): after a poll the app's
 * oldest waiting request, after one of its requests a due poll (an acks poll, the heartbeat, a watch that is due only while active,
 * in that order); a kind goes twice in a row only when the other has nothing waiting. It runs no app code. */
void apps_vmio_tick(apps_rt_t *rt, int64_t now_ms, bool active);

/* The monotonic time the loop should come back by for the module: now (0) when a callback waits or a request is ready to go; else
 * the earliest deadline (a command's time, a poll, the heartbeat, a watch's read while active); INT64_MAX when there is none. */
int64_t apps_vmio_next_deadline(const apps_rt_t *rt);

/* Whether a callback waits (apps_vmio_run_one would make a call). */
bool apps_vmio_has_work(const apps_rt_t *rt);

/* Runs the callback that is due first (a refusal's, then the results in the order they came) as one call of the app: true when one
 * ran; then the caller checks apps_rt_state, as after any call. The app task, bracketed like any call. */
bool apps_vmio_run_one(apps_rt_t *rt);
