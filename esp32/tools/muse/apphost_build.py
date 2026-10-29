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

"""Builds apphost, the host runner of Muse apps (tools/muse/apphost), for tools/muse/app.py check.

apphost is the runtime the gadget runs (apps_rt and what it stands on) compiled for the laptop, so an app does there
what it does on the board. This module knows what it is built from:

- the C files of tools/muse/apphost/sources.txt, and the Lua files, defines and options of
  components/lua/CMakeLists.txt, read by the parsers below. tests/apps_host.py reads them through the same parsers, so
  a host test, the dev loop and the firmware agree on Lua's files and on LUAI_MAXCCALLS (how deep C calls may nest,
  which the app task's stack is sized to);
- how to compile them with the compiler on the machine ($CC, else cc): plain C11 with the warnings on, no feature
  macros, no sanitizers, Lua's own seed for its string hashes as on the device. ensure() builds it once and keeps the
  binary in ~/.cache/muse-apphost/<sha1>/apphost, where the sha1 is of everything it was built from: the sources, every
  header beside them, the two files that say what is built, this file (the recipe: the include directories, the order
  and form of the commands), the compiler command and the machine. Anything that changes one of those is another
  directory, so a stale apphost can't run.

The parsers don't evaluate CMake's if(): the Lua component keeps its sources, defines and options each in one call.
"""

import concurrent.futures
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# C11 with the warnings on, for the runtime's own files (-Werror is the tests' and CI's: a newer compiler's new warning
# is no reason to stop someone's work). No feature macros: what the sources need from POSIX they ask for themselves.
MUSE_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-O1"]
LINK_FLAGS = ["-lm"]


class BuildError(AssertionError):
    """The build can't be done, and why; str() says it. (An AssertionError, as tests/apps_host.py has always raised
    for a Lua component it can't follow or a compile that fails.)"""


# ---- What apphost is made of ----

def _lua_cmake(root=ROOT):
    # Without its comments (the file has no quoted '#').
    return re.sub(r"#[^\n]*", "", (Path(root) / "components" / "lua" / "CMakeLists.txt").read_text())


def _lua_private(command, root=ROOT):
    """What the Lua component's one `command(${COMPONENT_LIB} PRIVATE ...)` call passes.

    The host build doesn't evaluate if(), so it only takes a file it can read whole: exactly one such call, and nothing
    PUBLIC or INTERFACE, which the muse component would pick up too.
    """
    calls = re.findall(rf"\b{command}\s*\(([^)]*)\)", _lua_cmake(root), re.IGNORECASE)
    if len(calls) != 1:
        raise BuildError(
            f"components/lua/CMakeLists.txt needs exactly one {command}() call, found {len(calls)}: "
            "tools/muse/apphost_build.py doesn't evaluate if(), so all of them go in that one call")
    words = calls[0].split()
    if words[:2] != ["${COMPONENT_LIB}", "PRIVATE"] or {"PUBLIC", "INTERFACE"} & set(words):
        raise BuildError(
            f"components/lua/CMakeLists.txt: {command}() must be (${{COMPONENT_LIB}} PRIVATE ...) "
            f"and nothing else, got ({' '.join(words)})")
    return words[2:]


def lua_sources(root=ROOT):
    """The Lua files the component compiles (its quoted "src/<name>.c" entries)."""
    names = re.findall(r'"(src/\w+\.c)"', _lua_cmake(root))
    if not names:
        raise BuildError("components/lua/CMakeLists.txt lists no sources")
    return [Path(root) / "components" / "lua" / name for name in names]


def lua_defines(root=ROOT):
    """The component's compile definitions, as NAME or NAME=VALUE (no -D)."""
    return _lua_private("target_compile_definitions", root)


def lua_options(root=ROOT):
    """The component's compile options: -O2 -w."""
    return _lua_private("target_compile_options", root)


