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

"""apps_task, the app task of Muse apps: its services, its supervisor and the order in which an app ends.

tests/muse_apps_task_harness.c runs the real apps_task.c over the real runtime (apps_rt, apps_ui, apps_vm and Lua),
with the app task, an LVGL task and the caller as threads: the fake FreeRTOS of tests/muse_apps_task_fakes.h is made of
pthreads there. Its FreeRTOS and ESP-IDF headers are staged as one line each that includes the fakes, as
test_muse_apps_lv.py stages apps_lv.c's. apps_lv is a fake that logs what is done to the screen and draws every image
object from the package's bytes each frame, so a package freed before the layer's reset is an AddressSanitizer report.
What these tests can't show is the board's own: PSRAM, the real LVGL and its timers, the USB console and the stack the
app task really uses; the board check covers the boot, the screens and the memory.
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
HARNESS = TESTS / "muse_apps_task_harness.c"
SOURCES = [HARNESS, APPS / "apps_task.c", APPS / "apps_rt.c", APPS / "apps_ui.c", APPS / "apps_vm.c", APPS / "apps_json.c",
           APPS / "apps_pkg.c", APPS / "apps_suggest.c"]

# What apps_task.c and muse_apps.h include of FreeRTOS, ESP-IDF and LVGL: each one is a line that includes the fakes.
FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/idf_additions.h", "freertos/queue.h", "freertos/semphr.h",
                "freertos/task.h", "esp_err.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "lvgl.h"]
SDKCONFIG = "#define CONFIG_MUSE_APPS 1\n#define CONFIG_SPIRAM 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n"


def stage_fakes(directory):
    directory = Path(directory)
    for name in FAKE_HEADERS:
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text('#include "muse_apps_task_fakes.h"\n')
    (directory / "sdkconfig.h").write_text(SDKCONFIG)
    return directory


class AppTaskTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = stage_fakes(Path(tmp.name) / "fake_headers")
        cls.harness = apps_host.build("muse_apps_task_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[apps_host.LUA_SRC, APPS, MUSE, stage, TESTS])

    def run_case(self, case, timeout=120):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-3000:] + done.stderr[-12000:])
        return done

    # ---- The brief's ----

    def test_the_task_and_its_queue(self):
        self.run_case("task_shape")

    def test_a_run_and_its_status(self):
        self.run_case("run_and_status")

    def test_compile_checks_every_file_and_runs_nothing(self):
        self.run_case("compile_checks_every_file")

    def test_overflow_warnings(self):  # run step 7, D1, C9
        self.run_case("overflow_warnings")

    def test_a_voice_turn_hides_the_app_and_brings_it_back(self):  # C11
        self.run_case("a_voice_turn_hides_the_app")

    def test_the_launcher_lists_the_resident_app(self):
        self.run_case("the_launcher_lists_the_resident_app")

    # ---- The rulings' ----

    def test_stop_tears_down_in_the_rulings_order(self):  # the teardown order (Task 11's ruling)
        self.run_case("stop_tears_down_in_order")

    def test_a_reset_that_comes_late_parks_the_images(self):  # the reset's invariants: the ack's timeout
        self.run_case("a_late_reset_parks_the_images")

    def test_a_new_app_is_published_after_the_old_ones_reset(self):  # the reset's invariants
        self.run_case("a_new_run_replaces_the_app")

    def test_a_crash_in_start_is_contained(self):  # crash containment follows the teardown order
        self.run_case("a_crash_in_start_is_contained")

    def test_a_crash_in_a_callback_is_contained(self):
        self.run_case("a_crash_in_a_callback_is_contained")

    def test_a_crash_at_main_luas_top_level_is_contained(self):  # Task 8's F2: a CRASHED runtime from apps_rt_new
        self.run_case("a_crash_at_the_top_level_is_contained")

    def test_a_main_that_does_not_compile(self):
        self.run_case("a_main_that_does_not_compile_fails_the_run")

    def test_a_hung_call_is_abandoned_and_calls_fail_fast(self):  # C7, the supervisor's 3000 ms rule
        self.run_case("a_hung_call_is_abandoned")

    def test_a_stop_that_hangs_is_abandoned(self):
        self.run_case("a_hung_stop_is_abandoned")

    def test_a_launch_never_waits(self):  # C5
        self.run_case("a_launch_never_waits")

    def test_the_layout_waits_for_the_screen_with_a_bound(self):  # C8
        self.run_case("the_layout_waits_for_the_screen")

    def test_a_right_swipe_only_backgrounds(self):  # the close callback only backgrounds
        self.run_case("a_right_swipe_only_backgrounds")

    def test_an_answer_after_the_caller_gave_up_writes_nothing(self):  # C7: every wait bounded
        self.run_case("a_late_answer_writes_nothing")

    def test_touches_past_a_full_queue_are_counted(self):
        self.run_case("touches_past_a_full_queue_are_counted")

    # ---- Fix round 1 ----

    def test_parking_is_bounded_to_one_package(self):  # F1
        self.run_case("parking_is_bounded")

    def test_a_replace_whose_stop_hangs_drops_the_run(self):  # F2
        self.run_case("a_replace_whose_stop_hangs_drops_the_run")

    def test_a_back_request_in_the_turns_last_frame_gets_no_show(self):  # F5 (a)
        self.run_case("a_back_request_in_the_turns_last_frame")

    def test_a_front_asked_during_a_turn_waits_for_the_table(self):  # F5 (b)
        self.run_case("a_front_asked_during_a_turn_waits_for_the_table")

    def test_a_new_app_drops_an_old_crash_card(self):  # F6
        self.run_case("a_new_app_drops_an_old_crash_card")

    def test_a_new_app_drops_an_old_hung_card(self):  # F6, the supervisor's card
        self.run_case("a_new_app_drops_an_old_hung_card")

    def test_cpu_pct_follows_idle_time(self):  # F9, T9 (1)
        self.run_case("cpu_pct_follows_idle_time")

    def test_dropped_is_read_with_its_base(self):  # F10
        self.run_case("dropped_is_read_with_its_base")

    def test_a_front_asked_during_a_turn_comes_after_it(self):  # T1, C11's want_front
        self.run_case("a_front_asked_during_a_turn_comes_after_it")

    def test_a_card_waits_for_the_turn_to_end(self):  # T9 (2)
        self.run_case("a_card_waits_for_the_turn_to_end")

    def test_a_turn_after_the_app_ended_fronts_nothing(self):  # T9 (3)
        self.run_case("a_turn_after_the_app_ended_fronts_nothing")

    def test_stop_reports_why_it_failed(self):  # F3
        self.run_case("stop_reports_failure")

    def test_the_tick_says_when_the_apps_cover(self):  # T9 (1), F7
        self.run_case("the_tick_says_when_the_apps_cover")

    # ---- Fix round 2 ----

    def test_a_turn_during_a_new_apps_load_fronts_nothing(self):  # N1
        self.run_case("a_turn_during_a_new_apps_load_fronts_nothing")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


if __name__ == "__main__":
    unittest.main()
