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
 * What the portable core of Muse apps (apps_rt, and what builds on it) needs from the platform. The
 * firmware implements it in apps_port_idf.c; the host runner (tools/muse/apphost) in apphost_port.c and
 * sha256.c. Later tasks add to it.
 *
 * "The app task" is the task that runs the Lua VM. The core calls every function here from it, unless
 * a comment says another task may too.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Milliseconds on a monotonic clock, from any fixed start: the clock the VM's wall-clock backstop and
 * the apps' timers run on. It never jumps, whatever happens to the wall clock. Any task. */
int64_t apps_port_mono_ms(void);

/* The wall clock, as milliseconds since 1970-01-01 UTC. False, leaving *out alone, until the clock is
 * set (SNTP: the device asks muse_time_valid()). It jumps when SNTP sets it, so timers run on
 * apps_port_mono_ms. Any task. */
bool apps_port_epoch_ms(int64_t *out);

/* Whether the screen is dark for sleep (the device: muse_ui_dark()). The apps' timers don't run while it is, and
 * the first round after it ends runs each one that came due. Any task. */
bool apps_port_dark(void);

/* The apps' heap: PSRAM on the device. realloc(3) for n > 0: ptr NULL allocates, otherwise the block is
 * resized and may move; on failure it returns NULL and leaves ptr as it was. It is never called with
 * n == 0. A block comes back with apps_port_free, never with free(). Any task.
 * The floor (C1, apps_pkg.h's room): on the device a request that grows a block (n above what the block has,
 * heap_caps_get_allocated_size) fails when it would leave less than APPS_RAM_RESERVE of PSRAM free, and one of
 * APPS_FLOOR_BIG_BLOCK bytes or more also when the largest free block less n would be under it (the heap works
 * that out by walking every block, so a smaller request doesn't ask). A shrink, or growth within the block, never
 * fails. So the apps run out of memory cleanly, and the rest of the gadget never does because of them.
 * A snapshot never kills the app that runs (N1): on the device, a growth of the app task's (apps_task_current) that the floor or
 * the heap refuses while a snapshot holds blocks (apps_port_snapshot_alloc, and the encoder's: apps_port_snapshot_adopt), and that
 * would fit were they free, waits: after the floor's refusal until the floor passes, after the heap's until the snapshot lets go of
 * some of what it held, polling every APPS_PORT_SNAPSHOT_POLL_MS for at most APPS_PORT_SNAPSHOT_WAIT_MS in all during one snapshot,
 * and is then tried once more. Never under the UI lock (the growth is refused at once). The wait is stolen time (apps_rt_steal_ms, less what others stole meanwhile), so the duty
 * cycle, the 1 s backstop and the hang supervisor don't count it. No other task ever waits here: not the snapshot's job (the I/O
 * worker), not the LVGL task. The app task holds no lock across an allocation (apps_task.c), so nothing the snapshot needs waits
 * behind it. */
#define APPS_FLOOR_BIG_BLOCK (16 * 1024)
#define APPS_PORT_SNAPSHOT_WAIT_MS 1000
#define APPS_PORT_SNAPSHOT_POLL_MS 10
void *apps_port_realloc(void *ptr, size_t n);
void apps_port_free(void *ptr);  /* NULL is ignored. Any task. */

/* The bytes the apps' heap has free now (PSRAM on the device), for the RAM store's room (apps_install.h). Any task. */
size_t apps_port_psram_free(void);

/*
 * The snapshot's side of the floor (N1, apps_snapshot.h; the device's port only, as only it takes snapshots). Any task.
 * - apps_port_psram_largest: the largest free block of the apps' heap now (a walk of the heap under its lock), for the admission.
 * - apps_port_room: whether a new block of n bytes passes the floor now (both clauses), for the encoder's own buffers.
 * - apps_port_snapshot_alloc: a block of n bytes, aligned to APPS_PORT_SNAPSHOT_ALIGN (the JPEG encoder's input), through the
 *   floor, never zeroed and never waiting; counted as the snapshot's while held (apps_port_snapshot_bytes). NULL when refused.
 * - apps_port_snapshot_shrink: the block cut to n bytes in place (the heap trims a shrink where it is: TLSF's realloc), never
 *   refused, the count following; returns the block (p as it was when the heap couldn't cut it). n above the block's size is p.
 * - apps_port_snapshot_free: gives it back (NULL is ignored).
 * - apps_port_snapshot_adopt, apps_port_snapshot_disown: a block the snapshot holds that the port didn't hand out (the JPEG
 *   encoder's, from the wrapped allocators: apps_snapshot.c) counted with the snapshot's bytes while it is held, its size as the
 *   heap has it (heap_caps_get_allocated_size); disown before freeing it. NULL is ignored.
 * - apps_port_snapshot_waits: how many waits of the app task the snapshots' blocks caused since the last call, with *ms what they
 *   came to; the call resets both (the snapshot's job logs them).
 */
