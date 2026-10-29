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

"""The countdown timer, the first Muse app (components/muse/apps/examples/timer): its screen, its buttons, its messages
and what it keeps across a restart, on apphost with the real runtime, alarm table and store of the gadget.

Every test runs apphost on the app folder with a script. Time is fake there and moves only when the script says so
(`advance`); the alarm is the system's, so it fires on that clock and an app that was stopped is launched again by it, as
the gadget does. `dump` shows the screen as declared (nothing is laid out on the host), so what is read is the text of the
labels and buttons and which of them are hidden. A firing comes out as an `alarm` event, the ring of the board's chirps as
`ring` events and `sys.show()` as `show`. The saves of the store (`store_save`) come at times the debounce picks and are
left out of the sequences compared here.

What only a board can show (the round screen, the snapshots, the alarm from sleep) is Task 19's gate, not these tests.
"""

import json
import re
import unittest
from pathlib import Path

from test_muse_apps_rt import AppHostTestCase

ROOT = Path(__file__).resolve().parents[1]
TIMER = ROOT / "components/muse/apps/examples/timer"
COPYRIGHT = (ROOT / "components/muse/muse_clock.h").read_text().splitlines()[1].lstrip("* ").strip()
LUA_HEADER = [f"-- {COPYRIGHT}", "-- SPDX-License-Identifier: Apache-2.0"]   # X7: .lua files have it, manifest.json doesn't

MIN_S, MAX_S = 60, 5999
SIDE = ("-1 min", "+1 min", "Reset")                       # the buttons that aren't the main one
CLOCK = re.compile(r"\d+:\d\d")
ALARM_FIRED = {"event": "alarm", "name": "done", "sound": True, "wake": True}
RING_ON = {"event": "ring", "on": True}
RING_OFF = {"event": "ring", "on": False}
SHOW = {"event": "show"}
STARTED = {"event": "started", "id": "timer", "name": "Timer"}
START_ERROR = "whole number from 1 to 5999"
NO_SUCH_EVENT = "use start, pause, resume, reset or status"


def clock(seconds):
    return f"{seconds // 60}:{seconds % 60:02d}"


def screen(event):
    """What a `dump` event shows: the digits, the hint above them, the main button, and which of the other three buttons
    are hidden. The widgets are the ring, two labels and four buttons, in the order the app makes them."""
    widgets = event["widgets"]
    assert [w["kind"] for w in widgets] == ["arc", "label", "label"] + ["button"] * 4, widgets
    labels = [w["text"] or "" for w in widgets if w["kind"] == "label"]
    buttons = [w for w in widgets if w["kind"] == "button"]
    (digits,) = [t for t in labels if CLOCK.fullmatch(t)]
    (hint,) = [t for t in labels if not CLOCK.fullmatch(t)]
    (main,) = [w["text"] for w in buttons if w["text"] not in SIDE]
    return {"digits": digits, "hint": hint, "button": main,
            "hidden": {w["text"]: w["hidden"] for w in buttons if w["text"] in SIDE}}


def idle(digits="5:00"):
    return {"digits": digits, "hint": "", "button": "Start",
            "hidden": {"-1 min": False, "+1 min": False, "Reset": True}}


def running(digits):
    return {"digits": digits, "hint": "", "button": "Pause",
            "hidden": {"-1 min": True, "+1 min": True, "Reset": False}}


def paused(digits):
    return {"digits": digits, "hint": "Paused", "button": "Resume",
            "hidden": {"-1 min": True, "+1 min": True, "Reset": False}}


def done():
    return {"digits": "0:00", "hint": "Time's up", "button": "OK",
            "hidden": {"-1 min": True, "+1 min": True, "Reset": True}}


def reply(state, remaining):
    return {"event": "reply", "data": {"state": state, "remaining": remaining}}


