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
 * Checks apps_rt, the runtime of one Muse app, through its C interface. Usage: muse_apps_rt_harness <case>,
 * one case per test of HarnessTest in tests/test_muse_apps_rt.py. Exits 0 when the case holds and prints
 * what it got when it doesn't. What a script of apphost can't reach is here: a heap that refuses at every
 * step, the cuts of err and of the log tail, and the return value of every entry point.
 *
 * The port below is the harness's own: a counting allocator that can refuse every growth request from the
 * Nth on (the leak check, since LeakSanitizer doesn't run on macOS: every case ends at 0 bytes), a clock
 * that stands still, and a record of the lines the runtime logs. Texts the runtime reads (data, the main
 * file) are exact-size heap blocks, and err is a block of exactly err_len bytes, so a read or write past
 * either is an AddressSanitizer report.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

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

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The exact messages (apps_rt.h and apps_vm.h). */
#define NOT_RUNNING "the app is not running"
#define BUDGET_CALL "stopped: ran more than 2000000 VM instructions in one callback (use timer.every for long work)"
#define BUDGET_START "stopped: ran more than 5000000 VM instructions in one callback (use timer.every for long work)"

/* e-acute (2 bytes), euro (3), a grinning face (4), then " end": 13 bytes. */
#define WIDE "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
#define WIDE_END WIDE " end"

/* ---- The port ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;            /* bytes handed out and not freed */
static long growths;           /* growth requests so far */
static long refuse_from = -1;  /* refuse every growth request from this one on (counted in growths); -1: none */

void *apps_port_realloc(void *ptr, size_t n) {
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old) {
        growths++;
        if (refuse_from >= 0 && growths >= refuse_from) {
            return NULL;
        }
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    held = held - old + n;
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        free(b);
    }
}

/* The clocks and the screen's sleep, which a case sets (the monotonic clock stands still at 0 and the wall clock isn't set,
 * as they were before the time module). */
static int64_t port_mono_ms, port_epoch_ms;
static bool port_epoch_valid, port_dark;

int64_t apps_port_mono_ms(void) {
    return port_mono_ms;
}

bool apps_port_epoch_ms(int64_t *out) {
    if (!port_epoch_valid) {
        return false;
    }
    *out = port_epoch_ms;
    return true;
}

bool apps_port_dark(void) {
    return port_dark;
}

void apps_port_ui_lock(void) {}

bool apps_port_ui_trylock(int ms) {
    (void)ms;
    return true;
}

void apps_port_ui_unlock(void) {}

void apps_port_sha256(const void *data, size_t len, uint8_t out[32]) {
    (void)data;
    (void)len;
    memset(out, 0, 32);
}

/* What apps_port_log was given: every line, and whether each came with the package's id. */
static char **logged;
static size_t logged_n, logged_room;
static bool log_bad_id;

void apps_port_log(const char *app_id, const char *line) {
    if (logged_n == logged_room) {
        logged_room = logged_room != 0 ? 2 * logged_room : 256;
        logged = realloc(logged, logged_room * sizeof *logged);
        if (logged == NULL) {
            fputs("out of memory in the harness\n", stderr);
            exit(2);
        }
    }
    if (strcmp(app_id, "app") != 0) {
        log_bad_id = true;
    }
    logged[logged_n++] = strdup(line);
}

static void forget_logged(void) {
    for (size_t i = 0; i < logged_n; i++) {
        free(logged[i]);
    }
    logged_n = 0;
    log_bad_id = false;
}

/* The nth line logged since forget_logged, or "(none)". */
static const char *logged_at(size_t n) {
    return n < logged_n ? logged[n] : "(none)";
}

static void expect_logged_at(int line, const char *const *want, size_t n) {
    bool same = logged_n == n && !log_bad_id;
    for (size_t i = 0; same && i < n; i++) {
        same = strcmp(logged[i], want[i]) == 0;
    }
    CHECK_AT(line, same, "logged %zu lines (want %zu), a wrong id: %d", logged_n, n, log_bad_id);
    if (!same) {
        for (size_t i = 0; i < logged_n || i < n; i++) {
            fprintf(stderr, "  [%zu] got '%s' want '%s'\n", i, logged_at(i), i < n ? want[i] : "(none)");
        }
    }
}
#define expect_logged(...)                                                 \
    do {                                                                   \
        const char *const want_[] = {__VA_ARGS__};                         \
        expect_logged_at(__LINE__, want_, sizeof want_ / sizeof want_[0]); \
    } while (0)

static void expect_no_logs_at(int line) {
    CHECK_AT(line, logged_n == 0, "logged %zu lines, the first '%s'", logged_n, logged_at(0));
}
#define expect_no_logs() expect_no_logs_at(__LINE__)

/* ---- Packages and runtimes ---- */

typedef struct {
    const char *path;
    const char *src;
} file_t;

/* An exact-size heap copy of src[0, len): a read past it is an AddressSanitizer report. */
static char *exact_copy(const char *src, size_t len) {
    char *p = malloc(len != 0 ? len : 1);
    if (p == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    memcpy(p, src, len);
    return p;
}

/* A package called "app" with these files, the first of them the main file, and memory_kb of heap. */
static apps_pkg_t *make_pkg_kb(unsigned memory_kb, const file_t *files, int n) {
    apps_pkg_t *pkg = calloc(1, sizeof *pkg);
    if (pkg == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    snprintf(pkg->manifest.id, sizeof pkg->manifest.id, "app");
    snprintf(pkg->manifest.name, sizeof pkg->manifest.name, "App");
    snprintf(pkg->manifest.main, sizeof pkg->manifest.main, "%s", files[0].path);
    pkg->manifest.memory_kb = (uint16_t)memory_kb;
    pkg->manifest.file_count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        snprintf(pkg->manifest.files[i], sizeof pkg->manifest.files[i], "%s", files[i].path);
        snprintf(pkg->files[i].path, sizeof pkg->files[i].path, "%s", files[i].path);
        pkg->files[i].len = strlen(files[i].src);
        pkg->files[i].data = (uint8_t *)exact_copy(files[i].src, pkg->files[i].len);
    }
    pkg->count = n;
    return pkg;
}

static apps_pkg_t *make_pkg(const char *main_src) {
    const file_t files[] = {{"main.lua", main_src}};
    return make_pkg_kb(256, files, 1);
}

static void free_pkg(apps_pkg_t *pkg) {
    apps_pkg_free(pkg, NULL);
    free(pkg);
}

/* A runtime for main_src and its package (*pkg_out), made and started; start_ok says whether start()
 * must succeed. NULL if it couldn't be made. */
static apps_rt_t *make_rt(apps_pkg_t **pkg_out, const char *main_src, bool start_ok) {
    *pkg_out = make_pkg(main_src);
    char err[256] = "";
    apps_rt_t *rt = apps_rt_new(*pkg_out, err, sizeof err);
    CHECK(rt != NULL, "apps_rt_new failed: %s", err);
    if (rt != NULL) {
        bool started = apps_rt_start(rt);
        CHECK(started == start_ok, "start returned %d (%s)", started, apps_rt_last_error(rt));
    }
    return rt;
}

static apps_rt_t *launch(apps_pkg_t **pkg_out, const char *main_src) {
    return make_rt(pkg_out, main_src, true);
}

/* Frees them and checks that nothing is held. */
static void finish(apps_rt_t *rt, apps_pkg_t *pkg) {
    apps_rt_free(rt);
    free_pkg(pkg);
    forget_logged();
    CHECK(held == 0, "%zu bytes still held", held);
}

/* A message with no data. NULL when it fails; err gets the reason. */
static char *send(apps_rt_t *rt, const char *event, char *err, size_t err_len) {
    return apps_rt_message(rt, event, NULL, 0, err, err_len);
}

/* ---- States and return values ---- */

static const char *const LIFECYCLE =
    "local app = {}\n"
    "function app.start() print('start') end\n"
    "function app.stop() print('stop') end\n"
    "function app.show() print('show') end\n"
    "function app.hide() print('hide') end\n"
    "function app.alarm(name) print('alarm ' .. tostring(name) .. ' ' .. type(name)) end\n"
    "function app.message(event, data) print('message ' .. event .. ' ' .. type(data)) return {ok = true} end\n"
    "return app\n";

static void lifecycle(void) {
    apps_pkg_t *pkg = make_pkg(LIFECYCLE);
    char err[256];
    apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
    CHECK(rt != NULL, "%s", err);
    if (rt == NULL) {
        free_pkg(pkg);
        return;
    }
    CHECK(apps_rt_state(rt) == APPS_STATE_STOPPED, "made: %d", apps_rt_state(rt));
    CHECK(apps_rt_last_error(rt)[0] == '\0' && apps_rt_warnings(rt)[0] == '\0', "'%s' '%s'", apps_rt_last_error(rt),
          apps_rt_warnings(rt));
    expect_no_logs();

    /* Made, not started: nothing runs. */
    char *reply = send(rt, "early", err, sizeof err);
    CHECK(reply == NULL && strcmp(err, NOT_RUNNING) == 0, "message before start: %p '%s'", (void *)reply, err);
    CHECK(!apps_rt_alarm(rt, "early"), "alarm before start");
    apps_rt_show(rt, true);
    apps_rt_show(rt, false);
    apps_rt_stop(rt);
    expect_no_logs();
    CHECK(apps_rt_state(rt) == APPS_STATE_STOPPED, "still stopped: %d", apps_rt_state(rt));

    CHECK(apps_rt_start(rt), "start");
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "started: %d", apps_rt_state(rt));
    CHECK(apps_rt_start(rt), "start when running");  /* true, and nothing runs */
    expect_logged("start");

    apps_rt_show(rt, true);
    apps_rt_show(rt, false);
    CHECK(apps_rt_alarm(rt, "wake"), "alarm");
    CHECK(apps_rt_alarm(rt, NULL), "alarm with no name");
    reply = apps_rt_message(rt, "ping", NULL, 0, err, sizeof err);
    CHECK(reply != NULL && strcmp(reply, "{\"ok\":true}") == 0 && err[0] == '\0', "reply '%s' err '%s'",
          reply != NULL ? reply : "(null)", err);
    apps_port_free(reply);
    reply = apps_rt_message(rt, "ping", "", 0, err, sizeof err);  /* empty data is nil */
    CHECK(reply != NULL, "message with empty data: '%s'", err);
    apps_port_free(reply);
    reply = apps_rt_message(rt, NULL, "{}", 2, err, sizeof err);  /* no event is "" */
    CHECK(reply != NULL, "message with no event: '%s'", err);
    apps_port_free(reply);
    expect_logged("start", "show", "hide", "alarm wake string", "alarm  string", "message ping nil",
                  "message ping nil", "message  table");
    forget_logged();

    apps_rt_stop(rt);
    CHECK(apps_rt_state(rt) == APPS_STATE_STOPPED, "stopped: %d", apps_rt_state(rt));
    apps_rt_stop(rt);
    apps_rt_show(rt, true);
    CHECK(!apps_rt_alarm(rt, "late"), "alarm after stop");
    reply = send(rt, "late", err, sizeof err);
    CHECK(reply == NULL && strcmp(err, NOT_RUNNING) == 0, "message after stop: '%s'", err);
    expect_logged("stop");
    forget_logged();

    CHECK(apps_rt_start(rt), "start again");  /* a stopped runtime starts again, start() and all */
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "started again");
    expect_logged("start");
    CHECK(apps_rt_last_error(rt)[0] == '\0', "'%s'", apps_rt_last_error(rt));
    finish(rt, pkg);
}

/* A source whose handler `crasher` runs `how`; the others print their own name. */
static char *crash_source(const char *crasher, const char *how, char *out, size_t cap) {
    snprintf(out, cap,
             "local app = {}\n"
             "for _, name in ipairs({'start', 'stop', 'show', 'hide', 'alarm', 'message'}) do\n"
             "    app[name] = function() print('ran ' .. name) return {} end\n"
             "end\n"
             "app.%s = function() print('crashing in %s') %s end\n"
             "return app\n",
             crasher, crasher, how);
    return out;
}

/* Makes the handler `name` fail, once the app is running (start fails on its own). */
static void trigger(apps_rt_t *rt, const char *name) {
    if (strcmp(name, "stop") == 0) {
        apps_rt_stop(rt);
    } else if (strcmp(name, "show") == 0) {
        apps_rt_show(rt, true);
    } else if (strcmp(name, "hide") == 0) {
        apps_rt_show(rt, false);
    } else if (strcmp(name, "alarm") == 0) {
        CHECK(!apps_rt_alarm(rt, "a"), "%s: alarm that fails returned true", name);
    }
}

static void crashed(void) {
    static const char *const crashers[] = {"start", "stop", "show", "hide", "alarm", "message"};
    static const char *const hows[] = {"error('boom', 0)", "local t; t.x = 1", "while true do end"};
    static const char *const expect[] = {"boom\nstack traceback:\n\t",
                                         "attempt to index a nil value (local 't')\nstack traceback:\n\t", NULL};
    for (size_t c = 0; c < ARRAY_LEN(crashers); c++) {
        for (size_t h = 0; h < ARRAY_LEN(hows); h++) {
            const char *name = crashers[c];
            char source[512], err[256];
            apps_pkg_t *pkg = make_pkg(crash_source(name, hows[h], source, sizeof source));
            apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
            CHECK(rt != NULL, "%s: %s", name, err);
            if (rt == NULL) {
                free_pkg(pkg);
                continue;
            }
            bool is_start = strcmp(name, "start") == 0, is_message = strcmp(name, "message") == 0;
            CHECK(apps_rt_start(rt) == !is_start, "%s/%zu: start()", name, h);
            if (is_message) {
                char *reply = send(rt, "go", err, sizeof err);
                CHECK(reply == NULL, "%s/%zu: a failing message answered", name, h);
                if (h != 2) {  /* an error() in message() fails the message alone */
                    CHECK(strstr(err, h == 0 ? "boom" : "attempt to index") != NULL && strstr(err, "traceback") == NULL,
                          "'%s'", err);
                    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING && apps_rt_last_error(rt)[0] == '\0',
                          "message error: state %d, last_error '%s'", apps_rt_state(rt), apps_rt_last_error(rt));
                    finish(rt, pkg);
                    continue;
                }
                CHECK(strcmp(err, BUDGET_CALL) == 0, "killed message: '%s'", err);
            } else {
                trigger(rt, name);
            }
            CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED, "%s/%zu: state %d", name, h, apps_rt_state(rt));
            char record[APPS_ERROR_MAX];
            snprintf(record, sizeof record, "%s", apps_rt_last_error(rt));
            if (h == 2) {  /* the kill's reason: start() has the bigger budget */
                CHECK(strcmp(record, is_start ? BUDGET_START : BUDGET_CALL) == 0, "%s: '%s'", name, record);
            } else {
                CHECK(strstr(record, expect[h]) != NULL, "%s/%zu: '%s'", name, h, record);
            }

            /* Final: whatever it is asked, no app code runs, and the record stays. */
            forget_logged();
            apps_rt_show(rt, true);
            apps_rt_show(rt, false);
            apps_rt_stop(rt);
            CHECK(!apps_rt_start(rt), "%s/%zu: start after a crash", name, h);
            CHECK(!apps_rt_alarm(rt, "b"), "%s/%zu: alarm after a crash", name, h);
            char *reply = send(rt, "again", err, sizeof err);
            CHECK(reply == NULL && strcmp(err, record) == 0, "%s/%zu: message after a crash: '%s'", name, h, err);
            reply = apps_rt_message(rt, "again", "{\"a\":1}", 7, err, sizeof err);
            CHECK(reply == NULL && strcmp(err, record) == 0, "%s/%zu: message with data after a crash: '%s'", name, h,
                  err);
            expect_no_logs();
            CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED, "%s/%zu: state %d", name, h, apps_rt_state(rt));
            CHECK(strcmp(apps_rt_last_error(rt), record) == 0, "%s/%zu: the record became '%s'", name, h,
                  apps_rt_last_error(rt));
            char tail[APPS_LOG_RING_BYTES + 1];
            apps_rt_log_tail(rt, 100, tail, sizeof tail);
            CHECK(strstr(tail, "crashing in") != NULL, "%s/%zu: the log lost its lines: '%s'", name, h, tail);
            finish(rt, pkg);
        }
    }
}

