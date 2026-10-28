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
 * The install pipeline of Muse apps (apps_install.c) and its RAM store, run for real on the host with the real cJSON,
 * the package rules (apps_pkg.c, apps_json.c) and apps_vm_excerpt (apps_vm.c and Lua). Usage:
 * muse_apps_install_harness <case>, one case per test in tests/test_muse_apps_install.py; it exits 0 when the case
 * holds, and otherwise prints why.
 *
 * The source is a fake that serves files from a table and records every read (its path, its limit, its deadline); the
 * ops are fakes that record what they were given and answer what the case set (compile returns only Lua's raw message,
 * as C17 has it; run fills the status record from the case's and frees the package, as apps_task_run does). Every
 * result is printed as "@result <json>" for the Python test, which checks each is valid JSON within 6144 bytes. The port
 * is a counting allocator that can refuse, cJSON's allocations are counted too (and can be refused), and each case
 * ends with nothing held.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apps_install.h"
#include "apps_port.h"
#include "cJSON.h"
#include "muse_apps_exact_stack.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define KB 1024u

/* ---- Failing ---- */

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void fail_at(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fflush(stdout);
    fprintf(stderr, "FAIL line %d: ", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    _exit(1);
}

#define CHECK(cond, ...)                    \
    do {                                    \
        if (!(cond)) {                      \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                   \
    } while (0)

/* ---- The port ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t port_held;
static long port_refuse = -1; /* refuse the growth request this many from now (0: the next); -1: never */
static long port_growths;     /* growth requests since the count was zeroed */

void *apps_port_realloc(void *ptr, size_t n) {
    CHECK(n > 0, "apps_port_realloc(%p, 0)", ptr);
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old) {
        port_growths++;
        if (port_refuse >= 0 && port_refuse-- == 0) {
            return NULL;
        }
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    port_held = port_held - old + n;
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        port_held -= b->size;
        free(b);
    }
}

static int64_t fake_ms = 5000;

int64_t apps_port_mono_ms(void) {
    return fake_ms;
}

bool apps_port_epoch_ms(int64_t *out) {
    (void)out;
    return false;
}

void apps_port_log(const char *app_id, const char *line) {
    (void)app_id;
    (void)line;
}

void apps_port_init(void) {}

void apps_port_ui_lock(void) {}

bool apps_port_ui_trylock(int ms) {
    (void)ms;
    return true;
}

void apps_port_ui_unlock(void) {}

/* apps_port_sha256 is tools/muse/apphost/sha256.c's. */

static size_t cjson_held;
static long cjson_refuse = -1;
static long cjson_allocs;

static void *cjson_malloc(size_t n) {
    cjson_allocs++;
    if (cjson_refuse >= 0 && cjson_refuse-- == 0) {
        return NULL;
    }
    block_t *b = malloc(sizeof *b + n);
    if (b == NULL) {
        return NULL;
    }
    b->size = n;
    cjson_held += n;
    return b + 1;
}

static void cjson_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        cjson_held -= b->size;
        free(b);
    }
}

/* ---- The source ---- */

static struct {
    const char *path;
    uint8_t *data;
    size_t len;
} files[40];
static int n_files;
static char reads[4096]; /* the paths read, in order, each followed by ' ' */
static size_t read_max[64];
static int64_t read_deadline[64];
static int n_reads;
static const char *fail_path, *fail_why;  /* a read of fail_path fails with fail_why */
static int read_step_ms;                  /* each read moves the clock this far */

static void serve(const char *path, const void *data, size_t len) {
    CHECK(n_files < (int)ARRAY_LEN(files), "too many files");
    files[n_files].path = path;
    files[n_files].data = malloc(len + 1);
    CHECK(files[n_files].data != NULL, "no memory");
    memcpy(files[n_files].data, data, len);
    files[n_files].len = len;
    n_files++;
}

static void serve_text(const char *path, const char *text) {
    serve(path, text, strlen(text));
}

static void unserve(void) {
    for (int i = 0; i < n_files; i++) {
        free(files[i].data);
    }
    n_files = 0;
    reads[0] = '\0';
    n_reads = 0;
    fail_path = fail_why = NULL;
}

static bool fake_read(void *ctx, const char *rel, size_t max, int64_t deadline_us, uint8_t **data, size_t *len,
                      char *err, size_t err_len) {
    CHECK(ctx == (void *)&files, "the source's ctx");
    size_t used = strlen(reads);
    snprintf(reads + used, sizeof reads - used, "%s ", rel);
    if (n_reads < (int)ARRAY_LEN(read_max)) {
        read_max[n_reads] = max;
        read_deadline[n_reads] = deadline_us;
    }
    n_reads++;
    fake_ms += read_step_ms;
    if (fail_path != NULL && strcmp(rel, fail_path) == 0) {
        snprintf(err, err_len, "reading %s: %s", rel, fail_why);
        return false;
    }
    for (int i = 0; i < n_files; i++) {
        if (strcmp(files[i].path, rel) == 0) {
            *data = apps_port_realloc(NULL, files[i].len > 0 ? files[i].len : 1);
            if (*data == NULL) {
                snprintf(err, err_len, "reading %s: out of memory", rel);
                return false;
            }
            memcpy(*data, files[i].data, files[i].len);
            *len = files[i].len;
            return true;
        }
    }
    snprintf(err, err_len, "reading %s: No such file or directory", rel);
    return false;
}

static const apps_source_t SOURCE = {fake_read, &files, "usb"};

/* ---- The ops ---- */

static int compiles, commits, runs, run_settle;
static const char *compile_raw;          /* NULL: it compiles; else Lua's raw message */
static char compiled_paths[1024];
static bool commit_ok = true, commit_unchanged;
static const char *commit_err = "";
static bool run_ok = true;
static const char *run_err = "";
static apps_status_t run_record;         /* what run fills st with */
static char run_main[APPS_PATH_MAX + 1]; /* the package's main, as run saw it */

