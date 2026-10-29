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
 * The store of Muse apps' packages: where an installed app lives, so that it outlives the gadget's restart. On a gadget whose
 * partition table has the "apps" partition (partitions_muse_apps.csv) it is a FAT file system on wear levelling, mounted at
 * APPS_STORE_BASE, with the apps' values, their alarms' records and the quarantine in an NVS partition of their own, "apps_nvs"
 * (namespace "apps"). The pairing's "nvs" is never opened, written or erased here. A gadget without the partition (one updated
 * over the air, which never rewrites the table) keeps its apps in PSRAM: the RAM store of apps_install.h (at most APPS_RAM_APPS),
 * and the values in RAM (apps_values.c). Every result and device.health then say "storage":"ram", and results add the hint
 * APPS_RAM_HINT.
 *
 * Who calls what (X11: a task with a PSRAM stack never touches flash, reads included):
 * - the I/O worker (apps_io.h), alone, mounts, commits, removes, loads and reads or writes apps_nvs: every function marked
 *   "worker" below runs from a job or an op;
 * - any task reads what the mount and the worker's changes cache: the mode, the list of apps and the room left;
 * - the app task's rounds call apps_store_tick, which submits the mount job until the worker has taken it, and the re-arm of
 *   the alarms' records once the clock is valid.
 *
 * The mount. muse_apps_start() makes it the worker's first step (apps_io_set_first) and submits the boot job that brings the
 * worker; apps_store_tick asks again every APPS_STORE_RETRY_MS while the worker refuses. In order:
 *   1. the partition: none, and the store is in RAM mode ("store: no apps partition: ... reflash over USB once to keep apps");
 *   2. FAT at APPS_STORE_BASE on "apps": format_if_mount_failed, 4 files open at most, units of 4 KB (a partition with no file
 *      system is formatted: approved for its first mount). A mount that fails even so is RAM mode too, with the reason;
 *   3. apps_nvs: nvs_flash_init_partition, and when it is full or of a newer NVS (ESP_ERR_NVS_NO_FREE_PAGES,
 *      ESP_ERR_NVS_NEW_VERSION_FOUND) nvs_flash_erase_partition("apps_nvs") and an init again; then its namespace;
 *   4. recovery (Layout, below);
 *   5. the list: every folder whose manifest.json parses for the folder's name, most recently installed first (the folder's
 *      time, which FAT keeps to 2 s; ties by id), at most APPS_STORE_APPS_MAX;
 *   6. the quarantine check (Quarantine, below), with one line, "boot: reset reason ...";
 *   7. the alarms' records (Alarms, below), re-armed now if the clock is valid;
 *   8. one line: "store: flash: mounted in <ms> ms[, a new file system], <F> KB free; apps_nvs <used> of <total> entries used",
 *      and the launcher is given the list (apps_task_set_list).
 * Until it has run, the store is empty and not ready: apps_store_mode() is APPS_STORAGE_NONE, apps_store_list() 0 and a load
 * "storage not ready", and nothing here blocks.
 *
 * Layout. An app is the folder APPS_STORE_BASE/<id>, manifest.json and its files at their paths. A commit writes the package to
 * the stage, APPS_STORE_BASE/.stage/<id>, then 1. renames <id> to .old-<id>, 2. renames the stage to <id>, 3. deletes .old-<id>.
 * Power lost at any point leaves either the old app or the new one: the mount's recovery puts .old-<id> back when <id> is missing,
 * and deletes every other leftover (a stage, an old copy whose app is there). APPS_STORE_BASE/.store marks a file system the
 * store has made its own (a mount that doesn't find it says "a new file system"). Nothing is recursive: the worker's stack is
 * 6 KB.
 *
 * apps_nvs, namespace "apps": s%08x (the CRC-32 of the id): the id, a NUL, then the values' JSON; a%08x (the CRC-32 of "id/name"):
 * an alarm's record (Alarms); q%08x (the CRC-32 of the id): the id, quarantined. A key's own id is checked when it is read.
 *
 * Quarantine. The marker (apps_marker_t) is in RTC memory, which keeps it across a reset but not a power-on. The app task marks an
 * app's launch and its clean end (any end that isn't a reset: a stop, a crash of the app that the runtime caught, an exit, a
 * replacement). At the mount, a reset that wasn't a crash (esp_reset_reason(): anything but a panic, a watchdog, a brownout or a
 * CPU lockup) is a clean stop first; then apps_marker_boot counts the reset if an app was running, and the second in a row
 * quarantines it: q%08x is written, its alarms go (their records and the armed ones), and its loads are refused with
 * "<id> was running during two restarts in a row; reinstall it to run it again" until a commit of it (a reinstall) clears it.
 *
 * Alarms. An alarm armed while the clock is valid has a record: its app, its name, its time as epoch ms and what it asked for. The
 * records follow the alarm table: about a second after an app's alarms change (armed, cancelled, told to it, given up on, all of
 * them cancelled), the app task hands the worker what the table has of the app (apps_values.h), and apps_store_sync_alarms writes a
 * record for each alarm and drops the others; a quarantine and a remove drop all of the app's here. An alarm armed less than a
 * second before a reset, or while the clock isn't valid, has no record, and doesn't outlive the reset. At the mount, each record
 * whose app is installed and not quarantined is armed again for the time left (one already due fires at once, 1 ms), unless the
 * app has armed that name itself since the boot; a record of an app that isn't installed is dropped. Without a valid clock the
 * records wait, and the first apps_store_tick that sees one submits the re-arm. Until then (and between the boot and the mount)
 * apps_port_alarm_remaining says -1 (alarm.remaining is nil), so an app that starts in that window sees no alarm, and the next
 * change of its alarms drops the records it had.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_alarm.h"
