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
 * submitted. Installs run on it (apps_install), and so does every use of the app store: the RAM store, the flash
 * store's reads and writes (X11: every FAT and NVS access runs here, which keeps flash waits off the app task and LVGL),
 * the snapshot's upload from Task 25.
 *
 * The task has a stack of APPS_IO_STACK bytes in PSRAM (its TCB is in internal RAM, as every TCB is) and priority
 * APPS_IO_PRIORITY, on core APPS_IO_CORE: its long console lines (a 6 KB reply spins about 0.53 s on the driverless UART)
 * and its heavy steps (SHA-256, package copies, the result's trim) never come in front of the app task or LVGL, on core 1.
 * A stack in PSRAM may write flash on this firmware (ruling X18): code and rodata run from PSRAM, so IDF keeps the cache on
 * through a flash write (apps_io.c refuses to build otherwise). So the worker takes none of the internal RAM the Link needs
 * but its TCB, and no rule of internal RAM decides whether it can be made.
 *
 * It is made when a job comes and there is none. Once it has had no job for APPS_IO_IDLE_MS it logs the least stack it had
 * left, "io: idle, stack %u of %u free", and parks (suspends itself); the next submit deletes it, with its stack and its
 * TCB, before it makes the next worker. A worker can't delete itself: IDF's vTaskDeleteWithCaps(NULL) makes a cleanup task
 * in internal RAM and aborts when it can't, and the boot's first worker goes idle while the Link's handshake has internal
 * RAM at its lowest.
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
 *
 * Only the app task submits ops (it registers itself as it starts: apps_io_register_op_task, and a submit from any other task asserts),
 * so the ring has one pusher, which the take-back of a refused op relies on. An op may run nested inside a job's wait: the job is the
 * worker, and apps_io_assist runs the ops from its await, so an op must never take a lock that a job can hold across a call of
 * apps_task_* (the job would wait for the app task with the lock, and the op for the lock, in the job's own stack). Task 20's flash ops
 * inherit this.
 */

#include <stdbool.h>

#define APPS_IO_STACK 8192        /* bytes, PSRAM: the deepest store step left 2328 of 6144 free */
#define APPS_IO_PRIORITY 3        /* over the app task (2), under the console (5) and the screen's tasks */
#define APPS_IO_CORE 0            /* the app task and LVGL are on core 1 */
#define APPS_IO_IDLE_MS 2000      /* the worker goes after this long without a job */
#define APPS_IO_JOBS 8            /* jobs that can wait at once */
#define APPS_IO_OPS 8             /* ops that can wait at once */

/* What a caller says when apps_io_submit refuses: no memory for a new worker, or its queue full, and that passes. */
#define APPS_IO_REFUSED "the gadget is short of memory right now; try again in a few seconds"

/* Makes the worker's lock (static) and its queue of APPS_IO_JOBS (PSRAM). muse_apps_start() calls it once, before
 * anything can submit; a submit before it, or after it failed, is refused. */
void apps_io_init(void);

/* fn runs on the worker once, before the first job or op that comes after this call: the store's mount (apps_store.h), so that
 * whatever is submitted first (the boot's mount job, an install, a launch, a values' op) finds the store mounted. muse_apps_start()
 * sets it before anything can submit. A worker that idles out and is made again doesn't run it again. */
void apps_io_set_first(void (*fn)(void));

/*
 * Queues fn(arg) for the worker, making the worker if there is none (and deleting the one that parked before it). Any
 * task, the worker's own jobs included; it never waits for another task's progress. True once the job is queued: fn(arg)
 * then runs exactly once, on the worker. False, with nothing queued (fn never runs, and arg stays the caller's), when
 * APPS_IO_JOBS jobs are waiting already, or when there is no worker and none can be made (no memory for its stack in
 * PSRAM or its TCB); each says why in one W log line. Callers try again later: a store flush at its next tick, an
 * install by saying APPS_IO_REFUSED.
 */
bool apps_io_submit(void (*fn)(void *arg), void *arg);

/* The calling task is the one that submits ops: the app task calls this as it starts, before it can ask the store for anything. A later
 * call, by another task, makes that the submitter instead (a test's own thread does, in the cases that have no app task of their own). */
void apps_io_register_op_task(void);

/*
 * Queues op(arg) for the worker, to run on it in the order of the ops queued (an op never runs before one queued before it has
 * ended), as soon as the worker is between jobs or a job that waits for the app task looks (apps_io_assist). ONLY THE APP TASK (the one
 * that registered itself with apps_io_register_op_task): the take-back below needs one pusher, and any other task asserts. It never
 * waits. True once the op is queued: op(arg) then runs exactly once. False, with
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
