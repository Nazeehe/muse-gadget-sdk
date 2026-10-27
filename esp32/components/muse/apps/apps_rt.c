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
 * One running Muse app (apps_rt.h). Every way into the app's Lua is one apps_vm_call, and everything that
 * allocates in the app's heap runs inside it, in a C function the call runs (a trampoline): a refused
 * allocation is a raised error there, which the call turns into the kill, and outside a protected call it
 * would abort. What runs outside is plain C, and the two pushes that start a call (a C function and a light
 * userdata, which allocate nothing).
 *
 * Stack, for the device's 56 KB app task. What this file puts under app code, by -fstack-usage on the S3
 * build (-O2): print 272 bytes, require 384, and 656 more for its not-found message (a luaL_Buffer, at the end
 * of the chain), the unknown-handler warning 352 (once, while main.lua loads). apps_rt_new (720) and
 * apps_rt_message (192) sit at the top, outside any app call. require compiles on the thread it was called on
 * (apps_vm_load_on), so the parser's recursion, which is C, counts against the C-call room of the code that
 * asked (a coroutine's included), and Lua's C-call limit bounds it like every other recursion under app code.
 * What is measured, beside the sandbox's own chains (test_muse_apps_rt.py, c_stack): the recursions through print
 * and through require, each to that limit, and a chain through a luaL_Buffer that requires a module nested
 * past the limit at each level, on the main thread and in a coroutine.
 */

#include "apps_rt.h"

#include <stdio.h>
#include <string.h>

#include "apps_json.h"
#include "apps_port.h"
#include "apps_suggest.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

#define NOINLINE __attribute__((noinline))

/* The handlers, in the order the did-you-mean suggests from. */
enum { H_START, H_STOP, H_SHOW, H_HIDE, H_MESSAGE, H_ALARM, H_COUNT };
static const char *const handler_names[H_COUNT] = {"start", "stop", "show", "hide", "message", "alarm"};

/* What print charges the call's budget, in VM instructions: per line it logs (the work after it, the port's
 * console write, is the platform's, and the wall clock is read at each), and per 16 bytes it scans. So a
 * call logs at most 20000 lines of START's 5000000 (8000 of CALL's 2000000). */
#define LINE_COST 250
#define SCAN_BYTES_PER_COST 16

/* A key or a name shown in a message is cut here (a path's longest), and a module's compile message here. */
#define SHOWN_MAX APPS_PATH_MAX
#define COMPILE_MESSAGE_MAX 320

/* The not-found message lists the manifest's files in at most this many bytes. */
#define FILE_LIST_MAX 480

#define NOT_RUNNING "the app is not running"

struct apps_rt {
    const apps_pkg_t *pkg;
    apps_vm_t *vm;
    apps_state_t state;
    int handler[H_COUNT];  /* registry references; LUA_NOREF for a handler the app hasn't */
    /* apps_vm_call writes its error here. It is the crash record while the state is CRASHED (and nothing runs
     * after that), and the scratch of a call that fails the message alone otherwise. */
    char err[APPS_ERROR_MAX];
    char warnings[APPS_WARNINGS_MAX];
    /* The log: lines, each its length (a byte) and its text, in a ring of bytes. */
    size_t ring_head, ring_used;
    unsigned ring_lines;
    uint8_t ring[APPS_LOG_RING_BYTES];
};

/* ---- Text that leaves the runtime ----
 *
 * What an app prints and raises is the app's own bytes, and it reaches the serial console and Muse inside
 * JSON. So a text this module hands out is valid UTF-8 (RFC 3629) with no control byte but, where a caller
 * says so, "\n" and tab: anything else is one '?', for each byte or, for a C1 control character
 * (U+0080 to U+009F, which a terminal can take for an escape sequence: U+009B is CSI), for the character. */

/* The character at p (end is the end of the text): the bytes it takes, at least one. *keep is whether it
 * passes as it is, or becomes one '?'. */
static size_t next_char(const unsigned char *p, const unsigned char *end, bool keep_newline, bool *keep) {
    unsigned char c = *p;
    if (c < 0x80) {
        *keep = (c >= 0x20 && c != 0x7F) || c == '\t' || (keep_newline && c == '\n');
        return 1;
    }
    int n = apps_json_utf8_len(p, end);
    if (n <= 0) {  /* not UTF-8, or cut short by the end: this byte alone */
        *keep = false;
        return 1;
    }
    *keep = !(n == 2 && p[0] == 0xC2 && p[1] <= 0x9F);
    return (size_t)n;
}

/* s, a C string, made clean in place (the text only shrinks or stays the same size). */
static void clean_text(char *s, bool keep_newline) {
    unsigned char *r = (unsigned char *)s, *w = r;
    const unsigned char *end = r + strlen(s);
    while (r < end) {
        bool keep;
        size_t n = next_char(r, end, keep_newline, &keep);
        if (keep) {
            memmove(w, r, n);
            w += n;
        } else {
            *w++ = '?';
        }
        r += n;
    }
    *w = '\0';
}

/* text with its traceback dropped: what follows the first "\nstack traceback:" (the message alone, as a
 * handler's error() text is given). */
static void cut_traceback(char *text) {
    char *traceback = strstr(text, "\nstack traceback:");
    if (traceback != NULL) {
        *traceback = '\0';
    }
}

/* text into out (cap bytes with the NUL), cut on a character boundary. NULL out or no room: nothing. */
static void put_text(char *out, size_t cap, const char *text) {
    if (out == NULL || cap == 0) {
        return;
    }
    size_t n = strlen(text);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;  /* text[n] is the first byte dropped: not in the middle of a character */
        }
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

/* s[0, len) cleaned (no newline kept) into out, at most max bytes of it and "..." when there was more. out
 * needs max + 4 bytes. Reads len bytes: s may hold NULs. */
static void echo(char *out, const char *s, size_t len, size_t max) {
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    size_t used = 0;
    while (p < end) {
        bool keep;
        size_t n = next_char(p, end, false, &keep);
        size_t take = keep ? n : 1;
        if (used + take > max) {
            memcpy(out + used, "...", 3);
            used += 3;
            break;
        }
        if (keep) {
            memcpy(out + used, p, n);
        } else {
            out[used] = '?';
        }
        used += take;
        p += n;
    }
    out[used] = '\0';
}

/* ---- The log ---- */

static void ring_write(apps_rt_t *rt, size_t at, const char *src, size_t n) {
    size_t first = APPS_LOG_RING_BYTES - at;
    if (first > n) {
        first = n;
    }
    memcpy(rt->ring + at, src, first);
    memcpy(rt->ring, src + first, n - first);
}

static void ring_read(const apps_rt_t *rt, size_t at, char *dst, size_t n) {
    size_t first = APPS_LOG_RING_BYTES - at;
    if (first > n) {
        first = n;
    }
    memcpy(dst, rt->ring + at, first);
    memcpy(dst + first, rt->ring, n - first);
}

static uint8_t ring_byte(const apps_rt_t *rt, size_t at) {
    return rt->ring[at % APPS_LOG_RING_BYTES];
}

/* Where the line after the one at `at` starts. */
static size_t ring_next(const apps_rt_t *rt, size_t at) {
    return (at + rt->ring[at] + 1u) % APPS_LOG_RING_BYTES;
}

/* Keeps one line (at most APPS_LOG_LINE_MAX bytes, so its length is a byte), dropping the oldest lines until
 * it fits: the ring holds APPS_LOG_RING_BYTES, a line taking its length plus one. */
static void ring_push(apps_rt_t *rt, const char *line, size_t len) {
    size_t cost = len + 1;
    while (rt->ring_used + cost > APPS_LOG_RING_BYTES) {
        rt->ring_used -= rt->ring[rt->ring_head] + 1u;
        rt->ring_head = ring_next(rt, rt->ring_head);
        rt->ring_lines--;
    }
    size_t at = (rt->ring_head + rt->ring_used) % APPS_LOG_RING_BYTES;
    rt->ring[at] = (uint8_t)len;
    ring_write(rt, (at + 1) % APPS_LOG_RING_BYTES, line, len);
    rt->ring_used += cost;
    rt->ring_lines++;
}

size_t apps_rt_log_tail(const apps_rt_t *rt, int lines, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (rt == NULL || lines <= 0 || rt->ring_lines == 0) {
        return 0;
    }
    /* The last `want` lines, from `at`; they joined are the sum of their lengths and a separator between. */
    size_t want = (size_t)lines < rt->ring_lines ? (size_t)lines : rt->ring_lines;
    size_t at = rt->ring_head;
    for (size_t skip = rt->ring_lines - want; skip > 0; skip--) {
        at = ring_next(rt, at);
    }
    size_t total = want - 1;
    for (size_t i = 0, p = at; i < want; i++, p = ring_next(rt, p)) {
        total += rt->ring[p];
    }
    /* What doesn't fit goes oldest first, a whole line at a time. */
    size_t room = cap - 1;
    while (total > room && want > 1) {
        total -= rt->ring[at] + 1u;
        at = ring_next(rt, at);
        want--;
    }
    size_t written = 0;
    for (size_t i = 0; i < want; i++, at = ring_next(rt, at)) {
        size_t n = rt->ring[at];
        if (i > 0) {
            out[written++] = '\n';
        }
        if (n > room - written) {  /* the newest line alone is too long: cut on a character boundary */
            n = room - written;
            while (n > 0 && (ring_byte(rt, at + 1 + n) & 0xC0) == 0x80) {
                n--;  /* the first byte dropped is the middle of a character */
            }
        }
        ring_read(rt, (at + 1) % APPS_LOG_RING_BYTES, out + written, n);
        written += n;
    }
    out[written] = '\0';
    return written;
}

/* ---- print ---- */

/* The line being built: text up to APPS_LOG_LINE_MAX bytes, already clean. */
typedef struct {
    apps_rt_t *rt;
    lua_State *L;
    size_t len;
    bool full;  /* it reached the cut: the rest of the line is dropped */
    char text[APPS_LOG_LINE_MAX + 1];
} line_t;

/* Ends the line: it costs the budget, goes into the ring and out through the port. */
static void line_end(line_t *ln) {
    apps_vm_charge(ln->L, LINE_COST);
    ln->text[ln->len] = '\0';
    ring_push(ln->rt, ln->text, ln->len);
    apps_port_log(ln->rt->pkg->manifest.id, ln->text);
    ln->len = 0;
    ln->full = false;
}

/* Adds s[0, n), which has no newline, to the line, cleaned, up to the cut (a character is never split). */
static void line_add(line_t *ln, const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end && !ln->full) {
        bool keep;
        size_t k = next_char(p, end, false, &keep);
        size_t take = keep ? k : 1;
        if (ln->len + take > APPS_LOG_LINE_MAX) {
            ln->full = true;
            break;
        }
        if (keep) {
            memcpy(ln->text + ln->len, p, k);
        } else {
            ln->text[ln->len] = '?';
        }
        ln->len += take;
        p += k;
    }
}