/* A kill from another task (apps_vm_kill) lands wherever the app is when it happens. The bindings the runtime
 * gives the app, print and require, check for it first, as every binding does (apps_vm.h), so that a dying app
 * starts no work. kill_me stands in for the other task: it kills at the moment the app calls it. */
static apps_vm_t *kill_target;
#define KILL_REASON "killed from outside"

static int kill_me(lua_State *L) {
    (void)L;
    apps_vm_kill(kill_target, KILL_REASON);
    return 0;
}

static apps_rt_t *make_rt_with_kill(apps_pkg_t **pkg_out, const char *main_src) {
    const file_t files[] = {{"main.lua", main_src}, {"m.lua", "print('module ran') return {}"}};
    *pkg_out = make_pkg_kb(256, files, 2);
    char err[256] = "";
    apps_rt_t *rt = apps_rt_new(*pkg_out, err, sizeof err);
    CHECK(rt != NULL, "%s", err);
    if (rt != NULL) {
        kill_target = apps_rt_vm(rt);
        lua_State *L = apps_vm_lua(kill_target);
        lua_pushcfunction(L, kill_me);
        lua_setglobal(L, "kill_me");
    }
    return rt;
}

static void external_kill(void) {
    static const char *const handlers[] = {"start", "show", "message"};
    static const struct {
        const char *text;
        bool preloaded;  /* main.lua's top level requires m first, so the require under test is a cached one */
    } afters[] = {
        {"print('after the kill')", false},
        {"require('m')", false},
        /* print runs app code */
        {"print(setmetatable({}, {__tostring = function() ran = true return 'x' end}))", false},
        /* A cached require starts no work and charges nothing, so only its own check can stop what follows it. */
        {"require('m') ran = true", true},
    };
    for (size_t h = 0; h < ARRAY_LEN(handlers); h++) {
        for (size_t a = 0; a < ARRAY_LEN(afters); a++) {
            char source[384], err[256] = "";
            snprintf(source, sizeof source,
                     "ran = false\n%slocal app = {}\nfunction app.%s() kill_me() %s end\nreturn app\n",
                     afters[a].preloaded ? "local _ = require('m')\n" : "", handlers[h], afters[a].text);
            apps_pkg_t *pkg;
            apps_rt_t *rt = make_rt_with_kill(&pkg, source);
            if (rt == NULL) {
                free_pkg(pkg);
                continue;
            }
            forget_logged();  /* what main.lua's top level logged: m's own print, for a preloaded m */
            bool started = true;
            if (h == 0) {
                started = apps_rt_start(rt);
            } else {
                CHECK(apps_rt_start(rt), "%s/%zu: start", handlers[h], a);
                if (h == 1) {
                    apps_rt_show(rt, true);
                } else {
                    char *reply = send(rt, "go", err, sizeof err);
                    CHECK(reply == NULL && strcmp(err, KILL_REASON) == 0, "%s/%zu: message: '%s'", handlers[h], a, err);
                }
            }
            CHECK(h != 0 || !started, "%s/%zu: start returned true", handlers[h], a);
            CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILL_REASON) == 0,
                  "%s/%zu: state %d, record '%s'", handlers[h], a, apps_rt_state(rt), apps_rt_last_error(rt));
            expect_no_logs();  /* neither print nor the module that require would have run */
            lua_State *L = apps_vm_lua(kill_target);
            lua_getglobal(L, "ran");
            CHECK(!lua_toboolean(L, -1), "%s/%zu: app code ran after the kill", handlers[h], a);
            lua_pop(L, 1);
            finish(rt, pkg);
        }
    }

    /* Killed while idle: the next call is where it shows, and runs nothing. */
    apps_pkg_t *pkg;
    apps_rt_t *rt = make_rt_with_kill(&pkg, "return {message = function() print('ran') return 1 end}");
    if (rt != NULL) {
        char err[256] = "";
        CHECK(apps_rt_start(rt), "start");
        apps_vm_kill(kill_target, KILL_REASON);
        CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "nothing has run since the kill: state %d", apps_rt_state(rt));
        char *reply = send(rt, "go", err, sizeof err);
        CHECK(reply == NULL && strcmp(err, KILL_REASON) == 0, "message after an idle kill: '%s'", err);
        CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILL_REASON) == 0, "state %d, '%s'",
              apps_rt_state(rt), apps_rt_last_error(rt));
        expect_no_logs();
    }
    finish(rt, pkg);
}

static void null_arguments(void) {
    char err[64] = "(untouched)";
    CHECK(apps_rt_new(NULL, err, sizeof err) == NULL && err[0] != '\0' && strcmp(err, "(untouched)") != 0,
          "new(NULL): '%s'", err);
    CHECK(apps_rt_new(NULL, NULL, 0) == NULL, "new(NULL, NULL)");
    apps_pkg_t *pkg = make_pkg("return {}");
    apps_rt_t *quiet = apps_rt_new(pkg, NULL, 0);
    CHECK(quiet != NULL, "new with no err");
    apps_rt_free(quiet);
    free_pkg(pkg);

    apps_rt_free(NULL);
    CHECK(!apps_rt_start(NULL), "start(NULL)");
    apps_rt_show(NULL, true);
    CHECK(!apps_rt_alarm(NULL, "x"), "alarm(NULL)");
    apps_rt_stop(NULL);
    CHECK(apps_rt_state(NULL) == APPS_STATE_STOPPED, "state(NULL)");
    CHECK(apps_rt_last_error(NULL)[0] == '\0' && apps_rt_warnings(NULL)[0] == '\0', "text of NULL");
    CHECK(apps_rt_vm(NULL) == NULL, "vm(NULL)");
    strcpy(err, "(untouched)");
    CHECK(apps_rt_message(NULL, "x", NULL, 0, err, sizeof err) == NULL && err[0] != '\0' &&
              strcmp(err, "(untouched)") != 0,
          "message(NULL): '%s'", err);
    CHECK(apps_rt_message(NULL, "x", NULL, 0, NULL, 0) == NULL, "message(NULL, no err)");
    char out[16] = "(untouched)";
    CHECK(apps_rt_log_tail(NULL, 5, out, sizeof out) == 0 && out[0] == '\0', "log_tail(NULL): '%s'", out);

    apps_rt_t *rt = launch(&pkg, "return {message = function() print('x') return 1 end}");
    CHECK(apps_rt_log_tail(rt, 5, NULL, 10) == 0, "log_tail with no buffer");
    strcpy(out, "(untouched)");
    CHECK(apps_rt_log_tail(rt, 5, out, 0) == 0 && strcmp(out, "(untouched)") == 0, "log_tail of cap 0 wrote: '%s'", out);
    char *reply = apps_rt_message(rt, "x", NULL, 0, NULL, 0);  /* no err to write */
    CHECK(reply != NULL, "message with no err");
    apps_port_free(reply);
    apps_rt_stop(rt);
    char one[1] = {'Z'};  /* room for nothing but the NUL */
    reply = apps_rt_message(rt, "x", NULL, 0, one, 1);
    CHECK(reply == NULL && one[0] == '\0', "err of 1 byte");
    reply = apps_rt_message(rt, "x", NULL, 0, NULL, 100);
    CHECK(reply == NULL, "err NULL with a length");
    finish(rt, pkg);
}

#define NOT_A_TABLE "main.lua must return a table of handlers: local app = {} \xE2\x80\xA6 return app"

/* A failure in main.lua's top level. The app ran, so there is a runtime to hold what it printed and how it
 * died: CRASHED from the start, whatever it did wrong. No runtime (NULL, and a reason in err) is for what can't
 * be made at all: a main.lua that doesn't compile (nothing of it ran), a heap too small for a VM. */
static void boot_crash(void) {
    static const struct {
        const char *name, *src, *record;
        bool traceback;  /* the record starts with `record` and goes on with the app's frames */
        unsigned memory_kb;
    } crashes[] = {
        {"runtime error", "print('before')\nlocal t\nt.x = 1\nreturn {}",
         "main.lua:3: attempt to index a nil value (local 't')\nstack traceback:\n\t", true, 256},
        {"error()", "print('before') error('boom', 0)", "boom\nstack traceback:\n\t", true, 256},
        {"kill", "print('before') while true do end", BUDGET_START, false, 256},
        {"out of memory", "print('before') local t = {} for i = 1, 1e6 do t[i] = i end",
         "out of memory: this app may use 64 KB (memory_kb in manifest.json)", false, 64},
        {"not a table", "print('before') return 1", NOT_A_TABLE, false, 256},
        {"nothing returned", "print('before')", NOT_A_TABLE, false, 256},
        {"handler that isn't a function", "print('before') return {start = 5}",
         "main.lua returned 'start' as a number; handlers must be functions", false, 256},
        /* Keys 1 and 2 (the array part comes first) are unknown handlers, and warnings, before start is found to
         * be a number: a crashed runtime reports none of them. */
        {"handler that isn't a function, after unknown keys", "print('before') return {1, 2, start = 5}",
         "main.lua returned 'start' as a number; handlers must be functions", false, 256},
    };
    for (size_t i = 0; i < ARRAY_LEN(crashes); i++) {
        const char *name = crashes[i].name;
        const file_t files[] = {{"main.lua", crashes[i].src}};
        forget_logged();
        apps_pkg_t *pkg = make_pkg_kb(crashes[i].memory_kb, files, 1);
        char err[APPS_ERROR_MAX] = "";
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        CHECK(rt != NULL, "%s: no runtime ('%s')", name, err);
        if (rt == NULL) {
            free_pkg(pkg);
            continue;
        }
        char record[APPS_ERROR_MAX];
        snprintf(record, sizeof record, "%s", apps_rt_last_error(rt));
        CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED, "%s: state %d", name, apps_rt_state(rt));
        CHECK(crashes[i].traceback ? strncmp(record, crashes[i].record, strlen(crashes[i].record)) == 0
                                   : strcmp(record, crashes[i].record) == 0,
              "%s: record '%s'", name, record);
        CHECK(err[0] == '\0', "%s: err '%s' (the record is last_error's)", name, err);
        CHECK(apps_rt_warnings(rt)[0] == '\0', "%s: warnings '%s'", name, apps_rt_warnings(rt));
        /* What it printed before failing went out as it was printed, and is in the ring. */
        expect_logged("before");
        char tail[64];
        apps_rt_log_tail(rt, 10, tail, sizeof tail);
        CHECK(strcmp(tail, "before") == 0, "%s: the log holds '%s'", name, tail);
        /* And, as after any crash, nothing runs. */
        forget_logged();
        apps_rt_show(rt, true);
        apps_rt_show(rt, false);
        apps_rt_stop(rt);
        CHECK(!apps_rt_start(rt), "%s: start", name);
        CHECK(!apps_rt_alarm(rt, "a"), "%s: alarm", name);
        char *reply = send(rt, "x", err, sizeof err);
        CHECK(reply == NULL && strcmp(err, record) == 0, "%s: message: '%s'", name, err);
        expect_no_logs();
        CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), record) == 0,
              "%s: state %d, record '%s'", name, apps_rt_state(rt), apps_rt_last_error(rt));
        finish(rt, pkg);
    }

    char nested[256];
    int n = snprintf(nested, sizeof nested, "return ");
    for (int i = 0; i < 60; i++) {
        nested[n++] = '(';
    }
    nested[n++] = '1';
    for (int i = 0; i < 60; i++) {
        nested[n++] = ')';
    }
    nested[n] = '\0';
    static const struct {
        const char *name, *src, *want;
        unsigned memory_kb;
    } refused[] = {
        {"syntax error", "print('x')\nx = = 1", "main.lua:2: unexpected symbol near '='", 256},
        {"<close>", "local x <close> = nil", "main.lua:1: '<close>' variables are not available in Muse apps", 256},
        {"no heap", "return {}", "out of memory: can't make the app's heap of 2 KB", 2},
        /* A runtime error of the parser, not a syntax error: it has no position, and no traceback once cut. */
        {"nested past the C-call room", NULL, "C stack overflow", 256},
    };
    for (size_t i = 0; i < ARRAY_LEN(refused); i++) {
        const file_t files[] = {{"main.lua", refused[i].src != NULL ? refused[i].src : nested}};
        forget_logged();
        apps_pkg_t *pkg = make_pkg_kb(refused[i].memory_kb, files, 1);
        char err[APPS_ERROR_MAX] = "";
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        CHECK(rt == NULL && strcmp(err, refused[i].want) == 0, "%s: %p '%s'", refused[i].name, (void *)rt, err);
        apps_rt_free(rt);
        expect_no_logs();  /* nothing of main.lua ran */
        free_pkg(pkg);
        CHECK(held == 0, "%s: %zu bytes held", refused[i].name, held);
    }
}

static void no_main(void) {
    apps_pkg_t *pkg = make_pkg("return {}");
    pkg->count = 0;  /* the manifest names main.lua, the package has no bytes for it */
    char err[128] = "";
    CHECK(apps_rt_new(pkg, err, sizeof err) == NULL, "new without the main file");
    CHECK(strcmp(err, "main.lua is missing from the package") == 0, "'%s'", err);
    pkg->count = 1;

    /* An empty main file is a file that returns nothing. */
    free(pkg->files[0].data);
    pkg->files[0].data = (uint8_t *)exact_copy("", 0);
    pkg->files[0].len = 0;
    apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
    CHECK(rt != NULL && apps_rt_state(rt) == APPS_STATE_CRASHED && err[0] == '\0' &&
              strstr(apps_rt_last_error(rt), "must return a table of handlers") != NULL,
          "an empty main file: %p '%s'", (void *)rt, err);  /* it ran, and returned nothing: a crashed runtime */
    apps_rt_free(rt);

    /* main names a file that isn't Lua: there is nothing to run. */
    snprintf(pkg->manifest.main, sizeof pkg->manifest.main, "%s", "data.json");
    snprintf(pkg->files[0].path, sizeof pkg->files[0].path, "%s", "data.json");
    CHECK(apps_rt_new(pkg, err, sizeof err) == NULL && strcmp(err, "data.json is not a .lua file") == 0, "'%s'", err);
    free_pkg(pkg);
    CHECK(held == 0, "%zu bytes still held", held);
}

/* A table without handlers is an app that does nothing, and every call is answered. */
static void optional_handlers(void) {
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, "return {}");
    if (rt == NULL) {
        free_pkg(pkg);
        return;
    }
    char err[256];
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "started: %d", apps_rt_state(rt));
    apps_rt_show(rt, true);
    apps_rt_show(rt, false);
    CHECK(apps_rt_alarm(rt, "wake"), "alarm of an app that has no alarm handler");
    char *reply = send(rt, "x", err, sizeof err);
    CHECK(reply == NULL && strcmp(err, "main.lua has no message(event, data) handler") == 0, "message: '%s'", err);
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING && apps_rt_last_error(rt)[0] == '\0', "state %d", apps_rt_state(rt));
    apps_rt_stop(rt);
    CHECK(apps_rt_state(rt) == APPS_STATE_STOPPED, "stopped: %d", apps_rt_state(rt));
    CHECK(apps_rt_start(rt) && apps_rt_state(rt) == APPS_STATE_RUNNING, "started again");
    expect_no_logs();
    finish(rt, pkg);
}

static void state_names(void) {
    CHECK(strcmp(apps_state_name(APPS_STATE_STOPPED), "stopped") == 0, "stopped");
    CHECK(strcmp(apps_state_name(APPS_STATE_RUNNING), "running") == 0, "running");
    CHECK(strcmp(apps_state_name(APPS_STATE_CRASHED), "crashed") == 0, "crashed");
    CHECK(strcmp(apps_state_name(APPS_STATE_HUNG), "hung") == 0, "hung");
    CHECK(strcmp(apps_state_name((apps_state_t)99), "?") == 0, "99");
}

/* Every handler and message path, with the stack checked after each call: a binding that leaves a value
 * behind is a leak in a runtime that lives for hours. */
