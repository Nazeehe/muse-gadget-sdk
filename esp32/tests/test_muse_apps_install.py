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

"""The install pipeline of Muse apps (apps_install.c) and its RAM store.

tests/muse_apps_install_harness.c runs the real pipeline with the real cJSON, package rules (apps_pkg, apps_json) and
apps_vm_excerpt (apps_vm and Lua), a fake source that serves files from a table and records every read, and fake ops
(compile, commit, run) that record what they were given. An install from the VM (Task 22's app.install,
apps_install_from_vm) runs the same pipeline over a fake muse_apps_vm_ops_t serving the same table: the paths it is asked
for, the deadline, a session that drops, a reader that is someone else's, and the log lines. Every result the harness prints is checked here to be valid
JSON within APPS_RESULT_MAX bytes, its bytes decoded strictly as UTF-8 (R13.3: a reply is text). The stack case runs
apps_install_run on exactly the I/O worker's 6,144 bytes (R13.7), in a build without the sanitizers. Needs the cJSON
sources: CJSON_SOURCE_DIR (~/.cache/muse-cjson), or an IDF build's managed_components.
"""

import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_install_harness.c"
SOURCES = [HARNESS, APPS / "apps_install.c", APPS / "apps_result.c", APPS / "apps_pkg.c", APPS / "apps_json.c",
           APPS / "apps_vm.c", APPS / "apps_suggest.c", APPS / "apps_rt.c", APPS / "apps_ui.c", APPS / "apps_svc.c",
           ROOT / "tools/muse/apphost/sha256.c"]
SDKCONFIG = "#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n"
# What muse_apps.h (the VM's ops, for apps_install_from_vm) includes of ESP-IDF and LVGL, as the little it uses.
ESP_ERR = "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n"
LVGL = "#pragma once\ntypedef struct lv_obj lv_obj_t;\n"
# The store logs what it drops (ESP_LOGW), and an install from the VM each file and its end: the harness keeps each line.
ESP_LOG = ("#pragma once\n"
           "void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));\n"
           "#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)\n"
           "#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)\n"
           "#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)\n")
RESULT_MAX = 6144


