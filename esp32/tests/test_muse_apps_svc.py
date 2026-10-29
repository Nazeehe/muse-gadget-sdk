# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The `time`, `os`, `timer`, `alarm`, `sound`, `store` and `sys` modules of Muse apps (apps_svc.c), the timers, the CPU duty cycle
and the store's cache of the runtime (apps_rt.c), and the commands of apphost that move time (advance, dark, sync, timers) or restart
the app.

Most tests run apphost on an app folder, a fixture (tests/muse_apps_fixtures/timers, time_label) or one made on the
spot, with a script on stdin, and read the one-JSON-object-a-line events it prints. Time is fake there and moves only
when the script says so (`advance`), in steps of 10 ms with a pass of the timers after each, as the app task's loop
does; `sync` sets the wall clock, which then moves with it; the time zone is UTC0. The C cases of
tests/muse_apps_rt_harness.c check what a script can't: the conversions in a zone that isn't UTC (the board's), a heap
that refuses at every step, and the duty cycle's windows, which a clock that jumps and calls that come when the case says
show and apphost's 10 ms steps can't (each window is closed as it ends there).
"""

import json
import random
import re
import subprocess
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

import apps_host
from test_muse_apps_rt import BUDGET_CALL, HARNESS_SOURCES, INCLUDES, AppHostTestCase, rt_harness, unknown_handler

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
FIXTURES = ROOT / "tests/muse_apps_fixtures"

SYNCED = 1767225600000  # 2026-01-01T00:00:00Z, a Thursday: what `sync` is given in these tests

# The messages, exactly (the brief's, the controller's ruling on the 128 bytes, and the ones the modules add).
CLOCK_NOT_SET = "time.date: the clock isn't set yet"
FORMAT_CLOCK_NOT_SET = "time.format: the clock isn't set yet"
TOO_MANY_TIMERS = "timer: no more than 16 timers per app"
THROTTLED = "throttled: using more than 40% of the CPU"
KILLED = "stopped: used more than 40% of the CPU for 5 s"
SPECIFIERS = "(use %Y %m %d %H %M %S %I %p %a %A %b %B %j %y %e)"
LONE_PERCENT = "the format ends in a lone % (write %% for a percent sign)"
TABLE_ARGUMENT = "the argument must be a table like {year = 2026, month = 1, day = 1}"
YEARS = "(the years 0 to 9999)"
DAYS_RULE = "os.time: day, hour, min and sec add up to more than 3000000 days"
NOT_REPRESENTED = f"os.time: the time can't be represented {YEARS}"
# The alarm and sound modules' (the brief's, and the controller's ruling on a full table).
ALARM_PERMISSION = 'alarm: add "alarm" to "permissions" in manifest.json'
SOUND_PERMISSION = 'sound: add "sound" to "permissions" in manifest.json'
ALARM_NAME_RULE = "alarm: name must be 1-23 characters of a-z, 0-9 and _"
ALARM_MS_RULE = "alarm.after: ms must be a whole number from 1 to 604800000"
TOO_MANY_ALARMS = "alarm: no more than 4 alarms per app"
# The store and sys modules' (the brief's, exactly).
STORE_KEY_RULE = "store: key must be a string of 1 to 32 characters"
STORE_FULL = "store: more than 16 KB stored; remove keys you don't need"
STORE_PERMISSION = 'store: add "store" to "permissions" in manifest.json'
KEEP_AWAKE_PERMISSION = 'sys: add "keep_awake" to "permissions" in manifest.json'
SHOW_RULE = "sys.show: allowed only in message() or alarm(), or within 10 s of a tap"
STORE_BYTES = 16 * 1024
# What apphost's port says of the gadget (apphost_port.c): the same in every run.
HOST_BATTERY = {"pct": 76, "charging": True, "usb": True}
HOST_WIFI = {"connected": True, "ssid": "muse-lab"}


def timer_ms_rule(name):
    return f"timer.{name}: ms must be a whole number from 40 to 86400000"


def unsupported(function, spec):
    return f"{function}: {spec} isn't supported {SPECIFIERS}"


def too_long(function):
    return f"{function}: the result is longer than 128 bytes"


def header_number(header, name):
    """What `#define NAME 1000000u` is in a header of components/muse/apps."""
    match = re.search(rf"^#define\s+{name}\s+(\d+)u?\b", (APPS / header).read_text(), re.M)
    assert match, f"no #define {name} in {header}"
    return int(match.group(1))


def chunks(run):
    """A run's events cut at its `state` events: for each one the events before it (a command's own output; the launch's
    are in the first), and last what came after the last one. The state events themselves are left out."""
    out, current = [], []
    for event in run.events:
        if event["event"] == "state":
            out.append(current)
            current = []
        else:
            current.append(event)
    out.append(current)
    return out


def logs(events):
    return [e["line"] for e in events if e["event"] == "log"]


def replies(run):
    return [e["data"] for e in run.of("reply")]


# What an app prints of a call that is refused: the message without the position Lua puts in front of it.
BARE = """
local function bare(message)
    if message:find("main.lua:", 1, true) == 1 then
        return message:sub(message:find(": ", 1, true) + 2)
    end
    return message
end
local function says(f, ...)
    local ok, message = pcall(f, ...)
    print(ok and "ok" or bare(message))