/* Adds s[0, n), ending a line at each "\n". */
static void line_put(line_t *ln, const char *s, size_t n) {
    while (n > 0) {
        const char *nl = memchr(s, '\n', n);
        size_t run = nl != NULL ? (size_t)(nl - s) : n;
        line_add(ln, s, run);
        if (nl == NULL) {
            return;
        }
        line_end(ln);
        s += run + 1;
        n -= run + 1;
    }
}

/* print(...): the tostrings of the arguments, joined with tabs, as lines of the app's log. A piece is read in
 * place, never joined into a string of the Lua heap: a print of a big string costs the heap nothing. */
static int rt_print(lua_State *L) {
    apps_vm_check(L);
    line_t ln = {.rt = lua_touserdata(L, lua_upvalueindex(1)), .L = L};
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) {
            line_add(&ln, "\t", 1);
        }
        line_put(&ln, s, len);
        apps_vm_charge(L, (unsigned)(len / SCAN_BYTES_PER_COST));
        lua_pop(L, 1);
    }
    line_end(&ln);
    return 0;
}

/* ---- require ---- */

/* The Lua file name resolves to: a file of the manifest's "files" (so not main), which has bytes in the
 * package; NULL when there is none. name is the path with or without ".lua". */
static const apps_blob_t *find_module(const apps_rt_t *rt, const char *name, size_t len) {
    const apps_pkg_t *pkg = rt->pkg;
    bool has_suffix = len >= 4 && memcmp(name + len - 4, ".lua", 4) == 0;
    size_t total = has_suffix ? len : len + 4;
    if (total > APPS_PATH_MAX || memchr(name, '\0', len) != NULL) {
        return NULL;
    }
    char path[APPS_PATH_MAX + 1];
    memcpy(path, name, len);
    memcpy(path + len, ".lua", has_suffix ? 0 : 4);
    path[total] = '\0';
    if (strcmp(path, pkg->manifest.main) == 0) {
        return NULL;
    }
    bool listed = false;
    for (int i = 0; i < pkg->manifest.file_count && !listed; i++) {
        listed = strcmp(pkg->manifest.files[i], path) == 0;
    }
    for (int i = 0; listed && i < pkg->count; i++) {
        if (strcmp(pkg->files[i].path, path) == 0) {
            return &pkg->files[i];
        }
    }
    return NULL;
}

