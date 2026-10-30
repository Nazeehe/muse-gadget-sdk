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

#include "apps_link.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "muse_apps.h"
#include "vm_file.h"

// Link's app.* commands (apps_link.h).
//
// The session's task. on_ws_command() runs these on the Noise session's task,
// which delivers every frame of the session: so nothing here waits. app.install
// checks its parameters, copies request_id and session_generation into a
// request of its own (PSRAM: the internal RAM is the Link's), and queues the
// install (muse_apps_install), which runs as one job of the apps' I/O worker;
// the job's done sends the result once and frees the request, whatever became
// of the session meanwhile (noise_ctrl_send_command_result drops a result of a
// session that has ended).
//
// The VM's files. The apps read Muse's VM through muse_apps_vm_ops_t, which this
// file puts vm_file behind: a read opens the one reader for the install, with
// no keep (the worker's stack and its blocks are PSRAM), reads the file whole
// into the caller's sink and closes it; its refusals are the apps' codes. They
// run on the worker, never on the session's task.

#define INSTALL_TIMEOUT_MS 120000  // the VM's wait for app.install's result: over the gadget's 90 s deadline and a run's settle
#define REQUEST_ID_MAX 64

// What the reader's "out_of_memory" means for the one who asked.
#define READER_NO_MEMORY "the gadget is short of memory for the VM reader right now; try again in a few seconds"

// ---- The VM's files, behind muse_apps_vm_ops_t ----

static bool vm_read(const char *path, size_t max, bool (*sink)(void *user, const uint8_t *d, size_t n), void *user,
                    int64_t deadline_us, const char **code, char *err, size_t err_len) {
    const char *open_code = NULL, *message = NULL;
    vm_file_t *f = vm_file_open(path, 0, VM_FILE_OWNER_INSTALL, &open_code, &message);
    if (!f) {
        bool busy = open_code && strcmp(open_code, "busy") == 0;
        bool no_memory = open_code && strcmp(open_code, "out_of_memory") == 0;
        *code = busy ? MUSE_APPS_VM_BUSY : no_memory ? MUSE_APPS_VM_OUT_OF_MEMORY : MUSE_APPS_VM_FAILED;
        snprintf(err, err_len, "%s", no_memory ? READER_NO_MEMORY : message ? message : "the VM file reader can't be opened");
        return false;
    }
    bool ok = vm_file_read_all(f, max, sink, user, deadline_us);
    if (!ok) {
        // vm_file says a sink's stop as "larger than <max>" too: only more than max having come is too large.
        *code = vm_file_timed_out(f)      ? MUSE_APPS_VM_TIMED_OUT
                : vm_file_bytes(f) > max ? MUSE_APPS_VM_TOO_LARGE
                                         : MUSE_APPS_VM_FAILED;
        const char *why = vm_file_error(f, err, err_len);  // a literal, or err written
        if (why != err) snprintf(err, err_len, "%s", why);
    }
    vm_file_close(f);
    return ok;
}

static bool vm_write(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                     size_t err_len) {
    int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
    if (left_ms <= 0) {
        *code = MUSE_APPS_VM_TIMED_OUT;
        snprintf(err, err_len, "write timed out");
        return false;
    }
    if (vm_file_write(path, data, len, left_ms > INT_MAX ? INT_MAX : (int)left_ms, err, err_len)) return true;
    // vm_file.h's words for these.
    *code = strcmp(err, "write timed out") == 0             ? MUSE_APPS_VM_TIMED_OUT
            : strcmp(err, "the VM file writer is busy") == 0 ? MUSE_APPS_VM_BUSY
            : strcmp(err, "out of memory") == 0              ? MUSE_APPS_VM_OUT_OF_MEMORY
                                                             : MUSE_APPS_VM_FAILED;
    return false;
}

static const muse_apps_vm_ops_t VM_OPS = {vm_read, vm_write};

void apps_link_init(void) {
    muse_apps_set_vm_ops(&VM_OPS);
}

// ---- link.register ----

static cJSON *param(const char *type, const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

// One entry of commands_v2, as noise_control.cpp's add_command makes them, with the VM's wait for its result.
static void add_command(cJSON *commands, const char *name, const char *description, cJSON *required, cJSON *optional,
                        int timeout_ms) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddNumberToObject(c, "timeout_ms", timeout_ms);
    cJSON_AddItemToObject(commands, name, c);
}

void apps_link_register(cJSON *commands) {
    cJSON *required = cJSON_CreateObject(), *optional = cJSON_CreateObject();
    cJSON_AddItemToObject(required, "id", param("string", "App id: lowercase, the folder name."));
    cJSON_AddItemToObject(optional, "run", param("boolean", "Run it after installing (default true)."));
    add_command(commands, "app.install",
                "Install the app in workspace/apps/<id>/ on this VM (manifest.json and the files it lists) onto the "
                "gadget, and run it. Returns compile errors, a crash traceback and logs, a widget dump and round-screen "
                "warnings. Call app.api first.",
                required, optional, INSTALL_TIMEOUT_MS);
}

// ---- The commands ----

static cJSON *error_result(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

// Who asked for an install: what its result is sent to.
typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[REQUEST_ID_MAX];
} request_t;

// The install's done (muse_apps.h), on the I/O worker: the result once, then the request goes.
static void install_done(void *ctx, cJSON *result) {
    request_t *req = ctx;
    if (!result) result = error_result("out_of_memory", "out of memory: no room for the install's result");
    noise_ctrl_send_command_result(req->session_generation, req->request_id, result);  // it owns the result
    heap_caps_free(req);
}

static cJSON *install_command(cJSON *params, const char *request_id,
                              noise_ctrl_session_generation_t session_generation) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0]) {
        return error_result("missing_param", "id is required");
    }
    char msg[128];
    if (!muse_apps_id_valid(id->valuestring, msg, sizeof(msg))) return error_result("invalid_params", msg);
    cJSON *run = cJSON_GetObjectItemCaseSensitive(params, "run");
    if (run && !cJSON_IsBool(run)) return error_result("invalid_params", "run must be true or false");
    if (!request_id || strlen(request_id) >= REQUEST_ID_MAX) {
        return error_result("invalid_params", "request_id is invalid");
    }
    request_t *req = heap_caps_calloc(1, sizeof(*req), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!req) return error_result("out_of_memory", "failed to allocate");
    req->session_generation = session_generation;
    memcpy(req->request_id, request_id, strlen(request_id) + 1);
    const char *code = "out_of_memory", *err = "failed to start the install";
    if (!muse_apps_install(id->valuestring, !run || cJSON_IsTrue(run), install_done, req, &code, &err)) {
        heap_caps_free(req);
        return error_result(code, err);
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

bool apps_link_handles(const char *command) {
    return command && strcmp(command, "app.install") == 0;
}

cJSON *apps_link_handle(const char *command, cJSON *params, const char *request_id,
                        noise_ctrl_session_generation_t session_generation) {
    if (command && strcmp(command, "app.install") == 0) {
        return install_command(params, request_id, session_generation);
    }
    return error_result("unsupported", "unknown app command");
}
