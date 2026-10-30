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
// which delivers every frame of the session: so nothing here waits long.
// app.install checks its parameters, copies request_id and session_generation
// into a request of its own (PSRAM: the internal RAM is the Link's), and queues
// the install (muse_apps_install), which runs as one job of the apps' I/O
// worker; the job's done sends the result once and frees the request, whatever
// became of the session meanwhile (noise_ctrl_send_command_result drops a
// result of a session that has ended). app.api (the guide's writes, which only
// this task can pump), app.send (the launch an app may need, and the app's
// reply, awaited for at most 5 s) and app.control's launch, stop, remove and
// snapshot (the screen drawn, encoded and written to the VM) go the same way;
// app.control's list, status and logs answer at once, after a wait for the
// worker of at most MUSE_APPS_CONTROL_WAIT_MS (D20).
//
// The VM's files. The apps read Muse's VM through muse_apps_vm_ops_t, which this
// file puts vm_file behind: a read opens the one reader for the install, with
// no keep (the worker's stack and its blocks are PSRAM), reads the file whole
// into the caller's sink and closes it; its refusals are the apps' codes. They
// run on the worker, never on the session's task.

#define INSTALL_TIMEOUT_MS 120000  // the VM's wait for app.install's result: over the gadget's 90 s deadline and a run's settle
#define API_TIMEOUT_MS 30000       // app.api's: the guide's writes (MUSE_APPS_GUIDE_WRITE_MS) and the answer
#define CONTROL_TIMEOUT_MS 30000   // app.control's: a launch's load, run and settle; a snapshot's 25 s (MUSE_APPS_SNAPSHOT_TOTAL_MS)
#define SEND_TIMEOUT_MS 10000      // app.send's: the app's reply bound (5 s, MUSE_APPS_SEND_REPLY_MS) and the round trip
#define REQUEST_ID_MAX 64

// The headroom rule (D11): link.register carries the apps' entries in at most
// 4499 bytes (APPS_REGISTER_BUDGET). All four, app.send's and snapshot's scale
// included, measure well under it (tests/test_muse_apps_link.py asserts it and
// prints the size), so app.control is advertised. Were they over, this would
// be 0: app.control would still dispatch, and app.api's payload would name it
// ("control").
#define ADVERTISE_CONTROL 1

// app.control's actions, as typed.
enum { ACT_LIST, ACT_STATUS, ACT_LOGS, ACT_LAUNCH, ACT_STOP, ACT_REMOVE, ACT_SNAPSHOT };
static const char *const ACTIONS[] = {"list", "status", "logs", "launch", "stop", "remove", "snapshot"};
#define ACTIONS_TEXT "list, status, logs, launch, stop, remove, snapshot"

// What the reader's "out_of_memory" means for the one who asked.
#define READER_NO_MEMORY "the gadget is short of memory for the VM reader right now; try again in a few seconds"

// vm_file's words (vm_file.h) for a session that is gone: the request wasn't sent, or the connection dropped. Nothing will
// read an answer then (MUSE_APPS_VM_DROPPED).
static bool session_gone(const char *why) {
    return strcmp(why, "the VM connection dropped") == 0 || strcmp(why, "could not send the request to the VM") == 0;
}

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
        const char *why = vm_file_error(f, err, err_len);  // a literal, or err written
        if (why != err) snprintf(err, err_len, "%s", why);
        *code = vm_file_timed_out(f)      ? MUSE_APPS_VM_TIMED_OUT
                : vm_file_bytes(f) > max ? MUSE_APPS_VM_TOO_LARGE
                : session_gone(err)      ? MUSE_APPS_VM_DROPPED
                                         : MUSE_APPS_VM_FAILED;
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
            : session_gone(err)                              ? MUSE_APPS_VM_DROPPED
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

static void register_send(cJSON *commands) {
    cJSON *required = cJSON_CreateObject(), *optional = cJSON_CreateObject();
    cJSON_AddItemToObject(required, "id", param("string", "App id."));
    cJSON_AddItemToObject(required, "event", param("string", "One of the app's messages (app.control list shows them)."));
    cJSON_AddItemToObject(optional, "data", param("object", "The event's data, e.g. {\"seconds\":300}."));
    cJSON_AddItemToObject(optional, "show", param("boolean", "Bring the app to the front once this turn ends."));
    add_command(commands, "app.send",
                "Send an event to a gadget app (its manifest's messages list them). Starts the app if needed. show=true "
                "brings it to the front once this turn ends. Returns the app's reply.",
                required, optional, SEND_TIMEOUT_MS);
}

