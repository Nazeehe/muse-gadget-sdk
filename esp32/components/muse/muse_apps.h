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
#define MUSE_APPS_VM_FAILED "failed"                /* anything else: the VM's answer, a dropped session, a bad path */

typedef struct muse_apps_vm_ops {
    bool (*read)(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                 int64_t deadline_us, const char **code, char *err, size_t err_len);
    bool (*write)(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                  size_t err_len);
} muse_apps_vm_ops_t;

/* The VM's files from now on (NULL: none). ops is kept, not copied: a static table. Any task, before or after muse_apps_start. */
void muse_apps_set_vm_ops(const muse_apps_vm_ops_t *ops);

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

/* Makes the app layer on screen (apps_lv_build) and takes its touches and swipes. In build_overlays(), after the
 * page dots and before the downloaded image, under the display lock and before the frame timer exists. */
void muse_apps_ui_build(lv_obj_t *screen);

/* The LVGL task, every frame, before update_sleep(): the hang supervisor (a call into the app that has run more than
 * APPS_TASK_HUNG_MS), the reset of an app that ended (before any apply in the frame), the front and back the app task
 * asked for (a front once the app's table is published), the layer's place after a voice turn has ended, and the
 * cards (neither of these two during a turn). */
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
