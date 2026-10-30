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

#include "apps_result.h"

#include <stdio.h>
#include <string.h>

#include "apps_install.h"
#include "apps_pkg.h"
#include "apps_store.h"
#include "muse_apps.h"

cJSON *apps_result_failure(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON *e = NULL;
    bool ok = r != NULL && cJSON_AddFalseToObject(r, "ok") != NULL && (e = cJSON_AddObjectToObject(r, "error")) != NULL &&
              cJSON_AddStringToObject(e, "code", code) != NULL && cJSON_AddStringToObject(e, "message", message) != NULL;
    if (!ok) {
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

cJSON *apps_result_success(cJSON *payload) {
    cJSON *r = payload != NULL ? cJSON_CreateObject() : NULL;
    if (r != NULL && cJSON_AddTrueToObject(r, "ok") != NULL && cJSON_AddItemToObject(r, "payload", payload)) {
        return r;
    }
    cJSON_Delete(r);
    cJSON_Delete(payload);
    return NULL;
}

cJSON *apps_result_not_found(const char *id) {
    char msg[sizeof MUSE_APPS_NOT_FOUND + APPS_ID_MAX];
    snprintf(msg, sizeof msg, MUSE_APPS_NOT_FOUND, id);
    return apps_result_failure(APPS_ERR_NOT_FOUND, msg);
}

bool apps_result_installed(const char *id, cJSON **r) {
    if (apps_store_mode() == APPS_STORAGE_NONE) {
        *r = apps_result_failure(APPS_ERR_UNAVAILABLE, APPS_STORE_NOT_READY);
        return false;
    }
    if (!apps_store_has(id)) {
        *r = apps_result_not_found(id);
        return false;
    }
    return true;
}

const char *apps_result_vm_code(const char *vm_code) {
    if (vm_code != NULL && strcmp(vm_code, MUSE_APPS_VM_BUSY) == 0) {
        return APPS_ERR_BUSY;
    }
    if (vm_code != NULL && strcmp(vm_code, MUSE_APPS_VM_OUT_OF_MEMORY) == 0) {
        return APPS_ERR_OUT_OF_MEMORY;
    }
    return APPS_ERR_UNAVAILABLE;
}
