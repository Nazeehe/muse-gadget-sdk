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
 * apps_port.h on the gadget. The heap is PSRAM; the clock is esp_timer's; an app's log line goes to the USB console
 * as "@app.log <id> <line>"; the UI lock is a static FreeRTOS mutex that muse_apps_start() makes (apps_port_init).
 *
 * The UI lock keeps the host runner's contract (tools/muse/apphost/apphost_port.c): it is never taken twice by one
 * task, given back only by its holder, and nothing uses the heap while it is held. The host aborts on a breach; here a
 * breach is logged once and carried past, never into a deadlock or one of FreeRTOS's asserts: a second take by the
 * holder is skipped, and so is a give by a task that doesn't hold it.
 */

#include "apps_port.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "muse_mem.h"

static const char *TAG = "muse_apps";

/* The wall clock counts once it reads 2025 or later: SNTP has set it (Task 16 asks muse_time_valid()). */
#define EPOCH_2025_S 1735689600LL

static StaticSemaphore_t s_ui_lock_buf;
static SemaphoreHandle_t s_ui_lock;
static TaskHandle_t volatile s_ui_holder;  /* written by the holder alone, under the lock */
static volatile bool s_breach_logged;

static void breach(const char *what) {
    if (!s_breach_logged) {
        s_breach_logged = true;
        ESP_LOGE(TAG, "UI lock: %s (logged once)", what);
    }
}

static bool held_by_me(void) {
    return s_ui_holder != NULL && s_ui_holder == xTaskGetCurrentTaskHandle();
}

void apps_port_init(void) {
    if (s_ui_lock == NULL) {
        s_ui_lock = xSemaphoreCreateMutexStatic(&s_ui_lock_buf);
    }
}

int64_t apps_port_mono_ms(void) {
    return esp_timer_get_time() / 1000;
}

bool apps_port_epoch_ms(int64_t *out) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < EPOCH_2025_S) {
        return false;
    }
    *out = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return true;
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (held_by_me()) {
        breach("the heap was used with the lock held");
    }
    return heap_caps_realloc(ptr, n, MUSE_BIG_CAPS);
}

void apps_port_free(void *ptr) {
    if (ptr != NULL && held_by_me()) {
        breach("a block was freed with the lock held");
    }
    heap_caps_free(ptr);
}

/* One printf is one line on the console: newlib locks stdout for it, so another task's line can't land inside it. A
 * console nobody reads costs a bounded wait: the USB Serial/JTAG VFS waits at most 50 ms, then drops what it can't
 * send. */
void apps_port_log(const char *app_id, const char *line) {
    printf("@app.log %s %s\n", app_id, line);
    fflush(stdout);
    ESP_LOGD(TAG, "%s: %s", app_id, line);
}

void apps_port_ui_lock(void) {
    if (held_by_me()) {
        breach("taken again by its holder");
        return;
    }
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_ui_holder = xTaskGetCurrentTaskHandle();
}

bool apps_port_ui_trylock(int ms) {
    if (held_by_me()) {
        breach("tried again by its holder");
        return false;
    }
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(ms)) != pdTRUE) {
        return false;
    }
    s_ui_holder = xTaskGetCurrentTaskHandle();
    return true;
}

void apps_port_ui_unlock(void) {
    if (!held_by_me()) {
        breach("given back by a task that doesn't hold it");
        return;
    }
    s_ui_holder = NULL;
    xSemaphoreGive(s_ui_lock);
}

/* PSA's SHA-256 (C21): IDF v6's mbedTLS is PSA, as main/link_pairing.c uses it. psa_crypto_init is cheap once it has
 * run. */
void apps_port_sha256(const void *data, size_t len, uint8_t out[32]) {
    static const uint8_t empty;
    size_t written = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, data != NULL ? data : &empty, len, out, 32, &written) != PSA_SUCCESS ||
        written != 32) {
        memset(out, 0, 32);
        ESP_LOGE(TAG, "sha256 failed");
    }
}
