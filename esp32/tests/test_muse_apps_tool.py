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

"""tools/muse/app.py, the laptop side of the Muse apps dev loop (check, push, watch, logs), the host build of
apphost it checks with (tools/muse/apphost_build.py), and the two apps that ship with it: the hello example and the
torture fixture.

The console half runs against a fake board: a gadget that answers app.put and app.commit as apps_console.c does, and a
clock that only moves when something waits, so a test sees exactly what the tool sent, in what order, and how long it
waited. One class (WireTest) runs the real chat.Board over a fake serial port of bytes. The apps themselves run under
apphost, with the sanitizers where the compiler has them.
"""

import ast
import base64
import contextlib
import importlib
import io
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import apps_host

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools" / "muse"
APPS = ROOT / "components" / "muse" / "apps"
HELLO = APPS / "examples" / "hello"
TORTURE = ROOT / "tests" / "muse_apps_fixtures" / "torture"
APP_PY = TOOLS / "app.py"
COPYRIGHT = (ROOT / "components/muse/muse_clock.h").read_text().splitlines()[1].lstrip("* ").strip()
LUA_HEADER = [f"-- {COPYRIGHT}", "-- SPDX-License-Identifier: Apache-2.0"]   # X7


def c_define(header, name):
    """What `#define NAME ...` is in a C header of components/muse/apps: a number (a product of them, like `(8 * 1024)`, or
    a string with no escapes). The tests read the board's limits and wordings here, so that the tool is held to the C."""
    text = (APPS / header).read_text()
    match = re.search(rf"^#define\s+{name}\s+(.+?)\s*(?:/\*.*\*/)?\s*$", text, re.M)
    assert match, f"no #define {name} in {header}"
    value = match.group(1)
    if value.startswith('"'):
        assert value.endswith('"') and "\\" not in value, value
        return value[1:-1]
    assert re.fullmatch(r"[0-9 *()+]+", value), value
    return eval(value, {"__builtins__": {}})


BAD_ID_MESSAGE = c_define("apps_pkg.h", "APPS_MSG_BAD_ID")   # "app id '%s' must match ^[a-z][a-z0-9_]{1,23}$"
ACK_FORMAT = re.search(r'printf\("(@app\.ack %s %u)\\n", path, seq\)', (APPS / "apps_console.c").read_text()).group(1)


@contextlib.contextmanager
def no_longer_than(seconds):
    """Fails the test, in the thread that waits, when what is inside takes more than `seconds`: a read of a FIFO that
    nobody writes never ends, and nothing else in unittest would say so."""
    def give_up(signum, frame):
        raise AssertionError(f"still waiting after {seconds} s: something blocked, or read what it shouldn't")

    before = signal.signal(signal.SIGALRM, give_up)
    signal.alarm(seconds)
    try:
        yield
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, before)


def load_app():
    """tools/muse/app.py with fresh copies of its siblings and pyserial blocked: everything but opening a port
    (check above all, which needs no gadget) has to work without it."""
    sys.path.insert(0, str(TOOLS))
    try:
        with mock.patch.dict(sys.modules, {"serial": None}):
            for name in ("app", "chat", "ports", "snap", "apphost_build"):
                sys.modules.pop(name, None)
            return importlib.import_module("app")
    finally:
        sys.path.remove(str(TOOLS))


class FakeClock:
    """The clock of the tool (app.time) and of chat (chat.time): it moves when the tool sleeps or waits."""

    START = 1000.0
    HOUR = 3600.0

    def __init__(self):
        self.now = self.START
        self.slept = []

    def monotonic(self):
        return self.now

    time = monotonic

    def advance_to(self, when):
        """Moves the clock on to `when` (never back). An hour past the start is a loop that never ends: the test fails
        there, whoever was waiting (a sleep, a read of the board, a read of the port)."""
        self.now = max(self.now, when)
        if self.now > self.START + self.HOUR:
            raise AssertionError("an hour went by on the test's clock: a loop that never ends")

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.advance_to(self.now + seconds)


class FakeBoard:
    """What app.py asks of chat.Board (write, write_line, read_line, close, port, ser), over a board that answers a
    command with `respond(command)`, a list of (delay in seconds, line). Lines come out of read_line in the order they
    are due, and a wait with nothing due costs the clock all of it. `backlog` is what the port held before anyone wrote,
    `later` lines [(seconds from now, line)] that turn up by themselves. `commands` is every command sent with the
    number of lines still unread when it went: a tool that sends the next piece before it has read the last ack shows
    up there. `events` is everything written, in order: ("raw", bytes) or ("line", command)."""

    def __init__(self, clock, respond=None, backlog=(), later=()):
        self.clock = clock
        self.respond = respond or (lambda command: [])
        self.queue = [(clock.now, line) for line in backlog] + [(clock.now + at, line) for at, line in later]
        self.commands, self.raw_writes, self.events, self.closed = [], [], [], False
        self.port = "/dev/cu.fake"
        self.ser = object()
        self.on_wait = None   # called each time a read gives up at its deadline: a test edits files, or interrupts

    def write(self, data):
        self.raw_writes.append(bytes(data))
        self.events.append(("raw", bytes(data)))

    def write_line(self, command):
        self.events.append(("line", command))
        self.commands.append((command, len(self.queue)))
        for delay, line in self.respond(command):
            self.queue.append((self.clock.now + delay, line))
        self.queue.sort(key=lambda item: item[0])

    def read_line(self, deadline):
        if self.queue and self.queue[0][0] <= deadline:
            ready, line = self.queue.pop(0)
            self.clock.advance_to(ready)
            return line
        self.clock.advance_to(deadline)
        if self.on_wait:
            self.on_wait()
        return None

    def close(self):
        self.closed = True

    def sent(self, word=None):
        """The commands sent (only those that start with `word`, if given), as text."""
        return [c for c, _ in self.commands if word is None or c.split(" ")[0] == word]


class Gadget:
    """The console of a board running Muse apps as far as app.py talks to it (apps_console.c): put with its seq rules
    and refusals, commit with its result, the usage errors. Call it with a command for the lines it prints."""

    ID = re.compile(r"^[a-z][a-z0-9_]{1,23}$")

    def __init__(self, ack_delay=0.01, commit_delay=0.2, result=None, glue="", commit_error=None, echo=()):
        self.ack_delay, self.commit_delay, self.glue, self.echo = ack_delay, commit_delay, glue, list(echo)
        self.result, self.commit_error = result, commit_error
        self.files, self.next, self.stage = {}, {}, None
        self.puts, self.commits = [], []
        self.refuse = {}       # (path, seq) -> message: that put is answered with an @app.err
        self.ack_after = {}    # (path, seq) -> seconds: that put's ack comes late
        self.silent = set()    # (path, seq): that put is never answered
        self.query_delays = []  # seconds the answers to the first sync queries take (the default is 0.01)

    @staticmethod
    def err(op, message, **more):
        # As cJSON prints it: UTF-8 as it is (U+009B too), and only the control characters under U+0020 escaped.
        return "@app.err " + json.dumps({"op": op, **more, "message": message}, separators=(",", ":"), ensure_ascii=False)

    def __call__(self, command):
        words = command.split(" ")
        replies = {"app.put": self.put, "app.commit": self.commit}.get(words[0], lambda w: [])(words)
        if replies and self.glue:
            replies[0] = (replies[0][0], self.glue + replies[0][1])   # a stale fragment in front of the first line
            self.glue = ""
        return replies

    def put(self, words):
        if len(words) < 4:
            return [(0.01, self.err("put", "usage: app.put <id> <path> <seq> <base64>"))]
        app_id, path, seq = words[1], words[2], int(words[3])
        b64 = words[4] if len(words) > 4 else ""
        if not self.ID.match(app_id):
            delay = self.query_delays.pop(0) if self.query_delays else 0.01
            return [(delay, self.err("put", BAD_ID_MESSAGE % app_id, path=path))]   # apps_pkg.h's own words
        if self.stage != app_id:
            self.files, self.next, self.stage = {}, {}, app_id
        if seq == 0:
            self.files[path], self.next[path] = bytearray(), 0
        elif seq != self.next.get(path):
            return [(0.01, self.err("put", f"expected seq {self.next.get(path, 0)}", path=path))]
        if (path, seq) in self.refuse:
            return [(0.01, self.err("put", self.refuse[(path, seq)], path=path))]
        self.files[path] += base64.b64decode(b64)
        self.next[path] = seq + 1
        self.puts.append((path, seq))
        echo = [(0.001, line) for line in self.echo]
        if (path, seq) in self.silent:
            return echo
        return echo + [(self.ack_after.get((path, seq), self.ack_delay), ACK_FORMAT % (path, seq))]   # apps_console.c's

    def commit(self, words):
        app_id, run = words[1], len(words) > 2 and words[2] == "run"
        self.commits.append((app_id, run))
        if self.commit_error:
            return [(0.01, self.err("commit", self.commit_error, id=app_id))]
        result = self.result
        if result is None:
            result = {"ok": True, "payload": {
                "id": app_id, "version": "", "storage": "ram",
                "files": [{"path": p, "bytes": len(d), "sha256": "0" * 64} for p, d in self.files.items()],
                "state": "running" if run else "installed", "warnings": [], "log": [],
                "ui": [{"id": "label#0", "kind": "label", "text": "Hi", "x": 0, "y": 0, "w": 10, "h": 10,
                        "hidden": False, "overflow": 0}] if run else [], "mem_kb": 12, "peak_kb": 14}}
        return [(0.001, line) for line in self.echo] + \
            [(self.commit_delay, "@app " + json.dumps({"op": "commit", **result}, separators=(",", ":"),
                                                     ensure_ascii=False))]


def make_package(root, app_id="demo", main="return {}\n", files=None, manifest=None):
    """<root>/<some folder>/<app_id>, with manifest.json, main.lua and `files` ({path: bytes or text}); the manifest
    lists them in order, then has the keys of `manifest`. Returns the folder."""
    folder = Path(tempfile.mkdtemp(dir=root)) / app_id
    folder.mkdir()
    doc = {"id": app_id, "name": app_id.title(), "api": 1}
    if files:
        doc["files"] = list(files)
    doc.update(manifest or {})
    (folder / "manifest.json").write_text(json.dumps(doc))
    for path, content in {doc.get("main", "main.lua"): main, **(files or {})}.items():
        target = folder / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content if isinstance(content, bytes) else content.encode())
    return folder


class ToolTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = load_app()

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.clock = FakeClock()
        patch = mock.patch.object(self.app, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)

    def board(self, gadget=None, **kw):
        return FakeBoard(self.clock, gadget if gadget is not None else Gadget(), **kw)

    def cli(self, argv, board=None, snap_result=(4, 2, bytes(range(16)))):
        """app.main(argv) with the port replaced by `board`: (exit status, stdout, stderr, screenshot calls)."""
        out, err, captures = io.StringIO(), io.StringIO(), []

        def capture(ser, keys="", settle=None):
            captures.append((ser, keys, list(self.clock.slept)))
            return snap_result

        with mock.patch.object(self.app, "open_board", return_value=board), \
                mock.patch.object(self.app.snap, "capture", capture), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.app.main(argv)
        return code, out.getvalue(), err.getvalue(), captures


# ---- What goes over the console ----

