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

/*
 * Checks apps_vm, the Lua state Muse apps run in, and apps_suggest, its did-you-mean
 * rule. Usage: muse_apps_vm_harness <case>, one case per test in
 * tests/test_muse_apps_vm.py. Exits 0 when the case holds and prints what it got when
 * it doesn't. Every case frees what it made, and the counting allocator below must
 * end at 0 bytes: that is the leak check, since LeakSanitizer doesn't run on macOS.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "apps_suggest.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

static int failures;

#define CHECK_AT(line, cond, ...)                                  \
    do {                                                           \
        if (!(cond)) {                                             \
            failures++;                                            \
            fprintf(stderr, "FAIL line %d: %s: ", (line), #cond);  \
            fprintf(stderr, __VA_ARGS__);                          \
            fputc('\n', stderr);                                   \
        }                                                          \
    } while (0)
#define CHECK(cond, ...) CHECK_AT(__LINE__, cond, __VA_ARGS__)

#define KB ((size_t)1024)
#define MB (1024 * KB)

/* The exact messages (the brief's, and rulings A17 and X10). Raised from a chunk, they
 * start with "main.lua:<line>: ". */
#define BINARY_MSG "binary chunks are not allowed: install Lua source text"
#define GC_MSG "setmetatable: __gc finalizers are not available in Muse apps"
#define FIND_MSG                                                                          \
    "string.find: patterns are not available in Muse apps (api 1); for a plain search " \
    "pass true as the 4th argument"
#define NO_PATTERNS(fn)                                                                   \
    "string." fn " is not available in Muse apps (api 1): use string.find(s, text, 1, " \
    "true), string.split or json.decode"
#define NO_LOADER(fn) "'" fn "' is not available in Muse apps: code comes only from the app's own files"

/* ---- The allocator apps_vm gets: realloc and free, counting what they hold ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;            /* bytes allocated and not freed */
static long grants_left = -1;  /* growth requests to grant before refusing all; -1: no limit */
static bool refuse_next;       /* refuse the next growth request, once */

static void *count_realloc(void *ptr, size_t n) {
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old && refuse_next) {
        refuse_next = false;
        return NULL;
    }
    if (n > old && grants_left >= 0) {
        if (grants_left == 0) {
            return NULL;
        }
        grants_left--;
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    held = held - old + n;
    return nb + 1;
}

static void count_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        free(b);
    }
}

/* The VM's clock: each read returns fake_ms, then moves it on by fake_step (0 but in the
 * wall-clock case). */
static int64_t fake_ms, fake_step;

static int64_t fake_now_ms(void) {
    int64_t now = fake_ms;
    fake_ms += fake_step;
    return now;
}

static apps_vm_t *new_vm(size_t mem_limit) {
    apps_vm_config_t cfg = {
        .mem_limit = mem_limit,
        .realloc_fn = count_realloc,
        .free_fn = count_free,
        .now_ms = fake_now_ms,
    };
    return apps_vm_new(&cfg);
}

/* ---- Running chunks ---- */

static const char *top_text(lua_State *L) {
    const char *text = lua_gettop(L) > 0 ? lua_tostring(L, -1) : NULL;
    return text != NULL ? text : "(not a string)";
}

/* The stack as "type:value, ...", for a failure message. */
static const char *describe(lua_State *L, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = 1; i <= lua_gettop(L); i++) {
        size_t used = strlen(out);
        const char *type = luaL_typename(L, i);
        const char *text;
        switch (lua_type(L, i)) {
            case LUA_TNIL: text = "nil"; break;
            case LUA_TBOOLEAN: text = lua_toboolean(L, i) ? "true" : "false"; break;
            case LUA_TNUMBER:
            case LUA_TSTRING: text = lua_tostring(L, i); break;
            default: text = "?"; break;
        }
        snprintf(out + used, cap - used, "%s%s:%s", i > 1 ? ", " : "", type, text);
    }
    return out;
}

/* Loads src as main.lua and calls it. Returns lua_pcall's status, or -1 when apps_vm_load
 * refused src (err says why). The results, or the error, stay on the stack. */
static int run(apps_vm_t *vm, const char *src, int nresults, char *err, size_t err_len) {
    if (!apps_vm_load(vm, "main.lua", src, strlen(src), err, err_len)) {
        return -1;
    }
    return lua_pcall(apps_vm_lua(vm), 0, nresults, 0);
}

/* src raises exactly want. */
static void expect_error_at(int line, apps_vm_t *vm, const char *src, const char *want) {
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    int rc = run(vm, src, 0, err, sizeof err);
    CHECK_AT(line, rc == LUA_ERRRUN && strcmp(top_text(L), want) == 0,
             "%s\n  want LUA_ERRRUN '%s'\n  got rc=%d '%s'", src, want, rc, rc == -1 ? err : top_text(L));
    lua_settop(L, 0);
}
#define expect_error(vm, src, want) expect_error_at(__LINE__, vm, src, want)

/* src returns true. A chunk that checks several things raises a message naming the one
 * that failed. */
static void expect_true_at(int line, apps_vm_t *vm, const char *src) {
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "", got[512];
    int rc = run(vm, src, 1, err, sizeof err);
    CHECK_AT(line, rc == LUA_OK && lua_isboolean(L, -1) && lua_toboolean(L, -1),
             "%s\n  want true, got rc=%d %s", src, rc,
             rc == -1 ? err : rc != LUA_OK ? top_text(L) : describe(L, got, sizeof got));
    lua_settop(L, 0);
}
#define expect_true(vm, src) expect_true_at(__LINE__, vm, src)

/* src returns the n integers in want. */
static void expect_ints_at(int line, apps_vm_t *vm, const char *src, int n, const lua_Integer *want) {
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "", got[512];
    int rc = run(vm, src, LUA_MULTRET, err, sizeof err);
    bool ok = rc == LUA_OK && lua_gettop(L) == n;
    for (int i = 0; ok && i < n; i++) {
        ok = lua_isinteger(L, i + 1) && lua_tointeger(L, i + 1) == want[i];
    }
    CHECK_AT(line, ok, "%s\n  got rc=%d %s", src, rc,
             rc == -1 ? err : rc != LUA_OK ? top_text(L) : describe(L, got, sizeof got));
    lua_settop(L, 0);
}
#define expect_ints(vm, src, ...)                                                  \
    expect_ints_at(__LINE__, vm, src, (int)(sizeof((lua_Integer[]){__VA_ARGS__}) / \
                                            sizeof(lua_Integer)),                  \
                   (lua_Integer[]){__VA_ARGS__})

/* The global name's value, read raw from C (lua_getglobal would raise the strict error). */
static int raw_global(lua_State *L, const char *name) {
    lua_pushglobaltable(L);
    lua_pushstring(L, name);
    int type = lua_rawget(L, -2);
    lua_remove(L, -2);
    return type;
}

/* ---- bytecode ---- */

typedef struct {
    char buf[4096];
    size_t n;
} dump_t;

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    dump_t *d = ud;
    if (sz > sizeof d->buf - d->n) {
        return 1;
    }
    memcpy(d->buf + d->n, p, sz);
    d->n += sz;
    return 0;
}

static void bytecode(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[256];

    /* The binary chunk header, then a chunk compiled and dumped by a stock state, with and
     * without a BOM in front. */
    static const char header[] = "\x1bLua\x54\x00\x19\x93\r\n\x1a\nrest";
    strcpy(err, "(untouched)");
    CHECK(!apps_vm_load(vm, "main.lua", header, sizeof header - 1, err, sizeof err) && strcmp(err, BINARY_MSG) == 0,
          "got '%s'", err);
    CHECK(lua_gettop(L) == 0, "a refused load left %d values on the stack", lua_gettop(L));

    lua_State *stock = luaL_newstate();
    static dump_t dump = {.n = 3};
    memcpy(dump.buf, "\xEF\xBB\xBF", 3);
    CHECK(luaL_loadstring(stock, "return 1") == LUA_OK && lua_dump(stock, dump_writer, &dump, 0) == 0
              && memcmp(dump.buf + 3, LUA_SIGNATURE, 4) == 0,
          "couldn't make bytecode: %s", top_text(stock));
    lua_close(stock);
    strcpy(err, "(untouched)");
    CHECK(!apps_vm_load(vm, "main.lua", dump.buf + 3, dump.n - 3, err, sizeof err) && strcmp(err, BINARY_MSG) == 0,
          "dumped chunk: got '%s'", err);
    strcpy(err, "(untouched)");
    CHECK(!apps_vm_load(vm, "main.lua", dump.buf, dump.n, err, sizeof err) && strcmp(err, BINARY_MSG) == 0,
          "BOM and dumped chunk: got '%s'", err);
    CHECK(lua_gettop(L) == 0, "refused loads left %d values on the stack", lua_gettop(L));

    /* Source text still loads, and the message fits a small err buffer. */
    expect_ints(vm, "return 7", 7);
    char small[8];
    CHECK(!apps_vm_load(vm, "main.lua", header, sizeof header - 1, small, sizeof small) && strcmp(small, "binary ") == 0,
          "small buffer: got '%s'", small);
    apps_vm_free(vm);
}

/* ---- bom_crlf ---- */

static void bom_crlf(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[256];

    expect_ints(vm, "\xEF\xBB\xBFlocal x = 1\r\nreturn x\r\n", 1);

    /* Each CRLF is one line, and the chunk is named main.lua (X10), not [string "main.lua"]. */
    expect_error(vm, "\xEF\xBB\xBF-- one\r\n-- two\r\nerror('boom')\r\n", "main.lua:3: boom");
    int top = lua_gettop(L);
    static const char bad[] = "x = 1\r\nif then\r\n";
    CHECK(!apps_vm_load(vm, "main.lua", bad, sizeof bad - 1, err, sizeof err)
              && strcmp(err, "main.lua:2: unexpected symbol near 'then'") == 0,
          "got '%s'", err);
    CHECK(lua_gettop(L) == top, "a failed load changed the stack from %d to %d", top, lua_gettop(L));

    /* The name is the caller's bare file name. */
    CHECK(apps_vm_load(vm, "lib/util.lua", "error('x')", 10, err, sizeof err), "%s", err);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN && strcmp(top_text(L), "lib/util.lua:1: x") == 0, "got '%s'",
          top_text(L));
    lua_settop(L, 0);

    /* Past the LUA_IDSIZE - 1 bytes Lua shows, a name keeps its head, as an '=' name does
     * (an '@' name would show "..." and its tail). */
    char long_name[80], want[128];
    memcpy(long_name, "lib/", 4);
    for (int i = 4; i < 74; i++) {
        long_name[i] = (char)('a' + i % 26);
    }
    long_name[74] = '\0';
    snprintf(want, sizeof want, "%.*s:1: x", LUA_IDSIZE - 1, long_name);
    CHECK(apps_vm_load(vm, long_name, "error('x')", 10, err, sizeof err), "%s", err);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN && strcmp(top_text(L), want) == 0, "got '%s', want '%s'",
          top_text(L), want);
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- gc_metatable ---- */

static void gc_metatable(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);

    expect_error(vm, "setmetatable({}, {__gc = function() end})", "main.lua:1: " GC_MSG);
    expect_error(vm, "setmetatable({}, {__gc = false})", "main.lua:1: " GC_MSG);

    /* Everything else setmetatable does is unchanged. */
    expect_true(vm, "local t = setmetatable({}, {__index = function(_, k) return k .. '!' end})\n"
                    "return t.hi == 'hi!' and getmetatable(setmetatable(t, nil)) == nil");
    expect_error(vm, "local t = setmetatable({}, {__metatable = 'locked'}) setmetatable(t, {})",
                 "main.lua:1: cannot change a protected metatable");
    expect_error(vm, "setmetatable(1, {})", "main.lua:1: bad argument #1 to 'setmetatable' (table expected, got number)");
    expect_error(vm, "setmetatable({}, 1)",
                 "main.lua:1: bad argument #2 to 'setmetatable' (nil or table expected, got number)");

    /* A __gc added after setmetatable never runs: Lua marks objects for finalization only
     * in setmetatable, which is what makes checking there enough. */
    expect_true(vm, "local mt = {} local t = setmetatable({}, mt)\n"
                    "mt.__gc = function() finalized = true end t = nil return true");
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(raw_global(L, "finalized") == LUA_TNIL, "a __gc added later ran");
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- strict_globals ---- */

static void strict_globals(void) {
    apps_vm_t *vm = new_vm(256 * KB);

    /* The brief's three. */
    expect_error(vm, "local digits = 1 return digts", "main.lua:1: unknown global 'digts' (did you mean 'digits'?)");
    expect_error(vm, "return io", "main.lua:1: 'io' is not available in Muse apps; use store / sys.log");
    expect_ints(vm, "count = 1 return count", 1);

    /* Every name left out on purpose has its own message (A17: each loader its own name). */
    expect_error(vm, "return debug", "main.lua:1: 'debug' is not available in Muse apps");
    expect_error(vm, "return load", "main.lua:1: " NO_LOADER("load"));
    expect_error(vm, "return loadstring", "main.lua:1: " NO_LOADER("loadstring"));
    expect_error(vm, "return dofile", "main.lua:1: " NO_LOADER("dofile"));
    expect_error(vm, "return loadfile", "main.lua:1: " NO_LOADER("loadfile"));
    expect_error(vm, "return collectgarbage", "main.lua:1: 'collectgarbage' is not available in Muse apps; use sys.mem()");

    /* No suggestion: nothing near, or a name too short for any (distance < strlen). */
    expect_error(vm, "return x", "main.lua:1: unknown global 'x'");
    expect_error(vm, "local y = 1 return x", "main.lua:1: unknown global 'x'");
    expect_error(vm, "local ab = 1 return ac", "main.lua:1: unknown global 'ac' (did you mean 'ab'?)");
    expect_error(vm, "local abc = 1 return xyz", "main.lua:1: unknown global 'xyz'");

    /* Candidates: upvalues of the reading function, the sandbox's own names and the app's
     * globals. Locals come first, so they win ties ('nexy' and the global 'next' are both
     * one edit from 'nexx'). */
    expect_error(vm, "local total = 0\nlocal function add(n)\n  total = total + n\n  return totl\nend\nreturn add(1)",
                 "main.lua:4: unknown global 'totl' (did you mean 'total'?)");
    expect_error(vm, "return pairz", "main.lua:1: unknown global 'pairz' (did you mean 'pairs'?)");
    expect_error(vm, "velocity = 1 return velocty", "main.lua:1: unknown global 'velocty' (did you mean 'velocity'?)");
    expect_error(vm, "local nexy = 1 return nexx", "main.lua:1: unknown global 'nexx' (did you mean 'nexy'?)");
    /* Lua's "(temporary)" slots are never suggested. */
    expect_error(vm, "return temporary", "main.lua:1: unknown global 'temporary'");
    /* A local of another function isn't a candidate. */
    expect_error(vm, "local function f()\n  return digts\nend\nlocal digits = 1\nreturn f()",
                 "main.lua:2: unknown global 'digts'");

    /* Only names are strict; raw reads never are. */
    expect_true(vm, "return _G[1] == nil and _G[true] == nil and rawget(_G, 'nope') == nil");
    expect_true(vm, "return rawget(_G, 'count') == 1 and _G.count == 1");
    apps_vm_free(vm);
}

/* ---- registry_globals (ruling X9) ---- */

static int twice(lua_State *L) {
    lua_pushinteger(L, 2 * luaL_checkinteger(L, 1));
    return 1;
}

static void registry_globals(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);

    lua_pushinteger(L, 42);
    lua_setglobal(L, "answer");
    expect_ints(vm, "return answer", 42);

    /* A module installed the way Tasks 6-18 install theirs. */
    lua_newtable(L);
    lua_pushcfunction(L, twice);
    lua_setfield(L, -2, "twice");
    lua_setglobal(L, "mod");
    expect_ints(vm, "return mod.twice(21)", 42);

    expect_true(vm, "return _ENV == _G and _G._G == _G");
    lua_pushglobaltable(L);
    CHECK(lua_getmetatable(L, -1) == 1, "the globals table has no metatable: globals aren't strict");
    lua_settop(L, 0);

    /* A global a chunk defines is the one C sees, and C reads a missing one as nil. */
    expect_true(vm, "made = 'here' return true");
    CHECK(raw_global(L, "made") == LUA_TSTRING && strcmp(lua_tostring(L, -1), "here") == 0, "C reads %s",
          luaL_typename(L, -1));
    lua_settop(L, 0);
    CHECK(raw_global(L, "nope") == LUA_TNIL, "a missing global reads %s", luaL_typename(L, -1));
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- sandbox_contents ---- */

static void sandbox_contents(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[256] = "";

    static const char src[] =
        "local function keys(t)\n"
        "  local out = {}\n"
        "  for k in pairs(t) do out[#out + 1] = tostring(k) end\n"
        "  table.sort(out)\n"
        "  return table.concat(out, ' ')\n"
        "end\n"
        "return keys(_G), keys(string)";
    int rc = run(vm, src, 2, err, sizeof err);
    CHECK(rc == LUA_OK, "rc=%d %s", rc, rc == -1 ? err : top_text(L));
    if (rc == LUA_OK) {
        /* Base without dofile loadfile load collectgarbage warn require print, setmetatable
         * wrapped, pcall and xpcall replaced (kill-safe); the five libraries. */
        static const char globals[] =
            "_G _VERSION assert coroutine error getmetatable ipairs math next pairs pcall rawequal rawget "
            "rawlen rawset select setmetatable string table tonumber tostring type utf8 xpcall";
        /* string without dump, with split and trim. */
        static const char strings[] =
            "byte char find format gmatch gsub len lower match pack packsize rep reverse split sub trim unpack upper";
        CHECK(strcmp(lua_tostring(L, 1), globals) == 0, "globals\n  want %s\n  got  %s", globals, lua_tostring(L, 1));
        CHECK(strcmp(lua_tostring(L, 2), strings) == 0, "string\n  want %s\n  got  %s", strings, lua_tostring(L, 2));
    }
    lua_settop(L, 0);
    expect_true(vm, "return _VERSION == 'Lua 5.4' and string.dump == nil");
    apps_vm_free(vm);
}

