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

"""Link's app.control tap and drag: components/muse/apps/apps_touch.c.

Touch runs tests/muse_apps_touch_harness.c: the real apps_touch.c, with the real package rules (apps_pkg.c, for the id) and the
real cJSON, over fakes of the I/O worker (jobs run when the case says), the LVGL task (a frame and an input read at every poll of
the job, on a clock the case moves), the touch player (muse_ui.c's guarded drag, played as the real one plays it: test_muse_bench.py
has that one) with LVGL's part (the events a widget hears, a long press, a box's scroll and throw), the screen's sleep, the voice
turn, the app layer's place, cards and hit test, and the app that runs. Every case checks that done ran exactly once (or not at all
for a refusal), that every block it allocated has been freed, and that nothing is left queued or playing. Needs the cJSON sources:
CJSON_SOURCE_DIR (~/.cache/muse-cjson), or an IDF build's managed_components.

Pins checks the numbers the guide and the contract quote (D12) against the headers they come from.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_touch_harness.c"
SOURCES = [HARNESS, APPS / "apps_touch.c", APPS / "apps_result.c", APPS / "apps_pkg.c", APPS / "apps_json.c", APPS / "apps_vm.c",
           APPS / "apps_suggest.c", APPS / "apps_ui.c"]  # apps_ui.c for a swipe's direction's name (apps_ui_dir_name)

# What apps_touch.c and the headers it includes need of ESP-IDF, FreeRTOS, LVGL and the input task: the little they use, each
# declared here and defined by the harness.
STAGED = {
    "sdkconfig.h": "#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n",
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n",
    "esp_attr.h": "#pragma once\n#define EXT_RAM_BSS_ATTR\n",
    "esp_log.h": ("#pragma once\n"
                  "void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));\n"
                  "#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)\n"),
    "freertos/FreeRTOS.h": ("#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\ntypedef int BaseType_t;\n"
                            "typedef unsigned UBaseType_t;\ntypedef void *TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n"),
    # A tick is a millisecond here, and the harness's clock moves on by the ticks a delay sleeps.
    "freertos/task.h": ("#pragma once\n#include \"freertos/FreeRTOS.h\"\n#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n"
                        "void vTaskDelay(TickType_t ticks);\n"),
    "muse_input.h": "#pragma once\nvoid muse_input_wake(const char *why);\n",
    "lvgl.h": "#pragma once\n#include <stdint.h>\ntypedef struct lv_obj lv_obj_t;\n",
}


class Touch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        staged = Path(tmp.name) / "staged"
        for name, text in STAGED.items():
            (staged / name).parent.mkdir(parents=True, exist_ok=True)
            (staged / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_touch_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[staged, APPS, MUSE, ROOT / "components/lua/src", apps_host.CJSON_DIR],
                                      objects=[cjson])

    def run_case(self, case):
        try:
            done = apps_host.run(self.harness, case, timeout=120)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 120 s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-4000:])
        return done.stdout

    def test_refusals(self):  # the ruling's codes at the call (ranges, id, one at a time, memory): nothing queued, done never called
        self.run_case("refusals")

    def test_a_tap_on_a_button(self):  # woken, admitted at the take, kept every read; pressed and heard named; the events tagged
        self.run_case("tap_on_a_button")

    def test_a_long_press_and_a_tap_on_the_layer(self):  # 500 ms is a long press with no tap; the layer presses nothing: null
        self.run_case("long_press_and_the_layer")

    def test_a_drag_scrolls_the_list(self):  # the row hears press and release, no tap; the box's y once its throw has come to rest
        self.run_case("drag_scrolls_the_list")

    def test_a_list_still_moving_is_reported(self):  # the settle's bound: its y then, and a warning
        self.run_case("a_list_still_moving_is_reported")

    def test_an_app_that_is_not_running(self):  # not_running, before any wake
        self.run_case("not_running")

    def test_the_guards_at_the_take(self):  # a card, a turn, gone, hung, busy, covered, behind, a finger, no input: nothing pressed
        self.run_case("refused_at_the_take")

    def test_a_dark_screen_wakes_and_is_touched_when_lit(self):  # the wake, then the first lit frame hands it on
        self.run_case("dark_then_lit")

    def test_a_screen_that_stays_dark_times_out(self):  # 5 s, unavailable, nothing handed on; a late frame hands on nothing
        self.run_case("dark_past_the_deadline")

    def test_a_touch_not_taken_in_time_is_withdrawn(self):  # the player never read: withdrawn at the screen's deadline
        self.run_case("withdrawn_at_the_deadline")

    def test_the_cuts_mid_touch(self):  # behind, a new app, a card, a turn, an edge swipe (played too), covered, a finger: no tap
        self.run_case("cut_mid_touch")

    def test_a_drag_that_swipes_is_heard_with_its_direction(self):  # swipes: press, swipe <dir>, release, no tap; front; slow taps
        self.run_case("drag_that_swipes_is_heard_with_its_direction")

    def test_the_tag_is_only_the_touchs(self):  # tagged from the admit to the end, never before or after or for a finger's
        self.run_case("the_tag_is_only_the_touchs")

    def test_heard_is_capped(self):  # the first 8, and a warning that counts the rest
        self.run_case("heard_more_than_listed")

    def test_a_job_that_waited_too_long_does_nothing(self):  # busy, before any wake or touch
        self.run_case("waited_too_long")

    def test_a_touch_that_never_ends_is_given_up(self):  # the whole deadline: slow; busy until its end; the next read cuts it
        self.run_case("abandoned")

    def test_every_allocation_refused_in_turn(self):  # done exactly once, nothing leaked, whichever allocation fails
        self.run_case("every_allocation_refused")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])


class Source(unittest.TestCase):
    """What the harness's fakes can't show: where the touch's state lives, and that the job is the only waiter."""

    def setUp(self):
        self.source = (APPS / "apps_touch.c").read_text()

    def test_the_state_is_psram_and_the_word_internal(self):  # I2: plain state in PSRAM; the handshake's atomics in internal RAM
        self.assertRegex(self.source, r"static EXT_RAM_BSS_ATTR struct \{[^}]*char id\[APPS_ID_MAX \+ 1\];")
        for word in ("static atomic_int s_state;", "static atomic_bool s_claimed;", "static _Atomic int64_t s_deadline_ms;"):
            self.assertIn(word, self.source)
            self.assertNotIn("EXT_RAM_BSS_ATTR " + word[len("static "):], self.source)

    def test_the_lvgl_side_never_waits(self):  # the guard runs inside LVGL's input read: no delay, no lock, no wait there
        lvgl_side = self.source[self.source.index("/* ---- The LVGL task's side ---- */"):
                                self.source.index("/* ---- The job (the I/O worker's) ---- */")]
        for call in ("vTaskDelay", "apps_io_assist", "xSemaphoreTake", "apps_port_realloc", "cJSON_"):
            self.assertNotIn(call, lvgl_side)

    def test_the_wake_is_a_snapshots(self):  # the same wake as app.control snapshot's, always, before the request
        play = self.source[self.source.index("static bool play("):]
        self.assertLess(play.index('muse_input_wake("touch");'), play.index("atomic_store(&s_state, T_ASKED);"))


