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

"""muse_sntp.c, the SNTP helper that moved out of the pocket watch (muse_clock.c): the watch and the apps' time module
share it.

tests/muse_sntp_harness.c runs a copy of the real source on the host over fakes: the ESP-IDF headers it includes are
staged next to the copy (a quoted include finds the source's own directory first, so the real muse_wifi.h would win
otherwise), the Wi-Fi is a flag, the SNTP client a counter, and time() the harness's own. What these tests can't show
is the board's: that SNTP really sets the clock, which the board check covers (the boot log says `SNTP started`, and the
watch shows the time).
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_sntp_harness.c"

FAKES = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
""",
    "esp_log.h": """#pragma once
#include "esp_err.h"
void fake_esp_log(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) fake_esp_log('I', (tag), __VA_ARGS__)
const char *esp_err_to_name(esp_err_t err);
""",
    "esp_netif_sntp.h": """#pragma once
#include "esp_err.h"
typedef struct {
    const char *server;
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(name) {.server = (name)}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
""",
    "muse_wifi.h": """#pragma once
#include <stdbool.h>
bool muse_wifi_connected(void);
""",
    "sdkconfig.h": '#define CONFIG_MUSE_CLOCK_TZ "PST8PDT,M3.2.0,M11.1.0"\n',
}


class SntpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = Path(tmp.name) / "stage"
        stage.mkdir()
        for name, text in FAKES.items():
            (stage / name).write_text(text)
        for name in ("muse_sntp.c", "muse_sntp.h"):
            shutil.copy(MUSE / name, stage / name)
        cls.harness = apps_host.build("muse_sntp_harness", [HARNESS, stage / "muse_sntp.c"], tmp.name,
                                      include_dirs=[stage])

    def run_case(self, case, timeout=30):
        done = subprocess.run([str(self.harness), case], capture_output=True, text=True, timeout=timeout)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_sntp_starts_once_when_wifi_is_up(self):
        self.run_case("starts_once_when_wifi_is_up")

    def test_a_failed_start_is_logged_and_not_retried(self):
        self.run_case("a_failed_start_is_logged_and_not_retried")

    def test_the_clock_is_valid_from_2025(self):
        self.run_case("the_clock_is_valid_from_2025")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


if __name__ == "__main__":
    unittest.main()