/* Raises that name isn't one of the app's files, listing them. A leaf: its luaL_Buffer is big. */
static NOINLINE int require_not_found(lua_State *L, const apps_rt_t *rt, const char *name, size_t len) {
    const apps_manifest_t *m = &rt->pkg->manifest;
    char shown[SHOWN_MAX + 4];
    echo(shown, name, len, SHOWN_MAX);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addchar(&b, '[');
    size_t used = 1;
    bool first = true;
    for (int i = 0; i < m->file_count; i++) {
        if (strcmp(m->files[i], m->main) == 0) {
            continue;
        }
        size_t cost = strlen(m->files[i]) + 2 + (first ? 0 : 1);  /* its quotes, and the comma before it */
        if (used + cost + sizeof ",...]" - 1 > FILE_LIST_MAX) {
            luaL_addstring(&b, ",...");
            break;
        }
        if (!first) {
            luaL_addchar(&b, ',');
        }
        luaL_addchar(&b, '"');
        luaL_addstring(&b, m->files[i]);
        luaL_addchar(&b, '"');
        used += cost;
        first = false;
    }
    luaL_addchar(&b, ']');
    luaL_pushresult(&b);
    return luaL_error(L, "require: '%s' is not one of this app's files (manifest.json \"files\": %s)", shown,
                      lua_tostring(L, -1));
}

