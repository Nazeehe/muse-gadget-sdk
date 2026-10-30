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

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_input.h"
#include "muse_voice.h"

/*
 * The system's alarms (apps_alarm.h has the contract). Three kinds of task meet here: the app task (arming, cancelling, asking,
 * taking what has fired, and saying that an app has ended), the esp_timer task (every callback below), and whoever else asks (the
 * console). One mutex guards the table, the ring's state, the budget and the journal, held for copies and calls that don't wait
 * (esp_timer_start_* and _stop, the voice and input requests): never across a log line, which can wait for the console, nor a wait for
 * another task. The esp_timer task takes it with a short try, since it serves every timer of the gadget; a try that fails is made
 * again 10 ms later. The pending word is atomic, so the app task asks whether anything has fired without the lock.
 *
 * Where it lives. What a lock, a flag or a counter is made of stays in internal RAM (a flag or a lock in PSRAM would not be atomic on
 * the S3): the mutex's buffer, the pending word, the init state, the fired callback and the journal's counters, all statics here with
 * no attribute. The table, the ring's names, the budget and the journal's lines, which are touched under the lock alone, are in PSRAM
 * (EXT_RAM_BSS_ATTR: an attribute that is empty unless the build lets statics go there, which the apps build does).
 *
 * A slot has two halves that are told apart because an app's start() may arm the name its own alarm launched it for: the arming (used:
 * a timer runs for due_us) and the firing that is waiting to be told to the app (fired). They are independent: a slot is free when
 * neither is, arming a name that fired and hasn't been told leaves that firing to be delivered, and a firing is taken, or cancelled,
 * without touching an arming made since.
 *
 * An alarm's esp_timer is made the first time its slot is used and never deleted: a callback that was already on its way to the
 * esp_timer task when the alarm is cancelled or replaced still finds its timer, and looks at the slot, which says whether anything is
 * due: armed, and its time reached. The same test runs a timer that came late as it is.
 *
 * The ring is one for the gadget, whichever alarm started it. What ends it is decided under the lock in one place each: the minute
 * (ring_cb), sound.stop (apps_alarm_ring_stop), and the lack of anything that could stop it (ring_orphaned_locked, run by every call
 * that takes a firing out of the table or says an app has ended): no firing with sound waits, and the app last told one is not there.
 *
 * The esp_timer task never logs: a console line can wait for another task's whole reply, and every timer of the gadget waits behind
 * it. What a callback would say it records, as a line of the journal (a small ring, under the lock), and the app task prints the
 * journal in its next round (apps_alarm_log_drain); a call that changes the table from another task prints what it recorded when it
 * has let go of the lock. The lines of firings and of cancels pass a token bucket, so no app makes the console busy with them.
 */

static const char *TAG = "muse_apps";

#define LOCK_TRY_MS 20
#define RETRY_US 10000
#define JOURNAL_LEN 24            /* lines waiting for the app task: a second's worth is about a dozen at the most */
#define LINE_LEN 112              /* a line's text, with its NUL */
#define BUCKET_US 250000          /* the bucket's refill: one line of firing or cancel every 250 ms, APPS_ALARM_LOG_BURST of them in all */
#define QUIET_US 1000000          /* lines refused, and quiet this long: the count of them is printed */
#define BUDGET_US ((int64_t)APPS_ALARM_BUDGET_MS * 1000)
#define BUDGET_APPS APPS_ALARMS_MAX /* apps whose firings are counted: the least recent is forgotten for a new one */
#define RING_PERIOD_US ((uint64_t)APPS_RING_PERIOD_MS * 1000)
#define RING_MAX_US ((int64_t)APPS_RING_MAX_MS * 1000)

typedef struct {
    bool used;                      /* armed: its timer runs, for due_us */
    bool fired;                     /* it fired, and its app hasn't been told */
    bool sound, wake;               /* the arming's */
    bool fired_sound, fired_wake;   /* the firing's (an arming made since may differ; a second firing that merged adds its own) */
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t due_us;                 /* the arming's, on esp_timer's clock */
    int64_t fired_due_us, fired_us; /* the firing's: when it was due, and when it fired */
    uint32_t gen;                   /* the arming's own, and */
    uint32_t fired_gen;             /* the firing's: from one counter, never 0 while in use */
    esp_timer_handle_t timer;
} slot_t;

