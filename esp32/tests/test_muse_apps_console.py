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

"""The USB console's hooks (muse_input.c) and its ">app.*" protocol (apps_console.c).

ConsoleHookChain compiles muse_input.c's hook chain on its own (style b). Console runs
tests/muse_apps_console_harness.c: the real apps_console.c and install pipeline (apps_install.c, with the real cJSON)
under muse_apps_console, as the serial task calls it, with a fake I/O worker and fake app task calls. Every line the
console prints is checked here to be one reply of the protocol, its JSON valid (R13.3). The stack case runs the hook on
exactly the serial task's 3,584 bytes (R13.1), in a build without the sanitizers. What these tests can't show is the
board's own: the UART's speed, the real stack's high-water mark (Task 15 reads muse_serial's), the RAM it all takes.
"""

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_console_harness.c"
SOURCES = [HARNESS, APPS / "apps_console.c", APPS / "apps_install.c", APPS / "apps_pkg.c", APPS / "apps_json.c",
           APPS / "apps_vm.c", APPS / "apps_suggest.c", APPS / "apps_rt.c", APPS / "apps_ui.c",
           ROOT / "tools/muse/apphost/sha256.c"]

# What apps_console.c and muse_apps.h include of ESP-IDF and LVGL, staged as the little they use.
STAGED = {
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n",
    "lvgl.h": "#pragma once\ntypedef struct lv_obj lv_obj_t;\n",
    "esp_log.h": ("#pragma once\n"
                  "void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));\n"
                  "#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)\n"),
    "sdkconfig.h": "#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n",
}

# The protocol's replies: an acknowledgement, a result or a refusal, each one line.
REPLY = re.compile(r"^(?:@app\.ack (?P<path>[a-z0-9_./-]+) (?P<seq>\d+)|@app (?P<result>\{.*\})|"
                   r"@app\.err (?P<err>\{.*\})|@stack (?P<stack>\{.*\}))$")


