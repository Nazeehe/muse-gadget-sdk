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
 *
 * Ops. The app task's own needs of the worker (the store's loads and saves, which touch flash from Task 20 on, and so are the worker's,
 * X11) can't wait their turn among the jobs: a launch is a job, which waits for the app task, and the app task's load of the store of the
 * app it is making would wait for that job to end. So the worker has a second, smaller queue, of ops: quick steps, which run on the worker
 * in the order they were submitted, whichever job is running: the worker runs those that are waiting after each job and before it goes, and
 * a job that waits for the app task runs them as it looks (apps_io_assist, which the app task's callers call each time they look).
 * An op must be short and must not wait for another task.
 */

#include <stdbool.h>

#define APPS_IO_STACK 6144        /* bytes, internal RAM */
#define APPS_IO_PRIORITY 3        /* over the app task (2), under the console (5) and the screen's tasks */
#define APPS_IO_CORE 0            /* the app task and LVGL are on core 1 */
#define APPS_IO_IDLE_MS 2000      /* the worker goes after this long without a job */
#define APPS_IO_HEADROOM 16384    /* internal RAM a submit leaves free besides the worker's stack */
#define APPS_IO_BLOCK_MARGIN 1024 /* what the largest free internal block keeps besides a new worker's stack and TCB */
#define APPS_IO_JOBS 8            /* jobs that can wait at once */
#define APPS_IO_OPS 8             /* ops that can wait at once */

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

/*
 * Queues op(arg) for the worker, to run on it in the order of the ops queued (an op never runs before one queued before it has
 * ended), as soon as the worker is between jobs or a job that waits for the app task looks (apps_io_assist). Any task, but the
 * order is for one submitter: the app task. It never waits. True once the op is queued: op(arg) then runs exactly once. False, with
 * nothing queued (op never runs and arg stays the caller's), when APPS_IO_OPS are waiting already, or when there is no worker and none
 * can be made (apps_io_submit's admission, and one W log line says why), so that nothing could ever run it. When a worker is there but
 * a job couldn't be queued to bring it, the op is queued all the same: the worker runs it when its job ends, or the app task's callers
 * do. The caller says what a refusal means: a store's save is asked again at the next tick.
 */
bool apps_io_submit_op(void (*op)(void *arg), void *arg);

/*
 * Runs the ops that are waiting, in order, if the caller is the worker; nothing for any other task. For a job of the worker that waits
 * for the app task (an install's or a launch's run): the app task, which can't answer before its load of the app's store has been
 * served, needs the op the job is holding up. The waits of apps_task.c (await) call it each time they look.
 */
void apps_io_assist(void);