static void stack_balanced(void) {
    static const char *const source =
        "local app = {}\n"
        "function app.start() print('s') end\n"
        "function app.stop() print('t') end\n"
        "function app.show() end\n"
        "function app.hide() end\n"
        "function app.alarm(n) return n, 2, 3 end\n"
        "function app.message(event, data)\n"
        "    if event == 'error' then error('no') end\n"
        "    if event == 'bad' then return print end\n"
        "    if event == 'many' then return 1, 2, 3 end\n"
        "    if event == 'deep' then return {{{{{{{{1}}}}}}}} end\n"
        "    return {event = event}\n"
        "end\n"
        "return app\n";
    apps_pkg_t *pkg = make_pkg(source);
    char err[256];
    apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
    CHECK(rt != NULL, "%s", err);
    if (rt == NULL) {
        free_pkg(pkg);
        return;
    }
    lua_State *L = apps_vm_lua(apps_rt_vm(rt));
    CHECK(lua_gettop(L) == 0, "after new: %d values", lua_gettop(L));
    CHECK(apps_rt_start(rt), "start");
    CHECK(lua_gettop(L) == 0, "after start: %d values", lua_gettop(L));
    for (int i = 0; i < 3; i++) {
        apps_rt_show(rt, true);
        apps_rt_show(rt, false);
        CHECK(apps_rt_alarm(rt, "n"), "alarm");
        CHECK(lua_gettop(L) == 0, "after show, hide, alarm: %d values", lua_gettop(L));
        static const char *const events[] = {"ok", "error", "bad", "many", "deep"};
        static const char *const data[] = {NULL, "{\"a\":[1,2]}", "{\"a\":", "[]"};
        for (size_t e = 0; e < ARRAY_LEN(events); e++) {
            for (size_t d = 0; d < ARRAY_LEN(data); d++) {
                size_t len = data[d] != NULL ? strlen(data[d]) : 0;
                char *input = data[d] != NULL ? exact_copy(data[d], len) : NULL;
                char *reply = apps_rt_message(rt, events[e], input, len, err, sizeof err);
                apps_port_free(reply);
                free(input);
                CHECK(lua_gettop(L) == 0, "after message %s/%zu: %d values", events[e], d, lua_gettop(L));
                CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "message %s/%zu: state %d (%s)", events[e], d,
                      apps_rt_state(rt), err);
            }
        }
    }
    apps_rt_stop(rt);
    CHECK(lua_gettop(L) == 0, "after stop: %d values", lua_gettop(L));
    finish(rt, pkg);
}

/* ---- err and the log ---- */

/* The length of the most of text that fits n bytes with a NUL, cut on a character boundary. */
static size_t cut_length(const char *text, size_t n) {
    size_t len = strlen(text);
    if (n == 0) {
        return 0;
    }
    size_t keep = len < n - 1 ? len : n - 1;
    if (keep < len) {
        while (keep > 0 && ((unsigned char)text[keep] & 0xC0) == 0x80) {
            keep--;
        }
    }
    return keep;
}

/* Whether s is made of complete UTF-8 characters (the lead byte's length; the texts here are well formed). */
static bool whole_characters(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0';) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0) {
            return false;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        p += n;
    }
    return true;
}

typedef struct {
    apps_rt_t *rt;
    const apps_pkg_t *pkg;
} ask_t;

/* The answers of apps_rt_message on a crashed runtime and of apps_rt_new on a package that fails. */
static void ask_message(const ask_t *a, char *err, size_t err_len) {
    char *reply = apps_rt_message(a->rt, "x", NULL, 0, err, err_len);
    CHECK(reply == NULL, "message on a crashed app answered");
    apps_port_free(reply);
}

static void ask_new(const ask_t *a, char *err, size_t err_len) {
    apps_rt_t *rt = apps_rt_new(a->pkg, err, err_len);
    CHECK(rt == NULL, "new of a failing main.lua answered");
    apps_rt_free(rt);
}

/* Calls ask with an err of exactly each length from 0 to a little over the full text's. */
static void check_every_cut(const char *what, const char *full, void (*ask)(const ask_t *, char *, size_t),
                            const ask_t *a) {
    size_t full_len = strlen(full);
    for (size_t n = 0; n <= full_len + 3; n++) {
        char *err = malloc(n != 0 ? n : 1);
        if (err == NULL) {
            exit(2);
        }
        memset(err, 'Z', n != 0 ? n : 1);
        ask(a, err, n);
        if (n == 0) {
            CHECK(err[0] == 'Z', "%s: err_len 0 wrote a byte", what);
        } else {
            size_t keep = cut_length(full, n);
            const char *nul = memchr(err, '\0', n);
            CHECK(nul == err + keep && memcmp(err, full, keep) == 0, "%s: err_len %zu: the NUL is at %td, want %zu", what, n,
                  nul != NULL ? nul - err : (ptrdiff_t)-1, keep);
            CHECK(nul != NULL && whole_characters(err), "%s: err_len %zu cut inside a character", what, n);
        }
        free(err);
    }
}

static void err_cut(void) {
    /* The crash record of a crashed app: a message with 2, 3 and 4-byte characters, and a traceback. */
    apps_pkg_t *pkg;
    apps_rt_t *rt = make_rt(&pkg, "return {start = function() error('" WIDE_END "', 0) end}", false);
    CHECK(rt != NULL && apps_rt_state(rt) == APPS_STATE_CRASHED, "no crash");
    char record[APPS_ERROR_MAX];
    snprintf(record, sizeof record, "%s", rt != NULL ? apps_rt_last_error(rt) : "");
    CHECK(strncmp(record, WIDE_END "\nstack traceback:\n\t", strlen(WIDE_END "\nstack traceback:\n\t")) == 0, "'%s'",
          record);
    ask_t ask = {rt, pkg};
    check_every_cut("message on a crashed app", record, ask_message, &ask);
    finish(rt, pkg);

    /* The reason apps_rt_new gives for a main.lua that doesn't compile (a failure of its top level is a crashed
     * runtime now, and its record is last_error's): Lua's message, which shows the text of the token. */
    pkg = make_pkg("x = '" WIDE_END "\n");
    char full[APPS_ERROR_MAX] = "";
    CHECK(apps_rt_new(pkg, full, sizeof full) == NULL &&
              strcmp(full, "main.lua:1: unfinished string near ''" WIDE_END "'") == 0,
          "'%s'", full);
    ask_t ask_load = {NULL, pkg};
    check_every_cut("new", full, ask_new, &ask_load);
    free_pkg(pkg);

    /* The handler's own message, which goes without a traceback. */
    rt = launch(&pkg, "return {message = function() error('" WIDE_END "', 0) end}");
    char message[64] = "";
    char *reply = send(rt, "x", message, sizeof message);
    CHECK(reply == NULL && strcmp(message, WIDE_END) == 0, "'%s'", message);
    for (size_t n = 1; n <= strlen(WIDE_END) + 3; n++) {
        char *err = malloc(n);
        reply = send(rt, "x", err, n);
        size_t keep = cut_length(WIDE_END, n);
        CHECK(reply == NULL && memchr(err, '\0', n) == err + keep && whole_characters(err) &&
                  memcmp(err, WIDE_END, keep) == 0,
              "err_len %zu: '%.*s'", n, (int)n, err);
        free(err);
    }
    finish(rt, pkg);
}

/* Line i of the model of case `ring`: (i * 37) % 201 copies of the letter 'A' + i % 26. */
static size_t model_line(long i, char *out) {
    size_t len = (size_t)((i * 37) % 201);
    memset(out, 'A' + (int)(i % 26), len);
    out[len] = '\0';
    return len;
}

static void ring(void) {
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg,
        "return {message = function(event, data)\n"
        "    for i = 1, data.n do print(string.rep(string.char(65 + i % 26), (i * 37) % 201)) end\n"
        "end}");
    char err[128], line[APPS_LOG_LINE_MAX + 1];
    const long n = 3000;
    char *reply = apps_rt_message(rt, "p", "{\"n\":3000}", 10, err, sizeof err);
    CHECK(reply != NULL, "'%s'", err);
    apps_port_free(reply);

    /* Every line went out through the port, as printed. */
    CHECK(logged_n == (size_t)n && !log_bad_id, "%zu lines logged (a wrong id: %d)", logged_n, log_bad_id);
    for (long i = 1; i <= n && (size_t)i <= logged_n; i++) {
        model_line(i, line);
        if (strcmp(logged[i - 1], line) != 0) {
            CHECK(false, "line %ld: '%s', want '%s'", i, logged[i - 1], line);
            break;
        }
    }

    /* The ring holds the longest end of them that fits 8192 bytes, a line taking its length plus one. */
    long first = n + 1;
    size_t used = 0;
    for (;;) {
        size_t cost = model_line(first - 1, line) + 1;
        if (first == 1 || used + cost > APPS_LOG_RING_BYTES) {
            break;
        }
        first--;
        used += cost;
    }
    char *tail = malloc(APPS_LOG_RING_BYTES + 1);
    size_t got = apps_rt_log_tail(rt, 100000, tail, APPS_LOG_RING_BYTES + 1);
    CHECK(got == strlen(tail), "returned %zu, the text is %zu bytes", got, strlen(tail));
    CHECK(got == used - 1, "the tail is %zu bytes, want %zu (%ld lines)", got, used - 1, n - first + 1);
    const char *p = tail;
    long at = first;
    for (; at <= n; at++) {
        size_t want = model_line(at, line);
        const char *end = strchr(p, '\n');
        size_t have = end != NULL ? (size_t)(end - p) : strlen(p);
        if (have != want || memcmp(p, line, want) != 0) {
            break;
        }
        p += have + (end != NULL ? 1 : 0);
    }
    CHECK(at == n + 1 && *p == '\0', "the ring's lines differ from the model's at line %ld", at);

    /* The last two. */
    char want_two[2 * (APPS_LOG_LINE_MAX + 1)];
    size_t l1 = model_line(n - 1, line);
    memcpy(want_two, line, l1);
    want_two[l1] = '\n';
    model_line(n, want_two + l1 + 1);
    got = apps_rt_log_tail(rt, 2, tail, APPS_LOG_RING_BYTES + 1);
    CHECK(strcmp(tail, want_two) == 0 && got == strlen(want_two), "two lines: %zu bytes '%s'", got, tail);
    free(tail);
    finish(rt, pkg);
}

/* The JSON array of the texts (they need no escaping) that the `p` handler below prints. */
static char *json_lines(const char *const *lines, size_t n) {
    size_t cap = 16;
    for (size_t i = 0; i < n; i++) {
        cap += strlen(lines[i]) + 4;
    }
    char *json = malloc(cap);
    size_t len = 0;
    json[len++] = '[';
    for (size_t i = 0; i < n; i++) {
        len += (size_t)snprintf(json + len, cap - len, "%s\"%s\"", i != 0 ? "," : "", lines[i]);
    }
    json[len++] = ']';
    json[len] = '\0';
    return json;
}

static void print_lines(apps_rt_t *rt, const char *const *lines, size_t n) {
    char *json = json_lines(lines, n);
    size_t len = strlen(json);
    char *input = exact_copy(json, len), err[128];
    char *reply = apps_rt_message(rt, "p", input, len, err, sizeof err);
    CHECK(reply != NULL, "'%s'", err);
    apps_port_free(reply);
    free(input);
    free(json);
}

static void expect_tail_at(int line, const apps_rt_t *rt, int lines, size_t cap, const char *want) {
    char *out = malloc(cap != 0 ? cap : 1);
    memset(out, 'Z', cap != 0 ? cap : 1);
    size_t got = apps_rt_log_tail(rt, lines, out, cap);
    if (cap == 0) {
        CHECK_AT(line, got == 0 && out[0] == 'Z', "tail(%d, cap 0): returned %zu", lines, got);
    } else {
        const char *nul = memchr(out, '\0', cap);
        CHECK_AT(line, nul == out + strlen(want) && strcmp(out, want) == 0 && got == strlen(want),
                 "tail(%d, cap %zu): returned %zu, text '%.*s', want '%s'", lines, cap, got, (int)cap, out, want);
    }
    free(out);
}
#define expect_tail(rt, lines, cap, want) expect_tail_at(__LINE__, rt, lines, cap, want)

static void tail_cuts(void) {
    static const char *const handler =
        "return {message = function(event, data) for _, l in ipairs(data) do print(l) end end}";
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, handler);

    /* An empty log is "". */
    expect_tail(rt, 5, 64, "");
    expect_tail(rt, 0, 64, "");

    static const char *const first[] = {"aaa", "bbbb", "cc"};
    print_lines(rt, first, 3);
    expect_tail(rt, 3, 64, "aaa\nbbbb\ncc");
    expect_tail(rt, 100, 64, "aaa\nbbbb\ncc");
    expect_tail(rt, 2, 64, "bbbb\ncc");
    expect_tail(rt, 1, 64, "cc");
    expect_tail(rt, 0, 64, "");
    expect_tail(rt, -3, 64, "");
    /* When the room is short the oldest go, whole: 11 bytes of text need a cap of 12. */
    expect_tail(rt, 3, 12, "aaa\nbbbb\ncc");
    expect_tail(rt, 3, 11, "bbbb\ncc");
    expect_tail(rt, 3, 8, "bbbb\ncc");
    expect_tail(rt, 3, 7, "cc");
    expect_tail(rt, 3, 3, "cc");
    /* A newest line that alone doesn't fit is cut. */
    expect_tail(rt, 3, 2, "c");
    expect_tail(rt, 3, 1, "");
    expect_tail(rt, 3, 0, "");
    finish(rt, pkg);

    /* Cuts fall on a character boundary: e-acute is 2 bytes, euro 3, a grinning face 4. */
    rt = launch(&pkg, handler);
    static const char *const wide[] = {"x", WIDE};
    print_lines(rt, wide, 2);
    expect_tail(rt, 2, 64, "x\n" WIDE);
    expect_tail(rt, 2, 11, WIDE);  /* "x" goes: both lines need 11 bytes with the NUL's room */
    expect_tail(rt, 2, 10, WIDE);
    expect_tail(rt, 2, 9, "\xC3\xA9\xE2\x82\xAC");
    expect_tail(rt, 2, 6, "\xC3\xA9\xE2\x82\xAC");
    expect_tail(rt, 2, 5, "\xC3\xA9");
    expect_tail(rt, 2, 3, "\xC3\xA9");
    expect_tail(rt, 2, 2, "");
    expect_tail(rt, 2, 1, "");
    finish(rt, pkg);

    /* Empty lines are lines. */
    rt = launch(&pkg, handler);
    static const char *const holes[] = {"", "a", ""};
    print_lines(rt, holes, 3);
    expect_tail(rt, 3, 64, "\na\n");
    expect_tail(rt, 2, 64, "a\n");
    expect_tail(rt, 1, 64, "");
    finish(rt, pkg);
}

static void port_log(void) {
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg,
        "print('top')\n"
        "return {start = function() print('one', 2, 'three\\nfour') print() end,\n"
        "        message = function() coroutine.wrap(function() print('in a coroutine') end)() end}");
    char err[64];
    char *reply = send(rt, "go", err, sizeof err);
    apps_port_free(reply);
    expect_logged("top", "one\t2\tthree", "four", "", "in a coroutine");  /* the id was the package's each time */
    char tail[APPS_LOG_RING_BYTES + 1];
    apps_rt_log_tail(rt, 100, tail, sizeof tail);
    CHECK(strcmp(tail, "top\none\t2\tthree\nfour\n\nin a coroutine") == 0, "the log holds '%s'", tail);
    finish(rt, pkg);
}

/* ---- A heap that refuses ----
 *
 * Each sweep runs a scenario with every growth request from the Nth on refused, for each N until none is
 * reached, and checks that nothing aborts, nothing leaks and the answer is the right one: a failure the
 * runtime reports when something was refused, success when nothing was. */

/* The growth requests apps_rt_new makes before main.lua runs: its own block, the widget table's, then
 * apps_vm_new's, counted by making a table and a VM with the config the runtime gives it. */