/* require(name), upvalues: the runtime, and the table of what was loaded (by path). */
static int rt_require(lua_State *L) {
    apps_vm_check(L);
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    const apps_blob_t *file = find_module(rt, name, len);
    if (file == NULL) {
        return require_not_found(L, rt, name, len);
    }
    lua_getfield(L, lua_upvalueindex(2), file->path);
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);

    /* Not loaded yet. Compiling is C work no count hook sees, so it is charged by the size of the source:
     * a require that fails over and over can't hold a call for long. It compiles on this thread, so the
     * parser's recursion counts against the C-call room of the code that asked, in a coroutine too: on the
     * VM's main thread it would add its levels to a count that leaves out the coroutine's own. */
    apps_vm_charge(L, (unsigned)file->len);
    char message[COMPILE_MESSAGE_MAX];
    if (!apps_vm_load_on(L, file->path, (const char *)file->data, file->len, message, sizeof message)) {
        cut_traceback(message);  /* the parser's own runtime error ("C stack overflow") arrives with the call's */
        clean_text(message, false);
        return luaL_error(L, "require: %s", message);
    }
    lua_pushvalue(L, 1);
    lua_pushstring(L, file->path);
    lua_call(L, 2, 1);
    if (lua_isnil(L, -1)) {  /* a module that returns nothing is loaded all the same */
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, lua_upvalueindex(2), file->path);
    return 1;
}