/* ---- patterns ---- */

enum { STOCK_FUNCS = 5 };

/* lstrlib's ways into its pattern matcher: find, match, gmatch, gsub, and gmatch's
 * iterator. A stock state's string table holds the same C functions. */
static void stock_pattern_functions(lua_CFunction out[STOCK_FUNCS]) {
    static const char *const names[] = {"find", "match", "gmatch", "gsub"};
    lua_State *S = luaL_newstate();
    luaL_requiref(S, LUA_STRLIBNAME, luaopen_string, 1);
    for (int i = 0; i < 4; i++) {
        lua_getfield(S, -1, names[i]);
        out[i] = lua_tocfunction(S, -1);
        lua_pop(S, 1);
    }
    lua_getfield(S, -1, "gmatch");
    lua_pushliteral(S, "a");
    lua_pushliteral(S, "a");
    lua_call(S, 2, 1);
    out[4] = lua_tocfunction(S, -1);
    lua_close(S);
}

/* How many of the values reachable from the one at idx (through table keys and values,
 * metatables and closure upvalues) are among bad. seen is the absolute index of a table of
 * what has been visited. */
static int reachable(lua_State *L, int idx, const lua_CFunction *bad, int nbad, int seen) {
    idx = lua_absindex(L, idx);
    int type = lua_type(L, idx);
    if (type != LUA_TTABLE && type != LUA_TFUNCTION && type != LUA_TUSERDATA) {
        return 0;
    }
    lua_pushvalue(L, idx);
    bool visited = lua_rawget(L, seen) != LUA_TNIL;
    lua_pop(L, 1);
    if (visited) {
        return 0;
    }
    lua_pushvalue(L, idx);
    lua_pushboolean(L, 1);
    lua_rawset(L, seen);
    if (!lua_checkstack(L, 4)) {
        failures++;
        fputs("FAIL: reachable: no Lua stack left\n", stderr);
        return 0;
    }
    int hits = 0;
    if (type == LUA_TFUNCTION) {
        lua_CFunction f = lua_tocfunction(L, idx);
        for (int i = 0; i < nbad; i++) {
            hits += f != NULL && f == bad[i];
        }
        for (int i = 1; lua_getupvalue(L, idx, i) != NULL; i++) {
            hits += reachable(L, -1, bad, nbad, seen);
            lua_pop(L, 1);
        }
    } else if (type == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            hits += reachable(L, -2, bad, nbad, seen);
            hits += reachable(L, -1, bad, nbad, seen);
            lua_pop(L, 1);
        }
    }
    if (lua_getmetatable(L, idx)) {
        hits += reachable(L, -1, bad, nbad, seen);
        lua_pop(L, 1);
    }
    return hits;
}

/* Everything a state's code could reach: its registry (globals, loaded modules) and the
 * string metatable, which lives outside the registry. */
static int reachable_in_state(lua_State *L, const lua_CFunction *bad, int nbad) {
    lua_newtable(L);
    int seen = lua_gettop(L);
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    int hits = reachable(L, -1, bad, nbad, seen);
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) {
        hits += reachable(L, -1, bad, nbad, seen);
    }
    lua_settop(L, seen - 1);
    return hits;
}

static void patterns(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);

    /* The brief's five. */
    expect_error(vm, "return ('a,b'):match(',')", "main.lua:1: " NO_PATTERNS("match"));
    expect_ints(vm, "return ('a.b'):find('.', 1, true)", 2, 2);
    expect_ints(vm, "return ('a,b'):find(',')", 2, 2);
    expect_error(vm, "return ('ab'):find('a+')", "main.lua:1: " FIND_MSG);
    expect_true(vm, "return string.dump == nil");

    /* Pattern recursion stays unreachable: gsub, gmatch and a pattern find through string,
     * method syntax, a saved reference and the string metatable. */
    static const struct {
        const char *src, *want;
    } routes[] = {
        {"return string.gsub('aaa', 'a', 'b')", "main.lua:1: " NO_PATTERNS("gsub")},
        {"return string.gmatch('aaa', 'a')", "main.lua:1: " NO_PATTERNS("gmatch")},
        {"return string.match('aaa', 'a')", "main.lua:1: " NO_PATTERNS("match")},
        {"return string.find('aaa', 'a*')", "main.lua:1: " FIND_MSG},
        {"local s = 'aaa' return s:gsub('a', 'b')", "main.lua:1: " NO_PATTERNS("gsub")},
        {"local s = 'aaa' return s:gmatch('a')", "main.lua:1: " NO_PATTERNS("gmatch")},
        {"local s = 'aaa' return s:find('(a)')", "main.lua:1: " FIND_MSG},
        {"local g = string.gsub return g('aaa', 'a', 'b')", "main.lua:1: " NO_PATTERNS("gsub")},
        {"local g = string.gmatch return g('aaa', 'a')", "main.lua:1: " NO_PATTERNS("gmatch")},
        {"local f = string.find return f('aaa', '%a')", "main.lua:1: " FIND_MSG},
        {"local f = string.find return f('aaa', '.', 1, false)", "main.lua:1: " FIND_MSG},
        {"local f = string.find return f('aaa', '.', 1, nil)", "main.lua:1: " FIND_MSG},
        {"return getmetatable('').__index.gsub('aaa', 'a', 'b')", "main.lua:1: " NO_PATTERNS("gsub")},
        {"return getmetatable('').__index.gmatch('aaa', 'a')", "main.lua:1: " NO_PATTERNS("gmatch")},
        {"return getmetatable('').__index.find('aaa', '^a')", "main.lua:1: " FIND_MSG},
        {"return ('aaa'):gsub('a', {a = 'b'})", "main.lua:1: " NO_PATTERNS("gsub")},
        {"return ('aaa'):gsub('a', function() end)", "main.lua:1: " NO_PATTERNS("gsub")},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        expect_error_at(__LINE__, vm, routes[i].src, routes[i].want);
    }
    /* A guard raises before it looks at its arguments. */
    expect_error(vm, "return string.gsub()", "main.lua:1: " NO_PATTERNS("gsub"));

    /* And none of lstrlib's pattern functions is reachable from the state at all (the scan
     * finds them in a stock state, so it would find them here). */
    lua_CFunction stock[STOCK_FUNCS];
    stock_pattern_functions(stock);
    CHECK(reachable_in_state(L, stock, STOCK_FUNCS) == 0, "lstrlib's pattern functions are reachable");
    lua_State *S = luaL_newstate();
    luaL_requiref(S, LUA_STRLIBNAME, luaopen_string, 1);
    lua_settop(S, 0);
    int found = reachable_in_state(S, stock, STOCK_FUNCS);
    CHECK(found >= 4, "the scan found %d of the stock functions in a stock state", found);
    lua_close(S);
    CHECK(lua_gettop(L) == 0, "the scan left %d values", lua_gettop(L));
    apps_vm_free(vm);
}

/* ---- plain_find ---- */

static void plain_find(void) {
    apps_vm_t *vm = new_vm(1 * MB);
    lua_State *L = apps_vm_lua(vm);
    lua_pushliteral(L, FIND_MSG);
    lua_setglobal(L, "FIND_MSG");

    /* Stock Lua 5.4's results for a plain find, init and empty patterns included. */
    expect_true(vm,
                "local function check(what, w1, w2, ...)\n"
                "  local g1, g2 = ...\n"
                "  if g1 ~= w1 or g2 ~= w2 then\n"
                "    error(what .. ': got ' .. tostring(g1) .. ' ' .. tostring(g2) ..\n"
                "          ', want ' .. tostring(w1) .. ' ' .. tostring(w2), 2)\n"
                "  end\n"
                "end\n"
                "check('l', 3, 3, ('hello'):find('l'))\n"
                "check('l from 4', 4, 4, ('hello'):find('l', 4))\n"
                "check('l from 5', nil, nil, ('hello'):find('l', 5))\n"
                "check('lo from -2', 4, 5, ('hello'):find('lo', -2))\n"
                "check('h from -10', 1, 1, ('hello'):find('h', -10))\n"
                "check('h from 0', 1, 1, ('hello'):find('h', 0))\n"
                "check('empty', 1, 0, ('hello'):find(''))\n"
                "check('empty from 3', 3, 2, ('hello'):find('', 3))\n"
                "check('empty from 6', 6, 5, ('hello'):find('', 6))\n"
                "check('empty from 7', nil, nil, ('hello'):find('', 7))\n"
                "check('absent', nil, nil, ('hello'):find('xyz'))\n"
                "check('longer', nil, nil, ('hello'):find('hello!'))\n"
                "check('restart', 2, 3, ('aab'):find('ab'))\n"
                "check('fallback', 3, 7, ('abababc'):find('ababc'))\n"
                "check('whole', 1, 5, ('hello'):find('hello'))\n"
                "check('plain +', 2, 2, ('a+b'):find('+', 1, true))\n"
                "check('truthy 4th', 2, 2, ('a.b'):find('.', 1, 1))\n"
                "check('nul', 2, 3, ('x\\0y'):find('\\0y'))\n"
                "check('numbers', 2, 2, string.find(12321, 2))\n"
                "check('percent', 1, 2, ('%d'):find('%d', 1, true))\n"
                "local specials = '^$*+?.([%-'\n"
                "for i = 1, #specials do\n"
                "  local c = specials:sub(i, i)\n"
                "  local ok, msg = pcall(string.find, 'x' .. c, c)\n"
                "  if ok or msg ~= FIND_MSG then error('pattern ' .. c .. ': ' .. tostring(msg)) end\n"
                "  check('plain ' .. c, 2, 2, ('x' .. c):find(c, 1, true))\n"
                "end\n"
                "return true");

    /* KMP against the obvious search, on strings of a and b, where partial matches abound. */
    expect_true(vm,
                "math.randomseed(4242)\n"
                "local function naive(s, p, init)\n"
                "  local ls = #s\n"
                "  if init < 0 then init = math.max(ls + init + 1, 1) elseif init == 0 then init = 1 end\n"
                "  if init > ls + 1 then return nil end\n"
                "  for i = init, ls - #p + 1 do\n"
                "    if s:sub(i, i + #p - 1) == p then return i, i + #p - 1 end\n"
                "  end\n"
                "  return nil\n"
                "end\n"
                "local function word(n)\n"
                "  local t = {}\n"
                "  for i = 1, n do t[i] = math.random(2) == 1 and 'a' or 'b' end\n"
                "  return table.concat(t)\n"
                "end\n"
                "for trial = 1, 4000 do\n"
                "  local s, p, init = word(math.random(0, 14)), word(math.random(0, 6)), math.random(-16, 16)\n"
                "  local w1, w2 = naive(s, p, init)\n"
                "  local g1, g2 = s:find(p, init, true)\n"
                "  if g1 ~= w1 or g2 ~= w2 then\n"
                "    error(('find(%q, %q, %d): got %s %s, want %s %s'):format(s, p, init,\n"
                "          tostring(g1), tostring(g2), tostring(w1), tostring(w2)))\n"
                "  end\n"
                "end\n"
                "return true");

    expect_error(vm, "return ('abc'):find('a', 1.5)",
                 "main.lua:1: bad argument #2 to 'find' (number has no integer representation)");
    apps_vm_free(vm);
}

/* ---- split_trim ---- */

static void split_trim(void) {
    apps_vm_t *vm = new_vm(1 * MB);

    /* The brief's two. */
    expect_true(vm, "return table.concat(('a,,b'):split(','), '|') == 'a||b'");
    expect_true(vm, "return ('  hi \\t'):trim() == 'hi'");

    expect_true(vm,
                "local function check(what, want, got)\n"
                "  if got ~= want then error(what .. ': got ' .. tostring(got) .. ', want ' .. tostring(want), 2) end\n"
                "end\n"
                "local function joined(t) return #t .. ':' .. table.concat(t, '|') end\n"
                "check('empty', '1:', joined((''):split(',')))\n"
                "check('trailing', '2:a|', joined(('a,'):split(',')))\n"
                "check('leading', '2:|a', joined((',a'):split(',')))\n"
                "check('none', '1:abc', joined(('abc'):split(',')))\n"
                "check('only', '2:|', joined((','):split(',')))\n"
                "check('word', '3:a|b|c', joined(('a, b, c'):split(', ')))\n"
                "check('overlap', '2:|a', joined(('aaa'):split('aa')))\n"
                "check('dot is plain', '2:a|b', joined(('a.b'):split('.')))\n"
                "check('percent is plain', '2:a|b', joined(('a%sb'):split('%s')))\n"
                "check('function form', '2:a|b', joined(string.split('a,b', ',')))\n"
                "check('longer sep', '1:ab', joined(('ab'):split('abc')))\n"
                "check('numbers', '3:|2|2', joined(string.split(1212, 1)))\n"
                "check('trim all', 'x', (' \\t\\r\\n\\v\\fx \\t\\r\\n\\v\\f'):trim())\n"
                "check('trim blank', '', ('   '):trim())\n"
                "check('trim empty', '', (''):trim())\n"
                "check('trim none', 'x', ('x'):trim())\n"
                "check('trim inner', 'a b', (' a b '):trim())\n"
                "check('trim number', '12', string.trim(12))\n"
                "check('trim is ASCII', '\\194\\160x', ('\\194\\160x'):trim())\n"
                "return true");

    expect_true(vm, "local ok, msg = pcall(string.split, 'abc', '')\n"
                    "return not ok and msg:find('empty separator', 1, true) ~= nil");
    expect_true(vm, "local ok, msg = pcall(string.split, 'abc')\n"
                    "return not ok and msg:find('string expected', 1, true) ~= nil");

    /* split against the obvious one, on strings of a and b. */
    expect_true(vm,
                "math.randomseed(77)\n"
                "local function naive(s, sep)\n"
                "  local out, i = {}, 1\n"
                "  while true do\n"
                "    local j\n"
                "    for k = i, #s - #sep + 1 do\n"
                "      if s:sub(k, k + #sep - 1) == sep then j = k break end\n"
                "    end\n"
                "    if not j then out[#out + 1] = s:sub(i) return out end\n"
                "    out[#out + 1] = s:sub(i, j - 1)\n"
                "    i = j + #sep\n"
                "  end\n"
                "end\n"
                "local function word(n)\n"
                "  local t = {}\n"
                "  for i = 1, n do t[i] = math.random(2) == 1 and 'a' or 'b' end\n"
                "  return table.concat(t)\n"
                "end\n"
                "for trial = 1, 4000 do\n"
                "  local s, sep = word(math.random(0, 14)), word(math.random(1, 4))\n"
                "  local want, got = naive(s, sep), s:split(sep)\n"
                "  if #got ~= #want or table.concat(got, '|') ~= table.concat(want, '|') then\n"
                "    error(('split(%q, %q): got %s, want %s'):format(s, sep,\n"
                "          table.concat(got, '|'), table.concat(want, '|')))\n"
                "  end\n"
                "end\n"
                "return true");
    apps_vm_free(vm);
}

/* ---- find_linear, split_linear (ruling A4: 16 MB, and the Python side's 2 s) ---- */

static const char ADVERSARY[] =
    "local s = ('a'):rep(1000000)\n"
    "local p = ('a'):rep(500000) .. 'b'\n";

static void find_linear(void) {
    apps_vm_t *vm = new_vm(16 * MB);
    char src[256];
    snprintf(src, sizeof src, "%sreturn s:find(p, 1, true) == nil", ADVERSARY);
    expect_true(vm, src);
    /* The KMP table, 4 bytes for each of p's 500,001, is in the capped heap with s and p. */
    size_t floor = 1000000 + 500001 + 4 * 500001;
    CHECK(apps_vm_mem_peak(vm) >= floor, "peak %zu bytes: the KMP table isn't in the Lua heap", apps_vm_mem_peak(vm));
    apps_vm_free(vm);
}

static void split_linear(void) {
    apps_vm_t *vm = new_vm(16 * MB);
    char src[256];
    snprintf(src, sizeof src, "%slocal t = s:split(p)\nreturn #t == 1 and t[1] == s", ADVERSARY);
    expect_true(vm, src);
    apps_vm_free(vm);
}

/* ---- Bounded C loops: rep, concat, move, insert, remove, sort (fix round 1) ----
 *
 * Lua's own versions of these can loop without allocating, where neither the cap nor a
 * count hook reaches. Each case runs under a 2 s timeout on the Python side, so a loop
 * that isn't bounded fails it. */

#define TOO_MANY(fn) "table." fn ": more elements than this app's memory can hold"

/* Two lines: a plain table, no metatable, whose #t is 2^61. Its integer keys sit in the
 * hash part the string keys made room for, so Lua finds the border by doubling. */
#define SPARSE                                                                                      \
    "local t = {} for i = 1, 300 do t['k' .. i] = i end for i = 0, 61 do t[1 << i] = true end\n" \
    "assert(#t == 1 << 61)\n"

/* Two lines: len, a __len that answers small on its first call and big after, counting
 * its calls. Lua's own function must get the count the wrapper checked. */
#define STATEFUL_LEN(small, big) \
    "local calls = 0\n"          \
    "local function len() calls = calls + 1 return calls == 1 and " small " or " big " end\n"

/* Two lines: t, whose missing elements read as "" without running Lua code or
 * allocating. The chain goes through a string, and a string's metatable is the app's. */
#define EMPTY_READS                             \
    "getmetatable('').__index = string.lower\n" \
    "local t = setmetatable({}, {__index = ''})\n"

/* What src raises in a stock state (base, coroutine, string, table, math), named
 * main.lua: the wrappers must keep Lua's own argument errors. */
