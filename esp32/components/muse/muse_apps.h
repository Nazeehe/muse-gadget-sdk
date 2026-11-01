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
 * Muse apps (CONFIG_MUSE_APPS), as the rest of the firmware sees them: the app task (components/muse/apps/apps_task.h
 * has its services) and the hooks muse_ui.c calls on the LVGL task. An app draws on a full-screen layer over the
 * pages, under the downloaded image and the top layer; the launcher and the crash card are cards over that layer.
 *
 * The LVGL task's hooks, in frame_tick's order: muse_apps_ui_turn when the voice mode changes;
 * muse_apps_ui_supervise before update_sleep(), so it runs while the screen sleeps; muse_apps_ui_tick after the
 * chrome. Other tasks never make an LVGL call here: they leave a request that the next frame carries out.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_MUSE_APPS

/* Makes the app task (apps_task.h) and adds muse_apps_console to the console's hooks. Call it once, after
 * muse_ui_start() and before muse_input_start(). Out of memory it returns ESP_ERR_NO_MEM, and the firmware runs on
 * without apps (the console then answers that they aren't running). */
esp_err_t muse_apps_start(void);

/*
 * I2: the boot's start of the apps' storage. What takes internal RAM (the FAT mount, apps_nvs and the I/O worker that mounts
 * them) waits until the Link's boot is past, since the lowest internal RAM of a boot falls between the Link's handshake and its
 * registration. The app task mounts the store at its first round after the first of:
 * - muse_apps_link_registered(): the reply to the first link.register, accepted or not (noise_control.cpp);
 * - muse_apps_link_offline(): the Link won't come up at boot (no Wi-Fi credentials, Wi-Fi not joined, no pairing, or its
 *   connection failed: app.c);
 * - APPS_BOOT_DEADLINE_MS after boot (apps_task.h).
 * Any task, before or after muse_apps_start (a word that comes first is kept); the first word counts, and each wakes the app
 * task with no wait. Until the store is mounted the apps are starting (muse_apps_starting): app.* commands answer unavailable
 * with MUSE_APPS_STARTING, the launcher says it is loading, and an alarm the store re-arms fires once it is mounted (up to the
 * deadline late). A USB console command mounts it at once (the worker's first step).
 */
void muse_apps_link_registered(void);
void muse_apps_link_offline(void);
#define MUSE_APPS_STARTING "apps are starting; try again in a few seconds"
/* Whether the app task is up and the store not mounted yet. Any task. */
bool muse_apps_starting(void);

/*
 * The USB console's ">app.*" lines (apps_console.c), as a muse_input console hook: line comes without its '>', and it
 * returns false for a line that doesn't start "app.". It runs on the console's task (muse_serial: a 3,584-byte stack),
 * keeps its frames small and never waits long: an install or a launch is a job for the I/O worker, which prints the
 * result. Every reply is one line: "@app.ack <path> <seq>", "@app {...}" or "@app.err {...}" (valid JSON).
 *   app.put <id> <path> <seq> <base64>   a piece of a file: seq 0 starts it again, each later one is the last + 1
 *   app.commit <id> [run]                installs what was put (apps_install.h) in the store (apps_store.h: the apps
 *                                        partition, or RAM on a gadget without one), and runs it with "run"
 *   app.launch <id>                      runs an installed app in front
 *   app.stop                             stops the app that runs
 *   app.status                           the app task's record
 *   app.list                             the installed apps, most recent first (as the launcher lists them)
 *   app.send <id> <event> [json]         message(event, data) to the app that runs, and its reply
 */
bool muse_apps_console(const char *line);

/* The console's stage (the files app.put sent, waiting for app.commit) is freed once it has had no put for this long, with one log
 * line: a sender that stopped half-way would otherwise hold up to 2 MB of PSRAM until the next put for another id or a commit. The app
 * task's housekeeping calls muse_apps_console_tick each round; it never waits (a console that holds the stage keeps it). */
