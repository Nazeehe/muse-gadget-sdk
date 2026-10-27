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
 * Checks the Lua that tests/apps_host.py builds from components/lua: the
 * number model apps run on, and the device's build definitions. Usage:
 * muse_apps_lua_harness numbers|nesting_limit. Exits 0 when the case holds
 * and prints what it got when it doesn't.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

_Static_assert(LUA_MAXINTEGER == INT64_MAX, "Lua integers must be 64-bit");
_Static_assert(sizeof(lua_Number) == sizeof(double), "Lua floats must be double");

static int failures;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            failures++;                                       \
            fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                     \
            fputc('\n', stderr);                              \
        }                                                     \
    } while (0)

/* The top of the stack as text, for a failure message. */
static const char *top_text(lua_State *L) {
    const char *text = lua_tostring(L, -1);
    return text != NULL ? text : "(not a string)";
}

/* The stack as "type:value, ...", for a failure message. */
static const char *describe(lua_State *L, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = 1; i <= lua_gettop(L); i++) {
        size_t used = strlen(out);
        const char *type = luaL_typename(L, i);
        const char *text = luaL_tolstring(L, i, NULL);
        snprintf(out + used, cap - used, "%s%s:%s", i > 1 ? ", " : "", type, text);
        lua_pop(L, 1);
    }
    return out;
}

/* Only what a case needs: the base functions and math. */
static lua_State *open_state(void) {
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        fputs("luaL_newstate failed\n", stderr);
        return NULL;
    }
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_settop(L, 0);
    return L;
}

static void numbers(lua_State *L) {
    char got[256];

    /* 2^53 + 1: no double holds it, so only a real 64-bit integer keeps it. */
    CHECK(luaL_dostring(L, "return 9007199254740993") == LUA_OK, "%s", top_text(L));
    CHECK(lua_gettop(L) == 1 && lua_isinteger(L, 1) && lua_tointeger(L, 1) == INT64_C(9007199254740993),
          "want integer:9007199254740993, got %s", describe(L, got, sizeof got));
    lua_settop(L, 0);

    CHECK(luaL_dostring(L, "return 7 // 2, 7 / 2, math.type(1), math.type(1.0)") == LUA_OK,
          "%s", top_text(L));
    CHECK(lua_gettop(L) == 4 && lua_isinteger(L, 1) && lua_tointeger(L, 1) == 3
              && lua_type(L, 2) == LUA_TNUMBER && !lua_isinteger(L, 2) && lua_tonumber(L, 2) == 3.5
              && lua_type(L, 3) == LUA_TSTRING && strcmp(lua_tostring(L, 3), "integer") == 0
              && lua_type(L, 4) == LUA_TSTRING && strcmp(lua_tostring(L, 4), "float") == 0,
          "want number:3, number:3.5, string:integer, string:float, got %s", describe(L, got, sizeof got));
}

/* Loads "return ((...(1)...))" with `depth` pairs of parentheses. */
static int load_nested(lua_State *L, size_t depth) {
    char src[512];
    size_t n = 0;
    if (7 + 2 * depth + 2 > sizeof src) {
        fputs("nesting too deep for the harness's buffer\n", stderr);
        return -1;
    }
    memcpy(src, "return ", 7);
    n += 7;
    memset(src + n, '(', depth);
    n += depth;
    src[n++] = '1';
    memset(src + n, ')', depth);
    n += depth;
    src[n] = '\0';
    return luaL_loadstring(L, src);
}

/* components/lua/CMakeLists.txt's LUAI_MAXCCALLS has to reach the host build, or the
 * host would accept nesting the device refuses. The parser spends one level on the
 * statement, one on the returned expression and one more inside each pair of
 * parentheses, and level LUAI_MAXCCALLS is the "C stack overflow" error. At 40,
 * `return ((...(1)...))` therefore parses with 37 pairs and fails with 38 (at Lua's
 * default of 200 it would be 197 and 198). Changing the limit changes this number. */
enum { DEEPEST_PAIRS = 37 };

static void nesting_limit(lua_State *L) {
    int rc = load_nested(L, DEEPEST_PAIRS);
    CHECK(rc == LUA_OK, "%d nested parentheses must parse, rc=%d: %s", DEEPEST_PAIRS, rc, top_text(L));
    lua_settop(L, 0);

    /* The parser's level check raises a runtime error, not a syntax error. */
    rc = load_nested(L, DEEPEST_PAIRS + 1);
    CHECK(rc != LUA_OK && strstr(top_text(L), "C stack overflow") != NULL,
          "%d nested parentheses must hit the C stack limit, rc=%d: %s", DEEPEST_PAIRS + 1, rc, top_text(L));
    lua_settop(L, 0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: muse_apps_lua_harness numbers|nesting_limit\n", stderr);
        return 2;
    }
    lua_State *L = open_state();
    if (L == NULL) {
        return 1;
    }
    if (strcmp(argv[1], "numbers") == 0) {
        numbers(L);
    } else if (strcmp(argv[1], "nesting_limit") == 0) {
        nesting_limit(L);
    } else {
        fprintf(stderr, "unknown case %s\n", argv[1]);
        failures++;
    }
    lua_close(L);
    return failures != 0;
}