class Install(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        staged = Path(tmp.name) / "staged"
        staged.mkdir()
        (staged / "sdkconfig.h").write_text(SDKCONFIG)
        (staged / "esp_log.h").write_text(ESP_LOG)
        (staged / "esp_err.h").write_text(ESP_ERR)
        (staged / "lvgl.h").write_text(LVGL)
        cls.includes = [apps_host.LUA_SRC, APPS, MUSE, staged, TESTS, apps_host.CJSON_DIR]
        cls.harness = apps_host.build("muse_apps_install_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=cls.includes, objects=[cjson])

    def run_case(self, case, harness=None):
        try:
            done = apps_host.run(harness or self.harness, case, timeout=300, text=False)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 300 s")
        stderr = done.stderr.decode("utf-8", errors="replace")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:].decode("utf-8", errors="replace") + stderr[-8000:])
        try:
            stdout = done.stdout.decode("utf-8")  # strictly: a reply that isn't UTF-8 fails here
        except UnicodeDecodeError as e:
            self.fail(f"not UTF-8 ({e}): {done.stdout[max(e.start - 100, 0):e.end + 100]!r}")
        results, stacks = [], []
        for line in stdout.split("\n"):
            if not line:
                continue
            kind, _, text = line.partition(" ")
            self.assertIn(kind, ("@result", "@stack"), f"not a line of the harness: {line[:200]!r}")
            if kind == "@stack":
                stacks.append(json.loads(text))
                continue
            self.assertLessEqual(len(text.encode()), RESULT_MAX, f"a result of {len(text.encode())} bytes")
            try:
                results.append(json.loads(text))
            except ValueError as e:
                self.fail(f"not valid JSON ({e}): {text[:300]!r}")
        return results, stacks

    def test_manifest_errors_propagate(self):  # bad id, net, api 2
        self.run_case("manifest_errors_propagate")

    def test_path_checked_before_read(self):  # the source never sees ../x.lua
        self.run_case("path_checked_before_read")

    def test_compiled_chunk_refused(self):
        self.run_case("compiled_chunk_refused")

    def test_oversize_and_totals(self):
        self.run_case("oversize_and_totals")

    def test_compile_error_has_excerpt(self):  # C17: the raw message, the excerpt from the package's file
        self.run_case("compile_error_has_excerpt")

    def test_a_compile_message_is_clean_text(self):  # fix round 1, A: the token and the lines, a '?' for each byte
        results, _ = self.run_case("compile_message_is_clean")
        self.assertEqual(len(results), 3)
        for result in results:
            message = result["error"]["message"]
            self.assertNotRegex(message, "[\x00-\x08\x0b-\x1f\x7f-\x9f]", message)  # newline and tab are text
            self.assertEqual(message.count("\n"), 3, message)

    def test_crash_in_start_message(self):
        results, _ = self.run_case("crash_in_start_message")
        message = results[0]["error"]["message"]
        for part in ("installed but crashed in start():", "stack traceback:", "log:"):
            self.assertIn(part, message)
        self.assertEqual(message.split("log:\n")[1].split("\n"), [f"l{i:02d}" for i in range(3, 13)])

    def test_crash_in_top_level_message(self):  # Task 8's F2
        results, _ = self.run_case("crash_in_top_level_message")
        self.assertTrue(results[0]["error"]["message"].startswith("installed but crashed while loading main.lua: "))

    def test_result_fits_6144_bytes(self):  # C3: 32 files, an 8 KB log, a 128-entry dump
        results, _ = self.run_case("result_fits_6144_bytes")
        full = results[0]["payload"]
        self.assertEqual(len(full["files"]), 32)
        self.assertEqual(full["log"], [])  # the oldest lines go first: all of them, before the dump's entries
        self.assertEqual(len(full["ui"]) + full["ui_truncated"], 128)
        self.assertGreater(len(full["ui"]), 0)
        logged = results[1]["payload"]
        self.assertEqual(logged["log"][-1][:8], "line 39 ")
        self.assertNotIn("ui_truncated", logged)

    def test_busy(self):
        self.run_case("busy")

    def test_success_payload(self):
        results, _ = self.run_case("success_payload")
        files = results[0]["payload"]["files"]
        self.assertEqual([f["path"] for f in files], ["main.lua", "data.json", "logo.bin"])
        main = "local app = {}\nfunction app.start() print('started') end\nreturn app\n".encode()
        self.assertEqual(files[0]["bytes"], len(main))
        self.assertEqual(files[0]["sha256"], hashlib.sha256(main).hexdigest())

    def test_read_and_commit_failures(self):
        self.run_case("read_and_commit_failures")

    def test_a_result_says_the_stores_hint(self):  # P7: an unmountable partition's hint, which doesn't say to reflash
        self.run_case("a_result_says_the_stores_hint")

    def test_the_deadline_is_one_absolute_time(self):  # E24, D3
        self.run_case("deadline_is_one_absolute_time")

    def test_vm_paths_built_on_device(self):  # D9: workspace/apps/<id>/<rel>, made here; D10's lines
        self.run_case("vm_paths_built_on_device")

    def test_deadline(self):  # D3, E24: the clock past 90 s, and a deadline that passes partway through a file
        self.run_case("deadline")

    def test_session_drop_commits_nothing(self):
        self.run_case("session_drop_commits_nothing")

    def test_unchanged(self):  # with and without run, on flash and in RAM
        self.run_case("unchanged")

    def test_vm_refusals(self):  # a reader that is someone else's (busy), no memory, too large, a file that isn't there
        self.run_case("vm_refusals")

    def test_vm_out_of_memory(self):  # every block refused in turn, the sink's included
        self.run_case("vm_out_of_memory")

    def test_an_update_short_of_memory_says_it_needs_two_copies(self):  # C1: the running app's update holds its files twice
        self.run_case("an_update_short_of_memory_says_it_needs_two_copies")

    def test_the_ram_store(self):  # C4, E26; F1's list, most recent first; the cap drops the oldest
        self.run_case("ram_store")

    def test_the_ram_budget(self):  # F2: memory_kb + 1 MiB free after a copy; the oldest go first, never the running app
        self.run_case("ram_budget")

    def test_results_are_utf8(self):  # T2: a compile error quoting bytes that aren't UTF-8; a long cut between characters
        results, _ = self.run_case("results_are_utf8")  # every result decoded strictly (run_case)
        self.assertIn("local caf? = = 1 -- ?", results[0]["error"]["message"])
        self.assertEqual(len(results), 11)
        for shift, result in enumerate(results[1:]):
            with self.subTest(shift=shift):
                message = result["error"]["message"]
                self.assertTrue(message.startswith("installed but crashed in start(): " + "a" * shift + "?\x01\U0001F600"),
                                message[:80])
                printed = json.dumps(result, separators=(",", ":"), ensure_ascii=False).encode()
                self.assertLessEqual(len(printed), RESULT_MAX)
                self.assertTrue(message.endswith("\U0001F600") or message.endswith("\x01"), message[-20:])

    def test_status_json(self):  # E13: instructions
        self.run_case("status_json")

    def test_out_of_memory_anywhere(self):
        self.run_case("out_of_memory")

    def test_install_stack(self):  # R13.7: the pipeline on exactly 6,144 bytes (the worker has 8,192 since X18), from the VM too
        harness = apps_host.build("muse_apps_install_stack", [*SOURCES, *apps_host.lua_sources()], self.tmp,
                                  include_dirs=self.includes, objects=[apps_host.cjson_object()], sanitize=False)
        _, stacks = self.run_case("install_stack", harness=harness)
        self.assertEqual([s["case"] for s in stacks], ["success", "crash", "compile", "vm"])
        for s in stacks:
            print(f"install_stack: {s['case']} used {s['used']} of {s['of']} bytes (host frames)", file=sys.stderr,
                  flush=True)

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])


if __name__ == "__main__":
    unittest.main()
