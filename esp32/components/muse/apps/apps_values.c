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
#include <stdint.h>
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
 * The alarms' records (P1): s_marked, each app's touched names that wait for their sync (each with its time), or all of them; s_recs, the
 * records the mount handed over, each waiting to be armed again, armed again, or done with (its name touched, its app gone, the table
 * full); s_unclocked, the names synced armed with no clock: the app task's alone. When an app's names are due, the tick reads the app's
 * alarms and queues an op that hands those names, each with what the table has of it, to apps_store_sync_alarms, which touches no other
 * record. Two notes come from other tasks, under the lock: s_waits, the worker's records that the budget couldn't take, which are synced
 * again once it can, and s_forgotten, the apps the store removed or quarantined, whose handed-over records are forgotten at the round's
 * start, before the re-arm looks. The re-arm and the app's own alarm calls are both the app task's, so the app's choice always wins.
 *
 * The tables of apps (s_failing, s_failed, s_slow, s_slow_new, s_marked, s_recs, s_unclocked, s_waits, s_forgotten; about 8.5 KB) are in
 * PSRAM (EXT_RAM_BSS_ATTR, as the store's are): plain memory that only tasks touch, never an atomic, kept out of the internal RAM the Link
 * needs. The lock stays internal.
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
    bool held;                          /* it waits for the budget (else, while it waits, for the worker): a newer save waits with it */
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

/* bytes given back to the budget: a write it was charged for that the worker never had (#4). Never past full. */
static void budget_give(uint32_t bytes) {
    lock();
    int64_t held = budget_now_locked() + (int64_t)bytes * UNITS_PER_BYTE;
    s_budget = held > BUDGET_FULL ? BUDGET_FULL : held;
    unlock();
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
static EXT_RAM_BSS_ATTR store_node_t *s_pending[APPS_VALUES_RAM_APPS];
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

/* n's cost, taken for the write the worker is about to be handed (on flash; RAM costs nothing): false when it doesn't fit, which a
 * forced one always does. A save is charged only for the worker's having it: one the worker refuses is refunded (#4). */
static bool charge(const store_node_t *n, bool flash) {
    uint32_t cost = node_cost(n);
    return !flash || cost == 0 || apps_values_budget_take(cost, n->forced);
}

static void refund(const store_node_t *n, bool flash) {
    uint32_t cost = node_cost(n);
    if (flash && cost > 0) {
        budget_give(cost);
    }
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
    n->held = n->forced = false;
    snprintf(n->app, sizeof n->app, "%s", app);
    memcpy(n->json, json, len);
    n->json[len] = '\0';
    bool flash = apps_store_mode() == APPS_STORAGE_FLASH;
    int at = pending_of(app);
    bool held_back = flash && at >= 0 && s_pending[at]->held;
    if (at >= 0) {
        apps_port_free(s_pending[at]);  /* this is the newer: a save that waited (never charged: #4) is of no use to anyone now */
        s_pending[at] = NULL;
    }
    if (held_back || !charge(n, flash)) {
        n->held = true;
        slow_begin(app);  /* the budget holds it back: it waits for the tick, in the place of the app's save that waited */
        return keep(n, false);
    }
    if (flash && node_cost(n) > 0) {
        slow_end(app);  /* it fit at once */
    }
    if (apps_io_submit_op(store_put_op, n)) {
        return true;
    }
    refund(n, flash);  /* the worker refused it: it pays when it is handed over, once (#4) */
    return keep(n, true);
}

void apps_values_flush(const char *app) {
    int at = app != NULL ? pending_of(app) : -1;
    if (at < 0) {
        return;
    }
    store_node_t *n = s_pending[at];
    bool flash = apps_store_mode() == APPS_STORAGE_FLASH;
    n->forced = true;  /* an app's end writes over the budget, which fills from below zero */
    n->held = false;
    (void)charge(n, flash);
    if (apps_io_submit_op(store_put_op, n)) {
        s_pending[at] = NULL;  /* the worker's: n isn't touched again */
        return;
    }
    refund(n, flash);  /* the worker refused it: the tick offers it again, forced */
}

/* ---- The alarms' records ---- */

/* An app's names that wait for their sync: each with its own time (a burst shares the first's, a record that waits for the budget has its
 * own), or every name of the app (all). The app task's alone. */
typedef struct {
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t due;                        /* when its sync is queued */
} touched_t;

typedef struct {
    char app[APPS_ID_MAX + 1];
    bool all;                           /* every name: the system cancelled them all, or the app touched more than names[] holds */
    int64_t all_due;
    int n;
    touched_t names[APPS_STORE_SYNC_MAX];
} marked_t;

typedef struct {
    char app[APPS_ID_MAX + 1];
    bool all;
    int n;
    apps_store_name_t names[APPS_STORE_SYNC_MAX];
} sync_t;

enum { REC_WAITING, REC_ARMED, REC_DONE };

typedef struct {
    apps_store_record_t r;
    uint8_t state;                      /* waiting to be armed again, armed again, or done with (touched, its app gone, refused) */
} handed_t;

typedef struct {
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
} note_t;

static EXT_RAM_BSS_ATTR marked_t s_marked[MARKED_MAX];
static int s_marked_n;
static int64_t s_marked_retry_at;       /* the worker refused an op, or there was no memory for one: not before then */

static EXT_RAM_BSS_ATTR handed_t s_recs[APPS_STORE_RECORDS_MAX];
static int s_recs_n;
static bool s_recs_taken;
static int64_t s_rearm_at;              /* a re-arm that got no memory: not before then */
/* Names synced armed with no clock (their old records dropped): synced again when it comes. At most the alarm table's. */
static EXT_RAM_BSS_ATTR note_t s_unclocked[APPS_ALARMS_MAX];
static int s_unclocked_n;

/* Notes from other tasks, under s_lock, which the app task's round takes: the records the worker's syncs couldn't write for the budget
 * (at most one for each alarm of the table), and the apps the store removed or quarantined (more than that between two rounds: every
 * record that waits goes). */
static EXT_RAM_BSS_ATTR note_t s_waits[APPS_ALARMS_MAX];
static int s_waits_n;
static EXT_RAM_BSS_ATTR char s_forgotten[APPS_STORE_RECORDS_MAX][APPS_ID_MAX + 1];
static int s_forgotten_n;
static bool s_forgotten_all;

static void sync_op(void *arg) {
    sync_t *s = arg;
    apps_store_sync_alarms(s->app, s->names, s->n, s->all);
    apps_port_free(s);
}

/* The earliest time any of m's names is due. */
static int64_t marked_due(const marked_t *m) {
    int64_t due = m->all ? m->all_due : INT64_MAX;
    for (int i = 0; i < m->n; i++) {
        due = m->names[i].due < due ? m->names[i].due : due;
    }
    return due;
}

/* app's alarm name (NULL: every name) is due a sync at due; batch: with the app's names that wait already, if they come sooner, so that a
 * burst of changes is one op. */
static void mark_name(const char *app, const char *name, int64_t due, bool batch) {
    marked_t *m = NULL;
    for (int i = 0; i < s_marked_n && m == NULL; i++) {
        m = strcmp(s_marked[i].app, app) == 0 ? &s_marked[i] : NULL;
    }
    if (m == NULL) {
        if (s_marked_n == MARKED_MAX) {
            ESP_LOGW(TAG, "alarm: the records of %s's alarms weren't updated: %d other apps' wait", app, MARKED_MAX);
            return;
        }
        m = &s_marked[s_marked_n++];
        memset(m, 0, sizeof *m);
        snprintf(m->app, sizeof m->app, "%s", app);
    }
    int64_t earliest = marked_due(m);
    if (batch && earliest < due) {
        due = earliest;
    }
    if (m->all) {
        m->all_due = due < m->all_due ? due : m->all_due;
        return;  /* every name waits already */
    }
    if (name != NULL) {
        for (int i = 0; i < m->n; i++) {
            if (strcmp(m->names[i].name, name) == 0) {
                m->names[i].due = due < m->names[i].due ? due : m->names[i].due;
                return;
            }
        }
        if (m->n < APPS_STORE_SYNC_MAX) {
            snprintf(m->names[m->n].name, sizeof m->names[0].name, "%s", name);
            m->names[m->n++].due = due;
            return;
        }
        ESP_LOGW(TAG, "alarm: %s touched more than %d names before its records were synced: they all follow the table", app,
                 APPS_STORE_SYNC_MAX);
    }
    m->all = true;
    m->all_due = due < earliest ? due : earliest;
    m->n = 0;
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
    if (app == NULL || strlen(app) > APPS_ID_MAX || (name != NULL && strlen(name) > APPS_ALARM_NAME_MAX)) {
        return;
    }
    records_take();
    for (int i = 0; i < s_recs_n; i++) {
        handed_t *h = &s_recs[i];
        if (h->state != REC_DONE && strcmp(h->r.id, app) == 0 && (name == NULL || strcmp(h->r.name, name) == 0)) {
            h->state = REC_DONE;  /* the app's, or the system's, choice: its record follows the table from now on */
        }
    }
    mark_name(app, name, apps_port_mono_ms() + APPS_VALUES_ALARM_DELAY_MS, true);
}

void apps_values_record_waits(const char *app, const char *name) {
    if (app == NULL || name == NULL || strlen(app) > APPS_ID_MAX || strlen(name) > APPS_ALARM_NAME_MAX) {
        return;
    }
    lock();
    bool noted = false;
    for (int i = 0; i < s_waits_n && !noted; i++) {
        noted = strcmp(s_waits[i].app, app) == 0 && strcmp(s_waits[i].name, name) == 0;
    }
    bool room = noted || s_waits_n < APPS_ALARMS_MAX;
    if (!noted && room) {
        snprintf(s_waits[s_waits_n].app, sizeof s_waits[0].app, "%s", app);
        snprintf(s_waits[s_waits_n++].name, sizeof s_waits[0].name, "%s", name);
    }
    unlock();
    if (!room) {
        ESP_LOGW(TAG, "alarm %s/%s: no record until it changes again: too many wait for the budget", app, name);
    }
}

void apps_values_forget_records(const char *app) {
    if (app == NULL || strlen(app) > APPS_ID_MAX) {
        return;
    }
    lock();
    bool noted = false;
    for (int i = 0; i < s_forgotten_n && !noted; i++) {
        noted = strcmp(s_forgotten[i], app) == 0;
    }
    if (!noted && s_forgotten_n < APPS_STORE_RECORDS_MAX) {
        snprintf(s_forgotten[s_forgotten_n++], sizeof s_forgotten[0], "%s", app);
    } else if (!noted) {
        s_forgotten_all = true;
    }
    unlock();
}

/* The other tasks' notes: an app the store removed or quarantined has its handed-over records forgotten (#2), before the re-arm can look
 * at them (once they are taken: a quarantine at the mount comes before); a record the budget couldn't take is synced again once it can
 * (#1), on its own. */
static void notes_take(int64_t now_ms) {
    note_t waits[APPS_ALARMS_MAX];
    char gone[APPS_STORE_RECORDS_MAX][APPS_ID_MAX + 1];
    int n_gone = 0;
    bool gone_all = false;
    lock();
    int n_waits = s_waits_n;
    memcpy(waits, s_waits, (size_t)n_waits * sizeof waits[0]);
    s_waits_n = 0;
    if (s_recs_taken) {
        n_gone = s_forgotten_n;
        gone_all = s_forgotten_all;
        memcpy(gone, s_forgotten, (size_t)n_gone * sizeof gone[0]);
        s_forgotten_n = 0;
        s_forgotten_all = false;
    }
    unlock();
    for (int i = 0; i < s_recs_n; i++) {
        bool forgotten = gone_all;
        for (int k = 0; k < n_gone && !forgotten; k++) {
            forgotten = strcmp(s_recs[i].r.id, gone[k]) == 0;
        }
        if (forgotten) {
            s_recs[i].state = REC_DONE;  /* the store dropped its records: nothing of the app that was is armed again */
        }
    }
    if (n_waits > 0) {
        int64_t wait = apps_values_budget_wait_ms(APPS_VALUES_RECORD_COST);
        int64_t due = now_ms + (wait > APPS_VALUES_ALARM_DELAY_MS ? wait : APPS_VALUES_ALARM_DELAY_MS);
        for (int i = 0; i < n_waits; i++) {
            mark_name(waits[i].app, waits[i].name, due, false);
        }
    }
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
            mark_name(h->r.id, h->r.name, now_ms + APPS_VALUES_ALARM_DELAY_MS, true);  /* not in the table: its record goes at the sync */
            ESP_LOGW(TAG, "store: alarm %s/%s couldn't be armed again (the gadget has no slot for it): dropped", h->r.id, h->r.name);
        }
    }
}

