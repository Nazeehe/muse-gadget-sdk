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
 * apps_port.h on the host, for apphost (sha256.c has apps_port_sha256).
 *
 * The clocks are fake and move only when a script says so (apphost_advance): a run doesn't depend on how fast the machine
 * is, and time stands still inside a call, so the VM's wall-clock backstop (APPS_WALL_MS) never fires and a runaway app is
 * stopped by its instruction budget alone, the same way on every run. The monotonic clock starts at FAKE_MONO_MS; the wall
 * clock is unset until a script syncs it (as on the gadget before SNTP), and then moves with the monotonic one. The
 * screen is lit until a script says it is dark. The alarms (the system's, as apps_alarm.c is on the gadget) are on the same
 * clock.
 *
 * The heap is malloc with a header that records each block's size, so that apphost can say how many bytes the
 * apps' heap holds (apphost_heap_held): a leak check that needs no LeakSanitizer, which macOS doesn't have.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apphost_port.h"
#include "apps_alarm.h"
#include "apps_port.h"

/* Not 0: a clock at 0 reads as "unset" in places. */
#define FAKE_MONO_MS 1000

/* A block's size, in front of it, padded so that the block keeps malloc's alignment. */
typedef union {
    size_t size;
    max_align_t align;
} header_t;

static size_t held;
static apphost_log_fn log_fn;

static int64_t mono_ms = FAKE_MONO_MS;
static bool epoch_set, screen_dark;
static int64_t epoch_at_sync, mono_at_sync;

int64_t apps_port_mono_ms(void) {
    return mono_ms;
}

bool apps_port_epoch_ms(int64_t *out) {
    if (!epoch_set) {
        return false;
    }
    *out = epoch_at_sync + (mono_ms - mono_at_sync);
    return true;
}

bool apps_port_dark(void) {
    return screen_dark;
}

void apphost_advance(int64_t ms) {
    mono_ms += ms;
}

void apphost_sync(int64_t epoch_ms) {
    epoch_set = true;
    epoch_at_sync = epoch_ms;
    mono_at_sync = mono_ms;
}

void apphost_set_dark(bool dark) {
    screen_dark = dark;
}

/* ---- The system's alarms and sound ---- */

/* The table of apps_alarm.c, without its timers: an alarm is due when the clock reaches it, and apphost.c asks after each step. */
typedef struct {
    bool used, sound, wake;
    char app[APPS_ID_MAX + 1];
    char name[APPS_ALARM_NAME_MAX + 1];
    int64_t due_ms;
    uint64_t order;  /* armed in this order: of two due together the first runs first */
} alarm_t;

static alarm_t alarms[APPS_ALARMS_MAX];
static uint64_t alarm_order;
static const apphost_sound_t *sound_sink;
static bool ringing;
static int64_t ring_end_ms;

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

static alarm_t *find_alarm(const char *app, const char *name) {
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        if (alarms[i].used && strcmp(alarms[i].app, app) == 0 && strcmp(alarms[i].name, name) == 0) {
            return &alarms[i];
        }
    }
    return NULL;
}

static bool runs_first(const alarm_t *a, const alarm_t *b) {
    return a->due_ms < b->due_ms || (a->due_ms == b->due_ms && a->order < b->order);
}

bool apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    size_t app_len = app != NULL ? strlen(app) : 0;
    if (app_len < 1 || app_len > APPS_ID_MAX || !name_valid(name) || ms < 1 || ms > APPS_ALARM_MAX_MS) {
        return false;
    }
    alarm_t *a = find_alarm(app, name);
    if (a == NULL) {
        int mine = 0;
        for (int i = 0; i < APPS_ALARMS_MAX; i++) {
            mine += alarms[i].used && strcmp(alarms[i].app, app) == 0 ? 1 : 0;
            a = a == NULL && !alarms[i].used ? &alarms[i] : a;
        }
        if (mine >= APPS_ALARMS_PER_APP || a == NULL) {
            return false;
        }
    }
    *a = (alarm_t){.used = true, .sound = sound, .wake = wake, .due_ms = mono_ms + ms, .order = ++alarm_order};
    snprintf(a->app, sizeof a->app, "%s", app);
    snprintf(a->name, sizeof a->name, "%s", name);
    return true;
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    alarm_t *a = name_valid(name) ? find_alarm(app, name) : NULL;
    if (a != NULL) {
        a->used = false;
    }
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    const alarm_t *a = name_valid(name) ? find_alarm(app, name) : NULL;
    if (a == NULL) {
        return -1;
    }
    return a->due_ms > mono_ms ? a->due_ms - mono_ms : 0;
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    const alarm_t *mine[APPS_ALARMS_PER_APP];
    int n = 0;
    for (int i = 0; i < APPS_ALARMS_MAX && n < APPS_ALARMS_PER_APP; i++) {
        if (alarms[i].used && strcmp(alarms[i].app, app) == 0) {
            int at = n++;
            for (; at > 0 && runs_first(&alarms[i], mine[at - 1]); at--) {
                mine[at] = mine[at - 1];
            }
            mine[at] = &alarms[i];
        }
    }
    for (int i = 0; i < n; i++) {
        snprintf(names[i], 24, "%s", mine[i]->name);
        remaining[i] = mine[i]->due_ms > mono_ms ? mine[i]->due_ms - mono_ms : 0;
    }
    return n;
}

