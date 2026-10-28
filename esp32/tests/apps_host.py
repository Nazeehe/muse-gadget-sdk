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
build_apphost() builds the host runner (tools/muse/apphost) from its sources.txt
and those same Lua files.
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
APPS = ROOT / "components" / "muse" / "apps"
APPHOST = ROOT / "tools" / "muse" / "apphost"

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
    """What the Lua component's one `command(${COMPONENT_LIB} PRIVATE ...)` call passes.

    The host build doesn't evaluate if(), so it only takes a file it can read whole:
    exactly one such call, and nothing PUBLIC or INTERFACE, which the muse
    component would pick up too.
    """
    calls = re.findall(rf"\b{command}\s*\(([^)]*)\)", _lua_cmake(), re.IGNORECASE)
    if len(calls) != 1:
        raise AssertionError(
            f"components/lua/CMakeLists.txt needs exactly one {command}() call, found {len(calls)}: "
            "tests/apps_host.py doesn't evaluate if(), so all of them go in that one call")
    words = calls[0].split()
    if words[:2] != ["${COMPONENT_LIB}", "PRIVATE"] or {"PUBLIC", "INTERFACE"} & set(words):
        raise AssertionError(
            f"components/lua/CMakeLists.txt: {command}() must be (${{COMPONENT_LIB}} PRIVATE ...) "
            f"and nothing else, got ({' '.join(words)})")
    return words[2:]


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
          lua_extra_defines: Sequence[str] = (), sanitize: bool = True,
          objects: Sequence[str | Path] = ()) -> Path:
    """Compiles and links `sources` into tmp/name, and returns that path.

    Lua's own files (components/lua/src) get the device's options and defines,
    lua_extra_defines (NAME or NAME=VALUE, for example LUA_USE_APICHECK) and no
    warnings: a harness lists them from lua_sources(). The others get
    include_dirs, defines (NAME or NAME=VALUE) and -Wall -Wextra -Werror
    -std=c11 -O1 -g. Every file gets -D_GNU_SOURCE and -pthread, as the other
    host tests do, and a fixed string-hash seed, so tables iterate the same on
    every run. sanitize=False skips ASan and UBSan, for tests whose stack use
    they would change. objects are linked in as they are (cjson_object()'s).
    A compile or link error raises AssertionError with the compiler's output.
    """
    cmd, tmp = cc(), Path(tmp)
    san, leaks = _probe(tmp) if sanitize else ([], False)
    common = [*cmd, "-D_GNU_SOURCE", "-pthread", "-Dluai_makeseed(L)=0x5eedu", *san]
    lua_flags = [*common, *lua_options(), *(_define(d) for d in [*lua_defines(), *lua_extra_defines])]
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
    args = [*cmd, *san, "-pthread", *(str(job[2]) for job in jobs), *(str(o) for o in objects), "-lm", "-o", str(exe)]
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


# The real cJSON, as the firmware has it: CI supplies pinned upstream sources, and a device build leaves them in
# managed_components (which board.sh deletes again: X16), so the suite runs with CJSON_SOURCE_DIR set.
CJSON_DIR = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
_cjson: Path | None = None


def cjson_object() -> Path:
    """cJSON.c compiled once per process, the way test_link_discovery.py compiles it (-std=c11 -Wall -Wextra -Werror
    -D_DEFAULT_SOURCE -DCJSON_NESTING_LIMIT=16) and without the sanitizers, which on macOS turn its sprintf calls into
    deprecation errors. Link it with build(..., objects=[...]) and add CJSON_DIR to include_dirs. Skips the test when the
    sources aren't on this machine.
    """
    global _cjson
    if _cjson is None:
        source = CJSON_DIR / "cJSON.c"
        if not source.is_file():
            raise unittest.SkipTest(f"cJSON sources not found in {CJSON_DIR}: set CJSON_SOURCE_DIR "
                                    "(~/.cache/muse-cjson) or run an IDF build once")
        out = Path(tempfile.mkdtemp(prefix="muse-apps-cjson-"))
        atexit.register(shutil.rmtree, out, ignore_errors=True)
        args = [*cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", "-DCJSON_NESTING_LIMIT=16",
                "-c", str(source), "-o", str(out / "cJSON.o")]
        done = subprocess.run(args, capture_output=True, text=True)
        if done.returncode != 0:
            raise AssertionError(f"{shlex.join(args)}\n{done.stdout}{done.stderr}")
        _cjson = out / "cJSON.o"
    return _cjson


def apphost_sources() -> list[Path]:
    """The C files of tools/muse/apphost/sources.txt (one path a line, relative to esp32/; blank lines and
    lines starting with # are skipped), as paths. Only components/muse/apps/*.c and tools/muse/apphost/*.c
    may be listed: the Lua files come from components/lua/CMakeLists.txt, so there is one list of them
    and one set of defines.
    """
    listed = []
    for line in (APPHOST / "sources.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path = (ROOT / line).resolve()
        if path.suffix != ".c" or path.parent not in (APPS, APPHOST) or not path.is_file():
            raise AssertionError(
                f"tools/muse/apphost/sources.txt: '{line}' must be an existing components/muse/apps/*.c or "
                "tools/muse/apphost/*.c (Lua's files come from components/lua/CMakeLists.txt)")
        listed.append(path)
    if not listed:
        raise AssertionError("tools/muse/apphost/sources.txt lists no sources")
    return listed


def build_apphost(tmp: str | Path, *, sanitize: bool = True) -> Path:
    """Builds the host runner from sources.txt and Lua's files, as the firmware's Lua is built (build()),
    into tmp/apphost, and returns that path. Same flags as build(): -Wall -Wextra -Werror for the runner's
    own files, ASan and UBSan where the compiler has them.
    """
    return build("apphost", [*apphost_sources(), *lua_sources()], tmp,
                 include_dirs=[LUA_SRC, APPS, APPHOST], sanitize=sanitize)


def run(binary: str | Path, *args: object, input: str | bytes | None = None, timeout: float = 60,
        text: bool = True) -> subprocess.CompletedProcess:
    """Runs a harness built by build() and returns the CompletedProcess.

    By default its output is text: bytes that aren't UTF-8 become U+FFFD, and input is a str. With
    text=False both are bytes, for a test that has to see exactly what the binary wrote. A sanitized
    binary aborts on its first report, and checks for leaks only where LeakSanitizer works.
    """
    sanitized, leaks = _built.get(Path(binary).resolve(), (False, False))
    env = os.environ.copy()
    if sanitized:
        env["ASAN_OPTIONS"] = f"detect_leaks={int(leaks)}:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"
    options = {"text": True, "errors": "replace"} if text else {}
    return subprocess.run([str(binary), *map(str, args)], cwd=ROOT, input=input,
                          capture_output=True, timeout=timeout, env=env, **options)
