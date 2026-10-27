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
 * Checks apps_json: the json module Muse apps get, the C decoder and encoder, and the pull
 * reader. Usage: muse_apps_json_harness <case>, one case per test in tests/test_muse_apps_json.py.
 * Exits 0 when the case holds and prints what it got when it doesn't.
 *
 * json.* runs through apps_vm_load and apps_vm_call in the real sandbox. apps_json_push and
 * apps_json_encode_value allocate on the app's heap and may raise, so they run only inside a
 * protected call: a probe a chunk calls, or a C function apps_vm_call runs. Every case frees what
 * it made, and the counting allocator below must end at 0 bytes: that is the leak check, since
 * LeakSanitizer doesn't run on macOS.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "apps_json.h"
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
#define NOINLINE_FOR_STACK __attribute__((noinline))
#define MB (1024 * KB)

/* A string literal and its length without the NUL, as two arguments. */
#define LIT(s) (s), (sizeof(s) - 1)

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

/* A realloc_fn that never has memory: apps_json_encode_value's own failure path. */
static void *refusing_realloc(void *ptr, size_t n) {
    (void)ptr;
    (void)n;
    return NULL;
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

/* The global name's value, pushed, read raw from C (lua_getglobal would raise the strict error). */
static int raw_global(lua_State *L, const char *name) {
    lua_pushglobaltable(L);
    lua_pushstring(L, name);
    int type = lua_rawget(L, -2);
    lua_remove(L, -2);
    return type;
}

/* ---- Running chunks ---- */

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

/* Loads src as main.lua and runs it through apps_vm_call. Returns the apps_call_t, or -1 when
 * apps_vm_load refused src; err gets the message either way. Results stay on the stack. */
static int call(apps_vm_t *vm, const char *src, int nresults, uint32_t budget, char *err, size_t err_len) {
    if (!apps_vm_load(vm, "main.lua", src, strlen(src), err, err_len)) {
        return -1;
    }
    return (int)apps_vm_call(vm, 0, nresults, budget, err, err_len);
}

/* src runs to its end. Chunks check with eq() and raise on a mismatch, so a failure's err (the
 * message and its traceback) names the line. */
static void run_ok_at(int line, apps_vm_t *vm, const char *src) {
    lua_State *L = apps_vm_lua(vm);
    char err[2048] = "";
    int rc = call(vm, src, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK_AT(line, rc == APPS_CALL_OK, "%s\n  got %s '%s'", src, call_name(rc), err);
    lua_settop(L, 0);
}
#define run_ok(vm, src) run_ok_at(__LINE__, vm, src)

/* What every chunk starts from. eq() compares a value and its number subtype, same() is a deep
 * comparison that does too, err_of() returns what f raises. */
static const char prelude[] =
    "function eq(got, want, what)\n"
    "  if got ~= want or math.type(got) ~= math.type(want) then\n"
    "    error((what or 'eq') .. ': got ' .. tostring(got) .. ' (' .. type(got) .. '), want '\n"
    "          .. tostring(want) .. ' (' .. type(want) .. ')', 2)\n"
    "  end\n"
    "end\n"
    "function same(a, b)\n"
    "  if type(a) ~= type(b) then return false end\n"
    "  if type(a) ~= 'table' then return a == b and math.type(a) == math.type(b) end\n"
    "  for k, v in pairs(a) do\n"
    "    if not same(v, b[k]) then return false end\n"
    "  end\n"
    "  for k in pairs(b) do\n"
    "    if a[k] == nil then return false end\n"
    "  end\n"
    "  return true\n"
    "end\n"
    "function err_of(f, ...)\n"
    "  local ok, e = pcall(f, ...)\n"
    "  if ok then error('expected an error, got ' .. tostring(e), 2) end\n"
    "  return e\n"
    "end\n";

/* ---- Probes: C functions the chunks call, which call the C entry points inside apps_vm_call ---- */

/* Whether the Lua string at idx is bytes[0, n). */
static bool string_is(lua_State *L, int idx, const char *bytes, size_t n) {
    size_t len;
    const char *s = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &len) : NULL;
    return s != NULL && len == n && memcmp(s, bytes, n) == 0;
}

/* Every byte of an error message is printable ASCII: it reaches Muse inside a JSON string. */
static bool ascii_text(const char *s) {
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) {
            return false;
        }
    }
    return true;
}

/* c_push(s) -> true, value | false, err. Two values sit below the call; the stack must come back
 * with them and nothing else on failure, or with them and one value on success. */
static int c_push(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    lua_settop(L, 1);
    lua_pushstring(L, "below");
    lua_pushinteger(L, 7);
    int top = lua_gettop(L);
    char err[96];
    memset(err, 'x', sizeof err - 1);
    err[sizeof err - 1] = '\0';
    bool ok = apps_json_push(L, s, len, err, sizeof err);
    int grew = lua_gettop(L) - top;
    CHECK(grew == (ok ? 1 : 0), "push %s and left %d new values", ok ? "succeeded" : "failed", grew);
    CHECK(string_is(L, top - 1, LIT("below")) && lua_tointeger(L, top) == 7, "the values below the call changed");
    CHECK(ok ? err[0] == '\0' : (err[0] != '\0' && ascii_text(err)), "err is '%s' after %s", err,
          ok ? "success" : "failure");
    if (ok) {
        lua_pushboolean(L, 1);
        lua_insert(L, -2);
        return 2;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, err);
    return 2;
}

/* c_validate(s) -> true | false, err. */
static int c_validate(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    char err[96];
    memset(err, 'x', sizeof err - 1);
    err[sizeof err - 1] = '\0';
    bool ok = apps_json_validate(s, len, err, sizeof err);
    CHECK(ok ? err[0] == '\0' : err[0] != '\0', "err is '%s' after %s", err, ok ? "success" : "failure");
    lua_pushboolean(L, ok);
    if (ok) {
        return 1;
    }
    lua_pushstring(L, err);
    return 2;
}

/* c_encode(v) -> text | nil, err. Encodes the value twice, by an index from the bottom and by a
 * negative one with a value above it, to the same result, and once more with len and err NULL.
 * Frees every result with the allocator's own free: a leak shows at the end of the case. */
static int c_encode(lua_State *L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    lua_pushvalue(L, 1);
    lua_pushstring(L, "above");
    int top = lua_gettop(L);
    size_t len = 99, len2 = 99;
    char err[96], err2[96];
    memset(err, 'x', sizeof err - 1);
    err[sizeof err - 1] = '\0';
    memset(err2, 'y', sizeof err2 - 1);
    err2[sizeof err2 - 1] = '\0';
    char *a = apps_json_encode_value(L, 1, count_realloc, &len, err, sizeof err);
    CHECK(lua_gettop(L) == top, "encode by index 1 left %d values, wanted %d", lua_gettop(L), top);
    char *b = apps_json_encode_value(L, -2, count_realloc, &len2, err2, sizeof err2);
    CHECK(lua_gettop(L) == top, "encode by index -2 left %d values, wanted %d", lua_gettop(L), top);
    char *c = apps_json_encode_value(L, 1, count_realloc, NULL, NULL, 0);
    CHECK(lua_gettop(L) == top, "encode with NULL len and err left %d values, wanted %d", lua_gettop(L), top);
    CHECK((a == NULL) == (b == NULL) && (a == NULL) == (c == NULL), "results %p, %p and %p", (void *)a, (void *)b,
          (void *)c);
    int results;
    if (a != NULL && b != NULL && c != NULL) {
        CHECK(len == len2 && memcmp(a, b, len) == 0 && memcmp(a, c, len + 1) == 0, "three encodings of one value differ");
        CHECK(a[len] == '\0' && strlen(a) == len, "the text isn't NUL-terminated at %zu", len);
        CHECK(err[0] == '\0' && err2[0] == '\0', "err is '%s' after success", err);
        lua_pushlstring(L, a, len);
        results = 1;
    } else {
        CHECK(len == 0 && len2 == 0, "failure left len %zu and %zu", len, len2);
        CHECK(err[0] != '\0' && strcmp(err, err2) == 0 && ascii_text(err), "err is '%s' and '%s'", err, err2);
        lua_pushnil(L);
        lua_pushstring(L, err);
        results = 2;
    }
    count_free(a);
    count_free(b);
    count_free(c);
    return results;
}

/* c_encode_nomem(v) -> err: realloc_fn has no memory for the result. */
static int c_encode_nomem(lua_State *L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    size_t len = 99;
    char err[96] = "";
    char *out = apps_json_encode_value(L, 1, refusing_realloc, &len, err, sizeof err);
    CHECK(out == NULL && len == 0 && lua_gettop(L) == 1, "got %p, len %zu, %d values", (void *)out, len, lua_gettop(L));
    lua_pushstring(L, err);
    return 1;
}

/* lightud() -> a light userdata that isn't json.null; fulluserdata() -> a full one. */
static int lightud(lua_State *L) {
    static char other;
    lua_pushlightuserdata(L, &other);
    return 1;
}

static int fulluserdata(lua_State *L) {
    lua_newuserdatauv(L, 8, 0);
    return 1;
}

/* A VM with json open, the probes and the prelude installed. */
static apps_vm_t *new_json_vm(size_t cap) {
    static const struct {
        const char *name;
        lua_CFunction fn;
    } probes[] = {
        {"c_push", c_push},
        {"c_validate", c_validate},
        {"c_encode", c_encode},
        {"c_encode_nomem", c_encode_nomem},
        {"lightud", lightud},
        {"fulluserdata", fulluserdata},
    };
    apps_vm_t *vm = new_vm(cap);
    if (vm == NULL) {
        fputs("apps_vm_new failed\n", stderr);
        exit(2);
    }
    lua_State *L = apps_vm_lua(vm);
    int top = lua_gettop(L);
    apps_json_open(L);
    CHECK(lua_gettop(L) == top, "apps_json_open left %d values on the stack", lua_gettop(L) - top);
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        lua_pushcfunction(L, probes[i].fn);
        lua_setglobal(L, probes[i].name);
    }
    char err[512] = "";
    int rc = call(vm, prelude, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK, "the prelude: %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
    return vm;
}