/* ---- Calling into the app ---- */

/* Runs fn(arg) as one apps_vm_call with the budget: fn is a C function that gets arg as a light userdata and
 * does everything that touches the app's heap, so that a refusal there is an error inside the call. The
 * result is apps_vm_call's; a call that can't start for want of stack room is an error too. */
static apps_call_t vm_run(apps_rt_t *rt, lua_CFunction fn, void *arg, uint32_t budget) {
    lua_State *L = apps_vm_lua(rt->vm);
    if (!lua_checkstack(L, 2)) {
        snprintf(rt->err, sizeof rt->err, "out of memory: no room on the Lua stack");
        return APPS_CALL_ERROR;
    }
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, arg);
    return apps_vm_call(rt->vm, 1, 0, budget, rt->err, sizeof rt->err);
}

/* A call of the app failed: it has crashed, and rt->err (what apps_vm_call wrote) is the record. */
static void crash(apps_rt_t *rt) {
    clean_text(rt->err, true);
    rt->state = APPS_STATE_CRASHED;
}

/* ---- Loading main.lua ---- */

static void warn_add(apps_rt_t *rt, const char *text) {
    size_t have = strlen(rt->warnings), n = strlen(text);
    size_t sep = have > 0 ? 2 : 0;
    if (have + sep + n >= sizeof rt->warnings) {
        return;  /* whole or not at all */
    }
    memcpy(rt->warnings + have, "; ", sep);
    memcpy(rt->warnings + have + sep, text, n + 1);
}

/* The warning for the key at idx of main's table, which isn't a handler. */
static void warn_unknown(apps_rt_t *rt, lua_State *L, int idx) {
    char shown[SHOWN_MAX + 4];
    const char *hint = NULL;
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            size_t len;
            const char *key = lua_tolstring(L, idx, &len);
            echo(shown, key, len, SHOWN_MAX);
            if (strlen(key) == len) {  /* a key with a NUL in it isn't a near miss of anything */
                hint = apps_suggest(key, handler_names, H_COUNT);
            }
            break;
        }
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                snprintf(shown, sizeof shown, "%lld", (long long)lua_tointeger(L, idx));
            } else {
                snprintf(shown, sizeof shown, "%.14g", (double)lua_tonumber(L, idx));
            }
            break;
        case LUA_TBOOLEAN: snprintf(shown, sizeof shown, "%s", lua_toboolean(L, idx) ? "true" : "false"); break;
        default: snprintf(shown, sizeof shown, "<%s>", luaL_typename(L, idx)); break;
    }
    char text[APPS_PATH_MAX + SHOWN_MAX + 96];
    int n = snprintf(text, sizeof text, "%s returned unknown handler '%s'", rt->pkg->manifest.main, shown);
    if (hint != NULL) {
        snprintf(text + n, sizeof text - (size_t)n, " (did you mean '%s'?)", hint);
    }
    warn_add(rt, text);
}

/* How main.lua's load went, when the call itself didn't fail. */
typedef enum {
    BOOT_OK,
    BOOT_NOT_COMPILED,  /* Lua's message is in fail: nothing of main.lua ran */
    BOOT_BAD_TABLE,     /* main.lua ran, and what it returned can't be handlers: the reason is in fail */
} boot_outcome_t;

/* What the boot trampoline reads and fills. */
typedef struct {
    apps_rt_t *rt;
    const apps_blob_t *main;
    boot_outcome_t outcome;
    char fail[512];
} boot_t;