#define APPS_PORT_SNAPSHOT_ALIGN 16
size_t apps_port_psram_largest(void);
bool apps_port_room(size_t n);
void *apps_port_snapshot_alloc(size_t n);
void *apps_port_snapshot_shrink(void *p, size_t n);
void apps_port_snapshot_free(void *p);
void apps_port_snapshot_adopt(void *p);
void apps_port_snapshot_disown(void *p);
size_t apps_port_snapshot_bytes(void);
uint32_t apps_port_snapshot_waits(uint32_t *ms);

/* One line of an app's log (what print wrote), for the development loop: the device echoes it on the console as
 * "@app.log <id> <line>", at most 1,024 bytes of them a second (apps_echo.h); a line past that isn't echoed, and the
 * next echoed one comes after "@app.log <id> (<N> lines not echoed)". line is one line of valid UTF-8, at most 200
 * bytes, with no newline and no control byte but tab, NUL-terminated. It returns promptly and never waits for the
 * console: the app task is inside a call, with its wall clock running. The device only queues the line (a small
 * bounded queue: a line it has no room for isn't echoed either), and apps_port_log_drain prints it. */
void apps_port_log(const char *app_id, const char *line);

/* Prints the lines apps_port_log queued, in order, each ended with apps_port_console_flush. The app task calls it after
 * every call into the app ends and in its housekeeping, never inside a call, so the time the console takes (another
 * task's long reply holding it) never counts against an app's wall clock. It holds the console once for the whole queue,
 * so it waits behind at most one other writer's line, and with nothing queued it returns without waiting for the
 * console. A port that prints at once (the host runner) has nothing to do here. */
void apps_port_log_drain(void);

/* Ends a console line printed with printf (a reply of the ">app.*" protocol, an echoed log line): flushes stdout, then
 * syncs it. On the USB-Serial-JTAG console the sync sends the zero-length packet that ends a line landing on a 64-byte
 * packet boundary; without it the host keeps such a line until the console prints something else. The console
 * (apps_console.c) calls it from the serial task and the I/O worker, and the device's apps_port_log_drain from the app
 * task, outside any call; the host runner has none. */
void apps_port_console_flush(void);

/* Makes what the port needs before anything here is used: the firmware's UI lock (a static mutex). The app task's
 * glue calls it once, first thing in muse_apps_start(), before the app task and the LVGL task can use the lock. A
 * port with nothing to make (the host runner) doesn't need it. */
void apps_port_init(void);

/* The lock that keeps the app's widget table steady while the LVGL task reads it (Task 9). Lock takes it,
 * waiting as long as it takes; trylock waits at most ms and says whether it got it; unlock gives it
 * back. Not recursive. */
void apps_port_ui_lock(void);
bool apps_port_ui_trylock(int ms);
void apps_port_ui_unlock(void);

/* The SHA-256 (FIPS 180-4) of data[0, len) into out. data may be NULL when len is 0. Any task. */
void apps_port_sha256(const void *data, size_t len, uint8_t out[32]);

/*
 * The system's alarms, which belong to no app: they run on the monotonic clock, outlive the app that armed them, and, when
 * one fires, the system starts that app again if it isn't running and tells it (alarm(name)). The device keeps them in
 * apps_alarm.c (apps_alarm.h says all of it), apphost on its fake clock. An app is named by its id, an alarm by a name of 1
 * to 23 characters of a-z, 0-9 and _; the arrays of names hold 24 bytes with the NUL. Any task.
 */

/* What arming an alarm came to. */
typedef enum {
    APPS_ALARM_ARMED = 0,
    APPS_ALARM_REFUSED,    /* the app has 4 of other names, the gadget has 8 in all, or an argument isn't valid */
    APPS_ALARM_NO_MEMORY,  /* the gadget has no memory for the alarm's timer just now: asking again later may work */
} apps_alarm_result_t;

/* Arms `name` of `app` to fire ms from now, 1 to 604800000 (a week), replacing the one of that name. With sound the ring
 * goes when it fires, and with wake the screen wakes. Not armed, and nothing else changed (but see apps_alarm.h: a replaced
 * arming that couldn't be started again is gone), for the reasons of apps_alarm_result_t. */
apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake);

/* Disarms it. Nothing for a name that isn't armed. */
void apps_port_alarm_cancel(const char *app, const char *name);

/* Milliseconds until it fires, rounded up (0 when it is due and its app hasn't been told), or -1 if there is no such alarm. */
int64_t apps_port_alarm_remaining(const char *app, const char *name);

/* The names of app's alarms, earliest first, with the time left of each; returns how many (at most 4). */
int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]);

/* One chirp, at the volume the gadget has set; it returns at once (the device asks the voice task, which plays it when it
 * is idle, and keeps one request). */
void apps_port_sound_chirp(void);

/* Ends the ring of an alarm that is going. Nothing when none is. */
void apps_port_sound_stop(void);

/*
 * The store of an app (the `store` module), which the runtime caches and saves (apps_rt.h): one JSON object an app, of at most 16 KB,
 * written by the runtime and read back by it, whole. The device keeps the objects behind the I/O worker (X11: no flash or NVS from
 * a task with a PSRAM stack, reads included), in apps_values.c (apps_values.h): on a gadget with the apps partition they are in
 * apps_nvs, where they survive the gadget's restart, are written within a write budget and are deleted with their app; on one without
 * it, in a map in PSRAM, where they survive an app's restart, not the gadget's. apphost keeps them in the process.
 *
 * The RAM map holds at most 16 apps (APPS_VALUES_RAM_APPS), and a save for a 17th drops the values of the app saved longest ago, with one
 * log line that names it: a bound against growth that has no limit, which only the RAM map has. Either backend deletes an app's values
 * when the app is removed, and drops no other app's.
 */

/* How long a load waits for the worker at most. The app task's call is watched for APPS_TASK_HUNG_MS (3 s), and the load is made inside
 * the launch's: so that the wait isn't the app's, the port begins the call again once it has waited (apps_task_call_restart), and the
 * supervisor counts what main.lua then does from there. */
#define APPS_STORE_WAIT_MS 1000

/* The object saved for `app`, as a block of apps_port_realloc with a NUL after its *len bytes, which the caller frees with
 * apps_port_free; "{}" when nothing is saved. NULL, with *len 0, when it can't be had: no memory, or the worker didn't answer within
 * APPS_STORE_WAIT_MS (a launch is then refused, and never starts with an empty store, which a later save would put in place of the
 * real one). Whatever was saved before this call is read, in the order the calls were made (a restart's load comes after the
 * stop's save). The app task, outside any call of the app. */
char *apps_port_store_load(const char *app, size_t *len);

/* Saves json[0, len), one JSON object, as `app`'s store, in place of what was there. The bytes are copied before this returns and
 * written on the worker, in the order of the calls. True when the port has the object, whatever the worker says to it: queued for the worker,
 * or, if the worker refuses it just now (its queue is full, or it can't be made), kept by the port until it takes it (apps_store.h), where a
 * load of the app reads it. False when the port can't keep it (no memory for the copy, or the object is more than APPS_STORE_MAX_BYTES, which
 * the runtime never makes): the caller tries again later (a periodic save) or says that the last changes are lost (the last one, when the
 * app ends). The app task. */
bool apps_port_store_save(const char *app, const char *json, size_t len);

/* The battery, as muse_state_power() has it: pct in 0 to 100, or -1 when there is none or it can't be told. */
typedef struct {
    int pct;
    bool charging, usb;
} apps_battery_t;
apps_battery_t apps_port_battery(void);

/* Whether the gadget is on a Wi-Fi network, and then the network's name in ssid (cap bytes with the NUL, cut if it is longer);
 * "" when it isn't. The name is bytes from the air: the caller cleans it as text. */
bool apps_port_wifi(char *ssid, size_t cap);

/* The app asks to be in front (sys.show: message() and alarm() may, and any callback within 10 s of a tap). The device goes through
 * the same door as every front (a request the LVGL task carries out, held back during a voice turn); apphost prints it. */
void apps_port_show(void);

/* sys.keep_awake(on): while on, the screen isn't let go dark for being idle as long as the app is in front. The app task's
 * housekeeping pokes the idle timer about once a second for it. Off by itself when the app ends. */
void apps_port_keep_awake(bool on);

/* sys.exit(): the app ends once the callback that called it has returned (stop() runs, its store is saved, the layer goes behind).
 * The callback itself runs to its end. */
void apps_port_exit(void);