static bool fake_compile(const apps_pkg_t *pkg, char *err, size_t n) {
    compiles++;
    compiled_paths[0] = '\0';
    for (int i = 0; i < pkg->count; i++) {
        size_t used = strlen(compiled_paths);
        snprintf(compiled_paths + used, sizeof compiled_paths - used, "%s ", pkg->files[i].path);
    }
    snprintf(err, n, "%s", compile_raw != NULL ? compile_raw : "");
    return compile_raw == NULL;
}

static bool fake_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n) {
    commits++;
    CHECK(pkg != NULL && pkg->count > 0, "commit of a package");
    *unchanged = commit_unchanged;
    snprintf(err, n, "%s", commit_ok ? "" : commit_err);
    return commit_ok;
}

static bool fake_run(apps_pkg_t *pkg, int settle_ms, apps_status_t *st, char *err, size_t n) {
    runs++;
    run_settle = settle_ms;
    snprintf(run_main, sizeof run_main, "%s", pkg->manifest.main);
    memcpy(st, &run_record, sizeof *st);
    apps_pkg_free(pkg, apps_port_free);
    apps_port_free(pkg);
    snprintf(err, n, "%s", run_ok ? "" : run_err);
    return run_ok;
}

static const char *fake_storage(void) {
    return "ram";
}

static const apps_install_ops_t OPS = {fake_compile, fake_commit, fake_run, fake_storage};

/* ---- Results ---- */

static char *last; /* the last result, printed */

/* Prints r as "@result <json>", keeps the text in `last` and frees r. */
static const char *show(cJSON *r) {
    free(last);
    last = NULL;
    CHECK(r != NULL, "no result");
    char *text = cJSON_PrintUnformatted(r);
    CHECK(text != NULL, "no memory to print the result");
    printf("@result %s\n", text);
    fflush(stdout);
    last = strdup(text);
    cJSON_free(text);
    cJSON_Delete(r);
    return last;
}

static const char *install(const char *id, bool run) {
    return show(apps_install_run(id, run, &SOURCE, &OPS));
}

/* The result failed with code and exactly message. */
static void expect_failure_at(int line, const char *got, const char *code, const char *message) {
    cJSON *r = cJSON_Parse(got);
    cJSON *error = cJSON_GetObjectItemCaseSensitive(r, "error");
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    bool ok = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "ok")) && c != NULL && m != NULL &&
              strcmp(c, code) == 0 && strcmp(m, message) == 0;
    if (!ok) {
        fail_at(line, "%s\ninstead of code %s, message\n%s", got, code, message);
    }
    cJSON_Delete(r);
}
#define EXPECT_FAILURE(got, code, message) expect_failure_at(__LINE__, (got), (code), (message))

/* The message of a failed result (the caller frees it). */
static char *message_of(const char *got) {
    cJSON *r = cJSON_Parse(got);
    const char *m = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "error"), "message"));
    CHECK(m != NULL, "no message in %s", got);
    char *copy = strdup(m);
    cJSON_Delete(r);
    return copy;
}

static void reset_ops(void) {
    compiles = commits = runs = run_settle = 0;
    compile_raw = NULL;
    commit_ok = true;
    commit_unchanged = false;
    commit_err = "";
    run_ok = true;
    run_err = "";
    memset(&run_record, 0, sizeof run_record);
    run_record.state = APPS_STATE_RUNNING;
    snprintf(run_record.log_tail, sizeof run_record.log_tail, "started");
    snprintf(run_record.ui_dump, sizeof run_record.ui_dump, "[]");
    run_record.mem_kb = 20;
    run_record.peak_kb = 24;
}

/* ---- Packages ---- */

static const char MAIN[] = "local app = {}\nfunction app.start() print('started') end\nreturn app\n";

static void serve_hello(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.2.3\",\"api\":1}");
    serve_text("main.lua", MAIN);
}

/* A valid LVGL v9 image of w x h: RGB565 (cf 0x12), or RGB565A8 (0x14) with its alpha plane. */
static uint8_t *make_image(unsigned w, unsigned h, bool alpha, size_t *len) {
    unsigned stride = w * 2;
    *len = 12 + (size_t)stride * h + (alpha ? (size_t)w * h : 0);
    uint8_t *img = calloc(1, *len);
    CHECK(img != NULL, "no memory");
    img[0] = 0x19;
    img[1] = alpha ? 0x14 : 0x12;
    img[4] = (uint8_t)w;
    img[5] = (uint8_t)(w >> 8);
    img[6] = (uint8_t)h;
    img[7] = (uint8_t)(h >> 8);
    img[8] = (uint8_t)stride;
    img[9] = (uint8_t)(stride >> 8);
    return img;
}

static void serve_image(const char *path, unsigned w, unsigned h, bool alpha) {
    size_t len;
    uint8_t *img = make_image(w, h, alpha, &len);
    serve(path, img, len);
    free(img);
}

/* The record of a crash: the error and its traceback, and a log of n lines "l01".."lnn". */
static void crash_record(const char *error, int n) {
    run_record.state = APPS_STATE_CRASHED;
    snprintf(run_record.last_error, sizeof run_record.last_error, "%s", error);
    size_t at = 0;
    run_record.log_tail[0] = '\0';
    for (int i = 1; i <= n; i++) {
        at += (size_t)snprintf(run_record.log_tail + at, sizeof run_record.log_tail - at, "%sl%02d", i > 1 ? "\n" : "", i);
    }
}