static void stock_error(const char *src, char *out, size_t cap) {
    lua_State *S = luaL_newstate();
    luaL_requiref(S, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(S, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(S, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(S, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(S, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_settop(S, 0);
    int rc = luaL_loadbufferx(S, src, strlen(src), "=main.lua", "t");
    if (rc == LUA_OK) {
        rc = lua_pcall(S, 0, 0, 0);
    }
    snprintf(out, cap, "%s", rc == LUA_OK ? "(no error)" : top_text(S));
    lua_close(S);
}

static void expect_stock_error_at(int line, apps_vm_t *vm, const char *src) {
    char want[512];
    stock_error(src, want, sizeof want);
    expect_error_at(line, vm, src, want);
}
#define expect_stock_error(vm, src) expect_stock_error_at(__LINE__, vm, src)

/* A 256 KB VM, with the most array slots it could hold (mem_limit / 16) as LIMIT. */
static apps_vm_t *bounded_vm(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    lua_pushinteger(L, (lua_Integer)(256 * KB / 16));
    lua_setglobal(L, "LIMIT");
    return vm;
}

static void rep_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    /* Nothing to repeat: "" at once, however many times. */
    expect_true(vm, "return (''):rep(math.maxinteger) == '' and (''):rep(math.maxinteger, '') == ''\n"
                    "  and string.rep('', math.maxinteger) == ''");
    expect_true(vm, "return ('x'):rep(0) == '' and ('x'):rep(-5) == '' and ('x'):rep(math.mininteger, ',') == ''");
    /* Anything else is Lua's own rep, bounded by the cap. */
    expect_true(vm, "return ('x'):rep(3) == 'xxx' and ('x'):rep(3, ',') == 'x,x,x' and (''):rep(3, ',') == ',,'\n"
                    "  and string.rep(1, 2) == '11'");
    expect_true(vm, "return not pcall(string.rep, 'x', math.maxinteger)\n"
                    "  and not pcall(string.rep, '', math.maxinteger, ',')");
    expect_stock_error(vm, "return ('x'):rep('a')");
    expect_stock_error(vm, "return string.rep()");
    expect_stock_error(vm, "return string.rep('x', 1.5)");
    apps_vm_free(vm);
}

static void move_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    expect_error(vm, "table.move({}, 1, math.maxinteger, 1)", "main.lua:1: " TOO_MANY("move"));
    expect_error(vm, "table.move({}, math.mininteger, math.maxinteger, 1)", "main.lua:1: " TOO_MANY("move"));
    expect_error(vm, "table.move('abc', 1, math.maxinteger, 1, {})", "main.lua:1: " TOO_MANY("move"));
    expect_error(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                     "table.move(t, 1, #t, 1, {})",
                 "main.lua:2: " TOO_MANY("move"));
    /* LIMIT elements is Lua's own move; one more is refused. */
    expect_true(vm, "return next(table.move({}, 1, LIMIT, 1)) == nil");
    expect_error(vm, "table.move({}, 1, LIMIT + 1, 1)", "main.lua:1: " TOO_MANY("move"));
    /* Lua's behaviour and errors otherwise. */
    expect_true(vm, "local t = table.move({1, 2, 3}, 1, 3, 2) local d = table.move({1, 2, 3}, 2, 3, 1, {})\n"
                    "return table.concat(t, ',') == '1,1,2,3' and table.concat(d, ',') == '2,3'\n"
                    "  and #table.move({1}, 5, 1, 1) == 1 and table.move({1}, math.maxinteger, math.maxinteger, 1)[1] == nil");
    expect_stock_error(vm, "table.move({}, 1, 2)");
    expect_stock_error(vm, "table.move({}, 'a', 2, 1)");
    expect_stock_error(vm, "table.move(nil, 1, 2, 1)");
    expect_stock_error(vm, "table.move({}, 1, 2, math.maxinteger)");
    apps_vm_free(vm);
}

static void insert_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    /* A __len of maxinteger - 1 (maxinteger itself wraps to an empty shift). */
    expect_error(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end})\n"
                     "table.insert(t, 1, 'v')",
                 "main.lua:2: " TOO_MANY("insert"));
    expect_true(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                    "table.insert(t, 1, 'v') return rawget(t, 1) == 'v'");
    /* A negative __len passes Lua's position check for the smallest position, and
     * would shift 2^63 elements. */
    expect_error(vm, "local t = setmetatable({}, {__len = function() return -2 end})\n"
                     "table.insert(t, math.mininteger, 'v')",
                 "main.lua:2: " TOO_MANY("insert"));
    /* Shifting LIMIT elements is Lua's own insert; one more is refused. */
    expect_true(vm, "local n = LIMIT local t = setmetatable({}, {__len = function() return n end})\n"
                    "table.insert(t, 1, 'v') return rawget(t, 1) == 'v'");
    expect_error(vm, "local n = LIMIT + 1 local t = setmetatable({}, {__len = function() return n end})\n"
                     "table.insert(t, 1, 'v')",
                 "main.lua:2: " TOO_MANY("insert"));
    /* Appending shifts nothing, whatever __len says. */
    expect_true(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end})\n"
                    "table.insert(t, 'v') return rawget(t, math.maxinteger) == 'v'");
    /* A __len that answers 1, then maxinteger - 1, is asked once. */
    expect_true(vm, STATEFUL_LEN("1", "math.maxinteger - 1")
                    "local t = setmetatable({}, {__len = len}) table.insert(t, 1, 'v')\n"
                    "return calls == 1 and rawget(t, 1) == 'v'");
    expect_error(vm, SPARSE "table.insert(t, 1, 'v')", "main.lua:3: " TOO_MANY("insert"));
    /* Lua's behaviour and errors otherwise. */
    expect_true(vm, "local t = {1, 2, 3} table.insert(t, 1, 0) table.insert(t, 'x')\n"
                    "return table.concat(t, ',') == '0,1,2,3,x'");
    expect_stock_error(vm, "table.insert({1, 2}, 5, 'x')");
    expect_stock_error(vm, "table.insert({1, 2})");
    expect_stock_error(vm, "table.insert(nil, 'x')");
    expect_stock_error(vm, "table.insert('abc', 1, 'x')");
    expect_stock_error(vm, "table.insert({}, 'a', 'x')");
    apps_vm_free(vm);
}

static void remove_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    expect_error(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                     "table.remove(t, 1)",
                 "main.lua:2: " TOO_MANY("remove"));
    expect_error(vm, "local t = setmetatable({}, {__len = function() return -5 end})\n"
                     "table.remove(t, math.mininteger)",
                 "main.lua:2: " TOO_MANY("remove"));
    /* Removing the last element shifts nothing, whatever __len says. */
    expect_true(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                    "return table.remove(t) == nil");
    /* Shifting LIMIT elements is Lua's own remove; one more is refused. */
    expect_true(vm, "local n = LIMIT + 1 local t = setmetatable({'a'}, {__len = function() return n end})\n"
                    "return table.remove(t, 1) == 'a'");
    expect_error(vm, "local n = LIMIT + 2 local t = setmetatable({}, {__len = function() return n end})\n"
                     "table.remove(t, 1)",
                 "main.lua:2: " TOO_MANY("remove"));
    expect_true(vm, STATEFUL_LEN("1", "math.maxinteger - 1")
                    "local t = setmetatable({'a'}, {__len = len})\n"
                    "return table.remove(t, 1) == 'a' and calls == 1 and rawget(t, 1) == nil");
    expect_error(vm, SPARSE "table.remove(t, 1)", "main.lua:3: " TOO_MANY("remove"));
    /* Lua's behaviour and errors otherwise. */
    expect_true(vm, "local t = {1, 2, 3} local a, b = table.remove(t, 1), table.remove(t)\n"
                    "return a == 1 and b == 3 and #t == 1 and t[1] == 2 and table.remove({}) == nil");
    expect_stock_error(vm, "table.remove({1, 2}, 5)");
    expect_stock_error(vm, "table.remove(nil)");
    expect_stock_error(vm, "table.remove({}, 'a')");
    apps_vm_free(vm);
}

static void sort_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    /* The review's case: a __len of 2e7 with nothing behind it, and a C comparator. */
    expect_error(vm, "local t = setmetatable({}, {__len = function() return 2e7 end})\n"
                     "table.sort(t, getmetatable)",
                 "main.lua:2: " TOO_MANY("sort"));
    expect_error(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                     "table.sort(t)",
                 "main.lua:2: " TOO_MANY("sort"));
    /* LIMIT elements is Lua's own sort; one more is refused. */
    expect_true(vm, "local n = LIMIT local t = setmetatable({}, {__len = function() return n end})\n"
                    "table.sort(t, getmetatable) return true");
    expect_error(vm, "local n = LIMIT + 1 local t = setmetatable({}, {__len = function() return n end})\n"
                     "table.sort(t, getmetatable)",
                 "main.lua:2: " TOO_MANY("sort"));
    expect_true(vm, STATEFUL_LEN("3", "math.maxinteger - 1")
                    "local t = setmetatable({3, 1, 2}, {__len = len}) table.sort(t)\n"
                    "return calls == 1 and t[1] == 1 and t[2] == 2 and t[3] == 3");
    expect_error(vm, SPARSE "table.sort(t)", "main.lua:3: " TOO_MANY("sort"));
    /* Lua's behaviour and errors otherwise. */
    expect_true(vm, "local t, u = {3, 1, 2}, {3, 1, 2} table.sort(t) table.sort(u, function(a, b) return a > b end)\n"
                    "return table.concat(t, ',') == '1,2,3' and table.concat(u, ',') == '3,2,1'");
    expect_stock_error(vm, "table.sort({3, 1, 2}, 1)");
    expect_stock_error(vm, "table.sort(nil)");
    expect_stock_error(vm, "table.sort({1, 'a'})");
    apps_vm_free(vm);
}

static void concat_bounded(void) {
    apps_vm_t *vm = bounded_vm();
    expect_error(vm, "local t = setmetatable({}, {__len = function() return math.maxinteger end})\n"
                     "table.concat(t)",
                 "main.lua:2: " TOO_MANY("concat"));
    expect_error(vm, "table.concat({}, '', 1, math.maxinteger)", "main.lua:1: " TOO_MANY("concat"));
    expect_error(vm, "table.concat({}, '', math.mininteger, math.maxinteger)", "main.lua:1: " TOO_MANY("concat"));
    expect_error(vm, SPARSE "table.concat(t)", "main.lua:3: " TOO_MANY("concat"));
    /* Lua's behaviour and errors otherwise, in Lua's order: sep before i and j. */
    expect_true(vm, "return table.concat({1, 2, 3}, ', ') == '1, 2, 3' and table.concat({}, 'x') == ''\n"
                    "  and table.concat({1, 2}, 0) == '102' and table.concat({'a', 'b', 'c'}, '', 2, 3) == 'bc'\n"
                    "  and table.concat({}, 'x', 5, 1) == '' and table.concat({'a'}, nil, 1, 1) == 'a'");
    expect_stock_error(vm, "table.concat({1, {}, 3})");
    expect_stock_error(vm, "table.concat({}, {})");
    expect_stock_error(vm, "table.concat({}, {}, 'a')");
    expect_stock_error(vm, "table.concat({}, '', 'a')");
    expect_stock_error(vm, "table.concat({}, '', 1, 'b')");
    expect_stock_error(vm, "table.concat(nil)");
    expect_stock_error(vm, "table.concat({}, '', math.maxinteger, math.maxinteger)");
    apps_vm_free(vm);

    /* Elements that read as "" with no Lua code and no allocation: only the count
     * bounds the loop. LIMIT of them is Lua's own concat; one more is refused. */
    vm = bounded_vm();
    expect_error(vm, EMPTY_READS "table.concat(t, '', 1, math.maxinteger)", "main.lua:3: " TOO_MANY("concat"));
    expect_true(vm, EMPTY_READS "return table.concat(t, '', 1, LIMIT) == ''");
    expect_error(vm, EMPTY_READS "table.concat(t, '', 1, LIMIT + 1)", "main.lua:3: " TOO_MANY("concat"));
    expect_error(vm, EMPTY_READS "setmetatable(t, {__index = '', __len = function() return math.maxinteger end})\n"
                                 "table.concat(t)",
                 "main.lua:4: " TOO_MANY("concat"));
    expect_true(vm, EMPTY_READS STATEFUL_LEN("3", "math.maxinteger")
                    "setmetatable(t, {__index = '', __len = len}) t[1], t[2], t[3] = 'a', 'b', 'c'\n"
                    "return table.concat(t) == 'abc' and calls == 1");
    /* A string is table-like to concat once its metatable has __index and __len. Its
     * length is its own (Lua never asks a string's __len), but a range needn't be. */
    expect_error(vm, "local mt = getmetatable('') mt.__index, mt.__len = string.sub, true\n"
                     "table.concat('abc', '', 1, math.maxinteger)",
                 "main.lua:2: " TOO_MANY("concat"));
    expect_true(vm, "local mt = getmetatable('') mt.__index, mt.__len = string.sub, true\n"
                    "return table.concat('abc') == 'abcbcc' and table.concat('abc', ',', 2, 5) == 'bc,c,,'");
    apps_vm_free(vm);
}

/* A module's userdata whose metamethods are all C: Lua's own loops over it would run no
 * Lua code at all. Its length is maxinteger - 1, reads give "" and writes do nothing. */
static int c_len(lua_State *L) {
    lua_pushinteger(L, LUA_MAXINTEGER - 1);
    return 1;
}
static int c_index(lua_State *L) {
    lua_pushliteral(L, "");
    return 1;
}
static int c_newindex(lua_State *L) {
    (void)L;
    return 0;
}
static void set_c_object(lua_State *L, const char *name, bool writable) {
    lua_newuserdatauv(L, 1, 0);
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, c_len);
    lua_setfield(L, -2, "__len");
    lua_pushcfunction(L, c_index);
    lua_setfield(L, -2, "__index");
    if (writable) {
        lua_pushcfunction(L, c_newindex);
        lua_setfield(L, -2, "__newindex");
    }
    lua_setmetatable(L, -2);
    lua_setglobal(L, name);
}

/* Lua's own insert, remove, sort and concat reach an object with __len through a proxy
 * (fix round 1). Its __len runs once, and its own metamethods get the object itself. */
static void len_proxy(void) {
    apps_vm_t *vm = bounded_vm();
#define OBJECT                                                                                        \
    "local store, calls, strangers = {'a', 'b', 'c'}, 0, 0\n"                                         \
    "local t\n"                                                                                       \
    "local function check(self) if not rawequal(self, t) then strangers = strangers + 1 end end\n"    \
    "t = setmetatable({}, {__len = function(self) check(self) calls = calls + 1 return #store end,\n" \
    "  __index = function(self, k) check(self) return store[k] end,\n"                               \
    "  __newindex = function(self, k, v) check(self) store[k] = v end})\n"
#define SEEN_ONCE " and calls == 1 and strangers == 0"
    expect_true(vm, OBJECT "table.insert(t, 1, 'z') return table.concat(store, ',') == 'z,a,b,c'" SEEN_ONCE);
    expect_true(vm, OBJECT "return table.remove(t, 1) == 'a' and table.concat(store, ',') == 'b,c'" SEEN_ONCE);
    expect_true(vm, OBJECT "table.sort(t, function(a, b) return a > b end)\n"
                           "return table.concat(store, ',') == 'c,b,a'" SEEN_ONCE);
    expect_true(vm, OBJECT "return table.concat(t, '-') == 'a-b-c'" SEEN_ONCE);
#undef SEEN_ONCE
#undef OBJECT
    /* __len's own errors are Lua's. */
    expect_stock_error(vm, "table.insert(setmetatable({}, {__len = function() return 1.5 end}), 1, 'v')");
    expect_stock_error(vm, "table.sort(setmetatable({}, {__len = function() error('no length') end}))");
    expect_stock_error(vm, "table.remove(setmetatable({}, {__len = 7}))");
    /* Not a table but table-like: refused the same. Without __newindex it is table-like
     * to concat alone, and the others raise Lua's own error. */
    lua_State *L = apps_vm_lua(vm);
    set_c_object(L, "RW", true);
    set_c_object(L, "RO", false);
    expect_true(vm, "local function refused(f, ...)\n"
                    "  local ok, e = pcall(f, ...) return not ok and string.find(e, 'more elements than', 1, true) ~= nil\n"
                    "end\n"
                    "local function not_a_table(f, ...)\n"
                    "  local ok, e = pcall(f, ...) return not ok and string.find(e, 'table expected, got userdata', 1, true) ~= nil\n"
                    "end\n"
                    "return refused(table.insert, RW, 1, 'x') and refused(table.remove, RW, 1) and refused(table.sort, RW)\n"
                    "  and refused(table.concat, RW) and refused(table.concat, RO) and not_a_table(table.insert, RO, 1, 'x')\n"
                    "  and not_a_table(table.remove, RO, 1) and not_a_table(table.sort, RO)");
    apps_vm_free(vm);
}

/* The did-you-mean's work per miss is capped (fix round 1): past the cap, the plain
 * message. */
static void suggest_capped(void) {
    apps_vm_t *vm = new_vm(1 * MB);
    char src[1024], want[256];
    static const char stem_src[] =
        "local stem, letters = ('n'):rep(58), 'abcdefghijklmnopqrstuvwxyz'\n"
        "for i = 0, %d - 1 do\n"
        "  local a, b = i // 26 + 1, i %% 26 + 1\n"
        "  rawset(_G, stem .. letters:sub(a, a) .. letters:sub(b, b), true)\n"
        "end\n"
        "return _G[stem .. '!!']";
    /* 300 globals of 60 characters, all two edits from the missing name, whose tables
     * stay open to the last row: about a million cells, past the cap. */
    char stem[59];
    memset(stem, 'n', 58);
    stem[58] = '\0';
    snprintf(src, sizeof src, stem_src, 300);
    snprintf(want, sizeof want, "main.lua:6: unknown global '%s!!'", stem);
    expect_error(vm, src, want);
    apps_vm_free(vm);

    /* The same with 5 such globals is under the cap and gets its suggestion. */
    vm = new_vm(1 * MB);
    lua_State *L = apps_vm_lua(vm);
    char err[256] = "";
    snprintf(src, sizeof src, stem_src, 5);
    int rc = run(vm, src, 0, err, sizeof err);
    CHECK(rc == LUA_ERRRUN && strstr(top_text(L), "(did you mean 'nnnn") != NULL, "5 near globals: rc=%d %s", rc,
          rc == -1 ? err : top_text(L));
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* More candidates than the did-you-mean gathers: the plain message, though 'w1' is
     * one edit from 'w1x'. */
    vm = new_vm(1 * MB);
    expect_error(vm, "for i = 1, 600 do rawset(_G, 'w' .. i, true) end\nreturn w1x", "main.lua:2: unknown global 'w1x'");
    expect_error(vm, "return pairz", "main.lua:1: unknown global 'pairz'");
    apps_vm_free(vm);
}