void apps_link_register(cJSON *commands) {
    add_command(commands, "app.api",
                "Call before writing an app. Writes the gadget app guide and a timer example to workspace/apps/_guide/ on "
                "this VM and returns the API level, screen, fonts, limits, storage and installed apps.",
                NULL, NULL, API_TIMEOUT_MS);
    cJSON *required = cJSON_CreateObject(), *optional = cJSON_CreateObject();
    cJSON_AddItemToObject(required, "id", param("string", "App id: lowercase, the folder name."));
    cJSON_AddItemToObject(optional, "run", param("boolean", "Run it after installing (default true)."));
    add_command(commands, "app.install",
                "Install the app in workspace/apps/<id>/ on this VM (manifest.json and the files it lists) onto the "
                "gadget, and run it. Returns compile errors, a crash traceback and logs, a widget dump and round-screen "
                "warnings. Call app.api first.",
                required, optional, INSTALL_TIMEOUT_MS);
    if (ADVERTISE_CONTROL) {
        required = cJSON_CreateObject();
        optional = cJSON_CreateObject();
        cJSON_AddItemToObject(required, "action", param("string", "list, status, logs, launch, stop, remove or snapshot."));
        cJSON_AddItemToObject(optional, "id", param("string", "App id, for every action but list."));
        cJSON_AddItemToObject(optional, "scale", param("integer", "1 full size (default), 2 half"));
        add_command(commands, "app.control",
                    "Manage gadget apps. action: list, status, logs, launch, stop, remove or snapshot; id for all but list. "
                    "status and logs return state, errors with traceback, the last 50 log lines, memory and CPU.",
                    required, optional, CONTROL_TIMEOUT_MS);
    }
    register_send(commands);
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

// Who asked for an install (or another command that answers later): what its result is sent to.
typedef struct {
    noise_ctrl_session_generation_t session_generation;
    char request_id[REQUEST_ID_MAX];
} request_t;

// A request of its own for request_id, in PSRAM; NULL, with *refusal the reply, when request_id isn't one or there is no
// memory.
static request_t *new_request(const char *request_id, noise_ctrl_session_generation_t session_generation,
                              cJSON **refusal) {
    if (!request_id || strlen(request_id) >= REQUEST_ID_MAX) {
        *refusal = error_result("invalid_params", "request_id is invalid");
        return NULL;
    }
    request_t *req = heap_caps_calloc(1, sizeof(*req), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!req) {
        *refusal = error_result("out_of_memory", "failed to allocate");
        return NULL;
    }
    req->session_generation = session_generation;
    memcpy(req->request_id, request_id, strlen(request_id) + 1);
    return req;
}

static cJSON *async_reply(void) {
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

// A queued command's done (muse_apps.h), on the I/O worker: the result once (no_room when there was no memory for one), then
// the request goes.
static void finish_request(void *ctx, cJSON *result, const char *no_room) {
    request_t *req = ctx;
    if (!result) result = error_result("out_of_memory", no_room);
    noise_ctrl_send_command_result(req->session_generation, req->request_id, result);  // it owns the result
    heap_caps_free(req);
}

// app.control's and app.send's queued actions (a snapshot's among them).
static void command_done(void *ctx, cJSON *result) {
    finish_request(ctx, result, "out of memory: no room for the result");
}

// app.api's done: as command_done, and a payload that names app.control when link.register leaves it out.
static void api_done(void *ctx, cJSON *result) {
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(result, "payload");
    if (!ADVERTISE_CONTROL && cJSON_IsObject(payload)) {
        cJSON_AddStringToObject(payload, "control", "app.control {action, id}");
    }
    command_done(ctx, result);
}

// app.install's.
static void install_done(void *ctx, cJSON *result) {
    finish_request(ctx, result, "out of memory: no room for the install's result");
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
    cJSON *refusal = NULL;
    request_t *req = new_request(request_id, session_generation, &refusal);
    if (!req) return refusal;
    const char *code = "out_of_memory", *err = "failed to start the install";
    if (!muse_apps_install(id->valuestring, !run || cJSON_IsTrue(run), install_done, req, &code, &err)) {
        heap_caps_free(req);
        return error_result(code, err);
    }
    return async_reply();
}

// app.api (D2): no params. The guide's writes wait for the VM, whose answers only this task delivers, so they are a job of
// the I/O worker, and the result follows.
static cJSON *api_command(const char *request_id, noise_ctrl_session_generation_t session_generation) {
    cJSON *refusal = NULL;
    request_t *req = new_request(request_id, session_generation, &refusal);
    if (!req) return refusal;
    const char *code = "out_of_memory", *err = "failed to start app.api";
    if (!muse_apps_write_guide(api_done, req, &code, &err)) {
        heap_caps_free(req);
        return error_result(code, err);
    }
    return async_reply();
}

// app.send's text for data (D25) and its limit, MUSE_APPS_SEND_DATA_MAX.
#define SEND_DATA_INVALID "data must be JSON (an object like {\"seconds\":300})"
#define SEND_DATA_LARGE "data is larger than 4 KB"

// data as the JSON text the app is sent: an object is printed compact, a string is the text as it is. Either must be at most
// MUSE_APPS_SEND_DATA_MAX bytes (checked first: nothing larger is parsed) and one JSON value whose top level is an object. A
// text here is *owned (cJSON_free it) for an object and the string's own otherwise. Returns NULL with *refusal the reply when it
// can't be; *text stays NULL for no data (absent or null).
static const char *send_data_json(const cJSON *data, char **owned, cJSON **refusal) {
    *owned = NULL;
    if (!data || cJSON_IsNull(data)) return NULL;
    const char *text;
    if (cJSON_IsObject(data)) {
        *owned = cJSON_PrintUnformatted(data);
        if (!*owned) {
            *refusal = error_result("out_of_memory", "out of memory: no room for the data");
            return NULL;
        }
        text = *owned;
    } else if (cJSON_IsString(data) && data->valuestring) {
        text = data->valuestring;
    } else {  // an array, a number, a boolean: not an object
        *refusal = error_result("invalid_params", SEND_DATA_INVALID);
        return NULL;
    }
    size_t len = strlen(text);
    char detail[96];  // why the wrapper refused it: the message is the same whichever way
    if (len > MUSE_APPS_SEND_DATA_MAX) {
        *refusal = error_result("invalid_params", SEND_DATA_LARGE);
    } else if (!muse_apps_json_object_valid(text, len, detail, sizeof(detail))) {
        *refusal = error_result("invalid_params", SEND_DATA_INVALID);
    }
    if (*refusal) {
        cJSON_free(*owned);
        *owned = NULL;
        return NULL;
    }
    return text;
}

// app.send: id and event required; data an object or JSON text (send_data_json); show a boolean. The job (muse_apps_send) starts
// the app if it isn't running and waits for the reply on the I/O worker, so this answers {"_async": true}.
static cJSON *send_command(cJSON *params, const char *request_id, noise_ctrl_session_generation_t session_generation) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0]) return error_result("missing_param", "id is required");
    char msg[128];
    if (!muse_apps_id_valid(id->valuestring, msg, sizeof(msg))) return error_result("invalid_params", msg);
    cJSON *event = cJSON_GetObjectItemCaseSensitive(params, "event");
    if (!cJSON_IsString(event) || !event->valuestring || !event->valuestring[0]) {
        return error_result("missing_param", "event is required");
    }
    if (strlen(event->valuestring) > MUSE_APPS_SEND_EVENT_MAX) {
        return error_result("invalid_params", "event must be 1 to 64 characters");
    }
    cJSON *show = cJSON_GetObjectItemCaseSensitive(params, "show");
    if (show && !cJSON_IsBool(show)) return error_result("invalid_params", "show must be true or false");
    cJSON *refusal = NULL;
    char *owned = NULL;
    const char *data = send_data_json(cJSON_GetObjectItemCaseSensitive(params, "data"), &owned, &refusal);
    if (refusal) return refusal;
    request_t *req = new_request(request_id, session_generation, &refusal);
    if (!req) {
        cJSON_free(owned);
        return refusal;
    }
    const char *code = "out_of_memory", *err = "failed to send the event";
    bool queued = muse_apps_send(id->valuestring, event->valuestring, data, cJSON_IsTrue(show), command_done, req, &code, &err);
    cJSON_free(owned);  // muse_apps_send copied what it needs
    if (!queued) {
        heap_caps_free(req);
        return error_result(code, err);
    }
    return async_reply();
}

