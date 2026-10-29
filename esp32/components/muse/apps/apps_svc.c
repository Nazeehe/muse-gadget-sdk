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

/* gmtime_r, localtime_r and the rest of POSIX 2008 come with a C library's default definitions. One that hides what a
 * standard doesn't name (glibc under -std=c11, which apphost is built with) declares them only when asked, and this has to
 * come before the first include, which is where the headers decide. */
#define _DEFAULT_SOURCE 1

#include "apps_svc.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "apps_port.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

/*
 * The `time`, `os` and `timer` modules (apps_svc.h says what they do). Every binding calls apps_vm_check first and runs
 * inside a call of the app, so what allocates in the app's heap (a table, a string, a handle, a registry reference) may be
 * refused and ends the app out of memory, as everywhere else. The clocks are the port's (apps_port.h): the wall clock is the
 * port's epoch, and the zone is the C library's (TZ), which the watch sets on the gadget (muse_clock_build) and apphost sets
 * to UTC. What a timer is lives in the runtime; the handle here is a slot and a gen.
 *
 * Stack, under app code: the formatter's output buffer (136 bytes) and a struct tm are the largest frames, in the bindings
 * of time.format and os.date.
 */

#define STR_(x) #x
#define STR(x) STR_(x)

#define TIMER_MT "muse.timer"

/* The longest result of time.format and os.date, in bytes. A longer one is an error, never cut. */
#define FORMAT_MAX 128
#define SPECIFIERS "(use %Y %m %d %H %M %S %I %p %a %A %b %B %j %y %e)"
/* What os.date formats without a format: C's %c in the C locale, which isn't one of the specifiers. */
#define DEFAULT_DATE_FORMAT "%a %b %e %H:%M:%S %Y"

/* The instants the calendar functions take, in epoch seconds: 0000-01-01T00:00:00Z to 9999-12-31T23:59:59Z. A time outside is
 * an error, on the gadget and in apphost alike: picolibc's gmtime_r wraps a year that doesn't fit an int (a time past two
 * billion years from 1970) and returns it, where glibc and Darwin's refuse, so what a program got would depend on where it
 * ran. (mktime's reach differs too: Darwin's takes no year before 1900. The calendar is the same bound for all of them.) */
#define CALENDAR_MIN_S (-62167219200LL)
#define CALENDAR_MAX_S 253402300799LL
#define CALENDAR_YEARS "(the years 0 to 9999)"

/* How far from its year and month an os.time table may put the date, in days: day, hour, min and sec added up (each by its
 * size). picolibc's mktime goes through the days it is given a month at a time, about a microsecond a month, so 2^31 days
 * would hold the app for six seconds, with the app task's watchdog and the supervisor's three seconds both passed. This is
 * about 8200 years and costs 10 ms at most. */
#define OS_TIME_MAX_DAYS 3000000

static const char *const DAYS[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MONTHS[12] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

/* ---- Arguments ---- */

/* The number at idx as a whole number: an integer, or a float that is one (and fits). Not a string, though Lua would
 * convert it. */
static bool whole(lua_State *L, int idx, lua_Integer *out) {
    int ok = 0;
    lua_Integer v = lua_type(L, idx) == LUA_TNUMBER ? lua_tointegerx(L, idx, &ok) : 0;
    if (ok) {
        *out = v;
    }
    return ok != 0;
}

/* What the value at idx is, for an error: a number by its value ("1.5"), anything else by its type ("no value" for none). */
static const char *got(lua_State *L, int idx, char *buf, size_t cap) {
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return luaL_typename(L, idx);
    }
    if (lua_isinteger(L, idx)) {
        snprintf(buf, cap, "%lld", (long long)lua_tointeger(L, idx));
    } else {
        double n = (double)lua_tonumber(L, idx);
        if (isnan(n)) {
            snprintf(buf, cap, "nan");
        } else if (isinf(n)) {
            snprintf(buf, cap, n > 0 ? "inf" : "-inf");
        } else {
            snprintf(buf, cap, "%.14g", n);
        }
    }
    return buf;
}

/* ---- Time ---- */

/* x / 1000, rounded down, for a time before 1970 too. */
static int64_t floor_seconds(int64_t ms) {
    int64_t s = ms / 1000;
    return ms % 1000 < 0 ? s - 1 : s;
}

/* Epoch seconds as broken-down time, local or UTC. False for a time outside the calendar (above), and for one the C library
 * can't take. */