/* A record as full as it gets: 40 lines of 200 bytes and a dump of 128 widgets. */
static void full_record(void) {
    size_t at = 0;
    for (int i = 0; i < 40; i++) {
        at += (size_t)snprintf(run_record.log_tail + at, sizeof run_record.log_tail - at, "%sline %02d ", i > 0 ? "\n" : "", i);
        memset(run_record.log_tail + at, 'y', 192);
        at += 192;
        run_record.log_tail[at] = '\0';
    }
    at = (size_t)snprintf(run_record.ui_dump, sizeof run_record.ui_dump, "[");
    for (int i = 0; i < 128; i++) {
        at += (size_t)snprintf(run_record.ui_dump + at, sizeof run_record.ui_dump - at,
                               "%s{\"id\":\"label#%d\",\"kind\":\"label\",\"text\":\"Row %d\",\"x\":%d,\"y\":%d,\"w\":null,"
                               "\"h\":null,\"hidden\":false,\"overflow\":0}",
                               i > 0 ? "," : "", i, i, i, 2 * i);
        CHECK(at < sizeof run_record.ui_dump - 2, "the dump fits its field");
    }
    snprintf(run_record.ui_dump + at, sizeof run_record.ui_dump - at, "]");
}

/* ---- The end of every case ---- */

static void finish(void) {
    free(last);
    last = NULL;
    unserve();
    apps_ram_entry_t e[APPS_RAM_APPS];
    int n = apps_ram_list(e, APPS_RAM_APPS);
    for (int i = 0; i < n; i++) {
        CHECK(apps_ram_remove(e[i].id), "remove %s", e[i].id);
    }
    CHECK(port_held == 0, "%zu bytes of the port's heap still held", port_held);
    CHECK(cjson_held == 0, "%zu bytes of cJSON's still held", cjson_held);
}

/* ---- The cases ---- */

static void manifest_errors_propagate(void) {
    EXPECT_FAILURE(install("Hello", true), APPS_ERR_INVALID_PARAMS, "app id 'Hello' must match ^[a-z][a-z0-9_]{1,23}$");
    EXPECT_FAILURE(install(NULL, true), APPS_ERR_INVALID_PARAMS, "app id '' must match ^[a-z][a-z0-9_]{1,23}$");
    CHECK(n_reads == 0, "nothing read for a bad id");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_READ_FAILED, "reading manifest.json: No such file or directory");
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"permissions\":[\"net\"]}");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "manifest: permission 'net' is not available in this firmware yet (api 1)");
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":2}");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "manifest: this gadget supports api 1 (manifest says api 2)");
    unserve();
    serve_text("manifest.json", "{\"id\":\"other\",\"name\":\"Hello\",\"api\":1}");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "manifest: id 'other' must match ^[a-z][a-z0-9_]{1,23}$ and the folder name 'hello'");
    unserve();
    char *big = malloc(8 * KB + 2);
    CHECK(big != NULL, "no memory");
    memset(big, ' ', 8 * KB + 1);
    big[0] = '{';
    big[8 * KB] = '}';
    serve("manifest.json", big, 8 * KB + 1);
    free(big);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE, "manifest.json is larger than 8 KB");
    CHECK(read_max[0] == APPS_MANIFEST_MAX, "manifest.json's limit: %zu", read_max[0]);
    CHECK(compiles == 0 && commits == 0 && runs == 0, "nothing compiled, kept or run");
}

static void path_checked_before_read(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"ok.lua\",\"../x.lua\"]}");
    serve_text("main.lua", MAIN);
    serve_text("ok.lua", "return 1\n");
    serve_text("../x.lua", "return 2\n");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "manifest: files[1] '../x.lua' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 "
                   "characters)");
    CHECK(strcmp(reads, "manifest.json ") == 0, "only the manifest was read: '%s'", reads);
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"main\":\"/etc/main.lua\"}");
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "manifest: main '/etc/main.lua' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 "
                   "characters)");
    CHECK(strcmp(reads, "manifest.json ") == 0, "only the manifest was read: '%s'", reads);
}

static void compiled_chunk_refused(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1}");
    serve("main.lua", "\x1bLuaT\x00\x19\x93", 8);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "files[0] main.lua is a compiled chunk: install Lua source text");
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"data.json\",\"lib.lua\"]}");
    serve_text("main.lua", MAIN);
    serve_text("data.json", "[1]");
    serve("lib.lua", "\x1bLua", 4);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_INVALID_PACKAGE,
                   "files[2] lib.lua is a compiled chunk: install Lua source text");
    CHECK(compiles == 0, "nothing compiled");
    /* Three bytes of the signature aren't a chunk. */
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1}");
    serve("main.lua", "\x1bLu", 3);
    install("hello", false);
    CHECK(strncmp(last, "{\"ok\":true,", 11) == 0, "%s", last);
}

static void oversize_and_totals(void) {
    uint8_t *lua = malloc(256 * KB + 1);
    CHECK(lua != NULL, "no memory");
    memset(lua, '-', 256 * KB + 1);
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1}");
    serve("main.lua", lua, 256 * KB + 1);
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "main.lua is larger than 256 KB");
    CHECK(read_max[1] == APPS_LUA_MAX, "a .lua's limit: %zu", read_max[1]);
    /* Exactly at the limit is fine. */
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1}");
    serve("main.lua", lua, 256 * KB);
    install("hello", false);
    CHECK(strncmp(last, "{\"ok\":true,", 11) == 0, "256 KB of main.lua: %.200s", last);
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"data.json\"]}");
    serve_text("main.lua", MAIN);
    serve("data.json", lua, 32 * KB + 1);
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "data.json is larger than 32 KB");
    CHECK(read_max[2] == APPS_JSON_FILE_MAX, "a .json's limit: %zu", read_max[2]);
    unserve();
    uint8_t *big = calloc(1, 512 * KB + 1);
    CHECK(big != NULL, "no memory");
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"a.bin\"]}");
    serve_text("main.lua", MAIN);
    serve("a.bin", big, 512 * KB + 1);
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "a.bin is larger than 512 KB");
    CHECK(read_max[2] == APPS_IMAGE_MAX, "an image's limit: %zu", read_max[2]);
    free(big);
    /* Images: three of 524,262 bytes (466 x 375, RGB565A8) fit 1.5 MB with 78 bytes to spare; a fourth of 140 doesn't. */
    unserve();
    serve_text("manifest.json",
               "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"a.bin\",\"b.bin\",\"c.bin\",\"d.bin\"]}");
    serve_text("main.lua", MAIN);
    serve_image("a.bin", 466, 375, true);
    serve_image("b.bin", 466, 375, true);
    serve_image("c.bin", 466, 375, true);
    serve_image("d.bin", 8, 8, false);
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "images add up to more than 1.5 MB");
    /* The app: 2 MB with manifest.json. */
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"a.bin\",\"b.bin\",\"c.bin\","
                                "\"x.lua\",\"y.lua\"]}");
    serve_text("main.lua", MAIN);
    serve_image("a.bin", 466, 375, true);
    serve_image("b.bin", 466, 375, true);
    serve_image("c.bin", 466, 375, true);
    serve("x.lua", lua, 256 * KB);
    serve("y.lua", lua, 256 * KB);
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "the app is larger than 2 MB");
    free(lua);
    /* A .json that isn't JSON, an image that isn't one. */
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"data.json\"]}");
    serve_text("main.lua", MAIN);
    serve_text("data.json", "{\"a\":");
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE, "data.json: unexpected end of input at byte 6");
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"logo.bin\"]}");
    serve_text("main.lua", MAIN);
    serve_text("logo.bin", "GIF89a");
    EXPECT_FAILURE(install("hello", false), APPS_ERR_INVALID_PACKAGE,
                   "logo.bin: image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)");
    CHECK(compiles == 1, "only the 256 KB main.lua was compiled: %d", compiles);
}