// A synchronous action's answer (muse_apps_list, muse_apps_status), or the lack of memory for one.
static cJSON *answer(cJSON *result) {
    return result ? result : error_result("out_of_memory", "out of memory: no room for the answer");
}

// snapshot's scale: absent or null is 1; else the number 1 or 2 (2.0 is 2). 0 for anything else.
static int snapshot_scale(const cJSON *params) {
    const cJSON *scale = cJSON_GetObjectItemCaseSensitive(params, "scale");
    if (!scale || cJSON_IsNull(scale)) return 1;
    if (!cJSON_IsNumber(scale)) return 0;
    return scale->valuedouble == 1 ? 1 : scale->valuedouble == 2 ? 2 : 0;
}

// app.control: action is one of ACTIONS, as typed; id an app id, for every action but list; snapshot's scale 1 or 2.
static cJSON *control_command(cJSON *params, const char *request_id,
                              noise_ctrl_session_generation_t session_generation) {
    cJSON *action = cJSON_GetObjectItemCaseSensitive(params, "action");
    if (!action || cJSON_IsNull(action)) {
        return error_result("missing_param", "action is required: one of " ACTIONS_TEXT);
    }
    int act = -1;
    for (int i = 0; cJSON_IsString(action) && action->valuestring && i < (int)(sizeof(ACTIONS) / sizeof(ACTIONS[0])); i++) {
        if (strcmp(action->valuestring, ACTIONS[i]) == 0) act = i;
    }
    if (act < 0) return error_result("invalid_params", "action must be one of " ACTIONS_TEXT);
    if (act == ACT_LIST) return answer(muse_apps_list());

    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id || cJSON_IsNull(id) || (cJSON_IsString(id) && (!id->valuestring || !id->valuestring[0]))) {
        char msg[48];
        snprintf(msg, sizeof(msg), "id is required for %s", ACTIONS[act]);
        return error_result("missing_param", msg);
    }
    if (!cJSON_IsString(id)) return error_result("invalid_params", "id must be a string");
    char msg[128];
    if (!muse_apps_id_valid(id->valuestring, msg, sizeof(msg))) return error_result("invalid_params", msg);
    if (act == ACT_STATUS || act == ACT_LOGS) return answer(muse_apps_status(id->valuestring, act == ACT_LOGS));
    int scale = act == ACT_SNAPSHOT ? snapshot_scale(params) : 1;
    if (!scale) return error_result("invalid_params", MUSE_APPS_SNAPSHOT_BAD_SCALE);

    cJSON *refusal = NULL;
    request_t *req = new_request(request_id, session_generation, &refusal);
    if (!req) return refusal;
    const char *code = "out_of_memory", *err = "failed to queue the action";
    bool queued = act == ACT_LAUNCH   ? muse_apps_launch_async(id->valuestring, command_done, req, &code, &err)
                  : act == ACT_STOP   ? muse_apps_stop(id->valuestring, command_done, req, &code, &err)
                  : act == ACT_REMOVE ? muse_apps_remove(id->valuestring, command_done, req, &code, &err)
                                      : muse_apps_snapshot(id->valuestring, scale, command_done, req, &code, &err);
    if (!queued) {
        heap_caps_free(req);
        return error_result(code, err);
    }
    return async_reply();
}

bool apps_link_handles(const char *command) {
    return command && (strcmp(command, "app.install") == 0 || strcmp(command, "app.api") == 0 ||
                       strcmp(command, "app.control") == 0 || strcmp(command, "app.send") == 0);
}

cJSON *apps_link_handle(const char *command, cJSON *params, const char *request_id,
                        noise_ctrl_session_generation_t session_generation) {
    if (command && strcmp(command, "app.install") == 0) {
        return install_command(params, request_id, session_generation);
    }
    if (command && strcmp(command, "app.api") == 0) {
        return api_command(request_id, session_generation);
    }
    if (command && strcmp(command, "app.control") == 0) {
        return control_command(params, request_id, session_generation);
    }
    if (command && strcmp(command, "app.send") == 0) {
        return send_command(params, request_id, session_generation);
    }
    return error_result("unsupported", "unknown app command");
}