class PackageTest(ToolTestCase):
    def test_lines_fit_the_console(self):
        # The console takes 1,023 bytes after the '>'; the brief says 1,000 with the '>' and the newline. Pieces of at
        # most 768 base64 characters (R13.6) keep every line well under that, whatever the id and the path are.
        data = bytes((i * 7 + 3) % 256 for i in range(300 * 1024))
        app_id, path = "a" * 24, "d" * 60 + ".lua"
        self.assertEqual(len(path), 64)
        commands = list(self.app.put_commands(app_id, path, data))
        lines = [b">" + c.encode() + b"\n" for c in commands]
        self.assertGreater(len(lines), 500)
        self.assertLessEqual(max(len(line) for line in lines), 1000)
        joined, seqs = b"", []
        for command in commands:
            word, got_id, got_path, seq, b64 = command.split(" ")
            self.assertEqual((word, got_id, got_path), ("app.put", app_id, path))
            self.assertLessEqual(len(b64), 768)
            self.assertEqual(len(b64) % 4, 0)
            seqs.append(int(seq))
            joined += base64.b64decode(b64, validate=True)
        self.assertEqual(seqs, list(range(len(commands))))
        self.assertEqual(joined, data)
        self.assertEqual({len(c.split(" ")[4]) for c in commands[:-1]}, {768}, "only the last piece is short")

    def test_an_empty_file_is_one_empty_piece(self):
        self.assertEqual(list(self.app.put_commands("demo", "empty.json", b"")), ["app.put demo empty.json 0"])
        self.assertEqual(list(self.app.put_commands("demo", "x.lua", b"x")), ["app.put demo x.lua 0 eA=="])

    def test_manifest_first_then_files_in_order(self):
        folder = make_package(self.tmp, files={"util.lua": "return 1", "lib/more.lua": "return 2", "data.json": "{}"})
        package = self.app.read_package(folder)
        self.assertEqual(package.id, "demo")
        self.assertEqual([path for path, _ in package.files],
                         ["manifest.json", "main.lua", "util.lua", "lib/more.lua", "data.json"])
        self.assertEqual(package.files[0][1], (folder / "manifest.json").read_bytes())
        self.assertEqual(dict(package.files)["lib/more.lua"], b"return 2")

    def test_main_is_second_whatever_the_manifest_lists(self):
        # C28: manifest.json, then main (main.lua unless the manifest says), then "files" in order, nothing twice.
        folder = make_package(self.tmp, main="return 'ui'",
                              files={"b.lua": "b", "ui/app.lua": "return 'ui'", "a.lua": "a"},
                              manifest={"main": "ui/app.lua", "files": ["b.lua", "ui/app.lua", "a.lua", "b.lua"]})
        paths = [path for path, _ in self.app.read_package(folder).files]
        self.assertEqual(paths, ["manifest.json", "ui/app.lua", "b.lua", "a.lua"])
        folder = make_package(self.tmp, app_id="plain", files={"z.lua": "z", "main.lua": "m"},
                              manifest={"files": ["z.lua", "main.lua"]})
        self.assertEqual([p for p, _ in self.app.read_package(folder).files], ["manifest.json", "main.lua", "z.lua"])

    def test_only_what_the_manifest_lists_is_sent(self):
        folder = make_package(self.tmp, files={"util.lua": "u"})
        (folder / "notes.txt").write_text("not for the gadget")
        (folder / ".hidden.lua").write_text("x")
        self.assertEqual([p for p, _ in self.app.read_package(folder).files], ["manifest.json", "main.lua", "util.lua"])

    def test_a_file_the_manifest_lists_and_the_folder_lacks_is_named_before_anything_is_sent(self):
        folder = make_package(self.tmp, manifest={"files": ["util.lua"]})
        board = self.board()
        code, out, err, _ = self.cli(["push", "/dev/x", str(folder)], board)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("util.lua", err)
        self.assertEqual(board.commands, [], "nothing goes to the board for a package that can't be read")

    def test_a_listed_path_outside_the_folder_is_refused(self):
        (self.tmp / "secret.lua").write_text("return 'secret'")
        folder = make_package(self.tmp, manifest={"files": ["../secret.lua"]})
        (folder.parent / "secret.lua").write_text("return 'beside'")
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertIn("'../secret.lua' is not an allowed path", str(raised.exception), "'..' is the board's no too")
        # A link out of the folder passes the board's rule and is refused for where it leads.
        linked = make_package(self.tmp, app_id="linked", manifest={"files": ["link.lua"]})
        os.symlink(self.tmp / "secret.lua", linked / "link.lua")
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(linked)
        self.assertIn("outside", str(raised.exception))
        # So is a folder that is a link out.
        elsewhere = self.tmp / "elsewhere"
        elsewhere.mkdir()
        (elsewhere / "x.lua").write_text("return 1")
        by_dir = make_package(self.tmp, app_id="bydir", manifest={"files": ["lib/x.lua"]})
        os.symlink(elsewhere, by_dir / "lib")
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(by_dir)
        self.assertIn("outside", str(raised.exception))

    # The corpora are test_muse_apps_pkg's (test_id_rules, test_paths): what apps_id_valid and apps_path_valid say.
    IDS_OK = ["ab", "timer", "a1", "a_", "a_b_9", "z9_", "a" * 24]
    IDS_BAD = ["", "a", "a" * 25, "Timer", "1a", "_a", "a-b", "a.b", "a b", "ab ", " ab", "aB", "a/b", "a\tb",
               "\u00e9a", "ab\n"]
    PATHS_OK = ["assets/x.bin", "a_b-c.d.lua", "main.lua", "lib/util.lua", "a/b/c/d.json", "x.lua", "0.bin", "-x.lua",
                "_x.lua", "a/b.c/d.lua", "a" * 60 + ".lua", "/".join(["abcdefg"] * 8)]
    PATHS_BAD = ["/x.lua", ".x.lua", "a/../b.lua", "A.lua", "x y.lua", "a" * 61 + ".lua", "a//b.lua", "a/", "a/./b.lua",
                 "a/.x.lua", "", "/", ".", "..", "../x.lua", "a/..", "a/b/..", "./x.lua", "a..b.lua", "a./b.lua",
                 "x.lua.", "x.", "x\ty.lua", "x\\y.lua", "x:y.lua", "x*y.lua", "x~1.lua", " x.lua", "x.lua ",
                 "\u00e9.lua", "x#y.lua", "x'y.lua", 'x"y.lua', "x%y.lua", "x.lua\n",
                 "/".join(["abcdefgh"] * 7) + "/x.lua"]

    def test_an_id_the_board_would_refuse_is_said_before_anything_is_sent(self):
        self.assertEqual(len(self.IDS_OK[-1]), 24)
        self.assertEqual(len(self.IDS_BAD[2]), 25)
        for app_id in self.IDS_OK:
            with self.subTest(ok=app_id):
                self.assertEqual(self.app.read_package(make_package(self.tmp, manifest={"id": app_id})).id, app_id)
        for app_id in self.IDS_BAD:
            with self.subTest(bad=app_id), self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(make_package(self.tmp, manifest={"id": app_id}))
            self.assertIn('"id"', str(raised.exception))

    def test_a_path_the_board_would_refuse_is_said_before_anything_is_sent(self):
        self.assertEqual(len(self.PATHS_OK[-2]), 64)
        self.assertEqual(len(self.PATHS_BAD[5]), 65)
        self.assertEqual(len(self.PATHS_OK[-1]), 63)
        for path in self.PATHS_OK:
            with self.subTest(ok=path):
                self.assertTrue(self.app.PATH_RULE.fullmatch(path))
                if path.endswith((".lua", ".json", ".bin")):
                    paths = [p for p, _ in self.app.read_package(make_package(self.tmp, files={path: "x"})).files]
                    self.assertEqual(paths, list(dict.fromkeys(["manifest.json", "main.lua", path])))
                else:   # a path the rule takes and the board has no use for: no kind
                    with self.assertRaises(self.app.AppError) as raised:
                        self.app.read_package(make_package(self.tmp, files={path: "x"}))
                    self.assertIn("unsupported type", str(raised.exception))
        for path in self.PATHS_BAD:
            with self.subTest(bad=path), self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(make_package(self.tmp, manifest={"files": [path]}))
            self.assertIn(f"'{path}' is not an allowed path", str(raised.exception))
        with self.assertRaises(self.app.AppError):   # the main file's is the same
            self.app.read_package(make_package(self.tmp, manifest={"main": "Main.lua"}))

    def test_a_manifest_that_cannot_be_read_is_said_in_words(self):
        folder = self.tmp / "empty"
        folder.mkdir()
        for text, word in ((None, "manifest.json"), ("{not json", "isn't JSON"), ('{"name": "x"}', '"id"'),
                           ('{"id": "xx", "files": "main.lua"}', "files"), ('{"id": "xx", "main": 5}', "main"),
                           ("[1]", "object")):
            with self.subTest(text):
                if text is not None:
                    (folder / "manifest.json").write_text(text)
                with self.assertRaises(self.app.AppError) as raised:
                    self.app.read_package(folder)
                self.assertIn(word, str(raised.exception))

    # ---- What a folder may hold (C and D): looked at before it is read ----

    def make_hazards(self):
        """A package per kind of file that isn't an ordinary one, each listing util.lua or its manifest: {name: folder}."""
        secret = self.tmp / "secret-notes.lua"
        secret.write_text("SECRET_LINE\nlocal b = = 2\n")
        found = {}
        folder = make_package(self.tmp, app_id="fifo", manifest={"files": ["util.lua"]})
        os.mkfifo(folder / "util.lua")
        found["a FIFO in the folder"] = folder
        folder = make_package(self.tmp, app_id="dir", manifest={"files": ["util.lua"]})
        (folder / "util.lua").mkdir()
        found["a folder"] = folder
        folder = make_package(self.tmp, app_id="mfifo")
        (folder / "manifest.json").unlink()
        os.mkfifo(folder / "manifest.json")
        found["a FIFO for a manifest"] = folder
        folder = make_package(self.tmp, app_id="linked", manifest={"files": ["util.lua"]})
        os.symlink(secret, folder / "util.lua")
        found["a link out of the folder"] = folder
        folder = make_package(self.tmp, app_id="fifolink", manifest={"files": ["util.lua"]})
        os.mkfifo(folder / "pipe")
        os.symlink(folder / "pipe", folder / "util.lua")
        found["a link to a FIFO"] = folder
        return found

    def test_a_file_that_is_not_regular_or_not_in_the_folder_is_refused_before_it_is_opened(self):
        words = {"a FIFO in the folder": "regular file", "a folder": "regular file", "a FIFO for a manifest": "regular file",
                 "a link out of the folder": "outside the app's folder", "a link to a FIFO": "regular file"}
        for what, folder in self.make_hazards().items():
            with self.subTest(what), no_longer_than(10), self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(folder)
            message = str(raised.exception)
            self.assertIn(words[what], message)
            self.assertNotIn("SECRET_LINE", message)

    def test_a_file_over_the_boards_limit_is_refused_by_its_size_before_it_is_read(self):
        # D: os.stat first. A sparse file of a gigabyte would take a long time and 1 GB to read, and the board refuses it at
        # 256 KB.
        folder = make_package(self.tmp, manifest={"files": ["big.lua"]})
        with open(folder / "big.lua", "wb") as f:
            f.truncate(1 << 30)
        with no_longer_than(20), self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertEqual(str(raised.exception), "big.lua is larger than 256 KB")
        # Nor is it opened: a file that nobody may read is refused for its size all the same, and not for being unreadable.
        os.chmod(folder / "big.lua", 0)
        self.addCleanup(os.chmod, folder / "big.lua", 0o644)
        if not os.access(folder / "big.lua", os.R_OK):
            with self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(folder)
            self.assertEqual(str(raised.exception), "big.lua is larger than 256 KB")

    def test_a_file_that_grew_since_it_was_looked_at_is_read_one_byte_past_its_limit_and_no_further(self):
        # D: the size was right when it was looked at; what is read is bounded all the same.
        with mock.patch("builtins.open", mock.mock_open(read_data=b"-" * 5000)) as opened:
            with self.assertRaises(self.app.AppError) as raised:
                self.app.read_listed(self.tmp / "util.lua", "util.lua", 2048)
        self.assertEqual(str(raised.exception), "util.lua is larger than 2 KB")
        opened().read.assert_called_once_with(2049)

    def test_the_limits_are_the_boards_and_a_file_exactly_at_one_is_taken(self):
        # The values come from the C headers: apps_pkg.h's, the ones the console's stage and the install check.
        manifest_max, lua_max = c_define("apps_pkg.h", "APPS_MANIFEST_MAX"), c_define("apps_pkg.h", "APPS_LUA_MAX")
        json_max, image_max = c_define("apps_pkg.h", "APPS_JSON_FILE_MAX"), c_define("apps_pkg.h", "APPS_IMAGE_MAX")
        app_max, images_max = c_define("apps_pkg.h", "APPS_APP_MAX"), c_define("apps_pkg.h", "APPS_IMAGES_TOTAL_MAX")
        files_max = c_define("apps_pkg.h", "APPS_MAX_FILES")
        self.assertEqual((self.app.MANIFEST_MAX, self.app.FILE_MAX), (manifest_max, {
            ".lua": lua_max, ".json": json_max, ".bin": image_max}))
        self.assertEqual((self.app.APP_MAX, self.app.IMAGES_MAX, self.app.FILES_MAX), (app_max, images_max, files_max))
        self.assertEqual((manifest_max, lua_max, json_max, image_max, app_max, images_max, files_max),
                         (8 * 1024, 256 * 1024, 32 * 1024, 512 * 1024, 2 * 1024 * 1024, 1536 * 1024, 32), "the numbers")
        for name, limit in (("x.lua", lua_max), ("x.json", json_max), ("x.bin", image_max)):
            folder = make_package(self.tmp, app_id="limits", files={name: b" " * limit})
            self.assertEqual(dict(self.app.read_package(folder).files)[name], b" " * limit, name)
            folder = make_package(self.tmp, app_id="limits", files={name: b" " * (limit + 1)})
            with self.subTest(name), self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(folder)
            self.assertEqual(str(raised.exception), f"{name} is larger than {limit // 1024} KB")
        # The main file too, named as it is.
        folder = make_package(self.tmp, main=" " * (lua_max + 1))
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertEqual(str(raised.exception), "main.lua is larger than 256 KB")

    def test_the_manifest_is_at_most_8_kb_and_that_is_checked_before_it_is_parsed(self):
        folder = make_package(self.tmp)
        base = (folder / "manifest.json").read_text()
        (folder / "manifest.json").write_text(base + " " * (8192 - len(base)))
        self.assertEqual(len(self.app.read_package(folder).files[0][1]), 8192)
        # One byte more is refused whatever it holds, before json.loads sees it: 100,000 nested arrays are 200 KB.
        for text in (base + " " * (8193 - len(base)), "[" * 100_000 + "]" * 100_000, "{not json" + " " * 9000):
            (folder / "manifest.json").write_text(text)
            with self.subTest(len(text)), mock.patch.object(self.app.json, "loads", side_effect=AssertionError("parsed")), \
                    self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(folder)
            self.assertEqual(str(raised.exception), "manifest.json is larger than 8 KB")

    def test_a_manifest_nested_deeper_than_python_parses_is_an_app_error_not_a_traceback(self):
        # 8 KB of arrays is 4,000 levels, which 3.12 parses; an interpreter with less room says RecursionError (100,000
        # levels do, on 3.12), and the watch must not end in it.
        folder = make_package(self.tmp)
        deep = '{"id":"demo","name":"x","api":1,"deep":' + "[" * 200 + "]" * 200 + "}"
        (folder / "manifest.json").write_text(deep)
        self.assertEqual(self.app.read_package(folder).id, "demo")
        with mock.patch.object(self.app.json, "loads", side_effect=RecursionError("maximum recursion depth exceeded")), \
                self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertIn("nested too deeply", str(raised.exception))

    def test_the_totals_and_the_file_count_are_the_boards(self):
        # Each file under its own limit, and together over the app's 2 MB (the manifest counts) or the images' 1.5 MB.
        def lua(n):
            return {f"m{i}.lua": b"-" * (250 * 1024) for i in range(n)}

        folder = make_package(self.tmp, app_id="total", files=lua(8))   # main.lua is small: 2,000 KB and a little
        self.assertEqual(len(self.app.read_package(folder).files), 10)
        folder = make_package(self.tmp, app_id="total", files=lua(9))
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertEqual(str(raised.exception), "the app is larger than 2 MB")
        images = {f"i{i}.bin": b"I" * (500 * 1024) for i in range(3)}   # 1,500 KB
        self.assertEqual(len(self.app.read_package(make_package(self.tmp, app_id="img", files=images)).files), 5)
        images["i3.bin"] = b"I" * (500 * 1024)
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(make_package(self.tmp, app_id="img", files=images))
        self.assertEqual(str(raised.exception), "images add up to more than 1.5 MB")
        # 32 files, main among them; a 33rd is one too many, whatever it holds.
        few = {f"f{i}.lua": b"-" for i in range(31)}
        paths = [p for p, _ in self.app.read_package(make_package(self.tmp, app_id="few", files=few)).files]
        self.assertEqual(len(paths), 1 + 32)
        many = {f"f{i}.lua": b"-" for i in range(32)}
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(make_package(self.tmp, app_id="many", files=many))
        self.assertEqual(str(raised.exception), "more than 32 files besides manifest.json")
        # The same file listed twice is one.
        twice = make_package(self.tmp, app_id="twice", files={"a.lua": b"-"}, manifest={"files": ["a.lua", "a.lua", "main.lua"]})
        self.assertEqual([p for p, _ in self.app.read_package(twice).files], ["manifest.json", "main.lua", "a.lua"])

    def test_a_file_the_board_has_no_kind_for_is_refused_before_it_is_read(self):
        folder = make_package(self.tmp, files={"notes.txt": b"x"})
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertEqual(str(raised.exception), "'notes.txt' has an unsupported type (use .lua, .json or .bin)")
        os.chmod(folder / "notes.txt", 0)   # said for its type, and not opened to find that it can't be read
        self.addCleanup(os.chmod, folder / "notes.txt", 0o644)
        if not os.access(folder / "notes.txt", os.R_OK):
            with self.assertRaises(self.app.AppError) as raised:
                self.app.read_package(folder)
            self.assertEqual(str(raised.exception), "'notes.txt' has an unsupported type (use .lua, .json or .bin)")
        folder = make_package(self.tmp, main="{}", manifest={"main": "main.json"})
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertRegex(str(raised.exception), r'^"main" in .*manifest\.json must be the path of a \.lua file$')

    def test_a_file_that_cannot_be_read_is_an_app_error_whatever_the_reason(self):
        folder = make_package(self.tmp, files={"util.lua": b"-"})
        os.chmod(folder / "util.lua", 0)
        self.addCleanup(os.chmod, folder / "util.lua", 0o644)
        if os.access(folder / "util.lua", os.R_OK):
            self.skipTest("this user reads what mode 0 says no to (root)")
        with self.assertRaises(self.app.AppError) as raised:
            self.app.read_package(folder)
        self.assertIn("util.lua", str(raised.exception))

    def test_the_pieces_are_made_as_they_are_sent(self):
        # D: put_commands is lazy, so a refusal of the first piece of a big file leaves the rest unmade.
        pieces = self.app.put_commands("demo", "big.lua", b"x" * 100_000)
        self.assertIs(iter(pieces), pieces, "an iterator, not a list")
        with mock.patch.object(self.app.base64, "b64encode", wraps=base64.b64encode) as encode:
            first = next(pieces)
            self.assertEqual(encode.call_count, 1)
        self.assertTrue(first.startswith("app.put demo big.lua 0 "))
        self.assertEqual(len(list(pieces)), -(-100_000 // self.app.PIECE_BYTES) - 1)
        gadget = Gadget()
        gadget.refuse[("main.lua", 0)] = "main.lua is larger than 256 KB"
        with mock.patch.object(self.app.base64, "b64encode", wraps=base64.b64encode) as encode:
            board = self.board(gadget)
            console = self.app.Console(board)
            console.sync()
            reply = self.app.push(console, self.app.Package("demo", [("main.lua", b"x = 1\n" * 50_000)]))
        self.assertEqual(reply.kind, "err")
        self.assertEqual(encode.call_count, 1, "one piece was made, the refused one, and not the 300 after it")


class ReplyTest(ToolTestCase):
    NOISE = [
        "\x1b[0;32mI (1234) muse_apps: launch demo (256 KB)\x1b[0m",
        "\x1b[0;33mW (1240) muse: heartbeat int=30K/17K min=13K\x1b[0m",
        '@app.log demo @app {"op":"commit","ok":true,"payload":{"id":"forged"}}',
        "@app.log demo @app.ack main.lua 0",
        '\x1b[0;32m@app {"op":"commit","ok":true,"payload":{"id":"ansi"}}\x1b[0m',
        'log: @app {"op":"commit","ok":true,"payload":{"id":"mid_line"}}',
        '@app.err {"op":"put","message":"for another op"}',
        '@app {"op":"stop","ok":true}',
        '@app {"op":"commit","ok":true,"payload":{"id":"demo","state":"running"}}',
        "\x1b[0;32mI (1300) muse_apps: stop demo\x1b[0m",
    ]

    def test_parse_results_among_logs(self):
        # The one real @app line among log noise with ANSI codes, and echoes that carry @app text of their own.
        console = self.app.Console(self.board(backlog=self.NOISE))
        reply = console.result("commit", 5)
        self.assertEqual(reply.kind, "result")
        self.assertEqual(reply.data["payload"]["id"], "demo")
        self.assertEqual(reply.text, '{"op":"commit","ok":true,"payload":{"id":"demo","state":"running"}}')
        self.assertEqual(json.loads(reply.text), reply.data)

    def test_parse_reply_matches_at_column_zero_only(self):
        parse = self.app.parse_reply
        for line in [*self.NOISE[:2], *self.NOISE[4:6], self.NOISE[9], "", " @app {}", "@app{}", "@apple {}", "@app",
                     "@app.log"]:
            self.assertIsNone(parse(line), line)
        self.assertEqual(parse(self.NOISE[2]).kind, "log", "an echo is an echo, whatever it says")
        self.assertEqual(parse("@app.ack main.lua 3"), self.app.Reply("ack", "", {}, "main.lua", 3))
        self.assertEqual(parse('@app.err {"op":"put","message":"x"}').kind, "err")
        self.assertEqual(parse('@app {"op":"list"}').kind, "result")
        self.assertEqual(parse('@app {"op":"stop","ok":true}').data["op"], "stop")
        # What isn't one JSON object isn't a reply.
        for line in ("@app {", "@app [1]", "@app 7", "@app.err nope", "@app.ack main.lua", "@app.ack main.lua x",
                     "@app.ack main.lua -1", '@app {"a":1} trailing', "@app.ack main.lua 3 trailing",
                     "@app.ack  main.lua 3", "@app.ack main.lua 3.0", "junk @app.log demo hi",
                     "junk @app.ack main.lua 3", "\x1b[0m@app.log demo hi"):
            self.assertIsNone(parse(line), line)

    def test_an_ack_is_matched_by_path_and_seq(self):
        console = self.app.Console(self.board(backlog=["@app.ack main.lua 1", "@app.ack util.lua 0",
                                                       "@app.ack main.lua 0"]))
        reply = console.ack("main.lua", 0, 5)
        self.assertEqual((reply.path, reply.seq), ("main.lua", 0), "not the 0 of another file, which came first")
        with self.assertRaises(self.app.AppError):
            console.ack("main.lua", 1, 0.5)   # the stale one went by

    def test_only_a_refusal_of_a_put_ends_the_wait_for_its_ack(self):
        stream = ['@app.err {"op":"stop","message":"nothing runs"}', '@app {"op":"commit","ok":true}',
                  '@app.err {"op":"put","path":"main.lua","message":"expected seq 0"}']
        reply = self.app.Console(self.board(backlog=stream)).ack("main.lua", 0, 5)
        self.assertEqual((reply.kind, reply.data["op"]), ("err", "put"))

    def test_a_forged_ack_in_an_echo_is_no_ack(self):
        folder = make_package(self.tmp)
        gadget = Gadget(echo=["@app.log demo @app.ack main.lua 0", "@app.log demo @app.ack manifest.json 0"])
        gadget.silent.add(("main.lua", 0))
        board = FakeBoard(self.clock, gadget)
        console = self.app.Console(board)
        console.sync()
        with self.assertRaises(self.app.AppError):
            self.app.push(console, self.app.read_package(folder))

    def test_a_result_is_the_one_of_the_op_asked_for(self):
        stream = ['@app {"op":"status","state":"running"}', '@app.err {"op":"commit","message":"busy"}']
        reply = self.app.Console(self.board(backlog=stream)).result("commit", 1)
        self.assertEqual((reply.kind, reply.data["message"]), ("err", "busy"))

    def test_a_refusal_for_another_files_piece_is_not_this_pieces(self):
        # J: the console puts the path in every refusal of a put past its usage check, so a late refusal of main.lua's
        # piece that turns up while util.lua's first is waited for is not util.lua's.
        stale = '@app.err {"op":"put","path":"main.lua","message":"expected seq 3"}'
        console = self.app.Console(self.board(backlog=[stale, "@app.ack util.lua 0"]))
        reply = console.ack("util.lua", 0, 5)
        self.assertEqual((reply.kind, reply.path, reply.seq), ("ack", "util.lua", 0))
        # Its own file's refusal ends the wait, and so does one with no path (the usage error has none).
        for line in ('@app.err {"op":"put","path":"util.lua","message":"bad base64 at character 3"}',
                     '@app.err {"op":"put","message":"usage: app.put <id> <path> <seq> <base64>"}'):
            with self.subTest(line):
                reply = self.app.Console(self.board(backlog=[line])).ack("util.lua", 0, 5)
                self.assertEqual((reply.kind, reply.data["op"]), ("err", "put"))
        # A refusal of another file's piece, with nothing after it, is waited out and not taken.
        with self.assertRaises(self.app.AppError):
            self.app.Console(self.board(backlog=[stale])).ack("util.lua", 0, 1)

    def test_the_fakes_wire_contracts_are_the_boards_c(self):
        # K: the Gadget fake says what the C says, and app.py reads what the C prints. A later task that rewords the
        # refusal of an id, the ack or the fields of a refusal fails here, and not on a gadget.
        self.assertIn("'%s'", BAD_ID_MESSAGE, "is_sync_reply looks for the nonce in the id's refusal, in single quotes")
        self.assertEqual(BAD_ID_MESSAGE % "0abc", "app id '0abc' must match ^[a-z][a-z0-9_]{1,23}$")
        self.assertEqual(ACK_FORMAT, "@app.ack %s %u")
        reply = self.app.parse_reply(ACK_FORMAT % ("lib/util.lua", 12))
        self.assertEqual((reply.kind, reply.path, reply.seq), ("ack", "lib/util.lua", 12))
        refusal = Gadget()("app.put 0abc sync 0")[0][1]
        self.assertEqual(self.app.parse_reply(refusal).data["message"], BAD_ID_MESSAGE % "0abc")
        self.assertTrue(self.app.is_sync_reply(refusal, "abc", False))
        # The console's refusals of a put carry the path (it is how a refusal is matched to a piece), but for the usage
        # error, which has none; and none carries an id of its own.
        source = (APPS / "apps_console.c").read_text()
        calls = re.findall(r'fail\("put",\s*(\w+),\s*(\w+),', source)
        self.assertGreater(len(calls), 8)
        self.assertEqual([c for c in calls if c[1] == "NULL"], [("NULL", "NULL")], "only the usage error has no path")
        self.assertEqual({c[0] for c in calls}, {"NULL"}, "a put's refusal says no id")
        self.assertIn('take_id("put", id_w, shown, id)', source, "the refusal of the id says the path too")
        self.assertIn('fail(op, strcmp(op, "put") == 0 ? NULL : shown, path, APPS_MSG_BAD_ID, shown)', source)
        self.assertEqual(Gadget.err("put", "m", path="x.lua"), '@app.err {"op":"put","path":"x.lua","message":"m"}')

    def test_there_is_no_str_splitlines_in_the_tool(self):
        # U+2028 and U+2029 break lines for str.splitlines() and for nothing else: an app may print them.
        self.assertNotIn("splitlines", re.sub(r"#[^\n]*", "", APP_PY.read_text()))


class TerminalTest(ToolTestCase):
    """What app.py prints is for a terminal that obeys what it is sent, and the board's and an app's text is not the
    tool's (B): human text shows each control character but newline and tab, DEL and each C1 character (U+009B is CSI) as
    \\xNN, and a JSON line printed whole keeps its meaning, with U+007F to U+009F as \\u00XX."""

    ESC, CSI = "\x1b", "\u009b"
    BAD = "boom \x1b]0;owned\x07 \u009b2J \x7f end"
    SHOWN = "boom \\x1b]0;owned\\x07 \\x9b2J \\x7f end"

    @staticmethod
    def unsafe(text):
        """What a terminal could act on in text: C0 but newline and tab, DEL and C1."""
        return re.findall("[\x00-\x08\x0b-\x1f\x7f-\x9f]", text)

    def test_a_result_prints_as_the_same_json_and_is_described_without_the_bytes(self):
        message = self.BAD + "\nlog:\ntab\there"
        failed = {"ok": False, "error": {"code": "crashed", "message": message}}
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp)), "--run"],
                                     self.board(Gadget(result=failed)))
        self.assertEqual(code, 1)
        self.assertEqual(self.unsafe(out), [], repr(out))
        (line,) = out.splitlines()
        self.assertEqual(json.loads(line)["error"]["message"], message, "the same JSON, whatever the escapes")
        self.assertIn("\\u009b", line)
        self.assertIn("\\u001b", line)
        self.assertEqual(err, f"crashed: {self.SHOWN}\nlog:\ntab\there\n")

    def test_a_refusal_is_shown_the_same_way(self):
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))],
                                     self.board(Gadget(commit_error=self.BAD)))
        self.assertEqual(code, 1)
        self.assertEqual(self.unsafe(out + err), [], repr(out + err))
        self.assertEqual(json.loads(out)["message"], self.BAD)
        self.assertEqual(err, f"app.commit was refused: {self.SHOWN}\n")
        gadget = Gadget()
        gadget.refuse[("main.lua", 0)] = self.BAD
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))], self.board(gadget))
        self.assertEqual((json.loads(out)["message"], err), (self.BAD, f"app.put was refused: {self.SHOWN}\n"))
        self.assertEqual(self.unsafe(out + err), [])

    def test_a_log_line_is_shown_without_the_bytes(self):
        line = f"@app.log demo {self.BAD} \ttab"
        board = self.board(later=[(0.1, line), (0.2, "@app.log demo plain")])
        board.on_wait = mock.Mock(side_effect=[None, KeyboardInterrupt])
        code, out, err, _ = self.cli(["logs", "/dev/x"], board)
        self.assertEqual(code, 0, err)
        self.assertEqual(out, f"@app.log demo {self.SHOWN} \ttab\n@app.log demo plain\n")
        # watch prints the app's lines between pushes the same way.
        board = self.board(later=[(1.2, line)])
        board.on_wait = mock.Mock(side_effect=[None, None, None, KeyboardInterrupt])
        code, out, err, _ = self.cli(["watch", "/dev/x", str(make_package(self.tmp))], board)
        self.assertEqual(self.unsafe(out), [], repr(out))
        self.assertIn(f"@app.log demo {self.SHOWN} \ttab\n", out)

    def test_a_message_of_the_tool_names_the_file_without_the_bytes(self):
        # What the package says about itself is as foreign as what the board says: a path in a manifest.
        folder = make_package(self.tmp, manifest={"files": ["x\x1b]0;owned\x07.lua"]})
        code, out, err, _ = self.cli(["push", "/dev/x", str(folder)], self.board())
        self.assertEqual((code, out), (1, ""))
        self.assertEqual(self.unsafe(err), [], repr(err))
        self.assertIn("'x\\x1b]0;owned\\x07.lua' is not an allowed path", err)

    def test_a_lone_surrogate_in_a_reply_is_text_too(self):
        # json.loads decodes "\ud800" to a character no output encoding takes: strict stderr would end the run in a
        # traceback.
        reply = self.app.parse_reply('@app {"op":"commit","ok":false,"error":{"code":"x","message":"a\\ud800b"}}')
        self.assertEqual(reply.data["error"]["message"], "a\ud800b")
        stdout = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        stderr = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            self.assertEqual(self.app.report(reply), 1)
        stdout.flush()
        stderr.flush()
        self.assertEqual(stderr.buffer.getvalue(), b"x: a\\ud800b\n")
        self.assertEqual(stdout.buffer.getvalue(), reply.text.encode() + b"\n")

    def test_the_two_helpers_leave_ordinary_text_alone(self):
        text = "plain é 世界 \U0001f600\ttab\nnext line   ¡  "
        self.assertEqual(self.app.printable(text), text)
        line = json.dumps({"s": text}, ensure_ascii=False)
        self.assertEqual(self.app.printable_json(line), line)
        self.assertEqual(self.app.printable("\x00\x01\x08\x0b\x0c\r\x1f\x7f\x80\x9f  "),
                         "\\x00\\x01\\x08\\x0b\\x0c\\x0d\\x1f\\x7f\\x80\\x9f  ")
        raw = '{"a":"\x7f\u0080\u009f ","b":["\u009b"]}'
        printed = self.app.printable_json(raw)
        self.assertEqual(printed, '{"a":"\\u007f\\u0080\\u009f ","b":["\\u009b"]}')
        self.assertEqual(json.loads(printed), json.loads(raw))
        # Something that isn't JSON is text.
        self.assertEqual(self.app.printable_json("not json \x1b[2J"), "not json \\x1b[2J")
        self.assertEqual(self.app.printable_json('{"a":1}\r'), '{"a":1} ', "a CR between tokens is a space, in JSON too")

    def test_nothing_is_printed_but_through_the_one_helper(self):
        # Every line of output goes through say() or say_json(), so there is no print in the tool that skips them.
        tree = ast.parse(APP_PY.read_text())
        found = []

        def walk(node, inside):
            for child in ast.iter_child_nodes(node):
                here = child.name if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)) else inside
                if isinstance(child, ast.Call):
                    func = child.func
                    name = func.id if isinstance(func, ast.Name) else ast.unparse(func)
                    if name == "print" or name in ("sys.stdout.write", "sys.stderr.write", "sys.stdout.buffer.write",
                                                   "sys.stderr.buffer.write"):
                        found.append((here, name))
                walk(child, here)

        walk(tree, None)
        self.assertTrue(found, "the helpers print")
        self.assertEqual({where for where, _ in found}, {"say", "say_json"}, found)