/* The handlers of the table at idx: each known key's function into the registry; every other key a warning. */
static void take_handlers(boot_t *b, lua_State *L, int idx) {
    apps_vm_charge(L, apps_vm_table_slots(L, idx) + 1);  /* a walk over slots no count hook sees */
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {  /* the key at -2, the value at -1 */
        int h = -1;
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t len;
            const char *key = lua_tolstring(L, -2, &len);
            for (int i = 0; i < H_COUNT; i++) {
                if (strlen(handler_names[i]) == len && memcmp(handler_names[i], key, len) == 0) {
                    h = i;
                }
            }
        }
        if (h < 0) {
            warn_unknown(b->rt, L, -2);
        } else if (lua_type(L, -1) != LUA_TFUNCTION) {
            snprintf(b->fail, sizeof b->fail, "%s returned '%s' as a %s; handlers must be functions",
                     b->rt->pkg->manifest.main, handler_names[h], luaL_typename(L, -1));
            b->outcome = BOOT_BAD_TABLE;
            return;
        } else {
            lua_pushvalue(L, -1);
            b->rt->handler[h] = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_pop(L, 1);
    }
}

/* Runs under apps_vm_call: installs print and require, loads main.lua, runs it and takes its handlers. */
static int boot_trampoline(lua_State *L) {
    boot_t *b = lua_touserdata(L, 1);
    apps_rt_t *rt = b->rt;
    lua_settop(L, 0);

    lua_pushlightuserdata(L, rt);
    lua_pushcclosure(L, rt_print, 1);
    lua_setglobal(L, "print");
    lua_pushlightuserdata(L, rt);
    lua_newtable(L);
    lua_pushcclosure(L, rt_require, 2);
    lua_setglobal(L, "require");

    apps_vm_charge(L, (unsigned)b->main->len);  /* compiling, as in require */
    if (!apps_vm_load(rt->vm, b->main->path, (const char *)b->main->data, b->main->len, b->fail, sizeof b->fail)) {
        b->outcome = BOOT_NOT_COMPILED;
        return 0;
    }
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) {
        snprintf(b->fail, sizeof b->fail,
                 "%s must return a table of handlers: local app = {} \xE2\x80\xA6 return app", rt->pkg->manifest.main);
        b->outcome = BOOT_BAD_TABLE;
        return 0;
    }
    take_handlers(b, L, lua_gettop(L));
    return 0;
}

/* The file of pkg at path, or NULL. */
static const apps_blob_t *find_blob(const apps_pkg_t *pkg, const char *path) {
    for (int i = 0; i < pkg->count; i++) {
        if (strcmp(pkg->files[i].path, path) == 0) {
            return &pkg->files[i];
        }
    }
    return NULL;
}

apps_rt_t *apps_rt_new(const apps_pkg_t *pkg, char *err, size_t err_len) {
    put_text(err, err_len, "");
    if (pkg == NULL) {
        put_text(err, err_len, "apps_rt_new: no package");
        return NULL;
    }
    char text[APPS_PATH_MAX + 64];
    const apps_blob_t *main = find_blob(pkg, pkg->manifest.main);
    if (main == NULL || apps_file_kind(main->path) != APPS_FILE_LUA) {
        snprintf(text, sizeof text, main == NULL ? "%s is missing from the package" : "%s is not a .lua file",
                 pkg->manifest.main);
        put_text(err, err_len, text);
        return NULL;
    }
    apps_rt_t *rt = apps_port_realloc(NULL, sizeof *rt);
    if (rt == NULL) {
        put_text(err, err_len, "out of memory: can't make room for the app");
        return NULL;
    }
    memset(rt, 0, sizeof *rt);
    rt->pkg = pkg;
    rt->state = APPS_STATE_STOPPED;
    for (int h = 0; h < H_COUNT; h++) {
        rt->handler[h] = LUA_NOREF;
    }
    apps_vm_config_t cfg = {
        .mem_limit = (size_t)pkg->manifest.memory_kb * 1024u,
        .realloc_fn = apps_port_realloc,
        .free_fn = apps_port_free,
        .now_ms = apps_port_mono_ms,
    };
    rt->vm = apps_vm_new(&cfg);
    if (rt->vm == NULL) {
        snprintf(text, sizeof text, "out of memory: can't make the app's heap of %u KB",
                 (unsigned)pkg->manifest.memory_kb);
        put_text(err, err_len, text);
        apps_port_free(rt);
        return NULL;
    }
    lua_State *L = apps_vm_lua(rt->vm);
    apps_json_open(L);  /* safe outside a call: if the heap refuses, the call below ends out of memory */

    boot_t boot = {.rt = rt, .main = main, .outcome = BOOT_OK};
    apps_call_t result = vm_run(rt, boot_trampoline, &boot, APPS_BUDGET_START);
    if (result == APPS_CALL_OK && boot.outcome == BOOT_NOT_COMPILED) {
        /* Nothing of main.lua ran, so there is no app to keep: Lua's message, which may be the parser's own
         * runtime error and have the call's traceback, as in require. */
        cut_traceback(boot.fail);
        clean_text(boot.fail, true);
        put_text(err, err_len, boot.fail);
        apps_rt_free(rt);
        return NULL;
    }
    if (result == APPS_CALL_OK && boot.outcome == BOOT_BAD_TABLE) {
        snprintf(rt->err, sizeof rt->err, "%s", boot.fail);  /* no Lua error says so: the record is ours */
    }
    if (result != APPS_CALL_OK || boot.outcome != BOOT_OK) {
        /* main.lua's top level ran and failed (an error, a kill, no memory, no table, a handler that isn't a
         * function): the app has crashed before it was made. The runtime is kept for the record and the log. A
         * table that was read part way has given warnings for the keys met so far, which the order of its keys
         * decides: they are dropped. */
        crash(rt);
        rt->warnings[0] = '\0';
    }
    return rt;
}

