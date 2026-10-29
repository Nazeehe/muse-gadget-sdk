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
 * The system's alarms of Muse apps (apps_alarm.c: the gadget's, and apphost's too, which runs the same file over a synchronous
 * fake of esp_timer and of the lock, on its fake clock: tools/muse/apphost/apphost_esp.c). An alarm belongs to the system, not to
 * the app that armed it: it is an esp_timer one-shot on the monotonic clock (the wall clock jumping when SNTP sets it moves
 * none), it wakes the chip from light sleep, and it outlives the app's stop, crash and replacement. Every app has at most
 * APPS_ALARMS_PER_APP, all the apps at most APPS_ALARMS_MAX, each named by its app's id and a name of the app's own. Arming a
 * name that is armed replaces it. Callable from any task; the table is guarded by a mutex held for copies only (in internal RAM
 * with the pending word: a lock or a flag in PSRAM would not be atomic on the S3; the table itself, touched under the lock alone,
 * is in PSRAM).
 *
 * When one fires, in the esp_timer task, which serves every timer of the gadget and so does not wait: the table is taken with a
 * try of at most 20 ms (LOCK_TRY_MS), and a try that fails comes again 10 ms later; the post is made with no wait; and it never
 * logs or prints, since a console line can wait for another task's whole reply. It records what it did and the app task
 * prints that in its next round (apps_alarm_log_drain, called from the loop's housekeeping, so within a second). In this order:
 *   1. the app's budget: more than APPS_ALARM_BUDGET firings of one app within APPS_ALARM_BUDGET_MS cancel every alarm of that
 *      app, this one too, with "alarm: <app> fired more than 6 alarms in a minute; its alarms are cancelled", and the ring (below)
 *      ends if that leaves nothing to stop it. Nothing of the rest happens. An app that arms an alarm each time it is told one,
 *      or from a timer, can neither keep the screen awake and ringing nor take the gadget over: it gets six, and its stop sticks;
 *   2. the alarm is marked fired and its bit in the pending word set: it keeps its slot until its app has been told;
 *   3. with sound, the ring starts: muse_voice_request_chirp() at once and every APPS_RING_PERIOD_MS, from a periodic esp_timer.
 *      A ring that is going goes on and is made a minute longer, and that is all a firing does to it: no chirp is asked for and
 *      none is logged (the ring's own period asks for one every 1.5 s, which keeps a flood of firings to one chirp per period).
 *      muse_voice_request_chirp sets a flag and a bit of an event group and returns, so it is safe here; the voice task plays the
 *      chirp when it is idle, and one request is all it keeps;
 *   4. with wake, muse_input_wake("alarm"): the screen wakes as it would for a press;
 *   5. the fired callback (apps_alarm_set_fired_cb) is called, outside the table's lock: the app task's posts an event with no
 *      payload and no wait, and a full queue loses nothing, as the bit stays.
 *
 * The ring belongs to the system and not to an alarm: an alarm that is armed again or cancelled after it has rung and been delivered
 * leaves it going. It is never silenced while a firing with sound still waits to be told, or while the app that was last told one
 * runs and can call sound.stop(). It ends:
 *   - at sound.stop (apps_alarm_ring_stop: any app, no permission);
 *   - APPS_RING_MAX_MS after the last alarm with sound fired ("timeout");
 *   - once nothing is left that could stop it: no firing with sound waits to be told (it was cancelled, or given up on:
 *     apps_alarm_cancel, apps_alarm_cancel_app, apps_alarm_take), and the app last told one has ended (apps_alarm_app_ended: the
 *     app task says so for every ending, with how it ended: crashed, stopped, replaced, exited, hung). Each of these decides and
 *     stops under one hold of the table's lock, so no firing can slip in between the two.
 *
 * Logs, all with the tag "muse_apps" and made by apps_alarm_log_drain (or by the call that changed something, which drains after it
 * has let go of the table): "alarm <app>/<name> fired (sound <0|1>, wake <0|1>)", "ring <app>/<name> chirp <n>" (naming the alarm
 * that rang last, for each chirp the ring asks for), "ring stopped (<why>)", "alarm: <app> <why>; its alarms are cancelled" (W) and
 * "(<n> alarm lines not logged)". The firing and cancellation lines go through a token bucket of APPS_ALARM_LOG_BURST, refilled
 * four a second, so no app can flood the console with them; a line it refuses is counted, and the count is printed before the next
 * line that gets in, or once the lines have been quiet for a second. The ring's lines are a few a minute at most and always get in.
 *
 * The app task delivers (apps_task.c): it looks at apps_alarm_pending() on every round, which is a load of one word, and takes the
 * fired alarms it can deliver. A firing stays in its slot until it is taken, so it counts toward the limits and shows in
 * apps_alarm_list with 0 left, until it is taken: delivered, or given up on when its app can't be started. It is a half of the slot
 * of its own, beside the arming: an app started by its alarm that arms the same name in start() (an alarm clock that sets its next
 * alarm there) has a new alarm armed, and is still told of the one that fired. A launch of an app for its alarms has an identity,
 * the gen of the last firing it was asked for (apps_alarm_wanted), so that it can be told that its alarms are gone.
 */

#include <stdbool.h>
#include <stdint.h>

#include "apps_pkg.h"
#include "apps_port.h"

#define APPS_ALARMS_MAX 8            /* all apps; 4 per app */
#define APPS_ALARMS_PER_APP 4
#define APPS_ALARM_NAME_MAX 23       /* characters of a-z, 0-9 and _; the buffers hold 24 with the NUL */
#define APPS_ALARM_MAX_MS 604800000  /* a week */
#define APPS_RING_PERIOD_MS 1500
#define APPS_RING_MAX_MS 60000
#define APPS_ALARM_BUDGET 6          /* the firings of one app that fit in APPS_ALARM_BUDGET_MS: the next one is the 7th */
#define APPS_ALARM_BUDGET_MS 60000
#define APPS_ALARM_LOG_BURST 8       /* the lines of the token bucket: all the table's alarms due together */

/* Whether name is 1 to APPS_ALARM_NAME_MAX characters of a-z, 0-9 and _: the one rule, which apps_alarm.c and the alarm module of
 * Lua (apps_svc.c) both use. */
static inline bool apps_alarm_name_valid(const char *name) {
    int n = 0;
    for (; name != NULL && name[n] != '\0'; n++) {
        bool ok = (name[n] >= 'a' && name[n] <= 'z') || (name[n] >= '0' && name[n] <= '9') || name[n] == '_';
        if (!ok || n >= APPS_ALARM_NAME_MAX) {
            return false;
        }
    }
    return n >= 1;
}

/*
 * Arms the alarm `name` of app to fire ms from now (1 to APPS_ALARM_MAX_MS), replacing the one of that name that is armed. One
 * that has fired and not been told to its app is told still: it is a firing, which waits in the slot beside this arming.
 * APPS_ALARM_REFUSED, with nothing changed, when the app has APPS_ALARMS_PER_APP of other names, the table has no free slot, or an
 * argument isn't valid (an id as apps_id_valid says, a name as apps_alarm_name_valid says, a time in range).
 * APPS_ALARM_NO_MEMORY when there was none for the lock or the ring's timer (made once: the next call tries again) or for the
 * alarm's own timer (made the first time its slot is used): the table is as it was. The one thing a refusal can change is an arming
 * that this replaced, whose timer couldn't be started again: it is gone.
 */
apps_alarm_result_t apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake);

/* Disarms the alarm, and drops its firing if one waits to be told: none of its delivery is made, and a ring it started ends if
 * that leaves nothing that could stop it (see above). Nothing for a name that isn't armed. */
void apps_alarm_cancel(const char *app, const char *name);

/* Cancels every alarm of app, armings and firings that wait, with "alarm: <app> <why>; its alarms are cancelled" in the log (why is a
 * string for the log, read before this returns). The app task does it when an app that its alarm launched crashes, and the esp_timer
 * task when the app is over its budget: the same cancel, by the id. A ring that this leaves with nothing to stop it ends. */
void apps_alarm_cancel_app(const char *app, const char *why);

/* Milliseconds until the alarm fires, rounded up (0 when it is due, or has fired and its app hasn't been told and it hasn't been
 * armed again), or -1 if there is no such alarm. */
int64_t apps_alarm_remaining(const char *app, const char *name);

/* The names of app's alarms, earliest first (of two due together, the one armed first), into names, with the time left of each (as
 * apps_alarm_remaining) into remaining; returns how many (at most APPS_ALARMS_PER_APP). */
int apps_alarm_list(const char *app, char names[4][24], int64_t remaining[4]);

/* Ends a ring that is going, with "ring stopped (sound.stop)" in the log. Nothing when none is. */
void apps_alarm_ring_stop(void);

/* Whether a ring is going now. */
bool apps_alarm_ringing(void);

/* The app has ended, however (crashed, stopped, replaced, exited, hung): why is "app crashed", "app stopped", ... for the log
 * (read before this returns). If it is the app that was last told an alarm with sound, nothing is left that it could stop the ring
 * with, and the ring ends unless another firing with sound still waits to be told. The app task calls it from the one place every
 * ending passes. */
void apps_alarm_app_ended(const char *app, const char *why);

/* Where a fired alarm is told: cb(app, name) runs in the esp_timer task after the alarm is marked fired, the ring started and the
 * screen woken, and must not block: apps_task.c posts APPS_EV_ALARM with no wait. NULL for none. */
void apps_alarm_set_fired_cb(void (*cb)(const char *app, const char *name));

/* Prints what the esp_timer task recorded and the other calls here decided (see the logs, above), oldest first. The app task calls it
 * outside any call into an app, in each round and after each call; it takes the table's lock for a copy of one line at a time and
 * prints with the lock free. Any task may. */
void apps_alarm_log_drain(void);

/* ---- What the app task needs to deliver ---- */

/* A firing no one has taken: its app and name from the table, and what names it for apps_alarm_take. */
typedef struct {
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    bool sound, wake;  /* what the firing asked for: an arming that fired while it waited adds its own */
    int64_t fired_ms;  /* when it fired, on apps_port_mono_ms's clock */
    uint8_t slot;
    uint32_t gen;      /* never 0; later firings have larger gens (compared as a difference: it wraps) */
} apps_alarm_fired_t;

/* Whether any alarm has fired and not been taken: one atomic load, for the loop to ask on every round. */
bool apps_alarm_pending(void);

/* Copies the firings that haven't been taken, the earliest to fire first, at most max; returns how many. It changes nothing: a
 * firing stays until apps_alarm_take. */
int apps_alarm_fired(apps_alarm_fired_t *out, int max);

/* Takes f's firing: true when this call did, false when it was taken or cancelled since f was copied (the same firing is never
 * delivered twice). Its slot is free again unless the name is armed. delivered true: its app was told, and if the firing had sound
 * that app is the one that can stop the ring. False: it is given up on, and a ring it started ends if nothing else could stop it
 * (another firing with sound still waiting, or the app that was told one still running). */
bool apps_alarm_take(const apps_alarm_fired_t *f, bool delivered);

/* Whether a firing of app with a gen up to upto (as a difference: the gens wrap) still waits: the launch of the app that was asked
 * for those firings is still wanted. The I/O worker asks before it loads and runs the app, and the app task before it replaces the
 * app that runs, so a launch whose alarms were given up on, or told in the meantime, runs nothing. */
bool apps_alarm_wanted(const char *app, uint32_t upto);

/* ---- What the alarms' records need (apps_values.h, apps_store.h) ---- */

/* One of an app's alarms: its name, the time left (as apps_alarm_remaining: 0 for a firing that only waits to be told), and what it
 * asks for (the arming's, or the waiting firing's when it isn't armed again). */
typedef struct {
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t remaining_ms;
    bool sound, wake;
} apps_alarm_info_t;

/* app's alarms, earliest first, as apps_alarm_list orders them; returns how many (at most APPS_ALARMS_PER_APP). */
int apps_alarm_list_info(const char *app, apps_alarm_info_t out[APPS_ALARMS_PER_APP]);

/* The apps whose alarms apps_alarm_cancel_app or the budget cancelled since the last call, each once, at most max of them (the
 * first APPS_ALARMS_MAX noted are kept): the app task's round takes them, so that their records follow. */
int apps_alarm_take_cancelled(char ids[][APPS_ID_MAX + 1], int max);