static void compile_error_has_excerpt(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"lib/util.lua\"]}");
    serve_text("main.lua", "local app = {}\nlocal x = = 1\nreturn app\n");
    serve_text("lib/util.lua", "oops\n");
    compile_raw = "main.lua:2: unexpected symbol near '='";
    char *m = message_of(install("hello", true));
    CHECK(strncmp(m, "compile failed: main.lua:2:", 27) == 0 && strstr(m, ">2|") != NULL &&
              strstr(m, "local x = = 1") != NULL && strstr(m, " 1| local app = {}") != NULL,
          "%s", m);
    free(m);
    CHECK(strstr(last, "\"code\":\"compile_failed\"") != NULL, "%s", last);
    CHECK(commits == 0 && runs == 0, "nothing kept or run");
    /* A module's error shows that module's line. */
    compile_raw = "lib/util.lua:1: syntax error near <eof>";
    m = message_of(install("hello", true));
    CHECK(strncmp(m, "compile failed: lib/util.lua:1: syntax error near <eof>\n", 56) == 0 && strstr(m, ">1| oops") != NULL,
          "%s", m);
    free(m);
    /* A message that names none of the files is shown as it is. */
    compile_raw = "out of memory: can't make the app's heap of 256 KB";
    EXPECT_FAILURE(install("hello", true), APPS_ERR_COMPILE_FAILED,
                   "compile failed: out of memory: can't make the app's heap of 256 KB");
    CHECK(strcmp(compiled_paths, "main.lua lib/util.lua ") == 0, "compile was given %s", compiled_paths);
}

static void crash_in_start_message(void) {
    serve_hello();
    run_ok = false;
    run_err = "crashed in start(): main.lua:4: boom";
    crash_record("main.lua:4: boom\nstack traceback:\n\tmain.lua:4: in function 'start'", 12);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_CRASHED,
                   "installed but crashed in start(): main.lua:4: boom\nstack traceback:\n\tmain.lua:4: in function "
                   "'start'\nlog:\nl03\nl04\nl05\nl06\nl07\nl08\nl09\nl10\nl11\nl12");
    CHECK(commits == 1 && runs == 1 && run_settle == APPS_SETTLE_MS, "kept, then run with the settle: %d %d %d", commits,
          runs, run_settle);
    /* In the settle. */
    run_err = "crashed: main.lua:9: late";
    crash_record("main.lua:9: late\nstack traceback:\n\tmain.lua:9: in function <main.lua:8>", 2);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_CRASHED,
                   "installed but crashed: main.lua:9: late\nstack traceback:\n\tmain.lua:9: in function "
                   "<main.lua:8>\nlog:\nl01\nl02");
    /* Not a crash: the waits. */
    run_err = "the app task didn't answer within 9000 ms";
    run_record.state = APPS_STATE_RUNNING;
    EXPECT_FAILURE(install("hello", true), APPS_ERR_NOT_RUNNING,
                   "installed but it didn't run: the app task didn't answer within 9000 ms");
}

static void crash_in_top_level_message(void) {
    serve_hello();
    run_ok = false;
    run_err = "crashed loading main.lua: main.lua:2: top";
    crash_record("main.lua:2: top\nstack traceback:\n\tmain.lua:2: in main chunk", 1);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_CRASHED,
                   "installed but crashed while loading main.lua: main.lua:2: top\nstack traceback:\n\tmain.lua:2: in "
                   "main chunk\nlog:\nl01");
    /* <main> is the manifest's main. */
    unserve();
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"main\":\"app.lua\"}");
    serve_text("app.lua", MAIN);
    run_err = "crashed loading app.lua: app.lua:1: nope";
    crash_record("app.lua:1: nope", 0);
    EXPECT_FAILURE(install("hello", true), APPS_ERR_CRASHED, "installed but crashed while loading app.lua: app.lua:1: nope\nlog:\n");
    CHECK(strcmp(run_main, "app.lua") == 0, "run got main %s", run_main);
}