def apphost_sources(root=ROOT):
    """The C files of tools/muse/apphost/sources.txt (one path a line, relative to esp32/; blank lines and lines
    starting with # are skipped), as paths. Only components/muse/apps/*.c and tools/muse/apphost/*.c may be listed: the
    Lua files come from components/lua/CMakeLists.txt, so there is one list of them and one set of defines.
    """
    root = Path(root).resolve()
    apps, host = root / "components" / "muse" / "apps", root / "tools" / "muse" / "apphost"
    listed = []
    for line in (host / "sources.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path = (root / line).resolve()
        if path.suffix != ".c" or path.parent not in (apps, host) or not path.is_file():
            raise BuildError(
                f"tools/muse/apphost/sources.txt: '{line}' must be an existing components/muse/apps/*.c or "
                "tools/muse/apphost/*.c (Lua's files come from components/lua/CMakeLists.txt)")
        listed.append(path)
    if not listed:
        raise BuildError("tools/muse/apphost/sources.txt lists no sources")
    return listed


# ---- The compiler, and the commands ----

def compiler():
    """The compiler command: $CC (it may have arguments, as `zig cc -target ...`), else cc."""
    cc = shlex.split(os.environ.get("CC") or "cc")
    if not cc or shutil.which(cc[0]) is None:
        raise BuildError(f"no C compiler: '{cc[0] if cc else 'cc'}' isn't on the PATH. Install one (Xcode's command "
                         "line tools, or gcc or clang from your package manager), or set CC to the one to use.")
    return cc


def _object(obj_dir, kind, source):
    return Path(obj_dir) / f"{kind}_{Path(source).stem}.o"


def compile_commands(cc, obj_dir, strict=False, root=ROOT):
    """One command for each C file: the runtime's and the runner's own (MUSE_FLAGS, plus -Werror when `strict`), and
    Lua's with the options and defines of its CMake file (-DLUAI_MAXCCALLS=40). Objects go to obj_dir."""
    root = Path(root).resolve()
    lua_src = root / "components" / "lua" / "src"
    include = [f"-I{lua_src}", f"-I{root / 'components' / 'muse' / 'apps'}", f"-I{root / 'tools' / 'muse' / 'apphost'}"]
    own = [*MUSE_FLAGS, *(["-Werror"] if strict else []), *include]
    lua = [*lua_options(root), *(f"-D{define}" for define in lua_defines(root)), f"-I{lua_src}"]
    commands = [[*cc, *own, "-c", str(src), "-o", str(_object(obj_dir, "muse", src))] for src in apphost_sources(root)]
    commands += [[*cc, *lua, "-c", str(src), "-o", str(_object(obj_dir, "lua", src))] for src in lua_sources(root)]
    return commands


def link_command(cc, objects, exe):
    return [*cc, *(str(obj) for obj in objects), *LINK_FLAGS, "-o", str(exe)]


def _run(args):
    done = subprocess.run(args, capture_output=True, text=True, errors="replace")
    if done.returncode != 0:
        raise BuildError(f"{shlex.join(args)}\n{done.stdout}{done.stderr}".rstrip())
    return done.stderr


def build(dest, cc=None, strict=False, root=ROOT):
    """Compiles and links apphost into dest/apphost (objects in dest/obj), the files in parallel, and returns its path.
    BuildError, with the compiler's own words, when a command fails. A compile that only warns says so on stderr."""
    cc = cc or compiler()
    dest = Path(dest)
    obj_dir = dest / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    commands = compile_commands(cc, obj_dir, strict, root)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        futures = [pool.submit(_run, args) for args in commands]
    errors, warnings = [], []
    for future in futures:
        try:
            warnings.append(future.result())
        except BuildError as e:
            errors.append(str(e))
    if errors:
        raise BuildError("\n".join(errors))
    exe = dest / "apphost"
    warnings.append(_run(link_command(cc, [Path(args[args.index("-o") + 1]) for args in commands], exe)))
    if any(warnings):
        print("".join(warnings), end="", file=sys.stderr)
    return exe


# ---- The cache ----

def key_inputs(root=ROOT):
    """Every file apphost is built from: the sources it lists (its own and Lua's), every header beside them (all of
    components/muse/apps, tools/muse/apphost and Lua's src: a header one of them includes is among them, and so are the
    ones that no listed source includes yet), and the two files that say what is built and with what: sources.txt and
    Lua's CMakeLists.txt. A header is a regular file whose name doesn't start with '.': the compiler never includes an
    editor's dot-file (Emacs's lock file, `.#apps_vm.h`, is a link to nothing) or an AppleDouble `._x.h`, and reading one
    would fail the key, or change it for nothing."""
    root = Path(root).resolve()
    files = {*apphost_sources(root), *lua_sources(root), root / "tools" / "muse" / "apphost" / "sources.txt",
             root / "components" / "lua" / "CMakeLists.txt"}
    for directory in ("components/muse/apps", "tools/muse/apphost", "components/lua/src"):
        files.update(p for p in (root / directory).glob("*.h") if p.is_file() and not p.name.startswith("."))
    return sorted(files)


def cache_key(cc, root=ROOT):
    """The sha1 of key_inputs's names and contents, and of how they are built: the compiler command, the machine, the
    flags and this file, which says the rest (the -I list, anything put into a command in compile_commands or
    link_command)."""
    root = Path(root).resolve()
    digest = hashlib.sha1()
    for path in key_inputs(root):
        data = path.read_bytes()
        digest.update(f"{path.relative_to(root).as_posix()}\0{len(data)}\0".encode())
        digest.update(data)
    recipe = Path(__file__).read_bytes()
    digest.update(f"recipe\0{len(recipe)}\0".encode())
    digest.update(recipe)
    how = {"cc": shlex.join(cc), "system": platform.system(), "machine": platform.machine(), "muse": MUSE_FLAGS,
           "lua": [*lua_options(root), *lua_defines(root)], "link": LINK_FLAGS}
    digest.update(json.dumps(how, sort_keys=True).encode())
    return digest.hexdigest()


def cache_root():
    return Path.home() / ".cache" / "muse-apphost"


def cache_path(cc, root=ROOT):
    return cache_root() / cache_key(cc, root) / "apphost"


def _usable(exe):
    return exe.is_file() and os.access(exe, os.X_OK)


def ensure(cc=None, root=ROOT, progress=None):
    """The path of an apphost built from these sources, building it first when the cache has none. `progress(text)` is
    told when a build starts (it takes a few seconds). The build is made in a directory of its own in the cache and
    moved into place whole, so a build that fails, or is stopped, leaves nothing behind, and two that race end with
    one."""
    cc = cc or compiler()
    exe = cache_path(cc, root)
    if _usable(exe):
        return exe
    cache = cache_root()
    try:
        cache.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix=f".build-{exe.parent.name[:8]}-", dir=cache))
    except OSError as e:
        raise BuildError(f"can't make apphost in {cache}: {e.strerror or e}")
    try:
        if progress:
            progress(f"building apphost for this checkout into {exe.parent} (once, until its sources change)")
        build(work, cc, root=root)
        shutil.rmtree(work / "obj")
        if exe.parent.exists() and not _usable(exe):
            shutil.rmtree(exe.parent, ignore_errors=True)   # an entry that is no apphost (a copy without its mode bits)
        try:
            os.rename(work, exe.parent)
        except OSError as e:
            if not _usable(exe):   # unless another build got there first, the move itself failed
                raise BuildError(f"can't move the build into {exe.parent}: {e.strerror or e}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return exe
