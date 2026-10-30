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

"""The USB console's hooks (muse_input.c) and its ">app.*" protocol (apps_console.c), and Link's app.control and app.api
underneath (apps_control.c).

ConsoleHookChain compiles muse_input.c's hook chain on its own (style b). Console runs
tests/muse_apps_console_harness.c: the real apps_console.c and install pipeline (apps_install.c, with the real cJSON)
under muse_apps_console, as the serial task calls it, with a fake I/O worker and fake app task calls, and the real
apps_control.c over the same fakes, with the guide and the timer embedded from their files as the firmware embeds them
(EMBED_FILES: the symbols come from an .incbin here). Every line the
console prints is checked here to be one reply of the protocol, its bytes decoded strictly as UTF-8 and its JSON valid
(R13.3). The stack case runs the hook on
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
           APPS / "apps_vm.c", APPS / "apps_suggest.c", APPS / "apps_rt.c", APPS / "apps_ui.c", APPS / "apps_svc.c",
           APPS / "apps_control.c", ROOT / "tools/muse/apphost/sha256.c"]

# The files the firmware embeds for app.api (components/muse/CMakeLists.txt's EMBED_FILES), as the symbols IDF makes of their
# names: _binary_<name>_start and _end.
EMBEDDED = {"apps_guide_md": APPS / "apps_guide.md", "manifest_json": APPS / "examples/timer/manifest.json",
            "main_lua": APPS / "examples/timer/main.lua"}

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
    # The stage's lock (a FreeRTOS mutex): the harness's one thread holds it or not.
    "freertos/FreeRTOS.h": ("#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\ntypedef int BaseType_t;\n"
                            "#define pdTRUE 1\n#define pdFALSE 0\n#define portMAX_DELAY ((TickType_t)0xffffffffu)\n"),
    "freertos/semphr.h": ("#pragma once\n#include \"freertos/FreeRTOS.h\"\ntypedef struct fake_mutex *SemaphoreHandle_t;\n"
                          "typedef struct { void *opaque[4]; } StaticSemaphore_t;\n"
                          "SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);\n"
                          "BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);\n"
                          "BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);\n"),
    # app.control's waits: a tick is 10 ms here, and the harness's clock moves on by the ticks a delay sleeps.
    "freertos/task.h": ("#pragma once\n#include \"freertos/FreeRTOS.h\"\n#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10))\n"
                        "void vTaskDelay(TickType_t ticks);\n"),
    # The internal RAM of app.control's waits, and the DMA RAM app.api logs.
    "esp_heap_caps.h": ("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_8BIT (1u << 2)\n"
                        "#define MALLOC_CAP_DMA (1u << 3)\n#define MALLOC_CAP_INTERNAL (1u << 11)\n"
                        "void *heap_caps_malloc(size_t size, uint32_t caps);\nvoid heap_caps_free(void *ptr);\n"
                        "size_t heap_caps_get_free_size(uint32_t caps);\nsize_t heap_caps_get_minimum_free_size(uint32_t caps);\n"),
}

# The protocol's replies: an acknowledgement, a result or a refusal, each one line.
MAIN_HELLO = "local app = {}\nfunction app.start() ui.label{text = 'Hi'} end\nreturn app\n"  # the harness's

REPLY = re.compile(r"^(?:@app\.ack (?P<path>[a-z0-9_./-]+) (?P<seq>\d+)|@app (?P<result>\{.*\})|"
                   r"@app\.err (?P<err>\{.*\})|@stack (?P<stack>\{.*\}))$")


def stage(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in STAGED.items():
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text(text)
    return directory


def embed_source(directory):
    """A C file that embeds EMBEDDED's files under the symbols EMBED_FILES makes (.incbin), as the firmware has them."""
    lines = ["/* Generated by tests/test_muse_apps_console.py: the files app.api writes, as EMBED_FILES embeds them. */",
             "extern int muse_apps_embedded;"]
    for name, path in EMBEDDED.items():
        symbol = f"_binary_{name}"
        lines.append(f'__asm__(".data\\n.p2align 2\\n.globl {symbol}_start\\n.globl {symbol}_end\\n{symbol}_start:\\n'
                     f'.incbin \\"{path}\\"\\n{symbol}_end:\\n.byte 0\\n");')
    path = Path(directory) / "muse_apps_embedded.c"
    path.write_text("\n".join(lines) + "\n")
    return path


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