class SyncTest(ToolTestCase):
    """app.py synchronizes when it opens the port (H1): it sends a query and takes that reply at column 0, or glued to
    the end of the first line (a stale USB packet may sit in front of it), and never from inside an @app.log line."""

    def sync(self, gadget=None, **kw):
        board = self.board(gadget, **kw)
        console = self.app.Console(board)
        console.sync()
        return console, board

    def test_the_query_is_a_put_the_board_refuses_before_it_touches_anything(self):
        console, board = self.sync()
        self.assertEqual(board.raw_writes, [b"\n"], "a newline first: it ends a half line a cut-off program left")
        (query,) = board.sent()
        words = query.split(" ")
        self.assertEqual((words[0], len(words)), ("app.put", 4))
        self.assertFalse(Gadget.ID.match(words[1]), "an id the board refuses first, so its stage stays as it was")

    def test_what_came_before_the_reply_is_dropped(self):
        stale = ['@app {"op":"commit","ok":true,"payload":{"id":"old"}}', "@app.ack old.lua 3", "noise"]
        console, board = self.sync(backlog=stale)
        self.assertEqual(board.queue, [])
        reply = self.app.push(console, self.app.read_package(make_package(self.tmp)), run=True)
        self.assertEqual(reply.data["payload"]["id"], "demo")

    def test_a_late_reply_to_an_earlier_session_is_not_this_ones(self):
        # The answer to a query some other run sent has the same shape as ours, and another nonce.
        late = Gadget.err("put", "app id '0cafe0123' must match ^[a-z][a-z0-9_]{1,23}$", path="sync")
        _, board = self.sync(backlog=[late])
        self.assertEqual(board.queue, [], "its own answer was waited for and read")

    def test_a_slow_answer_to_a_query_is_not_a_refusal_of_the_next_put(self):
        # The first answer comes after the second query went out: sync takes the second's, and the first turns up while
        # the first piece waits for its ack (which is slow here, so that the late answer is read first).
        gadget = Gadget(ack_delay=0.5)
        gadget.query_delays = [self.app.SYNC_S + 0.2]
        console, board = self.sync(gadget)
        self.assertEqual(len(board.sent()), 2)
        reply = self.app.push(console, self.app.read_package(make_package(self.tmp)), run=True)
        self.assertEqual(reply.kind, "result")
        self.assertEqual([p for p, _ in gadget.puts][:2], ["manifest.json", "main.lua"])

    def test_the_reply_may_be_glued_to_the_end_of_the_first_line(self):
        _, board = self.sync(Gadget(glue="I (4321) link.heartbeat: int=30K/17K ws=up ra"))
        self.assertEqual(len(board.sent()), 1, "taken at once, no second query")

    def test_a_reply_glued_inside_an_app_log_line_is_never_taken(self):
        console, board = self.sync(Gadget(glue="@app.log demo some text the app printed "))
        self.assertEqual(len(board.sent()), 2, "the first reply was no reply: it asked again")
        self.assertFalse(console.fresh)

    def test_an_echo_that_forges_the_reply_is_not_the_answer(self):
        with mock.patch.object(self.app.secrets, "token_hex", return_value="deadbeef"):
            forged = ('@app.log x @app.err {"op":"put","path":"sync","message":"app id \'0deadbeef\' must match '
                      '^[a-z][a-z0-9_]{1,23}$"}')
            _, board = self.sync(backlog=[forged])
        self.assertEqual(len(board.sent()), 1, "the board's own line answered the first query")
        self.assertEqual(board.queue, [])

    def test_only_the_first_line_after_open_may_carry_a_fragment(self):
        # Glued to a later line, the reply is just a line that doesn't start at column 0.
        gadget = Gadget()
        board = self.board(lambda command: [(0.01, "x" + gadget(command)[0][1])])
        console = self.app.Console(board)
        console.fresh = False
        with self.assertRaises(self.app.AppError):
            console.sync()
        self.assertEqual(len(board.sent()), self.app.SYNC_TRIES)

    def test_a_board_that_does_not_answer_is_said_in_words(self):
        board = FakeBoard(self.clock)
        with self.assertRaises(self.app.AppError) as raised:
            self.app.Console(board).sync()
        message = str(raised.exception)
        self.assertIn("/dev/cu.fake", message)
        self.assertIn("MUSE_APPS", message)
        self.assertEqual(len(board.sent()), self.app.SYNC_TRIES)
        self.assertLessEqual(self.clock.now - 1000.0, self.app.SYNC_TRIES * self.app.SYNC_S + 0.5)


