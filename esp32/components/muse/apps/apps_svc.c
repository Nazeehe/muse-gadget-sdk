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

#include "apps_alarm.h"
#include "apps_ask.h"
#include "apps_json.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_vm.h"
#include "apps_vmio.h"
#include "lauxlib.h"
#include "lua.h"

/*
 * The `time`, `os`, `timer`, `alarm`, `sound`, `store` and `sys` modules (apps_svc.h says what they do). Every binding calls apps_vm_check first and runs
 * inside a call of the app, so what allocates in the app's heap (a table, a string, a handle, a registry reference) may be
 * refused and ends the app out of memory, as everywhere else. The clocks are the port's (apps_port.h): the wall clock is the
 * port's epoch, and the zone is the C library's (TZ), which the watch sets on the gadget (muse_clock_build) and apphost sets
 * to UTC. What a timer is lives in the runtime; the handle here is a slot and a gen. An alarm is the system's (apps_alarm.h):
 * the port keeps it, named by the app's id and a name of the app's, and there is no handle. The store's values are the runtime's
 * (apps_rt.h: a cache of each value's JSON, saved through the port), and so are sys.show's rule and sys.log, which is print; the
 * battery, the Wi-Fi, the front, the screen's keep-awake and the exit are the port's (apps_port.h). None of them makes a handle:
 * what store.get, sys.battery and sys.wifi return is plain tables, afresh each time.
 *
 * Stack, under app code: the formatter's output buffer (136 bytes) and a struct tm are the largest frames, in the bindings
 * of time.format and os.date.
 */

#define STR_(x) #x
#define STR(x) STR_(x)

#define TIMER_MT "muse.timer"

/* The store: a lookup walks this many keys for an instruction of the call's budget (no count hook fires inside it), and the limit as the
 * message says it. */
#define STORE_KEYS_PER_INSTR 32
#define STORE_KB 16
#define STORE_FULL_MESSAGE "store: more than " STR(STORE_KB) " KB stored; remove keys you don't need"
_Static_assert(STORE_KB * 1024 == APPS_STORE_MAX_BYTES, "the message says the limit in whole KB");

/* A network name is at most this long (MUSE_SSID_MAX), and the API's number is sys.api. */
#define SSID_MAX 32
#define SYS_API 1

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
 * size). picolibc's mktime goes through the days it is given a month at a time, about 0.1 us a month on the board (3.2 ns a
 * day, measured: below), so 2^31 days would hold the app for about seven seconds, past the supervisor's three. This is
 * about 8200 years and costs 10 ms at most. Month and year offsets walk nothing: the library takes the months out in a division,
 * and the years it counts are those between 1970 and the date, which the calendar bounds (below). */
#define OS_TIME_MAX_DAYS 3000000
/* The days of that walk a VM instruction pays for. The board's mktime takes about 3.2 ns a day (10 ms for 3.16 M days, measured),
 * and an instruction of the VM about 0.4 us (2.5 M a second): about 120, rounded down to 100, so that a table at the bound costs
 * 30,001 instructions of the call's 1 M, and a callback can make about 33 of them. */
#define OS_TIME_DAYS_PER_INSTR 100
#define CALENDAR_MIN_YEAR 0
#define CALENDAR_MAX_YEAR 9999

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
    set_int(L, "month", (lua_Integer)tm->tm_mon + 1);
    set_int(L, "day", tm->tm_mday);
    set_int(L, "hour", tm->tm_hour);
    set_int(L, "min", tm->tm_min);
    set_int(L, "sec", tm->tm_sec);
    set_int(L, "wday", (lua_Integer)tm->tm_wday + 1);
    set_int(L, "yday", (lua_Integer)tm->tm_yday + 1);
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

