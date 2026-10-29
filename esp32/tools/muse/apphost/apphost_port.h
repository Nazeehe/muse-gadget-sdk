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
 * What apphost's port (apphost_port.c, which implements apps_port.h on the host) gives apphost.c besides
 * the port itself: where the app's log lines go, what the apps' heap holds, whether the UI lock is held, the clocks and
 * the screen a script moves, and the system's alarms and the ring.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_alarm.h"

/* Where apps_port_log sends a line: apphost.c's event writer, or nothing (NULL, the start) for --cycles. */
typedef void (*apphost_log_fn)(const char *app_id, const char *line);
void apphost_set_log(apphost_log_fn fn);

/* Bytes the apps' heap holds now: what apps_port_realloc handed out and apps_port_free hasn't taken back.
 * 0 when everything an app (and its runtime) allocated has been freed. */
size_t apphost_heap_held(void);

/* Whether the UI lock is held. The port aborts when the lock is taken twice, given back when it isn't held, or
 * when the apps' heap is used (a block taken, resized or freed) while it is held (the widget table's rule:
 * validate and allocate first, then lock, and let go of what was replaced after): a violation shows in every
 * run, and apphost checks the lock is free after each command. */
bool apphost_ui_locked(void);

/* How many times the UI lock has been taken. apphost compares it across a command: a command that left the screen
 * changed without taking the lock even once is a binding that writes the table where the LVGL task reads it. */
unsigned long apphost_ui_lock_takes(void);

/* The clocks and the screen, which a script moves (apphost_port.c): the monotonic clock only by apphost_advance, which
 * moves the wall clock with it once that is set; the wall clock is unset until apphost_sync gives it the epoch ms of now;
 * the screen is lit until apphost_set_dark. Time stands still inside a call, as nothing here advances it there. */
void apphost_advance(int64_t ms);
void apphost_sync(int64_t epoch_ms);
void apphost_set_dark(bool dark);

/*
 * The system's alarms and sound. The alarms are apps_alarm.c itself, the gadget's table, ring and budget, run over a fake of esp_timer, of
 * its lock and of its log (apphost_esp.h) on this clock: apphost.c is only what the app task is around it. After each step of the clock
 * it runs the timers that came due (apphost_esp_run_timers), announces the alarms that fired (apps_alarm_fired), launches the app again
 * if it doesn't run and tells it (apps_alarm_take, then alarm(name)), as apps_task.c's deliver_alarms does, and tells the alarms how an
 * app ended (apps_alarm_app_ended, apps_alarm_cancel_app), as its end_app does. What the port does of its own is the sound module
 * (a chirp, sound.stop) and the screen an alarm with wake lights. A chirp and the ring's start and end are told to apphost.c as they
 * happen, inside whatever call made them, so that they print in order with what the app printed. The ring's period has no counterpart:
 * it is on or off.
 */
typedef struct {
    void (*chirp)(void);    /* sound.chirp() */
    void (*ring)(bool on);  /* the ring began (a first alarm with sound fired), or ended (sound.stop, its minute, nothing left to stop it) */
} apphost_sound_t;

/* Where the sound goes: NULL (the start, and --cycles) is nowhere. sink must outlive the run. */
void apphost_set_sound(const apphost_sound_t *sink);

/* Tells the sink if the ring is on and was off, or off and was on, as apps_alarm_ringing() says: the real ring state, which sound.stop, the
 * ring's minute, a firing, a cancel and the end of an app each may change. The port calls it after the calls of the app that can, and
 * apphost.c after each step of the clock and each end of the app. */
void apphost_ring_sync(void);