/* ---- allocator ---- */

static void allocator(void) {
    const size_t cap = 256 * KB;
    apps_vm_t *vm = new_vm(cap);
    lua_State *L = apps_vm_lua(vm);
    char err[256] = "";

    /* What the counting allocator holds beyond the Lua heap is apps_vm's own struct, the
     * same at every step: the cap's accounting tracks the real allocations exactly. */
    const size_t own = held - apps_vm_mem_used(vm);
    CHECK(apps_vm_mem_used(vm) > 0 && apps_vm_mem_used(vm) <= cap, "used %zu", apps_vm_mem_used(vm));
    CHECK(apps_vm_mem_peak(vm) >= apps_vm_mem_used(vm), "peak %zu < used %zu", apps_vm_mem_peak(vm),
          apps_vm_mem_used(vm));
    CHECK(apps_vm_mem_refusals(vm) == 0, "%zu refusals before anything ran", apps_vm_mem_refusals(vm));

    int rc = run(vm, "return ('x'):rep(1 << 20)", 1, err, sizeof err);
    CHECK(rc == LUA_ERRMEM, "rc=%d %s", rc, rc == -1 ? err : top_text(L));
    CHECK(apps_vm_mem_peak(vm) <= cap, "peak %zu over the %zu cap", apps_vm_mem_peak(vm), cap);
    CHECK(apps_vm_mem_refusals(vm) >= 1, "the refused megabyte wasn't counted");
    CHECK(held - apps_vm_mem_used(vm) == own, "accounting drifted: held %zu, used %zu, own %zu", held,
          apps_vm_mem_used(vm), own);
    lua_settop(L, 0);

    /* No retry answered that refusal, so the app is out of memory for good (ruling A2,
     * amended): the next call runs nothing. Lua's state itself carries on: it collects, and
     * the accounting stays exact. */
    CHECK(apps_vm_load(vm, "main.lua", "survived = true", 15, err, sizeof err), "%s", err);
    apps_call_t status = apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(status == APPS_CALL_NOMEM && raw_global(L, "survived") == LUA_TNIL, "after the refusal: status %d '%s'",
          (int)status, err);
    lua_settop(L, 0);
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(held - apps_vm_mem_used(vm) == own, "accounting drifted after a collection");
    apps_vm_free(vm);

    /* A table bomb ends in LUA_ERRMEM too, inside the cap. */
    vm = new_vm(cap);
    L = apps_vm_lua(vm);
    rc = run(vm, "local t = {} for i = 1, 1e7 do t[i] = i end", 0, err, sizeof err);
    CHECK(rc == LUA_ERRMEM, "table bomb: rc=%d %s", rc, rc == -1 ? err : top_text(L));
    CHECK(apps_vm_mem_peak(vm) <= cap, "peak %zu over the %zu cap", apps_vm_mem_peak(vm), cap);
    CHECK(held - apps_vm_mem_used(vm) == own, "accounting drifted after the table bomb");
    lua_settop(L, 0);
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(apps_vm_mem_used(vm) < cap / 2, "%zu bytes still used after a collection", apps_vm_mem_used(vm));

    apps_vm_free(vm);
    CHECK(held == 0, "%zu bytes held after apps_vm_free", held);
}

/* ---- cycles ---- */

static void cycles(void) {
    for (int i = 0; i < 200; i++) {
        apps_vm_t *vm = new_vm(256 * KB);
        CHECK(vm != NULL, "cycle %d: apps_vm_new failed", i);
        if (vm == NULL) {
            return;
        }
        char err[256];
        expect_ints(vm, "local t = {} for i = 1, 100 do t[i] = tostring(i) end return #t", 100);
        /* The paths that allocate on the way to an error: the did-you-mean's candidates,
         * the KMP table, a syntax error. */
        expect_error(vm, "local digits = 1 return digts", "main.lua:1: unknown global 'digts' (did you mean 'digits'?)");
        expect_true(vm, "return ('abababc'):find('ababc', 1, true) == 3 and #('a,b,,c'):split(',') == 4");
        CHECK(!apps_vm_load(vm, "main.lua", "if", 2, err, sizeof err), "a syntax error loaded");
        apps_vm_free(vm);
        CHECK(held == 0, "cycle %d left %zu bytes", i, held);
        if (held != 0) {
            return;
        }
    }
}

/* ---- new_without_memory ---- */

static void new_without_memory(void) {
    /* Each allocation of apps_vm_new failing in turn: NULL, with nothing left behind, until
     * the first success. */
    long n = 0;
    for (;; n++) {
        grants_left = n;
        apps_vm_t *vm = new_vm(256 * KB);
        grants_left = -1;
        CHECK(vm != NULL || held == 0, "allocation %ld failing left %zu bytes", n, held);
        if (vm != NULL) {
            apps_vm_free(vm);
            break;
        }
        if (n == 100000) {
            CHECK(false, "apps_vm_new never succeeded");
            break;
        }
    }
    CHECK(n > 10, "apps_vm_new made only %ld allocations", n);

    /* Caps too small for the libraries: NULL, nothing left behind. */
    size_t cap = KB;
    apps_vm_t *vm;
    while ((vm = new_vm(cap)) == NULL && cap < 256 * KB) {
        CHECK(held == 0, "a %zu-byte cap left %zu bytes", cap, held);
        cap += KB;
    }
    CHECK(vm != NULL && cap > KB && apps_vm_mem_peak(vm) <= cap, "cap %zu: %s", cap,
          vm == NULL ? "no cap up to 256 KB works" : "over the cap");
    apps_vm_free(vm);

    /* A config without its functions. */
    apps_vm_config_t cfg = {.mem_limit = 256 * KB, .realloc_fn = count_realloc, .free_fn = count_free,
                            .now_ms = fake_now_ms};
    CHECK(apps_vm_new(NULL) == NULL, "a NULL config made a VM");
    apps_vm_config_t bad = cfg;
    bad.realloc_fn = NULL;
    CHECK(apps_vm_new(&bad) == NULL, "a config without realloc_fn made a VM");
    bad = cfg;
    bad.free_fn = NULL;
    CHECK(apps_vm_new(&bad) == NULL, "a config without free_fn made a VM");
    bad = cfg;
    bad.now_ms = NULL;
    CHECK(apps_vm_new(&bad) == NULL, "a config without now_ms made a VM");
    apps_vm_free(NULL);
}

/* ---- excerpt ---- */

static bool valid_utf8(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        unsigned c = *p++;
        int more = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (more < 0 || end - p < more) {
            return false;
        }
        while (more-- > 0) {
            if ((*p++ & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

static void expect_excerpt_at(int line, const char *msg, const char *src, size_t len, const char *want) {
    char out[1024];
    memset(out, '#', sizeof out);
    size_t n = apps_vm_excerpt(msg, src, len, out, sizeof out);
    CHECK_AT(line, n == strlen(want) && strcmp(out, want) == 0 && valid_utf8(out, n),
             "\n  want (%zu) '%s'\n  got  (%zu) '%s'", strlen(want), want, n, out);
}
#define expect_excerpt(msg, src, want) expect_excerpt_at(__LINE__, msg, src, strlen(src), want)

static void excerpt(void) {
    /* The brief's case, and that it is exactly what loading the source says. */
    static const char src[] = "local a = 1\nif (a then\nend\n";
    static const char msg[] = "main.lua:2: ')' expected near 'then'";
    expect_excerpt(msg, src, "main.lua:2: ')' expected near 'then'\n 1| local a = 1\n>2| if (a then\n 3| end");
    apps_vm_t *vm = new_vm(256 * KB);
    char err[256];
    CHECK(!apps_vm_load(vm, "main.lua", src, strlen(src), err, sizeof err) && strcmp(err, msg) == 0, "load says '%s'",
          err);
    apps_vm_free(vm);

    /* A11: lines are cut at 80 bytes, never inside a UTF-8 sequence. A 4-byte emoji at byte
     * 78 goes; at byte 76 it fits; a 2-byte character at byte 79 goes. */
    char line[128], want[160];
    memset(line, 'x', 78);
    strcpy(line + 78, "\xF0\x9F\x98\x80tail");
    snprintf(want, sizeof want, "main.lua:1: boom\n>1| %.78s", line);
    expect_excerpt("main.lua:1: boom", line, want);
    memset(line, 'x', 76);
    strcpy(line + 76, "\xF0\x9F\x98\x80tail");
    snprintf(want, sizeof want, "main.lua:1: boom\n>1| %.80s", line);
    expect_excerpt("main.lua:1: boom", line, want);
    memset(line, 'x', 79);
    strcpy(line + 79, "\xC3\xA9tail");
    snprintf(want, sizeof want, "main.lua:1: boom\n>1| %.79s", line);
    expect_excerpt("main.lua:1: boom", line, want);
    memset(line, 'y', 100);
    line[100] = '\0';
    snprintf(want, sizeof want, "main.lua:1: boom\n>1| %.80s", line);
    expect_excerpt("main.lua:1: boom", line, want);

    /* Line numbers right-aligned to the widest shown; no line 0, and no line past the end
     * (the empty one after a final newline only when the error is on it). */
    static const char eleven[] = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\n";
    expect_excerpt("main.lua:10: x", eleven, "main.lua:10: x\n  9| i\n>10| j\n 11| k");
    expect_excerpt("main.lua:9: x", eleven, "main.lua:9: x\n  8| h\n> 9| i\n 10| j");
    expect_excerpt("main.lua:1: x", eleven, "main.lua:1: x\n>1| a\n 2| b");
    expect_excerpt("main.lua:11: x", eleven, "main.lua:11: x\n 10| j\n>11| k");
    expect_excerpt("main.lua:12: x", eleven, "main.lua:12: x\n 11| k\n>12|");
    expect_excerpt("main.lua:13: x", eleven, "main.lua:13: x");
    expect_excerpt("not enough memory", eleven, "not enough memory");
    expect_excerpt("main.lua:0: x", eleven, "main.lua:0: x");

    /* Lines end as Lua's lexer ends them (\n, \r, \r\n, \n\r); a BOM isn't shown; an empty
     * line has no trailing space. */
    expect_excerpt("main.lua:2: x", "x = 1\r\ny = (\r\n", "main.lua:2: x\n 1| x = 1\n>2| y = (");
    expect_excerpt("main.lua:3: x", "a\rb\n\rc\r\nd", "main.lua:3: x\n 2| b\n>3| c\n 4| d");
    expect_excerpt("main.lua:1: x", "\xEF\xBB\xBFlocal a\nb c", "main.lua:1: x\n>1| local a\n 2| b c");
    expect_excerpt("main.lua:2: x", "a\n\nb", "main.lua:2: x\n 1| a\n>2|\n 3| b");
    /* The first ":N:" is the line, as in a traceback's first line. */
    expect_excerpt("main.lua:2: boom\nstack traceback:\n\tmain.lua:3: in main chunk", "a\nb\nc\nd",
                   "main.lua:2: boom\nstack traceback:\n\tmain.lua:3: in main chunk\n 1| a\n>2| b\n 3| c");

    /* out is always terminated, and cut on a character boundary. */
    char out[32];
    size_t n = apps_vm_excerpt(msg, src, strlen(src), out, 10);
    CHECK(n == 9 && strcmp(out, "main.lua:") == 0, "cap 10: (%zu) '%s'", n, out);
    n = apps_vm_excerpt("main.lua:1: caf\xC3\xA9", "x", 1, out, 17);
    CHECK(n == 15 && strcmp(out, "main.lua:1: caf") == 0, "cap 17: (%zu) '%s'", n, out);
    strcpy(out, "untouched");
    n = apps_vm_excerpt(msg, src, strlen(src), out, 0);
    CHECK(n == 0 && strcmp(out, "untouched") == 0, "cap 0: (%zu) '%s'", n, out);
}

/* ---- suggest (ruling A17's rule, in apps_suggest) ---- */

static void suggest(void) {
    static const char *const digits[] = {"x", "digits", "dig"};
    CHECK(apps_suggest("digts", digits, 3) == digits[1], "digts");

    /* Distance at most 2 and less than strlen(name). */
    static const char *const y[] = {"y"};
    CHECK(apps_suggest("x", y, 1) == NULL, "x -> y is 1 edit, not less than 1");
    static const char *const abc[] = {"abc"};
    CHECK(apps_suggest("ab", abc, 1) == abc[0], "ab -> abc");
    static const char *const ba[] = {"ba"};
    CHECK(apps_suggest("ab", ba, 1) == NULL, "ab -> ba is 2 edits, not less than 2");
    static const char *const two[] = {"abxy"};
    CHECK(apps_suggest("abcd", two, 1) == two[0], "abcd -> abxy is 2 edits");
    static const char *const three[] = {"axyz"};
    CHECK(apps_suggest("abcd", three, 1) == NULL, "abcd -> axyz is 3 edits");

    /* The nearest wins; among equals, the first. */
    static const char *const pairs[] = {"ipairs", "pairs"};
    CHECK(apps_suggest("pairz", pairs, 2) == pairs[1], "pairz: the nearest");
    static const char *const ties[] = {"bat", "cut"};
    CHECK(apps_suggest("cat", ties, 2) == ties[0], "cat: the first of two at 1");

    /* The name itself, NULL entries and over-long names are skipped. */
    static const char *const same[] = {"pairs", NULL, "pair"};
    CHECK(apps_suggest("pairs", same, 3) == same[2], "pairs: not itself");
    static const char long_name[] = "a_name_that_is_much_longer_than_any_identifier_an_app_would_use_x";
    static const char *const longs[] = {"a_name_that_is_much_longer_than_any_identifier_an_app_would_use_y"};
    CHECK(strlen(long_name) > 64 && apps_suggest(long_name, longs, 1) == NULL, "over-long names");
    CHECK(apps_suggest("digts", digits, 0) == NULL, "no candidates");
    CHECK(apps_suggest("", digits, 3) == NULL, "empty name");
    CHECK(apps_suggest(NULL, digits, 3) == NULL, "NULL name");
    CHECK(apps_suggest("digts", NULL, 3) == NULL, "NULL candidates");

    /* Case counts as an edit. */
    static const char *const lower[] = {"pairs"};
    CHECK(apps_suggest("Pairs", lower, 1) == lower[0], "Pairs");

    /* The cells scored per call are capped (64K, fix round 1). A 64-byte name one edit
     * from each 64-byte candidate fills every table to its last row: 4096 cells each, so
     * 10 candidates fit and 40 don't. */
    static char wide[40][65];
    const char *wides[40];
    for (int i = 0; i < 40; i++) {
        memset(wide[i], 'a', 63);
        wide[i][63] = (char)('0' + i);  /* '0'..'W': never the name's 'a' */
        wide[i][64] = '\0';
        wides[i] = wide[i];
    }
    char a64[65];
    memset(a64, 'a', 64);
    a64[64] = '\0';
    CHECK(apps_suggest(a64, wides, 10) == wides[0], "10 wide candidates are under the cap");
    CHECK(apps_suggest(a64, wides, 40) == NULL, "40 wide candidates are past the cap");

    /* The callers in Tasks 7 and 8. */
    static const char *const manifest[] = {"id", "name", "version", "api", "main", "memory_kb", "permissions", "files"};
    CHECK(apps_suggest("permission", manifest, 8) == manifest[6], "permission");
    CHECK(apps_suggest("entry", manifest, 8) == NULL, "entry");
    static const char *const handlers[] = {"start", "stop", "show", "hide", "message", "alarm"};
    CHECK(apps_suggest("on_tap", handlers, 6) == NULL, "on_tap");
    CHECK(apps_suggest("strat", handlers, 6) == handlers[0], "strat");
}

/* ==== Budgets and the kill (Task 5) ==== */

/* The exact reasons (the brief's), for a budget of n instructions and a cap of kb KB. */
#define BUDGET_MSG(n) "stopped: ran more than " n " VM instructions in one callback (use timer.every for long work)"
#define WALL_MSG "stopped: one callback ran longer than 1 s"
#define NOMEM_MSG(kb) "out of memory: this app may use " kb " KB (memory_kb in manifest.json)"

static const char *call_name(int status) {
    switch (status) {
        case -1: return "(load refused)";
        case APPS_CALL_OK: return "OK";
        case APPS_CALL_ERROR: return "ERROR";
        case APPS_CALL_KILLED: return "KILLED";
        case APPS_CALL_NOMEM: return "NOMEM";
        default: return "?";
    }
}

/* Loads src as main.lua and runs it through apps_vm_call. Returns the apps_call_t, or -1
 * when apps_vm_load refused src; err gets the message either way. Results stay on the
 * stack. */
static int call(apps_vm_t *vm, const char *src, int nresults, uint32_t budget, char *err, size_t err_len) {
    if (!apps_vm_load(vm, "main.lua", src, strlen(src), err, err_len)) {
        return -1;
    }
    return (int)apps_vm_call(vm, 0, nresults, budget, err, err_len);
}

/* In a fresh VM of cap bytes, src under APPS_BUDGET_CALL ends with want and err exactly
 * reason, leaving an empty stack, a dying VM and a heap within its cap. A chunk sets
 * survived where the value of what should have stopped it would come from: apps_vm_call
 * reports a kill that landed while a call ran even when something swallowed it, so the
 * status alone can't show a kill that was swallowed for a few instructions. */
static void expect_end_at(int line, size_t cap, uint32_t budget, const char *src, apps_call_t want, const char *reason,
                          const char *seen) {
    apps_vm_t *vm = new_vm(cap);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    int rc = call(vm, src, LUA_MULTRET, budget, err, sizeof err);
    CHECK_AT(line, rc == (int)want && strcmp(err, reason) == 0, "%s\n  want %s '%s'\n  got  %s '%s'", src,
             call_name((int)want), reason, call_name(rc), err);
    CHECK_AT(line, lua_gettop(L) == 0, "%s\n  left %d values on the stack", src, lua_gettop(L));
    CHECK_AT(line, raw_global(L, "survived") == LUA_TNIL, "%s\n  ran on after it should have stopped", src);
    if (seen != NULL) {
        /* A generic for's closing value ran during the unwind and saw the error, but couldn't
         * keep it: seen is the error Lua closed the value with. */
        bool same = raw_global(L, "seen") == LUA_TSTRING && strcmp(lua_tostring(L, -1), seen) == 0;
        CHECK_AT(line, same, "%s\n  seen is %s, not '%s'", src, top_text(L), seen);
    }
    lua_settop(L, 0);
    CHECK_AT(line, apps_vm_dying(vm) && !apps_vm_in_call(vm), "%s\n  dying %d, in a call %d", src, apps_vm_dying(vm),
             apps_vm_in_call(vm));
    CHECK_AT(line, apps_vm_mem_peak(vm) <= cap, "%s\n  peak %zu over the %zu cap", src, apps_vm_mem_peak(vm), cap);
    apps_vm_free(vm);
}
#define expect_killed(src) \
    expect_end_at(__LINE__, 256 * KB, APPS_BUDGET_CALL, src, APPS_CALL_KILLED, BUDGET_MSG("2000000"), NULL)
#define expect_nomem(src) expect_end_at(__LINE__, 256 * KB, APPS_BUDGET_CALL, src, APPS_CALL_NOMEM, NOMEM_MSG("256"), NULL)
/* A generic for's closing value saw the out-of-memory error during the unwind and couldn't keep it. */
#define expect_nomem_seen(src) \
    expect_end_at(__LINE__, 256 * KB, APPS_BUDGET_CALL, src, APPS_CALL_NOMEM, NOMEM_MSG("256"), "not enough memory")

/* src under apps_vm_call returns exactly the values describe() shows as want. */
static void expect_returns_at(int line, apps_vm_t *vm, const char *src, const char *want) {
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "", got[512];
    int rc = call(vm, src, LUA_MULTRET, APPS_BUDGET_CALL, err, sizeof err);
    describe(L, got, sizeof got);
    CHECK_AT(line, rc == APPS_CALL_OK && strcmp(got, want) == 0, "%s\n  want %s\n  got  %s '%s' %s", src, want,
             call_name(rc), err, got);
    lua_settop(L, 0);
}
#define expect_returns(vm, src, want) expect_returns_at(__LINE__, vm, src, want)

/* src under apps_vm_call fails with exactly want as err. */
static void expect_error_text_at(int line, apps_vm_t *vm, const char *src, const char *want) {
    lua_State *L = apps_vm_lua(vm);
    char err[2048] = "";
    int rc = call(vm, src, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK_AT(line, rc == APPS_CALL_ERROR && strcmp(err, want) == 0, "%s\n  want ERROR '%s'\n  got  %s '%s'", src, want,
             call_name(rc), err);
    CHECK_AT(line, lua_gettop(L) == 0, "%s\n  left %d values on the stack", src, lua_gettop(L));
    lua_settop(L, 0);
}
#define expect_error_text(vm, src, want) expect_error_text_at(__LINE__, vm, src, want)

/* ---- infinite_loop ---- */

static void infinite_loop(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    uint64_t before = apps_vm_instructions(vm);
    int rc = call(vm, "while true do end", 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, BUDGET_MSG("2000000")) == 0, "got %s '%s'", call_name(rc), err);
    /* Stopped at the hook's first count past the budget. */
    uint64_t ran = apps_vm_instructions(vm) - before;
    CHECK(ran == 2001000, "ran %llu instructions", (unsigned long long)ran);
    CHECK(lua_gettop(L) == 0 && apps_vm_dying(vm) && !apps_vm_in_call(vm), "stack %d, dying %d, in a call %d",
          lua_gettop(L), apps_vm_dying(vm), apps_vm_in_call(vm));

    /* For good: the next call runs nothing and says why. */
    rc = call(vm, "ran = true", 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, BUDGET_MSG("2000000")) == 0, "after the kill: %s '%s'", call_name(rc), err);
    CHECK(lua_gettop(L) == 0 && raw_global(L, "ran") == LUA_TNIL, "a dying VM ran a chunk");
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* The reason gives the call's own budget. */
    vm = new_vm(256 * KB);
    rc = call(vm, "local n = 0 while true do n = n + 1 end", 0, 12345, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, BUDGET_MSG("12345")) == 0, "budget 12345: %s '%s'", call_name(rc), err);
    apps_vm_free(vm);
}

/* ---- wall_clock ---- */

static void wall_clock(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    char err[512] = "";
    uint64_t before = apps_vm_instructions(vm);
    fake_step = 600;  /* each read of the clock is 600 ms after the one before */
    int rc = call(vm, "for i = 1, 1e12 do end", 0, UINT32_MAX, err, sizeof err);
    fake_step = 0;
    uint64_t ran = apps_vm_instructions(vm) - before;
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, WALL_MSG) == 0, "got %s '%s'", call_name(rc), err);
    CHECK(ran <= 10000, "ran %llu instructions: the clock didn't stop it", (unsigned long long)ran);
    apps_vm_free(vm);
}

