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

"""The `time`, `os` and `timer` modules of Muse apps (apps_svc.c), the timers and the CPU duty cycle of the runtime
(apps_rt.c), and the commands of apphost that move time (advance, dark, sync, timers).

Most tests run apphost on an app folder, a fixture (tests/muse_apps_fixtures/timers, time_label) or one made on the
spot, with a script on stdin, and read the one-JSON-object-a-line events it prints. Time is fake there and moves only
when the script says so (`advance`), in steps of 10 ms with a pass of the timers after each, as the app task's loop
does; `sync` sets the wall clock, which then moves with it; the time zone is UTC0. The C cases of
tests/muse_apps_rt_harness.c check what a script can't: the conversions in a zone that isn't UTC (the board's), a heap
that refuses at every step, and the duty cycle's windows, which a clock that jumps and calls that come when the case says
show and apphost's 10 ms steps can't (each window is closed as it ends there).
"""

import random
import re
import subprocess
import unittest
from datetime import datetime, timezone
from pathlib import Path

import apps_host
from test_muse_apps_rt import BUDGET_CALL, AppHostTestCase, rt_harness, unknown_handler

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


class SvcStaticTest(unittest.TestCase):
    def test_the_binding_tables_are_named_as_the_guide_test_reads_them(self):  # X8
        source = (APPS / "apps_svc.c").read_text()
        names = {"apps_time_funcs": ["now", "mono", "synced", "date", "format"],
                 "apps_os_funcs": ["time", "date", "clock", "difftime"],
                 "apps_timer_funcs": ["after", "every", "cancel"],
                 "apps_timer_methods": ["cancel"]}
        for table, functions in names.items():
            with self.subTest(table):
                body = re.search(rf"static const luaL_Reg {table}\[\] = \{{(.*?)\n\}};", source, re.S)
                self.assertIsNotNone(body, f"no `static const luaL_Reg {table}[]`")
                self.assertEqual(re.findall(r'\{"(\w+)",', body.group(1)), functions)

    def test_the_handle_metatable_is_protected(self):  # X17
        source = (APPS / "apps_svc.c").read_text()
        match = re.search(r"luaL_newmetatable\(L, (\w+)\);(.*?)lua_setfield\(L, -2, \"__metatable\"\)", source, re.S)
        self.assertIsNotNone(match, "the timer handle's metatable has no __metatable")
        self.assertLess(match.group(2).count("\n"), 6)

    def test_arm_gives_the_reference_back_when_the_runtime_has_no_slot(self):
        # Unreachable today (the count is checked just before and no app code runs between), so no script gets there: the
        # source is read. An unchecked -1 would be a handle to slot 65535, a leaked reference and a timer that never was.
        source = (APPS / "apps_svc.c").read_text()
        body = re.search(r"^static int arm\(lua_State \*L, bool every\) \{\n(.*?)^\}", source, re.S | re.M).group(1)
        added = body.index("apps_rt_timer_add(")
        check = re.search(r"if \(slot < 0\) \{[^}]*luaL_unref\(L, LUA_REGISTRYINDEX, ref\);[^}]*luaL_error[^}]*\}", body[added:], re.S)
        self.assertIsNotNone(check, "arm doesn't check the slot apps_rt_timer_add gave, and give the reference back")
        self.assertLess(added + check.start(), body.index("h->slot ="), "the handle is written before the check")

    def test_every_binding_calls_apps_vm_check_first(self):
        source = (APPS / "apps_svc.c").read_text()
        functions = re.findall(r"^static int (\w+)\(lua_State \*L\) \{\n(.*?)^\}", source, re.S | re.M)
        bindings = [(name, body) for name, body in functions if re.search(rf'\{{"\w+", {name}\}}', source)]
        self.assertGreaterEqual(len(bindings), 12)
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

    def test_os_time_charges_the_days_it_walks(self):
        # 100 days to an instruction and one more, for each of day, hour, min and sec; a plain table costs one; a year and month
        # that cannot land in the calendar are refused before anything is charged (the host's C library would refuse them too, so
        # that only shows in what is counted).
        self.run_case("os_time_charge")

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
