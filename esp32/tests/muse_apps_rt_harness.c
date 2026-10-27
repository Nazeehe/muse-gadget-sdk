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

int64_t apps_port_mono_ms(void) {
    return 0;
}

bool apps_port_epoch_ms(int64_t *out) {
    (void)out;
    return false;
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
    static stack_case_t cases[] = {{"require_chain", stack_require_chain}, {"print_chain", stack_print_chain}};
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
        {"new_refused", new_refused},
        {"handlers_refused", handlers_refused},
        {"message_refused", message_refused},
        {"require_refused", require_refused},
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
