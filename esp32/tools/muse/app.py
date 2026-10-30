#!/usr/bin/env python3
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

"""Develop a Muse app from a laptop: check it with no gadget, push it, run it, snapshot it, watch it, follow its log.

    app.py check DIR [--script FILE]                run the app here, on the gadget's runtime, and say what it did
    app.py push PORT DIR [--run] [--snap OUT.png]   install the app on the gadget; --run starts it, --snap saves a PNG
    app.py watch PORT DIR                           push with --run now, and again on every change to DIR
    app.py logs PORT                                print what the app prints (the console's @app.log lines)

DIR is the app's folder: manifest.json, main.lua (or the manifest's "main") and the files the manifest lists. PORT is
the gadget's USB serial port (/dev/cu.usbmodem101): `tools/muse/ports.py s3` finds it. The firmware has to be built
with MUSE_APPS=1 (tools/muse/board.sh), and MUSE_BENCH=1 as well for --snap. It opens the port the way
tools/muse/chat.py does, with the control lines left alone, so the board doesn't reset. check holds the folder to
the gadget's rule that its name is the app's id.

check looks at the folder as push does, before anything of it is read by apphost (read_package: every listed path
inside the folder and a regular file, within the board's limits). It builds apphost (tools/muse/apphost) from this
checkout the first time, and again whenever one of its sources changes (tools/muse/apphost_build.py), and runs the folder
on it. It prints apphost's events, one JSON object a line (a
`started` for an app that loads, a `log` for each line it prints, a `crash` for one that fails), and exits 1 on an
`error` or a `crash` event. --script FILE is apphost's script of commands (message, tap, dump, ...), which
tools/muse/apphost/apphost.c lists. It needs no gadget and no pyserial. Before it runs anything it compiles every .lua
file as the install does, so a package it passes is one the install doesn't refuse for its Lua: one that doesn't compile
is the one `crash` event the install would answer, "compile failed: util.lua:7: ..." and the lines around line 7, and
nothing runs.

push sends manifest.json, then main, then the manifest's "files" in order, as `>app.put` lines of at most 768 base64
characters each (a console line is at most 1,023 bytes after the '>'), one at a time, each after the `@app.ack` for
the last, and waits 2 s for each ack. It then sends `>app.commit ID [run]` and prints the board's `@app` result on
stdout, one line of JSON: {"op":"commit","ok":true,"payload":{"id","state","storage","warnings","log","ui",...}} or
{"op":"commit","ok":false,"error":{"code","message"}}. A refusal is an `@app.err` line, printed the same way. What it
means for a person goes to stderr. The exit status is 0 for an ok result and 1 for anything else: a result that says
not ok, a refusal, a wait that ran out, a gadget that doesn't answer. --run sends the `w` key first, so that a
sleeping screen is awake for the layout check and for the picture; --snap waits 1 s after the result and takes a
screenshot on the same port (tools/muse/snap.py).

Everything printed is for a terminal that obeys what it is sent, and what the board, an app or a package says is not
the tool's: text for a person shows each control character but newline and tab, DEL and each C1 character (U+009B is
CSI) as \\xNN, and a JSON line printed whole (the result, apphost's events) keeps its meaning, with U+007F to U+009F as
\\u00XX (printable, printable_json).

Replies are matched at column 0 of a line and nowhere else: an `@app.log` echo carries the app's own text, which could
say anything, and is never taken for a reply. The one exception is the first line after the port opens, which may
have a stale USB packet (at most 64 bytes) in front of it: opening the port is followed by a query whose answer is
taken at column 0 or glued to the end of that line, and everything before it is dropped. Lines are split at b"\\n" and
nothing else (an app may print U+2028).
"""

import argparse
import base64
import contextlib
import errno
import json
import os
import re
import secrets
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import NamedTuple

import apphost_build
import chat
import snap

