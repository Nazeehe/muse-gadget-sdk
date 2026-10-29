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
 * apphost: one Muse app on the laptop, run by the runtime the gadget runs (apps_rt), so that what an app
 * does here is what it does there: the same Lua, the same sandbox, budgets, messages and crash rules. It is
 * the development loop's runner and the tests' (tests/test_muse_apps_rt.py).
 *
 *   apphost [--no-compile] [--mem-kb N] [--script FILE|-] APP_DIR
 *   apphost [--mem-kb N] --cycles N APP_DIR
 *   apphost --sha256 TEXT|-
 *
 * APP_DIR holds manifest.json and the files it lists (the folder's name is the app's id). The package is read
 * the way the install pipeline reads one (apps_pkg: the manifest, the size limits, .json files that must
 * parse, .bin files that must be LVGL images) and compiled the way it compiles one (apps_task.c's compile: every
 * .lua file, main first and the others in the package's order, each loaded and not run, in a scratch VM with the
 * manifest's memory limit), so that a package this runs is a package the install accepts. Then the runtime is
 * made and start() is called, and the script's lines run, if there is one. --mem-kb N sets the heap in KB,
 * whatever the manifest says (1 to 65535). --no-compile leaves the compile out: it is for the runtime's own
 * tests of a module that doesn't compile when it is required, which the install never lets in.
 *
 * Output is one JSON object a line on stdout, flushed as it is written, whatever the app printed or raised:
 * every string is escaped, and bytes that aren't valid UTF-8 come out as U+FFFD.
 *   {"event":"warning","message":"…"}      a manifest or runtime warning (each one, split at "; "), at launch
 *   {"event":"started","id":"…","name":"…"}   the app is running: at launch, and for each `start` that returns
 *   {"event":"log","app":"…","line":"…"}     a line the app printed, as it is printed; and the lines of `log`
 *   {"event":"reply","data":<json>}        what message() returned
 *   {"event":"error","message":"…"}        a command that failed and left the app as it was; a package that
 *                                          can't be loaded
 *   {"event":"crash","message":"…"}        the app crashed, as it happens (once: a crash is final): the crash
 *                                          record, the error with its traceback or the kill's reason; or a
 *                                          package whose Lua doesn't compile, refused before anything runs
 *                                          with the install's words: "compile failed: util.lua:7: …" and the
 *                                          lines of the file around line 7 (apps_pkg_compile_message)
 *   {"event":"state","state":"running"}    the `state` command: stopped, running or crashed
 *   {"event":"dump","widgets":[...]}       the `dump` command: one object an alive widget (apps_ui.h), with
 *                                          "dropped":N after them when N didn't fit the size asked for
 *   {"event":"leak_check","bytes":0}       --cycles: what the apps' heap still holds
 * stderr is for the command line only (and an unreadable script), never for the app.
 *
 * The script, one command a line (blank lines and lines starting with # are skipped; text is split at
 * whitespace, and the line ends at its first NUL byte):
 *   message EVENT [JSON]   message(EVENT, JSON as a value, or nil) -> reply; error (a failed message,
 *                          "the app is not running", the crash record of a crashed app); crash
 *   start                  start() -> started; error (the crash record); crash
 *   stop | show | hide     the handler: nothing is printed, unless it crashes the app
 *   state                  -> state
 *   log [N]                the last N lines of the app's log ring, oldest first (all of them by default), as
 *                          log events. An empty log, and a log of one empty line, print nothing.
 *   tap #SLOT[@GEN]        a tap on the widget in that slot, with the gen it has now or the one given: its tap
 *                          handler runs with the app's budget (a crash prints a crash event). A gen the widget
 *                          has moved on from, a widget with no tap handler and an app that isn't running print
 *                          nothing: the touch is dropped, as on the gadget. A slot with no widget is an error.
 *   tap "TEXT"             a tap on the first button or label (in slot order) whose text is TEXT, a JSON string
 *   dump [N]               the widgets as the gadget reports them (apps_ui_dump), in at most N bytes (all of
 *                          them by default)
 * The LVGL task's apply step has no counterpart here but a stand-in that runs after each command: it takes every
 * request as applied, so a deleted widget's slot is free for the next command, as it is on the gadget after
 * a frame. Nothing is laid out, so a dump shows the declared geometry.
 * The UI lock is policed as the gadget needs it kept (apphost_port.h): taken while held, given back without being
 * held, or left held by a command ends the run, and so does a command that changes the screen without taking it.
 * An app whose load failed (a crash event at launch) is a crashed app: `state` says crashed, `message` and
 * `start` answer with its crash record, `log` shows what main.lua printed before it failed, and the rest do
 * nothing. A package whose Lua doesn't compile is no app: nothing ran or runs, the one crash event is all that is
 * printed (not the warnings, not the script's answers), and the status is 1. With --no-compile a main.lua that
 * doesn't compile never ran either, and the crash event is Lua's message with no log; the script's commands
 * answer as above.
 *
 * --cycles N makes, starts, stops and frees the app N times, and prints the heap's balance. Nothing else is
 * printed but the first failure, as a crash event: the app's log and warnings are dropped.
 * --sha256 prints the digest of TEXT, or of stdin with "-", as 64 hex digits (apps_port_sha256).
 *
 * Exit status: 0, the app didn't crash; 1, it crashed, or its package can't be loaded; 2, the command line or
 * the script can't be read (the reason, and the usage, on stderr).
 *
 * Time stands still here (apphost_port.c), so only the instruction budgets stop a runaway app.
 */

/* getline, realpath and PATH_MAX come with POSIX 2008 and a C library's default definitions. One that hides
 * what a standard doesn't name (glibc under -std=c11) declares them only when asked, and this has to come before
 * the first include, which is where the headers decide. _DEFAULT_SOURCE asks for the defaults back; glibc has
 * realpath under it or _XOPEN_SOURCE 500, not under _POSIX_C_SOURCE 200809L alone. */
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apphost_port.h"
#include "apps_json.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_ui.h"
#include "apps_vm.h"

/* The device's CONFIG_MUSE_APPS_HEAP_MAX_KB is set by its build; here the manifest's own ceiling holds. */
#define HEAP_MAX_KB 1024
#define MEM_KB_MAX 65535
#define CYCLES_MAX 1000000
/* A widget's entry in a dump is under 2 KB (apps_ui.h): room for all of them by default. */
#define DUMP_DEFAULT_BYTES (APPS_UI_SLOTS * 2048 + 16)
#define DUMP_MAX_BYTES 4000000

#define USAGE                                                                           \
    "usage: apphost [--no-compile] [--mem-kb N] [--script FILE|-] APP_DIR\n"            \
    "       apphost [--mem-kb N] --cycles N APP_DIR\n"                                  \
    "       apphost --sha256 TEXT|-\n"

/* ---- Events ---- */

/* s[0, len) as a JSON string: quotes, backslash and control bytes escaped, valid UTF-8 as it is, anything else
 * as U+FFFD. */
static void put_string(const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    putchar('"');
    while (p < end) {
        unsigned char c = *p;
        if (c >= 0x80) {
            int n = apps_json_utf8_len(p, end);
            if (n > 0) {
                fwrite(p, 1, (size_t)n, stdout);
                p += n;
            } else {
                fputs("\\ufffd", stdout);
                p++;
            }
            continue;
        }
        switch (c) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (c < 0x20) {
                    printf("\\u00%c%c", hex[c >> 4], hex[c & 15]);
                } else {
                    putchar(c);
                }
        }
        p++;
    }
    putchar('"');
}

static void begin(const char *kind) {
    printf("{\"event\":\"%s\"", kind);
}

static void end_event(void) {
    fputs("}\n", stdout);
    fflush(stdout);
}

static void field(const char *key, const char *value, size_t len) {
    printf(",\"%s\":", key);
    put_string(value, len);
}

/* {"event":kind,"message":text[0, len)}: warning, error and crash. */
static void emit_message(const char *kind, const char *text, size_t len) {
    begin(kind);
    field("message", text, len);
    end_event();
}

static void emit_text(const char *kind, const char *text) {
    emit_message(kind, text, strlen(text));
}

static void emit_log(const char *app_id, const char *line) {
    begin("log");
    field("app", app_id, strlen(app_id));
    field("line", line, strlen(line));
    end_event();
}

/* The warnings of a "; "-joined list, one event each. */
static void emit_warnings(const char *joined) {
    for (const char *p = joined; *p != '\0';) {
        const char *sep = strstr(p, "; ");
        size_t n = sep != NULL ? (size_t)(sep - p) : strlen(p);
        emit_message("warning", p, n);
        p += n + (sep != NULL ? 2 : 0);
    }
}

/* ---- The package ---- */

/*
 * Reads rel in folder: at most cap bytes of it, into a malloc'd block with a NUL after the bytes (the caller
 * asks for one more than the limit, to tell a file over it). False with the message in err.
 */
static bool read_file(const char *folder, const char *rel, size_t cap, uint8_t **data, size_t *len, char *err,
                      size_t err_len) {
    size_t path_len = strlen(folder) + 1 + strlen(rel) + 1;
    char *path = malloc(path_len);
    uint8_t *buf = malloc(cap + 1);
    FILE *f = NULL;
    if (path != NULL && buf != NULL) {
        snprintf(path, path_len, "%s/%s", folder, rel);
        errno = 0;
        f = fopen(path, "rb");
    } else {
        errno = ENOMEM;
    }
    free(path);
    if (f == NULL) {
        snprintf(err, err_len, "reading %s: %s", rel, strerror(errno));
        free(buf);
        return false;
    }
    size_t n = fread(buf, 1, cap, f);
    bool failed = ferror(f) != 0;  /* a directory opens, and fails here */
    int reason = errno;
    fclose(f);
    if (failed) {
        snprintf(err, err_len, "reading %s: %s", rel, strerror(reason != 0 ? reason : EIO));
        free(buf);
        return false;
    }
    buf[n] = '\0';
    uint8_t *fit = realloc(buf, n + 1);  /* the file's size, not the limit's */
    *data = fit != NULL ? fit : buf;
    *len = n;
    return true;
}

/* The limit of a file of this kind, in bytes. */
static size_t kind_max(apps_file_kind_t kind) {
    switch (kind) {
        case APPS_FILE_LUA: return APPS_LUA_MAX;
        case APPS_FILE_JSON: return APPS_JSON_FILE_MAX;
        default: return APPS_IMAGE_MAX;
    }
}

/*
 * Reads the package in folder into pkg (zeroed): the manifest (for the app id `id`), then every file it
 * lists, each checked the way the install pipeline checks it. On failure pkg is empty again and err says why:
 *   reading util.lua: No such file or directory
 *   main.lua is larger than 256 KB        (and 32 KB for .json, 512 KB for images)
 *   the app is larger than 2 MB           (manifest and files)
 *   images add up to more than 1.5 MB
 *   data.json: unexpected '}' at byte 6   (a .json file that doesn't parse)
 *   logo.bin: image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)
 * or, for the manifest, a message of apps_pkg.h ("manifest: ...").
 */
static bool load_package(const char *folder, const char *id, apps_pkg_t *pkg, char *err, size_t err_len) {
    uint8_t *json;
    size_t json_len;
    if (!read_file(folder, "manifest.json", APPS_MANIFEST_MAX + 1, &json, &json_len, err, err_len)) {
        return false;
    }
    bool ok = apps_manifest_parse((const char *)json, json_len, id, HEAP_MAX_KB, &pkg->manifest, err, err_len);
    size_t app_bytes = json_len, image_bytes = 0;
    free(json);
    if (!ok) {
        return false;
    }
    const apps_manifest_t *m = &pkg->manifest;
    for (int i = 0; i < m->file_count; i++) {
        const char *rel = m->files[i];
        apps_file_kind_t kind = apps_file_kind(rel);
        size_t max = kind_max(kind);
        uint8_t *data;
        size_t len;
        if (!read_file(folder, rel, max + 1, &data, &len, err, err_len)) {
            goto fail;
        }
        apps_blob_t *blob = &pkg->files[pkg->count++];  /* from here apps_pkg_free frees data */
        snprintf(blob->path, sizeof blob->path, "%s", rel);
        blob->data = data;
        blob->len = len;
        if (len > max) {
            snprintf(err, err_len, "%s is larger than %u KB", rel, (unsigned)(max / 1024));
            goto fail;
        }
        app_bytes += len;
        if (app_bytes > APPS_APP_MAX) {
            snprintf(err, err_len, "the app is larger than 2 MB");
            goto fail;
        }
        char why[160] = "";
        if (kind == APPS_FILE_JSON && !apps_json_validate((const char *)data, len, why, sizeof why)) {
            snprintf(err, err_len, "%s: %s", rel, why);
            goto fail;
        }
        if (kind == APPS_FILE_IMAGE) {
            image_bytes += len;
            if (image_bytes > APPS_IMAGES_TOTAL_MAX) {
                snprintf(err, err_len, "images add up to more than 1.5 MB");
                goto fail;
            }
            if (!apps_image_check(data, len, NULL, NULL, why, sizeof why)) {
                snprintf(err, err_len, "%s: %s", rel, why);
                goto fail;
            }
        }
    }
    return true;
fail:
    apps_pkg_free(pkg, NULL);
    return false;
}

/* ---- The compile ---- */

/*
 * What the install's compile step does (apps_task.c's compile()): every .lua file of the package, main first and the
 * others in the package's order, loaded and not run, in a scratch VM with the manifest's memory limit. False, with
 * what Lua said of the first that failed in raw, which apps_pkg_compile_message words. Each file is compiled as a
 * top-level chunk, as the install compiles it, and not as a require does, with a few C levels less to spend: with
 * LUAI_MAXCCALLS 40 a file nested 37 deep passes here and 38 is "C stack overflow", and a require takes 34.
 */
static bool compile_package(const apps_pkg_t *pkg, char *raw, size_t raw_len) {
    apps_vm_config_t cfg = {
        .mem_limit = (size_t)pkg->manifest.memory_kb * 1024u,
        .realloc_fn = apps_port_realloc,
        .free_fn = apps_port_free,
        .now_ms = apps_port_mono_ms,
    };
    apps_vm_t *vm = apps_vm_new(&cfg);
    if (vm == NULL) {
        snprintf(raw, raw_len, "out of memory: can't make the app's heap of %u KB", (unsigned)pkg->manifest.memory_kb);
        return false;
    }
    bool ok = true;
    for (int pass = 0; pass < 2; pass++) {  /* pass 0: main; pass 1: the rest */
        for (int i = 0; i < pkg->count && ok; i++) {
            const apps_blob_t *b = &pkg->files[i];
            if ((strcmp(b->path, pkg->manifest.main) == 0) != (pass == 0) || apps_file_kind(b->path) != APPS_FILE_LUA) {
                continue;
            }
            ok = apps_vm_load(vm, b->path, (const char *)b->data, b->len, raw, raw_len);
            if (ok) {
                lua_pop(apps_vm_lua(vm), 1);
            }
        }
    }
    apps_vm_free(vm);
    return ok;
}

/* ---- The app, and the commands that drive it ---- */

typedef struct {
    const apps_pkg_t *pkg;
    apps_rt_t *rt;                    /* NULL when the runtime couldn't be made */
    char crash_text[APPS_ERROR_MAX];  /* then, the reason: the crash record of an app that never ran */
} app_t;

static bool crashed(const app_t *app) {
    return app->rt == NULL || apps_rt_state(app->rt) == APPS_STATE_CRASHED;
}

/* A call that may have crashed the app: if it did just now, the crash event (a crash is reported once). */
static void report_crash(const app_t *app, bool was_crashed) {
    if (!was_crashed && crashed(app)) {
        emit_text("crash", apps_rt_last_error(app->rt));
    }
}

static void cmd_start(app_t *app) {
    if (app->rt == NULL) {
        emit_text("error", app->crash_text);
        return;
    }
    bool was_crashed = crashed(app);
    if (apps_rt_start(app->rt)) {
        begin("started");
        field("id", app->pkg->manifest.id, strlen(app->pkg->manifest.id));
        field("name", app->pkg->manifest.name, strlen(app->pkg->manifest.name));
        end_event();
    } else if (was_crashed) {
        emit_text("error", apps_rt_last_error(app->rt));
    } else {
        report_crash(app, was_crashed);
    }
}

static void cmd_message(app_t *app, const char *event, const char *data) {
    if (app->rt == NULL) {
        emit_text("error", app->crash_text);
        return;
    }
    char err[APPS_ERROR_MAX];
    bool was_crashed = crashed(app);
    char *reply = apps_rt_message(app->rt, event, data, strlen(data), err, sizeof err);
    if (reply != NULL) {
        begin("reply");
        fputs(",\"data\":", stdout);
        fputs(reply, stdout);  /* JSON text the encoder wrote */
        end_event();
        apps_port_free(reply);
    } else if (!was_crashed && crashed(app)) {
        report_crash(app, was_crashed);
    } else {
        emit_text("error", err);
    }
}

static void cmd_state(const app_t *app) {
    begin("state");
    const char *name = app->rt != NULL ? apps_state_name(apps_rt_state(app->rt)) : "crashed";
    field("state", name, strlen(name));
    end_event();
}

static void cmd_log(const app_t *app, int lines) {
    if (app->rt == NULL) {
        return;
    }
    char tail[APPS_LOG_RING_BYTES + 1];  /* holds the whole ring */
    if (apps_rt_log_tail(app->rt, lines, tail, sizeof tail) == 0) {
        return;
    }
    for (char *p = tail;;) {
        char *nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        emit_log(app->pkg->manifest.id, p);
        if (nl == NULL) {
            return;
        }
        p = nl + 1;
    }
}

/* What the LVGL task's apply step does between two callbacks, as far as a host can: every request is taken as
 * applied, and what was waiting to be deleted is gone, so its slot is free. */
static void apply_screen(const app_t *app) {
    apps_ui_t *ui = app->rt != NULL ? apps_rt_ui(app->rt) : NULL;
    if (ui == NULL) {
        return;
    }
    apps_port_ui_lock();
    (void)apps_ui_take_clear(ui);
    (void)apps_ui_take_bg(ui, NULL);
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        apps_ui_slot(ui, i)->dirty = 0;
    }
    apps_port_ui_unlock();
}

