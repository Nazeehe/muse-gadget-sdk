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
#include "apps_svc.h"
#include "apps_ui.h"
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

/* The duty cycle (apps_rt.h): a window is a whole second of the monotonic clock, hot over APPS_DUTY_BUSY_PCT of it spent in
 * calls; a throttled app's timers are this much later. */
#define DUTY_WINDOW_MS 1000
#define DUTY_BUSY_MS (DUTY_WINDOW_MS * APPS_DUTY_BUSY_PCT / 100)
#define THROTTLE_DELAY_MS 100
#define STR_(x) #x
#define STR(x) STR_(x)
static const char THROTTLE_WARNING[] = "throttled: using more than 40% of the CPU";
/* A static string: apps_vm_kill keeps the pointer. */
static const char DUTY_KILL_REASON[] = "stopped: used more than 40% of the CPU for " STR(APPS_DUTY_KILL_WINDOWS) " s";

/* An armed timer; gen 0 is a free slot. */
typedef struct {
    int fn_ref;      /* the callback, a registry reference; LUA_NOREF when free */
    int64_t due;     /* monotonic ms */
    int64_t period;  /* ms between runs of an every timer; 0 for a one-shot */
    uint32_t gen;    /* its life: a handle names (slot, gen); never 0 while armed */
} armed_t;

struct apps_rt {
    const apps_pkg_t *pkg;
    apps_vm_t *vm;
    apps_ui_t *ui;
    apps_state_t state;
    int handler[H_COUNT];  /* registry references; LUA_NOREF for a handler the app hasn't */
    armed_t timers[APPS_TIMERS_MAX];
    uint32_t timer_gen;  /* the last gen given */
    /* The duty cycle's books. The window open now is the whole second duty_second of apps_port_mono_ms, and these are what the
     * calls that ran in it have used so far: VM instructions, and milliseconds spent in calls (every call's time and its
     * instructions are booked to the windows it ran in when it ends: duty_book). Then how many hot windows in a row have
     * closed, the share of the last window to close that was spent in calls (the status's cpu_pct), and whether the app is
     * throttled and has been told so. */
    int64_t duty_second;
    uint64_t duty_instr;
    int64_t duty_busy_ms;
    int duty_hot;
    uint8_t cpu_pct;
    bool throttled, throttle_warned;
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
 * JSON. So a text this module hands out is clean (apps_text_clean, apps_pkg.h, which the install's compile
 * message uses too): valid UTF-8 (RFC 3629) with no control byte but, where a caller says so, "\n" and tab. */

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
        size_t n = apps_text_next(p, end, false, &keep);
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
        size_t k = apps_text_next(p, end, false, &keep);
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
        apps_text_clean(message, strlen(message), false);
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

static void duty_roll(apps_rt_t *rt, int64_t now_ms);
static void duty_book(apps_rt_t *rt, int64_t began, int64_t ended, uint64_t instr);
static void timers_disarm(apps_rt_t *rt);

/* Runs fn(arg) as one apps_vm_call with the budget: fn is a C function that gets arg as a light userdata and
 * does everything that touches the app's heap, so that a refusal there is an error inside the call. The
 * result is apps_vm_call's; a call that can't start for want of stack room is an error too. Every way into the app
 * comes through here, so this is where the duty cycle is kept: the windows that have ended are closed before the call, and a
 * kill it decides on is the call's result; the call's time and instructions are booked to the windows it ran in after it. */
static apps_call_t vm_run(apps_rt_t *rt, lua_CFunction fn, void *arg, uint32_t budget) {
    int64_t began = apps_port_mono_ms();
    duty_roll(rt, began);
    lua_State *L = apps_vm_lua(rt->vm);
    if (!lua_checkstack(L, 2)) {
        snprintf(rt->err, sizeof rt->err, "out of memory: no room on the Lua stack");
        return APPS_CALL_ERROR;
    }
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, arg);
    uint64_t before = apps_vm_instructions(rt->vm);
    apps_call_t result = apps_vm_call(rt->vm, 1, 0, budget, rt->err, sizeof rt->err);
    duty_book(rt, began, apps_port_mono_ms(), apps_vm_instructions(rt->vm) - before);
    return result;
}

