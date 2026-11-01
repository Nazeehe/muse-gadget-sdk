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

"""apps_rt, the runtime of one Muse app (handlers, messages, the crash record, the log ring), and
apphost, the host runner that drives it.

Most tests run apphost on an app folder, a fixture (tests/muse_apps_fixtures) or one made on the spot,
with a script on stdin, and read the one-JSON-object-a-line events it prints. Its stdout is decoded
strictly and every line must parse as JSON whatever the app printed or failed with, and its stderr must
stay empty: a sanitizer report would be there, and aborts the process. The C cases of
tests/muse_apps_rt_harness.c check what a script can't: a heap that refuses at every step, the cuts of
err and of the log tail, the return value of every entry point. Its counting allocator ends every case
at 0 bytes, and apphost's --cycles does the same for whole launches.
"""

import atexit
import hashlib
import json
import os
import random
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
FIXTURES = ROOT / "tests/muse_apps_fixtures"
TALLY = APPS / "examples/tally"
HARNESS_SOURCES = [ROOT / "tests/muse_apps_rt_harness.c", APPS / "apps_rt.c", APPS / "apps_vm.c",
                   APPS / "apps_json.c", APPS / "apps_pkg.c", APPS / "apps_suggest.c", APPS / "apps_ui.c",
                   APPS / "apps_svc.c", APPS / "apps_vmio.c", APPS / "apps_ask.c"]
INCLUDES = [ROOT / "components/lua/src", APPS]

_rt_harness = None


def rt_harness():
    """tests/muse_apps_rt_harness.c, built once for the process (with the sanitizers, as every harness is): HarnessTest here and
    SvcHarnessTest (test_muse_apps_svc.py) run its cases, and two builds of the same sources and flags could not differ."""
    global _rt_harness
    if _rt_harness is None:
        tmp = tempfile.mkdtemp(prefix="muse-apps-rt-harness-")
        atexit.register(shutil.rmtree, tmp, ignore_errors=True)
        _rt_harness = apps_host.build("muse_apps_rt_harness", [*HARNESS_SOURCES, *apps_host.lua_sources()], tmp,
                                      include_dirs=INCLUDES)
    return _rt_harness

# The messages, exactly (apps_rt.h lists them, and apps_vm.h the kills').
NOT_A_TABLE = "main.lua must return a table of handlers: local app = {} … return app"
NO_MESSAGE = "main.lua has no message(event, data) handler"
NOT_RUNNING = "the app is not running"


def header_number(header, name):
    """What `#define NAME 1000000u` is in a header of components/muse/apps."""
    match = re.search(rf"^#define\s+{name}\s+(\d+)u?\b", (APPS / header).read_text(), re.M)
    assert match, f"no #define {name} in {header}"
    return int(match.group(1))


# The budgets and the duty cycle's rate, in VM instructions, as the headers have them: Task 27 set them from the
# board's measured speed (apps_vm.h, apps_rt.h). Tests size their work from these and build the kill messages from them
# (D12), so a retune changes no test; test_the_budgets_are_the_measured_ones (test_muse_apps_svc.py) pins the values,
# and the harnesses print them as they were compiled.
CALL = header_number("apps_vm.h", "APPS_BUDGET_CALL")
START = header_number("apps_vm.h", "APPS_BUDGET_START")
DUTY = header_number("apps_rt.h", "APPS_DUTY_INSTR_PER_S")

# The speed they were set from: VM instructions a second on the Waveshare 1.75C, measured on 2026-10-30 with
# tests/muse_apps_fixtures/bench. Its `run` message starts a timer.every whose callback runs `for i = 1, n do end` (one VM
# instruction a turn, with the count hook on) and times itself with time.mono(); a run is 21 callbacks of n = 400,000, and the
# rate is >app.status's `instructions` after the run less before, over the time spent inside the callbacks (`busy_ms`), so the
# gaps between ticks don't count. Three runs on one board: 2,524,423, 2,521,392 and 2,522,907 a second; this is their mean.
# Table and call work (the fixture's `mix`) runs about 2.1 M. Re-measure this way before retuning:
# test_the_budgets_are_the_measured_ones (test_muse_apps_svc.py) derives CALL, START and DUTY from this number, and the docs'
# quoted speeds and times come from it (test_muse_apps_link.py).
MEASURED_RATE = 2_522_907


def budget_message(n):
    return f"stopped: ran more than {n} VM instructions in one callback (use timer.every for long work)"


BUDGET_CALL = budget_message(CALL)
BUDGET_START = budget_message(START)
# The commands apphost lists when it doesn't know one.
COMMANDS = ("(commands: message, stop, start, show, hide, state, log, tap, dump, advance, dark, sync, timers, restart, "
            "install, other_alarm, other_cancel, hold_alarms)")
# The ring: a line takes its length plus one of 8192 bytes; print cuts a line at 200 bytes.
RING_BYTES = 8192
LINE_MAX = 200


# A module nested NEST deep compiles from the main thread (its parser takes a level of the C-call count for each
# level, 40 in all) and not from a coroutine DEPTH pcalls deep.
NEST = 25
DEPTH = 20


def nomem(kb):
    return f"out of memory: this app may use {kb} KB (memory_kb in manifest.json)"


def unknown_handler(key):
    return f"main.lua returned unknown handler '{key}'"


def not_a_file(name, files):
    listed = json.dumps(files, separators=(",", ":"))
    return f"require: '{name}' is not one of this app's files (manifest.json \"files\": {listed})"


def compile_failed(message, *lines):
    """What the install says of a package whose Lua doesn't compile (apps_pkg_compile_message), and apphost with it:
    Lua's message, and the lines of the file around the one it names, the line itself marked with '>'."""
    return "\n".join(["compile failed: " + message, *lines])


def ring_suffix(lines, cap=RING_BYTES):
    """The lines the ring holds after these were printed: the longest end of them that fits."""
    kept, used = [], 0
    for line in reversed(lines):
        cost = len(line.encode()) + 1
        if used + cost > cap:
            break
        kept.append(line)
        used += cost
    return kept[::-1]


class Run:
    """What one apphost run printed: its events, in order, and how it ended."""

    def __init__(self, done):
        self.returncode = done.returncode
        self.stderr = done.stderr
        text = done.stdout.decode("utf-8")  # strictly: invalid UTF-8 on stdout fails the test
        lines = text.split("\n")
        assert lines[-1] == "", "the last line is unfinished"
        self.lines = lines[:-1]
        self.events = [json.loads(line) for line in self.lines]  # raw control bytes in a string fail here too
        for event in self.events:
            assert isinstance(event, dict) and isinstance(event.get("event"), str), event

    def kinds(self):
        return [e["event"] for e in self.events]

    def of(self, kind):
        return [e for e in self.events if e["event"] == kind]

    def messages(self, kind):
        return [e["message"] for e in self.of(kind)]

    def logs(self):
        return [e["line"] for e in self.of("log")]

    def between_states(self, which):
        """The events between the which-th and the next `state` event: a command's own output."""
        marks = [i for i, e in enumerate(self.events) if e["event"] == "state"]
        return self.events[marks[which] + 1:marks[which + 1]]


