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

"""apps_port_idf.c, the device's port of Muse apps: the console echo's cap and queue, the UI lock's breach log, the store's map
behind the I/O worker's ops, and sys's battery and Wi-Fi.

tests/muse_apps_port_harness.c runs the real apps_port_idf.c on the host over fakes: its ESP-IDF and FreeRTOS headers
are staged as one line each that includes tests/muse_apps_task_fakes.h (the harness defines what the port calls), and
psa/crypto.h as the few declarations it uses. What these tests can't show is the board's own: how long the console
really takes for a line (Task 13 measures it).
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
HARNESS = TESTS / "muse_apps_port_harness.c"

FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h", "esp_attr.h", "esp_err.h",
                "esp_heap_caps.h", "esp_log.h", "esp_timer.h"]
PSA = """#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length);
"""


def stage_fakes(directory):
    directory = Path(directory)
    for name in FAKE_HEADERS:
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text('#include "muse_apps_task_fakes.h"\n')
    (directory / "psa").mkdir(parents=True, exist_ok=True)
    (directory / "psa/crypto.h").write_text(PSA)
    (directory / "sdkconfig.h").write_text("#define CONFIG_SPIRAM 1\n")
    return directory


class AppPortTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        stage = stage_fakes(Path(tmp.name) / "fake_headers")
        cls.harness = apps_host.build("muse_apps_port_harness", [HARNESS, APPS / "apps_port_idf.c", APPS / "apps_values.c"], tmp.name,
                                      include_dirs=[APPS, MUSE, stage, TESTS])

    def run_case(self, case, timeout=60):
        try:
            done = subprocess.run([str(self.harness), case], capture_output=True, text=True, timeout=timeout,
                                  env={"TMPDIR": self.tmp})
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s: a breach blocked")
        self.assertEqual(done.returncode, 0, done.stderr[-6000:])

    def test_the_echo_buckets_arithmetic(self):  # F8
        self.run_case("echo_bucket")

    def test_the_console_echo_is_capped(self):  # F8
        self.run_case("echo_lines")

    def test_an_echo_only_queues_until_the_drain(self):  # Task 13's F3: never a wait for the console inside a call
        self.run_case("echo_queues_until_the_drain")

    def test_the_echo_queue_is_bounded(self):  # Task 13's F3
        self.run_case("echo_queue_is_bounded")

    def test_a_drain_holds_the_console_once(self):  # Task 13's N1: one wait for the whole queue, its lines together
        self.run_case("echo_drain_holds_the_console")

    def test_each_kind_of_breach_is_logged_once(self):  # F11
        self.run_case("breach_kinds")

    def test_the_psram_floor_refuses_growth_and_never_a_shrink(self):  # C1: a clean out of memory, never LVGL's abort
        self.run_case("psram_floor")

    def test_the_screen_is_dark_when_the_ui_says_so(self):  # Task 16: timers pause while it is
        self.run_case("dark_follows_the_ui")

    def test_the_wall_clock_counts_when_the_sntp_helper_says_so(self):  # Task 16: muse_time_valid(), not a year of its own
        self.run_case("the_wall_clock_counts_when_the_helper_says")

    def test_alarms_and_sound_go_to_their_owners(self):  # Task 17: apps_alarm.c's table and ring, the voice task's chirp
        self.run_case("alarms_and_sound_go_to_their_owners")

    # ---- Task 18: the store, sys's battery and Wi-Fi, and what goes to the app task ----

    def test_the_store_round_trips_through_the_worker(self):  # X11: saves and loads are the I/O worker's ops
        self.run_case("store_round_trip_through_the_worker")

    def test_the_store_serves_in_the_order_of_the_calls(self):  # a restart's load comes after the stop's save
        self.run_case("store_ops_run_in_the_order_of_the_calls")

    def test_a_load_the_worker_never_answers_gives_up_and_leaks_nothing(self):  # the load's bound, the abandoned request
        self.run_case("store_load_gives_up_when_the_worker_is_silent")

    def test_an_answer_that_lands_at_the_deadline_is_kept(self):  # whichever mark of the request came first decides
        self.run_case("store_load_answered_at_the_deadline")

    def test_a_refused_store_call_leaves_nothing_held(self):  # no op, no memory, an id or an object the runtime never makes
        self.run_case("store_refusals")

    def test_the_store_keeps_16_apps_and_drops_the_oldest_saved(self):
        self.run_case("store_keeps_16_apps_and_drops_the_oldest_saved")

    def test_the_loads_request_is_a_small_block_of_internal_ram(self):  # C: no read-modify-write in PSRAM
        self.run_case("store_load_request_is_a_small_internal_block")

    def test_a_save_the_worker_refuses_is_kept_and_offered_again(self):  # P1: kept in PSRAM, read back to a load, offered at the tick
        self.run_case("store_a_save_the_worker_refuses_is_kept")

    def test_kept_saves_are_one_for_each_app_and_bounded(self):  # P1: 16 slots, a tick offers until the ring is full
        self.run_case("store_kept_saves_are_one_for_each_app_and_bounded")

    def test_a_kept_save_comes_after_the_one_the_worker_took(self):  # P1: the order of one app's saves
        self.run_case("store_a_kept_save_comes_after_the_one_the_worker_took")

    def test_a_load_begins_the_app_tasks_call_again_once_it_has_waited(self):  # M: the wait isn't the app's
        self.run_case("store_load_begins_the_call_again")

    def test_battery_and_wifi_are_the_systems(self):  # C33's device half: muse_state's power, muse_wifi's status, no log
        self.run_case("battery_and_wifi_are_the_systems")

    def test_show_keep_awake_and_exit_are_the_app_tasks(self):  # C10, C11
        self.run_case("show_keep_awake_and_exit_go_to_the_app_task")

    # ---- Task 20: the values' backend picked by the store's mode, its failures, the alarms' records ----

    def test_values_go_to_apps_nvs_on_flash(self):
        self.run_case("values_go_to_apps_nvs_on_flash")

    def test_a_backend_failure_is_said_once_a_streak(self):  # the ruling: logged once per streak, not retried every 2 s
        self.run_case("a_backend_failure_is_said_once_a_streak")

    def test_alarm_records_follow_the_table(self):  # the ruling: queued on the app task's tick, after the call
        self.run_case("alarm_records_follow_the_table")

    def test_the_ram_map_forgets_a_removed_app(self):
        self.run_case("the_ram_map_forgets_a_removed_app")

    # ---- Task 20, fix round 1 ----

    def test_each_apps_failure_is_said(self):  # K: a streak per app
        self.run_case("each_apps_failure_is_said")

    def test_the_budget_holds_back_a_big_value(self):  # P3: at most 5 saves in 30 min, said once, the end writes the last
        self.run_case("the_budget_holds_back_a_big_value")

    def test_a_small_value_is_never_held_back(self):  # P3
        self.run_case("a_small_value_is_never_held_back")

    def test_untouched_records_wait_for_the_clock(self):  # P1: never in a sync, then armed again for the time left
        self.run_case("untouched_records_wait_for_the_clock")

    def test_a_touched_name_is_not_armed_again(self):  # P1: the app's choice wins; its new deadline once the clock comes
        self.run_case("a_touched_name_is_not_armed_again")

    def test_a_rearm_retries_drops_and_clamps(self):  # P1: no memory, no slot, the week, a due one, a removed app
        self.run_case("rearm_retries_drops_and_clamps")

    # ---- Task 20's fix round 2 ----

    def test_a_record_that_waits_for_the_budget_holds_no_other_sync(self):  # #1: handed over at once; drops never wait
        self.run_case("a_record_that_waits_for_the_budget_holds_no_other_sync")

    def test_a_removed_apps_records_are_not_armed_again(self):  # #2: forgotten at the next round, before the re-arm looks
        self.run_case("a_removed_apps_records_are_not_armed_again")

    def test_a_refused_save_is_charged_once(self):  # #4: charged when handed over, refunded on a refusal
        self.run_case("a_refused_save_is_charged_once")

    def test_the_values_tables_are_in_psram(self):  # the gadget's internal RAM is the Link's
        source = (APPS / "apps_values.c").read_text()
        self.assertIn('#include "esp_attr.h"', source)
        for name in ("s_failing", "s_failed", "s_slow", "s_slow_new", "s_marked", "s_recs", "s_unclocked", "s_waits", "s_forgotten"):
            with self.subTest(name):
                self.assertRegex(source, rf"(?m)^static EXT_RAM_BSS_ATTR [^\n]*\b{name}\[", f"{name}: plain memory of tasks, PSRAM")
        for name in ("s_lock_buf", "s_lock"):
            with self.subTest(name):
                self.assertRegex(source, rf"(?m)^static [^\n]*\b{name};", name)
                self.assertNotRegex(source, rf"(?m)^static [^\n]*EXT_RAM_BSS_ATTR[^\n]*\b{name};", f"{name}: the lock stays internal")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


if __name__ == "__main__":
    unittest.main()