end
"""


class SvcTestCase(AppHostTestCase):
    # The runs of apphost here are made in a zone that isn't UTC (the board's): apphost sets its own, UTC0, whatever the machine
    # says, and a test that only ran on a machine in another zone than UTC (a Linux CI runner is in UTC) would not see that go.
    host_env = {"TZ": "PST8PDT,M3.2.0,M11.1.0"}

    def lua_app(self, source, script=None, **kw):
        """A one-file app whose start() runs `source` (Lua statements, with `bare` and `says` at hand), then the script."""
        main = BARE + "local app = {}\nfunction app.start()\n" + source + "\nend\nreturn app\n"
        return self.host(self.make_app("scratch", main), script=script, **kw)

    def lua_lines(self, source, script=None, **kw):
        """What start() printed, line by line, with no warning, error or crash among the events."""
        run = self.lua_app(source, script=script, **kw)
        self.assertEqual([k for k in run.kinds() if k in ("warning", "error", "crash")], [], run.events)
        return run.logs()

    def handler_app(self, handlers, script, **kw):
        """An app with the handlers of `handlers` (Lua statements that fill `app`; `bare` and `says` at hand), then the script."""
        main = BARE + "local app = {}\n" + handlers + "\nreturn app\n"
        return self.host(self.make_app("scratch", main), script=script, **kw)


class TimerTest(SvcTestCase):
    # ---- The brief's tests ----

    def test_timer_after_every_cancel(self):
        script = ["timers", "advance 99", "state", "advance 1", "timers", "advance 150", "state", "advance 250", "state",
                  "advance 250", "state", "message cancel", "timers", "advance 5000", "state"]
        run = self.host(FIXTURES / "timers", script=script)

        def line(text):
            return {"event": "log", "app": "timers", "line": text}

        running = {"event": "state", "state": "running"}
        self.assertEqual(run.events, [
            {"event": "started", "id": "timers", "name": "Timers"},
            {"event": "timers", "count": 2},
            running,                                            # 99 ms: nothing has fired
            line("after\t100"),                                 # the one-shot, at 100
            {"event": "timers", "count": 1},                    # and it is gone
            line("every\t250"), running,                        # the repeating one, at 250, 500 and 750
            line("every\t500"), running,
            line("every\t750"), running,
            {"event": "reply", "data": True},
            {"event": "timers", "count": 0},                    # cancelled
            running,                                            # and 5 s later it hasn't come again
        ])

    def test_cancel_by_the_handle_and_by_timer_cancel(self):
        run = self.host(FIXTURES / "timers", script=["advance 250", "message cancel_by_name", "advance 5000", "timers"])
        self.assertEqual(run.logs(), ["after\t100", "every\t250"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_timer_limits(self):
        wrong = ["39", "0", "-1", "86400001", "1.5", "1e100", "0/0", "math.huge", "-math.huge", '"100"', "true", "{}"]
        calls = "".join(f"says(timer.every, {ms}, f) says(timer.after, {ms}, f)\n" for ms in wrong)
        run = self.lua_app("""
            local f = function() end
            """ + calls + """
            says(timer.every) says(timer.after)
            says(timer.after, 40, f) says(timer.after, 86400000, f) says(timer.every, 100.0, f) says(timer.every, 1e3, f)
            says(timer.after, 100) says(timer.after, 100, nil) says(timer.every, 100, "x") says(timer.after, 100, {})
            local handles = {}
            for i = 1, 12 do handles[i] = timer.every(1000, f) end
            says(timer.after, 1000, f) says(timer.every, 1000, f)
            handles[1]:cancel()
            says(timer.after, 1000, f) says(timer.every, 1000, f)
        """, script=["timers"])
        both = [timer_ms_rule("every"), timer_ms_rule("after")]
        self.assertEqual(run.logs(), [
            *both * len(wrong),
            *both,
            "ok", "ok", "ok", "ok",
            "timer.after: fn must be a function, got no value",
            "timer.after: fn must be a function, got nil",
            "timer.every: fn must be a function, got string",
            "timer.after: fn must be a function, got table",
            TOO_MANY_TIMERS, TOO_MANY_TIMERS,
            "ok", TOO_MANY_TIMERS,
        ])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 16})

    def test_a_one_shot_gives_its_slot_back_before_its_callback_runs(self):
        # Sixteen timers, one of them a one-shot that arms another when it fires: it can, at the limit.
        run = self.lua_app("""
            local n = 0
            local function again() n = n + 1 print("armed", n) timer.after(50, again) end
            for i = 1, 15 do timer.every(1000, function() end) end
            timer.after(50, again)
        """, script=["advance 50", "timers", "advance 50", "timers", "advance 50", "timers"])
        self.assertEqual(run.logs(), ["armed\t1", "armed\t2", "armed\t3"])
        self.assertEqual([e["count"] for e in run.of("timers")], [16, 16, 16])

    def test_timers_pause_while_dark_and_coalesce(self):
        run = self.handler_app("""
            local count = 0
            function app.start() timer.every(250, function() count = count + 1 end) end
            function app.message() return count end
        """, script=["advance 1000", "message count", "dark on", "advance 10000", "message count", "dark off", "advance 0",
                     "message count", "advance 249", "message count", "advance 1", "message count"])
        # Four before the screen sleeps. Dark, none runs, whatever passes. At the wake the one that is overdue runs once,
        # and the next comes a whole period after that: not the forty that were missed.
        self.assertEqual(replies(run), [4, 4, 5, 5, 6])

    def test_sntp_jump_does_not_shift_timers(self):
        run = self.handler_app("""
            function app.start() timer.after(5000, function() print("fired", time.mono()) end) end
        """, script=["advance 2000", f"sync {SYNCED}", "advance 2999", "state", "advance 1", "state"])
        before, at = chunks(run)[:2]
        self.assertEqual(logs(before), [])  # the wall clock jumped from nowhere to 2026, and no timer noticed
        self.assertEqual(logs(at), ["fired\t6000"])  # 5000 ms after start() on the monotonic clock, which began at 1000

    # ---- What a timer does ----

    def test_a_timer_may_be_armed_at_the_top_level_of_main_lua(self):
        # The module is installed before main.lua runs (the rulings' module install point).
        main = "timer.after(100, function() print('from the top level') end)\nreturn {}"
        run = self.host(self.make_app("scratch", main), script=["advance 99", "state", "advance 1"])
        before, at = chunks(run)[:2]
        self.assertEqual((logs(before), logs(at)), ([], ["from the top level"]))

    def test_the_handle_is_protected(self):  # X17
        lines = self.lua_lines("""
            local h = timer.after(100, function() end)
            print(type(h), type(getmetatable(h)), (pcall(setmetatable, h, {})))
            print(pcall(function() h.x = 1 end))
        """)
        self.assertEqual(lines[0], "userdata\tstring\tfalse")
        self.assertTrue(lines[1].startswith("false\t"), lines[1])

    def test_a_handle_prints_as_its_slot(self):
        lines = self.lua_lines("""
            local a = timer.after(100, function() end)
            local b = timer.every(100, function() end)
            print(tostring(a), tostring(b))
            a:cancel()
            print(tostring(timer.after(100, function() end)), tostring(a))
        """)
        self.assertEqual(lines, ["timer#0\ttimer#1", "timer#0\ttimer#0"])  # the slot a new timer takes is the free one

    def test_a_repeating_timer_may_cancel_itself(self):
        run = self.lua_app("""
            local n, me = 0
            me = timer.every(100, function() n = n + 1 print("run", n) if n == 3 then me:cancel() end end)
        """, script=["advance 1000", "timers"])
        self.assertEqual(run.logs(), ["run\t1", "run\t2", "run\t3"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_a_one_shot_may_cancel_itself(self):
        run = self.lua_app("""
            local once
            once = timer.after(100, function() once:cancel() timer.cancel(once) print("once") end)
        """, script=["advance 1000", "timers"])
        self.assertEqual(run.logs(), ["once"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_cancel_may_be_called_twice_and_either_way(self):
        run = self.lua_app("""
            local a = timer.after(100, function() print("a") end)
            local b = timer.every(100, function() print("b") end)
            a:cancel() a:cancel() timer.cancel(a)
            timer.cancel(b) b:cancel() timer.cancel(b)
            print("cancelled")
        """, script=["advance 1000", "timers"])
        self.assertEqual(run.logs(), ["cancelled"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_a_stale_handle_cancels_nothing_that_came_after_it(self):
        # A cancelled timer's slot is the next one's, and so is that of a one-shot that has fired: the old handle must not
        # reach the new timer.
        run = self.handler_app("""
            local old, fired, second
            function app.start()
                old = timer.after(100, function() print("old") end)
                old:cancel()
                timer.after(300, function() print("new") end)   -- takes the slot the old one had
                old:cancel() timer.cancel(old)
                fired = timer.after(50, function() print("fired") end)
            end
            function app.message()
                second = timer.after(100, function() print("second") end)   -- takes the slot the one-shot had
                fired:cancel() timer.cancel(fired)
                return true
            end
        """, script=["advance 50", "message arm", "advance 400", "timers"])
        self.assertEqual(run.logs(), ["fired", "second", "new"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_one_timer_may_cancel_another_that_is_due_in_the_same_pass(self):
        run = self.lua_app("""
            local second
            timer.after(100, function() print("first") second:cancel() end)
            second = timer.after(100, function() print("second") end)
        """, script=["advance 100", "advance 1000"])
        self.assertEqual(run.logs(), ["first"])

    def test_a_timer_armed_in_a_callback_comes_a_whole_period_after(self):
        run = self.lua_app("""
            timer.after(100, function() print("outer") timer.after(40, function() print("inner") end) end)
        """, script=["advance 100", "state", "advance 39", "state", "advance 1", "state"])
        self.assertEqual([logs(c) for c in chunks(run)], [["outer"], [], ["inner"], []])

    def test_overdue_timers_run_in_the_order_they_were_due(self):
        run = self.lua_app("""
            timer.after(300, function() print("300") end)
            timer.after(100, function() print("100") end)
            timer.after(200, function() print("200") end)
            timer.every(250, function() print("250") end)
        """, script=["dark on", "advance 1000", "dark off", "advance 0", "timers"])
        self.assertEqual(run.logs(), ["100", "200", "250", "300"])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 1})  # only the repeating one is left

    def test_a_timer_that_is_cancelled_or_has_fired_gives_its_registry_reference_back(self):
        # In a heap of 64 KB, 20000 references that were never given back (16 bytes of registry each) end the app, and the
        # ones that are given back are used again: 20000 cancelled timers, and 20000 one-shots that each arm the next.
        main = """
            local n = 0
            local function again() n = n + 1 if n < 20000 then timer.after(40, again) end end
            return {
                start = function()
                    for i = 1, 20000 do timer.after(100, again):cancel() end
                    timer.after(40, again)
                end,
                message = function() return n end,
            }
        """
        run = self.run_app(main, mem_kb=64, script=["advance 810000", "message count", "timers"])
        self.assertEqual(run.messages("crash"), [])
        self.assertEqual(replies(run), [20000])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    def test_a_timer_may_be_armed_and_cancelled_from_a_coroutine(self):
        # A coroutine runs on a stack of its own, and the registry reference of a cancelled timer is given back through the
        # thread that cancels it, not through the main thread, which is in the middle of a call.
        run = self.handler_app("""
            local co, outer
            function app.start()
                outer = timer.every(100, function() print("outer") end)
                co = coroutine.wrap(function()
                    local a = timer.after(100, function() print("a") end)
                    local b = timer.every(100, function() print("b") end)
                    a:cancel()
                    coroutine.yield()
                    outer:cancel()      -- armed outside, cancelled inside
                    timer.cancel(b)     -- armed inside, cancelled after the yield
                end)
                co()
            end
            function app.message() co() return true end
        """, script=["timers", "advance 250", "message finish", "timers", "advance 1000", "state"])
        self.assertEqual(run.logs(), ["outer", "b", "outer", "b"])
        self.assertEqual([e["count"] for e in run.of("timers")], [2, 0])

    def test_timers_cancelled_in_coroutines_give_their_registry_references_back(self):
        # 3000 coroutines (each is 1000 instructions of the budget) each arm a timer and cancel it: in a heap of 64 KB, 3000
        # references that were never given back (16 bytes of registry each, in an array that doubles) end the app.
        main = """
            local function f() end
            return {start = function()
                for i = 1, 3000 do coroutine.wrap(function() timer.after(100, f):cancel() end)() end
            end}
        """
        run = self.run_app(main, mem_kb=64, script=["timers", "state"])
        self.assertEqual(run.messages("crash"), [])
        self.assertEqual(run.events[-2:], [{"event": "timers", "count": 0}, {"event": "state", "state": "running"}])

    def test_stop_disarms_the_timers_and_start_arms_new_ones(self):
        run = self.handler_app("""
            function app.start() timer.every(100, function() print("tick") end) end
            function app.stop() print("stop") end
        """, script=["advance 100", "stop", "timers", "advance 1000", "start", "timers", "advance 100", "timers"])
        self.assertEqual(run.logs(), ["tick", "stop", "tick"])
        self.assertEqual([e["count"] for e in run.of("timers")], [0, 1, 1])

    def test_an_error_in_a_timer_callback_crashes_the_app_as_in_any_handler(self):
        # The convention the runtime has for a callback nobody waits for (a widget's handler, show, hide, alarm): the app
        # has crashed, with the error and its traceback as the record. Only message() fails alone: it has a caller.
        main = ("local app = {}\nfunction app.start()\n    timer.after(50, function() error('boom') end)\nend\nreturn app\n")
        run = self.host(self.make_app("scratch", main),
                        script=["advance 49", "state", "advance 1", "state", "timers", "advance 100", "state"], rc=1)
        crash = run.messages("crash")
        self.assertEqual(len(crash), 1)
        self.assertTrue(crash[0].startswith("main.lua:3: boom\nstack traceback:\n"), crash[0])
        self.assertIn("main.lua:3:", crash[0].split("stack traceback:")[1])
        self.assertEqual(run.kinds(), ["started", "state", "crash", "state", "timers", "state"])
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "crashed", "crashed"])
        self.assertEqual(run.of("timers"), [{"event": "timers", "count": 0}])  # a crash disarms them

    def test_a_timer_callback_has_the_budget_of_a_callback(self):
        main = "return {start = function()\n    timer.after(50, function() while true do end end)\nend}"
        run = self.host(self.make_app("scratch", main), script=["advance 50"], rc=1)
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])

    def test_a_timer_that_is_killed_ends_the_app_and_its_other_timers_with_it(self):
        main = ("return {start = function()\n"
                "    timer.every(50, function() print('tick') while true do end end)\n"
                "    timer.every(60, function() print('other') end)\nend}")
        run = self.host(self.make_app("scratch", main), script=["advance 1000", "state", "timers"], rc=1)
        self.assertEqual(run.logs(), ["tick"])
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])
        self.assertEqual(run.events[-1], {"event": "timers", "count": 0})

    # ---- The apphost commands ----

    def test_the_commands_that_move_time_say_what_they_need(self):
        run = self.run_app("return {}", script=[
            "advance", "advance x", "advance -1", "advance 1.5", "advance 86400001", "advance 1 2",
            "dark", "dark maybe", "dark on off",
            "sync", "sync x", "sync -1", "sync 1.5", "sync 99999999999999999999", "sync 1 2",
            "sync 0", "sync 1735689599999", "sync 253402300800000",   # before 2025, when the gadget's clock isn't set, and past 9999
            "timers 1"])
        self.assertEqual(run.messages("error"), [
            *["advance needs a number of milliseconds, 0 to 86400000: advance 250"] * 6,
            *["dark needs on or off: dark on | dark off"] * 3,
            *["sync needs the time in milliseconds since 1970 (2025 or later): sync 1767225600000"] * 9,
            "timers takes no arguments",
        ])
        # The two ends that are accepted, the first millisecond of 2025 and the last of 9999, set the clock.
        run = self.handler_app("function app.message() return time.now() end", script=[
            "sync 1735689600000", "message a", "sync 253402300799999", "message b"])
        self.assertEqual(run.messages("error"), [])
        self.assertEqual(replies(run), [1735689600000, 253402300799999])

    def test_advance_moves_the_clock_to_the_millisecond(self):
        run = self.handler_app("""
            function app.message() return time.mono() end
        """, script=["message now", "advance 0", "message now", "advance 7", "message now", "advance 10", "message now",
                     "advance 995", "message now"])
        first = replies(run)[0]
        self.assertEqual(replies(run), [first, first, first + 7, first + 17, first + 1012])

    def test_advance_runs_a_pass_of_the_timers_even_for_zero_ms(self):
        # A timer that came due while the screen was dark runs at `advance 0` after the wake: the clock doesn't move, and
        # the pass is made all the same.
        run = self.lua_app("timer.after(100, function() print('late') end)",
                           script=["dark on", "advance 500", "dark off", "state", "advance 0"])
        before, after = chunks(run)[:2]
        self.assertEqual((logs(before), logs(after)), ([], ["late"]))


class AlarmTest(SvcTestCase):
    """The `alarm` and `sound` modules (apps_svc.c) on apphost's fake clock: an alarm is the system's, so it outlives the app's
    stop and calls the app again, and it runs on the monotonic clock like a timer."""

    def alarm_app(self, handlers, script, permissions=("alarm", "sound"), **kw):
        """An app with the handlers of `handlers` (Lua statements that fill `app`; `bare` and `says` at hand), the manifest
        permissions given, then the script."""
        main = BARE + "local app = {}\n" + handlers + "\nreturn app\n"
        app = self.make_app("scratch", main, manifest={"permissions": list(permissions)})
        return self.host(app, script=script, **kw)

    def alarm_lines(self, source, permissions=("alarm", "sound"), script=None, **kw):
        """A one-file app whose start() runs `source` (Lua statements, `bare` and `says` at hand), with the permissions
        given: the run, whose logs are what start() printed."""
        main = BARE + "local app = {}\nfunction app.start()\n" + source + "\nend\nreturn app\n"
        app = self.make_app("scratch", main, manifest={"permissions": list(permissions)})
        return self.host(app, script=script, **kw)

    # ---- The brief's tests ----

    def test_alarm_fires_and_calls_handler(self):
        run = self.host(FIXTURES / "alarm5", script=["advance 4999", "state", "advance 1", "state", "dump"])
        before, at = chunks(run)[:2]
        self.assertEqual(before, [{"event": "started", "id": "alarm5", "name": "Alarm 5"}])  # 4999 ms: nothing has fired
        self.assertEqual(at, [
            {"event": "alarm", "name": "t", "sound": True, "wake": True},      # at 5000 ms, as the system fires it
            {"event": "ring", "on": True},                                      # with sound, the ring begins
            {"event": "log", "app": "alarm5", "line": "rang t"},                # then the app is told: alarm("t")
        ])
        self.assertEqual(run.of("dump")[0]["widgets"][0]["text"], "rang t")

    def test_alarm_remaining_counts_down(self):
        run = self.alarm_app("""
            function app.start() alarm.after("t", 5000) end
            function app.message()
                return {t = alarm.remaining("t") or false, other = alarm.remaining("other") or false,
                        kind = math.type(alarm.remaining("t")) or false}
            end
        """, script=["message a", "advance 1500", "message b", "advance 3499", "message c", "advance 1", "message d"])
        a, b, c, d = replies(run)
        self.assertEqual(a, {"t": 5000, "other": False, "kind": "integer"})
        self.assertEqual(b, {"t": 3500, "other": False, "kind": "integer"})  # 5000, and `advance 1500`, give 3500
        self.assertEqual(c["t"], 1)
        self.assertEqual(d, {"t": False, "other": False, "kind": False})     # a one-shot that has fired is gone

    def test_alarm_limit_and_name_rules(self):
        wrong_names = ['""', '"A"', '"a b"', '"a-b"', '"a.b"', '"' + "x" * 24 + '"', '"\u00e9"', '"a\\0b"', "5", "nil", "{}", "true"]
        right_names = ['"a"', '"a_1"', '"0"', '"_"', '"' + "x" * 23 + '"']
        wrong_ms = ["0", "-1", "604800001", "1.5", '"1000"', "true", "nil", "{}", "1e100", "0/0", "math.huge", "-math.huge"]
        name_calls = "".join(f"says(alarm.after, {n}, 1000) says(alarm.cancel, {n}) says(alarm.remaining, {n})\n"
                             for n in wrong_names)
        ms_calls = "".join(f'says(alarm.after, "t", {ms})\n' for ms in wrong_ms)
        run = self.alarm_lines(name_calls + ms_calls + """
            says(alarm.after, "t") says(alarm.after)
            says(alarm.after, "t", 1) says(alarm.after, "t", 604800000) says(alarm.after, "t", 5000.0) says(alarm.after, "t", 1e3)
            alarm.cancel("t")
            for _, name in ipairs({""" + ", ".join(right_names) + """}) do says(alarm.after, name, 1000) alarm.cancel(name) end
            for i = 1, 4 do alarm.after("n" .. i, 1000 * i) end
            says(alarm.after, "n5", 1000) says(alarm.after, "n3", 9000) says(alarm.after, "n5", 1000)
            alarm.cancel("n1")
            says(alarm.after, "n5", 1000) says(alarm.after, "n6", 1000)
            print(#alarm.list())
        """, permissions=("alarm",))
        self.assertEqual(run.messages("crash"), [])
        self.assertEqual(run.logs(), [
            *[ALARM_NAME_RULE] * (3 * len(wrong_names)),   # after, cancel and remaining all say it
            *[ALARM_MS_RULE] * len(wrong_ms),
            ALARM_MS_RULE, ALARM_NAME_RULE,                 # no ms; no name at all
            "ok", "ok", "ok", "ok",                         # 1 ms, a week, 5000.0 and 1e3: whole numbers
            *["ok"] * len(right_names),
            TOO_MANY_ALARMS, "ok", TOO_MANY_ALARMS,         # four are armed: a fifth is refused, a name armed is replaced
            "ok", TOO_MANY_ALARMS,                          # one cancelled makes room for one
            "4",
        ])

    def test_permission_messages(self):
        calls = """
            says(alarm.after, "t", 1000) says(alarm.cancel, "t") says(alarm.remaining, "t") says(alarm.list)
            says(sound.chirp) says(sound.stop)
        """
        none = self.alarm_lines('says(alarm.after, "BAD", 0)' + calls, permissions=())
        # The permission is the first thing checked: before the name and the ms.
        self.assertEqual(none.logs(), [ALARM_PERMISSION] * 5 + [SOUND_PERMISSION, "ok"])  # sound.stop needs none
        only_alarm = self.alarm_lines(calls, permissions=("alarm",))
        self.assertEqual(only_alarm.logs(), ["ok", "ok", "ok", "ok", SOUND_PERMISSION, "ok"])
        self.assertEqual(only_alarm.of("chirp"), [])
        only_sound = self.alarm_lines(calls, permissions=("sound",))
        self.assertEqual(only_sound.logs(), [ALARM_PERMISSION] * 4 + ["ok", "ok"])
        self.assertEqual(only_sound.of("chirp"), [{"event": "chirp"}])

    def test_alarm_launches_stopped_app(self):
        run = self.alarm_app("""
            function app.start() print("start") end
            function app.alarm(name) print("alarm " .. name) end
            function app.message() alarm.after("t", 2000) return true end
        """, script=["message arm", "stop", "state", "advance 1999", "state", "advance 1", "state"])
        # Stopped, the app has nothing armed (its timers go with it), but an alarm is the system's: at 2000 ms it starts the app
        # again, and alarm("t") follows start().
        quiet, at = chunks(run)[1:3]
        self.assertEqual(quiet, [])
        self.assertEqual(at, [
            {"event": "alarm", "name": "t", "sound": True, "wake": True},
            {"event": "ring", "on": True},
            {"event": "log", "app": "scratch", "line": "start"},
            {"event": "started", "id": "scratch", "name": "scratch"},
            {"event": "log", "app": "scratch", "line": "alarm t"},
        ])
        self.assertEqual([e["state"] for e in run.of("state")], ["stopped", "stopped", "running"])

    def test_the_launch_by_an_alarm_is_a_new_run_of_the_package(self):  # as on the gadget, where a stop frees the runtime
        run = self.alarm_app("""
            local launches = 0
            print("top")
            function app.start() launches = launches + 1 print("start", launches) end
            function app.alarm(name) print("alarm", name, launches) end
            function app.message() alarm.after("t", 2000) return true end
        """, script=["message arm", "stop", "advance 2000"])
        # main.lua runs from the top again and its locals are new: `launches` is 1 again, not 2.
        self.assertEqual(run.logs(), ["top", "start\t1", "top", "start\t1", "alarm\tt\t1"])

    def test_sound_stop_silences_ring(self):
        run = self.alarm_app("""
            function app.start() alarm.after("t", 1000) end
            function app.message(event) if event == "stop" then sound.stop() end return true end
        """, script=["advance 1000", "message stop", "message stop", "advance 100000"])
        # sound.stop ends the ring, once: the second, with nothing ringing, prints nothing, and nothing ends it later.
        self.assertEqual(run.kinds(), ["started", "alarm", "ring", "ring", "reply", "reply"])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])

    def test_cancelling_the_firing_that_held_the_ring_ends_it_in_that_call(self):
        # Two alarms due together, a1 without sound and a2 with: the ring starts for a2, which waits its turn. a1 is told first and its handler
        # cancels a2, which leaves nothing that could stop the ring (no firing with sound waits, and none was told): it ends in that call,
        # before what the handler prints next, as it does on the gadget.
        run = self.alarm_app("""
            function app.start()
                alarm.after("a1", 100, {sound = false})
                alarm.after("a2", 100)
            end
            function app.alarm(name)
                if name == "a1" then
                    alarm.cancel("a2")
                    print("cancelled")
                end
            end
        """, script=["advance 100", "advance 100"])
        self.assertEqual(run.kinds(), ["started", "alarm", "alarm", "ring", "ring", "log"])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])
        self.assertEqual(run.logs(), ["cancelled"])

    def test_alarm_unaffected_by_clock_jump(self):
        run = self.alarm_app("""
            function app.start() alarm.after("t", 5000) end
            function app.alarm(name) print("rang", name, time.mono()) end
        """, script=["advance 1000", f"sync {SYNCED}", "advance 3999", "state", "advance 1", "state"])
        before, at = chunks(run)[:2]
        self.assertEqual(logs(before), [])  # the wall clock jumped from nowhere to 2026 at 1000 ms, and nothing noticed
        self.assertEqual(logs(at), ["rang\tt\t6000"])  # 5000 ms after start() on the monotonic clock, which began at 1000


    # ---- What the brief's don't cover ----

    def test_the_options_decide_the_ring_and_the_screen(self):
        run = self.alarm_app("""
            function app.start()
                alarm.after("loud", 100)
                alarm.after("quiet", 200, {sound = false})
                alarm.after("dim", 300, {wake = false})
                alarm.after("none", 400, {sound = false, wake = false})
            end
        """, script=["advance 100", "state", "advance 100", "state", "advance 100", "state", "advance 100"])
        started, quiet, dim, none = chunks(run)[:4]
        loud = started[1:]  # after the launch's `started`
        self.assertEqual(loud, [{"event": "alarm", "name": "loud", "sound": True, "wake": True}, {"event": "ring", "on": True}])
        # No sound: no chirp and no change of the ring that goes; the alarm's event says what it asked for.
        self.assertEqual(quiet, [{"event": "alarm", "name": "quiet", "sound": False, "wake": True}])
        self.assertEqual(dim, [{"event": "alarm", "name": "dim", "sound": True, "wake": False}])
        self.assertEqual(none, [{"event": "alarm", "name": "none", "sound": False, "wake": False}])

    def test_an_alarm_with_wake_lights_a_dark_screen_and_one_without_leaves_it(self):
        main = """
            function app.start()
                timer.after(50, function() print("timer") end)
                alarm.after("t", 100, {wake = %s, sound = false})
            end
        """
        # Dark, no timer runs. The alarm at 100 ms lights the screen (wake): the timer, overdue since 50, runs in that step.
        woken = self.alarm_app(main % "true", script=["dark on", "advance 99", "state", "advance 1", "state"])
        before, at = chunks(woken)[:2]
        self.assertEqual(logs(before), [])
        self.assertEqual([e["event"] for e in at], ["alarm", "log"])  # the alarm first, then the timer it woke the screen for
        # Without wake the screen stays dark however long: the timer waits for the screen to be lit.
        dark = self.alarm_app(main % "false", script=["dark on", "advance 200", "state", "dark off", "advance 0"])
        before, after = chunks(dark)[:2]
        self.assertEqual([e["event"] for e in before], ["started", "alarm"])
        self.assertEqual([e["event"] for e in after], ["log"])
        self.assertEqual(logs(after), ["timer"])

    def test_alarms_are_replaced_cancelled_and_listed_earliest_first(self):
        run = self.alarm_app("""
            function app.start()
                alarm.after("b", 2000)
                alarm.after("a", 1000)
                alarm.after("c", 3000, {sound = false})
            end
            function app.message(event, data)
                if event == "list" then return alarm.list() end
                if event == "replace" then alarm.after("a", 5000) return true end
                if event == "cancel" then alarm.cancel("b") alarm.cancel("never") return true end
                if event == "kinds" then
                    local list = alarm.list()
                    return {math.type(list[1].remaining), math.type(alarm.remaining("a")), type(list[1].name), #list}
                end
            end
        """, script=["message list", "advance 400", "message list", "message replace", "message list", "message cancel", "message list",
                     "message kinds"])
        first, later, _, replaced, _, cancelled, kinds = replies(run)
        self.assertEqual(first, [{"name": "a", "remaining": 1000}, {"name": "b", "remaining": 2000}, {"name": "c", "remaining": 3000}])
        self.assertEqual(later, [{"name": "a", "remaining": 600}, {"name": "b", "remaining": 1600}, {"name": "c", "remaining": 2600}])
        self.assertEqual(replaced, [{"name": "b", "remaining": 1600}, {"name": "c", "remaining": 2600}, {"name": "a", "remaining": 5000}])
        self.assertEqual(cancelled, [{"name": "c", "remaining": 2600}, {"name": "a", "remaining": 5000}])
        self.assertEqual(kinds, ["integer", "integer", "string", 2])
        self.assertEqual(run.of("alarm"), [])  # nothing came due: a replaced and a cancelled alarm never fire

    def test_alarm_and_sound_return_no_handle_and_list_gives_plain_tables(self):  # X17: nothing for getmetatable to open
        run = self.alarm_app("""
            function app.message()
                local counts = {select("#", alarm.after("t", 1000)), select("#", alarm.cancel("t")), select("#", sound.chirp()),
                                select("#", sound.stop())}
                alarm.after("t", 1000)
                local item = alarm.list()[1]
                return {counts = counts, item = type(getmetatable(item)), list = type(getmetatable(alarm.list()))}
            end
        """, script=["message go"])
        self.assertEqual(replies(run), [{"counts": [0, 0, 0, 0], "item": "nil", "list": "nil"}])

    def test_a_fired_alarm_is_gone_when_alarm_is_called_and_the_handler_may_arm_it_again(self):
        run = self.alarm_app("""
            local n = 0
            function app.start() alarm.after("t", 1000) end
            function app.alarm(name)
                n = n + 1
                print("rang " .. name .. " " .. tostring(alarm.remaining(name)) .. " " .. #alarm.list())
                if n < 3 then alarm.after(name, 1000) end
            end
        """, script=["advance 3500"])
        self.assertEqual(logs(run.events), ["rang t nil 0", "rang t nil 0", "rang t nil 0"])
        self.assertEqual(len(run.of("alarm")), 3)

    def test_start_may_arm_the_name_that_started_the_app(self):
        # An alarm clock sets its next alarm in start(): the alarm that launched it is still told, and the new one is armed.
        run = self.alarm_app("""
            function app.start() alarm.after("t", 1000) end
            function app.alarm(name) print("rang " .. name .. " " .. tostring(alarm.remaining(name))) end
        """, script=["stop", "advance 1000", "state", "advance 1000", "state"])
        first, second = chunks(run)[0][1:], chunks(run)[1]
        # Started, then told, with start()'s new alarm 1000 ms away; and that one, 1000 ms on, finds nothing left to arm.
        self.assertEqual([e["event"] for e in first], ["alarm", "ring", "started", "log"])
        self.assertEqual(logs(first), ["rang t 1000"])
        self.assertEqual([e["event"] for e in second], ["alarm", "log"])
        self.assertEqual(logs(second), ["rang t nil"])

    def test_the_ring_goes_on_a_minute_from_the_last_alarm_with_sound(self):
        run = self.alarm_app("""
            function app.start() alarm.after("one", 1000) alarm.after("two", 30000) end
        """, script=["advance 29999", "state", "advance 1", "state", "advance 59999", "state", "advance 1", "state", "advance 1000", "state"])
        first, second, still, due, over = chunks(run)[:5]
        self.assertEqual([e["event"] for e in first], ["started", "alarm", "ring"])  # the ring began at 2000 ms (the clock's, which began at 1000)
        self.assertEqual([e["event"] for e in second], ["alarm"])  # at 31000, the second makes it a minute longer: to 91000
        self.assertEqual(still, [])                                 # past the first minute, 62000: still going
        # The minute is over at 91000, and the ring ends at its next tick, as on the gadget (the ring's period is 1.5 s from 2000: 92000).
        self.assertEqual(due, [])
        self.assertEqual(over, [{"event": "ring", "on": False}])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])

    def test_a_chirp_is_an_event_in_order_with_the_apps_own_logs(self):
        run = self.alarm_app("""
            function app.message() print("a") sound.chirp() print("b") sound.chirp() return true end
        """, script=["message go"])
        self.assertEqual([(e["event"], e.get("line")) for e in run.events[1:-1]],
                         [("log", "a"), ("chirp", None), ("log", "b"), ("chirp", None)])

    def test_an_app_that_crashed_is_started_again_by_its_alarm(self):
        # A crash ends the app, and its alarm, which is the system's, starts it from the package again (a new runtime: its timer,
        # the one that crashed it, was armed by start() as well, and is armed again).
        run = self.alarm_app("""
            function app.start()
                print("start")
                alarm.after("t", 1500)
                timer.after(500, function() error("boom") end)
            end
            function app.alarm(name) print("rang " .. name) end
        """, script=["advance 500", "state", "advance 1000", "state"])
        crashed, relaunched = chunks(run)[:2]
        self.assertEqual(crashed[-1]["event"], "crash")
        self.assertEqual([e["event"] for e in relaunched], ["alarm", "ring", "log", "started", "log"])
        self.assertEqual(logs(relaunched), ["start", "rang t"])
        self.assertEqual([e["state"] for e in run.of("state")], ["crashed", "running"])

    def test_an_error_in_alarm_crashes_the_app_and_ends_the_ring(self):
        run = self.alarm_app("""
            function app.start() alarm.after("t", 1000) end
            function app.alarm(name) error("alarm boom") end
        """, script=["advance 1000", "state", "advance 100000"], rc=1)
        at = chunks(run)[0][1:]  # after the launch's `started`
        self.assertEqual([e["event"] for e in at], ["alarm", "ring", "crash", "ring"])  # once: the advance doesn't say it again
        self.assertTrue(at[2]["message"].startswith("main.lua:"), at[2])
        self.assertEqual(at[3], {"event": "ring", "on": False})  # no one is left to stop it
        self.assertEqual(len(run.of("ring")), 2)
        self.assertEqual(chunks(run)[1], [])  # and no later ring

    def test_alarms_that_come_due_together_fire_in_the_order_they_were_armed(self):
        # Not the order of their slots: "junk" takes a slot and gives it back, so "y", armed after "x", is in a lower slot than it.
        run = self.alarm_app("""
            function app.start()
                alarm.after("late", 30, {sound = false})
                alarm.after("junk", 5000, {sound = false})
                alarm.after("x", 10, {sound = false})
                alarm.cancel("junk")
                alarm.after("y", 10, {sound = false})
            end
            function app.alarm(name) print(name) end
        """, script=["advance 100"])
        self.assertEqual([e["name"] for e in run.of("alarm")], ["x", "y", "late"])
        self.assertEqual(logs(run.events), ["x", "y", "late"])

    def test_alarms_due_together_are_listed_in_the_order_they_were_armed(self):
        run = self.alarm_app("""
            function app.start()
                alarm.after("junk", 5000)
                alarm.after("x", 100)
                alarm.cancel("junk")
                alarm.after("y", 100)
            end
            function app.message() return alarm.list() end
        """, script=["message list"])
        self.assertEqual(replies(run), [[{"name": "x", "remaining": 100}, {"name": "y", "remaining": 100}]])

    # ---- Fix round 1: what the system's alarms do when an app misuses them, and what apphost shows of it ----

    def test_a_fired_alarm_is_listed_inside_start_of_the_app_it_launched(self):  # P2: as on the gadget, it waits in its slot
        run = self.alarm_app("""
            function app.start() print("start", #alarm.list(), tostring(alarm.remaining("t"))) end
            function app.alarm(name) print("alarm", name, #alarm.list(), tostring(alarm.remaining(name))) end
            function app.message() alarm.after("t", 2000) return true end
        """, script=["message arm", "stop", "advance 2000"])
        # The first start has no alarm; the second is the alarm's launch, in which the alarm that fired is listed with nothing left
        # until the app is told (and is gone then).
        self.assertEqual(run.logs(), ["start\t0\tnil", "start\t1\t0", "alarm\tt\t0\tnil"])

    def test_an_app_whose_start_crashes_when_its_alarm_launches_it_is_launched_once(self):  # P1, and the failed relaunch
        run = self.alarm_app("""
            function app.start()
                print("start")
                if time.mono() > 1500 then error("boom") end
                alarm.after("t", 2000)
                alarm.after("far", 100000)
            end
            function app.alarm(name) print("alarm", name) end
        """, script=["stop", "advance 2000", "state", "advance 200000", "state"], rc=1)
        # The alarm at 3000 launches it again; start() raises; its other alarm goes with it, and the ring that alarm began ends.
        self.assertEqual(run.logs(), ["start", "start"])
        self.assertEqual([e["name"] for e in run.of("alarm")], ["t"])
        self.assertEqual(len(run.of("crash")), 1)
        self.assertEqual(run.messages("warning"), ["alarm: scratch crashed when its alarm launched it; its alarms are cancelled"])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])
        self.assertEqual([e["state"] for e in run.of("state")], ["crashed", "crashed"])
        after = run.kinds()
        self.assertEqual(after[after.index("crash"):], ["crash", "warning", "ring", "state", "state"])  # nothing came of "far"

    def test_an_app_that_crashes_in_the_alarm_it_was_launched_for_loses_its_alarms(self):  # P1, the second place
        run = self.alarm_app("""
            function app.start()
                if time.mono() < 1500 then alarm.after("t", 2000) alarm.after("far", 100000) end
                print("start")
            end
            function app.alarm(name) print("alarm", name) error("boom") end
        """, script=["stop", "advance 2000", "state", "advance 200000", "state"], rc=1)
        self.assertEqual(run.logs(), ["start", "start", "alarm\tt"])
        self.assertEqual([e["name"] for e in run.of("alarm")], ["t"])
        self.assertEqual(len(run.of("crash")), 1)
        self.assertEqual(run.messages("warning"), ["alarm: scratch crashed when its alarm launched it; its alarms are cancelled"])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])
        self.assertEqual([e["state"] for e in run.of("state")], ["crashed", "crashed"])

    def test_two_alarms_that_fire_together_launch_an_app_that_crashes_once(self):  # P1: what was copied is stale once the alarms are cancelled
        run = self.alarm_app("""
            function app.start()
                print("start")
                if time.mono() > 1500 then error("boom") end
                alarm.after("a", 2000)
                alarm.after("b", 2000)
            end
            function app.alarm(name) print("alarm", name) end
        """, script=["stop", "advance 2000", "state", "advance 200000", "state"], rc=1)
        self.assertEqual(run.logs(), ["start", "start"])
        self.assertEqual(len(run.of("crash")), 1)
        self.assertEqual([e["name"] for e in run.of("alarm")], ["a", "b"])  # both fired, and neither was told
        self.assertEqual(run.messages("warning"), ["alarm: scratch crashed when its alarm launched it; its alarms are cancelled"])

    def test_an_app_that_arms_its_alarm_again_in_alarm_is_cut_off_after_six_in_a_minute(self):  # A: the reviewer's package
        run = self.alarm_app("""
            function app.start() alarm.after("t", 1) end
            function app.alarm(name) alarm.after(name, 1) end
        """, script=["advance 100", "state", "stop", "state", "advance 200000", "state"])
        self.assertEqual(len(run.of("alarm")), 6)
        self.assertEqual(run.messages("warning"), ["alarm: scratch fired more than 6 alarms in a minute; its alarms are cancelled"])
        before, stopped, after = chunks(run)[:3]
        self.assertEqual(before[-1]["event"], "warning")
        # The app runs and was told a ring: it ends with the app, when the user stops it, and the stop sticks.
        self.assertEqual(stopped, [{"event": "ring", "on": False}])
        self.assertEqual(after, [])
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "stopped", "stopped"])
        self.assertEqual(run.kinds().count("started"), 1)

    def test_an_app_that_arms_alarms_from_a_timer_gets_six_in_a_minute_and_stays_stopped(self):  # A: the 1 ms re-arm
        run = self.alarm_app("""
            function app.start() timer.every(40, function() for k = 1, 4 do alarm.after("a" .. k, 1) end end) end
        """, script=["advance 3000", "state", "stop", "advance 200000", "state"])
        # Four alarms due together come every 40 ms: the first tick's four are told, and the second tick's 5th and 6th firings are the budget's
        # last; the 7th cancels the app's alarms, the ones of this burst that wait to be told among them (the table's own, as on the gadget).
        self.assertGreaterEqual(len(run.of("alarm")), 4)
        self.assertLessEqual(len(run.of("alarm")), 6)
        warnings = run.messages("warning")
        self.assertEqual(warnings[0], "alarm: scratch fired more than 6 alarms in a minute; its alarms are cancelled")
        self.assertLessEqual(len(warnings), 4 + 4 * 3 + 1, "the lines are held to about four a second")
        self.assertEqual(run.kinds().count("started"), 1)  # alarms that were left armed when it stopped don't launch it
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "stopped"])

    def test_the_ring_ends_when_the_app_that_was_told_is_stopped(self):  # B: nothing is left that could stop it
        run = self.alarm_app("""
            function app.start() alarm.after("t", 1000) end
        """, script=["advance 1000", "state", "stop", "state", "advance 100000"])
        before, stopped, after = chunks(run)[:3]
        self.assertEqual([e["event"] for e in before], ["started", "alarm", "ring"])
        self.assertEqual(stopped, [{"event": "ring", "on": False}])
        self.assertEqual(after, [])

    def test_alarm_and_sound_may_be_used_at_the_top_level_of_main_lua(self):  # modules are there before main.lua runs
        run = self.alarm_app("""
            alarm.after("t", 100)
            sound.stop()
            function app.alarm(name) print("alarm(" .. name .. ")") end
        """, script=["advance 99", "state", "advance 1"])
        before, at = chunks(run)[:2]
        self.assertEqual(before, [{"event": "started", "id": "scratch", "name": "scratch"}])
        self.assertEqual(at, [
            {"event": "alarm", "name": "t", "sound": True, "wake": True},
            {"event": "ring", "on": True},
            {"event": "log", "app": "scratch", "line": "alarm(t)"},
        ])

    def test_the_options_are_checked(self):
        run = self.alarm_lines("""
            says(alarm.after, "t", 1000, 5) says(alarm.after, "t", 1000, "loud") says(alarm.after, "t", 1000, {sound = 1})
            says(alarm.after, "t", 1000, {wake = "yes"}) says(alarm.after, "t", 1000, {loud = true}) says(alarm.after, "t", 1000, {true})
            says(alarm.after, "t", 1000, {}) says(alarm.after, "t", 1000, nil) says(alarm.after, "t", 1000, {sound = false, wake = false})
        """, permissions=("alarm",))
        self.assertEqual(run.logs(), [
            "alarm.after: options must be a table like {sound = true, wake = true}, got number",
            "alarm.after: options must be a table like {sound = true, wake = true}, got string",
            "alarm.after: sound must be true or false, got number",
            "alarm.after: wake must be true or false, got string",
            "alarm.after: unknown option 'loud' (valid: sound, wake)",
            "alarm.after: unknown option '<number>' (valid: sound, wake)",
            "ok", "ok", "ok",
        ])


class TimeTest(SvcTestCase):
    # ---- The brief's tests ----

    def test_time_now_nil_until_synced_then_date_format(self):
        run = self.handler_app("""
            function app.message(event)
                if event == "before" then
                    local ok, message = pcall(time.date)
                    local format_ok, format_message = pcall(time.format, "%Y")
                    return {now_is_nil = time.now() == nil, synced = time.synced(), ok = ok, message = bare(message),
                            format_ok = format_ok, format_message = bare(format_message)}
                end
                return {year = time.date().year, text = time.format("%Y-%m-%d %H:%M"), now = time.now(),
                        synced = time.synced(), os_time = os.time()}
            end
        """, script=["message before", f"sync {SYNCED}", "message after", "advance 1500", "message after"])
        before, after, later = replies(run)
        self.assertEqual(before, {"now_is_nil": True, "synced": False, "ok": False, "message": CLOCK_NOT_SET,
                                  "format_ok": False, "format_message": FORMAT_CLOCK_NOT_SET})
        self.assertEqual(after, {"year": 2026, "text": "2026-01-01 00:00", "now": SYNCED, "synced": True,
                                 "os_time": SYNCED // 1000})
        # The clock moves with the monotonic clock from then on.
        self.assertEqual(later["now"], SYNCED + 1500)
        self.assertEqual(later["os_time"], (SYNCED + 1500) // 1000)

    def test_os_aliases(self):
        run = self.handler_app("""
            function app.message()
                return {time_type = math.type(os.time()), year_type = type(os.date("*t").year),
                        clock_type = math.type(os.clock()), diff = os.difftime(5, 3), diff_type = math.type(os.difftime(5, 3)),
                        boot = os.time() == time.mono() // 1000, clock = os.clock() == time.mono() / 1000,
                        year = os.date("%Y")}
            end
        """, script=["message a", "advance 2500", "message a", f"sync {SYNCED}", "message a"])
        first, second, third = replies(run)
        for reply in (first, second, third):
            self.assertEqual((reply["time_type"], reply["year_type"], reply["clock_type"]), ("integer", "number", "float"))
            self.assertEqual((reply["diff"], reply["diff_type"], reply["clock"]), (2.0, "float", True))
        # Before the first sync os.time() is seconds since boot and os.date never raises: it is 1970 (the rulings).
        self.assertEqual((first["boot"], second["boot"], first["year"], second["year"]), (True, True, "1970", "1970"))
        self.assertEqual((third["boot"], third["year"]), (False, "2026"))

    def test_a_run_is_in_utc_whatever_the_zone_of_the_machine_is(self):
        # These tests run apphost in the board's zone (SvcTestCase.host_env), as a machine in any zone could: the run is UTC0
        # (apphost.c sets it), so that a script gives the same events everywhere, and `app.py check` with it.
        run = self.handler_app("""
            function app.message()
                local t = os.date("*t", 3600 * 20)
                return {os.date("%H:%M", 0), time.format("%H:%M", 0), t.hour, t.isdst, os.time{year = 1970, month = 1, day = 2, hour = 0},
                        os.date("%H", 3600 * 20)}
            end
        """, script=["message zone"])
        self.assertEqual(replies(run), [["00:00", "00:00", 20, False, 86400, "20"]])

    def test_the_label_of_the_time_fixture_follows_the_clock(self):
        run = self.host(FIXTURES / "time_label",
                        script=["dump", f"sync {SYNCED}", "advance 1000", "dump", "advance 61000", "dump"])
        texts = [e["widgets"][0]["text"] for e in run.of("dump")]
        self.assertEqual(texts, ["no time yet", "Thu 00:00", "Thu 00:01"])

    # ---- time.date and time.format ----

    def test_time_date_fields(self):
        cases = [
            (SYNCED, dict(year=2026, month=1, day=1, hour=0, min=0, sec=0, wday=5, yday=1)),            # a Thursday
            (1709251199000, dict(year=2024, month=2, day=29, hour=23, min=59, sec=59, wday=5, yday=60)),  # a leap day
            (1772964000000, dict(year=2026, month=3, day=8, hour=10, min=0, sec=0, wday=1, yday=67)),     # a Sunday: 1
            (1767398400000, dict(year=2026, month=1, day=3, hour=0, min=0, sec=0, wday=7, yday=3)),       # a Saturday: 7
            (1798761599999, dict(year=2026, month=12, day=31, hour=23, min=59, sec=59, wday=5, yday=365)),  # ms are cut
            (0, dict(year=1970, month=1, day=1, hour=0, min=0, sec=0, wday=5, yday=1)),                  # needs no sync
            (-1000, dict(year=1969, month=12, day=31, hour=23, min=59, sec=59, wday=4, yday=365)),
            (-1, dict(year=1969, month=12, day=31, hour=23, min=59, sec=59, wday=4, yday=365)),           # floored, not cut
        ]
        run = self.handler_app("""
            function app.message(event, data) return time.date(data) end
        """, script=[f"message date {ms}" for ms, _ in cases])
        for (ms, want), got in zip(cases, replies(run)):
            with self.subTest(ms=ms):
                self.assertEqual(got, want)  # exactly these eight fields
        # And a lot of others, against another way of working them out.
        rng = random.Random(16)
        picks = [rng.randrange(-2_000_000_000_000, 4_102_444_800_000) for _ in range(300)]
        run = self.handler_app("""
            function app.message(event, data)
                local out = {}
                for i, ms in ipairs(data) do
                    local d = time.date(ms)
                    out[i] = {d.year, d.month, d.day, d.hour, d.min, d.sec, d.wday, d.yday}
                end
                return out
            end
        """, script=["message dates [" + ",".join(map(str, picks)) + "]"])
        for ms, got in zip(picks, replies(run)[0]):
            dt = datetime.fromtimestamp(ms // 1000, timezone.utc)
            want = [dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.isoweekday() % 7 + 1, dt.timetuple().tm_yday]
            self.assertEqual(got, want, ms)

    def test_time_date_and_format_take_a_whole_number_of_milliseconds(self):
        run = self.handler_app("""
            function app.message()
                local out = {}
                for _, ms in ipairs({1.5, "0", true, {}, 0/0, math.huge}) do
                    local ok, message = pcall(time.date, ms)
                    out[#out + 1] = bare(message)
                    ok, message = pcall(time.format, "%Y", ms)
                    out[#out + 1] = bare(message)
                end
                out[#out + 1] = time.date(1e3).sec
                out[#out + 1] = time.format("%S", 2000.0)
                return out
            end
        """, script=["message x"])
        got = replies(run)[0]
        self.assertEqual(got[-2:], [1, "02"])
        for message in got[:-2]:
            self.assertRegex(message, r"^time\.(date|format): ms must be a whole number of milliseconds, got ")
        self.assertEqual(len(got), 14)

    def test_the_calendar_is_the_years_0_to_9999(self):
        # The first and the last instant of the calendar are taken, a millisecond outside either is an error, and so is
        # anything further: on the gadget (picolibc wraps the year of such a time) as here (the C library refuses), the same.
        run = self.handler_app("""
            function app.message(event, data)
                local out = {}
                for _, ms in ipairs(data) do
                    local ok, d = pcall(time.date, ms)
                    out[#out + 1] = ok and d.year or bare(d)
                    ok, d = pcall(time.format, "%Y", ms)
                    out[#out + 1] = ok and d or bare(d)
                    ok, d = pcall(os.date, "!%Y", ms // 1000)
                    out[#out + 1] = ok and d or bare(d)
                end
                for _, f in ipairs({function() return os.date("*t", math.maxinteger) end,
                                    function() return os.date("%Y", math.mininteger) end,
                                    function() return time.date(math.maxinteger) end,
                                    function() return time.format("%Y", math.mininteger) end}) do
                    local ok, d = pcall(f)
                    out[#out + 1] = ok and "ok" or bare(d)
                end
                return out
            end
        """, script=["message edges [-62167219200000,-62167219200001,253402300799999,253402300800000]"])
        got = replies(run)[0]
        out_of_range = [f"time.date: ms is out of range {YEARS}", f"time.format: ms is out of range {YEARS}",
                        f"os.date: the time is out of range {YEARS}"]
        self.assertEqual(got, [0, "0", "0", *out_of_range, 9999, "9999", "9999", *out_of_range,
                               f"os.date: the time is out of range {YEARS}", f"os.date: the time is out of range {YEARS}",
                               f"time.date: ms is out of range {YEARS}", f"time.format: ms is out of range {YEARS}"])

    def test_time_format_specifiers(self):
        days = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
        months = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
                  "November", "December"]

        def expected(fmt, ms):
            dt = datetime.fromtimestamp(ms // 1000, timezone.utc)
            names = {"Y": str(dt.year), "m": f"{dt.month:02d}", "d": f"{dt.day:02d}", "H": f"{dt.hour:02d}",
                     "M": f"{dt.minute:02d}", "S": f"{dt.second:02d}", "I": f"{dt.hour % 12 or 12:02d}",
                     "p": "AM" if dt.hour < 12 else "PM", "a": days[dt.weekday()][:3], "A": days[dt.weekday()],
                     "b": months[dt.month - 1][:3], "B": months[dt.month - 1], "j": f"{dt.timetuple().tm_yday:03d}",
                     "y": f"{dt.year % 100:02d}", "e": f"{dt.day:2d}", "%": "%"}
            return re.sub(r"%(.)", lambda m: names[m.group(1)], fmt)

        fmt = "%Y|%m|%d|%H|%M|%S|%I|%p|%a|%A|%b|%B|%j|%y|%e|%%"
        edges = [0, SYNCED, SYNCED - 1, 1709251199000, 1735689599000, 951782400000,    # the leap days, 2024 and 2000
                 1767268800000, 1767268799000, 1767272400000, 1767311999000,            # noon, 11:59:59, 13:00, 23:59:59
                 1767229200000, 1767225600999, 1798761600000, 4102444799000]            # 01:00, 999 ms, 2027, 2099
        rng = random.Random(1616)
        picks = edges + [rng.randrange(0, 4_102_444_800_000) for _ in range(250)]
        run = self.handler_app("""
            function app.message(event, data)
                local out = {}
                for i, ms in ipairs(data.list) do
                    out[i] = time.format(data.fmt, ms)
                    -- os.date takes the same specifiers, in seconds, and the zone here is UTC for both of its forms
                    if os.date("!" .. data.fmt, ms // 1000) ~= out[i] or os.date(data.fmt, ms // 1000) ~= out[i] then
                        out[i] = "os.date differs: " .. os.date(data.fmt, ms // 1000)
                    end
                end
                return out
            end
        """, script=["message formats " + '{"fmt":"' + fmt + '","list":[' + ",".join(map(str, picks)) + "]}"])
        got = replies(run)[0]
        self.assertEqual(len(got), len(picks))
        for ms, text in zip(picks, got):
            self.assertEqual(text, expected(fmt, ms), ms)
        # Read by eye: 2026-10-28 15:04:05 UTC is a Wednesday, the 301st day of the year; %e pads with a space and %d with a
        # zero; midnight and noon are 12 on the clock face of %I.
        run = self.handler_app("function app.message(event, data) return time.format(data.fmt, data.ms) end", script=[
            'message f {"fmt":"%A %a %B %b %d %e %j %H %I %M %p %S %y %Y %%","ms":1793199845000}',
            'message f {"fmt":"%e|%d|%I %p","ms":1767571200000}',
            'message f {"fmt":"%I %p","ms":1767268800000}'])
        self.assertEqual(replies(run), ["Wednesday Wed October Oct 28 28 301 15 03 04 PM 05 26 2026 %", " 5|05|12 AM",
                                        "12 PM"])
        # The first second of the calendar is the year 0: %Y has no padding, %y two digits.
        run = self.handler_app("function app.message() return time.format('%Y %y', -62167219200000) end", script=["message n"])
        self.assertEqual(replies(run), ["0 00"])

    def test_time_format_refuses_what_it_does_not_know_and_a_result_over_128_bytes(self):
        specs = ["%Q", "%c", "%x", "%-d", "%E", "%5", "x%Z", "%%%v"]
        calls = "".join(f'says(time.format, "{spec}", 0) says(os.date, "{spec}", 0) says(os.date, "!{spec}", 0)\n'
                        for spec in specs)
        lines = self.lua_lines(calls + """
            says(time.format, "%", 0) says(os.date, "%", 0) says(time.format, "100%", 0)
            says(time.format) says(time.format, 5, 0) says(time.format, {}, 0)
            local year = 1767225600000
            says(time.format, ("%Y"):rep(32), year) says(time.format, ("%Y"):rep(33), year)
            says(time.format, ("x"):rep(128), year) says(time.format, ("x"):rep(129), year)
            says(time.format, ("%%"):rep(128), year) says(time.format, ("%%"):rep(129), year)
            says(os.date, ("%Y"):rep(33), 0) says(os.date, "!" .. ("x"):rep(129), 0)
            print(#time.format(("%Y"):rep(32), year), #time.format(("%A"):rep(3), year))
        """)
        want = []
        for spec in ["%Q", "%c", "%x", "%-", "%E", "%5", "%Z", "%v"]:
            want += [unsupported("time.format", spec), unsupported("os.date", spec), unsupported("os.date", spec)]
        want += [f"time.format: {LONE_PERCENT}", f"os.date: {LONE_PERCENT}", f"time.format: {LONE_PERCENT}"]
        want += ["time.format: fmt must be a string, got no value", "time.format: fmt must be a string, got number",
                 "time.format: fmt must be a string, got table"]
        want += ["ok", too_long("time.format"), "ok", too_long("time.format"), "ok", too_long("time.format"),
                 too_long("os.date"), too_long("os.date"), "128\t24"]
        self.assertEqual(lines, want)

    def test_a_specifier_that_is_not_printable_is_shown_as_a_question_mark(self):
        # The one place these modules echo a byte of the app's own format: the error leaves as text, where "\n" would be a line
        # of its own, so the byte is a '?' (printable ASCII is shown as it is).
        lines = self.lua_lines(r"""
            says(time.format, "%\n", 0) says(time.format, "%\1", 0) says(time.format, "%\127", 0) says(time.format, "%\255", 0)
            says(os.date, "%\n", 0) says(os.date, "!%\1", 0) says(os.date, "%\127", 0) says(os.date, "!%\255", 0)
        """)
        self.assertEqual(lines, [unsupported("time.format", "%?")] * 4 + [unsupported("os.date", "%?")] * 4)

    # ---- os.date and os.time ----

    def test_os_date(self):
        run = self.handler_app("""
            function app.message(event)
                local s = 1793199845   -- 2026-10-28 15:04:05 UTC, a Wednesday
                if event == "star" then
                    return {local_t = os.date("*t", s), utc_t = os.date("!*t", s)}
                end
                return {os.date("%Y-%m-%d %H:%M:%S", s), os.date("!%Y-%m-%d %H:%M:%S", s), os.date(nil, s), os.date("x*t", s),
                        os.date("", s), os.date("%%", s), os.date("*t", 0.0 + s).hour, os.date("%H", 1.0e3)}
            end
        """, script=["message star", "message text"])
        star, text = replies(run)
        fields = dict(year=2026, month=10, day=28, hour=15, min=4, sec=5, wday=4, yday=301, isdst=False)
        self.assertEqual(star, {"local_t": fields, "utc_t": fields})
        self.assertEqual(text, ["2026-10-28 15:04:05", "2026-10-28 15:04:05", "Wed Oct 28 15:04:05 2026", "x*t", "", "%",
                                15, "00"])

    def test_os_date_without_a_time_is_the_clock(self):
        run = self.handler_app("""
            function app.message() return {os.date(), os.date("%Y-%m-%d %H:%M:%S"), os.date("!%H"), os.date("*t").year} end
        """, script=["message a", f"sync {SYNCED}", "advance 61000", "message b"])
        before, after = replies(run)
        self.assertEqual(before, ["Thu Jan  1 00:00:01 1970", "1970-01-01 00:00:01", "00", 1970])  # seconds since boot
        self.assertEqual(after, ["Thu Jan  1 00:01:01 2026", "2026-01-01 00:01:01", "00", 2026])

    def test_os_date_table_is_the_one_of_time_date_with_isdst(self):
        run = self.handler_app("""
            function app.message(event, data)
                local a, b = time.date(data * 1000), os.date("*t", data)
                local same = true
                for k, v in pairs(a) do same = same and b[k] == v end
                local n = 0
                for _ in pairs(b) do n = n + 1 end
                return {same = same, fields = n, isdst = type(b.isdst)}
            end
        """, script=["message d 1793199845", "message d 0"])
        self.assertEqual(replies(run), [{"same": True, "fields": 9, "isdst": "boolean"}] * 2)

    def test_os_time_with_a_table_is_mktime(self):
        def epoch(*args):
            return int(datetime(*args, tzinfo=timezone.utc).timestamp())

        run = self.handler_app("""
            function app.message(event, data)
                local out = {}
                for _, t in ipairs({{year = 2026, month = 1, day = 1, hour = 0}, {year = 2026, month = 1, day = 1},
                                    {year = 2026, month = 13, day = 1, hour = 0}, {year = 2026, month = 3, day = 0, hour = 0},
                                    {year = 2026, month = 1, day = 1, hour = 0, min = 90, sec = -30},
                                    {year = "2026", month = 1, day = 1, hour = 0, isdst = false}}) do
                    out[#out + 1] = os.time(t)
                end
                local t = {year = 2026, month = 13, day = 32, hour = 25}
                out[#out + 1] = os.time(t)
                out[#out + 1] = {t.year, t.month, t.day, t.hour, t.min, t.sec, t.wday, t.yday}
                return out
            end
        """, script=["message t"])
        got = replies(run)[0]
        self.assertEqual(got[:6], [epoch(2026, 1, 1, 0), epoch(2026, 1, 1, 12), epoch(2027, 1, 1, 0), epoch(2026, 2, 28, 0),
                                   epoch(2026, 1, 1, 1, 29, 30), epoch(2026, 1, 1, 0)])
        # Lua writes the normalized fields back: Feb 2 2027 is a Tuesday (wday 3), the 33rd day of the year.
        self.assertEqual(got[6], epoch(2027, 2, 2, 1))
        self.assertEqual(got[7], [2027, 2, 2, 1, 0, 0, 3, 33])

    def test_os_time_says_what_is_wrong_with_the_table(self):
        lines = self.lua_lines("""
            says(os.time, {year = 2026, month = 1})
            says(os.time, {month = 1, day = 1})
            says(os.time, {year = 2026, month = 1.5, day = 1})
            says(os.time, {year = "x", month = 1, day = 1})
            says(os.time, {year = 2^40, month = 1, day = 1})
            says(os.time, {year = 2026, month = 1, day = 1, hour = 2^31})
            says(os.time, 5)
            says(os.time, "now")
            says(os.time, nil)
            says(os.time, {year = 2026, month = 1, day = 1, hour = 0, sec = 0.0})
        """)
        self.assertEqual(lines, [
            "os.time: field 'day' missing in date table",
            "os.time: field 'year' missing in date table",
            "os.time: field 'month' is not an integer",
            "os.time: field 'year' is not an integer",
            "os.time: field 'year' is out-of-bound",
            "os.time: field 'hour' is out-of-bound",
            f"os.time: {TABLE_ARGUMENT}, got number",
            f"os.time: {TABLE_ARGUMENT}, got string",
            "ok",
            "ok",
        ])

    def test_os_time_refuses_what_would_hold_the_app_and_what_is_outside_the_calendar(self):
        # picolibc's mktime takes a month a step through the days it is given: 2^31 of them are six seconds on the gadget, so
        # the days that day, hour, min and sec add up to are limited, on both sides (the same here). The result must be in
        # the calendar too.
        lines = self.lua_lines("""
            says(os.time, {year = 2026, month = 1, day = 2^31 - 1})
            says(os.time, {year = 2026, month = 1, day = -(2^31 - 1)})
            says(os.time, {year = 2026, month = 1, day = 1, hour = 2^31 - 1})
            says(os.time, {year = 2026, month = 1, day = 1, hour = 72000001})    -- 3000001 days
            says(os.time, {year = 2026, month = 1, day = 1, hour = 71999999})    -- 3000000: the limit is taken, and is far
            says(os.time, {year = 2026, month = 1, day = 3000000})
            says(os.time, {year = 2026, month = 1, day = 3000001})
            says(os.time, {year = 2026, month = 1, day = 2000000, min = 2^31 - 1})   -- the minutes add 1491308 days
            says(os.time, {year = 2026, month = 1, day = 2990000, sec = 2^31 - 1})   -- the seconds add 24855
            says(os.time, {year = 2026, month = 1, day = 1, min = 2^31 - 1})     -- 1491308 days: 4085 years, in the calendar
            says(os.time, {year = 2026, month = 1, day = 1, sec = 2^31 - 1})
            says(os.time, {year = 2026, month = 1, day = 1, sec = -(2^31 - 1)})
            says(os.time, {year = 9999, month = 12, day = 31, hour = 23, min = 59, sec = 59})
            says(os.time, {year = 9999, month = 12, day = 31, hour = 23, min = 59, sec = 60})
            says(os.time, {year = 10000, month = 1, day = 1})
            says(os.time, {year = -1, month = 12, day = 31, hour = 23, min = 59, sec = 59})
            says(os.time, {year = 2026, month = 2^31 - 1, day = 1})
            says(os.time, {year = 2^31 - 1900, month = 1, day = 1})
        """)
        self.assertEqual(lines, [DAYS_RULE, DAYS_RULE, DAYS_RULE, DAYS_RULE, NOT_REPRESENTED, NOT_REPRESENTED, DAYS_RULE,
                                 DAYS_RULE, DAYS_RULE, "ok", "ok", "ok", "ok", NOT_REPRESENTED, NOT_REPRESENTED, NOT_REPRESENTED, NOT_REPRESENTED,
                                 NOT_REPRESENTED])

    def test_os_time_refuses_a_year_and_month_that_cannot_land_in_the_calendar(self):
        # month = 2^31 puts tm_mon at INT_MAX: the `+ 1` that writes the field back overflowed an int (UBSan aborts on it), and
        # a C library was given years of its own to walk. Such a pair is refused before mktime, however far the days could pull
        # it back (3 M of them are 8200 years): the calendar's message. A far year that the days do bring back is taken.
        lines = self.lua_lines("""
            says(os.time, {year = 2026, month = 2^31, day = 1})
            says(os.time, {year = 2026, month = -(2^31) + 1, day = 1})
            says(os.time, {year = 2^31 - 1900, month = 2^31, day = 1})
            says(os.time, {year = -(2^31) + 1900, month = -(2^31) + 1, day = 1})
            says(os.time, {year = 2^31 - 1900, month = 1, day = 1})
            says(os.time, {year = 1000000, month = 1, day = 1})
            says(os.time, {year = 9999, month = 12, day = -2500000})
        """)
        self.assertEqual(lines, [NOT_REPRESENTED] * 6 + ["ok"])

    def test_a_callback_of_big_os_time_calls_is_killed_by_the_budget(self):
        # mktime goes through 2.5 M days in C for each of these calls, 10 ms on the board, and os.time charges the walk: 25,001
        # instructions each (about 100 days to an instruction), so a hundred of them are over a callback's 2 M.
        run = self.handler_app("""
            function app.message()
                for i = 1, 100 do os.time{year = 9999, month = 12, day = -2500000} end
                return true
            end
        """, script=["message walk"], rc=1)
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])

    def test_a_sort_with_os_time_as_the_comparator_is_killed_by_the_budget(self):
        # The same through table.sort(list, os.time): each table is walked once (os.time writes the normal form back), a hundred
        # of them are over the budget, and the sort would otherwise run for a second before it gave up on the order function.
        run = self.handler_app("""
            function app.message()
                local list = {}
                for i = 1, 100 do list[i] = {year = 9999, month = 12, day = -2500000 - i} end
                table.sort(list, os.time)
                return true
            end
        """, script=["message sort"], rc=1)
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])

    def test_os_difftime_and_clock(self):
        lines = self.lua_lines("""
            print(os.difftime(5, 3), os.difftime(3, 5), os.difftime(2.5, 1), math.type(os.difftime(7, 7)))
            says(os.difftime) says(os.difftime, 1) says(os.difftime, "a", 1)
            print(os.clock() == time.mono() / 1000, math.type(os.clock()))
        """)
        self.assertEqual(lines[0], "2.0\t-2.0\t1.5\tfloat")
        self.assertEqual(len(lines), 5)
        for line in lines[1:4]:
            self.assertRegex(line, r"^os\.difftime: both arguments must be numbers, got ")
        self.assertEqual(lines[4], "true\tfloat")


class DutyCycleTest(SvcTestCase):
    """The duty cycle (the brief's rule): the VM's instructions, and the time spent in calls, are summed per 1 s window of the
    monotonic clock; a window over APPS_DUTY_INSTR_PER_S instructions or over 40% of the second in calls is hot; after 3 hot
    windows in a row every timer's next due time moves 100 ms later and a warning is logged once; after 5 the app is killed; a
    cool window resets the count. apphost's clock stands still inside a call, so here the instructions are the measure (the C
    cases of tests/muse_apps_rt_harness.c have calls that take time); what C does without a count hook is charged to them."""

    HOT_TIMER = """
        local app = {}
        function app.start()
            timer.every(100, function() for i = 1, %d do end print("run", time.mono()) end)
        end
        return app
    """

    # Three calls that burn the rate between them, in the second a script then lets go by.
    BURNER = """
        local app = {}
        local work = %d
        function app.message(event, data) if data ~= nil then work = data end for i = 1, work do end return true end
        return app
    """
    HOT = ["message burn"] * 3 + ["advance 1000"]

    def setUp(self):
        self.rate = header_number("apps_rt.h", "APPS_DUTY_INSTR_PER_S")
        self.budget = header_number("apps_vm.h", "APPS_BUDGET_CALL")
        # About 0.3 of the rate in each callback, every 100 ms: three times the rate in a window, and still 1.5 times it
        # once every period is 100 ms longer. Kept under the call budget (the rulings: sized from the constant).
        self.work = min(self.rate * 3 // 10, self.budget * 9 // 10)
        # A third of the rate and a little more, so that three of them in a window are over it.
        self.third = self.rate // 3 + 5000

    def test_the_rule_can_be_tested_at_all(self):
        self.assertGreater(self.work * 5, self.rate, "throttled to 5 runs a second, the app must still be hot")
        self.assertLess(self.work, self.budget)
        self.assertGreater(self.third * 3, self.rate)
        self.assertLess(self.third, self.budget)

    def test_duty_cycle_throttles_then_kills(self):
        run = self.host(self.make_app("scratch", self.HOT_TIMER % self.work),
                        script=["advance 3000", "state", "advance 2000", "state"], rc=1)
        first, second = chunks(run)[:2]
        # By t = 3 s the warning, in the app's warnings and in its log, once; the app still runs.
        self.assertEqual(run.messages("warning"), [THROTTLED])
        self.assertIn({"event": "warning", "message": THROTTLED}, first)
        self.assertEqual([line for line in run.logs() if not line.startswith("run\t")], [THROTTLED])
        self.assertNotIn("crash", [e["event"] for e in first])
        # By t = 5 s the app is killed, and the reason alone is its record.
        self.assertEqual(run.messages("crash"), [KILLED])
        self.assertIn("crash", [e["event"] for e in second])
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "crashed"])

    def test_the_throttle_makes_every_timer_wait_100_ms_more(self):
        run = self.host(self.make_app("scratch", self.HOT_TIMER % self.work), script=["advance 4500"])
        stamps = [int(line.split("\t")[1]) for line in run.logs() if line.startswith("run\t")]
        gaps = [b - a for a, b in zip(stamps, stamps[1:])]
        self.assertEqual(set(gaps[:8]), {100})   # every 100 ms until the third hot window has ended
        self.assertEqual(set(gaps[-4:]), {200})  # and every 200 ms after it
        # The third window ends at 4000 on the clock (it began at 1000), and the run due then is the one that moves: to 4100.
        self.assertIn(3900, stamps)
        self.assertNotIn(4000, stamps)
        self.assertEqual(stamps[stamps.index(3900) + 1:stamps.index(3900) + 4], [4100, 4300, 4500])

    def test_a_cool_window_resets_the_count(self):
        # Hot, hot, cool, hot, hot: no three in a row, so no warning. Hot, hot, hot is one.
        main = f"""
            local app = {{}}
            local work = {self.work}
            function app.start() timer.every(100, function() for i = 1, work do end end) end
            function app.message(event, data) work = data return work end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=[
            "advance 2000", "message work 0", "advance 1000", f"message work {self.work}", "advance 2000", "message work 0",
            "advance 3000", "state"])
        self.assertEqual(run.messages("warning"), [])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "running"}])
        run = self.host(self.make_app("scratch", main), script=["advance 2000", "message work 0", "advance 1000"])
        self.assertEqual(run.messages("warning"), [])
        run = self.host(self.make_app("scratch", main), script=["advance 3000", "message work 0", "advance 1000"])
        self.assertEqual(run.messages("warning"), [THROTTLED])

    def test_a_second_with_no_call_in_it_is_a_cool_window(self):
        # Two hot windows, two seconds without a call, two more: the count began again, and no warning. Three in a row is one.
        app = self.make_app("scratch", self.BURNER % self.third)
        run = self.host(app, script=[*self.HOT, *self.HOT, "advance 2000", *self.HOT, *self.HOT, "advance 1000"])
        self.assertEqual(run.messages("warning"), [])
        run = self.host(app, script=[*self.HOT, *self.HOT, *self.HOT, "advance 1000"])
        self.assertEqual(run.messages("warning"), [THROTTLED])

    def test_a_window_over_the_rate_is_hot_and_one_under_it_is_not(self):
        # Three calls in a window, over the rate between them by 12 000 instructions, or under it by as many (the count hook
        # counts in thousands, and a call has a few instructions of its own).
        def warnings(extra):
            app = self.make_app("scratch", self.BURNER % ((self.rate + extra) // 3))
            return self.host(app, script=[*self.HOT] * 4).messages("warning")

        self.assertEqual(warnings(-12000), [])
        self.assertEqual(warnings(12000), [THROTTLED])

    def test_os_time_calls_make_a_window_hot(self):
        # What C walks is charged as instructions: os.time of 2.5 M days is 25,001, so a callback of a few of them is as heavy as the
        # loop of HOT_TIMER, every 100 ms (the callback of the loop is 0.3 of the rate).
        calls = self.work // 25001 + 1
        main = f"""
            local app = {{}}
            function app.start()
                timer.every(100, function() for i = 1, {calls} do os.time{{year = 9999, month = 12, day = -2500000}} end end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=["advance 3000", "state", "advance 2000", "state"], rc=1)
        self.assertEqual(run.messages("warning"), [THROTTLED])
        self.assertEqual(run.messages("crash"), [KILLED])

    def test_a_late_throttle_warning_follows_the_launch_warnings(self):
        # The runtime's warnings are one text joined with "; ": the one that comes after the launch's is "; throttled: ...",
        # and each is an event of its own, with no empty one between.
        main = f"""
            local app = {{bogus = function() end}}
            function app.start() timer.every(100, function() for i = 1, {self.work} do end end) end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=["advance 3000"])
        self.assertEqual(run.messages("warning"), [unknown_handler("bogus"), THROTTLED])

    def test_every_call_counts_not_only_the_timers(self):
        # Five hot windows made of message handlers alone kill the app, at the end of the fifth.
        run = self.host(self.make_app("scratch", self.BURNER % self.third), script=[*self.HOT] * 6, rc=1)
        self.assertEqual(run.messages("warning"), [THROTTLED])
        self.assertEqual(run.messages("crash"), [KILLED])

    def test_the_throttle_is_warned_once_for_the_life_of_the_app(self):
        # The app can arm a timer of 100 ms, to see whether it is throttled: a throttled app's timers are 100 ms later.
        main = (self.BURNER % self.third).replace(
            "function app.message(event, data)",
            'function app.message(event, data)\n'
            '            if event == "arm" then timer.after(100, function() print("fired") end) return true end')
        cool = ["message cool 0", "advance 1000", f"message hot {self.third}"]
        script = [*self.HOT, *self.HOT, *self.HOT, *cool, *self.HOT, *self.HOT, *self.HOT,
                  "message arm", "advance 100", "state", "advance 100", "state", "log"]
        run = self.host(self.make_app("scratch", main), script=script)
        self.assertEqual(run.messages("warning"), [THROTTLED])  # the second time round, it is not said again
        before, at, ring = chunks(run)[:3]
        # The second throttle did happen: the timer armed after it is not due 100 ms on but 200.
        self.assertNotIn("fired", logs(before))
        self.assertEqual(logs(at), ["fired"])
        # The warning is in the app's log too, once (the ring, which `log` shows), and the echo said it once as it happened.
        self.assertEqual(logs(ring), [THROTTLED, "fired"])
        self.assertEqual(logs(before).count(THROTTLED), 1)
        self.assertEqual(run.of("state"), [{"event": "state", "state": "running"}] * 2)



# ---- The store and sys modules ----

# The handlers most store tests share: put and remove a key, get one, by message.
PUT_HANDLERS = """
    function app.message(event, data)
        if event == "put" then store.set(data.key, data.value) return true end
        if event == "remove" then store.remove(data) return true end
        if event == "get" then return store.get(data.key, data.default) end
    end
"""


def put(key, value):
    return "message put " + json.dumps({"key": key, "value": value})


def saved(run):
    """The sizes of the saves a run printed (apphost's `store_save` events: the bytes of the object that was saved)."""
    return [e["bytes"] for e in run.of("store_save")]


def serialized(values):
    """What a store holding these values saves: one compact JSON object, whose bytes are what the 16 KB counts."""
    return len(json.dumps(values, separators=(",", ":"), ensure_ascii=False).encode())


def line(text):
    return {"event": "log", "app": "scratch", "line": text}


class DeviceTestCase(SvcTestCase):
    """The store and sys modules, on apphost's port: its saves and its show are events, its battery and Wi-Fi are fixed
    (HOST_BATTERY, HOST_WIFI), and the clock is the script's. apphost's port saves at once and keeps what it saved in the
    process, so a `restart` reads it back; the device's goes through the I/O worker (the C cases of the task and port
    harnesses show that)."""

    def device_app(self, handlers, script=None, permissions=(), manifest=None, **kw):
        """An app with the handlers of `handlers` (Lua statements that fill `app`; `bare` and `says` at hand) and the
        permissions given, then the script."""
        main = BARE + "local app = {}\n" + handlers + "\nreturn app\n"
        doc = {"permissions": list(permissions), **(manifest or {})}
        return self.host(self.make_app("scratch", main, manifest=doc), script=script, **kw)

    def device_lines(self, source, permissions=(), script=None, **kw):
        """What start() printed when it ran `source` (Lua statements, `bare` and `says` at hand), with no warning, error or
        crash among the events."""
        main = BARE + "local app = {}\nfunction app.start()\n" + source + "\nend\nreturn app\n"
        app = self.make_app("scratch", main, manifest={"permissions": list(permissions)})
        run = self.host(app, script=script, **kw)
        self.assertEqual([k for k in run.kinds() if k in ("warning", "error", "crash")], [], run.events)
        return run.logs()


class StoreTest(DeviceTestCase):
    """The `store` module (apps_svc.c, over the runtime's cache in apps_rt.c): what an app keeps across a restart, the 16 KB,
    the key rules, and when the values are saved."""

    # ---- The brief's tests ----

    def test_store_roundtrip_across_restart(self):
        mix = {"i": 7, "f": 0.5, "whole": 3.0, "s": "héllo \"q\" \\ \n\ttab", "b": False,
               "list": [1, "two", None, {"x": 1}], "null": None, "nested": {"a": {"b": {"c": [True]}}}}
        run = self.device_app("""
            function app.start() print("start", store.get("runs", 0)) end
            function app.stop() print("stop") end
            function app.message(event, data)
                if event == "put" then store.set(data.key, data.value) return true end
                if event == "get" then return store.get(data.key, data.default) end
                if event == "types" then
                    local v = store.get("mix")
                    return {math.type(v.i), math.type(v.f), math.type(v.whole), type(v.s), type(v.b), type(v.list),
                            v.null == json.null, v.list[3] == json.null}
                end
                if event == "runs" then store.set("runs", store.get("runs", 0) + 1) return store.get("runs") end
            end
        """, permissions=["store"], script=[
            put("mix", mix), "message runs", "message runs", 'message get {"key":"mix"}', "message types",
            "state", "restart", "state",
            'message get {"key":"mix"}', "message types", "message runs", "state",
            "restart", "state", "restart", "state", 'message get {"key":"runs"}'])
        types = ["integer", "float", "float", "string", "boolean", "table", True, True]
        self.assertEqual(replies(run), [True, 1, 2, mix, types, mix, types, 3, 3])
        launch, first, _, second, third, last = chunks(run)
        self.assertEqual([e["event"] for e in launch], ["log", "started", *["reply"] * 5])
        self.assertEqual(logs(launch), ["start\t0"])
        # A restart stops the app (stop() prints), saves what it holds (the object whose bytes the 16 KB counts), and starts the
        # app again from the top: the values it saved are read back, whatever types they had.
        self.assertEqual([e["event"] for e in first], ["log", "store_save", "log", "started"])
        self.assertEqual(first[1], {"event": "store_save", "bytes": serialized({"mix": mix, "runs": 2})})
        self.assertEqual(logs(first), ["stop", "start\t2"])
        self.assertEqual([e["event"] for e in second], ["log", "store_save", "log", "started"])
        self.assertEqual(second[1], {"event": "store_save", "bytes": serialized({"mix": mix, "runs": 3})})
        self.assertEqual(logs(second), ["stop", "start\t3"])
        self.assertEqual([e["event"] for e in third], ["log", "log", "started"])  # nothing changed: nothing to save
        self.assertEqual([e["event"] for e in last], ["reply"])
        self.assertEqual(saved(run), [first[1]["bytes"], second[1]["bytes"]])

    def test_store_limit(self):
        # The 16 KB is the object that is saved, counted after encoding: {"k":"..."} is the value's n bytes and 8 more.
        lines = self.device_lines(r"""
            local function fill(n) return ("x"):rep(n) end
            says(store.set, "k", fill(16376))              -- {"k":"..."} is 16384 bytes
            says(store.set, "k", fill(16377))              -- one more is refused
            print(#store.get("k"))                         -- and the value it had is still there
            says(store.set, "k", "y")                      -- a smaller one replaces it: the bytes it had count for nothing
            says(store.set, "k", fill(16376))
            store.remove("k")
            says(store.set, "a", fill(100))                -- {"a":"...","b":"..."}: the second key's 7 bytes and its comma
            says(store.set, "b", fill(16269))
            says(store.set, "b", fill(16270))
            store.remove("a") store.remove("b")
            says(store.set, "q", ('"'):rep(8188))          -- a quote is 2 bytes once encoded: 8188 of them are 16384
            says(store.set, "q", ('"'):rep(8189))
            store.remove("q")
            says(store.set, "c", ("\1"):rep(2729))         -- a control character is 6: \u0001
            says(store.set, "c", ("\1"):rep(2730))
        """, permissions=["store"])
        self.assertEqual(lines, ["ok", STORE_FULL, "16376", "ok", "ok",
                                 "ok", "ok", STORE_FULL,
                                 "ok", STORE_FULL,
                                 "ok", STORE_FULL])
        # And the largest store that is taken is the largest that is saved: the bytes counted are the bytes of the object.
        run = self.device_app("function app.start() store.set('k', ('x'):rep(16376)) end", permissions=["store"],
                              script=["advance 2000"])
        self.assertEqual(saved(run), [STORE_BYTES])

    def test_store_saves_two_seconds_after_last_write_and_on_stop(self):
        run = self.device_app(PUT_HANDLERS, permissions=["store"], script=[
            put("a", 1),                                    # the first write, at 1000 on apphost's clock
            "advance 1999", "state",                        # not yet: 2999
            "advance 1", "state",                           # 2000 ms after the write: saved
            "advance 60000", "state",                       # and not again: nothing has changed
            put("a", 22), "advance 1500", put("a", 333),    # a write inside the 2 s starts them again
            "advance 1999", "state", "advance 1", "state",
            put("a", 4444), "stop", "state",                # a write in the last 2 s before the stop is saved by it
            "advance 60000", "state"])
        kinds = [[e["event"] for e in c] for c in chunks(run)]
        self.assertEqual(kinds, [["started", "reply"], ["store_save"], [], ["reply", "reply"], ["store_save"],
                                 ["reply", "store_save"], [], []])
        # Each save is of what the app wrote last by then (the sizes tell the values apart).
        self.assertEqual(saved(run), [serialized({"a": 1}), serialized({"a": 333}), serialized({"a": 4444})])
        # What is saved is what the app wrote last: a restart reads it back.
        again = self.device_app(PUT_HANDLERS, permissions=["store"],
                                script=[put("a", 5), "stop", "restart", 'message get {"key":"a"}'])
        self.assertEqual(replies(again), [True, 5])

    def test_a_writer_that_never_pauses_is_saved_ten_seconds_after_its_first_unsaved_write(self):
        # Its 2 s of quiet never come. The save comes 10 s after the first write that isn't saved yet, with what it has written by then, and
        # the next write begins the next 10 s.
        def writes(count, letter):
            return [step for i in range(count) for step in (put("n", letter * (i + 1)), "advance 1000")]

        run = self.device_app(PUT_HANDLERS, permissions=["store"], script=[
            *writes(9, "x"), put("n", "x" * 10),              # 9 writes a second apart, and the tenth: 9 s after the first
            "advance 999", "state",                           # 9.999 s after the first write: not yet
            "advance 1", "state",                             # 10 s: saved
            *writes(9, "y"), put("n", "y" * 10), "advance 999", "state", "advance 1", "state"])
        kinds = [[e["event"] for e in c] for c in chunks(run)]
        # No save in the first chunk, the 9.999 s included; one in the second, at the 10th; none among the next writes; one at their 10th.
        self.assertEqual(kinds, [["started"] + ["reply"] * 10, ["store_save"], ["reply"] * 10, ["store_save"], []])
        self.assertEqual(saved(run), [serialized({"n": "x" * 10}), serialized({"n": "y" * 10})])

    # ---- What the brief's don't cover: saving ----

    def test_a_crash_saves_what_was_written(self):
        run = self.device_app("""
            function app.message(event)
                store.set("a", 1)
                timer.after(100, function() error("boom") end)
                return true
            end
        """, permissions=["store"], script=["message go", "advance 99", "advance 1", "state"], rc=1)
        # The runtime saves as it ends the app, and apphost reports the crash once the call has returned.
        self.assertEqual(run.kinds(), ["started", "reply", "store_save", "crash", "state"])
        self.assertEqual(saved(run), [serialized({"a": 1})])

    def test_a_store_that_did_not_change_saves_nothing(self):
        run = self.device_app(PUT_HANDLERS, permissions=["store"], script=[
            put("a", 1), "advance 2000", put("a", 1), "advance 2000", 'message remove "never"', "advance 2000", "stop",
            "advance 2000"])
        self.assertEqual(saved(run), [serialized({"a": 1})])
        # A write of what is there is no change: it doesn't move the save either. The save is 2 s after the first write, so it is before the
        # state that follows the advance (a save that the second write had moved would come at the end of the run, after it).
        run = self.device_app(PUT_HANDLERS, permissions=["store"],
                              script=[put("a", 1), "advance 1500", put("a", 1), "advance 500", "state"])
        self.assertEqual(saved(run), [serialized({"a": 1})])
        self.assertEqual(run.kinds(), ["started", "reply", "reply", "store_save", "state"])

    def test_a_write_inside_stop_is_carried_by_the_stops_save(self):
        # stop() writes, and the save that follows it has what the app wrote before and what stop() wrote: one save, and nothing later.
        run = self.device_app("""
            function app.start() store.set("s", 1) end
            function app.stop() print("stop") store.set("bye", "yes") end
        """, permissions=["store"], script=["stop", "state", "advance 60000", "state"])
        self.assertEqual(run.kinds(), ["started", "log", "store_save", "state", "state"])
        self.assertEqual(saved(run), [serialized({"s": 1, "bye": "yes"})])

    def test_the_end_of_the_run_saves_what_is_left(self):
        # The runtime that is freed saves a store that still has a change in it (an owner may have forgotten to): the last event.
        run = self.device_app(PUT_HANDLERS, permissions=["store"], script=[put("a", 1)])
        self.assertEqual(run.kinds(), ["started", "reply", "store_save"])

    def test_the_stores_bytes_are_what_is_saved(self):
        # The 16 KB counts what is saved, whatever the keys and values: escapes and all, after replacing and removing.
        values = {"plain": 1, "q\"uote": "a \"b\" \\ c", "tab\there": [1.5, True, None], "café": {"k": "€\U0001f600"},
                  "\x01": "\x1f\x7f", "k" * 32: "z" * 40, "é" * 32: 0}
        script = [put(k, v) for k, v in values.items()]
        script += [put("plain", "replaced and longer"), "message remove " + json.dumps("café"), "advance 2000"]
        run = self.device_app(PUT_HANDLERS, permissions=["store"], script=script)
        want = {k: v for k, v in values.items() if k != "café"}
        want["plain"] = "replaced and longer"
        self.assertEqual(saved(run), [serialized(want)])

    def test_the_store_may_be_used_at_the_top_level_of_main_lua(self):  # the module is there before main.lua runs
        main = "store.set('top', 1)\nreturn {start = function() print('top', store.get('top')) end}"
        run = self.host(self.make_app("scratch", main, manifest={"permissions": ["store"]}), script=["restart"])
        self.assertEqual(run.logs(), ["top\t1", "top\t1"])  # and the second launch read it back
        self.assertEqual(saved(run), [serialized({"top": 1})])

    def test_many_small_keys_are_counted_as_they_are_saved(self):
        pairs = {f"key{i}": i for i in range(1000)}
        run = self.device_app("""
            function app.start() for i = 1, 1000 do store.set("key" .. (i - 1), i - 1) end end
            function app.message(event, data) return {store.get("key999"), store.get("key0"), store.get("key500")} end
        """, permissions=["store"], script=["advance 2000", "message get"])
        self.assertEqual(saved(run), [serialized(pairs)])
        self.assertEqual(replies(run), [[999, 0, 500]])

    def test_launch_stop_cycles_of_a_store_app_leak_nothing(self):
        # Each cycle loads the values the one before saved and saves them again at its end: the cache, the block the port
        # gave and the copies a get returned are all freed by the end of the cycles (apphost counts them, to the byte).
        run = self.host(FIXTURES / "store_check", cycles=25)
        self.assertEqual(run.events, [{"event": "leak_check", "bytes": 0}])
        # The cycles do load what the one before saved: the eleventh launch reads ten, and says so by crashing (which saves too).
        run = self.device_app("""
            function app.start()
                local n = (store.get("n") or 0) + 1
                store.set("n", n)
                if n == 11 then error("the eleventh launch read ten") end
            end
        """, permissions=["store"], cycles=25, rc=1)
        self.assertIn("the eleventh launch read ten", run.events[0]["message"])
        self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})

    def test_cycles_of_an_app_with_many_keys_and_an_unsaved_change_leak_nothing(self):
        # Many keys, a key removed, a value replaced by a longer one, and the change the app ends with: saved by free().
        run = self.device_app("""
            function app.start()
                for i = 1, 200 do store.set("key" .. i, ("v"):rep(i % 40 + 1)) end
                store.remove("key7")
                store.set("key8", {nested = {1, 2, 3}, text = "longer than before"})
            end
        """, permissions=["store"], cycles=25)
        self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})

    # ---- The key rules, and what a value may be ----

    def test_store_keys_are_strings_of_1_to_32_characters(self):
        lines = self.device_lines(r"""
            local long33, key32 = ("k"):rep(33), ("k"):rep(32)
            says(store.set, "", 1) says(store.get, "") says(store.remove, "")
            says(store.set, long33, 1) says(store.get, long33) says(store.remove, long33)
            says(store.set, 5, 1) says(store.get, 5) says(store.remove, 5)
            says(store.set, nil, 1) says(store.get) says(store.remove, {})
            says(store.set, key32, 1) print(store.get(key32))
            says(store.set, ("\u{e9}"):rep(32), 1)          -- 32 characters, 64 bytes
            says(store.set, ("\u{1F600}"):rep(32), 1)       -- 32 characters, 128 bytes
            says(store.set, ("\u{e9}"):rep(33), 1)          -- 33 characters
            says(store.set, "\255", 1) says(store.get, "\255") says(store.set, "a\0b", 1) says(store.remove, "a\0b")
        """, permissions=["store"])
        self.assertEqual(lines, [STORE_KEY_RULE] * 12 + ["ok", "1", "ok", "ok"] + [STORE_KEY_RULE] * 5)

    def test_a_value_that_can_never_fit_is_refused_before_anything_is_made_for_it(self):
        # The reviewer's input: 50 sets of a string of 400 KB in an app of 1 MB. Each is refused with the 16 KB message, as a table that holds
        # it is, and the app lives (the encoder's output, which would have doubled to 512 KB with the 400 KB string held, never exists).
        run = self.device_app(r"""
            function app.start()
                local big = ("x"):rep(400 * 1024)
                local failed, last, others = 0, nil, {}
                for i = 1, 50 do
                    local ok, message = pcall(store.set, "k", big)
                    if not ok then failed = failed + 1 last = bare(message) end
                    ok, message = pcall(store.set, "t", {big})
                    if not ok then others[bare(message)] = true end
                    ok, message = pcall(store.set, "n", {a = {b = big}})
                    if not ok then others[bare(message)] = true end
                end
                local kinds = 0 for _ in pairs(others) do kinds = kinds + 1 end
                print(failed, last, kinds, store.get("k", "none"), store.get("t", "none"), store.get("n", "none"))
            end
        """, permissions=["store"], manifest={"memory_kb": 1024})
        self.assertEqual(run.kinds(), ["log", "started"], run.events)
        self.assertEqual(run.logs(), [f"50\t{STORE_FULL}\t1\tnone\tnone\tnone"])
        # Half the app's memory is enough to end it, in a default app of 256 KB: a string of 128 KB gets the message too.
        run = self.device_app(r"""
            function app.start()
                local big = "x" for i = 1, 17 do big = big .. big end    -- 128 KB
                local ok, message = pcall(store.set, "k", big)
                print(ok, bare(message))
            end
            function app.message() return "alive" end
        """, permissions=["store"], script=["message ping"])
        self.assertEqual(run.logs(), [f"false\t{STORE_FULL}"])
        self.assertEqual(replies(run), ["alive"])

    def test_store_set_says_what_cannot_be_encoded(self):
        run = self.device_app(r"""
            function app.start()
                says(store.set, "f", function() end)
                says(store.set, "t", {f = print})
                says(store.set, "n", 0/0) says(store.set, "i", math.huge)
                says(store.set, "s", "\255")
                says(store.set, "k", {[{}] = 1}) says(store.set, "co", coroutine.create(print))
                local deep = "x" for i = 1, 15 do deep = {deep} end says(store.set, "d15", deep)
                local deeper = {deep} says(store.set, "d16", deeper)      -- 16 levels: what json.encode takes
                says(store.set, "d17", {deeper})                           -- 17: what it doesn't
                print(store.get("f", "none"), store.get("d15") ~= nil, store.get("d16") ~= nil, store.get("d17", "none"))
            end
            function app.message(event)
                local v, n = store.get(event), 0
                while type(v) == "table" do n = n + 1 v = v[1] end
                return n
            end
        """, permissions=["store"], script=["restart", "message d15", "message d16"])
        # The json.encode message after "store.set: " (the bare text, as ruled): a value is as deep as json.encode takes it, 16 levels, and the
        # message of one that isn't is json.encode's own.
        want = ["store.set: can't encode a function",
                "store.set: can't encode a function",
                "store.set: can't encode NaN or infinity", "store.set: can't encode NaN or infinity",
                "store.set: can't encode a string that isn't valid UTF-8",
                "store.set: can't encode a table key", "store.set: can't encode a thread",
                "ok", "ok", "store.set: nested deeper than 16",
                "none\ttrue\ttrue\tnone"]
        self.assertEqual(run.logs(), want * 2)  # and the restart's start() reads the same of what was saved
        self.assertEqual(replies(run), [15, 16])  # the values are read back from the saved object, which holds the second 17 levels down

    def test_get_returns_a_copy_and_the_default_as_it_is(self):
        run = self.device_app("""
            function app.start()
                local t = {x = 1, list = {1, 2}}
                store.set("t", t)
                t.x = 5 t.list[1] = 5          -- the app's own table, changed after the set: the store kept what it was given
            end
            function app.message(event)
                if event == "mutate" then
                    local t = store.get("t")
                    t.x = 99 t.list[1] = 99 t.list[#t.list + 1] = 5 t.extra = true
                    return {store.get("t"), t}
                end
                if event == "identity" then
                    local d = {}
                    return {store.get("t") ~= store.get("t"), store.get("nope", d) == d, store.get("nope") == nil,
                            store.get("nope", false) == false}
                end
            end
        """, permissions=["store"], script=["message mutate", "message identity"])
        (stored, mutated), identity = replies(run)
        self.assertEqual(stored, {"x": 1, "list": [1, 2]})
        self.assertEqual(mutated, {"x": 99, "list": [99, 2, 5], "extra": True})
        self.assertEqual(identity, [True, True, True, True])

    def test_remove_and_set_nil_delete_and_a_missing_key_is_no_error(self):
        lines = self.device_lines("""
            store.set("a", 1) store.set("b", 2)
            print(store.get("a"), store.get("b"))
            store.remove("a")
            print(store.get("a", "gone"), store.get("b"))
            store.remove("a") store.remove("never")                       -- nothing to remove: not an error
            store.set("b", nil)                                           -- the Lua way to delete
            print(store.get("b", "gone"), select("#", store.remove("b")), select("#", store.set("c", 1)))
            store.set("c", json.null)                                     -- a null, on purpose: it is there
            print(store.get("c") == json.null, store.get("c", "default") == json.null)
        """, permissions=["store"])
        self.assertEqual(lines, ["1\t2", "gone\t2", "gone\t0\t0", "true\ttrue"])

    def test_the_store_is_a_plain_value_store(self):  # X17: nothing for getmetatable to open
        lines = self.device_lines("""
            store.set("t", {1, 2})
            print(type(getmetatable(store.get("t"))), type(getmetatable(store)))
        """, permissions=["store"])
        self.assertEqual(lines, ["nil\tnil"])


class SysTest(DeviceTestCase):
    """The `sys` module (apps_svc.c) on apphost's port, and the commands that make it testable (`restart`, a `show` event)."""

    def test_sys_show_rules(self):
        main = BARE + """
            local app = {}
            local function try(label)
                local ok, message = pcall(sys.show)
                print(label, ok and "ok" or bare(message))
            end
            function app.start()
                try("start")
                local go = ui.button{text = "Go"}
                go:on("tap", function() print("tapped") end)
            end
            function app.message(event, data)
                if event == "show" then sys.show() print("message") return true end
                if event == "arm" then
                    for _, ms in ipairs(data) do timer.after(ms, function() try("after " .. ms) end) end
                    return true
                end
                if event == "alarm" then alarm.after("t", 500, {sound = false}) return true end
            end
            function app.alarm(name) sys.show() print("alarm " .. name) end
            return app
        """
        app = self.make_app("scratch", main, manifest={"permissions": ["alarm"]})
        run = self.host(app, script=[
            "message show",                                         # message(): allowed
            "message arm [100]", "advance 100",                     # a timer, and no tap yet: refused (the clock is at 1100)
            "message alarm", "advance 500",                         # alarm(): allowed
            'tap "Go"', "message arm [9990, 10000, 10001, 10040]",  # a tap opens a window of 10 s, to the millisecond included
            "advance 10100",
            'tap "Go"', "message arm [9000]", "advance 9000"])      # and a tap later opens it again
        show = {"event": "show"}
        reply = {"event": "reply", "data": True}
        self.assertEqual(run.events, [
            line(f"start\t{SHOW_RULE}"), {"event": "started", "id": "scratch", "name": "scratch"},   # start() is no place for it
            show, line("message"), reply,
            reply, line(f"after 100\t{SHOW_RULE}"),
            reply, {"event": "alarm", "name": "t", "sound": False, "wake": True}, show, line("alarm t"),
            line("tapped"), reply,
            show, line("after 9990\tok"), show, line("after 10000\tok"),
            line(f"after 10001\t{SHOW_RULE}"), line(f"after 10040\t{SHOW_RULE}"),  # 10001 ms on: the next step of 10 ms is past it
            line("tapped"), reply, show, line("after 9000\tok"),
        ])

    def test_sys_has_api_mem(self):
        names = ["ui", "time", "timer", "alarm", "store", "sound", "json", "sys", "net", "secrets", "muse", "log", "os", "string",
                 "math", "bogus", ""]
        run = self.device_app("""
            function app.message(event, data)
                local out = {}
                for i, name in ipairs(data) do out[i] = sys.has(name) end
                local before = sys.mem()
                local big = ("x"):rep(50 * 1024)
                local after = sys.mem()
                big = nil
                local later = sys.mem()
                return {has = out, api = sys.api, api_type = math.type(sys.api), before = before, after = after, later = later}
            end
        """, manifest={"memory_kb": 256}, script=["message names " + json.dumps(names)])
        reply = replies(run)[0]
        self.assertEqual(reply["has"], [True] * 8 + [False] * 9)  # ui time timer alarm store sound json sys, and nothing else
        self.assertEqual((reply["api"], reply["api_type"]), (1, "integer"))
        before, after, later = reply["before"], reply["after"], reply["later"]
        for mem in (before, after, later):
            self.assertEqual(sorted(mem), ["limit_kb", "peak_kb", "used_kb"])
            self.assertEqual(mem["limit_kb"], 256)  # the manifest's memory_kb
            self.assertTrue(1 <= mem["used_kb"] <= mem["peak_kb"] <= 256, mem)
        self.assertGreaterEqual(after["used_kb"], before["used_kb"] + 40)  # a string of 50 KB (less what a collection took back)
        self.assertGreaterEqual(later["peak_kb"], after["peak_kb"])        # the peak never goes down
        lines = self.device_lines("""
            says(sys.has) says(sys.has, nil) says(sys.has, 5) says(sys.has, {}) says(sys.has, true)
        """)
        self.assertEqual(lines, [f"sys.has: name must be a string, got {kind}" for kind in
                                 ("no value", "nil", "number", "table", "boolean")])

    def test_sys_exit_stops_after_callback(self):
        run = self.device_app("""
            function app.start() print("start") end
            function app.stop() print("stop") end
            function app.message(event)
                if event == "exit" then
                    print("before")
                    store.set("k", 1)
                    sys.exit()
                    print("after")        -- the callback runs to its end
                    return "bye"          -- and its reply is given
                end
                return "alive"
            end
        """, permissions=["store"], script=["message exit", "state", "message again", "start", "state"])
        started = {"event": "started", "id": "scratch", "name": "scratch"}
        self.assertEqual(run.events, [
            line("start"), started,
            line("before"), line("after"), {"event": "reply", "data": "bye"},
            line("stop"), {"event": "store_save", "bytes": serialized({"k": 1})},   # then the app ends: stop(), and the store
            {"event": "state", "state": "stopped"},
            {"event": "error", "message": "the app is not running"},
            line("start"), started, {"event": "state", "state": "running"},
        ])

    def test_sys_exit_in_start_and_in_a_timer(self):
        run = self.device_app("""
            function app.start() print("start") sys.exit() print("start ends") end
            function app.stop() print("stop") end
        """, script=["state", "timers"])
        # start() runs to its end and has started the app: then the app ends, stop() included.
        self.assertEqual(run.events, [line("start"), line("start ends"), {"event": "started", "id": "scratch", "name": "scratch"},
                                      line("stop"), {"event": "state", "state": "stopped"}, {"event": "timers", "count": 0}])
        run = self.device_app("""
            function app.start()
                timer.after(100, function() print("first") sys.exit() print("first ends") end)
                timer.after(100, function() print("second") end)     -- due with the first: it never runs
                timer.every(50, function() print("every") end)
            end
            function app.stop() print("stop") end
        """, script=["advance 500", "state", "timers"])
        self.assertEqual(run.logs(), ["every", "first", "first ends", "stop"])  # every at 50, then the two that are due at 100
        self.assertEqual(run.of("state"), [{"event": "state", "state": "stopped"}])
        self.assertEqual(run.of("timers"), [{"event": "timers", "count": 0}])

    def test_sys_exit_in_alarm_runs_no_later_timer_of_the_same_advance(self):
        run = self.device_app("""
            function app.start()
                alarm.after("t", 500, {sound = false})
                timer.after(600, function() print("timer 600") end)       -- due inside the same advance, after the alarm
                timer.after(700, function() print("timer 700") end)
            end
            function app.alarm(name) print("alarm " .. name) sys.exit() print("alarm ends") end
            function app.stop() print("stop") end
        """, permissions=["alarm"], script=["advance 1000", "state", "timers"])
        # The callback runs to its end and the app ends after it, stop() included: neither timer runs.
        self.assertEqual(run.logs(), ["alarm t", "alarm ends", "stop"])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "stopped"}])
        self.assertEqual(run.of("timers"), [{"event": "timers", "count": 0}])

    def test_sys_exit_at_the_top_level_of_main_lua_means_the_app_is_not_started(self):
        main = "print('top') sys.exit() print('top ends') return {start = function() print('start') end}"
        run = self.host(self.make_app("scratch", main), script=["state"])
        self.assertEqual(run.logs(), ["top", "top ends"])
        self.assertEqual(run.kinds(), ["log", "log", "state"])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "stopped"}])

    def test_permissions(self):
        lines = self.device_lines("""
            says(store.get, 5) says(store.set, 5, 1) says(store.remove, 5)
            says(store.get, "k") says(store.set, "k", function() end) says(store.remove, "k")
        """, permissions=[])
        # The permission is the first thing checked: before the key and before the value.
        self.assertEqual(lines, [STORE_PERMISSION] * 6)
        calls = """
            says(sys.keep_awake, true) says(sys.keep_awake, false) says(sys.keep_awake, "x") says(sys.keep_awake)
        """
        self.assertEqual(self.device_lines(calls, permissions=["store", "alarm", "sound"]), [KEEP_AWAKE_PERMISSION] * 4)
        self.assertEqual(self.device_lines(calls, permissions=["keep_awake"]), [
            "ok", "ok", "sys.keep_awake: on must be true or false, got string",
            "sys.keep_awake: on must be true or false, got no value"])
        # The rest of sys needs none.
        lines = self.device_lines("""
            says(sys.battery) says(sys.wifi) says(sys.has, "net") says(sys.mem) says(sys.log, "x")
        """, permissions=[])
        self.assertEqual(lines, ["ok", "ok", "ok", "ok", "x", "ok"])  # sys.log prints "x" itself, then says prints its "ok"

    def test_sys_battery_wifi_log(self):  # C33
        run = self.device_app("""
            function app.start()
                sys.log("a", 1)
                print("p", 2)
                sys.log()
                sys.log("x\\ny", nil, true, {} == {})
            end
            function app.message()
                local a, b, w = sys.battery(), sys.battery(), sys.wifi()
                return {battery = a, wifi = w, fresh = a ~= b, plain = getmetatable(a) == nil and getmetatable(w) == nil,
                        pct_type = math.type(a.pct)}
            end
        """, script=["message report", "log"])
        # sys.log is print: the same lines, in the same ring, in order with print's.
        lines = ["a\t1", "p\t2", "", "x", "y\tnil\ttrue\tfalse"]
        self.assertEqual(run.logs(), lines + lines)  # as printed, and as the log command shows the ring
        self.assertEqual(replies(run), [{"battery": HOST_BATTERY, "wifi": HOST_WIFI, "fresh": True, "plain": True,
                                         "pct_type": "integer"}])

    def test_sys_log_and_print_make_the_same_lines(self):
        lines = self.device_lines(r"""
            local words = {"plain", "tab\there", "bell\7bell", "nl\nnl", "\255bad", ("w"):rep(250), "\u{85}c1", ""}
            for _, w in ipairs(words) do print("p", w) end
            print(1, 2.5, true, nil, {} ~= nil)
            print("--")
            for _, w in ipairs(words) do sys.log("p", w) end
            sys.log(1, 2.5, true, nil, {} ~= nil)
        """)
        cut = lines.index("--")
        self.assertGreater(cut, 8)
        self.assertEqual(lines[:cut], lines[cut + 1:])

    def test_there_is_no_log_global(self):  # the rulings: logging is print and sys.log
        lines = self.device_lines("""
            local ok, message = pcall(function() return log end)
            print(ok, bare(message):find("unknown global 'log'", 1, true) ~= nil)
            print(sys.has("log"), type(sys.log), type(print))
        """)
        self.assertEqual(lines, ["false\ttrue", "false\tfunction\tfunction"])

    def test_keep_awake_with_its_permission_is_accepted(self):
        # apphost's port only records it (nothing on a laptop sleeps); the C cases of the runtime harness count the calls.
        lines = self.device_lines("sys.keep_awake(true) sys.keep_awake(false) print('fine')", permissions=["keep_awake"])
        self.assertEqual(lines, ["fine"])

    def test_the_keep_awake_board_fixture_asks_to_stay_lit_and_takes_the_request_back(self):
        # tests/muse_apps_fixtures/keep_awake is Task 19's board check: in front, with the permission, it asks for the screen to
        # stay lit, and the message "off" takes that back, so the same app is its own control. apphost only records the calls.
        run = self.host(FIXTURES / "keep_awake", script=["message off", "message on", "message nope"])
        self.assertEqual(run.logs(), ["keep_awake on", "keep_awake off", "keep_awake on"])
        self.assertEqual(replies(run), [{"keep_awake": False}, {"keep_awake": True}])
        (error,) = run.messages("error")
        self.assertIn("use on or off", error)
        self.assertEqual(run.of("started"), [{"event": "started", "id": "keep_awake", "name": "Keep awake"}])
        self.assertEqual(run.of("warning"), [])

    def test_restart_stops_frees_and_starts_a_new_runtime(self):
        run = self.handler_app("""
            local runs = 0
            print("top")
            function app.start() runs = runs + 1 print("start", runs) end
            function app.stop() print("stop") end
        """, script=["restart", "state", "restart", "stop", "restart", "state", "restart extra"])
        # Each restart is stop() (if the app was running), a new runtime (main.lua's top level runs again, its locals are new),
        # then start(): `runs` is 1 every time. After a stop() there is nothing to stop.
        self.assertEqual(run.logs(), ["top", "start\t1",
                                      "stop", "top", "start\t1",        # restart
                                      "stop", "top", "start\t1",        # restart
                                      "stop",                           # stop
                                      "top", "start\t1"])               # restart of a stopped app
        self.assertEqual(run.kinds().count("started"), 4)
        self.assertEqual(run.messages("error"), ["restart takes no arguments"])
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "running"])

    def test_restart_after_a_crash_makes_a_fresh_app(self):
        run = self.handler_app("""
            function app.start() print("start") end
            function app.message(event)
                if event == "boom" then timer.after(50, function() error("boom") end) return true end
                return "fine"
            end
        """, script=["message boom", "advance 50", "state", "restart", "state", "message ok"])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "crashed"}, {"event": "state", "state": "running"}])
        self.assertEqual(run.logs(), ["start", "start"])
        self.assertEqual(replies(run), [True, "fine"])
        self.assertEqual(len(run.of("crash")), 1)

    def test_restart_tells_the_alarms_of_an_app_that_stopped(self):
        # As the stop command does: a ring only this app could stop ends with it, there and then, and not at its minute's end.
        main = BARE + """
            local app = {}
            function app.start() if time.mono() < 1500 then alarm.after("t", 1000) end end
            return app
        """
        run = self.host(self.make_app("scratch", main, manifest={"permissions": ["alarm"]}),
                        script=["advance 1000", "restart", "advance 100000"])
        self.assertEqual(run.kinds(), ["started", "alarm", "ring", "ring", "started"])
        self.assertEqual(run.of("ring"), [{"event": "ring", "on": True}, {"event": "ring", "on": False}])


class SvcStaticTest(unittest.TestCase):
    def test_the_binding_tables_are_named_as_the_guide_test_reads_them(self):  # X8
        source = (APPS / "apps_svc.c").read_text()
        names = {"apps_time_funcs": ["now", "mono", "synced", "date", "format"],
                 "apps_os_funcs": ["time", "date", "clock", "difftime"],
                 "apps_timer_funcs": ["after", "every", "cancel"],
                 "apps_timer_methods": ["cancel"],
                 "apps_alarm_funcs": ["after", "cancel", "remaining", "list"],
                 "apps_sound_funcs": ["chirp", "stop"],
                 "apps_store_funcs": ["get", "set", "remove"],
                 "apps_sys_funcs": ["battery", "wifi", "show", "keep_awake", "has", "mem", "exit", "log"]}
        for table, functions in names.items():
            with self.subTest(table):
                body = re.search(rf"static const luaL_Reg {table}\[\] = \{{(.*?)\n\}};", source, re.S)
                self.assertIsNotNone(body, f"no `static const luaL_Reg {table}[]`")
                self.assertEqual(re.findall(r'\{"(\w+)",', body.group(1)), functions)

    def test_there_is_no_log_module(self):  # the rulings: logging is print and sys.log, so no apps_log_funcs and no `log` global
        source = (APPS / "apps_svc.c").read_text()
        self.assertNotIn("apps_log_funcs", source)
        self.assertNotRegex(source, r'lua_setglobal\(L, "log"\)')
        self.assertRegex(source, r'\{"log", sys_log\}')

    def test_the_handle_metatable_is_protected(self):  # X17
        source = (APPS / "apps_svc.c").read_text()
        match = re.search(r"luaL_newmetatable\(L, (\w+)\);(.*?)lua_setfield\(L, -2, \"__metatable\"\)", source, re.S)
        self.assertIsNotNone(match, "the timer handle's metatable has no __metatable")
        self.assertLess(match.group(2).count("\n"), 6)

    def test_alarm_and_sound_hand_out_no_handle(self):  # X17
        # The timer's handle is the one userdata with a metatable (protected, above); an alarm is the system's, named by a string,
        # and alarm.list() gives plain tables. A later handle needs its own __metatable, and this test to be told of it.
        source = (APPS / "apps_svc.c").read_text()
        self.assertEqual(source.count("luaL_newmetatable"), 1)
        for name in ("alarm_after", "alarm_cancel", "alarm_remaining", "alarm_list", "sound_chirp", "sound_stop"):
            body = re.search(rf"^static int {name}\(lua_State \*L\) \{{\n(.*?)^\}}", source, re.S | re.M).group(1)
            self.assertNotIn("lua_newuserdata", body, name)
            self.assertNotIn("lua_setmetatable", body, name)

    def test_arm_gives_the_reference_back_when_the_runtime_has_no_slot(self):
        # Unreachable today (the count is checked just before and no app code runs between), so no script gets there: the
        # source is read. An unchecked -1 would be a handle to slot 65535, a leaked reference and a timer that never was.
        source = (APPS / "apps_svc.c").read_text()
        body = re.search(r"^static int arm\(lua_State \*L, bool every\) \{\n(.*?)^\}", source, re.S | re.M).group(1)
        added = body.index("apps_rt_timer_add(")
        check = re.search(r"if \(slot < 0\) \{[^}]*luaL_unref\(L, LUA_REGISTRYINDEX, ref\);[^}]*luaL_error[^}]*\}", body[added:], re.S)
        self.assertIsNotNone(check, "arm doesn't check the slot apps_rt_timer_add gave, and give the reference back")
        self.assertLess(added + check.start(), body.index("h->slot ="), "the handle is written before the check")

    def test_apphost_runs_the_gadgets_alarm_table_and_not_a_copy_of_it(self):  # E
        sources = (ROOT / "tools/muse/apphost/sources.txt").read_text()
        self.assertIn("components/muse/apps/apps_alarm.c", sources)
        self.assertIn("tools/muse/apphost/apphost_esp.c", sources)
        port = (ROOT / "tools/muse/apphost/apphost_port.c").read_text()
        for copy in ("alarm_t", "name_valid", "find_alarm", "alarm_order", "runs_first"):
            self.assertNotIn(copy, port, f"apphost_port.c still has its own {copy}")
        arm = re.search(r"^apps_alarm_result_t apps_port_alarm_arm\(.*?\{\n(.*?)^\}", port, re.S | re.M)
        self.assertIsNotNone(arm, "apphost_port.c has no apps_port_alarm_arm")
        self.assertIn("apps_alarm_arm(", arm.group(1))
        self.assertLess(arm.group(1).count("\n"), 4)

    def test_the_alarm_name_rule_is_written_once(self):  # E: apps_alarm.h, which apps_svc.c and apps_alarm.c use
        header = (APPS / "apps_alarm.h").read_text()
        self.assertEqual(len(re.findall(r"static inline bool apps_alarm_name_valid\(", header)), 1)
        self.assertEqual(len(re.findall(r"'a' && \w+\[\w+\] <= 'z'", header)), 1)
        for name in ("apps_svc.c", "apps_alarm.c"):
            source = (APPS / name).read_text()
            self.assertIn("apps_alarm_name_valid(", source, name)
            self.assertNotRegex(source, r"name\[\w+\] <= 'z'", f"{name} has a character test of the name's own")

    def test_every_binding_calls_apps_vm_check_first(self):
        source = (APPS / "apps_svc.c").read_text()
        functions = re.findall(r"^static int (\w+)\(lua_State \*L\) \{\n(.*?)^\}", source, re.S | re.M)
        bindings = [(name, body) for name, body in functions if re.search(rf'\{{"\w+", {name}\}}', source)]
        self.assertGreaterEqual(len(bindings), 25)
        for name, body in bindings:
            with self.subTest(name):
                self.assertTrue(body.lstrip().startswith("apps_vm_check(L);"), f"{name} doesn't start with apps_vm_check(L)")


class SvcHarnessTest(unittest.TestCase):
    """The C cases of tests/muse_apps_rt_harness.c for these modules (its one build is shared with HarnessTest)."""

    @classmethod
    def setUpClass(cls):
        cls.harness = rt_harness()

    def run_case(self, case, timeout=120):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    def test_conversions_in_the_boards_time_zone(self):
        # PST8PDT on both sides of each change of the clock, `!` for UTC, mktime in the zone, the hour that happens twice with
        # isdst true and false, and the normalized fields.
        self.run_case("time_zone")

    def test_the_services_survive_a_heap_that_refuses_at_every_step(self):
        self.run_case("services_refused")

    def test_the_timers_at_the_runtimes_own_api(self):
        # Which timer a step runs and when it is due again, what a dark screen, a stop, a crash and a kill from outside leave.
        self.run_case("timer_api")

    def test_of_two_timers_due_together_the_one_armed_first_runs_first(self):
        # Not the one in the lower slot: a timer armed after a cancel takes the slot of the first.
        self.run_case("timer_tie")

    def test_a_full_gadget_says_so_and_a_name_that_is_armed_needs_no_slot(self):  # C31
        # The app's limit (4) and the gadget's (8) are different messages; the other apps' alarms fill the table.
        self.run_case("alarm_table_full")

    def test_a_gadget_short_of_memory_says_so_and_a_try_again_can_work(self):  # G: not "no free alarm slots"
        self.run_case("alarm_no_memory")

    def test_os_time_charges_the_days_it_walks(self):
        # 100 days to an instruction and one more, for each of day, hour, min and sec; a plain table costs one; a year and month
        # that cannot land in the calendar are refused before anything is charged (the host's C library would refuse them too, so
        # that only shows in what is counted).
        self.run_case("os_time_charge")

    # The store and sys (Task 18), where a script can't reach: the runtime's own API, an exact clock, a heap that refuses, the port's answers.

    def test_the_store_at_the_runtimes_own_api(self):
        # Set, get and remove; the key rules (characters of valid UTF-8, no NUL); the 16 KB to the byte on both sides of it; the object
        # that is saved, key escapes, order and length as the port is given them.
        self.run_case("store_api")

    def test_when_the_store_is_saved(self):
        # 2 s after the last change (a change inside the wait starts it again; one that changes nothing doesn't) and never later than 10 s
        # after the first change nobody has saved, writes going on or not, dark or lit, never as a call; 2 s after a save the port refused;
        # and when the app ends (stop, a crash, a crash in main.lua, free), asked once.
        self.run_case("store_flush")

    def test_what_a_load_makes_of_what_the_port_gives(self):
        # The keys, the last of a key that is there twice, none of one the store doesn't write; an empty store and a line in the log for
        # an object it didn't write; no app at all when the port can't give it; no question for an app without the permission.
        self.run_case("store_load")

    def test_the_store_survives_a_heap_that_refuses_at_every_step(self):
        # No app, never an empty store in the real one's place, for a refusal in the load; the app out of memory or a call that failed
        # alone for one in the app; and every object that is saved is the real one with the changes. Nothing held at the end of each.
        self.run_case("store_refused")

    def test_a_value_that_can_never_fit_asks_the_port_for_nothing(self):  # E
        # 400 KB in an app of 1 MB, as a string and in a table: the port holds no more than the app itself does, and the reply is the 16 KB message.
        self.run_case("store_set_too_large")

    def test_the_walk_over_the_stores_keys_is_charged_to_the_call(self):
        # get, set and remove each cost an instruction for every 32 keys, which no count hook sees C do.
        self.run_case("store_key_walk")

    def test_the_caches_worst_case_is_what_apps_rt_h_says(self):
        # The most keys 16 KB holds (one-character keys with one-byte values), what the port's heap holds for them: blocks of 8 bytes, the key and
        # the text, and a table of pointers that doubles from 8 slots.
        self.run_case("store_cache_bound")

    def test_a_save_checks_each_entry_against_the_room_before_writing_it(self):
        self.run_case("store_books_drift")  # true books: the save is made
        # A build of the runtime whose store counts a byte too few for each entry: the save finds out before it writes an entry that doesn't
        # fit, and refuses, where the writes would have run past the block (the sanitizer aborts the harness).
        source = (APPS / "apps_rt.c").read_text()
        drifted = source.replace("size_t sum = rt->store_sum + esc + 3 + len;", "size_t sum = rt->store_sum + esc + 2 + len;", 1)
        self.assertNotEqual(drifted, source)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        copy = Path(tmp.name) / "apps_rt.c"
        copy.write_text(drifted)
        sources = [copy if path.name == "apps_rt.c" else path for path in HARNESS_SOURCES]
        exe = apps_host.build("muse_apps_rt_drift", [*sources, *apps_host.lua_sources()], tmp.name, include_dirs=INCLUDES)
        done = apps_host.run(exe, "store_books_drift", env={"STORE_BOOKS_DRIFTED": "1"}, timeout=120)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_sys_at_the_runtime_with_the_ports_answers(self):
        # A battery with no percentage is -1, a number like every pct, and one past 100 is 100; a Wi-Fi name is clean text, "" with no
        # network, 32 bytes at most; keep_awake, show and exit go to the port; the exit lets the callback run to its end; mem.
        self.run_case("sys_services")

    def test_sys_show_at_the_runtimes_clock(self):
        # Allowed in message() (a coroutine of it too) and alarm(), refused in start(); a touch opens 10 s, the 10th second included
        # and not a millisecond past it, whatever the touch found; never before a touch, the clock a long way from 0.
        self.run_case("show_window")

    # The duty cycle's windows, which apphost's 10 ms steps close one at a time and so can't show: here the clock jumps, the
    # calls come when the case says, and they take the time the case says (apphost's clock stands still inside a call).

    def test_a_hot_window_closed_together_with_a_silent_one_still_counts(self):
        self.run_case("duty_jump")

    def test_a_hot_window_closed_with_a_silent_one_after_it_is_the_kill_when_it_is_the_fifth(self):
        self.run_case("duty_late_kill")

    def test_the_loops_pass_closes_a_window_with_no_call_or_timer_in_it(self):
        self.run_case("duty_pass")

    def test_a_call_closes_the_windows_that_ended_before_it_and_gets_the_kill(self):
        self.run_case("duty_calls")

    def test_a_throttled_app_arms_its_timers_late_and_a_cool_window_ends_the_throttle(self):
        self.run_case("duty_throttle")

    def test_a_call_that_runs_into_the_next_window_is_booked_to_both(self):
        # The review's trace: 0.76 s calls every 1.25 s were never killed, for the windows no call began in read as cool.
        self.run_case("duty_trace")

    def test_a_calls_instructions_are_split_over_the_windows_it_spanned_by_time(self):
        self.run_case("duty_split")

    def test_c_work_that_costs_no_instructions_is_hot_by_the_time_it_takes(self):
        # 400 ms of each second is the limit (cool), 401 is over it; a light app is never hot.
        self.run_case("duty_busy")

    def test_the_status_share_is_what_the_calls_took_of_the_last_window(self):
        # The same booking as the duty cycle's: a call that runs into the next window is in both, an idle window is 0.
        self.run_case("duty_cpu_pct")

    def test_a_throttled_timer_waits_from_the_end_of_its_callback(self):
        # The throttle's 100 ms are idle time, also for a callback that outlasts its period and 100 ms; unthrottled it is a period.
        self.run_case("duty_pace")

    def test_a_throttled_callback_that_swaps_its_timer_for_another_moves_nothing_of_the_new_one(self):
        # The end of a call moves the timer that ran, if it is still there: not the one that took its slot.
        self.run_case("duty_swap")

    def test_a_window_of_exactly_the_rate_is_cool_and_one_instruction_more_is_hot(self):
        # Four windows at the rate are no throttle, three over it are: the rate is not "over the rate".
        self.run_case("duty_rate")


if __name__ == "__main__":
    unittest.main()