/* ---- tbc_refused: <close> variables (ruling) ---- */

/* src is refused at load with exactly want, and nothing is left on the stack. */
static void expect_load_refused_at(int line, const char *src, const char *want) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[256] = "";
    bool ok = apps_vm_load(vm, "main.lua", src, strlen(src), err, sizeof err);
    CHECK_AT(line, !ok && strcmp(err, want) == 0, "%s\n  want refused '%s'\n  got loaded=%d '%s'", src, want, ok, err);
    CHECK_AT(line, lua_gettop(L) == 0, "%s\n  left %d values on the stack", src, lua_gettop(L));
    apps_vm_free(vm);
}
#define expect_load_refused(src, want) expect_load_refused_at(__LINE__, src, want)
#define TBC_MSG(line) "main.lua:" line ": '<close>' variables are not available in Muse apps"

static void tbc_refused(void) {
    /* A to-be-closed variable runs app code (its __close) when a block unwinds, with hooks off,
     * so no kill or out-of-memory could stop or outlast it. It is refused at load, whatever its
     * value (nil still compiles OP_TBC), and wherever it is declared. */
    expect_load_refused("local x <close> = nil", TBC_MSG("1"));
    expect_load_refused("local x <close> = setmetatable({}, {__close = function() end})", TBC_MSG("1"));
    /* The whole prototype tree is walked: a nested function, and a coroutine body. */
    expect_load_refused("local function f()\n  local y <close> = nil\nend\nreturn f", TBC_MSG("2"));
    expect_load_refused("local co = coroutine.create(function()\n  local z <close> = nil\nend)\nreturn co", TBC_MSG("2"));
    /* With two, the earliest line is named. */
    expect_load_refused("do local a <close> = nil end\ndo local b <close> = nil end", TBC_MSG("1"));

    /* The message carries a position, so apps_vm_excerpt shows the offending line. */
    const char *src = "local ok = 1\nlocal x <close> = nil\n";
    apps_vm_t *vm = new_vm(256 * KB);
    char err[256] = "", out[256];
    CHECK(!apps_vm_load(vm, "main.lua", src, strlen(src), err, sizeof err), "a <close> chunk loaded");
    apps_vm_excerpt(err, src, strlen(src), out, sizeof out);
    CHECK(strstr(out, "local x <close> = nil") != NULL, "excerpt didn't show the line: [%s]", out);
    apps_vm_free(vm);

    /* <const> is unaffected: the chunk loads and runs. */
    vm = new_vm(256 * KB);
    expect_returns(vm, "local n <const> = 5 return n + 1", "number:6");
    apps_vm_free(vm);
}

#define LOAD_ON_NEST 24
#define LOAD_ON_DEPTH 20

/* ---- load_on: apps_vm_load_on, the parser on the running thread ----
 *
 * apps_vm_load compiles on the VM's main thread. A binding that compiles for app code (the runtime's require)
 * has to compile on the thread it was called on: the parser's recursion is counted against that thread's
 * C-call room (lua_State.nCcalls), and a coroutine's count starts at its resumer's and grows with every C
 * level it adds, which the main thread's count leaves out. One implementation, two entry points: the same
 * rules (the '=' name, the BOM, bytecode, <close>), and the result lands on the given thread's stack. */

