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

#include "apps_alarm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_input.h"
#include "muse_voice.h"

/*
 * The system's alarms (apps_alarm.h has the contract). Three kinds of task meet here: the app task (arming, cancelling,
 * asking, and taking what has fired), the esp_timer task (every callback below), and whoever else asks (the console). One
 * mutex guards the table and the ring's state, held for copies and calls that don't wait (esp_timer_start_* and _stop, the
 * voice and input requests): never across a log line, which can wait for the console, nor a wait for another task. The
 * esp_timer task takes it with a short try, since it serves every timer of the gadget; a try that fails is made again 10 ms
 * later. The pending word is atomic, so the app task asks whether anything has fired without the lock. All of it is static,
 * so in internal RAM (a flag or a lock in PSRAM would not be atomic on the S3).
 *
 * A slot has two halves that are told apart because an app's start() may arm the name its own alarm launched it for: the
 * arming (used: a timer runs for due_us) and the firing that is waiting to be told to the app (fired). They are independent: a
 * slot is free when neither is, arming a name that fired and hasn't been told leaves that firing to be delivered, and a
 * firing is taken, or cancelled, without touching an arming made since.
 *
 * An alarm's esp_timer is made the first time its slot is used and never deleted: a callback that was already on its way to
 * the esp_timer task when the alarm is cancelled or replaced still finds its timer, and looks at the slot, which says whether
 * anything is due: armed, and its time reached. The same test runs a timer that came late as it is.
 */

static const char *TAG = "muse_apps";

#define LOCK_TRY_MS 20
#define RETRY_US 10000

typedef struct {
    bool used;                      /* armed: its timer runs, for due_us */
    bool fired;                     /* it fired, and its app hasn't been told */
    bool sound, wake;               /* the arming's */
    bool fired_sound;               /* the firing's (an arming made since may differ) */
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t due_us;                 /* the arming's, on esp_timer's clock */
    int64_t fired_due_us, fired_us; /* the firing's: when it was due, and when it fired */
    uint32_t gen;                   /* the arming's own, and */
    uint32_t fired_gen;             /* the firing's: from one counter, never 0 while in use */
    esp_timer_handle_t timer;
} slot_t;

typedef void (*fired_cb_t)(const char *app, const char *name);

typedef enum { INIT_NONE, INIT_MAKING, INIT_READY, INIT_FAILED } init_t;

static slot_t s_slot[APPS_ALARMS_MAX];
static atomic_uint s_pending;  /* bit i: slot i has fired and its app hasn't been told */
static uint32_t s_gen;         /* the last gen given */
static atomic_int s_init;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static _Atomic(fired_cb_t) s_fired_cb;

/* The ring: one for the gadget, whichever alarm started it. */
static esp_timer_handle_t s_ring_timer;
static bool s_ringing;
static int64_t s_ring_end_us;
static unsigned s_ring_chirps;
static char s_ring_app[APPS_ID_MAX + 1], s_ring_name[APPS_ALARM_NAME_MAX + 1];

static void fire_cb(void *arg);
static void ring_cb(void *arg);

/* The lock and the ring's timer, made once, by whichever task comes first (the others wait for it: a few microseconds of
 * work). False when there was no memory for them. */
static bool ready(void) {
    int state = atomic_load(&s_init);
    if (state == INIT_NONE) {
        int none = INIT_NONE;
        if (atomic_compare_exchange_strong(&s_init, &none, INIT_MAKING)) {
            const esp_timer_create_args_t ring = {
                .callback = ring_cb, .dispatch_method = ESP_TIMER_TASK, .name = "muse_ring"};
            s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
            bool ok = s_lock != NULL && esp_timer_create(&ring, &s_ring_timer) == ESP_OK;
            if (!ok) {
                ESP_LOGE(TAG, "alarms: no memory for the ring's timer");
            }
            atomic_store(&s_init, ok ? INIT_READY : INIT_FAILED);
        }
    }
    while ((state = atomic_load(&s_init)) == INIT_MAKING) {
        vTaskDelay(1);
    }
    return state == INIT_READY;
}

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

/* The esp_timer task's: false when the table is busy for longer than it should be. */
static bool try_lock(void) {
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_TRY_MS)) == pdTRUE;
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

static bool id_valid(const char *app) {
    size_t n = app != NULL ? strlen(app) : 0;
    return n >= 1 && n <= APPS_ID_MAX;
}