/* The last APPS_ALARM_BUDGET firings of an app, oldest first from head. */
typedef struct {
    char app[APPS_ID_MAX + 1];
    int64_t at_us[APPS_ALARM_BUDGET];
    uint8_t n, head;
    int64_t last_us;                /* of its newest: the least recent record is the one that is given up */
} budget_t;

/* A line the esp_timer task, or a call that holds the lock, means to print (apps_alarm_log_drain does). */
typedef struct {
    char level;                     /* 'I', 'W' or 'E' */
    unsigned skipped;               /* lines the bucket refused since the last that was printed: their count is printed first */
    char line[LINE_LEN];            /* empty for the count alone */
} entry_t;

typedef void (*fired_cb_t)(const char *app, const char *name);

typedef enum { INIT_NONE, INIT_MAKING, INIT_READY } init_t;

static EXT_RAM_BSS_ATTR slot_t s_slot[APPS_ALARMS_MAX];
static atomic_uint s_pending;  /* bit i: slot i has fired and its app hasn't been told */
static uint32_t s_gen;         /* the last gen given */
static atomic_int s_init;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static _Atomic(fired_cb_t) s_fired_cb;

/* The ring: one for the gadget, whichever alarm started it. */
static esp_timer_handle_t s_ring_timer;
static atomic_bool s_ringing;
static int64_t s_ring_end_us;
static unsigned s_ring_chirps;
static EXT_RAM_BSS_ATTR char s_ring_app[APPS_ID_MAX + 1], s_ring_name[APPS_ALARM_NAME_MAX + 1];  /* the alarm that rang last */
static EXT_RAM_BSS_ATTR char s_ring_told[APPS_ID_MAX + 1];  /* the app last told an alarm with sound while it rings, or "" */

static EXT_RAM_BSS_ATTR budget_t s_budget[BUDGET_APPS];

/* The apps cancel_app_locked emptied since the app task last took them (apps_alarm_take_cancelled), each once: under the lock. */
static EXT_RAM_BSS_ATTR char s_cancelled[APPS_ALARMS_MAX][APPS_ID_MAX + 1];
static int s_cancelled_n;

static EXT_RAM_BSS_ATTR entry_t s_journal[JOURNAL_LEN];
static unsigned s_j_head;
static atomic_uint s_j_count;   /* the lines waiting: changed under the lock, read without it (the drain's quick look) */
static atomic_uint s_skipped;   /* lines refused (or lost, with no room) that no line has counted yet; the same */
static int64_t s_last_refusal_us;
static int64_t s_credit_us, s_credit_at_us;  /* the bucket: what it holds, as the time to print that many lines, and when it was read */

static void fire_cb(void *arg);
static void ring_cb(void *arg);

/* The lock and the ring's timer, made once, by whichever task comes first (the others wait for it: a few microseconds of work). A
 * timer that can't be made leaves it all to be made again by the next call, so the gadget that was short of memory once is not
 * without alarms for good. False when there was no memory for them. */
static bool ready(void) {
    for (;;) {
        int state = atomic_load(&s_init);
        if (state == INIT_READY) {
            return true;
        }
        if (state == INIT_MAKING) {
            vTaskDelay(1);
            continue;
        }
        int none = INIT_NONE;
        if (!atomic_compare_exchange_strong(&s_init, &none, INIT_MAKING)) {
            continue;
        }
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        }
        bool ok = s_lock != NULL;
        if (ok && s_ring_timer == NULL) {
            const esp_timer_create_args_t ring = {
                .callback = ring_cb, .dispatch_method = ESP_TIMER_TASK, .name = "muse_ring"};
            ok = esp_timer_create(&ring, &s_ring_timer) == ESP_OK;
            if (!ok) {
                s_ring_timer = NULL;
            }
        }
        if (ok) {
            s_credit_us = (int64_t)APPS_ALARM_LOG_BURST * BUCKET_US;
            s_credit_at_us = esp_timer_get_time();
        } else {
            ESP_LOGE(TAG, "alarms: no memory for the ring's timer: the next alarm.after tries again");
        }
        atomic_store(&s_init, ok ? INIT_READY : INIT_NONE);
        return ok;
    }
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

