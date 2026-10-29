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

"""apps_alarm, the system's alarms of Muse apps: the table, the firing, the ring and what the esp_timer task may and may not do.

tests/muse_apps_alarm_harness.c runs the real apps_alarm.c on the host over a fake esp_timer (tests/muse_apps_fake_esp_timer.c:
one thread runs the callbacks, as the esp_timer task does, and the clock is the harness's, moved timer by timer), a fake lock
that checks its contract, and the chirp, the wake of the screen, the callback and the log as events in one list. Its ESP-IDF and
FreeRTOS headers are staged as one line each that includes the fakes (tests/muse_apps_task_fakes.h), as the other apps host
tests stage theirs. One case is one test. What these tests can't show is the board's own: the real esp_timer waking the chip
from light sleep, the voice task playing the chirp and the screen waking; the board check covers them.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_alarm_harness.c"
SOURCES = [HARNESS, TESTS / "muse_apps_fake_esp_timer.c", APPS / "apps_alarm.c"]

# What apps_alarm.c, muse_input.h and muse_voice.h include of FreeRTOS and ESP-IDF: each one is a line that includes the fakes.
FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h", "esp_err.h", "esp_log.h",
                "esp_timer.h"]


def stage_fakes(directory):
    directory = Path(directory)
    for name in FAKE_HEADERS:
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text('#include "muse_apps_task_fakes.h"\n')
    return directory


class AlarmTest(unittest.TestCase):
    harness = None

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = stage_fakes(Path(tmp.name) / "fake_headers")
        cls.harness = apps_host.build("muse_apps_alarm_harness", SOURCES, tmp.name, include_dirs=[MUSE, APPS, stage, TESTS])

    def run_case(self, case, timeout=120):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-3000:] + done.stderr[-12000:])

    # ---- The table ----

    def test_arm_counts_down_lists_replaces_and_cancels(self):
        self.run_case("arm_remaining_list_cancel")

    def test_four_to_an_app_eight_in_all_and_what_a_name_and_a_time_may_be(self):
        self.run_case("limits_and_names")

    # ---- Firing ----

    def test_a_fired_alarm_rings_wakes_and_waits_in_its_slot_until_it_is_taken(self):
        self.run_case("a_fired_alarm_waits_in_its_slot")

    def test_sound_and_wake_are_each_the_alarms_own(self):
        self.run_case("sound_and_wake_options")

    def test_firings_are_told_in_the_order_they_came_due(self):
        self.run_case("firings_are_told_in_the_order_they_came_due")

    def test_a_copy_of_a_firing_is_not_taken_once_its_slot_has_another(self):
        self.run_case("a_stale_copy_is_not_taken_twice")

    def test_the_pending_word_follows_the_firings(self):
        self.run_case("the_pending_word_follows_the_firings")

    def test_a_replaced_alarm_fires_once_at_its_new_time(self):
        self.run_case("a_replaced_alarm_fires_once_at_its_new_time")

    def test_arming_the_name_that_fired_keeps_that_firing_to_be_told(self):  # an app's start() that arms its own alarm's name
        self.run_case("arming_a_name_that_fired_keeps_its_firing")

    def test_a_cancelled_or_early_or_spent_timer_call_does_nothing(self):  # the callback was already on its way
        self.run_case("a_stale_call_does_nothing")

    # ---- The ring ----

    def test_the_ring_chirps_every_1500_ms_and_ends_after_a_minute(self):
        self.run_case("the_ring_chirps_until_its_minute_is_over")

    def test_sound_stop_ends_the_ring_once(self):
        self.run_case("stopping_the_ring")

    def test_a_second_alarm_makes_the_ring_a_minute_longer(self):
        self.run_case("a_second_alarm_makes_the_ring_a_minute_longer")

    def test_an_alarm_without_sound_leaves_the_ring_alone(self):
        self.run_case("an_alarm_without_sound_leaves_the_ring_alone")

    def test_a_firing_given_up_on_ends_the_ring_it_started(self):
        self.run_case("a_firing_given_up_on_ends_its_ring")

    def test_cancelling_a_firing_ends_the_ring_it_started(self):
        self.run_case("cancelling_a_firing_ends_its_ring")

    # ---- The esp_timer task never waits ----

    def test_a_busy_table_is_tried_again_ten_ms_later(self):
        self.run_case("a_busy_table_is_tried_again")

    def test_a_retry_started_behind_an_arming_does_not_lose_the_arming(self):
        self.run_case("a_retry_started_behind_an_arming_does_not_lose_it")

    def test_the_table_holds_up_to_three_tasks_and_the_esp_timer_task(self):
        self.run_case("the_table_holds_up_to_three_tasks_and_the_esp_timer_task")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


class AlarmStaticTest(unittest.TestCase):
    """What the cases above show of the esp_timer task, read in the source: a callback that waits holds up every timer of the
    gadget, so the two that run there take the table with a try and post and ask for nothing that waits, and the table, the pending
    word and the lock are static (internal RAM: a flag or a lock in PSRAM wouldn't be atomic on the S3)."""

    def test_the_esp_timer_callbacks_only_try_for_the_table(self):
        source = (APPS / "apps_alarm.c").read_text()
        for name in ("fire_cb", "ring_cb"):
            with self.subTest(name):
                body = function_source(source, f"static void {name}(void *arg) {{")
                self.assertIn("try_lock()", body)
                for waits in (r"\block\(\)", r"portMAX_DELAY", r"vTaskDelay", r"xQueueSend", r"xQueueReceive", r"ulTaskNotifyTake"):
                    self.assertNotRegex(body, waits)

    def test_the_apps_task_posts_the_alarm_event_without_waiting(self):
        body = function_source((APPS / "apps_task.c").read_text(), "static void on_alarm_fired(")
        self.assertRegex(body, r"xQueueSend\(s_queue, &ev, 0\)")
        self.assertIn("apps_alarm_set_fired_cb(on_alarm_fired)", function_source((APPS / "apps_task.c").read_text(), "esp_err_t muse_apps_start(void)"))

    def test_the_table_and_the_pending_word_are_static(self):
        source = (APPS / "apps_alarm.c").read_text()
        for heap in ("malloc", "calloc", "heap_caps", "apps_port_realloc"):
            self.assertNotIn(heap, source)
        self.assertRegex(source, r"(?m)^static slot_t s_slot\[APPS_ALARMS_MAX\];$")
        self.assertRegex(source, r"(?m)^static atomic_uint s_pending;")
        self.assertRegex(source, r"(?m)^static StaticSemaphore_t s_lock_buf;$")

    def test_the_limits_are_the_briefs(self):
        header = (APPS / "apps_alarm.h").read_text()
        for name, value in (("APPS_ALARMS_MAX", 8), ("APPS_ALARMS_PER_APP", 4), ("APPS_ALARM_NAME_MAX", 23),
                            ("APPS_ALARM_MAX_MS", 604800000), ("APPS_RING_PERIOD_MS", 1500), ("APPS_RING_MAX_MS", 60000)):
            self.assertRegex(header, rf"(?m)^#define {name} {value}\b", name)


class InputWakeTest(unittest.TestCase):
    """muse_input_wake (muse_input.c), which an alarm calls from the esp_timer task to wake the screen: compiled on its own, with
    the input task's side of it (apply_wake), as the other tests of muse_input.c compile the functions they check."""

    def test_a_wake_asked_from_another_task_is_the_input_tasks_to_make(self):
        source = (MUSE / "muse_input.c").read_text()
        declaration = re.findall(r"^static _Atomic\(const char \*\) s_wake_why;$", source, re.M)
        self.assertEqual(len(declaration), 1, declaration)
        program = "\n".join([
            "#include <assert.h>", "#include <stdatomic.h>", "#include <stdbool.h>", "#include <stdio.h>", "#include <string.h>",
            "typedef void *TaskHandle_t;",
            "static TaskHandle_t s_input;",
            "static int notifies, asleep_calls, pokes;",
            "static bool last_asleep = true;",
            "static char last_why[32];",
            "#define xTaskNotifyGive(task) ((void)(task), notifies++)",
            "static void set_asleep(bool asleep, const char *why) {",
            "    asleep_calls++;", "    last_asleep = asleep;", '    snprintf(last_why, sizeof last_why, "%s", why);', "}",
            "static void muse_state_poke(void) { pokes++; }",
            *declaration,
            function_source(source, "void muse_input_wake("),
            function_source(source, "static void apply_wake("),
            "int main(void) {",
            # Before the input task exists nothing can be notified, and the request waits for it.
            '    muse_input_wake("alarm");',
            "    assert(notifies == 0 && asleep_calls == 0);",
            "    apply_wake();",
            '    assert(asleep_calls == 1 && !last_asleep && strcmp(last_why, "alarm") == 0 && pokes == 1);',
            "    apply_wake();",
            "    assert(asleep_calls == 1 && pokes == 1);  /* the request was taken */",
            # With the task up, the wake notifies it (out of its wait), and a second request before it looks is one wake.
            "    s_input = (TaskHandle_t)1;",
            '    muse_input_wake("alarm");',
            "    assert(notifies == 1 && asleep_calls == 1);",
            '    muse_input_wake("timer");',
            "    assert(notifies == 2);",
            "    apply_wake();",
            '    assert(asleep_calls == 2 && strcmp(last_why, "timer") == 0 && pokes == 2);',
            "    apply_wake();",
            "    assert(asleep_calls == 2);",
            # NULL is a wake with a reason of its own.
            "    muse_input_wake(NULL);",
            "    apply_wake();",
            '    assert(asleep_calls == 3 && strcmp(last_why, "wake") == 0);',
            "    return 0;", "}",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input_wake.c"
            path.write_text(program)
            exe = apps_host.build("input_wake", [path], tmp)
            done = apps_host.run(exe)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_the_input_task_applies_a_wake_first_in_every_round(self):
        source = (MUSE / "muse_input.c").read_text()
        task = function_source(source, "static void input_task(void *arg)")
        self.assertRegex(task, r"for \(;;\) \{\s+apply_wake\(\);\s+unsigned ev = ")
        self.assertIn("void muse_input_wake(const char *why);", (MUSE / "muse_input.h").read_text())


if __name__ == "__main__":
    unittest.main()
