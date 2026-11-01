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

#include "apps_ask.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "apps_port.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

/*
 * The `muse` module (apps_ask.h has the contract). How an ask goes:
 *
 * 1. The call (the app task) checks its arguments and the gates, takes a registry reference to cb, then a block for the reply
 *    (apps_port_realloc): a refused block is an answer due at once, "the gadget is short of memory", and no turn goes. Otherwise the
 *    block becomes the slot's turn, the generation the one wanted, the in-flight flag set, and the turn goes (apps_port_ask).
 * 2. The chat task fills the block while the generation is wanted (apps_ask_piece), and at the turn's end (apps_ask_done) puts it on
 *    the results list, clears the flag and calls the notify hook.
 * 3. The app task's loop takes the list (apps_ask_results) and gives each result to the app that runs (apps_ask_take), which keeps it
 *    while its ask waits for that generation; then runs the callback (apps_ask_run_one). Its time running out (apps_ask_tick) detaches
 *    the ask, and its callback is due with "timed out"; a stop, a crash or the runtime's end detaches it too, and drops its callback.
 *    A detached ask whose turn is still out has it cancelled (apps_port_ask_cancel, P30): the session's done for it comes promptly,
 *    and clears the flag. A result nobody wants is freed.
 */

#define STR_(x) #x
#define STR(x) STR_(x)

#define ELLIPSIS "\xe2\x80\xa6"  /* "…", U+2026: what a cut reply ends with */
#define ELLIPSIS_LEN 3
#define HEAD_MAX (sizeof APPS_ASK_HEAD - 2 + APPS_ID_MAX)  /* APPS_ASK_HEAD with the id for its %s, NUL included */
#define SAY_MAX (sizeof "ask : done (99999 bytes)" + APPS_ID_MAX + APPS_ASK_ERR_MAX)  /* the ask's line */

/*
 * The slot (P17), shared by the app task and the chat task, each field written by one of them at a time, so no lock is needed. Plain
 * static data: internal RAM, where the atomics must be (S3 PSRAM has no compare-and-swap).
 * - in_flight: a turn of the apps' is at the chat. The app task sets it, while it is clear, just before the turn goes; the turn's end
 *   clears it (apps_ask_done), and nothing else does.
 * - want: the generation whose pieces are still wanted: the turn's from its start until its ask is detached (the app task's).
 * - turn: the turn's block. The app task sets it before the turn goes (in_flight clear, so the chat task has none); the chat task's
 *   from then until the turn's end, which puts it on done and clears it before it clears in_flight.
 * - done: the blocks of the turns that have ended, newest first: the chat task pushes, the app task takes the list whole.
 */
static struct {
    atomic_bool in_flight;
    atomic_uint want;
    apps_ask_result_t *turn;
    _Atomic(apps_ask_result_t *) done;
} s_slot;

/* The app task's hooks (apps_ask_setup): set before any ask, read after. */
static void (*s_notify)(void);
static void (*s_log)(const char *line);

/* The app task's own: the last generation given, and when the last accepted ask began (the gap is the gadget's, any app's). */
static uint32_t s_gen;
static bool s_asked;
static int64_t s_last_start;

void apps_ask_setup(void (*notify)(void), void (*log)(const char *line)) {
    s_notify = notify;
    s_log = log;
}

/* ---- The app's books ---- */

void apps_ask_init(apps_ask_app_t *a) {
    memset(a, 0, sizeof *a);
    a->ref = LUA_NOREF;
}

/* The app lets go of its turn: its pieces aren't wanted any more, and its end comes back to nobody. While the turn is still out it is
 * cancelled (P30, amending P17): the session ends it only while it is this ask's, and the done that follows clears the flag. */
static void detach(apps_ask_app_t *a) {
    unsigned gen = a->gen;
    if (gen != 0) {
        (void)atomic_compare_exchange_strong(&s_slot.want, &gen, 0u);
        if (atomic_load(&s_slot.in_flight)) {
            apps_port_ask_cancel((void *)(uintptr_t)a->gen);
        }
    }
    a->gen = 0;
}