static long growths_before_main(size_t mem_limit) {
    apps_vm_config_t cfg = {.mem_limit = mem_limit,
                            .realloc_fn = apps_port_realloc,
                            .free_fn = apps_port_free,
                            .now_ms = apps_port_mono_ms};
    growths = 0;
    apps_ui_t *ui = apps_ui_new();
    apps_vm_t *vm = apps_vm_new(&cfg);
    CHECK(ui != NULL && vm != NULL, "a widget table and a VM of %zu bytes", mem_limit);
    apps_vm_free(vm);
    apps_ui_free(ui);
    return 1 + growths;
}

static void new_refused(void) {
    apps_pkg_t *pkg = make_pkg("local app = {}\nfunction app.start() end\nreturn app\n");
    const long before_main = growths_before_main(256 * 1024);
    long total = 0;
    for (long k = 1; k < 100000; k++) {
        growths = 0;
        refuse_from = k;
        char err[256] = "";
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        bool refused = growths >= k;
        refuse_from = -1;
        if (refused) {
            /* No runtime while there is no VM (the reason is err); once there is one, the heap that ran out
             * is main.lua's top level running out of memory: a crashed runtime with its record, and no err.
             * Never a running one, never silence. */
            bool none = rt == NULL && err[0] != '\0';
            bool crashed = rt != NULL && err[0] == '\0' && apps_rt_state(rt) == APPS_STATE_CRASHED &&
                           strstr(apps_rt_last_error(rt), "out of memory") != NULL;
            CHECK(k <= before_main ? none : crashed,
                  "k=%ld (the runtime and VM took %ld requests): refused, but new returned %p '%s' (state %d)", k,
                  before_main, (void *)rt, err, rt != NULL ? apps_rt_state(rt) : -1);
            if (k <= before_main) {
                /* The message says which block there was no room for: the runtime's (the first request), the
                 * widget table's (the second) or the VM's heap (the rest, up to its own). */
                char want[128];
                if (k == 1) {
                    snprintf(want, sizeof want, "out of memory: can't make room for the app");
                } else if (k == 2) {
                    snprintf(want, sizeof want, "out of memory: can't make room for the app's screen");
                } else {
                    snprintf(want, sizeof want, "out of memory: can't make the app's heap of %u KB",
                             (unsigned)pkg->manifest.memory_kb);
                }
                CHECK(strcmp(err, want) == 0, "k=%ld: the message is '%s', want '%s'", k, err, want);
            }
        } else {
            CHECK(rt != NULL && apps_rt_state(rt) == APPS_STATE_STOPPED, "k=%ld: nothing refused, but new gave %p: %s",
                  k, (void *)rt, err);
            total = k;
        }
        apps_rt_free(rt);
        CHECK(held == 0, "k=%ld: %zu bytes held", k, held);
        if (!refused) {
            break;
        }
    }
    CHECK(total > 100, "only %ld growth requests in a new", total);
    free_pkg(pkg);
}

/* ---- The compile step ---- */

/* The install's compile (apps_rt_compile) for the files, each an exact-size heap copy, made one VM of memory_kb. */
static bool compile_files(const file_t *files, int n, unsigned memory_kb, char *err, size_t err_len) {
    apps_code_t code[8];
    for (int i = 0; i < n; i++) {
        code[i] = (apps_code_t){.path = files[i].path, .src = exact_copy(files[i].src, strlen(files[i].src)),
                                .len = strlen(files[i].src)};
    }
    bool ok = apps_rt_compile(code, n, memory_kb, err, err_len);
    for (int i = 0; i < n; i++) {
        free((void *)code[i].src);
    }
    return ok;
}

/* apps_rt_compile: every file is loaded and none runs; the first that fails ends it and is the message; the VM is
 * gone after (nothing held), and err is cut where every message is. */
static void compile_all(void) {
    char err[APPS_ERROR_MAX];
    /* No file, nothing to refuse; err is "" for every success. */
    snprintf(err, sizeof err, "stale");
    CHECK(apps_rt_compile(NULL, 0, 256, err, sizeof err) && err[0] == '\0', "no files: '%s'", err);
    const file_t good[] = {{"main.lua", "print('main ran')\nreturn {}"},
                           {"lib/util.lua", "print('util ran')\nreturn 1"},
                           {"b.lua", ""}};
    snprintf(err, sizeof err, "stale");
    CHECK(compile_files(good, 3, 256, err, sizeof err) && err[0] == '\0', "good files: '%s'", err);
    expect_no_logs();  /* loaded, never run */
    CHECK(held == 0, "%zu bytes held after a compile", held);

    /* The first file that fails is the message, in the order given, and the files after it aren't compiled. */
    const file_t bad_main_first[] = {{"main.lua", "x = = 1"}, {"c.lua", "y = = 2"}};
    CHECK(!compile_files(bad_main_first, 2, 256, err, sizeof err) && strcmp(err, "main.lua:1: unexpected symbol near '='") == 0,
          "main first: '%s'", err);
    const file_t bad_c_first[] = {{"c.lua", "y = = 2"}, {"main.lua", "x = = 1"}};
    CHECK(!compile_files(bad_c_first, 2, 256, err, sizeof err) && strcmp(err, "c.lua:1: unexpected symbol near '='") == 0,
          "c first: '%s'", err);
    const file_t second_bad[] = {{"main.lua", "return {}"}, {"lib/b.lua", "return {\n"}};
    CHECK(!compile_files(second_bad, 2, 256, err, sizeof err) && strcmp(err, "lib/b.lua:2: unexpected symbol near <eof>") == 0,
          "second: '%s'", err);
    const file_t closing[] = {{"main.lua", "local x <close> = nil"}};
    CHECK(!compile_files(closing, 1, 256, err, sizeof err) &&
              strcmp(err, "main.lua:1: '<close>' variables are not available in Muse apps") == 0,
          "<close>: '%s'", err);
    const file_t compiled[] = {{"main.lua", "\x1b" "Lua"}};
    CHECK(!compile_files(compiled, 1, 256, err, sizeof err) &&
              strcmp(err, "binary chunks are not allowed: install Lua source text") == 0,
          "a compiled chunk: '%s'", err);
    CHECK(held == 0, "%zu bytes held after refusals", held);

    /* The VM has the app's memory_kb: a heap too small to make is the one message of apps_rt_new's, and a source that
     * doesn't fit its heap is Lua's. */
    CHECK(!compile_files(good, 1, 2, err, sizeof err) && strcmp(err, "out of memory: can't make the app's heap of 2 KB") == 0,
          "2 KB: '%s'", err);
    CHECK(!compile_files(good, 1, 0, err, sizeof err) && strcmp(err, "out of memory: can't make the app's heap of 0 KB") == 0,
          "0 KB: '%s'", err);
    static char big[10000 * 2 + 16];
    int at = snprintf(big, sizeof big, "return {");
    for (int i = 0; i < 10000; i++) {
        big[at++] = '1';
        big[at++] = ',';
    }
    snprintf(big + at, sizeof big - (size_t)at, "}");
    const file_t large[] = {{"big.lua", big}};
    CHECK(compile_files(large, 1, 256, err, sizeof err) && compile_files(large, 1, 128, err, sizeof err),
          "10000 numbers: '%s'", err);
    CHECK(!compile_files(large, 1, 64, err, sizeof err) && strcmp(err, "not enough memory") == 0, "in 64 KB: '%s'", err);
    CHECK(held == 0, "%zu bytes held after the heap refusals", held);

    /* err is cut like the others (on a character boundary, a block of exactly err_len bytes), and may be NULL. */
    const file_t accent[] = {{"caf\xc3\xa9.lua", "x = = 1"}};
    static const char whole[] = "caf\xc3\xa9.lua:1: unexpected symbol near '='";
    for (size_t cap = 1; cap <= sizeof whole + 1; cap++) {
        char *out = malloc(cap);  /* a block of exactly cap bytes */
        CHECK(out != NULL, "no memory");
        bool ok = compile_files(accent, 1, 256, out, cap);
        size_t want = cap - 1 < sizeof whole - 1 ? cap - 1 : sizeof whole - 1;
        while (want < sizeof whole - 1 && want > 0 && ((unsigned char)whole[want] & 0xC0) == 0x80) {
            want--;
        }
        CHECK(!ok && strlen(out) == want && memcmp(out, whole, want) == 0, "cap %zu: '%s'", cap, out);
        free(out);
    }
    CHECK(!compile_files(accent, 1, 256, NULL, 0) && !compile_files(accent, 1, 256, NULL, 5), "a NULL err");
    CHECK(compile_files(good, 3, 256, NULL, 0), "a NULL err, a package that compiles");
    CHECK(held == 0, "%zu bytes held at the end", held);
}

/* A heap that refuses from the Nth growth request on, for each N: a compile either finishes (and no request was
 * refused) or says so, the VM never leaks, and nothing aborts. */
static void compile_refused(void) {
    static const char main_src[] = "local t = {}\nfor i = 1, 3 do t[i] = 'x' .. i end\nreturn {start = function() return t end}\n";
    const file_t files[] = {{"main.lua", main_src}, {"lib/util.lua", "local a = {1, 2, 3}\nreturn a\n"}};
    long total = 0;
    for (long k = 1; k < 100000; k++) {
        char err[256] = "";
        growths = 0;
        refuse_from = k;
        bool ok = compile_files(files, 2, 256, err, sizeof err);
        bool refused = growths >= k;
        refuse_from = -1;
        CHECK(ok == !refused && (ok ? err[0] == '\0' : err[0] != '\0'), "k=%ld: refused %d, compile %d '%s'", k, refused, ok, err);
        if (k == 1) {  /* the VM's own first request: no heap to compile in */
            CHECK(!ok && strcmp(err, "out of memory: can't make the app's heap of 256 KB") == 0, "k=1: '%s'", err);
        }
        CHECK(held == 0, "k=%ld: %zu bytes held", k, held);
        if (!refused) {
            total = k;
            break;
        }
    }
    CHECK(total > 50, "only %ld growth requests in a compile", total);
}

/* The source the sweeps run: it allocates (strings, tables), so every step has something to refuse. Each
 * handler makes strings of its own (the tag): a short string that is still alive from an earlier call is
 * found, not allocated, and a sweep of the second handler would have little to refuse. */
static const char *const ALLOCATING =
    "local app = {}\n"
    "local function work(tag) local t = {} for i = 1, 20 do t[i] = tostring(i) .. tag end return t end\n"
    "function app.start() work('a') end\n"
    "function app.stop() work('b') end\n"
    "function app.show() work('c') end\n"
    "function app.hide() work('d') end\n"
    "function app.alarm(name) work('e') end\n"
    "function app.message(event, data) work('f') print('m', #work('g')) return {result = 'ok'} end\n"
    "return app\n";

typedef enum { H_START, H_STOP, H_SHOW, H_HIDE, H_ALARM, H_COUNT } handler_t;

static void handlers_refused(void) {
    static const char *const names[] = {"start", "stop", "show", "hide", "alarm"};
    for (int h = 0; h < H_COUNT; h++) {
        long reached = 0;
        for (long k = 1; k < 100000; k++) {
            char err[256] = "";
            apps_pkg_t *pkg = make_pkg(ALLOCATING);
            apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
            CHECK(rt != NULL, "%s", err);
            if (rt == NULL) {
                free_pkg(pkg);
                return;
            }
            if (h != H_START) {
                CHECK(apps_rt_start(rt), "start");
            }
            long before = growths;
            refuse_from = growths + k;
            bool returned = true;  /* stop, show and hide return nothing: always "true" */
            switch (h) {
                case H_START: returned = apps_rt_start(rt); break;
                case H_STOP: apps_rt_stop(rt); break;
                case H_SHOW: apps_rt_show(rt, true); break;
                case H_HIDE: apps_rt_show(rt, false); break;
                case H_ALARM: returned = apps_rt_alarm(rt, "wake"); break;
            }
            bool refused = growths >= refuse_from;
            refuse_from = -1;
            if (refused) {
                CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strstr(apps_rt_last_error(rt), "out of memory") != NULL,
                      "%s, k=%ld: refused, state %d, record '%s'", names[h], k, apps_rt_state(rt), apps_rt_last_error(rt));
                CHECK(!returned || h == H_STOP || h == H_SHOW || h == H_HIDE, "%s, k=%ld: returned true after a crash",
                      names[h], k);
            } else {
                CHECK(apps_rt_state(rt) != APPS_STATE_CRASHED && returned, "%s, k=%ld: nothing refused, state %d", names[h],
                      k, apps_rt_state(rt));
                reached = growths - before;
            }
            apps_rt_free(rt);
            free_pkg(pkg);
            forget_logged();
            CHECK(held == 0, "%s, k=%ld: %zu bytes held", names[h], k, held);
            if (!refused) {
                break;
            }
        }
        CHECK(reached > 10, "%s: only %ld growth requests", names[h], reached);
    }
}

static void message_refused(void) {
    static const char *const data = "{\"n\":[1,2,3],\"s\":\"text\",\"o\":{\"a\":null}}";
    bool saw_soft_failure = false, saw_crash = false;
    long reached = 0;
    for (long k = 1; k < 100000; k++) {
        char err[256] = "";
        apps_pkg_t *pkg = make_pkg(ALLOCATING);
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        CHECK(rt != NULL && apps_rt_start(rt), "%s", err);
        if (rt == NULL) {
            free_pkg(pkg);
            return;
        }
        long before = growths;
        refuse_from = growths + k;
        char *input = exact_copy(data, strlen(data));
        char *reply = apps_rt_message(rt, "m", input, strlen(data), err, sizeof err);
        bool refused = growths >= refuse_from;
        refuse_from = -1;
        free(input);
        if (refused) {
            CHECK(reply == NULL, "k=%ld: refused, but a reply came: '%s'", k, reply);
            if (apps_rt_state(rt) == APPS_STATE_CRASHED) {
                saw_crash = true;
                CHECK(strcmp(err, apps_rt_last_error(rt)) == 0 && strstr(err, "out of memory") != NULL, "k=%ld: '%s'", k,
                      err);
            } else {
                /* Only the reply's own block can be refused without the app running out of memory. */
                saw_soft_failure = true;
                CHECK(strcmp(err, "message: out of memory") == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING,
                      "k=%ld: '%s', state %d", k, err, apps_rt_state(rt));
            }
        } else {
            CHECK(reply != NULL && strcmp(reply, "{\"result\":\"ok\"}") == 0 && err[0] == '\0', "k=%ld: '%s' '%s'", k,
                  reply != NULL ? reply : "(null)", err);
            reached = growths - before;
        }
        apps_port_free(reply);
        apps_rt_free(rt);
        free_pkg(pkg);
        forget_logged();
        CHECK(held == 0, "k=%ld: %zu bytes held", k, held);
        if (!refused) {
            break;
        }
    }
    CHECK(reached > 20, "only %ld growth requests in a message", reached);
    CHECK(saw_crash, "no refusal ever crashed the app");
    CHECK(saw_soft_failure, "the reply's block was never the one refused");
}

/* require compiles on the thread that calls it, a coroutine's too, and allocates there (the parser, the chunk, its
 * strings): a heap that refuses at any step of it is the app running out of memory, as anywhere else, and never
 * an abort or a leak. */
static void require_refused(void) {
    static const char *const mains[] = {
        "return {message = function() return require('m').v end}",
        "return {message = function() return coroutine.wrap(function() return require('m').v end)() end}",
    };
    for (size_t scenario = 0; scenario < ARRAY_LEN(mains); scenario++) {
        const file_t files[] = {
            {"main.lua", mains[scenario]},
            {"m.lua", "local t = {} for i = 1, 20 do t[i] = tostring(i) .. 'x' end return {v = #t}"},
        };
        bool saw_crash = false;
        long reached = 0;
        for (long k = 1; k < 100000; k++) {
            apps_pkg_t *pkg = make_pkg_kb(256, files, 2);
            char err[256] = "";
            apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
            CHECK(rt != NULL && apps_rt_start(rt), "scenario %zu: %s", scenario, err);
            if (rt == NULL) {
                free_pkg(pkg);
                return;
            }
            long before = growths;
            refuse_from = growths + k;
            char *reply = send(rt, "go", err, sizeof err);
            bool refused = growths >= refuse_from;
            refuse_from = -1;
            if (refused) {
                CHECK(reply == NULL, "scenario %zu, k=%ld: refused, but a reply came: '%s'", scenario, k, reply);
                if (apps_rt_state(rt) == APPS_STATE_CRASHED) {
                    saw_crash = true;
                    CHECK(strcmp(err, apps_rt_last_error(rt)) == 0 && strstr(err, "out of memory") != NULL,
                          "scenario %zu, k=%ld: '%s'", scenario, k, err);
                } else {  /* only the reply's own block can be refused without the app running out of memory */
                    CHECK(strcmp(err, "message: out of memory") == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING,
                          "scenario %zu, k=%ld: '%s', state %d", scenario, k, err, apps_rt_state(rt));
                }
            } else {
                CHECK(reply != NULL && strcmp(reply, "20") == 0 && err[0] == '\0', "scenario %zu, k=%ld: '%s' '%s'",
                      scenario, k, reply != NULL ? reply : "(none)", err);
                reached = growths - before;
            }
            apps_port_free(reply);
            finish(rt, pkg);
            if (!refused) {
                break;
            }
        }
        CHECK(reached > 10, "scenario %zu: only %ld growth requests in the message", scenario, reached);
        CHECK(saw_crash, "scenario %zu: no refusal ever crashed the app", scenario);
    }
}