PIECE_CHARS = 768                    # base64 characters in one app.put (R13.6): a line of about 900 bytes at most
PIECE_BYTES = PIECE_CHARS // 4 * 3   # the bytes of a file one piece carries
ACK_S = 2.0                          # how long an @app.ack is waited for
COMMIT_S = 95.0                      # the result of an install: the board's own deadline (APPS_INSTALL_DEADLINE_MS) and 5 s
SYNC_TRIES = 3                       # queries sent to a board that doesn't answer the first
SYNC_S = 1.5                         # how long each is waited for
SNAP_DELAY_S = 1.0                   # --snap: the pause between the result and the picture
POLL_S = 0.5                         # watch: how often it looks at the files, and logs reads at least this long
# What the board takes of a package (apps_pkg.h; tests/test_muse_apps_tool.py reads the header and holds these to it):
# manifest.json at most 8 KB, a file at most the limit of its kind, the app (manifest and files) at most 2 MB, the images
# among them at most 1.5 MB, and at most 32 files besides manifest.json (main among them).
MANIFEST_MAX = 8 * 1024
FILE_MAX = {".lua": 256 * 1024, ".json": 32 * 1024, ".bin": 512 * 1024}
APP_MAX = 2 * 1024 * 1024
IMAGES_MAX = 1536 * 1024
FILES_MAX = 32
# What apps_path_valid and apps_id_valid (apps_pkg.c) take: a path is 1 to 64 characters of parts split by '/', and no
# part is empty, starts or ends with '.', or has '..' in it; an id is 2 to 24 characters.
SEGMENT = r"[a-z0-9_-]+(?:\.[a-z0-9_-]+)*"
PATH_RULE = re.compile(rf"(?=.{{1,64}}\Z){SEGMENT}(?:/{SEGMENT})*")
ID_RULE = re.compile(r"[a-z][a-z0-9_]{1,23}")


class AppError(Exception):
    """Something the person at the keyboard can act on; str() says what."""


# ---- What is printed ----
#
# The terminal obeys what it is sent, and the board's, an app's and a package's text is not the tool's: ESC and the C1
# controls (U+009B is CSI) are escape sequences that can write the clipboard, rewrite the screen or type into the shell.
# Everything the tool prints goes through say() or say_json(), and nothing else prints.

UNSAFE_TEXT = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f\ud800-\udfff]")   # all but \n and tab; and what no encoding takes
UNSAFE_JSON = re.compile(r"[\x7f-\x9f\ud800-\udfff]")    # what a JSON string may hold as it is (it escapes the C0 itself)


def printable(text):
    """`text` for a person's terminal: each control character but newline and tab, and each character no output encoding
    takes, written out as \\xNN (or \\uNNNN)."""
    return UNSAFE_TEXT.sub(lambda m: f"\\x{ord(m[0]):02x}" if ord(m[0]) < 0x100 else f"\\u{ord(m[0]):04x}", text)


def printable_json(line):
    """A line of JSON for a terminal, meaning the same: U+007F to U+009F (and a lone surrogate), which a string may hold
    raw, as the \\u00XX that decodes to them, and a CR between tokens, which is a space to JSON, as one. A line that
    isn't JSON is text (printable)."""
    try:
        json.loads(line)
    except (ValueError, RecursionError):
        return printable(line)
    return UNSAFE_JSON.sub(lambda m: f"\\u{ord(m[0]):04x}", line).replace("\r", " ")


def say(text, err=False, end="\n"):
    """Prints text for a person, on stderr when `err`."""
    print(printable(text), end=end, file=sys.stderr if err else sys.stdout, flush=True)


def say_json(line):
    """Prints a JSON line (a result, an event) on stdout."""
    print(printable_json(line), flush=True)


# ---- The package ----

class Package(NamedTuple):
    id: str
    files: list   # [(path, bytes)]: manifest.json first, then main, then the manifest's "files" in order


LINK_LOOP = "its links go round in a loop"


def resolved(path, cant_read):
    """path.resolve(), or AppError(cant_read(why)) for a path whose links go round in a loop (Python 3.12's resolve()
    raises RuntimeError on one, later ones OSError or nothing, which stat() then meets) or that can't be looked at."""
    try:
        return path.resolve()
    except RuntimeError:
        raise AppError(cant_read(LINK_LOOP))
    except OSError as e:
        raise AppError(cant_read(LINK_LOOP if e.errno == errno.ELOOP else e.strerror or e))


def listed_file(here, path, cant_read):
    """`path` (the manifest's, relative to the app's folder `here`) as the file it is on this machine: -> (where, how
    big). Where it leads after its links must be inside the folder and be a regular file, and it is looked at before
    it is opened, as a FIFO nobody writes never ends, and a file of a gigabyte is no reason to read a gigabyte.
    `cant_read(why)` is the message for a file that isn't there, or can't be looked at."""
    target = resolved(here / path, cant_read)
    if here not in target.parents:
        raise AppError(f"'{path}' is outside the app's folder {here}")
    try:
        info = target.stat()   # resolve() has followed the links
    except OSError as e:
        raise AppError(cant_read(LINK_LOOP if e.errno == errno.ELOOP else e.strerror or e))
    if not stat.S_ISREG(info.st_mode):
        raise AppError(f"'{path}' isn't a regular file: the board takes files, and only those")
    return target, info.st_size