/* A call of the app failed: it has crashed, and rt->err (what apps_vm_call wrote) is the record. Its timers go. */
static void crash(apps_rt_t *rt) {
    apps_text_clean(rt->err, strlen(rt->err), true);
    rt->state = APPS_STATE_CRASHED;
    timers_disarm(rt);
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

/* The package's images as the table ui.image looks in: each .bin that passes the image check, in the package's
 * order, pointing into the package. */
static int build_assets(const apps_pkg_t *pkg, apps_asset_t *assets) {
    int n = 0;
    for (int i = 0; i < pkg->count && n < APPS_UI_ASSETS_MAX; i++) {
        const apps_blob_t *file = &pkg->files[i];
        uint16_t w, h;
        if (apps_file_kind(file->path) == APPS_FILE_IMAGE && apps_image_check(file->data, file->len, &w, &h, NULL, 0)) {
            assets[n++] = (apps_asset_t){.path = file->path, .data = file->data, .len = file->len, .w = w, .h = h};
        }
    }
    return n;
}

/* A VM for an app: memory_kb of heap from the port. NULL, with err saying so, when it can't be made. */
static apps_vm_t *make_vm(unsigned memory_kb, char *err, size_t err_len) {
    apps_vm_config_t cfg = {
        .mem_limit = (size_t)memory_kb * 1024u,
        .realloc_fn = apps_port_realloc,
        .free_fn = apps_port_free,
        .now_ms = apps_port_mono_ms,
    };
    apps_vm_t *vm = apps_vm_new(&cfg);
    if (vm == NULL) {
        char text[64];
        snprintf(text, sizeof text, "out of memory: can't make the app's heap of %u KB", memory_kb);
        put_text(err, err_len, text);
    }
    return vm;
}

bool apps_rt_compile(const apps_code_t *code, int n, unsigned memory_kb, char *err, size_t err_len) {
    put_text(err, err_len, "");
    apps_vm_t *vm = make_vm(memory_kb, err, err_len);
    if (vm == NULL) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        ok = apps_vm_load(vm, code[i].path, code[i].src, code[i].len, err, err_len);
        if (ok) {
            lua_pop(apps_vm_lua(vm), 1);
        }
    }
    apps_vm_free(vm);
    return ok;
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
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        rt->timers[i].fn_ref = LUA_NOREF;
    }
    rt->ui = apps_ui_new();
    if (rt->ui == NULL) {
        put_text(err, err_len, "out of memory: can't make room for the app's screen");
        apps_port_free(rt);
        return NULL;
    }
    apps_asset_t assets[APPS_UI_ASSETS_MAX];
    /* Before main.lua runs, so that a ui.image at its top level finds them. */
    apps_ui_set_assets(rt->ui, assets, build_assets(pkg, assets));
    rt->vm = make_vm(pkg->manifest.memory_kb, err, err_len);
    if (rt->vm == NULL) {
        apps_ui_free(rt->ui);
        apps_port_free(rt);
        return NULL;
    }
    lua_State *L = apps_vm_lua(rt->vm);
    apps_json_open(L);  /* safe outside a call: if the heap refuses, the call below ends out of memory */
    apps_ui_open(L, rt->ui);
    apps_svc_open(L, rt);  /* time, os, timer, alarm and sound: before main.lua, so its top level can arm a timer */
    rt->duty_second = apps_port_mono_ms() / DUTY_WINDOW_MS;  /* main.lua's top level is in the first window */

    boot_t boot = {.rt = rt, .main = main, .outcome = BOOT_OK};
    apps_call_t result = vm_run(rt, boot_trampoline, &boot, APPS_BUDGET_START);
    if (result == APPS_CALL_OK && boot.outcome == BOOT_NOT_COMPILED) {
        /* Nothing of main.lua ran, so there is no app to keep: Lua's message, which may be the parser's own
         * runtime error and have the call's traceback, as in require. */
        cut_traceback(boot.fail);
        apps_text_clean(boot.fail, strlen(boot.fail), true);
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
        timers_disarm(rt);
    }
}

