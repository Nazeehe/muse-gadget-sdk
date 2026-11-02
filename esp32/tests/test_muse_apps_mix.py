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

"""apps_mix.c, the mixer of app sound that the voice task plays: tones, clips, their voices and ids, the fades, the saturated sum and
the clip-lifetime guarantee.

tests/muse_apps_mix_harness.c runs apps_mix.c alone, with ASan and UBSan, over a mixer lock of pthreads that aborts on a misuse. Each
test writes it a script and reads one answer a command, and the sound is checked here: pitch from zero crossings, the envelope from
the peak of each 1 ms window. A clip lives in a heap block of exactly its size, so a read past its end, or after the test frees it,
is an AddressSanitizer report. What only the board shows (the speaker's clipping, the I2S queue's latency) is Task 3's to measure.
"""

import re
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_mix_harness.c"

WAVES = ("sine", "square", "triangle", "saw", "noise")

# What apps_mix.c and muse_apps.h include of ESP-IDF and LVGL, as the least that compiles on the host.
FAKES = {
    "esp_attr.h": "#pragma once\n#define EXT_RAM_BSS_ATTR\n",
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n",
    "lvgl.h": "#pragma once\ntypedef struct _lv_obj_t lv_obj_t;\n",
    "sdkconfig.h": "#pragma once\n#define CONFIG_SPIRAM 1\n#define CONFIG_MUSE_APPS 1\n",
}


def header_number(path, name):
    """What `#define NAME 123` is in a header."""
    match = re.search(rf"^#define\s+{name}\s+(\d+)\b", Path(path).read_text(), re.M)
    assert match, f"no #define {name} in {path}"
    return int(match.group(1))


def strip_c_comments(source):
    return re.sub(r'("(?:\\.|[^"\\\n])*")|/\*.*?\*/|//[^\n]*', lambda m: m.group(1) or "", source, flags=re.S)


def crossings(samples):
    """The zero crossings, in frames: between the last sample that wasn't 0 and the next one of the other sign, interpolated."""
    found, last = [], None
    for i, s in enumerate(samples):
        if s == 0:
            continue
        if last is not None and (s > 0) != (last[1] > 0):
            j, a = last
            found.append(j + (i - j) * a / (a - s))
        last = (i, s)
    return found


def frequency(times, rate):
    """The mean frequency over crossings at these frames: two a cycle."""
    assert len(times) >= 3, f"too few crossings to measure: {len(times)}"
    return (len(times) - 1) / 2 / ((times[-1] - times[0]) / rate)


def window_peaks(samples, width):
    return [max(abs(s) for s in samples[i:i + width]) for i in range(0, len(samples) - width + 1, width)]


class Mix:
    """The answers to one script, in the order of its commands."""

    def __init__(self, answers):
        self.answers = answers

    def render(self, i):
        live, *frames = self.answers[i].split()
        return live == "1", [int(f) for f in frames]

    def number(self, i):
        return int(self.answers[i])


class MixTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = Path(tmp.name) / "fakes"
        stage.mkdir()
        for name, text in FAKES.items():
            (stage / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_mix_harness", [HARNESS, APPS / "apps_mix.c"], tmp.name,
                                      include_dirs=[stage, APPS, MUSE])
        header = APPS / "apps_mix.h"
        cls.peak = header_number(header, "APPS_MIX_TONE_PEAK")
        cls.rate = header_number(header, "APPS_MIX_RATE")
        cls.voices = header_number(header, "APPS_MIX_VOICES")
        cls.fade = header_number(header, "APPS_MIX_FADE_MS") * cls.rate // 1000
        cls.ms = cls.rate // 1000

    def run_script(self, *commands, timeout=120):
        script = "".join(f"{c}\n" for c in commands)
        done = apps_host.run(self.harness, input=script, timeout=timeout)
        self.assertEqual(done.returncode, 0, done.stderr[-6000:])
        answers = done.stdout.splitlines()
        self.assertEqual(len(answers), len(commands), "one answer a command")
        return Mix(answers)

    def assert_envelope_falls_to_zero(self, before, after, why):
        """After a stop or a replacement: the peak of each 1 ms window never rises, and is 0 from the fade's end on (every frame of
        it). The fade starts from where the envelope was, not from the full level: a stop during a fade in stays near it."""
        peaks = window_peaks(after, self.ms)
        self.assertLessEqual(peaks[0], max(abs(s) for s in before) * 1.2 + 1, f"{why}: the fade jumped up")
        for a, b in zip(peaks, peaks[1:]):
            self.assertLessEqual(b, a, f"{why}: the envelope rose {peaks}")
        self.assertEqual(after[self.fade:], [0] * (len(after) - self.fade), f"{why}: still sounding after {self.fade} frames")
        self.assertGreater(peaks[0], 0, f"{why}: no fade, a cut")

    def assert_fades_within(self, before, after, why):
        """A fade of several voices at once: the sum's window peaks follow the voices' phases, so they needn't fall each window,
        but the sum is never louder than before, sounds at first and is 0 from the fade's end on."""
        self.assertLessEqual(max(abs(s) for s in after), max(abs(s) for s in before) * 1.2 + 1, f"{why}: the fade jumped up")
        self.assertGreater(max(abs(s) for s in after[:self.fade]), 0, f"{why}: no fade, a cut")
        self.assertEqual(after[self.fade:], [0] * (len(after) - self.fade), f"{why}: still sounding after {self.fade} frames")

    # ---- The brief's ----

    def test_tone_has_its_length_and_pitch(self):
        frames = 100 * self.ms
        mix = self.run_script("tone 1 1000 1000 100 sine 100", f"render {frames - 1}", "playing #0", "render 1", "playing #0",
                              "render 320")
        live, first = mix.render(1)
        self.assertTrue(live)
        self.assertEqual(mix.number(2), 1, "it plays until its last frame")
        live, last = mix.render(3)
        self.assertTrue(live)
        self.assertEqual(mix.number(4), 0, "it ends with its last frame")
        live, after = mix.render(5)
        self.assertFalse(live)
        self.assertEqual(after, [0] * 320)
        tone = first + last
        sounding = [i for i, s in enumerate(tone) if s != 0]
        self.assertLessEqual(sounding[0], 2)
        self.assertGreaterEqual(sounding[-1], frames - 4)
        f = frequency(crossings(tone), self.rate)
        self.assertAlmostEqual(f, 1000, delta=10, msg=f"{f:.1f} Hz")

    def test_slide_goes_from_hz_to_to_hz(self):
        for hz, to_hz in ((200, 2000), (2000, 200)):
            with self.subTest(hz=hz, to_hz=to_hz):
                frames = 500 * self.ms
                live, tone = self.run_script(f"tone 1 {hz} {to_hz} 500 sine 100", f"render {frames}").render(1)
                self.assertTrue(live)
                times = crossings(tone)

                def expected(at):  # the linear slide at a frame
                    return hz + (to_hz - hz) * at / frames

                # Each 50 ms: a linear slide's mean over the window is its value at the middle.
                window = 50 * self.ms
                for start in range(0, frames, window):
                    inside = [t for t in times if start <= t < start + window]
                    f = frequency(inside, self.rate)
                    want = expected((inside[0] + inside[-1]) / 2)
                    self.assertAlmostEqual(f, want, delta=want * 0.02, msg=f"{f:.1f} Hz at {start} (want {want:.1f})")
                # The first cycle is about hz and the last about to_hz.
                first, last = frequency(times[:3], self.rate), frequency(times[-3:], self.rate)
                self.assertAlmostEqual(first, hz, delta=hz * 0.1, msg=f"first cycle {first:.1f} Hz")
                self.assertAlmostEqual(last, to_hz, delta=to_hz * 0.1, msg=f"last cycle {last:.1f} Hz")

    def test_waves_stay_within_the_peak(self):
        frames = 200 * self.ms
        for wave in WAVES:
            with self.subTest(wave):
                mix = self.run_script(f"tone 1 100 100 200 {wave} 100", f"render {frames}", f"tone 1 100 100 200 {wave} 50",
                                      f"render {frames}", f"tone 1 100 100 200 {wave} 0", f"render {frames}")
                full, half, none = (mix.render(i) for i in (1, 3, 5))
                top = max(abs(s) for s in full[1])
                self.assertLessEqual(top, self.peak + 1)
                self.assertGreaterEqual(top, self.peak * 0.97, "it reaches its peak")
                self.assertAlmostEqual(max(abs(s) for s in half[1]), top / 2, delta=1, msg="volume 50 halves it")
                self.assertTrue(none[0], "a voice at volume 0 is live")
                self.assertEqual(none[1], [0] * frames, "and silent")

    def test_fades_have_no_step(self):
        # Ruling G2: the envelope, not sample steps (a 1 kHz sine steps about 3,200 between samples, a square its whole peak).
        frames = 100 * self.ms
        live, tone = self.run_script("tone 1 1000 1000 100 sine 100", f"render {frames}").render(1)
        self.assertLessEqual(abs(tone[0]), self.peak / 40, "it starts at 0")
        peaks = window_peaks(tone, self.ms)
        fades = self.fade // self.ms
        rise, fall = peaks[:fades + 1], peaks[-fades:]
        self.assertEqual(rise, sorted(set(rise)), f"the fade in rises: {rise}")
        self.assertEqual(fall, sorted(set(fall), reverse=True), f"the fade out falls: {fall}")
        self.assertEqual(tone[-1], 0, "it ends at 0")
        # A stop fades from where the envelope is: in the middle of a tone, during its fade in, and a clip's.
        for at, start in ((800, "tone 1 1000 1000 1000 sine 100"), (40, "tone 1 1000 1000 1000 sine 100"),
                          (800, "clip 1 c 100 1"), (800, "tone 1 1000 1000 1000 square 100")):
            with self.subTest(start, at=at):
                mix = self.run_script("make c 16000 2000", start, f"render {at}", "stop #0", "playing #0", "render 160",
                                      "render 16")
                self.assertEqual(mix.number(4), 0, "a stopped id is stale at once")
                self.assert_envelope_falls_to_zero(mix.render(2)[1], mix.render(5)[1], "stop")
                self.assertFalse(mix.render(6)[0], "the fade has ended")

    def test_tone_edges_render_bounded(self):
        cases = []  # (a start's arguments after its owner, the frames it plays)
        for wave in WAVES:
            for hz, to_hz in ((20, 20), (8000, 8000), (20, 8000), (8000, 20)):
                cases.append((f"{hz} {to_hz} 10 {wave} 100", 10 * self.ms))
        # The mixer clamps what the sound module refuses: hz to 1 to APPS_MIX_RATE / 2, ms to at least 1, volume to 0 to 100, and a
        # wave it doesn't know is a sine.
        cases += [("0 0 10 sine 100", 10 * self.ms), ("-5 9 10 square 100", 10 * self.ms),
                  ("100000 100000 10 saw 100", 10 * self.ms), ("440 440 0 sine 100", self.ms), ("440 440 -7 triangle 100", self.ms),
                  ("440 880 10 sine 150", 10 * self.ms), ("440 880 10 noise -3", 10 * self.ms), ("440 880 10 99 100", 10 * self.ms),
                  ("440 880 10 -1 100", 10 * self.ms)]
        commands = []
        for args, frames in cases:
            commands += [f"tone 1 {args}", f"render {frames - 1}", "render 1", "render 16"]
        mix = self.run_script(*commands)
        for i, (args, frames) in enumerate(cases):
            with self.subTest(args):
                # No NaN either: UBSan aborts on a float that doesn't fit the int it is cast to.
                (live, tone), (last_live, last) = mix.render(4 * i + 1), mix.render(4 * i + 2)
                self.assertTrue(live and last_live, "it plays all its frames")
                self.assertLessEqual(max(abs(s) for s in tone + last), self.peak, "within the peak")
                self.assertEqual(mix.render(4 * i + 3), (False, [0] * 16), "and no more")

    def test_clip_plays_scaled_and_loops(self):
        mix = self.run_script("make r ramp 1000", "samples r", "clip 1 r 50 0", "render 1000", "playing #0", "render 320",
                              "clip 1 r 50 1", "render 3000", "playing #1", "stop_owner 1", "clip 2 r 100 0", "render 1000",
                              "clip 2 r 250 0", "render 1000")
        ramp = [int(s) for s in mix.answers[1].split()[1:]]
        self.assertEqual((ramp[0], ramp[-1]), (-32767, 32767))
        half = [int(s * 50 / 100) for s in ramp]  # C's division: toward 0
        live, once = mix.render(3)
        self.assertTrue(live)
        self.assertEqual(once, half, "half the samples")
        self.assertEqual(mix.number(4), 0, "it ends after its frames")
        self.assertEqual(mix.render(5), (False, [0] * 320))
        live, looped = mix.render(7)
        self.assertEqual(looped, half * 3, "looped, seamlessly")
        self.assertEqual(mix.number(8), 1, "a loop plays on")
        self.assertEqual(mix.render(11)[1], ramp, "volume 100 is the samples")
        self.assertEqual(mix.render(13)[1], ramp, "a volume over 100 is 100")

    def test_mix_saturates(self):
        commands = ["make h high 320", "make l low 320"]
        cases = (("hhhh", 32767), ("llll", -32767), ("hhhl", 32767), ("hhll", -2), ("lll", -32767))
        for voices, _ in cases:
            commands += [f"clip 1 {'h' if v == 'h' else 'l'} 100 0" for v in voices] + ["render 320", "stop_owner 1"]
        mix = self.run_script(*commands)
        at = 2
        for voices, want in cases:
            with self.subTest(voices):
                at += len(voices)
                self.assertEqual(mix.render(at), (True, [want] * 320))
                at += 2

    def test_many_starts_keep_four_voices(self):
        # Review Focus 3: a game that starts a sound every tick for minutes.
        commands, starts = ["make r ramp 500"], 0
        while starts < 10000 - self.voices:
            k = starts % 11
            commands.append(f"clip {1 + k % 3} r {k * 9} {starts // 11 % 2}" if k % 4 == 0 else
                            f"tone {1 + k % 3} {100 + 37 * k} {300 + 91 * k} {10 + 23 * k} {WAVES[k % 5]} {k * 9}")
            starts += 1
            if starts % 7 == 0:
                commands.append("render 320")
            if starts % 100 == 0:
                commands.append("count")
        for _ in range(self.voices):  # the last ones are long: they still play at the end
            commands.append("tone 4 440 440 2000 sine 10")
            starts += 1
        commands += ["count", "playing 0", "stop 0"]
        commands += [f"stop #{k}" for k in range(starts - self.voices)]  # every stale id: none stops anything
        commands += ["count"] + [f"playing #{k}" for k in range(starts - self.voices, starts)]
        commands += ["render 320", "stop_all", "count", "active", "render 320", "active"]
        mix = self.run_script(*commands)
        ids = [int(a) for c, a in zip(commands, mix.answers) if c.startswith(("tone", "clip"))]
        self.assertEqual(len(ids), 10000)
        self.assertNotIn(0, ids)
        self.assertEqual(len(set(ids)), len(ids), "every id is new")
        counts = [int(a) for c, a in zip(commands, mix.answers) if c == "count"]
        self.assertLessEqual(max(counts), self.voices, "at most 4 voices live")
        self.assertEqual(counts[-3:], [self.voices, self.voices, 0], "the stale ids stopped nothing")
        tail = mix.answers[-(self.voices + 6):-6]
        self.assertEqual(tail, ["1"] * self.voices)
        self.assertEqual(mix.answers[commands.index("playing 0")], "0", "0 is never an id")
        self.assertEqual(mix.answers[-3], "1", "stop_all's fades are live until they are rendered")
        self.assertEqual(mix.answers[-1], "0")
        renders = [mix.render(i) for i, c in enumerate(commands) if c.startswith("render")]
        self.assertTrue(all(abs(s) <= 32767 for _, frames in renders for s in frames))

    def test_stop_owner_frees_only_that_owner(self):
        mix = self.run_script(
            "make a ramp 1000", "make b ramp 700",
            "clip 1 a 100 1", "tone 1 440 440 2000 sine 100", "clip 2 b 100 1", "tone 2 660 660 2000 square 100",
            "render 320",
            "stop #0",  # owner 1's clip fades: its tail reads the clip too
            "render 16",
            "stop_owner 1", "free a",  # owner 1's bytes go at once: a render that read them is a use after free
            "render 320", "playing #0", "playing #1", "playing #2", "playing #3",
            "stop_owner 9", "playing #2", "playing #3",
            "stop_owner 2", "free b", "render 320", "active")
        self.assertTrue(mix.render(11)[0], "owner 2 plays on")
        self.assertEqual([mix.number(i) for i in (12, 13, 14, 15)], [0, 0, 1, 1])
        self.assertEqual([mix.number(i) for i in (17, 18)], [1, 1], "an owner with no voice stops nothing")
        self.assertEqual(mix.render(21), (False, [0] * 320))
        self.assertEqual(mix.number(22), 0)

    def test_render_reports_silence(self):
        mix = self.run_script("render 320", "active", "tone 1 440 440 30 sine 100", "active", "render 320", "render 320",
                              "active", "render 320", "active",
                              "render 0", "tone 1 440 440 1000 triangle 100", "render 0")
        self.assertEqual(mix.render(0), (False, [0] * 320))
        self.assertEqual([mix.number(i) for i in (1, 3)], [0, 1])
        self.assertTrue(mix.render(4)[0])
        live, ending = mix.render(5)  # the tone's last 160 frames, then nothing
        self.assertTrue(live, "true while a voice was live for any of it")
        self.assertEqual(ending[160:], [0] * 160)
        self.assertEqual(mix.number(6), 0)
        self.assertEqual(mix.render(7), (False, [0] * 320))
        self.assertEqual(mix.number(8), 0)
        self.assertEqual(mix.render(9), (False, []))
        self.assertEqual(mix.render(11), (True, []))

    # ---- The rest of the contract ----

    def test_the_oldest_is_replaced_and_fades_out(self):
        silent = "tone 1 440 440 2000 sine 0"
        mix = self.run_script("make c 8000 4000", "clip 1 c 100 1", silent, silent, silent, "render 320",
                              silent, "playing #0", "playing #1", "playing #2", "playing #3", "playing #4", "render 160",
                              silent, "playing #1", "playing #2", "playing #5")
        self.assertEqual(mix.render(5), (True, [8000] * 320))
        self.assertEqual([mix.number(i) for i in range(7, 12)], [0, 1, 1, 1, 1], "the fifth replaced the first")
        self.assert_envelope_falls_to_zero([8000] * 320, mix.render(12)[1], "replaced")
        self.assertEqual([mix.number(i) for i in (14, 15, 16)], [0, 1, 1], "then the next oldest")

    def test_stop_all_fades_out(self):
        # Ruling R1a: Muse taking the speaker fades every voice out over APPS_MIX_FADE_MS, as a stop does, tones and clips alike, and
        # the voice task renders the fades (one chunk holds them) before it records or rests.
        for start in ("tone 1 1000 1000 1000 sine 100", "tone 1 440 1760 1000 square 100", "clip 1 c 100 1",
                      "tone 1 1000 1000 1000 sine 100\ntone 2 330 330 1000 triangle 60\nclip 3 c 50 1"):
            with self.subTest(start):
                starts = start.split("\n")
                mix = self.run_script("make c 12000 3000", *starts, "render 800", "stop_all", "active",
                                      *(f"playing #{k}" for k in range(len(starts))), "render 160", "active", "render 16")
                at = 1 + len(starts)
                self.assertEqual(mix.number(at + 2), 1, "live while the fades last")
                self.assertEqual([mix.number(at + 3 + k) for k in range(len(starts))], [0] * len(starts), "every id stale at once")
                after = at + 3 + len(starts)
                check = self.assert_envelope_falls_to_zero if len(starts) == 1 else self.assert_fades_within
                check(mix.render(at)[1], mix.render(after)[1], "stop_all")
                self.assertEqual(mix.number(after + 1), 0, "the fades rendered, nothing is live")
                self.assertEqual(mix.render(after + 2), (False, [0] * 16))
        # A stop_all with a clip's fade under way leaves its bytes in use: only stop_owner lets them go (the clip-lifetime guarantee).
        mix = self.run_script("make c 12000 3000", "clip 1 c 100 1", "render 320", "stop_all", "stop_owner 1", "free c",
                              "render 320", "active")
        self.assertEqual(mix.render(6), (False, [0] * 320))
        self.assertEqual(mix.number(7), 0)

    def test_hooks_are_the_mixer(self):
        # muse_apps.h's hooks for the voice task. A cut (a talk press, the dark) fades every voice out, those already fading too, and
        # the next chunk the voice task renders holds the whole fade: after it nothing is live.
        mix = self.run_script("make c 9000 4000", "hook_active", "clip 1 c 100 1", "tone 2 440 440 2000 saw 100", "hook_active",
                              "hook_render 320", "stop #1", "hook_cut", "hook_active", "active", "playing #0",
                              "hook_render 320", "hook_active", "render 320")
        self.assertEqual([mix.number(i) for i in (1, 4)], [0, 1])
        self.assertTrue(mix.render(5)[0])
        self.assertEqual([mix.number(i) for i in (8, 9, 10)], [1, 1, 0], "fading: live, and every id stale")
        live, fade = mix.render(11)
        self.assertTrue(live)
        self.assert_fades_within(mix.render(5)[1], fade, "cut")
        self.assertEqual(mix.number(12), 0)
        self.assertEqual(mix.render(13), (False, [0] * 320))

    def test_an_idle_mixer_takes_no_lock(self):
        # The voice task asks every 20 ms pass, mostly with nothing to play: those asks are a load of an atomic, not the lock.
        mix = self.run_script("locks", "render 320", "hook_render 320", "active", "hook_active", "stop_all", "hook_cut", "locks",
                              "tone 1 440 440 10 sine 100", "render 320", "render 320", "locks", "stop_owner 1", "locks")
        self.assertEqual(mix.number(7), 0)
        self.assertEqual(mix.number(11), 2, "a start and a render that played")
        self.assertEqual(mix.number(13), 1, "stop_owner always takes it: its return is the clip-lifetime guarantee")

    def test_stop_owner_is_safe_against_a_concurrent_render(self):
        # The clip-lifetime guarantee across tasks: once stop_owner returns, the voice task's render never reads the bytes again.
        self.assertEqual(self.run_script("race 3000", timeout=300).answers, ["ok"])

    def test_the_mixer_allocates_waits_and_logs_nothing(self):
        # Every mixer call holds the lock only for bounded work: no allocation, no log line, no wait but the lock and no Lua. A bench
        # build's lines (games Task 3, G4) are logged by the render once it has released the lock, and by nothing else.
        code = strip_c_comments((APPS / "apps_mix.c").read_text())
        plain = re.sub(r"#if CONFIG_MUSE_BENCH\n.*?#endif\n", "", code, flags=re.S)
        for word in ("malloc", "calloc", "realloc", "free(", "heap_caps", "apps_port_realloc", "ESP_LOG", "printf", "puts",
                     "vTaskDelay", "xSemaphore", "xQueue", "lua_", "luaL_", "apps_port_ui_lock"):
            with self.subTest(word):
                self.assertNotIn(word, plain)
        self.assertEqual(code.count("apps_port_mix_lock()"), code.count("apps_port_mix_unlock()"))
        render = code[code.index("bool apps_mix_render("):]
        render = render[:render.index("\n}\n")]
        self.assertEqual(code.count("ESP_LOG"), render.count("ESP_LOGI("))
        self.assertEqual(render.count("apps_port_mix_unlock();"), 1)
        self.assertGreater(render.index("ESP_LOGI("), render.index("apps_port_mix_unlock();"))

    def test_plain_state_is_in_psram_and_the_flag_and_lock_are_internal(self):
        # I2's rule: what only the lock guards is plain memory in PSRAM; what is atomic or a lock stays in internal RAM.
        mix = (APPS / "apps_mix.c").read_text()
        self.assertIn('#include "esp_attr.h"', mix)
        self.assertRegex(mix, r"(?m)^static EXT_RAM_BSS_ATTR struct \{\n(?:.*\n)*?\} M;")
        self.assertRegex(mix, r"(?m)^static atomic_bool s_active;")
        port = (APPS / "apps_port_idf.c").read_text()
        self.assertRegex(port, r"(?m)^static StaticSemaphore_t s_mix_lock_buf;")
        init = port[port.index("void apps_port_init(void) {"):]
        init = init[:init.index("\n}\n")]
        self.assertIn("s_mix_lock = xSemaphoreCreateMutexStatic(&s_mix_lock_buf);", init)