/* ---- Handlers ---- */

/* One handler call: which handler, and its argument if it has one (alarm's name, a string of the Lua heap,
 * so it is made inside the call). */
typedef struct {
    apps_rt_t *rt;
    int handler;
    const char *arg;  /* NULL: no argument */
} handler_call_t;

static int handler_trampoline(lua_State *L) {
    const handler_call_t *call = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, call->rt->handler[call->handler]);
    if (call->arg != NULL) {
        lua_pushstring(L, call->arg);
    }
    lua_call(L, call->arg != NULL ? 1 : 0, 0);
    return 0;
}

/* Calls handler h, which the app has, with the budget. False when it failed, and then the app has crashed. */
static bool call_handler(apps_rt_t *rt, int h, const char *arg, uint32_t budget) {
    handler_call_t call = {rt, h, arg};
    if (vm_run(rt, handler_trampoline, &call, budget) == APPS_CALL_OK) {
        return true;
    }
    crash(rt);
    return false;
}

bool apps_rt_start(apps_rt_t *rt) {
    if (rt == NULL) {
        return false;
    }
    if (rt->state == APPS_STATE_RUNNING) {
        return true;
    }
    if (rt->state != APPS_STATE_STOPPED) {
        return false;
    }
    if (rt->handler[H_START] != LUA_NOREF && !call_handler(rt, H_START, NULL, APPS_BUDGET_START)) {
        return false;
    }
    rt->state = APPS_STATE_RUNNING;
    return true;
}

void apps_rt_show(apps_rt_t *rt, bool shown) {
    int h = shown ? H_SHOW : H_HIDE;
    if (rt != NULL && rt->state == APPS_STATE_RUNNING && rt->handler[h] != LUA_NOREF) {
        call_handler(rt, h, NULL, APPS_BUDGET_CALL);
    }
}

void apps_rt_stop(apps_rt_t *rt) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return;
    }
    if (rt->handler[H_STOP] == LUA_NOREF || call_handler(rt, H_STOP, NULL, APPS_BUDGET_CALL)) {
        rt->state = APPS_STATE_STOPPED;
    }
}

bool apps_rt_alarm(apps_rt_t *rt, const char *name) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return false;
    }
    return rt->handler[H_ALARM] == LUA_NOREF ||
           call_handler(rt, H_ALARM, name != NULL ? name : "", APPS_BUDGET_CALL);
}

/* ---- Messages ---- */