class Pins(unittest.TestCase):
    """D12: the numbers the touch's contract quotes are its headers'."""

    def setUp(self):
        self.header = (MUSE / "muse_apps.h").read_text()
        self.ui = (MUSE / "muse_ui.h").read_text()

    def define(self, name, text=None):
        m = re.search(rf"^#define {name} (\d+)", text if text is not None else self.header, re.M)
        self.assertIsNotNone(m, name)
        return int(m.group(1))

    def test_the_ranges_are_the_players(self):
        self.assertEqual(self.define("MUSE_APPS_TOUCH_MS_MIN"), self.define("MUSE_UI_DRAG_MS_MIN", self.ui))
        self.assertEqual(self.define("MUSE_APPS_TOUCH_MS_MAX"), self.define("MUSE_UI_DRAG_MS_MAX", self.ui))
        self.assertEqual(self.define("MUSE_APPS_TOUCH_XY_MAX"), 465)  # the 466 px screen's last pixel

    def test_the_messages_quote_the_ranges(self):
        lo, hi, xy = (self.define(n) for n in ("MUSE_APPS_TOUCH_MS_MIN", "MUSE_APPS_TOUCH_MS_MAX", "MUSE_APPS_TOUCH_XY_MAX"))
        tap = re.search(r'#define MUSE_APPS_TOUCH_BAD_TAP "([^"]+)"', self.header).group(1)
        drag = re.search(r'#define MUSE_APPS_TOUCH_BAD_DRAG "([^"]+)"', self.header).group(1)
        for text in (tap, drag):
            self.assertIn(f"0 to {xy}", text)
            self.assertIn(f"ms {lo} to {hi}", text)

    def test_the_tap_and_long_press(self):
        # The bench's tap holds 80 ms; LVGL's long press is 400 ms at a read every 15 ms (sdkconfig.defaults' refresh period),
        # so a press of 400 ms can end before the read that would fire it: 500 is always one.
        self.assertEqual(self.define("MUSE_APPS_TOUCH_TAP_MS"), 80)
        self.assertIn("A tap and a long press hold the point (x, y) for 80 and 900 ms", self.ui)
        self.assertEqual(self.define("MUSE_APPS_TOUCH_LONG_MS"), 500)
        self.assertGreaterEqual(self.define("MUSE_APPS_TOUCH_LONG_MS"), 400 + 15)

    def test_the_fakes_gesture_is_lvgl_9_5_0s(self):
        # The fake read's swipe uses the lv harness's gesture numbers, which test_muse_apps_lv.py pins to LVGL 9.5.0's.
        lv = (TESTS / "muse_apps_lv_harness.c").read_text()
        touch = HARNESS.read_text()
        for name in ("FAKE_GESTURE_LIMIT", "FAKE_GESTURE_MIN_VELOCITY"):
            with self.subTest(name):
                self.assertEqual(self.define(name, touch), self.define(name, lv))

    def test_the_deadlines_fit_app_controls(self):
        link = (ROOT / "main/apps_link.c").read_text()
        control = int(re.search(r"#define CONTROL_TIMEOUT_MS (\d+)", link).group(1))
        total = self.define("MUSE_APPS_TOUCH_TOTAL_MS")
        self.assertLess(total, control)
        touch = (APPS / "apps_touch.h").read_text()
        min_left = self.define("APPS_TOUCH_MIN_LEFT_MS", touch)
        self.assertLess(self.define("MUSE_APPS_TOUCH_SCREEN_MS") + self.define("MUSE_APPS_TOUCH_MS_MAX")
                        + self.define("MUSE_APPS_TOUCH_SETTLE_MS"), min_left)
        self.assertLess(min_left, total)


if __name__ == "__main__":
    unittest.main()