void apphost_set_sound(const apphost_sound_t *sink) {
    sound_sink = sink;
}

bool apphost_alarm_next_due(apphost_alarm_t *out) {
    alarm_t *pick = NULL;
    for (int i = 0; i < APPS_ALARMS_MAX; i++) {
        if (alarms[i].used && alarms[i].due_ms <= mono_ms && (pick == NULL || runs_first(&alarms[i], pick))) {
            pick = &alarms[i];
        }
    }
    if (pick == NULL) {
        return false;
    }
    snprintf(out->app, sizeof out->app, "%s", pick->app);
    snprintf(out->name, sizeof out->name, "%s", pick->name);
    out->sound = pick->sound;
    out->wake = pick->wake;
    pick->used = false;
    return true;
}

static void set_ringing(bool on) {
    if (ringing != on) {
        ringing = on;
        if (sound_sink != NULL && sound_sink->ring != NULL) {
            sound_sink->ring(on);
        }
    }
}

void apphost_ring(void) {
    ring_end_ms = mono_ms + APPS_RING_MAX_MS;
    set_ringing(true);
}

void apphost_ring_tick(void) {
    if (ringing && mono_ms >= ring_end_ms) {
        set_ringing(false);
    }
}

void apps_port_sound_chirp(void) {
    if (sound_sink != NULL && sound_sink->chirp != NULL) {
        sound_sink->chirp();
    }
}

void apps_port_sound_stop(void) {
    set_ringing(false);
}

static bool ui_locked;
static unsigned long ui_takes;

/* A violation of the lock's rules is a bug in the core, and ends the run: apphost's tests (and every run of an
 * app) have an empty stderr, so none goes unseen. */
static void lock_violation(const char *what) {
    fprintf(stderr, "apphost: %s\n", what);
    abort();
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (ui_locked) {  /* the heap is Lua's too: a Lua call under the lock would land here */
        lock_violation("the apps' heap was used while the UI lock was held");
    }
    header_t *old = ptr != NULL ? (header_t *)ptr - 1 : NULL;
    if (n == 0 || n > SIZE_MAX - sizeof(header_t)) {  /* never n == 0 (apps_port.h) */
        return NULL;
    }
    size_t was = old != NULL ? old->size : 0;  /* before realloc frees the block */
    header_t *block = realloc(old, sizeof(header_t) + n);
    if (block == NULL) {
        return NULL;
    }
    block->size = n;
    held = held - was + n;
    return block + 1;
}

void apps_port_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (ui_locked) {  /* a block is let go of after the lock is released, so the lock is held for short, bounded work */
        lock_violation("a block of the apps' heap was freed while the UI lock was held");
    }
    header_t *block = (header_t *)ptr - 1;
    held -= block->size;
    free(block);
}

size_t apps_port_psram_free(void) {
    return SIZE_MAX;  /* the host's heap: no store here to make room in */
}

void apps_port_log(const char *app_id, const char *line) {
    if (log_fn != NULL) {
        log_fn(app_id, line);
    }
}

/* apps_port_log prints at once: nothing waits for a drain. */
void apps_port_log_drain(void) {}

/* The UI lock guards the widget table against the LVGL task; there is no other task here, so it only keeps
 * count, to check that the core holds it by the rules (apps_ui.h): never twice, never across an allocation. */
void apps_port_ui_lock(void) {
    if (ui_locked) {
        lock_violation("the UI lock was taken while it was held");
    }
    ui_locked = true;
    ui_takes++;
}

bool apps_port_ui_trylock(int ms) {
    (void)ms;
    apps_port_ui_lock();
    return true;
}

void apps_port_ui_unlock(void) {
    if (!ui_locked) {
        lock_violation("the UI lock was given back and wasn't held");
    }
    ui_locked = false;
}

bool apphost_ui_locked(void) {
    return ui_locked;
}

unsigned long apphost_ui_lock_takes(void) {
    return ui_takes;
}

void apphost_set_log(apphost_log_fn fn) {
    log_fn = fn;
}

size_t apphost_heap_held(void) {
    return held;
}
