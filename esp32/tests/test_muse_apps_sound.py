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

"""The `sound` module of Muse apps (apps_sound.c): sound.tone, sound.play and their handles, and when the runtime ends an app's sounds.

The tests run apphost, the real runtime over the host's port, on an app made on the spot, and read its events. apphost's port is the
mixer as an app sees it (apps_port.h's apps_port_sound_*, apphost_port.c): each voice that starts is a `tone` or a `play` event with its
voice's id, each that a stop ends while it plays is a `sound_stop`, and it keeps the mixer's 4 voices (APPS_MIX_VOICES), a fifth
replacing the oldest, and a voice's end (its ms, a clip's frames at 16 kHz) on the fake clock, so that a handle whose voice has ended or
been replaced stops nothing, as on the gadget. What only the gadget shows (the speaker, the I2S queue, the lock's hold time, the
latency) is the board step's; the mixer itself is test_muse_apps_mix.py's.
"""

import json
import re
import unittest
from pathlib import Path

from test_muse_apps_pkg import ffmpeg_wav, wav
from test_muse_apps_rt import AppHostTestCase, header_number
from test_muse_apps_svc import BARE, SOUND_PERMISSION

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"

KEYS = "(valid: hz, ms, to_hz, wave, volume)"
WAVES = "sine, square, triangle, saw, noise"


def not_a_sound(path):
    return f"sound.play: '{path}' is not one of this app's sounds (manifest.json \"files\")"


def pcm(frames):
    """frames of 16-bit mono PCM, a ramp, as a WAV's data chunk holds them."""
    return b"".join(((i * 37) % 2000 - 1000).to_bytes(2, "little", signed=True) for i in range(frames))


# A clip of 1600 frames: 100 ms at 16 kHz.
BEEP = wav(pcm(1600))