#include "apps_pkg.h"

#ifndef APPS_STORE_BASE
#define APPS_STORE_BASE "/apps"      /* where the FAT is mounted (the host tests build with their own) */
#endif
#define APPS_STORE_PARTITION "apps"  /* the FAT's partition */
#define APPS_STORE_NVS "apps_nvs"    /* the values', alarms' and quarantine's NVS partition */
#define APPS_STORE_NAMESPACE "apps"
#define APPS_STORE_APPS_MAX 32       /* apps the flash store keeps: a commit of another new id is refused */
#define APPS_STORE_RETRY_MS 2000     /* a mount or a re-arm the worker refused is asked for again this often */
#define APPS_STORE_UNIT 4096         /* the FAT's allocation unit */

typedef enum { APPS_STORAGE_NONE, APPS_STORAGE_RAM, APPS_STORAGE_FLASH } apps_storage_t;

/* "none", "ram" or "flash": what results, device.health and the heartbeat say. */
const char *apps_storage_name(apps_storage_t s);

/* ---- The I/O worker's ---- */

/* The mount (above). Its mode; it runs once a boot (a second call does it all again, for the host tests). */
apps_storage_t apps_store_mount(void);

/* apps_store_mount unless the store is mounted: the worker's first step (apps_io_set_first) and the boot job's. */
void apps_store_mount_once(void);

/*
 * Keeps a copy of pkg (the install pipeline's: pkg stays the caller's) as its id's, in place of the one there. FLASH: when manifest.json
 * and every file are the stored copy's, byte for byte (the files' SHA-256 against the stored files'), nothing is written and *unchanged
 * is true; otherwise the room is checked first ("not enough room on the gadget for <id>: <F> KB free, <N> KB needed; remove an app you
 * don't use"), then the stage is written and put in place (Layout), and a quarantine of the id is cleared. RAM: apps_ram_commit. False
 * with why in err: the room, "the gadget keeps at most 32 apps; remove one first", "couldn't write <id> to the gadget's storage: <what>"
 * (the old copy stays), "storage not ready", or the RAM store's refusals.
 */
bool apps_store_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n);

/* Deletes the app id: its folder (or its RAM copy and RAM values), its values, its alarms' records and its quarantine, and cancels its
 * armed alarms. False with why in err: "<id> is running; stop it first", "no such app on the gadget", "storage not ready" or "couldn't
 * remove <id>: <what>". */
bool apps_store_remove(const char *id, char *err, size_t n);

/* FLASH: id's values, json[0, len), as s%08x ("{}" erases the key: nothing is kept for an empty store). True when nothing is left to do:
 * written, or moot (id isn't installed: it was removed). False when apps_nvs refused it (full, a write error). */
bool apps_store_save_values(const char *id, const char *json, size_t len);

/* FLASH: id's values, a block of apps_port_realloc with a NUL after *len bytes, which the caller frees with apps_port_free; "{}" when
 * nothing is saved (or what is under the key isn't id's, or apps_nvs couldn't be opened). NULL, *len 0, without memory. */
