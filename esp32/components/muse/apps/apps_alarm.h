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
 * The system's alarms of Muse apps, on the gadget (apps_alarm.c; apphost has its own on its fake clock: apphost_port.c).
 * An alarm belongs to the system, not to the app that armed it: it is an esp_timer one-shot on the monotonic clock (the
 * wall clock jumping when SNTP sets it moves none), it wakes the chip from light sleep, and it outlives the app's stop, crash
 * and replacement. Every app has at most APPS_ALARMS_PER_APP, all the apps at most APPS_ALARMS_MAX, each named by its app's
 * id and a name of the app's own. Arming a name that is armed replaces it. Callable from any task; the table is guarded
 * by a mutex held for copies only, in internal RAM like the pending bits.
 *
 * When one fires, in the esp_timer task, which never blocks (the table is taken with a short try, and a try that fails
 * comes again 10 ms later) and posts with no wait:
 *   1. the alarm is marked fired and its bit in the pending word set: it keeps its slot until its app has been told;
 *   2. with sound, the ring starts: muse_voice_request_chirp() at once and every APPS_RING_PERIOD_MS, from a periodic
 *      esp_timer, until apps_alarm_ring_stop() or APPS_RING_MAX_MS after the last alarm with sound fired. A ring that is
 *      going goes on and is made a minute longer. It logs "ring <app>/<name> chirp <n>" on each chirp and "ring stopped
 *      (<why>)" at its end. muse_voice_request_chirp sets a flag and a bit of an event group and returns, so it is safe
 *      here; the voice task plays the chirp when it is idle, and one request is all it keeps;
 *   3. with wake, muse_input_wake("alarm"): the screen wakes as it would for a press;
 *   4. the fired callback (apps_alarm_set_fired_cb) is called, outside the table's lock: the app task's posts an event
 *      with no payload and no wait, and a full queue loses nothing, as the bit stays.
 * The ring belongs to the system and not to an alarm: an alarm that is armed again or cancelled after it has rung and been
 * delivered leaves it going, and sound.stop (any app, no permission) or the minute ends it. What ends it too: an alarm that
 * fired, with sound, and is dropped before its app was told (cancelled, or given up on: apps_alarm_take), since nothing is
 * then going to tell the user how to stop it.
 *
 * The app task delivers (apps_task.c): it looks at apps_alarm_pending() on every round, which is a load of one word, and
 * takes the fired alarms it can deliver. A firing stays in its slot until it is taken, so it counts toward the limits and
 * shows in apps_alarm_list with 0 left, until it is taken: delivered, or given up on when its app can't be started. It is
 * a half of the slot of its own, beside the arming: an app started by its alarm that arms the same name in start() (an alarm
 * clock that sets its next alarm there) has a new alarm armed, and is still told of the one that fired.
 */

#include <stdbool.h>
#include <stdint.h>

#include "apps_pkg.h"

#define APPS_ALARMS_MAX 8            /* all apps; 4 per app */
#define APPS_ALARMS_PER_APP 4
#define APPS_ALARM_NAME_MAX 23       /* characters of a-z, 0-9 and _; the buffers hold 24 with the NUL */
#define APPS_ALARM_MAX_MS 604800000  /* a week */
#define APPS_RING_PERIOD_MS 1500
#define APPS_RING_MAX_MS 60000

/*
 * Arms the alarm `name` of app to fire ms from now (1 to APPS_ALARM_MAX_MS), replacing the one of that name that is armed. One
 * that has fired and not been told to its app is told still: it is a firing, which waits in the slot beside this arming. False,
 * with nothing changed, when the app has APPS_ALARMS_PER_APP of other names, the table has no free slot, there is no memory for
 * its timer, or an argument isn't valid (an id, a name of APPS_ALARM_NAME_MAX characters of a-z, 0-9 and _).
 */
bool apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake);

/* Disarms the alarm, and drops its firing if one waits to be told: none of its delivery is made, and a ring it started ends.
 * Nothing for a name that isn't armed. */
void apps_alarm_cancel(const char *app, const char *name);

/* Milliseconds until the alarm fires, rounded up (0 when it is due, or has fired and its app hasn't been told and it hasn't been
 * armed again), or -1 if there is no such alarm. */
int64_t apps_alarm_remaining(const char *app, const char *name);

/* The names of app's alarms, earliest first, into names, with the time left of each (as apps_alarm_remaining) into
 * remaining; returns how many (at most APPS_ALARMS_PER_APP). */
int apps_alarm_list(const char *app, char names[4][24], int64_t remaining[4]);

/* Ends a ring that is going, with "ring stopped (sound.stop)" in the log. Nothing when none is. */
void apps_alarm_ring_stop(void);

/* The same with another reason in the log, "ring stopped (<why>)": the app task ends it so when the app crashed in alarm(),
 * with no one left to stop it. why is a string for the log; it is read before this returns. */
void apps_alarm_ring_end(const char *why);

/* Where a fired alarm is told: cb(app, name) runs in the esp_timer task after the alarm is marked fired, the ring started
 * and the screen woken, and must not block: apps_task.c posts APPS_EV_ALARM with no wait. NULL for none. */
void apps_alarm_set_fired_cb(void (*cb)(const char *app, const char *name));

/* ---- What the app task needs to deliver ---- */

/* A firing no one has taken: its app and name from the table, and what names it for apps_alarm_take. */
typedef struct {
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    bool sound;
    int64_t fired_ms;  /* when it fired, on apps_port_mono_ms's clock */
    uint8_t slot;
    uint32_t gen;
} apps_alarm_fired_t;

/* Whether any alarm has fired and not been taken: one atomic load, for the loop to ask on every round. */
bool apps_alarm_pending(void);

/* Copies the firings that haven't been taken, the earliest to fire first, at most max; returns how many. It changes nothing: a
 * firing stays until apps_alarm_take. */
int apps_alarm_fired(apps_alarm_fired_t *out, int max);

/* Takes f's firing: true when this call did, false when it was taken or cancelled since f was copied (the same firing is never
 * delivered twice). Its slot is free again unless the name is armed. delivered false means it is given up on, and a ring it
 * started (sound) ends: with its app unable to run, nothing would tell the user how to stop it. */
bool apps_alarm_take(const apps_alarm_fired_t *f, bool delivered);