class SoundTest(AppHostTestCase):
    def sound_app(self, handlers, script=None, permissions=("sound",), files=None, manifest=None, **kw):
        """An app whose main.lua has `bare` and `says` (test_muse_apps_svc.py) and these handlers, with the permissions, run."""
        main = BARE + "local app = {}\n" + handlers + "\nreturn app\n"
        doc = {"permissions": list(permissions), **(manifest or {})}
        return self.host(self.make_app("scratch", main, files=files, manifest=doc), script=script, **kw)

    def assert_clean(self, run):
        self.assertEqual([k for k in run.kinds() if k in ("warning", "error", "crash")], [], run.events)

    @staticmethod
    def sound_events(run):
        return [e for e in run.events if e["event"] in ("tone", "play", "sound_stop")]

    # ---- sound.tone ----

    def test_tone_events_carry_rounded_clamped_values(self):
        # Rounded halves away from zero, then clamped to the ranges; an infinity clamps; to_hz is hz unless given; wave sine and
        # volume 100 unless given. The fifth tone replaces the first of the 4 voices: no event says so.
        run = self.sound_app("""
            function app.start()
                sound.tone{hz = 9000.4, ms = 5}
                sound.tone{hz = 440.5, ms = 99.5, to_hz = 10, wave = "square", volume = 50.5}
                sound.tone{hz = 19.4, ms = 2000.6, volume = -0.5, wave = "triangle"}
                sound.tone{hz = 1/0, ms = -1/0, to_hz = -1/0, volume = 1/0, wave = "saw"}
                sound.tone{hz = -440.5, wave = "noise", to_hz = 7999.5}
                sound.tone{hz = 880}
            end
        """)
        self.assert_clean(run)
        self.assertEqual(self.sound_events(run), [
            {"event": "tone", "id": 1, "hz": 8000, "to_hz": 8000, "ms": 10, "wave": "sine", "volume": 100},
            {"event": "tone", "id": 2, "hz": 441, "to_hz": 20, "ms": 100, "wave": "square", "volume": 51},
            {"event": "tone", "id": 3, "hz": 20, "to_hz": 20, "ms": 2000, "wave": "triangle", "volume": 0},
            {"event": "tone", "id": 4, "hz": 8000, "to_hz": 20, "ms": 10, "wave": "saw", "volume": 100},
            {"event": "tone", "id": 5, "hz": 20, "to_hz": 8000, "ms": 100, "wave": "noise", "volume": 100},
            {"event": "tone", "id": 6, "hz": 880, "to_hz": 880, "ms": 100, "wave": "sine", "volume": 100},
        ])

    def test_tone_rejects_bad_types_and_unknown_keys(self):
        # An unknown key is found first, whatever else is wrong, and its message lists the keys; a value of the wrong type or NaN says
        # its range; a wave names the five. Nothing plays for a call that is refused.
        run = self.sound_app("""
            function app.start()
                says(sound.tone, {hz = 440, pitch = 2})
                says(sound.tone, {hz = "x", [1] = 2})
                says(sound.tone, {hz = 440, [true] = 1})
                says(sound.tone, {hz = "440"})
                says(sound.tone, {hz = 0/0})
                says(sound.tone, {hz = 440, ms = true})
                says(sound.tone, {hz = 440, ms = 0/0})
                says(sound.tone, {hz = 440, to_hz = {}})
                says(sound.tone, {hz = 440, volume = "loud"})
                says(sound.tone, {hz = 440, wave = "sawtooth"})
                says(sound.tone, {hz = 440, wave = 1})
                says(sound.tone, {hz = 440, wave = true})
                says(sound.tone, {ms = 100})
                says(sound.tone, 440)
                says(sound.tone)
            end
        """)
        self.assert_clean(run)
        self.assertEqual(run.logs(), [
            f"sound.tone: unknown key 'pitch' {KEYS}",
            f"sound.tone: unknown key '1' {KEYS}",
            f"sound.tone: unknown key '<boolean>' {KEYS}",
            "sound.tone: 'hz' must be a number of Hz, 20 to 8000, got string",
            "sound.tone: 'hz' must be a number of Hz, 20 to 8000, got NaN",
            "sound.tone: 'ms' must be a number of milliseconds, 10 to 2000, got boolean",
            "sound.tone: 'ms' must be a number of milliseconds, 10 to 2000, got NaN",
            "sound.tone: 'to_hz' must be a number of Hz, 20 to 8000, got table",
            "sound.tone: 'volume' must be a number, 0 to 100, got string",
            f"sound.tone: wave 'sawtooth' is not one of {WAVES}",
            f"sound.tone: wave '1' is not one of {WAVES}",
            f"sound.tone: wave '<boolean>' is not one of {WAVES}",
            "sound.tone: hz is required (a number of Hz, 20 to 8000)",
            "sound.tone: needs a table like {hz = 440, ms = 100}, got number",
            "sound.tone: needs a table like {hz = 440, ms = 100}, got no value",
        ])
        self.assertEqual(self.sound_events(run), [])

    def test_the_ranges_are_the_headers(self):
        # D12: what the messages say is apps_sound.h's, and the mixer's 4 voices are what apphost keeps.
        for name, value in (("APPS_SOUND_HZ_MIN", 20), ("APPS_SOUND_HZ_MAX", 8000), ("APPS_SOUND_MS_MIN", 10),
                            ("APPS_SOUND_MS_MAX", 2000), ("APPS_SOUND_MS_DEFAULT", 100), ("APPS_SOUND_VOLUME_MAX", 100)):
            with self.subTest(name):
                self.assertEqual(header_number(APPS / "apps_sound.h", name), value)
        self.assertEqual(header_number(APPS / "apps_mix.h", "APPS_MIX_VOICES"), 4)

    # ---- sound.play ----

    def test_play_needs_a_packaged_wav(self):
        # Only a .wav of the package's files plays; any other path, a file of another type included, is the brief's message. The
        # options are checked as the tone's keys are.
        files = {"sfx/beep.wav": BEEP, "sfx/ff.wav": ffmpeg_wav(pcm(8)), "data.json": "{}", "util.lua": "return 1"}
        run = self.sound_app("""
            function app.start()
                says(sound.play, "sfx/missing.wav")
                says(sound.play, "data.json")
                says(sound.play, "util.lua")
                says(sound.play, "main.lua")
                says(sound.play, "sfx/BEEP.wav")
                says(sound.play, "sfx/beep.wav\\n")
                says(sound.play, string.rep("a", 80) .. ".wav")
                says(sound.play, 5)
                says(sound.play)
                says(sound.play, "sfx/beep.wav", 3)
                says(sound.play, "sfx/beep.wav", {loop = 1})
                says(sound.play, "sfx/beep.wav", {volume = "x"})
                says(sound.play, "sfx/beep.wav", {volume = 0/0})
                says(sound.play, "sfx/beep.wav", {gain = 1, volume = "x"})
                sound.play("sfx/beep.wav")
                sound.play("sfx/ff.wav", {volume = 30.5, loop = true})
                sound.play("sfx/beep.wav", {volume = -1/0})
                sound.play("sfx/beep.wav", nil)
            end
        """, files=files)
        self.assert_clean(run)
        self.assertEqual(run.logs(), [
            not_a_sound("sfx/missing.wav"),
            not_a_sound("data.json"),
            not_a_sound("util.lua"),
            not_a_sound("main.lua"),
            not_a_sound("sfx/BEEP.wav"),
            not_a_sound("sfx/beep.wav?"),
            not_a_sound("a" * 64 + "..."),
            "sound.play: path must be a string, got number",
            "sound.play: path must be a string, got no value",
            "sound.play: options must be a table like {volume = 100, loop = false}, got number",
            "sound.play: 'loop' must be true or false, got number",
            "sound.play: 'volume' must be a number, 0 to 100, got string",
            "sound.play: 'volume' must be a number, 0 to 100, got NaN",
            "sound.play: unknown key 'gain' (valid: volume, loop)",
        ])
        self.assertEqual(self.sound_events(run), [
            {"event": "play", "id": 1, "path": "sfx/beep.wav", "volume": 100, "loop": False},
            {"event": "play", "id": 2, "path": "sfx/ff.wav", "volume": 31, "loop": True},
            {"event": "play", "id": 3, "path": "sfx/beep.wav", "volume": 0, "loop": False},
            {"event": "play", "id": 4, "path": "sfx/beep.wav", "volume": 100, "loop": False},
        ])

    def test_sound_needs_the_permission(self):
        # The permission comes before the arguments; sound.stop needs none, and a handle's stop is a sound's, which only an app with
        # the permission has.
        run = self.sound_app("""
            function app.start()
                says(sound.tone, {hz = 440})
                says(sound.tone, 5)
                says(sound.play, "beep.wav")
                says(sound.play, 5)
                says(sound.chirp)
                says(sound.stop)
            end
        """, permissions=(), files={"beep.wav": BEEP})
        self.assert_clean(run)
        self.assertEqual(run.logs(), [SOUND_PERMISSION] * 5 + ["ok"])
        self.assertEqual(self.sound_events(run), [])
        self.assertEqual(run.of("chirp"), [])

    # ---- Handles ----

    def test_handles_stop_and_print(self):
        # h:stop() ends the voice it names once: a second stop, and one of a sound that has ended, stops nothing. tostring counts the
        # app's sounds from 1. The handle is protected and has one method.
        run = self.sound_app("""
            local held
            function app.message(event)
                if event == "go" then
                    local a = sound.tone{hz = 440, ms = 2000}
                    local b = sound.play("beep.wav", {loop = true})
                    print(tostring(a), tostring(b))
                    a:stop()
                    a:stop()
                    print("stopped")
                    b:stop()
                    print(type(a), getmetatable(a))
                    says(function() return a.volume end)
                    says(function() return a[1] end)
                    says(function() a.x = 1 end)
                    says(a.stop, 5)
                    says(a.stop)
                    held = sound.tone{hz = 220, ms = 100}
                elseif event == "late" then
                    held:stop()
                    print(tostring(held))
                end
                return true
            end
        """, script=["message go", "advance 200", "message late"], files={"beep.wav": BEEP})
        self.assert_clean(run)
        self.assertEqual(run.logs(), [
            "sound#1\tsound#2", "stopped", "userdata\tmuse.sound",
            "sound: unknown method 'volume' (valid: stop)",
            "sound: unknown method '<number>' (valid: stop)",
            "sound: a sound has no fields: h:stop() stops it",
            "sound: stop needs a sound: call it as h:stop()",
            "sound: stop needs a sound: call it as h:stop()",
            "sound#3",
        ])
        kinds = [(e["event"], e.get("id")) for e in self.sound_events(run)]
        self.assertEqual(kinds, [("tone", 1), ("play", 2), ("sound_stop", 1), ("sound_stop", 2), ("tone", 3)])
        # The order: the first stop's event comes inside the call, before what it printed next.
        order = [(e["event"], e.get("line")) for e in run.events if e["event"] in ("log", "sound_stop")]
        self.assertEqual(order[:4], [("log", "sound#1\tsound#2"), ("sound_stop", None), ("log", "stopped"), ("sound_stop", None)])

    def test_behind_or_dark_returns_a_done_handle(self):
        # Behind (hide) or in the dark, a call checks its arguments and returns a handle that is already done: nothing plays, its stop
        # stops nothing, and it still counts in tostring. In front and lit again, sounds play.
        run = self.sound_app("""
            function app.message(event)
                local t = sound.tone{hz = 440, ms = 1000}
                local p = sound.play("beep.wav", {loop = true})
                print(event, tostring(t), tostring(p))
                says(sound.tone, {hz = "x"})
                says(sound.play, "nope.wav")
                t:stop()
                p:stop()
                return true
            end
        """, script=["hide", "message behind", "show", "message front", "dark on", "message dark", "dark off", "message lit"],
            files={"beep.wav": BEEP})
        self.assert_clean(run)
        bad = ["sound.tone: 'hz' must be a number of Hz, 20 to 8000, got string", not_a_sound("nope.wav")]
        self.assertEqual(run.logs(), ["behind\tsound#1\tsound#2", *bad, "front\tsound#3\tsound#4", *bad,
                                      "dark\tsound#5\tsound#6", *bad, "lit\tsound#7\tsound#8", *bad])
        kinds = [e["event"] for e in self.sound_events(run)]
        self.assertEqual(kinds, ["tone", "play", "sound_stop", "sound_stop", "tone", "play", "sound_stop", "sound_stop"])

    def test_an_app_launched_in_front_starts_shown(self):
        # apphost launches every app in front: start() and main.lua's later calls play; main.lua's top level runs before the launch
        # is in front, so its sound is done at once.
        run = self.sound_app("""
            local early = sound.tone{hz = 440}
            function app.start() print(tostring(early)) sound.tone{hz = 660} end
        """)
        self.assert_clean(run)
        self.assertEqual(run.logs(), ["sound#1"])
        self.assertEqual([e["hz"] for e in run.of("tone")], [660])

    # ---- The runtime ends the app's sounds (Review Focus 1) ----

    def test_a_sound_started_before_hide_never_plays_after_it(self):
        # hide: the app's sounds end before hide() runs, so their sound_stop comes before what hide() prints, and a sound hide() asks
        # for is done at once.
        run = self.sound_app("""
            function app.message(event)
                sound.tone{hz = 440, ms = 2000}
                sound.play("beep.wav", {loop = true})
                return true
            end
            function app.hide() print("hide") print(tostring(sound.tone{hz = 330})) end
        """, script=["message go", "hide"], files={"beep.wav": BEEP})
        self.assert_clean(run)
        tail = [(e["event"], e.get("id"), e.get("line")) for e in run.events if e["event"] in ("sound_stop", "log", "tone")]
        self.assertEqual(tail, [("tone", 1, None), ("sound_stop", 1, None), ("sound_stop", 2, None), ("log", None, "hide"),
                                ("log", None, "sound#3")])

    def test_exit_stop_crash_and_restart_end_the_apps_sounds(self):
        # sys.exit(), the stop command and a restart end the app's sounds before stop() runs (so before what it prints), and again
        # after it, for one that stop() started; a crash (a timer's error: message()'s own fails the message alone) ends them with it.
        handlers = """
            function app.message(event)
                sound.play("beep.wav", {loop = true})
                if event == "bye" then sys.exit() end
                if event == "boom" then timer.after(40, function() error("boom") end) end
                return true
            end
            function app.stop() print("stop") sound.tone{hz = 200, ms = 2000} end
        """
        files = {"beep.wav": BEEP}
        for script, crashes in ((["message bye"], False), (["message go", "stop"], False), (["message go", "restart"], False),
                                (["message boom", "advance 50"], True)):
            with self.subTest(script[0] if crashes else script[-1]):
                run = self.sound_app(handlers, script=script, files=files, rc=1 if crashes else 0)
                seen = [(e["event"], e.get("line")) for e in run.events
                        if e["event"] in ("play", "tone", "sound_stop", "log", "crash")]
                if crashes:
                    self.assertEqual(seen[:2], [("play", None), ("sound_stop", None)])
                    self.assertEqual(seen[2][0], "crash")
                else:
                    self.assertEqual(seen[:5], [("play", None), ("sound_stop", None), ("log", "stop"), ("tone", None),
                                                ("sound_stop", None)])

    def test_an_alarm_ring_cuts_the_apps_sound(self):
        # The ring's start is Muse's: every voice is stopped (apps_alarm.c's ring_sound_locked), before the app hears its alarm.
        run = self.sound_app("""
            function app.start() alarm.after("t", 1000) end
            function app.message() sound.play("beep.wav", {loop = true}) return true end
            function app.alarm(name) print("alarm " .. name) end
        """, script=["message loop", "advance 1000"], permissions=("sound", "alarm"), files={"beep.wav": BEEP})
        self.assert_clean(run)
        seen = [e["event"] for e in run.events if e["event"] in ("play", "sound_stop", "alarm", "ring", "log")]
        self.assertEqual(seen, ["play", "sound_stop", "alarm", "ring", "log"])

    # ---- Review Focus 3 (G7) ----

    def test_a_thousand_tones_leave_no_trace(self):
        # A game that starts a tone on every tick: 1,000 tones, each a voice (the oldest replaced past 4) and a handle the app drops.
        # No crash, the heap stays under memory_kb, the first handle is stale (its voice was replaced long ago) and the last stops.
        run = self.sound_app("""
            local first, last
            function app.message(event)
                if event == "many" then
                    for i = 1, 1000 do
                        local h = sound.tone{hz = 200 + i, ms = 2000}
                        if i == 1 then first = h end
                        last = h
                    end
                    local mem = sys.mem()
                    print(mem.used_kb < mem.limit_kb, mem.peak_kb < mem.limit_kb, tostring(last))
                elseif event == "stop" then
                    first:stop()
                    print("first")
                    last:stop()
                    print("last")
                end
                return true
            end
        """, script=["message many", "message stop", "state"], manifest={"memory_kb": 64})
        self.assert_clean(run)
        self.assertEqual(len(run.of("tone")), 1000)
        self.assertEqual(run.logs(), ["true\ttrue\tsound#1000", "first", "last"])
        stops = [e for e in run.events if e["event"] in ("sound_stop", "log")][1:]
        self.assertEqual([e["event"] for e in stops], ["log", "sound_stop", "log"])
        self.assertEqual(stops[1]["id"], run.of("tone")[-1]["id"])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "running"}])


