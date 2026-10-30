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
 * Link's app.* commands (main/apps_link.c), the whole file, run on the host with the real cJSON. Usage:
 * muse_apps_link_harness <case>, one case per test in tests/test_muse_apps_link.py; it exits 0 when the case holds, and
 * otherwise prints why.
 *
 * Fakes: the apps (muse_apps_install records what it was given and answers what the case set; muse_apps_id_valid checks
 * the id's pattern and words a refusal as the real one does), the session (noise_ctrl_send_command_result records the
 * result and frees it, or only frees it, as it does for a session that has ended), the VM file reader and writer
 * (vm_file.h's calls, scripted), the clock, and the PSRAM heap the handler's request context comes from (counted).
 * cJSON's allocations are counted too: each case ends with nothing held.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apps_link.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "muse_apps.h"
#include "noise_control.h"
#include "vm_file.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

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

/* ---- cJSON's heap and the PSRAM heap, counted ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t cjson_held;

static void *cjson_malloc(size_t n) {
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

static int psram_blocks;      /* held now */
static uint32_t psram_caps;   /* what the last block asked for */
static bool psram_refuses;

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    psram_caps = caps;
    if (psram_refuses) {
        return NULL;
    }
    void *p = calloc(n, size);
    psram_blocks += p != NULL;
    return p;
}

void heap_caps_free(void *ptr) {
    if (ptr != NULL) {
        psram_blocks--;
        free(ptr);
    }
}

/* ---- The clock ---- */

static int64_t now_us = 1000000;

int64_t esp_timer_get_time(void) {
    return now_us;
}

/* ---- The apps (muse_apps.h) ---- */

static const muse_apps_vm_ops_t *vm_ops;

void muse_apps_set_vm_ops(const muse_apps_vm_ops_t *ops) {
    vm_ops = ops;
}

