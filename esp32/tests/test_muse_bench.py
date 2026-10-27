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

"""The bench's memory report (>mem) and the size of link.register in the log."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseBenchTest(unittest.TestCase):
    def test_bench_option_and_memory_report(self):
        kc = (MUSE / "Kconfig").read_text()
        self.assertIn("config MUSE_BENCH", kc)
        self.assertIn("CONFIG_MUSE_BENCH=y", (ROOT / "devices/sdkconfig.muse-bench").read_text())
        inp = (MUSE / "muse_input.c").read_text()
        block = inp[inp.index("#if CONFIG_MUSE_BENCH"):]
        self.assertIn('!strcmp(line, "mem")', block[:block.index("#endif")])
        self.assertIn('"lvgl"', inp)

    def test_register_size_is_logged(self):
        # The size comes from the log line every session already prints when it sends
        # the registration: "sent link.register (N bytes)".
        source = (ROOT / "main/noise_control.cpp").read_text()
        self.assertIn('"sent %s (%u bytes)"', source)
        self.assertIn('is_registration ? "link.register"', source)


if __name__ == "__main__":
    unittest.main()
