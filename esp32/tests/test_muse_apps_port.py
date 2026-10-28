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

"""apps_port_idf.c, the device's port of Muse apps: the console echo's cap and queue, and the UI lock's breach log.

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

FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "esp_heap_caps.h", "esp_log.h",
                "esp_timer.h"]
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
        cls.harness = apps_host.build("muse_apps_port_harness", [HARNESS, APPS / "apps_port_idf.c"], tmp.name,
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

    def test_each_kind_of_breach_is_logged_once(self):  # F11
        self.run_case("breach_kinds")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


if __name__ == "__main__":
    unittest.main()