/* s as the digits of a number, up to max, at *s, which moves past them. False when there is no digit. */
static bool take_number(const char **s, long max, long *out) {
    if (**s < '0' || **s > '9') {
        return false;
    }
    long v = 0;
    for (; **s >= '0' && **s <= '9'; (*s)++) {
        v = v * 10 + (**s - '0');
        if (v > max) {
            v = max + 1;  /* too big, and it can't overflow */
        }
    }
    *out = v;
    return true;
}

/* The slot of the first button or label whose text is `text`, or -1. */
static int find_by_text(apps_ui_t *ui, const char *text) {
    int found = -1;
    apps_port_ui_lock();
    for (int i = 0; i < APPS_UI_SLOTS && found < 0; i++) {
        const apps_widget_t *w = apps_ui_slot(ui, i);
        if (w->alive && (w->kind == APPS_W_BUTTON || w->kind == APPS_W_LABEL) &&
            strcmp(w->text != NULL ? w->text : "", text) == 0) {
            found = i;
        }
    }
    apps_port_ui_unlock();
    return found;
}

/* tap #SLOT[@GEN] or tap "TEXT": see the commands, above. */
static void cmd_tap(app_t *app, const char *args) {
    if (app->rt == NULL) {
        return;
    }
    apps_ui_t *ui = apps_rt_ui(app->rt);
    long slot = -1, gen = -1;
    if (args[0] == '#') {
        const char *p = args + 1;
        bool ok = take_number(&p, APPS_UI_SLOTS, &slot);
        if (ok && *p == '@') {
            p++;
            ok = take_number(&p, 65535, &gen) && gen <= 65535;
        }
        if (!ok || *p != '\0' || slot >= APPS_UI_SLOTS) {
            emit_text("error", "tap needs a slot of 0 to 127 and, after an @, a gen of 0 to 65535: tap #3 or tap #3@1");
            return;
        }
        if (gen < 0) {  /* the gen the slot has now: a tap on what is there */
            apps_port_ui_lock();
            const apps_widget_t *w = apps_ui_slot(ui, (int)slot);
            bool alive = w->alive;
            gen = w->gen;
            apps_port_ui_unlock();
            if (!alive) {
                char text[64];
                snprintf(text, sizeof text, "no widget in slot %ld", slot);
                emit_text("error", text);
                return;
            }
        }
    } else if (args[0] == '"') {
        apps_jr_t reader;
        char text[APPS_UI_TEXT_MAX + 1];
        apps_jr_init(&reader, args, strlen(args));
        if (!apps_jr_string(&reader, text, sizeof text) || !apps_jr_end(&reader)) {
            char message[160];
            snprintf(message, sizeof message, "tap needs the text as a JSON string: %s", reader.err);
            emit_text("error", message);
            return;
        }
        slot = find_by_text(ui, text);
        if (slot < 0) {
            char message[APPS_UI_TEXT_MAX + 48];
            snprintf(message, sizeof message, "no button or label has the text \"%s\"", text);
            emit_text("error", message);
            return;
        }
        apps_port_ui_lock();
        gen = apps_ui_slot(ui, (int)slot)->gen;
        apps_port_ui_unlock();
    } else {
        emit_text("error", "tap needs #<slot>[@<gen>] or a text in quotes: tap #3 or tap \"Go\"");
        return;
    }
    bool was_crashed = crashed(app);
    apps_rt_touch(app->rt, (uint16_t)slot, (uint16_t)gen, APPS_UI_TAP);
    report_crash(app, was_crashed);
}