#define MUSE_APPS_STAGE_IDLE_MS 600000
void muse_apps_console_tick(int64_t now_ms);

/*
 * Launches the installed app id, from any task, the LVGL task included: it queues app.launch's job on the I/O worker
 * (apps_io.h) and returns at once. The job loads the store's copy and runs it through apps_task_run_by_id, in front or
 * behind, with no settle and nothing printed: an app of that id that runs by then is fronted (or left where it is), not
 * started again, and a failure is one log line ("launch <id>: <why>"). False, with nothing queued, when the job can't
 * be queued: one log line says why. apps_task_launch calls it for an app that isn't running.
 */
bool muse_apps_launch(const char *id, bool front);

/*
 * muse_apps_launch for an app's alarms (apps_task.c's deliver_alarms asks, never waiting): always in front, and the job has an identity,
 * upto, the gen of the last firing of the app that it was asked for (apps_alarm_wanted). The job, on the I/O worker, runs nothing when
 * none of those firings waits by the time it comes to the front of the queue (they were told, cancelled or given up on); otherwise it
 * runs the app through apps_task_run_for_alarm and tells the app task what came of it, whichever way it went
 * (apps_task_alarm_launch_result: started, failed, or missing from the store). False, with nothing queued, for a launch that can't be
 * queued (one log line says why), which the app task asks for again. upto is never 0.
 */
bool muse_apps_launch_alarm(const char *id, uint32_t upto);

/* cJSON's struct, declared here so that this header needs none of cJSON's. */
struct cJSON;

/*
 * Files on Muse's VM, for the apps (Task 22): main/apps_link.c puts vm_file (main/vm_file.h) behind these and hands them over
 * with muse_apps_set_vm_ops (apps_link_init), so that nothing here needs main/. A path is relative to the user's home on the VM,
 * as vm_file takes it. deadline_us is absolute, in esp_timer_get_time()'s microseconds (apps_port_mono_ms's clock, times 1000),
 * and bounds the whole call. Both wait for the VM, whose answers only the Noise session's task delivers: never call them on it.
 *
 * read hands the file to sink a piece at a time (a piece is valid only during the call; sink returns false to stop the read),
 * and is true once the whole file has gone to the sink. max bounds the file: no piece takes it past max, and a longer file
 * fails with MUSE_APPS_VM_TOO_LARGE. write writes len bytes of data as the whole file, made or replaced (POST /fs/upload).
 * Either is false with *code one of the MUSE_APPS_VM_ codes and err (err_len bytes) saying why in the reader's or the
 * writer's words.
 */
#define MUSE_APPS_VM_BUSY "busy"                    /* someone else has the reader (a display.draw_file), or the writer */
#define MUSE_APPS_VM_OUT_OF_MEMORY "out_of_memory"  /* no memory for the request now: try again later */
#define MUSE_APPS_VM_TIMED_OUT "timed_out"          /* deadline_us came first */
#define MUSE_APPS_VM_TOO_LARGE "too_large"          /* read: the file is longer than max */
#define MUSE_APPS_VM_DROPPED "dropped"              /* the session is gone: the request wasn't sent, or the connection dropped */
#define MUSE_APPS_VM_NOT_FOUND "not_found"          /* read: the VM has no such file (HTTP 404) */
#define MUSE_APPS_VM_FAILED "failed"                /* anything else: the VM's answer (a refusal), a bad path */

typedef struct muse_apps_vm_ops {
    bool (*read)(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                 int64_t deadline_us, const char **code, char *err, size_t err_len);
    bool (*write)(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                  size_t err_len);
    /* read, for a running app's own request (its vm module: apps_net.c), which yields to the firmware's (connected apps Task 3,
     * R3b): it never waits for the reader, and is MUSE_APPS_VM_BUSY at once while anyone else holds it or waits for it, while an
     * install's read waits for an app's to end (main/vm_file.h: VM_FILE_OWNER_APP). */
    bool (*app_read)(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                     int64_t deadline_us, const char **code, char *err, size_t err_len);
} muse_apps_vm_ops_t;