bool apps_rt_alarm(apps_rt_t *rt, const char *name) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return false;
    }
    return rt->handler[H_ALARM] == LUA_NOREF ||
           call_handler(rt, H_ALARM, name != NULL ? name : "", APPS_BUDGET_CALL);
}

/* ---- Touch ---- */

typedef struct {
    apps_rt_t *rt;
    uint16_t slot, gen;
    uint8_t event;
} touch_call_t;

/* Runs under apps_vm_call: the handler, the widget and the event's name are pushed in here, since the widget's
 * handle is made in the app's heap. */
static int touch_trampoline(lua_State *L) {
    const touch_call_t *t = lua_touserdata(L, 1);
    if (apps_ui_push_handler(L, t->rt->ui, t->slot, t->gen, t->event)) {
        lua_call(L, 2, 0);
    }
    return 0;
}

void apps_rt_touch(apps_rt_t *rt, uint16_t slot, uint16_t gen, uint8_t event) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return;
    }
    touch_call_t t = {rt, slot, gen, event};
    if (vm_run(rt, touch_trampoline, &t, APPS_BUDGET_CALL) != APPS_CALL_OK) {
        crash(rt);
    }
}

/* ---- Timers ---- */

static void timer_free(armed_t *t) {
    t->gen = 0;
    t->fn_ref = LUA_NOREF;
}

/* Every timer given back to the registry and disarmed: the app has ended, or stopped. luaL_unref allocates nothing, so it
 * is safe wherever the VM is, and the main thread's stack is empty here (no call is running). */
static void timers_disarm(apps_rt_t *rt) {
    lua_State *L = apps_vm_lua(rt->vm);
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        if (rt->timers[i].gen != 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, rt->timers[i].fn_ref);
            timer_free(&rt->timers[i]);
        }
    }
}

int apps_rt_timer_add(apps_rt_t *rt, int fn_ref, int64_t ms, bool every, uint32_t *gen) {
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        armed_t *t = &rt->timers[i];
        if (t->gen == 0) {
            if (++rt->timer_gen == 0) {
                rt->timer_gen = 1;  /* never 0: that is a free slot */
            }
            int64_t due = apps_port_mono_ms() + ms + (rt->throttled ? THROTTLE_DELAY_MS : 0);
            *t = (armed_t){.fn_ref = fn_ref, .due = due, .period = every ? ms : 0, .gen = rt->timer_gen};
            *gen = t->gen;
            return i;
        }
    }
    return -1;
}

bool apps_rt_timer_take(apps_rt_t *rt, int slot, uint32_t gen, int *fn_ref) {
    if (slot < 0 || slot >= APPS_TIMERS_MAX || gen == 0 || rt->timers[slot].gen != gen) {
        return false;
    }
    *fn_ref = rt->timers[slot].fn_ref;
    timer_free(&rt->timers[slot]);
    return true;
}

int apps_rt_timer_count(const apps_rt_t *rt) {
    int n = 0;
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        n += rt->timers[i].gen != 0 ? 1 : 0;
    }
    return n;
}

/* Whether a runs before b: the earlier due, and of two due together the one armed first. */
static bool runs_first(const armed_t *a, const armed_t *b) {
    return a->due < b->due || (a->due == b->due && a->gen < b->gen);
}

int64_t apps_rt_next_deadline(const apps_rt_t *rt) {
    int64_t next = INT64_MAX;
    if (rt == NULL || rt->state != APPS_STATE_RUNNING || apps_port_dark()) {
        return next;
    }
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        if (rt->timers[i].gen != 0 && rt->timers[i].due < next) {
            next = rt->timers[i].due;
        }
    }
    return next;
}

typedef struct {
    apps_rt_t *rt;
    int slot;
    uint32_t gen;
} timer_call_t;

/* Runs under apps_vm_call: the callback, which a one-shot gives up its slot for before it runs. */
static int timer_trampoline(lua_State *L) {
    const timer_call_t *call = lua_touserdata(L, 1);
    armed_t *t = &call->rt->timers[call->slot];
    if (t->gen != call->gen) {
        return 0;  /* not the timer picked (it can't happen: nothing runs between the pick and here) */
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->fn_ref);
    if (t->period == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);  /* the function stays on the stack for its call */
        timer_free(t);
    }
    lua_call(L, 0, 0);
    return 0;
}