def stage(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in STAGED.items():
        (directory / name).write_text(text)
    return directory


def replies(test, stdout):
    """Every line the console printed, parsed: each is one reply, and its JSON is valid (R13.3)."""
    parsed = []
    for line in stdout.split("\n"):
        if not line:
            continue
        match = REPLY.match(line)
        test.assertIsNotNone(match, f"not a reply of the protocol: {line!r}")
        text = match.group("result") or match.group("err") or match.group("stack")
        if text is None:
            parsed.append(("ack", match.group("path"), int(match.group("seq"))))
            continue
        try:
            value = json.loads(text)
        except ValueError as e:
            test.fail(f"not valid JSON ({e}): {line!r}")
        test.assertIsInstance(value, dict, line)
        if not match.group("stack"):
            test.assertIn("op", value, line)
        parsed.append(("result" if match.group("result") else "err" if match.group("err") else "stack", value))
    return parsed


class ConsoleHookChain(unittest.TestCase):
    def test_console_hooks_chain_in_order(self):
        # muse_input_add_console_hook and run_console_hooks, compiled on their own: up to 4 hooks, in the order added,
        # the first to take a line wins, and a 5th (or a NULL) is ignored.
        source = (MUSE / "muse_input.c").read_text()
        declarations = re.findall(r"^(?:#define CONSOLE_HOOKS_MAX .*|static bool \(\*s_console_hooks\[.*\]\)\(const char "
                                  r"\*line\);|static int s_console_hook_count;)$", source, re.M)
        self.assertEqual(len(declarations), 3, declarations)
        program = "\n".join([
            "#include <stdbool.h>", "#include <stdio.h>", "#include <string.h>",
            "#define ESP_LOGW(tag, ...) ((void)(tag))", 'static const char *TAG = "muse_input";',
            *declarations,
            function_source(source, "void muse_input_add_console_hook("),
            function_source(source, "static bool run_console_hooks("),
            "static char calls[16];",
            "static bool hook(char name, const char *line) {",
            "    size_t n = strlen(calls);", "    calls[n] = name;", "    calls[n + 1] = 0;",
            "    return line[0] == name;", "}",
            *(f"static bool hook_{c}(const char *line) {{ return hook('{c}', line); }}" for c in "abcde"),
            "static void run(const char *line) {",
            '    calls[0] = 0;', '    bool taken = run_console_hooks(line);', '    printf("%s %d %s\\n", line, taken, calls);',
            "}",
            "int main(void) {",
            "    muse_input_add_console_hook(NULL);",
            *(f"    muse_input_add_console_hook(hook_{c});" for c in "abcde"),
            '    run("a");', '    run("b");', '    run("d");', '    run("e");', '    run("z");',
            "    return 0;", "}",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "console_hooks.c"
            path.write_text(program)
            exe = apps_host.build("console_hooks", [path], tmp)
            done = apps_host.run(exe)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.splitlines(), [
            "a 1 a",      # A owns "a": B never sees it
            "b 1 ab",     # "b" reaches B after A said no
            "d 1 abcd",   # the 4th hook
            "e 0 abcd",   # the 5th add was ignored: nothing takes "e"
            "z 0 abcd",
        ])

    def test_console_command_runs_the_chain_and_the_setter_is_gone(self):
        source = (MUSE / "muse_input.c").read_text()
        command = function_source(source, "static bool console_command(char *line, bool whole)")
        # Only a whole line reaches the hooks: a longer one (over SERIAL_LINE - 1 bytes) never does, and gets no reply.
        self.assertRegex(command, r"if \(whole && run_console_hooks\(line\)\) \{\s*return true;")
        self.assertIn("#define SERIAL_LINE 1024", source)
        self.assertNotIn("muse_input_set_console_hook", (MUSE / "muse_input.h").read_text())
        for path in [*MUSE.glob("*.[ch]"), *(MUSE / "apps").glob("*.[ch]"), *(ROOT / "main").glob("*.[ch]")]:
            with self.subTest(path.name):
                self.assertNotIn("muse_input_set_console_hook", path.read_text())
        glue = (ROOT / "main/muse_glue.c").read_text()
        self.assertIn("muse_input_add_console_hook(sdk_token_console_command);", glue)
        self.assertIn("(muse_input_add_console_hook)", (ROOT / "main/sdk_token_console.h").read_text())
        # muse_apps_start adds the apps' hook (after the SDK token's, which muse_glue_start adds first).
        task = (APPS / "apps_task.c").read_text()
        start = function_source(task, "esp_err_t muse_apps_start(void)")
        self.assertIn("muse_input_add_console_hook(muse_apps_console);", start)


class Console(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        staged = stage(Path(tmp.name) / "staged")
        cls.includes = [apps_host.LUA_SRC, APPS, MUSE, staged, TESTS, apps_host.CJSON_DIR]
        cls.harness = apps_host.build("muse_apps_console_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=cls.includes, objects=[cjson])

    def run_case(self, case, harness=None):
        try:
            done = apps_host.run(harness or self.harness, case, timeout=120)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 120 s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-8000:])
        return replies(self, done.stdout)

    def test_put_sequence_and_ack(self):
        self.run_case("put_sequence_and_ack")

    def test_put_out_of_order_errors(self):
        self.run_case("put_out_of_order")

    def test_put_bad_base64(self):
        self.run_case("put_bad_base64")

    def test_put_over_kind_limit(self):
        self.run_case("put_over_kind_limit")

    def test_limits_are_enforced_as_bytes_arrive(self):
        self.run_case("limits_as_bytes_arrive")

    def test_commit_submits_staged_files(self):
        parsed = self.run_case("commit_submits_staged_files")
        results = [r[1] for r in parsed if r[0] == "result" and r[1]["op"] == "commit" and r[1]["ok"]]
        payload = results[0]["payload"]
        self.assertEqual([f["path"] for f in payload["files"]], ["main.lua", "lib/util.lua", "data.json", "logo.bin"])
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{64}", f["sha256"]) for f in payload["files"]), payload["files"])
        self.assertEqual(payload["storage"], "ram")

    def test_commit_refusals(self):  # R13.4: busy, short of memory, nothing staged
        self.run_case("commit_refusals")

    def test_a_new_id_discards_the_stage(self):
        self.run_case("a_new_id_discards_the_stage")

    def test_launch_runs_an_installed_app(self):  # R13.5
        self.run_case("launch_runs_an_installed_app")

    def test_stop_reports_the_task(self):  # Task 12's ruling: apps_task_stop's bool
        self.run_case("stop_reports_the_task")

    def test_replies_end_with_a_push(self):
        # On the board a reply whose line, CRLF included, is a multiple of 64 bytes ends on a USB packet boundary, and
        # the host kept it until the console printed again (up to the next 5 s heartbeat). apps_port_console_flush
        # sends the packet that ends it; every reply calls it after its newline (every case checks this too).
        self.run_case("replies_end_with_a_push")

    def test_status_prints_the_record(self):  # E13: instructions; C3's trim; "ui_truncated"
        parsed = self.run_case("status_prints_the_record")
        status = [r[1] for r in parsed if r[0] == "result" and r[1]["op"] == "status"]
        self.assertEqual(len(status), 3)
        self.assertEqual(status[0]["instructions"], 4321)
        logged, full = status[1], status[2]
        self.assertEqual(logged["log"][-1][:8], "line 39 ")  # the oldest lines go first
        self.assertNotIn("ui_truncated", logged)
        del full["op"]  # the console's own: the status is at most 6144 bytes without it
        self.assertLessEqual(len(json.dumps(full, separators=(",", ":"), ensure_ascii=False).encode()), 6144)
        self.assertEqual(full["log"], [])  # every line goes before a widget does
        self.assertGreater(full["ui_truncated"], 5)  # the record's 5 and the trim's
        self.assertEqual(len(full["ui"]) + full["ui_truncated"], 128 + 5)

    def test_list_shows_installed_apps(self):
        self.run_case("list_shows_installed_apps")

    def test_send_returns_the_reply(self):
        self.run_case("send_returns_the_reply")

    def test_foreign_and_unknown_lines(self):
        self.run_case("foreign_and_unknown_lines")

    def test_stage_out_of_memory(self):
        self.run_case("stage_out_of_memory")

    def test_console_stack(self):  # R13.1: the hook on exactly 3,584 bytes
        harness = apps_host.build("muse_apps_console_stack", [*SOURCES, *apps_host.lua_sources()], self.tmp,
                                  include_dirs=self.includes, objects=[apps_host.cjson_object()], sanitize=False)
        parsed = self.run_case("console_stack", harness=harness)
        stack = [r[1] for r in parsed if r[0] == "stack"]
        print(f"console_stack: the hook used {stack[0]['used']} of {stack[0]['of']} bytes (host frames)",
              file=sys.stderr, flush=True)

    def test_console_frames_are_small(self):
        # R13.1: no array over 128 bytes on the serial task's stack in apps_console.c (the replies and the status
        # record are on the heap; the stage is in PSRAM). Every array declared inside a function body is a byte array
        # whose size, with the headers' and the file's own defines, is 128 or less.
        source = (APPS / "apps_console.c").read_text()
        defines = {}
        for text in [*(h.read_text() for h in (APPS / "apps_pkg.h", APPS / "apps_install.h", APPS / "apps_io.h")), source]:
            defines.update(dict(re.findall(r"^#define\s+(\w+)\s+\(?([\w\s+*-]+?)\)?\s*(?:/\*.*)?$", text, re.M)))

        def value(expr):
            for _ in range(5):
                expr = re.sub(r"\b[A-Z][A-Z0-9_]{2,}\b", lambda m: f"({defines.get(m.group(0), m.group(0))})", expr)
            return eval(expr, {})

        bodies = []
        for match in re.finditer(r"^[a-z][^\n;=]*\)\s*\{\n", source, re.M):
            depth, at = 1, match.end()
            while depth:
                depth += {"{": 1, "}": -1}.get(source[at], 0)
                at += 1
            bodies.append(source[match.end():at])
        self.assertGreater(len(bodies), 10, "the function bodies weren't found")
        found = 0
        for body in bodies:
            # Each declaration that has an array in it (a type, then a declarator with '['), and each array it declares.
            for kind, declarators in re.findall(r"^\s+(?!(?:return|else|case|goto|do)\b)(?:static\s+)?(?:const\s+)?"
                                                r"([A-Za-z_]\w*(?: [A-Za-z_]\w*)?)\s+(\**\w+\[[^;]*);", body, re.M):
                for name, size in re.findall(r"(\w+)\[([^\]]+)\]", declarators):
                    with self.subTest(name):
                        found += 1
                        self.assertIn(kind, ("char", "unsigned char", "uint8_t"), f"{kind} {name}[{size}]: a byte array only")
                        self.assertLessEqual(value(size), 128, f"{name}[{size}]")
        self.assertGreater(found, 0, "no local arrays: the pattern no longer reads the file")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])


if __name__ == "__main__":
    unittest.main()
