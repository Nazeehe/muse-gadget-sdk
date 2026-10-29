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
 * The services of the device a Muse app can call: the Lua modules `time`, `os` and `timer` (later tasks add `alarm`, `sound`,
 * `store`, `sys` and `log` here). Portable C: the clocks and the screen's sleep come through apps_port.h, and what a timer
 * is lives in the runtime (apps_rt.h, which also says when the timers run). Every binding calls apps_vm_check first, and
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
 */

#include "apps_rt.h"

typedef struct lua_State lua_State;

/* Installs `time`, `os` and `timer` into L's globals, for the app of rt, which must outlive L. It runs under lua_pcall, so a
 * heap that refuses ends the app out of memory at its next call rather than aborting. Called by apps_rt_new, before
 * main.lua's top level runs. */
void apps_svc_open(lua_State *L, apps_rt_t *rt);
