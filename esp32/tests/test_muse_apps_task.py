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
SOURCES = [HARNESS, TESTS / "muse_apps_fake_esp_timer.c", APPS / "apps_task.c", APPS / "apps_io.c", APPS / "apps_alarm.c",
           APPS / "apps_rt.c", APPS / "apps_ui.c", APPS / "apps_vm.c", APPS / "apps_json.c", APPS / "apps_pkg.c",
           APPS / "apps_suggest.c", APPS / "apps_svc.c"]
CONSOLE_SOURCES = [APPS / "apps_console.c", APPS / "apps_install.c"]

# What apps_task.c and muse_apps.h include of FreeRTOS, ESP-IDF and LVGL: each one is a line that includes the fakes.
FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/idf_additions.h", "freertos/queue.h", "freertos/semphr.h",
                "freertos/task.h", "esp_attr.h", "esp_err.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "lvgl.h"]
SDKCONFIG = "#define CONFIG_MUSE_APPS 1\n#define CONFIG_SPIRAM 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n"


def stage_fakes(directory):
    directory = Path(directory)
    for name in FAKE_HEADERS:
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text('#include "muse_apps_task_fakes.h"\n')
    (directory / "sdkconfig.h").write_text(SDKCONFIG)
    return directory


def build_harness(cls, name, console, defines=()):
    tmp = tempfile.TemporaryDirectory()
    cls.addClassCleanup(tmp.cleanup)
    stage = stage_fakes(Path(tmp.name) / "fake_headers")
    includes = [apps_host.LUA_SRC, APPS, MUSE, stage, TESTS]
    if not console:
        return apps_host.build(name, [*SOURCES, *apps_host.lua_sources()], tmp.name, include_dirs=includes,
                               defines=list(defines))
    cjson = apps_host.cjson_object()  # skips the class, alone, when the cJSON sources aren't here
    return apps_host.build(name, [*SOURCES, *CONSOLE_SOURCES, *apps_host.lua_sources()], tmp.name,
                           include_dirs=[*includes, apps_host.CJSON_DIR], defines=["HARNESS_CONSOLE", *defines],
                           objects=[cjson])


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

    # ---- Task 16, fix round 1 ----

    def test_a_lone_timer_far_away_does_not_make_the_loop_wait_longer_than_a_second(self):  # H, C22
        self.run_case("a_lone_far_timer_never_makes_the_loop_wait_long")

    def test_a_far_timer_does_not_delay_the_answer_of_a_run_that_settles(self):  # H
        self.run_case("a_far_timer_does_not_delay_a_settle")

    def test_a_timer_due_within_a_tick_is_waited_for_and_not_spun_on(self):  # G
        self.run_case("a_timer_due_within_a_tick_is_waited_for_not_spun_on")

    def test_the_wake_runs_the_overdue_timers_at_once(self):  # R: the supervisor posts a no-op event at the dark-to-lit edge
        self.run_case("the_wake_runs_the_overdue_timers_at_once")

    def test_a_throttle_warning_raised_by_a_pass_joins_the_launch_warnings(self):  # K, and the pass that makes no call
        self.run_case("a_throttle_warning_from_a_pass_joins_the_launch_warnings")

    def test_cpu_pct_and_the_duty_cycle_count_the_same_time(self):  # B: one booking, read twice
        self.run_case("cpu_pct_and_the_duty_cycle_agree")

    # ---- Task 17: the system's alarms, told by the loop ----

    def test_an_alarm_of_the_running_app_fronts_it_and_calls_alarm(self):  # the esp_timer task marks, rings, wakes and posts
        self.run_case("an_alarm_of_the_running_app_fronts_it_and_calls_it")

    def test_an_alarms_delivery_does_not_depend_on_the_post(self):  # C22, J: any event's round delivers it, not only a timeout's
        self.run_case("a_delivery_does_not_depend_on_the_post")

    def test_a_firing_with_no_post_and_no_event_is_told_when_the_loops_second_ends(self):  # C22: the wait's end delivers too
        self.run_case("a_firing_with_no_post_and_no_event_is_told_when_the_loops_second_ends")

    def test_an_alarm_is_not_posted_to_a_queue_that_is_full_to_the_requests_room_and_is_told_later(self):  # C22, M, R
        self.run_case("an_alarm_posted_to_a_full_queue_is_told")

    def test_a_flood_of_firings_posts_one_event_and_leaves_room_for_requests(self):  # R
        self.run_case("a_flood_of_firings_posts_one_event_and_leaves_room_for_requests")

    def test_alarms_are_told_in_the_order_they_fired(self):
        self.run_case("alarms_are_told_in_the_order_they_fired")

    def test_an_alarm_during_a_voice_turn_fronts_the_app_after_it(self):  # C11
        self.run_case("an_alarm_during_a_voice_turn_fronts_the_app_after_it")

    def test_an_alarm_is_told_while_the_screen_sleeps(self):
        self.run_case("an_alarm_is_told_while_the_screen_sleeps")

    def test_a_crash_in_alarm_ends_the_ring(self):
        self.run_case("a_crash_in_alarm_ends_the_ring")

    def test_an_alarm_cancelled_between_the_copy_and_the_take_is_not_told(self):  # the same alarm is never told twice
        self.run_case("an_alarm_cancelled_before_it_is_taken_is_not_told")

    def test_a_run_for_alarms_that_are_gone_replaces_nothing(self):  # D: the app task asks too, before it replaces the app that runs
        self.run_case("a_run_for_alarms_that_are_gone_replaces_nothing")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