static int os_time_unrepresentable(lua_State *L) {
    return luaL_error(L, "os.time: the time can't be represented " CALENDAR_YEARS);
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
    /* The year the month and year fields name (a month past 11 or before 0 moves it), in 64 bits: the C library takes ints, and
     * a year it can't hold or a month of 2^31 is for it to wrap on. The days move it by at most days / 365 years (and two more
     * for the carries): a date that cannot be in the calendar even so is refused here, as it is below when mktime has put
     * it outside. */
    int64_t months = ts.tm_mon;
    int64_t year = (int64_t)ts.tm_year + 1900 + months / 12 - (months % 12 < 0 ? 1 : 0);
    int64_t reach = days / 365 + 2;
    if (year - reach > CALENDAR_MAX_YEAR || year + reach < CALENDAR_MIN_YEAR) {
        return os_time_unrepresentable(L);
    }
    /* mktime walks the days in C, where no count hook sees it: they are charged to the call before it does (a call over its
     * budget, or killed, stops here), so that an app that does it over and over is stopped, and counts in the duty cycle. */
    apps_vm_charge(L, (unsigned)(days / OS_TIME_DAYS_PER_INSTR) + 1);
    time_t t = mktime(&ts);  /* local time; it normalizes ts, which goes back into the table, as Lua's does */
    fill_fields(L, &ts, true);
    if (t == (time_t)-1 || t < CALENDAR_MIN_S || t > CALENDAR_MAX_S) {
        return os_time_unrepresentable(L);
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
    if (slot < 0) {  /* the count was checked above and no app code runs between (no finalizers): but never a handle to nothing */
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "timer: no more than " STR(APPS_TIMERS_MAX) " timers per app");
    }
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

/* ---- alarm ---- */

/* Raises the module's message unless the manifest gives the app its permission. Upvalue 1 is the runtime. The module and its
 * permission are named alike ("alarm", "sound", "store"), except for sys's one (need_as). */
static void need_as(lua_State *L, unsigned perm, const char *module, const char *permission) {
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    if (!apps_rt_permits(rt, perm)) {
        luaL_error(L, "%s: add \"%s\" to \"permissions\" in manifest.json", module, permission);
    }
}

static void need(lua_State *L, unsigned perm, const char *module) {
    need_as(L, perm, module, module);
}

#define ALARM_MS_RULE "ms must be a whole number from 1 to " STR(APPS_ALARM_MAX_MS)

/* The name at idx: a string of 1 to 23 characters of a-z, 0-9 and _ (the rule is apps_alarm.h's, which the table applies too). A number
 * isn't taken for text, and a string with a NUL in it isn't a name: the rule reads to the first one. */
static const char *alarm_name(lua_State *L, int idx) {
    size_t len = 0;
    const char *name = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &len) : NULL;
    if (name == NULL || strlen(name) != len || !apps_alarm_name_valid(name)) {
        luaL_error(L, "alarm: name must be 1-" STR(APPS_ALARM_NAME_MAX) " characters of a-z, 0-9 and _");
    }
    return name;
}

/* alarm.after's options, the table at idx: sound and wake, each true or false, both true unless it says so. A key that is
 * neither is an error, as in the ui module. The table is read raw (no metamethod, so no app code runs here) and walked with
 * lua_next, which no count hook sees: charged by the slots it visits. */
static void alarm_options(lua_State *L, int idx, bool *sound, bool *wake) {
    if (lua_type(L, idx) != LUA_TTABLE) {
        luaL_error(L, "alarm.after: options must be a table like {sound = true, wake = true}, got %s", luaL_typename(L, idx));
    }
    apps_vm_charge(L, apps_vm_table_slots(L, idx) + 1);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        size_t len = 0;
        const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &len) : NULL;
        bool *option = key == NULL ? NULL : (len == 5 && memcmp(key, "sound", 5) == 0) ? sound :
                       (len == 4 && memcmp(key, "wake", 4) == 0) ? wake : NULL;
        if (option == NULL) {
            char shown[24 + 4];
            if (key != NULL) {
                apps_text_shown(key, len, 24, shown);
            } else {
                snprintf(shown, sizeof shown, "<%s>", luaL_typename(L, -2));
            }
            luaL_error(L, "alarm.after: unknown option '%s' (valid: sound, wake)", shown);
        }
        if (lua_type(L, -1) != LUA_TBOOLEAN) {
            luaL_error(L, "alarm.after: %s must be true or false, got %s", option == sound ? "sound" : "wake",
                       luaL_typename(L, -1));
        }
        *option = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);  /* the value; the key stays for the next step */
    }
}

/* alarm.after(name, ms, {sound, wake}), upvalue 1 the runtime. The limits are the app's (4) and the gadget's (8): the port
 * enforces both, and this says which, as the first is checked here (a name already armed is replaced, so it is no new one). */