static bool convert(int64_t seconds, bool utc, struct tm *tm) {
    if (seconds < CALENDAR_MIN_S || seconds > CALENDAR_MAX_S) {
        return false;
    }
    time_t t = (time_t)seconds;
    if ((int64_t)t != seconds) {  /* a time_t narrower than 64 bits */
        return false;
    }
    return (utc ? gmtime_r(&t, tm) : localtime_r(&t, tm)) != NULL;
}

/* Epoch seconds now, or seconds since boot until the clock is set (os.time and os.date's defaults). */
static int64_t now_seconds(void) {
    int64_t ms;
    return apps_port_epoch_ms(&ms) ? floor_seconds(ms) : apps_port_mono_ms() / 1000;
}

/* time.date's and time.format's ms at idx: a whole number, or the wall clock when there is none (an error until it is set). */
static int64_t arg_ms(lua_State *L, int idx, const char *fname) {
    if (lua_isnoneornil(L, idx)) {
        int64_t ms;
        if (!apps_port_epoch_ms(&ms)) {
            luaL_error(L, "%s: the clock isn't set yet", fname);
        }
        return ms;
    }
    lua_Integer ms = 0;
    if (!whole(L, idx, &ms)) {
        char buf[32];
        luaL_error(L, "%s: ms must be a whole number of milliseconds, got %s", fname, got(L, idx, buf, sizeof buf));
    }
    return ms;
}