/* The names synced armed with no clock, which get records when it comes: name's entry added, or dropped (synced with its time, or gone). */
static void unclocked_note(const char *app, const char *name, bool add) {
    for (int i = 0; i < s_unclocked_n; i++) {
        if (strcmp(s_unclocked[i].app, app) == 0 && strcmp(s_unclocked[i].name, name) == 0) {
            if (!add) {
                s_unclocked[i] = s_unclocked[--s_unclocked_n];
            }
            return;
        }
    }
    if (add && s_unclocked_n < APPS_ALARMS_MAX) {
        snprintf(s_unclocked[s_unclocked_n].app, sizeof s_unclocked[0].app, "%s", app);
        snprintf(s_unclocked[s_unclocked_n++].name, sizeof s_unclocked[0].name, "%s", name);
    }
}

/* One name into s, with what the table (info, n_info) has of it now. */
static void sync_add(sync_t *s, const char *name, const apps_alarm_info_t *info, int n_info, bool clock, int64_t epoch_now) {
    apps_store_name_t *a = &s->names[s->n++];
    snprintf(a->name, sizeof a->name, "%s", name);
    a->state = APPS_STORE_GONE;
    for (int i = 0; i < n_info; i++) {
        if (strcmp(info[i].name, name) == 0) {
            a->state = clock ? APPS_STORE_ARMED : APPS_STORE_ARMED_NO_CLOCK;
            a->epoch_ms = clock ? epoch_now + info[i].remaining_ms : 0;
            a->sound = info[i].sound;
            a->wake = info[i].wake;
        }
    }
}

