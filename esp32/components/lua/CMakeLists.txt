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
# tests/apps_host.py reads this file for the host builds, so keep every source
# a quoted "src/<name>.c" and each setting in its own PRIVATE call below.
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
    # deep nesting needs more stack than the app task's 32 KB.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
    # Lua stops nested C calls and parser levels at 200 by default, which could
    # overflow the app task's 32 KB stack; 64 is the limit for it.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=64)
else()
    idf_component_register()
endif()
