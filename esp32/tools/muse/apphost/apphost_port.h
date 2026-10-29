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
 * The system's alarms and sound, on the fake clock (what apps_alarm.c is on the gadget: the table of apps_alarm.h, the same
 * limits, and the ring, which lasts APPS_RING_MAX_MS from the last alarm with sound that fired). The port keeps them, and
 * apphost.c is the system: after each step of the clock it asks which are due, prints them, rings, wakes the screen and tells
 * the app. A chirp (sound.chirp) and the ring's start and end are told to apphost.c as they happen, inside whatever call
 * made them, so that they print in order with what the app printed. The ring's period has no counterpart: it is on or off.
 */
typedef struct {
    void (*chirp)(void);   /* sound.chirp() */
    void (*ring)(bool on);  /* the ring began (a first alarm with sound fired), or ended (sound.stop, its minute, a drop) */
} apphost_sound_t;

/* Where the sound goes: NULL (the start, and --cycles) is nowhere. sink must outlive the run. */
void apphost_set_sound(const apphost_sound_t *sink);

typedef struct {
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    bool sound, wake;
} apphost_alarm_t;

/* The alarm that is due at the clock's now (the earliest, and of two together the one armed first), taken out of the table:
 * true with it in *out; false when none is due. Nothing happens with it here: the ring and the screen are apphost.c's to
 * start (apphost_ring), since an alarm's event comes before its ring's. */
bool apphost_alarm_next_due(apphost_alarm_t *out);

/* An alarm with sound has fired: the ring begins, and tells the sink so, or goes on and lasts a minute more. */
void apphost_ring(void);

/* The ring's end, if its minute is over: it tells the sink. apphost.c calls it after each step of the clock. */
void apphost_ring_tick(void);
