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

"""Builds and runs the C harnesses of the Muse apps host tests.

Every test_muse_apps_*.py compiles its harness with build() and runs it with
run(). Lua's files, defines and options come from components/lua/CMakeLists.txt,
the file the device build reads, so a host binary and the firmware agree.
"""

from __future__ import annotations

import atexit
import concurrent.futures
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / "components" / "lua"
LUA_SRC = LUA / "src"

# ASan aborts on its first report; -fno-sanitize-recover makes UBSan's fatal too,
# however the binary is run.
_SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
             "-fno-omit-frame-pointer"]


def cc() -> list[str]:
    """The compiler command, $CC else cc. Skips the test when there is none."""
    cmd = shlex.split(os.environ.get("CC", "cc"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C compiler not available")
    return cmd


def _lua_cmake() -> str:
    # Without its comments (the file has no quoted '#').
    return re.sub(r"#[^\n]*", "", (LUA / "CMakeLists.txt").read_text())


def _lua_private(command: str) -> list[str]:
    """The arguments of `command(${COMPONENT_LIB} PRIVATE ...)` in the Lua component."""
    found = re.search(command + r"\(\s*\$\{COMPONENT_LIB\}\s+PRIVATE\s+([^)]*)\)", _lua_cmake())
    if not found:
        raise AssertionError(f"components/lua/CMakeLists.txt has no {command}(${{COMPONENT_LIB}} PRIVATE ...)")
    return found.group(1).split()


def lua_sources() -> list[Path]:
    """The Lua files the component compiles (its quoted "src/<name>.c" entries)."""
    names = re.findall(r'"(src/\w+\.c)"', _lua_cmake())
    if not names:
        raise AssertionError("components/lua/CMakeLists.txt lists no sources")
    return [LUA / name for name in names]


def lua_defines() -> list[str]:
    """The component's compile definitions, as NAME or NAME=VALUE (no -D)."""
    return _lua_private("target_compile_definitions")


def lua_options() -> list[str]:
    """The component's compile options: -O2 -w."""
    return _lua_private("target_compile_options")


# cc -> (flags, leak checks work): what this compiler can run, probed once.
_probes: dict[tuple[str, ...], tuple[list[str], bool]] = {}
# binary -> (sanitized, leak checks on), for run().
_built: dict[Path, tuple[bool, bool]] = {}


def _probe(tmp: Path) -> tuple[list[str], bool]:
    cmd = cc()
    key = tuple(cmd)
    if key not in _probes:
        flags, leaks = [], False
        probe = Path(tmp) / "apps_sanitizer_probe"
        built = subprocess.run([*cmd, "-x", "c", "-std=c11", *_SANITIZE, "-o", str(probe), "-"],
                               input=b"int main(void) { return 0; }\n", capture_output=True)

        def runs(options: str) -> bool:
            done = subprocess.run([str(probe)], capture_output=True,
                                  env={**os.environ, "ASAN_OPTIONS": options})
            return done.returncode == 0

        # LeakSanitizer is a second question: macOS aborts on detect_leaks=1.
        if built.returncode == 0 and runs("detect_leaks=0:abort_on_error=1"):
            flags, leaks = _SANITIZE, runs("detect_leaks=1:abort_on_error=1")
        _probes[key] = (flags, leaks)
    return _probes[key]


def sanitizer_flags(tmp: Path) -> list[str]:
    """ASan and UBSan flags if this compiler can build and run them, else []."""
    return list(_probe(tmp)[0])


# Lua is the biggest compile and the same for every harness: build it once per
# process for each set of flags.
_lua_objects: Path | None = None


def _lua_object_dir(flags: Sequence[str]) -> Path:
    global _lua_objects
    if _lua_objects is None:
        _lua_objects = Path(tempfile.mkdtemp(prefix="muse-apps-lua-"))
        atexit.register(shutil.rmtree, _lua_objects, ignore_errors=True)
    out = _lua_objects / hashlib.sha256(" ".join(flags).encode()).hexdigest()[:12]
    out.mkdir(exist_ok=True)
    return out


def _define(define: str) -> str:
    return define if define.startswith("-D") else f"-D{define}"


def build(name: str, sources: Sequence[str | Path], tmp: str | Path, *,
          include_dirs: Sequence[str | Path] = (), defines: Sequence[str] = (),
          sanitize: bool = True) -> Path:
    """Compiles and links `sources` into tmp/name, and returns that path.

    Lua's own files (components/lua/src) get the device's options and defines
    and no warnings: a harness lists them from lua_sources(). The others get
    include_dirs, defines (NAME or NAME=VALUE) and -Wall -Wextra -Werror
    -std=c11 -O1 -g. Every file gets -D_GNU_SOURCE and -pthread, as the other
    host tests do, and a fixed string-hash seed, so tables iterate the same on
    every run. sanitize=False skips ASan and UBSan, for tests whose stack use
    they would change. A compile or link error raises AssertionError with the
    compiler's output.
    """
    cmd, tmp = cc(), Path(tmp)
    san, leaks = _probe(tmp) if sanitize else ([], False)
    common = [*cmd, "-D_GNU_SOURCE", "-pthread", "-Dluai_makeseed(L)=0x5eedu", *san]
    lua_flags = [*common, *lua_options(), *(_define(d) for d in lua_defines())]
    c_flags = [*common, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               *(f"-I{d}" for d in include_dirs), *(_define(d) for d in defines)]

    jobs = []  # (flags, source, object, reuse an object built earlier)
    for i, source in enumerate((ROOT / s).resolve() for s in sources):
        if source.parent == LUA_SRC:
            jobs.append((lua_flags, source, _lua_object_dir(lua_flags) / f"{source.stem}.o", True))
        else:
            jobs.append((c_flags, source, tmp / f"{name}.{i}.{source.stem}.o", False))

    def fail(args: list[str], done: subprocess.CompletedProcess) -> str:
        return f"{shlex.join(args)}\n{done.stdout}{done.stderr}"

    def compile_one(job: tuple[list[str], Path, Path, bool]) -> str | None:
        flags, source, obj, reuse = job
        if reuse and obj.exists():
            return None
        args = [*flags, "-c", str(source), "-o", str(obj)]
        done = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        return None if done.returncode == 0 else fail(args, done)

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        errors = [e for e in pool.map(compile_one, jobs) if e]
    if errors:
        raise AssertionError("\n".join(errors))

    exe = tmp / name
    args = [*cmd, *san, "-pthread", *(str(job[2]) for job in jobs), "-lm", "-o", str(exe)]
    done = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError(fail(args, done))

    _built[exe.resolve()] = (bool(san), leaks)
    if not sanitize:
        state = "off (sanitize=False)"
    elif not san:
        state = "OFF (this compiler can't run ASan and UBSan)"
    elif leaks:
        state = "ASan+UBSan, leak checks on"
    else:
        state = "ASan+UBSan, no leak checks (LeakSanitizer isn't available here)"
    print(f"apps_host: {name}: sanitizers {state}", file=sys.stderr, flush=True)
    return exe


def run(binary: str | Path, *args: object, input: str | None = None,
        timeout: float = 60) -> subprocess.CompletedProcess:
    """Runs a harness built by build() and returns the CompletedProcess.

    Its output is text: bytes that aren't UTF-8 become U+FFFD. A sanitized
    binary aborts on its first report, and checks for leaks only where
    LeakSanitizer works.
    """
    sanitized, leaks = _built.get(Path(binary).resolve(), (False, False))
    env = os.environ.copy()
    if sanitized:
        env["ASAN_OPTIONS"] = f"detect_leaks={int(leaks)}:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"
    return subprocess.run([str(binary), *map(str, args)], cwd=ROOT, input=input,
                          capture_output=True, text=True, errors="replace",
                          timeout=timeout, env=env)
