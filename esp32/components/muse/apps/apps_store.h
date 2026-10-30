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
 * APPS_STORE_BASE, with the apps' values, their alarms' records, the quarantine and the launcher's order in an NVS partition of
 * their own, "apps_nvs" (namespace "apps"). The pairing's "nvs" is never opened, written or erased here. A gadget without the
 * partition (one updated over the air, which never rewrites the table) keeps its apps in PSRAM: the RAM store of apps_install.h (at
 * most APPS_RAM_APPS), and the values in RAM (apps_values.c). Every result and device.health then say "storage":"ram", and results
 * add a hint (apps_store_hint): APPS_RAM_HINT, or APPS_STORE_NO_FAT_HINT for a partition that is there and couldn't be mounted.
 *
 * Who calls what (X11: a task with a PSRAM stack never touches flash, reads included):
 * - the I/O worker (apps_io.h), alone, mounts, commits, removes, loads and reads or writes apps_nvs: every function marked
 *   "worker" below runs from a job or an op;
 * - any task reads what the mount and the worker's changes cache: the mode, the hint, the list of apps and the room left;
 * - the app task's rounds call apps_store_tick (once the boot lets them, I2), which submits the mount job until the worker has taken it, and the write of the
 *   launcher's order that the write budget held back (apps_values.h), once it fits; and take the alarms' records the mount read
 *   (apps_store_take_records), which the app task arms again (apps_values.h).
 *
 * The mount. muse_apps_start() makes it the worker's first step (apps_io_set_first), and the app task's first apps_store_tick
 * submits the boot job that brings the worker, once the boot lets it (I2: after the Link's registration, at once without the Link,
 * or APPS_BOOT_DEADLINE_MS after boot: muse_apps.h); apps_store_tick asks again every APPS_STORE_RETRY_MS while the worker refuses.
 * In order:
 *   1. the partition: none, and the store is in RAM mode ("store: no apps partition: ... reflash over USB once to keep apps");
 *   2. FAT at APPS_STORE_BASE on "apps": format_if_mount_failed, 4 files open at most, units of 4 KB (a partition with no file
 *      system is formatted: approved for its first mount). A mount that fails even so is RAM mode for this boot, with its own line
 *      and hint ("store: the apps partition couldn't be mounted: apps are kept in RAM until the gadget restarts"): a reflash
 *      wouldn't help, a restart may;
 *   3. apps_nvs: nvs_flash_init_partition, and when it is full or of a newer NVS (ESP_ERR_NVS_NO_FREE_PAGES,
 *      ESP_ERR_NVS_NEW_VERSION_FOUND) nvs_flash_erase_partition("apps_nvs") and an init again; then its namespace;
 *   4. recovery (Layout, below);
 *   5. the list: every folder whose manifest.json parses for the folder's name, in the launcher's order (Order, below), at most
 *      APPS_STORE_APPS_MAX;
 *   6. the quarantine check (Quarantine, below), with one line, "boot: reset reason ...";
 *   7. the alarms' records (Alarms, below) read, for the app task;
 *   8. one line: "store: flash: mounted in <ms> ms[, a new file system], <F> KB free; apps_nvs <used> of <total> entries used, <n>
 *      apps", and the launcher is given the list (apps_task_set_list).
 * Until it has run, the store is empty and not ready: apps_store_mode() is APPS_STORAGE_NONE, apps_store_list() 0 and a load
 * "storage not ready", and nothing here blocks.
 *
 * Layout. An app is the folder APPS_STORE_BASE/<id>, manifest.json and its files at their paths. A commit writes the package to its
 * stage, APPS_STORE_BASE/.stage-<id>, then 1. renames <id> to .old-<id>, 2. renames the stage to <id>, 3. deletes .old-<id>. Every
 * rename stays in APPS_STORE_BASE: FatFs rewrites a folder's ".." only when it changes parent, and that rewrite is a window in which
 * a reset leaves two entries on one folder (f_rename's own warning). Power lost at any point leaves either the old app or the new one:
 * the mount's recovery puts .old-<id> back when <id> is missing, and deletes every other leftover (a stage, an old copy whose app is
 * there, and the .stage folder of an earlier layout). APPS_STORE_BASE/.store marks a file system the store has made its own (a mount
 * that doesn't find it says "a new file system"). Nothing is recursive: the worker's stack is 8 KB (apps_io.h).
 *
 * apps_nvs, namespace "apps": s<key> (the id): the id, a NUL, then the values' JSON; a<key> (the id, a slash and the alarm's name): an
 * alarm's record (Alarms); q<key> (the id): the id, quarantined; and "order" (Order). A <key> is the CRC-32 (zlib's) of what it is
 * for, in 8 hex digits, which another id can share (CRC-32 is easy to collide on purpose): so what a key holds names its owner, and
 * every read, write and erase finds the owner's own key among APPS_STORE_KEY_PROBES, the CRC-32 and the next ones (+1, +2, ...),
 * and writes a new one into the first that is free. A key whose blob is another's is never written or erased for this one.
 *
 * Order. The launcher's order, most recently used first, is the store's, not the file system's (FAT's times come from a clock that
 * may not be set): an install (changed or unchanged) and a launch (a load for a run, an alarm's included) make the app the first, a
 * remove drops it, and each of those writes the ids, NUL-terminated, most recent first, to "order", within the write budget
 * (apps_values.h: one that doesn't fit is written later, apps_store_tick). At the mount, an id of "order" with no folder is left out,
 * and a folder that "order" doesn't name goes after the rest, by id. The list, the launcher and app.list all have this order.
 *
 * Quarantine. The marker (apps_marker_t) is in RTC memory, which keeps it across a reset but not a power-on. The app task marks an
 * app's launch and its clean end (any end that isn't a reset: a stop, a crash of the app that the runtime caught, an exit, a
 * replacement). At the mount, a reset that wasn't a crash (esp_reset_reason(): anything but a panic, a watchdog, a brownout or a
 * CPU lockup) is a clean stop first; then apps_marker_boot counts the reset if an app was running, and the second in a row
 * quarantines it: q<key> is written, its alarms go (their records and the armed ones), and its loads are refused with
 * "<id> was running during two restarts in a row; reinstall it to run it again" until a commit of it (a reinstall, unchanged or
 * not) clears it. The boot's line has the marker's id and its count of resets as read: "boot: reset reason <r>; app marker <id>,
 * resets <n>, <what came of it>".
 *
 * Alarms. An alarm armed while the clock is valid has a record: its app, its name, its time as epoch ms and what it asked for. The
 * app task keeps the records in line with the names that the app, or the system, touched since the boot (apps_values.h): about a
 * second after one changes, it hands the worker each touched name with what the alarm table has of it (armed with its time, armed
 * with no clock, or gone), and apps_store_sync_alarms writes or drops the records of those names and of no other. The mount reads
 * the records (one that can't be read for now is left as it is; one that isn't a record, or whose time isn't between 2025 and 2100,
 * goes), drops those of an app that isn't installed (unless the list couldn't be read) or is quarantined, and hands the rest to the
 * app task (apps_store_take_records), which arms each again for the time left unless the app touched its name first: at once if the
 * clock is valid, else on the first round that sees it valid. A quarantine and a remove drop all of the app's records here, and tell
 * the app task to forget those it was handed (apps_values_forget_records), so that a reinstall arms nothing old.
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
#define APPS_STORE_NVS "apps_nvs"    /* the values', alarms', quarantine's and order's NVS partition */
#define APPS_STORE_NAMESPACE "apps"
#define APPS_STORE_ORDER_KEY "order" /* the launcher's order */
#define APPS_STORE_APPS_MAX 32       /* apps the flash store keeps: a commit of another new id is refused */
#define APPS_STORE_RETRY_MS 2000     /* a mount the worker refused is asked for again this often */
#define APPS_STORE_UNIT 4096         /* the FAT's allocation unit */
#define APPS_STORE_KEY_PROBES 8      /* the keys a blob may have: its CRC-32 and the next 7 */
#define APPS_STORE_RECORDS_MAX 16    /* records the mount hands the app task: twice the alarm table */
#define APPS_STORE_EPOCH_MIN 1735689600000LL  /* 2025-01-01: a record's time before this, or */
#define APPS_STORE_EPOCH_MAX 4102444800000LL  /* after 2100-01-01, isn't a record's */
#define APPS_STORE_NO_FAT_HINT "the gadget couldn't open its app storage; restart it to try again"
#define APPS_STORE_NO_SUCH_APP "no such app on the gadget"  /* a remove's or a load's refusal of an id it has no folder for */

