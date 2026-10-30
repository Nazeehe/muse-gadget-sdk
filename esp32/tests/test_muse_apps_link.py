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

"""Link's app.* commands (main/apps_link.c, CONFIG_MUSE_APPS): Muse's way to the gadget's apps.

tests/muse_apps_link_harness.c compiles the whole of apps_link.c with the real cJSON against fakes of the apps
(muse_apps_install, muse_apps_id_valid, muse_apps_set_vm_ops), the session (noise_ctrl_send_command_result), the VM file
reader and writer (vm_file.h) and the PSRAM heap: link.register's entries and their size, the install handler (never
waiting, its request copied and freed once the result is sent, also when the session has ended), and vm_file behind the
apps' VM ops. The wiring is checked in the firmware's own sources: advertised, dispatched, started and built only with the
option. Needs the cJSON sources: CJSON_SOURCE_DIR (~/.cache/muse-cjson), or an IDF build's managed_components.
"""

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_wiring import c_function, open_conditions, strip_c_comments

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
MUSE = ROOT / "components/muse"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_link_harness.c"

# The room link.register has for the apps' commands, in bytes. /tmp/muse-apps-log.md, "## Baseline", "### REGISTER_BUDGET"
# (Task 1, measured on the 1.75C on 2026-10-26): "REGISTER_BUDGET = 7168 - 2669 = 4499 bytes (Task 22). 2669 is the N in this
# board's `sent link.register (N bytes)` boot-log line." 7168 is the global constraint's 7 KB; 2669 the registration without
# the apps (it carries the Wi-Fi network's name, so a longer one costs a few bytes more). Tasks 23 to 25 add their entries
# under the same budget.
APPS_REGISTER_BUDGET = 4499

INSTALL_DESCRIPTION = ("Install the app in workspace/apps/<id>/ on this VM (manifest.json and the files it lists) onto the "
                       "gadget, and run it. Returns compile errors, a crash traceback and logs, a widget dump and round-screen "
                       "warnings. Call app.api first.")

# What apps_link.c and the headers it includes need of ESP-IDF and LVGL, as the little they use.
STAGED = {
    "sdkconfig.h": ("#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_SPIRAM 1\n"
                    "#define CONFIG_HOMEHUB_DAEMON_REQUESTS 1\n"),
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n",
    "lvgl.h": "#pragma once\ntypedef struct lv_obj lv_obj_t;\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_heap_caps.h": ("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_8BIT (1u << 2)\n"
                        "#define MALLOC_CAP_SPIRAM (1u << 10)\n"
                        "void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);\nvoid heap_caps_free(void *ptr);\n"),
}


class LinkCommands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        staged = Path(tmp.name) / "staged"
        staged.mkdir()
        for name, text in STAGED.items():
            (staged / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_link_harness", [HARNESS, MAIN / "apps_link.c"], tmp.name,
                                      include_dirs=[staged, MAIN, MUSE, apps_host.CJSON_DIR], objects=[cjson])

    def run_case(self, case):
        try:
            done = apps_host.run(self.harness, case, timeout=60)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 60 s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-4000:])
        return done.stdout

    def test_register_entries_fit_budget(self):
        stdout = self.run_case("register_entries")
        lines = [line for line in stdout.splitlines() if line.startswith("@register ")]
        self.assertEqual(len(lines), 1, stdout)
        text = lines[0][len("@register "):]
        print(f"link.register: the apps' entries are {len(text.encode())} of {APPS_REGISTER_BUDGET} bytes", file=sys.stderr,
              flush=True)
        self.assertLessEqual(len(text.encode()), APPS_REGISTER_BUDGET)
        commands = json.loads(text)
        self.assertEqual(sorted(commands), ["app.install"])
        install = commands["app.install"]
        self.assertEqual(install, {
            "description": INSTALL_DESCRIPTION,
            "required": {"id": {"type": "string", "description": "App id: lowercase, the folder name."}},
            "optional": {"run": {"type": "boolean", "description": "Run it after installing (default true)."}},
            "timeout_ms": 120000,
        })
        self.assertNotRegex(text, re.compile("hatch", re.I), "say Muse, never Hatch")

    def test_install_handler_copies_request(self):  # and frees it once the result is sent, sent or dropped
        self.run_case("install_handler")
        self.run_case("handles")

    def test_vm_ops_put_vm_file_behind_the_apps(self):  # D3: the deadline forwarded; vm_file's answers as the apps' codes
        self.run_case("vm_ops_are_vm_file")

class LinkWiring(unittest.TestCase):
    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])

    def test_advertised_and_dispatched_only_under_the_option(self):
        # link.register: after the BLE commands, before commands_v2 is attached, only with the option.
        noise = (MAIN / "noise_control.cpp").read_text()
        register = c_function(noise, "static char *build_register_json(void)")
        self.assertEqual(noise.count("apps_link_register(commands);"), 1)
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(noise, noise.index("apps_link_register(commands);")))
        self.assertLess(register.index("ble_commands_register(commands);"), register.index("apps_link_register(commands);"))
        self.assertLess(register.index("apps_link_register(commands);"),
                        register.index('cJSON_AddItemToObject(params, "commands_v2", commands);'))
        # on_ws_command: next to the BLE commands' dispatch, before anything else, only with the option.
        app = (MAIN / "app.c").read_text()
        handler = c_function(app, "static cJSON *on_ws_command(")
        self.assertEqual(app.count("apps_link_handles(command)"), 1)
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(app, app.index("apps_link_handles(command)")))
        self.assertRegex(handler, r"if \(apps_link_handles\(command\)\) \{\s*return apps_link_handle\(command, params, "
                                  r"request_id, session_generation\);\s*\}")
        self.assertLess(handler.index("ble_commands_handles(command)"), handler.index("apps_link_handles(command)"))
        self.assertLess(handler.index("apps_link_handles(command)"), handler.index('"device.list_vms"'))
        # The VM's files behind the apps: set as Muse starts (muse_glue_start), only with the option (X5, E12).
        glue = (MAIN / "muse_glue.c").read_text()
        self.assertEqual(glue.count("apps_link_init();"), 1)
        self.assertIn("apps_link_init();", c_function(glue, "void muse_glue_start(void)"))
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(glue, glue.index("apps_link_init();")))
        # Every other mention of the file in main/, includes included, is under the option.
        for path in sorted([*MAIN.glob("*.c"), *MAIN.glob("*.cpp")]):
            if path.name == "apps_link.c":
                continue
            source = strip_c_comments(path.read_text())
            for match in re.finditer(r"apps_link", source):
                with self.subTest(path.name, at=match.start()):
                    self.assertIn("if CONFIG_MUSE_APPS", open_conditions(source, match.start()))
        # Built only with the option.
        cmake = (MAIN / "CMakeLists.txt").read_text()
        self.assertEqual(cmake.count('"apps_link.c"'), 1)
        block = re.search(r"^if\(CONFIG_MUSE_APPS\)\n(.*?)^endif\(\)", cmake, re.M | re.S)
        self.assertIsNotNone(block, "no if(CONFIG_MUSE_APPS) block in main/CMakeLists.txt")
        self.assertIn('list(APPEND GADGET_SRCS "apps_link.c")', block.group(1))


if __name__ == "__main__":
    unittest.main()
