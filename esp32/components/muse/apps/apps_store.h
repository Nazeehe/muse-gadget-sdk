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
 * Where an app's saved object lives: the backend behind apps_port_store_load and apps_port_store_save (apps_port.h says what the
 * runtime asks of them, and apps_port_idf.c forwards them here). apps_store_ram.c is the backend until Task 20: a map in PSRAM
 * that survives an app's restart and not the gadget's. Task 20's flash backend (apps_store.c) replaces that one file, in the build
 * (components/muse/CMakeLists.txt) and nowhere else: it keeps this contract.
 *
 * Every function here is the app task's: they call apps_io_submit_op, which only the app task may.
 *
 * What a backend promises, and Task 20's flash one keeps: a save that was taken (true) is applied in the order the saves were made and before
 * a load that comes after it, whatever the worker's moods in between; and a load that waited is not charged to the app (apps_port.h). What a
 * backend need not promise is the RAM map's own limits: it holds 16 apps and drops the oldest saved for a 17th, which is a bound on memory
 * that has no place on flash (apps_port.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_STORE_RETRY_MS 2000  /* a save the worker hasn't taken is offered again this often */

/* The object saved for `app`: a block of apps_port_realloc with a NUL after its *len bytes, which the caller frees with apps_port_free;
 * "{}" when nothing is saved. NULL, with *len 0, when it can't be had: no memory, or the worker didn't answer within APPS_STORE_WAIT_MS. */
char *apps_store_load(const char *app, size_t *len);

/* Saves json[0, len), one JSON object, as `app`'s, in place of what was there. The bytes are copied before this returns. True when the object
 * is held: queued for the worker, or, when the worker refuses it (the ring of ops is full, or there is no worker and none can be made), kept
 * in PSRAM in a slot of the app's own, and offered again by apps_store_tick every APPS_STORE_RETRY_MS until the worker takes it; so a true is a
 * save that is not lost to a refusal, only to the gadget's restart. A later save of the app takes the place of one that waits, and a load of
 * the app is given what waits (it is the newest). False when it can't be held: no memory for the copy, an id or an object the runtime never
 * makes, or all 16 slots taken by other apps' saves; the caller says what that costs it. */
bool apps_store_save(const char *app, const char *json, size_t len);

/* The app task's every round: offers the saves that wait for the worker (see above) once APPS_STORE_RETRY_MS has gone since they were last
 * offered. It never waits. */
void apps_store_tick(int64_t now_ms);