bool apps_rt_has_work(apps_rt_t *rt, int64_t now_ms) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return false;
    }
    duty_roll(rt, now_ms);
    return apps_vm_dying(rt->vm) || apps_rt_next_deadline(rt) <= now_ms;
}

static int nop_trampoline(lua_State *L) {
    (void)L;
    return 0;
}

bool apps_rt_run_one_timer(apps_rt_t *rt, int64_t now_ms) {
    if (rt == NULL || rt->state != APPS_STATE_RUNNING) {
        return false;
    }
    duty_roll(rt, now_ms);
    if (apps_vm_dying(rt->vm)) {
        /* Killed (by the duty cycle just now, or by another task) with no call to find out: this one does, and the app has
         * crashed with the reason. */
        if (vm_run(rt, nop_trampoline, NULL, APPS_BUDGET_CALL) != APPS_CALL_OK) {
            crash(rt);
        }
        return true;
    }
    if (apps_port_dark()) {
        return false;
    }
    armed_t *pick = NULL;
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        armed_t *t = &rt->timers[i];
        if (t->gen != 0 && t->due <= now_ms && (pick == NULL || runs_first(t, pick))) {
            pick = t;
        }
    }
    if (pick == NULL) {
        return false;
    }
    timer_call_t call = {.rt = rt, .slot = (int)(pick - rt->timers), .gen = pick->gen};
    if (pick->period > 0) {  /* before the call, so a callback that cancels itself finds it armed */
        pick->due = now_ms + pick->period;
    }
    if (vm_run(rt, timer_trampoline, &call, APPS_BUDGET_CALL) != APPS_CALL_OK) {
        crash(rt);
    } else if (rt->throttled) {
        /* A throttled every timer is due a period and the throttle's 100 ms after its callback ENDED, so that the delay is time
         * the core is idle: counted from when the callback began, a callback that outlasts its period and 100 ms is due again
         * before it has returned, and the throttle changes nothing. (The call may have ended the throttle: then the due time
         * above stands, a period after it began, as the brief says.) A timer the callback cancelled, or one that took its slot,
         * isn't this one. */
        armed_t *t = &rt->timers[call.slot];
        if (t->gen == call.gen && t->period > 0) {
            t->due = apps_port_mono_ms() + t->period + THROTTLE_DELAY_MS;
        }
    }
    return true;
}

void apps_rt_run_timers(apps_rt_t *rt, int64_t now_ms) {
    /* Each timer once: one that a callback arms is a period away, so no more steps than there are timers (a now_ms that isn't
     * the port's clock can't make a pass endless). */
    for (int step = 0; step < APPS_TIMERS_MAX && apps_rt_run_one_timer(rt, now_ms); step++) {
    }
}

/* ---- The duty cycle ---- */

/* The app has been hot for APPS_DUTY_THROTTLE_WINDOWS windows: every timer moves later, and it is told once. */
static void throttle_begin(apps_rt_t *rt) {
    rt->throttled = true;
    for (int i = 0; i < APPS_TIMERS_MAX; i++) {
        if (rt->timers[i].gen != 0) {
            rt->timers[i].due += THROTTLE_DELAY_MS;
        }
    }
    if (!rt->throttle_warned) {
        rt->throttle_warned = true;
        warn_add(rt, THROTTLE_WARNING);
        ring_push(rt, THROTTLE_WARNING, sizeof THROTTLE_WARNING - 1);
        apps_port_log(rt->pkg->manifest.id, THROTTLE_WARNING);
    }
}

/* Closes the window that is open, and opens the next: it is hot when what was booked to it is over the rate in instructions or
 * over APPS_DUTY_BUSY_PCT of it in time (a window with nothing booked is cool), and its share of time in calls is the last
 * the status reports. The count of hot windows in a row, the throttle and the kill follow, so windows are closed one at a
 * time and in the order of the clock, however late. */