/* ---- The time module ---- */

/* The conversions that depend on the time zone, which apphost (UTC0 always) can't tell from UTC: the board's zone,
 * PST8PDT, on both sides of a change of the clock, and a local time that doesn't exist. Each line is one check, as
 * the app joins its values with spaces; the expected values are the zone's, worked out with Python's zoneinfo
 * (America/Los_Angeles) and not with this code. A is 2026-01-01T00:00:00Z, B is 2026-07-01T12:30:45Z, SPRING is
 * 2026-03-08T10:00:00Z (2:00 PST, when the clocks go to 3:00 PDT) and FALL is 2026-11-01T09:00:00Z (2:00 PDT, 1:00 PST).
 * The hour that happens twice, 1:30 on 2026-11-01, is 08:30Z when the table says isdst = true and 09:30Z for false (the
 * second to last line). The last is the calendar's start (0000-01-01T00:00:00Z) seen from PST: the year -1 for %Y, and 99 for
 * %y. */
static const char *const ZONE_APP =
    "local A, B, SPRING, FALL = 1767225600000, 1782909045000, 1772964000000, 1793523600000\n"
    "local out = {}\n"
    "local function add(...) out[#out + 1] = table.concat({...}, ' ') end\n"
    "local function when(ms) return time.format('%Y-%m-%d %H:%M:%S %a %j', ms) end\n"
    "return {message = function()\n"
    "  add(when(A)) add(when(B)) add(when(SPRING - 1000)) add(when(SPRING)) add(when(FALL - 1000)) add(when(FALL))\n"
    "  local d = time.date(A)\n"
    "  add(d.year, d.month, d.day, d.hour, d.min, d.sec, d.wday, d.yday)\n"
    "  add(os.date('%H:%M', A // 1000), os.date('!%H:%M', A // 1000))\n"
    "  local local_t, utc_t = os.date('*t', B // 1000), os.date('!*t', B // 1000)\n"
    "  add(local_t.hour, tostring(local_t.isdst), utc_t.hour, tostring(utc_t.isdst))\n"
    "  add(os.time{year = 2026, month = 7, day = 1, hour = 5, min = 30, sec = 45})\n"
    "  add(os.time{year = 2026, month = 1, day = 1, hour = 0})\n"
    "  add(os.time{year = 2026, month = 7, day = 1})\n"
    "  local t = {year = 2026, month = 13, day = 32, hour = 25}\n"
    "  add(os.time(t), t.year, t.month, t.day, t.hour, t.min, t.sec, t.wday, t.yday)\n"
    "  add(os.time{year = 2026, month = 11, day = 1, hour = 1, min = 30, isdst = true},\n"
    "      os.time{year = 2026, month = 11, day = 1, hour = 1, min = 30, isdst = false})\n"
    "  add(time.format('%Y %y', -62167219200000))\n"
    "  return out\n"
    "end}\n";

static const char *const ZONE_WANT =
    "[\"2025-12-31 16:00:00 Wed 365\","
    "\"2026-07-01 05:30:45 Wed 182\","
    "\"2026-03-08 01:59:59 Sun 067\","
    "\"2026-03-08 03:00:00 Sun 067\","
    "\"2026-11-01 01:59:59 Sun 305\","
    "\"2026-11-01 01:00:00 Sun 305\","
    "\"2025 12 31 16 0 0 4 365\","
    "\"16:00 00:00\","
    "\"5 true 12 false\","
    "\"1782909045\","
    "\"1767254400\","
    "\"1782932400\","
    "\"1801558800 2027 2 2 1 0 0 3 33\","
    "\"1793521800 1793525400\","
    "\"-1 99\"]";

static void time_zone(void) {
    CHECK(setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1) == 0, "setenv");
    tzset();
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, ZONE_APP);
    if (rt != NULL) {
        char err[256] = "";
        char *reply = send(rt, "go", err, sizeof err);
        CHECK(reply != NULL, "the message failed: %s", err);
        if (reply != NULL) {
            /* One line each, so that a difference says which. */
            const char *want = ZONE_WANT;
            for (const char *got = reply; *got != '\0' && *want != '\0';) {
                const char *got_end = strstr(got, "\",\""), *want_end = strstr(want, "\",\"");
                size_t got_n = got_end != NULL ? (size_t)(got_end - got) + 1 : strlen(got);
                size_t want_n = want_end != NULL ? (size_t)(want_end - want) + 1 : strlen(want);
                CHECK(got_n == want_n && strncmp(got, want, got_n) == 0, "got  %.*s\n   want %.*s", (int)got_n, got,
                      (int)want_n, want);
                if (got_end == NULL || want_end == NULL) {
                    break;
                }
                got += got_n + 1;
                want += want_n + 1;
            }
            CHECK(strcmp(reply, ZONE_WANT) == 0, "the replies differ: %s", reply);
        }
        apps_port_free(reply);
    }
    finish(rt, pkg);
    CHECK(setenv("TZ", "UTC0", 1) == 0, "setenv");
    tzset();
}

/* Every binding of the time module and the timers allocates (tables, a handle, a registry reference), so a heap that
 * refuses at any of them is the app running out of memory: a crashed app with its record, as anywhere else, and never
 * an abort or a leak. */
static const char *const SERVICES_APP =
    "local function f() end\n"
    "return {message = function()\n"
    "  local d = time.date(0)\n"
    "  local text = time.format('%a %b %e %H:%M:%S %Y', 86400000)\n"
    "  local t = os.date('*t', 0)\n"
    "  local s = os.time{year = 2026, month = 1, day = 1, hour = 0}\n"
    "  timer.after(100, f)\n"
    "  timer.every(100, f):cancel()\n"
    "  return {d.year, text, t.year, s > 0}\n"
    "end}\n";

static void services_refused(void) {
    CHECK(setenv("TZ", "UTC0", 1) == 0, "setenv");
    tzset();
    bool saw_crash = false, saw_soft_failure = false;
    long reached = 0;
    for (long k = 1; k < 100000; k++) {
        char err[256] = "";
        apps_pkg_t *pkg = make_pkg(SERVICES_APP);
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        CHECK(rt != NULL && apps_rt_start(rt), "%s", err);
        if (rt == NULL) {
            free_pkg(pkg);
            return;
        }
        long before = growths;
        refuse_from = growths + k;
        char *reply = send(rt, "go", err, sizeof err);
        bool refused = growths >= refuse_from;
        refuse_from = -1;
        if (refused) {
            CHECK(reply == NULL, "k=%ld: refused, but a reply came: '%s'", k, reply);
            if (apps_rt_state(rt) == APPS_STATE_CRASHED) {
                saw_crash = true;
                CHECK(strcmp(err, apps_rt_last_error(rt)) == 0 && strstr(err, "out of memory") != NULL, "k=%ld: '%s'", k,
                      err);
            } else {  /* only the reply's own block can be refused without the app running out of memory */
                saw_soft_failure = true;
                CHECK(strcmp(err, "message: out of memory") == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING,
                      "k=%ld: '%s', state %d", k, err, apps_rt_state(rt));
            }
        } else {
            CHECK(reply != NULL && strcmp(reply, "[1970,\"Fri Jan  2 00:00:00 1970\",1970,true]") == 0 && err[0] == '\0',
                  "k=%ld: '%s' '%s'", k, reply != NULL ? reply : "(none)", err);
            reached = growths - before;
        }
        apps_port_free(reply);
        finish(rt, pkg);
        if (!refused) {
            break;
        }
    }
    CHECK(reached > 10, "only %ld growth requests in the message", reached);
    CHECK(saw_crash, "no refusal ever crashed the app");
    CHECK(saw_soft_failure, "the reply's block was never the one refused");
}

/* The timers' contract at the runtime's own API, which the app task and apphost build on, where a script only sees events:
 * when the next one is due, what one step of a pass does (which timer, in what order, from what time it reschedules), what the
 * dark screen, a stop, a crash and a kill from outside leave. The clocks are the case's. */
static const char *const TIMER_APP =
    "local app = {}\n"
    "function app.start()\n"
    "  timer.after(300, function() print('c') end)\n"
    "  timer.after(100, function() print('a') end)\n"
    "  timer.every(200, function() print('b') end)\n"
    "  timer.after(100, function() print('a2') end)\n"
    "end\n"
    "function app.stop() print('stop') end\n"
    "return app\n";

static void timer_api(void) {
    port_mono_ms = 5000;
    port_dark = false;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, TIMER_APP);
    if (rt == NULL) {
        return;
    }
    CHECK(apps_rt_timer_count(rt) == 4, "%d timers", apps_rt_timer_count(rt));
    CHECK(apps_rt_next_deadline(rt) == 5100, "the earliest is due at %lld", (long long)apps_rt_next_deadline(rt));
    CHECK(apps_rt_next_deadline(NULL) == INT64_MAX && !apps_rt_has_work(NULL, 0) && !apps_rt_run_one_timer(NULL, 0), "NULL");
    /* Nothing is due a millisecond early. */
    CHECK(!apps_rt_has_work(rt, 5099) && !apps_rt_run_one_timer(rt, 5099), "a timer ran early");
    expect_no_logs();
    /* Two are due at 5100: a step runs one, the one armed first, and the next step the other. */
    CHECK(apps_rt_has_work(rt, 5100), "no work at the deadline");
    CHECK(apps_rt_run_one_timer(rt, 5100), "no step at 5100");
    expect_logged("a");
    CHECK(apps_rt_timer_count(rt) == 3 && apps_rt_next_deadline(rt) == 5100, "a one-shot is gone, its twin is due");
    CHECK(apps_rt_run_one_timer(rt, 5100), "no second step");
    CHECK(!apps_rt_run_one_timer(rt, 5100) && !apps_rt_has_work(rt, 5100), "a third step at 5100");
    expect_logged("a", "a2");
    CHECK(apps_rt_next_deadline(rt) == 5200, "next: %lld", (long long)apps_rt_next_deadline(rt));
    /* A dark screen: no deadline, no work, no step, whatever is overdue. */
    port_dark = true;
    CHECK(apps_rt_next_deadline(rt) == INT64_MAX && !apps_rt_has_work(rt, 99999) && !apps_rt_run_one_timer(rt, 99999),
          "a timer was due while the screen was dark");
    expect_logged("a", "a2");
    port_dark = false;
    /* A pass at 9000, long after: each due timer once, earliest first, and the every timer's next run is a period after the
     * pass's time, not after the port's clock (which is still 5000). */
    apps_rt_run_timers(rt, 9000);
    expect_logged("a", "a2", "b", "c");
    CHECK(apps_rt_timer_count(rt) == 1 && apps_rt_next_deadline(rt) == 9200, "after the pass: %d timers, next at %lld",
          apps_rt_timer_count(rt), (long long)apps_rt_next_deadline(rt));
    /* A stop takes the timers with it, and a stopped runtime has no deadline and runs none. */
    apps_rt_stop(rt);
    CHECK(apps_rt_state(rt) == APPS_STATE_STOPPED && apps_rt_timer_count(rt) == 0, "stopped: %d timers", apps_rt_timer_count(rt));
    CHECK(apps_rt_next_deadline(rt) == INT64_MAX && !apps_rt_has_work(rt, 99999) && !apps_rt_run_one_timer(rt, 99999),
          "a stopped runtime has work");
    expect_logged("a", "a2", "b", "c", "stop");
    finish(rt, pkg);

    /* A callback that fails is a crash, and the other timers go with the app. */
    rt = launch(&pkg, "return {start = function()\n"
                      "  timer.every(1000, function() print('never') end)\n"
                      "  timer.after(100, function() error('boom') end)\n"
                      "end}");
    if (rt != NULL) {
        apps_rt_run_timers(rt, 5100);
        CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strncmp(apps_rt_last_error(rt), "main.lua:3: boom\nstack traceback:", 33) == 0,
              "%d '%s'", apps_rt_state(rt), apps_rt_last_error(rt));
        CHECK(apps_rt_timer_count(rt) == 0 && apps_rt_next_deadline(rt) == INT64_MAX && !apps_rt_run_one_timer(rt, 99999),
              "a crashed app keeps timers");
        expect_no_logs();
    }
    finish(rt, pkg);

    /* An app that another task killed, with no call to find out: the next step is the call that does, and the crash has the
     * kill's reason. */
    rt = launch(&pkg, "return {start = function() timer.every(1000, function() print('never') end) end}");
    if (rt != NULL) {
        apps_vm_kill(apps_rt_vm(rt), "stopped: killed by the case");
        CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "the kill alone doesn't crash it");
        CHECK(apps_rt_has_work(rt, 5000), "a killed app has no work to do");
        CHECK(apps_rt_run_one_timer(rt, 5000), "no step for a killed app");
        CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), "stopped: killed by the case") == 0,
              "%d '%s'", apps_rt_state(rt), apps_rt_last_error(rt));
        CHECK(apps_rt_timer_count(rt) == 0 && !apps_rt_has_work(rt, 5000), "a crashed app has work");
        expect_no_logs();
    }
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* Of two timers due together the one armed first runs first, not the one in the lower slot: a is armed (slot 0), b (slot 1),
 * a is cancelled, and c takes slot 0, due when b is. */