def blanked(source):
    """source with its comments, strings and character literals blanked (each character but a newline a space), so their
    braces and brackets don't count."""
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), source, flags=re.S)


def function_bodies(source):
    """Every function body of a C file, by name: from each '{' at the top level that follows a ')' (however the signature
    is split across lines) to its matching '}', in the blanked text."""
    text = blanked(source)
    bodies, depth, start, name = {}, 0, None, None
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                j = i - 1
                while j >= 0 and text[j].isspace():
                    j -= 1
                start = None
                if j >= 0 and text[j] == ")":
                    parens, k = 0, j
                    while k >= 0:
                        parens += {")": 1, "(": -1}.get(text[k], 0)
                        if parens == 0:
                            break
                        k -= 1
                    match = re.search(r"(\w+)\s*$", text[:k])
                    start, name = i + 1, match.group(1) if match else None
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None and name is not None:
                bodies[name] = text[start:i]
                start = None
    return bodies


def top_level_split(text, sep=","):
    """text cut at each sep outside parentheses, brackets and braces."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    return [*parts, text[start:]]


def local_arrays(body):
    """Every array a (blanked) function body declares, as (type, name, size): every declarator of every declaration, read
    up to its initializer (Task 13's N2: `char c, buf[512];` declares an array too; a subscript in an initializer
    doesn't)."""
    found = []
    for kind, declarators in re.findall(r"^\s+(?!(?:return|else|case|goto|do)\b)(?:static\s+)?(?:const\s+)?"
                                        r"([A-Za-z_]\w*(?: [A-Za-z_]\w*)?)\s+(\**\w+[^;]*);", body, re.M):
        for declarator in top_level_split(declarators):
            head = declarator.split("=", 1)[0]
            found += [(kind, name, size) for name, size in re.findall(r"(\w+)\s*\[([^\]]+)\]", head)]
    return found


class FrameGuard(unittest.TestCase):
    def test_the_frame_guard_reads_every_declarator(self):
        # Task 13's N2: the guard's own reading (local_arrays, which test_console_frames_are_small runs on
        # apps_console.c), on probe lines: an array anywhere in a declaration is one, first declarator or not; a
        # subscript in an initializer or a statement isn't.
        probe = """
static void probe(staged_t *stage, int i, int k) {
    char c, buf[512];
    uint8_t *p, arr[300];
    char shown[64], big[512];
    char one[128];
    char a = 'x', b[4] = "abc";
    staged_t *g = &stage->files[i];
    size_t n = lens[3], m = sizes[k + 1];
    n = table[2];
}
"""
        body = function_bodies(probe)["probe"]
        self.assertEqual(local_arrays(body), [("char", "buf", "512"), ("uint8_t", "arr", "300"), ("char", "shown", "64"),
                                              ("char", "big", "512"), ("char", "one", "128"), ("char", "b", "4")])


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
        cls.sources = [*SOURCES, embed_source(staged), *apps_host.lua_sources()]
        cls.defines = [f'APPS_SOURCE_DIR="{APPS}"']
        cls.harness = apps_host.build("muse_apps_console_harness", cls.sources, tmp.name, include_dirs=cls.includes,
                                      defines=cls.defines, objects=[cjson])

    def run_case(self, case, harness=None):
        try:
            done = apps_host.run(harness or self.harness, case, timeout=120, text=False)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 120 s")
        stderr = done.stderr.decode("utf-8", errors="replace")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:].decode("utf-8", errors="replace") + stderr[-8000:])
        try:
            stdout = done.stdout.decode("utf-8")  # strictly: a reply that isn't UTF-8 fails here (T2)
        except UnicodeDecodeError as e:
            self.fail(f"not UTF-8 ({e}): {done.stdout[max(e.start - 100, 0):e.end + 100]!r}")
        return replies(self, stdout)

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

    def test_a_launch_by_id_prints_nothing(self):  # P2: muse_apps_launch, app.launch's job with a place
        self.run_case("launch_by_id_prints_nothing")

    def test_a_launch_for_alarms_tells_the_app_task_what_came_of_it(self):  # D: started, failed or missing; a revoked launch runs nothing
        self.run_case("launch_for_alarms_tells_the_app_task_what_came_of_it")

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

    def test_list_says_the_stores_hint(self):  # P7: an unmountable partition's hint, which doesn't say to reflash
        self.run_case("list_says_the_stores_hint")

    def test_health_says_the_app_and_the_store(self):  # device.health's "apps": resident null with none loaded; the store's mode and room
        self.run_case("health_says_the_app_and_the_store")

    def test_an_idle_stage_is_freed(self):  # ten minutes with no put, by the app task's rounds; never while the console holds it
        self.run_case("an_idle_stage_is_freed")

    def test_send_returns_the_reply(self):
        self.run_case("send_returns_the_reply")

    def test_foreign_and_unknown_lines(self):
        self.run_case("foreign_and_unknown_lines")

    def test_stage_out_of_memory(self):
        self.run_case("stage_out_of_memory")

    def test_a_repeated_commit_is_busy(self):  # F5: the hold first, then the stage
        self.run_case("a_repeated_commit_is_busy")

    def test_a_link_install_is_one_job(self):  # Task 22: app.install's job on the worker, done once with the result
        self.run_case("a_link_install_is_one_job")

    def test_a_second_install_is_busy(self):  # D14: busy at once, from Link and from USB, both ways
        self.run_case("a_second_install_is_busy")

    def test_link_install_refusals(self):  # unavailable, invalid_params, out_of_memory; the id's message
        self.run_case("link_install_refusals")

    def test_control_list_and_status(self):  # Task 23: the store's apps and the record, on the worker; the last 50 lines (D26)
        self.run_case("control_list_and_status")

    def test_control_waits_at_most_500_ms(self):  # D20: busy after 500 ms, and the late job frees what it made
        self.run_case("control_list_waits_at_most_500_ms")

    def test_control_launch_stop_remove(self):  # D20: queued, done once; the store's refusals as codes
        self.run_case("control_launch_stop_remove")

    def test_control_send(self):  # Task 24: the resident app at once, a stored one launched behind first, the failures with codes
        self.run_case("control_send")

    def test_send_has_one_deadline(self):  # A: the queue's wait, the load, the launch and the reply share 9 s of the VM's 10
        self.run_case("control_send_deadline")

    def test_json_object_valid(self):  # D25: the wrapper main/ validates app.send's data through
        self.run_case("json_object_valid")

    def test_guide_written_or_inline(self):  # D2, D3: the firmware's bytes to the VM, or inline when a write is refused
        self.run_case("guide_written_or_inline")

    def test_api_fonts_are_the_ui_fonts(self):
        # app.api's "fonts" are the ui module's (apps_ui.c's apps_ui_fonts[]), in its order.
        def names(source, array):
            match = re.search(rf"\b{array}\[\]\s*=\s*\{{(.*?)\}};", source, re.S)
            self.assertIsNotNone(match, array)
            return re.findall(r'"(\w+)"', match.group(1))

        self.assertEqual(names((APPS / "apps_control.c").read_text(), "FONTS"),
                         names((APPS / "apps_ui.c").read_text(), "apps_ui_fonts"))

    def test_the_stage_is_bounded(self):  # F6: what was put and a small slack, whatever the files
        self.run_case("the_stage_is_bounded")

    def test_empty_and_missing_files(self):  # T5, F8: stage_read's branches; an empty first piece
        parsed = self.run_case("empty_and_missing_files")
        results = [r[1] for r in parsed if r[0] == "result" and r[1]["op"] == "commit"]
        self.assertEqual([r["error"]["code"] for r in results[:2]], ["read_failed", "read_failed"])
        files = results[2]["payload"]["files"]
        self.assertEqual([(f["path"], f["bytes"]) for f in files], [("main.lua", len(MAIN_HELLO)), ("lib.lua", 0)])

    def test_console_stack(self):  # R13.1: the hook on exactly 3,584 bytes
        harness = apps_host.build("muse_apps_console_stack", self.sources, self.tmp, include_dirs=self.includes,
                                  defines=self.defines, objects=[apps_host.cjson_object()], sanitize=False)
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

        found_bodies = function_bodies(source)
        # Every function the file defines, signatures over several lines included (T6: stage_read's is), is scanned: the
        # brace walk and a pattern that reads definitions across lines must agree.
        defined = set(re.findall(r"^[a-z][^;{}#]*?\b(\w+)\([^;{}]*\)\s*\{", blanked(source), re.M))
        self.assertEqual(sorted(defined - set(found_bodies)), [])
        self.assertEqual(sorted(set(found_bodies) - defined), [])
        self.assertIn("stage_read", found_bodies)
        self.assertGreater(len(found_bodies), 30, "the function bodies weren't found")
        found = 0
        for body in found_bodies.values():
            for kind, name, size in local_arrays(body):  # FrameGuard checks the reading itself
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