class SoundFixtureTest(AppHostTestCase):
    def test_the_board_fixture_runs_here(self):
        # tests/muse_apps_fixtures/sound, which the board step drives (press, mute, chord, loop, alarm, state), does here what the
        # board is to measure: a tone a press unless muted, four voices for a chord, the loop that the ring's start stops, and
        # nothing behind.
        run = self.host(ROOT / "tests/muse_apps_fixtures/sound", script=[
            "message press", "advance 200", 'message mute {"on":true}', "message press", "message mute", "message chord",
            "message loop", 'message loop {"on":false}', "message alarm", "advance 5000", "message state", "hide", "message press"])
        self.assertEqual([k for k in run.kinds() if k in ("warning", "error", "crash")], [], run.events)
        tones = run.of("tone")
        self.assertEqual(tones[0], {"event": "tone", "id": 1, "hz": 520, "to_hz": 780, "ms": 60, "wave": "square", "volume": 40})
        self.assertEqual([t["hz"] for t in tones[1:]], [262, 330, 392, 523])
        replies = [e["data"] for e in run.of("reply")]
        self.assertEqual(replies[2], {"flaps": 2, "muted": True})  # muted: no sound
        self.assertEqual(run.of("play"), [{"event": "play", "id": 6, "path": "loop.wav", "volume": 60, "loop": True},
                                          {"event": "play", "id": 7, "path": "loop.wav", "volume": 60, "loop": True}])
        tail = [e["event"] for e in run.events if e["event"] in ("play", "sound_stop", "alarm", "ring", "log")][2:]
        self.assertEqual(tail, ["play", "sound_stop", "alarm", "ring", "log", "ring"])
        self.assertEqual(replies[-2], {"flaps": 2, "muted": False, "looping": True, "y": 150, "rang": "ring"})
        self.assertEqual(replies[-1], {"flaps": 3, "muted": False, "sound": "sound#8"})
        self.assertEqual(len(tones), 5, "behind, the last press played nothing")
        manifest = json.loads((ROOT / "tests/muse_apps_fixtures/sound/manifest.json").read_text())
        self.assertEqual(sorted(manifest["messages"]), ["alarm", "chord", "loop", "mute", "press", "state"])
        main = (ROOT / "tests/muse_apps_fixtures/sound/main.lua").read_text().splitlines()
        self.assertEqual(main[:2], ["-- Copyright (c) Meta Platforms, Inc. and affiliates.", "-- SPDX-License-Identifier: Apache-2.0"])


