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

"""The vendored Lua, built on the host the way components/lua builds it for the device."""

import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]


class MuseAppsLuaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = apps_host.build(
            "muse_apps_lua_harness",
            [ROOT / "tests/muse_apps_lua_harness.c", *apps_host.lua_sources()],
            tmp.name,
            include_dirs=[ROOT / "components/lua/src"],
        )

    def run_case(self, case: str) -> None:
        done = apps_host.run(self.harness, case)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_number_model_is_int64_and_double(self):
        self.run_case("numbers")

    def test_device_defines_reach_the_host_build(self):
        self.assertEqual(apps_host.lua_defines(), ["LUAI_MAXCCALLS=40"])
        self.run_case("nesting_limit")


if __name__ == "__main__":
    unittest.main()