bool muse_apps_id_valid(const char *id, char *err, size_t err_len) {
    size_t n = id != NULL ? strlen(id) : 0;
    bool ok = n >= 2 && n <= 24 && id[0] >= 'a' && id[0] <= 'z';
    for (size_t i = 1; ok && i < n; i++) {
        ok = (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_';
    }
    if (!ok) {
        snprintf(err, err_len, "app id '%s' must match ^[a-z][a-z0-9_]{1,23}$", id != NULL ? id : "");
    }
    return ok;
}

static int installs;
static char install_id[64];
static bool install_run;
static muse_apps_done_cb install_done_cb;
static void *install_ctx;
static bool install_ok = true;
static const char *install_code = "busy", *install_err = "busy: another install is running";

bool muse_apps_install(const char *id, bool run, muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    installs++;
    snprintf(install_id, sizeof install_id, "%s", id);
    install_run = run;
    install_done_cb = done;
    install_ctx = ctx;
    if (!install_ok) {
        *code = install_code;
        *err = install_err;
    }
    return install_ok;
}

/* ---- The session ---- */

static int sends;
static noise_ctrl_session_generation_t sent_generation;
static char sent_request[80];
static char sent_result[1024];
static bool session_ended; /* the result is for a session that has ended: freed, not sent */

void noise_ctrl_send_command_result(noise_ctrl_session_generation_t session_generation, const char *request_id, cJSON *result) {
    sends++;
    if (!session_ended) {
        sent_generation = session_generation;
        snprintf(sent_request, sizeof sent_request, "%s", request_id != NULL ? request_id : "(null)");
        char *text = result != NULL ? cJSON_PrintUnformatted(result) : NULL;
        snprintf(sent_result, sizeof sent_result, "%s", text != NULL ? text : "(null)");
        cJSON_free(text);
    }
    cJSON_Delete(result);
}

/* ---- The VM's file reader and writer (vm_file.h), scripted ---- */

static struct vm_file {
    int unused;
} the_file;

static int opens, closes, open_now;
static char opened_path[160];
static size_t opened_keep;
static const char *opened_owner;
static const char *open_code;     /* NULL: the open works; else its code and message */
static const char *open_message;
static size_t read_max;
static int64_t read_deadline;
static const char *file_text = ""; /* what the read hands the sink, in pieces of 4 */
static bool read_fails, read_timed_out;
static size_t read_bytes;          /* vm_file_bytes after the read */
static const char *read_error = "the VM connection dropped";
static bool error_in_msg;          /* vm_file_error writes its text into msg, as it does for "VM answered HTTP <n>" */

vm_file_t *vm_file_open(const char *path, size_t keep, const char *owner, const char **code, const char **message) {
    opens++;
    snprintf(opened_path, sizeof opened_path, "%s", path);
    opened_keep = keep;
    opened_owner = owner;
    if (open_code != NULL) {
        *code = open_code;
        *message = open_message;
        return NULL;
    }
    CHECK(open_now == 0, "two readers open");
    open_now++;
    return &the_file;
}

int vm_file_read(vm_file_t *f, uint8_t *buf, size_t len, int64_t deadline_us) {
    (void)f;
    (void)buf;
    (void)len;
    (void)deadline_us;
    fail_at(__LINE__, "vm_file_read: the adapter reads with vm_file_read_all");
}

bool vm_file_read_all(vm_file_t *f, size_t max, vm_file_sink_t sink, void *user, int64_t deadline_us) {
    CHECK(f == &the_file && open_now == 1, "a read of an open reader");
    read_max = max;
    read_deadline = deadline_us;
    size_t n = strlen(file_text);
    for (size_t at = 0; at < n; at += 4) {
        if (!sink(user, (const uint8_t *)file_text + at, n - at < 4 ? n - at : 4)) {
            return false;
        }
    }
    return !read_fails;
}

const char *vm_file_error(vm_file_t *f, char *msg, size_t msg_len) {
    CHECK(f == &the_file && open_now == 1, "the error of an open reader");
    if (error_in_msg) {
        snprintf(msg, msg_len, "%s", read_error);
        return msg;
    }
    return read_error;
}

size_t vm_file_bytes(const vm_file_t *f) {
    CHECK(f == &the_file && open_now == 1, "the bytes of an open reader");
    return read_bytes;
}

size_t vm_file_chunk(const vm_file_t *f) {
    (void)f;
    return 63488;
}

bool vm_file_timed_out(const vm_file_t *f) {
    CHECK(f == &the_file && open_now == 1, "the time of an open reader");
    return read_timed_out;
}

void vm_file_close(vm_file_t *f) {
    if (f != NULL) {
        CHECK(f == &the_file && open_now == 1, "a close of the open reader");
        open_now--;
        closes++;
    }
}

static int writes;
static char written_path[160];
static size_t written_len;
static int write_timeout_ms;
static const char *write_error; /* NULL: the write works */

bool vm_file_write(const char *path, const void *data, size_t len, int timeout_ms, char *err, size_t err_len) {
    (void)data;
    writes++;
    snprintf(written_path, sizeof written_path, "%s", path);
    written_len = len;
    write_timeout_ms = timeout_ms;
    if (write_error != NULL) {
        snprintf(err, err_len, "%s", write_error);
        return false;
    }
    return true;
}

/* ---- Helpers ---- */

/* r printed, then freed. */
static char *printed(cJSON *r) {
    CHECK(r != NULL, "no reply");
    char *text = cJSON_PrintUnformatted(r);
    CHECK(text != NULL, "no memory to print");
    char *copy = strdup(text);
    cJSON_free(text);
    cJSON_Delete(r);
    return copy;
}

/* The handler's reply to params (a JSON text, or NULL for none) is exactly want. */
static void expect_reply_at(int line, const char *params_json, const char *request_id, const char *want) {
    cJSON *params = params_json != NULL ? cJSON_Parse(params_json) : NULL;
    CHECK(params_json == NULL || params != NULL, "the case's params: %s", params_json);
    char *got = printed(apps_link_handle("app.install", params, request_id, 7));
    cJSON_Delete(params);
    if (strcmp(got, want) != 0) {
        fail_at(line, "app.install %s replied\n%s\ninstead of\n%s", params_json ? params_json : "(no params)", got, want);
    }
    free(got);
}
#define EXPECT_REPLY(params, request_id, want) expect_reply_at(__LINE__, (params), (request_id), (want))

/* ---- The cases ---- */

/* The app.* entries of link.register's commands_v2: printed for the Python test, which checks them against the budget. */
static void register_entries(void) {
    cJSON *commands = cJSON_CreateObject();
    CHECK(commands != NULL, "no memory");
    apps_link_register(commands);
    char *text = cJSON_PrintUnformatted(commands);
    CHECK(text != NULL, "no memory to print");
    printf("@register %s\n", text);
    cJSON_free(text);
    cJSON_Delete(commands);
}

/* Which commands are this file's: app.install, and nothing it doesn't handle. */
static void handles(void) {
    CHECK(apps_link_handles("app.install"), "app.install");
    static const char *const others[] = {"app.api", "app.control", "app.send", "app.", "app", "device.health", "ble.state",
                                         "app.install ", ""};
    for (size_t i = 0; i < ARRAY_LEN(others); i++) {
        CHECK(!apps_link_handles(others[i]), "'%s' isn't app.install's", others[i]);
    }
    CHECK(!apps_link_handles(NULL), "no command");
    char *got = printed(apps_link_handle("app.nope", NULL, "r1", 7));
    CHECK(strcmp(got, "{\"ok\":false,\"error\":{\"code\":\"unsupported\",\"message\":\"unknown app command\"}}") == 0, "%s", got);
    free(got);
    CHECK(installs == 0, "nothing installed");
}

/*
 * The install handler, on the Noise session's task: it checks id and run, copies request_id and session_generation into a
 * request of its own (in PSRAM), and starts the install, never waiting for it: {"_async":true}. The done callback sends the
 * result once, with the request's id and generation, and frees the request, also when the session has ended and the send only
 * frees the result. A refusal of muse_apps_install is the reply, with its code, and the request is freed.
 */
static void install_handler(void) {
    /* What is refused before anything is started. */
    EXPECT_REPLY(NULL, "r1", "{\"ok\":false,\"error\":{\"code\":\"missing_param\",\"message\":\"id is required\"}}");
    EXPECT_REPLY("{}", "r1", "{\"ok\":false,\"error\":{\"code\":\"missing_param\",\"message\":\"id is required\"}}");
    EXPECT_REPLY("{\"id\":\"\"}", "r1", "{\"ok\":false,\"error\":{\"code\":\"missing_param\",\"message\":\"id is required\"}}");
    EXPECT_REPLY("{\"id\":5}", "r1", "{\"ok\":false,\"error\":{\"code\":\"missing_param\",\"message\":\"id is required\"}}");
    EXPECT_REPLY("{\"ID\":\"hello\"}", "r1", "{\"ok\":false,\"error\":{\"code\":\"missing_param\",\"message\":\"id is required\"}}");
    EXPECT_REPLY("{\"id\":\"Hello\"}", "r1",
                 "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"app id 'Hello' must match "
                 "^[a-z][a-z0-9_]{1,23}$\"}}");
    EXPECT_REPLY("{\"id\":\"hello\",\"run\":\"yes\"}", "r1",
                 "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"run must be true or false\"}}");
    EXPECT_REPLY("{\"id\":\"hello\",\"run\":1}", "r1",
                 "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"run must be true or false\"}}");
    char long_id[80];
    memset(long_id, 'r', 64);
    long_id[64] = '\0';
    EXPECT_REPLY("{\"id\":\"hello\"}", long_id,
                 "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"request_id is invalid\"}}");
    EXPECT_REPLY("{\"id\":\"hello\"}", NULL,
                 "{\"ok\":false,\"error\":{\"code\":\"invalid_params\",\"message\":\"request_id is invalid\"}}");
    psram_refuses = true;
    EXPECT_REPLY("{\"id\":\"hello\"}", "r1", "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"failed to allocate\"}}");
    psram_refuses = false;
    CHECK(installs == 0 && psram_blocks == 0, "nothing started, nothing held: %d installs, %d blocks", installs, psram_blocks);

    /* Started: _async, and the request is a copy the handler's caller can free at once (the session frees its request). */
    char request[64];
    snprintf(request, sizeof request, "req-0001");
    EXPECT_REPLY("{\"id\":\"hello\"}", request, "{\"_async\":true}");
    memset(request, 'x', sizeof request - 1);
    CHECK(installs == 1 && strcmp(install_id, "hello") == 0 && install_run, "install hello with run (the default)");
    CHECK(install_done_cb != NULL && install_ctx != NULL && psram_blocks == 1, "a request of its own, waiting for done");
    CHECK(psram_caps & MALLOC_CAP_SPIRAM, "the request is in PSRAM: caps 0x%x", (unsigned)psram_caps);
    CHECK(sends == 0, "nothing sent yet");
    cJSON *result = cJSON_Parse("{\"ok\":true,\"payload\":{\"id\":\"hello\"}}");
    install_done_cb(install_ctx, result);
    CHECK(sends == 1 && sent_generation == 7 && strcmp(sent_request, "req-0001") == 0, "sent once, as req-0001 of session 7: %s",
          sent_request);
    CHECK(strcmp(sent_result, "{\"ok\":true,\"payload\":{\"id\":\"hello\"}}") == 0, "%s", sent_result);
    CHECK(psram_blocks == 0, "the request freed");

    /* run false; the session has ended by the time the result comes: the send frees it, and so does done its request. */
    EXPECT_REPLY("{\"id\":\"timer\",\"run\":false}", "req-0002", "{\"_async\":true}");
    CHECK(installs == 2 && strcmp(install_id, "timer") == 0 && !install_run, "install timer without run");
    session_ended = true;
    install_done_cb(install_ctx, cJSON_Parse("{\"ok\":false,\"error\":{\"code\":\"read_failed\",\"message\":\"x\"}}"));
    session_ended = false;
    CHECK(sends == 2 && psram_blocks == 0, "handed to the send once, and the request freed: %d sends, %d blocks", sends, psram_blocks);

    /* No memory for the result: done sends one that says so. */
    EXPECT_REPLY("{\"id\":\"hello\",\"run\":true}", "req-0003", "{\"_async\":true}");
    install_done_cb(install_ctx, NULL);
    CHECK(sends == 3 && strcmp(sent_request, "req-0003") == 0 &&
              strcmp(sent_result, "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"out of memory: no room for "
                                  "the install's result\"}}") == 0,
          "%s", sent_result);
    CHECK(psram_blocks == 0, "the request freed");

    /* muse_apps_install refuses (another install runs): its code and words are the reply, and the request is freed. */
    install_ok = false;
    EXPECT_REPLY("{\"id\":\"hello\"}", "req-0004",
                 "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"busy: another install is running\"}}");
    install_code = "unavailable";
    install_err = "the gadget can't read files on the VM";
    EXPECT_REPLY("{\"id\":\"hello\"}", "req-0005",
                 "{\"ok\":false,\"error\":{\"code\":\"unavailable\",\"message\":\"the gadget can't read files on the VM\"}}");
    install_ok = true;
    CHECK(installs == 5 && sends == 3 && psram_blocks == 0, "refused twice, nothing sent, nothing held");
}

/* The sink the adapter's reads are given here: it keeps what it gets. */
static char sunk[256];
static size_t sunk_len;
static bool sink_stops;

static bool keep_sunk(void *user, const uint8_t *d, size_t n) {
    CHECK(user == (void *)sunk, "the user pointer is passed through");
    if (sink_stops) {
        return false;
    }
    CHECK(sunk_len + n < sizeof sunk, "too much for the case");
    memcpy(sunk + sunk_len, d, n);
    sunk_len += n;
    sunk[sunk_len] = '\0';
    return true;
}

/* One read through the ops: true or false, with the code and the words. */
static bool vm_read(const char *path, size_t max, int64_t deadline_us, const char **code, char *err, size_t err_len) {
    sunk_len = 0;
    sunk[0] = '\0';
    *code = NULL;
    err[0] = '\0';
    return vm_ops->read(path, max, keep_sunk, sunk, deadline_us, code, err, err_len);
}

/*
 * apps_link_init puts vm_file behind the apps' VM ops. read opens the reader for the path with the install's owner and no keep
 * (the install runs on a PSRAM-stack task), reads it whole into the sink with max and the deadline as given, and always closes
 * it. Its refusals are the apps' codes: busy (someone else's reader, in its words), out_of_memory (with words that say to try
 * again), timed_out (the reader said its deadline came), too_large (more than max came), failed (the rest, in the reader's
 * words, whether they are a literal or written into the caller's room). write is vm_file_write with the time left to the
 * deadline, which a deadline already past never reaches.
 */
static void vm_ops_are_vm_file(void) {
    CHECK(vm_ops == NULL, "nothing before apps_link_init");
    apps_link_init();
    CHECK(vm_ops != NULL && vm_ops->read != NULL && vm_ops->write != NULL, "the ops");
    const char *code;
    char err[160];

    file_text = "return {}\n";
    read_bytes = strlen(file_text);
    CHECK(vm_read("workspace/apps/hello/main.lua", 262144, 91000000, &code, err, sizeof err), "a read: %s %s", code, err);
    CHECK(strcmp(sunk, "return {}\n") == 0, "the file went to the sink: '%s'", sunk);
    CHECK(strcmp(opened_path, "workspace/apps/hello/main.lua") == 0 && opened_keep == 0 && opened_owner != NULL &&
              strcmp(opened_owner, VM_FILE_OWNER_INSTALL) == 0,
          "opened %s, keep %zu, owner %s", opened_path, opened_keep, opened_owner ? opened_owner : "(null)");
    CHECK(read_max == 262144 && read_deadline == 91000000, "max %zu, deadline %lld", read_max, (long long)read_deadline);
    CHECK(closes == 1 && open_now == 0, "closed");

    /* The open's refusals: nothing to close. */
    open_code = "busy";
    open_message = "the VM file reader is busy (an image download)";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "busy");
    CHECK(strcmp(code, MUSE_APPS_VM_BUSY) == 0 && strcmp(err, "the VM file reader is busy (an image download)") == 0, "%s: %s", code,
          err);
    open_code = "out_of_memory";
    open_message = "failed to allocate";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "no memory");
    CHECK(strcmp(code, MUSE_APPS_VM_OUT_OF_MEMORY) == 0 &&
              strcmp(err, "the gadget is short of memory for the VM reader right now; try again in a few seconds") == 0,
          "%s: %s", code, err);
    open_code = "invalid_params";
    open_message = "vm_file_open: bad path";
    CHECK(!vm_read("/etc/passwd", 100, 1, &code, err, sizeof err), "a bad path");
    CHECK(strcmp(code, MUSE_APPS_VM_FAILED) == 0 && strcmp(err, "vm_file_open: bad path") == 0, "%s: %s", code, err);
    open_code = NULL;
    CHECK(closes == 1 && open_now == 0, "nothing opened, nothing closed");

    /* The read's: always closed. */
    read_fails = true;
    read_error = "the VM connection dropped";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "a drop");
    CHECK(strcmp(code, MUSE_APPS_VM_FAILED) == 0 && strcmp(err, "the VM connection dropped") == 0, "%s: %s", code, err);
    error_in_msg = true;
    read_error = "VM answered HTTP 404";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "a 404");
    CHECK(strcmp(code, MUSE_APPS_VM_FAILED) == 0 && strcmp(err, "VM answered HTTP 404") == 0, "%s: %s", code, err);
    error_in_msg = false;
    read_timed_out = true;
    read_error = "download timed out";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "a timeout");
    CHECK(strcmp(code, MUSE_APPS_VM_TIMED_OUT) == 0 && strcmp(err, "download timed out") == 0, "%s: %s", code, err);
    read_timed_out = false;
    read_bytes = 101;
    error_in_msg = true;
    read_error = "larger than 100 bytes";
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "too large");
    CHECK(strcmp(code, MUSE_APPS_VM_TOO_LARGE) == 0 && strcmp(err, "larger than 100 bytes") == 0, "%s: %s", code, err);
    /* A sink that stops the read (vm_file says "larger than" for that too) isn't too large when no more than max came. */
    read_bytes = 40;
    read_fails = false;
    sink_stops = true;
    CHECK(!vm_read("workspace/apps/hello/main.lua", 100, 1, &code, err, sizeof err), "the sink stopped it");
    CHECK(strcmp(code, MUSE_APPS_VM_FAILED) == 0, "%s: %s", code, err);
    sink_stops = false;
    error_in_msg = false;
    CHECK(closes == 6 && open_now == 0, "every read closed: %d", closes);

    /* write: the time left to the deadline, in ms. */
    now_us = 5000000;
    CHECK(vm_ops->write("workspace/apps/_guide/guide.md", "abc", 3, 5000000 + 12345678, &code, err, sizeof err), "a write");
    CHECK(writes == 1 && strcmp(written_path, "workspace/apps/_guide/guide.md") == 0 && written_len == 3 &&
              write_timeout_ms == 12345,
          "wrote %s, %zu bytes, %d ms", written_path, written_len, write_timeout_ms);
    CHECK(vm_ops->write("a", "abc", 3, INT64_MAX, &code, err, sizeof err) && write_timeout_ms == INT_MAX, "a far deadline: %d ms",
          write_timeout_ms);
    code = NULL;
    CHECK(!vm_ops->write("a", "abc", 3, 5000000, &code, err, sizeof err), "a deadline that has come");
    CHECK(writes == 2 && strcmp(code, MUSE_APPS_VM_TIMED_OUT) == 0 && strcmp(err, "write timed out") == 0,
          "not written: %d writes, %s: %s", writes, code, err);
    static const struct {
        const char *error, *code;
    } failures[] = {
        {"write timed out", MUSE_APPS_VM_TIMED_OUT},
        {"the VM file writer is busy", MUSE_APPS_VM_BUSY},
        {"out of memory", MUSE_APPS_VM_OUT_OF_MEMORY},
        {"VM answered HTTP 403: route not allowed", MUSE_APPS_VM_FAILED},
        {"the VM connection dropped", MUSE_APPS_VM_FAILED},
    };
    for (size_t i = 0; i < ARRAY_LEN(failures); i++) {
        write_error = failures[i].error;
        code = NULL;
        CHECK(!vm_ops->write("a", "abc", 3, 9000000, &code, err, sizeof err), "%s", failures[i].error);
        CHECK(code != NULL && strcmp(code, failures[i].code) == 0 && strcmp(err, failures[i].error) == 0, "%s: %s for %s",
              code ? code : "(null)", err, failures[i].error);
    }
    write_error = NULL;
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"register_entries", register_entries},
        {"handles", handles},
        {"install_handler", install_handler},
        {"vm_ops_are_vm_file", vm_ops_are_vm_file},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    cJSON_Hooks hooks = {cjson_malloc, cjson_free};
    cJSON_InitHooks(&hooks);
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(cjson_held == 0, "%zu bytes of cJSON's still held", cjson_held);
            CHECK(psram_blocks == 0, "%d PSRAM blocks still held", psram_blocks);
            CHECK(open_now == 0, "a reader left open");
            fflush(stdout);
            _exit(0);
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
