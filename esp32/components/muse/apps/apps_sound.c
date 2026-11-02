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

#include "apps_sound.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apps_mix.h"  /* apps_wave_t's order and the mixer's rate: no call (apps_sound.h) */
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

/*
 * The `sound` module (apps_sound.h has the contract). Every binding calls apps_vm_check first and runs inside a call of the app, so a
 * handle the app's heap refuses ends the app out of memory, as everywhere else. A start makes its handle first and starts the voice
 * last, which can't fail: a refused allocation leaves nothing playing. Upvalue 1 of the module's functions is the runtime, upvalue 2
 * (tone and play's) the books.
 *
 * Stack, under app code: a key or a path as a message shows it (68 bytes) is the largest frame.
 */

/* A key shown in a message is cut here, as the ui module's are; a path at the longest a package has. */
#define SHOWN_KEY_MAX 40

_Static_assert(APPS_SOUND_HZ_MAX <= APPS_MIX_RATE / 2, "the mixer plays every tone the module takes");
_Static_assert(APPS_WAVE_SINE == 0 && APPS_WAVE_NOISE == APPS_SOUND_WAVES - 1, "apps_sound_waves names apps_wave_t in order");

const char *const apps_sound_waves[] = {"sine", "square", "triangle", "saw", "noise"};
_Static_assert(sizeof apps_sound_waves / sizeof apps_sound_waves[0] == APPS_SOUND_WAVES, "a name for each wave");

/* What a handle holds: the port's voice (0 once stopped, or for a sound done from the start) and the app's count of it. */
typedef struct {
    uint32_t id;
    uint32_t n;
} sound_handle_t;

/* The module's books, a userdata of the app's heap: the sounds the app has made, done ones included. */
typedef struct {
    uint32_t made;
} sound_books_t;

/* A number a call takes: its key, its unit for the message (NULL for none) and its range. */
typedef struct {
    const char *key, *unit;
    int lo, hi;
} range_t;

static const range_t HZ = {"hz", "Hz", APPS_SOUND_HZ_MIN, APPS_SOUND_HZ_MAX};
static const range_t TO_HZ = {"to_hz", "Hz", APPS_SOUND_HZ_MIN, APPS_SOUND_HZ_MAX};
static const range_t MS = {"ms", "milliseconds", APPS_SOUND_MS_MIN, APPS_SOUND_MS_MAX};
static const range_t VOLUME = {"volume", NULL, 0, APPS_SOUND_VOLUME_MAX};

static const char *const TONE_KEYS[] = {"hz", "ms", "to_hz", "wave", "volume"};
static const char *const PLAY_KEYS[] = {"volume", "loop"};

static apps_rt_t *rt_of(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

/* Raises the module's message unless the manifest gives the app the "sound" permission. */
static void need_sound(lua_State *L) {
    if (!apps_rt_permits(rt_of(L), APPS_PERM_SOUND)) {
        luaL_error(L, "sound: add \"sound\" to \"permissions\" in manifest.json");
    }
}

/* Whether a sound started now plays: the app is shown and the screen lit (apps_sound.h). */
static bool plays_now(const apps_rt_t *rt) {
    return apps_rt_is_shown(rt) && !apps_port_dark();
}

/* The value at idx as a message shows it: text and numbers as text (a control character as '?', cut at max), anything else by its
 * type. out has room for max + 4 bytes. A number is converted on a copy, so a key lua_next walks is left as it is. */
static const char *shown_value(lua_State *L, int idx, size_t max, char *out) {
    int type = lua_type(L, idx);
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        lua_pushvalue(L, idx);
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        apps_text_shown(s, len, max, out);
        lua_pop(L, 1);
    } else {
        snprintf(out, max + 4, "<%s>", luaL_typename(L, idx));
    }
    return out;
}

/* Every key of the table at t is one of keys[0, n), or the call raises, listing them: before any value is looked at. The walk is C's,
 * where no count hook fires: charged by the slots lua_next visits. */
static void check_keys(lua_State *L, const char *call, int t, const char *const *keys, int n, const char *valid) {
    apps_vm_charge(L, apps_vm_table_slots(L, t) + 1);
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        lua_pop(L, 1);  /* the value; the key stays for the next step */
        bool known = false;
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t len;
            const char *key = lua_tolstring(L, -1, &len);
            for (int i = 0; i < n && !known; i++) {
                known = strlen(keys[i]) == len && memcmp(keys[i], key, len) == 0;
            }
        }
        if (!known) {
            char shown[SHOWN_KEY_MAX + 4];
            luaL_error(L, "%s: unknown key '%s' (valid: %s)", call, shown_value(L, -1, SHOWN_KEY_MAX, shown), valid);
        }
    }
}

