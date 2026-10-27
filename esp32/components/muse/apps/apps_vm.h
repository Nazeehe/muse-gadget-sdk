/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/*
 * The Lua state one Muse app runs in: a heap with a byte cap, the sandbox, and
 * loading of source text only. Portable C: the platform comes in through the config.
 *
 * The sandbox is the state's globals table (LUA_RIDX_GLOBALS), so a loaded chunk's
 * _ENV is the sandbox and a module installs with lua_setglobal. It holds base,
 * coroutine, table, string, math and utf8, less what an app mustn't have:
 * - base has no dofile, loadfile, load, collectgarbage, warn, require or print, and
 *   its setmetatable refuses a metatable with __gc (finalizers run with hooks off);
 * - string has no dump; match, gmatch and gsub raise (Lua's pattern matching
 *   backtracks without bound, out of reach of any hook); find searches plain text
 *   only, in linear time; split and trim are added.
 * Globals are strict: reading an undefined one raises "unknown global 'x'", with a
 * "did you mean" when a name is near, or advice for a library left out (io, debug,
 * load...); assigning one defines it. Strictness lives in the table's __index, so C
 * code reads globals with lua_rawget: lua_getglobal of a missing name raises.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"

typedef struct apps_vm apps_vm_t;

/* Every field is required. */
typedef struct {
    size_t mem_limit;                          /* bytes */
    void *(*realloc_fn)(void *ptr, size_t n);  /* device: PSRAM; host: realloc */
    void (*free_fn)(void *ptr);
    int64_t (*now_ms)(void);                   /* monotonic */
} apps_vm_config_t;

/* A state with the sandbox open. NULL if out of memory (realloc_fn failed, or the
 * libraries don't fit in mem_limit), or if cfg lacks a function. */
apps_vm_t *apps_vm_new(const apps_vm_config_t *cfg);
/* Closes the state and frees everything; NULL is ignored. */
void apps_vm_free(apps_vm_t *vm);
lua_State *apps_vm_lua(apps_vm_t *vm);

/*
 * Compiles len bytes of src, Lua source text, as chunkname: the file's bare name
 * ("main.lua", "lib/util.lua"), to which this adds Lua's '=' itself, so messages read
 * "main.lua:3: boom". Callers never add '=' or '@'. A leading UTF-8 BOM is skipped;
 * a binary chunk is refused with "binary chunks are not allowed: install Lua source
 * text". On success pushes the function (its _ENV is the sandbox) and returns true.
 * Otherwise pushes nothing, puts the message in err (cut to err_len on a character
 * boundary) and returns false.
 */
bool apps_vm_load(apps_vm_t *vm, const char *chunkname, const char *src, size_t len, char *err, size_t err_len);

/* The Lua heap against mem_limit: bytes held now, the most ever held, and how many
 * requests were refused (past the cap, or realloc_fn failing). A refusal returns NULL
 * to Lua, which collects garbage and retries before it raises LUA_ERRMEM. */
size_t apps_vm_mem_used(const apps_vm_t *vm);
size_t apps_vm_mem_peak(const apps_vm_t *vm);
size_t apps_vm_mem_refusals(const apps_vm_t *vm);

/*
 * "msg\n 41| …\n>42| …\n 43| …" for a "name:42: …" message about src (the first
 * ":<line>:" in msg is the line). Shows the lines before and after when they exist,
 * numbers right-aligned to the widest, each line cut at 80 bytes on a character
 * boundary. Lines end where Lua's lexer ends them (\n, \r, \r\n or \n\r), and a BOM
 * isn't shown. Without a line in msg, or for a line src doesn't have, it is msg alone.
 * Writes at most cap - 1 bytes and a NUL, cut on a character boundary; returns the
 * bytes written.
 */
size_t apps_vm_excerpt(const char *msg, const char *src, size_t len, char *out, size_t cap);