/* An app's id as apps_id_valid says (apps_pkg.c: ^[a-z][a-z0-9_]{1,23}$), which that file can't be asked here: it is built with Lua. */
static bool id_valid(const char *app) {
    size_t n = 0;
    for (; app != NULL && app[n] != '\0'; n++) {
        char c = app[n];
        bool ok = (c >= 'a' && c <= 'z') || (n > 0 && ((c >= '0' && c <= '9') || c == '_'));
        if (!ok || n >= APPS_ID_MAX) {
            return false;
        }
    }
    return n >= 2;
}

/* ---- The journal (the lock held) ---- */

/* Whether a line of a firing or a cancel may be printed now: the bucket holds a line's worth, which it then spends. */
static bool bucket_take_locked(int64_t now_us) {
    const int64_t cap = (int64_t)APPS_ALARM_LOG_BURST * BUCKET_US;
    s_credit_us += now_us - s_credit_at_us;
    s_credit_at_us = now_us;
    if (s_credit_us > cap) {
        s_credit_us = cap;
    }
    if (s_credit_us < BUCKET_US) {
        return false;
    }
    s_credit_us -= BUCKET_US;
    return true;
}

/* Records a line, to be printed by apps_alarm_log_drain; bucketed lines (firings, cancels) are counted instead when the bucket is
 * empty, and the count goes with the next line that is let in. With no room the oldest line is the one that is lost, and counted. */
static void journal_locked(char level, bool bucketed, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void journal_locked(char level, bool bucketed, const char *fmt, ...) {
    int64_t now_us = esp_timer_get_time();
    if (bucketed && !bucket_take_locked(now_us)) {
        atomic_fetch_add(&s_skipped, 1);
        s_last_refusal_us = now_us;
        return;
    }
    unsigned count = atomic_load(&s_j_count);
    if (count == JOURNAL_LEN) {
        atomic_fetch_add(&s_skipped, 1 + s_journal[s_j_head].skipped);
        s_last_refusal_us = now_us;
        s_j_head = (s_j_head + 1) % JOURNAL_LEN;
        count--;
    }
    entry_t *e = &s_journal[(s_j_head + count) % JOURNAL_LEN];
    e->level = level;
    e->skipped = atomic_exchange(&s_skipped, 0);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->line, sizeof e->line, fmt, ap);
    va_end(ap);
    atomic_store(&s_j_count, count + 1);
}

/* Prints what has been recorded, oldest first: the lock is taken for one line at a time and let go of before it is printed. With
 * nothing recorded and no refusals counted it is two loads. */
void apps_alarm_log_drain(void) {
    if (atomic_load(&s_init) != INIT_READY) {
        return;
    }
    while (atomic_load(&s_j_count) > 0 || atomic_load(&s_skipped) > 0) {
        entry_t e = {0};
        bool got = false;
        lock();
        if (atomic_load(&s_j_count) > 0) {
            e = s_journal[s_j_head];
            s_j_head = (s_j_head + 1) % JOURNAL_LEN;
            atomic_fetch_sub(&s_j_count, 1);
            got = true;
        } else if (atomic_load(&s_skipped) > 0 && esp_timer_get_time() - s_last_refusal_us >= QUIET_US) {
            e.level = 'I';
            e.skipped = atomic_exchange(&s_skipped, 0);  /* the lines have been quiet: the count is not held back for another line */
            got = true;
        }
        unlock();
        if (!got) {
            break;
        }
        if (e.skipped > 0) {
            ESP_LOGI(TAG, "(%u alarm lines not logged)", e.skipped);
        }
        if (e.line[0] == '\0') {
            continue;
        }
        switch (e.level) {
        case 'E':
            ESP_LOGE(TAG, "%s", e.line);
            break;
        case 'W':
            ESP_LOGW(TAG, "%s", e.line);
            break;
        default:
            ESP_LOGI(TAG, "%s", e.line);
            break;
        }
    }
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

/* app is one whose alarms were cancelled (its records follow): noted once, among the first APPS_ALARMS_MAX. */
static void note_cancelled_locked(const char *app) {
    for (int i = 0; i < s_cancelled_n; i++) {
        if (strcmp(s_cancelled[i], app) == 0) {
            return;
        }
    }
    if (s_cancelled_n < APPS_ALARMS_MAX) {
        snprintf(s_cancelled[s_cancelled_n++], sizeof s_cancelled[0], "%s", app);
    }
}

/* Disarms every alarm of app and drops every firing of it that waits; how many slots that freed. */
static int cancel_app_locked(const char *app) {
    int n = 0;
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (occupied(s) && strcmp(s->app, app) == 0) {
            if (s->timer != NULL) {
                (void)esp_timer_stop(s->timer);
            }
            s->used = false;
            clear_fired(s);
            n++;
        }
    }
    if (n > 0) {
        note_cancelled_locked(app);
    }
    return n;
}