/* Everything the books hold but the callback's reference goes. */
static void drop(apps_ask_app_t *a) {
    detach(a);
    apps_ask_result_free(a->ready);
    a->ready = NULL;
    a->why = NULL;
}

void apps_ask_release(apps_ask_app_t *a, lua_State *L) {
    if (a == NULL) {
        return;
    }
    if (a->ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, a->ref);
        a->ref = LUA_NOREF;
    }
    drop(a);
}

void apps_ask_free(apps_ask_app_t *a) {
    if (a != NULL) {
        drop(a);
        a->ref = LUA_NOREF;  /* the VM, and its registry, go with the runtime */
    }
}

/* ---- The sink (the chat task's) ---- */

void apps_ask_piece(void *user, const char *text, size_t len) {
    apps_ask_result_t *r = s_slot.turn;
    uint32_t gen = (uint32_t)(uintptr_t)user;
    if (r == NULL || r->gen != gen || atomic_load(&s_slot.want) != gen || r->cut) {
        return;  /* detached (timed out, or its app ended), or cut already: dropped */
    }
    size_t room = APPS_ASK_REPLY_MAX - r->len;
    if (len <= room) {
        memcpy(r->text + r->len, text, len);
        r->len = (uint16_t)(r->len + len);
        return;
    }
    /* Past the most: cut where a character starts (text[keep] is the first byte left out, never a continuation byte), and "…". */
    size_t keep = room;
    while (keep > 0 && ((unsigned char)text[keep] & 0xC0) == 0x80) {
        keep--;
    }
    memcpy(r->text + r->len, text, keep);
    memcpy(r->text + r->len + keep, ELLIPSIS, ELLIPSIS_LEN);
    r->len = (uint16_t)(r->len + keep + ELLIPSIS_LEN);
    r->cut = true;
}

/* A block onto the results list (one pusher at a time: the turn's end). */
static void push(apps_ask_result_t *r) {
    apps_ask_result_t *head = atomic_load(&s_slot.done);
    do {
        r->next = head;
    } while (!atomic_compare_exchange_weak(&s_slot.done, &head, r));
}

void apps_ask_done(void *user, const char *err) {
    apps_ask_result_t *r = s_slot.turn;
    if (r == NULL || r->gen != (uint32_t)(uintptr_t)user) {
        return;  /* not the turn that is out: the session ends each turn once, so this never comes */
    }
    s_slot.turn = NULL;
    r->ok = err == NULL;
    if (err != NULL) {
        int n = snprintf(r->err, sizeof r->err, "%s", err);
        size_t len = n < 0 ? 0 : (size_t)n < sizeof r->err ? (size_t)n : sizeof r->err - 1;
        r->err[apps_text_clean(r->err, len, false)] = '\0';  /* the chat's words, printed by the log hook */
    }
    r->text[r->len] = '\0';
    push(r);
    atomic_store(&s_slot.in_flight, false);
    if (s_notify != NULL) {
        s_notify();
    }
}

/* ---- The app task's loop ---- */

apps_ask_result_t *apps_ask_results(void) {
    apps_ask_result_t *newest = atomic_exchange(&s_slot.done, NULL);
    apps_ask_result_t *oldest = NULL;
    while (newest != NULL) {
        apps_ask_result_t *next = newest->next;
        newest->next = oldest;
        oldest = newest;
        newest = next;
    }
    return oldest;
}

void apps_ask_result_free(apps_ask_result_t *r) {
    apps_port_free(r);
}

void apps_ask_take(apps_rt_t *rt, apps_ask_result_t *r) {
    apps_ask_app_t *a = rt != NULL && apps_rt_state(rt) == APPS_STATE_RUNNING ? apps_rt_ask(rt) : NULL;
    if (a != NULL && a->ref != LUA_NOREF && a->gen != 0 && a->gen == r->gen && a->ready == NULL) {
        r->next = NULL;
        a->ready = r;
        a->gen = 0;  /* its turn has ended: nothing of it is wanted now, and it can't time out */
        return;
    }
    apps_ask_result_free(r);  /* an app that ended, an ask that timed out, or no ask: Review Focus 1 */
}