static void result_fits_6144_bytes(void) {
    /* C3's fake: 32 files, an 8 KB log and a 128-entry dump. */
    char manifest[2048];
    size_t at = (size_t)snprintf(manifest, sizeof manifest, "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[");
    static char paths[31][16];
    for (int i = 0; i < 31; i++) {
        snprintf(paths[i], sizeof paths[i], "mod%02d.lua", i);
        at += (size_t)snprintf(manifest + at, sizeof manifest - at, "%s\"%s\"", i > 0 ? "," : "", paths[i]);
    }
    snprintf(manifest + at, sizeof manifest - at, "]}");
    serve_text("manifest.json", manifest);
    serve_text("main.lua", MAIN);
    for (int i = 0; i < 31; i++) {
        serve_text(paths[i], "return {}\n");
    }
    full_record();
    /* The oldest log lines go first, every one of them before the dump's last entries. */
    const char *got = install("hello", true);
    CHECK(strlen(got) <= APPS_RESULT_MAX, "the result is %zu bytes", strlen(got));
    CHECK(strstr(got, "\"path\":\"mod30.lua\"") != NULL, "every file is listed");
    CHECK(strstr(got, "\"log\":[],\"ui\":[{\"id\":\"label#0\"") != NULL && strstr(got, "\"ui_truncated\":") != NULL,
          "the log went, then the dump's last entries: %s", got);
    /* A log and a small dump: only the oldest lines go, and the dump is whole. */
    snprintf(run_record.ui_dump, sizeof run_record.ui_dump,
             "[{\"id\":\"label#0\",\"kind\":\"label\",\"text\":\"Hi\",\"x\":1,\"y\":2,\"w\":3,\"h\":4,\"hidden\":false,"
             "\"overflow\":0}]");
    got = install("hello", true);
    CHECK(strlen(got) <= APPS_RESULT_MAX, "the result is %zu bytes", strlen(got));
    CHECK(strstr(got, "\"line 39 ") != NULL && strstr(got, "\"line 00 ") == NULL && strstr(got, "ui_truncated") == NULL,
          "the newest lines are kept: %s", got);
    /* With room, nothing is trimmed and nothing is said. */
    reset_ops();
    install("hello", true);
    CHECK(strstr(last, "\"log\":[\"started\"],\"ui\":[],\"mem_kb\":20,\"peak_kb\":24}}") != NULL, "%s", last);
    /* The dump's own drops count too. */
    run_record.ui_truncated = 3;
    install("hello", true);
    CHECK(strstr(last, "\"ui\":[],\"ui_truncated\":3,") != NULL, "%s", last);
    /* A long error is cut to fit, on a character's boundary. */
    run_ok = false;
    run_err = "crashed in start(): main.lua:1: x";
    run_record.state = APPS_STATE_CRASHED;
    for (size_t i = 0; i + 1 < sizeof run_record.last_error; i++) {
        run_record.last_error[i] = i % 3 == 0 ? '"' : i % 3 == 1 ? '\x01' : 'e';
    }
    run_record.last_error[sizeof run_record.last_error - 1] = '\0';
    full_record();
    got = install("hello", true);
    CHECK(strlen(got) <= APPS_RESULT_MAX && strstr(got, "installed but crashed in start(): \\\"\\u0001e") != NULL &&
              strstr(got, "\"}}") == got + strlen(got) - 3,
          "%zu bytes: %.300s", strlen(got), got);
}

static void busy(void) {
    CHECK(apps_install_try_begin(), "the first begins");
    CHECK(!apps_install_try_begin(), "a second doesn't");
    apps_install_end();
    CHECK(apps_install_try_begin(), "after the end, another begins");
    apps_install_end();
}

static void success_payload(void) {
    size_t img_len;
    uint8_t *img = make_image(40, 30, false, &img_len);
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.2.3\",\"api\":1,\"colour\":1,"
                                "\"files\":[\"data.json\",\"logo.bin\"],\"messages\":{\"start\":\"starts it\"}}");
    serve_text("main.lua", MAIN);
    serve_text("data.json", "{\"steps\":[1,2]}");
    serve("logo.bin", img, img_len);
    free(img);
    /* Without run: installed, the manifest's warnings, nothing of a run. */
    const char *got = install("hello", false);
    char want[200];
    snprintf(want, sizeof want, "{\"ok\":true,\"payload\":{\"id\":\"hello\",\"version\":\"1.2.3\",\"storage\":\"ram\","
             "\"files\":[{\"path\":\"main.lua\",\"bytes\":%zu,\"sha256\":\"", strlen(MAIN));
    CHECK(strncmp(got, want, strlen(want)) == 0, "%s", got);
    CHECK(strstr(got, "\"state\":\"installed\",\"warnings\":[\"unknown key 'colour' ignored\"],\"log\":[],\"ui\":[],"
                      "\"mem_kb\":0,\"peak_kb\":0}}") != NULL,
          "%s", got);
    CHECK(compiles == 1 && commits == 1 && runs == 0, "compiled and kept, not run");
    CHECK(strcmp(reads, "manifest.json main.lua data.json logo.bin ") == 0, "read in order: %s", reads);
    /* With run: the run's record. */
    snprintf(run_record.warnings, sizeof run_record.warnings,
             "unknown key 'colour' ignored; label#0 'Hi' extends 3 px past the round edge");
    snprintf(run_record.log_tail, sizeof run_record.log_tail, "one\n\ttwo \"2\"");
    snprintf(run_record.ui_dump, sizeof run_record.ui_dump,
             "[{\"id\":\"label#0\",\"kind\":\"label\",\"text\":\"Hi\",\"x\":1,\"y\":2,\"w\":3,\"h\":4,\"hidden\":false,"
             "\"overflow\":3}]");
    got = install("hello", true);
    CHECK(strstr(got, "\"state\":\"running\",\"warnings\":[\"unknown key 'colour' ignored\",\"label#0 'Hi' extends 3 px "
                      "past the round edge\"],\"log\":[\"one\",\"\\ttwo \\\"2\\\"\"],\"ui\":[{\"id\":\"label#0\",\"kind\":"
                      "\"label\",\"text\":\"Hi\",\"x\":1,\"y\":2,\"w\":3,\"h\":4,\"hidden\":false,\"overflow\":3}],"
                      "\"mem_kb\":20,\"peak_kb\":24}}") != NULL,
          "%s", got);
    CHECK(runs == 1 && run_settle == APPS_SETTLE_MS, "run with the settle");
    /* The store's word. */
    commit_unchanged = true;
    got = install("hello", false);
    CHECK(strstr(got, "\"peak_kb\":0,\"unchanged\":true}}") != NULL, "%s", got);
}

