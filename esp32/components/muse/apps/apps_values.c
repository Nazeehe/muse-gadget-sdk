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

#include "apps_values.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "apps_alarm.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_store.h"

/*
 * An app's values (apps_values.h has the contract). Only the I/O worker touches the backend (X11: flash is the worker's, reads included),
 * so the app task's calls queue ops (apps_io_submit_op), which the worker runs in the order they were queued, so a load never reads before
 * a save that came ahead of it has been written (a restart's load follows the stop's save). Each op asks the store's mode which backend it
 * is: the mount fixed it before the worker's first op, for the boot.
 *
 * The RAM backend is a list of nodes in PSRAM, one for each app and at most APPS_VALUES_RAM_APPS, each holding the app's one saved object;
 * the worker's alone, so it takes no lock. The flash backend is apps_nvs (apps_store_save_values, apps_store_load_values).
 *
 * A save makes its node on the calling task, with a copy of the bytes, so that a want of memory is the caller's to try again, and the op
 * links it in place of the app's old one (RAM) or writes it and frees it (flash). A save that the worker refuses (its ring of ops is full,
 * or there is no worker and no room to make one) isn't lost: its node waits in a PSRAM slot of the app's own, s_pending, which is the app
 * task's alone, and the app task's rounds offer it again (apps_values_tick) until the worker takes it. A load reads what waits there first:
 * it is newer than anything the worker has. A load makes a request, which its op fills with a copy of the object, and polls for the
 * answer, at most APPS_STORE_WAIT_MS. Who owns the request is decided by one atomic: the op marks it done, the caller that gives up marks
 * it abandoned, whichever finds the other's mark frees it, so a worker that answers late finds nobody waiting, and frees the request and
 * its answer. The request is a small block of internal RAM, where the house rule has an atomic: no read-modify-write is made on PSRAM.
 *
 * A failure of the flash backend (apps_nvs full, a write error) is said once per streak: s_failure goes from OK to TELL with the app's id
 * in s_failed (the worker's write, published by the atomic), the app task takes it (TELL to TOLD) and says it, and the next save that
 * succeeds ends the streak (OK). WRITING keeps the app task off the id while the worker writes it.
 *
 * The alarms' records: s_marked, the apps whose alarms changed since their records were last brought in line, the app task's alone. A tick
 * APPS_VALUES_ALARM_DELAY_MS after the first mark reads each one's alarms and queues an op that hands them to apps_store_sync_alarms.
 */

static const char *TAG = "muse_apps";

#define STORE_OBJECT_MAX APPS_STORE_MAX_BYTES  /* the runtime saves no more, and reads back no more (apps_rt.h): the map never holds a runaway */
#define STORE_POLL_MS 10                /* how often a load looks for its answer: at least a tick (a build at 100 Hz has none shorter) */
#define MARKED_MAX APPS_ALARMS_MAX      /* apps whose alarms changed between two ticks: every app that has an alarm, at the most */

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

enum { FAILURE_OK, FAILURE_WRITING, FAILURE_TELL, FAILURE_TOLD };
static atomic_int s_failure;
static char s_failed[APPS_ID_MAX + 1];  /* the app of the failure to tell: written by the worker in WRITING, read by the app task in TELL */

/* A save the flash backend took or failed: the streak of failures, said once (see above). */
static void backend_said(const char *app, bool ok) {
    if (ok) {
        int told = FAILURE_TOLD, tell = FAILURE_TELL;
        if (!atomic_compare_exchange_strong(&s_failure, &told, FAILURE_OK)) {
            (void)atomic_compare_exchange_strong(&s_failure, &tell, FAILURE_OK);  /* gone before it was said: nothing to say */
        }
        return;
    }
    int none = FAILURE_OK;
    if (atomic_compare_exchange_strong(&s_failure, &none, FAILURE_WRITING)) {
        snprintf(s_failed, sizeof s_failed, "%s", app);
        atomic_store(&s_failure, FAILURE_TELL);
    }
}