/* The VM's files from now on (NULL: none). ops is kept, not copied: a static table. Any task, before or after muse_apps_start. */
void muse_apps_set_vm_ops(const muse_apps_vm_ops_t *ops);

/* The VM's files as last set (NULL: none). Any task. */
const muse_apps_vm_ops_t *muse_apps_vm_ops(void);

/* What the apps' asynchronous calls answer, once, on the I/O worker: a command's result, {"ok":true,"payload":{...}} or
 * {"ok":false,"error":{"code","message"}}, whose ownership passes to the callback (cJSON_Delete it, or hand it on); NULL only
 * when there was no memory for even that. */
typedef void (*muse_apps_done_cb)(void *ctx, struct cJSON *result);

/* Whether id is an app id (^[a-z][a-z0-9_]{1,23}$), any task. If not, err (err_len bytes) says so as an install does: "app id
 * '<id>' must match ^[a-z][a-z0-9_]{1,23}$", the id shown as typed text is (a control character as '?', cut at 64 bytes with
 * "..."). 128 bytes always hold it. */
bool muse_apps_id_valid(const char *id, char *err, size_t err_len);

/*
 * Link's app.install: installs the app in workspace/apps/<id>/ on Muse's VM. One job of the I/O worker reads manifest.json and
 * the files it lists through the VM's ops (each path made on the gadget: app.install never takes one), then checks, compiles,
 * stores and, with run, runs it in front, as the USB console's commit does (apps_install.h: the result is apps_install_run's,
 * with "storage" and the store's "hint"). Any task, the Noise session's included: it never waits.
 *
 * True once the job is queued: done(ctx, result) then runs exactly once, on the I/O worker, after the install has let its hold
 * go (so a next install once the result is read isn't busy). False, with nothing queued and done never called, and *code and
 * *err (literals) saying why (D14's codes):
 *   invalid_params   id is not an app id, or done is NULL
 *   unavailable      the gadget has no VM ops (muse_apps_set_vm_ops)
 *   busy             another install runs, this one's or the USB console's: "busy: another install is running"
 *   out_of_memory    no memory for the job, or the I/O worker can't take it now ("... try again in a few seconds")
 */
bool muse_apps_install(const char *id, bool run, muse_apps_done_cb done, void *ctx, const char **code, const char **err);

/*
 * Link's app.control and app.api (apps_control.c; main/apps_link.c has the commands). Any task, the Noise session's included,
 * which they never hold for long: list and status wait for one job of the I/O worker (the store is the worker's: X11) for at most
 * MUSE_APPS_CONTROL_WAIT_MS, and the others queue theirs and return.
 *
 * Results are a command's: {"ok":true,"payload":{...}} or {"ok":false,"error":{"code","message"}}. The codes are D14's
 * (invalid_params, not_found, busy, unavailable, out_of_memory) and a launch's own (apps_install.h: crashed, not_running). An id
 * the gadget hasn't installed is not_found, MUSE_APPS_NOT_FOUND, and a store that isn't mounted yet is unavailable, "storage not
 * ready". The caller owns a result (cJSON_Delete); NULL only when there was no memory for even a refusal.
 */
#define MUSE_APPS_CONTROL_WAIT_MS 500   /* list and status: the most they wait for the worker, then busy */
#define MUSE_APPS_LOG_LINES 50          /* the log's last lines that status and logs show (D26) */
#define MUSE_APPS_GUIDE_WRITE_MS 20000  /* app.api: the guide's writes end by then (the VM waits 30 s for the result) */
#define MUSE_APPS_GUIDE_DIR "workspace/apps/_guide/"
#define MUSE_APPS_NOT_FOUND "no app '%s' on this gadget (app.control list shows the installed ones)"