static void read_and_commit_failures(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"lib.lua\"]}");
    serve_text("main.lua", MAIN);
    serve_text("lib.lua", "return {}\n");
    fail_path = "lib.lua";
    fail_why = "the VM connection dropped";
    EXPECT_FAILURE(install("hello", true), APPS_ERR_READ_FAILED, "reading lib.lua: the VM connection dropped");
    CHECK(compiles == 0 && commits == 0, "nothing compiled or kept");
    fail_path = NULL;
    commit_ok = false;
    commit_err = "out of memory: no room to keep the app in RAM";
    EXPECT_FAILURE(install("hello", true), APPS_ERR_COMMIT_FAILED, "out of memory: no room to keep the app in RAM");
    CHECK(commits == 1 && runs == 0, "kept failed, nothing run");
}

static void deadline_is_one_absolute_time(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"api\":1,\"files\":[\"a.lua\",\"b.lua\"]}");
    serve_text("main.lua", MAIN);
    serve_text("a.lua", "return 1\n");
    serve_text("b.lua", "return 2\n");
    fake_ms = 7000;
    read_step_ms = 30000; /* each read takes 30 s of the clock */
    install("hello", false);
    read_step_ms = 0;
    CHECK(n_reads == 4, "four reads: %d", n_reads);
    for (int i = 0; i < 4; i++) {
        CHECK(read_deadline[i] == (int64_t)(7000 + APPS_INSTALL_DEADLINE_MS) * 1000, "read %d's deadline: %lld", i,
              (long long)read_deadline[i]);
    }
}

/* A package for the store, as an install builds it: every block from apps_port_realloc. */
static apps_pkg_t *make_pkg(const char *id, const char *name, const char *main_src) {
    apps_pkg_t *p = apps_port_realloc(NULL, sizeof *p);
    CHECK(p != NULL, "no memory");
    memset(p, 0, sizeof *p);
    snprintf(p->manifest.id, sizeof p->manifest.id, "%s", id);
    snprintf(p->manifest.name, sizeof p->manifest.name, "%s", name);
    snprintf(p->manifest.version, sizeof p->manifest.version, "0.%zu", strlen(name));
    snprintf(p->manifest.main, sizeof p->manifest.main, "main.lua");
    snprintf(p->manifest.files[0], sizeof p->manifest.files[0], "main.lua");
    p->manifest.file_count = 1;
    p->manifest.memory_kb = 256;
    p->manifest.messages_json = apps_port_realloc(NULL, 16);
    CHECK(p->manifest.messages_json != NULL, "no memory");
    snprintf(p->manifest.messages_json, 16, "{\"go\":\"goes\"}");
    snprintf(p->files[0].path, sizeof p->files[0].path, "main.lua");
    p->files[0].len = strlen(main_src);
    p->files[0].data = apps_port_realloc(NULL, p->files[0].len);
    CHECK(p->files[0].data != NULL, "no memory");
    memcpy(p->files[0].data, main_src, p->files[0].len);
    apps_port_sha256(p->files[0].data, p->files[0].len, p->files[0].sha256);
    p->count = 1;
    return p;
}

static void free_pkg(apps_pkg_t *p) {
    apps_pkg_free(p, apps_port_free);
    apps_port_free(p);
}