typedef struct {
    apps_rt_t *rt;
    const char *event;
    const char *data;
    size_t data_len;
    char *reply;     /* the encoded reply, from apps_port_realloc: the caller's once the call is over */
    char note[128];  /* when the message fails without a crash: why ("data: ...", "message: ...") */
} message_t;

/* Runs under apps_vm_call: decodes data, calls message(event, data) and encodes what it returns. Everything
 * that allocates in the app's heap is here, so a heap that refuses is an error raised inside the call. */
static int message_trampoline(lua_State *L) {
    message_t *m = lua_touserdata(L, 1);
    char bare[96];
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->rt->handler[H_MESSAGE]);
    lua_pushstring(L, m->event);
    if (m->data == NULL || m->data_len == 0) {
        lua_pushnil(L);
    } else if (!apps_json_push(L, m->data, m->data_len, bare, sizeof bare)) {
        snprintf(m->note, sizeof m->note, "data: %s", bare);
        return 0;
    }
    lua_call(L, 2, 1);
    /* The block is allocated last, once the text is built, and nothing here raises after it: it is stored at
     * once, and the caller frees it if the call comes back as a failure all the same. */
    m->reply = apps_json_encode_value(L, -1, apps_port_realloc, NULL, bare, sizeof bare);
    if (m->reply == NULL) {
        snprintf(m->note, sizeof m->note, "message: %s", bare);
    }
    return 0;
}

char *apps_rt_message(apps_rt_t *rt, const char *event, const char *data_json, size_t data_len, char *err,
                      size_t err_len) {
    put_text(err, err_len, "");
    if (rt == NULL || (rt->state != APPS_STATE_RUNNING && rt->state != APPS_STATE_CRASHED)) {
        put_text(err, err_len, NOT_RUNNING);
        return NULL;
    }
    if (rt->state == APPS_STATE_CRASHED) {
        put_text(err, err_len, rt->err);
        return NULL;
    }
    if (rt->handler[H_MESSAGE] == LUA_NOREF) {
        char text[APPS_PATH_MAX + 48];
        snprintf(text, sizeof text, "%s has no message(event, data) handler", rt->pkg->manifest.main);
        put_text(err, err_len, text);
        return NULL;
    }
    message_t m = {.rt = rt, .event = event != NULL ? event : "", .data = data_json, .data_len = data_len};
    switch (vm_run(rt, message_trampoline, &m, APPS_BUDGET_CALL)) {
        case APPS_CALL_OK:
            if (m.reply != NULL) {
                return m.reply;
            }
            put_text(err, err_len, m.note);
            return NULL;
        case APPS_CALL_ERROR: {  /* the handler's error() or a runtime error: the message alone fails */
            apps_port_free(m.reply);
            cut_traceback(rt->err);
            clean_text(rt->err, true);
            put_text(err, err_len, rt->err);
            return NULL;
        }
        default:  /* killed, or out of memory: the app is gone */
            apps_port_free(m.reply);
            crash(rt);
            put_text(err, err_len, rt->err);
            return NULL;
    }
}

/* ---- The rest of the API ---- */

void apps_rt_free(apps_rt_t *rt) {
    if (rt != NULL) {
        apps_vm_free(rt->vm);
        apps_port_free(rt);
    }
}

apps_state_t apps_rt_state(const apps_rt_t *rt) {
    return rt != NULL ? rt->state : APPS_STATE_STOPPED;
}

const char *apps_state_name(apps_state_t state) {
    switch (state) {
        case APPS_STATE_STOPPED: return "stopped";
        case APPS_STATE_RUNNING: return "running";
        case APPS_STATE_CRASHED: return "crashed";
        case APPS_STATE_HUNG: return "hung";
    }
    return "?";
}

const char *apps_rt_last_error(const apps_rt_t *rt) {
    return rt != NULL && rt->state == APPS_STATE_CRASHED ? rt->err : "";
}

const char *apps_rt_warnings(const apps_rt_t *rt) {
    return rt != NULL ? rt->warnings : "";
}

apps_vm_t *apps_rt_vm(apps_rt_t *rt) {
    return rt != NULL ? rt->vm : NULL;
}