/* dump [N]: the widgets, as apps_ui_dump writes them, in at most `cap` bytes. */
static void cmd_dump(const app_t *app, size_t cap) {
    char *out = malloc(cap);
    if (out == NULL) {
        emit_text("error", "dump: out of memory");
        return;
    }
    apps_ui_t *ui = app->rt != NULL ? apps_rt_ui(app->rt) : NULL;
    int dropped = 0;
    size_t len = ui != NULL ? apps_ui_dump(ui, out, cap, &dropped) : 0;
    begin("dump");
    fputs(",\"widgets\":", stdout);
    fputs(len > 0 ? out : "[]", stdout);  /* JSON text the dump wrote */
    if (dropped > 0) {
        printf(",\"dropped\":%d", dropped);
    }
    end_event();
    free(out);
}

/* stop, show or hide: nothing is printed unless the handler crashed the app. */
static void cmd_handler(app_t *app, const char *name) {
    if (app->rt == NULL) {
        return;
    }
    bool was_crashed = crashed(app);
    if (strcmp(name, "stop") == 0) {
        apps_rt_stop(app->rt);
    } else {
        apps_rt_show(app->rt, strcmp(name, "show") == 0);
    }
    report_crash(app, was_crashed);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static char *skip_space(char *s) {
    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    return s;
}

/* The end of the word at s: the first space, or the NUL. */
static char *word_end(char *s) {
    while (*s != '\0' && !is_space(*s)) {
        s++;
    }
    return s;
}

/* s as a whole number of digits, none but digits, a larger one as max; false for anything else. */
static bool parse_count(const char *s, long max, long *out) {
    if (*s == '\0') {
        return false;
    }
    long v = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10 + (*s - '0');
        if (v > max) {
            v = max;
        }
    }
    *out = v;
    return true;
}