/* ---- The ring (the lock held) ---- */

/* The ring ends, with why in the log. Nothing is left that waits for it. */
static void ring_stop_locked(const char *why) {
    atomic_store(&s_ringing, false);
    s_ring_told[0] = '\0';
    (void)esp_timer_stop(s_ring_timer);
    journal_locked('I', false, "ring stopped (%s)", why);
}

static bool sounding_pending_locked(void) {
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        if (s_slot[i].fired && s_slot[i].fired_sound) {
            return true;
        }
    }
    return false;
}

/* A ring with nothing left that could stop it, or that has to be told of it, ends: no firing with sound waits to be told, and no app that
 * was told one is there. Every change that could make that so ends here, with the lock held through the look and the stop: no firing
 * can come between them. */
static void ring_orphaned_locked(const char *why) {
    if (atomic_load(&s_ringing) && s_ring_told[0] == '\0' && !sounding_pending_locked()) {
        ring_stop_locked(why);
    }
}

/* An alarm with sound has fired. A ring that is going goes on, a minute from now, and its log names this alarm from now on; that is
 * all: no chirp is asked for, since the ring's period asks for one every 1.5 s, which keeps a flood of firings to the one chirp. One
 * that isn't going starts, with its first chirp. The call to the voice task sets a flag and an event-group bit and returns: nothing
 * that waits. */
static void ring_sound_locked(const char *app, const char *name, int64_t now_us) {
    snprintf(s_ring_app, sizeof s_ring_app, "%s", app);
    snprintf(s_ring_name, sizeof s_ring_name, "%s", name);
    s_ring_end_us = now_us + RING_MAX_US;
    if (atomic_load(&s_ringing)) {
        return;
    }
    s_ring_told[0] = '\0';
    s_ring_chirps = 1;
    esp_err_t err = esp_timer_start_periodic(s_ring_timer, RING_PERIOD_US);
    atomic_store(&s_ringing, err == ESP_OK || err == ESP_ERR_INVALID_STATE);  /* already going: the ring only goes on */
    muse_voice_request_chirp();
    journal_locked('I', false, "ring %s/%s chirp %u", s_ring_app, s_ring_name, s_ring_chirps);
}

/* Every APPS_RING_PERIOD_MS while it rings, in the esp_timer task: a chirp, or the end when its minute is over. A tick of a ring
 * that was stopped meanwhile stops the timer, which stop's own call may have been too early for. A tick that can't take the
 * table is lost: the next one is 1.5 s on. */
static void ring_cb(void *arg) {
    (void)arg;
    if (!try_lock()) {
        return;
    }
    if (!atomic_load(&s_ringing)) {
        (void)esp_timer_stop(s_ring_timer);
        unlock();
        return;
    }
    if (esp_timer_get_time() >= s_ring_end_us) {
        ring_stop_locked("timeout");
    } else {
        muse_voice_request_chirp();
        journal_locked('I', false, "ring %s/%s chirp %u", s_ring_app, s_ring_name, ++s_ring_chirps);
    }
    unlock();
}