/* Runs f as a protected call in vm, so the C entry points inside it may raise. */
static void run_c_at(int line, apps_vm_t *vm, lua_CFunction f) {
    lua_State *L = apps_vm_lua(vm);
    char err[512] = "";
    lua_pushcfunction(L, f);
    int rc = (int)apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK_AT(line, rc == APPS_CALL_OK, "got %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
}
#define run_c(vm, f) run_c_at(__LINE__, vm, f)

/* The setup chunk builds what the work chunk then decodes or encodes. With room (roomy) both
 * end OK, so the work is sound; in a VM of cap bytes the setup still fits and the work ends NOMEM
 * with exactly the out-of-memory reason and a heap that never passed its cap: it was json that the
 * cap stopped, not the setup. */
static void memory_case_at(int line, size_t cap, bool roomy, const char *setup, const char *work) {
    char err[512] = "", want[128];
    snprintf(want, sizeof want, "out of memory: this app may use %zu KB (memory_kb in manifest.json)", cap / KB);
    for (int small = roomy ? 0 : 1; small < 2; small++) {
        apps_vm_t *vm = new_json_vm(small ? cap : 8 * MB);
        lua_State *L = apps_vm_lua(vm);
        int rc = call(vm, setup, 0, APPS_BUDGET_CALL, err, sizeof err);
        CHECK_AT(line, rc == APPS_CALL_OK, "%s VM, setup: %s '%s'\n%s", small ? "small" : "roomy", call_name(rc), err, setup);
        lua_settop(L, 0);
        rc = call(vm, work, 0, APPS_BUDGET_CALL, err, sizeof err);
        if (small) {
            CHECK_AT(line, rc == APPS_CALL_NOMEM && strcmp(err, want) == 0, "%s\n  want NOMEM '%s'\n  got  %s '%s'", work, want,
                     call_name(rc), err);
            CHECK_AT(line, apps_vm_mem_peak(vm) <= cap, "peak %zu over the %zu cap", apps_vm_mem_peak(vm), cap);
        } else {
            CHECK_AT(line, rc == APPS_CALL_OK, "%s\n  with room: %s '%s'", work, call_name(rc), err);
        }
        lua_settop(L, 0);
        apps_vm_free(vm);
    }
}
#define memory_case(cap, setup, work) memory_case_at(__LINE__, cap, true, setup, work)
#define memory_case_small_only(cap, setup, work) memory_case_at(__LINE__, cap, false, setup, work)

/* ==== json.decode ==== */

/* ---- decode_types (the brief's test_decode_types) ---- */

static void decode_types(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    /* c is the UTF-8 bytes of U+00E9 U+1F600, raw in the JSON text. */
    run_ok(vm,
           "local t = json.decode('{\"a\":1,\"b\":[true,false,null],\"c\":\"\303\251\360\237\230\200\","
           "\"d\":1.5,\"e\":9007199254740993}')\n"
           "eq(type(t), 'table')\n"
           "eq(math.type(t.a), 'integer', 'a'); eq(t.a, 1, 'a')\n"
           "eq(t.b[1], true, 'b[1]'); eq(t.b[2], false, 'b[2]'); eq(t.b[3], json.null, 'b[3]'); eq(#t.b, 3, '#b')\n"
           "eq(t.c, utf8.char(233, 128512), 'c'); eq(#t.c, 6, '#c')\n"
           "eq(math.type(t.d), 'float', 'd'); eq(t.d, 1.5, 'd')\n"
           "eq(math.type(t.e), 'integer', 'e'); eq(t.e, 9007199254740993, 'e')\n"
           "local n = 0 for _ in pairs(t) do n = n + 1 end eq(n, 5, 'keys')\n");

    /* Any value may be the top one, with whitespace around it. */
    run_ok(vm,
           "eq(json.decode('1'), 1); eq(json.decode(' -2.5 '), -2.5); eq(json.decode('true'), true)\n"
           "eq(json.decode('false'), false); eq(json.decode('null'), json.null); eq(json.decode('\"x\"'), 'x')\n"
           "eq(json.decode('\\n\\t\\r 7 \\r\\n'), 7, 'whitespace')\n");

    /* Shapes: containers nest, keys stay strings, the last of a repeated key wins, "" is a key,
     * null leaves no hole, and nothing has a metatable. */
    run_ok(vm,
           "local e = json.decode('{}'); eq(type(e), 'table'); eq(next(e), nil); eq(getmetatable(e), nil)\n"
           "local a = json.decode('[]'); eq(type(a), 'table'); eq(next(a), nil); eq(#a, 0)\n"
           "local s = json.decode('[1,[2,[3,[]]],{\"k\":[{}]}]')\n"
           "eq(#s, 3); eq(s[2][2][1], 3); eq(#s[2][2][2], 0); eq(type(s[3].k[1]), 'table')\n"
           "local o = json.decode('{\"1\":\"one\",\"a\":1,\"a\":2,\"\":\"empty\"}')\n"
           "eq(o['1'], 'one'); eq(o[1], nil); eq(o.a, 2); eq(o[''], 'empty')\n"
           "local h = json.decode('[null,1,null]'); eq(#h, 3); eq(h[1], json.null); eq(h[2], 1); eq(h[3], json.null)\n"
           "eq(getmetatable(json.decode('[1]')), nil); eq(getmetatable(json.decode('{\"a\":1}')), nil)\n"
           "eq(same(json.decode(' [ 1 , { \"a\" : [ true ] } ] '), {1, {a = {true}}}), true, 'whitespace inside')\n");
    apps_vm_free(vm);
}

/* ---- decode_numbers ---- */

static void decode_numbers(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    /* No '.', 'e' or 'E' and within int64: an integer. Anything else is a float. */
    run_ok(vm,
           "eq(json.decode('0'), 0); eq(json.decode('-0'), 0); eq(json.decode('123'), 123); eq(json.decode('-123'), -123)\n"
           "eq(json.decode('9223372036854775807'), math.maxinteger)\n"
           "eq(json.decode('-9223372036854775808'), math.mininteger)\n"
           "eq(json.decode('9223372036854775808'), 2^63)            -- one past int64: a float\n"
           "eq(json.decode('-9223372036854775809'), -2^63)\n"
           "eq(json.decode('18446744073709551615'), 18446744073709551615)   -- Lua's own numeral is a float too\n"
           "eq(json.decode('123456789012345678901234567890'), 123456789012345678901234567890)\n"
           "eq(json.decode('1.0'), 1.0); eq(json.decode('1e2'), 100.0); eq(json.decode('1E2'), 100.0)\n"
           "eq(json.decode('1e+2'), 100.0); eq(json.decode('1e-2'), 0.01); eq(json.decode('-1.5e3'), -1500.0)\n"
           "eq(json.decode('0.5'), 0.5); eq(json.decode('0.1'), 0.1); eq(json.decode('2.5E-1'), 0.25)\n"
           "eq(json.decode('0.30000000000000004'), 0.1 + 0.2)\n"
           "eq(json.decode('4.9406564584124654e-324'), 5e-324)\n"
           "eq(json.decode('1.7976931348623157e308'), 1.7976931348623157e308)\n"
           "local nz = json.decode('-0.0'); eq(nz, 0.0); eq(1 / nz, -math.huge, 'the sign of -0.0')\n"
           "eq(json.decode('1e999'), math.huge); eq(json.decode('-1e999'), -math.huge); eq(json.decode('1e-999'), 0.0)\n");

    /* A number token has at most 200 characters, in text json.decode and apps_json_validate both read. */
    run_ok(vm,
           "eq(json.decode('1' .. string.rep('0', 199)), 1e199)\n"
           "eq(json.decode('0.' .. string.rep('0', 198)), 0.0)\n"
           "local long = '1' .. string.rep('0', 200)\n"
           "eq(err_of(json.decode, long), 'json.decode: number longer than 200 characters at byte 1')\n"
           "eq(err_of(json.decode, '[1,' .. long .. ']'), 'json.decode: number longer than 200 characters at byte 4')\n"
           "eq(err_of(json.decode, '0.' .. string.rep('0', 199)), 'json.decode: number longer than 200 characters at byte 1')\n"
           "eq(c_validate(long), false)\n");
    apps_vm_free(vm);
}

/* ---- decode_strings ---- */

typedef struct {
    const char *json;
    size_t jlen;
    const char *want;
    size_t wlen;
} string_row_t;
#define SROW(json, want) {(json), sizeof(json) - 1, (want), sizeof(want) - 1}

static const string_row_t string_rows[] = {
    SROW("\"\"", ""),
    SROW("\"abc\"", "abc"),
    SROW("\"\\\"\"", "\""),
    SROW("\"\\\\\"", "\\"),
    SROW("\"\\/\"", "/"),
    SROW("\"\\b\\f\\n\\r\\t\"", "\b\f\n\r\t"),
    SROW("\"\\u0041\"", "A"),
    SROW("\"\\u00e9\"", "\303\251"),
    SROW("\"\\u00E9\"", "\303\251"),
    SROW("\"\\u20ac\"", "\342\202\254"),
    SROW("\"\\ud83d\\ude00\"", "\360\237\230\200"),
    SROW("\"\\uD83D\\uDE00\"", "\360\237\230\200"),
    SROW("\"\\ud800\\udc00\"", "\360\220\200\200"),
    SROW("\"\\udbff\\udfff\"", "\364\217\277\277"),
    SROW("\"\\u0000\"", "\0"),
    SROW("\"a\\u0000b\"", "a\0b"),
    SROW("\"\\u007f\"", "\177"),
    SROW("\"\\u0080\"", "\302\200"),
    SROW("\"\\u07ff\"", "\337\277"),
    SROW("\"\\u0800\"", "\340\240\200"),
    SROW("\"\\ud7ff\"", "\355\237\277"),
    SROW("\"\\ue000\"", "\356\200\200"),
    SROW("\"\\uffff\"", "\357\277\277"),
    SROW("\"\303\251\360\237\230\200\"", "\303\251\360\237\230\200"),
    SROW("\"x\\n\303\251\\u00e9y\"", "x\n\303\251\303\251y"),
    SROW("\"\\\"quoted\\\" and \\\\slash\\\\\"", "\"quoted\" and \\slash\\"),
};

/* The JSON text json[0, jlen) is a string holding exactly want[0, wlen). */
static void expect_decoded(lua_State *L, const char *json, size_t jlen, const char *want, size_t wlen, const char *what) {
    char err[96] = "";
    int top = lua_gettop(L);
    bool ok = apps_json_push(L, json, jlen, err, sizeof err);
    CHECK(ok && lua_gettop(L) == top + 1 && string_is(L, -1, want, wlen), "%s: ok %d, err '%s', %d values", what, ok, err,
          lua_gettop(L) - top);
    lua_settop(L, top);
    CHECK(apps_json_validate(json, jlen, err, sizeof err), "%s: validate: %s", what, err);
}

static int t_decode_strings(lua_State *L) {
    for (size_t i = 0; i < sizeof string_rows / sizeof string_rows[0]; i++) {
        char what[32];
        snprintf(what, sizeof what, "row %zu", i);
        expect_decoded(L, string_rows[i].json, string_rows[i].jlen, string_rows[i].want, string_rows[i].wlen, what);
    }

    /* A key is decoded as a value is. */
    char err[96] = "";
    static const char keys[] = "{\"a\\nb\":1,\"\\u00e9\":2}";
    CHECK(apps_json_push(L, keys, sizeof keys - 1, err, sizeof err), "keys: %s", err);
    lua_pushlstring(L, "a\nb", 3);
    lua_rawget(L, -2);
    CHECK(lua_tointeger(L, -1) == 1, "key a\\nb is %s", luaL_typename(L, -1));
    lua_pop(L, 1);
    lua_pushlstring(L, "\303\251", 2);
    lua_rawget(L, -2);
    CHECK(lua_tointeger(L, -1) == 2, "key e-acute is %s", luaL_typename(L, -1));
    lua_settop(L, 0);

    /* Long strings: past the 512 bytes (device) and 1024 bytes (host) of a luaL_Buffer's own space,
     * with an escape between two long runs, with thousands of escapes, and the longest string the
     * input limit allows (no escape, so no buffer). */
    size_t run = 2000;
    char *json = malloc(2 * run + 16), *want = malloc(2 * run + 16);
    size_t j = 0, w = 0;
    json[j++] = '"';
    memset(json + j, 'a', run);
    memset(want + w, 'a', run);
    j += run;
    w += run;
    memcpy(json + j, "\\n", 2);
    j += 2;
    want[w++] = '\n';
    memset(json + j, 'b', run);
    memset(want + w, 'b', run);
    j += run;
    w += run;
    json[j++] = '"';
    expect_decoded(L, json, j, want, w, "2000, an escape, 2000");
    free(json);
    free(want);

    size_t escapes = 4000;
    json = malloc(6 * escapes + 2);
    want = malloc(2 * escapes);
    j = 0;
    w = 0;
    json[j++] = '"';
    for (size_t i = 0; i < escapes; i++) {
        memcpy(json + j, "\\u00e9", 6);
        memcpy(want + w, "\303\251", 2);
        j += 6;
        w += 2;
    }
    json[j++] = '"';
    expect_decoded(L, json, j, want, w, "4000 escapes");
    free(json);
    free(want);

    size_t longest = APPS_JSON_MAX_INPUT - 2;
    json = malloc(longest + 2);
    json[0] = '"';
    memset(json + 1, 'x', longest);
    json[longest + 1] = '"';
    expect_decoded(L, json, longest + 2, json + 1, longest, "the longest string");
    free(json);
    return 0;
}

static void decode_strings(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_decode_strings);
    apps_vm_free(vm);
}

/* ---- decode_valid ---- */

typedef struct {
    const char *json;
    size_t len;
} text_t;
#define TEXT(json) {(json), sizeof(json) - 1}

static const text_t valid_texts[] = {
    TEXT("null"), TEXT("true"), TEXT("false"), TEXT("0"), TEXT("-0"), TEXT("1"), TEXT("-1.5e+10"), TEXT("0.5"),
    TEXT("1E-2"), TEXT("1e0"), TEXT("123456789012345678901234567890"), TEXT("[-0.0, 0.0, 1E400]"),
    TEXT("\"\""), TEXT("\"/\""), TEXT("\"\\/\""), TEXT("[]"), TEXT("{}"), TEXT(" [ ] "), TEXT(" { } "),
    TEXT("\t\r\n[1, 2 ,3]\n"), TEXT("{ \"a\" : 1 , \"b\" : [ ] }"), TEXT("[[[]]]"), TEXT("{\"\":0}"),
    TEXT("{\"k\":{\"k\":{\"k\":{}}}}"), TEXT("[true,false,null,0,\"\",[],{}]"),
    TEXT("\"\\u0000\""), TEXT("[\"\\u0000\",\"\\ud83d\\ude00\"]"),
    /* The ends of each UTF-8 range, raw: U+007F, U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF,
     * U+10000 and U+10FFFF. */
    TEXT("\"\177\""), TEXT("\"\302\200\""), TEXT("\"\337\277\""), TEXT("\"\340\240\200\""), TEXT("\"\355\237\277\""),
    TEXT("\"\356\200\200\""), TEXT("\"\357\277\277\""), TEXT("\"\360\220\200\200\""), TEXT("\"\364\217\277\277\""),
};

static int t_decode_valid(lua_State *L) {
    for (size_t i = 0; i < sizeof valid_texts / sizeof valid_texts[0]; i++) {
        char err[96] = "", rerr[96] = "";
        int top = lua_gettop(L);
        bool v = apps_json_validate(valid_texts[i].json, valid_texts[i].len, err, sizeof err);
        bool p = apps_json_push(L, valid_texts[i].json, valid_texts[i].len, rerr, sizeof rerr);
        CHECK(v && p && lua_gettop(L) == top + 1 && err[0] == '\0' && rerr[0] == '\0', "text %zu: validate %d '%s', push %d '%s'",
              i, v, err, p, rerr);
        lua_settop(L, top);
        apps_jr_t r;
        apps_jr_init(&r, valid_texts[i].json, valid_texts[i].len);
        CHECK(apps_jr_skip(&r) && apps_jr_end(&r), "text %zu: the reader's skip: %s", i, r.err);
    }
    return 0;
}

static void decode_valid(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_decode_valid);
    apps_vm_free(vm);
}

/* ---- decode_errors ---- */

typedef struct {
    const char *json;
    size_t len;
    const char *want;
} error_row_t;
#define EROW(json, want) {(json), sizeof(json) - 1, (want)}

/* Positions are 1-based; end of input is one past the last byte. */
static const error_row_t error_rows[] = {
    EROW("", "unexpected end of input at byte 1"),
    EROW("  ", "unexpected end of input at byte 3"),
    EROW("\n\t \r", "unexpected end of input at byte 5"),
    EROW("[1,2,]", "unexpected ']' at byte 6"),
    EROW("[1,2,3,]", "unexpected ']' at byte 8"),
    EROW("{\"a\":}", "unexpected '}' at byte 6"),
    EROW("{\"a\":[1,2,{\"b\":}]}", "unexpected '}' at byte 16"),
    EROW("[1,2", "unexpected end of input at byte 5"),
    EROW("[1 2]", "unexpected '2' at byte 4"),
    EROW("{\"a\" 1}", "unexpected '1' at byte 6"),
    EROW("{\"a\":1 \"b\":2}", "unexpected '\"' at byte 8"),
    EROW("{,\"a\":1}", "unexpected ',' at byte 2"),
    EROW("[,1]", "unexpected ',' at byte 2"),
    EROW("[1,,2]", "unexpected ',' at byte 4"),
    EROW("{\"a\":1,}", "unexpected '}' at byte 8"),
    EROW("[1,]", "unexpected ']' at byte 4"),
    EROW("{1:2}", "unexpected '1' at byte 2"),
    EROW("{'a':1}", "unexpected ''' at byte 2"),
    EROW("{\"a\":1}}", "unexpected '}' at byte 8"),
    EROW("[1]x", "unexpected 'x' at byte 4"),
    EROW("1 2", "unexpected '2' at byte 3"),
    EROW("]", "unexpected ']' at byte 1"),
    EROW(":", "unexpected ':' at byte 1"),
    /* Literals. */
    EROW("tru", "unexpected end of input at byte 4"),
    EROW("trux", "unexpected 'x' at byte 4"),
    EROW("True", "unexpected 'T' at byte 1"),
    EROW("nul", "unexpected end of input at byte 4"),
    EROW("nulll", "unexpected 'l' at byte 5"),
    EROW("falsy", "unexpected 'y' at byte 5"),
    EROW("tru e", "unexpected byte 0x20 at byte 4"),
    /* Numbers. */
    EROW("01", "unexpected '1' at byte 2"),
    EROW("-", "unexpected end of input at byte 2"),
    EROW("-x", "unexpected 'x' at byte 2"),
    EROW("1.", "unexpected end of input at byte 3"),
    EROW("[1.]", "unexpected ']' at byte 4"),
    EROW(".5", "unexpected '.' at byte 1"),
    EROW("+1", "unexpected '+' at byte 1"),
    EROW("1e", "unexpected end of input at byte 3"),
    EROW("1e+", "unexpected end of input at byte 4"),
    EROW("1.e3", "unexpected 'e' at byte 3"),
    EROW("0x10", "unexpected 'x' at byte 2"),
    EROW("Infinity", "unexpected 'I' at byte 1"),
    EROW("-Infinity", "unexpected 'I' at byte 2"),
    EROW("NaN", "unexpected 'N' at byte 1"),
    /* Strings: control characters, escapes. */
    EROW("\"abc", "unexpected end of input at byte 5"),
    EROW("\"a\001b\"", "unexpected byte 0x01 at byte 3"),
    EROW("\"a\nb\"", "unexpected byte 0x0A at byte 3"),
    EROW("\"a\tb\"", "unexpected byte 0x09 at byte 3"),
    EROW("{\"a\001\":1}", "unexpected byte 0x01 at byte 4"),
    EROW("\"\\x\"", "unexpected 'x' at byte 3"),
    EROW("[\"\\q\"]", "unexpected 'q' at byte 4"),
    EROW("\"\\", "unexpected end of input at byte 3"),
    EROW("\"\\u12\"", "invalid \\u escape at byte 2"),
    EROW("\"\\u12", "unexpected end of input at byte 6"),
    EROW("\"\\uZZZZ\"", "invalid \\u escape at byte 2"),
    /* A lone or unpaired surrogate is at its own backslash; bad digits are where the digits are. */
    EROW("\"\\ud800\"", "invalid \\u escape at byte 2"),
    EROW("[\"\\ud800\"]", "invalid \\u escape at byte 3"),
    EROW("{\"a\":\"\\ud800\"}", "invalid \\u escape at byte 7"),
    EROW("\"\\udc00\"", "invalid \\u escape at byte 2"),
    EROW("\"\\ud800\\u0041\"", "invalid \\u escape at byte 2"),
    EROW("\"\\ud800\\n\"", "invalid \\u escape at byte 2"),
    EROW("\"\\ud83d\\uZZZZ\"", "invalid \\u escape at byte 8"),
    EROW("\"\\ud800", "unexpected end of input at byte 8"),
    EROW("\"\\ud800\\", "unexpected end of input at byte 9"),
    EROW("\"\\ud800\\u00", "unexpected end of input at byte 12"),
    /* UTF-8 by RFC 3629: a stray or missing continuation byte, an overlong form, an encoded
     * surrogate, a code point past U+10FFFF, a lead byte that starts nothing. */
    EROW("\"\303\"", "invalid UTF-8 at byte 2"),
    EROW("\"\303(\"", "invalid UTF-8 at byte 2"),
    EROW("\"\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\300\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\301\277\"", "invalid UTF-8 at byte 2"),
    EROW("\"\340\200\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\340\237\277\"", "invalid UTF-8 at byte 2"),
    EROW("\"\355\240\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\355\277\277\"", "invalid UTF-8 at byte 2"),
    EROW("\"\360\200\200\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\360\217\277\277\"", "invalid UTF-8 at byte 2"),
    EROW("\"\364\220\200\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\365\200\200\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\370\210\200\200\200\"", "invalid UTF-8 at byte 2"),
    EROW("\"\377\"", "invalid UTF-8 at byte 2"),
    EROW("\"\360\237\230\"", "invalid UTF-8 at byte 2"),
    EROW("\"a\342\202\"", "invalid UTF-8 at byte 3"),
    /* Bytes that start no value, as hex since a raw one could break the JSON that carries the message. */
    EROW("\303\251", "unexpected byte 0xC3 at byte 1"),
    EROW("[\177]", "unexpected byte 0x7F at byte 2"),
    EROW("[\001]", "unexpected byte 0x01 at byte 2"),
    EROW("\357\273\277[]", "unexpected byte 0xEF at byte 1"),
    EROW("\000", "unexpected byte 0x00 at byte 1"),
    EROW("1\000", "unexpected byte 0x00 at byte 2"),
};