/* One script line, which this edits in place. */
static void run_line(app_t *app, char *line) {
    char *end = line + strlen(line);
    while (end > line && is_space(end[-1])) {
        *--end = '\0';
    }
    char *cmd = skip_space(line);
    if (*cmd == '\0' || *cmd == '#') {
        return;
    }
    char *args = word_end(cmd);
    if (*args != '\0') {
        *args++ = '\0';
        args = skip_space(args);
    }
    char text[160];
    if (strcmp(cmd, "message") == 0) {
        char *data = word_end(args);  /* args is the event, a space and the data */
        if (*data != '\0') {
            *data++ = '\0';
            data = skip_space(data);
        }
        if (*args == '\0') {
            emit_text("error", "message needs an event name: message <event> [json]");
        } else {
            cmd_message(app, args, data);
        }
    } else if (strcmp(cmd, "log") == 0) {
        long lines = INT_MAX;
        if (*args != '\0' && !parse_count(args, INT_MAX, &lines)) {
            emit_text("error", "log needs a number of lines: log [n]");
        } else {
            cmd_log(app, (int)lines);
        }
    } else if (strcmp(cmd, "tap") == 0) {
        cmd_tap(app, args);
    } else if (strcmp(cmd, "dump") == 0) {
        long cap = DUMP_DEFAULT_BYTES;
        if (*args != '\0' && (!parse_count(args, DUMP_MAX_BYTES, &cap) || cap < 1)) {
            emit_text("error", "dump needs a number of bytes: dump [n]");
        } else {
            cmd_dump(app, (size_t)cap);
        }
    } else if (strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0 || strcmp(cmd, "show") == 0 ||
               strcmp(cmd, "hide") == 0 || strcmp(cmd, "state") == 0) {
        if (*args != '\0') {
            snprintf(text, sizeof text, "%s takes no arguments", cmd);
            emit_text("error", text);
        } else if (strcmp(cmd, "start") == 0) {
            cmd_start(app);
        } else if (strcmp(cmd, "state") == 0) {
            cmd_state(app);
        } else {
            cmd_handler(app, cmd);
        }
    } else {
        snprintf(text, sizeof text,
                 "unknown command '%.60s' (commands: message, stop, start, show, hide, state, log, tap, dump)", cmd);
        emit_text("error", text);
    }
}