/* load_on(src): the compiled function (pushed on the calling thread by apps_vm_load_on), or nil and the message. */
static int c_load_on(lua_State *L) {
    size_t len;
    const char *src = luaL_checklstring(L, 1, &len);
    char err[256];
    lua_settop(L, 1);
    if (apps_vm_load_on(L, "lib/t.lua", src, len, err, sizeof err)) {
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

static void load_on(void) {
    apps_vm_t *vm = new_vm(1 * MB);
    lua_State *L = apps_vm_lua(vm);
    char a_err[256], b_err[256];

    /* The same answer, message and stack effect as apps_vm_load on the main thread, for what it accepts and for
     * each way it refuses. */
    static const char bytecode_header[] = "\x1bLua\x54\x00\x19\x93\r\n\x1a\nrest";
    static const char *const inputs[] = {
        "return 1", "", "\xEF\xBB\xBFreturn 2", "\xEF\xBB\xBF\x1b" "Lua", bytecode_header,
        "local x <close> = nil", "x = = 1", "return (",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        size_t len = strcmp(inputs[i], bytecode_header) == 0 ? sizeof bytecode_header - 1 : strlen(inputs[i]);
        int top = lua_gettop(L);
        bool a = apps_vm_load(vm, "main.lua", inputs[i], len, a_err, sizeof a_err);
        int a_pushed = lua_gettop(L) - top;
        lua_settop(L, top);
        bool b = apps_vm_load_on(L, "main.lua", inputs[i], len, b_err, sizeof b_err);
        int b_pushed = lua_gettop(L) - top;
        lua_settop(L, top);
        CHECK(a == b && a_pushed == b_pushed && (a || strcmp(a_err, b_err) == 0),
              "input %zu: apps_vm_load %d '%s' pushed %d, apps_vm_load_on %d '%s' pushed %d", i, a, a_err, a_pushed, b,
              b_err, b_pushed);
    }
    char long_name[80], want[128];
    memcpy(long_name, "lib/", 4);
    for (int i = 4; i < 74; i++) {
        long_name[i] = (char)('a' + i % 26);
    }
    long_name[74] = '\0';
    snprintf(want, sizeof want, "%.*s:1: x", LUA_IDSIZE - 1, long_name);
    CHECK(apps_vm_load_on(L, long_name, "error('x')", 10, a_err, sizeof a_err), "%s", a_err);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN && strcmp(top_text(L), want) == 0, "long name: got '%s', want '%s'",
          top_text(L), want);
    lua_settop(L, 0);

    /* On a thread of its own: the function lands on that thread's stack and nowhere else, in the one sandbox. */
    lua_State *co = lua_newthread(L);
    int main_top = lua_gettop(L);
    CHECK(apps_vm_load_on(co, "lib/t.lua", "shared = 42 return shared", 25, a_err, sizeof a_err), "%s", a_err);
    CHECK(lua_gettop(co) == 1 && lua_gettop(L) == main_top, "stacks: thread %d (want 1), main %d (want %d)",
          lua_gettop(co), lua_gettop(L), main_top);
    CHECK(lua_pcall(co, 0, 1, 0) == LUA_OK && lua_isinteger(co, -1) && lua_tointeger(co, -1) == 42, "ran: '%s'",
          top_text(co));
    CHECK(raw_global(L, "shared") == LUA_TNUMBER && lua_tointeger(L, -1) == 42, "the thread's chunk didn't write the sandbox");
    lua_pop(L, 1);
    lua_settop(co, 0);
    CHECK(!apps_vm_load_on(co, "lib/t.lua", "local x <close> = nil", 21, a_err, sizeof a_err) &&
              strcmp(a_err, "lib/t.lua:1: '<close>' variables are not available in Muse apps") == 0 && lua_gettop(co) == 0,
          "<close> on a thread: '%s', thread stack %d", a_err, lua_gettop(co));
    lua_settop(L, 0);

    /* The parser's room is the calling thread's. A chunk nested NEST levels deep loads on the main thread and in a
     * fresh coroutine, but not in a coroutine that is already DEPTH pcall levels down: its count carries them, so
     * the same bound as every other C recursion applies. */
    lua_pushcfunction(L, c_load_on);
    lua_setglobal(L, "load_on");
    char src[1024];
    snprintf(src, sizeof src,
             "local nested = 'return ' .. ('('):rep(%d) .. '1' .. (')'):rep(%d)\n"
             "local function at_depth(n, f)\n"
             "    if n == 0 then return f() end\n"
             "    return select(2, pcall(at_depth, n - 1, f))\n"
             "end\n"
             "local function try() local fn, msg = load_on(nested) return fn ~= nil, msg end\n"
             "local main_ok = try()\n"
             "local fresh_ok = coroutine.wrap(try)()\n"
             "local deep_ok, deep_msg = coroutine.wrap(function() return at_depth(%d, try) end)()\n"
             "return main_ok, fresh_ok, deep_ok, deep_msg",
             LOAD_ON_NEST, LOAD_ON_NEST, LOAD_ON_DEPTH);
    char err[512] = "";
    int rc = call(vm, src, 4, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK && lua_toboolean(L, -4) && lua_toboolean(L, -3) && !lua_toboolean(L, -2) &&
              strcmp(lua_tostring(L, -1) != NULL ? lua_tostring(L, -1) : "", "C stack overflow") == 0,
          "got %s '%s': main %d, fresh coroutine %d, deep coroutine %d '%s'", call_name(rc), err,
          lua_toboolean(L, -4), lua_toboolean(L, -3), lua_toboolean(L, -2), top_text(L));
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- short_coroutines ---- */

/* src, in a fresh VM under apps_vm_call with budget, ends KILLED with reason. Returns the
 * global n it had counted to, or -1 without one. */
static lua_Integer killed_at_n_at(int line, const char *src, uint32_t budget, const char *reason) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    int rc = call(vm, src, 0, budget, err, sizeof err);
    CHECK_AT(line, rc == APPS_CALL_KILLED && strcmp(err, reason) == 0, "%s\n  got %s '%s'", src, call_name(rc), err);
    lua_Integer n = raw_global(L, "n") == LUA_TNUMBER ? lua_tointeger(L, -1) : -1;
    lua_settop(L, 0);
    apps_vm_free(vm);
    return n;
}
#define killed_at_n(src, budget, reason) killed_at_n_at(__LINE__, src, budget, reason)

/* main -> A -> B -> C, 10,101 coroutines for each A, none of which runs 1000 instructions,
 * while the main thread runs about 6 for each A: without a count for each coroutine, nothing
 * reaches the hook for some 165 A's, 1.5e9 instructions. */
#define TREE                                                   \
    "local wrap = coroutine.wrap\n"                            \
    "local function C() for i = 1, 900 do end end\n"           \
    "local function B() for i = 1, 100 do wrap(C)() end end\n" \
    "local function A() for i = 1, 100 do wrap(B)() end end\n" \
    "while true do wrap(A)() survived = true end"

static void short_coroutines(void) {
    /* A coroutine starts its own countdown, so one that runs fewer than 1000 instructions never
     * reaches the hook: creating one counts as 1000. The same work on the main thread, then in
     * a new coroutine each round, made by wrap and by create. */
    lua_Integer n = killed_at_n("n = 0 while true do for i = 1, 990 do end n = n + 1 end", 100000, BUDGET_MSG("100000"));
    CHECK(n >= 98 && n <= 104, "main thread: n = %lld", (long long)n);
    n = killed_at_n("local wrap = coroutine.wrap local function work() for i = 1, 990 do end end\n"
                    "n = 0 while true do wrap(work)() n = n + 1 end",
                    100000, BUDGET_MSG("100000"));
    CHECK(n >= 95 && n <= 110, "wrap: n = %lld", (long long)n);
    n = killed_at_n("local create, resume = coroutine.create, coroutine.resume\n"
                    "local function work() for i = 1, 990 do end end\n"
                    "n = 0 while true do resume(create(work)) n = n + 1 end",
                    100000, BUDGET_MSG("100000"));
    CHECK(n >= 95 && n <= 110, "create: n = %lld", (long long)n);

    /* One A of the tree is 10,101 coroutines, and that is all that counts: no thread reaches
     * the hook. */
    apps_vm_t *vm = new_vm(256 * KB);
    char err[512] = "";
    uint64_t before = apps_vm_instructions(vm);
    int rc = call(vm,
                  "local wrap = coroutine.wrap\n"
                  "local function C() for i = 1, 900 do end end\n"
                  "local function B() for i = 1, 100 do wrap(C)() end end\n"
                  "local function A() for i = 1, 100 do wrap(B)() end end\n"
                  "wrap(A)()",
                  0, UINT32_MAX, err, sizeof err);
    uint64_t counted = apps_vm_instructions(vm) - before;
    CHECK(rc == APPS_CALL_OK && counted == 10101000, "one A: %s '%s', %llu counted", call_name(rc), err,
          (unsigned long long)counted);
    apps_vm_free(vm);

    /* A tree of them: the budget stops it, and so does the clock, which creating one reads. */
    expect_killed(TREE);
    fake_step = 600;
    expect_end_at(__LINE__, 256 * KB, UINT32_MAX, TREE, APPS_CALL_KILLED, WALL_MSG, NULL);
    fake_step = 0;
}

/* ---- kill_not_swallowed ---- */

/* A generic for's fourth value is its closing value: a to-be-closed slot made at run time by
 * OP_TFORPREP, with no OP_TBC, so the chunk loads. Its __close is the one piece of app code Lua
 * still runs during an unwind. FOR_IT is an iterator that yields one value; FOR_CLOSE(fn) opens a
 * loop whose closing value runs fn when the block unwinds, with the hook on. */
#define FOR_IT "local function it() return 1 end\n"
#define FOR_CLOSE(fn) "for _ in it, nil, nil, setmetatable({}, {__close = " fn "}) do"

static void kill_not_swallowed(void) {
    /* The brief's five, then each with survived where its value would go. */
    expect_killed("pcall(function() while true do end end) return 'survived'");
    expect_killed("xpcall(function() while true do end end, function(e) return e end) return 'x'");
    expect_killed("pcall(coroutine.wrap(function() while true do end end)) return 'x'");
    expect_killed("local co = coroutine.create(function() while true do end end) return coroutine.resume(co)");
    expect_killed("local co = coroutine.create(function() coroutine.yield() while true do end end)\n"
                  "coroutine.resume(co) pcall(coroutine.resume, co) return 1");
    expect_killed("pcall(function() while true do end end) survived = true");
    expect_killed("xpcall(function() while true do end end, function(e) return e end) survived = true");
    expect_killed("pcall(coroutine.wrap(function() while true do end end)) survived = true");
    expect_killed("local co = coroutine.create(function() while true do end end) coroutine.resume(co) survived = true");
    expect_killed("local co = coroutine.create(function() coroutine.yield() while true do end end)\n"
                  "coroutine.resume(co) pcall(coroutine.resume, co) survived = true");

    /* pcall in pcall. */
    expect_killed("pcall(pcall, function() while true do end end) survived = true");
    /* xpcall's handler: never run for the kill, which the hook raised with hooks off (this
     * handler would spin unstopped: the test's timeout), and killed when it spins itself. */
    expect_killed("xpcall(function() while true do end end, function() while true do end end) survived = true");
    expect_killed("xpcall(function() while true do end end, function(e) return pcall(error, e) end) survived = true");
    expect_killed("xpcall(function() error('e') end, function() while true do end end) survived = true");
    /* In a coroutine, pcall and xpcall recover through their continuation. */
    expect_killed("local co = coroutine.create(function()\n"
                  "  pcall(function() while true do end end) survived = true\n"
                  "end)\n"
                  "coroutine.resume(co) survived = true");
    expect_killed("local co = coroutine.wrap(function()\n"
                  "  xpcall(function() error('e') end, function() while true do end end) survived = true\n"
                  "end)\n"
                  "pcall(co) survived = true");
    /* A yield across pcall, then the loop. */
    expect_killed("local co = coroutine.wrap(function()\n"
                  "  pcall(function() coroutine.yield() while true do end end) survived = true\n"
                  "end)\n"
                  "co() co() survived = true");
    /* App code that C calls: a metamethod, a comparator. */
    expect_killed("pcall(string.format, '%s', setmetatable({}, {__tostring = function() while true do end end}))\n"
                  "survived = true");
    expect_killed("pcall(table.sort, {3, 2, 1}, function() while true do end end) survived = true");

    /* A generic for's closing value is the one piece of app code Lua still runs while a kill
     * unwinds (<close> is refused at load, __gc is out). It can't keep the kill either.
     * A closing value killed by its own loop, closed alone and inside pcall: */
    expect_killed(FOR_IT
                  "local co = coroutine.create(function()\n"
                  "  " FOR_CLOSE("function() while true do end end") " coroutine.yield() end\n"
                  "end)\n"
                  "coroutine.resume(co) coroutine.close(co) survived = true");
    expect_killed(FOR_IT
                  "local co = coroutine.create(function()\n"
                  "  " FOR_CLOSE("function() while true do end end") " coroutine.yield() end\n"
                  "end)\n"
                  "coroutine.resume(co) pcall(coroutine.close, co) survived = true");
    /* The hook killed this coroutine, so its hooks are off: wrap must not close it, or the
     * closing value would spin unstopped (the test's timeout). */
    expect_killed(FOR_IT
                  "local f = coroutine.wrap(function()\n"
                  "  " FOR_CLOSE("function() while true do end end") " while true do end end\n"
                  "end)\n"
                  "pcall(f) survived = true");
    /* A closing value replaces the kill with its own error, at the top and inside pcall, or yields
     * out of the unwind in a coroutine. */
    expect_killed(FOR_IT "do " FOR_CLOSE("function() error('replaced') end") " while true do end end end\nsurvived = true");
    expect_killed(FOR_IT "pcall(function()\n"
                         "  " FOR_CLOSE("function() error('replaced') end") " while true do end end\n"
                         "end)\nsurvived = true");
    expect_killed(FOR_IT "local co = coroutine.wrap(function()\n"
                         "  pcall(function() " FOR_CLOSE("function() coroutine.yield('escaped') end") " while true do end end end)\n"
                         "  survived = true\n"
                         "end)\nco() survived = true");
    /* Closing values Lua runs while a kill unwinds stop before their first instruction, at the
     * top, inside pcall, on a coroutine that passes the kill up, and through xpcall's handler. */
    expect_killed(FOR_IT "do " FOR_CLOSE("function() survived = true end") " while true do end end end");
    expect_killed(FOR_IT "pcall(function() " FOR_CLOSE("function() survived = true end") " while true do end end end)");
    expect_killed(FOR_IT "local co = coroutine.wrap(function()\n"
                         "  " FOR_CLOSE("function() survived = true end") " pcall(function() while true do end end) end\n"
                         "end)\n"
                         "" FOR_CLOSE("function() survived = true end") " co() end");
    expect_killed(FOR_IT "xpcall(function() " FOR_CLOSE("function() error('replaced') end") " while true do end end end,\n"
                         "       function(e) survived = true return e end)\nsurvived = true");
}

/* ---- memory_kill ---- */

#define HUGE_REP "string.rep('x', 1 << 24)"
/* A closing value that notes the error it is closed with, then raises another in its place. */
#define FOR_SWALLOW "function(_, e) seen = e error('swallowed', 0) end"

/* refuse_next_growth(): the allocator refuses the next request to grow, once. */
static int refuse_next_growth(lua_State *L) {
    (void)L;
    refuse_next = true;
    return 0;
}

/* shrug_refusal(): what a refusal Lua shrugs off looks like. The stack can't grow, and
 * lua_checkstack only says so: returns its answer, false. */
static int shrug_refusal(lua_State *L) {
    refuse_next = true;
    lua_pushboolean(L, lua_checkstack(L, 10000));
    return 1;
}

static void memory_kill(void) {
    /* The brief's two. */
    expect_nomem("pcall(string.rep, 'x', 1 << 24) return 'survived'");
    expect_nomem("local t = {} for i = 1, 1e7 do t[i] = i end");
    /* xpcall's handler isn't called for it (Lua calls none for a memory error). */
    expect_nomem("xpcall(string.rep, function() return 'handled' end, 'x', 1 << 24) return 'x'");
    /* In coroutines: resume, wrap, and pcall inside. */
    expect_nomem("local co = coroutine.create(function() return string.rep('x', 1 << 24) end)\n"
                 "return coroutine.resume(co)");
    expect_nomem("return pcall(coroutine.wrap(function() local t = {} for i = 1, 1e7 do t[i] = i end end))");
    expect_nomem("local co = coroutine.wrap(function() return pcall(string.rep, 'x', 1 << 24) end) return co()");
    expect_nomem("local co = coroutine.create(function() local s = " HUGE_REP " end) coroutine.resume(co) survived = true");
    /* The cap in the reason is the VM's. */
    expect_end_at(__LINE__, 100 * KB, APPS_BUDGET_CALL, "return string.rep('x', 1 << 20)", APPS_CALL_NOMEM,
                  NOMEM_MSG("100"), NULL);
    /* Lua's own message, raised as is, is a memory error too (lapi.c lua_error): the app only
     * stops itself. */
    expect_nomem("pcall(error, 'not enough memory', 0) survived = true");

    /* Out of memory is sticky (ruling A2, amended) and can't be swallowed, even by the one piece
     * of app code Lua still runs during an unwind: a generic for's closing value. It sees the
     * error (seen) but can't keep it. At the top, in pcall, in a coroutine, round a loop, and
     * through xpcall (whose handler isn't called for a memory error). */
    expect_nomem_seen(FOR_IT "pcall(function() " FOR_CLOSE(FOR_SWALLOW) " local s = " HUGE_REP " end end) survived = true");
    expect_nomem_seen(FOR_IT FOR_CLOSE(FOR_SWALLOW) " local s = " HUGE_REP " end");
    expect_nomem_seen(FOR_IT "local co = coroutine.wrap(function()\n"
                             "  pcall(function() " FOR_CLOSE(FOR_SWALLOW) " local s = " HUGE_REP " end end) survived = true\n"
                             "end)\n"
                             "co() survived = true");
    expect_nomem_seen(FOR_IT "while true do\n"
                             "  pcall(function() " FOR_CLOSE(FOR_SWALLOW) " local t = {} for i = 1, 1e7 do t[i] = i end end end)\n"
                             "  survived = true\n"
                             "end");
    expect_nomem_seen(FOR_IT "xpcall(function() " FOR_CLOSE(FOR_SWALLOW) " local s = " HUGE_REP " end end,\n"
                             "       function(e) survived = e return e end)\n"
                             "survived = true");
    /* A closing value that runs on is stopped by the hook within 1000 instructions; one that
     * calls a binding is stopped there, by apps_vm_check. */
    expect_nomem_seen(FOR_IT "pcall(function()\n"
                             "  " FOR_CLOSE("function(_, e) seen = e while true do end end") " local s = " HUGE_REP " end\n"
                             "end)\n"
                             "survived = true");
    expect_nomem_seen(FOR_IT "pcall(function()\n"
                             "  " FOR_CLOSE("function(_, e) seen = e survived = ('a,b'):split(',') end") " local s = " HUGE_REP " end\n"
                             "end)\n"
                             "survived = true");
    /* The resizebox path (luaL_Buffer calls the allocator directly and never retries): string.rep
     * at the cap, with a closing value that repeats the identical string.rep. Even if that
     * identical request would now fit, no emergency collection ran, so the refusal stays sticky. */
    expect_nomem_seen(FOR_IT "pcall(function()\n"
                             "  " FOR_CLOSE("function(_, e) seen = e local s = " HUGE_REP " end") " local s = " HUGE_REP " end\n"
                             "end)\n"
                             "survived = true");

    /* A coroutine's stack that can't grow for resume's arguments: lua_checkstack fails, which
     * would be a catchable "too many arguments to resume". The main thread's stack has grown for
     * them first, so the request refused is the coroutine's. */
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    lua_pushcfunction(L, refuse_next_growth);
    lua_setglobal(L, "refuse_next_growth");
    int rc = call(vm,
                  "local args = {} for i = 1, 60 do args[i] = i end\n"
                  "local co = coroutine.create(function(...) return select('#', ...) end)\n"
                  "local function count(...) return select('#', ...) end\n"
                  "count(table.unpack(args))\n"
                  "refuse_next_growth()\n"
                  "coroutine.resume(co, table.unpack(args)) survived = true",
                  0, APPS_BUDGET_CALL, err, sizeof err);
    refuse_next = false;
    CHECK(rc == APPS_CALL_NOMEM && strcmp(err, NOMEM_MSG("256")) == 0, "resume: %s '%s'", call_name(rc), err);
    CHECK(apps_vm_mem_refusals(vm) == 1, "resume: %zu refusals", apps_vm_mem_refusals(vm));
    CHECK(raw_global(L, "survived") == LUA_TNIL, "resume: ran on after the refusal");
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* A refusal that Lua's own code shrugs off, as a string table that can't grow does, with no
     * check after it but apps_vm_call's own: the call still ends out of memory. */
    vm = new_vm(256 * KB);
    L = apps_vm_lua(vm);
    lua_pushcfunction(L, shrug_refusal);
    lua_setglobal(L, "shrug_refusal");
    rc = call(vm, "return shrug_refusal()", LUA_MULTRET, APPS_BUDGET_CALL, err, sizeof err);
    refuse_next = false;
    CHECK(rc == APPS_CALL_NOMEM && strcmp(err, NOMEM_MSG("256")) == 0 && lua_gettop(L) == 0, "shrugged: %s '%s', %d values",
          call_name(rc), err, lua_gettop(L));
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* The same shrugged refusal, but a later allocation succeeds before the check: Lua never
     * retried the refused stack growth, so it stays sticky, the hook kills within 1000
     * instructions, and survived never runs. (The successful table is a different request, not
     * the refused one's retry, so it mustn't clear the refusal.) */
    vm = new_vm(256 * KB);
    L = apps_vm_lua(vm);
    lua_pushcfunction(L, shrug_refusal);
    lua_setglobal(L, "shrug_refusal");
    rc = call(vm, "shrug_refusal() local t = {} for i = 1, 5000 do t[i % 8 + 1] = i end survived = true", 0,
              APPS_BUDGET_CALL, err, sizeof err);
    refuse_next = false;
    CHECK(rc == APPS_CALL_NOMEM && strcmp(err, NOMEM_MSG("256")) == 0 && raw_global(L, "survived") == LUA_TNIL,
          "shrug then alloc: %s '%s', survived %s", call_name(rc), err, lua_typename(L, raw_global(L, "survived")));
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- churn (ruling A2) ---- */

/* full_gc(): collects garbage and returns the bytes the heap holds then. */
static int full_gc(lua_State *L) {
    void *vm;
    lua_getallocf(L, &vm);
    lua_gc(L, LUA_GCCOLLECT);
    lua_pushinteger(L, (lua_Integer)apps_vm_mem_used(vm));
    return 1;
}

static void churn(void) {
    const size_t cap = 256 * KB;
    apps_vm_t *vm = new_vm(cap);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    lua_pushcfunction(L, full_gc);
    lua_setglobal(L, "full_gc");
    /* About 150 KB live, then about 6 MB of garbage made a kilobyte at a time. Lua starts
     * its next collection at twice the live size, past the cap, so the heap keeps reaching
     * the cap, and each refusal is met by an emergency collection, not a kill. */
    static const char src[] =
        "local live = {}\n"
        "for i = 1, 150 do live[i] = string.rep(string.char(65 + i % 26), 1000) .. i end\n"
        "local live_bytes = full_gc()\n"
        "local n = 0\n"
        "for round = 1, 4000 do\n"
        "  local junk = {string.rep('j', 500) .. round, {round}}\n"
        "  n = n + #junk[1]\n"
        "end\n"
        "return #live, live_bytes, n";
    int rc = call(vm, src, 3, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK, "got %s '%s'", call_name(rc), err);
    if (rc == APPS_CALL_OK) {
        lua_Integer live = lua_tointeger(L, 2);
        CHECK(lua_tointeger(L, 1) == 150 && lua_tointeger(L, 3) == 2014893, "the loops didn't finish: %lld %lld",
              (long long)lua_tointeger(L, 1), (long long)lua_tointeger(L, 3));
        CHECK(live >= 150 * (lua_Integer)KB && live <= 190 * (lua_Integer)KB, "live %lld bytes", (long long)live);
    }
    CHECK(apps_vm_mem_refusals(vm) > 0, "no refusal: the churn never reached the cap");
    CHECK(apps_vm_mem_peak(vm) <= cap && !apps_vm_dying(vm), "peak %zu, dying %d", apps_vm_mem_peak(vm), apps_vm_dying(vm));
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- stack_growth_oom: the re-reviewer's generic-for stack-growth swallow ---- */

/* A deep recursion where each level enters a generic for whose shared closing value swallows:
 * a stack growth is refused deep in, the memory error unwinds through every closer, and each
 * tries to keep it (and succeeds, before the fix, when its own identical stack growth is granted
 * by the GC that the close triggered). vm_alloc now models Lua's retry exactly, so a refusal Lua
 * never retries (stack growth runs under gcstopem) is final at once. The cap at which the growth
 * is refused shifts with the harness's heap baseline, so sweep a window: wherever a refusal
 * happens, the call must end NOMEM with survived unset. */
static void stack_growth_oom(void) {
    static const char src[] =
        "local sw = setmetatable({}, {__close = function(_, e) error('sw', 0) end})\n"
        "local live = {}\n"
        "local D = 156\n"
        "local function it() return 1 end\n"
        "local function work(n)\n"
        "  for _ in it, nil, nil, sw do\n"
        "    local p0\n"
        "    if n < D then return work(n + 1) + 1 end\n"
        "    for i = 1, 264 do live[#live + 1] = i end\n"
        "    local z = 0 for i = 1, 2500 do z = z + i end\n"
        "    return 0\n"
        "  end\n"
        "end\n"
        "pcall(work, 1)\n"
        "survived = true\n";
    int oom_caps = 0;
    for (size_t kb = 40; kb <= 90; kb++) {
        apps_vm_t *vm = new_vm(kb * KB);
        if (vm == NULL) {
            continue;
        }
        lua_State *L = apps_vm_lua(vm);
        char err[512] = "";
        int rc = call(vm, src, 0, UINT32_MAX, err, sizeof err);
        bool refused = apps_vm_mem_refusals(vm) > 0;
        bool survived = raw_global(L, "survived") != LUA_TNIL;
        lua_settop(L, 0);
        if (refused) {
            oom_caps++;
            CHECK(rc == APPS_CALL_NOMEM && !survived, "cap %zu KB: a stack-growth refusal was swallowed: %s survived=%d",
                  kb, call_name(rc), survived);
        }
        apps_vm_free(vm);
    }
    CHECK(oom_caps > 0, "no cap in 40-90 KB hit a refusal: the regression didn't exercise out of memory");
}

/* ---- external_kill ---- */

static void *kill_after_50_ms(void *vm) {
    struct timespec pause = {0, 50 * 1000 * 1000};
    nanosleep(&pause, NULL);
    apps_vm_kill(vm, "stopped by test");
    return NULL;
}

/* kill_now(): kills its VM, as another task would while C code runs, and returns. */
static int kill_now(lua_State *L) {
    void *vm;
    lua_getallocf(L, &vm);
    apps_vm_kill(vm, "stopped by test");
    return 0;
}

/* A kill that lands while C code runs, before anything raised it: src gets KILLED and runs
 * nothing after the call that should stop it. */
static void expect_killed_from_c_at(int line, const char *src) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    lua_pushcfunction(L, kill_now);
    lua_setglobal(L, "kill_now");
    int rc = call(vm, src, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK_AT(line, rc == APPS_CALL_KILLED && strcmp(err, "stopped by test") == 0, "%s\n  got %s '%s'", src, call_name(rc),
             err);
    CHECK_AT(line, lua_gettop(L) == 0 && raw_global(L, "survived") == LUA_TNIL, "%s\n  ran on after the kill", src);
    apps_vm_free(vm);
}
#define expect_killed_from_c(src) expect_killed_from_c_at(__LINE__, src)

static void external_kill(void) {
    /* While the main thread runs, in a coroutine, and inside pcall. The budget would let each
     * run for many seconds. */
    static const char *const loops[] = {
        "while true do end",
        "coroutine.wrap(function() while true do end end)() survived = true",
        "pcall(function() while true do end end) survived = true",
        "local co = coroutine.wrap(function() while true do coroutine.yield() end end) while true do co() end",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        apps_vm_t *vm = new_vm(256 * KB);
        char err[512] = "";
        pthread_t killer;
        if (pthread_create(&killer, NULL, kill_after_50_ms, vm) != 0) {
            CHECK(false, "no thread to kill from");
            apps_vm_free(vm);
            return;
        }
        int rc = call(vm, loops[i], 0, UINT32_MAX, err, sizeof err);
        pthread_join(killer, NULL);
        CHECK(rc == APPS_CALL_KILLED && strcmp(err, "stopped by test") == 0, "%s\n  got %s '%s'", loops[i], call_name(rc), err);
        CHECK(lua_gettop(apps_vm_lua(vm)) == 0 && apps_vm_dying(vm), "%s: stack %d, dying %d", loops[i],
              lua_gettop(apps_vm_lua(vm)), apps_vm_dying(vm));
        CHECK(raw_global(apps_vm_lua(vm), "survived") == LUA_TNIL, "%s: ran on after the kill", loops[i]);
        apps_vm_free(vm);
    }

    /* A kill while C code runs: each replacement's own check stops what comes next, and
     * apps_vm_call reports a kill even when the chunk returned. */
    expect_killed_from_c("kill_now()");
    expect_killed_from_c("pcall(kill_now) survived = true");
    expect_killed_from_c("xpcall(kill_now, function() end) survived = true");
    expect_killed_from_c("coroutine.resume(coroutine.create(kill_now)) survived = true");
    expect_killed_from_c("coroutine.wrap(kill_now)() survived = true");
    expect_killed_from_c("local co = coroutine.create(function() coroutine.yield() end)\n"
                         "coroutine.resume(co) kill_now() coroutine.close(co) survived = true");
    /* A generic for's closing value in a suspended coroutine, closed after a kill: sandbox_close
     * raises at apps_vm_check before lua_closethread, so the closing value never runs. Without that
     * check, close would run it (it sets survived) before the hook could stop it: this is where
     * banning <close> alone isn't enough, since a for-closer has no OP_TBC. */
    expect_killed_from_c(FOR_IT
                         "local co = coroutine.create(function()\n"
                         "  " FOR_CLOSE("function() survived = true end") " coroutine.yield() end\n"
                         "end)\n"
                         "coroutine.resume(co) kill_now() coroutine.close(co)");
    /* Every binding checks first (X8): split, trim and coroutine.create too. */
    expect_killed_from_c("kill_now() survived = ('a,b'):split(',')");
    expect_killed_from_c("kill_now() survived = ('  a '):trim()");
    expect_killed_from_c("kill_now() survived = coroutine.create(type)");

    /* A kill between calls: the next call runs nothing. The first kill's reason stays. */
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    CHECK(!apps_vm_dying(vm), "dying from the start");
    apps_vm_kill(vm, "stopped by test");
    apps_vm_kill(vm, "stopped again");
    CHECK(apps_vm_dying(vm), "apps_vm_kill didn't kill");
    int rc = call(vm, "ran = true", 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, "stopped by test") == 0, "got %s '%s'", call_name(rc), err);
    CHECK(lua_gettop(L) == 0 && raw_global(L, "ran") == LUA_TNIL, "a dying VM ran a chunk");
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- yield_through_pcall ---- */

static void yield_through_pcall(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    /* The brief's. */
    expect_returns(vm, "local co = coroutine.wrap(function() pcall(coroutine.yield, 1) return 2 end) return co(), co()",
                   "number:1, number:2");
    /* xpcall too, and pcall's results once resumed. */
    expect_returns(vm,
                   "local co = coroutine.wrap(function() xpcall(coroutine.yield, function(e) return e end, 1) return 2 end)\n"
                   "return co(), co()",
                   "number:1, number:2");
    expect_returns(vm,
                   "local co = coroutine.wrap(function() return pcall(coroutine.yield, 1) end)\n"
                   "local a = co() return a, co('back')",
                   "number:1, boolean:true, string:back");
    /* An error after the resume reaches pcall's continuation, and xpcall's handler. */
    expect_returns(vm,
                   "local co = coroutine.wrap(function()\n"
                   "  return pcall(function() coroutine.yield(1) error('after', 0) end)\n"
                   "end)\n"
                   "local a = co() return a, co()",
                   "number:1, boolean:false, string:after");
    expect_returns(vm,
                   "local co = coroutine.wrap(function()\n"
                   "  return xpcall(function() coroutine.yield(1) error('after', 0) end, function(e) return 'handled ' .. e end)\n"
                   "end)\n"
                   "local a = co() return a, co()",
                   "number:1, boolean:false, string:handled after");

    /* The rest of Lua's coroutine behaviour, through the replacements. */
    expect_returns(vm,
                   "local co = coroutine.create(function(a) local b = coroutine.yield(a + 1) return b * 2 end)\n"
                   "local ok1, v1 = coroutine.resume(co, 1)\n"
                   "local ok2, v2 = coroutine.resume(co, 10)\n"
                   "local ok3, e3 = coroutine.resume(co)\n"
                   "return ok1, v1, ok2, v2, ok3, e3, coroutine.status(co)",
                   "boolean:true, number:2, boolean:true, number:20, boolean:false, string:cannot resume dead coroutine, "
                   "string:dead");
    expect_returns(vm,
                   "local f = coroutine.wrap(function() error('inside') end)\n"
                   "local ok, e = pcall(function() f() end) return ok, e",
                   "boolean:false, string:main.lua:2: main.lua:1: inside");
    /* coroutine.close of a plain suspended coroutine (no closing value pending): it goes dead,
     * and a dead one is a no-op. (A closing value would run during the close; see stack_growth_oom
     * and the close-after-kill case in external_kill.) */
    expect_returns(vm,
                   "local co = coroutine.create(function() coroutine.yield() end)\n"
                   "coroutine.resume(co)\n"
                   "local a, b = coroutine.close(co), coroutine.status(co)\n"
                   "return a, b, coroutine.close(co)",
                   "boolean:true, string:dead, boolean:true");
    /* Lua's own errors, word for word. */
    expect_stock_error(vm, "pcall()");
    expect_stock_error(vm, "xpcall(type)");
    expect_stock_error(vm, "xpcall(type, 1)");
    expect_stock_error(vm, "coroutine.resume(1)");
    expect_stock_error(vm, "coroutine.create(1)");
    expect_stock_error(vm, "coroutine.wrap(1)");
    expect_stock_error(vm, "coroutine.close(1)");
    expect_stock_error(vm, "coroutine.close(coroutine.running())");
    expect_stock_error(vm, "local f = coroutine.wrap(function() error('inside') end)\nf()");
    expect_stock_error(vm, "local f = coroutine.wrap(function() return 1 end)\nf() f()");
    expect_stock_error(vm, "local co = coroutine.wrap(function() coroutine.close(coroutine.running()) end)\nco()");
    CHECK(!apps_vm_dying(vm), "errors made the VM dying");
    apps_vm_free(vm);
}

/* ---- deep_recursion ---- */

/* return ((...(1)...)), n parentheses deep. */
static void nested_parens(char *out, size_t cap, int n) {
    size_t len = 0;
    len += (size_t)snprintf(out + len, cap - len, "return ");
    for (int i = 0; i < n && len + 1 < cap; i++) {
        out[len++] = '(';
    }
    if (len + 1 < cap) {
        out[len++] = '1';
    }
    for (int i = 0; i < n && len + 1 < cap; i++) {
        out[len++] = ')';
    }
    out[len] = '\0';
}

static void deep_recursion(void) {
    static const char lua_recursion[] = "local function f(n) return 1 + f(n + 1) end return f(1)";
    char err[1024] = "";

    /* Lua-to-Lua recursion runs out of memory at 256 KB... */
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    int rc = call(vm, lua_recursion, 1, APPS_BUDGET_CALL, err, sizeof err);
    CHECK((rc == APPS_CALL_ERROR && strstr(err, "stack overflow") != NULL) || rc == APPS_CALL_NOMEM, "256 KB: %s '%s'",
          call_name(rc), err);
    CHECK(lua_gettop(L) == 0, "left %d values", lua_gettop(L));
    apps_vm_free(vm);

    /* ... and with room, it reaches Lua's own stack limit: an error, with a traceback. */
    vm = new_vm(64 * MB);
    L = apps_vm_lua(vm);
    rc = call(vm, lua_recursion, 1, UINT32_MAX, err, sizeof err);
    CHECK(rc == APPS_CALL_ERROR && strncmp(err, "main.lua:1: stack overflow\nstack traceback:\n", 44) == 0
              && strstr(err, "[C]:") == NULL,
          "64 MB: %s '%s'", call_name(rc), err);
    CHECK(lua_gettop(L) == 0 && !apps_vm_dying(vm), "stack %d, dying %d", lua_gettop(L), apps_vm_dying(vm));
    apps_vm_free(vm);

    /* C-level recursion: an __index chain, then the parser. */
    vm = new_vm(256 * KB);
    L = apps_vm_lua(vm);
    rc = call(vm, "local t = setmetatable({}, {__index = function(t, k) return t[k] end}) return t.x", 1, APPS_BUDGET_CALL,
              err, sizeof err);
    CHECK(rc == APPS_CALL_ERROR && strstr(err, "C stack overflow") != NULL && strstr(err, "[C]:") == NULL,
          "__index chain: %s '%s'", call_name(rc), err);
    char src[512];
    nested_parens(src, sizeof src, 200);
    CHECK(!apps_vm_load(vm, "main.lua", src, strlen(src), err, sizeof err)
              && (strstr(err, "C stack overflow") != NULL || strstr(err, "too many") != NULL),
          "200 parentheses: '%s'", err);
    /* Errors, not kills: the VM carries on. */
    CHECK(lua_gettop(L) == 0 && !apps_vm_dying(vm), "stack %d, dying %d", lua_gettop(L), apps_vm_dying(vm));
    expect_returns(vm, "return 1 + 1", "number:2");
    apps_vm_free(vm);
}

/* ---- traceback ---- */

static void traceback(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    /* The brief's: an error in inner, on line 3. */
    expect_error_text(vm,
                      "local x = 1\n"
                      "local y = 2\n"
                      "local function inner() error('boom') end\n"
                      "local function outer() inner() return x + y end\n"
                      "outer()\n",
                      "main.lua:3: boom\nstack traceback:\n"
                      "\tmain.lua:3: in upvalue 'inner'\n"
                      "\tmain.lua:4: in local 'outer'\n"
                      "\tmain.lua:5: in main chunk");
    /* A tail call and a C function in between: their lines go, the app's frames around them stay. */
    expect_error_text(vm,
                      "local function inner() error('boom') end\n"
                      "local function tail() return inner() end\n"
                      "local function cmp(a, b) tail() return a < b end\n"
                      "table.sort({2, 1}, cmp)\n",
                      "main.lua:1: boom\nstack traceback:\n"
                      "\tmain.lua:1: in function <main.lua:1>\n"
                      "\tmain.lua:3: in function <main.lua:3>\n"
                      "\tmain.lua:4: in main chunk");
    /* Only traceback lines are filtered: the message stays whole. */
    expect_error_text(vm, "error('a [C]: b (...tail calls...)')",
                      "main.lua:1: a [C]: b (...tail calls...)\nstack traceback:\n\tmain.lua:1: in main chunk");
    /* A runtime error, an error with no position, and error objects that aren't strings. */
    expect_error_text(vm, "local t = nil\nreturn t.x",
                      "main.lua:2: attempt to index a nil value (local 't')\nstack traceback:\n\tmain.lua:2: in main chunk");
    expect_error_text(vm, "error('plain', 0)", "plain\nstack traceback:\n\tmain.lua:1: in main chunk");
    expect_error_text(vm, "error(42)", "42\nstack traceback:\n\tmain.lua:1: in main chunk");
    expect_error_text(vm, "error({})", "(error object is a table value)\nstack traceback:\n\tmain.lua:1: in main chunk");
    expect_error_text(vm, "error()", "(error object is a nil value)\nstack traceback:\n\tmain.lua:1: in main chunk");

    /* A message over 1 KB is cut at 1 KB on a character boundary, and the traceback
     * follows: 'x' and 600 two-byte characters keep 'x' and 511 of them. */
    char want[2048] = "x";
    for (int i = 0; i < 511; i++) {
        strcat(want, "\xC3\xA9");
    }
    strcat(want, "\nstack traceback:\n\tmain.lua:1: in main chunk");
    expect_error_text(vm, "error('x' .. string.rep('\\195\\169', 600), 0)", want);

    /* A small err is cut on a character boundary, and always terminated: 4 bytes would end
     * inside the é. */
    lua_State *L = apps_vm_lua(vm);
    char small[16];
    memset(small, '#', sizeof small);
    int rc = call(vm, "error('caf\\195\\169 au lait', 0)", 0, APPS_BUDGET_CALL, small, 5);
    CHECK(rc == APPS_CALL_ERROR && strcmp(small, "caf") == 0, "cap 5: %s '%s'", call_name(rc), small);
    CHECK(lua_gettop(L) == 0, "left %d values", lua_gettop(L));
    rc = call(vm, "error('x')", 0, APPS_BUDGET_CALL, NULL, 0);
    CHECK(rc == APPS_CALL_ERROR && lua_gettop(L) == 0, "no err: %s, %d values", call_name(rc), lua_gettop(L));
    apps_vm_free(vm);
}

/* ---- call_contract ---- */

static apps_vm_t *probe_vm;
static int probe_calls;
static bool probe_in_call;
static int64_t probe_started;

/* probe(): a C function for apps, as the bindings are written. Counts the calls that get
 * past apps_vm_check, and notes what a call looks like from inside. */
static int probe(lua_State *L) {
    apps_vm_check(L);
    probe_calls++;
    probe_in_call = apps_vm_in_call(probe_vm);
    probe_started = apps_vm_call_started_ms(probe_vm);
    return 0;
}

static int nested_status = -1;
static bool nested_in_call;

/* nested(f): runs f through apps_vm_call from inside a call, noting its status and whether
 * the outer call still shows as running, then returns as if nothing happened. */
static int nested(lua_State *L) {
    char err[256];
    lua_pushvalue(L, 1);
    nested_status = (int)apps_vm_call(probe_vm, 0, 0, UINT32_MAX, err, sizeof err);
    nested_in_call = apps_vm_in_call(probe_vm);
    return 0;
}

static void call_contract(void) {
    apps_vm_t *vm = new_vm(256 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[512], got[512];
    probe_vm = vm;
    lua_pushcfunction(L, probe);
    lua_setglobal(L, "probe");
    lua_pushcfunction(L, nested);
    lua_setglobal(L, "nested");

    /* The results replace the function and its arguments, above what was below them; err
     * is "". nresults is kept to. */
    lua_pushliteral(L, "below");
    CHECK(apps_vm_load(vm, "main.lua", "return ...", 10, err, sizeof err), "%s", err);
    lua_pushinteger(L, 4);
    lua_pushinteger(L, 5);
    strcpy(err, "untouched");
    int rc = apps_vm_call(vm, 2, LUA_MULTRET, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK && err[0] == '\0' && strcmp(describe(L, got, sizeof got), "string:below, number:4, number:5") == 0,
          "MULTRET: %s '%s' %s", call_name(rc), err, got);
    lua_settop(L, 1);
    CHECK(apps_vm_load(vm, "main.lua", "return 1, 2, 3", 14, err, sizeof err), "%s", err);
    rc = apps_vm_call(vm, 0, 1, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK && strcmp(describe(L, got, sizeof got), "string:below, number:1") == 0, "1 result: %s %s",
          call_name(rc), got);
    /* An error pops the function and its arguments and leaves what was below. */
    lua_settop(L, 1);
    CHECK(apps_vm_load(vm, "main.lua", "error('boom')", 13, err, sizeof err), "%s", err);
    lua_pushinteger(L, 4);
    lua_pushinteger(L, 5);
    rc = apps_vm_call(vm, 2, 1, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_ERROR && strncmp(err, "main.lua:1: boom\nstack traceback:\n", 34) == 0
              && strcmp(describe(L, got, sizeof got), "string:below") == 0,
          "error: %s '%s' %s", call_name(rc), err, got);
    lua_settop(L, 0);
    /* No function at all: an error, and the stack as it was. */
    lua_pushinteger(L, 1);
    rc = apps_vm_call(vm, 1, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_ERROR && lua_gettop(L) == 1, "no function: %s, %d values", call_name(rc), lua_gettop(L));
    lua_settop(L, 0);

    /* In a call: in_call, and when it began. Between calls: not in one. */
    fake_ms = 123456;
    CHECK(!apps_vm_in_call(vm), "in a call between calls");
    rc = call(vm, "probe() return true", 1, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK && probe_calls == 1 && probe_in_call && probe_started == 123456,
          "inside: %s '%s', %d calls, in a call %d, started %lld", call_name(rc), err, probe_calls, probe_in_call,
          (long long)probe_started);
    CHECK(!apps_vm_in_call(vm), "still in a call after it");
    lua_settop(L, 0);

    /* The budget is per call: a loop's measured count, run twice under a budget just above
     * it. The count goes in steps of 1000, from wherever the hook's countdown stood. */
    static const char loop[] = "local s = 0 for i = 1, 200000 do s = s + i end return s";
    uint64_t before = apps_vm_instructions(vm);
    rc = call(vm, loop, 1, APPS_BUDGET_CALL, err, sizeof err);
    uint64_t used = apps_vm_instructions(vm) - before;
    CHECK(rc == APPS_CALL_OK && lua_tointeger(L, -1) == 20000100000 && used >= 200000 && used % 1000 == 0,
          "loop: %s '%s', %llu instructions", call_name(rc), err, (unsigned long long)used);
    lua_settop(L, 0);
    for (int i = 0; i < 2; i++) {
        rc = call(vm, loop, 1, (uint32_t)used + 2000, err, sizeof err);
        CHECK(rc == APPS_CALL_OK, "run %d under %llu: %s '%s'", i, (unsigned long long)used + 2000, call_name(rc), err);
        lua_settop(L, 0);
    }

    /* A call inside a call runs under the outer call's budget, and the outer call still
     * shows. The C function ignores the kill, and the chunk still runs nothing after it:
     * the thread that raised the kill raises it again before its next instruction. */
    rc = call(vm, "nested(function() while true do end end) survived = true", 0, 100000, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, BUDGET_MSG("100000")) == 0 && lua_gettop(L) == 0, "nested: %s '%s'",
          call_name(rc), err);
    CHECK(nested_status == APPS_CALL_KILLED && nested_in_call, "inside: %s, in a call %d", call_name(nested_status),
          nested_in_call);
    CHECK(raw_global(L, "survived") == LUA_TNIL, "ran on after the nested kill");
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* apps_vm_check raises the kill before a binding does any work: here from a chunk run
     * outside apps_vm_call, after a kill. */
    vm = new_vm(256 * KB);
    L = apps_vm_lua(vm);
    probe_vm = vm;
    probe_calls = 0;
    lua_pushcfunction(L, probe);
    lua_setglobal(L, "probe");
    apps_vm_kill(vm, "stopped by test");
    CHECK(apps_vm_load(vm, "main.lua", "probe()", 7, err, sizeof err), "%s", err);
    rc = lua_pcall(L, 0, 0, 0);
    CHECK(rc == LUA_ERRRUN && lua_type(L, -1) == LUA_TLIGHTUSERDATA && probe_calls == 0, "rc %d, %s, %d calls", rc,
          luaL_typename(L, -1), probe_calls);
    lua_settop(L, 0);
    /* Outside apps_vm_call too, the hook stops a dying VM's code (the test's timeout). */
    CHECK(apps_vm_load(vm, "main.lua", "while true do end", 17, err, sizeof err), "%s", err);
    rc = lua_pcall(L, 0, 0, 0);
    CHECK(rc == LUA_ERRRUN && lua_type(L, -1) == LUA_TLIGHTUSERDATA, "loop: rc %d, %s", rc, luaL_typename(L, -1));
    lua_settop(L, 0);
    apps_vm_free(vm);
    fake_ms = 0;
}

/* ---- table_slots and charge: for C that loops where no count hook fires ---- */

/* slots(v): apps_vm_table_slots of the first argument, by a positive index, by a negative one with
 * it on top, and of an index with nothing there. */
static int slots(lua_State *L) {
    lua_settop(L, 1);
    lua_pushinteger(L, (lua_Integer)apps_vm_table_slots(L, 1));
    lua_pushinteger(L, (lua_Integer)apps_vm_table_slots(L, -2));
    lua_pushinteger(L, (lua_Integer)apps_vm_table_slots(L, 4));
    return 3;
}

/* charge(n): apps_vm_charge(L, n), as a binding that loops in C would call it. */
static int charge(lua_State *L) {
    apps_vm_charge(L, (unsigned)luaL_checkinteger(L, 1));
    return 0;
}

/* oom_survived(): a request far past the cap, in a protected call of its own that catches the error: the
 * binding survives, returns, and the VM is out of memory without anything having raised the kill yet.
 * Returns the status of that call. */
static int huge_request(lua_State *L) {
    lua_newuserdatauv(L, 64 * MB, 0);
    return 0;
}

static int oom_survived(lua_State *L) {
    lua_pushcfunction(L, huge_request);
    lua_pushinteger(L, lua_pcall(L, 0, 0, 0));
    return 1;
}

static apps_vm_t *probe_vm_with(size_t cap) {
    apps_vm_t *vm = new_vm(cap);
    lua_State *L = apps_vm_lua(vm);
    lua_pushcfunction(L, slots);
    lua_setglobal(L, "slots");
    lua_pushcfunction(L, charge);
    lua_setglobal(L, "charge");
    lua_pushcfunction(L, kill_now);
    lua_setglobal(L, "kill_now");
    lua_pushcfunction(L, oom_survived);
    lua_setglobal(L, "oom_survived");
    return vm;
}

static void table_slots(void) {
    apps_vm_t *vm = probe_vm_with(4 * MB);
    /* What lua_next walks: the array part's allocated size and the hash part's, by any index. */
    expect_returns(vm, "return slots({})", "number:0, number:0, number:0");
    expect_returns(vm, "return slots({1, 2, 3})", "number:3, number:3, number:0");
    expect_returns(vm, "return slots({a = 1, b = 2, c = 3})", "number:4, number:4, number:0");
    expect_returns(vm, "return slots({1, 2, 3, a = 1})", "number:4, number:4, number:0");
    /* #t on a table whose last slots are empty moves its alimit (a hint) below its real size, 4097 of 8192: lua_next
     * walks the real size, and so do the slots. */
    expect_returns(vm,
                   "local t = {} for i = 1, 8192 do t[i] = true end for i = 4098, 8192 do t[i] = nil end\n"
                   "local n = #t return n, slots(t)",
                   "number:4097, number:8192, number:8192, number:0");
    /* Grown, then emptied: the slots stay, so a walk costs what the table held. */
    expect_returns(vm, "local t = {} for i = 1, 8192 do t[i] = true end return slots(t)", "number:8192, number:8192, number:0");
    expect_returns(vm,
                   "local t = {} for i = 1, 8192 do t[i] = true end for i = 1, 8191 do t[i] = nil end\n"
                   "return slots(t)",
                   "number:8192, number:8192, number:0");
    expect_returns(vm, "local t = {} for i = 1, 100 do t['k' .. i] = i end return slots(t)", "number:128, number:128, number:0");
    expect_returns(vm,
                   "local t = {} for i = 1, 100 do t['k' .. i] = i end for k in pairs(t) do t[k] = nil end\n"
                   "return (slots(t)), next(t)",
                   "number:128, nil:nil");
    /* Anything that isn't a table, including the other collectable values lua_topointer gives an address. */
    expect_returns(vm, "return slots(nil)", "number:0, number:0, number:0");
    expect_returns(vm, "return slots(1)", "number:0, number:0, number:0");
    expect_returns(vm, "return slots(true)", "number:0, number:0, number:0");
    expect_returns(vm, "return slots('a string that is not short enough to be an interned one, for sure, no way')",
                   "number:0, number:0, number:0");
    expect_returns(vm, "return slots(slots)", "number:0, number:0, number:0");
    expect_returns(vm, "return slots(function() end)", "number:0, number:0, number:0");
    expect_returns(vm, "return slots(coroutine.create(function() end))", "number:0, number:0, number:0");
    apps_vm_free(vm);
}

static void charge_case(void) {
    char err[512];
    lua_State *L;
    int rc;

    /* The lifetime counter takes what is charged, on top of what the hook counts. */
    apps_vm_t *vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    uint64_t before = apps_vm_instructions(vm);
    rc = call(vm, "charge(1500) charge(0) return true", 1, APPS_BUDGET_CALL, err, sizeof err);
    uint64_t added = apps_vm_instructions(vm) - before;
    CHECK(rc == APPS_CALL_OK && added >= 1500 && (added - 1500) % 1000 == 0 && lua_toboolean(L, -1),
          "charge(1500): %s '%s', %llu instructions", call_name(rc), err, (unsigned long long)added);
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* Over the call's budget it kills with the budget reason, and what follows doesn't run. */
    vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    rc = call(vm, "charge(60000) first = true charge(60000) second = true", 0, 100000, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, BUDGET_MSG("100000")) == 0, "over the budget: %s '%s'", call_name(rc), err);
    CHECK(raw_global(L, "first") == LUA_TBOOLEAN && raw_global(L, "second") == LUA_TNIL,
          "a charge within the budget stopped, or the one over it didn't");
    lua_settop(L, 0);
    CHECK(apps_vm_dying(vm) && !apps_vm_in_call(vm), "dying %d, in a call %d", apps_vm_dying(vm), apps_vm_in_call(vm));
    apps_vm_free(vm);

    /* Past APPS_WALL_MS it kills with the wall reason: a charge of 0 is the clock check. */
    vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    fake_step = 600;  /* each read of the clock is 600 ms after the one before: the call's start, then each check */
    rc = call(vm, "charge(0) first = true charge(0) second = true", 0, UINT32_MAX, err, sizeof err);
    fake_step = 0;
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, WALL_MSG) == 0, "past the clock: %s '%s'", call_name(rc), err);
    CHECK(raw_global(L, "first") == LUA_TBOOLEAN && raw_global(L, "second") == LUA_TNIL,
          "the check at 600 ms stopped, or the one at 1200 ms didn't");
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* A dying VM raises its kill, with a charge of 0 too: here the kill lands while C code runs. */
    vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    rc = call(vm, "kill_now() charge(0) survived = true", 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_KILLED && strcmp(err, "stopped by test") == 0 && raw_global(L, "survived") == LUA_TNIL,
          "dying: %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* Out of memory, with the kill not raised yet, is a dying VM too: a charge of 0 raises the NOMEM kill. */
    vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    rc = call(vm, "local status = oom_survived() charge(0) survived = true", 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_NOMEM && strcmp(err, NOMEM_MSG("256")) == 0 && raw_global(L, "survived") == LUA_TNIL,
          "out of memory: %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
    apps_vm_free(vm);

    /* Outside apps_vm_call no budget is in force: the charge counts, and nothing is killed. */
    vm = probe_vm_with(256 * KB);
    L = apps_vm_lua(vm);
    before = apps_vm_instructions(vm);
    lua_pushcfunction(L, charge);
    lua_pushinteger(L, 3000000);
    rc = lua_pcall(L, 1, 0, 0);
    CHECK(rc == LUA_OK && !apps_vm_dying(vm) && apps_vm_instructions(vm) - before == 3000000,
          "outside a call: status %d, dying %d, %llu instructions", rc, apps_vm_dying(vm),
          (unsigned long long)(apps_vm_instructions(vm) - before));
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- c_stack (the Task 5 rulings) ----
 *
 * The recursion that goes deepest in C, each case to Lua's C-call limit, on a thread
 * whose stack is exactly TASK_STACK_HOST bytes above a guard page: overflowing it is a
 * crash, not a silent pass. 96 KB stands in for the device's 56 KB (host frames are about
 * 1.5-1.7 times Xtensa's), and the build has no sanitizers, which fatten frames. Prints
 * each case's high-water mark. The device is the authority (Task 15). */

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif

#ifdef UNDER_ASAN

static void c_stack(void) {
    CHECK(false, "c_stack measures stacks: build it with apps_host.build(..., sanitize=False)");
}

#else

#define TASK_STACK_HOST (96 * KB)

static char stack_err[512];
static lua_Integer stack_depth;

/* Runs src through apps_vm_call in a fresh VM: want, with err containing part (or the chunk's
 * one integer result, the depth it reached, in stack_depth). */
static void stack_chain(const char *name, const char *src, apps_call_t want, const char *part) {
    apps_vm_t *vm = new_vm(1 * MB);
    lua_State *L = apps_vm_lua(vm);
    int rc = call(vm, src, 1, APPS_BUDGET_CALL, stack_err, sizeof stack_err);
    CHECK(rc == (int)want && (part == NULL || strstr(stack_err, part) != NULL), "%s: got %s '%s'", name, call_name(rc), stack_err);
    if (rc == APPS_CALL_OK) {
        stack_depth = lua_tointeger(L, -1);
    }
    lua_settop(L, 0);
    apps_vm_free(vm);
}

static void stack_index_chain(void) {
    stack_chain("index_chain", "local t = setmetatable({}, {__index = function(t, k) return t[k] end}) return t.x",
                APPS_CALL_ERROR, "C stack overflow");
}

static void stack_parser(void) {
    static char src[512];
    nested_parens(src, sizeof src, 200);
    apps_vm_t *vm = new_vm(1 * MB);
    CHECK(!apps_vm_load(vm, "main.lua", src, strlen(src), stack_err, sizeof stack_err)
              && (strstr(stack_err, "C stack overflow") != NULL || strstr(stack_err, "too many") != NULL),
          "parser: '%s'", stack_err);
    apps_vm_free(vm);
}

static void stack_pcall_chain(void) {
    stack_chain("pcall_chain",
                "local depth = 0\n"
                "local function f(n) depth = n return pcall(f, n + 1) end\n"
                "f(1)\n"
                "return depth",
                APPS_CALL_OK, NULL);
}

static void stack_resume_chain(void) {
    stack_chain("resume_chain",
                "local depth = 0\n"
                "local function f(n) depth = n return coroutine.resume(coroutine.create(f), n + 1) end\n"
                "f(1)\n"
                "return depth",
                APPS_CALL_OK, NULL);
}

static void stack_concat_chain(void) {
    stack_chain("concat_chain",
                "local t = setmetatable({}, {__index = function(t, k) return table.concat(t, '', 1, 1) end})\n"
                "return t[1]",
                APPS_CALL_ERROR, "C stack overflow");
}

static void stack_tostring_chain(void) {
    stack_chain("tostring_chain",
                "local o = setmetatable({}, {__tostring = function(self) return string.format('%s', self) end})\n"
                "return tostring(o)",
                APPS_CALL_ERROR, "C stack overflow");
}

typedef struct {
    const char *name;
    void (*run)(void);
} stack_case_t;

static void *run_stack_case(void *c) {
    ((stack_case_t *)c)->run();
    return NULL;
}

/* Runs c on a thread whose stack is exactly TASK_STACK_HOST bytes: an mmap'd region above a
 * PROT_NONE page, painted first, so the deepest byte the case changed shows. Returns how
 * far down that is from the top (the high-water mark), or 0 if the thread didn't run. */
static size_t on_exact_stack(stack_case_t *c) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE), size = TASK_STACK_HOST;
    unsigned char *map = mmap(NULL, page + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        CHECK(false, "%s: mmap failed", c->name);
        return 0;
    }
    unsigned char *stack = map + page;
    int rc = mprotect(map, page, PROT_NONE);
    memset(stack, 0xA5, size);
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstack(&attr, stack, size);
    }
    if (rc == 0) {
        rc = pthread_create(&thread, &attr, run_stack_case, c);
    }
    if (rc == 0) {
        pthread_join(thread, NULL);
    }
    pthread_attr_destroy(&attr);
    size_t untouched = 0;
    while (untouched < size && stack[untouched] == 0xA5) {
        untouched++;
    }
    munmap(map, page + size);
    CHECK(rc == 0, "%s: no thread on the exact stack (%d)", c->name, rc);
    return rc == 0 ? size - untouched : 0;
}