static void timer_tie(void) {
    port_mono_ms = 5000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, "local a\n"
                                 "return {message = function()\n"
                                 "  a = timer.after(100, function() print('a') end)\n"
                                 "  timer.after(100, function() print('b') end)\n"
                                 "  a:cancel()\n"
                                 "  timer.after(100, function() print('c') end)\n"
                                 "  return true\n"
                                 "end}\n");
    if (rt == NULL) {
        return;
    }
    char err[256] = "";
    char *reply = send(rt, "go", err, sizeof err);
    CHECK(reply != NULL, "go: '%s'", err);
    apps_port_free(reply);
    CHECK(apps_rt_timer_count(rt) == 2 && apps_rt_next_deadline(rt) == 5100, "%d timers, the next at %lld", apps_rt_timer_count(rt),
          (long long)apps_rt_next_deadline(rt));
    apps_rt_run_timers(rt, 5100);
    expect_logged("b", "c");
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* os.time walks the days of its table in C, a month at a time, and charges them to the call (apps_vm_charge) before it does, at
 * 100 days to an instruction (about what the board spends: 3.2 ns a day against 0.4 us for an instruction) and one more: a call
 * with a table of 2.5 M days costs 25,001. Every field counts, as the bound on them does: day, hour / 24, min / 1440 and
 * sec / 86400. The tables count backwards from the end of the calendar, so each lands inside it (a result outside is an error
 * after the walk, and Darwin's mktime takes no year before 1900 either). Each case is n calls of one table, and the
 * instructions the VM counted for them (the charge is exact; the loop and the count hook, which counts in thousands, add a
 * little). */
static const char *const CHARGE_APP =
    "return {message = function(event, n)\n"
    "  for i = 1, n do\n"
    "    if event == 'day' then os.time{year = 9999, month = 12, day = -2500000}\n"
    "    elseif event == 'hour' then os.time{year = 9999, month = 12, day = 0, hour = -60000000}\n"
    "    elseif event == 'min' then os.time{year = 9999, month = 12, day = 0, min = -(2^31 - 1)}\n"
    "    elseif event == 'sec' then os.time{year = 9999, month = 12, day = 0, sec = -(2^31 - 1)}\n"
    "    elseif event == 'all' then\n"
    "      os.time{year = 9999, month = 12, day = -1000000, hour = -24000000, min = -720000000, sec = -(2^31 - 1)}\n"
    "    elseif event == 'refused' then\n"
    "      pcall(os.time, {year = 2026, month = 2^31, day = 1})\n"
    "      pcall(os.time, {year = 2^31 - 1900, month = 2^31, day = 1})\n"
    "    else os.time{year = 2026, month = 1, day = 1} end\n"
    "  end\n"
    "  return true\n"
    "end}\n";

static void expect_os_time_cost_at(int line, apps_rt_t *rt, const char *event, unsigned n, unsigned per_call) {
    char data[16], err[256] = "";
    snprintf(data, sizeof data, "%u", n);
    uint64_t before = apps_vm_instructions(apps_rt_vm(rt));
    char *reply = apps_rt_message(rt, event, data, strlen(data), err, sizeof err);
    CHECK_AT(line, reply != NULL, "%s x %u: '%s'", event, n, err);
    apps_port_free(reply);
    uint64_t cost = apps_vm_instructions(apps_rt_vm(rt)) - before;
    uint64_t least = (uint64_t)n * per_call, most = least + (uint64_t)n * 40 + 2000;
    CHECK_AT(line, cost >= least && cost <= most, "%s x %u cost %llu instructions: %u each, so from %llu to %llu", event, n,
             (unsigned long long)cost, per_call, (unsigned long long)least, (unsigned long long)most);
}
#define EXPECT_OS_TIME_COST(rt, event, n, per_call) expect_os_time_cost_at(__LINE__, (rt), (event), (n), (per_call))

/* n calls of `event` that charge nothing: the VM's count moves in the steps of its hook (a thousand: HOOK_EVERY in apps_vm.c)
 * and by what C functions charge, so a call that charged one instruction would leave the count off a whole step. */
static void expect_os_time_free_at(int line, apps_rt_t *rt, const char *event, unsigned n) {
    char data[16], err[256] = "";
    snprintf(data, sizeof data, "%u", n);
    uint64_t before = apps_vm_instructions(apps_rt_vm(rt));
    char *reply = apps_rt_message(rt, event, data, strlen(data), err, sizeof err);
    CHECK_AT(line, reply != NULL, "%s x %u: '%s'", event, n, err);
    apps_port_free(reply);
    uint64_t cost = apps_vm_instructions(apps_rt_vm(rt)) - before;
    CHECK_AT(line, cost % 1000 == 0, "%s x %u was charged %llu instructions, not a whole number of the hook's thousands", event, n,
             (unsigned long long)cost);
}
#define EXPECT_OS_TIME_FREE(rt, event, n) expect_os_time_free_at(__LINE__, (rt), (event), (n))

static void os_time_charge(void) {
    CHECK(setenv("TZ", "UTC0", 1) == 0, "setenv");
    tzset();
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, CHARGE_APP);
    if (rt != NULL) {
        EXPECT_OS_TIME_COST(rt, "plain", 100, 1);
        EXPECT_OS_TIME_COST(rt, "day", 50, 25001);    /* 2500000 days */
        EXPECT_OS_TIME_COST(rt, "hour", 50, 25001);   /* 60000000 / 24 = 2500000 days */
        EXPECT_OS_TIME_COST(rt, "min", 80, 14914);    /* 2147483647 / 1440 = 1491308 days */
        EXPECT_OS_TIME_COST(rt, "sec", 100, 249);     /* 2147483647 / 86400 = 24855 days */
        EXPECT_OS_TIME_COST(rt, "all", 50, 25249);    /* 1000000 + 1000000 + 500000 + 24855 */
        /* A year and month that can't land in the calendar are refused before the days are charged (and before the C library
         * sees them): 200 refusals that charged their one instruction each would leave the count 200 off a step. */
        EXPECT_OS_TIME_FREE(rt, "refused", 100);
        CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "state %d", apps_rt_state(rt));
    }
    finish(rt, pkg);
}

/* ---- The duty cycle's windows at the runtime's own API ----
 *
 * What apphost can't show: it passes through every 10 ms and closes each window as it ends, while the app task can sleep in its
 * queue across a boundary, wake for an event, and find windows over. The clock and the calls are the case's: `burn` is one call
 * of the app that runs a loop of n iterations, n VM instructions, at a time of its choosing. A window is hot over
 * APPS_DUTY_INSTR_PER_S, so a call of 0.4 of it is cool twice in a window and hot three times. */
static const char *const BURN_APP = "return {message = function(event, n) for i = 1, n do end return true end}\n";

#define THROTTLED_TEXT "throttled: using more than 40% of the CPU"
#define KILLED_TEXT "stopped: used more than 40% of the CPU for 5 s"

static unsigned burn_size(void) {
    unsigned n = APPS_DUTY_INSTR_PER_S / 10 * 4;
    CHECK(n < APPS_BUDGET_CALL, "a call of 0.4 of the rate (%u) is over the call budget (%u)", n, (unsigned)APPS_BUDGET_CALL);
    return n;
}

static void burn(apps_rt_t *rt, int64_t at, unsigned n) {
    char data[16], err[256] = "";
    snprintf(data, sizeof data, "%u", n);
    port_mono_ms = at;
    char *reply = apps_rt_message(rt, "burn", data, strlen(data), err, sizeof err);
    CHECK(reply != NULL, "a call of %u at %lld: '%s'", n, (long long)at, err);
    apps_port_free(reply);
}

/* Three calls inside the window that began at `begin`: hot. */
static void hot_window(apps_rt_t *rt, int64_t begin) {
    for (int i = 0; i < 3; i++) {
        burn(rt, begin + 100 + 100 * i, burn_size());
    }
}

/* BURN_APP, and `arm`, which arms a one-shot of 50 ms (to see when it is due: a throttled app's timers are 100 ms later). */
static const char *const ARM_APP =
    "return {message = function(event, n)\n"
    "  if event == 'arm' then timer.after(50, function() end) return true end\n"
    "  for i = 1, n do end return true end}\n";

/* The monotonic time the earliest timer is due at, after `arm` at `at` ms; then that timer runs, which frees its slot. */
static int64_t arm_at(apps_rt_t *rt, int64_t at) {
    char err[256] = "";
    port_mono_ms = at;
    char *reply = apps_rt_message(rt, "arm", NULL, 0, err, sizeof err);
    CHECK(reply != NULL, "arm at %lld: '%s'", (long long)at, err);
    apps_port_free(reply);
    int64_t due = apps_rt_next_deadline(rt);
    CHECK(due != INT64_MAX && apps_rt_run_one_timer(rt, due) && apps_rt_timer_count(rt) == 0, "the timer armed at %lld",
          (long long)at);
    return due;
}

/* Windows count in time order, however late they are closed. A hot window that closes together with a silent one (the task slept
 * across both) is hot all the same: it is the third in a row, so the app is throttled and told, and the silent one after it
 * ends the throttle (the review's concurrency finding: the old rule zeroed the count when more than one window ended between
 * two rolls, and a hot window that came before the silence counted for nothing). */
static void duty_jump(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, ARM_APP);  /* the first window began at 1000 */
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 2; w++) {
        hot_window(rt, 1000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * w), "work at the end of window %d", w);
    }
    CHECK(apps_rt_warnings(rt)[0] == '\0', "two hot windows are not three: '%s'", apps_rt_warnings(rt));
    /* The third is hot as well, and nothing looks at the clock until 5100: the window that ended at 4000 (hot, the third in a row)
     * and the one that ended at 5000 (nothing in it) are closed together, in that order. */
    hot_window(rt, 3000);
    CHECK(!apps_rt_has_work(rt, 5100), "work after the jump");
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING,
          "a hot window followed by a silent one is still the third: '%s'", apps_rt_warnings(rt));
    expect_logged(THROTTLED_TEXT);
    /* The silent window ended the throttle and the count: a timer armed now is on time, and it takes three hot windows from
     * here (5000, 6000 and 7000: two of them are not enough) to throttle again, which is not said again. */
    for (int w = 0; w < 3; w++) {
        hot_window(rt, 5000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 6000 + 1000 * w), "work at the end of window %d after the jump", w);
        if (w == 1) {
            int64_t due = arm_at(rt, 7050);
            CHECK(due == 7100, "after two hot windows a timer armed at 7050 for 50 ms is due at %lld", (long long)due);
        }
    }
    int64_t due = arm_at(rt, 8050);
    CHECK(due == 8200, "after three more it is throttled again: a timer armed at 8050 for 50 ms is due at %lld", (long long)due);
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0, "'%s'", apps_rt_warnings(rt));
    expect_logged(THROTTLED_TEXT);  /* once */
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* The same for the kill: the fifth hot window closes together with a silent one, and it is the fifth. */
static void duty_late_kill(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, BURN_APP);
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 4; w++) {
        hot_window(rt, 1000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * w), "work at the end of window %d", w + 1);
    }
    hot_window(rt, 5000);
    CHECK(apps_rt_has_work(rt, 7100), "the fifth hot window, closed with a silent one after it, is the kill");
    CHECK(apps_rt_run_one_timer(rt, 7100), "no step for a killed app");
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILLED_TEXT) == 0, "%d '%s'",
          apps_rt_state(rt), apps_rt_last_error(rt));
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* The loop's pass at the end of a window closes it, with no call or timer in it: the warning at the third hot window, and at the
 * fifth the kill, which the pass reports as work and the step after it as the app's crash. */
static void duty_pass(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, BURN_APP);
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 5; w++) {
        hot_window(rt, 1000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 1999 + 1000 * w), "window %d closed a millisecond early", w + 1);
        bool work = apps_rt_has_work(rt, 2000 + 1000 * w);
        CHECK(work == (w == 4), "after %d hot windows: work %d", w + 1, work);
        CHECK((apps_rt_warnings(rt)[0] != '\0') == (w >= 2), "after %d hot windows: '%s'", w + 1, apps_rt_warnings(rt));
    }
    expect_logged(THROTTLED_TEXT);
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "the kill alone doesn't crash it");
    CHECK(apps_rt_run_one_timer(rt, 6000), "no step for a killed app");
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILLED_TEXT) == 0, "%d '%s'",
          apps_rt_state(rt), apps_rt_last_error(rt));
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* A throttled app's timers wait 100 ms more, those it arms while it is throttled too, and a cool window ends it. */
static void duty_throttle(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, ARM_APP);
    if (rt == NULL) {
        return;
    }
    int64_t due = arm_at(rt, 1050);
    CHECK(due == 1100, "a timer armed at 1050 for 50 ms is due at %lld", (long long)due);
    for (int w = 0; w < 3; w++) {
        hot_window(rt, 1000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * w), "work at the end of window %d", w + 1);
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0, "three hot windows: '%s'", apps_rt_warnings(rt));
    due = arm_at(rt, 4050);
    CHECK(due == 4200, "a timer armed at 4050 for 50 ms, while throttled, is due at %lld", (long long)due);
    /* Nothing but those two calls in the window that follows: it is cool, and the throttle ends with it. */
    CHECK(!apps_rt_has_work(rt, 5000), "work at the end of the cool window");
    due = arm_at(rt, 5050);
    CHECK(due == 5100, "a timer armed at 5050 for 50 ms, after the throttle, is due at %lld", (long long)due);
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* A call closes the windows that have ended before it runs, with no pass of the loop in between (the task asleep in its queue
 * across a boundary wakes for an event): the warning is there after the first call of the window that follows the third hot
 * one, and the first call of the one after the fifth is refused, with the kill as its result. */
static void duty_calls(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch(&pkg, BURN_APP);
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 5; w++) {
        hot_window(rt, 1000 + 1000 * w);
        /* Window w is closed by the call that opens window w + 1, so the third hot one by the first call of the fourth. */
        CHECK((apps_rt_warnings(rt)[0] != '\0') == (w >= 3), "after the calls of window %d: '%s'", w + 1, apps_rt_warnings(rt));
    }
    expect_logged(THROTTLED_TEXT);
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "state %d", apps_rt_state(rt));
    char data[16], err[256] = "";
    snprintf(data, sizeof data, "%u", burn_size());
    port_mono_ms = 6100;
    char *reply = apps_rt_message(rt, "burn", data, strlen(data), err, sizeof err);
    CHECK(reply == NULL && strcmp(err, KILLED_TEXT) == 0, "the call after the fifth hot window: '%s' '%s'",
          reply != NULL ? reply : "(none)", err);
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILLED_TEXT) == 0, "%d '%s'",
          apps_rt_state(rt), apps_rt_last_error(rt));
    apps_port_free(reply);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* ---- Calls that take time ----
 *
 * apphost's clock stands still inside a call, so there no call straddles the end of a window and the time a call takes is 0:
 * the instructions are the only measure. On the board a call takes up to 0.8 s (the budget of 2 M instructions at 2.5 M a
 * second) and C work costs time and no instructions. The app of these cases says how long each call took by calling
 * advance_ms(n), a function of the harness's own, which moves the case's clock n ms right there: a call that begins at 1250
 * and takes 760 ms ends at 2010, in the next window. The windows are the whole seconds of the clock (the first began at
 * 1000), and a call's instructions and time go to the windows it spans in proportion to the time it spent in each. */

static int advance_ms(lua_State *L) {
    port_mono_ms += luaL_checkinteger(L, 1);
    return 0;
}

/* charge(n): n VM instructions to the call, to the instruction, as a C function that walks does (apps_vm_charge). */
static int charge_instr(lua_State *L) {
    apps_vm_charge(L, (unsigned)luaL_checkinteger(L, 1));
    return 0;
}

/* A runtime for main_src with advance_ms and charge in its globals (put there before main.lua's handlers run), made and started. */
static apps_rt_t *launch_clocked(apps_pkg_t **pkg_out, const char *main_src) {
    *pkg_out = make_pkg(main_src);
    char err[256] = "";
    apps_rt_t *rt = apps_rt_new(*pkg_out, err, sizeof err);
    CHECK(rt != NULL, "apps_rt_new failed: %s", err);
    if (rt != NULL) {
        lua_State *L = apps_vm_lua(apps_rt_vm(rt));
        lua_pushcfunction(L, advance_ms);
        lua_setglobal(L, "advance_ms");
        lua_pushcfunction(L, charge_instr);
        lua_setglobal(L, "charge");
        CHECK(apps_rt_start(rt), "start: %s", apps_rt_last_error(rt));
    }
    return rt;
}

/* `work`: a loop of n iterations (n VM instructions) that takes ms. */
static const char *const WORK_APP =
    "return {message = function(event, d)\n"
    "  for i = 1, d[1] do end\n"
    "  advance_ms(d[2])\n"
    "  return true\n"
    "end}\n";

/* The call at `at` that burns n instructions and takes ms: its reply, or NULL with err when it is refused. */
static char *work_at(apps_rt_t *rt, int64_t at, unsigned n, int ms, char *err, size_t err_len) {
    char data[48];
    snprintf(data, sizeof data, "[%u,%d]", n, ms);
    port_mono_ms = at;
    return apps_rt_message(rt, "work", data, strlen(data), err, err_len);
}

static void work_ok_at(int line, apps_rt_t *rt, int64_t at, unsigned n, int ms) {
    char err[256] = "";
    char *reply = work_at(rt, at, n, ms, err, sizeof err);
    CHECK_AT(line, reply != NULL, "the call at %lld (%u instructions, %d ms) was refused: '%s'", (long long)at, n, ms, err);
    apps_port_free(reply);
}
#define WORK(rt, at, n, ms) work_ok_at(__LINE__, (rt), (at), (n), (ms))

static bool warned(const apps_rt_t *rt) {
    return apps_rt_warnings(rt)[0] != '\0';
}

