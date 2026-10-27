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
 * One Muse app's Lua state (apps_vm.h). Every function the sandbox gives app code
 * checks its arguments, since an app can call any of them with anything, and keeps
 * what it allocates in the Lua heap (userdata), so an error raised halfway leaks
 * nothing and every byte counts against the app's cap.
 */

#include "apps_vm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "apps_suggest.h"
#include "lauxlib.h"
#include "lualib.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The count hook's period, in VM instructions. */
#define HOOK_EVERY 1000

/* Room for a kill reason apps_vm formats: the longest, with a 10-digit budget, is 97 bytes. */
#define KILL_REASON_MAX 128

struct apps_vm {
    apps_vm_config_t cfg;
    lua_State *L;
    size_t mem_used;      /* bytes the Lua heap holds; never more than cfg.mem_limit */
    size_t mem_peak;      /* the most it has held */
    size_t mem_refusals;  /* requests answered with NULL */

    /* Only the task running the VM touches these. */
    int depth;                   /* apps_vm_call nesting: 0 between calls */
    uint32_t budget;             /* the outermost call's, in VM instructions */
    uint64_t call_instructions;  /* run since the outermost call began */
    uint64_t instructions;       /* run in the VM's lifetime */
    int64_t started_ms;          /* now_ms() when the outermost call began */

    /* Other tasks read these, and apps_vm_kill writes the reason. Atomic loads and stores
     * only, which need nothing of the platform: on the device the struct is in PSRAM, where
     * the S3 has no hardware compare-and-set. */
    _Atomic(const char *) kill_reason;  /* NULL until the VM is killed, then never NULL again */
    atomic_bool in_call;
    _Atomic int64_t call_started_ms;

    /* The reasons apps_vm formats when it kills, each written at most once, before
     * kill_reason points at it. */
    char budget_reason[KILL_REASON_MAX];
    char nomem_reason[KILL_REASON_MAX];
};

/* The VM a state, or any of its coroutines, belongs to: the allocator's user data. */
static apps_vm_t *vm_of(lua_State *L) {
    void *ud;
    lua_getallocf(L, &ud);
    return ud;
}

/* ---- Text helpers ---- */

/* How much of s, which is longer than n bytes, to keep so as to cut at most n bytes
 * without splitting a UTF-8 sequence: steps back past up to 3 continuation bytes
 * (0x80-0xBF), the most a valid sequence has. */
static size_t utf8_cut(const char *s, size_t n) {
    for (int i = 0; i < 3 && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80; i++) {
        n--;
    }
    return n;
}

/* Copies msg into out (cap bytes with the NUL), cut on a character boundary. */
static void copy_message(char *out, size_t cap, const char *msg) {
    if (out == NULL || cap == 0) {
        return;
    }
    size_t n = strlen(msg);
    if (n >= cap) {
        n = utf8_cut(msg, cap - 1);
    }
    memcpy(out, msg, n);
    out[n] = '\0';
}

/* Text written into a fixed buffer, always terminated. Once something has to be cut,
 * nothing more is written. */
typedef struct {
    char *buf;
    size_t cap, len;
    bool full;
} text_t;

static void text_put(text_t *t, const char *s, size_t n) {
    if (t->full) {
        return;
    }
    size_t room = t->cap - 1 - t->len;
    if (n > room) {
        n = utf8_cut(s, room);
        t->full = true;
    }
    if (n > 0) {
        memcpy(t->buf + t->len, s, n);
        t->len += n;
    }
    t->buf[t->len] = '\0';
}