/* What the UI lock's count was when the command began. */
static unsigned long takes_before_command;

static void begin_command(void) {
    takes_before_command = apphost_ui_lock_takes();
}

/* Whether any widget has a change waiting for the apply step. */
static bool screen_changed(const app_t *app) {
    apps_ui_t *ui = app->rt != NULL ? apps_rt_ui(app->rt) : NULL;
    for (int i = 0; ui != NULL && i < APPS_UI_SLOTS; i++) {
        if (apps_ui_slot(ui, i)->dirty != 0) {
            return true;
        }
    }
    return false;
}

/* After a command, the way a frame follows a callback: the screen is applied, and the UI lock is checked: no
 * command leaves it held, and none changes the screen without taking it (every change is made under it). */
static void end_of_command(const app_t *app) {
    if (apphost_ui_locked()) {
        fputs("apphost: a command left the UI lock held\n", stderr);
        abort();
    }
    if (screen_changed(app) && apphost_ui_lock_takes() == takes_before_command) {
        fputs("apphost: a command changed the screen without taking the UI lock\n", stderr);
        abort();
    }
    apply_screen(app);
}

/* Runs the lines of in; false if reading failed. */
static bool run_script(app_t *app, FILE *in) {
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, in) != -1) {
        begin_command();
        run_line(app, line);
        end_of_command(app);
    }
    free(line);
    return !ferror(in);
}