/* The review's trace: a call of 1.9 M instructions, which takes 0.76 s on the board, every 1.25 s. A call is booked to the
 * windows it ran in, so no second goes unseen: calls at 1250, 2500, 3750, 5000 fill 750, 510, 510, 510 and 760 ms of the
 * windows from 1000 on (each is a hot window: over 400 ms and over the rate in instructions). The old rule booked a call wholly
 * to the window it began in, and the window the call at 3750 ran on into (4000 to 5000) had no call that began there: a cool
 * window, the count began again, and the app was never killed. Here the third hot window ends inside the call at 3750, and
 * its share closes it: the warning is there when that call returns. The fifth, 5000 to 6000, is closed by the loop's pass. */
static void duty_trace(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, WORK_APP);
    if (rt == NULL) {
        return;
    }
    const unsigned n = APPS_BUDGET_CALL / 100 * 95;
    for (int k = 0; k < 4; k++) {
        WORK(rt, 1250 + 1250 * k, n, 760);
        CHECK(warned(rt) == (k >= 2), "after the call at %d, which ends at %d: '%s'", 1250 + 1250 * k, 2010 + 1250 * k,
              apps_rt_warnings(rt));
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0, "'%s'", apps_rt_warnings(rt));
    CHECK(!apps_rt_has_work(rt, 5999), "the fifth window closed a millisecond early");
    CHECK(apps_rt_has_work(rt, 6000), "the fifth hot window (5000 to 6000, 760 ms of the call at 5000) isn't the kill");
    char err[256] = "";
    char *reply = work_at(rt, 6250, n, 760, err, sizeof err);
    CHECK(reply == NULL && strcmp(err, KILLED_TEXT) == 0, "the call at 6250: '%s' '%s'", reply != NULL ? reply : "(none)", err);
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILLED_TEXT) == 0, "%d '%s'",
          apps_rt_state(rt), apps_rt_last_error(rt));
    apps_port_free(reply);
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* A call's instructions go to the windows it spanned in proportion to the time in each. Calls of 400 ms and 1.9 M instructions
 * that begin 200 ms before a boundary (at 1800, 2800, ...) put half of each in each window: the window 1000 to 2000 holds
 * 950 k (the call's first half), and every window after it a tail and a head, 1.9 M in 400 ms: hot by instructions, with its
 * time exactly the 400 ms that isn't over the limit. So the hot windows are those from 2000 on, the third is closed when the
 * call at 4800 ends (it runs into 5000), and the fifth when the call at 6800 does: that call is not refused (the kill is
 * for the one after it). Booked to the window it began in, every window from 1000 on would be hot, and the call at 6800 would
 * be refused. */
static void duty_split(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, WORK_APP);
    if (rt == NULL) {
        return;
    }
    const unsigned n = APPS_BUDGET_CALL / 100 * 95;
    for (int k = 0; k < 6; k++) {
        WORK(rt, 1800 + 1000 * k, n, 400);
        /* The call at 1800 + 1000 k ends at 2200 + 1000 k and closes the window it began in only when that window has a tail and
         * a head (k >= 1): the windows 2000 to 3000, 3000 to 4000, 4000 to 5000 are the first three hot ones, so the warning is
         * there after the call at 4800 (k = 3). */
        CHECK(warned(rt) == (k >= 3), "after the call at %d: '%s'", 1800 + 1000 * k, apps_rt_warnings(rt));
    }
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "state %d", apps_rt_state(rt));
    CHECK(apps_rt_has_work(rt, 7200), "the fifth hot window (6000 to 7000) isn't the kill");
    char err[256] = "";
    char *reply = work_at(rt, 7800, 1000, 1, err, sizeof err);
    CHECK(reply == NULL && strcmp(err, KILLED_TEXT) == 0, "the call after it: '%s' '%s'", reply != NULL ? reply : "(none)", err);
    apps_port_free(reply);
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* C work no count hook sees (the review's s:upper() loop: 200 calls of a byte loop over a copy, 450 ms on the board and a few
 * hundred instructions) is hot by the time it takes: a call of 450 ms in each window is over 40% of it. 400 ms is not (it has
 * to be over), 401 is. A light app (a call of 1 ms every 40 ms) is never hot. */
static const char *const CWORK_APP =
    "local s = ('x'):rep(20000)\n"
    "return {message = function(event, ms)\n"
    "  for i = 1, 200 do local u = s:upper() end\n"
    "  advance_ms(ms)\n"
    "  return true\n"
    "end}\n";

static char *cwork_at(apps_rt_t *rt, int64_t at, int ms, char *err, size_t err_len) {
    char data[16];
    snprintf(data, sizeof data, "%d", ms);
    port_mono_ms = at;
    return apps_rt_message(rt, "cwork", data, strlen(data), err, err_len);
}

static void cwork_ok_at(int line, apps_rt_t *rt, int64_t at, int ms) {
    char err[256] = "";
    char *reply = cwork_at(rt, at, ms, err, sizeof err);
    CHECK_AT(line, reply != NULL, "the call at %lld (%d ms) was refused: '%s'", (long long)at, ms, err);
    apps_port_free(reply);
}
#define CWORK(rt, at, ms) cwork_ok_at(__LINE__, (rt), (at), (ms))

static void duty_busy(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, CWORK_APP);
    if (rt == NULL) {
        return;
    }
    /* 400 ms in each window, eight windows: cool. */
    for (int k = 0; k < 8; k++) {
        CWORK(rt, 1100 + 1000 * k, 400);
    }
    CHECK(!apps_rt_has_work(rt, 9000) && !warned(rt) && apps_rt_state(rt) == APPS_STATE_RUNNING,
          "400 ms of each second is the limit, not over it: '%s'", apps_rt_warnings(rt));
    CHECK(apps_vm_instructions(apps_rt_vm(rt)) < APPS_DUTY_INSTR_PER_S / 10, "the calls were meant to cost no instructions: %llu",
          (unsigned long long)apps_vm_instructions(apps_rt_vm(rt)));
    finish(rt, pkg);

    /* 401 ms is over it: throttled by the third window, killed by the fifth, with no instruction to show for it. */
    port_mono_ms = 1000;
    rt = launch_clocked(&pkg, CWORK_APP);
    if (rt == NULL) {
        return;
    }
    for (int k = 0; k < 5; k++) {
        CWORK(rt, 1100 + 1000 * k, 401);
        /* Window k is closed by the call that opens window k + 1 (the one at 2100 closes 1000 to 2000), and the warning is there
         * when the third has closed: after the call at 4100. */
        CHECK(warned(rt) == (k >= 3), "after the call at %d: '%s'", 1100 + 1000 * k, apps_rt_warnings(rt));
    }
    CHECK(apps_vm_instructions(apps_rt_vm(rt)) < APPS_DUTY_INSTR_PER_S / 10, "the calls were meant to cost no instructions: %llu",
          (unsigned long long)apps_vm_instructions(apps_rt_vm(rt)));
    char err[256] = "";
    char *reply = cwork_at(rt, 6100, 401, err, sizeof err);
    CHECK(reply == NULL && strcmp(err, KILLED_TEXT) == 0, "the call after the fifth: '%s' '%s'", reply != NULL ? reply : "(none)", err);
    apps_port_free(reply);
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);

    /* A light app: 25 calls a second, a millisecond each. */
    port_mono_ms = 1000;
    rt = launch_clocked(&pkg, CWORK_APP);
    if (rt == NULL) {
        return;
    }
    for (int second = 0; second < 8; second++) {
        for (int i = 0; i < 25; i++) {
            CWORK(rt, 1000 + 1000 * second + 40 * i, 1);
        }
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * second) && !warned(rt), "second %d: '%s'", second, apps_rt_warnings(rt));
    }
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "state %d", apps_rt_state(rt));
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* The status's cpu_pct is the share of the last window to close that the calls took, from the booking that decides what is hot:
 * the runtime closes the windows that have ended (as a pass of the loop would) to say, a call that ran past the end of a window
 * has closed it as its share landed, and a runtime that isn't running keeps what it had. */
static void duty_cpu_pct(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, WORK_APP);
    if (rt == NULL) {
        return;
    }
    CHECK(apps_rt_cpu_pct(NULL, 0) == 0, "NULL");
    WORK(rt, 1100, 1000, 500);  /* 500 ms of the window 1000 to 2000 */
    CHECK(apps_rt_cpu_pct(rt, 1999) == 0, "the window is still open: %u", apps_rt_cpu_pct(rt, 1999));
    CHECK(apps_rt_cpu_pct(rt, 2000) == 50, "500 ms of a second: %u%%", apps_rt_cpu_pct(rt, 2000));
    CHECK(apps_rt_cpu_pct(rt, 3000) == 0, "a window with no call in it: %u%%", apps_rt_cpu_pct(rt, 3000));
    WORK(rt, 3900, 1000, 400);  /* 100 ms of 3000 to 4000, which the call closes by running past it, and 300 of 4000 to 5000 */
    CHECK(apps_rt_cpu_pct(rt, 4300) == 10, "100 ms of the window the call ran out of: %u%%", apps_rt_cpu_pct(rt, 4300));
    CHECK(apps_rt_cpu_pct(rt, 5000) == 30, "300 ms of the next: %u%%", apps_rt_cpu_pct(rt, 5000));
    WORK(rt, 5000, 1000, 1000);  /* all of 5000 to 6000 */
    CHECK(apps_rt_cpu_pct(rt, 6000) == 100, "all of a second: %u%%", apps_rt_cpu_pct(rt, 6000));
    apps_rt_stop(rt);
    CHECK(apps_rt_cpu_pct(rt, 99999) == 100, "a stopped runtime keeps what it had: %u%%", apps_rt_cpu_pct(rt, 99999));
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* ---- The loop's pace ----
 *
 * drive() is the app task's loop on the case's clock (apps_task.c): it waits until the earliest timer is due, at most a second,
 * and a timer that is due already waits a millisecond (one tick); then it asks apps_rt_has_work and takes one step with the
 * time it woke at, as the task's round does. The callback of an every timer prints when it began (the time at its first
 * instruction) and takes `ms`. */
static void drive(apps_rt_t *rt, int64_t until) {
    while (port_mono_ms < until && apps_rt_state(rt) == APPS_STATE_RUNNING) {
        int64_t now = port_mono_ms, due = apps_rt_next_deadline(rt);
        int64_t wake = due == INT64_MAX || due > now + 1000 ? now + 1000 : due;
        port_mono_ms = wake > now ? wake : now + 1;
        if (apps_rt_has_work(rt, port_mono_ms)) {
            (void)apps_rt_run_one_timer(rt, port_mono_ms);
        }
    }
}

/* The times the callbacks began, from the log (the lines "cb<TAB>time"). */
static size_t callback_starts(int64_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < logged_n && n < cap; i++) {
        if (strncmp(logged[i], "cb\t", 3) == 0) {
            out[n++] = strtoll(logged[i] + 3, NULL, 10);
        }
    }
    return n;
}

/* An app whose every(period) callback burns n instructions and takes ms; started at 1000 on the case's clock. */
static apps_rt_t *launch_every(apps_pkg_t **pkg_out, int period, unsigned n, int ms) {
    char source[320];
    snprintf(source, sizeof source,
             "return {start = function()\n"
             "  timer.every(%d, function()\n"
             "    print('cb', time.mono())\n"
             "    for i = 1, %u do end\n"
             "    advance_ms(%d)\n"
             "  end)\n"
             "end}\n",
             period, n, ms);
    port_mono_ms = 1000;
    return launch_clocked(pkg_out, source);
}

#define GAPS_MAX 32

static size_t gaps_of(const int64_t *starts, size_t n, int64_t *gaps) {
    size_t g = 0;
    for (size_t i = 1; i < n && g < GAPS_MAX; i++) {
        gaps[g++] = starts[i] - starts[i - 1];
    }
    return g;
}

/* An every timer is due a period after it began (the brief: now + period), however long its callback took, until the app is
 * throttled; from then on it is due a period and 100 ms after the callback ENDED, so that the delay is idle time (a callback that
 * outlasts its period and 100 ms is as slow after the throttle as before it, otherwise, and the app task never gives the core
 * up). The shift of 100 ms that the third hot window gives every timer is the first gap after it.
 *
 * every(500) with a callback of 300 ms and 600 k instructions: 2 calls in a second, 600 ms and 1.2 M of them in it, hot. It begins
 * at 1500, 2000, ... (500 apart: not 800); the third hot window (3000 to 4000... 2000 to 3000, 3000 to 4000, 4000 to 5000)
 * ends at 5000, when the loop's pass closes it: the timer due at 5000 moves to 5100 (gap 600), and since the callback of 5100
 * ended at 5400 the next is due at 5400 + 500 + 100 = 6000 (gap 900, not 600). The window 5000 to 6000 holds that one call,
 * and is cool: the throttle ends, and the next gap is a period again. */