typedef enum { APPS_STORAGE_NONE, APPS_STORAGE_RAM, APPS_STORAGE_FLASH } apps_storage_t;

/* "none", "ram" or "flash": what results, device.health and the heartbeat say. */
const char *apps_storage_name(apps_storage_t s);

/* ---- The I/O worker's ---- */

/* The mount (above). Its mode; it runs once a boot (a second call does it all again, for the host tests). */
apps_storage_t apps_store_mount(void);

/* apps_store_mount unless the store is mounted: the worker's first step (apps_io_set_first) and the boot job's. */
void apps_store_mount_once(void);

/*
 * Keeps a copy of pkg (the install pipeline's: pkg stays the caller's) as its id's, in place of the one there, and makes it the most
 * recent. FLASH: when manifest.json and every file are the stored copy's, byte for byte (the files' SHA-256 against the stored files'),
 * nothing is written to the FAT and *unchanged is true; otherwise the room is checked first ("not enough room on the gadget for <id>:
 * <F> KB free, <N> KB needed; remove an app you don't use"), then the stage is written and put in place (Layout). Either way a
 * quarantine of the id is cleared. RAM: apps_ram_commit. False with why in err: the room, "the gadget keeps at most 32 apps; remove one
 * first", "couldn't write <id> to the gadget's storage: <what>" (the old copy stays), "storage not ready", or the RAM store's refusals.
 */