static bool name_valid(const char *name) {
    size_t n = name != NULL ? strlen(name) : 0;
    for (size_t i = 0; i < n; i++) {
        bool ok = (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_';
        if (!ok) {
            return false;
        }
    }
    return n >= 1 && n <= APPS_ALARM_NAME_MAX;
}

/* ---- The table (the lock held) ---- */

static bool occupied(const slot_t *s) {
    return s->used || s->fired;
}

static slot_t *find(const char *app, const char *name) {
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        if (occupied(&s_slot[i]) && strcmp(s_slot[i].app, app) == 0 && strcmp(s_slot[i].name, name) == 0) {
            return &s_slot[i];
        }
    }
    return NULL;
}

static unsigned bit_of(const slot_t *s) {
    return 1u << (s - s_slot);
}

static uint32_t next_gen(void) {
    if (++s_gen == 0) {
        s_gen = 1;
    }
    return s_gen;
}

/* The firing that waits in s is over: told, or dropped. The arming, if there is one, goes on. */
static void clear_fired(slot_t *s) {
    s->fired = false;
    atomic_fetch_and(&s_pending, ~bit_of(s));
}

/* What the app is told of the alarm s: the time left of its arming, rounded up, and 0 for one that is due, or that only waits to be told. */
static int64_t left_ms(const slot_t *s, int64_t now_us) {
    int64_t left_us = s->due_us - now_us;
    return !s->used || left_us <= 0 ? 0 : (left_us + 999) / 1000;
}

/* When s is, or was, due, and its tie-break: the arming's, else the firing's that waits. */
static int64_t due_key(const slot_t *s) {
    return s->used ? s->due_us : s->fired_due_us;
}

static uint32_t gen_key(const slot_t *s) {
    return s->used ? s->gen : s->fired_gen;
}

/* Whether a runs before b: in a list of an app's alarms the earlier due, then the one armed first; among the ones that wait to
 * be told (by_fired) the one that fired first, then by that order. */
static bool before(const slot_t *a, const slot_t *b, bool by_fired) {
    if (by_fired && a->fired_us != b->fired_us) {
        return a->fired_us < b->fired_us;
    }
    int64_t ta = by_fired ? a->fired_due_us : due_key(a), tb = by_fired ? b->fired_due_us : due_key(b);
    if (ta != tb) {
        return ta < tb;
    }
    return (by_fired ? a->fired_gen : gen_key(a)) < (by_fired ? b->fired_gen : gen_key(b));
}

/* The first `max` (at least 1) slots in use, in order, into out: only the ones that wait to be told, or only app's; the count. */
static int collect(const slot_t **out, int max, const char *app, bool fired_only) {
    int n = 0;
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        const slot_t *s = &s_slot[i];
        if (!occupied(s) || (fired_only && !s->fired) || (app != NULL && strcmp(s->app, app) != 0)) {
            continue;
        }
        if (n == max && !before(s, out[n - 1], fired_only)) {
            continue;
        }
        int at = n < max ? n++ : n - 1;
        for (; at > 0 && before(s, out[at - 1], fired_only); at--) {
            out[at] = out[at - 1];
        }
        out[at] = s;
    }
    return n;
}

/* ---- The ring ---- */

/* Under the lock: the ring starts, or goes on and lasts APPS_RING_MAX_MS more, and one chirp is asked for. Returns the number
 * of that chirp. The call to the voice task sets a flag and an event-group bit and returns: nothing that waits. */
static unsigned ring_chirp_locked(const char *app, const char *name, int64_t now_us, bool start) {
    snprintf(s_ring_app, sizeof s_ring_app, "%s", app);
    snprintf(s_ring_name, sizeof s_ring_name, "%s", name);
    s_ring_end_us = now_us + (int64_t)APPS_RING_MAX_MS * 1000;
    if (start) {
        s_ring_chirps = 0;
        esp_err_t err = esp_timer_start_periodic(s_ring_timer, (uint64_t)APPS_RING_PERIOD_MS * 1000);
        s_ringing = err == ESP_OK || err == ESP_ERR_INVALID_STATE;  /* already going: the ring only goes on */
    }
    muse_voice_request_chirp();
    return ++s_ring_chirps;
}

