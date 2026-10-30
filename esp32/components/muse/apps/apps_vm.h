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
 * - No library call runs an unbounded loop in C, since count hooks never fire
 *   there: rep of an empty string returns "" at once, and table's concat, move,
 *   insert, remove and sort refuse to walk more elements than the heap could hold
 *   (mem_limit / 16: a range or a __len with nothing behind it). A C function that
 *   walks without allocating anyway charges the walk to the call's budget with
 *   apps_vm_charge (json.encode: the slots lua_next visits, empty ones too; os.time: the days mktime walks).
 * Globals are strict: reading an undefined one raises "unknown global 'x'", with a
 * "did you mean" when a name is near, or advice for a library left out (io, debug,
 * load...); assigning one defines it. Strictness lives in the table's __index, so C
 * code reads globals with lua_rawget: lua_getglobal of a missing name raises.
 *
 * CPU and memory: the VM stops an app that runs away, and the gadget goes on. What a C function does in
 * between is bounded by the rules above and, past them, by the app task's supervisor (apps_task.h).
 * - A count hook runs every 1000 VM instructions, in every coroutine (lua_newthread
 *   copies it). A coroutine starts its own countdown, so creating one counts as 1000
 *   instructions: one that runs fewer is still paid for. Inside apps_vm_call, a call that
 *   has run more than its budget of instructions, or for longer than APPS_WALL_MS, stops at
 *   the next count.
 * - A stop is a kill, and a kill is for good: the VM is dying from then on, and no app
 *   code runs in it again. The kill is raised as an error app code can't keep: the
 *   sandbox's pcall, xpcall and coroutine resume, wrap and close raise it again once
 *   their call returns, and xpcall's handler is never called with it.
 * - Out of memory (ruling A2, amended): the allocator only refuses, and vm_alloc models Lua's
 *   one retry exactly, so a refusal becomes final the moment Lua will not turn it into a
 *   working allocation (lmem.c): out of memory is fatal regardless of what runs next. The next
 *   check then kills: the count hook, pcall, xpcall, coroutine create, resume, wrap and close,
 *   apps_vm_check and apps_vm_call. App code can still run while the error unwinds: a generic
 *   for's closing value is a to-be-closed slot made at run time with no OP_TBC, so it can't be
 *   refused at load the way a <close> variable is. It is bounded all the same: it runs with the
 *   hook on and can't swallow the error, which is already final, so the kill lands within about
 *   1000 pure-Lua instructions per closer (exactly 1000 for a table or string refusal; a refused
 *   stack growth can take one count per closer, bounded by the stack's room), and no binding acts in
 *   between, since each calls apps_vm_check first. (<close> is still refused at load and __gc is
 *   out, so this is the only such code.)
 * - A kill lands within 1000 VM instructions; a C function runs to its end first, which
 *   the bounds above keep short. Spec §8.2 re-arms the hook with a count of 1 at once:
 *   lua_sethook from another task isn't safe (ruling A15). Once the kill is raised, the
 *   thread re-arms its own hook with a count of 1, so app code reached while it unwinds (a
 *   generic for's closing value) stops before its first instruction.
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
    int64_t (*now_ms)(void);                   /* ms; only spans are taken of it (see below) */
} apps_vm_config_t;
/* now_ms is the clock of the 1 s wall backstop and of apps_vm_call_started_ms, which only ever take spans of it. The device's
 * (apps_rt.c's vm_clock_ms) is the monotonic clock less the time a snapshot's drawing took from the core (apps_rt_steal_ms), so
 * that the drawing isn't the call's: it steps back by a drawing's length once the drawing is booked, and isn't comparable with
 * apps_port_mono_ms. The host's is plain monotonic. */

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
 * text". A chunk that declares a to-be-closed variable is refused with
 * "<name>:<line>: '<close>' variables are not available in Muse apps" (see the CPU and
 * memory note above). On success pushes the function (its _ENV is the sandbox) and returns
 * true. Otherwise pushes nothing, puts the message in err (cut to err_len on a character
 * boundary) and returns false.
 */
bool apps_vm_load(apps_vm_t *vm, const char *chunkname, const char *src, size_t len, char *err, size_t err_len);

/*
 * apps_vm_load on the thread L, one of the VM's (its main thread, or a coroutine): the same rules, and the
 * function is pushed on L. apps_vm_load is this on the main thread.
 * Where the chunk compiles matters. The parser recurses in C, a level of Lua's C-call count (LUAI_MAXCCALLS)
 * for each level of nesting, and the count it adds to is the compiling thread's: a coroutine starts at its
 * resumer's count and adds a level for each C call under it, which the main thread's count leaves out. A
 * binding that loads for the code it was called from compiles on that thread, so the parse is bounded by the
 * C-call room that code has left, like every other recursion under app code.
 * An error the parser raises as a runtime error rather than a syntax error ("C stack overflow", for a source
 * nested past the room) has no position, and has the call's traceback appended when it arrives with
 * apps_vm_call's message handler (on the main thread, outside any pcall of app code): a caller that shows the
 * message cuts it at the first "\nstack traceback:".
 */
bool apps_vm_load_on(lua_State *L, const char *chunkname, const char *src, size_t len, char *err, size_t err_len);

/* The Lua heap against mem_limit: bytes held now, the most ever held, and how many
 * requests were refused (past the cap, or realloc_fn failing). A refusal returns NULL
 * to Lua, which may collect garbage and retry it once (never under gcstopem: a stack
 * growth or a lua_checkstack); one it won't retry, or whose retry is refused too, is
 * the app running out of memory (above). */
size_t apps_vm_mem_used(const apps_vm_t *vm);
size_t apps_vm_mem_peak(const apps_vm_t *vm);
size_t apps_vm_mem_refusals(const apps_vm_t *vm);

/* ---- Budgets and the kill ----
 *
 * Every function here runs on the task that runs the VM, except apps_vm_kill,
 * apps_vm_dying, apps_vm_in_call and apps_vm_call_started_ms, which any task may call,
 * though never during or after apps_vm_free: the caller keeps the VM alive for as long as
 * another task may use it.
 */

typedef enum { APPS_CALL_OK = 0, APPS_CALL_ERROR, APPS_CALL_KILLED, APPS_CALL_NOMEM } apps_call_t;
/* The 1.75C's VM runs 2.52 M instructions a second of the simplest loop with the count hook on (Task 27's bench fixture:
 * 2,521,392 to 2,524,423 over three runs), and about 2.1 M of table and call work. A callback gets about 300 ms of that and
 * main.lua's top level with start() about 750 ms, each rounded to 0.5 M: 756,872 -> 1 M (0.4 s), 1,892,180 -> 2 M (0.8 s).
 * Heavier work reaches APPS_WALL_MS first. */
#define APPS_BUDGET_CALL  1000000u   /* VM instructions per callback */
#define APPS_BUDGET_START 2000000u   /* load + start() */
#define APPS_WALL_MS      1000

/*
 * Calls the function on top of the stack, below its nargs arguments, with at most
 * budget VM instructions and APPS_WALL_MS of time.
 * - APPS_CALL_OK: its nresults results (all of them for LUA_MULTRET) replace the function
 *   and the arguments; err is "".
 * - Otherwise the function and the arguments are popped and err says why, cut to err_len
 *   on a character boundary:
 *   - APPS_CALL_ERROR: "message\nstack traceback:\n\t…", the app's frames only (no "[C]:"
 *     or "(...tail calls...)" line), the message cut at 1 KB;
 *   - APPS_CALL_KILLED, APPS_CALL_NOMEM: the kill's reason, with no traceback. A dying VM
 *     returns one of them at once and runs nothing.
 * - No function below the arguments (nargs < 0, or nargs >= lua_gettop): APPS_CALL_ERROR,
 *   err "apps_vm_call: no function to call", and the stack as it was.
 * For the C code that drives the VM: it runs on the VM's main thread. A C function Lua
 * calls runs Lua with lua_call, under the call in progress. A call nested anyway (from such
 * a function, on the main thread) runs under the outer call's budget and time.
 */
apps_call_t apps_vm_call(apps_vm_t *vm, int nargs, int nresults, uint32_t budget, char *err, size_t err_len);

/* Kills the VM: from now on it is dying, with reason (a static string) as apps_vm_call's
 * err. A running call stops within 1000 VM instructions. Any task: it only stores the
 * reason. A kill of a dying VM changes nothing, and when kills race, one of their reasons
 * stays; once set, the reason is never NULL again. */
void apps_vm_kill(apps_vm_t *vm, const char *reason);
/* Whether the VM has been killed. Any task. Running out of memory becomes the kill at the
 * next check on the VM's task, so until then this can trail it. */
bool apps_vm_dying(const apps_vm_t *vm);
/* The modules and bindings Muse gives apps call this first, and so do the sandbox's split,
 * trim, pcall, xpcall and coroutine functions; the rest of the library starts no work outside
 * the VM and doesn't. When the VM is dying, or out of memory, it raises the kill, so a binding
 * starts no work for a dying app. It pushes one value before raising, which the slots Lua
 * guarantees a C function hold. */
void apps_vm_check(lua_State *L);
/*
 * Charges n VM instructions to the call and to the VM's lifetime, as the count hook does for the
 * instructions it counts, and then checks as the hook does: inside apps_vm_call, a call that is
 * over its budget or over APPS_WALL_MS is killed; at any time, a dying VM (out of memory
 * included) raises its kill. For a C function that loops where no count hook fires: it charges
 * the work it is about to do, in the VM's own unit (json.encode charges the slots of each table
 * it walks, empty or not), and calls with 0 now and then in a long loop, so the clock is read
 * there too. It raises the kill as apps_vm_check does, so it needs one free stack slot, and
 * otherwise returns.
 */
void apps_vm_charge(lua_State *L, unsigned n);
/*
 * The slots lua_next walks in the table at idx: the allocated size of its array part plus that of
 * its hash part. They stay when the table is emptied, so a walk costs what the table once held,
 * not what it holds now. 0 for a value that isn't a table. It reads the pinned Lua 5.4.9's Table.
 */
unsigned apps_vm_table_slots(lua_State *L, int idx);
/* Whether apps_vm_call is running, and since when (now_ms() when the outermost call
 * began: the VM's clock, which on the device isn't apps_port_mono_ms). Any task: the
 * supervisor reads them to spot a hung app. Read in_call first,
 * then the start: apps_vm_call stores the start before it sets in_call, so a reader that
 * saw in_call true gets that call's start, or a later call's. */
bool apps_vm_in_call(const apps_vm_t *vm);
int64_t apps_vm_call_started_ms(const apps_vm_t *vm);
/* VM instructions run in the VM's lifetime, counted 1000 at a time by the hook, 1000
 * for each coroutine created, and what apps_vm_charge charges. */
uint64_t apps_vm_instructions(const apps_vm_t *vm);

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