/* The RAM map's save: n goes in place of the app's node, and when there is no room for a new app the oldest saved goes. */
static void ram_put(store_node_t *n) {
    for (store_node_t **at = &s_store; *at != NULL; at = &(*at)->next) {
        if (strcmp((*at)->app, n->app) == 0) {
            store_node_t *old = *at;
            *at = old->next;
            s_store_n--;
            apps_port_free(old);
            break;
        }
    }
    if (s_store_n >= APPS_VALUES_RAM_APPS) {
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

/* The op of a save, on the worker: the backend the store's mode says. */
static void store_put_op(void *arg) {
    store_node_t *n = arg;
    if (apps_store_mode() == APPS_STORAGE_FLASH) {
        bool ok = apps_store_save_values(n->app, n->json, n->len);
        if (!ok) {
            ESP_LOGW(TAG, "store: apps_nvs refused %u bytes of values for %s", (unsigned)n->len, n->app);
        }
        backend_said(n->app, ok);
        apps_port_free(n);
        return;
    }
    ram_put(n);
}

void apps_values_forget(const char *app) {
    for (store_node_t **at = &s_store; *at != NULL; at = &(*at)->next) {
        if (strcmp((*at)->app, app) == 0) {
            store_node_t *gone = *at;
            *at = gone->next;
            s_store_n--;
            apps_port_free(gone);
            return;
        }
    }
}

/* The saves the worker hasn't taken: a slot for each app, as many as the RAM map has (APPS_VALUES_RAM_APPS), the app task's alone (a save
 * and a load are its calls, and so is the tick that offers them again). A slot's node is no longer ours once an op has it: nothing touches
 * it then but a clear of the slot. s_retry_at is when the tick offers them again, APPS_VALUES_RETRY_MS after the last refusal. */
static store_node_t *s_pending[APPS_VALUES_RAM_APPS];
static int64_t s_retry_at;

/* A copy of json[0, len) with a NUL after it, from the apps' heap; NULL without memory. */
static char *copy_of(const char *json, size_t len) {
    char *copy = apps_port_realloc(NULL, len + 1);
    if (copy != NULL) {
        memcpy(copy, json, len);
        copy[len] = '\0';
    }
    return copy;
}

/* The op of a load, on the worker: a copy of the app's object, or "{}" when it has none. */
static void store_get_op(void *arg) {
    store_load_t *r = arg;
    char *copy = NULL;
    size_t len = 0;
    if (apps_store_mode() == APPS_STORAGE_FLASH) {
        copy = apps_store_load_values(r->app, &len);
    } else {
        const store_node_t *n = NULL;
        for (const store_node_t *at = s_store; at != NULL && n == NULL; at = at->next) {
            n = strcmp(at->app, r->app) == 0 ? at : NULL;
        }
        len = n != NULL ? n->len : sizeof "{}" - 1;
        copy = copy_of(n != NULL ? n->json : "{}", len);
    }
    r->json = copy;
    r->len = copy != NULL ? len : 0;
    int waiting = LOAD_WAITING;
    if (!atomic_compare_exchange_strong(&r->state, &waiting, LOAD_DONE)) {
        apps_port_free(copy);  /* abandoned: the caller has gone, and this was the last to look at the request */
        heap_caps_free(r);
    }
}

char *apps_values_load(const char *app, size_t *len) {
    *len = 0;
    if (strlen(app) > APPS_ID_MAX) {
        return NULL;  /* an id the packages can't have */
    }
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        const store_node_t *kept = s_pending[i];
        if (kept != NULL && strcmp(kept->app, app) == 0) {
            /* A save the worker hasn't taken: newer than what it has, and needing no worker. */
            char *copy = copy_of(kept->json, kept->len);
            *len = copy != NULL ? kept->len : 0;
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
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        if (s_pending[i] != NULL && strcmp(s_pending[i]->app, app) == 0) {
            apps_port_free(s_pending[i]);
            s_pending[i] = NULL;
        }
    }
}

/* ---- The alarms' records ---- */

typedef struct {
    char app[APPS_ID_MAX + 1];
    int n;
    bool epochs;                        /* the clock was valid: each alarm's epoch_ms is when it fires */
    apps_store_alarm_t alarms[APPS_ALARMS_PER_APP];
} sync_t;

static char s_marked[MARKED_MAX][APPS_ID_MAX + 1];
static int s_marked_n;
static int64_t s_marked_due;            /* when the marks are brought to the records */
static int64_t s_marked_retry_at;       /* a refusal's: not before then */

static void sync_op(void *arg) {
    sync_t *s = arg;
    apps_store_sync_alarms(s->app, s->alarms, s->n, s->epochs);
    apps_port_free(s);
}

void apps_values_alarms_changed(const char *app) {
    if (app == NULL || strlen(app) > APPS_ID_MAX) {
        return;
    }
    for (int i = 0; i < s_marked_n; i++) {
        if (strcmp(s_marked[i], app) == 0) {
            return;
        }
    }
    if (s_marked_n == MARKED_MAX) {
        ESP_LOGW(TAG, "alarm: the records of %s's alarms weren't updated: %d other apps' wait", app, MARKED_MAX);
        return;
    }
    if (s_marked_n == 0) {
        s_marked_due = apps_port_mono_ms() + APPS_VALUES_ALARM_DELAY_MS;
    }
    snprintf(s_marked[s_marked_n++], sizeof s_marked[0], "%s", app);
}

/* The marked apps' records, once due: an op each, with what the alarm table has of the app now. */
static void alarms_tick(int64_t now_ms) {
    if (s_marked_n == 0 || now_ms < s_marked_due || now_ms < s_marked_retry_at) {
        return;
    }
    apps_storage_t mode = apps_store_mode();
    if (mode == APPS_STORAGE_NONE) {
        return;  /* the mount hasn't run: they wait for it */
    }
    if (mode == APPS_STORAGE_RAM) {
        s_marked_n = 0;  /* nothing outlives a reset in RAM mode */
        return;
    }
    int64_t epoch_now = 0;
    bool epochs = apps_port_epoch_ms(&epoch_now);
    while (s_marked_n > 0) {
        const char *app = s_marked[s_marked_n - 1];
        sync_t *s = apps_port_realloc(NULL, sizeof *s);
        if (s == NULL) {
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;
            return;
        }
        memset(s, 0, sizeof *s);
        snprintf(s->app, sizeof s->app, "%s", app);
        apps_alarm_info_t info[APPS_ALARMS_PER_APP];
        s->n = apps_alarm_list_info(app, info);
        s->epochs = epochs;
        for (int i = 0; i < s->n; i++) {
            memcpy(s->alarms[i].name, info[i].name, strnlen(info[i].name, sizeof s->alarms[i].name - 1));  /* its NUL: the memset's */
            s->alarms[i].epoch_ms = epochs ? epoch_now + info[i].remaining_ms : 0;
            s->alarms[i].sound = info[i].sound;
            s->alarms[i].wake = info[i].wake;
        }
        if (!apps_io_submit_op(sync_op, s)) {
            apps_port_free(s);
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker can't now (it said why) */
            return;
        }
        s_marked_n--;
    }
}

void apps_values_tick(int64_t now_ms) {
    alarms_tick(now_ms);
    if (now_ms < s_retry_at) {
        return;
    }
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        store_node_t *n = s_pending[i];
        if (n == NULL) {
            continue;
        }
        if (!apps_io_submit_op(store_put_op, n)) {
            s_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker still can't: the same again, a little later (it said why in its log line) */
            return;
        }
        s_pending[i] = NULL;  /* the worker's: n isn't touched again */
    }
}

bool apps_values_save(const char *app, const char *json, size_t len) {
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
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        if (s_pending[i] == NULL) {
            s_pending[i] = n;  /* kept, for the tick to offer again: the save is the port's now */
            s_retry_at = apps_port_mono_ms() + APPS_VALUES_RETRY_MS;
            return true;
        }
    }
    apps_port_free(n);  /* a slot for each of the 16 apps the map holds, and all are taken */
    return false;
}

bool apps_values_take_failure(char *app, size_t cap) {
    int tell = FAILURE_TELL;
    if (!atomic_compare_exchange_strong(&s_failure, &tell, FAILURE_TOLD)) {
        return false;
    }
    snprintf(app, cap, "%s", s_failed);
    return true;
}
