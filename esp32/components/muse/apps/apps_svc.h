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
 * The services of the device a Muse app can call: the Lua modules `time`, `os`, `timer`, `alarm`, `sound`, `store` and `sys`
 * (logging is `print` and `sys.log`: there is no `log` module). Portable C: the clocks, the screen's sleep, the battery, the front
 * and the rest come through apps_port.h, and what a timer is, and the store's cache, live in the runtime (apps_rt.h, which also
 * says when the timers run and when the store is saved). Every binding calls apps_vm_check first, and
 * everything that allocates in the app's heap runs inside a call of the app, so a heap that refuses is the app running
 * out of memory and never an abort.
 *
 * time (the clocks, all in milliseconds):
 *   time.now()            the wall clock, epoch ms as an integer; nil until the clock is set (SNTP)
 *   time.mono()           ms since boot, on the clock that never jumps; the timers' too
 *   time.synced()         whether the wall clock is set
 *   time.date(ms?)        {year, month, day, hour, min, sec, wday, yday} of ms (default: now) in the local time zone; wday 1
 *                         is Sunday, yday 1 is January 1st. ms is a whole number (an integer, or a float with no fraction;
 *                         a string isn't taken for a number), from the year 0 to the end of 9999 (UTC): another is an error
 *   time.format(fmt, ms?) fmt with %Y %m %d %H %M %S %I %p %a %A %b %B %j %y %e and %% (English names, "%e" a day padded with a
 *                         space), for ms (default: now), in local time. Any other specifier, and a lone % at the end, is an
 *                         error that says so. The result is at most 128 bytes: a longer one is an error, never cut.
 *   Without ms, time.date and time.format need the clock set: "time.date: the clock isn't set yet". With ms they don't.
 *   The calendar is the years 0 to 9999: a time outside it (time.date, time.format, os.date, and what os.time makes of a
 *   table) is an error, the same on the gadget and in apphost, where the C libraries would otherwise answer differently.
 *
 * os (Lua's names, and no more of them):
 *   os.time(t?)           without t: epoch seconds, or seconds since boot until the clock is set. With a table
 *                         {year, month, day, hour = 12, min = 0, sec = 0, isdst}: mktime in the local time zone, which also
 *                         writes the normalized fields back into the table, as Lua's does (so a month of 13 is January).
 *                         day, hour, min and sec may add up to at most 3000000 days (the C library works through the days
 *                         a month at a time, and 2^31 of them would hold the app for seconds), and the walk is charged to the
 *                         call like Lua's own instructions, 100 days to an instruction and one more (so a callback that makes
 *                         them by the hundred is over its budget, and counts in the duty cycle: apps_rt.h). A year and month
 *                         that no days within that bound could bring into the calendar are refused before the C library sees them
 *   os.date(fmt?, s?)     Lua's: fmt (default "%a %b %e %H:%M:%S %Y", what C's %c is) with time.format's specifiers, for the
 *                         epoch seconds s (default os.time(): before the clock is set it is 1970, and never an error); a
 *                         "!" in front of fmt is UTC; "*t" (or "!*t") is the table of time.date's fields and isdst
 *   os.clock()            seconds since boot, a float
 *   os.difftime(a, b)     a - b, a float
 *
 * timer (the runtime runs them: apps_rt.h):
 *   timer.after(ms, fn)   fn() runs once, ms from now
 *   timer.every(ms, fn)   fn() runs every ms; a run that is late (the screen was dark) is made once, not for each one missed
 *   Each returns a handle: h:cancel(), and timer.cancel(h) does the same; either may be called again, and from the timer's
 *   own callback. ms is a whole number from 40 to 86400000. No more than 16 timers at once. The handle is protected (X17).
 *   tostring(h) is "timer#<slot>".
 *
 * alarm (the system's: they belong to the gadget, not to the app, so they run on the monotonic clock, fire while the gadget
 * sleeps, and outlive the app's stop and its crash; apps_alarm.h says how, apphost mimics it on its fake clock). The "alarm"
 * permission of the manifest is needed for all of them: without it each raises
 * `alarm: add "alarm" to "permissions" in manifest.json`, before anything else is looked at.
 *   alarm.after(name, ms, {sound = true, wake = true})
 *                         fires ms from now (a whole number from 1 to 604800000, a week). name is 1 to 23 characters of
 *                         a-z, 0-9 and _ and names the alarm among this app's: arming a name that is armed replaces it.
 *                         sound rings (a chirp now and every 1.5 s, for a minute at most, until sound.stop()), wake lights
 *                         the screen as a press does; both default to true, a key that is neither is an error, and no
 *                         "sound" permission is needed for the ring. When it fires the app is brought to the front, started
 *                         first if it isn't running (from the last package installed with its id), and its handler
 *                         alarm(name) is called, once start() has returned. At most 4 alarms an app and 8 on the gadget:
 *                         `alarm: no more than 4 alarms per app`, `alarm: the gadget has no free alarm slots (8 in all); cancel
 *                         one`. The one-shot is gone from alarm.list() once alarm(name) is called (and inside it), so the
 *                         handler may arm the same name again; so may start(), when it is the alarm that started the app: the
 *                         alarm that fired is still called, and the new one is armed beside it. It returns nothing: there is
 *                         no handle.
 *   alarm.cancel(name)    disarms it; nothing for one that isn't armed. Cancelled after it fired and before alarm(name) was
 *                         called, it is never delivered, and the ring it started ends.
 *   alarm.remaining(name) the milliseconds left, an integer rounded up (0 for one that has fired and isn't delivered yet), or
 *                         nil when there is no such alarm
 *   alarm.list()          {{name = "t", remaining = 4200}, ...}, the earliest first
 *   A wrong name is `alarm: name must be 1-23 characters of a-z, 0-9 and _` (in all four), a wrong ms
 *   `alarm.after: ms must be a whole number from 1 to 604800000`.
 *
 * sound:
 *   sound.chirp()         one chirp at the gadget's volume (it plays when the voice task is idle); needs the "sound"
 *                         permission: `sound: add "sound" to "permissions" in manifest.json`
 *   sound.stop()          ends the ring of an alarm that is going, whichever app's it is; no permission, and nothing happens
 *                         when none is going
 *
 * store (the app's own keys and values, kept across restarts; the "store" permission of the manifest is needed for all of them:
 * without it each raises `store: add "store" to "permissions" in manifest.json`, before anything else is looked at). A key is a
 * string of 1 to 32 characters (valid UTF-8, no NUL; another is `store: key must be a string of 1 to 32 characters`), a value
 * anything json.encode takes, as deep as it takes it (16 levels). The saved object, counted after encoding as it is saved, may be 16 KB:
 * more is `store: more than 16 KB stored; remove keys you don't need`, and nothing is changed.
 *   store.get(key, default)   the value, a copy (decoded afresh from what is kept: change it and the store doesn't change); the
 *                         default, as it is (nil if none is given), when there is no such key. A json.null stored is json.null
 *   store.set(key, value) keeps value (a copy: what the app does to its table afterwards isn't seen). Raises
 *                         `store.set: ` and json.encode's message ("store.set: can't encode a function", "store.set: nested
 *                         deeper than 16"). A value that can never fit in the 16 KB (a string or a table of more than that) is
 *                         refused with the 16 KB message before anything is made of it. nil removes the key, as store.remove
 *                         does; a write of what is there is no change. Returns nothing
 *   store.remove(key)     removes it; nothing for a key that isn't there
 *   The values are cached and saved 2 s after the last change, and at the latest 10 s after the first change that isn't saved yet
 *   (an app that writes every second is saved all the same), and when the app ends (apps_rt.h says how, and what a refused save
 *   does), loaded before main.lua runs, whose top level may use them. The flush isn't a timer: it isn't one of the 16.
 *
 * sys (the device, and the app's own life):
 *   sys.battery()         {pct = 0 to 100, charging = bool, usb = bool}; pct is always a number, -1 when the gadget can't tell
 *   sys.wifi()            {connected = bool, ssid = "..."}: the network's name, "" when it isn't connected
 *   sys.show()            asks for the front. Allowed in message() and alarm(), and in any callback within 10 s of a tap (the 10th
 *                         second included); otherwise `sys.show: allowed only in message() or alarm(), or within 10 s of a tap`
 *   sys.keep_awake(on)    true keeps the screen lit while the app is in front, false lets it sleep again. Needs the "keep_awake"
 *                         permission: `sys: add "keep_awake" to "permissions" in manifest.json`; on must be true or false
 *   sys.has(name)         whether the gadget has that module: true for ui time timer alarm store sound json sys vm, false for any
 *                         other name (net, secrets and muse aren't there yet; nor is log). name must be a string
 *   sys.api               1, the api of manifest.json it speaks
 *   sys.mem()             {used_kb, peak_kb, limit_kb} of the Lua heap (rounded up, as the status has them), limit_kb the manifest's
 *   sys.exit()            the app ends once this callback has returned: it runs to its end, stop() runs, the store is saved
 *   sys.log(...)          print: the same lines in the same log
 */

#include "apps_rt.h"

typedef struct lua_State lua_State;

/* Installs `time`, `os`, `timer`, `alarm`, `sound`, `store` and `sys` into L's globals, and the `vm` module beside them (apps_vmio.h:
 * apps_vmio_open), for the app of rt, which must outlive L. It runs under lua_pcall, so a
 * heap that refuses ends the app out of memory at its next call rather than aborting. Called by apps_rt_new, before
 * main.lua's top level runs. */
void apps_svc_open(lua_State *L, apps_rt_t *rt);
