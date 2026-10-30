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
 * runtime asks of them, and apps_port_idf.c forwards them here); the alarms' records and their re-arm; and the write budget of apps_nvs.
 * Every function but apps_values_forget, apps_values_record_waits, apps_values_forget_records, the budget's and apps_values_init is the
 * app task's: they call apps_io_submit_op, which only the app task may, or keep state only it touches.
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
 * A save that waits. A save the worker refuses (its ring of ops is full, or there is no worker and none can be made), or one the write
 * budget can't take yet (below), waits in a slot of its app and is offered again (apps_values_tick); a later save of the app takes its
 * place, and a load of the app is given it (it is the newest). The app's end (apps_values_flush) writes what waits for it whatever the
 * budget says. A save the backend fails (apps_nvs full, a write error) is not offered again: the values stay in the runtime's cache, the
 * next change saves them again, and the failure is said once per streak of that app (until a save of it succeeds): apps_values_take_failure
 * hands the app task each app whose streak began, which writes APPS_VALUES_FULL in that app's log if it runs, and in the gadget's log.
 *
 * The write budget (FLASH). Every write of apps_nvs, values, an alarm's record or the launcher's order (apps_store.h), costs its blob's
 * length and APPS_VALUES_ENTRY_COST; an erase costs nothing (it writes no entry). The budget starts the boot full, at
 * APPS_VALUES_BUDGET_BYTES, and fills again at APPS_VALUES_BUDGET_PER_DAY (about 24 bytes a second). The arithmetic: 2 MB a day is 512
 * pages of 4 KB; spread over apps_nvs's 16, with what NVS's garbage collection copies of the live entries besides (say a fifth of the
 * partition), each page is erased about 40 times a day, so its 100,000 erases last about 7 years, where an app saving a few KB every
 * second would wear it out in months. A save that doesn't fit waits (above) for the budget to fill: the app's values are kept, and
 * saved less often, which apps_values_take_slowdown says once per streak of the app (until one of its saves fits again). A save is
 * charged when the worker is handed it: one the worker refuses gets its cost back, so the save that replaces it pays once. A record
 * that doesn't fit waits too, and the order. An app's end writes over the budget, which then fills from below zero.
 *
 * The alarms' records (apps_store.h, Alarms). An app's names are touched when the app arms or cancels one through the port, when one of
 * its firings is told to it or given up on, and when the system cancels all of its alarms (its budget, a crash, a remove: through
 * apps_alarm_take_cancelled, every name); apps_values_alarm_touched says so. APPS_VALUES_ALARM_DELAY_MS after the first touch of a burst
 * (so a burst of changes is one write, never inside the call that made them, and no app writes apps_nvs more than once a second), the tick
 * reads the app's alarms (apps_alarm_list_info) and hands the worker an op with each name it touched and what the table has of it: armed
 * with its time (the clock is valid), armed with no clock, or gone (apps_store_sync_alarms). The op writes or drops the records of those
 * names and of no other: a record nothing touched (handed over at the mount or not: one the mount couldn't read, one past the ones it hands
 * over) is never written or dropped. A name armed with no clock loses its old record and is synced again once the clock comes. A record that
 * the budget can't take loses its old one at once and comes back here (apps_values_record_waits), to be synced again once the budget has
 * room; drops never wait. Each app's names are synced on their own: one that waits holds no other. An app that touches more than the tick
 * keeps has all its records follow the table. FLASH only: in RAM mode nothing outlives a reset, and the touches are dropped.
 *
 * The re-arm. The first tick after the mount takes the records the mount read (apps_store_take_records). Each one whose app is still
 * installed and whose name nothing has touched is armed again for the time left (apps_alarm_arm: 1 ms if it is due, the week of
 * APPS_ALARM_MAX_MS at most), at once if the clock is valid, else on the first tick that sees it valid. One that gets no memory is tried
 * again APPS_VALUES_REARM_RETRY_MS later; one the alarm table refuses (no slot) is dropped, record and all. Until then alarm.remaining is
 * nil for it, and an app that arms or cancels its name first has its own way: the re-arm and the app's calls are both the app task's.
 * A remove or a quarantine of the app (the store's, on the worker) has its handed-over records forgotten at the next round
 * (apps_values_forget_records), so that an app installed again under that id before the clock comes is armed nothing of the old one's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_VALUES_RETRY_MS 2000        /* a save the worker hasn't taken is offered again this often */
#define APPS_VALUES_RAM_APPS 16          /* the RAM map's apps */
#define APPS_VALUES_ALARM_DELAY_MS 1000  /* a change of an app's alarms reaches its records at most this late */
#define APPS_VALUES_REARM_RETRY_MS 2000  /* a re-arm that got no memory is tried again this much later */
#define APPS_VALUES_FULL "store: the gadget's app storage is full; values weren't saved"
#define APPS_VALUES_SLOWER "store: saving less often to spare the gadget's flash"
#define APPS_VALUES_BUDGET_BYTES (48 * 1024)            /* the write budget of apps_nvs, full at boot */
#define APPS_VALUES_BUDGET_PER_DAY (2 * 1024 * 1024)    /* what it fills by in a day */
#define APPS_VALUES_ENTRY_COST 32                       /* what a write costs besides its blob: NVS's entry */
#define APPS_VALUES_RECORD_COST (58 + APPS_VALUES_ENTRY_COST)  /* a record's blob (apps_store.c's RECORD_LEN) and its entry */

/* Makes the lock of the budget and of the failures' notices: muse_apps_start(), before any task that calls here exists. */
void apps_values_init(void);

/* The object saved for `app`: a block of apps_port_realloc with a NUL after its *len bytes, which the caller frees with apps_port_free;
 * "{}" when nothing is saved. NULL, with *len 0, when it can't be had: no memory, the backend can't read it now, or the worker didn't
 * answer within APPS_STORE_WAIT_MS. */
char *apps_values_load(const char *app, size_t *len);

/* Saves json[0, len), one JSON object, as `app`'s, in place of what was there. The bytes are copied before this returns. True when the
 * object is held: queued for the worker, or kept in PSRAM in a slot of the app's own when the worker refuses it or the budget can't take
 * it yet, and offered again by apps_values_tick until it is taken; so a true is a save that is not lost to a refusal, only to the
 * gadget's restart before it is written (or to the backend's failure, above). False when it can't be held: no memory for the copy, an id
 * or an object the runtime never makes, or all 16 slots taken by other apps' saves; the caller says what that costs it. */
bool apps_values_save(const char *app, const char *json, size_t len);

/* The app task's every round: the re-arm and the alarms' records that are due (above), and the saves that wait, each offered when the
 * worker may take it again (APPS_VALUES_RETRY_MS after a refusal) and the budget can. It never waits. */
void apps_values_tick(int64_t now_ms);

/* app has ended (the app task, after its runtime's last save): a save of it that waits is queued now, whatever the budget says. */
void apps_values_flush(const char *app);

/* An app whose saves the backend began to fail, once per streak of that app: true, with its id in app (cap bytes), until every such
 * app has been handed over; false otherwise. */
bool apps_values_take_failure(char *app, size_t cap);

/* An app whose saves the budget began to hold back, once per streak of that app: true, with its id in app (cap bytes). */
bool apps_values_take_slowdown(char *app, size_t cap);

/* app's alarm `name` was touched (armed, cancelled, told, given up on), or all of them when name is NULL (the system cancelled them):
 * its records follow at a tick, and a record the mount handed over for that name is armed again no more (above). */
void apps_values_alarm_touched(const char *app, const char *name);

/* Any task (the I/O worker's sync, apps_store.c): the record of app's alarm `name` had to wait for the budget, its old one dropped; the
 * app task syncs the name again once the budget has room. */
void apps_values_record_waits(const char *app, const char *name);

/* Any task (the store's remove and quarantine, on the I/O worker): the records the mount handed over for app are forgotten at the app
 * task's next round, before its re-arm looks at them. */
void apps_values_forget_records(const char *app);

/* The I/O worker's: the RAM map forgets app's values (its remove, in RAM mode). */
void apps_values_forget(const char *app);

/* ---- The write budget of apps_nvs (any task: a lock held for the arithmetic) ---- */

/* Takes bytes from the budget: true when they fit in what it holds now, false (nothing taken) when they don't. force takes them
 * whatever it holds (an app's end, a record the app task checked), which can leave it below zero. */
bool apps_values_budget_take(uint32_t bytes, bool force);

/* How long until bytes fit, in ms: 0 when they do now. */
int64_t apps_values_budget_wait_ms(uint32_t bytes);