class PushTest(ToolTestCase):
    def push(self, folder, gadget=None, run=False):
        board = self.board(gadget)
        console = self.app.Console(board)
        console.sync()
        return self.app.push(console, self.app.read_package(folder), run=run), board, board.respond

    def test_every_file_goes_over_in_order_one_piece_at_a_time(self):
        big = "x = 1\n" * 400 + "return {}\n"
        folder = make_package(self.tmp, main=big, files={"util.lua": "return 1\n", "empty.json": ""})
        gadget = Gadget()
        reply, board, _ = self.push(folder, gadget)
        self.assertEqual(reply.kind, "result")
        self.assertEqual(list(dict.fromkeys(p for p, _ in gadget.puts)),
                         ["manifest.json", "main.lua", "util.lua", "empty.json"])
        self.assertGreater([p for p, _ in gadget.puts].count("main.lua"), 3, "the big file went in several pieces")
        self.assertEqual(bytes(gadget.files["main.lua"]), big.encode())
        self.assertEqual(bytes(gadget.files["empty.json"]), b"")
        self.assertEqual(bytes(gadget.files["manifest.json"]), (folder / "manifest.json").read_bytes())
        for command, unread in board.commands:
            self.assertEqual(unread, 0, "a piece goes out after the last one's ack has been read: " + command[:30])
            self.assertLessEqual(len(command) + 2, 1000)

    def test_a_run_wakes_the_screen_first_and_a_plain_push_leaves_it(self):
        # A sleeping screen has no layout for the run to check ("layout not checked: the screen was asleep") and shows
        # nothing. `w` is the console's wake key: it does nothing to a screen that is on.
        folder = make_package(self.tmp)
        _, board, _ = self.push(folder, run=False)
        self.assertEqual(board.raw_writes, [b"\n"])
        _, board, _ = self.push(folder, run=True)
        self.assertEqual(board.raw_writes, [b"\n", b"w\n"])
        events = board.events
        wake = events.index(("raw", b"w\n"))
        self.assertTrue(events[wake - 1][1].startswith("app.put 0"), "after the sync query")
        self.assertTrue(events[wake + 1][1].startswith("app.put demo manifest.json 0"), "before the first piece")

    def test_commit_follows_the_last_ack_and_run_is_the_flag(self):
        folder = make_package(self.tmp)
        for run in (False, True):
            gadget = Gadget()
            reply, board, _ = self.push(folder, gadget, run=run)
            self.assertEqual(board.sent()[-1], "app.commit demo" + (" run" if run else ""))
            self.assertEqual(gadget.commits, [("demo", run)])
            self.assertEqual(reply.data["payload"]["state"], "running" if run else "installed")

    def test_an_ack_is_waited_for_two_seconds_and_no_longer(self):
        folder = make_package(self.tmp)
        gadget = Gadget()
        gadget.ack_after[("main.lua", 0)] = 1.9
        reply, _, _ = self.push(folder, gadget)
        self.assertEqual(reply.kind, "result", "an ack at 1.9 s is in time")
        gadget = Gadget()
        gadget.ack_after[("main.lua", 0)] = 2.1
        with self.assertRaises(self.app.AppError) as raised:
            self.push(folder, gadget)
        message = str(raised.exception)
        for word in ("main.lua", "2 s", "@app.ack"):
            self.assertIn(word, message)
        gadget = Gadget()
        gadget.silent.add(("manifest.json", 0))
        with self.assertRaises(self.app.AppError) as raised:
            self.push(folder, gadget)
        self.assertIn("manifest.json", str(raised.exception))

    def test_a_refused_put_is_the_end_of_the_push(self):
        folder = make_package(self.tmp, files={"util.lua": "return 1"})
        gadget = Gadget()
        gadget.refuse[("main.lua", 0)] = "main.lua is larger than 256 KB"
        reply, board, _ = self.push(folder, gadget)
        self.assertEqual(reply.kind, "err")
        self.assertEqual(reply.data["message"], "main.lua is larger than 256 KB")
        self.assertEqual(board.sent("app.commit"), [])
        self.assertEqual([p for p, _ in gadget.puts], ["manifest.json"])

    def test_a_refused_commit_is_a_reply_to_print(self):
        reply, _, _ = self.push(make_package(self.tmp), Gadget(commit_error="busy: another install is running"))
        self.assertEqual((reply.kind, reply.data["op"]), ("err", "commit"))

    def test_a_commit_that_never_answers_is_an_error_at_the_deadline(self):
        with self.assertRaises(self.app.AppError) as raised:
            self.push(make_package(self.tmp), Gadget(commit_delay=10_000))
        self.assertIn("app.commit", str(raised.exception))
        self.assertIn(f"{self.app.COMMIT_S:g} s", str(raised.exception))
        self.assertGreaterEqual(self.app.COMMIT_S, 15, "an install with a run takes about 2.2 s, and may take more")

    def test_the_commit_is_waited_for_the_devices_deadline_and_five_seconds_more(self):
        # G: the board's install deadline is APPS_INSTALL_DEADLINE_MS (apps_install.h), and a commit can take all of it.
        deadline_s = c_define("apps_install.h", "APPS_INSTALL_DEADLINE_MS") / 1000
        self.assertEqual(deadline_s, 90)
        self.assertEqual(self.app.COMMIT_S, deadline_s + 5)
        reply, _, _ = self.push(make_package(self.tmp), Gadget(commit_delay=deadline_s + 4))
        self.assertEqual(reply.kind, "result", "an install that ends 4 s after the board's own deadline")
        with self.assertRaises(self.app.AppError) as raised:
            self.push(make_package(self.tmp), Gadget(commit_delay=deadline_s + 6))
        self.assertIn("no result of app.commit within 95 s", str(raised.exception))

    def test_echo_lines_while_waiting_are_not_replies(self):
        gadget = Gadget(echo=["@app.log demo hello", "@app.log demo (3 lines not echoed)"])
        reply, _, _ = self.push(make_package(self.tmp), gadget)
        self.assertEqual(reply.kind, "result")

    def test_the_cli_prints_the_result_and_exits_zero_when_it_is_ok(self):
        folder = make_package(self.tmp)
        board = self.board()
        code, out, err, captures = self.cli(["push", "/dev/x", str(folder), "--run"], board)
        self.assertEqual(code, 0, err)
        (line,) = out.splitlines()
        result = json.loads(line)
        self.assertEqual((result["op"], result["ok"], result["payload"]["state"]), ("commit", True, "running"))
        self.assertEqual(result["payload"]["storage"], "ram")
        self.assertEqual(captures, [], "no --snap, no screenshot")
        self.assertTrue(board.closed)
        self.assertIn("running", err)

    def test_the_cli_exits_one_when_the_result_says_not_ok(self):
        failed = {"ok": False, "error": {"code": "crashed", "message": "installed but crashed in start(): boom\nmore"}}
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp)), "--run"],
                                     self.board(Gadget(result=failed)))
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["error"]["code"], "crashed")
        self.assertIn("boom", err)

    def test_only_a_true_ok_is_ok(self):
        for ok in (False, None, 0, 1, "true", "yes", [], {}):
            with self.subTest(ok=ok):
                code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))],
                                             self.board(Gadget(result={"ok": ok, "payload": {"id": "demo"}})))
                self.assertEqual(code, 1, out)
                self.assertEqual(len(out.splitlines()), 1, "the board's line is printed all the same")
                self.assertTrue(err.strip())

    def test_ctrl_c_ends_the_run_with_130_and_no_traceback(self):
        gadget = Gadget()
        gadget.silent.add(("manifest.json", 0))
        board = self.board(gadget)
        board.on_wait = mock.Mock(side_effect=KeyboardInterrupt)   # the first wait that has to give up
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))], board)
        self.assertEqual((code, out, err), (130, "", ""))
        self.assertTrue(board.closed)

    def test_the_cli_prints_a_refusal_and_exits_one(self):
        gadget = Gadget()
        gadget.refuse[("manifest.json", 0)] = "manifest.json is larger than 8 KB"
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))], self.board(gadget))
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["message"], "manifest.json is larger than 8 KB")
        self.assertIn("manifest.json is larger than 8 KB", err)

    def test_the_cli_says_a_timeout_in_words_and_exits_one(self):
        gadget = Gadget()
        gadget.silent.add(("main.lua", 0))
        code, out, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))], self.board(gadget))
        self.assertEqual((code, out), (1, ""))
        self.assertIn("main.lua", err)
        self.assertIn("2 s", err)

    def test_snap_waits_a_second_then_captures_on_the_open_port(self):
        board = self.board()
        shot = self.tmp / "shot.png"
        argv = ["push", "/dev/x", str(make_package(self.tmp)), "--run", "--snap", str(shot)]
        code, out, err, captures = self.cli(argv, board)
        self.assertEqual(code, 0, err)
        ((ser, keys, slept_by_then),) = captures
        self.assertIs(ser, board.ser, "the picture is taken on the port the push used (C15)")
        self.assertEqual(keys, "w", "the screen is woken first: a sleeping screen's picture is stale")
        self.assertEqual(slept_by_then, [1.0], "it waits 1 s after the result, and then takes the picture")
        self.assertEqual(self.clock.slept, [1.0])
        self.assertEqual(len(out.splitlines()), 1, "stdout is the result and nothing else")
        self.assertTrue(shot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertIn(str(shot), err)

    def test_a_screenshot_that_cannot_be_taken_fails_the_run_after_the_result_is_printed(self):
        board = self.board()
        snap = self.app.snap

        def fail(ser, keys="", settle=None):
            raise snap.SnapError("no screenshot: the board never answered")

        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(self.app, "open_board", return_value=board), mock.patch.object(snap, "capture", fail), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.app.main(["push", "/dev/x", str(make_package(self.tmp)), "--snap", str(self.tmp / "x.png")])
        self.assertEqual(code, 1)
        self.assertIn("never answered", err.getvalue())
        self.assertEqual(json.loads(out.getvalue())["ok"], True)
        self.assertFalse((self.tmp / "x.png").exists())

    def test_a_picture_that_cannot_be_saved_is_said_before_the_push_and_after_the_capture(self):
        board = self.board()
        argv = ["push", "/dev/x", str(make_package(self.tmp)), "--run", "--snap"]
        code, out, err, captures = self.cli([*argv, str(self.tmp / "no-such-folder" / "x.png")], board)
        self.assertEqual((code, out, captures, board.commands), (1, "", [], []), "nothing is pushed for a lost picture")
        self.assertIn("no-such-folder", err)
        board = self.board()
        code, out, err, captures = self.cli([*argv, str(self.tmp)], board)   # a folder, not a file
        self.assertEqual((code, len(captures)), (1, 1))
        self.assertEqual(json.loads(out)["ok"], True, "the install is reported all the same")
        self.assertIn(f"can't write {self.tmp}", err)
        self.assertNotIn("lost the board", err)

    def test_no_screenshot_after_a_refusal(self):
        argv = ["push", "/dev/x", str(make_package(self.tmp)), "--snap", str(self.tmp / "x.png")]
        code, _, _, captures = self.cli(argv, self.board(Gadget(commit_error="nothing is staged")))
        self.assertEqual((code, captures), (1, []))

    def test_a_crashed_app_is_photographed_all_the_same(self):
        # The crash card is on the screen, and it is what the person wants to see.
        failed = {"ok": False, "error": {"code": "crashed", "message": "installed but crashed in start(): boom"}}
        code, _, _, captures = self.cli(["push", "/dev/x", str(make_package(self.tmp)), "--run", "--snap",
                                         str(self.tmp / "x.png")], self.board(Gadget(result=failed)))
        self.assertEqual((code, len(captures)), (1, 1))


class OpenTest(ToolTestCase):
    def test_the_port_is_opened_the_way_chat_does_without_toggling_dtr_or_rts(self):
        opened = []

        class Recorder:
            def __init__(self, port):
                opened.append(port)

        with mock.patch.object(self.app.chat, "Board", Recorder):
            board = self.app.open_board("/dev/cu.usbmodem101")
        self.assertIsInstance(board, Recorder)
        self.assertEqual(opened, ["/dev/cu.usbmodem101"])
        source = re.sub(r"#[^\n]*", "", APP_PY.read_text())
        for word in (".dtr", ".rts", "setDTR", "setRTS", "serial.Serial"):
            self.assertNotIn(word, source, "chat.Board opens the port; app.py never touches the control lines")

    def test_a_port_that_cannot_be_opened_is_an_app_error(self):
        def refuse(port):
            raise self.app.chat.BoardError("Can't open /dev/x: busy")

        with mock.patch.object(self.app.chat, "Board", refuse):
            with self.assertRaises(self.app.AppError) as raised:
                self.app.open_board("/dev/x")
        self.assertIn("busy", str(raised.exception))

    def test_a_board_that_goes_away_ends_the_run_with_a_message(self):
        board = self.board()
        board.read_line = mock.Mock(side_effect=OSError(5, "Input/output error"))
        code, _, err, _ = self.cli(["push", "/dev/x", str(make_package(self.tmp))], board)
        self.assertEqual(code, 1)
        self.assertIn("Input/output error", err)
        self.assertTrue(board.closed)

    def test_a_board_that_goes_away_is_the_boards_and_a_checks_failure_is_the_hosts(self):
        # I: an OSError of push, watch or logs is the port's; one of check never had a board to lose.
        for argv in (["push", "/dev/x", str(make_package(self.tmp))], ["logs", "/dev/x"],
                     ["watch", "/dev/x", str(make_package(self.tmp))]):
            with self.subTest(argv[0]):
                board = self.board()
                board.read_line = mock.Mock(side_effect=OSError(5, "Input/output error"))
                code, _, err, _ = self.cli(argv, board)
                self.assertEqual(code, 1)
                self.assertIn("lost the board: [Errno 5] Input/output error", err)
        out, err = io.StringIO(), io.StringIO()
        failing = OSError(28, "No space left on device")
        with mock.patch.object(self.app.apphost_build, "ensure", side_effect=failing), \
                mock.patch.object(self.app, "open_board", side_effect=AssertionError("check needs no gadget")), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.app.main(["check", str(make_package(self.tmp))])
        self.assertEqual((code, out.getvalue()), (1, ""))
        self.assertIn("No space left on device", err.getvalue())
        self.assertNotIn("board", err.getvalue())
        self.assertIn("check", err.getvalue())

    def test_the_help_names_the_four_commands(self):
        for argv, words in ((["--help"], ("check", "push", "watch", "logs")),
                            (["push", "--help"], ("--run", "--snap", "PORT", "DIR")),
                            (["check", "--help"], ("--script", "DIR"))):
            out = io.StringIO()
            with self.subTest(argv), contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as raised:
                self.app.main(argv)
            self.assertEqual(raised.exception.code, 0)
            for word in words:
                self.assertIn(word, out.getvalue())


class LogsTest(ToolTestCase):
    def test_logs_prints_the_apps_echo_and_nothing_else(self):
        echo = ["@app.log demo hello", "@app.log demo (7 lines not echoed)", "@app.log demo tab\there",
                "@app.log demo two lines? no, one line"]
        later = [(0.1, "\x1b[0;32mI (1) muse: noise\x1b[0m"), (0.1, echo[0]),
                 (0.1, "junk @app.log demo not at column 0"), (0.1, '@app {"op":"status"}'), (0.2, echo[1]),
                 (0.2, "@app.ack x 1"), (0.3, echo[2]), (0.3, echo[3])]
        board = self.board(later=later)
        board.on_wait = mock.Mock(side_effect=[None, KeyboardInterrupt])
        code, out, err, _ = self.cli(["logs", "/dev/x"], board)
        self.assertEqual(code, 0, "Ctrl-C is how logs ends")
        self.assertEqual(out.split("\n")[:-1], echo, "one line to each echo, and U+2028 is no line break")
        self.assertTrue(board.closed)

    def test_logs_synchronizes_first(self):
        board = self.board(Gadget(glue="@app.log demo fragment of an echo "), backlog=["@app.log demo old"])
        board.on_wait = mock.Mock(side_effect=[None, KeyboardInterrupt])   # the first query's wait, then logs' own
        code, out, _, _ = self.cli(["logs", "/dev/x"], board)
        self.assertEqual(code, 0)
        self.assertEqual(out, "", "what came before the sync is history, and a fragment isn't a line")
        self.assertEqual(len(board.sent("app.put")), 2)


class WatchTest(ToolTestCase):
    def watch(self, folder, edits, gadget=None, polls=6):
        """Runs `watch` until it has waited `polls` times for the port; edits is {number of the wait: function(folder)}.
        -> (exit status, stdout, stderr, board, the times of the waits)."""
        board = self.board(gadget)
        waits = []

        def on_wait():
            waits.append(self.clock.now)
            board.events.append(("wait", self.clock.now))   # in the order of what was sent: a test says what came before
            if len(waits) in edits:
                edits[len(waits)](folder)
            if len(waits) >= polls:
                raise KeyboardInterrupt

        board.on_wait = on_wait
        code, out, err, _ = self.cli(["watch", "/dev/x", str(folder)], board)
        return code, out, err, board, waits

    @staticmethod
    def edit(path, text, seconds=1):
        """Writes the file and moves its mtime on, so that the change shows whatever the clock's resolution."""
        before = path.stat().st_mtime_ns if path.exists() else 0
        path.write_text(text)
        os.utime(path, ns=(before, before + seconds * 1_000_000_000))

    def test_the_first_push_is_at_once_and_each_change_pushes_again_with_run(self):
        folder = make_package(self.tmp, files={"util.lua": "return 1"})
        code, out, err, board, waits = self.watch(folder, {
            2: lambda f: self.edit(f / "main.lua", "return {start = function() end}\n"),
            5: lambda f: self.edit(f / "util.lua", "return 2\n", 2),
        }, polls=8)
        self.assertEqual(code, 0, err)
        self.assertEqual(board.sent("app.commit"), ["app.commit demo run"] * 3)
        # Q: at once is before the first poll, and a change is pushed before the next one: commit, two polls (the edit is
        # made in the second and seen as it ends), a commit, three polls (the next edit is in the fifth), a commit.
        marks = ["wait" if e[0] == "wait" else "commit" for e in board.events
                 if e[0] == "wait" or (e[0] == "line" and e[1].startswith("app.commit"))]
        self.assertEqual(marks[:9], ["commit", "wait", "wait", "commit", "wait", "wait", "wait", "commit", "wait"])
        self.assertEqual(board.raw_writes.count(b"w\n"), 3, "each push wakes the screen: one looks away while editing")
        self.assertEqual([json.loads(line)["op"] for line in out.splitlines()], ["commit"] * 3)
        gaps = [round(b - a, 3) for a, b in zip(waits, waits[1:])]
        self.assertTrue(all(gap >= 0.5 for gap in gaps), gaps)
        self.assertEqual(gaps[-2:], [0.5, 0.5], "polling every half second")

    def test_an_edit_made_while_a_push_is_under_way_is_not_lost(self):
        folder = make_package(self.tmp)
        gadget, edited = Gadget(), []

        def respond(command):
            if command.startswith("app.commit") and not edited:
                edited.append(command)
                self.edit(folder / "main.lua", "return {start = function() end}\n")   # the push read the old one
            return gadget(command)

        _, _, _, board, _ = self.watch(folder, {}, gadget=respond, polls=4)
        self.assertEqual(len(board.sent("app.commit")), 2, "the poll after the push sees what changed during it")

    def test_nothing_changing_pushes_nothing_more(self):
        _, _, _, board, _ = self.watch(make_package(self.tmp), {}, polls=6)
        self.assertEqual(len(board.sent("app.commit")), 1)

    def test_editor_droppings_are_not_changes(self):
        def git(f):
            (f / ".git").mkdir()
            (f / ".git" / "index").write_text("x")

        _, _, _, board, _ = self.watch(make_package(self.tmp), {
            2: lambda f: (f / ".main.lua.swp").write_text("x"),
            3: lambda f: (f / "main.lua~").write_text("x"),
            4: git,
        }, polls=7)
        self.assertEqual(len(board.sent("app.commit")), 1)

    def test_a_new_file_is_a_change(self):
        _, _, _, board, _ = self.watch(make_package(self.tmp), {2: lambda f: (f / "extra.lua").write_text("return 1")},
                                       polls=5)
        self.assertEqual(len(board.sent("app.commit")), 2)

    def test_a_push_that_fails_does_not_end_the_watch(self):
        folder = make_package(self.tmp)
        good = (folder / "manifest.json").read_text()
        code, out, err, board, _ = self.watch(folder, {
            2: lambda f: self.edit(f / "manifest.json", "{not json", 5),
            4: lambda f: self.edit(f / "manifest.json", good, 6),
        }, polls=7)
        self.assertEqual(code, 0)
        self.assertIn("isn't JSON", err)
        self.assertEqual(len(board.sent("app.commit")), 2, "the first push, and the one after the manifest is mended")

    def test_a_package_the_tool_refuses_is_said_and_the_watch_goes_on(self):
        # D: every refusal of reading the folder is an AppError, whatever is wrong with it, so the watch says what and
        # polls on, and the push after the folder is mended goes through. (Each case: what to do to util.lua at the
        # second wait, what to do to mend it at the fourth.)
        def replace(folder, make):
            path = folder / "util.lua"
            if path.is_dir() and not path.is_symlink():
                path.rmdir()
            elif path.exists() or path.is_symlink():
                path.unlink()
            make(path)

        def sparse(path):
            with open(path, "wb") as f:
                f.truncate(1 << 30)

        def nested_manifest(folder):
            self.edit(folder / "manifest.json", "[" * 100_000 + "]" * 100_000, 7)

        def mend_manifest(folder):
            self.edit(folder / "manifest.json", self.good_manifest, 9)

        def mend(folder):
            replace(folder, lambda path: self.edit(path, "return 2\n", 8))

        elsewhere = self.tmp / "elsewhere.lua"
        elsewhere.write_text("return 'elsewhere'")
        cases = {
            "a FIFO": (lambda f: replace(f, os.mkfifo), mend, "regular file"),
            "a folder": (lambda f: replace(f, os.mkdir), mend, "regular file"),
            "a link out": (lambda f: replace(f, lambda path: os.symlink(elsewhere, path)), mend, "outside the app's folder"),
            "a huge file": (lambda f: replace(f, sparse), mend, "util.lua is larger than 256 KB"),
            "a manifest of nested arrays": (nested_manifest, mend_manifest, "manifest.json is larger than 8 KB"),
            "a manifest that is a FIFO": (lambda f: (f / "manifest.json").unlink() or os.mkfifo(f / "manifest.json"),
                                          lambda f: (f / "manifest.json").unlink() or self.edit(
                                              f / "manifest.json", self.good_manifest, 9), "regular file"),
        }
        for what, (hurt, mended, words) in cases.items():
            with self.subTest(what), no_longer_than(60):
                folder = make_package(self.tmp, files={"util.lua": "return 1"})
                self.good_manifest = (folder / "manifest.json").read_text()
                code, out, err, board, _ = self.watch(folder, {2: hurt, 4: mended}, polls=8)
                self.assertEqual(code, 0, err)
                self.assertIn(words, err)
                self.assertNotIn("Traceback", err)
                self.assertEqual(len(board.sent("app.commit")), 2, "the first push, and the one after it was mended")

    def test_a_crash_or_a_refusal_is_printed_and_the_watch_goes_on(self):
        crashed = {"ok": False, "error": {"code": "crashed", "message": "installed but crashed in start(): boom"}}
        folder = make_package(self.tmp)
        code, out, err, board, _ = self.watch(folder, {2: lambda f: self.edit(f / "main.lua", "return {}\n")},
                                              gadget=Gadget(result=crashed), polls=4)
        self.assertEqual(code, 0)
        self.assertEqual(len(out.splitlines()), 2)
        self.assertIn("boom", err)
        folder = make_package(self.tmp)
        code, out, err, board, _ = self.watch(folder, {2: lambda f: self.edit(f / "main.lua", "return {}\n")},
                                              gadget=Gadget(commit_error="busy: another install is running"), polls=4)
        self.assertEqual((code, len(out.splitlines())), (0, 2))
        self.assertIn("busy", err)

    def test_a_board_that_stops_answering_is_said_and_the_watch_goes_on(self):
        folder = make_package(self.tmp)
        gadget = Gadget()
        calls = []
        original = gadget.put

        def put(words):
            calls.append(words[2])
            return [] if len(calls) == 3 else original(words)

        gadget.put = put
        code, out, err, board, _ = self.watch(folder, {2: lambda f: self.edit(f / "main.lua", "return {}\n")},
                                              gadget=gadget, polls=5)
        self.assertEqual(code, 0)
        self.assertIn("@app.ack", err)
        # Q: the watch goes on: the push that gave up at its second piece never committed, and the edit at the second wait
        # (the first is the ack's own) is pushed whole, and committed, before the third.
        marks = ["wait" if e[0] == "wait" else "commit" for e in board.events
                 if e[0] == "wait" or (e[0] == "line" and e[1].startswith("app.commit"))]
        self.assertEqual(marks[:4], ["wait", "wait", "commit", "wait"])
        self.assertEqual(len(board.sent("app.commit")), 1)
        self.assertEqual(json.loads(out)["op"], "commit")

    def test_the_apps_log_is_printed_between_pushes(self):
        folder = make_package(self.tmp)
        board = self.board(later=[(1.2, "@app.log demo tick 1"), (1.2, "junk"), (1.3, "@app.log demo tick 2")])
        board.on_wait = mock.Mock(side_effect=[None, None, None, KeyboardInterrupt])
        code, out, _, _ = self.cli(["watch", "/dev/x", str(folder)], board)
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertEqual(lines[1:], ["@app.log demo tick 1", "@app.log demo tick 2"])
        self.assertEqual(json.loads(lines[0])["op"], "commit")

    def test_the_signature_follows_mtime_and_size(self):
        folder = make_package(self.tmp)
        before = self.app.signature(folder)
        self.assertEqual(before, self.app.signature(folder))
        self.edit(folder / "main.lua", "return {}\n", 3)
        edited = self.app.signature(folder)
        self.assertNotEqual(before, edited)
        # A write that keeps the mtime (a tool that restores it) is still a change when the size moves ...
        path = folder / "main.lua"
        stat = path.stat()
        path.write_text("return {}\n-- and more\n")
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        longer = self.app.signature(folder)
        self.assertNotEqual(longer, edited)
        # ... and the mtime moving alone, with the size as it was, is one too.
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns + 5_000_000_000))
        self.assertNotEqual(self.app.signature(folder), longer)
        self.assertEqual({s[0] for s in self.app.signature(folder)}, {str(folder / "manifest.json"), str(path)})