/* json[0, len) is refused with exactly want, bare, by apps_json_validate, apps_json_push and the
 * reader's skip: one decoder, one message. */
static void expect_refused(lua_State *L, const char *json, size_t len, const char *want, size_t row) {
    char verr[96] = "", perr[96] = "";
    int top = lua_gettop(L);
    bool v = apps_json_validate(json, len, verr, sizeof verr);
    bool p = apps_json_push(L, json, len, perr, sizeof perr);
    CHECK(!v && !p && strcmp(verr, want) == 0 && strcmp(perr, want) == 0 && lua_gettop(L) == top,
          "row %zu: want '%s'\n  validate %d '%s'\n  push %d '%s', %d new values", row, want, v, verr, p, perr,
          lua_gettop(L) - top);
    lua_settop(L, top);
    CHECK(ascii_text(want), "row %zu: the message isn't printable ASCII", row);
    apps_jr_t r;
    apps_jr_init(&r, json, len);
    bool s = apps_jr_skip(&r) && apps_jr_end(&r);
    CHECK(!s && strcmp(r.err, want) == 0, "row %zu: the reader says %d '%s'", row, s, r.err);
}

static int t_decode_errors(lua_State *L) {
    for (size_t i = 0; i < sizeof error_rows / sizeof error_rows[0]; i++) {
        expect_refused(L, error_rows[i].json, error_rows[i].len, error_rows[i].want, i);
    }
    return 0;
}

static void decode_errors(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_decode_errors);

    /* json.decode adds its own prefix to the bare text (ruling R6.1): the brief's messages. */
    run_ok(vm,
           "eq(err_of(json.decode, '[1,2,]'), \"json.decode: unexpected ']' at byte 6\")\n"
           "eq(err_of(json.decode, '{\"a\":}'), \"json.decode: unexpected '}' at byte 6\")\n"
           "eq(err_of(json.decode, ''), 'json.decode: unexpected end of input at byte 1')\n"
           "eq(err_of(json.decode, '\"\\\\ud800\"'), 'json.decode: invalid \\\\u escape at byte 2')\n"
           "eq(err_of(json.decode, '\"\\\\udc00\"'), 'json.decode: invalid \\\\u escape at byte 2')\n"
           "eq(err_of(json.decode, '\\xc3\\xa9'), 'json.decode: unexpected byte 0xC3 at byte 1')\n"
           "eq(err_of(json.decode, '\"\\xc3\"'), 'json.decode: invalid UTF-8 at byte 2')\n"
           "eq(err_of(json.decode, string.rep('[', 17) .. string.rep(']', 17)), 'json.decode: nested deeper than 16')\n"
           "eq(err_of(json.decode, string.rep(' ', 32769)), 'json.decode: input larger than 32 KB')\n");

    /* Raised from a Lua function, it carries the caller's position, as any binding's error does. */
    run_ok(vm,
           "local function boom() local v = json.decode('[') return v end\n"
           "eq(err_of(boom), 'main.lua:1: json.decode: unexpected end of input at byte 2')\n");
    apps_vm_free(vm);
}

/* ---- decode_limits (ruling R6.10) ---- */

/* Text nested depth deep in one of three shapes: arrays, objects, or both in turn. */
static size_t nested_text(char *out, int depth, int shape) {
    char closers[APPS_JSON_MAX_DEPTH + 2];
    size_t n = 0;
    int closing = 0;
    for (int i = 0; i < depth; i++) {
        if (shape == 0 || (shape == 2 && i % 2 == 0)) {
            out[n++] = '[';
            closers[closing++] = ']';
        } else {
            memcpy(out + n, "{\"a\":", 5);
            n += 5;
            closers[closing++] = '}';
        }
    }
    out[n++] = '1';
    while (closing > 0) {
        out[n++] = closers[--closing];
    }
    out[n] = '\0';
    return n;
}

static int t_decode_limits(lua_State *L) {
    char text[16 * (APPS_JSON_MAX_DEPTH + 2)];
    for (int depth = APPS_JSON_MAX_DEPTH; depth <= APPS_JSON_MAX_DEPTH + 1; depth++) {
        for (int shape = 0; shape < 3; shape++) {
            size_t n = nested_text(text, depth, shape);
            if (depth == APPS_JSON_MAX_DEPTH) {
                char err[96] = "", rerr[96] = "";
                int top = lua_gettop(L);
                bool v = apps_json_validate(text, n, err, sizeof err);
                bool p = apps_json_push(L, text, n, rerr, sizeof rerr);
                CHECK(v && p && lua_gettop(L) == top + 1, "depth %d shape %d: validate %d '%s', push %d '%s'", depth, shape,
                      v, err, p, rerr);
                lua_settop(L, top);
                apps_jr_t r;
                apps_jr_init(&r, text, n);
                CHECK(apps_jr_skip(&r) && apps_jr_end(&r), "depth %d shape %d: reader: %s", depth, shape, r.err);
            } else {
                expect_refused(L, text, n, "nested deeper than 16", (size_t)(depth * 10 + shape));
            }
        }
    }

    /* The input limit: exactly 32768 bytes pass the size check, 32769 don't, and the check comes
     * before any scanning (so garbage that long is "too large", not "unexpected"). */
    size_t max = APPS_JSON_MAX_INPUT;
    char *big = malloc(max + 2);
    big[0] = '"';
    memset(big + 1, 'x', max - 2);
    big[max - 1] = '"';
    expect_decoded(L, big, max, big + 1, max - 2, "a string of exactly the limit");
    memset(big, ' ', max + 1);
    big[0] = '1';
    char err[96] = "";
    CHECK(apps_json_validate(big, max, err, sizeof err), "1 and spaces to the limit: %s", err);
    expect_refused(L, big, max + 1, "input larger than 32 KB", 1);
    memset(big, 1, max + 1);
    expect_refused(L, big, max + 1, "input larger than 32 KB", 2);
    free(big);
    return 0;
}

static void decode_limits(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_decode_limits);
    /* The same limits through json.decode. */
    run_ok(vm,
           "local function deep(d) local t = 1 for i = 1, d do t = '[' .. t .. ']' end return t end\n"
           "local n, t = 0, json.decode(deep(16)) while type(t) == 'table' do t = t[1] n = n + 1 end eq(n, 16)\n"
           "eq(err_of(json.decode, deep(17)), 'json.decode: nested deeper than 16')\n"
           "eq(json.decode(string.rep(' ', 32767) .. '1'), 1)\n"
           "eq(err_of(json.decode, string.rep(' ', 32768) .. '1'), 'json.decode: input larger than 32 KB')\n"
           "eq(#json.decode('\"' .. string.rep('x', 32766) .. '\"'), 32766)\n"
           "eq(err_of(json.decode, '\"' .. string.rep('x', 32767) .. '\"'), 'json.decode: input larger than 32 KB')\n");
    apps_vm_free(vm);
}

/* ==== json.encode ==== */

/* ---- encode_roundtrip (the brief's test_encode_roundtrip) ---- */

static void encode_roundtrip(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "eq(json.encode({}), '{}'); eq(json.encode({1, 2}), '[1,2]')\n"
           "local v = {\n"
           "  name = 'Muse \"gadget\"\\nline two', n = 42, neg = -7, big = math.maxinteger, small = math.mininteger,\n"
           "  f = 1.5, third = 1 / 3, tiny = 5e-324, huge = 1.7976931348623157e308, whole = 3.0, negzero = -0.0,\n"
           "  list = {1, 2.5, 'three', {4, 5}, {k = 'v'}, true, false, json.null, 1e100, 2^53},\n"
           "  nested = {a = {b = {c = {d = {}}}}}, empty = {},\n"
           "  unicode = utf8.char(233, 128512), ctl = '\\1\\2\\31\\127', slash = 'a/b', bs = 'back\\\\slash', nul = 'a\\0b',\n"
           "}\n"
           "local back = json.decode(json.encode(v))\n"
           "eq(same(back, v), true, 'round trip, number subtypes included')\n"
           "eq(1 / back.negzero, -math.huge, 'the sign of -0.0')\n");
    apps_vm_free(vm);
}

/* ---- encode_values ---- */

static void encode_values(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "eq(json.encode(nil), 'null'); eq(json.encode(true), 'true'); eq(json.encode(false), 'false')\n"
           "eq(json.encode(json.null), 'null')\n"
           "eq(json.encode(0), '0'); eq(json.encode(-5), '-5')\n"
           "eq(json.encode(math.maxinteger), '9223372036854775807')\n"
           "eq(json.encode(math.mininteger), '-9223372036854775808')\n"
           "-- floats: %.17g, plus '.0' where that reads as an integer, so they decode as floats again\n"
           "eq(json.encode(1.5), '1.5'); eq(json.encode(0.5), '0.5'); eq(json.encode(0.1), '0.10000000000000001')\n"
           "eq(json.encode(3.0), '3.0'); eq(json.encode(-0.0), '-0.0'); eq(json.encode(0.0), '0.0')\n"
           "eq(json.encode(1e15), '1000000000000000.0'); eq(json.encode(2^53), '9007199254740992.0')\n"
           "eq(json.encode(1e100), '1e+100'); eq(json.encode(-1e-7), '-9.9999999999999995e-08')\n"
           "eq(json.encode(math.pi), '3.1415926535897931')\n"
           "eq(json.encode(5e-324), '4.9406564584124654e-324')\n"
           "eq(math.type(json.decode(json.encode(3.0))), 'float'); eq(math.type(json.decode(json.encode(3))), 'integer')\n"
           "-- arrays and objects\n"
           "eq(json.encode({1, 2, 3}), '[1,2,3]'); eq(json.encode({{}}), '[{}]'); eq(json.encode({{1}, {2}}), '[[1],[2]]')\n"
           "eq(json.encode({a = 1}), '{\"a\":1}'); eq(json.encode({a = {b = {}}}), '{\"a\":{\"b\":{}}}')\n"
           "eq(json.encode({json.null, 1, json.null}), '[null,1,null]'); eq(json.encode({a = json.null}), '{\"a\":null}')\n"
           "local t = {} t[3] = 'c' t[2] = 'b' t[1] = 'a'   -- keys 1..3, inserted backwards\n"
           "eq(json.encode(t), '[\"a\",\"b\",\"c\"]')\n"
           "-- anything but the keys 1..n (n >= 1) is an object, and an integer key becomes its text\n"
           "eq(json.encode({[10] = 'x'}), '{\"10\":\"x\"}'); eq(json.encode({[-1] = true}), '{\"-1\":true}')\n"
           "eq(json.encode({[0] = 1}), '{\"0\":1}'); eq(json.encode({[2] = 1}), '{\"2\":1}')\n"
           "eq(json.encode({[math.mininteger] = 1}), '{\"-9223372036854775808\":1}')\n"
           "eq(same(json.decode(json.encode({[1] = 1, [3] = 3})), {['1'] = 1, ['3'] = 3}), true, 'a hole')\n"
           "eq(same(json.decode(json.encode({[-1] = 'a', [2] = 'b', [3] = 'c'})), {['-1'] = 'a', ['2'] = 'b', ['3'] = 'c'}), true, 'as many keys as the largest')\n"
           "eq(same(json.decode(json.encode({[0] = 'z', 'a', 'b'})), {['0'] = 'z', ['1'] = 'a', ['2'] = 'b'}), true, 'a zero key')\n"
           "eq(same(json.decode(json.encode({1, 2, x = 3})), {['1'] = 1, ['2'] = 2, x = 3}), true, 'mixed')\n"
           "eq(same(json.decode(json.encode({[1.0] = 'a', [2] = 'b'})), json.decode('[\"a\",\"b\"]')), true, 'float key 1.0')\n");
    apps_vm_free(vm);
}

/* ---- encode_strings ---- */