/* ---- Launch ---- */

/* The app's launch, as events, then the script; the exit status. A package that doesn't compile (unless compile is
 * false) is refused first, with one crash event and the status 1, and nothing else happens. A script that can't be read
 * is the status 2, after what it did run. */
static int run_app(const apps_pkg_t *pkg, FILE *script, const char *script_name, bool compile) {
    char raw[512];  /* what the app task gives Lua's message in its compile: the install's words are made from the same */
    if (compile && !compile_package(pkg, raw, sizeof raw)) {
        char message[APPS_ERROR_MAX];
        apps_pkg_compile_message(pkg, raw, message, sizeof message);
        emit_text("crash", message);
        return 1;
    }
    app_t app = {.pkg = pkg};
    apphost_set_log(emit_log);
    emit_warnings(pkg->manifest.warnings);
    begin_command();
    app.rt = apps_rt_new(pkg, app.crash_text, sizeof app.crash_text);
    if (app.rt == NULL) {  /* nothing could be made (main.lua doesn't compile, ...): the reason */
        emit_text("crash", app.crash_text);
    } else if (apps_rt_state(app.rt) == APPS_STATE_CRASHED) {  /* main.lua ran and failed: its record */
        emit_text("crash", apps_rt_last_error(app.rt));
    } else {
        emit_warnings(apps_rt_warnings(app.rt));
        cmd_start(&app);
    }
    end_of_command(&app);
    int status = 0;
    if (script != NULL && !run_script(&app, script)) {
        fprintf(stderr, "apphost: can't read script '%s': %s\n", script_name, strerror(errno));
        status = 2;
    }
    if (status == 0) {
        status = crashed(&app) ? 1 : 0;
    }
    apps_rt_free(app.rt);
    return status;
}