class AppTaskTickTest(HarnessCase):
    """The loop's waits on a build whose tick is 10 ms (CONFIG_FREERTOS_HZ=100, ESP-IDF's default: the board's is 1000), where a
    wait of 1 to 9 ms is no tick at all (pdMS_TO_TICKS rounds down) and the loop must still wait."""

    @classmethod
    def setUpClass(cls):
        cls.harness = build_harness(cls, "muse_apps_task_harness_100hz", console=False, defines=["MUSE_FAKE_TICK_HZ=100"])

    def test_timers_that_are_due_never_make_the_loop_spin_at_100_hz(self):
        self.run_case("due_timers_never_make_the_loop_spin")

    def test_a_timer_due_within_a_tick_is_waited_for_at_100_hz(self):
        self.run_case("a_timer_due_within_a_tick_is_waited_for_not_spun_on")

    def test_timers_run_on_the_app_task_at_100_hz(self):
        self.run_case("timers_run_on_the_app_task")


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

    # ---- Task 17: an alarm for an app that doesn't run ----

    def test_an_alarm_starts_a_stopped_app_and_calls_it_after_start(self):  # X11: the worker loads it; start() may arm the name again
        self.run_case("an_alarm_starts_a_stopped_app_and_calls_it_after_start")

    def test_an_alarm_replaces_the_app_that_runs_with_its_own(self):
        self.run_case("an_alarm_replaces_the_app_that_runs_with_its_own")

    def test_an_alarm_for_a_missing_app_is_dropped_and_its_ring_ends(self):  # "alarm <name> for missing app <id>"
        self.run_case("an_alarm_for_a_missing_app_is_dropped_and_its_ring_ends")

    def test_an_alarm_launch_behind_a_long_job_of_the_worker_is_waited_for(self):  # D: no give-up of a launch the worker has
        self.run_case("an_alarm_launch_behind_a_long_job_is_waited_for")

    def test_a_launch_the_worker_never_reports_is_given_up_at_the_backstop_and_then_runs_nothing(self):  # D
        self.run_case("an_unreported_alarm_launch_is_given_up_and_runs_nothing")

    def test_an_alarm_cancelled_while_its_launch_waits_launches_nothing(self):  # D: the revoked launch
        self.run_case("an_alarm_cancelled_while_its_launch_waits_launches_nothing")

    def test_a_refused_alarm_launch_is_asked_for_again_a_second_later(self):  # not at every round
        self.run_case("a_refused_alarm_launch_is_asked_for_again")

    def test_a_refused_alarm_launch_is_given_up_after_ten_seconds(self):  # D: every wait bounded
        self.run_case("a_refused_alarm_launch_is_given_up_after_ten_seconds")

    def test_a_failed_alarm_launch_drops_only_the_firings_it_was_for(self):  # D
        self.run_case("a_failed_alarm_launch_drops_only_the_firings_it_was_for")

    def test_a_failed_alarm_launch_drops_every_firing_it_was_asked_for(self):  # D: the identity is the last of them
        self.run_case("a_failed_alarm_launch_drops_every_firing_it_was_asked_for")

    def test_a_result_for_another_launch_is_ignored(self):  # D: the identity is what ties a result to its launch
        self.run_case("a_result_for_another_launch_is_ignored")

    def test_an_app_that_crashes_in_start_when_its_alarm_launches_it_loses_its_alarms(self):  # P1
        self.run_case("a_crash_in_start_of_an_alarm_launch_cancels_the_alarms")

    def test_an_app_that_crashes_in_the_alarm_it_was_launched_for_loses_its_alarms(self):  # P1
        self.run_case("a_crash_in_the_launching_alarm_cancels_the_alarms")

    def test_a_crash_after_the_launch_costs_the_app_nothing(self):  # P1's window is the first round after the launch
        self.run_case("a_crash_after_the_launch_costs_the_app_nothing")

    def test_alarms_of_two_stopped_apps_are_told_one_after_the_other(self):
        self.run_case("alarms_of_two_stopped_apps_are_told_one_after_the_other")

    def test_a_launch_whose_alarm_went_does_not_hold_back_the_next_apps(self):  # D: one at a time, and none is remembered for nothing
        self.run_case("a_launch_whose_alarm_went_does_not_hold_back_the_next_apps")

    def test_a_crash_in_alarm_makes_the_loop_look_again_for_the_next_app(self):  # the pass is cut short by an app's end
        self.run_case("a_crash_in_alarm_makes_the_loop_look_again_for_the_next_app")

    def test_a_launch_given_up_lets_the_next_apps_be_asked_for_in_the_same_round(self):  # the pass is cut short by a give-up
        self.run_case("a_launch_given_up_lets_the_next_apps_be_asked_for_in_the_same_round")

    def test_a_launch_given_up_at_the_backstop_lets_the_next_apps_be_asked_for_in_the_same_round(self):  # and so is the backstop's
        self.run_case("a_launch_given_up_at_the_backstop_lets_the_next_apps_be_asked_for_in_the_same_round")


if __name__ == "__main__":
    unittest.main()