/* {"apps":[{"id","name","version","state","last_error","messages":{...}}]}, in the store's order (most recent first). state and
 * last_error are the app task's record for the record's app (the last one that ran), "stopped" and "" for the others; messages
 * are the manifest's, {} for none. Busy when the worker hasn't answered within MUSE_APPS_CONTROL_WAIT_MS (an install holds it,
 * say): the job, when it runs, frees what it made. */
struct cJSON *muse_apps_list(void);

/* The installed app id: apps_install_status's fields (state, id, name, version, front, last_error with its traceback, warnings,
 * log, ui, instructions, mem_kb, peak_kb, limit_kb, worst_dispatch_ms, cpu_pct, dropped_events, stack_free, widgets) and
 * "storage" with the store's "hint". For the record's app they are the record's; for any other, a stopped app as its manifest
 * has it (name, version, limit_kb) and nothing else. The log is its last MUSE_APPS_LOG_LINES lines; with logs the payload is
 * {"id","log"} alone. Waits as muse_apps_list does. */
struct cJSON *muse_apps_status(const char *id, bool logs);

/*
 * launch, stop and remove, queued on the I/O worker (D20) and answered once through done, as muse_apps_install is: true once
 * queued, and done then runs exactly once, on the worker; false, with nothing queued and done never called, and *code and *err
 * (literals) saying why: invalid_params (not an app id, no done) or out_of_memory (no memory for the job, or the worker can't
 * take it now).
 *   launch  runs the store's copy of id in front by id, with APPS_SETTLE_MS (apps_task_run_by_id: an app of that id that runs is
 *           fronted, not started again). The result is muse_apps_status's for it, or the launch's failure (apps_install_launch's
 *           crashed or not_running), not_found, or the store's refusal as unavailable (the quarantine, a damaged copy, no room).
 *   stop    stops the app if id is the one that runs (apps_task_stop): {} either way, or busy with the app task's words.
 *   remove  apps_store_remove: the app's files, values, alarms and quarantine go. {"removed":id}, or busy for the app that runs,
 *           not_found or unavailable.
 */
bool muse_apps_launch_async(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err);
bool muse_apps_stop(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err);
bool muse_apps_remove(const char *id, muse_apps_done_cb done, void *ctx, const char **code, const char **err);

/*
 * Link's app.send (Task 24): message(event, data) to the app id, starting it first when it isn't the one that runs. Queued on the I/O worker
 * (the store is the worker's: X11), so the Noise session's task never waits, and answered once through done, as muse_apps_install is: true
 * once queued, and done then runs exactly once, on the worker; false, with nothing queued and done never called, and *code and *err
 * (literals) saying why: invalid_params (not an app id, an event that isn't 1 to MUSE_APPS_SEND_EVENT_MAX bytes, data over
 * MUSE_APPS_SEND_DATA_MAX bytes, no done) or out_of_memory (no memory for the job, or the worker can't take it now). data_json is JSON text,
 * NULL for none; id, event and data_json are copied. With show, the app is brought to the front once the message has been answered, after
 * the voice turn if one is on (request_front: C11).
 *
 * One deadline for the whole send: MUSE_APPS_SEND_TOTAL_MS from this call (a 1 s margin under the VM's 10 s wait for the result), which
 * the job's wait in the worker's queue, the store's load, the launch and the reply all spend. A job that finds less than
 * MUSE_APPS_SEND_MIN_LEFT_MS left does nothing (no load, no launch, no message) and answers busy, "the gadget was busy for <n> ms; the event
 * wasn't sent, so try again": a retry can't double the event. The same check comes again after a launch.
 *
 * The job, on the worker. When id isn't the app that runs: the store must have it (not_found, MUSE_APPS_NOT_FOUND; "storage not ready" is
 * unavailable), its copy loads (the quarantine and a damaged copy are unavailable, and the load makes it the most recent) and runs behind,
 * through apps_task_run_for_send, in at most the least of APPS_TASK_SEND_LAUNCH_MS and what is left (a failure to start is crashed, when the
 * app's record says it crashed, or not_running, "'<id>' didn't start: <why>"; one not answered in time is timed_out). Then apps_task_message,
 * whose bound is the least of MUSE_APPS_SEND_REPLY_MS and what is left. It covers waiting for room in the app task's queue as well as the
 * handler's reply, so it counts from the post. The result is
 *   {"ok":true,"payload":{"reply":<the handler's return value as JSON, null for none>}}
 * or {"ok":false,"error":{"code","message"}}:
 *   not_found, unavailable   as above
 *   timed_out                no reply in time: MUSE_APPS_SEND_NO_REPLY when the full bound was given, else "the app didn't reply within <n> ms"
 *   app_error                the handler's error() text as it is (the app keeps running), or a reply over MUSE_APPS_SEND_REPLY_MAX bytes
 *   crashed                  the handler was killed or ran out of memory: the app has ended, and the text is its crash record
 *   not_running              "<id> isn't running" (the app ended first)
 *   busy, unavailable, out_of_memory   the app task's own refusals, or no memory for the reply
 */