static void c_stack(void) {
    static stack_case_t cases[] = {
        {"index_chain", stack_index_chain},   {"parser", stack_parser},
        {"pcall_chain", stack_pcall_chain},   {"resume_chain", stack_resume_chain},
        {"concat_chain", stack_concat_chain}, {"tostring_chain", stack_tostring_chain},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        stack_depth = 0;
        size_t used = on_exact_stack(&cases[i]);
        printf("c_stack: %-14s %6zu of %zu bytes", cases[i].name, used, (size_t)TASK_STACK_HOST);
        if (stack_depth > 0) {
            printf(" (depth %lld)", (long long)stack_depth);
        }
        printf("\n");
        fflush(stdout);
        CHECK(used > 0, "%s: the case didn't run", cases[i].name);
        /* The chains that count their depth went to the limit: past 30 of the 40 levels. */
        CHECK(stack_depth == 0 || stack_depth > 30, "%s: reached depth %lld", cases[i].name, (long long)stack_depth);
    }
}

#endif /* UNDER_ASAN */

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"bytecode", bytecode},
        {"bom_crlf", bom_crlf},
        {"gc_metatable", gc_metatable},
        {"strict_globals", strict_globals},
        {"registry_globals", registry_globals},
        {"sandbox_contents", sandbox_contents},
        {"patterns", patterns},
        {"plain_find", plain_find},
        {"split_trim", split_trim},
        {"find_linear", find_linear},
        {"split_linear", split_linear},
        {"rep_bounded", rep_bounded},
        {"move_bounded", move_bounded},
        {"insert_bounded", insert_bounded},
        {"remove_bounded", remove_bounded},
        {"sort_bounded", sort_bounded},
        {"concat_bounded", concat_bounded},
        {"len_proxy", len_proxy},
        {"suggest_capped", suggest_capped},
        {"allocator", allocator},
        {"cycles", cycles},
        {"new_without_memory", new_without_memory},
        {"excerpt", excerpt},
        {"suggest", suggest},
        {"infinite_loop", infinite_loop},
        {"wall_clock", wall_clock},
        {"tbc_refused", tbc_refused},
        {"load_on", load_on},
        {"short_coroutines", short_coroutines},
        {"kill_not_swallowed", kill_not_swallowed},
        {"memory_kill", memory_kill},
        {"stack_growth_oom", stack_growth_oom},
        {"churn", churn},
        {"external_kill", external_kill},
        {"yield_through_pcall", yield_through_pcall},
        {"deep_recursion", deep_recursion},
        {"traceback", traceback},
        {"call_contract", call_contract},
        {"table_slots", table_slots},
        {"charge", charge_case},
        {"c_stack", c_stack},
    };
    if (argc != 2) {
        fputs("usage: muse_apps_vm_harness <case>\n", stderr);
        return 2;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(held == 0, "%s: %zu bytes still held at the end", argv[1], held);
            return failures != 0;
        }
    }
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