void apps_ask_tick(apps_rt_t *rt, int64_t now_ms) {
    apps_ask_app_t *a = apps_rt_ask(rt);
    if (a != NULL && a->ref != LUA_NOREF && a->gen != 0 && a->ready == NULL && a->why == NULL && now_ms >= a->until) {
        detach(a);  /* P19: the turn runs on, and its end comes back to nobody */
        a->why = APPS_ASK_MSG_TIMED_OUT;
    }
}

bool apps_ask_has_work(const apps_rt_t *rt) {
    const apps_ask_app_t *a = apps_rt_ask(rt);
    return a != NULL && a->ref != LUA_NOREF && (a->ready != NULL || a->why != NULL);
}

int64_t apps_ask_next_deadline(const apps_rt_t *rt) {
    const apps_ask_app_t *a = apps_rt_ask(rt);
    if (apps_ask_has_work(rt)) {
        return 0;
    }
    return a != NULL && a->ref != LUA_NOREF && a->gen != 0 ? a->until : INT64_MAX;
}

/* One callback to run: its reference, and its result or what it is told without one. */
typedef struct {
    int ref;
    apps_ask_result_t *r;
    const char *why;
} deliver_t;

/* Runs under apps_vm_call (apps_rt_callback): cb(reply) or cb(nil, err). The reference goes before the call, with the callback on the
 * stack, so it runs once whatever the call does. */
static int deliver_trampoline(lua_State *L) {
    const deliver_t *d = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->ref);
    luaL_unref(L, LUA_REGISTRYINDEX, d->ref);
    if (d->r != NULL && d->r->ok) {
        lua_pushlstring(L, d->r->text, d->r->len);
        lua_call(L, 1, 0);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, d->r != NULL ? d->r->err : d->why);
        lua_call(L, 2, 0);
    }
    return 0;
}

/* The ask's one line: how it went, never what it or its reply said. */
static void say(const apps_rt_t *rt, const deliver_t *d) {
    if (s_log == NULL) {
        return;
    }
    char line[SAY_MAX];
    if (d->r != NULL && d->r->ok) {
        snprintf(line, sizeof line, "ask %s: done (%u bytes)", apps_rt_id(rt), (unsigned)d->r->len);
    } else {
        snprintf(line, sizeof line, "ask %s: %s", apps_rt_id(rt), d->r != NULL ? d->r->err : d->why);
    }
    s_log(line);
}

bool apps_ask_run_one(apps_rt_t *rt) {
    apps_ask_app_t *a = apps_rt_ask(rt);
    if (a == NULL || apps_rt_state(rt) != APPS_STATE_RUNNING || !apps_ask_has_work(rt)) {
        return false;
    }
    /* Taken from the books before the call, which may ask again or end them (a crash releases them). */
    deliver_t d = {.ref = a->ref, .r = a->ready, .why = a->why};
    a->ref = LUA_NOREF;
    a->ready = NULL;
    a->why = NULL;
    a->gen = 0;
    say(rt, &d);
    (void)apps_rt_callback(rt, deliver_trampoline, &d);  /* a failure has crashed the app */
    apps_ask_result_free(d.r);
    return true;
}

/* ---- The binding ---- */

/* The bytes of the line break that starts c[0, n), or 0: LF, CR, VT, FF, or U+0085, U+2028 or U+2029 in UTF-8. */
static size_t line_break(const unsigned char *c, size_t n) {
    if (*c == '\n' || *c == '\r' || *c == '\v' || *c == '\f') {
        return 1;
    }
    if (n >= 2 && c[0] == 0xC2 && c[1] == 0x85) {
        return 2;
    }
    if (n >= 3 && c[0] == 0xE2 && c[1] == 0x80 && (c[2] == 0xA8 || c[2] == 0xA9)) {
        return 3;
    }
    return 0;
}

/* s[0, n) on one line, in place: each run of line breaks is one space. Its new length; it never lengthens. */
static size_t flatten_lines(char *s, size_t n) {
    size_t w = 0;
    bool in_break = false;
    for (size_t r = 0; r < n;) {
        size_t k = line_break((const unsigned char *)s + r, n - r);
        if (k == 0) {
            s[w++] = s[r++];
            in_break = false;
            continue;
        }
        if (!in_break) {
            s[w++] = ' ';
        }
        in_break = true;
        r += k;
    }
    return w;
}