bool apps_store_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n);

/* Deletes the app id: its folder (or its RAM copy and RAM values), its values, its alarms' records, its quarantine and its place in
 * the order, and cancels its armed alarms. False with why in err: "<id> is running; stop it first", APPS_STORE_NO_SUCH_APP,
 * "storage not ready" or "couldn't remove <id>: <what>". */
bool apps_store_remove(const char *id, char *err, size_t n);

/* FLASH: id's values, json[0, len), in its s key ("{}" erases the key: nothing is kept for an empty store). True when nothing is left
 * to do: written, or moot (id isn't installed: it was removed). False when apps_nvs refused it (full, a write error, every key it could
 * have taken another's). The write budget is the caller's (apps_values.c takes it before it queues the save). */
bool apps_store_save_values(const char *id, const char *json, size_t len);

/* FLASH: id's values, a block of apps_port_realloc with a NUL after *len bytes, which the caller frees with apps_port_free; "{}" when
 * nothing is saved for id. NULL, *len 0, when they are there and can't be read now (no memory for them, an error of apps_nvs): the
 * launch is then refused, and never starts with an empty store that a later save would put in place of the real one. */
char *apps_store_load_values(const char *id, size_t *len);

/* FLASH: drops the record of the alarm name of id, or every record of id when name is NULL. Nothing when there is none. */
void apps_store_drop_alarm(const char *id, const char *name);

#define APPS_STORE_SYNC_MAX 16  /* names in one sync */

/* What the alarm table has of a name an app touched (apps_store_name_t's state). */
enum {
    APPS_STORE_ARMED,           /* armed, and the clock is valid: epoch_ms is when it fires */
    APPS_STORE_ARMED_NO_CLOCK,  /* armed with no valid clock: when it fires as an epoch isn't known */
    APPS_STORE_GONE,            /* not armed: cancelled, told or given up on */
};

/* A name an app touched since its last sync, as the alarm table has it now (apps_values.h). */
typedef struct {
    char name[APPS_ALARM_NAME_MAX + 1];
    uint8_t state;     /* APPS_STORE_ARMED, _ARMED_NO_CLOCK or _GONE */
    int64_t epoch_ms;  /* ARMED: when it fires */
    bool sound, wake;  /* ARMED: what it asked for */
} apps_store_name_t;