def read_listed(target, path, limit):
    """The bytes of `target`, at most `limit` of them (more is a file that grew since it was looked at)."""
    try:
        with open(target, "rb") as f:
            data = f.read(limit + 1)
    except OSError as e:
        raise AppError(f"{path} can't be read: {e.strerror or e}")
    if len(data) > limit:
        raise AppError(f"{path} is larger than {limit // 1024} KB")
    return data


def read_package(folder):
    """The package in `folder`, in the order the console takes it: manifest.json, the main file (main.lua unless the
    manifest says another) and then the manifest's "files", none twice. What the manifest doesn't list stays home.

    What push sends and what check runs: it is said before anything is sent or run when a file is missing, is outside
    the folder or isn't a regular file, has a path the console would read as something else or a kind it has no use
    for, or is over what the board takes (the limit of its kind, the app's 2 MB, the images' 1.5 MB, 32 files), all of
    it decided by looking at the files, not by reading them. A manifest that can't be read is said in words."""
    folder = Path(folder)
    manifest_path = folder / "manifest.json"
    here = resolved(folder, lambda why: f"can't read {folder}: {why}")
    target, size = listed_file(here, "manifest.json", lambda why: f"can't read {manifest_path}: {why}")
    if size > MANIFEST_MAX:
        raise AppError(f"manifest.json is larger than {MANIFEST_MAX // 1024} KB")   # before it is parsed: that bounds the nesting
    text = read_listed(target, "manifest.json", MANIFEST_MAX)
    try:
        manifest = json.loads(text)
    except RecursionError:
        raise AppError(f"{manifest_path} is nested too deeply to read")
    except ValueError as e:
        raise AppError(f"{manifest_path} isn't JSON: {e}")
    if not isinstance(manifest, dict):
        raise AppError(f"{manifest_path} must be a JSON object")
    app_id, main, listed = manifest.get("id"), manifest.get("main", "main.lua"), manifest.get("files", [])
    if not isinstance(app_id, str) or not ID_RULE.fullmatch(app_id):
        raise AppError(f'{manifest_path} needs an "id" that matches ^[a-z][a-z0-9_]{{1,23}}$, got {json.dumps(app_id)}')
    if not isinstance(main, str):
        raise AppError(f'"main" in {manifest_path} must be the path of a .lua file')
    if not isinstance(listed, list) or not all(isinstance(path, str) for path in listed):
        raise AppError(f'"files" in {manifest_path} must be a list of paths, like ["util.lua"]')
    paths = list(dict.fromkeys([main, *listed]))
    for path in paths:
        if not PATH_RULE.fullmatch(path):
            raise AppError(f"'{path}' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 characters)")
    if not main.endswith(".lua"):
        raise AppError(f'"main" in {manifest_path} must be the path of a .lua file')
    if len(paths) > FILES_MAX:
        raise AppError(f"more than {FILES_MAX} files besides manifest.json")
    # Every file is looked at, in the board's order and by the board's rules, before the first is read.
    found, app_bytes, image_bytes = [], size, 0
    for path in paths:
        kind = next((suffix for suffix in FILE_MAX if path.endswith(suffix)), None)
        if kind is None:
            raise AppError(f"'{path}' has an unsupported type (use .lua, .json or .bin)")
        target, size = listed_file(here, path,
                                   lambda why: f"{path} is listed in {manifest_path} but can't be read: {why}")
        if size > FILE_MAX[kind]:
            raise AppError(f"{path} is larger than {FILE_MAX[kind] // 1024} KB")
        app_bytes += size
        if app_bytes > APP_MAX:
            raise AppError(f"the app is larger than {APP_MAX // (1024 * 1024)} MB")
        if kind == ".bin":
            image_bytes += size
            if image_bytes > IMAGES_MAX:
                raise AppError("images add up to more than 1.5 MB")
        found.append((path, target, FILE_MAX[kind]))
    return Package(app_id, [("manifest.json", text), *((path, read_listed(target, path, limit))
                                                          for path, target, limit in found)])