static const string_row_t encode_string_rows[] = {
    SROW("", "\"\""),
    SROW("abc", "\"abc\""),
    SROW("\"", "\"\\\"\""),
    SROW("\\", "\"\\\\\""),
    SROW("/", "\"/\""),
    SROW("\b\f\n\r\t", "\"\\b\\f\\n\\r\\t\""),
    SROW("\001", "\"\\u0001\""),
    SROW("\013", "\"\\u000b\""),
    SROW("\037", "\"\\u001f\""),
    SROW("\0", "\"\\u0000\""),
    SROW("a\0b", "\"a\\u0000b\""),
    SROW("\177", "\"\177\""),
    SROW("\303\251\360\237\230\200", "\"\303\251\360\237\230\200\""),
    SROW("a\nb", "\"a\\nb\""),
    SROW("\"quoted\" and \\slash\\", "\"\\\"quoted\\\" and \\\\slash\\\\\""),
    /* The ends of each UTF-8 range pass through. */
    SROW("\302\200", "\"\302\200\""),
    SROW("\337\277", "\"\337\277\""),
    SROW("\340\240\200", "\"\340\240\200\""),
    SROW("\355\237\277", "\"\355\237\277\""),
    SROW("\356\200\200", "\"\356\200\200\""),
    SROW("\357\277\277", "\"\357\277\277\""),
    SROW("\360\220\200\200", "\"\360\220\200\200\""),
    SROW("\364\217\277\277", "\"\364\217\277\277\""),
};

static const text_t invalid_utf8[] = {
    TEXT("\200"), TEXT("\303"), TEXT("\303("), TEXT("\300\200"), TEXT("\301\277"), TEXT("\340\200\200"),
    TEXT("\340\237\277"), TEXT("\355\240\200"), TEXT("\355\277\277"), TEXT("\360\200\200\200"),
    TEXT("\360\217\277\277"), TEXT("\364\220\200\200"), TEXT("\365\200\200\200"), TEXT("\370\210\200\200\200"),
    TEXT("\377"), TEXT("\360\237\230"), TEXT("a\342\202"), TEXT("ok \303"),
};

static int t_encode_strings(lua_State *L) {
    for (size_t i = 0; i < sizeof encode_string_rows / sizeof encode_string_rows[0]; i++) {
        const string_row_t *row = &encode_string_rows[i];
        size_t len = 0;
        char err[96] = "", what[32];
        lua_pushlstring(L, row->json, row->jlen);
        char *out = apps_json_encode_value(L, -1, count_realloc, &len, err, sizeof err);
        CHECK(out != NULL && len == row->wlen && memcmp(out, row->want, row->wlen) == 0, "row %zu: got %zu bytes, err '%s'", i,
              len, err);
        /* And what is encoded decodes to what went in. */
        if (out != NULL) {
            snprintf(what, sizeof what, "row %zu decoded", i);
            expect_decoded(L, out, len, row->json, row->jlen, what);
        }
        count_free(out);
        lua_pop(L, 1);
    }

    /* A string that isn't valid UTF-8 is refused, as a value, as a key, and inside containers. */
    for (size_t i = 0; i < sizeof invalid_utf8 / sizeof invalid_utf8[0]; i++) {
        for (int where = 0; where < 3; where++) {
            char err[96] = "";
            size_t len = 7;
            int top = lua_gettop(L);
            if (where == 0) {
                lua_pushlstring(L, invalid_utf8[i].json, invalid_utf8[i].len);
            } else if (where == 1) {
                lua_createtable(L, 0, 1);
                lua_pushlstring(L, invalid_utf8[i].json, invalid_utf8[i].len);
                lua_pushinteger(L, 1);
                lua_rawset(L, -3);
            } else {
                lua_createtable(L, 2, 0);
                lua_pushinteger(L, 1);
                lua_rawseti(L, -2, 1);
                lua_pushlstring(L, invalid_utf8[i].json, invalid_utf8[i].len);
                lua_rawseti(L, -2, 2);
            }
            char *out = apps_json_encode_value(L, -1, count_realloc, &len, err, sizeof err);
            CHECK(out == NULL && len == 0 && strcmp(err, "can't encode a string that isn't valid UTF-8") == 0 &&
                      lua_gettop(L) == top + 1,
                  "invalid UTF-8 %zu in place %d: %p, len %zu, err '%s'", i, where, (void *)out, len, err);
            count_free(out);
            lua_settop(L, top);
        }
    }
    return 0;
}

static void encode_strings(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_encode_strings);
    /* Strings the json module takes round-trip: every byte below 0x20, and the ones that are escaped. */
    run_ok(vm,
           "local all = {} for i = 0, 255 do if i < 128 then all[#all + 1] = string.char(i) end end\n"
           "local s = table.concat(all)\n"
           "eq(json.decode(json.encode(s)), s, 'all of ASCII')\n"
           "eq(json.decode(json.encode(utf8.char(0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff))),\n"
           "   utf8.char(0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff), 'the ends of the UTF-8 ranges')\n");
    apps_vm_free(vm);
}

/* ---- encode_errors (the brief's test_encode_errors) ---- */

static void encode_errors(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "eq(err_of(json.encode, function() end), \"json.encode: can't encode a function\")\n"
           "eq(err_of(json.encode, 0 / 0), \"json.encode: can't encode NaN or infinity\")\n"
           "eq(err_of(json.encode, math.huge), \"json.encode: can't encode NaN or infinity\")\n"
           "eq(err_of(json.encode, -math.huge), \"json.encode: can't encode NaN or infinity\")\n"
           "eq(err_of(json.encode, {1, {2, 0 / 0}}), \"json.encode: can't encode NaN or infinity\")\n"
           "eq(err_of(json.encode, coroutine.create(function() end)), \"json.encode: can't encode a thread\")\n"
           "eq(err_of(json.encode, lightud()), \"json.encode: can't encode a userdata\")\n"
           "eq(err_of(json.encode, fulluserdata()), \"json.encode: can't encode a userdata\")\n"
           "eq(err_of(json.encode, {a = {b = function() end}}), \"json.encode: can't encode a function\")\n"
           "-- a table that holds itself runs into the depth limit, as an array or as an object\n"
           "local t = {} t[1] = t\n"
           "eq(err_of(json.encode, t), 'json.encode: nested deeper than 16')\n"
           "local o = {} o.self = o\n"
           "eq(err_of(json.encode, o), 'json.encode: nested deeper than 16')\n"
           "-- strings that aren't UTF-8, as values and as keys\n"
           "local bad = \"json.encode: can't encode a string that isn't valid UTF-8\"\n"
           "eq(err_of(json.encode, '\\xff'), bad); eq(err_of(json.encode, {1, {'\\xc3'}}), bad)\n"
           "eq(err_of(json.encode, {['\\xc3'] = 1}), bad); eq(err_of(json.encode, {a = {['k\\x80'] = 1}}), bad)\n"
           "eq(err_of(json.encode, '\\xed\\xa0\\x80'), bad)   -- an encoded surrogate\n"
           "-- keys that are neither strings nor integers\n"
           "eq(err_of(json.encode, {[true] = 1}), \"json.encode: can't encode a boolean key\")\n"
           "eq(err_of(json.encode, {[{}] = 1}), \"json.encode: can't encode a table key\")\n"
           "eq(err_of(json.encode, {[function() end] = 1}), \"json.encode: can't encode a function key\")\n"
           "eq(err_of(json.encode, {[1.5] = 1}), \"json.encode: can't encode a number key\")\n"
           "eq(err_of(json.encode, {[-math.huge] = 1}), \"json.encode: can't encode a number key\")\n"
           "eq(err_of(json.encode, {[json.null] = 1}), \"json.encode: can't encode a userdata key\")\n"
           "eq(err_of(json.encode, {[coroutine.create(function() end)] = 1}), \"json.encode: can't encode a thread key\")\n"
           "-- a first error stops the encoding, and no output half-written is returned\n"
           "eq(err_of(json.encode, {1, 2, 3, function() end, 5}), \"json.encode: can't encode a function\")\n");

    /* Raised from a Lua function it carries the caller's position, as decode's does. */
    run_ok(vm,
           "local function boom() local v = json.encode(function() end) return v end\n"
           "eq(err_of(boom), \"main.lua:1: json.encode: can't encode a function\")\n"
           "eq(err_of(function() json.encode() end):find('bad argument #1 to \\'encode\\' (value expected)', 1, true) ~= nil, true)\n");
    apps_vm_free(vm);
}

/* ---- encode_limits (ruling R6.10) ---- */

static void encode_limits(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "local function arrays(d) local t = {} for i = 2, d do t = {t} end return t end\n"
           "local function objects(d) local t = {} for i = 2, d do t = {a = t} end return t end\n"
           "eq(json.encode(arrays(16)), string.rep('[', 15) .. '{}' .. string.rep(']', 15))\n"
           "eq(json.encode(objects(16)), string.rep('{\"a\":', 15) .. '{}' .. string.rep('}', 15))\n"
           "eq(err_of(json.encode, arrays(17)), 'json.encode: nested deeper than 16')\n"
           "eq(err_of(json.encode, objects(17)), 'json.encode: nested deeper than 16')\n"
           "-- and what is encoded at the limit decodes at the limit\n"
           "eq(same(json.decode(json.encode(arrays(16))), arrays(16)), true)\n"
           "eq(same(json.decode(json.encode(objects(16))), objects(16)), true)\n"
           "-- a wide table at the limit, and one level deeper in a sibling\n"
           "local w = {arrays(15), arrays(15)} eq(type(json.encode(w)), 'string')\n"
           "eq(err_of(json.encode, {w}), 'json.encode: nested deeper than 16')\n"
           "eq(err_of(json.encode, {arrays(15), arrays(16)}), 'json.encode: nested deeper than 16')\n");
    apps_vm_free(vm);
}

/* ---- encode_large_nested (ruling B6) ---- */

static void encode_large_nested(void) {
    apps_vm_t *vm = new_json_vm(4 * MB);
    /* Text over 8 KB, so the output grows several times, some of them while lua_next is mid-table:
     * a luaL_Buffer holds its box at the top of the stack and can't be held across that. It round-trips. */
    run_ok(vm,
           "local obj = {}\n"
           "for i = 1, 120 do obj['key' .. i] = string.rep('v', 30) .. i end\n"
           "local list = {}\n"
           "for i = 1, 80 do list[i] = {id = i, name = 'item' .. i, tags = {'x', 'y', 'z'}, ratio = i + 0.5} end\n"
           "local v = {obj = obj, list = list, deep = {a = {b = {c = {d = {obj = obj}}}}}}\n"
           "local s = json.encode(v)\n"
           "eq(#s > 8192 and #s <= 32768, true, 'between 8 and 32 KB: ' .. #s)\n"
           "eq(same(json.decode(s), v), true, 'round trip')\n");

    /* Text past the decoder's limit, checked by its exact length. 2500 one-key objects in an
     * array (the output grows with a lua_next in flight at depth 2), 6000 of them for text of
     * 64 KB, and one object of 2000 keys. */
    run_ok(vm,
           "local many = {}\n"
           "for i = 1, 2500 do many[i] = {i = i} end\n"
           "local s = json.encode(many)\n"
           "local want = 2 + 2499 for i = 1, 2500 do want = want + 6 + #tostring(i) end\n"
           "eq(#s, want); eq(#s <= 32768, true); eq(same(json.decode(s), many), true, 'round trip')\n"
           "local more = {}\n"
           "for i = 1, 6000 do more[i] = {i = i} end\n"
           "local s2 = json.encode(more)\n"
           "local want2 = 2 + 5999 for i = 1, 6000 do want2 = want2 + 6 + #tostring(i) end\n"
           "eq(#s2, want2); eq(#s2 > 60000, true, 'over 60000: ' .. #s2)\n"
           "eq(s2:find('[{\"i\":1},{\"i\":2},{\"i\":3},', 1, true), 1); eq(s2:sub(-11), '{\"i\":6000}]')\n"
           "local big = {}\n"
           "for i = 1, 2000 do big['k' .. i] = 'v' .. i end\n"
           "local s3 = json.encode(big)\n"
           "local want3 = 2 + 1999 for i = 1, 2000 do want3 = want3 + #('k' .. i) + #('v' .. i) + 5 end\n"
           "eq(#s3, want3); eq(same(json.decode(s3), big), true, 'round trip')\n");
    apps_vm_free(vm);
}

/* ---- encode_shared_subtables (ruling R6.8) ---- */

static void encode_shared_subtables(void) {
    /* Depth 16 and about 4^15 visits of the same {} (the brief's chunk, with its table built in the
     * setup): every visit writes bytes, so the app's cap stops the encode, and the run ends NOMEM at
     * once, not after a billion visits. */
    memory_case_small_only(256 * KB, "t = {} for d = 1, 15 do t = {t, t, t, t} end", "return json.encode(t)\n");
    memory_case_small_only(256 * KB, "t = {} for d = 1, 15 do t = {a = t, b = t, c = t, d = t} end", "return json.encode(t)\n");
    /* One string held by many slots writes its bytes once for each. */
    memory_case(256 * KB, "local s = string.rep('x', 1000) t = {} for i = 1, 300 do t[i] = s end", "return json.encode(t)\n");
    /* Fewer visits than the cap allows still work: 2^15 visits of the {} are 5 * 2^15 - 3 bytes. */
    apps_vm_t *vm = new_json_vm(2 * MB);
    run_ok(vm,
           "local t = {} for d = 1, 15 do t = {t, t} end\n"
           "eq(#json.encode(t), 163837)\n");
    apps_vm_free(vm);
}

/* ---- encode_raw (ruling R6.7) ---- */

static void encode_raw(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "local calls = 0\n"
           "local function bump() calls = calls + 1 end\n"
           "local mt = {__index = function() bump() end, __newindex = function() bump() end,\n"
           "            __len = function() bump() return 99 end, __pairs = function() bump() end,\n"
           "            __tostring = function() bump() return 'x' end, __call = function() bump() end,\n"
           "            __eq = function() bump() return true end, __lt = function() bump() return true end,\n"
           "            __concat = function() bump() return '' end, __close = function() bump() end}\n"
           "local arr = setmetatable({10, 20, 30}, mt)\n"
           "local obj = setmetatable({a = 1, b = setmetatable({c = 2}, mt)}, mt)\n"
           "eq(json.encode(arr), '[10,20,30]')\n"
           "eq(same(json.decode(json.encode(obj)), {a = 1, b = {c = 2}}), true)\n"
           "eq(json.encode({inner = setmetatable({1, 2}, mt)}), '{\"inner\":[1,2]}')\n"
           "-- a table that only pretends, through __index and __len, to have elements has none\n"
           "eq(json.encode(setmetatable({}, {__index = function(t, i) bump() return i end, __len = function() bump() return 5 end})), '{}')\n"
           "eq(calls, 0, 'metamethods called')\n"
           "-- and decode builds plain tables\n"
           "eq(getmetatable(json.decode('{\"a\":[1]}').a), nil)\n");
    apps_vm_free(vm);
}

