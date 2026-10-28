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
 * set (SNTP). Any task. */
bool apps_port_epoch_ms(int64_t *out);

/* The apps' heap: PSRAM on the device. realloc(3) for n > 0: ptr NULL allocates, otherwise the block is
 * resized and may move; on failure it returns NULL and leaves ptr as it was. It is never called with
 * n == 0. A block comes back with apps_port_free, never with free(). Any task. */
void *apps_port_realloc(void *ptr, size_t n);
void apps_port_free(void *ptr);  /* NULL is ignored. Any task. */

/* One line of an app's log (what print wrote), for the development loop: the device prints
 * "@app.log <id> <line>" on the console, at most 1,024 bytes of them a second (apps_echo.h); a line past
 * that isn't printed, and the next printed one comes after "@app.log <id> (<N> lines not echoed)". line
 * is one line of valid UTF-8, at most 200 bytes, with no newline and no control byte but tab,
 * NUL-terminated. It returns promptly: the app task is waiting on it, with its CPU budget running. */
void apps_port_log(const char *app_id, const char *line);

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