class RawSerial:
    """A serial port as pyserial gives one: bytes written, bytes read in small pieces, an empty read after the timeout.
    The board behind it answers whole lines (`respond(command)` as in FakeBoard) with CR LF line ends, as the ESP
    does."""

    baudrate = 115200

    def __init__(self, clock, respond, piece=7):
        self.clock, self.respond, self.piece = clock, respond, piece
        self.incoming, self.pending, self.received, self.unfinished = bytearray(), [], bytearray(), bytearray()

    def write(self, data):
        self.received += data
        self.unfinished += data
        while b"\n" in self.unfinished:
            line, _, self.unfinished = bytes(self.unfinished).partition(b"\n")
            self.unfinished = bytearray(self.unfinished)
            if line.startswith(b">"):
                for delay, text in self.respond(line[1:].decode()):
                    self.pending.append((self.clock.now + delay, (text + "\r\n").encode()))

    def flush(self):
        pass

    def _deliver(self):
        for due in sorted(item for item in self.pending if item[0] <= self.clock.now):
            self.pending.remove(due)
            self.incoming += due[1]

    @property
    def in_waiting(self):
        self._deliver()
        return len(self.incoming)

    def read(self, size=1):
        self._deliver()
        if not self.incoming:
            self.clock.advance_to(self.clock.now + 0.05)   # the port's timeout
            return b""
        chunk = bytes(self.incoming[:min(size, self.piece)])
        del self.incoming[:len(chunk)]
        return chunk


class FakesTest(unittest.TestCase):
    """The fakes of this file fail a test that waits for ever: an hour on the test's clock is a loop that doesn't end,
    whoever is waiting (M)."""

    def test_a_sleep_a_board_read_and_a_port_read_all_reach_the_hour_guard(self):
        clock = FakeClock()
        clock.sleep(FakeClock.HOUR - 1)
        with self.assertRaises(AssertionError):
            clock.sleep(2)
        clock = FakeClock()
        with self.assertRaises(AssertionError):
            FakeBoard(clock).read_line(clock.now + 2 * FakeClock.HOUR)
        # A loop that ignores that nothing came goes on to the guard: 36 reads of 100 s.
        clock, reads = FakeClock(), 0
        board = FakeBoard(clock)
        with self.assertRaises(AssertionError):
            while True:
                board.read_line(clock.now + 100)
                reads += 1
        self.assertEqual(reads, 36)
        # A line that is due after the hour is no escape either.
        clock = FakeClock()
        board = FakeBoard(clock, later=[(2 * FakeClock.HOUR, "late")])
        with self.assertRaises(AssertionError):
            board.read_line(clock.now + 3 * FakeClock.HOUR)
        clock = FakeClock()
        port = RawSerial(clock, lambda command: [])
        with self.assertRaises(AssertionError):
            for _ in range(int(FakeClock.HOUR / 0.05) + 100):
                port.read()
        self.assertGreater(clock.now, clock.START + FakeClock.HOUR)
        # Time inside the hour is just time, and a clock never goes back.
        clock = FakeClock()
        clock.advance_to(clock.now + 10)
        clock.advance_to(clock.START)
        self.assertEqual(clock.now, FakeClock.START + 10)


class WireTest(ToolTestCase):
    """The real chat.Board over a fake port of bytes: the framing of every line, CR LF, a read at a time."""

    def board(self, gadget=None, piece=7, extra=()):
        from collections import deque
        gadget = gadget or Gadget()
        lines = list(extra)
        chat = self.app.chat
        port = RawSerial(self.clock, lambda command: gadget(command) or [], piece)
        for text in lines:
            port.pending.append((self.clock.now + 0.3, (text + "\r\n").encode()))
        board = object.__new__(chat.Board)
        board.port, board.ser, board.paced, board.buf = "/dev/cu.fake", port, False, b""
        board.log, board.last_seq, board.lost_lines = deque(maxlen=30), None, 0
        board.close = lambda: None
        patch = mock.patch.object(chat, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        return board

    def test_a_push_over_bytes(self):
        folder = make_package(self.tmp, main="return {}\n" + "-- " + "y" * 3000 + "\n", files={"u.lua": "return 1\n"})
        gadget = Gadget(echo=["@app.log demo a b"])
        board = self.board(gadget)
        code, out, err, _ = self.cli(["push", "/dev/x", str(folder), "--run"], board)
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["payload"]["id"], "demo")
        self.assertEqual(bytes(gadget.files["main.lua"]), (folder / "main.lua").read_bytes())
        sent = bytes(board.ser.received)
        self.assertTrue(sent.startswith(b"\n>app.put 0"), "a newline, then the query")
        self.assertIn(b"\nw\n>app.put demo manifest.json 0 ", sent, "a run wakes the screen first")
        for line in sent.split(b"\n"):
            self.assertLessEqual(len(line) + 1, 1000)
            self.assertTrue(line in (b"", b"w") or line.startswith(b">app."), line[:30])
        self.assertTrue(sent.endswith(b">app.commit demo run\n"))

    def test_logs_over_bytes_keeps_a_line_whole_that_has_unicode_line_separators(self):
        echo = "@app.log demo one two three \xe9 世"
        board = self.board(extra=[echo, "@app.log demo second"])
        board.ser.on_idle = None
        counter = []
        real_read = board.ser.read

        def read(size=1):
            data = real_read(size)
            if not data:
                counter.append(1)
                if len(counter) > 40:
                    raise KeyboardInterrupt
            return data

        board.ser.read = read
        code, out, err, _ = self.cli(["logs", "/dev/x"], board)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.split("\n")[:-1], [echo, "@app.log demo second"])


# ---- check, and the host build behind it ----

class BuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = load_app()
        cls.build = cls.app.apphost_build

    def test_the_parsers_are_apps_hosts_own(self):
        # E14: one parser of components/lua/CMakeLists.txt and of sources.txt, for the host tests and the dev loop.
        build = self.build
        self.assertEqual(build.lua_sources(), apps_host.lua_sources())
        self.assertEqual(build.lua_defines(), apps_host.lua_defines())
        self.assertEqual(build.lua_options(), apps_host.lua_options())
        self.assertEqual(build.apphost_sources(), apps_host.apphost_sources())
        self.assertIn("LUAI_MAXCCALLS=40", build.lua_defines())

    GOOD_CMAKE = ('if(CONFIG_X)\n    idf_component_register(SRCS "src/lapi.c" "src/lvm.c" INCLUDE_DIRS "src")\n'
                  '    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)  # a comment\n'
                  '    target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=40 LUA_FOO)\nendif()\n')

    def test_a_lua_component_the_parsers_cannot_follow_is_refused(self):
        build, good = self.build, self.GOOD_CMAKE
        with tempfile.TemporaryDirectory() as tmp:
            cmake = Path(tmp) / "components" / "lua" / "CMakeLists.txt"
            cmake.parent.mkdir(parents=True)
            cmake.write_text(good)
            self.assertEqual(build.lua_defines(tmp), ["LUAI_MAXCCALLS=40", "LUA_FOO"])
            self.assertEqual(build.lua_options(tmp), ["-O2", "-w"])
            self.assertEqual([p.name for p in build.lua_sources(tmp)], ["lapi.c", "lvm.c"])
            define = "target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=40 LUA_FOO)"
            for what, text, words in (
                    ("a second call", good + "target_compile_definitions(${COMPONENT_LIB} PRIVATE X=1)\n",
                     "exactly one"),
                    ("no call", good.replace(define, ""), "exactly one"),
                    ("PUBLIC", good.replace("PRIVATE LUAI", "PUBLIC LUAI"), "PRIVATE"),
                    ("PUBLIC later", good.replace("LUA_FOO)", "PUBLIC LUA_FOO)"), "PRIVATE"),
                    ("INTERFACE later", good.replace("LUA_FOO)", "INTERFACE LUA_FOO)"), "PRIVATE"),
                    ("another target", good.replace("(${COMPONENT_LIB} PRIVATE LUAI", "(other PRIVATE LUAI"),
                     "PRIVATE")):
                with self.subTest(what):
                    cmake.write_text(text)
                    with self.assertRaises(build.BuildError) as raised:
                        build.lua_defines(tmp)
                    self.assertIn(words, str(raised.exception))
            cmake.write_text("idf_component_register()\n")
            with self.assertRaises(build.BuildError):
                build.lua_sources(tmp)

    def test_a_sources_list_the_build_cannot_follow_is_refused(self):
        build = self.build
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in ("components/muse/apps/a.c", "components/muse/apps/a.h", "components/lua/src/lvm.c",
                        "tools/muse/apphost/b.c"):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("")
            listing = root / "tools/muse/apphost/sources.txt"
            listing.write_text("# a comment\n\n  components/muse/apps/a.c  \ntools/muse/apphost/b.c\n")
            self.assertEqual([p.name for p in build.apphost_sources(root)], ["a.c", "b.c"])
            for line in ("components/muse/apps/a.h", "components/lua/src/lvm.c", "components/muse/apps/nope.c",
                         "tools/muse/apphost/../../../x.c", "/etc/passwd"):
                with self.subTest(line):
                    listing.write_text(line + "\n")
                    with self.assertRaises(build.BuildError) as raised:
                        build.apphost_sources(root)
                    self.assertIn(line, str(raised.exception))
            listing.write_text("# nothing\n")
            with self.assertRaises(build.BuildError):
                build.apphost_sources(root)

    def test_the_compile_command_has_the_defines_of_the_device(self):
        commands = self.build.compile_commands(["cc"], Path("/obj"))
        (lua,) = [c for c in commands if any(a.endswith("/lvm.c") for a in c)]
        self.assertIn("-DLUAI_MAXCCALLS=40", lua)
        for option in self.build.lua_options():
            self.assertIn(option, lua)
        # It builds the files CMake and sources.txt list: no more, no fewer.
        built = sorted(Path(arg) for c in commands for arg in c if arg.endswith(".c"))
        self.assertEqual(built, sorted([*apps_host.lua_sources(), *apps_host.apphost_sources()]))
        # The runner's own files get no Lua define and the tests' strictness, but a developer's compiler may be newer.
        rt = next(c for c in commands if any(a.endswith("/apps_rt.c") for a in c))
        self.assertNotIn("-DLUAI_MAXCCALLS=40", rt)
        for flag in ("-std=c11", "-Wall", "-Wextra"):
            self.assertIn(flag, rt)
        self.assertNotIn("-Werror", rt, "a newer compiler's new warning mustn't end someone's dev loop")
        strict = self.build.compile_commands(["cc"], Path("/obj"), strict=True)
        self.assertEqual(sum("-Werror" in c for c in strict), len(apps_host.apphost_sources()))
        self.assertIn("-lm", self.build.link_command(["cc"], [Path("/obj/a.o")], Path("/obj/apphost")))

    def test_the_compiler_is_cc_or_the_one_the_environment_names(self):
        build = self.build
        with mock.patch.dict(os.environ, {"CC": "zig cc -target x86_64-linux-gnu.2.31"}), \
                mock.patch.object(build.shutil, "which", return_value="/usr/bin/zig"):
            self.assertEqual(build.compiler(), ["zig", "cc", "-target", "x86_64-linux-gnu.2.31"])
        without = {k: v for k, v in os.environ.items() if k != "CC"}
        with mock.patch.dict(os.environ, without, clear=True), mock.patch.object(build.shutil, "which",
                                                                                  return_value="/usr/bin/cc"):
            self.assertEqual(build.compiler(), ["cc"])
        with mock.patch.dict(os.environ, {"CC": "no-such-compiler"}), mock.patch.object(build.shutil, "which",
                                                                                         return_value=None):
            with self.assertRaises(build.BuildError) as raised:
                build.compiler()
        self.assertIn("no-such-compiler", str(raised.exception))
        self.assertIn("CC", str(raised.exception))

    # ---- The cache key (C32) ----

    @staticmethod
    def copy_tree(dest):
        """The directories apphost is built from, in a tree of their own."""
        for rel in ("components/lua", "components/muse/apps", "tools/muse/apphost"):
            shutil.copytree(ROOT / rel, Path(dest) / rel, ignore=shutil.ignore_patterns("fonts", "examples", "*.md"))
        return Path(dest)

    def test_the_key_covers_every_source_and_header_it_should(self):
        build = self.build
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(tmp)
            key = build.cache_key(["cc"], tree)
            self.assertRegex(key, r"^[0-9a-f]{40}$")
            self.assertEqual(key, build.cache_key(["cc"], ROOT), "where the checkout is makes no difference")

            def changes(rel):
                path = tree / rel
                original = path.read_bytes()
                text = path.suffix == ".txt" or path.name == "CMakeLists.txt"
                comment = b"# changed" if text else b"/* changed */"
                try:
                    path.write_bytes(original + b"\n" + comment + b"\n")
                    return build.cache_key(["cc"], tree) != key
                finally:
                    path.write_bytes(original)

            for rel in ("components/muse/apps/apps_rt.c",          # a listed source
                        "tools/muse/apphost/apphost.c",            # the runner's own
                        "components/lua/src/lvm.c",                 # Lua's
                        "components/muse/apps/apps_pkg.h",          # every *.h of apps/ ...
                        "components/muse/apps/apps_task.h",         # ... the ones apphost includes or not
                        "tools/muse/apphost/apphost_port.h",        # every *.h of apphost/
                        "components/lua/src/lua.h",                 # the Lua headers
                        "components/lua/src/luaconf.h",
                        "tools/muse/apphost/sources.txt",           # what is listed
                        "components/lua/CMakeLists.txt"):           # Lua's list, defines and options
                with self.subTest(rel):
                    self.assertTrue(changes(rel), f"{rel} goes into apphost, and not into its cache key")
            self.assertFalse(changes("components/muse/apps/apps_task.c"), "a device-only source isn't apphost's")
            for rel in ("components/muse/apps/apps_rt.c", "components/muse/apps/apps_pkg.h"):
                with self.subTest(same_length=rel):
                    path = tree / rel
                    original = path.read_bytes()
                    flipped = bytearray(original)
                    flipped[len(flipped) // 2] ^= 1
                    try:
                        path.write_bytes(bytes(flipped))
                        self.assertNotEqual(build.cache_key(["cc"], tree), key, "what is in a file, not how long it is")
                    finally:
                        path.write_bytes(original)
            # A header with another name is another build, whatever is in it.
            header = tree / "components/muse/apps/apps_task.h"
            header.rename(tree / "components/muse/apps/apps_task_renamed.h")
            self.assertNotEqual(build.cache_key(["cc"], tree), key, "a renamed file isn't the same build")
            (tree / "components/muse/apps/apps_task_renamed.h").rename(header)
            self.assertEqual(build.cache_key(["cc"], tree), key)
            with mock.patch.object(build, "MUSE_FLAGS", [*build.MUSE_FLAGS, "-DEXTRA"]):
                self.assertNotEqual(build.cache_key(["cc"], tree), key, "the flags are part of it")
            with mock.patch.object(build, "LINK_FLAGS", [*build.LINK_FLAGS, "-lextra"]):
                self.assertNotEqual(build.cache_key(["cc"], tree), key, "and the link's")
            cmake = tree / "components/lua/CMakeLists.txt"
            original = cmake.read_text()
            cmake.write_text(original.replace("LUAI_MAXCCALLS=40", "LUAI_MAXCCALLS=44"))
            self.assertNotEqual(build.cache_key(["cc"], tree), key, "a changed define is another binary")
            cmake.write_text(original)
            self.assertNotEqual(build.cache_key(["gcc"], tree), key, "the compiler is part of it")
            with mock.patch.object(build.platform, "machine", return_value="riscv64"):
                self.assertNotEqual(build.cache_key(["cc"], tree), key, "so is the machine")
            with mock.patch.object(build.platform, "system", return_value="Plan9"):
                self.assertNotEqual(build.cache_key(["cc"], tree), key, "and the system")

    def test_the_end_of_one_file_cannot_pass_for_the_start_of_the_next(self):
        # Each file goes into the digest as its name, its length and its bytes. With only the names between the bytes,
        # a header's whole entry could be written at the end of the file before it, and both trees would have one key.
        build = self.build
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(tmp)
            listing = tree / "tools/muse/apphost/sources.txt"
            original = listing.read_bytes()
            extra = tree / "tools/muse/apphost/zz.h"
            self.assertEqual(build.key_inputs(tree)[-1], listing.resolve(), "sources.txt is the last file keyed")
            listing.write_bytes(original + b"# ")
            extra.write_bytes(b"C")
            apart = build.cache_key(["cc"], tree)
            extra.unlink()
            listing.write_bytes(original + b"# tools/muse/apphost/zz.h\0C")
            self.assertNotEqual(build.cache_key(["cc"], tree), apart)

    def test_every_file_a_listed_source_includes_is_in_the_key(self):
        covered = {p.resolve() for p in self.build.key_inputs(ROOT)}
        pending = [p.resolve() for p in [*apps_host.apphost_sources(), *apps_host.lua_sources()]]
        seen = set()
        while pending:
            path = pending.pop()
            if path in seen:
                continue
            seen.add(path)
            self.assertIn(path, covered, f"{path.relative_to(ROOT)} goes into apphost and not into its cache key")
            # What it includes from the build's include directories (a device-only include, esp_heap_caps.h in
            # apps_pkg.c, is behind #ifdef ESP_PLATFORM and in none of them).
            for name in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(errors="replace"), re.M):
                for base in (path.parent, APPS, ROOT / "tools/muse/apphost", ROOT / "components/lua/src"):
                    if (base / name).is_file():
                        pending.append((base / name).resolve())
                        break

    def test_the_cache_is_a_directory_named_by_the_key_under_home(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}):
            self.assertEqual(self.build.cache_root(), Path(home) / ".cache" / "muse-apphost")
            path = self.build.cache_path(["cc"], ROOT)
        self.assertEqual((path.name, path.parent.name), ("apphost", self.build.cache_key(["cc"], ROOT)))
        self.assertEqual(path.parent.parent, Path(home) / ".cache" / "muse-apphost")

    def test_a_build_lands_in_the_cache_once(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}):
            exe = self.build.ensure()
            self.assertEqual(exe, self.build.cache_path(self.build.compiler(), ROOT))
            self.assertTrue(os.access(exe, os.X_OK))
            self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["apphost"], "no objects are kept")
            before = exe.stat().st_mtime_ns
            with mock.patch.object(self.build.subprocess, "run", side_effect=AssertionError("built again")):
                self.assertEqual(self.build.ensure(), exe)
            self.assertEqual(exe.stat().st_mtime_ns, before)
            self.assertEqual([p.name for p in exe.parent.parent.iterdir()], [exe.parent.name],
                             "no build directories are left behind")

    def test_an_entry_that_is_no_executable_is_replaced(self):
        build = self.build

        def fake_build(dest, cc=None, strict=False, root=ROOT):
            (Path(dest) / "obj").mkdir(parents=True, exist_ok=True)
            exe = Path(dest) / "apphost"
            exe.write_text("#!/bin/sh\n")
            exe.chmod(0o755)
            return exe

        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}), \
                mock.patch.object(build, "build", side_effect=fake_build) as made:
            entry = build.cache_path(["cc"], ROOT)
            entry.parent.mkdir(parents=True)
            entry.write_text("a copy that lost its mode bits")
            entry.chmod(0o644)
            (entry.parent / "stray").write_text("x")
            exe = build.ensure(["cc"])
            self.assertEqual(exe, entry)
            self.assertTrue(os.access(exe, os.X_OK), "the broken entry was no apphost, and is not handed back")
            self.assertEqual(made.call_count, 1)
            self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), ["apphost"])
            # A directory by that name is no apphost either (it passes an execute check).
            shutil.rmtree(exe.parent)
            (exe.parent / "apphost").mkdir(parents=True)
            self.assertTrue(os.access(exe, os.X_OK))
            self.assertTrue(os.path.isfile(build.ensure(["cc"])))
            self.assertEqual(made.call_count, 2)

    def test_two_builds_that_race_end_with_one(self):
        build = self.build

        def fake_build(dest, cc=None, strict=False, root=ROOT):
            (Path(dest) / "obj").mkdir(parents=True, exist_ok=True)
            (Path(dest) / "apphost").write_text("mine")
            (Path(dest) / "apphost").chmod(0o755)
            return Path(dest) / "apphost"

        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}), \
                mock.patch.object(build, "build", side_effect=fake_build):
            entry = build.cache_path(["cc"], ROOT)
            real_rename = os.rename

            def other_got_there_first(src, dst):
                entry.parent.mkdir(parents=True)   # the other build's move
                (entry.parent / "apphost").write_text("theirs")
                (entry.parent / "apphost").chmod(0o755)
                raise OSError(66, "Directory not empty")

            with mock.patch.object(build.os, "rename", side_effect=other_got_there_first):
                exe = build.ensure(["cc"])
            self.assertEqual((exe, exe.read_text()), (entry, "theirs"), "theirs stays, and ours is no error")
            self.assertEqual([p.name for p in build.cache_root().iterdir()], [entry.parent.name], "no build left over")
            self.assertIs(real_rename, os.rename)

    def test_a_move_that_fails_is_a_build_error_in_words(self):
        build = self.build

        def fake_build(dest, cc=None, strict=False, root=ROOT):
            (Path(dest) / "obj").mkdir(parents=True, exist_ok=True)
            (Path(dest) / "apphost").write_text("x")
            return Path(dest) / "apphost"

        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home}), \
                mock.patch.object(build, "build", side_effect=fake_build), \
                mock.patch.object(build.os, "rename", side_effect=OSError(13, "Permission denied")):
            with self.assertRaises(build.BuildError) as raised:
                build.ensure(["cc"])
            self.assertIn("Permission denied", str(raised.exception))
            self.assertEqual([p.name for p in build.cache_root().iterdir()], [], "nothing is left behind")

    def test_a_compile_error_says_what_failed_and_leaves_no_half_build(self):
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, {"HOME": home, "CC": "false"}):
            with self.assertRaises(self.build.BuildError) as raised:
                self.build.ensure()
            self.assertIn("false", str(raised.exception))
            root = Path(home) / ".cache" / "muse-apphost"
            self.assertEqual(list(root.iterdir()) if root.exists() else [], [])

    def test_a_dot_file_or_a_dangling_link_beside_the_headers_is_no_key_input(self):
        # H: pathlib's `*` takes dot-files: Emacs's lock file `.#apps_vm.h` is a dangling link, and reading it failed the
        # key (and so `app.py check`) for as long as the buffer was unsaved. The compiler never includes a dot-file.
        build = self.build
        with tempfile.TemporaryDirectory() as tmp:
            tree = self.copy_tree(tmp)
            key = build.cache_key(["cc"], tree)
            apps = tree / "components/muse/apps"
            os.symlink("zero@host.1234:5678", apps / ".#apps_vm.h")            # an Emacs lock file: dangling
            (apps / "._apps_rt.h").write_bytes(b"AppleDouble")                  # a regular hidden header
            os.symlink("apps_rt.h", apps / ".linked.h")                         # a link to a real one, hidden
            (apps / "dir.h").mkdir()                                            # not a file
            os.symlink("no-such-file.h", apps / "dangling.h")                   # dangling, and not hidden
            names = [p.name for p in build.key_inputs(tree)]
            for name in (".#apps_vm.h", "._apps_rt.h", ".linked.h", "dir.h", "dangling.h"):
                self.assertNotIn(name, names)
            self.assertEqual(build.cache_key(["cc"], tree), key, "none of them is part of the build")
            (apps / "visible.h").write_bytes(b"/* a header */")
            self.assertNotEqual(build.cache_key(["cc"], tree), key, "an ordinary new header is")

    def test_the_strict_build_passes_with_the_host_compiler(self):
        # O: `app.py check`'s own flags, as the manual zig run and CI build them (-Werror), with the compiler of this
        # machine: a call that wants a feature macro under -std=c11, or a warning the test build's flags hide, fails here
        # and not on someone's Linux.
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build.build(tmp, strict=True)
            self.assertTrue(os.access(exe, os.X_OK))
            done = subprocess.run([str(exe), str(HELLO)], capture_output=True, text=True, timeout=60)
            self.assertEqual((done.returncode, done.stderr), (0, ""))
            self.assertIn('"event":"started"', done.stdout)

    def test_a_change_to_the_build_recipe_is_another_key(self):
        # R: the include list and the order of the commands are in apphost_build.py, not in the flags the key lists:
        # a define or an include directory added there changes the binary, so it must change the key.
        build = self.build
        recipe = Path(build.__file__).read_bytes()
        with tempfile.TemporaryDirectory() as tmp:
            key = build.cache_key(["cc"], ROOT)
            copy = Path(tmp) / "apphost_build.py"
            copy.write_bytes(recipe)
            with mock.patch.object(build, "__file__", str(copy)):
                self.assertEqual(build.cache_key(["cc"], ROOT), key, "a copy of the recipe is the same recipe")
                copy.write_bytes(recipe + b"\n# a new -I, a new define\n")
                longer = build.cache_key(["cc"], ROOT)
                self.assertNotEqual(longer, key)
                # The same length and one byte different (a -I swapped for a -D, say): the text is in the key, not only how
                # long it is.
                copy.write_bytes(recipe[:-2] + bytes([recipe[-2] ^ 1]) + recipe[-1:])
                self.assertNotIn(build.cache_key(["cc"], ROOT), (key, longer))