/* Pushes t[key], read raw (no metamethod, so no app code runs here), and returns its type. */
static int field(lua_State *L, int t, const char *key) {
    lua_pushstring(L, key);
    return lua_rawget(L, t);
}

/* The number at idx for r: rounded, halves away from zero, and clamped to r's range (as doubles: an int made of a NaN or of an out of
 * range double is undefined), or the call's error for a value that isn't a number, or is NaN. */
static int read_number(lua_State *L, const char *call, const range_t *r, int idx) {
    bool number = lua_type(L, idx) == LUA_TNUMBER;
    double d = number ? (double)lua_tonumber(L, idx) : 0;
    if (!number || isnan(d)) {
        const char *got = number ? "NaN" : luaL_typename(L, idx);
        if (r->unit != NULL) {
            luaL_error(L, "%s: '%s' must be a number of %s, %d to %d, got %s", call, r->key, r->unit, r->lo, r->hi, got);
        }
        luaL_error(L, "%s: '%s' must be a number, %d to %d, got %s", call, r->key, r->lo, r->hi, got);
    }
    d = round(d);
    return d < r->lo ? r->lo : d > r->hi ? r->hi : (int)d;
}

/* The wave at idx: one of apps_sound_waves by name (a number is its text, which is no name either), or tone's error. */
static int read_wave(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        for (int i = 0; i < APPS_SOUND_WAVES; i++) {
            if (strlen(apps_sound_waves[i]) == len && memcmp(apps_sound_waves[i], s, len) == 0) {
                return i;
            }
        }
    }
    char shown[SHOWN_KEY_MAX + 4];
    return luaL_error(L, "sound.tone: wave '%s' is not one of sine, square, triangle, saw, noise",
                      shown_value(L, idx, SHOWN_KEY_MAX, shown));
}

/* A new handle, done until a start gives it a voice, with the app's next count: made before anything plays. */
static sound_handle_t *new_handle(lua_State *L) {
    sound_books_t *books = lua_touserdata(L, lua_upvalueindex(2));
    sound_handle_t *h = lua_newuserdatauv(L, sizeof *h, 0);
    luaL_setmetatable(L, APPS_SOUND_MT);
    h->id = 0;
    h->n = ++books->made;
    return h;
}

/* ---- The calls ---- */

/* sound.chirp(): one chirp, with the "sound" permission. */
static int sound_chirp(lua_State *L) {
    apps_vm_check(L);
    need_sound(L);
    apps_port_sound_chirp();
    return 0;
}

/* sound.stop(): ends the ring of an alarm that is going, whichever app's it is: no permission (an app that can't make a
 * noise can still be asked to stop one), and nothing happens when none is going. */
static int sound_stop(lua_State *L) {
    apps_vm_check(L);
    apps_port_sound_stop();
    return 0;
}

/* sound.tone{hz, ms, to_hz, wave, volume}: the keys, then each value in a fixed order, so which error comes first doesn't depend on the
 * table's own order. */
static int sound_tone(lua_State *L) {
    apps_vm_check(L);
    need_sound(L);
    if (lua_type(L, 1) != LUA_TTABLE) {
        return luaL_error(L, "sound.tone: needs a table like {hz = 440, ms = 100}, got %s", luaL_typename(L, 1));
    }
    lua_settop(L, 1);
    check_keys(L, "sound.tone", 1, TONE_KEYS, 5, "hz, ms, to_hz, wave, volume");
    if (field(L, 1, "hz") == LUA_TNIL) {
        return luaL_error(L, "sound.tone: hz is required (a number of Hz, %d to %d)", APPS_SOUND_HZ_MIN, APPS_SOUND_HZ_MAX);
    }
    int hz = read_number(L, "sound.tone", &HZ, -1);
    int ms = field(L, 1, "ms") == LUA_TNIL ? APPS_SOUND_MS_DEFAULT : read_number(L, "sound.tone", &MS, -1);
    int to_hz = field(L, 1, "to_hz") == LUA_TNIL ? hz : read_number(L, "sound.tone", &TO_HZ, -1);
    int wave = field(L, 1, "wave") == LUA_TNIL ? APPS_WAVE_SINE : read_wave(L, -1);
    int volume = field(L, 1, "volume") == LUA_TNIL ? APPS_SOUND_VOLUME_MAX : read_number(L, "sound.tone", &VOLUME, -1);
    lua_settop(L, 1);
    apps_rt_t *rt = rt_of(L);
    sound_handle_t *h = new_handle(L);
    if (plays_now(rt)) {
        h->id = apps_port_sound_tone(apps_rt_gen(rt), hz, to_hz, ms, wave, volume);
    }
    return 1;
}

