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
 * The I/O worker of Muse apps: one task, "muse_apps_io", that runs jobs one at a time, in the order they were
 * submitted. Installs run on it (apps_install), and so does every use of the app store: the RAM store now, the flash
 * store's reads and writes from Task 20 (X11: a task with a PSRAM stack never touches flash), the snapshot's upload
 * from Task 25.
 *
 * The task has a stack of APPS_IO_STACK bytes in internal RAM and priority APPS_IO_PRIORITY, on core APPS_IO_CORE: its
 * long console lines (a 6 KB reply spins about 0.53 s on the driverless UART) and its heavy steps (SHA-256, package
 * copies, the result's trim) never come in front of the app task or LVGL, on core 1. It is made when a job comes and
 * there is none, and it deletes itself once it has had no job for APPS_IO_IDLE_MS, logging the
 * least stack it had left: "io: idle, stack %u of %u free". So it costs no internal RAM while nothing is being
 * installed.
 *
 * A job may wait (an install waits for the app task: a run's settle and 7 s more), but it must end: the jobs behind it
 * wait for it. A job may submit another.
 */

#include <stdbool.h>

#define APPS_IO_STACK 6144        /* bytes, internal RAM */
#define APPS_IO_PRIORITY 3        /* over the app task (2), under the console (5) and the screen's tasks */
#define APPS_IO_CORE 0            /* the app task and LVGL are on core 1 */
#define APPS_IO_IDLE_MS 2000      /* the worker goes after this long without a job */
#define APPS_IO_HEADROOM 16384    /* internal RAM a submit leaves free besides the worker's stack */
#define APPS_IO_BLOCK_MARGIN 1024 /* what the largest free internal block keeps besides a new worker's stack and TCB */
#define APPS_IO_JOBS 8            /* jobs that can wait at once */

/* What a caller says when apps_io_submit refuses: internal RAM dips while Wi-Fi joins, and that passes. */
#define APPS_IO_REFUSED "the gadget is short of memory right now; try again in a few seconds"

/* Makes the worker's lock (static) and its queue of APPS_IO_JOBS (PSRAM). muse_apps_start() calls it once, before
 * anything can submit; a submit before it, or after it failed, is refused. */
void apps_io_init(void);

/*
 * Queues fn(arg) for the worker, making the worker if there is none. Any task, the worker's own jobs included; it
 * never waits for another task's progress. True once the job is queued: fn(arg) then runs exactly once, on the worker.
 * False, with nothing queued (fn never runs, and arg stays the caller's), when internal RAM is short
 * (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < APPS_IO_STACK + APPS_IO_HEADROOM), when APPS_IO_JOBS jobs are
 * waiting already, when there is no worker and the largest free internal block can't hold a new one's stack, its TCB
 * and APPS_IO_BLOCK_MARGIN (heap_caps_get_largest_free_block), or when the worker can't be made; each says why in one
 * W log line. Callers try again later: a store flush at its next tick, an install by saying APPS_IO_REFUSED.
 */
bool apps_io_submit(void (*fn)(void *arg), void *arg);