class AppHostTestCase(unittest.TestCase):
    # Added to the environment of every apphost run of the class: what a test that must hold on any machine sets (TZ).
    host_env = None

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        cls.apphost = apps_host.build_apphost(tmp.name)

    def make_app(self, app_id, main, files=None, manifest=None):
        """A folder app_id with main.lua (text or bytes), the other files {path: text or bytes} and a
        manifest that lists them, plus the keys of `manifest`. Returns the folder."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        folder = Path(tmp.name) / app_id
        folder.mkdir()
        doc = {"id": app_id, "name": app_id, "api": 1}
        if files:
            doc["files"] = list(files)
        doc.update(manifest or {})
        (folder / "manifest.json").write_text(json.dumps(doc))
        for path, content in {"main.lua": main, **(files or {})}.items():
            target = folder / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content if isinstance(content, bytes) else content.encode())
        return folder

    def host(self, app, *, script=None, mem_kb=None, cycles=None, rc=0, timeout=60, compile=True):
        """Runs apphost on app with the script lines on stdin; expects exit status rc and an empty stderr. Unless
        compile is false (--no-compile: the tests of what the runtime does with a module that doesn't compile, which
        the install never lets in), the package is compiled as the install compiles it before anything runs."""
        args = [] if compile else ["--no-compile"]
        if mem_kb is not None:
            args += ["--mem-kb", mem_kb]
        if cycles is not None:
            args += ["--cycles", cycles]
        if script is not None:
            args += ["--script", "-"]
        text = "".join(f"{line}\n" for line in script) if isinstance(script, (list, tuple)) else script
        done = apps_host.run(self.apphost, *args, app, input=text.encode() if text else None, text=False,
                             timeout=timeout, env=self.host_env)
        self.assertEqual(done.returncode, rc,
                         f"{done.stdout.decode(errors='replace')}\n{done.stderr.decode(errors='replace')}")
        self.assertEqual(done.stderr, b"")
        return Run(done)

    def run_app(self, main, script=None, **kw):
        """A one-file app, run."""
        return self.host(self.make_app("scratch", main), script=script, **kw)


class RuntimeTest(AppHostTestCase):
    # ---- Handlers (the brief; R8.1; B9) ----

    def test_handlers_and_unknown_key_warning(self):
        run = self.host(FIXTURES / "handlers")
        self.assertEqual(run.kinds(), ["warning", "started"])
        self.assertEqual(run.events[0]["message"], unknown_handler("on_tap"))
        self.assertEqual(run.events[1], {"event": "started", "id": "handlers", "name": "Handlers"})

    def test_unknown_handler_suggests_only_a_near_name(self):
        # B9: apps_suggest's rule (distance 2 or less, and less than the name's length); no suffix otherwise.
        run = self.run_app("local f = function() end\n"
                           "return {strat = f, Start = f, mesage = f, shwo = f, hid = f, on_tap = f, update = f}")
        self.assertEqual(sorted(run.messages("warning")), sorted([
            unknown_handler("strat") + " (did you mean 'start'?)",
            unknown_handler("Start") + " (did you mean 'start'?)",
            unknown_handler("mesage") + " (did you mean 'message'?)",
            unknown_handler("shwo") + " (did you mean 'show'?)",
            unknown_handler("hid") + " (did you mean 'hide'?)",
            unknown_handler("on_tap"),
            unknown_handler("update"),
        ]))
        self.assertEqual(run.kinds()[-1], "started")

    def test_keys_that_are_not_text_are_unknown_handlers_too(self):
        run = self.run_app("local f = function() end\nreturn {f, [2.5] = f, [true] = f, start = f}")
        self.assertEqual(sorted(run.messages("warning")), sorted([
            unknown_handler("1"), unknown_handler("2.5"), unknown_handler("true")]))

    def test_unknown_handler_warnings_stay_whole_in_512_bytes(self):
        keys = [f"key_{i:03d}" for i in range(100)]
        run = self.run_app("local f = function() end\nreturn {" + ", ".join(f"{k} = f" for k in keys) + "}")
        warnings = run.messages("warning")
        self.assertTrue(0 < len(warnings) < 100, len(warnings))
        for w in warnings:
            self.assertRegex(w, r"^main\.lua returned unknown handler 'key_\d{3}'$")
        self.assertLessEqual(len("; ".join(warnings)), 511)
        self.assertGreater(len("; ".join(warnings)) + len("; main.lua returned unknown handler 'key_000'"), 511)

    def test_a_hostile_key_is_shown_safely(self):
        run = self.run_app(b'local f = function() end\nreturn {["a\\n@app \\255\\27x"] = f}')
        self.assertEqual(run.messages("warning"), [unknown_handler("a?@app ??x")])

    def test_main_must_return_table(self):
        for source in ("return 1", "return", "return nil", "return 'x'", "return print", "return true"):
            with self.subTest(source):
                run = self.run_app(source, rc=1)
                self.assertEqual(run.events, [{"event": "crash", "message": NOT_A_TABLE}])

    def test_the_message_names_the_manifests_main_file(self):
        app = self.make_app("scratch", "", files={"ui/app.lua": "return 1"}, manifest={"main": "ui/app.lua"})
        run = self.host(app, rc=1)
        self.assertEqual(run.messages("crash"), [NOT_A_TABLE.replace("main.lua", "ui/app.lua")])

    def test_a_handler_that_is_not_a_function_crashes_the_load(self):
        for key, value, kind in (("start", "5", "number"), ("stop", "'x'", "string"), ("show", "true", "boolean"),
                                 ("hide", "{}", "table"), ("message", "false", "boolean"), ("alarm", "{}", "table")):
            with self.subTest(key):
                run = self.run_app(f"return {{{key} = {value}}}", rc=1)
                self.assertEqual(run.events, [{"event": "crash", "message":
                                               f"main.lua returned '{key}' as a {kind}; handlers must be functions"}])

    def test_every_handler_is_optional(self):
        run = self.run_app("return {}", script=["show", "hide", "message ping", "state", "stop", "state",
                                                "message ping", "start", "state"])
        self.assertEqual(run.events, [
            {"event": "started", "id": "scratch", "name": "scratch"},
            {"event": "error", "message": NO_MESSAGE},
            {"event": "state", "state": "running"},
            {"event": "state", "state": "stopped"},
            {"event": "error", "message": NOT_RUNNING},
            {"event": "started", "id": "scratch", "name": "scratch"},
            {"event": "state", "state": "running"},
        ])

    def test_handlers_run_with_their_arguments_and_start_again_after_stop(self):
        run = self.run_app(
            "local app = {}\n"
            "for _, name in ipairs({'start', 'stop', 'show', 'hide'}) do\n"
            "    app[name] = function(...) print(name, select('#', ...)) end\n"
            "end\n"
            "return app", script=["show", "hide", "stop", "show", "start", "stop", "stop", "state"])
        self.assertEqual(run.logs(), ["start\t0", "show\t0", "hide\t0", "stop\t0",
                                      "start\t0", "stop\t0"])  # show after stop, the second stop: nothing runs
        self.assertEqual(run.events[-1], {"event": "state", "state": "stopped"})

    def test_start_when_running_runs_nothing(self):
        run = self.run_app("return {start = function() print('start') end}", script=["start", "state"])
        self.assertEqual(run.logs(), ["start"])
        self.assertEqual(run.kinds(), ["log", "started", "started", "state"])

    # ---- Messages (the brief; R8.4; B5) ----

    def test_message_reply_and_error_isolated(self):
        run = self.host(FIXTURES / "messages", script=["message status", "message bad", "state", "message status"])
        self.assertEqual(run.kinds(), ["started", "reply", "error", "state", "reply"])
        self.assertEqual(run.lines[1], '{"event":"reply","data":{"state":"idle"}}')
        self.assertRegex(run.events[2]["message"], r"^main\.lua:\d+: unknown event$")
        self.assertEqual(run.events[3], {"event": "state", "state": "running"})
        self.assertEqual(run.lines[4], '{"event":"reply","data":{"state":"idle"}}')

    def test_message_data_reaches_the_handler(self):
        run = self.run_app(
            "return {message = function(event, data) return {event = event, data = data, kind = type(data)} end}",
            script=["message echo", "message echo   {\"a\": [1, 2.5, \"x\", null, true], \"b\": {\"c\": 1}}",
                    "message echo 7", "message echo \"text\"", "message echo null"])
        replies = [e["data"] for e in run.of("reply")]
        self.assertEqual(replies, [
            {"event": "echo", "kind": "nil"},
            {"event": "echo", "kind": "table", "data": {"a": [1, 2.5, "x", None, True], "b": {"c": 1}}},
            {"event": "echo", "kind": "number", "data": 7},
            {"event": "echo", "kind": "string", "data": "text"},
            {"event": "echo", "kind": "userdata", "data": None},  # null decodes as json.null, as json.decode's
        ])

    def test_a_nil_reply_is_null(self):
        for returns in ("", "return nil", "return nil, 1", "return false", "return 1, 2", "return 'a\"b'"):
            with self.subTest(returns):
                run = self.run_app(f"return {{message = function(e, d) {returns} end}}", script=["message x"])
                want = {"": None, "return nil": None, "return nil, 1": None, "return false": False,
                        "return 1, 2": 1, "return 'a\"b'": 'a"b'}[returns]
                self.assertEqual(run.events[1], {"event": "reply", "data": want})
        run = self.run_app("return {message = function() end}", script=["message x"])
        self.assertEqual(run.lines[1], '{"event":"reply","data":null}')

    def test_data_that_is_not_json_is_the_callers_error(self):
        run = self.run_app("return {message = function(e, d) return {ok = true} end}",
                           script=["message x {\"a\":}", "message x [1,", "message x {", "state", "message x"])
        self.assertEqual(run.messages("error"), ["data: unexpected '}' at byte 6",
                                                 "data: unexpected end of input at byte 4",
                                                 "data: unexpected end of input at byte 2"])
        self.assertEqual(run.of("state"), [{"event": "state", "state": "running"}])
        self.assertEqual(run.events[-1], {"event": "reply", "data": {"ok": True}})  # and it keeps answering

    def test_data_limits_are_the_decoders(self):
        run = self.run_app("return {message = function(e, d) return 1 end}",
                           script=["message x " + "[" * 17 + "]" * 17, "message x " + "[" * 16 + "]" * 16,
                                   "message x \"" + "a" * 33000 + "\""])
        self.assertEqual(run.messages("error"), ["data: nested deeper than 16", "data: input larger than 32 KB"])
        self.assertEqual(len(run.of("reply")), 1)

    def test_a_reply_that_cannot_be_encoded_keeps_the_app_running(self):
        cases = {
            "function() end": "message: can't encode a function",
            "{1, print}": "message: can't encode a function",
            "0/0": "message: can't encode NaN or infinity",
            "'\\255'": "message: can't encode a string that isn't valid UTF-8",
            "coroutine.create(print)": "message: can't encode a thread",
            "{[{}] = 1}": "message: can't encode a table key",
        }
        deep = "{" * 17 + "}" * 17
        cases[deep] = "message: nested deeper than 16"
        for value, want in cases.items():
            with self.subTest(value):
                run = self.run_app(f"return {{message = function() return {value} end}}",
                                   script=["message x", "state"])
                self.assertEqual(run.events[1:], [{"event": "error", "message": want},
                                                  {"event": "state", "state": "running"}])

    def test_a_handlers_error_is_its_message_without_the_traceback(self):
        run = self.run_app(
            "local app = {}\n"
            "function app.message(event)\n"
            "    if event == 'index' then local t; t.x = 1 end\n"
            "    if event == 'plain' then error('plain', 0) end\n"
            "    if event == 'table' then error({code = 1}) end\n"
            "    if event == 'nil' then error() end\n"
            "    if event == 'number' then error(42, 0) end\n"
            "    if event == 'fake' then error('boom\\nstack traceback:\\nfake', 0) end\n"
            "end\n"
            "return app",
            script=["message index", "message plain", "message table", "message nil", "message number",
                    "message fake", "state"])
        self.assertEqual(run.messages("error")[:1], ["main.lua:3: attempt to index a nil value (local 't')"])
        self.assertEqual(run.messages("error")[1:6], ["plain", "(error object is a table value)",
                                                      "(error object is a nil value)", "42", "boom"])
        self.assertNotIn("stack traceback:", "\n".join(run.messages("error")))
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})

    def test_deep_recursion_in_a_handler_runs_out_of_memory_and_crashes(self):
        # The Lua stack grows in the app's capped heap, so a recursion that never ends is the kill, not
        # Lua's "stack overflow" error (Task 5).
        run = self.run_app("return {message = function() local function f() return f() + 1 end return f() end}",
                           script=["message deep", "state"], rc=1)
        self.assertEqual(run.events[1:], [{"event": "crash", "message": nomem(256)},
                                          {"event": "state", "state": "crashed"}])

    def test_a_kill_in_message_is_a_crash_and_the_crash_is_final(self):
        run = self.run_app("return {message = function() while true do end end, show = function() print('show') end}",
                           script=["message loop", "state", "message loop", "show", "state"], rc=1)
        self.assertEqual(run.events[1:], [
            {"event": "crash", "message": BUDGET_CALL},
            {"event": "state", "state": "crashed"},
            {"event": "error", "message": BUDGET_CALL},  # R8.2: a crashed app answers with its crash record
            {"event": "state", "state": "crashed"},
        ])
        self.assertEqual(run.logs(), [])  # show() didn't run

    def test_a_message_with_data_near_the_cap_is_a_crash_not_an_abort(self):
        # B5: decoding data allocates in the capped heap, inside the protected call.
        numbers = "[" + ",".join(["1"] * 16000) + "]"  # 32 KB of text, 256 KB of Lua array
        run = self.run_app("return {message = function(e, d) return #d end}", mem_kb=64,
                           script=[f"message big {numbers}", "state", "message big []"], rc=1)
        self.assertEqual(run.events[1:], [
            {"event": "crash", "message": nomem(64)},
            {"event": "state", "state": "crashed"},
            {"event": "error", "message": nomem(64)},
        ])
        # The same text fits a bigger heap.
        run = self.run_app("return {message = function(e, d) return #d end}", mem_kb=1024,
                           script=[f"message big {numbers}"])
        self.assertEqual(run.events[1], {"event": "reply", "data": 16000})

    def test_a_huge_reply_is_a_crash_not_an_abort(self):
        # B5: the encoder builds its text in the app's heap. Ten references to one 20 KB string are 20 KB of
        # heap and 200 KB of JSON, so it is the encoding that runs out.
        main = ("local s = string.rep('x', 20000)\n"
                "return {message = function() local t = {} for i = 1, 10 do t[i] = s end return t end}")
        run = self.run_app(main, mem_kb=256, script=["message huge", "state"], rc=1)
        self.assertEqual(run.events[1:], [{"event": "crash", "message": nomem(256)},
                                          {"event": "state", "state": "crashed"}])
        run = self.run_app(main, mem_kb=1024, script=["message huge"])
        self.assertEqual(run.events[1], {"event": "reply", "data": ["x" * 20000] * 10})

    # ---- Crashes (the brief; R8.2) ----

    def test_crash_in_start_reports_traceback_and_log(self):
        run = self.host(FIXTURES / "crash_start", script=["state", "log"], rc=1)
        self.assertEqual(run.kinds(), ["log", "crash", "state", "log"])
        message = run.events[1]["message"]
        self.assertIn("attempt to index a nil value (local 'arc')", message)
        self.assertIn("stack traceback:", message)
        self.assertRegex(message, r"^main\.lua:\d+: attempt to index a nil value \(local 'arc'\)\nstack traceback:\n\t")
        self.assertNotIn("[C]:", message)
        self.assertEqual(run.events[0], {"event": "log", "app": "crash_start", "line": "booting"})
        self.assertEqual(run.events[2], {"event": "state", "state": "crashed"})
        self.assertEqual(run.events[3], {"event": "log", "app": "crash_start", "line": "booting"})  # the ring kept it

    def test_crash_in_main_top_level(self):
        run = self.run_app("print('top')\nlocal t\nt.x = 1\nreturn {}", rc=1)
        self.assertEqual(run.kinds(), ["log", "crash"])
        self.assertRegex(run.events[1]["message"],
                         r"^main\.lua:3: attempt to index a nil value \(local 't'\)\nstack traceback:\n\t")

    def test_syntax_error_in_main_is_refused_in_the_installs_words(self):
        # The compile the install does comes first and says what it says: "compile failed: ", Lua's message and the
        # lines around the one it names. Nothing ran, and that one event is all there is.
        run = self.run_app("local x = 1\nx = = 2\n", rc=1)
        self.assertEqual(run.events, [{"event": "crash", "message": compile_failed(
            "main.lua:2: unexpected symbol near '='", " 1| local x = 1", ">2| x = = 2")}])
        run = self.run_app("local x <close> = nil\nreturn {}", rc=1)
        self.assertEqual(run.messages("crash"), [compile_failed(
            "main.lua:1: '<close>' variables are not available in Muse apps", ">1| local x <close> = nil",
            " 2| return {}")])

    def test_syntax_error_in_main_without_the_compile_step_is_lua_s_own_message(self):
        # --no-compile: the runtime makes the app itself, and the crash event is what Lua said.
        run = self.run_app("local x = 1\nx = = 2\n", rc=1, compile=False)
        self.assertEqual(run.events, [{"event": "crash", "message": "main.lua:2: unexpected symbol near '='"}])
        run = self.run_app("local x <close> = nil\nreturn {}", rc=1, compile=False)
        self.assertEqual(run.messages("crash"), ["main.lua:1: '<close>' variables are not available in Muse apps"])

    def test_a_compile_error_shows_the_token_as_clean_text(self):
        # Lua's message for an unfinished string quotes the token, raw bytes and all. The runtime cleans it like any
        # text this module hands out (--no-compile, below, and a module's error the same way, in one line), and so
        # does the install's message, the token and the source line under it: apps_pkg_compile_message makes both from
        # the same rule, so the board's reply and check's crash event are one text, a '?' for each control byte,
        # each C1 character and each byte that isn't UTF-8 (fix round 1, A).
        run = self.run_app(b"x = '\x01\x1b[2J\xff\n", rc=1)
        self.assertEqual(run.messages("crash"), [compile_failed(
            "main.lua:1: unfinished string near ''??[2J?'", ">1| x = '??[2J?")])
        # A C1 character (U+009B is CSI) next to the error, in a comment: the lines show it as '?', and so does
        # whatever the terminal gets.
        run = self.run_app("-- \x1b]0;owned\x07 \u009b2J\nlocal b = = 2\nreturn {}\n", rc=1)
        self.assertEqual(run.messages("crash"), [compile_failed(
            "main.lua:2: unexpected symbol near '='", " 1| -- ?]0;owned? ?2J", ">2| local b = = 2", " 3| return {}")])
        run = self.run_app(b"x = '\x01\x1b[2J\xff\n", rc=1, compile=False)
        self.assertEqual(run.messages("crash"), ["main.lua:1: unfinished string near ''??[2J?'"])
        app = self.make_app("scratch", "return {message = function() return require('m') end}",
                            files={"m.lua": b"x = '\x01\x1b[2J\xff\n"})
        run = self.host(app, script=["message x"], compile=False)
        self.assertEqual(run.messages("error"), ["main.lua:1: require: m.lua:1: unfinished string near ''??[2J?'"])
        # One line: a token can hold a newline ('\x and the end of the line is "hexadecimal digit expected").
        app = self.make_app("scratch", "return {message = function() return {pcall(require, 'm')} end}",
                            files={"m.lua": b"x = '\\x\n"})
        run = self.host(app, script=["message x"], compile=False)
        self.assertEqual(run.of("reply")[0]["data"],
                         [False, "require: m.lua:1: hexadecimal digit expected near ''\\x?'"])

    def test_after_a_crash_nothing_runs(self):
        run = self.run_app(
            "local app = {}\n"
            "for _, name in ipairs({'stop', 'show', 'hide', 'message', 'alarm'}) do\n"
            "    app[name] = function() print('ran ' .. name) return {} end\n"
            "end\n"
            "function app.start() error('boom', 0) end\n"
            "return app", script=["message x", "start", "show", "hide", "stop", "state", "log"], rc=1)
        boom = "boom\nstack traceback:\n\tmain.lua:5: in function <main.lua:5>"
        self.assertEqual(run.events, [
            {"event": "crash", "message": boom},
            {"event": "error", "message": boom},  # message: the crash record
            {"event": "error", "message": boom},  # start: false, with the record
            {"event": "state", "state": "crashed"},
        ])

    def test_stop_that_fails_crashes_the_app(self):
        run = self.run_app("return {stop = function() error('late', 0) end}", script=["stop", "state"], rc=1)
        self.assertEqual(run.events[1]["event"], "crash")
        self.assertTrue(run.events[1]["message"].startswith("late\nstack traceback:\n"))
        self.assertEqual(run.events[2], {"event": "state", "state": "crashed"})

    def test_show_and_hide_that_fail_crash_the_app(self):
        for script in ("show", "hide"):
            with self.subTest(script):
                run = self.run_app("return {show = function() local t; t.x = 1 end, hide = function() error('h', 0) end}",
                                   script=[script, "state"], rc=1)
                self.assertEqual(run.kinds(), ["started", "crash", "state"])

    # ---- Budgets (R8.3) ----

    def test_budgets_main_chunk_and_start_each_get_start_and_the_rest_get_call(self):
        # A loop between the two budgets is over CALL and under START; one over START by half a CALL is over both (sized
        # from the constants, D12).
        loop = "for i = 1, {n} do end"
        self.assertLess(CALL * 5 // 4, START)
        three, six = loop.format(n=(CALL + START) // 2), loop.format(n=START + CALL // 2)
        cases = {
            "top level under START": (f"{three}\nreturn {{}}", None, ["started"]),
            "top level over START": (f"{six}\nreturn {{}}", None, [("crash", BUDGET_START)]),
            "start under START": (f"return {{start = function() {three} end}}", None, ["started"]),
            "start over START": (f"return {{start = function() {six} end}}", None, [("crash", BUDGET_START)]),
            "top level and start each have their own": (f"{three}\nreturn {{start = function() {three} end}}",
                                                        None, ["started"]),
            "message over CALL": (f"return {{message = function() {three} end}}", "message x",
                                  ["started", ("crash", BUDGET_CALL)]),
            "show over CALL": (f"return {{show = function() {three} end}}", "show", ["started", ("crash", BUDGET_CALL)]),
            "hide over CALL": (f"return {{hide = function() {three} end}}", "hide", ["started", ("crash", BUDGET_CALL)]),
            "stop over CALL": (f"return {{stop = function() {three} end}}", "stop", ["started", ("crash", BUDGET_CALL)]),
        }
        for name, (main, command, want) in cases.items():
            with self.subTest(name):
                run = self.run_app(main, script=[command] if command else None,
                                   rc=1 if any(isinstance(w, tuple) for w in want) else 0)
                got = [(e["event"], e["message"]) if e["event"] == "crash" else e["event"] for e in run.events]
                self.assertEqual(got, want)

    def test_every_call_has_a_fresh_budget(self):
        # Three quarters of CALL is under it; three in a row aren't one call.
        run = self.run_app(f"return {{message = function() for i = 1, {CALL * 3 // 4} do end return 1 end}}",
                           script=["message a"] * 3)
        self.assertEqual(run.kinds(), ["started", "reply", "reply", "reply"])

    # ---- The log (the brief; R8.5) ----

    def test_print_ring_wraps(self):
        lines = [f"line {i}" for i in range(1, 2001)]
        run = self.host(FIXTURES / "print_ring", script=["state", "log 3", "state", "log 100000", "state"])
        self.assertEqual(run.logs()[:2000], lines)  # every line goes out through the port as it is printed
        self.assertEqual([e["line"] for e in run.between_states(0)], lines[-3:])
        kept = [e["line"] for e in run.between_states(1)]
        self.assertEqual(kept, ring_suffix(lines))
        self.assertLessEqual(sum(len(l) + 1 for l in kept), RING_BYTES)
        self.assertGreater(sum(len(l) + 1 for l in kept), RING_BYTES - 10)  # and the ring is full
        self.assertGreater(len(kept), 700)

    def test_the_ring_counts_every_line_as_its_length_plus_one(self):
        # 200-byte lines: 40 of them take 8040 bytes, a 41st would take 8241.
        main = "return {start = function() for i = 1, 100 do print(string.rep(string.char(64 + i % 26), 200)) end end}"
        run = self.run_app(main, script=["state", "log 1000", "state"])
        kept = [e["line"] for e in run.between_states(0)]
        self.assertEqual(len(kept), 40)
        self.assertEqual(kept, [chr(64 + i % 26) * 200 for i in range(61, 101)])
        # Empty lines cost one byte each. A print costs 250 instructions (LINE_COST), so the 9000 lines go in messages of
        # a third of what CALL allows each (a callback logs at most CALL / 250 lines).
        per_call = CALL // 250 // 3
        calls = 9000 // per_call + 1
        run = self.run_app("return {message = function(e, n) for i = 1, n do print() end if e == 'last' then print('last') end "
                           "end}", script=[f"message more {per_call}"] * (calls - 1) + [f"message last {per_call}", "state",
                                                                                        "log 100000", "state"])
        kept = [e["line"] for e in run.between_states(0)]
        self.assertEqual(kept, [""] * (RING_BYTES - 5) + ["last"])

    def test_print_joins_arguments_with_tabs_and_splits_lines(self):
        run = self.run_app(
            "return {start = function()\n"
            "    print('a\\nb')\n"
            "    print(1, nil, true, false, 'x', 1.5, 3.0)\n"
            "    print()\n"
            "    print('tail\\n')\n"
            "    print(setmetatable({}, {__tostring = function() return 'custom' end}))\n"
            "    print('p\\nq', 'r\\ns')\n"
            "end}")
        self.assertEqual(run.logs(), ["a", "b", "1\tnil\ttrue\tfalse\tx\t1.5\t3.0", "", "tail", "", "custom",
                                      "p", "q\tr", "s"])
        self.assertTrue(all(e["app"] == "scratch" for e in run.of("log")))

    def test_print_text_is_safe(self):
        hostile = {
            "\\27[2J": "?[2J",                      # a terminal escape sequence
            "a\\0b": "a?b",                         # NUL
            "\\127del": "?del",
            "x\\1\\2\\31y": "x???y",
            "line\\r": "line?",
            "tab\\there": "tab\there",              # tab stays
            "\\255": "?",
            "\\128": "?",                           # a stray continuation byte
            "\\226\\130": "??",                     # a cut 3-byte sequence
            "\\192\\175": "??",                     # an overlong '/'
            "\\237\\160\\128": "???",               # a surrogate
            "\\244\\144\\128\\128": "????",         # past U+10FFFF
            "\\194\\155": "?",                      # U+009B, the 8-bit CSI: one character, one '?'
            "\\194\\159x": "?x",
            "\\194\\160": "\u00a0",                # U+00A0 is no control
            "h\\195\\169llo w\\195\\182rld": "héllo wörld",
            "\\230\\151\\165\\230\\156\\172": "日本",
            "\\240\\159\\152\\128": "\U0001F600",
            "\\225\\136\\180\\127": "ሴ?",
        }
        main = "return {start = function()\n" + "".join(f'    print("{k}")\n' for k in hostile) + "end}"
        run = self.run_app(main, script=["log"])
        want = list(hostile.values())
        self.assertEqual(run.logs()[:len(want)], want)
        self.assertEqual(run.logs()[len(want):], want)  # the ring holds the same text
        for line in run.logs():
            self.assertIsNone(re.search("[\x00-\x08\x0a-\x1f\x7f\x80-\x9f]", line), repr(line))

    def test_print_lines_are_cut_at_200_bytes_on_a_character_boundary(self):
        cases = [
            ("('x'):rep(300)", "x" * 200),
            ("('x'):rep(200)", "x" * 200),
            ("('x'):rep(201)", "x" * 200),
            ("('x'):rep(199) .. '\\195\\169'", "x" * 199),                 # an e-acute would end at 201
            ("('x'):rep(198) .. '\\195\\169'", "x" * 198 + "é"),      # and at 200 it fits
            ("('x'):rep(197) .. '\\240\\159\\152\\128'", "x" * 197),
            ("('x'):rep(196) .. '\\240\\159\\152\\128'", "x" * 196 + "\U0001F600"),
            ("('x'):rep(199) .. '\\255'", "x" * 199 + "?"),                # a replacement takes a byte too
            ("('x'):rep(200) .. '\\255'", "x" * 200),
            ("('x'):rep(300) .. '\\nshort'", "x" * 200),                   # the cut is per line
            ("('x'):rep(150), ('y'):rep(150)", "x" * 150 + "\t" + "y" * 49),
        ]
        main = "return {start = function()\n" + "".join(f"    print({c})\n" for c, _ in cases) + "end}"
        run = self.run_app(main)
        want = []
        for (code, line) in cases:
            want.append(line)
            if "short" in code:
                want.append("short")
        self.assertEqual(run.logs(), want)
        for line in run.logs():
            self.assertLessEqual(len(line.encode()), LINE_MAX)

    def test_a_call_that_prints_without_end_is_killed(self):
        # Each line costs the call's budget (250 instructions), and the wall clock is read at each: START / 250 lines at
        # most.
        run = self.run_app("return {start = function() while true do print('x') end end}", rc=1)
        self.assertEqual(run.messages("crash"), [BUDGET_START])
        self.assertLess(len(run.logs()), 60000)

    def test_one_print_of_200000_lines_is_killed_too(self):
        run = self.run_app("return {start = function() print(string.rep('x\\n', 200000)) end}", mem_kb=1024, rc=1,
                           timeout=30)
        self.assertEqual(run.messages("crash"), [BUDGET_START])
        self.assertLess(len(run.logs()), 30000)

    def test_a_long_line_without_a_newline_is_cheap(self):
        run = self.run_app("return {start = function() print(string.rep('y', 400000)) end}", mem_kb=1024)
        self.assertEqual(run.logs(), ["y" * 200])
        self.assertEqual(run.kinds()[-1], "started")

    def test_print_charges_for_the_length_of_what_it_reads(self):
        # A string is read in place, so it costs the heap nothing, but not the CPU: 400 KB is 25000
        # instructions a print, and 250 for its line. Half of what CALL holds fit one callback and twice as many don't.
        fit = CALL // 25250
        few, many = fit // 2, fit * 2
        main = ("local s = string.rep('y', 400000)\n"
                f"return {{message = function(e) for i = 1, e == 'few' and {few} or {many} do print(s) end end}}")
        run = self.run_app(main, mem_kb=1024, script=["message few", "message many"], rc=1)
        kinds = run.kinds()
        self.assertEqual(kinds[:few + 2], ["started"] + ["log"] * few + ["reply"])
        self.assertEqual(kinds[-1], "crash")
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])
        printed = len(kinds) - few - 3  # the lines the killed callback got to print: about what CALL holds
        self.assertTrue(fit * 9 // 10 < printed <= fit + 1, (printed, fit))

    def test_print_when_tostring_fails(self):
        run = self.run_app("return {start = function() print('before') print(setmetatable({}, {__tostring = "
                           "function() error('no text', 0) end})) end}", rc=1)
        self.assertEqual(run.logs(), ["before"])
        self.assertTrue(run.messages("crash")[0].startswith("no text\nstack traceback:"))

    # ---- The text of errors ----

    def test_error_text_is_valid_text_whatever_the_app_raised(self):
        run = self.run_app(
            "return {message = function(e) error('bad\\27[2J\\255 end\\nnext\\tline\\194\\155\\0gone', 0) end,\n"
            "        start = function() print('x') end}",
            script=["message x", "state"])
        self.assertEqual(run.messages("error"), ["bad?[2J? end\nnext\tline?"])  # a NUL ends the C string
        run = self.run_app("return {start = function() error('\\1\\255\\226\\130', 0) end}", rc=1)
        self.assertRegex(run.messages("crash")[0], r"^\?\?\?\?\nstack traceback:\n\t")
        # A crash record is cleaned as print's lines are, a C1 control character (U+009B is CSI) included, and
        # keeps "\n" and tab (R8.4, R8.5 as ratified in Task 8's fix round 1).
        run = self.run_app("return {start = function() error('a\\194\\155b\\194\\133c\\rd\\te\\nf', 0) end}", rc=1)
        self.assertRegex(run.messages("crash")[0], r"^a\?b\?c\?d\te\nf\nstack traceback:\n\t")

    def test_valid_utf8_in_error_text_stays(self):
        run = self.run_app("return {message = function() error('h\\195\\169llo \\240\\159\\152\\128', 0) end}",
                           script=["message x"])
        self.assertEqual(run.messages("error"), ["héllo \U0001F600"])

    # ---- require (the brief; X10) ----

    def test_require_only_manifest_files(self):
        files = ["util.lua", "lib/helper.lua"]
        run = self.host(FIXTURES / "require_util", script=[
            "message answer",
            'message load {"name": "util"}', 'message load {"name": "util.lua"}',
            'message load {"name": "lib/helper"}', 'message load {"name": "lib/helper.lua"}',
            'message load {"name": "nope"}', 'message load {"name": "main"}', 'message load {"name": "main.lua"}',
            'message load {"name": "../x"}', 'message load {"name": "util.json"}', 'message load {"name": ""}',
            'message load {"name": "util.lua.lua"}', 'message load {"name": "Util"}',
            "message same",
        ])
        replies = [e["data"] for e in run.of("reply")]
        self.assertEqual(replies[0], {"answer": 42})
        self.assertEqual(replies[1:5], [{"ok": True, "kind": "table"}] * 4)
        for reply, name in zip(replies[5:13], ["nope", "main", "main.lua", "../x", "util.json", "", "util.lua.lua", "Util"]):
            self.assertEqual(reply, {"ok": False, "error": not_a_file(name, files)})
        self.assertEqual(replies[13], {"same": True})  # util and util.lua are one cache entry
        self.assertEqual(run.kinds()[0], "started")

    def test_a_require_that_fails_outside_pcall_is_an_error_with_a_position(self):
        run = self.host(FIXTURES / "require_util", script=['message direct {"name": "nope"}', "state"])
        self.assertRegex(run.messages("error")[0],
                         r"^main\.lua:\d+: " + re.escape(not_a_file("nope", ["util.lua", "lib/helper.lua"])) + "$")
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})

    def test_require_names_are_echoed_safely_and_the_list_is_bounded(self):
        run = self.run_app("return {message = function(e, d) return {pcall(require, d.name)} end}",
                           script=['message x {"name": "a\\nb\\u001b\\u00e9"}', 'message x {"name": "' + "q" * 300 + '"}'])
        first, second = [e["data"][1] for e in run.of("reply")]
        self.assertEqual(first, not_a_file("a?b?é", []))
        self.assertTrue(second.startswith("require: '" + "q" * 64 + "...' is not one of"), second)
        # 32 files of 64 characters: the message lists what fits and says so.
        names = {f"{'d' * 55}{i:02d}.lua": "return 1" for i in range(31)}
        app = self.make_app("scratch", "return {message = function(e, d) return {pcall(require, 'zz')} end}", files=names)
        run = self.host(app, script=["message x"])
        text = run.of("reply")[0]["data"][1]
        self.assertTrue(text.startswith("require: 'zz' is not one of this app's files (manifest.json \"files\": [\"ddd"))
        self.assertTrue(text.endswith(",...])"), text)
        self.assertLess(len(text), 700)

    def test_require_loads_each_file_once_and_names_its_errors_by_path(self):
        files = {
            "once.lua": "print('loading once')\nreturn {n = 1}",
            "noret.lua": "print('loading noret')",
            "lib/bad.lua": "print('loading bad')\nlocal t\nt.x = 1",
            "lib/syntax.lua": "x = = 1",
            "lib/a.lua": "return require('lib/b')",
            "lib/b.lua": "return require('lib/a')",
        }
        main = ("return {message = function(e, d)\n"
                "    local ok, v = pcall(require, d.name)\n"
                "    return {ok = ok, v = (type(v) == 'table') and 'table' or v}\n"
                "end}")
        app = self.make_app("scratch", main, files=files)
        run = self.host(app, script=[
            'message x {"name": "once"}', 'message x {"name": "once"}', 'message x {"name": "once.lua"}',
            'message x {"name": "noret"}', 'message x {"name": "noret"}',
            'message x {"name": "lib/bad"}', 'message x {"name": "lib/bad"}',
            'message x {"name": "lib/syntax"}', 'message x {"name": "lib/a"}', "state"], compile=False)
        self.assertEqual(run.logs(), ["loading once", "loading noret", "loading bad", "loading bad"])
        replies = [e["data"] for e in run.of("reply")]
        self.assertEqual(replies[:3], [{"ok": True, "v": "table"}] * 3)
        self.assertEqual(replies[3:5], [{"ok": True, "v": True}] * 2)  # a chunk that returns nothing gives true
        for reply in replies[5:7]:  # a failure isn't cached: it ran twice, and the chunk is named by its bare path
            self.assertEqual(reply["ok"], False)
            self.assertRegex(reply["v"], r"^lib/bad\.lua:3: attempt to index a nil value \(local 't'\)$")
        self.assertEqual(replies[7], {"ok": False, "v": "require: lib/syntax.lua:1: unexpected symbol near '='"})
        self.assertEqual(replies[8]["ok"], False)  # a cycle is an ordinary error
        self.assertIn("stack overflow", replies[8]["v"])
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})

    def test_a_required_files_functions_see_the_sandbox(self):
        app = self.make_app("scratch", "local m = require('m') return {message = function() return m.go() end}",
                            files={"m.lua": "local calls = 0\n"
                                            "return {go = function() calls = calls + 1 print('m', calls) "
                                            "return {n = calls, j = json.encode({1})} end}"})
        run = self.host(app, script=["message a", "message b"])
        self.assertEqual([e["data"] for e in run.of("reply")], [{"n": 1, "j": "[1]"}, {"n": 2, "j": "[1]"}])
        self.assertEqual(run.logs(), ["m\t1", "m\t2"])

    def test_require_of_a_file_with_a_close_variable_is_refused(self):
        app = self.make_app("scratch", "return {message = function() return require('c') end}",
                            files={"c.lua": "local x <close> = nil"})
        run = self.host(app, script=["message x"], compile=False)
        self.assertRegex(run.messages("error")[0],
                         r"^main\.lua:1: require: c\.lua:1: '<close>' variables are not available in Muse apps$")
        run = self.host(app, script=["message x"], rc=1)  # the install refuses such a package, so check does
        self.assertEqual(run.events, [{"event": "crash", "message": compile_failed(
            "c.lua:1: '<close>' variables are not available in Muse apps", ">1| local x <close> = nil")}])

    def test_require_runs_in_a_coroutine(self):
        app = self.make_app("scratch", "return {message = function()\n"
                            "  return coroutine.wrap(function() return require('m').v end)()\nend}",
                            files={"m.lua": "return {v = 7}"})
        run = self.host(app, script=["message x", "message x"])
        self.assertEqual([e["data"] for e in run.of("reply")], [7, 7])

    def test_a_module_compiles_against_the_c_call_room_of_the_thread_that_requires_it(self):
        # The parser recurses in C, a level of Lua's C-call count for each level of nesting, so the room it has
        # is the requiring thread's: a coroutine already deep in C calls has less than the main thread, and a
        # module that compiles on one doesn't on the other. (Compiled on the main thread whatever called
        # require, the parse added its levels to a count that left the coroutine's out.) One module a thread,
        # as a module that compiled is cached.
        self.assertIn("LUAI_MAXCCALLS=40", apps_host.lua_defines(), "NEST and DEPTH are calibrated to the limit")
        nest = "return " + "(" * NEST + "1" + ")" * NEST
        main = ("local function at_depth(n, f, ...)\n"
                "    if n == 0 then return f(...) end\n"
                "    return select(2, pcall(at_depth, n - 1, f, ...))\n"
                "end\n"
                "local function try(name) local ok, e = pcall(require, name) return ok and 'compiled' or e end\n"
                "return {message = function(where)\n"
                "    if where == 'main' then return try('n1') end\n"
                "    if where == 'coroutine' then return coroutine.wrap(try)('n2') end\n"
                f"    return coroutine.wrap(function() return at_depth({DEPTH}, try, 'n3') end)()\n"
                "end}")
        app = self.make_app("scratch", main, files={"n1.lua": nest, "n2.lua": nest, "n3.lua": nest})
        run = self.host(app, script=["message main", "message coroutine", "message deep", "state"])
        self.assertEqual([e["data"] for e in run.of("reply")], ["compiled", "compiled", "require: C stack overflow"])
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})

    def test_a_parse_that_overflows_is_one_clean_line(self):
        # A runtime error in the parser (C stack overflow, for source nested past the C-call room) comes with
        # the call's traceback and no position, unlike a syntax error: the message is cut before it, as a
        # handler's error is.
        nested = "return " + "(" * 60 + "1" + ")" * 60
        run = self.run_app(nested, rc=1)  # main.lua itself: nothing of it ran
        self.assertEqual(run.events, [{"event": "crash", "message": "compile failed: C stack overflow"}])
        run = self.run_app(nested, rc=1, compile=False)  # the runtime's own, without the install's words
        self.assertEqual(run.events, [{"event": "crash", "message": "C stack overflow"}])
        modules = {"deep.lua": nested}
        # require outside a pcall: an error with the require's position, from a handler ...
        app = self.make_app("scratch", "return {message = function() return require('deep') end}", files=modules)
        run = self.host(app, script=["message x", "message x", "state"], compile=False)
        self.assertEqual(run.messages("error"), ["main.lua:1: require: C stack overflow"] * 2)  # not cached
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})
        # ... and from main.lua's top level, a crash with the traceback of the require.
        app = self.make_app("scratch", "require('deep') return {}", files=modules)
        run = self.host(app, rc=1, compile=False)
        record = run.messages("crash")[0]
        self.assertRegex(record, r"^main\.lua:1: require: C stack overflow\nstack traceback:\n\t")
        self.assertNotIn("?stack traceback", record)
        # Under a pcall there is no traceback to cut: the one line.
        app = self.make_app("scratch", "return {message = function() return select(2, pcall(require, 'deep')) end}",
                            files=modules)
        run = self.host(app, script=["message x"], compile=False)
        self.assertEqual(run.of("reply")[0]["data"], "require: C stack overflow")

    # ---- Launch, stop, free ----

    def test_launch_stop_cycles_leak_nothing(self):
        run = self.host(FIXTURES / "cycles", cycles=100)
        self.assertEqual(run.lines, ['{"event":"leak_check","bytes":0}'])

    def test_cycles_of_every_kind_of_app_leak_nothing(self):
        for fixture, rc, first in (("handlers", 0, None), ("messages", 0, None), ("require_util", 0, None),
                                   ("crash_start", 1, "crash"), ("bad_return", 1, "crash"), ("print_ring", 0, None)):
            with self.subTest(fixture):
                run = self.host(FIXTURES / fixture, cycles=25, rc=rc)
                self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})
                self.assertEqual(len(run.events), 1 if first is None else 2)  # a failing launch is reported once
                if first:
                    self.assertEqual(run.events[0]["event"], first)

    def test_cycles_that_run_out_of_memory_leak_nothing(self):
        run = self.run_app("return {start = function() local t = {} for i = 1, 1e6 do t[i] = i end end}",
                           mem_kb=64, cycles=10, rc=1)
        self.assertEqual(run.events[0], {"event": "crash", "message": nomem(64)})
        self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})

    def test_the_leak_check_can_fail(self):
        # A leak check that always said 0 would pass every test above. Build apphost with a runtime that
        # forgets to free its VM: the check has to report what the VM held.
        source = (APPS / "apps_rt.c").read_text()
        leaky = source.replace("        apps_vm_free(rt->vm);\n", "", 1)
        self.assertNotEqual(leaky, source)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        copy = Path(tmp.name) / "apps_rt.c"
        copy.write_text(leaky)
        sources = [copy if path.name == "apps_rt.c" else path for path in apps_host.apphost_sources()]
        exe = apps_host.build("apphost_leaky", [*sources, *apps_host.lua_sources()], tmp.name,
                              include_dirs=[apps_host.LUA_SRC, apps_host.APPS, apps_host.APPHOST],
                              sanitize=False)  # a leak sanitizer would abort at exit, and the check is the point
        done = apps_host.run(exe, "--cycles", 3, FIXTURES / "handlers", text=False)
        self.assertEqual(done.returncode, 0)
        last = Run(done).events[-1]
        self.assertEqual(last["event"], "leak_check")
        self.assertGreater(last["bytes"], 0)

    def test_stop_then_state_then_start_again(self):
        run = self.host(FIXTURES / "cycles", script=["state", "stop", "state", "message x", "start", "state", "stop"])
        self.assertEqual(run.logs(), ["top level\t42\thelper", '{"n":50}\t50', "stopping", '{"n":50}\t50', "stopping"])
        self.assertEqual([e["state"] for e in run.of("state")], ["running", "stopped", "running"])
        self.assertEqual(run.messages("error"), [NOT_RUNNING])
        self.assertEqual(run.kinds().count("started"), 2)

    def test_commands_after_a_load_that_failed(self):
        # main.lua's top level ran and failed, so the app is a crashed one and its runtime kept the log: the
        # commands answer with the crash record, and `log` shows what main.lua printed before it failed.
        run = self.run_app("print('one')\nprint('two')\nreturn 1",
                           script=["state", "message x", "start", "show", "hide", "stop", "log", "log 1"], rc=1)
        one, two = ({"event": "log", "app": "scratch", "line": line} for line in ("one", "two"))
        self.assertEqual(run.events, [
            one, two,  # as they were printed
            {"event": "crash", "message": NOT_A_TABLE},
            {"event": "state", "state": "crashed"},
            {"event": "error", "message": NOT_A_TABLE},
            {"event": "error", "message": NOT_A_TABLE},
            one, two,  # `log`
            two,       # `log 1`
        ])

    def test_a_main_that_does_not_compile_is_refused_and_the_script_does_not_run(self):
        # The install's compile comes first: its refusal is the one event, with no `state` or error after it.
        run = self.run_app("print('never')\nx = = 1", script=["state", "message x", "start", "show", "stop", "log"], rc=1)
        self.assertEqual(run.events, [{"event": "crash", "message": compile_failed(
            "main.lua:2: unexpected symbol near '='", " 1| print('never')", ">2| x = = 1")}])

    def test_commands_after_a_main_that_does_not_compile_without_the_compile_step(self):
        # --no-compile: nothing of main ran, so there is no runtime and nothing to log: the commands answer with the
        # reason.
        syntax = "main.lua:2: unexpected symbol near '='"
        run = self.run_app("print('never')\nx = = 1", script=["state", "message x", "start", "show", "stop", "log"], rc=1,
                           compile=False)
        self.assertEqual(run.events, [
            {"event": "crash", "message": syntax},
            {"event": "state", "state": "crashed"},
            {"event": "error", "message": syntax},
            {"event": "error", "message": syntax},
        ])

    def test_every_failure_of_main_lua_s_top_level_keeps_its_log(self):
        # A runtime error, error(), a kill, running out of memory, a main that returns no table, a handler that
        # isn't a function: the app ran, so the crash event carries the record and `log` the lines.
        for name, source, mem_kb, record in (
                ("runtime error", "print('before')\nlocal t\nt.x = 1\nreturn {}", None,
                 r"^main\.lua:3: attempt to index a nil value \(local 't'\)\nstack traceback:\n\t"),
                ("error()", "print('before') error('boom', 0)", None, r"^boom\nstack traceback:\n\t"),
                ("kill", "print('before') while true do end", None, "^" + re.escape(BUDGET_START) + "$"),
                ("out of memory", "print('before') local t = {} for i = 1, 1e6 do t[i] = i end", 64,
                 "^" + re.escape(nomem(64)) + "$"),
                ("no table", "print('before') return 1", None, "^" + re.escape(NOT_A_TABLE) + "$"),
                ("not a function", "print('before') return {start = 5}", None,
                 r"^main\.lua returned 'start' as a number; handlers must be functions$")):
            with self.subTest(name):
                run = self.run_app(source, script=["state", "log"], mem_kb=mem_kb, rc=1)
                self.assertEqual(run.kinds(), ["log", "crash", "state", "log"])
                before = {"event": "log", "app": "scratch", "line": "before"}
                self.assertEqual([run.events[0], run.events[3]], [before, before])  # printed; and kept in the ring
                self.assertRegex(run.messages("crash")[0], record)
                self.assertEqual(run.events[2], {"event": "state", "state": "crashed"})

    def test_a_crash_in_main_has_no_warnings(self):
        # The table was read up to the key that isn't a function; the warnings so far would depend on the order
        # the keys came in, so there are none (the runtime never got as far as a table it could report on).
        app = self.make_app("scratch", "return {1, 2, start = 5}")
        run = self.host(app, rc=1)
        self.assertEqual(run.events, [{"event": "crash", "message":
                                       "main.lua returned 'start' as a number; handlers must be functions"}])

    # ---- apphost ----

    def test_mem_kb_caps_the_heap(self):
        main = "return {start = function() local t = {} for i = 1, 1e6 do t[i] = i end end}"
        run = self.run_app(main, mem_kb=64, rc=1)
        self.assertEqual(run.events, [{"event": "crash", "message": nomem(64)}])
        run = self.run_app("return {start = function() local t = {} for i = 1, 100000 do t[i] = i end end}", mem_kb=4096)
        self.assertEqual(run.kinds(), ["started"])

    def test_a_heap_too_small_to_start_is_a_crash(self):
        run = self.run_app("return {}", mem_kb=2, rc=1, compile=False)
        self.assertEqual(run.kinds(), ["crash"])
        self.assertIn("out of memory", run.events[0]["message"])
        run = self.run_app("return {}", mem_kb=2, rc=1)  # the install's compile has no room for it either, and says so
        self.assertEqual(run.events, [{"event": "crash", "message":
                                       "compile failed: out of memory: can't make the app's heap of 2 KB"}])

    def test_manifest_warnings_are_events(self):
        app = self.make_app("scratch", "return {}", manifest={"permission": ["sound"], "colour": 1})
        run = self.host(app)
        self.assertEqual(sorted(run.messages("warning")), [
            "unknown key 'colour' ignored", "unknown key 'permission' ignored (did you mean 'permissions'?)"])
        self.assertEqual(run.kinds()[-1], "started")

    def test_script_lines(self):
        run = self.run_app("return {message = function(e) return e end}", script=[
            "", "   ", "# a comment", "  state  ", "message   ping   ", "message", "frobnicate now", "log x", "log -1",
            "state extra", "message ping\r"])
        self.assertEqual(run.kinds(), ["started", "state", "reply", "error", "error", "error", "error", "error",
                                       "reply"])
        self.assertEqual(run.events[2]["data"], "ping")
        self.assertEqual(run.messages("error"), [
            "message needs an event name: message <event> [json]",
            "unknown command 'frobnicate' " + COMMANDS,
            "log needs a number of lines: log [n]",
            "log needs a number of lines: log [n]",
            "state takes no arguments"])
        self.assertEqual(run.events[8]["data"], "ping")

    def test_script_from_a_file(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        script = Path(tmp.name) / "script.txt"
        script.write_text("message status\nstate\n")
        done = apps_host.run(self.apphost, "--script", script, FIXTURES / "messages", text=False)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(Run(done).kinds(), ["started", "reply", "state"])
        done = apps_host.run(self.apphost, "--script", Path(tmp.name) / "missing.txt", FIXTURES / "messages", text=False)
        self.assertEqual(done.returncode, 2)
        self.assertIn(b"missing.txt", done.stderr)

    def test_usage(self):
        app = str(FIXTURES / "handlers")
        for args in ([], ["--mem-kb"], ["--mem-kb", "x", app], ["--mem-kb", "0", app], ["--cycles", "0", app],
                     ["--cycles", "-1", app], ["--bogus", app], [app, app], ["--cycles", "2", "--script", "-", app],
                     ["--no-compile"], ["--sha256"], ["--sha256", "a", "b"]):
            with self.subTest(args):
                done = apps_host.run(self.apphost, *args, text=False)
                self.assertEqual(done.returncode, 2)
                self.assertEqual(done.stdout, b"")
                self.assertIn(b"usage: apphost", done.stderr)

    def test_a_package_that_cannot_be_loaded_is_one_error(self):
        def run_dir(app):
            return self.host(app, rc=1)

        missing = Path(self.tmp) / "nothing_here"
        run = run_dir(missing)
        self.assertEqual(run.kinds(), ["error"])
        self.assertEqual(run.events[0]["message"], "reading manifest.json: No such file or directory")

        app = self.make_app("scratch", "return {}")
        (app / "manifest.json").unlink()
        (app / "manifest.json").mkdir()  # a directory opens, and fails when it is read
        self.assertEqual(run_dir(app).events, [{"event": "error", "message": "reading manifest.json: Is a directory"}])

        app = self.make_app("scratch", "return {}")
        (app / "manifest.json").write_text("{\"id\": \"scratch\",}")
        self.assertEqual(run_dir(app).events, [{"event": "error", "message": "manifest: unexpected '}' at byte 18"}])

        app = self.make_app("scratch", "return {}", manifest={"id": "other"})
        self.assertRegex(run_dir(app).messages("error")[0], r"^manifest: id 'other' must match .* the folder name 'scratch'$")

        app = self.make_app("scratch", "return {}", manifest={"files": ["util.lua"]})
        self.assertEqual(run_dir(app).events, [{"event": "error", "message": "reading util.lua: No such file or directory"}])

        app = self.make_app("scratch", "return {}", files={"d.json": "{\"a\":}"})
        self.assertEqual(run_dir(app).events, [{"event": "error", "message": "d.json: unexpected '}' at byte 6"}])

        app = self.make_app("scratch", "return {}", files={"i.bin": b"not an image"})
        self.assertEqual(run_dir(app).messages("error"),
                         ["i.bin: image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)"])

        app = self.make_app("scratch", "return {}", files={"big.lua": "x" * (256 * 1024 + 1)})
        self.assertEqual(run_dir(app).messages("error"), ["big.lua is larger than 256 KB"])

        app = self.make_app("scratch", "return {}", files={"big.json": " " * (32 * 1024 + 1)})
        self.assertEqual(run_dir(app).messages("error"), ["big.json is larger than 32 KB"])

    def test_a_compiled_chunk_is_refused_in_the_installs_words(self):
        # Fix round 1, F: the install refuses a .lua that starts with Lua's signature before it compiles anything, with
        # the manifest's own index of the file (main is files[0]); apphost reads the package the same way, through the
        # same check, so check says what the install says (and not "binary chunks are not allowed", the compile's).
        for compiled in (b"\x1bLua", b"\x1bLuaT\x00\x19\x93\r\n\x1a\n", b"\x1bLua" + bytes(100)):
            with self.subTest(compiled=compiled[:12]):
                app = self.make_app("scratch", compiled)
                self.assertEqual(self.host(app, rc=1).events, [{
                    "event": "error", "message": "files[0] main.lua is a compiled chunk: install Lua source text"}])
        app = self.make_app("scratch", "return {}", files={"d.json": "{}", "lib/lib.lua": b"\x1bLua"})
        self.assertEqual(self.host(app, rc=1).events, [{
            "event": "error", "message": "files[2] lib/lib.lua is a compiled chunk: install Lua source text"}])
        # Only a .lua file's start is the signature: a .json file that starts that way is refused as JSON, and three bytes
        # of it are not the signature, so the install's step 3 lets them by and the compile says what it says of any
        # source that starts with ESC (as it does here, and in the install: one text).
        app = self.make_app("scratch", "return {}", files={"d.json": b"\x1bLua"})
        self.assertEqual(self.host(app, rc=1).messages("error"), ["d.json: unexpected byte 0x1B at byte 1"])
        app = self.make_app("scratch", b"\x1bLu")
        self.assertEqual(self.host(app, rc=1).events, [{
            "event": "crash", "message": compile_failed("binary chunks are not allowed: install Lua source text")}])

    def test_the_totals_of_a_package_are_limited(self):
        # 8 files of 250 KB make 2000 KB, a 9th goes past the app's 2 MB (no one file is over its own limit).
        files = {f"m{i}.lua": "x" * (250 * 1024) for i in range(1, 10)}
        run = self.host(self.make_app("scratch", "return {}", files=files), rc=1)
        self.assertEqual(run.events, [{"event": "error", "message": "the app is larger than 2 MB"}])
        # Images: 409 KB each, so three are 1228 KB and the fourth goes past 1.5 MB.
        image = struct.pack("<BBHHHHH", 0x19, 0x12, 0, 466, 450, 932, 0) + bytes(932 * 450)
        files = {f"i{i}.bin": image for i in range(1, 5)}
        run = self.host(self.make_app("scratch", "return {}", files=files), rc=1)
        self.assertEqual(run.events, [{"event": "error", "message": "images add up to more than 1.5 MB"}])
        del files["i4.bin"]
        run = self.host(self.make_app("scratch", "return {}", files=files))
        self.assertEqual(run.kinds(), ["started"])

    def test_a_valid_image_and_json_file_load(self):
        # A 2x1 RGB565 LVGL v9 image: magic 0x19, format 0x12, flags 0, w, h, stride, reserved, 4 pixel bytes.
        header = bytes([0x19, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0])
        app = self.make_app("scratch", "return {}", files={"i.bin": header + b"\x00\xf8\xe0\x07",
                                                            "d.json": '{"a": [1, 2]}'})
        run = self.host(app)
        self.assertEqual(run.kinds(), ["started"])

    def test_the_package_may_be_named_with_a_trailing_slash(self):
        done = apps_host.run(self.apphost, str(FIXTURES / "handlers") + "/", text=False)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(Run(done).kinds(), ["warning", "started"])

    def test_every_line_is_json_whatever_the_app_does(self):
        hostile = (b"return {start = function() print('\\255\\0\\27[31m') end,\n"
                   b"        message = function(e) if e == 'bad' then error('\\255\\n\\0\"\\\\') end return e end}")
        run = self.run_app(hostile, script=["message bad", "message \"quoted\"", "message '\\\\'", "log", "state"])
        self.assertEqual(run.logs()[0], "???[31m")
        self.assertEqual(run.kinds().count("reply"), 2)

    def test_bytes_that_are_not_text_come_out_as_replacement_characters(self):
        # What the runtime hands out is clean already, but a script line is the user's own bytes, and the
        # events must stay JSON: a raw control byte or invalid UTF-8 on stdout would fail Run.
        app = self.make_app("scratch", "return {}")
        done = apps_host.run(self.apphost, "--script", "-", app, text=False,
                             input=b'\xff\xc3(\ncaf\xc3\xa9\x01\nq"uote\\back\n')
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stderr, b"")
        tail = " " + COMMANDS
        self.assertEqual(Run(done).messages("error"), [
            "unknown command '\ufffd\ufffd('" + tail,
            "unknown command 'caf\u00e9\x01'" + tail,
            "unknown command 'q\"uote\\back'" + tail,
        ])

    # ---- sha256 ----

    def sha256(self, data):
        done = apps_host.run(self.apphost, "--sha256", "-", input=data, text=False)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stderr, b"")
        return done.stdout.decode()

    def test_sha256_vector(self):
        done = apps_host.run(self.apphost, "--sha256", "abc")
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stdout, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n")

    def test_sha256_known_answers(self):
        answers = {
            b"": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq":
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            b"a" * 1000000: "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        }
        for data, digest in answers.items():
            with self.subTest(len(data)):
                self.assertEqual(self.sha256(data), digest + "\n")
        done = apps_host.run(self.apphost, "--sha256", "")
        self.assertEqual(done.stdout, answers[b""] + "\n")

    def test_sha256_matches_hashlib_around_every_block_boundary(self):
        rng = random.Random(8)
        lengths = [0, 1, 2, 3, 54, 55, 56, 57, 58, 62, 63, 64, 65, 66, 118, 119, 120, 121, 127, 128, 129, 130, 191,
                   192, 193, 255, 256, 257, 1000, 4096, 65537]
        for n in lengths:
            data = rng.randbytes(n)
            self.assertEqual(self.sha256(data), hashlib.sha256(data).hexdigest() + "\n", n)

    # ---- The build ----

    def test_apphost_asks_for_the_posix_it_uses(self):
        # getline, realpath and PATH_MAX are hidden on glibc under -std=c11 unless the defaults are asked for
        # (_DEFAULT_SOURCE: realpath is not under _POSIX_C_SOURCE 200809L alone), and build() passes
        # -D_GNU_SOURCE to every file, which hides that from this test. A build that doesn't (app.py check
        # builds apphost with its own command) needs the file to ask. The macro comes before any include, or the
        # headers have already decided.
        source = (apps_host.APPHOST / "apphost.c").read_text()
        directives = [line for line in source.splitlines() if line.startswith("#")]
        self.assertRegex(directives[0], r"^#define _DEFAULT_SOURCE( 1)?$")
        # And nothing else it uses is hidden: it compiles with no feature macro from the command line.
        args = [*apps_host.cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                *(f"-I{d}" for d in (apps_host.LUA_SRC, apps_host.APPS, apps_host.APPHOST)),
                str(apps_host.APPHOST / "apphost.c")]
        done = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_sources_txt_lists_only_apps_and_apphost_c_files(self):
        # E14: Lua's files and defines come from components/lua/CMakeLists.txt, not from here.
        listed = [line.strip() for line in (ROOT / "tools/muse/apphost/sources.txt").read_text().splitlines()
                  if line.strip() and not line.startswith("#")]
        self.assertEqual(len(listed), len(set(listed)))
        for path in listed:
            self.assertRegex(path, r"^(components/muse/apps|tools/muse/apphost)/[a-z0-9_]+\.c$")
            self.assertTrue((ROOT / path).is_file(), path)
        self.assertIn("components/muse/apps/apps_rt.c", listed)
        self.assertIn("tools/muse/apphost/apphost.c", listed)
        self.assertEqual([p.relative_to(ROOT).as_posix() for p in apps_host.apphost_sources()], listed)

    def test_apphost_is_built_with_the_lua_the_device_gets(self):
        # E14: LUAI_MAXCCALLS=40 comes from components/lua/CMakeLists.txt. Lua's own default is 200, which
        # would let nested pcalls go five times deeper.
        self.assertIn("LUAI_MAXCCALLS=40", apps_host.lua_defines())
        run = self.run_app("local depth = 0\nlocal function f() depth = depth + 1 pcall(f) end\nf()\n"
                           "return {start = function() print(depth) end}")
        self.assertLess(int(run.logs()[0]), 60)

    def test_apps_rt_is_in_the_device_build(self):
        # X6: a device .c of components/muse/apps is listed in the CONFIG_MUSE_APPS block.
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        block = re.search(r"if\(CONFIG_MUSE_APPS\)(.*?)endif\(\)", cmake, re.S).group(1)
        self.assertIn('"apps/apps_rt.c"', block)

    # ---- install: a new version replaces the app that runs (Task 20) ----

    def test_reinstall_running_app_stops_flushes_and_replaces(self):
        # apps_task_run's replacement, as apphost's `install DIR` emulates it: the app that runs is stopped (its stop() prints), its
        # store is flushed (the value set a moment ago, which the 2 s debounce hadn't saved yet), it is freed, and the new version is
        # made and started, reading back what the old one saved. The screen is the new version's alone.
        v1 = FIXTURES / "reinstall_v1/reinstall"
        v2 = FIXTURES / "reinstall_v2/reinstall"
        run = self.host(v1, script=['message set {"n":7}', f"install {v2}", "dump"])
        self.assertEqual(run.kinds(), ["log", "started", "reply", "log", "store_save", "log", "started", "dump"])
        self.assertEqual(run.logs(), ["v1 start 0", "v1 stop", "v2 start 7"])
        self.assertEqual(run.of("store_save"), [{"event": "store_save", "bytes": len('{"n":7}')}])
        self.assertEqual([e["id"] for e in run.of("started")], ["reinstall", "reinstall"])
        self.assertEqual([w["text"] for w in run.of("dump")[0]["widgets"]], ["v2"])

    def test_install_of_a_package_that_fails_keeps_the_app(self):
        # The device's pipeline reads and compiles the new package before apps_task_run replaces anything: a package that fails says why
        # and leaves the app that runs as it was (no stop, no save, its screen).
        v1 = FIXTURES / "reinstall_v1/reinstall"
        broken = self.make_app("reinstall", "return {start = function( end}")
        missing = Path(self.tmp) / "nowhere" / "reinstall"
        run = self.host(v1, script=[f"install {broken}", f"install {missing}", "install", "dump"])
        self.assertEqual(run.kinds(), ["log", "started", "error", "error", "error", "dump"])
        errors = run.messages("error")
        self.assertTrue(errors[0].startswith("compile failed: main.lua:1: "), errors)
        self.assertTrue(errors[1].startswith("reading manifest.json: "), errors)
        self.assertEqual(errors[2], "install needs an app folder: install <dir>")
        self.assertEqual([w["text"] for w in run.of("dump")[0]["widgets"]], ["v1"])

    def test_install_of_another_app_replaces_the_one_that_runs(self):
        # The id is the folder's: another app replaces the one that runs, as a run of another package does on the gadget, and the store
        # it reads is its own.
        v1 = FIXTURES / "reinstall_v1/reinstall"
        other = self.make_app("other", 'return {start = function() ui.label{text = "other", x = 1, y = 2} print("other", '
                                       'store.get("n", 0)) end}', manifest={"permissions": ["store"]})
        run = self.host(v1, script=['message set {"n":3}', f"install {other}", "dump"])
        self.assertEqual(run.kinds(), ["log", "started", "reply", "log", "store_save", "log", "started", "dump"])
        self.assertEqual(run.logs(), ["v1 start 0", "v1 stop", "other\t0"])
        self.assertEqual([e["id"] for e in run.of("started")], ["reinstall", "other"])
        self.assertEqual([w["text"] for w in run.of("dump")[0]["widgets"]], ["other"])


class CompileStepTest(AppHostTestCase):
    """apphost compiles a package as the install does before it makes the runtime (apps_task.c's compile): every .lua
    file, main first and the others in the package's order, in a scratch VM with the manifest's memory limit. A package
    it refuses is one crash event, in the install's words, and nothing else: that is what `app.py check` is for."""

    BAD = "local a = 1\nlocal b = = 2\nreturn {}\n"

    def refused(self, app, message, **kw):
        """app is refused with this message as the one event, and exit status 1."""
        run = self.host(app, rc=1, **kw)
        self.assertEqual(run.events, [{"event": "crash", "message": message}])

    def test_a_module_nothing_requires_is_compiled_all_the_same(self):
        # main.lua prints when it loads and would print again in start(): neither happens, and no command answers.
        app = self.make_app("scratch", "print('main ran')\nreturn {start = function() print('start ran') end}",
                            files={"util.lua": self.BAD})
        self.refused(app, compile_failed("util.lua:2: unexpected symbol near '='", " 1| local a = 1",
                                         ">2| local b = = 2", " 3| return {}"), script=["state", "message x", "log"])

    def test_a_file_in_a_folder_is_named_by_its_path(self):
        app = self.make_app("scratch", "return {}", files={"lib/util.lua": "return {\n  x = 1,\n  y = ,\n}\n"})
        self.refused(app, compile_failed("lib/util.lua:3: unexpected symbol near ','", " 2|   x = 1,", ">3|   y = ,",
                                         " 4| }"))

    def test_main_is_compiled_first_then_the_files_in_the_packages_order(self):
        def broken(name):
            return f"-- {name}\nx = = 1\n"

        files = {"b.lua": broken("b"), "a.lua": broken("a"), "c.lua": "return 1"}
        for main, listed, first in ((broken("main"), files, "main.lua"), ("return {}", files, "b.lua"),
                                    ("return {}", {"a.lua": files["a.lua"], "b.lua": files["b.lua"]}, "a.lua"),
                                    ("return {}", {"c.lua": files["c.lua"], "a.lua": files["a.lua"]}, "a.lua")):
            with self.subTest(first=first, listed=list(listed)):
                run = self.host(self.make_app("scratch", main, files=listed), rc=1)
                self.assertEqual(run.kinds(), ["crash"])
                self.assertTrue(run.messages("crash")[0].startswith(f"compile failed: {first}:2: unexpected symbol"),
                                run.messages("crash"))

    def test_a_json_file_is_not_compiled(self):
        # Only .lua is Lua: this .json would be a syntax error as Lua, and is data.
        app = self.make_app("scratch", "return {}", files={"data.json": '{"a": [1, 2]}'})
        self.assertEqual(self.host(app).kinds(), ["started"])

    def test_nesting_37_deep_compiles_and_38_is_a_c_stack_overflow(self):
        # The install compiles each file as a top-level chunk, the main thread's whole C stack for it: the parser takes
        # one of LUAI_MAXCCALLS (40) levels for the statement, one for the returned expression and one for each
        # parenthesis, so a file nested 37 deep compiles and one nested 38 is refused.
        for depth, accepted in ((36, True), (37, True), (38, False), (39, False), (60, False)):
            with self.subTest(depth=depth):
                app = self.make_app("scratch", "return {}",
                                    files={"deep.lua": "return " + "(" * depth + "1" + ")" * depth + "\n"})
                if accepted:
                    self.assertEqual(self.host(app).kinds(), ["started"])
                else:
                    self.refused(app, "compile failed: C stack overflow")

    def test_the_scratch_vm_has_the_manifests_memory_limit(self):
        # A table of 10000 numbers needs more than 64 KB to compile and less than 128: the default of 256 KB takes it, a
        # manifest of 64 doesn't, and --mem-kb (which the runtime's heap follows too) says the same.
        big = "return {" + ",".join(["1"] * 10000) + "}\n"
        app = self.make_app("scratch", "return {}", files={"big.lua": big})
        self.assertEqual(self.host(app).kinds(), ["started"])
        self.assertEqual(self.host(app, mem_kb=128).kinds(), ["started"])
        self.refused(app, "compile failed: not enough memory", mem_kb=64)
        small = self.make_app("scratch", "return {}", files={"big.lua": big}, manifest={"memory_kb": 64})
        self.refused(small, "compile failed: not enough memory")
        self.assertEqual(self.host(small, mem_kb=128).kinds(), ["started"])

    def test_lua_s_message_is_cut_where_the_install_cuts_it(self):
        # The app task gives Lua's message 512 bytes in its compile (511 and the NUL), and so does check; the line
        # under it is cut at 80.
        app = self.make_app("scratch", "return {}", files={"long.lua": "x = '" + "a" * 600 + "\n"})
        (message,) = self.host(app, rc=1).messages("crash")
        head, _, rest = message.partition("\n")
        self.assertTrue(head.startswith("compile failed: long.lua:1: unfinished string near ''aaaa"), head)
        self.assertEqual(len(head), len("compile failed: ") + 511)
        self.assertEqual(rest, ">1| x = '" + "a" * 75)

    def test_a_heap_too_small_to_make_is_the_installs_message(self):
        self.refused(self.make_app("scratch", "return {}"), "compile failed: out of memory: can't make the app's heap of 2 KB",
                     mem_kb=2)

    def test_the_compile_leaves_the_runtime_with_a_heap_of_its_own(self):
        # The scratch VM is gone when the app starts: a package that compiles in all of its memory still has all of it.
        big = "return {" + ",".join(["1"] * 5000) + "}\n"
        app = self.make_app("scratch", "return {start = function() local t = {} for i = 1, 1000 do t[i] = i end end}",
                            files={"big.lua": big}, manifest={"memory_kb": 128})
        self.assertEqual(self.host(app).kinds(), ["started"])

    def test_no_compile_leaves_the_compile_out(self):
        # What check never says: with the flag, a module that doesn't compile is the app's own business when it is
        # required, and the package starts.
        app = self.make_app("scratch", "return {message = function(e) return require('util') end}",
                            files={"util.lua": self.BAD})
        run = self.host(app, script=["message x"], compile=False)
        self.assertEqual(run.kinds(), ["started", "error"])
        self.assertEqual(run.messages("error"), ["main.lua:1: require: util.lua:2: unexpected symbol near '='"])
        self.assertEqual(self.host(app, rc=1).kinds(), ["crash"])

    def test_warnings_wait_for_a_package_that_compiles(self):
        # A refused package is one event: the manifest's warnings are part of launching one that isn't.
        app = self.make_app("scratch", self.BAD, manifest={"colour": 1})
        self.refused(app, compile_failed("main.lua:2: unexpected symbol near '='", " 1| local a = 1",
                                         ">2| local b = = 2", " 3| return {}"))
        app = self.make_app("scratch", "return {}", manifest={"colour": 1})
        self.assertEqual(self.host(app).kinds(), ["warning", "started"])

    def test_cycles_make_the_app_without_compiling_it_first(self):
        # --cycles measures make, start, stop and free: it has no compile step of the install's to repeat.
        app = self.make_app("scratch", "return {}", files={"util.lua": self.BAD})
        run = self.host(app, cycles=3)
        self.assertEqual(run.events, [{"event": "leak_check", "bytes": 0}])


class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = rt_harness()

    def run_case(self, case, timeout=120, harness=None):
        try:
            done = apps_host.run(harness or self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    def test_the_compile_step_loads_in_order_and_leaves_nothing(self):
        # Fix round 1, E: apps_rt_compile, which the app task's compile and apphost's check share.
        self.run_case("compile_all")

    def test_the_compile_step_survives_a_heap_that_refuses_at_every_step(self):
        self.run_case("compile_refused")

    def test_new_survives_a_heap_that_refuses_at_every_step(self):
        self.run_case("new_refused")

    def test_handlers_survive_a_heap_that_refuses_at_every_step(self):
        self.run_case("handlers_refused")

    def test_messages_survive_a_heap_that_refuses_at_every_step(self):
        self.run_case("message_refused")

    def test_requires_survive_a_heap_that_refuses_at_every_step(self):
        self.run_case("require_refused")

    def test_err_is_cut_on_a_character_boundary(self):
        self.run_case("err_cut")

    def test_log_ring_and_tail(self):
        self.run_case("ring")

    def test_log_tail_cuts_and_edges(self):
        self.run_case("tail_cuts")

    def test_every_line_goes_through_the_port(self):
        self.run_case("port_log")

    def test_states_and_return_values(self):
        self.run_case("lifecycle")

    def test_a_crashed_runtime_runs_nothing(self):
        self.run_case("crashed")

    def test_a_kill_from_outside_crashes_the_app_and_print_and_require_check_for_it(self):
        self.run_case("external_kill")

    def test_null_and_missing_arguments(self):
        self.run_case("null_arguments")

    def test_c_stack_chains_through_print_and_require_fit_the_task_stack(self):
        # The Task 5 rule for what this module adds: the recursions that go deepest in C through print and
        # require (and, from Task 16, through os.time's reads of a table's fields, the one binding of the time
        # services that runs app code from C), each to Lua's C-call limit, on an exact 96 KB region (the host's
        # stand-in for the device's 56 KB task stack), in a build without the sanitizers, which fatten frames. The
        # high-water marks go to the log.
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        plain = apps_host.build("muse_apps_rt_stack", [*HARNESS_SOURCES, *apps_host.lua_sources()], tmp.name,
                                include_dirs=INCLUDES, sanitize=False)
        done = self.run_case("c_stack", harness=plain)
        print(done.stdout, end="", file=sys.stderr, flush=True)

    def test_the_stack_is_balanced_after_every_call(self):
        self.run_case("stack_balanced")

    def test_a_package_without_a_main_file(self):
        self.run_case("no_main")

    def test_a_failure_in_main_luas_top_level_is_a_crashed_runtime_with_its_log(self):
        self.run_case("boot_crash")

    def test_an_app_without_handlers_answers_every_call(self):
        self.run_case("optional_handlers")

    def test_state_names(self):
        self.run_case("state_names")

    # ---- Connected apps Task 2: runtime images ----

    def test_a_runtime_images_handle(self):  # size, free, tostring, protected; ui.image takes it; free while shown is an error
        self.run_case("images_api")

    def test_the_fifth_image_is_refused(self):
        self.run_case("the_fifth_image_is_refused")

    def test_a_freed_image_waits_for_a_pass_that_doesnt_draw_it(self):  # Review Focus 3, with the pass's report given by hand
        self.run_case("images_wait_for_the_screen")

    def test_an_apps_end_hands_its_images_over(self):  # apps_rt_images_take: the app task frees them after the layer's reset
        self.run_case("images_at_the_end")

    def test_a_runtime_image_spins_under_both_caps(self):  # P9: sized from its header, 200 px spins, 201 is refused, 40,000 px
        self.run_case("images_spin_caps")

    def test_a_push_survives_a_heap_that_refuses_at_every_step(self):
        self.run_case("images_refused")

    def test_the_image_constants(self):
        # The brief's numbers; the side is apps_image_check's screen, which the add relies on.
        self.assertEqual(header_number("apps_rt.h", "APPS_RT_IMAGES_MAX"), 4)
        self.assertEqual(header_number("apps_rt.h", "APPS_RT_IMAGE_SIDE_MAX"), 466)
        pkg = (APPS / "apps_pkg.c").read_text()
        self.assertRegex(pkg, r"#define IMAGE_SCREEN 466\b")
        # P2: the add and the push are the app task's, and the header says so.
        header = (APPS / "apps_rt.h").read_text()
        self.assertRegex(header, r"(?s)apps_rt_image_add.{0,600}app task|app task.{0,600}apps_rt_image_add")


class AppHostSession:
    """apphost talked to a line at a time, for a test that changes the fake VM's files between commands, as a hook does on the VM:
    `do` sends script lines, then `state` as a mark, and returns the events the lines printed, the mark's own excepted."""

    def __init__(self, test, binary, *args):
        self.test = test
        self.proc = apps_host.popen(binary, *args)
        self.buf = b""
        self.events = []
        test.addCleanup(self.kill)

    def do(self, *lines, timeout=60):
        self.proc.stdin.write("".join(f"{line}\n" for line in [*lines, "state"]).encode())
        self.proc.stdin.flush()
        events = []
        while True:
            event = json.loads(self.line(timeout))
            self.events.append(event)
            if event["event"] == "state":
                return events
            events.append(event)

    def line(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            self.test.assertGreater(left, 0, "apphost said nothing in time")
            if select.select([self.proc.stdout], [], [], left)[0]:
                chunk = os.read(self.proc.stdout.fileno(), 65536)
                if not chunk:
                    self.test.fail(f"apphost ended: {self.proc.stderr.read().decode(errors='replace')}")
                self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8")  # strictly

    def close(self):
        """Ends the script; apphost exits 0 with nothing on stderr."""
        out, err = self.proc.communicate(timeout=60)
        self.test.assertEqual((self.proc.returncode, err), (0, b""), err.decode(errors="replace"))
        return [json.loads(line) for line in out.decode("utf-8").splitlines()]

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.communicate()


def tally_screen(events):
    """The tally's screen in a dump: the count, the status line, whether the spinner is hidden."""
    (dump,) = [e for e in events if e["event"] == "dump"]
    widgets = dump["widgets"]
    labels = [w["text"] or "" for w in widgets if w["kind"] == "label"]
    (spinner,) = [w for w in widgets if w["kind"] == "spinner"]
    return {"count": labels[0], "status": labels[1], "waiting": not spinner["hidden"]}


class TallyOnAppHost(AppHostTestCase):
    """The tally example (components/muse/apps/examples/tally), the guide's connected app, on apphost with --vm: a folder that
    stands in for the app's folder on the VM (workspace/apps/tally/). The app's reads, watches, writes and commands go to it
    as they go to the VM from the gadget; the test plays the hook's part between the app's steps (test_muse_apps_hook.py runs
    the hook itself)."""

    def vm_folder(self, files=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        folder = Path(tmp.name)
        for rel, text in (files or {}).items():
            (folder / rel).parent.mkdir(parents=True, exist_ok=True)
            (folder / rel).write_text(text)
        return folder

    def test_tally_runs_against_a_fake_vm(self):
        vm = self.vm_folder({"state/count.json": '{"count":41}', "state/rev": "7\n"})
        host = AppHostSession(self, self.apphost, "--vm", vm, "--script", "-", TALLY)
        # It watches state/rev and reads the count when the rev comes; the heartbeat says it's in front.
        self.assertEqual(tally_screen(host.do("sync 1767225600000", "advance 1500", "dump")),
                         {"count": "41", "status": "", "waiting": False})
        self.assertEqual(json.loads((vm / "outbox/active.json").read_text()), {"app": "tally", "n": 1})

        def sent(name):
            (path,) = sorted((vm / "outbox").glob("cmd-*.json"))
            doc = json.loads(path.read_text())
            self.assertEqual((doc["name"], doc["seq"]), (name, int(path.stem[4:])))
            path.unlink()  # the hook takes it
            return doc

        def ack(seq, result, count=None, rev=None):
            if count is not None:
                (vm / "state/count.json").write_text(json.dumps({"count": count}))
                (vm / "state/rev").write_text(f"{rev}\n")
            (vm / "state/acks.json").write_text(json.dumps({"acks": [{"seq": seq, "result": result}]}))

        # "+1" is a command: the spinner shows until the hook's ack, whose result shows; the rev's change reads the same.
        self.assertTrue(tally_screen(host.do('tap "+1"', "advance 50", "dump"))["waiting"])
        doc = sent("add")
        self.assertEqual(doc["args"], {"n": 1})
        ack(doc["seq"], {"ok": True, "count": 42}, count=42, rev=8)
        self.assertEqual(tally_screen(host.do("advance 2500", "dump")), {"count": "42", "status": "", "waiting": False})

        # "Reset" (no args) refused by the hook: its error shows, cut to fit a label at a character's start.
        host.do('tap "Reset"', "advance 50")
        doc = sent("reset")
        self.assertIsNone(doc["args"])
        ack(doc["seq"], {"ok": False, "error": "é" * 200})
        screen = tally_screen(host.do("advance 1500", "dump"))
        self.assertEqual((screen["count"], screen["waiting"]), ("42", False))
        self.assertTrue(screen["status"].startswith("éé") and screen["status"].endswith("..."), screen["status"])
        self.assertLessEqual(len(screen["status"].encode()), 128)

        # No ack: "timed out" after 15 s.
        host.do('tap "+1"', "advance 50")
        sent("add")
        self.assertEqual(tally_screen(host.do("advance 15500", "dump")), {"count": "42", "status": "timed out", "waiting": False})

        # "Ask": apphost has no Muse, so the callback says "not set up"; asked again within 30 s, the call itself refuses, and
        # the app shows that rather than crashing (the guide's pcall).
        self.assertEqual(tally_screen(host.do('tap "Ask"', "advance 100", "dump"))["status"], "not set up")
        screen = tally_screen(host.do('tap "Ask"', "advance 100", "dump"))
        self.assertRegex(screen["status"], r"^muse\.ask: wait \d+ s between asks$")
        self.assertFalse(screen["waiting"])
        self.assertEqual([e for e in host.events if e["event"] in ("crash", "error", "warning")], [])
        self.assertEqual(host.events[-1], {"event": "state", "state": "running"})
        host.close()

    def test_the_fake_vm_is_a_folder(self):
        # --vm DIR: reads come from DIR (a missing file in the gadget's words, a .txt as text), writes land in it (outbox/ made
        # as needed), and a JPEG, which only the gadget decodes, is refused through the callback.
        vm = self.vm_folder({"note.txt": "hi"})
        main = """
            local app = {}
            function app.message(event)
                vm.read("note.txt", function(v, err) print("txt", v, err) end)
                vm.read("state/none.json", function(v, err) print("json", v, err) end)
                vm.read("cover.jpg", function(v, err) print("jpg", v, err ~= nil) end)
                vm.write("outbox/x.json", {n = 1}, function(ok, err) print("write", ok, err) end)
            end
            return app
        """
        app = self.make_app("scratch", main, manifest={"permissions": ["vm"]})
        done = apps_host.run(self.apphost, "--vm", vm, "--script", "-", app, input=b"message go\nadvance 100\n", text=False)
        self.assertEqual((done.returncode, done.stderr), (0, b""), done.stderr)
        run = Run(done)
        self.assertEqual(run.logs(), ["txt\thi\tnil", "json\tnil\tno such file on the VM: state/none.json", "jpg\tnil\ttrue",
                                      "write\ttrue\tnil"])
        self.assertEqual(json.loads((vm / "outbox/x.json").read_text()), {"n": 1})


if __name__ == "__main__":
    unittest.main()