class SoundStaticTest(unittest.TestCase):
    def test_the_module_reaches_the_mixer_through_the_port(self):  # G1
        source = re.sub(r"/\*.*?\*/", "", (APPS / "apps_sound.c").read_text(), flags=re.S)
        self.assertNotRegex(source, r"\bapps_mix_\w+\(", "apps_sound.c calls the mixer: it must go through apps_port_sound_*")
        for name in ("apps_port_sound_tone(", "apps_port_sound_clip(", "apps_port_sound_end("):
            self.assertIn(name, source)
        self.assertNotIn("__gc", source)  # X17: no finalizer of ours either

    def test_every_binding_calls_apps_vm_check_first(self):
        source = (APPS / "apps_sound.c").read_text()
        functions = re.findall(r"^static int (\w+)\(lua_State \*L\) \{\n(.*?)^\}", source, re.S | re.M)
        self.assertGreaterEqual(len(functions), 7)
        for name, body in functions:
            with self.subTest(name):
                self.assertTrue(body.lstrip().startswith("apps_vm_check(L);"), f"{name} doesn't start with apps_vm_check(L)")

    def test_the_handle_metatable_is_protected(self):  # X17
        source = (APPS / "apps_sound.c").read_text()
        self.assertEqual(source.count("luaL_newmetatable"), 1)
        match = re.search(r"luaL_newmetatable\(L, APPS_SOUND_MT\);(.*?)lua_setfield\(L, -2, \"__metatable\"\)", source, re.S)
        self.assertIsNotNone(match, "the sound handle's metatable has no __metatable")
        self.assertLess(match.group(1).count("\n"), 6)
        self.assertRegex((APPS / "apps_sound.h").read_text(), r'#define APPS_SOUND_MT "muse\.sound"')


if __name__ == "__main__":
    unittest.main()