#define MUSE_APPS_SEND_DATA_MAX 4096                /* data's bytes, as JSON text */
#define MUSE_APPS_SEND_REPLY_MAX 4096               /* the reply's bytes, as JSON text */
#define MUSE_APPS_SEND_EVENT_MAX 64                 /* the event's bytes */
#define MUSE_APPS_SEND_REPLY_MS 5000                /* the app's time to reply, from the message's post */
#define MUSE_APPS_SEND_TOTAL_MS 9000                /* the whole send's, from muse_apps_send: a 1 s margin under the VM's 10 s */
#define MUSE_APPS_SEND_MIN_LEFT_MS 1000             /* less than this left, and the job does nothing and says busy */
#define MUSE_APPS_SEND_NO_REPLY "the app didn't reply within 5 s"
bool muse_apps_send(const char *id, const char *event, const char *data_json, bool show, muse_apps_done_cb done, void *ctx,
                    const char **code, const char **err);

/*
 * Whether s[0, len) is one JSON value (apps_json_validate: depth at most 16) with an object at its top level, for main/, which never
 * includes apps_json.h. Else false with err (err_len bytes) saying why. Any task; it allocates nothing.
 */
bool muse_apps_json_object_valid(const char *s, size_t len, char *err, size_t err_len);

/*
 * app.api (D2, and D3 as the user decided it): one job of the I/O worker writes the guide and the timer example that the firmware
 * carries (apps/apps_guide.md and apps/examples/timer/, EMBED_FILES) to MUSE_APPS_GUIDE_DIR on the VM, as README.md,
 * timer/manifest.json, timer/main.lua and timer/icon.bin (the upload carries the binary), through the VM ops' write. It logs the DMA RAM's free and lowest marks before and after
 * the writes (the session's sends need it), and answers {"delivered":"vm","guide","example","api","screen","fonts","limits",
 * "storage"[,"hint"],"apps":[{"id","name","version","state"}]}; limits.room_kb is what an app's files and its heap may take
 * together now, free PSRAM less APPS_RAM_RESERVE (apps/apps_pkg.h), 0 when less is free. When the VM refuses a write (MUSE_APPS_VM_FAILED) the answer is
 * {"delivered":"inline","write_error",...,"guide_text","example_files":{"manifest.json","main.lua"}} instead (the manifest less its icon: the icon isn't carried inline), with the same facts;
 * a write that is busy, out of memory or out of time fails app.api (busy, out_of_memory, unavailable), to be called again, and so
 * does one whose session is gone (MUSE_APPS_VM_DROPPED: unavailable, with no inline answer, which nothing would read). Each way is
 * one log line that names the path and why. Any task; refusals as above, and unavailable when the gadget has no VM ops.
 */