/* sound.play(path, {volume, loop}): the path, then the options' keys, then their values. */
static int sound_play(lua_State *L) {
    apps_vm_check(L);
    need_sound(L);
    if (lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "sound.play: path must be a string, got %s", luaL_typename(L, 1));
    }
    apps_rt_t *rt = rt_of(L);
    size_t len;
    const char *path = lua_tolstring(L, 1, &len);
    const int16_t *samples = NULL;
    size_t frames = 0;
    if (strlen(path) != len || !apps_rt_sound(rt, path, &samples, &frames)) {  /* a NUL in it is no path of the package's */
        char shown[APPS_PATH_MAX + 4];
        apps_text_shown(path, len, APPS_PATH_MAX, shown);
        return luaL_error(L, "sound.play: '%s' is not one of this app's sounds (manifest.json \"files\")", shown);
    }
    int volume = APPS_SOUND_VOLUME_MAX;
    bool loop = false;
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) != LUA_TTABLE) {
            return luaL_error(L, "sound.play: options must be a table like {volume = 100, loop = false}, got %s", luaL_typename(L, 2));
        }
        lua_settop(L, 2);
        check_keys(L, "sound.play", 2, PLAY_KEYS, 2, "volume, loop");
        if (field(L, 2, "volume") != LUA_TNIL) {
            volume = read_number(L, "sound.play", &VOLUME, -1);
        }
        if (field(L, 2, "loop") != LUA_TNIL) {
            if (lua_type(L, -1) != LUA_TBOOLEAN) {
                return luaL_error(L, "sound.play: 'loop' must be true or false, got %s", luaL_typename(L, -1));
            }
            loop = lua_toboolean(L, -1) != 0;
        }
    }
    lua_settop(L, 2);
    sound_handle_t *h = new_handle(L);
    if (plays_now(rt)) {
        h->id = apps_port_sound_clip(apps_rt_gen(rt), samples, frames, volume, loop);
    }
    return 1;
}

/* ---- The handle ---- */

/* h:stop(): the port stops nothing for a voice that has ended or was replaced; one stopped, or done from the start, is 0 here. */
static int handle_stop(lua_State *L) {
    apps_vm_check(L);
    sound_handle_t *h = luaL_testudata(L, 1, APPS_SOUND_MT);
    if (h == NULL) {
        return luaL_error(L, "sound: stop needs a sound: call it as h:stop()");
    }
    if (h->id != 0) {
        apps_port_sound_end(h->id);
        h->id = 0;
    }
    return 0;
}

/* h.name: a method (upvalue 1, the methods), or an error that lists them. */
static int handle_index(lua_State *L) {
    apps_vm_check(L);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) {
            return 1;
        }
    }
    char shown[SHOWN_KEY_MAX + 4];
    if (lua_type(L, 2) == LUA_TSTRING) {
        size_t len;
        const char *name = lua_tolstring(L, 2, &len);
        apps_text_shown(name, len, SHOWN_KEY_MAX, shown);
    } else {
        snprintf(shown, sizeof shown, "<%s>", luaL_typename(L, 2));
    }
    return luaL_error(L, "sound: unknown method '%s' (valid: stop)", shown);
}

static int handle_newindex(lua_State *L) {
    apps_vm_check(L);
    return luaL_error(L, "sound: a sound has no fields: h:stop() stops it");
}

static int handle_tostring(lua_State *L) {
    apps_vm_check(L);
    const sound_handle_t *h = luaL_testudata(L, 1, APPS_SOUND_MT);
    if (h == NULL) {
        return luaL_error(L, "sound: tostring needs a sound");
    }
    lua_pushfstring(L, "sound#%I", (lua_Integer)h->n);
    return 1;
}

static const luaL_Reg apps_sound_funcs[] = {
    {"chirp", sound_chirp},
    {"stop", sound_stop},
    {"tone", sound_tone},
    {"play", sound_play},
    {NULL, NULL},
};

static const luaL_Reg apps_sound_methods[] = {
    {"stop", handle_stop},
    {NULL, NULL},
};

void apps_sound_open(lua_State *L, apps_rt_t *rt) {
    /* Protected: getmetatable gives the name, never the table (X17: a finalizer put on it would run with the hooks off). */
    luaL_newmetatable(L, APPS_SOUND_MT);
    lua_pushliteral(L, APPS_SOUND_MT);
    lua_setfield(L, -2, "__metatable");
    lua_createtable(L, 0, 1);  /* the methods */
    luaL_setfuncs(L, apps_sound_methods, 0);
    lua_pushcclosure(L, handle_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, handle_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, handle_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);
    lua_pushlightuserdata(L, rt);
    sound_books_t *books = lua_newuserdatauv(L, sizeof *books, 0);
    books->made = 0;
    luaL_setfuncs(L, apps_sound_funcs, 2);
    lua_setglobal(L, "sound");
}