/* ==== The C entry points, the module, and memory ==== */

/* ---- validate (the brief's test_validate) ---- */

static void validate(void) {
    char err[96] = "untouched";
    static const char sample[] = "{\"a\":[1,2.5,true,null,\"x\"],\"b\":{\"c\":\"\\u00e9\"}} ";
    CHECK(apps_json_validate(sample, sizeof sample - 1, err, sizeof err) && err[0] == '\0', "valid sample: '%s'", err);
    /* The byte position is in err, bare (ruling R6.1). */
    CHECK(!apps_json_validate(LIT("{\"a\":}"), err, sizeof err) && strcmp(err, "unexpected '}' at byte 6") == 0, "got '%s'", err);
    CHECK(!apps_json_validate(LIT(""), err, sizeof err) && strcmp(err, "unexpected end of input at byte 1") == 0, "got '%s'", err);
    CHECK(!apps_json_validate(LIT("[1,2]x"), err, sizeof err) && strcmp(err, "unexpected 'x' at byte 6") == 0, "got '%s'", err);
    /* Cut to err_len, always terminated; no err at all is fine. */
    char cut[12];
    CHECK(!apps_json_validate(LIT("[1,2,]"), cut, sizeof cut) && strcmp(cut, "unexpected ") == 0, "cut: '%s'", cut);
    char one[1] = {'x'};
    CHECK(!apps_json_validate(LIT("["), one, sizeof one) && one[0] == '\0', "cut to 1: first byte %d", one[0]);
    char zero[4] = "abc";
    CHECK(!apps_json_validate(LIT("["), zero, 0) && strcmp(zero, "abc") == 0, "err_len 0 wrote: '%s'", zero);
    CHECK(!apps_json_validate(LIT("["), NULL, 0), "NULL err");
    CHECK(apps_json_validate(LIT("[]"), NULL, 0), "NULL err on success");
    CHECK(!apps_json_validate(NULL, 0, err, sizeof err) && strcmp(err, "unexpected end of input at byte 1") == 0,
          "NULL text: '%s'", err);
    /* On success err is "". */
    strcpy(err, "stale");
    CHECK(apps_json_validate(LIT("[]"), err, sizeof err) && err[0] == '\0', "err after success: '%s'", err);
}

/* ---- c_contracts (ruling R6.11) ---- */

static void c_contracts(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_ok(vm,
           "-- push: one value on success, none on failure, bare messages\n"
           "local ok, v = c_push('{\"a\":[1,2,{\"b\":null}]}')\n"
           "eq(ok, true); eq(v.a[3].b, json.null); eq(#v.a, 3)\n"
           "local ok2, e2 = c_push('[[1,2],{\"a\":[3,')\n"
           "eq(ok2, false); eq(e2, 'unexpected end of input at byte 16')\n"
           "local ok3, e3 = c_push('[1,2,]'); eq(ok3, false); eq(e3, \"unexpected ']' at byte 6\")\n"
           "local ok4, e4 = c_push(string.rep('[', 17)); eq(e4, 'nested deeper than 16')\n"
           "local ok5, e5 = c_push(string.rep(' ', 32769)); eq(e5, 'input larger than 32 KB')\n"
           "eq(c_push('  null '), true); eq(select(2, c_push('7')), 7)\n"
           "-- validate\n"
           "eq(c_validate('[1,2,3]'), true)\n"
           "local f, ve = c_validate('{\"a\":}'); eq(f, false); eq(ve, \"unexpected '}' at byte 6\")\n"
           "-- encode_value: the same text by any index, freed by the caller; failures are bare\n"
           "eq(c_encode({1, 2, {a = true}}), '[1,2,{\"a\":true}]')\n"
           "eq(c_encode('x'), '\"x\"'); eq(c_encode(nil), 'null'); eq(c_encode(2.5), '2.5'); eq(c_encode(7), '7')\n"
           "local t, te = c_encode(function() end); eq(t, nil); eq(te, \"can't encode a function\")\n"
           "local t2, te2 = c_encode({1, {2, '\\xff'}}); eq(t2, nil); eq(te2, \"can't encode a string that isn't valid UTF-8\")\n"
           "local d = {} d[1] = d\n"
           "local t3, te3 = c_encode(d); eq(t3, nil); eq(te3, 'nested deeper than 16')\n"
           "local t4, te4 = c_encode(0 / 0); eq(te4, \"can't encode NaN or infinity\")\n"
           "local t5, te5 = c_encode({[true] = 1}); eq(te5, \"can't encode a boolean key\")\n"
           "-- a realloc_fn without memory: out of memory, no raise\n"
           "eq(c_encode_nomem({1, 2}), 'out of memory'); eq(c_encode_nomem(1), 'out of memory')\n");
    apps_vm_free(vm);
}

/* ---- stack_discipline (ruling R6.9): the C entry points reserve what they push ---- */

/* Lua gives a C function LUA_MINSTACK (20) free slots, and 16 levels of table and key want about
 * twice that, so the decoder and the encoder reserve room as they go (luaL_checkstack). A push past
 * what was reserved goes unseen by a plain Lua build, which keeps 5 slots spare; the Python test
 * builds Lua with LUA_USE_APICHECK, where it aborts. Every call below is the whole of one protected
 * call, so it starts with exactly the 20. */
static const char *deep_what;
static const char *deep_text;
static size_t deep_len;
static int deep_fill;  /* values the caller has pushed already: the entry point gets what is left of the 20 */
static int deep_key;   /* its address is the registry key of the value the encoder gets */

static int t_deep_decode(lua_State *L) {
    char err[96] = "";
    for (int i = 0; i < deep_fill; i++) {
        lua_pushnil(L);
    }
    int top = lua_gettop(L);
    bool ok = apps_json_push(L, deep_text, deep_len, err, sizeof err);
    CHECK(ok && lua_gettop(L) == top + 1, "%s, %d slots taken: decode: '%s'", deep_what, deep_fill, err);
    if (!ok) {
        lua_pushnil(L);
    }
    lua_rawsetp(L, LUA_REGISTRYINDEX, &deep_key);
    return 0;
}

static int t_deep_encode(lua_State *L) {
    char err[96] = "";
    size_t len = 0;
    for (int i = 0; i < deep_fill; i++) {
        lua_pushnil(L);
    }
    lua_rawgetp(L, LUA_REGISTRYINDEX, &deep_key);
    char *out = apps_json_encode_value(L, -1, count_realloc, &len, err, sizeof err);
    CHECK(out != NULL && len == deep_len && memcmp(out, deep_text, len) == 0, "%s, %d slots taken: encoded %zu of %zu bytes: '%s'",
          deep_what, deep_fill, len, deep_len, err);
    count_free(out);
    return 0;
}

/* The text decodes, and what it decodes to encodes back to it, in two protected calls each, so each
 * starts with its 20: once with the caller having taken none of them, once with it having left one
 * slot (the encoder is handed a value, which takes the last). */
static void deep_round_trip(apps_vm_t *vm, const char *what, const char *text, size_t len) {
    deep_what = what;
    deep_text = text;
    deep_len = len;
    for (deep_fill = 0; deep_fill < LUA_MINSTACK; deep_fill += LUA_MINSTACK - 1) {
        run_c(vm, t_deep_decode);
        run_c(vm, t_deep_encode);
    }
}

/* Whether a luaL_Buffer has made its box yet: the first one also makes the box's metatable. */
static bool box_made;

static int t_box_metatable(lua_State *L) {
    box_made = luaL_getmetatable(L, "_UBOX*") == LUA_TTABLE;
    return 0;
}

enum { BOXED_IN_ARRAY, BOXED_VALUE, BOXED_KEY };
#define BOXED_STRING_LEN 1100

/* depth containers deep, and in the innermost a string of BOXED_STRING_LEN bytes and an escape: past
 * the 1024 bytes (host) or 512 (device) a luaL_Buffer holds itself, so decoding it makes the
 * buffer's box. It is the only element of arrays, the value of the last key (the table and the key
 * above it are on the stack), or that key itself. Text the encoder gives back exactly. */
static size_t boxed_text(char *out, int depth, int kind) {
    size_t n = 0;
    for (int i = 0; i < depth - (kind == BOXED_KEY ? 1 : 0); i++) {
        if (kind == BOXED_IN_ARRAY) {
            out[n++] = '[';
        } else {
            memcpy(out + n, "{\"a\":", 5);
            n += 5;
        }
    }
    if (kind == BOXED_KEY) {
        out[n++] = '{';
    }
    out[n++] = '"';
    memset(out + n, 'x', BOXED_STRING_LEN);
    n += BOXED_STRING_LEN;
    memcpy(out + n, "\\n\"", 3);
    n += 3;
    if (kind == BOXED_KEY) {
        memcpy(out + n, ":1", 2);
        n += 2;
    }
    for (int i = 0; i < depth; i++) {
        out[n++] = kind == BOXED_IN_ARRAY ? ']' : '}';
    }
    out[n] = '\0';
    return n;
}

static void stack_discipline(void) {
    static char text[16 * (APPS_JSON_MAX_DEPTH + 2) + BOXED_STRING_LEN + 64];
    static const struct {
        const char *what;
        int kind, depth;
    } boxed[] = {
        /* The value first: the box, its metatable (new, in this VM) and what goes into it, over a table
         * and a key at the deepest level, is the tightest fit there is. */
        {"16 objects, a boxed value", BOXED_VALUE, APPS_JSON_MAX_DEPTH},
        {"16 arrays, a boxed element", BOXED_IN_ARRAY, APPS_JSON_MAX_DEPTH},
        {"16 objects, a boxed key", BOXED_KEY, APPS_JSON_MAX_DEPTH},
        {"a boxed string", BOXED_VALUE, 0},
    };
    apps_vm_t *vm = new_json_vm(4 * MB);
    run_c(vm, t_box_metatable);
    CHECK(!box_made, "a luaL_Buffer made its box before the case: the first one is the tightest fit");
    for (size_t i = 0; i < sizeof boxed / sizeof boxed[0]; i++) {
        size_t n = boxed_text(text, boxed[i].depth, boxed[i].kind);
        deep_round_trip(vm, boxed[i].what, text, n);
        if (i == 0) {
            run_c(vm, t_box_metatable);
            CHECK(box_made, "no luaL_Buffer made a box: the string is too short to need one");
        }
    }
    static const char *const shapes[] = {"16 arrays", "16 objects", "16 arrays and objects"};
    for (int shape = 0; shape < 3; shape++) {
        size_t n = nested_text(text, APPS_JSON_MAX_DEPTH, shape);
        deep_round_trip(vm, shapes[shape], text, n);
    }
    apps_vm_free(vm);
}

/* The Python test that builds Lua with LUA_USE_APICHECK runs this to see that the checks are on:
 * it pushes past the 20 slots a C function has, as the decoder would without luaL_checkstack, and
 * there the process aborts. A plain build goes on (5 slots are spare), so no other test runs it. */
static int t_overrun(lua_State *L) {
    for (int i = 0; i < LUA_MINSTACK + 2; i++) {
        lua_pushnil(L);
    }
    return 0;
}

static void overrun(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    run_c(vm, t_overrun);
    apps_vm_free(vm);
}

/* ---- module (rulings R6.12, X8, X9) ---- */

static int probe_calls;

static int count_probe(lua_State *L) {
    (void)L;
    probe_calls++;
    return 0;
}

static void module(void) {
    apps_vm_t *vm = new_json_vm(1 * MB);
    lua_State *L = apps_vm_lua(vm);
    /* The global json, installed with lua_setglobal into the sandbox: the registry's globals table. */
    lua_pushglobaltable(L);
    lua_pushliteral(L, "json");
    CHECK(lua_rawget(L, -2) == LUA_TTABLE, "json isn't a global table");
    lua_settop(L, 0);
    run_ok(vm,
           "eq(type(json), 'table'); eq(type(json.encode), 'function'); eq(type(json.decode), 'function')\n"
           "eq(type(json.null), 'userdata')\n"
           "local n = 0 for _ in pairs(json) do n = n + 1 end eq(n, 3, 'keys of json')\n"
           "eq(json.null == json.null, true); eq(json.null ~= nil, true); eq(json.null ~= lightud(), true)\n"
           "eq(rawequal(json.null, json.decode('null')), true)\n"
           "-- argument errors name the function\n"
           "local function msg(f) local ok, e = pcall(f) return e end\n"
           "eq(msg(function() json.decode() end):find(\"bad argument #1 to 'decode' (string expected, got no value)\", 1, true) ~= nil, true)\n"
           "eq(msg(function() json.decode({}) end):find(\"bad argument #1 to 'decode' (string expected, got table)\", 1, true) ~= nil, true)\n"
           "eq(msg(function() json.encode() end):find(\"bad argument #1 to 'encode' (value expected)\", 1, true) ~= nil, true)\n"
           "-- a number is a string to decode, as everywhere in Lua\n"
           "eq(json.decode(12), 12)\n");

    /* Opening twice is harmless, and the second json is the one in the sandbox. */
    int top = lua_gettop(L);
    apps_json_open(L);
    CHECK(lua_gettop(L) == top, "a second apps_json_open changed the stack");
    run_ok(vm, "eq(json.encode({1}), '[1]'); eq(json.decode('[1]')[1], 1)\n");
    apps_vm_free(vm);

    /* Every binding calls apps_vm_check first (ruling R6.12): after a kill, neither does any work. Each runs
     * in a VM of its own, killed first, from a chunk run outside apps_vm_call: a kill re-arms the thread's
     * hook to fire at every instruction, so only the first chunk after it could show the check. Here the
     * chunk is three instructions, and only the check can stop it. */
    static const char *const chunks[] = {
        "local v = json.decode('[1]') probe() return v",
        "local v = json.encode({1}) probe() return v",
    };
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        vm = new_json_vm(1 * MB);
        L = apps_vm_lua(vm);
        probe_calls = 0;
        lua_pushcfunction(L, count_probe);
        lua_setglobal(L, "probe");
        apps_vm_kill(vm, "stopped by test");
        char err[256] = "";
        CHECK(apps_vm_load(vm, "main.lua", chunks[i], strlen(chunks[i]), err, sizeof err), "%s", err);
        int rc = lua_pcall(L, 0, 1, 0);
        CHECK(rc == LUA_ERRRUN && lua_type(L, -1) == LUA_TLIGHTUSERDATA && probe_calls == 0,
              "chunk %zu: status %d, error is %s, %d probe calls", i, rc, luaL_typename(L, -1), probe_calls);
        lua_settop(L, 0);
        apps_vm_free(vm);
    }

    /* apps_json_open is safe outside a protected call, even where the heap refuses: no abort, no json,
     * nothing left on the stack, and the VM is out of memory, which the next apps_vm_call reports. */
    vm = new_vm(1 * MB);
    L = apps_vm_lua(vm);
    grants_left = 0;
    apps_json_open(L);
    grants_left = -1;
    char err[256] = "";
    CHECK(lua_gettop(L) == 0 && raw_global(L, "json") == LUA_TNIL, "%d values, json is %s", lua_gettop(L), luaL_typename(L, -1));
    lua_settop(L, 0);
    int rc = call(vm, "return 1", 1, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_NOMEM, "after a refused apps_json_open: %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
    apps_vm_free(vm);
}

