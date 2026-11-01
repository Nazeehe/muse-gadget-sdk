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
 * screen is lit until a script says it is dark. The alarms (the system's: apps_alarm.c itself, over the fakes of apphost_esp.c) are on
 * the same clock.
 *
 * The heap is malloc with a header that records each block's size, so that apphost can say how many bytes the
 * apps' heap holds (apphost_heap_held): a leak check that needs no LeakSanitizer, which macOS doesn't have.
 *
 * The store's objects are kept in the process, one for each app (a plain malloc: not the apps' heap, which a leak check counts),
 * and given back by apphost_store_clear; a load returns a copy from the apps' heap, which the runtime frees. The battery and the
 * Wi-Fi are fixed, so that a run is the same everywhere; show is told to the sink; the exit is a flag apphost.c takes.
 *
 * The VM: none, unless apphost_set_vm gives a folder that stands in for the app's folder on the VM (apphost --vm DIR).
 */

/* mkdir comes with POSIX: a C library that hides what C11 doesn't name declares it only when asked (as apphost.c says). */
#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apphost_esp.h"
#include "apphost_port.h"
#include "apps_alarm.h"
#include "apps_ask.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_vmio.h"

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

/* The table is apps_alarm.c's (see apphost_port.h): what the Lua bindings ask of the port goes to it, and the ring is told to the sink. */
static const apphost_sound_t *sound_sink;
static bool ring_shown;

apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    return apps_alarm_arm(app, name, ms, sound, wake);
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    apps_alarm_cancel(app, name);
    apphost_ring_sync();  /* a firing that waits is dropped with it, and a ring it started may end */
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    return apps_alarm_remaining(app, name);
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    return apps_alarm_list(app, names, remaining);
}

void apphost_set_sound(const apphost_sound_t *sink) {
    sound_sink = sink;
}

void apphost_ring_sync(void) {
    bool on = apps_alarm_ringing();
    if (on != ring_shown) {
        ring_shown = on;
        if (sound_sink != NULL && sound_sink->ring != NULL) {
            sound_sink->ring(on);
        }
    }
}

void apps_port_sound_chirp(void) {
    if (sound_sink != NULL && sound_sink->chirp != NULL) {
        sound_sink->chirp();
    }
}

void apps_port_sound_stop(void) {
    apps_alarm_ring_stop();
    apphost_ring_sync();
}

/* What an alarm asks of the rest of the gadget (apps_alarm.c calls them from its timer): the screen wakes, which here is lit; the voice
 * task's chirp for the ring has no counterpart (the ring is an event, and a chirp is the app's own sound.chirp). */
void muse_input_wake(const char *why) {
    (void)why;
    apphost_set_dark(false);
}

void muse_voice_request_chirp(void) {}

static bool ui_locked;
static unsigned long ui_takes;

/* A violation of the lock's rules is a bug in the core, and ends the run: apphost's tests (and every run of an
 * app) have an empty stderr, so none goes unseen. */
static void lock_violation(const char *what) {
    fprintf(stderr, "apphost: %s\n", what);
    abort();
}

/* ---- The store, and what sys asks of the gadget ---- */

typedef struct saved {
    struct saved *next;
    char app[APPS_ID_MAX + 1];
    char *json;
    size_t len;
} saved_t;

static saved_t *saved_head;
static const apphost_system_t *system_sink;
static bool exit_asked;

void apphost_set_system(const apphost_system_t *sink) {
    system_sink = sink;
}

char *apps_port_store_load(const char *app, size_t *len) {
    const char *json = "{}";
    size_t n = 2;
    for (const saved_t *s = saved_head; s != NULL; s = s->next) {
        if (strcmp(s->app, app) == 0) {
            json = s->json;
            n = s->len;
            break;
        }
    }
    char *copy = apps_port_realloc(NULL, n + 1);
    if (copy == NULL) {
        *len = 0;
        return NULL;
    }
    memcpy(copy, json, n);
    copy[n] = '\0';
    *len = n;
    return copy;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    saved_t *s = saved_head;
    while (s != NULL && strcmp(s->app, app) != 0) {
        s = s->next;
    }
    if (s == NULL) {
        s = calloc(1, sizeof *s);
        if (s == NULL) {
            free(copy);
            return false;
        }
        snprintf(s->app, sizeof s->app, "%s", app);
        s->next = saved_head;
        saved_head = s;
    }
    free(s->json);
    s->json = copy;
    s->len = len;
    if (system_sink != NULL && system_sink->store_saved != NULL) {
        system_sink->store_saved(len);
    }
    return true;
}

void apphost_store_clear(void) {
    while (saved_head != NULL) {
        saved_t *s = saved_head;
        saved_head = s->next;
        free(s->json);
        free(s);
    }
}

