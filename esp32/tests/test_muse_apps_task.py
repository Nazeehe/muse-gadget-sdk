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

tests/muse_apps_task_harness.c runs the real apps_task.c over the real runtime (apps_rt, apps_ui, apps_vm and Lua) and
the real I/O worker (apps_io.c), with the app task, an LVGL task, the worker and the caller as threads: the fake
FreeRTOS of tests/muse_apps_task_fakes.h is made of pthreads there. Its FreeRTOS and ESP-IDF headers are staged as one
line each that includes the fakes, as test_muse_apps_lv.py stages apps_lv.c's. apps_lv is a fake that logs what is done
to the screen and draws every image object from the package's bytes each frame, so a package freed before the layer's
reset is an AddressSanitizer report. AppTaskTest builds it without the console (no cJSON: Task 12's cases and the
worker's); AppTaskStoreTest builds it with the console's launch job and the RAM store (apps_console.c, apps_install.c,
the real cJSON, HARNESS_CONSOLE), for the cases that need them.
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
SOURCES = [HARNESS, APPS / "apps_task.c", APPS / "apps_io.c", APPS / "apps_rt.c", APPS / "apps_ui.c", APPS / "apps_vm.c",
           APPS / "apps_json.c", APPS / "apps_pkg.c", APPS / "apps_suggest.c", APPS / "apps_svc.c"]
CONSOLE_SOURCES = [APPS / "apps_console.c", APPS / "apps_install.c"]

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


def build_harness(cls, name, console):
    tmp = tempfile.TemporaryDirectory()
    cls.addClassCleanup(tmp.cleanup)
    stage = stage_fakes(Path(tmp.name) / "fake_headers")
    includes = [apps_host.LUA_SRC, APPS, MUSE, stage, TESTS]
    if not console:
        return apps_host.build(name, [*SOURCES, *apps_host.lua_sources()], tmp.name, include_dirs=includes)
    cjson = apps_host.cjson_object()  # skips the class, alone, when the cJSON sources aren't here
    return apps_host.build(name, [*SOURCES, *CONSOLE_SOURCES, *apps_host.lua_sources()], tmp.name,
                           include_dirs=[*includes, apps_host.CJSON_DIR], defines=["HARNESS_CONSOLE"], objects=[cjson])


class HarnessCase(unittest.TestCase):
    harness = None

    def run_case(self, case, timeout=120):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-3000:] + done.stderr[-12000:])
        return done


class AppTaskTest(HarnessCase):
    """Task 12's cases and the I/O worker's, without the console: no cJSON needed, so they never skip for want of it."""

    @classmethod
    def setUpClass(cls):
        cls.harness = build_harness(cls, "muse_apps_task_harness", console=False)

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

    def test_the_launcher_lists_the_running_app(self):  # F1: the app that runs, where no store lists it
        self.run_case("the_launcher_lists_the_running_app")

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

    # ---- Task 13 ----

    def test_a_turn_brings_back_the_app_that_was_in_front(self):  # Task 12's last review
        self.run_case("a_turn_brings_back_the_app_that_was_in_front")

    def test_a_message_gets_its_reply(self):  # apps_task_message
        self.run_case("a_message_gets_its_reply")

    def test_a_message_after_its_caller_gave_up(self):
        self.run_case("a_message_after_its_caller_gave_up")

    def test_the_io_worker_runs_jobs_in_order(self):  # apps_io: made on demand, gone when idle
        self.run_case("io_runs_jobs_in_order")

    def test_the_io_worker_refuses_when_short(self):  # apps_io's admission
        self.run_case("io_refuses_when_short")

    # ---- Task 13's pre-review fix ----

    def test_the_io_worker_refuses_a_fragmented_heap(self):  # P1: the largest free internal block
        self.run_case("io_refuses_when_fragmented")

    # ---- Task 13's fix round 1 ----

    def test_the_io_worker_takes_a_job_that_came_as_it_timed_out(self):  # T3: the lost-wakeup guard
        self.run_case("io_takes_a_job_that_came_as_it_timed_out")

    def test_an_echo_waits_outside_the_call(self):  # F3: a print only queues; the drain comes after the call
        self.run_case("an_echo_waits_outside_the_call")

    # ---- Task 16: timers and the duty cycle on the loop ----

    def test_timers_run_on_the_app_task_and_the_loop_wakes_for_them(self):
        self.run_case("timers_run_on_the_app_task")

    def test_a_hung_timer_callback_is_abandoned(self):  # each callback is bracketed for the supervisor
        self.run_case("a_hung_timer_callback_is_abandoned")

    def test_timers_that_are_due_never_make_the_loop_spin(self):  # a wait of a tick at least, or the idle task starves
        self.run_case("due_timers_never_make_the_loop_spin")

    def test_dark_pauses_the_timers_and_the_wake_runs_each_once(self):
        self.run_case("dark_pauses_the_timers_and_the_wake_runs_each_once")

    def test_a_timer_error_crashes_the_app_and_shows_the_card(self):
        self.run_case("a_timer_error_crashes_the_app_and_shows_the_card")

    def test_the_duty_cycle_warns_the_status_then_kills(self):
        self.run_case("the_duty_cycle_warns_the_status_then_kills")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


class AppTaskStoreTest(HarnessCase):
    """The cases that need the console's launch job and the RAM store: the harness built with them and the real cJSON
    (HARNESS_CONSOLE). The class skips, alone, when the cJSON sources aren't here."""

    @classmethod
    def setUpClass(cls):
        cls.harness = build_harness(cls, "muse_apps_task_store_harness", console=True)

    # ---- Task 13's pre-review fix ----

    def test_a_pick_runs_a_stored_app_that_stopped(self):  # P2: the launcher lists it; the worker runs it
        self.run_case("a_pick_runs_a_stored_app_that_stopped")

    def test_a_refused_launch_logs_once(self):  # P2
        self.run_case("a_refused_launch_logs_once")

    # ---- Task 13's fix round 1 ----

    def test_the_launcher_lists_the_store(self):  # F1: most recent first, four rows, the running app first
        self.run_case("the_launcher_lists_the_store")

    def test_the_launcher_keeps_apps_that_ended(self):  # T4: installed, crashed, no app, hung and back
        self.run_case("the_launcher_keeps_apps_that_ended")

    def test_two_launches_of_one_id_run_it_once(self):  # F10: the second fronts it
        self.run_case("two_launches_of_one_id_run_it_once")


if __name__ == "__main__":
    unittest.main()