def put_commands(app_id, path, data):
    """The `app.put` commands (no '>' and no newline) that carry one file, one at a time: pieces of at most PIECE_BYTES
    bytes, as base64, with their seq numbers from 0; an empty file is one piece with nothing in it. A piece is made
    when it is asked for, so a refusal of the first leaves the rest unmade."""
    for seq, start in enumerate(range(0, max(len(data), 1), PIECE_BYTES)):
        piece = data[start:start + PIECE_BYTES]
        yield f"app.put {app_id} {path} {seq}" + (f" {base64.b64encode(piece).decode()}" if piece else "")


# ---- The console ----

class Reply(NamedTuple):
    kind: str        # "ack", "result" (an @app line), "err" (an @app.err line) or "log" (an @app.log echo)
    text: str        # the JSON of a result or an error, as the board printed it; an echo's whole line
    data: dict       # that JSON, parsed
    path: str = ""   # an ack's file
    seq: int = -1    # and piece


ACK = re.compile(r"@app\.ack (\S+) ([0-9]+)")


def parse_reply(line):
    """What the board's `line` is, when it is a reply or an echo; None for anything else: log noise, a line that has
    an @app in the middle of it, an @app line that isn't one JSON object. Replies match at column 0 and nowhere else:
    an @app.log echo carries the app's own words, and they could be a forged reply."""
    if line.startswith("@app.log "):
        return Reply("log", line, {})
    match = ACK.fullmatch(line)
    if match:
        return Reply("ack", "", {}, match[1], int(match[2]))
    for prefix, kind in (("@app.err ", "err"), ("@app ", "result")):
        if line.startswith(prefix):
            text = line[len(prefix):]
            try:
                data = json.loads(text)
            except ValueError:
                return None
            return Reply(kind, text, data) if isinstance(data, dict) else None
    return None


def is_sync_reply(line, nonce, first):
    """Whether `line` answers the query that carried `nonce`: the refusal of its id. At column 0, or, for the first line
    since the port opened (`first`), at the end of it: a stale USB packet may have been waiting in front. Never inside
    an @app.log line, whose text is the app's, and the nonce is something no app has seen."""
    if line.startswith("@app.err "):
        at = 0
    elif first and not line.startswith("@app.log "):
        at = line.rfind("@app.err ")
    else:
        return False
    reply = parse_reply(line[at:]) if at >= 0 else None
    return reply is not None and reply.kind == "err" and f"'0{nonce}'" in str(reply.data.get("message", ""))


class Console:
    """The board's console as chat.Board reads and writes it. The board's `read_line(deadline)` splits at b"\\n"."""

    def __init__(self, board):
        self.board = board
        self.fresh = True   # no line read yet since the port opened: the first may have a stale fragment in front
        self.queries = []   # the ids of the sync queries sent: the refusal of one that comes late is not a put's

    @property
    def ser(self):
        return self.board.ser

    def close(self):
        self.board.close()

    def send(self, command):
        self.board.write_line(command)

    def wake(self):
        """The console's `w` key: a screen that sleeps comes on, one that is on stays as it is."""
        self.board.write(b"w\n")

    def sync(self):
        """Makes sure the replies that follow are the board's answers to what is sent next: a query (an app.put the
        board refuses for its id, before it touches anything) goes out, and every line is dropped until its answer
        comes. A newline goes first, to end a half line that a program cut off left in the board's buffer."""
        for _ in range(SYNC_TRIES):
            nonce = secrets.token_hex(4)
            self.queries.append(f"'0{nonce}'")
            self.board.write(b"\n")
            self.send(f"app.put 0{nonce} sync 0")
            deadline = time.monotonic() + SYNC_S
            while (line := self.board.read_line(deadline)) is not None:
                first, self.fresh = self.fresh, False
                if is_sync_reply(line, nonce, first):
                    return
        raise AppError(f"the board on {self.board.port} didn't answer app.put. Is it running firmware built with "
                       "MUSE_APPS=1 (tools/muse/board.sh), and is nothing else using the port?")

    def wait(self, timeout, accept, what):
        """The first reply `accept` takes, within `timeout` seconds; every other line is skipped (the app's echo, the
        log, an answer to something else)."""
        deadline = time.monotonic() + timeout
        while True:
            line = self.board.read_line(deadline)
            if line is None:
                raise AppError(f"no {what} within {timeout:g} s")
            reply = parse_reply(line)
            if reply is not None and accept(reply):
                return reply

    def ack(self, path, seq, timeout=ACK_S):
        """The ack for piece `seq` of `path`, or the board's refusal of a put of that file. The console names the file in
        every refusal of a put but the usage error's, so a late refusal of another file's piece (a push that gave up
        on it, in watch) is not this one's."""
        def accept(r):
            if r.kind == "ack":
                return (r.path, r.seq) == (path, seq)
            late = any(query in str(r.data.get("message", "")) for query in self.queries)   # a slow sync query's
            return (r.kind == "err" and r.data.get("op") == "put" and r.data.get("path", path) == path and not late)

        return self.wait(timeout, accept, f"@app.ack for {path} piece {seq}")

    def result(self, op, timeout):
        """The result of app.<op>, or the board's refusal of it."""
        return self.wait(timeout, lambda r: r.kind in ("result", "err") and r.data.get("op") == op,
                         f"result of app.{op}")

    def pump(self, seconds, echo):
        """Reads for `seconds` seconds and gives each @app.log line to echo, whole."""
        deadline = time.monotonic() + seconds
        while (line := self.board.read_line(deadline)) is not None:
            reply = parse_reply(line)
            if reply is not None and reply.kind == "log":
                echo(reply.text)