class MixBenchTest(unittest.TestCase):
    """The bench's lines (CONFIG_MUSE_BENCH, games Task 3's G4): each read under the mixer's lock and logged once it is released, on the
    muse_apps tag. The harness's clock is esp_timer_get_time, which a `clock` command sets; its log is stderr, and a line under the lock
    aborts."""

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = Path(tmp.name) / "fakes"
        stage.mkdir()
        fakes = {**FAKES,
                 "sdkconfig.h": "#pragma once\n#define CONFIG_SPIRAM 1\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_MUSE_BENCH 1\n",
                 "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
                 "esp_log.h": ("#pragma once\nvoid fake_esp_log(const char *tag, const char *fmt, ...);\n"
                               "#define ESP_LOGI(tag, ...) fake_esp_log(tag, __VA_ARGS__)\n")}
        for name, text in fakes.items():
            (stage / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_mix_bench_harness", [HARNESS, APPS / "apps_mix.c"], tmp.name,
                                      include_dirs=[stage, APPS, MUSE])

    def run_script(self, *commands):
        done = apps_host.run(self.harness, input="".join(f"{c}\n" for c in commands), timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr[-6000:])
        return [line for line in done.stderr.splitlines() if line.startswith("@log ")]

    def test_a_voice_logs_its_start_once_and_a_render_its_hold(self):
        # A tone asked for at 1.000 s and a clip at 1.004 s, first rendered at 1.0125 s: 13 and 9 ms (rounded), each once. The hold
        # is logged once for each number of voices a render has: two here, then one after the tone has ended; the clip's next
        # renders, of as many voices, log nothing.
        lines = self.run_script("clock 1000000", "tone 1 440 440 30 sine 100", "make c ramp 16000", "clock 1004000",
                                "clip 1 c 100 0", "clock 1012500", "render 320", "render 320", "clock 1100000",
                                "render 640", "render 320", "render 320")
        self.assertEqual([line for line in lines if " sound start " in line],
                         ["@log muse_apps sound start 13 ms", "@log muse_apps sound start 9 ms"])
        holds = [line for line in lines if " sound render " in line]
        self.assertEqual(len(holds), 2, lines)
        self.assertRegex(holds[0], r"^@log muse_apps sound render \d+ us \(320 frames, 2 voices\)$")
        self.assertRegex(holds[1], r"^@log muse_apps sound render \d+ us \(640 frames, 1 voices\)$")

    def test_a_cut_logs_the_voices_it_faded_at_the_next_render(self):
        # Two voices play, stop_all fades both: one line at the render that begins the fades, none at the next, and a cut with
        # nothing playing (or only tails) logs nothing.
        lines = self.run_script("stop_all", "render 320", "tone 1 440 440 500 sine 100", "tone 1 660 660 500 sine 100",
                                "render 320", "stop_all", "stop_all", "render 320", "render 320", "stop_all", "render 320")
        cuts = [line for line in lines if " sound cut " in line]
        fade_ms = header_number(APPS / "apps_mix.h", "APPS_MIX_FADE_MS")
        self.assertEqual(cuts, [f"@log muse_apps sound cut 2 voices ({fade_ms} ms fade)"], lines)

    def test_the_lines_are_pinned(self):
        source = (APPS / "apps_mix.c").read_text()
        self.assertIn('ESP_LOGI(TAG, "sound start %d ms", started_ms[i]);', source)
        self.assertIn('ESP_LOGI(TAG, "sound render %d us (%d frames, %d voices)"', source)
        self.assertIn('ESP_LOGI(TAG, "sound cut %d voices (%d ms fade)", cut, APPS_MIX_FADE_MS);', source)


if __name__ == "__main__":
    unittest.main()