/* The apps' names that are due, an op for each app with what the alarm table has of them now (or all of it, for all). An app whose names
 * aren't due yet (a record that waits for the budget) holds no other. */
static void alarms_tick(int64_t now_ms, bool clock, int64_t epoch_now) {
    if (s_marked_n == 0 || now_ms < s_marked_retry_at) {
        return;
    }
    apps_storage_t mode = apps_store_mode();
    if (mode == APPS_STORAGE_NONE) {
        return;  /* the mount hasn't run: they wait for it */
    }
    if (mode == APPS_STORAGE_RAM) {
        s_marked_n = s_unclocked_n = 0;  /* nothing outlives a reset in RAM mode */
        return;
    }
    for (int i = 0; i < s_marked_n;) {
        marked_t *m = &s_marked[i];
        bool all = m->all && m->all_due <= now_ms;
        int due = 0;
        for (int k = 0; k < m->n; k++) {
            due += m->names[k].due <= now_ms ? 1 : 0;
        }
        if (!all && due == 0) {
            i++;
            continue;
        }
        sync_t *s = apps_port_realloc(NULL, sizeof *s);
        if (s == NULL) {
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;
            return;
        }
        memset(s, 0, sizeof *s);
        snprintf(s->app, sizeof s->app, "%s", m->app);
        s->all = all;
        apps_alarm_info_t info[APPS_ALARMS_PER_APP];
        int n_info = apps_alarm_list_info(m->app, info);
        if (all) {
            for (int k = 0; k < n_info; k++) {
                sync_add(s, info[k].name, info, n_info, clock, epoch_now);  /* the rest of the app's records go */
            }
        } else {
            for (int k = 0; k < m->n; k++) {
                if (m->names[k].due <= now_ms) {
                    sync_add(s, m->names[k].name, info, n_info, clock, epoch_now);
                }
            }
        }
        if (!apps_io_submit_op(sync_op, s)) {
            apps_port_free(s);
            s_marked_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker can't now (it said why) */
            return;
        }
        if (all) {
            for (int k = s_unclocked_n - 1; k >= 0; k--) {
                if (strcmp(s_unclocked[k].app, m->app) == 0) {
                    s_unclocked[k] = s_unclocked[--s_unclocked_n];  /* their records went, or are in the op */
                }
            }
        }
        for (int k = 0; k < s->n; k++) {
            unclocked_note(m->app, s->names[k].name, s->names[k].state == APPS_STORE_ARMED_NO_CLOCK);
        }
        if (all) {
            m->all = false;
            m->n = 0;
        } else {
            for (int k = m->n - 1; k >= 0; k--) {
                if (m->names[k].due <= now_ms) {
                    m->names[k] = m->names[--m->n];
                }
            }
        }
        if (!m->all && m->n == 0) {
            *m = s_marked[--s_marked_n];  /* its place has another app now, looked at next */
        } else {
            i++;
        }
    }
}

/* The records' share of a round: the mount's records taken, the other tasks' notes, the clock's coming (the names synced without it are
 * synced again), the re-arm, and the syncs that are due. */
static void records_tick(int64_t now_ms) {
    records_take();
    notes_take(now_ms);
    int64_t epoch_now = 0;
    bool clock = apps_port_epoch_ms(&epoch_now);
    if (clock && s_unclocked_n > 0) {
        for (int i = 0; i < s_unclocked_n; i++) {
            mark_name(s_unclocked[i].app, s_unclocked[i].name, now_ms + APPS_VALUES_ALARM_DELAY_MS, true);  /* armed with no clock: records now */
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
        if (!charge(n, flash)) {
            n->held = true;  /* it waits for the budget */
            continue;
        }
        n->held = false;
        if (!apps_io_submit_op(store_put_op, n)) {
            refund(n, flash);
            s_retry_at = now_ms + APPS_VALUES_RETRY_MS;  /* the worker still can't: the same again, a little later (it said why in its log line) */
            return;
        }
        s_pending[i] = NULL;  /* the worker's: n isn't touched again */
    }
}