def open_board(port):
    """chat.Board's way of opening a port: nothing toggles DTR or RTS, so the board doesn't reset."""
    try:
        return chat.Board(port)
    except chat.BoardError as e:
        raise AppError(str(e))


def open_console(port):
    board = open_board(port)
    console = Console(board)
    try:
        console.sync()
    except BaseException:
        board.close()
        raise
    return console


# ---- push ----

def push(console, package, run=False):
    """Sends the package and commits it. -> the board's Reply: the result of the commit, or the refusal of a put or of
    the commit. AppError when a wait runs out. A run wakes the screen first: a sleeping one has no layout for the run
    to check, and nobody to show the app to."""
    if run:
        console.wake()
    for path, data in package.files:
        for seq, command in enumerate(put_commands(package.id, path, data)):
            console.send(command)
            reply = console.ack(path, seq)
            if reply.kind == "err":
                return reply
    console.send(f"app.commit {package.id}" + (" run" if run else ""))
    return console.result("commit", COMMIT_S)


def describe(reply):
    """What a reply means, for a person: the one line of an install that worked (with its warnings), and the whole of
    what went wrong (a crash's traceback and log included)."""
    data = reply.data
    if reply.kind == "err":
        return f"app.{data.get('op', '?')} was refused: {data.get('message', reply.text)}"
    if data.get("ok") is not True:
        error = data.get("error") if isinstance(data.get("error"), dict) else {}
        return f"{error.get('code', 'failed')}: {error.get('message', reply.text)}"
    payload = data.get("payload") if isinstance(data.get("payload"), dict) else {}
    warnings = payload.get("warnings") or []
    widgets = len(payload.get("ui") or []) + int(payload.get("ui_truncated") or 0)
    text = (f"{payload.get('id')}: {payload.get('state')} ({payload.get('storage')}), {widgets} widgets, "
            f"{len(warnings) or 'no'} warnings")
    return text + "".join(f"\nwarning: {w}" for w in warnings)


def report(reply):
    """Prints the board's JSON on stdout, one line, and what it means on stderr. -> the exit status: 0 for an ok
    result, 1 for anything else."""
    say_json(reply.text)
    say(describe(reply), err=True)
    return 0 if reply.kind == "result" and reply.data.get("ok") is True else 1


def take_picture(console, out):
    """Waits for the app to draw, then takes a screenshot on the console's port, waking the screen first."""
    time.sleep(SNAP_DELAY_S)
    try:
        w, h, raw = snap.capture(console.ser, "w")
    except snap.SnapError as e:
        raise AppError(str(e))
    try:
        snap.write_png(out, w, h, raw)
    except OSError as e:
        raise AppError(f"can't write {out}: {e.strerror or e}")
    say(f"{out}: {w}x{h}", err=True)


def cmd_push(args):
    package = read_package(args.dir)
    if args.snap and not Path(args.snap).resolve().parent.is_dir():
        raise AppError(f"--snap {args.snap}: there is no folder to put the picture in")   # before anything is pushed
    with contextlib.closing(open_console(args.port)) as console:
        reply = push(console, package, run=args.run)
        status = report(reply)
        if args.snap and reply.kind == "result":
            take_picture(console, args.snap)
    return status


# ---- watch and logs ----