void apps_alarm_ring_stop(void) {
    if (atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    if (atomic_load(&s_ringing)) {
        ring_stop_locked("sound.stop");
    }
    unlock();
    apps_alarm_log_drain();
}

bool apps_alarm_ringing(void) {
    return atomic_load(&s_ringing);
}

void apps_alarm_app_ended(const char *app, const char *why) {
    if (!id_valid(app) || atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    if (atomic_load(&s_ringing) && strcmp(s_ring_told, app) == 0) {
        s_ring_told[0] = '\0';
        ring_orphaned_locked(why != NULL ? why : "app ended");
    }
    unlock();
    apps_alarm_log_drain();
}

/* ---- The budget (the lock held) ---- */

/* Counts a firing of app at now_us toward its budget; true when it is the one past it, APPS_ALARM_BUDGET_MS having not yet gone since
 * the first of the last APPS_ALARM_BUDGET it had. The one that is over isn't counted: what it costs is its app's alarms, and the window
 * goes on from the firings that were. */
static bool budget_over_locked(const char *app, int64_t now_us) {
    budget_t *b = NULL, *least = &s_budget[0];
    for (int i = 0; i < BUDGET_APPS; i++) {
        if (s_budget[i].n > 0 && strcmp(s_budget[i].app, app) == 0) {
            b = &s_budget[i];
            break;
        }
        if (s_budget[i].n == 0 || (least->n > 0 && s_budget[i].last_us < least->last_us)) {
            least = &s_budget[i];
        }
    }
    if (b == NULL) {
        b = least;
        memset(b, 0, sizeof *b);
        snprintf(b->app, sizeof b->app, "%s", app);
    }
    if (b->n == APPS_ALARM_BUDGET && now_us - b->at_us[b->head] < BUDGET_US) {
        return true;
    }
    if (b->n < APPS_ALARM_BUDGET) {
        b->at_us[(b->head + b->n++) % APPS_ALARM_BUDGET] = now_us;
    } else {
        b->at_us[b->head] = now_us;
        b->head = (uint8_t)((b->head + 1) % APPS_ALARM_BUDGET);
    }
    b->last_us = now_us;
    return false;
}

/* ---- Firing ---- */

/* An alarm's timer, in the esp_timer task (see the top of the file: a stale call finds nothing due and does nothing). Over its app's budget
 * it cancels every alarm of the app, this one too, and that is all. Otherwise the arming is spent and its firing waits in the slot for
 * the app task, which is told by the bit and by the post. A firing that is still waiting when the next one comes (an app that started
 * late, and had armed the name again) is told once, not twice. */
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
    char app[APPS_ID_MAX + 1], name[APPS_ALARM_NAME_MAX + 1];
    snprintf(app, sizeof app, "%s", s->app);
    snprintf(name, sizeof name, "%s", s->name);
    bool sound = s->sound, wake = s->wake;
    if (budget_over_locked(app, now_us)) {
        (void)cancel_app_locked(app);
        note_cancelled_locked(app);  /* this alarm went too, though its slot was freed above: its record goes with the others' */
        journal_locked('W', true, "alarm: %s fired more than %d alarms in a minute; its alarms are cancelled", app, APPS_ALARM_BUDGET);
        ring_orphaned_locked("alarms cancelled");
        unlock();
        return;
    }
    if (!s->fired) {
        s->fired = true;
        s->fired_sound = false;
        s->fired_wake = false;
        s->fired_us = now_us;
        s->fired_due_us = s->due_us;
        s->fired_gen = next_gen();
    }
    s->fired_sound = s->fired_sound || sound;
    s->fired_wake = s->fired_wake || wake;
    atomic_fetch_or(&s_pending, bit_of(s));
    journal_locked('I', true, "alarm %s/%s fired (sound %d, wake %d)", app, name, sound, wake);
    if (sound) {
        ring_sound_locked(app, name, now_us);
    }
    unlock();
    /* What matters first: the wake and the app task's post, which a line must not hold up. The lines are printed by the app task. */
    if (wake) {
        muse_input_wake("alarm");
    }
    fired_cb_t cb = atomic_load(&s_fired_cb);
    if (cb != NULL) {
        cb(app, name);
    }
}

/* ---- The API ---- */

apps_alarm_result_t apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    if (!id_valid(app) || !apps_alarm_name_valid(name) || ms < 1 || ms > APPS_ALARM_MAX_MS) {
        return APPS_ALARM_REFUSED;
    }
    if (!ready()) {
        return APPS_ALARM_NO_MEMORY;
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
            return APPS_ALARM_REFUSED;
        }
    }
    if (s->timer == NULL) {
        const esp_timer_create_args_t args = {.callback = fire_cb,
                                              .arg = (void *)(uintptr_t)(s - s_slot),
                                              .dispatch_method = ESP_TIMER_TASK,
                                              .name = "muse_alarm"};
        if (esp_timer_create(&args, &s->timer) != ESP_OK) {
            s->timer = NULL;  /* the slot is as it was: free, and tried again by the next call */
            unlock();
            return APPS_ALARM_NO_MEMORY;
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
    return armed ? APPS_ALARM_ARMED : APPS_ALARM_REFUSED;
}

void apps_alarm_cancel(const char *app, const char *name) {
    if (!id_valid(app) || !apps_alarm_name_valid(name) || atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    slot_t *s = find(app, name);
    if (s != NULL) {
        (void)esp_timer_stop(s->timer);
        s->used = false;
        clear_fired(s);  /* what fired and wasn't told is dropped too, and the ring it started if that leaves nothing to stop it */
        ring_orphaned_locked("alarm cancelled");
    }
    unlock();
    apps_alarm_log_drain();
}

void apps_alarm_cancel_app(const char *app, const char *why) {
    if (!id_valid(app) || atomic_load(&s_init) != INIT_READY) {
        return;
    }
    lock();
    if (cancel_app_locked(app) > 0) {
        journal_locked('W', true, "alarm: %s %s; its alarms are cancelled", app, why != NULL ? why : "was stopped");
        ring_orphaned_locked("alarms cancelled");
    }
    unlock();
    apps_alarm_log_drain();
}

int64_t apps_alarm_remaining(const char *app, const char *name) {
    if (!id_valid(app) || !apps_alarm_name_valid(name) || atomic_load(&s_init) != INIT_READY) {
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
        out[i].wake = fired[i]->fired_wake;
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
    if (mine) {
        bool sounding = s->fired_sound;
        char app[APPS_ID_MAX + 1];
        snprintf(app, sizeof app, "%s", s->app);
        clear_fired(s);
        if (!delivered) {
            ring_orphaned_locked("alarm dropped");
        } else if (sounding && atomic_load(&s_ringing)) {
            snprintf(s_ring_told, sizeof s_ring_told, "%s", app);  /* it can stop the ring now, and the ring waits for its end */
        }
    }
    unlock();
    apps_alarm_log_drain();
    return mine;
}

bool apps_alarm_wanted(const char *app, uint32_t upto) {
    if (!id_valid(app) || upto == 0 || atomic_load(&s_init) != INIT_READY) {
        return false;
    }
    bool wanted = false;
    lock();
    for (int i = 0; i < APPS_ALARMS_MAX && !wanted; i++) {
        const slot_t *s = &s_slot[i];
        wanted = s->fired && strcmp(s->app, app) == 0 && (int32_t)(upto - s->fired_gen) >= 0;
    }
    unlock();
    return wanted;
}

/* ---- The records' side ---- */

int apps_alarm_list_info(const char *app, apps_alarm_info_t out[APPS_ALARMS_PER_APP]) {
    if (!id_valid(app) || atomic_load(&s_init) != INIT_READY) {
        return 0;
    }
    const slot_t *mine[APPS_ALARMS_PER_APP];
    lock();
    int64_t now_us = esp_timer_get_time();
    int n = collect(mine, APPS_ALARMS_PER_APP, app, false);
    for (int i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof out[i].name, "%s", mine[i]->name);
        out[i].remaining_ms = left_ms(mine[i], now_us);
        out[i].sound = mine[i]->used ? mine[i]->sound : mine[i]->fired_sound;
        out[i].wake = mine[i]->used ? mine[i]->wake : mine[i]->fired_wake;
    }
    unlock();
    return n;
}

int apps_alarm_take_cancelled(char ids[][APPS_ID_MAX + 1], int max) {
    if (atomic_load(&s_init) != INIT_READY || max <= 0) {
        return 0;
    }
    lock();
    int n = s_cancelled_n < max ? s_cancelled_n : max;
    for (int i = 0; i < n; i++) {
        memcpy(ids[i], s_cancelled[i], sizeof s_cancelled[i]);  /* the same size, NUL within */
    }
    memmove(&s_cancelled[0], &s_cancelled[n], (size_t)(s_cancelled_n - n) * sizeof s_cancelled[0]);
    s_cancelled_n -= n;
    unlock();
    return n;
}
