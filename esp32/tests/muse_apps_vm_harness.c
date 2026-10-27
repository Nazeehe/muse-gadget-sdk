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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void *count_realloc(void *ptr, size_t n) {
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
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

static int64_t fake_now_ms(void) {
    return 0;
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
         * wrapped, pcall and xpcall as Lua has them until Task 5; the five libraries. */
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

    /* The state carries on after a refusal. */
    expect_ints(vm, "local t = {} for i = 1, 1000 do t[i] = i end return #t", 1000);
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(held - apps_vm_mem_used(vm) == own, "accounting drifted after a collection");

    /* A table bomb ends in LUA_ERRMEM too, inside the cap. */
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

    /* The callers in Tasks 7 and 8. */
    static const char *const manifest[] = {"id", "name", "version", "api", "main", "memory_kb", "permissions", "files"};
    CHECK(apps_suggest("permission", manifest, 8) == manifest[6], "permission");
    CHECK(apps_suggest("entry", manifest, 8) == NULL, "entry");
    static const char *const handlers[] = {"start", "stop", "show", "hide", "message", "alarm"};
    CHECK(apps_suggest("on_tap", handlers, 6) == NULL, "on_tap");
    CHECK(apps_suggest("strat", handlers, 6) == handlers[0], "strat");
}

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
        {"allocator", allocator},
        {"cycles", cycles},
        {"new_without_memory", new_without_memory},
        {"excerpt", excerpt},
        {"suggest", suggest},
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
