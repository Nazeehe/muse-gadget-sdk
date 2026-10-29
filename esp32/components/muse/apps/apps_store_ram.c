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

#include "apps_store.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"

/*
 * The store of an app (apps_store.h), in RAM: until Task 20 gives the values flash, a list of nodes in PSRAM, one for each app and at
 * most STORE_APPS, each holding the app's one saved object. Only the I/O worker touches the list (X11: Task 20's flash is the worker's,
 * reads included), so it takes no lock: the app task's calls queue ops (apps_io_submit_op), which the worker runs in the order they
 * were queued, so a load never reads before a save that came ahead of it has been written (a restart's load follows the stop's save).
 *
 * A save makes its node on the calling task, with a copy of the bytes, so that a want of memory is the caller's to try again, and the
 * op links it in place of the app's old one: nothing in it can fail. A save that the worker refuses (its ring of ops is full, or there is
 * no worker and no room to make one) isn't lost: its node waits in a PSRAM slot of the app's own, s_pending, which is the app task's alone,
 * and the app task's rounds offer it again (apps_store_tick) until the worker takes it. A load reads what waits there first: it is newer
 * than anything the worker has. A load makes a request, which its op fills with a copy of the object, and polls for the answer, at most
 * APPS_STORE_WAIT_MS. Who owns the request is decided by one atomic: the op marks it done, the caller that gives up marks it abandoned,
 * whichever finds the other's mark frees it, so a worker that answers late finds nobody waiting, and frees the request and its answer.
 * The request is a small block of internal RAM, where the house rule has an atomic: no read-modify-write is made on PSRAM.
 */

static const char *TAG = "muse_apps";

#define STORE_APPS 16                   /* apps whose values are kept; a new one past that drops the oldest saved */
#define STORE_OBJECT_MAX APPS_STORE_MAX_BYTES  /* the runtime saves no more, and reads back no more (apps_rt.h): the map never holds a runaway */
#define STORE_POLL_MS 10                /* how often a load looks for its answer: at least a tick (a build at 100 Hz has none shorter) */

typedef struct store_node {
    struct store_node *next;
    uint32_t seq;                       /* the order of the saves, for the oldest */
    size_t len;
    char app[APPS_ID_MAX + 1];
    char json[];                        /* len bytes, then a NUL */
} store_node_t;

static store_node_t *s_store;           /* the worker's alone */
static unsigned s_store_n;
static uint32_t s_store_seq;

enum { LOAD_WAITING, LOAD_DONE, LOAD_ABANDONED };

typedef struct {
    atomic_int state;
    char app[APPS_ID_MAX + 1];
    char *json;                         /* the answer, a block of the apps' heap, set before DONE; NULL for no memory */
    size_t len;
} store_load_t;

/* The op of a save, on the worker: n goes in place of the app's node, and when there is no room for a new app the oldest saved goes. */
static void store_put_op(void *arg) {
    store_node_t *n = arg;
    for (store_node_t **at = &s_store; *at != NULL; at = &(*at)->next) {
        if (strcmp((*at)->app, n->app) == 0) {
            store_node_t *old = *at;
            *at = old->next;
            s_store_n--;
            apps_port_free(old);
            break;
        }
    }
    if (s_store_n >= STORE_APPS) {
        store_node_t **oldest = &s_store;
        for (store_node_t **at = &s_store; *at != NULL; at = &(*at)->next) {
            if ((*at)->seq < (*oldest)->seq) {
                oldest = at;
            }
        }
        store_node_t *gone = *oldest;
        *oldest = gone->next;
        s_store_n--;
        ESP_LOGW(TAG, "store: dropped the values of %s to make room for another app's", gone->app);
        apps_port_free(gone);
    }
    n->seq = ++s_store_seq;
    n->next = s_store;
    s_store = n;
    s_store_n++;
}

/* The saves the worker hasn't taken: a slot for each app, as many as the map has (STORE_APPS), the app task's alone (a save and a load are
 * its calls, and so is the tick that offers them again). A slot's node is no longer ours once an op has it: nothing touches it then but a
 * clear of the slot. s_retry_at is when the tick offers them again, APPS_STORE_RETRY_MS after the last refusal. */
static store_node_t *s_pending[STORE_APPS];
static int64_t s_retry_at;

/* The op of a load, on the worker: a copy of the app's object, or "{}" when it has none. */
static void store_get_op(void *arg) {
    store_load_t *r = arg;
    const store_node_t *n = NULL;
    for (const store_node_t *at = s_store; at != NULL && n == NULL; at = at->next) {
        n = strcmp(at->app, r->app) == 0 ? at : NULL;
    }
    size_t len = n != NULL ? n->len : sizeof "{}" - 1;
    char *copy = apps_port_realloc(NULL, len + 1);
    if (copy != NULL) {
        memcpy(copy, n != NULL ? n->json : "{}", len);
        copy[len] = '\0';
    }
    r->json = copy;
    r->len = copy != NULL ? len : 0;
    int waiting = LOAD_WAITING;
    if (!atomic_compare_exchange_strong(&r->state, &waiting, LOAD_DONE)) {
        apps_port_free(copy);  /* abandoned: the caller has gone, and this was the last to look at the request */
        heap_caps_free(r);
    }
}

