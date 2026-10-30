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

/*
 * The D14 results of the apps' Link commands, in one place for every job that makes them (apps_control.c, apps_snapshot.c and
 * apps_install.c): their shapes, the store's refusals and the VM ops' codes as D14's. Any task; the results are cJSON's, made with
 * its hooks.
 */

#include <stdbool.h>

#include "cJSON.h"

#define APPS_ERR_NOT_FOUND "not_found"            /* an id the store doesn't have (MUSE_APPS_NOT_FOUND's words) */
#define APPS_STORE_NOT_READY "storage not ready"  /* apps_store.c's words, for a store that isn't mounted yet */
#define APPS_NO_ROOM_RESULT "out of memory: no room for the result"

/* {"ok":false,"error":{"code","message"}}; NULL without memory. */
cJSON *apps_result_failure(const char *code, const char *message);

/* {"ok":true,"payload":payload}, payload taken; NULL (payload freed) without memory. */
cJSON *apps_result_success(cJSON *payload);

/* not_found, in MUSE_APPS_NOT_FOUND's words for id. NULL without memory. */
cJSON *apps_result_not_found(const char *id);

/* True when the store is mounted and has id; otherwise *r is the refusal: unavailable APPS_STORE_NOT_READY, or not_found (NULL
 * without memory). The store's calls don't block, so any task. */
bool apps_result_installed(const char *id, cJSON **r);

/* D14's code for a VM op's failure (muse_apps_vm_ops_t's MUSE_APPS_VM_* codes): busy and out_of_memory as they are, unavailable for
 * anything else (a refusal, a dropped session, the deadline). A NULL code is unavailable. */
const char *apps_result_vm_code(const char *vm_code);