/* N rounds of make, start, stop and free: what is left on the apps' heap is a leak. The first round that
 * fails is reported, once, and the rounds go on (a failed round has to free everything too). */
static int run_cycles(const apps_pkg_t *pkg, int cycles) {
    bool failed = false;
    for (int i = 0; i < cycles; i++) {
        char err[APPS_ERROR_MAX];
        apps_rt_t *rt = apps_rt_new(pkg, err, sizeof err);
        if (rt != NULL) {
            if (apps_rt_start(rt)) {
                apps_rt_stop(rt);
            }
            if (apps_rt_state(rt) == APPS_STATE_CRASHED) {
                snprintf(err, sizeof err, "%s", apps_rt_last_error(rt));
                apps_rt_free(rt);
                rt = NULL;
            }
        }
        if (rt == NULL) {
            if (!failed) {
                emit_text("crash", err);
            }
            failed = true;
        } else {
            apps_rt_free(rt);
        }
    }
    printf("{\"event\":\"leak_check\",\"bytes\":%zu}\n", apphost_heap_held());
    fflush(stdout);
    return failed ? 1 : 0;
}

/* ---- The command line ---- */

typedef struct {
    const char *folder, *script, *sha256;
    long mem_kb, cycles;
    bool no_compile;
} options_t;

static bool usage(const char *why, const char *arg) {
    fprintf(stderr, "apphost: %s%s%s\n%s", why, arg != NULL ? " " : "", arg != NULL ? arg : "", USAGE);
    return false;
}