/* ---- memory_caps ---- */

/* 400 strings of 70 bytes encoded through the C entry point. Run by apps_vm_call, so it may raise. */
static int t_encode_big(lua_State *L) {
    lua_createtable(L, 0, 0);
    int t = lua_gettop(L);
    for (int i = 1; i <= 400; i++) {
        lua_pushliteral(L, "0123456789012345678901234567890123456789012345678901234567890123456789");
        lua_rawseti(L, t, i);
    }
    size_t len;
    char err[96];
    count_free(apps_json_encode_value(L, t, count_realloc, &len, err, sizeof err));
    return 0;
}

static void memory_caps(void) {
    /* A decode that needs far more than the app may hold ends NOMEM, like any allocation in the VM.
     * 15000 numbers want 240 KB of array, and 10000 empty tables 560 KB, from 30 KB of text. */
    memory_case(128 * KB, "text = '[' .. string.rep('0,', 14999) .. '0]'", "return json.decode(text)\n");
    memory_case(256 * KB, "text = '[' .. string.rep('[],', 10000) .. '[]]'", "return json.decode(text)\n");
    /* A long string with an escape: the decoded text and the buffer holding it come on top of the input. */
    memory_case(100 * KB, "text = '\"' .. string.rep('a', 30000) .. '\\\\n\"'", "return json.decode(text)\n");
    /* An encode whose text outgrows the cap: 70 KB of text, with the output's copies, in 64 KB. */
    memory_case(64 * KB, "local s = string.rep('x', 1000) t = {} for i = 1, 70 do t[i] = s end", "return json.encode(t)\n");
    /* The C entry point raises the same way, inside apps_vm_call. */
    apps_vm_t *vm = new_json_vm(36 * KB);
    lua_State *L = apps_vm_lua(vm);
    char err[256] = "";
    lua_pushcfunction(L, t_encode_big);
    int rc = (int)apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_NOMEM, "apps_json_encode_value in 36 KB: %s '%s'", call_name(rc), err);
    lua_settop(L, 0);
    apps_vm_free(vm);
    vm = new_json_vm(1 * MB);
    lua_State *M = apps_vm_lua(vm);
    lua_pushcfunction(M, t_encode_big);
    rc = (int)apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK, "the same in 1 MB: %s '%s'", call_name(rc), err);
    lua_settop(M, 0);
    apps_vm_free(vm);
}

/* ---- allocation_sweep: every allocation refused in turn ---- */

/* The setup chunk makes globals; the work chunk then runs with every growth request from the n-th
 * on refused, for n = 0, 1, 2... until the call passes. Every outcome is OK or NOMEM, every VM
 * frees to 0 bytes, and the call that passes gives the right answer (the check chunk). */
static void sweep(const char *name, const char *setup, const char *work, const char *check) {
    int first_ok = -1;
    for (int n = 0; n < 600 && first_ok < 0; n++) {
        apps_vm_t *vm = new_json_vm(4 * MB);
        lua_State *L = apps_vm_lua(vm);
        char err[512] = "";
        int rc = call(vm, setup, 0, APPS_BUDGET_CALL, err, sizeof err);
        CHECK(rc == APPS_CALL_OK, "%s setup: %s '%s'", name, call_name(rc), err);
        lua_settop(L, 0);
        CHECK(apps_vm_load(vm, "main.lua", work, strlen(work), err, sizeof err), "%s: %s", name, err);
        grants_left = n;
        rc = (int)apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
        grants_left = -1;
        CHECK(rc == APPS_CALL_OK || rc == APPS_CALL_NOMEM, "%s, refusing from allocation %d: %s '%s'", name, n, call_name(rc),
              err);
        if (rc == APPS_CALL_OK) {
            first_ok = n;
            lua_settop(L, 0);
            run_ok(vm, check);
        }
        lua_settop(L, 0);
        apps_vm_free(vm);
        CHECK(held == 0, "%s, refusing from allocation %d: %zu bytes held after the VM was freed", name, n, held);
    }
    CHECK(first_ok > 0, "%s: never passed, or passed with nothing refused (first pass at %d)", name, first_ok);
}

static void allocation_sweep(void) {
    /* Decoding: tables, interned strings, and a string with an escape long enough for a luaL_Buffer box. */
    sweep("decode",
          "local long = string.rep('a', 1500)\n"
          "text = '{\"list\":[1,2.5,\"x\",null,[true,false]],\"s\":\"' .. long .. '\\\\n' .. long .. '\",\"o\":{\"k\":\"v\"}}'\n",
          "result = json.decode(text)\n",
          "eq(#result.list, 5); eq(#result.s, 3001); eq(result.o.k, 'v'); eq(result.list[4], json.null)\n");
    /* Encoding: the output grows through several doublings with lua_next in flight. */
    sweep("encode",
          "local o = {} for i = 1, 40 do o['key' .. i] = string.rep('v', 20) .. i end\n"
          "value = {o, o, {1, 2.5, 'x', {a = {b = 1}}}, string.rep('s', 700)}\n",
          "result = json.encode(value)\n",
          "eq(same(json.decode(result), value), true, 'round trip')\n");
}

/* ==== The pull reader ==== */

static void jr_init_text(apps_jr_t *r, const char *s) {
    apps_jr_init(r, s, strlen(s));
}

/* ---- reader_manifest (the brief's test_pull_reader_reads_manifest_shape) ---- */

/* Reads the manifest shape from json: the id and api, the file names, x skipped, messages raw. */
static void read_manifest(const char *json, const char *want_messages, const char *label) {
    apps_jr_t r;
    jr_init_text(&r, json);
    char key[32], str[32], files[4][8] = {"", "", "", ""};
    int64_t api = -1;
    int nfiles = 0, rc;
    bool saw_id = false, saw_x = false, saw_messages = false;
    size_t raw_len = 0;
    const char *raw = NULL;
    CHECK(apps_jr_object(&r), "%s: object: %s", label, r.err);
    CHECK(r.depth == 1, "%s: depth %d after the opening brace", label, r.depth);
    while ((rc = apps_jr_key(&r, key, sizeof key)) == 1) {
        if (strcmp(key, "id") == 0) {
            CHECK(apps_jr_string(&r, str, sizeof str) && strcmp(str, "timer") == 0, "%s: id '%s' %s", label, str, r.err);
            saw_id = true;
        } else if (strcmp(key, "api") == 0) {
            CHECK(apps_jr_integer(&r, &api) && api == 1, "%s: api %lld %s", label, (long long)api, r.err);
        } else if (strcmp(key, "files") == 0) {
            int more;
            CHECK(apps_jr_array(&r), "%s: files: %s", label, r.err);
            while ((more = apps_jr_next(&r)) == 1) {
                char file[8];
                CHECK(apps_jr_string(&r, file, sizeof file), "%s: file: %s", label, r.err);
                if (nfiles < 4) {
                    strcpy(files[nfiles], file);
                }
                nfiles++;
            }
            CHECK(more == 0, "%s: the files array ended with %d: %s", label, more, r.err);
        } else if (strcmp(key, "x") == 0) {
            CHECK(apps_jr_skip(&r), "%s: skipping x: %s", label, r.err);
            saw_x = true;
        } else if (strcmp(key, "messages") == 0) {
            raw = apps_jr_raw_value(&r, &raw_len);
            saw_messages = true;
        } else {
            CHECK(false, "%s: unexpected key '%s'", label, key);
        }
    }
    CHECK(rc == 0, "%s: the keys ended with %d: %s", label, rc, r.err);
    CHECK(saw_id && saw_x && saw_messages && nfiles == 2 && strcmp(files[0], "a.lua") == 0 && strcmp(files[1], "b.bin") == 0,
          "%s: id %d, x %d, messages %d, %d files '%s' '%s'", label, saw_id, saw_x, saw_messages, nfiles, files[0], files[1]);
    CHECK(raw != NULL && raw_len == strlen(want_messages) && memcmp(raw, want_messages, raw_len) == 0, "%s: messages are '%.*s'",
          label, raw != NULL ? (int)raw_len : 4, raw != NULL ? raw : "NULL");
    CHECK(apps_jr_end(&r) && r.err[0] == '\0' && r.depth == 0, "%s: end: %s, depth %d", label, r.err, r.depth);
}

static void reader_manifest(void) {
    read_manifest("{\"id\":\"timer\",\"api\":1,\"files\":[\"a.lua\",\"b.bin\"],\"x\":{\"y\":[1,{\"z\":null}]},"
                  "\"messages\":{\"start\":\"go\"}}",
                  "{\"start\":\"go\"}", "compact");
    /* The same document with whitespace everywhere the grammar allows it. */
    read_manifest(" {\n  \"id\" : \"timer\" ,\n  \"api\" : 1 ,\n  \"files\" : [ \"a.lua\" , \"b.bin\" ] ,\n"
                  "  \"x\" : { \"y\" : [ 1 , { \"z\" : null } ] } ,\n  \"messages\" : { \"start\" : \"go\" }\n}\n ",
                  "{ \"start\" : \"go\" }", "pretty");
    /* Keys in another order, and escapes in the strings the reader decodes. */
    apps_jr_t r;
    jr_init_text(&r, "{\"messages\":{},\"files\":[],\"id\":\"t\\u00e9\\n\",\"api\":-3}");
    char key[16], str[16];
    int64_t n = 0;
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && strcmp(key, "messages") == 0, "%s", r.err);
    size_t len;
    const char *raw = apps_jr_raw_value(&r, &len);
    CHECK(raw != NULL && len == 2 && memcmp(raw, "{}", 2) == 0, "empty object raw");
    CHECK(apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_array(&r) && apps_jr_next(&r) == 0, "empty array: %s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_string(&r, str, sizeof str) && strcmp(str, "t\303\251\n") == 0,
          "escapes: '%s' %s", str, r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_integer(&r, &n) && n == -3, "api %lld: %s", (long long)n, r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == 0 && apps_jr_end(&r), "end: %s", r.err);

    /* apps_json_compact turns the raw text of a pretty messages object into the compact one. */
    static const char pretty[] = "{ \"start\" : \"go\" }";
    char out[32];
    CHECK(apps_json_compact(pretty, sizeof pretty - 1, out, sizeof out) == 14 && strcmp(out, "{\"start\":\"go\"}") == 0, "got '%s'", out);
}

/* ---- reader_strict (ruling R6.13, and the coordinator's clarification) ---- */

/* The reader's string at the start of json fills out, or fails with want. */
static void expect_read_string(const char *json, const char *want_text, size_t cap, const char *want_err, int line) {
    apps_jr_t r;
    jr_init_text(&r, json);
    char out[64];
    memset(out, 'x', sizeof out);
    out[sizeof out - 1] = '\0';
    bool ok = apps_jr_string(&r, out, cap);
    if (want_err == NULL) {
        CHECK_AT(line, ok && strcmp(out, want_text) == 0 && r.err[0] == '\0', "%s: '%s' %s", json, out, r.err);
    } else {
        /* Never cut: a failure is an error, and out holds no half of the string. */
        CHECK_AT(line, !ok && strcmp(r.err, want_err) == 0 && (cap == 0 || out[0] == '\0'),
                 "%s: ok %d, err '%s', out '%s'; want '%s'", json, ok, r.err, out, want_err);
    }
}
#define expect_string(json, want, cap) expect_read_string(json, want, cap, NULL, __LINE__)
#define expect_string_error(json, cap, err) expect_read_string(json, NULL, cap, err, __LINE__)

