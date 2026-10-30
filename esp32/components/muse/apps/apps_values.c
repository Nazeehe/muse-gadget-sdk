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

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
 * links it in place of the app's old one (RAM) or writes it and frees it (flash). A save that waits (the worker refused it: its ring of
 * ops is full, or there is no worker and no room to make one; or, on flash, the write budget can't take it yet) isn't lost: its node waits
 * in a PSRAM slot of the app's own, s_pending, which is the app task's alone, and the app task's rounds offer it again (apps_values_tick)
 * until the worker takes it. A save of an app whose save waits for the budget waits with it, in its place. A load reads what waits there
 * first: it is newer than anything the worker has. A load makes a request, which its op fills with a copy of the object, and polls for the
 * answer, at most APPS_STORE_WAIT_MS. Who owns the request is decided by one atomic: the op marks it done, the caller that gives up marks
 * it abandoned, whichever finds the other's mark frees it, so a worker that answers late finds nobody waiting, and frees the request and
 * its answer. The request is a small block of internal RAM, where the house rule has an atomic: no read-modify-write is made on PSRAM.
 *
 * s_lock (made by apps_values_init, before any task here runs) is held for the budget's arithmetic and for the failures' notices, both
 * written by one task and read by another: never across a wait. A failure of the flash backend (apps_nvs full, a write error) is said once
 * per streak of its app: s_failing, the worker's own, has the apps whose last save failed; the first failure of a streak puts the app in
 * s_failed, under the lock, which the app task takes from (apps_values_take_failure), and the next save of the app that succeeds ends its
 * streak. A budget that holds a save back is said once per streak too, all on the app task: s_slow has the apps whose saves it holds back,
 * s_slow_new the ones not said yet, and a save that fits at once ends the app's streak.
 *
 * The alarms' records (P1): s_marked, the apps whose records are due a sync, and s_recs, the records the mount handed over, each waiting
 * to be armed again, armed again, or done with (its name touched, its app gone, the table full): the app task's alone. A tick
 * APPS_VALUES_ALARM_DELAY_MS after the first mark reads each marked app's alarms and queues an op that hands them, with the names of its
 * records that nothing touched (to keep), to apps_store_sync_alarms. The re-arm and the app's own alarm calls are both the app task's, so
 * the app's choice always wins.
 *
 * The tables of apps (s_failing, s_failed, s_slow, s_slow_new, s_marked, s_recs, s_unclocked; about 3 KB) are in PSRAM (EXT_RAM_BSS_ATTR,
 * as the store's are): plain memory that only tasks touch, never an atomic, kept out of the internal RAM the worker's stack is made from.
 * The lock stays internal.
 */

static const char *TAG = "muse_apps";

#define STORE_OBJECT_MAX APPS_STORE_MAX_BYTES  /* the runtime saves no more, and reads back no more (apps_rt.h): the map never holds a runaway */
#define STORE_POLL_MS 10                /* how often a load looks for its answer: at least a tick (a build at 100 Hz has none shorter) */
#define MARKED_MAX APPS_ALARMS_MAX      /* apps whose alarms changed between two ticks: every app that has an alarm, at the most */
#define UNITS_PER_BYTE 86400000LL       /* a byte of the budget in units it fills by a whole number of each ms: a day's ms */
#define BUDGET_FULL ((int64_t)APPS_VALUES_BUDGET_BYTES * UNITS_PER_BYTE)

typedef struct store_node {
    struct store_node *next;
    uint32_t seq;                       /* the order of the saves, for the oldest */
    size_t len;
    bool paid;                          /* its cost is taken from the budget (or it needs none: RAM) */
    bool forced;                        /* an app's end's: written whatever the budget says */
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
    char *json;                         /* the answer, a block of the apps' heap, set before DONE; NULL for none */
    size_t len;
} store_load_t;

/* ---- The lock, the budget and the failures' notices ---- */

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static int64_t s_budget;                /* under s_lock: in units, below zero after an app's end overdrew it */
static int64_t s_budget_at;             /* when it was brought up to date (apps_port_mono_ms) */
static bool s_budget_ready;

static EXT_RAM_BSS_ATTR char s_failing[APPS_VALUES_RAM_APPS][APPS_ID_MAX + 1];  /* the worker's: apps whose last save the backend failed */
static int s_failing_n;
static EXT_RAM_BSS_ATTR char s_failed[APPS_VALUES_RAM_APPS][APPS_ID_MAX + 1];   /* under s_lock: apps whose streak began, not taken yet */
static int s_failed_n;

void apps_values_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

static void lock(void) {
    apps_values_init();  /* made by muse_apps_start before any task here: this is for the host tests that call in first */
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

/* What the budget holds now, filled for the time since it was last brought up to date: full at the first look (the boot). */
static int64_t budget_now_locked(void) {
    int64_t now = apps_port_mono_ms();
    if (!s_budget_ready) {
        s_budget = BUDGET_FULL;
        s_budget_at = now;
        s_budget_ready = true;
    }
    if (now > s_budget_at) {
        int64_t ms = now - s_budget_at;
        int64_t gain = ms > 2 * UNITS_PER_BYTE ? 2 * BUDGET_FULL : ms * APPS_VALUES_BUDGET_PER_DAY;  /* PER_DAY units a ms; no overflow */
        s_budget = s_budget + gain > BUDGET_FULL ? BUDGET_FULL : s_budget + gain;
        s_budget_at = now;
    }
    return s_budget;
}

bool apps_values_budget_take(uint32_t bytes, bool force) {
    int64_t need = (int64_t)bytes * UNITS_PER_BYTE;
    lock();
    int64_t held = budget_now_locked();  /* brought up to date first, even for a forced take: its debt is charged to the budget of now */
    bool fits = force || held >= need;
    if (fits) {
        s_budget -= need;
    }
    unlock();
    return fits;
}

int64_t apps_values_budget_wait_ms(uint32_t bytes) {
    int64_t need = (int64_t)bytes * UNITS_PER_BYTE;
    lock();
    int64_t short_by = need - budget_now_locked();
    unlock();
    return short_by <= 0 ? 0 : (short_by + APPS_VALUES_BUDGET_PER_DAY - 1) / APPS_VALUES_BUDGET_PER_DAY;
}

/* A save the flash backend took or failed (the worker's): the app's streak of failures, said once (see above). */
static void backend_said(const char *app, bool ok) {
    int at = -1;
    for (int i = 0; i < s_failing_n && at < 0; i++) {
        at = strcmp(s_failing[i], app) == 0 ? i : -1;
    }
    if (ok) {
        if (at >= 0) {
            memcpy(s_failing[at], s_failing[--s_failing_n], sizeof s_failing[0]);  /* its streak is over */
        }
        return;
    }
    if (at >= 0) {
        return;  /* its streak goes on: said already */
    }
    if (s_failing_n < APPS_VALUES_RAM_APPS) {
        snprintf(s_failing[s_failing_n++], sizeof s_failing[0], "%s", app);  /* more apps failing than that: each failure is said */
    }
    lock();
    bool queued = false;
    for (int i = 0; i < s_failed_n && !queued; i++) {
        queued = strcmp(s_failed[i], app) == 0;
    }
    if (!queued && s_failed_n < APPS_VALUES_RAM_APPS) {
        snprintf(s_failed[s_failed_n++], sizeof s_failed[0], "%s", app);
    }
    unlock();
}

bool apps_values_take_failure(char *app, size_t cap) {
    lock();
    bool got = s_failed_n > 0;
    if (got) {
        snprintf(app, cap, "%s", s_failed[0]);
        memmove(&s_failed[0], &s_failed[1], (size_t)(s_failed_n - 1) * sizeof s_failed[0]);
        s_failed_n--;
    }
    unlock();
    return got;
}

/* ---- The RAM map, and the ops ---- */

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

/* ---- The saves that wait (the app task's) ---- */

/* The saves the worker hasn't taken: a slot for each app, as many as the RAM map has (APPS_VALUES_RAM_APPS), the app task's alone (a save
 * and a load are its calls, and so is the tick that offers them again). A slot's node is no longer ours once an op has it: nothing touches
 * it then but a clear of the slot. s_retry_at is when the tick offers them again, APPS_VALUES_RETRY_MS after the worker's last refusal. */
static store_node_t *s_pending[APPS_VALUES_RAM_APPS];
static int64_t s_retry_at;

static EXT_RAM_BSS_ATTR char s_slow[APPS_VALUES_RAM_APPS][APPS_ID_MAX + 1];     /* apps whose saves the budget holds back: a streak each */
static int s_slow_n;
static EXT_RAM_BSS_ATTR char s_slow_new[APPS_VALUES_RAM_APPS][APPS_ID_MAX + 1]; /* those not said yet (apps_values_take_slowdown) */
static int s_slow_new_n;

/* A copy of json[0, len) with a NUL after it, from the apps' heap; NULL without memory. */
static char *copy_of(const char *json, size_t len) {
    char *copy = apps_port_realloc(NULL, len + 1);
    if (copy != NULL) {
        memcpy(copy, json, len);
        copy[len] = '\0';
    }
    return copy;
}

/* What writing n costs the budget: its blob (the id, a NUL, the object) and its entry; nothing for "{}", which erases the key. */
static uint32_t node_cost(const store_node_t *n) {
    bool empty = n->len == 2 && memcmp(n->json, "{}", 2) == 0;
    return empty ? 0 : (uint32_t)(strlen(n->app) + 1 + n->len + APPS_VALUES_ENTRY_COST);
}

static int slow_find(const char *app) {
    for (int i = 0; i < s_slow_n; i++) {
        if (strcmp(s_slow[i], app) == 0) {
            return i;
        }
    }
    return -1;
}

/* The budget held a save of app back: a streak begins, to be said once, unless one is on. */
static void slow_begin(const char *app) {
    if (slow_find(app) >= 0 || s_slow_n == APPS_VALUES_RAM_APPS) {
        return;
    }
    snprintf(s_slow[s_slow_n++], sizeof s_slow[0], "%s", app);
    if (s_slow_new_n < APPS_VALUES_RAM_APPS) {
        snprintf(s_slow_new[s_slow_new_n++], sizeof s_slow_new[0], "%s", app);
    }
}

/* A save of app fit at once: its streak is over. */
static void slow_end(const char *app) {
    int at = slow_find(app);
    if (at >= 0) {
        memcpy(s_slow[at], s_slow[--s_slow_n], sizeof s_slow[0]);
    }
}

bool apps_values_take_slowdown(char *app, size_t cap) {
    if (s_slow_new_n == 0) {
        return false;
    }
    snprintf(app, cap, "%s", s_slow_new[0]);
    memmove(&s_slow_new[0], &s_slow_new[1], (size_t)(s_slow_new_n - 1) * sizeof s_slow_new[0]);
    s_slow_new_n--;
    return true;
}

/* The slot of the save of app that waits, or -1. */
static int pending_of(const char *app) {
    for (int i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        if (s_pending[i] != NULL && strcmp(s_pending[i]->app, app) == 0) {
            return i;
        }
    }
    return -1;
}

/* n waits in a free slot: refused is the worker's refusal (the tick waits APPS_VALUES_RETRY_MS), else the budget's. False, with n freed,
 * when every slot holds another app's save. */
static bool keep(store_node_t *n, bool refused) {
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        if (s_pending[i] == NULL) {
            s_pending[i] = n;  /* kept, for the tick to offer again: the save is the port's now */
            if (refused) {
                s_retry_at = apps_port_mono_ms() + APPS_VALUES_RETRY_MS;
            }
            return true;
        }
    }
    apps_port_free(n);  /* a slot for each of the 16 apps the map holds, and all are taken */
    return false;
}

/* The op of a load, on the worker: a copy of the app's object, or "{}" when it has none (NULL when the backend can't read it now). */
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
    int at = pending_of(app);
    if (at >= 0) {
        /* A save the worker hasn't taken: newer than what it has, and needing no worker. */
        const store_node_t *kept = s_pending[at];
        char *copy = copy_of(kept->json, kept->len);
        *len = copy != NULL ? kept->len : 0;
        return copy;
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
    n->paid = n->forced = false;
    snprintf(n->app, sizeof n->app, "%s", app);
    memcpy(n->json, json, len);
    n->json[len] = '\0';
    bool flash = apps_store_mode() == APPS_STORAGE_FLASH;
    int at = pending_of(app);
    bool held_back = flash && at >= 0 && !s_pending[at]->paid && !s_pending[at]->forced;
    if (at >= 0) {
        apps_port_free(s_pending[at]);  /* this is the newer: a save that waited is of no use to anyone now */
        s_pending[at] = NULL;
    }
    uint32_t cost = node_cost(n);
    if (!flash || cost == 0) {
        n->paid = true;
    } else if (!held_back && apps_values_budget_take(cost, false)) {
        n->paid = true;
        slow_end(app);
    } else {
        slow_begin(app);  /* the budget holds it back: it waits for the tick, in the place of the app's save that waited */
        return keep(n, false);
    }
    if (apps_io_submit_op(store_put_op, n)) {
        return true;
    }
    return keep(n, true);
}

void apps_values_flush(const char *app) {
    int at = app != NULL ? pending_of(app) : -1;
    if (at < 0) {
        return;
    }
    store_node_t *n = s_pending[at];
    if (!n->paid) {
        (void)apps_values_budget_take(node_cost(n), true);  /* an app's end writes over the budget, which fills from below zero */
        n->paid = true;
    }
    n->forced = true;
    if (apps_io_submit_op(store_put_op, n)) {
        s_pending[at] = NULL;  /* the worker's: n isn't touched again */
    }  /* else the worker refused it: the tick offers it again, paid */
}

/* ---- The alarms' records ---- */

typedef struct {
    char app[APPS_ID_MAX + 1];
    int n, n_keep;
    bool epochs;                        /* the clock was valid: each alarm's epoch_ms is when it fires */
    apps_store_alarm_t alarms[APPS_ALARMS_PER_APP];
    char keep[APPS_STORE_RECORDS_MAX][APPS_ALARM_NAME_MAX + 1];
} sync_t;

enum { REC_WAITING, REC_ARMED, REC_DONE };

typedef struct {
    apps_store_record_t r;
    uint8_t state;                      /* waiting to be armed again, armed again, or done with (touched, its app gone, refused) */
} handed_t;

static EXT_RAM_BSS_ATTR char s_marked[MARKED_MAX][APPS_ID_MAX + 1];
static int s_marked_n;
static int64_t s_marked_due;            /* when the marks are brought to the records */
static int64_t s_marked_retry_at;       /* a refusal's, or the budget's: not before then */

static EXT_RAM_BSS_ATTR handed_t s_recs[APPS_STORE_RECORDS_MAX];
static int s_recs_n;
static bool s_recs_taken;
static int64_t s_rearm_at;              /* a re-arm that got no memory: not before then */
static bool s_clock_seen;
/* Apps synced with no clock while they had alarms: synced again when it comes. */
static EXT_RAM_BSS_ATTR char s_unclocked[MARKED_MAX][APPS_ID_MAX + 1];
static int s_unclocked_n;

static void sync_op(void *arg) {
    sync_t *s = arg;
    apps_store_sync_alarms(s->app, s->alarms, s->n, s->keep, s->n_keep, s->epochs);
    apps_port_free(s);
}

/* app's records are due a sync, APPS_VALUES_ALARM_DELAY_MS after the first mark of a batch. */
static void mark_app(const char *app) {
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

/* The records the mount read, taken once it has run (RAM mode has none). */
static void records_take(void) {
    if (s_recs_taken || apps_store_mode() == APPS_STORAGE_NONE) {
        return;
    }
    s_recs_taken = true;
    apps_store_record_t got[APPS_STORE_RECORDS_MAX];
    s_recs_n = apps_store_take_records(got, APPS_STORE_RECORDS_MAX);
    for (int i = 0; i < s_recs_n; i++) {
        s_recs[i].r = got[i];
        s_recs[i].state = REC_WAITING;
    }
    int64_t epoch;
    if (s_recs_n > 0 && !apps_port_epoch_ms(&epoch)) {
        ESP_LOGI(TAG, "store: %d alarm records wait for the clock", s_recs_n);
    }
}

void apps_values_alarm_touched(const char *app, const char *name) {
    if (app == NULL || strlen(app) > APPS_ID_MAX) {
        return;
    }
    records_take();
    for (int i = 0; i < s_recs_n; i++) {
        handed_t *h = &s_recs[i];
        if (h->state != REC_DONE && strcmp(h->r.id, app) == 0 && (name == NULL || strcmp(h->r.name, name) == 0)) {
            h->state = REC_DONE;  /* the app's, or the system's, choice: its record follows the table from now on */
        }
    }
    mark_app(app);
}

/* The handed-over records nothing touched arm again, for the time left (P1): the clock is valid. */
static void rearm(int64_t now_ms, int64_t epoch_now) {
    if (now_ms < s_rearm_at) {
        return;
    }
    for (int i = 0; i < s_recs_n; i++) {
        handed_t *h = &s_recs[i];
        if (h->state != REC_WAITING) {
            continue;
        }
        if (!apps_store_has(h->r.id)) {
            /* Removed since the mount, its records with it: the mount hands over no record when it couldn't read the list (S). */
            h->state = REC_DONE;
            continue;
        }
        int64_t ms = h->r.epoch_ms - epoch_now;  /* both are times of this century (apps_store.c checks a record's) */
        ms = ms < 1 ? 1 : ms > APPS_ALARM_MAX_MS ? APPS_ALARM_MAX_MS : ms;
        apps_alarm_result_t got = apps_alarm_arm(h->r.id, h->r.name, ms, h->r.sound, h->r.wake);
        if (got == APPS_ALARM_ARMED) {
            h->state = REC_ARMED;
            ESP_LOGI(TAG, "store: alarm %s/%s armed again: %lld ms left", h->r.id, h->r.name, (long long)ms);
        } else if (got == APPS_ALARM_NO_MEMORY) {
            s_rearm_at = now_ms + APPS_VALUES_REARM_RETRY_MS;
            ESP_LOGW(TAG, "store: alarm %s/%s: no memory to arm it again; it is tried again in %d s", h->r.id, h->r.name,
                     APPS_VALUES_REARM_RETRY_MS / 1000);
            return;
        } else {
            h->state = REC_DONE;
            mark_app(h->r.id);  /* its record goes at the sync: neither kept nor in the table */
            ESP_LOGW(TAG, "store: alarm %s/%s couldn't be armed again (the gadget has no slot for it): dropped", h->r.id, h->r.name);
        }
    }
}

static bool in_keep(const sync_t *s, const char *name) {
    for (int i = 0; i < s->n_keep; i++) {
        if (strcmp(s->keep[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* The marked apps' records, once due: an op each, with what the alarm table has of the app now and the names to keep. */
static void alarms_tick(int64_t now_ms, bool clock, int64_t epoch_now) {
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
    while (s_marked_n > 0) {
        const char *app = s_marked[s_marked_n - 1];
        sync_t *s = apps_port_realloc(NULL, sizeof *s);
        if (s == NULL) {
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;
            return;
        }
        memset(s, 0, sizeof *s);
        snprintf(s->app, sizeof s->app, "%s", app);
        for (int i = 0; i < s_recs_n && s->n_keep < APPS_STORE_RECORDS_MAX; i++) {
            if (s_recs[i].state != REC_DONE && strcmp(s_recs[i].r.id, app) == 0) {
                memcpy(s->keep[s->n_keep++], s_recs[i].r.name, sizeof s->keep[0]);  /* the same size, NUL within */
            }
        }
        apps_alarm_info_t info[APPS_ALARMS_PER_APP];
        int n = apps_alarm_list_info(app, info);
        for (int i = 0; i < n; i++) {
            if (in_keep(s, info[i].name)) {
                continue;  /* armed again from its record, which holds it already */
            }
            apps_store_alarm_t *a = &s->alarms[s->n++];
            memcpy(a->name, info[i].name, strnlen(info[i].name, sizeof a->name - 1));  /* its NUL: the memset's */
            a->epoch_ms = clock ? epoch_now + info[i].remaining_ms : 0;
            a->sound = info[i].sound;
            a->wake = info[i].wake;
        }
        s->epochs = clock;
        uint32_t cost = clock ? (uint32_t)s->n * APPS_VALUES_RECORD_COST : 0;
        int64_t wait = cost > 0 ? apps_values_budget_wait_ms(cost) : 0;
        if (wait > 0) {
            apps_port_free(s);
            s_marked_retry_at = now_ms + wait;  /* P3: the records wait for the budget, as values do */
            return;
        }
        if (!clock && s->n > 0) {
            bool listed = false;
            for (int i = 0; i < s_unclocked_n && !listed; i++) {
                listed = strcmp(s_unclocked[i], app) == 0;
            }
            if (!listed && s_unclocked_n < MARKED_MAX) {
                snprintf(s_unclocked[s_unclocked_n++], sizeof s_unclocked[0], "%s", app);
            }
        }
        if (!apps_io_submit_op(sync_op, s)) {
            apps_port_free(s);
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker can't now (it said why) */
            return;
        }
        s_marked_n--;
    }
}

/* The records' share of a round: the mount's records taken, the clock's coming (the apps synced without it are marked), the re-arm, and
 * the syncs that are due. */
static void records_tick(int64_t now_ms) {
    records_take();
    int64_t epoch_now = 0;
    bool clock = apps_port_epoch_ms(&epoch_now);
    if (clock && !s_clock_seen) {
        s_clock_seen = true;
        for (int i = 0; i < s_unclocked_n; i++) {
            mark_app(s_unclocked[i]);  /* their alarms were armed with no clock: they get records now */
        }
        s_unclocked_n = 0;
    }
    if (clock && s_recs_taken) {
        rearm(now_ms, epoch_now);
    }
    alarms_tick(now_ms, clock, epoch_now);
}

void apps_values_tick(int64_t now_ms) {
    records_tick(now_ms);
    if (now_ms < s_retry_at) {
        return;
    }
    bool flash = apps_store_mode() == APPS_STORAGE_FLASH;
    for (size_t i = 0; i < APPS_VALUES_RAM_APPS; i++) {
        store_node_t *n = s_pending[i];
        if (n == NULL) {
            continue;
        }
        if (flash && !n->paid) {
            if (!apps_values_budget_take(node_cost(n), false)) {
                continue;  /* it waits for the budget */
            }
            n->paid = true;
        }
        if (!apps_io_submit_op(store_put_op, n)) {
            s_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker still can't: the same again, a little later (it said why in its log line) */
            return;
        }
        s_pending[i] = NULL;  /* the worker's: n isn't touched again */
    }
}