static void duty_pace(void) {
    int64_t starts[GAPS_MAX + 1], gaps[GAPS_MAX];
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_every(&pkg, 500, APPS_DUTY_INSTR_PER_S / 5 * 3, 300);
    if (rt == NULL) {
        return;
    }
    drive(rt, 8000);
    size_t n = callback_starts(starts, GAPS_MAX + 1), g = gaps_of(starts, n, gaps);
    CHECK(g >= 9, "%zu callbacks", n);
    for (size_t i = 0; i < g && i < 9; i++) {
        int64_t want = i < 6 ? 500 : i == 6 ? 600 : i == 7 ? 900 : 500;
        CHECK(gaps[i] == want, "gap %zu is %lld (the callbacks began at %lld and %lld), want %lld", i, (long long)gaps[i],
              (long long)starts[i], (long long)starts[i + 1], (long long)want);
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING, "'%s' %d",
          apps_rt_warnings(rt), apps_rt_state(rt));
    finish(rt, pkg);

    /* An app that is hot all the same while it is throttled is killed: every(40) with a callback of 400 ms and a rate of
     * instructions begins every 401 ms (due long ago: the loop's tick), and after the third hot window every 540 (the
     * callback's end, and 40, and 100), which is idle time (it is 74% of the core still: the fifth window is the kill). */
    rt = launch_every(&pkg, 40, APPS_DUTY_INSTR_PER_S, 400);
    if (rt == NULL) {
        return;
    }
    drive(rt, 10000);
    n = callback_starts(starts, GAPS_MAX + 1);
    g = gaps_of(starts, n, gaps);
    CHECK(g >= 9, "%zu callbacks", n);
    for (size_t i = 0; i < g; i++) {
        int64_t want = i < 7 ? 401 : 540;
        CHECK(gaps[i] == want, "every(40): gap %zu is %lld (the callbacks began at %lld and %lld), want %lld", i, (long long)gaps[i],
              (long long)starts[i], (long long)starts[i + 1], (long long)want);
    }
    CHECK(apps_rt_state(rt) == APPS_STATE_CRASHED && strcmp(apps_rt_last_error(rt), KILLED_TEXT) == 0, "%d '%s'",
          apps_rt_state(rt), apps_rt_last_error(rt));
    finish(rt, pkg);

    /* A callback that fits in its period and isn't hot doesn't move the timer: every(1000), 300 ms, begins every 1000. */
    rt = launch_every(&pkg, 1000, APPS_DUTY_INSTR_PER_S / 10, 300);
    if (rt == NULL) {
        return;
    }
    drive(rt, 12000);
    n = callback_starts(starts, GAPS_MAX + 1);
    g = gaps_of(starts, n, gaps);
    CHECK(g >= 9, "%zu callbacks", n);
    for (size_t i = 0; i < g; i++) {
        CHECK(gaps[i] == 1000, "every(1000): gap %zu is %lld", i, (long long)gaps[i]);
    }
    CHECK(!warned(rt) && apps_rt_state(rt) == APPS_STATE_RUNNING, "'%s' %d", apps_rt_warnings(rt), apps_rt_state(rt));
    finish(rt, pkg);

    /* The review's app, as a timer: every(1250), 760 ms, 1.9 M instructions. Hot from the first window; throttled when the third
     * ends (inside the call that began at 4750), and from then each call is 760 + 1250 + 100 = 2110 ms after the last began.
     * That is 36% of the core, so its windows turn cool and the throttle ends: the old gaps, and the throttle again. The kill is
     * for an app that stays over 40% in spite of the throttle (the one above); this one is held to the 40% by it. */
    rt = launch_every(&pkg, 1250, APPS_BUDGET_CALL / 100 * 95, 760);
    if (rt == NULL) {
        return;
    }
    drive(rt, 15000);
    n = callback_starts(starts, GAPS_MAX + 1);
    g = gaps_of(starts, n, gaps);
    CHECK(g >= 5, "%zu callbacks", n);
    for (size_t i = 0; i < g && i < 5; i++) {
        int64_t want = i == 2 ? 2110 : 1250;
        CHECK(gaps[i] == want, "every(1250): gap %zu is %lld (the callbacks began at %lld and %lld), want %lld", i, (long long)gaps[i],
              (long long)starts[i], (long long)starts[i + 1], (long long)want);
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING, "'%s' %d",
          apps_rt_warnings(rt), apps_rt_state(rt));
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* A window of exactly APPS_DUTY_INSTR_PER_S instructions isn't over it: cool, so four of them in a row are no throttle; the rate
 * and one more is hot, and three of those are. The call charges its instructions to the instruction (charge: a loop's are counted
 * in the hook's thousands), and each call's count is checked to be what it was charged, so that the boundary is hit exactly. */
static const char *const RATE_APP = "return {message = function(event, n) charge(n) return true end}\n";

static void duty_rate(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, RATE_APP);
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 7; w++) {
        const unsigned n = APPS_DUTY_INSTR_PER_S + (w < 4 ? 0 : 1);
        char data[16], err[256] = "";
        snprintf(data, sizeof data, "%u", n);
        uint64_t before = apps_vm_instructions(apps_rt_vm(rt));
        port_mono_ms = 1100 + 1000 * w;
        char *reply = apps_rt_message(rt, "charge", data, strlen(data), err, sizeof err);
        CHECK(reply != NULL, "the call at %lld: '%s'", (long long)port_mono_ms, err);
        apps_port_free(reply);
        uint64_t counted = apps_vm_instructions(apps_rt_vm(rt)) - before;
        CHECK(counted == n, "a call charged %u was counted %llu", n, (unsigned long long)counted);
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * w), "work at the end of window %d", w + 1);
        CHECK(warned(rt) == (w == 6), "after window %d (%u instructions): '%s'", w + 1, n, apps_rt_warnings(rt));
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0 && apps_rt_state(rt) == APPS_STATE_RUNNING, "'%s' %d",
          apps_rt_warnings(rt), apps_rt_state(rt));
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* A throttled every timer is moved by the call that ends, and only if the timer is still the one that ran: a callback that
 * cancels its own timer and arms another, which takes the slot, leaves that one due as it was armed (its own period and the
 * throttle's 100 ms from the time it was armed), and not from the end of a callback that has nothing to do with it. The callback
 * takes 30 ms after it arms: from the end the new timer would be due 30 ms later than it is. */
static const char *const SWAP_APP =
    "local h\n"
    "return {message = function(event, n)\n"
    "  if event == 'go' then\n"
    "    h = timer.every(200, function()\n"
    "      h:cancel()\n"
    "      h = timer.every(300, function() end)\n"
    "      advance_ms(30)\n"
    "    end)\n"
    "    return true\n"
    "  end\n"
    "  for i = 1, n do end\n"
    "  return true\n"
    "end}\n";

static void duty_swap(void) {
    port_mono_ms = 1000;
    apps_pkg_t *pkg;
    apps_rt_t *rt = launch_clocked(&pkg, SWAP_APP);
    if (rt == NULL) {
        return;
    }
    for (int w = 0; w < 3; w++) {
        hot_window(rt, 1000 + 1000 * w);
        CHECK(!apps_rt_has_work(rt, 2000 + 1000 * w), "work at the end of window %d", w + 1);
    }
    CHECK(strcmp(apps_rt_warnings(rt), THROTTLED_TEXT) == 0, "three hot windows: '%s'", apps_rt_warnings(rt));
    char err[256] = "";
    port_mono_ms = 4100;
    char *reply = apps_rt_message(rt, "go", NULL, 0, err, sizeof err);
    CHECK(reply != NULL, "go: '%s'", err);
    apps_port_free(reply);
    CHECK(apps_rt_next_deadline(rt) == 4400, "armed at 4100 for 200 ms, throttled: due at %lld", (long long)apps_rt_next_deadline(rt));
    port_mono_ms = 4400;
    CHECK(apps_rt_run_one_timer(rt, 4400), "the timer ran");
    CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING && apps_rt_timer_count(rt) == 1, "state %d, %d timers", apps_rt_state(rt),
          apps_rt_timer_count(rt));
    int64_t due = apps_rt_next_deadline(rt);
    CHECK(due == 4800, "the timer armed at 4400 for 300 ms, throttled, is due at %lld (not 4830, from the callback's end)",
          (long long)due);
    expect_logged(THROTTLED_TEXT);
    finish(rt, pkg);
    port_mono_ms = 0;
}

/* ---- The C stack under this module's bindings ----
 *
 * The recursions that go deepest in C through print and require, each to Lua's C-call limit, on a thread whose
 * stack is exactly TASK_STACK_HOST bytes above a guard page, as the vm harness does it for the sandbox's own
 * (the Task 5 rule): 96 KB stands in for the device's 56 KB, since host frames are 1.5 to 1.7 times Xtensa's,
 * and the build has no sanitizers, which fatten frames. Prints each case's high-water mark. The device is the
 * authority (Task 15). */

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif

#ifdef UNDER_ASAN

static void c_stack(void) {
    CHECK(false, "c_stack measures stacks: build it with apps_host.build(..., sanitize=False)");
}

#else

#define TASK_STACK_HOST ((size_t)96 * 1024)

static lua_Integer stack_depth;

/* Runs the message "go" of the package, which recurses until Lua's C-call limit, and keeps how deep it got (the
 * global `depth`, which the chain counts). The error is the message's alone: the app goes on. */
static void stack_chain(const char *name, const file_t *files, int n) {
    apps_pkg_t *pkg = make_pkg_kb(256, files, n);
    char err[256] = "";
    apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
    CHECK(rt != NULL && apps_rt_start(rt), "%s: %s", name, err);
    if (rt != NULL) {
        char *reply = send(rt, "go", err, sizeof err);
        CHECK(reply == NULL && strstr(err, "stack overflow") != NULL, "%s: '%s'", name, err);
        CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "%s: state %d", name, apps_rt_state(rt));
        lua_State *L = apps_vm_lua(apps_rt_vm(rt));
        lua_getglobal(L, "depth");
        stack_depth = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    finish(rt, pkg);
}

static void stack_require_chain(void) {
    const file_t files[] = {
        {"main.lua", "depth = 0\nreturn {message = function() return require('a') end}"},
        {"a.lua", "depth = depth + 1 return require('b')"},
        {"b.lua", "depth = depth + 1 return require('a')"},
    };
    stack_chain("require_chain", files, 3);
}

static void stack_print_chain(void) {
    const file_t files[] = {{"main.lua",
                             "depth = 0\n"
                             "local o = setmetatable({}, {__tostring = function(self) depth = depth + 1 print(self) "
                             "return 'x' end})\n"
                             "return {message = function() print(o) end}"}};
    stack_chain("print_chain", files, 1);
}

/* Task 16: os.time reads the fields of the table it is given with lua_getfield, which runs the table's __index, which
 * here calls os.time with the same table again, to the C-call limit. (time.format, os.date and the timer functions run no
 * app code from C: the timers' callbacks are called by the runtime, outside any C frame of the module.) */
static void stack_os_time_chain(void) {
    const file_t files[] = {{"main.lua",
                             "depth = 0\n"
                             "local t = setmetatable({}, {__index = function(self, key) depth = depth + 1 "
                             "return os.time(self) end})\n"
                             "return {message = function() return os.time(t) end}"}};
    stack_chain("os_time_chain", files, 1);
}

/* The reviewer's probe (Task 8's review): string.format('%s', o), which holds a luaL_Buffer, calls o's __tostring,
 * which does the same one level down, to the C-call limit. The message runs it on the main thread or in a
 * coroutine, and each level may first pcall(require, 'deep'). deep.lua is nested 60 deep, past any thread's
 * C-call room, so a require parses as deep as the count of the thread that compiles it allows, then fails
 * ("C stack overflow"), and the module is never cached: every level parses again. Before the fix a coroutine's
 * require compiled on the main thread, whose count leaves out the coroutine's C levels, so the parse went about
 * 38 levels deeper than the coroutine's count allowed, on top of the chain. */
static bool format_in_coroutine, format_requires;
static lua_Integer format_parsed;

static void stack_format_chain(void) {
    char deep[256];
    int n = snprintf(deep, sizeof deep, "return ");
    memset(deep + n, '(', 60);
    n += 60;
    deep[n++] = '1';
    memset(deep + n, ')', 60);
    n += 60;
    deep[n] = '\0';
    char main_lua[1024];
    snprintf(main_lua, sizeof main_lua,
             "depth = 0 parsed = 0\n"
             "local o\n"
             "o = setmetatable({}, {__tostring = function(self)\n"
             "  depth = depth + 1\n"
             "%s"
             "  local s = string.format('%%s', self) depth = depth - 1 return s end})\n"
             "local function run() local ok, e = pcall(string.format, '%%s', o) return tostring(e):sub(1, 120) end\n"
             "return {message = function() return %s end}\n",
             format_requires ? "  if not pcall(require, 'deep') then parsed = parsed + 1 end\n" : "",
             format_in_coroutine ? "coroutine.wrap(run)()" : "run()");
    const file_t files[] = {{"main.lua", main_lua}, {"deep.lua", deep}};
    apps_pkg_t *pkg = make_pkg_kb(256, files, 2);
    char err[256] = "";
    apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
    CHECK(rt != NULL && apps_rt_start(rt), "format chain: %s", err);
    if (rt != NULL) {
        char *reply = send(rt, "go", err, sizeof err);
        CHECK(reply != NULL && strstr(reply, "stack overflow") != NULL, "format chain: reply '%s', err '%s'",
              reply != NULL ? reply : "(none)", err);
        apps_port_free(reply);
        CHECK(apps_rt_state(rt) == APPS_STATE_RUNNING, "format chain: state %d", apps_rt_state(rt));
        lua_State *L = apps_vm_lua(apps_rt_vm(rt));
        lua_getglobal(L, "depth");
        stack_depth = lua_tointeger(L, -1);
        lua_getglobal(L, "parsed");
        format_parsed = lua_tointeger(L, -1);
        lua_pop(L, 2);
    }
    finish(rt, pkg);
}

typedef struct {
    const char *name;
    void (*run)(void);
} stack_case_t;

static void *run_stack_case(void *c) {
    ((stack_case_t *)c)->run();
    return NULL;
}

/* Runs c on a thread whose stack is exactly TASK_STACK_HOST bytes: an mmap'd region above a PROT_NONE page,
 * painted first, so the deepest byte the case changed shows. Returns how far down that is from the top (the
 * high-water mark), or 0 if the thread didn't run. */
static size_t on_exact_stack(stack_case_t *c) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE), size = TASK_STACK_HOST;
    unsigned char *map = mmap(NULL, page + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        CHECK(false, "%s: mmap failed", c->name);
        return 0;
    }
    unsigned char *stack = map + page;
    int rc = mprotect(map, page, PROT_NONE);
    memset(stack, 0xA5, size);
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstack(&attr, stack, size);
    }
    if (rc == 0) {
        rc = pthread_create(&thread, &attr, run_stack_case, c);
    }
    if (rc == 0) {
        pthread_join(thread, NULL);
    }
    pthread_attr_destroy(&attr);
    size_t untouched = 0;
    while (untouched < size && stack[untouched] == 0xA5) {
        untouched++;
    }
    munmap(map, page + size);
    CHECK(rc == 0, "%s: no thread on the exact stack (%d)", c->name, rc);
    return rc == 0 ? size - untouched : 0;
}

/* The room a require adds to a chain on the same kind of thread: the parse of what it compiles, which the
 * C-call limit bounds like every other recursion, so a few levels of it at most (each of a parser's levels is far
 * smaller than the chain's own). Before the fix the coroutine's chain took 8.6 KB or more over the same chain
 * without the require. */
#define REQUIRE_SLACK ((size_t)4096)

static void c_stack(void) {
    static stack_case_t cases[] = {{"require_chain", stack_require_chain},
                                   {"print_chain", stack_print_chain},
                                   {"os_time_chain", stack_os_time_chain}};
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        stack_depth = 0;
        size_t used = on_exact_stack(&cases[i]);
        printf("c_stack: %-22s %6zu of %zu bytes (depth %lld)\n", cases[i].name, used, TASK_STACK_HOST,
               (long long)stack_depth);
        fflush(stdout);
        CHECK(used > 0, "%s: the case didn't run", cases[i].name);
        CHECK(stack_depth > 30, "%s: reached depth %lld, short of the C-call limit", cases[i].name,
              (long long)stack_depth);
    }

    /* The format chain, with and without a require at each level, on the main thread and in a coroutine. */
    static stack_case_t format = {"format_chain", stack_format_chain};
    size_t used[2][2];  /* [in a coroutine][with require] */
    for (int co = 0; co < 2; co++) {
        for (int req = 0; req < 2; req++) {
            format_in_coroutine = co != 0;
            format_requires = req != 0;
            stack_depth = format_parsed = 0;
            used[co][req] = on_exact_stack(&format);
            printf("c_stack: %s%s %6zu of %zu bytes (depth %lld, %lld modules failed to compile)\n",
                   co ? "co_format_chain" : "format_chain", req ? "_require" : "", used[co][req], TASK_STACK_HOST,
                   (long long)stack_depth, (long long)format_parsed);
            fflush(stdout);
            CHECK(used[co][req] > 0, "format chain %d/%d: the case didn't run", co, req);
            CHECK(stack_depth > 30, "format chain %d/%d: depth %lld, short of the C-call limit", co, req,
                  (long long)stack_depth);
            /* With require, every level's compile fails (60 levels don't fit) and is reported as the require's
             * error, which the chain catches: the app goes on, and the stack comes back. */
            CHECK(req ? format_parsed >= stack_depth - 2 : format_parsed == 0,
                  "format chain %d/%d: %lld failed compiles, depth %lld", co, req, (long long)format_parsed,
                  (long long)stack_depth);
        }
        CHECK(used[co][1] <= used[co][0] + REQUIRE_SLACK,
              "%s: a require at each level takes %zu bytes, %zu more than the chain without it (slack %zu)",
              co ? "in a coroutine" : "on the main thread", used[co][1], used[co][1] - used[co][0], REQUIRE_SLACK);
    }
}

#endif /* UNDER_ASAN */

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"lifecycle", lifecycle},
        {"crashed", crashed},
        {"external_kill", external_kill},
        {"null_arguments", null_arguments},
        {"no_main", no_main},
        {"boot_crash", boot_crash},
        {"optional_handlers", optional_handlers},
        {"state_names", state_names},
        {"stack_balanced", stack_balanced},
        {"err_cut", err_cut},
        {"ring", ring},
        {"tail_cuts", tail_cuts},
        {"port_log", port_log},
        {"compile_all", compile_all},
        {"compile_refused", compile_refused},
        {"new_refused", new_refused},
        {"handlers_refused", handlers_refused},
        {"message_refused", message_refused},
        {"require_refused", require_refused},
        {"time_zone", time_zone},
        {"services_refused", services_refused},
        {"timer_api", timer_api},
        {"timer_tie", timer_tie},
        {"os_time_charge", os_time_charge},
        {"duty_jump", duty_jump},
        {"duty_late_kill", duty_late_kill},
        {"duty_pass", duty_pass},
        {"duty_calls", duty_calls},
        {"duty_throttle", duty_throttle},
        {"duty_trace", duty_trace},
        {"duty_split", duty_split},
        {"duty_busy", duty_busy},
        {"duty_cpu_pct", duty_cpu_pct},
        {"duty_pace", duty_pace},
        {"duty_rate", duty_rate},
        {"duty_swap", duty_swap},
        {"c_stack", c_stack},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            forget_logged();
            free(logged);
            CHECK(held == 0, "%s ended with %zu bytes held", argv[1], held);
            return failures != 0 ? 1 : 0;
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