char *apps_store_load(const char *app, size_t *len) {
    *len = 0;
    if (strlen(app) > APPS_ID_MAX) {
        return NULL;  /* an id the packages can't have */
    }
    for (size_t i = 0; i < STORE_APPS; i++) {
        const store_node_t *kept = s_pending[i];
        if (kept != NULL && strcmp(kept->app, app) == 0) {
            /* A save the worker hasn't taken: newer than what it has, and needing no worker. */
            char *copy = apps_port_realloc(NULL, kept->len + 1);
            if (copy != NULL) {
                memcpy(copy, kept->json, kept->len);
                copy[kept->len] = '\0';
                *len = kept->len;
            }
            return copy;
        }
    }
    store_load_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (r == NULL) {
        return NULL;
    }
    atomic_init(&r->state, LOAD_WAITING);
    snprintf(r->app, sizeof r->app, "%s", app);
    r->json = NULL;
    r->len = 0;
    if (!apps_io_submit_op(store_get_op, r)) {
        heap_caps_free(r);  /* refused: the worker never saw it (apps_io_submit_op says why in its log line) */
        return NULL;
    }
    int64_t until = apps_port_mono_ms() + APPS_STORE_WAIT_MS;
    const TickType_t poll_ticks = pdMS_TO_TICKS(STORE_POLL_MS) > 0 ? pdMS_TO_TICKS(STORE_POLL_MS) : 1;
    while (atomic_load(&r->state) != LOAD_DONE) {
        if (apps_port_mono_ms() >= until) {
            int waiting = LOAD_WAITING;
            if (atomic_compare_exchange_strong(&r->state, &waiting, LOAD_ABANDONED)) {
                ESP_LOGW(TAG, "store: no answer from the I/O worker for %s within %d ms: launch refused", app, APPS_STORE_WAIT_MS);
                return NULL;  /* the worker frees the request when it comes to it */
            }
            break;  /* it answered as the wait ran out */
        }
        vTaskDelay(poll_ticks);
    }
    char *json = r->json;
    *len = r->len;
    heap_caps_free(r);
    return json;
}

/* Drops the save that is kept for app, if there is one: a newer one has come. */
static void pending_drop(const char *app) {
    for (size_t i = 0; i < STORE_APPS; i++) {
        if (s_pending[i] != NULL && strcmp(s_pending[i]->app, app) == 0) {
            apps_port_free(s_pending[i]);
            s_pending[i] = NULL;
        }
    }
}

void apps_store_tick(int64_t now_ms) {
    if (now_ms < s_retry_at) {
        return;
    }
    for (size_t i = 0; i < STORE_APPS; i++) {
        store_node_t *n = s_pending[i];
        if (n == NULL) {
            continue;
        }
        if (!apps_io_submit_op(store_put_op, n)) {
            s_retry_at = now_ms + APPS_STORE_RETRY_MS;  /* the worker still can't: the same again, a little later (it said why in its log line) */
            return;
        }
        s_pending[i] = NULL;  /* the worker's: n isn't touched again */
    }
}

bool apps_store_save(const char *app, const char *json, size_t len) {
    if (strlen(app) > APPS_ID_MAX || len > STORE_OBJECT_MAX) {
        ESP_LOGW(TAG, "store: refused %u bytes of values for %s: more than the runtime reads back", (unsigned)len, app);
        return false;
    }
    store_node_t *n = apps_port_realloc(NULL, sizeof *n + len + 1);
    if (n == NULL) {
        return false;  /* no memory for the copy: the caller asks again, and what was kept for the app stays */
    }
    n->next = NULL;
    n->seq = 0;
    n->len = len;
    snprintf(n->app, sizeof n->app, "%s", app);
    memcpy(n->json, json, len);
    n->json[len] = '\0';
    pending_drop(app);  /* this is the newer: a save that waited for the worker is of no use to anyone now */
    if (apps_io_submit_op(store_put_op, n)) {
        return true;
    }
    for (size_t i = 0; i < STORE_APPS; i++) {
        if (s_pending[i] == NULL) {
            s_pending[i] = n;  /* kept, for the tick to offer again: the save is the port's now */
            s_retry_at = apps_port_mono_ms() + APPS_STORE_RETRY_MS;
            return true;
        }
    }
    apps_port_free(n);  /* a slot for each of the 16 apps the map holds, and all are taken */
    return false;
}