bool muse_apps_write_guide(muse_apps_done_cb done, void *ctx, const char **code, const char **err);

/*
 * Link's app.control snapshot (Task 25; apps/apps_snapshot.h has how it runs): a JPEG of the gadget's screen, whatever is on it (D22),
 * written to workspace/apps/<id>/snapshot.jpg on Muse's VM through the VM ops' write (D2: no base64 in the result). scale 1 is the
 * screen's 466 x 466, 2 half of it. The screen is woken (muse_input_wake("snapshot"): a dark one lights, a lit one stays lit) and
 * taken on its first lit frame (E9). Any
 * task, the Noise session's included: it never waits. True once queued: done then runs exactly once, on the I/O worker; false, with
 * nothing queued and done never called, and *code and *err (literals) saying why (D14's codes):
 *   invalid_params   not an app id, a scale that isn't 1 or 2 (MUSE_APPS_SNAPSHOT_BAD_SCALE), no done
 *   unavailable      the gadget has no VM ops
 *   busy             a snapshot is already under way (one at a time)
 *   out_of_memory    no room for it now, before anything is drawn (apps/apps_snapshot.h's admission: free PSRAM and the largest
 *                    free block must each hold its peak above APPS_RAM_RESERVE): MUSE_APPS_SNAPSHOT_TRY_SCALE_2 for scale 1 when
 *                    scale 2 fits, else MUSE_APPS_SNAPSHOT_STOP_APP while an app runs and MUSE_APPS_SNAPSHOT_NO_MEMORY when none
 *                    does; no memory for the job; or the I/O worker can't take it now
 * One deadline for the whole snapshot, MUSE_APPS_SNAPSHOT_TOTAL_MS from this call, under app.control's 30 s: a job that starts with
 * less than APPS_SNAPSHOT_MIN_LEFT_MS of it left does nothing and answers busy. The screen has MUSE_APPS_SNAPSHOT_SCREEN_MS from the
 * job's request to be lit and drawn; the write has what is left of the deadline. The result is
 *   {"ok":true,"payload":{"path":"workspace/apps/<id>/snapshot.jpg","format":"jpeg","w","h","bytes","front","warnings":[...],
 *    "ui":[...][,"ui_truncated":N]}}
 * w and h the picture's (466 or 233; a size the encoder refuses is padded with black to a multiple of 16 in the file, and a warning
 * says so); front whether id is the app on screen as it was drawn (it ran, its layer was in front and no card covered it: a
 * downloaded image over the layer isn't seen); ui its widgets as the app
 * task's record has them (at most a second old), for an app that runs, whole entries from the start of the dump so that the result
 * prints in APPS_RESULT_MAX bytes (ui_truncated counts the rest); warnings say when id isn't on screen or the file is padded, and
 * which widgets extend past the round edge (worked out again from the dump; APPS_SNAPSHOT_WARNINGS_MAX bytes of them, then how
 * many more). Or {"ok":false,"error":{"code","message"}}:
 *   not_found        MUSE_APPS_NOT_FOUND; unavailable "storage not ready" for a store that isn't mounted
 *   busy             the job came too late ("the gadget was busy for <n> ms; take the snapshot again"), or an abandoned drawing
 *                    is still under way
 *   unavailable      MUSE_APPS_SNAPSHOT_NO_WAKE (the screen wasn't lit and drawn in time), LVGL or the encoder failed
 *   out_of_memory    the admission's refusal again (the memory went while the job waited in the queue), or no memory for the
 *                    pixels, the encoder or the result
 *   the write's      "couldn't save the snapshot to <path>: <the writer's words>", busy or out_of_memory as the writer says,
 *                    unavailable for anything else (a refusal, a dropped session, the deadline): without the file Muse sees nothing
 */