char *apps_store_load_values(const char *id, size_t *len);

/* FLASH: the record of the alarm name of id, at epoch_ms. True when nothing is left to do (written, or moot: id isn't installed);
 * false when apps_nvs refused it. */
bool apps_store_save_alarm(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake);

/* FLASH: drops the record of the alarm name of id, or every record of id when name is NULL. Nothing when there is none. */
void apps_store_drop_alarm(const char *id, const char *name);

/* One of an app's alarms as the alarm table has it, for its record. */
typedef struct {
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t epoch_ms;  /* when it fires (0 when the clock wasn't valid) */
    bool sound, wake;
} apps_store_alarm_t;

/* FLASH: id's records made to match its n alarms (apps_values.h's op): every record of a name that isn't among them is dropped; with
 * epochs (the clock was valid) each alarm's is written unless it holds that already (its time within a second, the same sound and wake),
 * and without, the records of the alarms are left as they are. Nothing is written for an app that isn't installed. */
void apps_store_sync_alarms(const char *id, const apps_store_alarm_t *alarms, int n, bool epochs);

/*
 * A fresh deep copy of id's package into pkg (a caller's apps_pkg_t, overwritten), every block of it from apps_port_realloc, for the
 * caller to free (apps_pkg_free with apps_port_free) or to give to apps_task_run: the manifest's bytes and parse, and every file it
 * lists with its SHA-256. False, with pkg zeroed and why in err: "no such app on the gadget", "storage not ready", the quarantine's
 * refusal, "the stored copy of <id> is damaged (<what>); install it again", "out of memory: no room to load the app", or the RAM
 * store's (RAM: apps_ram_load).
 */
bool apps_store_load(const char *id, apps_pkg_t *pkg, char *err, size_t n);

/* id's manifest alone (D26: the list's and app.api's), into m (overwritten): its messages_json a block of apps_port_realloc
 * (apps_manifest_clear(m, apps_port_free)). False, with m zeroed, when there is none. */
bool apps_store_manifest(const char *id, apps_manifest_t *m);

/* ---- Any task: what the mount and the worker's changes cache ---- */

/* Up to max of the apps, most recently installed first (RAM: most recently used, as the RAM store keeps them); returns how many. 0
 * before the mount. */
int apps_store_list(char ids[][APPS_ID_MAX + 1], char names[][25], int max);

/* Whether the store has an app of that id (false before the mount). */
bool apps_store_has(const char *id);

/* APPS_STORAGE_NONE until the mount has run, then FLASH or RAM for the rest of the boot. */
apps_storage_t apps_store_mode(void);

/* The room left: FLASH the FAT's free space as of the last change, RAM the apps' heap's free PSRAM, 0 before the mount. */
uint32_t apps_store_free_kb(void);

/* ---- The app task's ---- */

/* Each round: submits the mount job until the worker has taken it (again APPS_STORE_RETRY_MS after a refusal), and once mounted, the
 * re-arm of the alarms' records the mount left for a valid clock, once the clock is valid. Never waits. */
void apps_store_tick(int64_t now_ms);

/* ---- Quarantine: the marker (portable: the host tests run it) ---- */

#define APPS_MARKER_MAGIC 0x4d415050u  /* "MAPP" */

/* id is the app last launched (kept after its end, for D19's same-id rule), running whether it runs now, resets the resets in a row
 * that it was running during. */
typedef struct {
    uint32_t magic;
    char id[APPS_ID_MAX + 1];
    uint8_t resets;
    uint8_t running;
} apps_marker_t;

/* A launch of id: running. The same id as the last keeps resets; another sets it to 0. A marker with a bad magic is zeroed first. */
void apps_marker_launch(apps_marker_t *m, const char *id);

/* A clean end: nothing runs, and resets is 0. */
void apps_marker_clean_stop(apps_marker_t *m);

/* The boot's look: a bad magic (RTC memory after a power-on is noise) zeroes the marker; an app that was running counts one more reset
 * and runs no more, and at the second in a row its id goes into quarantine_id (APPS_ID_MAX + 1 bytes), resets starts again at 0, and
 * the answer is true. False otherwise. */
bool apps_marker_boot(apps_marker_t *m, char *quarantine_id);

/* The gadget's marker, in RTC memory: the app task's (launches, ends) and the mount's. */
apps_marker_t *apps_store_marker(void);
