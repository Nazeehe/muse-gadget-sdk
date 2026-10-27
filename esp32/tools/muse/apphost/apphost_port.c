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
 * The clock is fake and never moves: a run doesn't depend on how fast the machine is, so the VM's wall-clock
 * backstop (APPS_WALL_MS) never fires and a runaway app is stopped by its instruction budget alone, the same
 * way on every run. (A later task that needs time to pass, for timers, adds a way to move it.)
 *
 * The heap is malloc with a header that records each block's size, so that apphost can say how many bytes the
 * apps' heap holds (apphost_heap_held): a leak check that needs no LeakSanitizer, which macOS doesn't have.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "apphost_port.h"
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

int64_t apps_port_mono_ms(void) {
    return FAKE_MONO_MS;
}

bool apps_port_epoch_ms(int64_t *out) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return false;
    }
    *out = (int64_t)now * 1000;
    return true;
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

void apps_port_log(const char *app_id, const char *line) {
    if (log_fn != NULL) {
        log_fn(app_id, line);
    }
}

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