#define MUSE_APPS_SNAPSHOT_TOTAL_MS 25000   /* the whole snapshot's, from muse_apps_snapshot: 5 s under app.control's 30 s */
#define MUSE_APPS_SNAPSHOT_SCREEN_MS 5000   /* E9: the screen's, from the job's request, to be lit and drawn */
#define MUSE_APPS_SNAPSHOT_FILE "snapshot.jpg"
#define MUSE_APPS_SNAPSHOT_NO_WAKE "the gadget's screen didn't wake for the snapshot; try again"
#define MUSE_APPS_SNAPSHOT_NO_MEMORY "not enough memory for a snapshot right now"
#define MUSE_APPS_SNAPSHOT_TRY_SCALE_2 "not enough memory for a full-size snapshot right now; take it at scale 2"
#define MUSE_APPS_SNAPSHOT_STOP_APP \
    "not enough memory for a snapshot while the app runs; stop it (app.control stop) or make it use less memory, then try again"
#define MUSE_APPS_SNAPSHOT_BAD_SCALE "scale must be 1 (full size) or 2 (half)"
bool muse_apps_snapshot(const char *id, int scale, muse_apps_done_cb done, void *ctx, const char **code, const char **err);

/* Makes the app layer on screen (apps_lv_build) and takes its touches and swipes. In build_overlays(), after the
 * page dots and before the downloaded image, under the display lock and before the frame timer exists. */
void muse_apps_ui_build(lv_obj_t *screen);

/* The LVGL task, every frame, before update_sleep(): the hang supervisor (a call into the app that has run more than
 * APPS_TASK_HUNG_MS), the reset of an app that ended (before any apply in the frame), the front and back the app task
 * asked for (a front once the app's table is published), the layer's place after a voice turn has ended, and the
 * cards (neither of these two during a turn); a launcher left open while the screen is dark closes, which frees its
 * icons (not one that holds for a pick); last, a snapshot app.control asked for, on the first frame the screen isn't dark
 * (apps_snapshot_take_pending). */
void muse_apps_ui_supervise(void);

/* The LVGL task, every frame the screen is awake: the apply step (apps_lv_apply). True while the app layer or a card
 * covers the screen, and frame_tick then draws nothing under it. */
bool muse_apps_ui_tick(void);

/* The LVGL task, when the voice mode changes: in_turn while Muse listens, thinks or speaks. A turn hides an app that is
 * in front (hide() runs) and closes the cards; its end brings the app back (show() runs) if it was in front, or was
 * asked to the front during the turn. */
void muse_apps_ui_turn(bool in_turn);

/* Any task: the launcher opens at the next frame (after a voice turn). A long press on the watch calls this. */
void muse_apps_open_launcher(void);

/* Any task: the app layer is in front. */
bool muse_apps_front(void);

/* Any task: the app layer is in front or a card (the launcher, a crash card) is shown, as of the LVGL task's last
 * frame or change. The watch yields to it (muse_ui_clock_showing). */
bool muse_apps_covering(void);

/*
 * device.health's "apps" (main/noise_control.cpp), any task: {"resident":<id>|null,"state":"running"|"stopped"|"crashed"|"hung",
 * "lua_kb":N,"peak_kb":N,"storage":"flash"|"ram"|"none","storage_free_kb":N}. resident is the app that is loaded (running, or
 * stuck in a call), null otherwise; state and peak_kb are the app task's record (the last app's, once it has ended), lua_kb its Lua
 * heap now (0 with none loaded); storage is the store's mode ("none" until the mount has run, with storage_free_kb 0). The caller
 * owns the object (cJSON_Delete); NULL without memory.
 */
struct cJSON *muse_apps_health(void);

/* The heartbeat's " apps=<resident>/<state> lua=<KB>K" (main/app.c), any task: resident, state and lua_kb as above (resident "" for
 * none, state its name). */
void muse_apps_brief(char *resident, size_t cap, const char **state, unsigned *lua_kb);

#endif
