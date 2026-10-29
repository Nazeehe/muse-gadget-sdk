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

# Lua 5.4.9, the interpreter behind Muse apps (CONFIG_MUSE_APPS). See README.md.
# The core and the libraries apps get: base, coroutine, math, string, table and
# utf8. Not io, os, debug or package, and not linit.c (the apps runtime opens
# the libraries one at a time), nor the lua and luac programs.
#
# tools/muse/apphost_build.py reads this file for the host builds (tests/apps_host.py
# and tools/muse/app.py check build with what it finds) and doesn't evaluate
# if(): keep every source a quoted "src/<name>.c", all the definitions in the one
# target_compile_definitions call and all the options in the one
# target_compile_options call, both PRIVATE.
if(CONFIG_MUSE_APPS)
    idf_component_register(
        SRCS
            "src/lapi.c" "src/lcode.c" "src/lctype.c" "src/ldebug.c" "src/ldo.c"
            "src/ldump.c" "src/lfunc.c" "src/lgc.c" "src/llex.c" "src/lmem.c"
            "src/lobject.c" "src/lopcodes.c" "src/lparser.c" "src/lstate.c"
            "src/lstring.c" "src/ltable.c" "src/ltm.c" "src/lundump.c" "src/lvm.c"
            "src/lzio.c"
            "src/lauxlib.c" "src/lbaselib.c" "src/lcorolib.c" "src/lmathlib.c"
            "src/lstrlib.c" "src/ltablib.c" "src/lutf8lib.c"
        INCLUDE_DIRS "src")
    # Upstream code, warnings and all. -O2, as the host tests build it: unoptimized,
    # deep nesting needs more stack than the app task's 56 KB.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
    # How deep C calls and parser levels may nest before Lua raises "C stack
    # overflow" (200 by default). The app task runs on a 56 KB stack in PSRAM
    # (57,344 bytes), and error handling may go on to 1.1 times this limit, 44
    # levels. Measured on the S3 with tests/muse_apps_fixtures/torture, which
    # tools/muse/app.py pushes: the four C-call chains (pcall, coroutine.resume,
    # __index through table.concat, __tostring through string.format) run to the
    # limit with a 16-deep json.encode and json.decode, each ending in a float, at
    # the bottom of each. The worst of them used 42,076 bytes of the stack and left
    # 15,268 free. Lua-to-Lua calls don't count. README.md has the rest.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=40)
else()
    idf_component_register()
endif()