/* Whether s[0, n) holds what. */
static bool contains(const char *s, size_t n, const char *what) {
    size_t m = strlen(what);
    for (size_t i = 0; m <= n && i <= n - m; i++) {
        if (memcmp(s + i, what, m) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- The capped heap ---- */

/* Lua's allocator. When ptr is NULL, osize is the new object's type, not a size. */
static void *vm_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    apps_vm_t *vm = ud;
    size_t old = ptr != NULL ? osize : 0;
    if (nsize == 0) {
        if (ptr != NULL) {
            vm->cfg.free_fn(ptr);
            vm->mem_used -= old;
        }
        return NULL;
    }
    /* Past the cap, only NULL: Lua collects garbage and asks again, then raises
     * LUA_ERRMEM. An allocator must never raise, and must not end the app on the first
     * refusal, which a collection may still satisfy. */
    if (nsize > old && nsize - old > vm->cfg.mem_limit - vm->mem_used) {
        vm->mem_refusals++;
        return NULL;
    }
    void *block = vm->cfg.realloc_fn(ptr, nsize);
    if (block == NULL) {
        vm->mem_refusals++;
        return NULL;
    }
    vm->mem_used = vm->mem_used - old + nsize;
    if (vm->mem_used > vm->mem_peak) {
        vm->mem_peak = vm->mem_used;
    }
    return block;
}

/* An error outside every protected call: a bug in the C code driving the state. Lua
 * aborts when this returns; say what the error was first. */
static int vm_panic(lua_State *L) {
    const char *msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "error object is not a string";
    lua_writestringerror("apps_vm: unprotected Lua error (%s)\n", msg);
    return 0;
}

/* ---- Strict globals ---- */

/* Names apps reach for that the sandbox leaves out on purpose. Each message is a
 * format for the name (ruling A17: every loader names itself). */
#define FROM_OWN_FILES "'%s' is not available in Muse apps: code comes only from the app's own files"
static const struct {
    const char *name, *why;
} absent[] = {
    {"io", "'io' is not available in Muse apps; use store / sys.log"},
    {"debug", "'debug' is not available in Muse apps"},
    {"load", FROM_OWN_FILES},
    {"loadstring", FROM_OWN_FILES},
    {"dofile", FROM_OWN_FILES},
    {"loadfile", FROM_OWN_FILES},
    {"collectgarbage", "'collectgarbage' is not available in Muse apps; use sys.mem()"},
};

/* The most names one miss gathers. Past it the miss gets no suggestion: its cost, and
 * the userdata it fills, stay small however many globals an app makes. */
#define SUGGEST_CANDIDATES_MAX 512

typedef struct {
    const char **names;  /* NULL while counting */
    int cap, n;
} candidates_t;

static void add_candidate(candidates_t *c, const char *name) {
    if (name[0] == '(' || name[0] == '\0') {
        return;  /* Lua's "(temporary)" slots, a C upvalue */
    }
    if (c->names != NULL && c->n < c->cap) {
        c->names[c->n] = name;
    }
    c->n++;
}

/* The names a misspelt global may have meant, nearest scope first: the reading
 * function's locals, its upvalues, then the sandbox's keys (the table at index 1),
 * stopping once there are more than SUGGEST_CANDIDATES_MAX. The strings stay alive
 * while the function runs and the table holds its keys. */
static void collect_candidates(lua_State *L, lua_Debug *reader, candidates_t *c) {
    const char *name;
    if (reader != NULL) {
        for (int i = 1; c->n <= SUGGEST_CANDIDATES_MAX && (name = lua_getlocal(L, reader, i)) != NULL; i++) {
            lua_pop(L, 1);
            add_candidate(c, name);
        }
        lua_getinfo(L, "f", reader);
        for (int i = 1; c->n <= SUGGEST_CANDIDATES_MAX && (name = lua_getupvalue(L, -1, i)) != NULL; i++) {
            lua_pop(L, 1);
            add_candidate(c, name);
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (c->n <= SUGGEST_CANDIDATES_MAX && lua_next(L, 1) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            add_candidate(c, lua_tostring(L, -2));
        }
        lua_pop(L, 1);  /* the value; lua_next takes the key */
    }
    if (c->n > SUGGEST_CANDIDATES_MAX) {
        lua_pop(L, 1);  /* the key lua_next didn't get to finish with */
    }
}

/* The sandbox's __index, so only for names it doesn't have. Only string keys are
 * globals: _G[1] reads nil, as in any table. */
static int sandbox_index(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_type(L, 2) != LUA_TSTRING) {
        return 0;
    }
    size_t len;
    const char *name = lua_tolstring(L, 2, &len);
    for (size_t i = 0; i < ARRAY_LEN(absent); i++) {
        if (strlen(absent[i].name) == len && memcmp(name, absent[i].name, len) == 0) {
            return luaL_error(L, absent[i].why, absent[i].name);
        }
    }
    /* Level 1 is the function that read the name. Count the candidates, then gather
     * them into a userdata, which the error leaves to the collector. */
    lua_Debug ar;
    lua_Debug *reader = lua_getstack(L, 1, &ar) ? &ar : NULL;
    candidates_t c = {NULL, 0, 0};
    collect_candidates(L, reader, &c);
    const char *hint = NULL;
    if (c.n > 0 && c.n <= SUGGEST_CANDIDATES_MAX) {
        c.cap = c.n;
        c.n = 0;
        c.names = lua_newuserdatauv(L, (size_t)c.cap * sizeof *c.names, 0);
        collect_candidates(L, reader, &c);
        hint = apps_suggest(name, c.names, c.n < c.cap ? c.n : c.cap);
    }
    if (hint != NULL) {
        return luaL_error(L, "unknown global '%s' (did you mean '%s'?)", name, hint);
    }
    return luaL_error(L, "unknown global '%s'", name);
}

/* ---- setmetatable ---- */

/* Base's setmetatable, refusing a metatable that has __gc: Lua runs finalizers with
 * hooks off, beyond any instruction budget. Lua marks an object for finalization only
 * when its metatable is set, and looks __gc up raw, so a raw check here is enough: a
 * __gc added to the metatable later never runs. */
static int sandbox_setmetatable(lua_State *L) {
    int t = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
    if (t == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        bool finalizer = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (finalizer) {
            return luaL_error(L, "setmetatable: __gc finalizers are not available in Muse apps");
        }
    }
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) {
        return luaL_error(L, "cannot change a protected metatable");
    }
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

/* ---- string ---- */

/* string.match, gmatch and gsub, named by the closure's upvalue. They raise before
 * looking at their arguments: nothing reaches lstrlib's matcher, whose backtracking
 * no hook can interrupt. */
static int sandbox_no_patterns(lua_State *L) {
    return luaL_error(L, "string.%s is not available in Muse apps (api 1): use string.find(s, text, 1, true), string.split or json.decode",
                      lua_tostring(L, lua_upvalueindex(1)));
}

/* lstrlib's SPECIALS: a pattern with none of these matches only itself. */
static bool has_specials(const char *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != '\0' && strchr("^$*+?.([%-", p[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* string.find's init as a 0-based offset, as lstrlib's posrelatI gives it, except that
 * a start past the end stays past it with a 32-bit size_t. More than len: nothing can
 * be found. */
static size_t start_offset(lua_Integer init, size_t len) {
    if (init > 0) {
        return (lua_Unsigned)init - 1 > (lua_Unsigned)len ? len + 1 : (size_t)(init - 1);
    }
    if (init == 0 || init < -(lua_Integer)len) {
        return 0;
    }
    return len + (size_t)init;
}

/* Plain text to search for: memchr for one byte, Knuth-Morris-Pratt for more, so a
 * search takes time linear in what it scans, whatever the input. */
typedef struct {
    const char *p;
    size_t len;
    const uint32_t *next;  /* KMP's failure table, len entries; NULL when unused */
} needle_t;

/* The longest needle whose table uint32_t can index and size_t can size. */
#define NEEDLE_MAX (SIZE_MAX / sizeof(uint32_t) < UINT32_MAX ? SIZE_MAX / sizeof(uint32_t) : (size_t)UINT32_MAX)

/* Readies p for searches of at most hay_len bytes. The KMP table is a userdata in the
 * capped heap, left on the stack. arg is p's argument, for the error. */
static void needle_init(lua_State *L, needle_t *nd, const char *p, size_t len, size_t hay_len, int arg) {
    nd->p = p;
    nd->len = len;
    nd->next = NULL;
    if (len < 2 || len > hay_len) {
        return;  /* memchr, or no search at all */
    }
    luaL_argcheck(L, len <= NEEDLE_MAX, arg, "too long");
    uint32_t *next = lua_newuserdatauv(L, len * sizeof *next, 0);
    /* next[i]: the longest proper prefix of p[0..i] that is also its suffix. */
    uint32_t k = 0;
    next[0] = 0;
    for (size_t i = 1; i < len; i++) {
        while (k > 0 && p[i] != p[k]) {
            k = next[k - 1];
        }
        if (p[i] == p[k]) {
            k++;
        }
        next[i] = k;
    }
    nd->next = next;
}

/* The first occurrence of the needle in s[0, n), n at most needle_init's hay_len, or NULL. */
static const char *needle_find(const needle_t *nd, const char *s, size_t n) {
    if (nd->len == 0) {
        return s;
    }
    if (nd->len > n) {
        return NULL;
    }
    if (nd->len == 1) {
        return memchr(s, (unsigned char)nd->p[0], n);
    }
    uint32_t k = 0;  /* bytes of the needle matched */
    for (size_t i = 0; i < n; i++) {
        while (k > 0 && s[i] != nd->p[k]) {
            k = nd->next[k - 1];
        }
        if (s[i] == nd->p[k]) {
            k++;
        }
        if (k == nd->len) {
            return s + i + 1 - nd->len;
        }
    }
    return NULL;
}

/* string.find(s, text [, init [, plain]]): Lua's find, minus patterns. A pattern
 * without special characters is plain text, as in Lua; any other raises. */
static int sandbox_find(lua_State *L) {
    size_t ls, lp;
    const char *s = luaL_checklstring(L, 1, &ls);
    const char *p = luaL_checklstring(L, 2, &lp);
    lua_Integer init = luaL_optinteger(L, 3, 1);
    if (!lua_toboolean(L, 4) && has_specials(p, lp)) {
        return luaL_error(L, "string.find: patterns are not available in Muse apps (api 1); for a plain search pass true as the 4th argument");
    }
    size_t start = start_offset(init, ls);
    if (start <= ls) {
        needle_t nd;
        needle_init(L, &nd, p, lp, ls - start, 2);
        const char *at = needle_find(&nd, s + start, ls - start);
        if (at != NULL) {
            lua_pushinteger(L, (lua_Integer)(at - s) + 1);
            lua_pushinteger(L, (lua_Integer)(at - s) + (lua_Integer)lp);
            return 2;
        }
    }
    luaL_pushfail(L);
    return 1;
}

/* string.split(s, sep): the pieces of s between occurrences of sep, plain text, empty
 * pieces kept. ("a,,b"):split(",") is {"a", "", "b"}; (""):split(",") is {""}. */
static int sandbox_split(lua_State *L) {
    size_t ls, lsep;
    const char *s = luaL_checklstring(L, 1, &ls);
    const char *sep = luaL_checklstring(L, 2, &lsep);
    luaL_argcheck(L, lsep > 0, 2, "empty separator");
    lua_settop(L, 2);
    needle_t nd;
    needle_init(L, &nd, sep, lsep, ls, 2);
    lua_newtable(L);
    int pieces = lua_gettop(L);
    const char *at = s, *end = s + ls;
    for (lua_Integer i = 1;; i++) {
        const char *hit = needle_find(&nd, at, (size_t)(end - at));
        const char *stop = hit != NULL ? hit : end;
        lua_pushlstring(L, at, (size_t)(stop - at));
        lua_rawseti(L, pieces, i);
        if (hit == NULL) {
            return 1;  /* the table, on top */
        }
        at = hit + lsep;
    }
}

static bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* string.trim(s): s without leading and trailing ASCII whitespace (space \t \n \v \f \r). */
static int sandbox_trim(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    size_t first = 0, end = len;
    while (first < end && is_space(s[first])) {
        first++;
    }
    while (end > first && is_space(s[end - 1])) {
        end--;
    }
    if (first == 0 && end == len) {
        lua_settop(L, 1);  /* nothing to trim: s itself */
        return 1;
    }
    lua_pushlstring(L, s + first, end - first);
    return 1;
}

/* What api 1 adds to string. */
static const luaL_Reg apps_string_funcs[] = {
    {"split", sandbox_split},
    {"trim", sandbox_trim},
    {NULL, NULL},
};

/* ---- Lua's C loops, bounded ----
 *
 * Count hooks run between VM instructions, so C work between two hook checks has to be
 * bounded, and the cap bounds only work that allocates. Lua's string.rep and five
 * table functions can loop without allocating: rep over an empty string, the table
 * functions over a range or a lying __len with nothing behind it. Reading an element
 * that isn't there needn't allocate either: an __index chain can end at a C function
 * that returns "" at once (a string's metatable is the app's to change). Each wrapper
 * checks the count first, then runs Lua's own function (its upvalue 1) on its own
 * frame, so arguments, results and Lua's errors are unchanged. */

/* Lua's own function, called as if it had been called instead of the wrapper. */
static int call_lua_own(lua_State *L) {
    return lua_tocfunction(L, lua_upvalueindex(1))(L);
}

/* string.rep(s, n [, sep]): an empty result returns at once; Lua's would copy nothing
 * n - 1 times. A non-empty one allocates its full size first, which the cap bounds. */
static int sandbox_rep(lua_State *L) {
    size_t len, sep_len;
    luaL_checklstring(L, 1, &len);
    lua_Integer n = luaL_checkinteger(L, 2);
    luaL_optlstring(L, 3, "", &sep_len);
    if (n <= 0 || len + sep_len == 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    return call_lua_own(L);
}

/* An array slot is a TValue: 16 bytes with 64-bit integers and doubles. */
#define SLOT_BYTES 16

/* The most elements this app's heap could hold. A count past it can only be a lying
 * __len or a range with nothing in it, which Lua would still walk element by element. */
static lua_Unsigned slot_limit(lua_State *L) {
    return vm_of(L)->cfg.mem_limit / SLOT_BYTES;
}

static int too_many(lua_State *L, const char *fn) {
    return luaL_error(L, "table.%s: more elements than this app's memory can hold", fn);
}

static bool has_metafield(lua_State *L, const char *name) {
    if (luaL_getmetafield(L, 1, name) == LUA_TNIL) {
        return false;
    }
    lua_pop(L, 1);
    return true;
}

/* Whether ltablib's checktab accepts arg 1 for reading and length, and for writing too
 * if writes: a table, or a value whose metatable has __index and __len (and
 * __newindex). Otherwise Lua's own function raises its usual error, before taking any
 * length. */
static bool table_like(lua_State *L, bool writes) {
    return lua_type(L, 1) == LUA_TTABLE ||
           (has_metafield(L, "__index") && has_metafield(L, "__len") && (!writes || has_metafield(L, "__newindex")));
}

/* A proxy's __len: the count its wrapper checked (upvalue 1). */
static int proxy_len(lua_State *L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

/* #arg 1 as Lua's own function will see it, taking __len only once.
 * - A table without __len: Lua's own function computes the same length again, since
 *   nothing runs in between.
 * - Otherwise arg 1 becomes a proxy whose __len returns this count and whose reads and
 *   writes go through to the object (whose own metamethods still see the object). A
 *   __len that answers small here and huge to Lua's own call can't slip past. */
static lua_Integer len_for_lua_own(lua_State *L) {
    if (luaL_getmetafield(L, 1, "__len") == LUA_TNIL) {
        return luaL_len(L, 1);
    }
    lua_pop(L, 1);
    lua_Integer n = luaL_len(L, 1);
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "__index");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "__newindex");
    lua_pushinteger(L, n);
    lua_pushcclosure(L, proxy_len, 1);
    lua_setfield(L, -2, "__len");
    lua_setmetatable(L, -2);
    lua_replace(L, 1);
    return n;
}

/* table.move(a1, f, e, t [, a2]) moves e - f + 1 elements. */
static int sandbox_move(lua_State *L) {
    lua_Integer f = luaL_checkinteger(L, 2);
    lua_Integer e = luaL_checkinteger(L, 3);
    luaL_checkinteger(L, 4);
    /* e - f + 1 > limit, without overflow: e - f is exact as unsigned when e >= f. */
    if (e >= f && (lua_Unsigned)e - (lua_Unsigned)f >= slot_limit(L)) {
        return too_many(L, "move");
    }
    return call_lua_own(L);
}

/* table.insert(t, pos, v) shifts t[pos..#t] up one: #t + 1 - pos elements. Counted
 * from the shift itself, not #t alone: a negative __len passes Lua's position check
 * for math.mininteger and would shift 2^63 elements. */
static int sandbox_insert(lua_State *L) {
    if (lua_gettop(L) == 3 && table_like(L, true)) {
        lua_Integer e = luaL_intop(+, len_for_lua_own(L), 1);  /* the first empty slot, as Lua has it */
        lua_Integer pos = luaL_checkinteger(L, 2);
        bool lua_accepts = (lua_Unsigned)pos - 1u < (lua_Unsigned)e;
        if (lua_accepts && e > pos && (lua_Unsigned)e - (lua_Unsigned)pos > slot_limit(L)) {
            return too_many(L, "insert");
        }
    }
    return call_lua_own(L);
}

/* table.remove(t [, pos]) shifts t[pos + 1..#t] down one: #t - pos elements, counted as
 * insert counts them. */
static int sandbox_remove(lua_State *L) {
    if (table_like(L, true)) {
        lua_Integer size = len_for_lua_own(L);
        lua_Integer pos = luaL_optinteger(L, 2, size);
        bool lua_accepts = pos == size || (lua_Unsigned)pos - 1u <= (lua_Unsigned)size;
        if (lua_accepts && pos < size && (lua_Unsigned)size - (lua_Unsigned)pos > slot_limit(L)) {
            return too_many(L, "remove");
        }
    }
    return call_lua_own(L);
}

/* table.sort(t [, comp]) sorts #t elements. */
static int sandbox_sort(lua_State *L) {
    if (table_like(L, true)) {
        lua_Integer n = len_for_lua_own(L);
        if (n > 1 && (lua_Unsigned)n > slot_limit(L)) {
            return too_many(L, "sort");
        }
    }
    return call_lua_own(L);
}

/* table.concat(t [, sep [, i [, j]]]) reads t[i..j], j - i + 1 elements, j defaulting to
 * #t. Arguments are checked in Lua's order; sep isn't converted here, which would
 * allocate between this length and Lua's own. */
static int sandbox_concat(lua_State *L) {
    if (table_like(L, false)) {
        lua_Integer len = len_for_lua_own(L);
        luaL_argexpected(L, lua_isnoneornil(L, 2) || lua_isstring(L, 2), 2, "string");
        lua_Integer i = luaL_optinteger(L, 3, 1);
        lua_Integer j = luaL_optinteger(L, 4, len);
        if (j >= i && (lua_Unsigned)j - (lua_Unsigned)i >= slot_limit(L)) {
            return too_many(L, "concat");
        }
    }
    return call_lua_own(L);
}

/* Replaces the function name in the table on top with wrapper, which gets Lua's own as
 * its upvalue 1. */
static void wrap_lua_own(lua_State *L, const char *name, lua_CFunction wrapper) {
    lua_getfield(L, -1, name);
    lua_pushcclosure(L, wrapper, 1);
    lua_setfield(L, -2, name);
}

/* ---- Budgets and the kill ----
 *
 * A kill is the VM's kill_reason going from NULL to a reason, for good. It is raised as
 * the sentinel below, but every check is of kill_reason, never of the error object: a
 * __close handler can replace the error while it unwinds. */

static char kill_sentinel;

#define WALL_REASON "stopped: one callback ran longer than 1 s"

static const char *kill_reason(const apps_vm_t *vm) {
    return atomic_load(&vm->kill_reason);
}

/* Kills the VM unless it is dying already. Any task: a load and a store, so two kills at
 * the same moment leave one of their reasons, never a torn one. */
static void set_kill(apps_vm_t *vm, const char *reason) {
    if (atomic_load(&vm->kill_reason) == NULL) {
        atomic_store(&vm->kill_reason, reason);
    }
}

/* head, n in decimal, then tail, into out (KILL_REASON_MAX bytes). No allocation, and
 * hardly any stack: the hook can kill at the deepest point of a call. */
static const char *format_reason(char *out, const char *head, unsigned n, const char *tail) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    text_t t = {out, KILL_REASON_MAX, 0, false};
    text_put(&t, head, strlen(head));
    while (count > 0) {
        text_put(&t, &digits[--count], 1);
    }
    text_put(&t, tail, strlen(tail));
    return out;
}

static void kill_over_budget(apps_vm_t *vm) {
    if (kill_reason(vm) == NULL) {
        set_kill(vm, format_reason(vm->budget_reason, "stopped: ran more than ", (unsigned)vm->budget,
                                   " VM instructions in one callback (use timer.every for long work)"));
    }
}

/* A LUA_ERRMEM reached a protected call: Lua's emergency collection couldn't make room
 * (ruling A2). */
static void kill_for_memory(apps_vm_t *vm) {
    if (kill_reason(vm) == NULL) {
        set_kill(vm, format_reason(vm->nomem_reason, "out of memory: this app may use ",
                                   (unsigned)(vm->cfg.mem_limit / 1024), " KB (memory_kb in manifest.json)"));
    }
}

static apps_call_t kill_status(const apps_vm_t *vm, const char *reason) {
    return reason == vm->nomem_reason ? APPS_CALL_NOMEM : APPS_CALL_KILLED;
}

static void vm_hook(lua_State *L, lua_Debug *ar);

/* Raises the kill on L, the running thread, after setting its hook to run before every
 * instruction: the __close handlers Lua runs while the kill unwinds then stop before their
 * first. lua_sethook is safe here, on the task running the VM; it isn't from another task
 * (ruling A15). Needs one free stack slot. */
static void raise_kill_here(lua_State *L) {
    lua_sethook(L, vm_hook, LUA_MASKCOUNT, 1);
    lua_pushlightuserdata(L, &kill_sentinel);
    lua_error(L);
}

/* raise_kill_here from one of the C functions below, dropping its stack first (none marks
 * a slot to-be-closed) so that the push has room whatever a call left. */
static int raise_kill(lua_State *L) {
    lua_settop(L, 0);
    raise_kill_here(L);
    return 0;
}

/* The count hook, every HOOK_EVERY instructions in every coroutine. Inside a call it kills
 * a call that is over its budget or over APPS_WALL_MS; any time, it raises the kill once
 * the VM is dying. Lua runs it with hooks off, and a message handler the raise reaches
 * runs inside it, hooks still off: both message handlers let a kill through untouched. */
static void vm_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    apps_vm_t *vm = vm_of(L);
    if (kill_reason(vm) == NULL) {
        vm->instructions += HOOK_EVERY;
        vm->call_instructions += HOOK_EVERY;
        if (vm->depth > 0) {
            if (vm->call_instructions > vm->budget) {
                kill_over_budget(vm);
            } else if (vm->cfg.now_ms() - vm->started_ms > APPS_WALL_MS) {
                set_kill(vm, WALL_REASON);
            }
        }
        if (kill_reason(vm) == NULL) {
            return;
        }
    }
    raise_kill_here(L);  /* a hook gets LUA_MINSTACK free slots */
}

/* The most of an error message apps_vm_call's traceback copies: the copy is in the Lua heap,
 * and a huge message near the cap would turn an app's error into running out of memory. */
#define TRACE_MSG_MAX 1024

/* apps_vm_call's message handler, run where the error was raised: the message, then Lua's
 * traceback, which put_error trims to the app's frames. A kill goes through untouched and
 * allocates nothing: when the hook raised it, this runs inside the hook. */
static int traceback_handler(lua_State *L) {
    if (kill_reason(vm_of(L)) != NULL) {
        return 1;
    }
    size_t len;
    const char *msg = lua_tolstring(L, 1, &len);
    if (msg == NULL) {
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    } else if (len > TRACE_MSG_MAX) {
        msg = lua_pushlstring(L, msg, utf8_cut(msg, TRACE_MSG_MAX));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* The error on top of the stack into out (cap bytes, cut on a character boundary): the
 * message, then the traceback without its "[C]:" and "(...tail calls...)" lines. The
 * traceback is what follows the last "\nstack traceback:": a message can hold that text,
 * a frame line can't. */
static void put_error(lua_State *L, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return;
    }
    if (lua_type(L, -1) != LUA_TSTRING) {
        snprintf(out, cap, "(error object is a %s value)", luaL_typename(L, -1));
        return;
    }
    static const char header[] = "\nstack traceback:";
    const size_t header_len = sizeof header - 1;
    size_t len;
    const char *s = lua_tolstring(L, -1, &len), *end = s + len, *tb = NULL;
    for (size_t i = len >= header_len ? len - header_len + 1 : 0; i-- > 0;) {
        if (memcmp(s + i, header, header_len) == 0) {
            tb = s + i;
            break;
        }
    }
    text_t t = {out, cap, 0, false};
    out[0] = '\0';
    if (tb == NULL) {
        text_put(&t, s, len);
        return;
    }
    const char *line = tb + header_len;
    text_put(&t, s, (size_t)(line - s));
    while (line < end) {  /* each line starts at its '\n' */
        const char *next = memchr(line + 1, '\n', (size_t)(end - line - 1));
        size_t n = (size_t)((next != NULL ? next : end) - line);
        if (!contains(line, n, "[C]:") && !contains(line, n, "(...tail calls...)")) {
            text_put(&t, line, n);
        }
        line += n;
    }
}

/* ---- pcall, xpcall and coroutines that can't keep a kill ----
 *
 * Lua's own, except that each raises the kill again once its protected call or its resume
 * returns, also after a yield, and a memory error that reaches one kills first. */

/* After pcall's or xpcall's protected call: called by them, and by Lua as the continuation
 * when the call yielded, so both stay yieldable in a coroutine. extra is how many values
 * lie below the results. */
static int finish_pcall(lua_State *L, int status, lua_KContext extra) {
    apps_vm_t *vm = vm_of(L);
    if (status == LUA_ERRMEM) {
        kill_for_memory(vm);
    }
    if (kill_reason(vm) != NULL) {
        return raise_kill(L);
    }
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);  /* the error */
        return 2;
    }
    return lua_gettop(L) - (int)extra;
}

static int sandbox_pcall(lua_State *L) {
    apps_vm_check(L);
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_pcall);
    return finish_pcall(L, status, 0);
}

/* xpcall's message handler, around the app's (upvalue 1). A kill goes through without the
 * app's handler seeing it: when the hook raised the kill, the handler would run with hooks
 * off, and it could return something else. */
static int guarded_handler(lua_State *L) {
    if (kill_reason(vm_of(L)) != NULL) {
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

/* xpcall(f, msgh, ...): Lua's, with msgh behind guarded_handler. The stack becomes
 * <f, handler, true, f, args...>, and the results start after the first two. */
static int sandbox_xpcall(lua_State *L) {
    apps_vm_check(L);
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, guarded_handler, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
    return finish_pcall(L, status, 2);
}

static lua_State *get_co(lua_State *L) {
    lua_State *co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");
    return co;
}

/* Resumes co with the narg values on top of L: lcorolib's auxresume, plus the kill, raised
 * in L once co stops. Returns how many results it moved to L, or -1 with the error on top. */
static int resume_co(lua_State *L, lua_State *co, int narg) {
    if (!lua_checkstack(co, narg)) {
        lua_pushliteral(L, "too many arguments to resume");
        return -1;
    }
    lua_xmove(L, co, narg);
    int nres;
    int status = lua_resume(co, L, narg, &nres);
    apps_vm_t *vm = vm_of(L);
    if (status == LUA_ERRMEM) {
        kill_for_memory(vm);
    }
    if (kill_reason(vm) != NULL) {
        return raise_kill(L);
    }
    if (status == LUA_OK || status == LUA_YIELD) {
        if (!lua_checkstack(L, nres + 1)) {
            lua_pop(co, nres);
            lua_pushliteral(L, "too many results to resume");
            return -1;
        }
        lua_xmove(co, L, nres);
        return nres;
    }
    lua_xmove(co, L, 1);
    return -1;
}

static int sandbox_resume(lua_State *L) {
    apps_vm_check(L);
    lua_State *co = get_co(L);
    int r = resume_co(L, co, lua_gettop(L) - 1);
    if (r < 0) {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2;  /* false, the error */
    }
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* true, the results */
}

/* The function coroutine.wrap returns (the coroutine is upvalue 1): lcorolib's auxwrap.
 * A coroutine that died of a kill is never closed, which would run its __close handlers:
 * resume_co raises first. That matters, since a coroutine the hook killed keeps hooks off. */
static int wrap_call(lua_State *L) {
    apps_vm_check(L);
    lua_State *co = lua_tothread(L, lua_upvalueindex(1));
    int r = resume_co(L, co, lua_gettop(L));
    if (r >= 0) {
        return r;
    }
    int status = lua_status(co);
    if (status != LUA_OK && status != LUA_YIELD) {  /* an error in the coroutine */
        status = lua_closethread(co, L);            /* runs its __close handlers */
        if (status == LUA_ERRMEM) {
            kill_for_memory(vm_of(L));
        }
        if (kill_reason(vm_of(L)) != NULL) {
            return raise_kill(L);
        }
        lua_xmove(co, L, 1);
    }
    if (lua_type(L, -1) == LUA_TSTRING) {  /* never a memory error: that killed */
        luaL_where(L, 1);
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    return lua_error(L);
}

static int sandbox_wrap(lua_State *L) {
    apps_vm_check(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State *co = lua_newthread(L);  /* with the hook, as every coroutine */
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    lua_pushcclosure(L, wrap_call, 1);
    return 1;
}

enum { CO_RUNNING, CO_DEAD, CO_SUSPENDED, CO_NORMAL };
static const char *const co_status_names[] = {"running", "dead", "suspended", "normal"};

/* lcorolib's auxstatus: co as L sees it. */
static int co_status(lua_State *L, lua_State *co) {
    if (L == co) {
        return CO_RUNNING;
    }
    switch (lua_status(co)) {
        case LUA_YIELD:
            return CO_SUSPENDED;
        case LUA_OK: {
            lua_Debug ar;
            if (lua_getstack(co, 0, &ar)) {
                return CO_NORMAL;  /* it has frames: it resumed another */
            }
            return lua_gettop(co) == 0 ? CO_DEAD : CO_SUSPENDED;
        }
        default:
            return CO_DEAD;  /* an error ended it */
    }
}

/* coroutine.close(co): closes a suspended or dead coroutine, running its __close
 * handlers. A dying VM refuses before any of them runs (apps_vm_check). */
static int sandbox_close(lua_State *L) {
    apps_vm_check(L);
    lua_State *co = get_co(L);
    int status = co_status(L, co);
    if (status != CO_DEAD && status != CO_SUSPENDED) {
        return luaL_error(L, "cannot close a %s coroutine", co_status_names[status]);
    }
    status = lua_closethread(co, L);
    if (status == LUA_ERRMEM) {
        kill_for_memory(vm_of(L));
    }
    if (kill_reason(vm_of(L)) != NULL) {
        return raise_kill(L);
    }
    if (status == LUA_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_xmove(co, L, 1);  /* the error */
    return 2;
}

/* ---- The sandbox ---- */

/* Opens the sandbox in L's globals table. Runs under lua_pcall: it allocates, and an
 * error outside a protected call would abort. */
static int open_sandbox(lua_State *L) {
    static const struct {
        const char *name;
        lua_CFunction open;
    } libs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < ARRAY_LEN(libs); i++) {
        luaL_requiref(L, libs[i].name, libs[i].open, 1);
        lua_pop(L, 1);
    }

    lua_pushglobaltable(L);
    /* Code from anywhere but the app's own files, the collector and warnings stay out;
     * print and require belong to the app runtime, which installs its own. */
    static const char *const removed[] = {"dofile", "loadfile", "load", "collectgarbage", "warn", "require", "print"};
    for (size_t i = 0; i < ARRAY_LEN(removed); i++) {
        lua_pushnil(L);
        lua_setfield(L, -2, removed[i]);
    }
    lua_pushcfunction(L, sandbox_setmetatable);
    lua_setfield(L, -2, "setmetatable");
    /* pcall, xpcall and coroutine's resume, wrap and close that can't keep a kill. */
    lua_pushcfunction(L, sandbox_pcall);
    lua_setfield(L, -2, "pcall");
    lua_pushcfunction(L, sandbox_xpcall);
    lua_setfield(L, -2, "xpcall");
    lua_pushliteral(L, LUA_COLIBNAME);
    lua_rawget(L, -2);
    lua_pushcfunction(L, sandbox_resume);
    lua_setfield(L, -2, "resume");
    lua_pushcfunction(L, sandbox_wrap);
    lua_setfield(L, -2, "wrap");
    lua_pushcfunction(L, sandbox_close);
    lua_setfield(L, -2, "close");
    lua_pop(L, 1);

    /* The string library, also the __index of every string's metatable, so the changes
     * reach s:find() as well as string.find(). */
    lua_pushliteral(L, LUA_STRLIBNAME);
    lua_rawget(L, -2);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pushcfunction(L, sandbox_find);
    lua_setfield(L, -2, "find");
    static const char *const matchers[] = {"match", "gmatch", "gsub"};
    for (size_t i = 0; i < ARRAY_LEN(matchers); i++) {
        lua_pushstring(L, matchers[i]);
        lua_pushcclosure(L, sandbox_no_patterns, 1);
        lua_setfield(L, -2, matchers[i]);
    }
    wrap_lua_own(L, "rep", sandbox_rep);
    luaL_setfuncs(L, apps_string_funcs, 0);
    lua_pop(L, 1);

    /* The table functions that can walk a range or a __len with nothing behind it. */
    lua_pushliteral(L, LUA_TABLIBNAME);
    lua_rawget(L, -2);
    wrap_lua_own(L, "move", sandbox_move);
    wrap_lua_own(L, "insert", sandbox_insert);
    wrap_lua_own(L, "remove", sandbox_remove);
    wrap_lua_own(L, "sort", sandbox_sort);
    wrap_lua_own(L, "concat", sandbox_concat);
    lua_pop(L, 1);

    /* Strict globals: __index raises. No __newindex, so assigning defines. */
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, sandbox_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
    return 0;
}

/* ---- The API ---- */

apps_vm_t *apps_vm_new(const apps_vm_config_t *cfg) {
    if (cfg == NULL || cfg->realloc_fn == NULL || cfg->free_fn == NULL || cfg->now_ms == NULL) {
        return NULL;
    }
    apps_vm_t *vm = cfg->realloc_fn(NULL, sizeof *vm);
    if (vm == NULL) {
        return NULL;
    }
    *vm = (apps_vm_t){.cfg = *cfg};
    atomic_init(&vm->kill_reason, NULL);
    atomic_init(&vm->in_call, false);
    atomic_init(&vm->call_started_ms, 0);
    vm->L = lua_newstate(vm_alloc, vm);
    if (vm->L == NULL) {
        cfg->free_fn(vm);
        return NULL;
    }
    lua_atpanic(vm->L, vm_panic);
    lua_pushcfunction(vm->L, open_sandbox);
    if (lua_pcall(vm->L, 0, 0, 0) != LUA_OK) {
        lua_close(vm->L);
        cfg->free_fn(vm);
        return NULL;
    }
    /* Set once, on the main thread, before any coroutine exists: lua_newthread copies it.
     * Never set again: lua_sethook isn't safe from another task (ruling A15). */
    lua_sethook(vm->L, vm_hook, LUA_MASKCOUNT, HOOK_EVERY);
    return vm;
}

void apps_vm_free(apps_vm_t *vm) {
    if (vm != NULL) {
        lua_close(vm->L);
        vm->cfg.free_fn(vm);
    }
}

lua_State *apps_vm_lua(apps_vm_t *vm) {
    return vm->L;
}

bool apps_vm_load(apps_vm_t *vm, const char *chunkname, const char *src, size_t len, char *err, size_t err_len) {
    if (len >= 3 && memcmp(src, "\xEF\xBB\xBF", 3) == 0) {
        src += 3;
        len -= 3;
    }
    /* Lua takes any chunk that starts with ESC for bytecode, which it doesn't verify. */
    if (len > 0 && src[0] == LUA_SIGNATURE[0]) {
        copy_message(err, err_len, "binary chunks are not allowed: install Lua source text");
        return false;
    }
    /* '=' and the name as given (X10). Lua shows at most LUA_IDSIZE - 1 bytes of it, so a
     * longer name reads the same cut here. */
    char name[LUA_IDSIZE + 1] = "=";
    size_t name_len = chunkname != NULL ? strlen(chunkname) : 0;
    if (name_len > sizeof name - 2) {
        name_len = sizeof name - 2;
    }
    if (name_len > 0) {
        memcpy(name + 1, chunkname, name_len);
    }
    name[1 + name_len] = '\0';

    lua_State *L = vm->L;
    if (!lua_checkstack(L, 1)) {
        copy_message(err, err_len, "not enough memory");
        return false;
    }
    if (luaL_loadbufferx(L, src, len, name, "t") != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        copy_message(err, err_len, msg != NULL ? msg : "error loading the chunk");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

size_t apps_vm_mem_used(const apps_vm_t *vm) {
    return vm->mem_used;
}

size_t apps_vm_mem_peak(const apps_vm_t *vm) {
    return vm->mem_peak;
}

size_t apps_vm_mem_refusals(const apps_vm_t *vm) {
    return vm->mem_refusals;
}

apps_call_t apps_vm_call(apps_vm_t *vm, int nargs, int nresults, uint32_t budget, char *err, size_t err_len) {
    lua_State *L = vm->L;
    int fn = lua_gettop(L) - nargs;  /* the function's index */
    if (nargs < 0 || fn < 1) {
        copy_message(err, err_len, "apps_vm_call: no function to call");
        return APPS_CALL_ERROR;
    }
    if (kill_reason(vm) == NULL && !lua_checkstack(L, 1)) {
        kill_for_memory(vm);  /* no room for the message handler */
    }
    const char *reason = kill_reason(vm);
    if (reason != NULL) {
        lua_settop(L, fn - 1);
        copy_message(err, err_len, reason);
        return kill_status(vm, reason);
    }

    lua_pushcfunction(L, traceback_handler);
    lua_insert(L, fn);
    bool outer = vm->depth == 0;
    if (outer) {
        vm->budget = budget;
        vm->call_instructions = 0;
        vm->started_ms = vm->cfg.now_ms();
        atomic_store(&vm->call_started_ms, vm->started_ms);
        atomic_store(&vm->in_call, true);
    }
    vm->depth++;
    int status = lua_pcall(L, nargs, nresults, fn);
    vm->depth--;
    if (outer) {
        atomic_store(&vm->in_call, false);
    }
    if (status == LUA_ERRMEM) {
        kill_for_memory(vm);
    }

    /* A kill wins, even over a call that returned before the hook saw it. */
    reason = kill_reason(vm);
    if (status == LUA_OK && reason == NULL) {
        lua_remove(L, fn);
        copy_message(err, err_len, "");
        return APPS_CALL_OK;
    }
    apps_call_t result = APPS_CALL_ERROR;
    if (reason != NULL) {
        copy_message(err, err_len, reason);
        result = kill_status(vm, reason);
    } else {
        put_error(L, err, err_len);
    }
    lua_settop(L, fn - 1);
    return result;
}

void apps_vm_kill(apps_vm_t *vm, const char *reason) {
    if (vm != NULL) {
        set_kill(vm, reason != NULL ? reason : "stopped");
    }
}

bool apps_vm_dying(const apps_vm_t *vm) {
    return kill_reason(vm) != NULL;
}

void apps_vm_check(lua_State *L) {
    if (kill_reason(vm_of(L)) != NULL) {
        raise_kill_here(L);
    }
}

bool apps_vm_in_call(const apps_vm_t *vm) {
    return atomic_load(&vm->in_call);
}

int64_t apps_vm_call_started_ms(const apps_vm_t *vm) {
    return atomic_load(&vm->call_started_ms);
}

uint64_t apps_vm_instructions(const apps_vm_t *vm) {
    return vm->instructions;
}

/* ---- Excerpt ---- */

/* The line in the first ":<digits>:" of msg, or 0. */
static int message_line(const char *msg) {
    for (const char *colon = strchr(msg, ':'); colon != NULL; colon = strchr(colon + 1, ':')) {
        const char *d = colon + 1;
        long line = 0;
        while (*d >= '0' && *d <= '9' && line < 100000000) {
            line = line * 10 + (*d++ - '0');
        }
        if (d > colon + 1 && *d == ':' && line > 0) {
            return (int)line;
        }
    }
    return 0;
}

static int decimal_digits(int v) {
    int digits = 1;
    for (; v >= 10; v /= 10) {
        digits++;
    }
    return digits;
}

#define EXCERPT_LINE_MAX 80

size_t apps_vm_excerpt(const char *msg, const char *src, size_t len, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    text_t t = {out, cap, 0, false};
    out[0] = '\0';
    if (msg == NULL) {
        msg = "";
    }
    text_put(&t, msg, strlen(msg));
    int line = message_line(msg);
    if (line == 0 || src == NULL) {
        return t.len;
    }
    if (len >= 3 && memcmp(src, "\xEF\xBB\xBF", 3) == 0) {
        src += 3;
        len -= 3;
    }

    /* Lines line - 1, line and line + 1, ended where Lua's lexer ends them. */
    struct {
        const char *text;
        size_t len;
        bool shown;
    } rows[3] = {{NULL, 0, false}, {NULL, 0, false}, {NULL, 0, false}};
    const char *p = src, *end = src + len;
    for (int at = 1; at <= line + 1; at++) {
        const char *eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') {
            eol++;
        }
        bool last = eol == end;
        /* The empty line after a final newline shows only as the error's own line. */
        if (at >= line - 1 && !(last && eol == p && at == line + 1)) {
            rows[at - (line - 1)].text = p;
            rows[at - (line - 1)].len = (size_t)(eol - p);
            rows[at - (line - 1)].shown = true;
        }
        if (last) {
            break;
        }
        char newline = *eol++;
        if (eol < end && (*eol == '\n' || *eol == '\r') && *eol != newline) {
            eol++;  /* \r\n or \n\r: one line break */
        }
        p = eol;
    }
    if (!rows[1].shown) {
        return t.len;  /* src has no such line */
    }

    int width = decimal_digits(rows[2].shown ? line + 1 : line);
    for (int i = 0; i < 3; i++) {
        if (!rows[i].shown) {
            continue;
        }
        char number[12];
        int digits = snprintf(number, sizeof number, "%d", line - 1 + i);
        text_put(&t, i == 1 ? "\n>" : "\n ", 2);
        for (int pad = digits; pad < width; pad++) {
            text_put(&t, " ", 1);
        }
        text_put(&t, number, (size_t)digits);
        text_put(&t, "|", 1);
        if (rows[i].len > 0) {
            size_t keep = rows[i].len > EXCERPT_LINE_MAX ? utf8_cut(rows[i].text, EXCERPT_LINE_MAX) : rows[i].len;
            text_put(&t, " ", 1);
            text_put(&t, rows[i].text, keep);
        }
    }
    return t.len;
}