static apps_rt_t *rt_of(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

/* muse.ask(text, cb). */
static int muse_ask(lua_State *L) {
    apps_vm_check(L);
    apps_rt_t *rt = rt_of(L);
    if (!apps_rt_permits(rt, APPS_PERM_MUSE)) {
        return luaL_error(L, "muse: add \"muse\" to \"permissions\" in manifest.json");
    }
    if (lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "muse.ask: text must be a string, got %s", luaL_typename(L, 1));
    }
    size_t len;
    const char *text = lua_tolstring(L, 1, &len);
    if (len < 1 || len > APPS_ASK_TEXT_MAX) {
        return luaL_error(L, "muse.ask: text must be 1 to " STR(APPS_ASK_TEXT_MAX) " bytes, got %d", (int)len);
    }
    if (strlen(text) != len) {
        return luaL_error(L, "muse.ask: text must not hold a NUL byte");
    }
    if (lua_type(L, 2) != LUA_TFUNCTION) {
        return luaL_error(L, "muse.ask: cb must be a function, got %s", luaL_typename(L, 2));
    }
    /* The gates (P18: raised at the call). Only a tap opens the first: message() and alarm() don't. */
    int64_t now = apps_port_mono_ms();
    int64_t tap_ms = 0;
    if (!apps_rt_tapped(rt, &tap_ms) || now - tap_ms > APPS_SHOW_TAP_MS) {
        return luaL_error(L, APPS_ASK_MSG_TAP, APPS_SHOW_TAP_MS / 1000);
    }
    apps_ask_app_t *a = apps_rt_ask(rt);
    if (a->ref != LUA_NOREF || atomic_load(&s_slot.in_flight)) {
        return luaL_error(L, "%s", APPS_ASK_MSG_ONE);
    }
    if (s_asked && now - s_last_start < APPS_ASK_GAP_MS) {
        int64_t left = s_last_start + APPS_ASK_GAP_MS - now;
        return luaL_error(L, APPS_ASK_MSG_WAIT, (int)((left + 999) / 1000));
    }
    /* What goes, on this stack: APPS_ASK_HEAD for the app, then the text on one line (flatten_lines) and cleaned to UTF-8, tabs kept. */
    char msg[HEAD_MAX + APPS_ASK_TEXT_MAX];
    size_t head = (size_t)snprintf(msg, HEAD_MAX, APPS_ASK_HEAD, apps_rt_id(rt));
    memcpy(msg + head, text, len);
    size_t n = head + apps_text_clean(msg + head, flatten_lines(msg + head, len), false);
    /* The callback, in the app's heap before any block is held: a heap that refuses raises here, and leaks nothing. */
    lua_pushvalue(L, 2);
    a->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    a->until = now + APPS_ASK_TIMEOUT_MS;
    s_asked = true;
    s_last_start = now;  /* P18: the gap counts from the start */
    apps_ask_result_t *r = apps_port_realloc(NULL, sizeof *r);
    if (r == NULL) {
        a->why = APPS_ASK_MSG_NO_MEMORY;  /* its callback is due at the next round, and no turn goes */
        return 0;
    }
    memset(r, 0, sizeof *r);
    s_gen = s_gen + 1 != 0 ? s_gen + 1 : 1;
    r->gen = s_gen;
    a->gen = s_gen;
    s_slot.turn = r;
    atomic_store(&s_slot.want, s_gen);
    atomic_store(&s_slot.in_flight, true);
    apps_port_ask(msg, n, (void *)(uintptr_t)s_gen);
    return 0;
}

static const luaL_Reg apps_muse_funcs[] = {
    {"ask", muse_ask},
    {NULL, NULL},
};

void apps_ask_open(lua_State *L, apps_rt_t *rt) {
    lua_createtable(L, 0, 1);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_muse_funcs, 1);
    lua_setglobal(L, "muse");
}