/* argv as options; false after printing why not. */
static bool parse_args(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof *o);
    if (argc >= 2 && strcmp(argv[1], "--sha256") == 0) {
        if (argc != 3) {
            return usage("--sha256 takes one argument: the text, or - for stdin", NULL);
        }
        o->sha256 = argv[2];
        return true;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--no-compile") == 0) {
            o->no_compile = true;
        } else if (strcmp(a, "--mem-kb") == 0 || strcmp(a, "--cycles") == 0 || strcmp(a, "--script") == 0) {
            if (value == NULL) {
                return usage("needs a value:", a);
            }
            i++;
            if (strcmp(a, "--script") == 0) {
                o->script = value;
                continue;
            }
            bool is_mem = strcmp(a, "--mem-kb") == 0;
            long max = is_mem ? MEM_KB_MAX : CYCLES_MAX, n;
            if (!parse_count(value, max + 1, &n) || n < 1 || n > max) {
                return usage(is_mem ? "--mem-kb takes a whole number from 1 to 65535, not" :
                                      "--cycles takes a whole number from 1 to 1000000, not", value);
            }
            *(is_mem ? &o->mem_kb : &o->cycles) = n;
        } else if (a[0] == '-' && a[1] != '\0') {
            return usage("unknown option", a);
        } else if (o->folder != NULL) {
            return usage("more than one app folder:", a);
        } else {
            o->folder = a;
        }
    }
    if (o->folder == NULL) {
        return usage("no app folder", NULL);
    }
    if (o->cycles > 0 && o->script != NULL) {
        return usage("--cycles and --script can't be used together", NULL);
    }
    return true;
}

/* The last part of folder's path, as the app's id must equal it: of the resolved path when there is one (so
 * "." and "app/" work), else of the path as given. */
static const char *folder_name(const char *folder, char *buf, size_t cap) {
    char resolved[PATH_MAX];
    snprintf(buf, cap, "%s", realpath(folder, resolved) != NULL ? resolved : folder);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') {
        buf[--n] = '\0';
    }
    const char *slash = strrchr(buf, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : buf;
}

static int sha256_command(const char *arg) {
    uint8_t digest[32];
    if (strcmp(arg, "-") == 0) {
        uint8_t *data = NULL;
        size_t len = 0, cap = 0;
        for (;;) {
            if (len == cap) {
                cap = cap != 0 ? cap * 2 : 65536;
                uint8_t *bigger = realloc(data, cap);
                if (bigger == NULL) {
                    free(data);
                    fprintf(stderr, "apphost: out of memory reading stdin\n");
                    return 2;
                }
                data = bigger;
            }
            size_t got = fread(data + len, 1, cap - len, stdin);
            len += got;
            if (got == 0) {
                break;
            }
        }
        if (ferror(stdin)) {
            free(data);
            fprintf(stderr, "apphost: can't read stdin: %s\n", strerror(errno));
            return 2;
        }
        apps_port_sha256(data, len, digest);
        free(data);
    } else {
        apps_port_sha256(arg, strlen(arg), digest);
    }
    for (int i = 0; i < 32; i++) {
        printf("%02x", digest[i]);
    }
    putchar('\n');
    return 0;
}

int main(int argc, char **argv) {
    options_t o;
    if (!parse_args(argc, argv, &o)) {
        return 2;
    }
    if (o.sha256 != NULL) {
        return sha256_command(o.sha256);
    }
    FILE *script = NULL;
    if (o.script != NULL) {
        script = strcmp(o.script, "-") == 0 ? stdin : fopen(o.script, "r");
        if (script == NULL) {
            fprintf(stderr, "apphost: can't read script '%s': %s\n", o.script, strerror(errno));
            return 2;
        }
    }
    apps_pkg_t *pkg = calloc(1, sizeof *pkg);  /* about 7 KB: not for the stack */
    if (pkg == NULL) {
        fprintf(stderr, "apphost: out of memory\n");
        return 2;
    }
    char name_buf[PATH_MAX + 1], err[APPS_ERROR_MAX];
    int status = 1;
    if (load_package(o.folder, folder_name(o.folder, name_buf, sizeof name_buf), pkg, err, sizeof err)) {
        if (o.mem_kb > 0) {
            pkg->manifest.memory_kb = (uint16_t)o.mem_kb;
        }
        status = o.cycles > 0 ? run_cycles(pkg, (int)o.cycles) : run_app(pkg, script, o.script, !o.no_compile);
        apps_pkg_free(pkg, NULL);
    } else {
        emit_text("error", err);
    }
    free(pkg);
    if (script != NULL && script != stdin) {
        fclose(script);
    }
    return status;
}
