<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Lua

The language Muse apps are written in (`CONFIG_MUSE_APPS`). It's Lua 5.4.9 from
lua.org as released, with its stock 64-bit integers and `double` floats.

| | |
|---|---|
| Upstream | https://www.lua.org/ftp/lua-5.4.9.tar.gz |
| Version | 5.4.9, released 2026-08-10 |
| SHA-256 | `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` (the tarball) |
| License | MIT, Lua.org, PUC-Rio's copyright. See [`LICENSE`](LICENSE). |

## What's upstream and what's ours

| File | From | License |
|---|---|---|
| `src/` | upstream's `src/`, unmodified, without `lua.c`, `luac.c` and `Makefile` | MIT |
| `LICENSE` | upstream: the MIT text at the end of `src/lua.h` | MIT |
| `CMakeLists.txt` | Meta | Apache-2.0 |
| `README.md` | Meta | Apache-2.0 |

`CMakeLists.txt` compiles Lua's core and the libraries apps get: `lauxlib`,
`lbaselib` (base), `lcorolib` (coroutine), `lmathlib` (math), `lstrlib`
(string), `ltablib` (table) and `lutf8lib` (utf8). It doesn't compile
`liolib.c`, `loslib.c`, `ldblib.c`, `loadlib.c` or `linit.c`, which stay in
`src/` untouched: apps get no io, os, debug or package library, and the apps
runtime opens the libraries it wants one at a time instead of through
`linit.c`. Without `CONFIG_MUSE_APPS` the component is empty.

Don't edit or restyle anything in `src/`, and don't add a Meta copyright
header to it. Set build options in `CMakeLists.txt` instead.

## Build settings

- `LUAI_MAXCCALLS=64` (Lua's default is 200) is the deepest nesting of C calls
  and parser levels. The Lua task's stack is 32 KB, which 200 levels could
  overflow.
- `-O2 -w`: upstream's code, warnings and all. Unoptimized, the deepest
  nesting needs more than that 32 KB as well.
- `LUA_COMPAT_5_3` isn't defined, though upstream's own Makefile defines it, so
  `math.atan2`, `cosh`, `sinh`, `tanh`, `pow`, `frexp`, `ldexp` and `log10` are
  absent.

`tests/apps_host.py` reads the source list, the definition and the options
from `CMakeLists.txt`, so the host tests build Lua the way the firmware does.
It adds only `-D_GNU_SOURCE`, `-pthread` and `luai_makeseed`, a fixed string
hash seed, so a table's keys come in the same order on every run. The device
keeps Lua's own seed.

## Updating

1. Download the new tarball into an empty directory, check its SHA-256 against
   the one on lua.org's download page, and extract it there.
2. Replace `src/` with the tarball's, leaving out `lua.c`, `luac.c` and
   `Makefile`. `diff -r` against the extracted `src/` must show only those
   three files.
3. Copy the license text at the end of `src/lua.h` into `LICENSE`.
4. Compare `CMakeLists.txt`'s sources with `CORE_O` and `LIB_O` in upstream's
   `src/Makefile`: new core files belong in the list.
5. Update the version, date and SHA-256 above, and the Lua rows in the
   license tables of the repository's `README.md` and `esp32/README.md`.
6. Run `tests/test_muse_apps_lua.py`, then the other `tests/test_muse_apps_*.py`.
