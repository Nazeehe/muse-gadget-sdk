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

#pragma once

// Link's app.* commands (CONFIG_MUSE_APPS): how Muse puts its apps on the
// gadget. Muse writes an app's folder, workspace/apps/<id>/, on its VM and
// calls app.install; the gadget reads the files itself (vm_file, GET /fs/raw),
// then checks, compiles, stores and runs the app, and answers with one result
// (components/muse/muse_apps.h has the install, apps_install.h the result).
// Every caller of these sits inside #if CONFIG_MUSE_APPS.

#include <stdbool.h>

#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// Puts vm_file (vm_file.h) behind the apps' muse_apps_vm_ops_t. Once, as Muse
// starts (muse_glue_start).
void apps_link_init(void);

// Adds the app.* commands to link.register's commands_v2 object.
void apps_link_register(cJSON *commands);

// Whether command is one of these.
bool apps_link_handles(const char *command);

// on_ws_command() for these, on the Noise session's task: it never waits. An
// install is queued and answers {"_async": true}, and its result follows
// through noise_ctrl_send_command_result(), once; a refusal is the reply.
cJSON *apps_link_handle(const char *command, cJSON *params, const char *request_id,
                        noise_ctrl_session_generation_t session_generation);

#ifdef __cplusplus
}
#endif