/* FLASH: the records of the n names of id that its app, or the system, touched (apps_values.h's op), and of no other name (P1). An
 * ARMED name's record is written unless it holds that already (its time within a second, the same sound and wake), which costs nothing;
 * a write takes its cost from the write budget first (apps_values.h), and when that doesn't fit, the name's old record is dropped at
 * once (its time is the alarm's that the app replaced: no record, which arms nothing after a reset, is better than a wrong one) and the
 * name goes back to the app task (apps_values_record_waits), which syncs it again once the budget has room; a write that apps_nvs
 * refuses drops the old record too, and says so. An ARMED_NO_CLOCK name's record is dropped (the app armed it again, and its time isn't
 * known: the app task syncs it again when the clock comes), and so is a GONE one's. Drops never wait: an erase costs nothing. With all,
 * every record of id whose name isn't among the n goes too: the system cancelled all of id's alarms (apps_alarm_take_cancelled), or the
 * app touched more names than the app task keeps. Nothing is written for an app that isn't installed. At most APPS_STORE_SYNC_MAX
 * names. */
void apps_store_sync_alarms(const char *id, const apps_store_name_t *names, int n, bool all);

/*
 * A fresh deep copy of id's package into pkg (a caller's apps_pkg_t, overwritten), every block of it from apps_port_realloc, for the
 * caller to free (apps_pkg_free with apps_port_free) or to give to apps_task_run, and the app made the most recent (a load is for a run).
 * The manifest's bytes and parse, and every file it lists with its SHA-256. FLASH admits the load first (C1, apps_room in apps_pkg.h):
 * free PSRAM must hold the files (their sizes on the FAT), the heap (none for the app that runs: a launch of it only fronts it) and
 * APPS_RAM_RESERVE; beside another app that is loaded, the files and the reserve now, and the files, the heap and the reserve once that
 * app has gone, with what its end frees (N3: apps_room_beside, apps_task_footprint), the other running on when it is refused. False,
 * with pkg zeroed and why in err: "no such app on the gadget", "storage not ready", the quarantine's refusal, the admission's
 * (APPS_MSG_SHORT_OF_MEMORY, APPS_MSG_SHORT_ONCE_ENDED, APPS_MSG_SHORT_BESIDE), "the stored copy of <id> is damaged (<what>); install it
 * again", "out of memory: no room to load the app", or the RAM store's (RAM: apps_ram_load).
 */
bool apps_store_load(const char *id, apps_pkg_t *pkg, char *err, size_t n);

/* id's manifest alone (D26: the list's and app.api's), into m (overwritten): its messages_json a block of apps_port_realloc
 * (apps_manifest_clear(m, apps_port_free)). False, with m zeroed, when there is none. */
bool apps_store_manifest(const char *id, apps_manifest_t *m);

/* ---- Any task: what the mount and the worker's changes cache ---- */

/* Up to max of the apps, in the launcher's order (RAM: most recently used, as the RAM store keeps them); returns how many. 0 before
 * the mount. */
int apps_store_list(char ids[][APPS_ID_MAX + 1], char names[][25], int max);

/* Whether the store has an app of that id (false before the mount). */
bool apps_store_has(const char *id);

/* APPS_STORAGE_NONE until the mount has run, then FLASH or RAM for the rest of the boot. */
apps_storage_t apps_store_mode(void);

/* What results and app.list add about where apps are kept: NULL on flash and before the mount; in RAM mode APPS_RAM_HINT when the
 * table has no apps partition, APPS_STORE_NO_FAT_HINT when it has one that couldn't be mounted. */
const char *apps_store_hint(void);

/* apps_storage_name(apps_store_mode()), as a function: apps_install_ops_t.storage for the store's callers (a commit, a launch, Link's
 * install and app.control's launch). Any task. */
static inline const char *apps_store_storage(void) {
    return apps_storage_name(apps_store_mode());
}

/* The room left: FLASH the FAT's free space as of the last change, RAM the apps' heap's free PSRAM, 0 before the mount. */
uint32_t apps_store_free_kb(void);

/* ---- The app task's ---- */

/* Each round: submits the mount job until the worker has taken it (again APPS_STORE_RETRY_MS after a refusal), and once mounted, the
 * write of the order that the budget held back, once it fits. Never waits. */
void apps_store_tick(int64_t now_ms);

/* One alarm's record, as the mount read it. */
typedef struct {
    char id[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t epoch_ms;
    bool sound, wake;
} apps_store_record_t;

/* The records the mount read, of installed and unquarantined apps, at most max of them (APPS_STORE_RECORDS_MAX): the first call once the
 * mode isn't NONE takes them, and every other call gets 0 (RAM mode has none). */
int apps_store_take_records(apps_store_record_t *out, int max);

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