static int alarm_after(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_ALARM, "alarm");
    const char *name = alarm_name(L, 1);
    lua_Integer ms = 0;
    if (!whole(L, 2, &ms) || ms < 1 || ms > APPS_ALARM_MAX_MS) {
        return luaL_error(L, "alarm.after: " ALARM_MS_RULE);
    }
    bool sound = true, wake = true;
    if (!lua_isnoneornil(L, 3)) {
        alarm_options(L, 3, &sound, &wake);
    }
    const char *app = apps_rt_id(lua_touserdata(L, lua_upvalueindex(1)));
    char names[APPS_ALARMS_PER_APP][APPS_ALARM_NAME_MAX + 1];
    int64_t left[APPS_ALARMS_PER_APP];
    if (apps_port_alarm_remaining(app, name) < 0 && apps_port_alarm_list(app, names, left) >= APPS_ALARMS_PER_APP) {
        return luaL_error(L, "alarm: no more than " STR(APPS_ALARMS_PER_APP) " alarms per app");
    }
    apps_alarm_result_t result = apps_port_alarm_arm(app, name, ms, sound, wake);
    if (result == APPS_ALARM_NO_MEMORY) {
        return luaL_error(L, "alarm: the gadget is short of memory; try again");
    }
    if (result != APPS_ALARM_ARMED) {
        return luaL_error(L, "alarm: the gadget has no free alarm slots (" STR(APPS_ALARMS_MAX) " in all); cancel one");
    }
    return 0;
}

/* alarm.cancel(name): nothing to do for one that isn't armed (or has gone). */
static int alarm_cancel(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_ALARM, "alarm");
    const char *name = alarm_name(L, 1);
    apps_port_alarm_cancel(apps_rt_id(lua_touserdata(L, lua_upvalueindex(1))), name);
    return 0;
}

/* alarm.remaining(name): the milliseconds left as an integer, or nil. */
static int alarm_remaining(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_ALARM, "alarm");
    const char *name = alarm_name(L, 1);
    int64_t left = apps_port_alarm_remaining(apps_rt_id(lua_touserdata(L, lua_upvalueindex(1))), name);
    if (left < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, left);
    }
    return 1;
}

/* alarm.list(): {{name = ..., remaining = ...}, ...}, the earliest first. */
static int alarm_list(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_ALARM, "alarm");
    char names[APPS_ALARMS_PER_APP][APPS_ALARM_NAME_MAX + 1];
    int64_t left[APPS_ALARMS_PER_APP];
    int n = apps_port_alarm_list(apps_rt_id(lua_touserdata(L, lua_upvalueindex(1))), names, left);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, names[i]);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, left[i]);
        lua_setfield(L, -2, "remaining");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ---- sound ---- */

/* sound.chirp(): one chirp, with the "sound" permission. */
static int sound_chirp(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_SOUND, "sound");
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

/* ---- store ---- */

/* The key at idx: a string of 1 to 32 characters (apps_rt_store_key_valid). A number isn't taken for text. */
static const char *store_key(lua_State *L, int idx, size_t *len) {
    const char *key = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, len) : NULL;
    if (key == NULL || !apps_rt_store_key_valid(key, *len)) {
        luaL_error(L, "store: key must be a string of 1 to " STR(APPS_STORE_KEY_MAX) " characters");
    }
    return key;
}

/* store.get(key, default), upvalue 1 the runtime: the value, decoded afresh from its text (so it is a copy, and what the app does to
 * it changes nothing the store holds); the default, as it is, for a key that isn't there. */
static int store_get(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_STORE, "store");
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    size_t key_len = 0;
    const char *key = store_key(L, 1, &key_len);
    apps_vm_charge(L, apps_rt_store_count(rt) / STORE_KEYS_PER_INSTR);  /* the walk over the keys, which no count hook sees */
    size_t len;
    const char *text = apps_rt_store_get(rt, key, key_len, &len);
    if (text == NULL) {
        lua_settop(L, 2);  /* the default, nil if there is none */
        return 1;
    }
    char bare[96];
    if (!apps_json_push(L, text, len, bare, sizeof bare)) {
        return luaL_error(L, "store: a saved value can't be read: %s", bare);  /* not reachable: the text is the encoder's, or was checked */
    }
    return 1;
}

/* store.set(key, value), upvalue 1 the runtime: the value is encoded (the same JSON as json.encode, with its 16 levels, and its errors, without
 * its prefix) and kept; nil removes the key. A value whose text can never fit in the 16 KB is refused before anything is made for it: a string
 * longer than that at once, and anything else by the encoder, which stops at the limit (apps_json_encode_value_max) and doesn't build a text
 * and a copy of it first. */