static void set_int(lua_State *L, const char *key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

/* tm's fields into the table on top of the stack: time.date's, and os.date's isdst, which time.date doesn't have. */
static void fill_fields(lua_State *L, const struct tm *tm, bool with_isdst) {
    set_int(L, "year", (lua_Integer)tm->tm_year + 1900);
    set_int(L, "month", tm->tm_mon + 1);
    set_int(L, "day", tm->tm_mday);
    set_int(L, "hour", tm->tm_hour);
    set_int(L, "min", tm->tm_min);
    set_int(L, "sec", tm->tm_sec);
    set_int(L, "wday", tm->tm_wday + 1);
    set_int(L, "yday", tm->tm_yday + 1);
    if (with_isdst && tm->tm_isdst >= 0) {
        lua_pushboolean(L, tm->tm_isdst);
        lua_setfield(L, -2, "isdst");
    }
}

static void push_fields(lua_State *L, const struct tm *tm, bool with_isdst) {
    lua_createtable(L, 0, 9);
    fill_fields(L, tm, with_isdst);
}

static int time_now(lua_State *L) {
    apps_vm_check(L);
    int64_t ms;
    if (apps_port_epoch_ms(&ms)) {
        lua_pushinteger(L, ms);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int time_mono(lua_State *L) {
    apps_vm_check(L);
    lua_pushinteger(L, apps_port_mono_ms());
    return 1;
}

static int time_synced(lua_State *L) {
    apps_vm_check(L);
    int64_t ms;
    lua_pushboolean(L, apps_port_epoch_ms(&ms));
    return 1;
}

static int time_date(lua_State *L) {
    apps_vm_check(L);
    int64_t ms = arg_ms(L, 1, "time.date");
    struct tm tm;
    if (!convert(floor_seconds(ms), false, &tm)) {
        return luaL_error(L, "time.date: ms is out of range " CALENDAR_YEARS);
    }
    push_fields(L, &tm, false);
    return 1;
}

/* ---- Formatting: time.format and os.date ---- */

typedef struct {
    char text[FORMAT_MAX + 1];
    size_t len;
    bool full;  /* the result passed FORMAT_MAX: nothing more is kept */
} out_t;

static void put(out_t *o, const char *s, size_t n) {
    if (o->len + n > FORMAT_MAX) {
        o->full = true;
        return;
    }
    memcpy(o->text + o->len, s, n);
    o->len += n;
}

/* v in decimal, padded on the left to width with pad. */
static void put_int(out_t *o, long long v, int width, char pad) {
    char digits[24];
    int n = snprintf(digits, sizeof digits, "%lld", v);
    for (int i = n; i < width; i++) {
        put(o, &pad, 1);
    }
    put(o, digits, (size_t)n);
}

typedef enum { FMT_OK, FMT_TOO_LONG, FMT_LONE_PERCENT, FMT_UNSUPPORTED } fmt_status_t;

/* fmt[0, len) formatted for tm into o: stops at the first thing wrong, with *bad the unsupported specifier's character. A
 * format makes at least a byte of output for each character it has, so a long one is stopped at the cap's end. */
static fmt_status_t format_tm(out_t *o, const char *fmt, size_t len, const struct tm *tm, char *bad) {
    for (size_t i = 0; i < len; i++) {
        if (o->full) {
            break;
        }
        if (fmt[i] != '%') {
            put(o, &fmt[i], 1);
            continue;
        }
        if (++i == len) {
            return FMT_LONE_PERCENT;
        }
        long long year = (long long)tm->tm_year + 1900;
        int hour12 = tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12;
        switch (fmt[i]) {
            case 'Y': put_int(o, year, 0, '0'); break;
            case 'y': put_int(o, ((year % 100) + 100) % 100, 2, '0'); break;
            case 'm': put_int(o, tm->tm_mon + 1, 2, '0'); break;
            case 'd': put_int(o, tm->tm_mday, 2, '0'); break;
            case 'e': put_int(o, tm->tm_mday, 2, ' '); break;
            case 'H': put_int(o, tm->tm_hour, 2, '0'); break;
            case 'I': put_int(o, hour12, 2, '0'); break;
            case 'M': put_int(o, tm->tm_min, 2, '0'); break;
            case 'S': put_int(o, tm->tm_sec, 2, '0'); break;
            case 'j': put_int(o, tm->tm_yday + 1, 3, '0'); break;
            case 'p': put(o, tm->tm_hour < 12 ? "AM" : "PM", 2); break;
            case 'a': put(o, DAYS[tm->tm_wday], 3); break;
            case 'A': put(o, DAYS[tm->tm_wday], strlen(DAYS[tm->tm_wday])); break;
            case 'b': put(o, MONTHS[tm->tm_mon], 3); break;
            case 'B': put(o, MONTHS[tm->tm_mon], strlen(MONTHS[tm->tm_mon])); break;
            case '%': put(o, "%", 1); break;
            default:
                *bad = fmt[i];
                return FMT_UNSUPPORTED;
        }
    }
    return o->full ? FMT_TOO_LONG : FMT_OK;
}

/* Raises what format_tm found wrong, in fname's words ("time.format" or "os.date"). */
static int format_error(lua_State *L, const char *fname, fmt_status_t status, char bad) {
    switch (status) {
        case FMT_TOO_LONG:
            return luaL_error(L, "%s: the result is longer than " STR(FORMAT_MAX) " bytes", fname);
        case FMT_LONE_PERCENT:
            return luaL_error(L, "%s: the format ends in a lone %% (write %%%% for a percent sign)", fname);
        default:
            /* A byte that isn't printable ASCII is shown as '?': the error leaves as text (apps_rt.h). */
            return luaL_error(L, "%s: %%%c isn't supported %s", fname, bad >= 0x20 && bad < 0x7F ? bad : '?', SPECIFIERS);
    }
}

/* Pushes fmt[0, len) formatted for tm, or raises. */
static int push_formatted(lua_State *L, const char *fname, const char *fmt, size_t len, const struct tm *tm) {
    out_t o = {.len = 0, .full = false};
    char bad = '?';
    fmt_status_t status = format_tm(&o, fmt, len, tm, &bad);
    if (status != FMT_OK) {
        return format_error(L, fname, status, bad);
    }
    lua_pushlstring(L, o.text, o.len);
    return 1;
}

static int time_format(lua_State *L) {
    apps_vm_check(L);
    if (lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "time.format: fmt must be a string, got %s", luaL_typename(L, 1));
    }
    size_t len;
    const char *fmt = lua_tolstring(L, 1, &len);
    int64_t ms = arg_ms(L, 2, "time.format");
    struct tm tm;
    if (!convert(floor_seconds(ms), false, &tm)) {
        return luaL_error(L, "time.format: ms is out of range " CALENDAR_YEARS);
    }
    return push_formatted(L, "time.format", fmt, len, &tm);
}

/* ---- os ---- */

/* os.time's table fields, as Lua's own reads them: year, month and day must be there; the others have defaults. */
static int os_time_field(lua_State *L, const char *key, int dflt, int delta) {
    int is_integer;
    int type = lua_getfield(L, -1, key);
    lua_Integer res = lua_tointegerx(L, -1, &is_integer);
    if (!is_integer) {
        if (type != LUA_TNIL) {
            return luaL_error(L, "os.time: field '%s' is not an integer", key);
        } else if (dflt < 0) {
            return luaL_error(L, "os.time: field '%s' missing in date table", key);
        }
        res = dflt;
    } else if (!(res >= 0 ? res - delta <= INT_MAX : INT_MIN + delta <= res)) {
        return luaL_error(L, "os.time: field '%s' is out-of-bound", key);
    } else {
        res -= delta;
    }
    lua_pop(L, 1);
    return (int)res;
}

static int64_t magnitude(int v) {
    return v < 0 ? -(int64_t)v : v;
}

static int os_time_isdst(lua_State *L) {
    int res = lua_getfield(L, -1, "isdst") == LUA_TNIL ? -1 : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return res;
}

static int os_time(lua_State *L) {
    apps_vm_check(L);
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, now_seconds());
        return 1;
    }
    if (lua_type(L, 1) != LUA_TTABLE) {
        return luaL_error(L, "os.time: the argument must be a table like {year = 2026, month = 1, day = 1}, got %s",
                          luaL_typename(L, 1));
    }
    lua_settop(L, 1);
    struct tm ts;
    memset(&ts, 0, sizeof ts);
    ts.tm_year = os_time_field(L, "year", -1, 1900);
    ts.tm_mon = os_time_field(L, "month", -1, 1);
    ts.tm_mday = os_time_field(L, "day", -1, 0);
    ts.tm_hour = os_time_field(L, "hour", 12, 0);
    ts.tm_min = os_time_field(L, "min", 0, 0);
    ts.tm_sec = os_time_field(L, "sec", 0, 0);
    ts.tm_isdst = os_time_isdst(L);
    int64_t days = magnitude(ts.tm_mday) + magnitude(ts.tm_hour) / 24 + magnitude(ts.tm_min) / 1440 +
                   magnitude(ts.tm_sec) / 86400;
    if (days > OS_TIME_MAX_DAYS) {
        return luaL_error(L, "os.time: day, hour, min and sec add up to more than " STR(OS_TIME_MAX_DAYS) " days");
    }
    time_t t = mktime(&ts);  /* local time; it normalizes ts, which goes back into the table, as Lua's does */
    fill_fields(L, &ts, true);
    if (t == (time_t)-1 || t < CALENDAR_MIN_S || t > CALENDAR_MAX_S) {
        return luaL_error(L, "os.time: the time can't be represented " CALENDAR_YEARS);
    }
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

static int os_date(lua_State *L) {
    apps_vm_check(L);
    size_t len;
    const char *fmt = luaL_optlstring(L, 1, DEFAULT_DATE_FORMAT, &len);
    int64_t seconds = lua_isnoneornil(L, 2) ? now_seconds() : (int64_t)luaL_checkinteger(L, 2);
    bool utc = len > 0 && fmt[0] == '!';
    if (utc) {
        fmt++;
        len--;
    }
    struct tm tm;
    if (!convert(seconds, utc, &tm)) {
        return luaL_error(L, "os.date: the time is out of range " CALENDAR_YEARS);
    }
    if (len == 2 && memcmp(fmt, "*t", 2) == 0) {
        push_fields(L, &tm, true);
        return 1;
    }
    return push_formatted(L, "os.date", fmt, len, &tm);
}

static int os_clock(lua_State *L) {
    apps_vm_check(L);
    lua_pushnumber(L, (lua_Number)apps_port_mono_ms() / 1000.0);
    return 1;
}

static int os_difftime(lua_State *L) {
    apps_vm_check(L);
    if (lua_type(L, 1) != LUA_TNUMBER || lua_type(L, 2) != LUA_TNUMBER) {
        char a[32], b[32];
        return luaL_error(L, "os.difftime: both arguments must be numbers, got %s and %s", got(L, 1, a, sizeof a),
                          got(L, 2, b, sizeof b));
    }
    lua_pushnumber(L, lua_tonumber(L, 1) - lua_tonumber(L, 2));
    return 1;
}

/* ---- timer ---- */

/* What a handle holds: the slot of the runtime's table and the gen the timer was armed with, so a handle to a timer that has
 * fired or been cancelled names nothing, whatever took its slot after. */
typedef struct {
    uint32_t gen;
    uint16_t slot;
} timer_handle_t;

#define TIMER_MS_RULE "ms must be a whole number from " STR(APPS_TIMER_MIN_MS) " to " STR(APPS_TIMER_MAX_MS)

/* timer.after and timer.every, upvalue 1 the runtime. The handle is made first and the timer armed last, which can't fail:
 * a refused allocation in between leaves nothing armed. */
static int arm(lua_State *L, bool every) {
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = every ? "timer.every" : "timer.after";
    lua_Integer ms = 0;
    if (!whole(L, 1, &ms) || ms < APPS_TIMER_MIN_MS || ms > APPS_TIMER_MAX_MS) {
        return luaL_error(L, "%s: " TIMER_MS_RULE, name);
    }
    if (lua_type(L, 2) != LUA_TFUNCTION) {
        return luaL_error(L, "%s: fn must be a function, got %s", name, luaL_typename(L, 2));
    }
    if (apps_rt_timer_count(rt) >= APPS_TIMERS_MAX) {
        return luaL_error(L, "timer: no more than " STR(APPS_TIMERS_MAX) " timers per app");
    }
    timer_handle_t *h = lua_newuserdatauv(L, sizeof *h, 0);
    luaL_setmetatable(L, TIMER_MT);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    uint32_t gen;
    int slot = apps_rt_timer_add(rt, ref, ms, every, &gen);
    h->gen = gen;
    h->slot = (uint16_t)slot;
    return 1;
}

static int timer_after(lua_State *L) {
    apps_vm_check(L);
    return arm(L, false);
}

static int timer_every(lua_State *L) {
    apps_vm_check(L);
    return arm(L, true);
}

/* timer.cancel(h) and h:cancel(), upvalue 1 the runtime: nothing to do for a timer that is gone. The registry reference is
 * given back through the thread that called (a coroutine's own, not the main thread's, which may be mid-call). */
static int timer_cancel(lua_State *L) {
    apps_vm_check(L);
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    const timer_handle_t *h = luaL_testudata(L, 1, TIMER_MT);
    if (h == NULL) {
        return luaL_error(L, "timer.cancel: needs a timer from timer.after or timer.every, got %s", luaL_typename(L, 1));
    }
    int ref;
    if (apps_rt_timer_take(rt, h->slot, h->gen, &ref)) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    return 0;
}

static int timer_tostring(lua_State *L) {
    apps_vm_check(L);
    const timer_handle_t *h = luaL_testudata(L, 1, TIMER_MT);
    if (h == NULL) {
        return luaL_error(L, "timer: tostring needs a timer");
    }
    lua_pushfstring(L, "timer#%d", (int)h->slot);
    return 1;
}

/* ---- The modules ---- */

static const luaL_Reg apps_time_funcs[] = {
    {"now", time_now},
    {"mono", time_mono},
    {"synced", time_synced},
    {"date", time_date},
    {"format", time_format},
    {NULL, NULL},
};

static const luaL_Reg apps_os_funcs[] = {
    {"time", os_time},
    {"date", os_date},
    {"clock", os_clock},
    {"difftime", os_difftime},
    {NULL, NULL},
};

static const luaL_Reg apps_timer_funcs[] = {
    {"after", timer_after},
    {"every", timer_every},
    {"cancel", timer_cancel},
    {NULL, NULL},
};

static const luaL_Reg apps_timer_methods[] = {
    {"cancel", timer_cancel},
    {NULL, NULL},
};

static const luaL_Reg apps_timer_meta[] = {
    {"__tostring", timer_tostring},
    {NULL, NULL},
};

/* Runs under lua_pcall: the three globals and the timer handle's metatable. */
static int open_svc(lua_State *L) {
    apps_rt_t *rt = lua_touserdata(L, 1);
    lua_settop(L, 0);

    lua_createtable(L, 0, 5);
    luaL_setfuncs(L, apps_time_funcs, 0);
    lua_setglobal(L, "time");

    lua_createtable(L, 0, 4);
    luaL_setfuncs(L, apps_os_funcs, 0);
    lua_setglobal(L, "os");

    /* Protected: getmetatable gives the name, never the table (X17: a finalizer put on it would run with the hooks off). */
    luaL_newmetatable(L, TIMER_MT);
    lua_pushliteral(L, TIMER_MT);
    lua_setfield(L, -2, "__metatable");
    lua_createtable(L, 0, 1);  /* the methods */
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_timer_methods, 1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, apps_timer_meta, 0);
    lua_pop(L, 1);

    lua_createtable(L, 0, 3);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_timer_funcs, 1);
    lua_setglobal(L, "timer");
    return 0;
}

void apps_svc_open(lua_State *L, apps_rt_t *rt) {
    int top = lua_gettop(L);
    if (lua_checkstack(L, 3)) {
        lua_pushcfunction(L, open_svc);
        lua_pushlightuserdata(L, rt);
        /* Protected, so a heap that refuses doesn't abort: the VM is then out of memory and the next apps_vm_call says so. */
        (void)lua_pcall(L, 1, 0, 0);
    }
    lua_settop(L, top);
}