apps_battery_t apps_port_battery(void) {
    return (apps_battery_t){.pct = 76, .charging = true, .usb = true};
}

bool apps_port_wifi(char *ssid, size_t cap) {
    snprintf(ssid, cap, "muse-lab");
    return true;
}

void apps_port_show(void) {
    if (system_sink != NULL && system_sink->show != NULL) {
        system_sink->show();
    }
}

void apps_port_keep_awake(bool on) {
    if (system_sink != NULL && system_sink->keep_awake != NULL) {
        system_sink->keep_awake(on);  /* nothing on a laptop goes to sleep: the request is only told */
    }
}

void apps_port_exit(void) {
    exit_asked = true;
}

/*
 * The VM. Without a folder apphost has none: every vm request is refused, in the words the gadget uses for a VM it can't reach
 * (apps_vmio.h). With one (apphost --vm DIR), the folder stands in for the app's folder on the VM, workspace/apps/<id>/: a request
 * runs at once, as the network worker would run it, and its result waits for apphost_vm_take, as the worker's results wait for the
 * app task. A read gives the file's bytes (the gadget's words for a missing file or one over the kind's limit), and a write
 * replaces the file whole, making its folders. A JPEG isn't decoded here (the gadget's decoder is the ROM's): it is refused.
 */
#define VM_PATH_MAX 4096

static const char *vm_dir;
static struct apps_vm_job *vm_done, **vm_done_tail = &vm_done;

void apphost_set_vm(const char *dir) {
    vm_dir = dir;
}

struct apps_vm_job *apphost_vm_take(void) {
    struct apps_vm_job *list = vm_done;
    vm_done = NULL;
    vm_done_tail = &vm_done;
    return list;
}

static void vm_read(apps_vm_job_t *job, const char *path, const char *rel) {
    if (job->as == APPS_VM_AS_JPEG) {
        snprintf(job->err, sizeof job->err, "apphost doesn't decode JPEGs: read %s on the gadget", rel);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(job->err, sizeof job->err, APPS_VM_MSG_NO_FILE, rel);
        return;
    }
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    uint8_t *data = NULL;
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        snprintf(job->err, sizeof job->err, "apphost can't read %s: %s", rel, strerror(errno));
    } else if ((unsigned long)n > job->max) {
        snprintf(job->err, sizeof job->err, APPS_VM_MSG_TOO_LARGE, rel, (unsigned)(job->max / 1024));
    } else if (n > 0 && (data = apps_port_realloc(NULL, (size_t)n)) == NULL) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NO_MEMORY);
    } else if (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) {
        snprintf(job->err, sizeof job->err, "apphost can't read %s", rel);
        apps_port_free(data);
    } else {
        job->data = data;  /* NULL for an empty file, as the worker gives it */
        job->len = (size_t)n;
        job->ok = true;
    }
    fclose(f);
}

static void vm_write(apps_vm_job_t *job, const char *path, const char *rel) {
    char dir[VM_PATH_MAX], tmp[VM_PATH_MAX + 8];
    for (const char *slash = strchr(rel, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        snprintf(dir, sizeof dir, "%s/%.*s", vm_dir, (int)(slash - rel), rel);
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            snprintf(job->err, sizeof job->err, "apphost can't make %s: %s", dir, strerror(errno));
            return;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL && fwrite(job->data, 1, job->len, f) == job->len;
    ok = (f != NULL && fclose(f) == 0) && ok && rename(tmp, path) == 0;
    if (!ok) {
        snprintf(job->err, sizeof job->err, "apphost can't write %s: %s", rel, strerror(errno));
        remove(tmp);
        return;
    }
    job->ok = true;
}

bool apps_port_vm_submit(struct apps_vm_job *job) {
    if (vm_dir == NULL) {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NOT_CONNECTED);
        return false;
    }
    const char *rel = job->path + job->rel;
    char path[VM_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", vm_dir, rel);
    job->ok = false;
    job->err[0] = '\0';
    if (job->op == APPS_VM_READ) {
        vm_read(job, path, rel);
    } else {
        vm_write(job, path, rel);
    }
    job->next = NULL;
    *vm_done_tail = job;
    vm_done_tail = &job->next;
    return true;
}

/* apphost has no Muse: every ask's turn ends at once, in the words the gadget uses when Muse isn't set up (muse_chat.h), and its
 * callback runs on the next `advance` (apphost.c: run_ask). */
void apps_port_ask(const char *msg, size_t len, void *user) {
    (void)msg, (void)len;
    apps_ask_done(user, "not set up");
}

/* Every turn ended at once above, so there is never one to cancel (P30). */
void apps_port_ask_cancel(void *user) {
    (void)user;
}

bool apphost_take_exit(void) {
    bool asked = exit_asked;
    exit_asked = false;
    return asked;
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