static int store_set(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_STORE, "store");
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    size_t key_len = 0;
    const char *key = store_key(L, 1, &key_len);
    apps_vm_charge(L, apps_rt_store_count(rt) / STORE_KEYS_PER_INSTR);
    if (lua_isnoneornil(L, 2)) {
        apps_rt_store_remove(rt, key, key_len);
        return 0;
    }
    lua_settop(L, 2);
    if (lua_type(L, 2) == LUA_TSTRING && lua_rawlen(L, 2) > APPS_STORE_MAX_BYTES) {
        return luaL_error(L, STORE_FULL_MESSAGE);
    }
    char bare[96];
    size_t len = 0;
    char *text = apps_json_encode_value_max(L, 2, apps_port_realloc, APPS_STORE_MAX_BYTES, &len, bare, sizeof bare);
    if (text == NULL) {
        if (strcmp(bare, "out of memory") == 0) {  /* the port's heap, not Lua's (that raises): the same try-again as the alarms' */
            return luaL_error(L, "store.set: the gadget is short of memory; try again");
        }
        if (strcmp(bare, APPS_JSON_TOO_LARGE) == 0) {
            return luaL_error(L, STORE_FULL_MESSAGE);
        }
        return luaL_error(L, "store.set: %s", bare);
    }
    apps_store_result_t result = apps_rt_store_set(rt, key, key_len, text, len);
    apps_port_free(text);
    if (result == APPS_STORE_FULL) {
        return luaL_error(L, STORE_FULL_MESSAGE);
    }
    if (result != APPS_STORE_OK) {  /* APPS_STORE_NO_MEMORY: the key was checked above */
        return luaL_error(L, "store.set: the gadget is short of memory; try again");
    }
    return 0;
}

/* store.remove(key), upvalue 1 the runtime: nothing for a key that isn't there. */
static int store_remove(lua_State *L) {
    apps_vm_check(L);
    need(L, APPS_PERM_STORE, "store");
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    size_t key_len = 0;
    const char *key = store_key(L, 1, &key_len);
    apps_vm_charge(L, apps_rt_store_count(rt) / STORE_KEYS_PER_INSTR);
    apps_rt_store_remove(rt, key, key_len);
    return 0;
}

/* ---- sys ---- */

/* sys.battery(): {pct, charging, usb}; pct is always a number, 0 to 100, and -1 when the gadget can't tell (the port's convention), so that
 * `sys.battery().pct < 20` and a format of it work on a gadget that has no reading yet. */
static int sys_battery(lua_State *L) {
    apps_vm_check(L);
    apps_battery_t battery = apps_port_battery();
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, battery.pct < 0 ? -1 : battery.pct > 100 ? 100 : battery.pct);
    lua_setfield(L, -2, "pct");
    lua_pushboolean(L, battery.charging);
    lua_setfield(L, -2, "charging");
    lua_pushboolean(L, battery.usb);
    lua_setfield(L, -2, "usb");
    return 1;
}

/* sys.wifi(): {connected, ssid}, the ssid "" when it isn't connected. A network's name is bytes from the air, so it is cleaned as any
 * text from outside is (apps_text_clean). */
static int sys_wifi(lua_State *L) {
    apps_vm_check(L);
    char ssid[SSID_MAX + 1] = "";
    bool connected = apps_port_wifi(ssid, sizeof ssid);
    ssid[SSID_MAX] = '\0';
    size_t len = connected ? apps_text_clean(ssid, strlen(ssid), false) : 0;
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, connected);
    lua_setfield(L, -2, "connected");
    lua_pushlstring(L, ssid, len);
    lua_setfield(L, -2, "ssid");
    return 1;
}

/* sys.show(), upvalue 1 the runtime: ask for the front, in message() and alarm() and for 10 s after a tap (apps_rt_show_allowed). */
static int sys_show(lua_State *L) {
    apps_vm_check(L);
    if (!apps_rt_show_allowed(lua_touserdata(L, lua_upvalueindex(1)))) {
        return luaL_error(L, "sys.show: allowed only in message() or alarm(), or within 10 s of a tap");
    }
    apps_port_show();
    return 0;
}

/* sys.keep_awake(on), upvalue 1 the runtime: needs the "keep_awake" permission, which the message names under sys. */
static int sys_keep_awake(lua_State *L) {
    apps_vm_check(L);
    need_as(L, APPS_PERM_KEEP_AWAKE, "sys", "keep_awake");
    if (lua_type(L, 1) != LUA_TBOOLEAN) {
        return luaL_error(L, "sys.keep_awake: on must be true or false, got %s", luaL_typename(L, 1));
    }
    apps_port_keep_awake(lua_toboolean(L, 1) != 0);
    return 0;
}

