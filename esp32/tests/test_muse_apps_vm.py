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

"""apps_vm, the Lua state every Muse app runs in: the capped heap, the sandbox, text-only loading.

Each test runs one case of tests/muse_apps_vm_harness.c, which prints what it got when the
case fails. Every case also ends with the harness's counting allocator at 0 bytes.
"""

import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"


class MuseAppsVmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = apps_host.build(
            "muse_apps_vm_harness",
            [ROOT / "tests/muse_apps_vm_harness.c", APPS / "apps_vm.c", APPS / "apps_suggest.c",
             *apps_host.lua_sources()],
            tmp.name,
            include_dirs=[ROOT / "components/lua/src", APPS],
        )

    def run_case(self, case: str, timeout: float = 60) -> None:
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            done = None
        if done is None:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_bytecode_is_refused(self):
        self.run_case("bytecode")

    def test_bom_and_crlf_load(self):
        self.run_case("bom_crlf")

    def test_gc_metatable_refused(self):
        self.run_case("gc_metatable")

    def test_strict_globals_suggest(self):
        self.run_case("strict_globals")

    def test_sandbox_is_the_registry_globals(self):
        # C installs with lua_setglobal and reads with lua_rawget; chunks see the same table.
        self.run_case("registry_globals")

    def test_sandbox_contents(self):
        self.run_case("sandbox_contents")

    def test_patterns_absent_with_guidance(self):
        self.run_case("patterns")

    def test_plain_find_keeps_string_find_semantics(self):
        self.run_case("plain_find")

    def test_split_trim(self):
        self.run_case("split_trim")

    def test_find_is_linear_on_adversarial_input(self):
        # A 16 MB heap. KMP takes milliseconds; Lua's own memchr+memcmp search takes about 5 s.
        self.run_case("find_linear", timeout=2)

    def test_split_is_linear_on_adversarial_input(self):
        self.run_case("split_linear", timeout=2)

    def test_allocator_caps_and_counts(self):
        self.run_case("allocator")

    def test_create_destroy_cycles_leak_nothing(self):
        self.run_case("cycles")

    def test_new_fails_cleanly_without_memory(self):
        self.run_case("new_without_memory")

    def test_excerpt_format(self):
        self.run_case("excerpt")

    def test_suggest_rule(self):
        self.run_case("suggest")


if __name__ == "__main__":
    unittest.main()