static void ram_store(void) {
    CHECK(!apps_ram_has("one") && !apps_ram_remove("one"), "an empty store");
    CHECK(apps_ram_put(make_pkg("one", "One", "return 1\n")), "put one");
    CHECK(apps_ram_put(make_pkg("two", "Two", "return 2\n")), "put two");
    CHECK(apps_ram_put(make_pkg("one", "Uno", "return 'uno'\n")), "one again replaces it");
    apps_ram_entry_t e[APPS_RAM_APPS];
    int n = apps_ram_list(e, APPS_RAM_APPS);
    CHECK(n == 2 && strcmp(e[0].id, "one") == 0 && strcmp(e[0].name, "Uno") == 0 && strcmp(e[1].id, "two") == 0 &&
              strcmp(e[1].version, "0.3") == 0,
          "the list: %d %s %s", n, n > 0 ? e[0].name : "", n > 1 ? e[1].id : "");
    CHECK(apps_ram_list(e, 1) == 1, "max is kept");
    /* A load is a deep copy: freeing it, or changing it, leaves the store's. */
    apps_pkg_t *copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("one", copy), "load one");
    CHECK(copy->count == 1 && copy->files[0].len == 13 && memcmp(copy->files[0].data, "return 'uno'\n", 13) == 0 &&
              strcmp(copy->manifest.messages_json, "{\"go\":\"goes\"}") == 0 && strcmp(copy->manifest.name, "Uno") == 0,
          "the copy");
    copy->files[0].data[0] = 'X';
    free_pkg(copy);
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("one", copy) && copy->files[0].data[0] == 'r', "the store's own is untouched");
    free_pkg(copy);
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && !apps_ram_load("three", copy) && copy->count == 0, "no three");
    apps_port_free(copy);
    /* Out of memory, a load fails whole: its two blocks are messages_json and main.lua. */
    for (long k = 0; k < 4; k++) {
        size_t before = port_held;
        copy = apps_port_realloc(NULL, sizeof *copy);
        CHECK(copy != NULL, "no memory");
        port_refuse = k;
        bool loaded = apps_ram_load("one", copy);
        port_refuse = -1;
        CHECK(loaded == (k >= 2), "refusal %ld: loaded %d", k, loaded);
        if (loaded) {
            apps_pkg_free(copy, apps_port_free);
        }
        apps_port_free(copy);
        CHECK(port_held == before, "refusal %ld: %zu bytes left", k, port_held - before);
    }
    /* Full: eight ids, then a ninth is refused (and freed), while one of the eight is replaced. */
    char id[8];
    for (int i = 3; i <= 8; i++) {
        snprintf(id, sizeof id, "app%d", i);
        CHECK(apps_ram_put(make_pkg(id, "App", "return {}\n")), "put %s", id);
    }
    CHECK(!apps_ram_put(make_pkg("nine", "Nine", "return {}\n")), "a ninth id");
    CHECK(!apps_ram_has("nine"), "not kept");
    CHECK(apps_ram_put(make_pkg("two", "Two again", "return {}\n")), "a kept id when full");
    char err[200];
    bool unchanged = true;
    apps_pkg_t *p = make_pkg("nine", "Nine", "return {}\n");
    CHECK(!apps_ram_commit(p, &unchanged, err, sizeof err) && !unchanged &&
              strcmp(err, "the gadget keeps at most 8 apps in RAM; restart it to make room") == 0,
          "commit when full: %s", err);
    CHECK(apps_ram_remove("app8") && apps_ram_commit(p, &unchanged, err, sizeof err) && !unchanged && err[0] == '\0',
          "commit after a remove: %s", err);
    /* The commit kept a copy: the caller's package is still the caller's. */
    p->files[0].data[0] = 'Z';
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("nine", copy) && copy->files[0].data[0] == 'r', "the store has its own copy");
    free_pkg(copy);
    port_refuse = 0;
    CHECK(!apps_ram_commit(p, &unchanged, err, sizeof err) && strcmp(err, "out of memory: no room to keep the app in RAM") == 0,
          "commit without memory: %s", err);
    port_refuse = -1;
    free_pkg(p);
    /* A launch of a stored copy has the shape of an install's run, without "installed but". */
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("one", copy), "load one");
    const char *got = show(apps_install_launch(copy, &OPS));
    static const char launched[] = "{\"ok\":true,\"payload\":{\"id\":\"one\",\"version\":\"0.3\",\"storage\":\"ram\","
                                   "\"files\":[{\"path\":\"main.lua\",\"bytes\":13,\"sha256\":\"";
    CHECK(strncmp(got, launched, strlen(launched)) == 0 && strstr(got, "\"state\":\"running\"") != NULL, "%s", got);
    CHECK(runs == 1 && run_settle == APPS_SETTLE_MS, "run in front with the settle");
    run_ok = false;
    run_err = "crashed in start(): main.lua:1: no";
    crash_record("main.lua:1: no", 1);
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("one", copy), "load one");
    EXPECT_FAILURE(show(apps_install_launch(copy, &OPS)), APPS_ERR_CRASHED, "crashed in start(): main.lua:1: no\nlog:\nl01");
    run_err = "the app task is busy; try again";
    run_record.state = APPS_STATE_RUNNING;
    copy = apps_port_realloc(NULL, sizeof *copy);
    CHECK(copy != NULL && apps_ram_load("one", copy), "load one");
    EXPECT_FAILURE(show(apps_install_launch(copy, &OPS)), APPS_ERR_NOT_RUNNING, "the app task is busy; try again");
}

static void status_json(void) {
    apps_status_t *st = calloc(1, sizeof *st);
    CHECK(st != NULL, "no memory");
    st->state = APPS_STATE_CRASHED;
    snprintf(st->id, sizeof st->id, "hello");
    snprintf(st->name, sizeof st->name, "Hello");
    snprintf(st->version, sizeof st->version, "1.0");
    snprintf(st->last_error, sizeof st->last_error, "main.lua:1: x\nstack traceback:\n\tmain.lua:1: in main chunk");
    snprintf(st->warnings, sizeof st->warnings, "a; b");
    snprintf(st->log_tail, sizeof st->log_tail, "only");
    snprintf(st->ui_dump, sizeof st->ui_dump, "[]");
    st->instructions = 9007199254740993ull; /* more than a double holds exactly: it prints as the integer it is */
    st->mem_kb = 1;
    st->peak_kb = 2;
    st->limit_kb = 3;
    st->worst_dispatch_ms = 4;
    st->cpu_pct = 5;
    st->dropped_events = 6;
    st->stack_free = 7;
    st->widgets = 8;
    st->front = true;
    const char *got = show(apps_install_status(st));
    CHECK(strcmp(got, "{\"state\":\"crashed\",\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0\",\"front\":true,"
                      "\"last_error\":\"main.lua:1: x\\nstack traceback:\\n\\tmain.lua:1: in main chunk\","
                      "\"warnings\":[\"a\",\"b\"],\"log\":[\"only\"],\"ui\":[],\"instructions\":9007199254740993,"
                      "\"mem_kb\":1,\"peak_kb\":2,\"limit_kb\":3,\"worst_dispatch_ms\":4,\"cpu_pct\":5,\"dropped_events\":6,"
                      "\"stack_free\":7,\"widgets\":8}") == 0,
          "%s", got);
    /* No record yet: an empty log and dump are empty arrays. */
    memset(st, 0, sizeof *st);
    got = show(apps_install_status(st));
    CHECK(strstr(got, "{\"state\":\"stopped\",\"id\":\"\",") == got && strstr(got, "\"log\":[],\"ui\":[],") != NULL,
          "%s", got);
    /* The fullest record is trimmed as a result is: the log, then the dump's last entries. */
    full_record();
    memcpy(st, &run_record, sizeof *st);
    st->ui_truncated = 2;
    got = show(apps_install_status(st));
    CHECK(strlen(got) <= APPS_RESULT_MAX && strstr(got, "\"log\":[],\"ui\":[{") != NULL &&
              strstr(got, "\"ui_truncated\":") != NULL && strstr(got, "\"ui_truncated\":2,") == NULL,
          "%zu bytes: %s", strlen(got), got);
    free(st);
}