class RuleTest(unittest.TestCase):
    """app.py checks an id and a path before it sends anything (PATH_RULE, ID_RULE), so that the board's refusal is the
    tool's. They are two more copies of the C's rule (apps_id_valid and apps_path_valid), and are held to it here: the
    same texts go to the pkg harness's own `check id` and `check path`, which run the real functions (L)."""

    @classmethod
    def setUpClass(cls):
        cls.app = load_app()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        import test_muse_apps_pkg as pkg
        cls.harness = apps_host.build("muse_apps_pkg_harness", [*pkg.SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=pkg.INCLUDES, defines=["ESP_PLATFORM"])

    def board_says(self, what, texts):
        done = apps_host.run(self.harness, "check", what, input="".join(f"{t}\n" for t in texts).encode(), text=False,
                             timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)
        answers = done.stdout.decode().split("\n")[:-1]
        self.assertEqual(len(answers), len(texts))
        return [answer == "1" for answer in answers]

    def agree(self, what, rule, texts):
        texts = [t for t in dict.fromkeys(texts) if "\n" not in t and "\0" not in t]   # the harness reads a line a text
        board = self.board_says(what, texts)
        tool = [rule.fullmatch(t) is not None for t in texts]
        wrong = [(t, b) for t, b, m in zip(texts, board, tool) if b != m]
        self.assertEqual(wrong, [], f"{len(wrong)} of {len(texts)} {what}s: the board's verdict is the second")
        self.assertGreater(sum(board), 50, f"{what}s that are valid are in the set")
        self.assertGreater(len(board) - sum(board), 50, f"{what}s that aren't are in the set")

    @staticmethod
    def exhaustive(alphabet, longest):
        found = [""]
        last = [""]
        for _ in range(longest):
            last = [text + c for text in last for c in alphabet]
            found += last
        return found

    def test_ids_are_the_boards(self):
        import random
        rng = random.Random(0x1D)
        alphabet = "abzAZ09_-. /é\t"
        texts = [*PackageTest.IDS_OK, *PackageTest.IDS_BAD, *self.exhaustive("aZ0_-. é", 4)]
        for length in (0, 1, 2, 3, 23, 24, 25, 26, 64):
            texts += ["".join(rng.choice("abcxyz09_") for _ in range(length)) for _ in range(40)]
            texts += ["".join(rng.choice(alphabet) for _ in range(length)) for _ in range(300)]
        self.agree("id", self.app.ID_RULE, texts)

    def test_paths_are_the_boards(self):
        import random
        rng = random.Random(0x9A7)
        pieces = ["a", "b.c", ".", "..", "", "-", "_", "x.lua", ".x", "a.", "a..b", "A", "a b", "é", "0", "a-b_c.d",
                  "lib", ".hidden", "x.", "a:b", "~1", "a" * 30]
        texts = [*PackageTest.PATHS_OK, *PackageTest.PATHS_BAD, *self.exhaustive("a0_-./A é", 4)]
        for _ in range(4000):
            texts.append("/".join(rng.choice(pieces) for _ in range(rng.randint(1, 5))))
        for length in (1, 2, 62, 63, 64, 65, 66, 100):   # the length limit, whatever is in the text
            texts += ["".join(rng.choice("ab./-_") for _ in range(length)) for _ in range(200)]
            texts += [("a" * (length - 4) + ".lua") if length > 4 else "a" * length]
            texts += [("a/" * (length // 2))[:length], "a" * length]
        self.agree("path", self.app.PATH_RULE, texts)


class CheckTest(unittest.TestCase):
    @staticmethod
    def say_sanitizers():
        """X13: the other apphost builds of this suite say which sanitizers they have (apps_host); this one is the dev
        loop's own, which the tests below run their scratch apps on, and has none."""
        print("apps_host: apphost (app.py check's build): sanitizers off, the dev loop's own build", file=sys.stderr,
              flush=True)

    @classmethod
    def setUpClass(cls):
        cls.say_sanitizers()
        cls.app = load_app()
        home = tempfile.TemporaryDirectory()
        cls.addClassCleanup(home.cleanup)
        cls.home = home.name
        env = mock.patch.dict(os.environ, {"HOME": cls.home})
        env.start()
        cls.addClassCleanup(env.stop)
        cls.app.apphost_build.ensure()

    def check(self, folder, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
                mock.patch.object(self.app, "open_board", side_effect=AssertionError("check needs no gadget")):
            code = self.app.main(["check", str(folder), *args])
        return code, out.getvalue(), err.getvalue()

    def make(self, main, script=None, manifest=None, files=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        folder = make_package(tmp.name, app_id="scratch", main=main, manifest=manifest, files=files)
        path = folder.parent / "script.txt"
        if script is not None:
            path.write_text(script)
        return folder, path

    @staticmethod
    def kinds(out):
        return [json.loads(line)["event"] for line in out.split("\n")[:-1]]

    def test_check_runs_hello(self):
        # The way someone runs it: the script, in a process of its own, from the checkout.
        env = {**os.environ, "HOME": self.home}
        done = subprocess.run([sys.executable, str(APP_PY), "check", "components/muse/apps/examples/hello"], cwd=ROOT,
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("started", done.stdout)
        self.assertEqual(self.kinds(done.stdout), ["started"], "no warning either")
        self.assertEqual(done.stderr, "")

    def test_a_clean_app_exits_zero_and_prints_apphosts_events(self):
        folder, _ = self.make("return {start = function() print('hi') end}")
        code, out, err = self.check(folder)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.kinds(out), ["log", "started"])

    def test_a_crash_exits_one(self):
        folder, _ = self.make("return {start = function() local x; x.y = 1 end}")
        code, out, _ = self.check(folder)
        self.assertEqual(code, 1)
        self.assertIn("crash", self.kinds(out))

    def refusal(self, folder, *args):
        """check exits 1 on a package whose Lua doesn't compile, with that one crash event and nothing run. -> its
        message."""
        code, out, err = self.check(folder, *args)
        self.assertEqual((code, err, self.kinds(out)), (1, "", ["crash"]))
        return json.loads(out)["message"]

    def test_a_module_nobody_requires_is_compiled_as_the_install_compiles_it(self):
        # The install compiles every .lua file: a syntax error in util.lua, which main.lua never requires, is refused
        # there, so check refuses it, in the install's words. main.lua printed when it loaded: nothing ran.
        folder, script = self.make("print('main ran')\nreturn {start = function() print('started') end}",
                                   script="state\nmessage x\n",
                                   files={"util.lua": "local a = 1\nlocal b = = 2\nreturn {}\n"})
        self.assertEqual(self.refusal(folder, "--script", str(script)),
                         "compile failed: util.lua:2: unexpected symbol near '='\n 1| local a = 1\n>2| local b = = 2\n"
                         " 3| return {}")

    def test_a_module_nested_38_deep_is_refused_as_the_install_refuses_it_and_37_is_not(self):
        # Each file is compiled as a top-level chunk (LUAI_MAXCCALLS is 40: a level for the statement, one for the
        # expression, one a parenthesis): 37 deep compiles and 38 is "C stack overflow".
        for depth, accepted in ((37, True), (38, False)):
            with self.subTest(depth=depth):
                nest = "return " + "(" * depth + "1" + ")" * depth + "\n"
                folder, _ = self.make("return {}", files={"deep.lua": nest})
                if accepted:
                    code, out, err = self.check(folder)
                    self.assertEqual((code, err, self.kinds(out)), (0, "", ["started"]))
                else:
                    self.assertEqual(self.refusal(folder), "compile failed: C stack overflow")

    def test_a_syntax_error_in_main_lua_is_refused_in_the_installs_words(self):
        folder, _ = self.make("local x = 1\nx = = 2\n")
        self.assertEqual(self.refusal(folder),
                         "compile failed: main.lua:2: unexpected symbol near '='\n 1| local x = 1\n>2| x = = 2")

    def test_a_crash_with_unicode_line_separators_in_it_is_still_a_crash(self):
        # The event is one line of JSON with U+2028 in it as it is. (apphost's own status is 1 for a crash, so the exit
        # status says nothing of how the line was read: this one shows it comes through whole, and the test for an `error`
        # event, whose status is 0, is the one that lets the tool's reading of the line decide the exit status.)
        folder, _ = self.make("return {start = function() error('a\\u{2028}b\\u{2029}c', 0) end}")
        code, out, _ = self.check(folder)
        self.assertEqual(code, 1)
        (crash,) = [json.loads(line) for line in out.split("\n")[:-1] if '"crash"' in line]
        self.assertIn("a b c", crash["message"])

    def refused_before_apphost(self, folder, *args):
        """check on a folder it refuses on its own: exit 1, a word on stderr, nothing on stdout, and apphost never
        started. -> stderr."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), no_longer_than(30), \
                mock.patch.object(self.app.subprocess, "Popen", side_effect=AssertionError("apphost was started")), \
                mock.patch.object(self.app, "open_board", side_effect=AssertionError("check needs no gadget")):
            code = self.app.main(["check", str(folder), *args])
        self.assertEqual((code, out.getvalue()), (1, ""))
        return err.getvalue()

    def test_a_file_that_push_refuses_is_refused_by_check_before_apphost_starts(self):
        # C: one rule for both. A link out of the folder is no way to read a file of someone else's (its lines would be
        # quoted by the compile's message), and a FIFO listed in a manifest is no reason for check to wait for ever.
        secret = Path(tempfile.mkdtemp(dir=self.home)) / "id_secret.lua"
        secret.write_text("SECRET_LINE_ONE\nlocal b = = 2\n")
        for what, make, words in (
                ("a link out", lambda path: os.symlink(secret, path), "outside the app's folder"),
                ("a FIFO", os.mkfifo, "regular file"),
                ("a folder", os.mkdir, "regular file")):
            with self.subTest(what):
                folder, _ = self.make("return {}", manifest={"files": ["notes.lua"]})
                make(folder / "notes.lua")
                err = self.refused_before_apphost(folder)
                self.assertIn(words, err)
                self.assertIn("notes.lua", err)
                self.assertNotIn("SECRET_LINE", err)
        folder, _ = self.make("return {}")
        (folder / "manifest.json").unlink()
        os.mkfifo(folder / "manifest.json")
        self.assertIn("regular file", self.refused_before_apphost(folder))

    def test_check_and_push_agree_on_which_folders_are_acceptable(self):
        # The same package, the same verdict: what push says before it sends anything is what check says before it runs.
        cases = {
            "an id the board refuses": dict(manifest={"id": "Bad"}),
            "a path the board refuses": dict(manifest={"files": ["../x.lua"]}),
            "a file nobody made": dict(manifest={"files": ["missing.lua"]}),
            "a file with no kind": dict(files={"notes.txt": "x"}),
            "a file over its limit": dict(files={"big.lua": b" " * (256 * 1024 + 1)}),
        }
        for what, kw in cases.items():
            with self.subTest(what):
                folder, _ = self.make("return {}", **kw)
                with self.assertRaises(self.app.AppError) as raised:
                    self.app.read_package(folder)
                err = self.refused_before_apphost(folder)
                self.assertEqual(err, f"app.py: {raised.exception}\n")

    def test_what_apphost_says_is_printed_without_a_control_character_a_terminal_would_act_on(self):
        # B: an app's data round-trips (a reply), so U+009B (CSI) in it reaches the line apphost prints, and DEL.
        folder, script = self.make('return {message = function() return "\\u{9B}2J\\u{7F}" end}', script="message x\n")
        code, out, err = self.check(folder, "--script", str(script))
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(re.findall("[\x00-\x08\x0b-\x1f\x7f-\x9f]", out), [], repr(out))
        (reply,) = [json.loads(line) for line in out.split("\n")[:-1] if '"reply"' in line]
        self.assertEqual(reply["data"], "\u009b2J\u007f", "the same JSON")
        self.assertIn('{"event":"reply","data":"\\u009b2J\\u007f"}', out)
        # apphost's stderr (its usage, a script it can't read) is text for a person.
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "apphost"
            fake.write_text("#!/bin/sh\nprintf 'apphost: \\033]0;owned\\007 \\302\\233 2J\\n' >&2\nexit 2\n")
            fake.chmod(0o755)
            with mock.patch.object(self.app.apphost_build, "ensure", return_value=fake):
                code, out, err = self.check(folder)
        self.assertEqual((code, out), (2, ""))
        self.assertEqual(err, "apphost: \\x1b]0;owned\\x07 \\x9b 2J\n")

    def test_an_error_event_with_unicode_line_separators_decides_the_exit_status(self):
        # N: a message the app refuses is an `error` event and apphost's own status is 0, so the exit status is the tool's
        # alone, and it comes from reading the event: its line has U+2028 and U+2029 in it as they are. Split at
        # str.splitlines() the line is three that aren't JSON, no event is seen, and check says all is well.
        folder, script = self.make("return {message = function() error('a\\u{2028}b\\u{2029}c', 0) end}",
                                   script="message x\n")
        code, out, err = self.check(folder, "--script", str(script))
        self.assertEqual((code, err, self.kinds(out)), (1, "", ["started", "error"]))
        self.assertEqual(json.loads(out.split("\n")[1])["message"], "a\u2028b\u2029c")
        self.assertEqual(out.count("\n"), 2, "two lines of events, however many the text would split into")

    def test_the_notice_that_the_dev_loops_apphost_has_no_sanitizers_is_given(self):
        # X13: CheckTest's own scratch apps (the 38-deep and the crash ones) run on the unsanitized binary the dev loop
        # builds, and the output says so.
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            CheckTest.say_sanitizers()
        self.assertEqual(err.getvalue(), "apps_host: apphost (app.py check's build): sanitizers off, the dev loop's own build\n")

    def test_a_package_that_cannot_load_is_an_error_event_and_exits_one(self):
        # What the tool's own look at the folder can't see: a .json file that isn't JSON, an image that isn't one, a
        # manifest apphost reads further (its api) - those are apphost's `error` events, in the install's words. (A file that
        # is missing, or not a file at all, or outside the folder, is the tool's to say before it starts apphost, as push's.)
        for files, manifest, words in (({"d.json": "{\"a\":}"}, None, "d.json: unexpected '}' at byte 6"),
                                       ({"i.bin": b"not an image"}, None, "i.bin: image: not an LVGL v9 image file"),
                                       (None, {"api": 2}, "manifest: this gadget supports api 1")):
            with self.subTest(words):
                folder, _ = self.make("return {}", files=files, manifest=manifest)
                code, out, _ = self.check(folder)
                self.assertEqual((code, self.kinds(out)), (1, ["error"]))
                self.assertIn(words, json.loads(out)["message"])

    def test_a_folder_that_is_not_there_or_lacks_a_file_is_said_on_stderr_as_push_says_it(self):
        folder, _ = self.make("return {}", manifest={"files": ["missing.lua"]})
        code, out, err = self.check(folder)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("missing.lua is listed in", err)
        code, out, err = self.check(Path(tempfile.gettempdir()) / "no-such-muse-app-folder")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("no-such-muse-app-folder", err)
        self.assertIn("manifest.json", err)

    def test_a_failed_command_of_the_script_exits_one_though_the_app_lives(self):
        # apphost's own status is 0 for a message the app refuses (the app didn't crash): the error event is the tool's.
        folder, script = self.make("return {message = function(e) if e == 'bad' then error('no') end return 1 end}",
                                   script="message good\nmessage bad\nmessage good\n")
        code, out, _ = self.check(folder, "--script", str(script))
        self.assertEqual((code, self.kinds(out)), (1, ["started", "reply", "error", "reply"]))
        folder, script = self.make("return {message = function(e) return 1 end}", script="message a\nmessage b\n")
        code, out, _ = self.check(folder, "--script", str(script))
        self.assertEqual((code, self.kinds(out)), (0, ["started", "reply", "reply"]))

    def test_an_error_or_a_crash_event_ends_in_one_whatever_the_runners_own_status(self):
        folder, _ = self.make("return {}")
        for event, want in (("started", 0), ("log", 0), ("warning", 0), ("error", 1), ("crash", 1)):
            with self.subTest(event), tempfile.TemporaryDirectory() as tmp:
                fake = Path(tmp) / "apphost"
                fake.write_text(f"#!/bin/sh\nprintf '%s\\n' '{{\"event\":\"{event}\",\"message\":\"x\"}}'\nexit 0\n")
                fake.chmod(0o755)
                with mock.patch.object(self.app.apphost_build, "ensure", return_value=fake):
                    code, out, _ = self.check(folder)
                self.assertEqual((code, self.kinds(out)), (want, [event]))

    def test_the_runners_own_failure_status_is_passed_on(self):
        folder, _ = self.make("return {}")
        for status, want in ((3, 3), (1, 1), (2, 2)):
            with self.subTest(status), tempfile.TemporaryDirectory() as tmp:
                fake = Path(tmp) / "apphost"
                fake.write_text(f"#!/bin/sh\nexit {status}\n")
                fake.chmod(0o755)
                with mock.patch.object(self.app.apphost_build, "ensure", return_value=fake):
                    self.assertEqual(self.check(folder)[0], want)
        with tempfile.TemporaryDirectory() as tmp:   # killed by a signal: no status of its own, still not a pass
            fake = Path(tmp) / "apphost"
            fake.write_text("#!/bin/sh\nkill -9 $$\n")
            fake.chmod(0o755)
            with mock.patch.object(self.app.apphost_build, "ensure", return_value=fake):
                self.assertEqual(self.check(folder)[0], 1)

    def test_a_script_apphost_cannot_read_is_a_usage_error_on_stderr(self):
        folder, _ = self.make("return {}")
        code, out, err = self.check(folder, "--script", "/no/such/script.txt")
        self.assertNotEqual(code, 0)
        self.assertIn("script", err)

    def test_check_never_imports_pyserial(self):
        self.assertEqual(re.findall(r"^\s*(?:import|from)\s+serial\b", APP_PY.read_text(), re.M), [],
                         "check needs no gadget, so nothing at the top of the tool may need pyserial")

    def test_a_build_that_fails_is_reported_on_stderr(self):
        folder, _ = self.make("return {}")
        failing = self.app.apphost_build.BuildError("cc: boom")
        with mock.patch.object(self.app.apphost_build, "ensure", side_effect=failing):
            code, out, err = self.check(folder)
        self.assertEqual((code, out), (1, ""))
        self.assertIn("cc: boom", err)


# ---- The apps that ship with the tool ----

class AppsTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.apphost = apps_host.build_apphost(tmp.name)

    def run_app(self, folder, script=()):
        text = "".join(f"{line}\n" for line in script)
        done = apps_host.run(self.apphost, "--script", "-", folder, input=text.encode(), text=False, timeout=120)
        shown = done.stdout.decode(errors="replace") + done.stderr.decode(errors="replace")
        self.assertEqual(done.returncode, 0, shown)
        self.assertEqual(done.stderr, b"")
        return [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]

    @staticmethod
    def of(events, kind):
        return [e for e in events if e["event"] == kind]


class HelloTest(AppsTestCase):
    def test_hello_is_what_the_brief_says(self):
        source = (HELLO / "main.lua").read_text()
        self.assertEqual(source.splitlines()[:2], LUA_HEADER)
        self.assertIn('ui.bg("#000000")', source)
        self.assertRegex(source, r"ui\.arc\{[^}]*\bw\s*=\s*400\b[^}]*\bh\s*=\s*400\b[^}]*\bvalue\s*=\s*60\b")
        self.assertRegex(source, r'ui\.label\{[^}]*"12:34"[^}]*"digits96"|ui\.label\{[^}]*"digits96"[^}]*"12:34"')
        self.assertRegex(source, r'ui\.label\{[^}]*"Hello from Muse"[^}]*"sans20"|'
                                 r'ui\.label\{[^}]*"sans20"[^}]*"Hello from Muse"')
        self.assertRegex(source, r'ui\.button\{[^}]*"Tap me"')
        self.assertIn('"Tapped"', source)
        manifest = json.loads((HELLO / "manifest.json").read_text())
        self.assertEqual((manifest["id"], manifest["api"], HELLO.name), ("hello", 1, "hello"))
        self.assertLessEqual(len(manifest["name"]), 24)

    def test_hello_starts_clean_and_draws_what_it_says(self):
        events = self.run_app(HELLO, ["dump"])
        self.assertEqual([e["event"] for e in events], ["started", "dump"], "no warning, no log, no crash")
        arc, clock, greeting, button = events[1]["widgets"]
        self.assertEqual([w["kind"] for w in (arc, clock, greeting, button)], ["arc", "label", "label", "button"])
        self.assertEqual((arc["w"], arc["h"]), (400, 400))
        self.assertEqual((clock["text"], greeting["text"], button["text"]), ("12:34", "Hello from Muse", "Tap me"))

    def test_the_button_prints_and_the_label_says_tapped(self):
        events = self.run_app(HELLO, ['tap "Tap me"', "dump"])
        self.assertEqual([e["line"] for e in self.of(events, "log")], ["tapped"], "a tap prints one line, and says so")
        labels = [w["text"] for w in self.of(events, "dump")[0]["widgets"] if w["kind"] == "label"]
        self.assertEqual(labels, ["12:34", "Tapped"])


class TortureTest(AppsTestCase):
    NAMES = ("pcall", "resume", "index", "tostring", "xpcall")
    # The C-call levels each chain reaches, exactly: Lua's own count (LUAI_MAXCCALLS is 40), the same on the host as on
    # the board, which is what lets the board's stack readings stand for the deepest the sandbox goes. xpcall's is the
    # __index chain's, with the message handler running at the limit.
    DEPTHS = {"pcall": 37, "resume": 37, "index": 36, "tostring": 36, "xpcall": 36}
    NESTING = 34   # the deepest of deep26..deep36 a require loads: a top-level chunk takes 37, a require a few levels less
    # How far the handler gets under xpcall: it is called at level 41, guarded_handler's call of it is 42, and each nested
    # pcall one more, to 43 (Lua stops at 44, LUAI_MAXCCALLS / 10 * 11): its own frame and one nest, and the next is refused.
    HANDLER = {"levels": 2, "refused": "error in error handling"}

    def test_the_fixture_is_what_the_rulings_say(self):
        manifest = json.loads((TORTURE / "manifest.json").read_text())
        self.assertEqual((manifest["id"], manifest["api"]), ("torture", 1))
        # C24: a file of each depth, 26 to 36 (a require takes 34 at most; the install compiles every file as a
        # top-level chunk, which takes 37 levels and not 38: LUAI_MAXCCALLS is 40).
        self.assertEqual(sorted(manifest["files"]), sorted([*(f"deep{n}.lua" for n in range(26, 37)), "tile.bin"]))
        self.assertEqual(sorted(p.name for p in TORTURE.glob("deep*.lua")),
                         sorted(f"deep{n}.lua" for n in range(26, 37)))
        for n in range(26, 37):
            text = (TORTURE / f"deep{n}.lua").read_text()
            self.assertEqual(text.splitlines()[:2], LUA_HEADER)
            body = [line for line in text.splitlines() if line.strip() and not line.startswith("--")]
            self.assertEqual(body, ["return " + "(" * n + "1" + ")" * n], f"deep{n}.lua nests {n} deep")
        self.assertEqual((TORTURE / "main.lua").read_text().splitlines()[:2], LUA_HEADER)
        self.assertEqual(sorted(manifest["messages"]), sorted(
            ["chains", "json", "xpcall", "floats", "floats_exact", "floats_time", "widgets", "hits"]))

    def test_start_runs_the_two_cases_and_the_chains(self):
        events = self.run_app(TORTURE)
        self.assertEqual(events[-1]["event"], "started")
        self.assertEqual(self.of(events, "warning"), [])
        logs = [e["line"].split("\t") for e in self.of(events, "log")]
        (index,) = [row for row in logs if row[0] == "index recursion"]
        self.assertEqual(index[1], "false")
        self.assertTrue(index[2].endswith("C stack overflow"), index)
        (deepest,) = [row for row in logs if row[0] == "deepest nesting that loads"]
        self.assertEqual(int(deepest[1]), self.NESTING, deepest)
        chains = {row[1]: (int(row[2]), row[3]) for row in logs if row[0] == "chain"}
        self.assertEqual(sorted(chains), sorted(self.NAMES))
        for name, (depth, err) in chains.items():
            self.assertEqual(depth, self.DEPTHS[name], name)
            self.assertEqual(err, "C stack overflow", name)

    def test_the_chains_reach_the_limit_and_json_runs_at_the_bottom_of_each(self):
        events = self.run_app(TORTURE, ["message chains", "message json"])
        plain, with_json = [e["data"] for e in self.of(events, "reply")]
        for name in self.NAMES:
            with self.subTest(name):
                self.assertEqual(plain[name]["depth"], self.DEPTHS[name])
                self.assertEqual(plain[name]["err"], "C stack overflow")
                self.assertNotIn("json", plain[name])
                self.assertEqual(with_json[name]["depth"], plain[name]["depth"], "the same depth, with work there")
                leaf = with_json[name]["json"]
                self.assertEqual(leaf["decoded_levels"], 16)
                self.assertGreater(leaf["encoded"], 100)
                self.assertEqual(leaf["tail"], 0.1 + 0.2, "the 17-digit float at the deepest point reads back")

    def test_the_handler_of_an_xpcall_at_the_limit_runs_on_to_the_end_of_error_handling_with_json_there(self):
        # U: the one reachable part of the C-call budget the other chains don't run, levels 41 to 44, where the message
        # handler sits on every frame of the chain it was raised in. It nests pcall until Lua refuses with "error in error
        # handling", and json.encode and json.decode run at the deepest level it reached.
        events = self.run_app(TORTURE, ["message xpcall", "message json"])
        alone, everything = [e["data"] for e in self.of(events, "reply")]
        self.assertEqual({k: alone[k] for k in ("depth", "err", "handler")},
                         {"depth": self.DEPTHS["xpcall"], "err": "C stack overflow", "handler": self.HANDLER})
        self.assertEqual(alone["json"]["decoded_levels"], 16)
        self.assertEqual(alone["json"]["tail"], 0.1 + 0.2)
        self.assertGreater(alone["json"]["encoded"], 100)
        self.assertEqual(everything["xpcall"], alone, "the same in the run of every chain")
        # No chain but this one has a handler: it is the only one that goes past 40.
        self.assertEqual([name for name in self.NAMES if "handler" in everything[name]], ["xpcall"])

    def test_floats_print_in_the_fewest_digits_that_read_back_and_round_trip(self):
        events = self.run_app(TORTURE, ["message floats"])
        result = self.of(events, "reply")[0]["data"]
        self.assertTrue(result["ok"] and result["integer_ok"] and result["round_trip"])
        values = (0.1, 5e-324, 1e100, -1e-07)
        want = [apps_host.json_float_text(v) for v in values]
        self.assertEqual(want, ["0.1", "4.94065645841247e-324", "1e+100", "-1e-07"])
        self.assertEqual([c["expected"] for c in result["cases"]], want)
        self.assertEqual([c["encoded"] for c in result["cases"]], want, "a host's printf: the board's has 5e-324")
        # The board's text for the subnormal is the other one; both decode to the double.
        self.assertEqual([c.get("other") for c in result["cases"]], [None, "5e-324", None, None])
        self.assertTrue(all(c["encode_ok"] and c["decode_ok"] and c["round_trip_ok"] for c in result["cases"]))

    def test_a_thousand_floats_come_back_as_themselves(self):
        events = self.run_app(TORTURE, ["message floats_exact"])
        self.assertEqual(self.of(events, "reply")[0]["data"], {"count": 1000, "same": 1000})

    def test_the_thousand_floats_are_there_to_time_in_two_sets(self):
        events = self.run_app(TORTURE, ['message floats_time {"n": 2}', 'message floats_time {"n": 1, "decode": true}',
                                        'message floats_time {"n": 1, "digits": 15}',
                                        'message floats_time {"n": 1, "digits": 15, "decode": true}',
                                        'message floats_time {"n": 1, "scale": 1e300}',
                                        'message floats_time {"n": 1, "scale": 1e-300, "decode": true}'])
        encode, decode, short_encode, short_decode, big, tiny = [e["data"] for e in self.of(events, "reply")]
        self.assertGreater(big["bytes"], 22 * 1000, "17 digits and an exponent of e+300 each")
        self.assertEqual((tiny["count"], tiny["decode"]), (1000, True))
        self.assertEqual((encode["n"], encode["decode"], decode["n"], decode["decode"]), (2, False, 1, True))
        self.assertEqual((decode["count"], short_decode["count"]), (1000, 1000))
        self.assertGreater(encode["bytes"], 19 * 1000, "the default set takes 17 digits, a point and a comma each")
        self.assertLess(short_encode["bytes"], 7 * 1000, "i / 10 takes at most 6 bytes with its comma")
        self.assertEqual(decode["bytes"], encode["bytes"])
        self.assertEqual(short_decode["bytes"], short_encode["bytes"])

    def test_the_screen_is_128_widgets_of_every_kind_and_the_deep_tap_reaches_the_app(self):
        events = self.run_app(TORTURE, ["message widgets", "dump", 'tap "deepest"', "message hits"])
        built = self.of(events, "reply")[0]["data"]
        self.assertEqual(built["count"], 128)
        self.assertEqual(built["tap"], {"x": 233, "y": 303})
        widgets = self.of(events, "dump")[0]["widgets"]
        self.assertEqual(len(widgets), 128)
        self.assertEqual({w["kind"] for w in widgets}, {"label", "button", "box", "arc", "bar", "image"})
        self.assertEqual([w["kind"] for w in widgets[:6]], ["box", "box", "box", "box", "button", "box"])
        self.assertEqual(built["deepest"], widgets[4]["id"])
        # The dump has no parent field. The boxes nest as deep as a widget may (four box ancestors: the button, and the
        # fifth box beside it), which is known from the runtime's refusal of a box in that fifth one; a widget that was
        # made without its parent would take it.
        self.assertEqual(c_define("apps_ui.h", "APPS_UI_BOX_DEPTH_MAX"), 4)
        self.assertEqual(built["nest"], {"ok": False, "err": "ui.box: boxes nest at most 4 deep"})
        text_max = c_define("apps_ui.h", "APPS_UI_TEXT_MAX")
        self.assertEqual(max(len(w["text"] or "") for w in widgets), text_max, "a text as long as text gets")
        hits = self.of(events, "reply")[1]["data"]
        self.assertEqual((hits["count"], hits["last"]), (1, f"{built['deepest']} tap"))
        again = self.run_app(TORTURE, ["message widgets", "message widgets"])
        self.assertEqual([e["data"]["count"] for e in self.of(again, "reply")], [128, 128],
                         "a second round starts over, it doesn't run out of slots")


if __name__ == "__main__":
    unittest.main()