static void duty_close(apps_rt_t *rt) {
    bool hot = rt->duty_instr > APPS_DUTY_INSTR_PER_S || rt->duty_busy_ms > DUTY_BUSY_MS;
    rt->cpu_pct = rt->duty_busy_ms >= DUTY_WINDOW_MS ? 100 : (uint8_t)(rt->duty_busy_ms * 100 / DUTY_WINDOW_MS);
    rt->duty_second++;
    rt->duty_instr = 0;
    rt->duty_busy_ms = 0;
    rt->duty_hot = hot ? rt->duty_hot + 1 : 0;
    if (rt->duty_hot >= APPS_DUTY_KILL_WINDOWS) {
        apps_vm_kill(rt->vm, DUTY_KILL_REASON);
    } else if (rt->duty_hot >= APPS_DUTY_THROTTLE_WINDOWS) {
        if (!rt->throttled) {
            throttle_begin(rt);
        }
    } else {
        rt->throttled = false;
    }
}

/* Closes the windows that have ended by now_ms: the one that is open, with what has been booked to it (every call books when it
 * ends, so a call that is still running isn't in it), and any others, which held nothing: a second with no call in it is cool. A
 * long silence is no longer than two windows to close: a second one that is empty is every one after it. */
static void duty_roll(apps_rt_t *rt, int64_t now_ms) {
    int64_t second = now_ms / DUTY_WINDOW_MS;
    for (int closed = 0; closed < 2 && rt->duty_second < second; closed++) {
        duty_close(rt);
    }
    if (rt->duty_second < second) {
        rt->duty_second = second;
    }
}

/* Books a call that began at `began` and ended at `ended` (apps_port_mono_ms before and after it) and ran `instr` VM
 * instructions to the windows it ran in, in proportion to the time it spent in each: a call that begins at 1250 and ends at 2010
 * puts 750 of its 760 ms and 750/760 of its instructions in the window of 1000 to 2000, and the rest in the next. A window the
 * call runs past the end of is closed as its share lands, and the windows are closed in order. The window the call ended in
 * stays open. A call that took no time (the clock stood still, as apphost's does inside a call) is all in the window it is in. */
static void duty_book(apps_rt_t *rt, int64_t began, int64_t ended, uint64_t instr) {
    int64_t open_from = rt->duty_second * DUTY_WINDOW_MS;
    if (began < open_from) {  /* the clock is monotonic: a case's that goes back is read as the open window's start */
        began = open_from;
    }
    if (ended < began) {
        ended = began;
    }
    const uint64_t span = (uint64_t)(ended - began);
    uint64_t given = 0;
    int64_t at = began;
    for (;;) {
        const bool last = ended / DUTY_WINDOW_MS <= rt->duty_second;
        const int64_t upto = last ? ended : (rt->duty_second + 1) * DUTY_WINDOW_MS;
        /* The instructions up to `upto`, in all: what is added to the window is the part since the last, so the shares add up
         * to `instr` exactly. */
        const uint64_t share = span > 0 ? instr * (uint64_t)(upto - began) / span : instr;
        rt->duty_instr += share - given;
        given = share;
        rt->duty_busy_ms += upto - at;
        at = upto;
        if (last) {
            break;
        }
        duty_close(rt);
    }
}

uint8_t apps_rt_cpu_pct(apps_rt_t *rt, int64_t now_ms) {
    if (rt == NULL) {
        return 0;
    }
    if (rt->state == APPS_STATE_RUNNING) {
        duty_roll(rt, now_ms);
    }
    return rt->cpu_pct;
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
            apps_text_clean(rt->err, strlen(rt->err), true);
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
        /* The VM goes first: no app code runs after it, so no handle to the table is left to use. */
        apps_vm_free(rt->vm);
        apps_ui_free(rt->ui);
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

const char *apps_rt_id(const apps_rt_t *rt) {
    return rt->pkg->manifest.id;
}

bool apps_rt_permits(const apps_rt_t *rt, unsigned perm) {
    return (rt->pkg->manifest.perms & perm) != 0;
}

apps_vm_t *apps_rt_vm(apps_rt_t *rt) {
    return rt != NULL ? rt->vm : NULL;
}

apps_ui_t *apps_rt_ui(apps_rt_t *rt) {
    return rt != NULL ? rt->ui : NULL;
}