/* Every allocation an install with a run makes, refused in turn: each ends with a result or NULL, never a crash, and
 * nothing held after it. */
static void out_of_memory(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.2.3\",\"api\":1,\"files\":[\"lib.lua\"],"
                                "\"messages\":{\"go\":\"goes\"}}");
    serve_text("main.lua", MAIN);
    serve_text("lib.lua", "return {}\n");
    full_record();
    port_growths = 0;
    cJSON *r = apps_install_run("hello", true, &SOURCE, &OPS);
    CHECK(r != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")), "the install itself works");
    cJSON_Delete(r);
    long port_total = port_growths;
    CHECK(port_total > 5, "%ld growth requests", port_total);
    for (long k = 0; k < port_total; k++) {
        port_refuse = k;
        r = apps_install_run("hello", true, &SOURCE, &OPS);
        port_refuse = -1;
        if (r != NULL) {
            char *text = cJSON_PrintUnformatted(r);
            CHECK(text != NULL && strlen(text) <= APPS_RESULT_MAX, "refusal %ld: %zu bytes", k, text ? strlen(text) : 0);
            printf("@result %s\n", text);
            cJSON_free(text);
            cJSON_Delete(r);
        }
        (void)apps_ram_remove("hello");
        CHECK(port_held == 0, "port refusal %ld: %zu bytes held", k, port_held);
        CHECK(cjson_held == 0, "port refusal %ld: %zu bytes of cJSON held", k, cjson_held);
    }
    cjson_allocs = 0;
    r = apps_install_run("hello", true, &SOURCE, &OPS);
    cJSON_Delete(r);
    (void)apps_ram_remove("hello");
    long cjson_total = cjson_allocs;
    for (long k = 0; k < cjson_total; k++) {
        cjson_refuse = k;
        r = apps_install_run("hello", true, &SOURCE, &OPS);
        cjson_refuse = -1;
        if (r != NULL) {
            char *text = cJSON_PrintUnformatted(r);
            CHECK(text != NULL && strlen(text) <= APPS_RESULT_MAX, "cJSON refusal %ld", k);
            printf("@result %s\n", text);
            cJSON_free(text);
            cJSON_Delete(r);
        }
        (void)apps_ram_remove("hello");
        CHECK(port_held == 0, "cJSON refusal %ld: %zu bytes held", k, port_held);
        CHECK(cjson_held == 0, "cJSON refusal %ld: %zu bytes of cJSON held", k, cjson_held);
    }
    fflush(stdout);
    fprintf(stderr, "out_of_memory: %ld port and %ld cJSON allocations refused in turn\n", port_total, cjson_total);
}

/* R13.7: apps_install_run on exactly the I/O worker's 6,144 bytes: a success with a 128-widget dump, a crash, a
 * compile error. */
#define WORKER_STACK 6144

static cJSON *stack_result;
static bool stack_run;

static void install_on_the_stack(void *arg) {
    (void)arg;
    stack_result = apps_install_run("hello", stack_run, &SOURCE, &OPS);
}

static size_t on_worker_stack(const char *what, bool run) {
    stack_run = run;
    int exact = 0;
    size_t used = on_exact_stack(install_on_the_stack, NULL, WORKER_STACK, &exact);
    CHECK(used > 0 && exact, "%s: on exactly %d bytes (used %zu, exact %d)", what, WORKER_STACK, used, exact);
    fprintf(stderr, "install_stack: %-8s %zu of %d bytes\n", what, used, WORKER_STACK);
    printf("@stack {\"case\":\"%s\",\"used\":%zu,\"of\":%d}\n", what, used, WORKER_STACK);
    show(stack_result);
    return used;
}

static void install_stack(void) {
    serve_text("manifest.json", "{\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.2.3\",\"api\":1,\"files\":[\"lib/util.lua\","
                                "\"data.json\",\"logo.bin\"],\"permissions\":[\"sound\"],\"messages\":{\"go\":\"goes\"}}");
    serve_text("main.lua", "local app = {}\nlocal x = = 1\nreturn app\n");
    serve_text("lib/util.lua", "return {}\n");
    serve_text("data.json", "{\"a\":[[[[[[[[1]]]]]]]],\"b\":\"text\"}");
    serve_image("logo.bin", 20, 10, true);
    full_record();
    on_worker_stack("success", true);
    CHECK(strncmp(last, "{\"ok\":true,", 11) == 0, "%.200s", last);
    run_ok = false;
    run_err = "crashed in start(): main.lua:4: boom";
    crash_record("main.lua:4: boom\nstack traceback:\n\tmain.lua:4: in function 'start'", 40);
    on_worker_stack("crash", true);
    CHECK(strstr(last, "installed but crashed in start()") != NULL, "%.200s", last);
    compile_raw = "main.lua:2: unexpected symbol near '='";
    on_worker_stack("compile", false);
    CHECK(strstr(last, "compile failed: main.lua:2:") != NULL, "%.200s", last);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"manifest_errors_propagate", manifest_errors_propagate},
        {"path_checked_before_read", path_checked_before_read},
        {"compiled_chunk_refused", compiled_chunk_refused},
        {"oversize_and_totals", oversize_and_totals},
        {"compile_error_has_excerpt", compile_error_has_excerpt},
        {"crash_in_start_message", crash_in_start_message},
        {"crash_in_top_level_message", crash_in_top_level_message},
        {"result_fits_6144_bytes", result_fits_6144_bytes},
        {"busy", busy},
        {"success_payload", success_payload},
        {"read_and_commit_failures", read_and_commit_failures},
        {"deadline_is_one_absolute_time", deadline_is_one_absolute_time},
        {"ram_store", ram_store},
        {"status_json", status_json},
        {"out_of_memory", out_of_memory},
        {"install_stack", install_stack},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    cJSON_Hooks hooks = {cjson_malloc, cjson_free};
    cJSON_InitHooks(&hooks);
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            reset_ops();
            cases[i].run();
            finish();
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