static void reader_strings(void) {
    expect_string("\"abc\"", "abc", 64);
    expect_string("  \"a\\u00e9b\"", "a\303\251b", 64);
    expect_string("\"\\ud83d\\ude00\"", "\360\237\230\200", 64);
    expect_string("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\/\b\f\n\r\t", 64);
    expect_string("\"\"", "", 1);
    /* cap counts the NUL, and a string that doesn't fit fails: it is never cut. */
    expect_string("\"abc\"", "abc", 4);
    expect_string_error("\"abcd\"", 4, "string longer than 3 bytes at byte 1");
    expect_string_error("  [\"abcd\"]", 4, "unexpected '[' at byte 3");
    expect_string_error("\"\\u00e9\\u00e9\"", 4, "string longer than 3 bytes at byte 1");
    expect_string_error("\"x\"", 0, "no room for a string at byte 1");
    /* The same checks as the decoder's. */
    expect_string_error("\"\\u0000\"", 64, "\\u0000 isn't allowed in this string at byte 2");
    expect_string_error("\"a\\u0000\"", 64, "\\u0000 isn't allowed in this string at byte 3");
    expect_string_error("\"a\001b\"", 64, "unexpected byte 0x01 at byte 3");
    expect_string_error("\"a\nb\"", 64, "unexpected byte 0x0A at byte 3");
    expect_string_error("\"\303\"", 64, "invalid UTF-8 at byte 2");
    expect_string_error("\"\355\240\200\"", 64, "invalid UTF-8 at byte 2");
    expect_string_error("\"\\ud800\"", 64, "invalid \\u escape at byte 2");
    expect_string_error("\"\\x\"", 64, "unexpected 'x' at byte 3");
    expect_string_error("\"abc", 64, "unexpected end of input at byte 5");
    expect_string_error("", 64, "unexpected end of input at byte 1");
    expect_string_error("5", 64, "unexpected '5' at byte 1");
    expect_string_error("null", 64, "unexpected 'n' at byte 1");

    /* A key is a string, read the same way, and the ':' is part of it. */
    apps_jr_t r;
    char key[8];
    jr_init_text(&r, "{\"a\\u00e9\":1,\"toolongkey\":2}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && strcmp(key, "a\303\251") == 0, "key: '%s' %s", key, r.err);
    int64_t one;
    CHECK(apps_jr_integer(&r, &one) && one == 1, "value: %s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "string longer than 7 bytes at byte 14") == 0, "long key: %s", r.err);
    jr_init_text(&r, "{\"a\001\":1}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected byte 0x01 at byte 4") == 0,
          "control character in a key: %s", r.err);
    jr_init_text(&r, "{\"a\" 1}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected '1' at byte 6") == 0,
          "missing colon: %s", r.err);
    jr_init_text(&r, "{\"\\u0000\":1}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == -1, "a key with \\u0000");
}

static void expect_integer(const char *json, int64_t want, const char *want_err, int line) {
    apps_jr_t r;
    jr_init_text(&r, json);
    int64_t got = 12345;
    bool ok = apps_jr_integer(&r, &got);
    if (want_err == NULL) {
        CHECK_AT(line, ok && got == want && apps_jr_end(&r), "%s: ok %d, got %lld, %s", json, ok, (long long)got, r.err);
    } else {
        CHECK_AT(line, !ok && strcmp(r.err, want_err) == 0 && got == 12345, "%s: ok %d, got %lld, err '%s'; want '%s'", json, ok,
                 (long long)got, r.err, want_err);
    }
}

static void reader_integers(void) {
    expect_integer("0", 0, NULL, __LINE__);
    expect_integer("-0", 0, NULL, __LINE__);
    expect_integer(" 7 ", 7, NULL, __LINE__);
    expect_integer("-7", -7, NULL, __LINE__);
    expect_integer("9223372036854775807", INT64_MAX, NULL, __LINE__);
    expect_integer("-9223372036854775808", INT64_MIN, NULL, __LINE__);
    /* No fraction, no exponent, nothing outside int64. */
    expect_integer("9223372036854775808", 0, "integer out of range at byte 1", __LINE__);
    expect_integer("-9223372036854775809", 0, "integer out of range at byte 1", __LINE__);
    expect_integer("99999999999999999999", 0, "integer out of range at byte 1", __LINE__);
    expect_integer("1.0", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("1.5", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("1e2", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("1E2", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("-1e-2", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("\"1\"", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("true", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("[1]", 0, "expected an integer at byte 1", __LINE__);
    expect_integer("", 0, "unexpected end of input at byte 1", __LINE__);
    expect_integer("-", 0, "unexpected end of input at byte 2", __LINE__);
    expect_integer("-x", 0, "unexpected 'x' at byte 2", __LINE__);
    /* 01 is the integer 0, then a stray 1 for whatever reads on. */
    apps_jr_t r;
    int64_t v;
    jr_init_text(&r, "01");
    CHECK(apps_jr_integer(&r, &v) && v == 0 && !apps_jr_end(&r) && strcmp(r.err, "unexpected '1' at byte 2") == 0, "01: %s", r.err);
}

static void reader_commas_and_structure(void) {
    apps_jr_t r;
    char key[8];
    int64_t v;
    /* An object: a leading, a doubled, a trailing comma, and a missing one. */
    jr_init_text(&r, "{,\"a\":1}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected ',' at byte 2") == 0, "%s", r.err);
    jr_init_text(&r, "{\"a\":1,,\"b\":2}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r), "%s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected ',' at byte 8") == 0, "%s", r.err);
    jr_init_text(&r, "{\"a\":1,}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r), "%s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected '}' at byte 8") == 0, "%s", r.err);
    jr_init_text(&r, "{\"a\":1 \"b\":2}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r), "%s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected '\"' at byte 8") == 0, "%s", r.err);
    jr_init_text(&r, "{\"a\":1");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r), "%s", r.err);
    CHECK(apps_jr_key(&r, key, sizeof key) == -1 && strcmp(r.err, "unexpected end of input at byte 7") == 0, "%s", r.err);
    jr_init_text(&r, "{}");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 0 && r.depth == 0 && apps_jr_end(&r), "empty object: %s", r.err);
    jr_init_text(&r, "{ }");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 0 && apps_jr_end(&r), "empty object with space: %s", r.err);
    /* An array, the same. */
    jr_init_text(&r, "[,1]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && !apps_jr_skip(&r) && strcmp(r.err, "unexpected ',' at byte 2") == 0, "%s", r.err);
    jr_init_text(&r, "[1,,2]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) && apps_jr_next(&r) == 1, "%s", r.err);
    CHECK(!apps_jr_skip(&r) && strcmp(r.err, "unexpected ',' at byte 4") == 0, "%s", r.err);
    jr_init_text(&r, "[1,]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) && apps_jr_next(&r) == 1, "%s", r.err);
    CHECK(!apps_jr_skip(&r) && strcmp(r.err, "unexpected ']' at byte 4") == 0, "%s", r.err);
    jr_init_text(&r, "[1 2]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) && apps_jr_next(&r) == -1 &&
              strcmp(r.err, "unexpected '2' at byte 4") == 0,
          "%s", r.err);
    jr_init_text(&r, "[]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 0 && r.depth == 0 && apps_jr_end(&r), "empty array: %s", r.err);
    jr_init_text(&r, "[ ]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 0 && apps_jr_end(&r), "empty array with space: %s", r.err);
    jr_init_text(&r, "[1,2");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) &&
              apps_jr_next(&r) == -1 && strcmp(r.err, "unexpected end of input at byte 5") == 0,
          "%s", r.err);
    /* The wrong opening bracket. */
    jr_init_text(&r, "[1]");
    CHECK(!apps_jr_object(&r) && strcmp(r.err, "unexpected '[' at byte 1") == 0, "%s", r.err);
    jr_init_text(&r, "{}");
    CHECK(!apps_jr_array(&r) && strcmp(r.err, "unexpected '{' at byte 1") == 0, "%s", r.err);

    /* apps_jr_end: only whitespace is left, and no container is open. */
    jr_init_text(&r, "{\"a\":1} x");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r) && apps_jr_key(&r, key, sizeof key) == 0, "%s", r.err);
    CHECK(!apps_jr_end(&r) && strcmp(r.err, "unexpected 'x' at byte 9") == 0, "%s", r.err);
    jr_init_text(&r, "{\"a\":1}\n \t\r");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r) && apps_jr_key(&r, key, sizeof key) == 0 &&
              apps_jr_end(&r),
          "%s", r.err);
    jr_init_text(&r, "{\"a\":1");
    CHECK(apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r), "%s", r.err);
    CHECK(!apps_jr_end(&r) && strcmp(r.err, "unexpected end of input at byte 7") == 0, "end inside an object: %s", r.err);

    /* An error is final: the first message stays, and every later call fails at once. */
    jr_init_text(&r, "[1,,2]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_integer(&r, &v) && apps_jr_next(&r) == 1 && !apps_jr_skip(&r), "%s", r.err);
    char first[96];
    strcpy(first, r.err);
    CHECK(!apps_jr_skip(&r) && !apps_jr_integer(&r, &v) && apps_jr_next(&r) == -1 && apps_jr_key(&r, key, sizeof key) == -1 &&
              !apps_jr_end(&r) && !apps_jr_array(&r) && !apps_jr_object(&r) && apps_jr_raw_value(&r, NULL) == NULL &&
              strcmp(r.err, first) == 0,
          "after an error: '%s' became '%s'", first, r.err);
}

static void reader_skip_and_depth(void) {
    /* The reader's skip is the decoder's validation, for everything the decode_errors and valid tables
     * hold (expect_refused and t_decode_valid run them). Here: a skipped value is checked as it passes. */
    static const text_t bad_inside[] = {
        TEXT("{\"x\":[01],\"k\":1}"), TEXT("{\"x\":[1.],\"k\":1}"), TEXT("{\"x\":\"\\u12\",\"k\":1}"),
        TEXT("{\"x\":\"a\001\",\"k\":1}"), TEXT("{\"x\":\"\303\",\"k\":1}"), TEXT("{\"x\":[1,],\"k\":1}"),
        TEXT("{\"x\":{\"a\"},\"k\":1}"), TEXT("{\"x\":[true,nul],\"k\":1}"), TEXT("{\"x\":\"\\ud800\",\"k\":1}"),
    };
    for (size_t i = 0; i < sizeof bad_inside / sizeof bad_inside[0]; i++) {
        apps_jr_t r;
        char key[8];
        apps_jr_init(&r, bad_inside[i].json, bad_inside[i].len);
        bool reached = apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && !apps_jr_skip(&r);
        CHECK(reached && r.err[0] != '\0', "bad value %zu was skipped: '%s'", i, r.err);
        /* raw_value checks as well. */
        apps_jr_init(&r, bad_inside[i].json, bad_inside[i].len);
        reached = apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_raw_value(&r, NULL) == NULL;
        CHECK(reached && r.err[0] != '\0', "bad value %zu had a raw text: '%s'", i, r.err);
    }

    /* Depth counts the containers the caller opened: an object open, the skipped value may nest
     * 15 more, and 16 is one too many. */
    for (int inner = 15; inner <= 16; inner++) {
        char text[64], key[8];
        size_t n = 0;
        memcpy(text, "{\"x\":", 5);
        n = 5;
        memset(text + n, '[', (size_t)inner);
        n += (size_t)inner;
        text[n++] = '1';
        memset(text + n, ']', (size_t)inner);
        n += (size_t)inner;
        text[n++] = '}';
        apps_jr_t r;
        apps_jr_init(&r, text, n);
        bool ok = apps_jr_object(&r) && apps_jr_key(&r, key, sizeof key) == 1 && apps_jr_skip(&r);
        if (inner == 15) {
            CHECK(ok && apps_jr_key(&r, key, sizeof key) == 0 && apps_jr_end(&r), "skipping 15 levels in an object: %s", r.err);
        } else {
            CHECK(!ok && strcmp(r.err, "nested deeper than 16") == 0, "skipping 16 levels in an object: ok %d '%s'", ok, r.err);
        }
    }
    /* The same through the pull calls themselves: 16 levels open, a 17th fails. */
    apps_jr_t r;
    char text[64];
    memset(text, '[', 20);
    apps_jr_init(&r, text, 20);
    int opened = 0;
    while (opened < 20 && apps_jr_array(&r)) {
        opened++;
        if (apps_jr_next(&r) != 1) {
            break;
        }
    }
    CHECK(opened == 16 && strcmp(r.err, "nested deeper than 16") == 0 && r.depth == 16, "opened %d levels: '%s'", opened, r.err);
    /* A closed container gives its level back. */
    jr_init_text(&r, "[[],[],[[]]]");
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_array(&r) && r.depth == 2 && apps_jr_next(&r) == 0 && r.depth == 1 &&
              apps_jr_next(&r) == 1 && apps_jr_skip(&r) && apps_jr_next(&r) == 1 && apps_jr_skip(&r) && apps_jr_next(&r) == 0 &&
              r.depth == 0 && apps_jr_end(&r),
          "levels given back: %s, depth %d", r.err, r.depth);

    /* The input limit applies from the first call. */
    char *big = malloc(APPS_JSON_MAX_INPUT + 2);
    memset(big, ' ', APPS_JSON_MAX_INPUT + 1);
    big[0] = '1';
    apps_jr_init(&r, big, APPS_JSON_MAX_INPUT);
    CHECK(apps_jr_skip(&r) && apps_jr_end(&r), "32768 bytes: %s", r.err);
    apps_jr_init(&r, big, APPS_JSON_MAX_INPUT + 1);
    CHECK(r.err[0] != '\0' && !apps_jr_skip(&r) && strcmp(r.err, "input larger than 32 KB") == 0, "32769 bytes: '%s'", r.err);
    free(big);
    apps_jr_init(&r, NULL, 0);
    CHECK(!apps_jr_skip(&r) && strcmp(r.err, "unexpected end of input at byte 1") == 0, "NULL text: '%s'", r.err);

    /* raw_value: the text of each kind of value, escapes and nesting included, and nothing around it. */
    static const struct {
        const char *json, *raw;
    } raws[] = {
        {"  \"a\\\"b\"  ", "\"a\\\"b\""}, {"-12.5e+3 ", "-12.5e+3"}, {" true", "true"}, {"null ", "null"},
        {" [1, [2, {\"a\" : \"]\"}]] ", "[1, [2, {\"a\" : \"]\"}]]"}, {"{ }", "{ }"}, {"\"\\\\\"", "\"\\\\\""},
    };
    for (size_t i = 0; i < sizeof raws / sizeof raws[0]; i++) {
        size_t len = 0;
        jr_init_text(&r, raws[i].json);
        const char *raw = apps_jr_raw_value(&r, &len);
        CHECK(raw != NULL && len == strlen(raws[i].raw) && memcmp(raw, raws[i].raw, len) == 0 && apps_jr_end(&r),
              "raw %zu: '%.*s' (%s)", i, raw != NULL ? (int)len : 4, raw != NULL ? raw : "NULL", r.err);
    }
}

static void reader_strict(void) {
    reader_strings();
    reader_integers();
    reader_commas_and_structure();
    reader_skip_and_depth();
}

/* ---- compact (ruling R6.14) ---- */

static void expect_compact(const char *in, const char *want, int line) {
    size_t len = strlen(in), want_len = strlen(want);
    char out[256];
    memset(out, 'x', sizeof out);
    size_t n = apps_json_compact(in, len, out, sizeof out);
    CHECK_AT(line, n == want_len && strcmp(out, want) == 0, "'%s': got %zu '%s'; want %zu '%s'", in, n, out, want_len, want);
    /* Exactly room for the text and its NUL is enough; one byte less is not. */
    char exact[256];
    n = apps_json_compact(in, len, exact, want_len + 1);
    CHECK_AT(line, n == want_len && strcmp(exact, want) == 0, "'%s': in %zu bytes got %zu", in, want_len + 1, n);
    memset(exact, 'x', sizeof exact);
    n = apps_json_compact(in, len, exact, want_len);
    CHECK_AT(line, n == 0 && exact[0] == '\0', "'%s': in %zu bytes got %zu '%.4s'", in, want_len, n, exact);
    /* Compacting never grows the text, so len + 1 is always enough. */
    n = apps_json_compact(in, len, exact, len + 1);
    CHECK_AT(line, n == want_len, "'%s': in len + 1 bytes got %zu", in, n);
}

static void compact(void) {
    expect_compact("{\"start\":\"go\"}", "{\"start\":\"go\"}", __LINE__);
    expect_compact("{ \"start\" : \"go\" }", "{\"start\":\"go\"}", __LINE__);
    expect_compact("{\n  \"start\": \"go\",\n  \"stop\" : \"halt now\"\n}\n", "{\"start\":\"go\",\"stop\":\"halt now\"}", __LINE__);
    expect_compact("[ 1 ,\t2,\r\n3 ]", "[1,2,3]", __LINE__);
    /* Whitespace inside a string stays, and so does what looks like a string's end: \" and \\. */
    expect_compact("{\"a\" : \"x \\\" y\", \"b\" : \"\\\\\" , \"c\":\" \"}", "{\"a\":\"x \\\" y\",\"b\":\"\\\\\",\"c\":\" \"}", __LINE__);
    expect_compact("\"  a  b  \"", "\"  a  b  \"", __LINE__);
    expect_compact(" [ \"\\\\\" , \"\\\\\\\"\" ] ", "[\"\\\\\",\"\\\\\\\"\"]", __LINE__);
    expect_compact("[]", "[]", __LINE__);
    /* No room at all. */
    char out[4] = "abc";
    CHECK(apps_json_compact("[]", 2, out, 0) == 0 && strcmp(out, "abc") == 0, "cap 0 wrote '%s'", out);
    /* The text the manifest reader gave out compacts to what Task 7 stores. */
    char big[32];
    CHECK(apps_json_compact(LIT("{ \"start\" : \"go\" }"), big, sizeof big) == 14, "got '%s'", big);
}

/* ---- fuzz_agreement: mutated text, one decoder and a faithful encoder ---- */

/* Whether the values at a and b are the same, deeply: tables by raw contents, numbers with their
 * subtype, everything else raw-equal. */
static bool same_value(lua_State *L, int a, int b) {
    a = lua_absindex(L, a);
    b = lua_absindex(L, b);
    int type = lua_type(L, a);
    if (type != lua_type(L, b)) {
        return false;
    }
    if (type == LUA_TNUMBER) {
        return lua_isinteger(L, a) == lua_isinteger(L, b) && lua_rawequal(L, a, b);
    }
    if (type != LUA_TTABLE) {
        return lua_rawequal(L, a, b);
    }
    size_t in_a = 0, in_b = 0;
    bool same = true;
    luaL_checkstack(L, 8, "same_value");  /* a key, two values and their copies, at every level */
    lua_pushnil(L);
    while (same && lua_next(L, a) != 0) {  /* key at -2, value at -1 */
        in_a++;
        lua_pushvalue(L, -2);
        lua_rawget(L, b);
        same = lua_type(L, -1) != LUA_TNIL && same_value(L, lua_gettop(L) - 1, lua_gettop(L));
        lua_pop(L, 2);  /* b's value and a's */
    }
    if (!same) {
        lua_settop(L, lua_gettop(L) - 1);  /* the key */
        return false;
    }
    lua_pushnil(L);
    while (lua_next(L, b) != 0) {
        in_b++;
        lua_pop(L, 1);
    }
    return in_a == in_b;
}

static uint32_t fuzz_state = 0x2545F491u;

static uint32_t fuzz_next(void) {
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 17;
    fuzz_state ^= fuzz_state << 5;
    return fuzz_state;
}

/* The edits that make a valid text invalid, or valid and different: structural characters, digits,
 * escapes, and bytes that start or break a UTF-8 sequence. */
static const char fuzz_alphabet[] = "{}[]\",:\\ \t-+.eE0123456789tfnulabu/\001\037\177\200\303\251\355\240\360\237\230\377";

static size_t mutate(const char *src, size_t n, char *out, size_t cap) {
    memcpy(out, src, n);
    size_t len = n;
    unsigned edits = 1 + fuzz_next() % 3;
    for (unsigned i = 0; i < edits; i++) {
        size_t at = len > 0 ? fuzz_next() % len : 0;
        char c = fuzz_alphabet[fuzz_next() % (sizeof fuzz_alphabet - 1)];
        switch (fuzz_next() % 5) {
            case 0:
                if (len > 0) {
                    out[at] = c;
                }
                break;
            case 1:
                if (len > 0) {
                    memmove(out + at, out + at + 1, len - at - 1);
                    len--;
                }
                break;
            case 2:
                if (len + 1 < cap) {
                    memmove(out + at + 1, out + at, len - at);
                    out[at] = c;
                    len++;
                }
                break;
            case 3:
                len = at;  /* truncate */
                break;
            default:
                if (len + 8 < cap && len > 0) {  /* duplicate a span */
                    size_t span = 1 + fuzz_next() % 6;
                    if (at + span > len) {
                        span = len - at;
                    }
                    memmove(out + at + span, out + at, len - at);
                    len += span;
                }
                break;
        }
    }
    return len;
}

static int t_fuzz_agreement(lua_State *L) {
    static const char *const corpus[] = {
        "{\"id\":\"timer\",\"api\":1,\"files\":[\"a.lua\",\"b.bin\"],\"x\":{\"y\":[1,{\"z\":null}]},\"messages\":{\"start\":\"go\"}}",
        "[1,-2,3.5,-0.0,1e5,1E-2,123456789012345678901234567890,9223372036854775807,-9223372036854775808,0]",
        "{\"s\":\"a\\n\\t\\\"\\\\\\/\\u00e9\\ud83d\\ude00\\u0000\",\"u\":\"\303\251\360\237\230\200\",\"e\":\"\",\"k\\u00e9\":[]}",
        "[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]",
        "{\"a\":{\"b\":{\"c\":{\"d\":[true,false,null,{\"e\":[{}]}]}}}}",
        " \t\r\n[ { \"a\" : [ ] , \"b\" : { } } , \"x\" , 7 , null ] \n",
        "\"just a string with \\u2028 and \303\251\"",
        "-12.5e+3",
    };
    char text[4096], verr[96], perr[96], eerr[96];
    int accepted = 0, rejected = 0, unencodable = 0;
    for (int iteration = 0; iteration < 30000; iteration++) {
        const char *base = corpus[fuzz_next() % (sizeof corpus / sizeof corpus[0])];
        size_t len = iteration % 4 == 0 ? strlen(base) : mutate(base, strlen(base), text, sizeof text);
        if (iteration % 4 == 0) {
            memcpy(text, base, len);
        }
        int top = lua_gettop(L);
        bool v = apps_json_validate(text, len, verr, sizeof verr);
        bool p = apps_json_push(L, text, len, perr, sizeof perr);
        apps_jr_t r;
        apps_jr_init(&r, text, len);
        bool s = apps_jr_skip(&r) && apps_jr_end(&r);
        /* One decoder: the same verdict and the same message from all three, text that is ASCII. */
        CHECK(v == p && p == s && strcmp(verr, perr) == 0 && strcmp(perr, p ? "" : r.err) == 0 && ascii_text(verr) &&
                  lua_gettop(L) == top + (p ? 1 : 0),
              "iteration %d: validate %d '%s', push %d '%s', reader %d '%s'\n  text '%.*s'", iteration, v, verr, p, perr, s,
              r.err, (int)len, text);
        if (!p) {
            rejected++;
            continue;
        }
        accepted++;
        /* What decodes encodes (but for inf, which 1e999 makes), and the text decodes to the same value. */
        size_t out_len = 0;
        char *out = apps_json_encode_value(L, -1, count_realloc, &out_len, eerr, sizeof eerr);
        if (out == NULL) {
            CHECK(strcmp(eerr, "can't encode NaN or infinity") == 0, "iteration %d: encode: '%s'\n  text '%.*s'", iteration,
                  eerr, (int)len, text);
            unencodable++;
        } else {
            CHECK(apps_json_validate(out, out_len, verr, sizeof verr) && apps_json_push(L, out, out_len, perr, sizeof perr) &&
                      same_value(L, -1, -2),
                  "iteration %d: the encoding '%.*s' doesn't decode to the same value\n  text '%.*s'", iteration, (int)out_len,
                  out, (int)len, text);
            count_free(out);
        }
        lua_settop(L, top);
    }
    CHECK(accepted > 3000 && rejected > 3000 && unencodable < accepted, "accepted %d, rejected %d, unencodable %d", accepted,
          rejected, unencodable);
    printf("fuzz_agreement: %d accepted (%d with an inf), %d rejected\n", accepted, unencodable, rejected);
    return 0;
}

static void fuzz_agreement(void) {
    apps_vm_t *vm = new_json_vm(8 * MB);
    run_c(vm, t_fuzz_agreement);
    apps_vm_free(vm);
}

/* ---- stack (ruling R6.9) ---- */

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif

#ifdef UNDER_ASAN

static void stack(void) {
    CHECK(false, "stack measures stack use: build it with apps_host.build(..., sanitize=False)");
}

#else

/* 64 KB stands in for the device's 56 KB (host frames are about 1.5 to 1.7 times Xtensa's). */
#define STACK_HOST (64 * KB)
/* What is left unpainted below the measuring function, for its own frame and the painter's. */
#define STACK_MARGIN 1024

static unsigned char *stack_lo;  /* the lowest byte of the thread's stack */

/* How much stack f uses below this call: the distance from here down to the lowest byte it changed.
 * The stack below is painted first, so the deepest byte f touched shows. The margin under this
 * frame stays unpainted, so f must go deeper than it for the number to be f's (they all do:
 * apps_vm_call and Lua alone take several KB). */
static NOINLINE_FOR_STACK size_t stack_used_by(void (*f)(void *), void *arg) {
    unsigned char here;
    unsigned char *top = &here, *limit = top - STACK_MARGIN, *p = stack_lo;
    memset(stack_lo, 0xA5, (size_t)(limit - stack_lo));
    f(arg);
    while (p < limit && *p == 0xA5) {
        p++;
    }
    return p < limit ? (size_t)(top - p) : 0;
}

/* A chunk, loaded in a VM, run through apps_vm_call as the app task runs one. */
static void call_loaded(void *arg) {
    apps_vm_t *vm = arg;
    char err[256] = "";
    int rc = (int)apps_vm_call(vm, 0, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(rc == APPS_CALL_OK, "got %s '%s'", call_name(rc), err);
}

/* 16 levels, objects and arrays, strings with escapes (the luaL_Buffer leaf) and a float (strtod). */
static void validate16(void *arg) {
    (void)arg;
    char text[16 * (APPS_JSON_MAX_DEPTH + 2)], err[96];
    size_t n = nested_text(text, APPS_JSON_MAX_DEPTH, 2);
    CHECK(apps_json_validate(text, n, err, sizeof err), "%s", err);
    /* And with an error at the deepest point, which formats its message there. */
    text[n - APPS_JSON_MAX_DEPTH - 1] = 'x';
    CHECK(!apps_json_validate(text, n, err, sizeof err), "a bad text passed");
}

static void reader15(void *arg) {
    (void)arg;
    char text[16 * (APPS_JSON_MAX_DEPTH + 2)];
    size_t n = nested_text(text, APPS_JSON_MAX_DEPTH - 1, 0);
    apps_jr_t r;
    apps_jr_init(&r, text, n);
    CHECK(apps_jr_array(&r) && apps_jr_next(&r) == 1 && apps_jr_skip(&r) && apps_jr_next(&r) == 0 && apps_jr_end(&r), "%s", r.err);
}

typedef struct {
    const char *name;
    const char *chunk;         /* run in a VM through apps_vm_call, or NULL: */
    void (*run)(void *);       /* a C function on its own */
    size_t used;
} stack_case_t;

static stack_case_t stack_cases[] = {
    {"baseline", "local v = json.decode('1') return v\n", NULL, 0},
    {"decode16",
     "local s = '\"a\\\\n\\\\u00e9\"'\n"
     "local t = '[' .. s .. ',1.5e3,{\"k\":' .. s .. '}]'\n"
     "for i = 4, 16, 2 do t = '[{\"k\":' .. t .. '}]' end\n"
     "local v = json.decode(t)\n",
     NULL, 0},
    {"decode16_error",
     "local t = '[{\"k\":[' for i = 4, 16 do t = t .. '[' end\n"
     "local ok, e = pcall(json.decode, t .. 'x')\n"
     "eq(ok, false); eq(e, \"json.decode: unexpected 'x' at byte 21\")\n",
     NULL, 0},
    {"encode16",
     "local t = {'a\\n' .. utf8.char(233), 1.5, 3, 'x', k = 2.5}\n"
     "for i = 2, 16 do t = i % 2 == 0 and {t, 'text\\n', 1.25} or {k = t, j = 'v'} end\n"
     "local s = json.encode(t)\n",
     NULL, 0},
    {"validate16", NULL, validate16, 0},
    {"reader15", NULL, reader15, 0},
};

static void *stack_thread(void *unused) {
    (void)unused;
    for (size_t i = 0; i < sizeof stack_cases / sizeof stack_cases[0]; i++) {
        stack_case_t *c = &stack_cases[i];
        if (c->chunk != NULL) {
            apps_vm_t *vm = new_json_vm(1 * MB);
            char err[256] = "";
            CHECK(apps_vm_load(vm, "main.lua", c->chunk, strlen(c->chunk), err, sizeof err), "%s: %s", c->name, err);
            c->used = stack_used_by(call_loaded, vm);
            lua_settop(apps_vm_lua(vm), 0);
            apps_vm_free(vm);
        } else {
            c->used = stack_used_by(c->run, NULL);
        }
    }
    return NULL;
}

/* What JSON adds over the baseline (a chunk calling json.decode('1')) must stay small: 16 levels of
 * frames and one leaf. The bound is generous against the marks on the host and tight against a large
 * array in a recursive function, which would add hundreds of bytes to each level. */
#define JSON_STACK_BOUND (6 * KB)

/* Runs every case on a thread whose stack is exactly STACK_HOST bytes: an mmap'd region above a PROT_NONE
 * page, so overflowing it is a crash, not a silent pass. */
static void stack(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE), size = STACK_HOST;
    unsigned char *map = mmap(NULL, page + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        CHECK(false, "mmap failed");
        return;
    }
    stack_lo = map + page;
    int rc = mprotect(map, page, PROT_NONE);
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstack(&attr, stack_lo, size);
    }
    if (rc == 0) {
        rc = pthread_create(&thread, &attr, stack_thread, NULL);
    }
    if (rc == 0) {
        pthread_join(thread, NULL);
    }
    pthread_attr_destroy(&attr);
    munmap(map, page + size);
    CHECK(rc == 0, "no thread on the exact stack (%d)", rc);
    if (rc != 0) {
        return;
    }
    size_t base = stack_cases[0].used;
    for (size_t i = 0; i < sizeof stack_cases / sizeof stack_cases[0]; i++) {
        const stack_case_t *c = &stack_cases[i];
        bool over_baseline = c->chunk != NULL && i > 0;
        printf("json stack: %-15s %6zu bytes", c->name, c->used);
        if (over_baseline) {
            printf(" (%+zd over the baseline)", (ssize_t)c->used - (ssize_t)base);
        }
        printf("\n");
        CHECK(c->used > STACK_MARGIN, "%s: %zu bytes, too few to be measured", c->name, c->used);
        CHECK(!over_baseline || c->used < base + JSON_STACK_BOUND, "%s: %zu bytes over the baseline", c->name, c->used - base);
        CHECK(c->chunk != NULL || c->used < JSON_STACK_BOUND, "%s: %zu bytes", c->name, c->used);
    }
    fflush(stdout);
}

#endif /* UNDER_ASAN */

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"decode_types", decode_types},
        {"decode_numbers", decode_numbers},
        {"decode_strings", decode_strings},
        {"decode_valid", decode_valid},
        {"decode_errors", decode_errors},
        {"decode_limits", decode_limits},
        {"encode_roundtrip", encode_roundtrip},
        {"encode_values", encode_values},
        {"encode_strings", encode_strings},
        {"encode_errors", encode_errors},
        {"encode_limits", encode_limits},
        {"encode_large_nested", encode_large_nested},
        {"encode_shared_subtables", encode_shared_subtables},
        {"encode_raw", encode_raw},
        {"c_contracts", c_contracts},
        {"validate", validate},
        {"module", module},
        {"memory_caps", memory_caps},
        {"allocation_sweep", allocation_sweep},
        {"reader_manifest", reader_manifest},
        {"reader_strict", reader_strict},
        {"compact", compact},
        {"fuzz_agreement", fuzz_agreement},
        {"stack_discipline", stack_discipline},
        {"overrun", overrun},
        {"stack", stack},
    };
    if (argc != 2) {
        fputs("usage: muse_apps_json_harness <case>|list\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "list") == 0) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            puts(cases[i].name);
        }
        return 0;
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
