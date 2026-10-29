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
 * An app's saved object (the `store` module's values), behind apps_port_store_load and apps_port_store_save (apps_port.h says what the
 * runtime asks of them, and apps_port_idf.c forwards them here), and the alarms' records. Every function but apps_values_forget is the
 * app task's: they call apps_io_submit_op, which only the app task may.
 *
 * The backend is chosen at run time, by the store's mode (apps_store.h), which the mount fixes before the I/O worker runs any op (it is
 * the worker's first step): FLASH, the op reads and writes apps_nvs (apps_store_load_values, apps_store_save_values); RAM, a map in PSRAM
 * that survives an app's restart and not the gadget's, of at most APPS_VALUES_RAM_APPS apps (a save for another drops the values of the
 * app saved longest ago, with one log line: a bound on memory that has no place on flash, which drops no other app's values and deletes
 * an app's with the app).
 *
 * What it promises, whatever the backend: a save that was taken (true) is applied in the order the saves were made and before a load
 * that comes after it, whatever the worker's moods in between; and a load that waited is not charged to the app (apps_port.h).
 *
 * A refusal and a failure are two things. A save the worker refuses (its ring of ops is full, or there is no worker and none can be
 * made) waits in a slot of its app and is offered again (apps_values_save, apps_values_tick). A save the backend fails (apps_nvs full,
 * a write error) is not offered again: the values stay in the runtime's cache, the next change saves them again, and the failure is
 * said once per streak (until a save succeeds): apps_values_take_failure hands the app task the app it was for, which writes
 * APPS_VALUES_FULL in that app's log if it runs, and in the gadget's log.
 *
 * The alarms' records (apps_store.h, Alarms) follow the alarm table: apps_values_alarms_changed marks an app whose alarms changed (armed,
 * cancelled, told, given up on, all cancelled), and apps_values_tick, APPS_VALUES_ALARM_DELAY_MS after the first mark (so a burst of
 * changes is one write, never inside the call that made them, and no app can write apps_nvs more than once a second), reads the app's
 * alarms (apps_alarm_list_info) and hands the worker an op that makes its records match: written for each alarm, with the time it fires
 * as epoch ms when the clock is valid (kept as it was when it isn't), and dropped for every other name. FLASH only: in RAM mode nothing
 * outlives a reset, and the marks are dropped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_VALUES_RETRY_MS 2000        /* a save the worker hasn't taken is offered again this often */
#define APPS_VALUES_RAM_APPS 16          /* the RAM map's apps */
#define APPS_VALUES_ALARM_DELAY_MS 1000  /* a change of an app's alarms reaches its records at most this late */
#define APPS_VALUES_FULL "store: the gadget's app storage is full; values weren't saved"

/* The object saved for `app`: a block of apps_port_realloc with a NUL after its *len bytes, which the caller frees with apps_port_free;
 * "{}" when nothing is saved. NULL, with *len 0, when it can't be had: no memory, or the worker didn't answer within APPS_STORE_WAIT_MS. */
char *apps_values_load(const char *app, size_t *len);

/* Saves json[0, len), one JSON object, as `app`'s, in place of what was there. The bytes are copied before this returns. True when the object
 * is held: queued for the worker, or, when the worker refuses it, kept in PSRAM in a slot of the app's own, and offered again by
 * apps_values_tick every APPS_VALUES_RETRY_MS until the worker takes it; so a true is a save that is not lost to a refusal, only to the
 * gadget's restart before the worker takes it (or to the backend's failure, above). A later save of the app takes the place of one that
 * waits, and a load of the app is given what waits (it is the newest). False when it can't be held: no memory for the copy, an id or an
 * object the runtime never makes, or all 16 slots taken by other apps' saves; the caller says what that costs it. */
bool apps_values_save(const char *app, const char *json, size_t len);

/* The app task's every round: offers the saves that wait for the worker once APPS_VALUES_RETRY_MS has gone since they were last offered,
 * and the alarms' records that are due (above). It never waits. */
void apps_values_tick(int64_t now_ms);

/* The app whose save the backend failed, once per streak: true, with its id in app (cap bytes), the first time the app task asks after
 * the streak began; false otherwise. */
bool apps_values_take_failure(char *app, size_t cap);

/* app's alarms changed: its records follow at a tick (above). */
void apps_values_alarms_changed(const char *app);

/* The I/O worker's: the RAM map forgets app's values (its remove, in RAM mode). */
void apps_values_forget(const char *app);