def signature(folder):
    """What watch compares from one poll to the next: (path, mtime in ns, size) of every file in `folder`, the folders
    and files that start with '.' (an editor's swap file, .git) and a trailing '~' (a backup) left out."""
    found = []
    for here, dirs, names in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(names):
            if name.startswith(".") or name.endswith("~"):
                continue
            path = os.path.join(here, name)
            try:
                st = os.stat(path)
            except OSError:
                continue   # gone since the walk read the folder
            found.append((path, st.st_mtime_ns, st.st_size))
    return found


def echo(line):
    say(line)


def deploy(console, folder):
    """One push with a run, for watch: what goes wrong is said and the watch goes on."""
    try:
        report(push(console, read_package(folder), run=True))
    except AppError as e:
        say(f"app.py: {e}", err=True)


def cmd_watch(args):
    with contextlib.closing(open_console(args.port)) as console:
        seen = signature(args.dir)   # taken before the push: an edit while it goes on is seen at the next poll
        try:
            deploy(console, args.dir)
            while True:
                console.pump(POLL_S, echo)
                now = signature(args.dir)
                if now != seen:
                    seen = now
                    deploy(console, args.dir)
        except KeyboardInterrupt:
            return 0


def cmd_logs(args):
    with contextlib.closing(open_console(args.port)) as console:
        try:
            while True:
                console.pump(POLL_S, echo)
        except KeyboardInterrupt:
            return 0


# ---- check ----

def failed_event(line):
    """Whether a line of apphost's output is an `error` or a `crash` event."""
    try:
        event = json.loads(line)
    except ValueError:
        return False
    return isinstance(event, dict) and event.get("event") in ("error", "crash")


def cmd_check(args):
    read_package(args.dir)   # push's look at the folder, before apphost opens anything in it
    try:
        exe = apphost_build.ensure(progress=lambda text: say(text, err=True))
    except apphost_build.BuildError as e:
        raise AppError(str(e))
    command = [str(exe), *(["--script", args.script] if args.script else []), str(args.dir)]
    failed = False
    with tempfile.TemporaryFile() as stderr:
        try:
            proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr)
        except OSError as e:
            raise AppError(f"can't run {exe}: {e.strerror or e}")
        with proc:
            for raw in proc.stdout:   # bytes, so a line ends at b"\n" and at nothing else
                line = raw.decode("utf-8", "replace")
                say_json(line.rstrip("\n"))
                failed = failed or failed_event(line)
        status = proc.returncode
        stderr.seek(0)
        say(stderr.read().decode("utf-8", "replace"), err=True, end="")
    if status != 0:
        return status if status > 0 else 1
    return 1 if failed else 0


# ---- The command line ----

def build_parser():
    parser = argparse.ArgumentParser(prog="app.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True, metavar="{check,push,watch,logs}")
    check = commands.add_parser("check", help="run the app on this machine, no gadget needed")
    check.add_argument("dir", metavar="DIR", help="the app's folder")
    check.add_argument("--script", metavar="FILE", help="apphost's script of commands (message, tap, dump ...)")
    check.set_defaults(func=cmd_check)
    pushing = commands.add_parser("push", help="install the app on the gadget")
    pushing.add_argument("port", metavar="PORT", help="the gadget's serial port")
    pushing.add_argument("dir", metavar="DIR", help="the app's folder")
    pushing.add_argument("--run", action="store_true", help="start it, waking the screen first")
    pushing.add_argument("--snap", metavar="OUT.png", help="save a screenshot 1 s after the result")
    pushing.set_defaults(func=cmd_push)
    watching = commands.add_parser("watch", help="push with --run on every change to the app's folder")
    watching.add_argument("port", metavar="PORT", help="the gadget's serial port")
    watching.add_argument("dir", metavar="DIR", help="the app's folder")
    watching.set_defaults(func=cmd_watch)
    logs = commands.add_parser("logs", help="print what the app prints")
    logs.add_argument("port", metavar="PORT", help="the gadget's serial port")
    logs.set_defaults(func=cmd_logs)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except AppError as e:
        say(f"app.py: {e}", err=True)
        return 1
    except BrokenPipeError:
        return 0   # whoever reads our output has gone
    except OSError as e:
        # The port's, for a command that has one; check never opened a board, and a full disk or a file it can't read is
        # the host's.
        say(f"app.py: {'check failed on this machine' if args.command == 'check' else 'lost the board'}: {e}", err=True)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
