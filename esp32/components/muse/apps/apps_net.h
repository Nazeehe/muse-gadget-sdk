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
 * The network worker of Muse apps (connected apps Task 3): one task, "muse_apps_net", that runs the apps' VM requests (apps_vmio.h's
 * jobs) one at a time, in the order they came, through the VM ops (muse_apps_vm_ops, main/apps_link.c: vm_file; a read as app_read,
 * which yields the reader to the firmware's own: R3b). It is the only task
 * that waits on the VM for an app: not the Noise session's task (whose frames the VM ops wait for), not the I/O worker (whose jobs
 * would wait behind a stalled read), not the app task and never the LVGL task. A JPEG is decoded here too (apps_jpeg.h), into the
 * image block the app task adds (P2).
 *
 * The task has a stack of APPS_NET_STACK bytes in PSRAM (its TCB in internal RAM, as every TCB is), priority APPS_NET_PRIORITY (with
 * the I/O worker, over the app task) and core APPS_NET_CORE, away from the app task and LVGL. It is made by the first submit and stays.
 *
 * A job runs once, with a deadline of APPS_VM_REQUEST_MS from when it begins, and a busy reader or writer is tried again
 * APPS_VM_BUSY_RETRIES times, APPS_VM_BUSY_WAIT_MS apart, within it. Its result is said in the callback's words (apps_vmio.h): no ops,
 * or a session that is gone, is "the VM is not connected"; the VM's 404 "no such file on the VM: <path>"; a file over the job's max
 * "<path> is larger than <n> KB"; a deadline "timed out"; a buffer the floor refuses "the gadget is short of memory"; a reader or
 * writer still busy "the VM file reader is busy" (or writer); a JPEG the decoder's own words; anything else the VM's words. A job
 * whose app has ended by the time the worker comes to it isn't run: it is freed. VM bytes are never logged.
 *
 * Results. The worker puts a job it ran on a list and calls done() (the app task's post of APPS_EV_VM_DONE, which never waits); the
 * app task takes the list with apps_net_take in its housekeeping, so a post the queue had no room for loses nothing: the loop comes
 * round within a second. The worker never waits for the app task.
 */

#include <stdbool.h>
#include <stdint.h>

#include "apps_vmio.h"

#define APPS_NET_STACK 8192   /* bytes, PSRAM: the VM ops' reads and the JPEG decoder's frames (apps_jpeg.h) */
#define APPS_NET_PRIORITY 3   /* the I/O worker's: over the app task (2), under the console and the screen's tasks */
#define APPS_NET_CORE 0       /* the app task and LVGL are on core 1 */
#define APPS_NET_JOBS 8       /* jobs that can wait at once (an app sends one at a time) */

/* What the worker asks of its user (muse_apps_start, the app task's glue: apps_task.c): done, called on the worker after each job it
 * ran, never waiting; alive, whether the app of an app_gen still runs, asked before a job is run. Either may be NULL. Before the first
 * submit. */
void apps_net_init(void (*done)(void), bool (*alive)(uint32_t app_gen));

/* Queues job for the worker, making the worker (and its lock and queue) the first time. The app task alone submits (apps_port.h's
 * apps_port_vm_submit). True once queued: the job is the worker's, and comes back through apps_net_take unless its app has ended,
 * when it is freed. False, with the job the caller's, when the worker can't be made (no memory for its stack or TCB) or
 * APPS_NET_JOBS wait already. */
bool apps_net_submit(apps_vm_job_t *job);

/* The jobs the worker has run since the last call, oldest first, linked by next (NULL when there are none). The app task. */
apps_vm_job_t *apps_net_take(void);