/* The gadget's own modules, what sys.has says yes to: not Lua's libraries, and none of the ones that aren't there yet. */
static const char *const HAS[] = {"ui", "time", "timer", "alarm", "store", "sound", "json", "sys", "vm", "muse"};

/* sys.has(name): whether the gadget has the module of that name. */
static int sys_has(lua_State *L) {
    apps_vm_check(L);
    if (lua_type(L, 1) != LUA_TSTRING) {
        return luaL_error(L, "sys.has: name must be a string, got %s", luaL_typename(L, 1));
    }
    size_t len;
    const char *name = lua_tolstring(L, 1, &len);
    bool has = false;
    for (size_t i = 0; i < sizeof HAS / sizeof HAS[0]; i++) {
        has = has || (strlen(HAS[i]) == len && memcmp(HAS[i], name, len) == 0);
    }
    lua_pushboolean(L, has);
    return 1;
}

static lua_Integer kb(size_t bytes) {
    return (lua_Integer)((bytes + 1023) / 1024);
}

/* sys.mem(): {used_kb, peak_kb, limit_kb} of the Lua heap, as the status reports it (rounded up). */
static int sys_mem(lua_State *L) {
    apps_vm_check(L);
    apps_rt_t *rt = lua_touserdata(L, lua_upvalueindex(1));
    const apps_vm_t *vm = apps_rt_vm(rt);
    lua_Integer used = kb(apps_vm_mem_used(vm)), peak = kb(apps_vm_mem_peak(vm));
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, used);
    lua_setfield(L, -2, "used_kb");
    lua_pushinteger(L, peak);
    lua_setfield(L, -2, "peak_kb");
    lua_pushinteger(L, (lua_Integer)apps_rt_memory_kb(rt));
    lua_setfield(L, -2, "limit_kb");
    return 1;
}

/* sys.exit(): the app ends once this callback has returned (apps_port_exit). */
static int sys_exit(lua_State *L) {
    apps_vm_check(L);
    apps_port_exit();
    return 0;
}

/* sys.log(...): print. */
static int sys_log(lua_State *L) {
    apps_vm_check(L);
    return apps_rt_print(L);
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

static const luaL_Reg apps_alarm_funcs[] = {
    {"after", alarm_after},
    {"cancel", alarm_cancel},
    {"remaining", alarm_remaining},
    {"list", alarm_list},
    {NULL, NULL},
};

static const luaL_Reg apps_sound_funcs[] = {
    {"chirp", sound_chirp},
    {"stop", sound_stop},
    {NULL, NULL},
};

static const luaL_Reg apps_store_funcs[] = {
    {"get", store_get},
    {"set", store_set},
    {"remove", store_remove},
    {NULL, NULL},
};

static const luaL_Reg apps_sys_funcs[] = {
    {"battery", sys_battery},
    {"wifi", sys_wifi},
    {"show", sys_show},
    {"keep_awake", sys_keep_awake},
    {"has", sys_has},
    {"mem", sys_mem},
    {"exit", sys_exit},
    {"log", sys_log},
    {NULL, NULL},
};

static const luaL_Reg apps_timer_meta[] = {
    {"__tostring", timer_tostring},
    {NULL, NULL},
};

/* Runs under lua_pcall: the seven globals and the timer handle's metatable, then the vm module (apps_vmio.h). */
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

    /* alarm and sound make no handle: nothing to protect, but the runtime, which every one of them asks for the app's id and
     * what its manifest permits. */
    lua_createtable(L, 0, 4);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_alarm_funcs, 1);
    lua_setglobal(L, "alarm");

    lua_createtable(L, 0, 2);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_sound_funcs, 1);
    lua_setglobal(L, "sound");

    /* store and sys make no handle either: plain tables of functions, which take the runtime for the app's id, permissions and
     * cache. There is no `log` global: logging is print and sys.log. */
    lua_createtable(L, 0, 3);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_store_funcs, 1);
    lua_setglobal(L, "store");

    lua_createtable(L, 0, 9);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_sys_funcs, 1);
    lua_pushinteger(L, SYS_API);
    lua_setfield(L, -2, "api");
    lua_setglobal(L, "sys");

    apps_vmio_open(L, rt);  /* `vm` and its watch handle's metatable */
    apps_ask_open(L, rt);   /* `muse` (apps_ask.h) */
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