/* The ring ends, if one is going; why is what the log says. */
void apps_alarm_ring_end(const char *why) {
    if (atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    bool was = s_ringing;
    if (was) {
        s_ringing = false;
        (void)esp_timer_stop(s_ring_timer);
    }
    unlock();
    if (was) {
        ESP_LOGI(TAG, "ring stopped (%s)", why);
    }
}

/* Every APPS_RING_PERIOD_MS while it rings, in the esp_timer task: a chirp, or the end when its minute is over. A tick of a ring
 * that was stopped meanwhile stops the timer, which stop's own call may have been too early for. A tick that can't take the
 * table is lost: the next one is 1.5 s on. */
static void ring_cb(void *arg) {
    (void)arg;
    if (!try_lock()) {
        return;
    }
    char app[APPS_ID_MAX + 1], name[APPS_ALARM_NAME_MAX + 1];
    unsigned chirp = 0;
    bool timed_out = false;
    if (!s_ringing) {
        (void)esp_timer_stop(s_ring_timer);
        unlock();
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (now_us >= s_ring_end_us) {
        s_ringing = false;
        (void)esp_timer_stop(s_ring_timer);
        timed_out = true;
    } else {
        snprintf(app, sizeof app, "%s", s_ring_app);
        snprintf(name, sizeof name, "%s", s_ring_name);
        muse_voice_request_chirp();
        chirp = ++s_ring_chirps;
    }
    unlock();
    if (timed_out) {
        ESP_LOGI(TAG, "ring stopped (timeout)");
    } else {
        ESP_LOGI(TAG, "ring %s/%s chirp %d", app, name, (int)chirp);
    }
}

void apps_alarm_ring_stop(void) {
    apps_alarm_ring_end("sound.stop");
}

/* ---- Firing ---- */

/* An alarm's timer, in the esp_timer task (see the top of the file: a stale call finds nothing due and does nothing). The arming
 * is spent and its firing waits in the slot for the app task, which is told by the bit and by the post. A firing that is still
 * waiting when the next one comes (an app that started late, and had armed the name again) is told once, not twice. */
static void fire_cb(void *arg) {
    slot_t *s = &s_slot[(uintptr_t)arg];
    if (!try_lock()) {
        (void)esp_timer_start_once(s->timer, RETRY_US);
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (!s->used || now_us < s->due_us) {
        unlock();
        return;
    }
    s->used = false;
    if (!s->fired) {
        s->fired = true;
        s->fired_sound = false;
        s->fired_us = now_us;
        s->fired_due_us = s->due_us;
        s->fired_gen = next_gen();
    }
    s->fired_sound = s->fired_sound || s->sound;
    atomic_fetch_or(&s_pending, bit_of(s));
    char app[APPS_ID_MAX + 1], name[APPS_ALARM_NAME_MAX + 1];
    snprintf(app, sizeof app, "%s", s->app);
    snprintf(name, sizeof name, "%s", s->name);
    bool sound = s->sound, wake = s->wake;
    unsigned chirp = 0;
    if (sound) {
        chirp = ring_chirp_locked(app, name, now_us, !s_ringing);
    }
    unlock();
    /* What matters first, then the log: a line can wait for the console, which the screen's wake and the app's post must not. */
    if (wake) {
        muse_input_wake("alarm");
    }
    fired_cb_t cb = atomic_load(&s_fired_cb);
    if (cb != NULL) {
        cb(app, name);
    }
    ESP_LOGI(TAG, "alarm %s/%s fired (sound %d, wake %d)", app, name, sound, wake);
    if (sound) {
        ESP_LOGI(TAG, "ring %s/%s chirp %d", app, name, (int)chirp);
    }
}

/* ---- The API ---- */

bool apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    if (!id_valid(app) || !name_valid(name) || ms < 1 || ms > APPS_ALARM_MAX_MS || !ready()) {
        return false;
    }
    lock();
    slot_t *s = find(app, name);
    if (s == NULL) {  /* a name of its own: a slot, within the limits (a firing that waits holds its slot) */
        int mine = 0;
        for (int i = 0; i < APPS_ALARMS_MAX; i++) {
            mine += occupied(&s_slot[i]) && strcmp(s_slot[i].app, app) == 0 ? 1 : 0;
            s = s == NULL && !occupied(&s_slot[i]) ? &s_slot[i] : s;
        }
        if (mine >= APPS_ALARMS_PER_APP || s == NULL) {
            unlock();
            return false;
        }
    }
    if (s->timer == NULL) {
        const esp_timer_create_args_t args = {.callback = fire_cb,
                                              .arg = (void *)(uintptr_t)(s - s_slot),
                                              .dispatch_method = ESP_TIMER_TASK,
                                              .name = "muse_alarm"};
        if (esp_timer_create(&args, &s->timer) != ESP_OK) {
            s->timer = NULL;
            unlock();
            return false;
        }
    }
    (void)esp_timer_stop(s->timer);  /* a replaced alarm's is running; one that fired is not, and says so: nothing to do */
    s->used = true;
    s->sound = sound;
    s->wake = wake;
    snprintf(s->app, sizeof s->app, "%s", app);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->due_us = esp_timer_get_time() + ms * 1000;
    s->gen = next_gen();
    esp_err_t err = esp_timer_start_once(s->timer, (uint64_t)ms * 1000);
    if (err == ESP_ERR_INVALID_STATE) {
        /* fire_cb's retry of an earlier arming started it between our stop and now (it can't take the lock to be told otherwise):
         * the timer is ours, so it is stopped again and started for this arming. */
        (void)esp_timer_stop(s->timer);
        err = esp_timer_start_once(s->timer, (uint64_t)ms * 1000);
    }
    bool armed = err == ESP_OK;
    s->used = armed;
    unlock();
    return armed;
}

void apps_alarm_cancel(const char *app, const char *name) {
    if (!id_valid(app) || !name_valid(name) || atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    slot_t *s = find(app, name);
    bool drops_ring = false;
    if (s != NULL) {
        (void)esp_timer_stop(s->timer);
        s->used = false;
        drops_ring = s->fired && s->fired_sound;  /* what fired and wasn't told is dropped too, and the ring it started */
        clear_fired(s);
    }
    unlock();
    if (drops_ring) {
        apps_alarm_ring_end("alarm cancelled");
    }
}

int64_t apps_alarm_remaining(const char *app, const char *name) {
    if (!id_valid(app) || !name_valid(name) || atomic_load(&s_init) != INIT_READY) {
        return -1;
    }
    lock();
    const slot_t *s = find(app, name);
    int64_t left = s != NULL ? left_ms(s, esp_timer_get_time()) : -1;
    unlock();
    return left;
}

int apps_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    _Static_assert(APPS_ALARMS_PER_APP == 4 && APPS_ALARM_NAME_MAX + 1 == 24, "apps_port.h's arrays");
    if (!id_valid(app) || atomic_load(&s_init) != INIT_READY) {
        return 0;
    }
    const slot_t *mine[APPS_ALARMS_PER_APP];
    lock();
    int64_t now_us = esp_timer_get_time();
    int n = collect(mine, APPS_ALARMS_PER_APP, app, false);
    for (int i = 0; i < n; i++) {
        snprintf(names[i], 24, "%s", mine[i]->name);
        remaining[i] = left_ms(mine[i], now_us);
    }
    unlock();
    return n;
}

void apps_alarm_set_fired_cb(void (*cb)(const char *app, const char *name)) {
    atomic_store(&s_fired_cb, cb);
}

/* ---- The app task's side ---- */

bool apps_alarm_pending(void) {
    return atomic_load(&s_pending) != 0;
}

int apps_alarm_fired(apps_alarm_fired_t *out, int max) {
    if (out == NULL || max <= 0 || atomic_load(&s_init) != INIT_READY) {
        return 0;
    }
    const slot_t *fired[APPS_ALARMS_MAX];
    lock();
    int n = collect(fired, max < APPS_ALARMS_MAX ? max : APPS_ALARMS_MAX, NULL, true);
    for (int i = 0; i < n; i++) {
        snprintf(out[i].app, sizeof out[i].app, "%s", fired[i]->app);
        snprintf(out[i].name, sizeof out[i].name, "%s", fired[i]->name);
        out[i].sound = fired[i]->fired_sound;
        out[i].fired_ms = fired[i]->fired_us / 1000;
        out[i].slot = (uint8_t)(fired[i] - s_slot);
        out[i].gen = fired[i]->fired_gen;
    }
    unlock();
    return n;
}

bool apps_alarm_take(const apps_alarm_fired_t *f, bool delivered) {
    if (f == NULL || f->slot >= APPS_ALARMS_MAX || atomic_load(&s_init) != INIT_READY) {
        return false;
    }
    lock();
    slot_t *s = &s_slot[f->slot];
    bool mine = s->fired && s->fired_gen == f->gen;
    bool drops_ring = mine && !delivered && s->fired_sound;
    if (mine) {
        clear_fired(s);
    }
    unlock();
    if (drops_ring) {
        apps_alarm_ring_end("alarm dropped");
    }
    return mine;
}