class TimerTest(AppHostTestCase):
    def go(self, *script):
        return self.host(TIMER, script=list(script))

    def events(self, run):
        return [e for e in run.events if e["event"] != "store_save"]

    def kinds(self, run):
        return [e["event"] for e in self.events(run)]

    def screens(self, run):
        return [screen(e) for e in run.of("dump")]

    # ---- The package ----

    def test_the_package_files(self):
        manifest = json.loads((TIMER / "manifest.json").read_text())
        self.assertEqual((manifest["id"], manifest["name"], manifest["api"], manifest["main"]),
                         ("timer", "Timer", 1, "main.lua"))
        self.assertEqual(manifest["files"], [])                                  # no image, no module
        self.assertEqual(manifest["permissions"], ["sound", "alarm", "store"])
        messages = manifest["messages"]
        self.assertEqual(list(messages), ["start", "pause", "resume", "reset", "status"])
        for event, text in messages.items():
            with self.subTest(event):
                self.assertIn("{state, remaining}", text)                        # every event replies it
                self.assertIn("in seconds", text)
                self.assertLess(len(text.encode()), 200)
        self.assertIn("data: {seconds: whole number 1..5999}", messages["start"])
        self.assertTrue((TIMER / "manifest.json").read_text().startswith("{"), "a JSON file has no header")
        source = (TIMER / "main.lua").read_text()
        self.assertEqual(source.splitlines()[:2], LUA_HEADER)
        # alarm.after takes sound and wake as booleans: begin() and resume() each say so.
        self.assertEqual(source.count("alarm.after(ALARM, ms, { sound = true, wake = true })"), 1, "begin")
        self.assertEqual(source.count("alarm.after(ALARM, s.left, { sound = true, wake = true })"), 1, "resume")
        self.assertNotIn('sound = "alarm"', source)

    # ---- The screen ----

    def test_idle_layout(self):
        run = self.go("dump", "message status")
        self.assertEqual(run.kinds()[0], "started", "it loads with no warning")
        self.assertEqual(self.kinds(run), ["started", "dump", "reply"])
        self.assertEqual(self.screens(run), [idle()])
        self.assertEqual(run.of("reply"), [reply("idle", 300)])
        # The label, the button and the three side buttons are the ones the brief names.
        widgets = run.of("dump")[0]["widgets"]
        self.assertEqual([w["text"] for w in widgets if w["kind"] == "label"], ["", "5:00"])
        self.assertEqual([(w["text"], w["hidden"]) for w in widgets if w["kind"] == "button"],
                         [("-1 min", False), ("+1 min", False), ("Reset", True), ("Start", False)])

    def test_run_pause_resume_done(self):
        run = self.go('tap "Start"', "advance 1000", "dump",
                      'tap "Pause"', "dump", "advance 5000", "dump",
                      'tap "Resume"', "advance 299000", "dump",
                      'tap "OK"', "dump")
        self.assertEqual(self.screens(run), [
            running("4:59"),            # one second in
            paused("4:59"),             # Pause: the hint says so
            paused("4:59"),             # five seconds later: unchanged
            done(),                     # 299 s after the Resume: 0:00, Time's up, OK
            idle(),                     # OK: back to 5:00
        ])
        # The alarm is what ended it, with its sound and the screen's wake; the ring is on until OK silences it.
        self.assertEqual(self.kinds(run), ["started", "dump", "dump", "dump", "alarm", "ring", "dump", "ring", "dump"])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED])
        self.assertEqual(run.of("ring"), [RING_ON, RING_OFF])

    def test_the_alarm_rings_with_sound_and_wakes_the_screen_from_start_as_from_resume(self):
        # begin() and resume() each arm it: a millisecond short of the end nothing has fired, and at the end it has.
        started = self.go('message start {"seconds":5}', "advance 4999", "dump", "advance 1", "dump")
        self.assertEqual(self.kinds(started), ["started", "show", "reply", "dump", "alarm", "ring", "dump"])
        self.assertEqual(started.of("alarm"), [ALARM_FIRED])
        self.assertEqual(self.screens(started), [running("0:01"), done()])
        resumed = self.go('tap "Start"', "advance 1000", 'tap "Pause"', 'tap "Resume"', "advance 298999", "dump",
                          "advance 1", "dump")
        self.assertEqual(self.kinds(resumed), ["started", "dump", "alarm", "ring", "dump"])
        self.assertEqual(resumed.of("alarm"), [ALARM_FIRED])
        self.assertEqual(self.screens(resumed), [running("0:01"), done()])

    def test_the_label_never_says_0_00_before_it_rings(self):
        # Paused with a second left and with a millisecond left: the digits round up, and 0:00 is for the alarm.
        run = self.go('message start {"seconds":5}', "advance 4000", 'tap "Pause"', "dump", "advance 7000", "dump",
                      'tap "Resume"', "advance 999", "dump", "advance 1", "dump")
        self.assertEqual(self.screens(run), [paused("0:01"), paused("0:01"), running("0:01"), done()])
        self.assertEqual(self.kinds(run), ["started", "show", "reply", "dump", "dump", "dump", "alarm", "ring", "dump"])
        last = self.go('message start {"seconds":5}', "advance 4999", 'tap "Pause"', "dump", "message status",
                       'tap "Resume"', "advance 1", "dump")
        self.assertEqual(self.screens(last), [paused("0:01"), done()])
        self.assertEqual(last.of("reply"), [reply("running", 5), reply("paused", 1)])
        self.assertEqual(last.of("alarm"), [ALARM_FIRED])

    # ---- -1 min and +1 min ----

    def test_minus_plus_bounds(self):
        taps = ['tap "-1 min"'] * 5 + ['tap "+1 min"'] * 101 + ['tap "-1 min"']
        run = self.go(*[line for tap in taps for line in (tap, "dump")], "message status")
        digits = [s["digits"] for s in self.screens(run)]
        # 5:00 down a minute at a time to the 60 s minimum, and one tap more stays on it.
        self.assertEqual(digits[:5], ["4:00", "3:00", "2:00", "1:00", "1:00"])
        # Up from 60 s a minute a tap: 99 taps reach 5999 s (99:59), the maximum, and the next two stay on it.
        for k in range(1, 102):
            self.assertEqual(digits[4 + k], clock(min(MAX_S, MIN_S + 60 * k)), f"+1 min, tap {k}")
        self.assertEqual(digits[4 + 98:4 + 102], ["99:00", "99:59", "99:59", "99:59"])
        # A minute less from the maximum is 5939 s, and the status says it in seconds.
        self.assertEqual(digits[-1], "98:59")
        self.assertEqual(run.of("reply"), [reply("idle", 5939)])
        for shown in self.screens(run):
            self.assertEqual((shown["button"], shown["hint"]), ("Start", ""))

    def test_the_maximum_and_the_minimum_start_and_count_down(self):
        top = self.go(*['tap "+1 min"'] * 99, "dump", 'tap "Start"', "advance 1000", "dump", "message status")
        self.assertEqual([s["digits"] for s in self.screens(top)], ["99:59", "99:58"])
        self.assertEqual(top.of("reply"), [reply("running", 5998)])
        bottom = self.go(*['tap "-1 min"'] * 6, 'tap "Start"', "advance 59999", "dump", "advance 1", "dump",
                         'tap "OK"', "dump")
        self.assertEqual(self.screens(bottom), [running("0:01"), done(), idle("1:00")])
        self.assertEqual(bottom.of("alarm"), [ALARM_FIRED])

    # ---- Messages ----

    def test_messages(self):
        run = self.go('message start {"seconds":90}', "message status",
                      'message start {"seconds":0}', "message nope", "message status")
        self.assertEqual(self.kinds(run), ["started", "show", "reply", "reply", "error", "error", "reply"])
        # start brings the timer to the front and replies; status right after replies the same.
        self.assertEqual(self.events(run)[1:4], [SHOW, reply("running", 90), reply("running", 90)])
        self.assertEqual(run.of("reply"), [reply("running", 90)] * 3)
        zero, nope = run.messages("error")
        self.assertIn(START_ERROR, zero)
        self.assertIn(NO_SUCH_EVENT, nope)
        self.assertIn("'nope'", nope)
        self.assertEqual(run.of("show"), [SHOW], "only a start asks for the front, and a refused one doesn't")

    def test_start_refuses_what_is_not_a_whole_number_of_seconds_from_1_to_5999(self):
        bad = ['{"seconds":0}', '{"seconds":-5}', '{"seconds":6000}', '{"seconds":1.5}', '{"seconds":[]}',
               '{"seconds":true}', '{"seconds":null}', '{"seconds":"soon"}', "{}", '{"secs":10}', "[]", '"abc"']
        run = self.go(*[f"message start {data}" for data in bad], 'message start {"seconds":1}',
                      'message start {"seconds":5999}', "message status", "dump")
        errors = run.messages("error")
        self.assertEqual(len(errors), len(bad), errors)
        for data, message in zip(bad, errors):
            with self.subTest(data):
                self.assertIn(START_ERROR, message)
        self.assertEqual(run.of("reply"), [reply("running", 1), reply("running", 5999), reply("running", 5999)])
        self.assertEqual(self.screens(run), [running("99:59")])

    def test_start_with_data_that_is_not_an_object_is_refused_and_changes_nothing(self):
        # A number, a boolean and null (json.null, a userdata) can't be indexed: Lua's own message comes back, not the
        # one above. The call is refused all the same, with the timer as it was.
        run = self.go("message start 90", "message start true", "message start null", "message status", "dump")
        self.assertEqual(self.kinds(run), ["started", "error", "error", "error", "reply", "dump"])
        self.assertEqual(run.of("reply"), [reply("idle", 300)])
        self.assertEqual(self.screens(run), [idle()])
        self.assertEqual(run.of("show"), [])

    def test_pause_resume_reset_and_status_reply_state_and_remaining(self):
        run = self.go('message start {"seconds":90}', "advance 30000", "message pause", "advance 10000", "message status",
                      "message resume", "advance 15000", "message status", "message reset", "message status", "dump")
        self.assertEqual(run.of("reply"), [
            reply("running", 90), reply("paused", 60), reply("paused", 60),
            reply("running", 60), reply("running", 45), reply("idle", 90), reply("idle", 90)])
        self.assertEqual(self.screens(run), [idle("1:30")], "reset gives back the minutes it was started with")
        self.assertEqual(run.of("show"), [SHOW], "pause, resume, reset and status leave the screen where it is")

    def test_a_message_that_does_not_fit_the_state_changes_nothing(self):
        run = self.go("message pause", "message resume", "message status", "dump",
                      'tap "Start"', "message resume", "advance 1000", "message status")
        self.assertEqual(run.of("reply"), [reply("idle", 300)] * 3 + [reply("running", 300), reply("running", 299)])
        self.assertEqual(self.screens(run), [idle()])
        self.assertEqual(run.of("show"), [])

    def test_a_start_while_running_replaces_the_countdown(self):
        run = self.go('message start {"seconds":90}', "advance 30000", 'message start {"seconds":120}',
                      "advance 119999", "message status", "advance 1", "message status")
        # The first alarm would have gone off 90 s in, 60 s into the second countdown: it was replaced, not added.
        self.assertEqual(self.kinds(run), ["started", "show", "reply", "show", "reply", "reply", "alarm", "ring", "reply"])
        self.assertEqual(run.of("reply"),
                         [reply("running", 90), reply("running", 120), reply("running", 1), reply("done", 0)])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED])

    def test_reset_silences_the_ring(self):
        run = self.go('message start {"seconds":5}', "advance 5000", "message reset", "dump", "message status")
        self.assertEqual(self.kinds(run), ["started", "show", "reply", "alarm", "ring", "ring", "reply", "dump", "reply"])
        self.assertEqual(run.of("ring"), [RING_ON, RING_OFF])
        self.assertEqual(run.of("reply")[1:], [reply("idle", 5), reply("idle", 5)])
        self.assertEqual(self.screens(run), [idle("0:05")])

    def test_reset_stops_a_running_or_a_paused_countdown_and_takes_its_alarm_away(self):
        run = self.go('tap "Start"', "advance 100000", 'tap "Reset"', "dump", "advance 400000", "dump",
                      'tap "Start"', "advance 1000", 'tap "Pause"', 'tap "Reset"', "dump", "advance 400000", "dump")
        self.assertEqual(self.screens(run), [idle()] * 4)
        self.assertEqual(self.kinds(run), ["started"] + ["dump"] * 4, "no alarm fires, nothing rings")

    # ---- OK ----

    def test_ok_ends_the_ring_and_the_next_start_counts_again(self):
        run = self.go('tap "Start"', "advance 300000", 'tap "OK"', 'tap "Start"', "advance 1000", "dump", "message status")
        self.assertEqual(self.kinds(run), ["started", "alarm", "ring", "ring", "dump", "reply"])
        self.assertEqual(run.of("ring"), [RING_ON, RING_OFF])
        self.assertEqual(self.screens(run), [running("4:59")])
        self.assertEqual(run.of("reply"), [reply("running", 299)])

    def test_ok_without_a_ring_just_goes_back_to_idle(self):
        # The ring's minute is over (it went off on its own), so there is nothing for OK's sound.stop() to stop.
        run = self.go('message start {"seconds":5}', "advance 5000", "advance 61500", "dump", 'tap "OK"', "dump",
                      "message status")
        self.assertEqual(self.kinds(run), ["started", "show", "reply", "alarm", "ring", "ring", "dump", "dump", "reply"])
        self.assertEqual(run.of("ring"), [RING_ON, RING_OFF], "on at the alarm, off a minute later, nothing at OK")
        self.assertEqual(self.screens(run), [done(), idle("0:05")])
        self.assertEqual(run.of("reply")[1], reply("idle", 5))

    # ---- What it keeps ----

    def test_persists_and_recovers_after_restart(self):
        run = self.go('tap "Start"', "advance 10000", "restart", "dump", "advance 1000", "dump", "message status")
        # Back from the store and the system's alarm, running: 4:50 straight away, and counting from the alarm.
        self.assertEqual(self.screens(run), [running("4:50"), running("4:49")])
        self.assertEqual(run.of("reply"), [reply("running", 289)])
        self.assertEqual(run.of("started"), [STARTED, STARTED])
        self.assertEqual(run.of("alarm"), [], "the restart neither rings nor loses the alarm")
        rung = self.go('tap "Start"', "advance 5000", "restart", "advance 295000", "dump", "message status")
        self.assertEqual(rung.of("alarm"), [ALARM_FIRED])
        self.assertEqual(self.screens(rung), [done()])
        self.assertEqual(rung.of("reply"), [reply("done", 0)])

    def test_a_restart_keeps_the_minutes_the_pause_and_the_end(self):
        run = self.go('tap "+1 min"', 'tap "+1 min"', "restart", "dump",
                      'tap "Start"', "advance 1000", 'tap "Pause"', "restart", "dump", "advance 400000", "dump",
                      'tap "Resume"', "advance 418999", "dump", "advance 1", "dump", "restart", "dump")
        self.assertEqual(self.screens(run), [
            idle("7:00"),               # 5:00 and two minutes, kept
            paused("6:59"),             # paused a second in, kept
            paused("6:59"),             # no alarm is waiting: 400 s later it is the same
            running("0:01"),
            done(),
            done(),                     # the end of the countdown survives a restart too
        ])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED], "only the Resume's alarm ever fired")

    def test_a_running_timer_whose_alarm_is_gone_comes_back_done(self):
        # start() reads "running" from the store and finds no alarm for it: it rang while nobody was there to be told. The
        # system takes alarms away from an app that fires more than 6 in a minute (apps_alarm.h), so the seventh
        # countdown of a second in a row never rings and leaves the timer running with nothing to wait for.
        run = self.go(*['message start {"seconds":1}', "advance 1000"] * 7, "message status", "restart", "dump",
                      "message status")
        self.assertEqual(len(run.of("alarm")), 6)
        (warning,) = run.messages("warning")
        self.assertIn("6 alarms in a minute", warning)
        self.assertEqual(run.of("reply")[-2:], [reply("running", 0), reply("done", 0)])
        self.assertEqual(self.screens(run), [done()])

    def test_an_alarm_that_fires_while_the_app_is_stopped_leaves_it_done(self):
        # The system launches the app again for its alarm (a new runtime: start() reads the store) and tells it.
        run = self.go('tap "Start"', "stop", "advance 299999", "state", "advance 1", "state", "message status", "dump")
        self.assertEqual(self.kinds(run), ["started", "state", "alarm", "ring", "started", "state", "reply", "dump"])
        self.assertEqual([e["state"] for e in run.of("state")], ["stopped", "running"])
        self.assertEqual(run.of("started"), [STARTED, STARTED])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED])
        self.assertEqual(run.of("reply"), [reply("done", 0)])
        self.assertEqual(self.screens(run), [done()])

    def test_a_stopped_timer_comes_back_counting_from_its_alarm(self):
        run = self.go('tap "Start"', "advance 10000", "stop", "advance 20000", "restart", "dump", "message status")
        self.assertEqual(self.screens(run), [running("4:30")])
        self.assertEqual(run.of("reply"), [reply("running", 270)])
        self.assertEqual(run.of("alarm"), [])

    def test_a_done_timer_is_still_done_after_a_restart(self):
        run = self.go('message start {"seconds":5}', "advance 5000", "restart", "dump", 'tap "OK"', "dump")
        self.assertEqual(self.screens(run), [done(), idle("0:05")])

    # ---- Behind, and dark ----

    def test_an_alarm_while_the_app_is_behind_is_told_and_shown_when_it_comes_back(self):
        run = self.go('tap "Start"', "hide", "advance 301000", "dump", "show", "dump")
        self.assertEqual(self.kinds(run), ["started", "alarm", "ring", "dump", "dump"])
        self.assertEqual(self.screens(run), [done(), done()])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED])

    def test_a_hidden_timer_does_not_tick_and_catches_up_when_shown(self):
        run = self.go('tap "Start"', "hide", "advance 100000", "dump", "show", "dump", "advance 1000", "dump")
        self.assertEqual(self.screens(run), [running("5:00"), running("3:20"), running("3:19")])

    def test_the_digits_follow_the_clock_within_a_quarter_of_a_second(self):
        # Restarted a quarter of a second before the digits turn over (289.25 s left), the next quarter shows the new second.
        run = self.go('tap "Start"', "advance 10750", "restart", "dump", "advance 250", "dump")
        self.assertEqual(self.screens(run), [running("4:50"), running("4:49")])

    def test_a_dark_screen_does_not_leave_a_stale_time(self):
        # The board's gate: running, the screen asleep for 10 s, then awake: the first picture is of the time left now.
        run = self.go('tap "Start"', "dark on", "advance 10000", "dump", "dark off", "advance 0", "dump", "message status")
        self.assertEqual(self.screens(run), [running("5:00"), running("4:50")])
        self.assertEqual(run.of("reply"), [reply("running", 290)])
        self.assertEqual(run.of("alarm"), [])

    def test_an_alarm_in_the_dark_is_told_and_shows_done(self):
        run = self.go('message start {"seconds":5}', "dark on", "advance 5000", "dump")
        self.assertEqual(self.screens(run), [done()])
        self.assertEqual(run.of("alarm"), [ALARM_FIRED])
        self.assertEqual(run.of("ring"), [RING_ON])


if __name__ == "__main__":
    unittest.main()
