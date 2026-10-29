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
 * apps_port.h on the gadget. The heap is PSRAM; the clock is esp_timer's; an app's log line goes to the console as
 * "@app.log <id> <line>", at most 1,024 bytes of them a second, queued in the call and printed after it; the UI lock is
 * a static FreeRTOS mutex that muse_apps_start() makes (apps_port_init); the system's alarms are apps_alarm.c's, and a chirp
 * is the voice task's. An app's store is a map in PSRAM that the I/O worker keeps (below); sys's battery and Wi-Fi are
 * muse_state's and muse_wifi's, and its show, keep_awake and exit are the app task's (apps_task.h).
 *
 * The UI lock keeps the host runner's contract (tools/muse/apphost/apphost_port.c): it is never taken twice by one
 * task, given back only by its holder, and nothing uses the heap while it is held. The host aborts on a breach; here a
 * breach is logged once for each kind and carried past, never into a deadlock or one of FreeRTOS's asserts: a second
 * take by the holder is skipped, and so is a give by a task that doesn't hold it.
 */

#include "apps_port.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "apps_alarm.h"
#include "apps_echo.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_task.h"
#include "muse_mem.h"
#include "muse_sntp.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "muse_apps";

static StaticSemaphore_t s_ui_lock_buf;
static SemaphoreHandle_t s_ui_lock;
static TaskHandle_t volatile s_ui_holder;  /* written by the holder alone, under the lock */

/* The kinds of breach, each logged once: a harmless first one of one kind can't hide a later one of another. */
typedef enum { BREACH_RETAKE, BREACH_GIVE, BREACH_HEAP } breach_t;
static const char *const BREACH_KINDS[] = {"re-take", "give by a non-holder", "heap use under the lock"};
static atomic_uint s_breaches_logged;  /* a bit for each kind */

static void breach(breach_t kind, const char *what) {
    unsigned bit = 1u << kind;
    if ((atomic_fetch_or(&s_breaches_logged, bit) & bit) == 0) {
        ESP_LOGE(TAG, "UI lock breach, %s: %s (each kind is logged once)", BREACH_KINDS[kind], what);
    }
}

static bool held_by_me(void) {
    return s_ui_holder != NULL && s_ui_holder == xTaskGetCurrentTaskHandle();
}

static char *s_echo_queue;  /* APPS_ECHO_QUEUE bytes of PSRAM, made once: see the echo below */

void apps_port_init(void) {
    if (s_ui_lock == NULL) {
        s_ui_lock = xSemaphoreCreateMutexStatic(&s_ui_lock_buf);
    }
    if (s_echo_queue == NULL) {
        s_echo_queue = heap_caps_malloc(APPS_ECHO_QUEUE, MUSE_BIG_CAPS);  /* none: nothing is echoed, all is counted */
    }
}

int64_t apps_port_mono_ms(void) {
    return esp_timer_get_time() / 1000;
}

bool apps_port_epoch_ms(int64_t *out) {
    struct timeval tv;
    if (!muse_time_valid() || gettimeofday(&tv, NULL) != 0) {
        return false;
    }
    *out = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return true;
}

bool apps_port_dark(void) {
    return muse_ui_dark();
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (held_by_me()) {
        breach(BREACH_HEAP, "the heap was used with the lock held");
    }
    return heap_caps_realloc(ptr, n, MUSE_BIG_CAPS);
}

size_t apps_port_psram_free(void) {
    return heap_caps_get_free_size(MUSE_BIG_CAPS);
}

void apps_port_free(void *ptr) {
    if (ptr != NULL && held_by_me()) {
        breach(BREACH_HEAP, "a block was freed with the lock held");
    }
    heap_caps_free(ptr);
}

/*
 * The console echo. On the 1.75C the console is UART0 at 115200 with no driver installed, so printf busy-waits on the
 * UART's FIFO, about 87 us a byte: a whole line (236 bytes) is about 20 ms. So the echo takes at most 1,024 bytes a
 * second (apps_echo.h). A line that doesn't fit isn't echoed and is counted; the next echoed line comes after
 * "@app.log <id> (<N> lines not echoed)", which takes its bytes too, <id> being the app of the last line not echoed.
 * The log ring and the status keep every line.
 *
 * The app task never waits for the console inside a call (Task 13's F3). picolibc holds stdout's lock for a whole
 * printf, so a print there would wait behind another task's 6 KB reply (about 0.53 s) with the call's 1 s wall clock
 * running. So apps_port_log only queues the line, with its note, in a block of APPS_ECHO_QUEUE bytes as the console
 * sends them (CRLF counted): what the bucket lets a call echo, a burst and a second of the rate. A line the queue has no
 * room for is counted as one past the bucket is. apps_port_log_drain, which the app task calls after every call and in
 * its housekeeping, prints each queued entry with one printf (so another task's line can't land inside the note or the
 * line) and ends it with apps_port_console_flush, as a reply ends, so the host doesn't keep one that lands on a 64-byte
 * USB packet. The app task is the only caller of both, so the bucket, the count and the queue are its own.
 *
 * The drain holds stdout's lock once, for the whole queue (Task 13's N1). Taking it again for each line, the app task
 * (priority 2) would lose the console after every line to a waiting writer such as the serial task (priority 5), and
 * wait behind one long reply per line: tens of seconds under back-to-back statuses. picolibc's stdio lock is recursive
 * (ESP-IDF's esp_libc locks.c makes it a FreeRTOS recursive mutex; POSIX's flockfile is recursive on the host too), so
 * the printf and the flush inside take it again freely. So a drain waits behind at most one other writer's line, and
 * another writer waits behind at most the queue, APPS_ECHO_QUEUE bytes (about 0.18 s at the UART's pace), and its
 * flushes. With nothing queued, the drain never takes the lock.
 */
#define ECHO_EOL_BYTES 2  /* the console ends a line with CRLF (CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF) */

static apps_echo_t s_echo;
static uint32_t s_unechoed;                     /* lines not echoed since the last echoed one */
static char s_unechoed_id[APPS_ID_MAX + 1];
static size_t s_echo_used;                      /* bytes of s_echo_queue used: entries, each NUL-terminated */
static size_t s_echo_bytes;                     /* what they are as the console sends them, at most APPS_ECHO_QUEUE */

void apps_port_log(const char *app_id, const char *line) {
    char note[APPS_ID_MAX + 64] = "";
    size_t bytes = 0;
    if (s_unechoed > 0) {
        int n = snprintf(note, sizeof note, "@app.log %s (%" PRIu32 " lines not echoed)\n", s_unechoed_id, s_unechoed);
        bytes = n > 0 ? (size_t)n - 1 + ECHO_EOL_BYTES : 0;
    }
    bytes += sizeof "@app.log " - 1 + strlen(app_id) + 1 + strlen(line) + ECHO_EOL_BYTES;
    /* An entry takes at most its console bytes in the block (a newline for each CRLF, and one NUL). */
    if (s_echo_queue == NULL || s_echo_bytes + bytes > APPS_ECHO_QUEUE ||
        !apps_echo_take(&s_echo, apps_port_mono_ms(), bytes)) {
        s_unechoed = s_unechoed < UINT32_MAX ? s_unechoed + 1 : s_unechoed;
        snprintf(s_unechoed_id, sizeof s_unechoed_id, "%s", app_id);
        return;
    }
    int n = snprintf(s_echo_queue + s_echo_used, APPS_ECHO_QUEUE - s_echo_used, "%s@app.log %s %s\n", note, app_id, line);
    s_echo_used += (size_t)n + 1;
    s_echo_bytes += bytes;
    s_unechoed = 0;
}

void apps_port_log_drain(void) {
    if (s_echo_used == 0) {
        return;  /* nothing queued: the housekeeping never waits for the console */
    }
    flockfile(stdout);  /* once for the whole queue (Task 13's N1): see the echo above */
    for (size_t at = 0; at < s_echo_used;) {
        const char *entry = s_echo_queue + at;
        printf("%s", entry);
        apps_port_console_flush();
        at += strlen(entry) + 1;
    }
    funlockfile(stdout);
    s_echo_used = 0;
    s_echo_bytes = 0;
}

/*
 * The end of a console reply. The USB-Serial-JTAG port sends a line as 64-byte packets, flushing at its newline, and the
 * host hands a transfer to the reader only once a packet shorter than 64 bytes ends it. A line whose length, CRLF
 * included, is a multiple of 64 ends on a full packet, so the host kept it until the console printed again. ESP-IDF's
 * console forwards fsync to that port, which waits (50 ms at most) for the host to take the last packet and then sends
 * a zero-length one. The UART's fsync waits for its FIFO to empty, about 11 ms at most at 115200 baud.
 *
 * Both calls are made holding stdout's lock. fflush takes it for itself, but fsync takes none, and its flushes reach
 * the USB port's FIFO while another task may be filling it. A screenshot (muse_console_hold) streams through that
 * FIFO, and an fsync that began just before its hold lost a byte of the stream's first rows: about one capture in forty
 * with the app echoing 70 lines a second. Held, such an fsync is over before the screenshot takes the console, and
 * none begins until it lets go.
 */
void apps_port_console_flush(void) {
    flockfile(stdout);
    fflush(stdout);
    (void)fsync(fileno(stdout));
    funlockfile(stdout);
}

void apps_port_ui_lock(void) {
    if (held_by_me()) {
        breach(BREACH_RETAKE, "taken again by its holder");
        return;
    }
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_ui_holder = xTaskGetCurrentTaskHandle();
}

bool apps_port_ui_trylock(int ms) {
    if (held_by_me()) {
        breach(BREACH_RETAKE, "tried again by its holder");
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
        breach(BREACH_GIVE, "given back by a task that doesn't hold it");
        return;
    }
    s_ui_holder = NULL;
    xSemaphoreGive(s_ui_lock);
}

/* The alarms and the sound: apps_alarm.c keeps the first (its table, its timers and its ring), the voice task plays the second. */

apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    return apps_alarm_arm(app, name, ms, sound, wake);
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    apps_alarm_cancel(app, name);
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    return apps_alarm_remaining(app, name);
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    return apps_alarm_list(app, names, remaining);
}

/* A flag and a bit of an event group, whichever task asks: the voice task chirps when it is idle, at the volume that is set. */
void apps_port_sound_chirp(void) {
    muse_voice_request_chirp();
}

void apps_port_sound_stop(void) {
    apps_alarm_ring_stop();
}

/*
 * The store of an app (apps_port.h): until Task 20 gives the values flash, a list of nodes in PSRAM, one for each app and at most
 * STORE_APPS, each holding the app's one saved object. Only the I/O worker touches the list (X11: Task 20's flash is the worker's,
 * reads included), so it takes no lock: the app task's calls queue ops (apps_io_submit_op), which the worker runs in the order they
 * were queued, so a load never reads before a save that came ahead of it has been written (a restart's load follows the stop's save).
 *
 * A save makes its node on the calling task, with a copy of the bytes, so that a want of memory is the caller's to try again, and the
 * op links it in place of the app's old one: nothing in it can fail. A load makes a request, which its op fills with a copy of the
 * object, and polls for the answer, at most APPS_STORE_WAIT_MS. Who owns the request is decided by one atomic: the op marks it done, the
 * caller that gives up marks it abandoned, whichever finds the other's mark frees it, so a worker that answers late finds nobody
 * waiting, and frees the request and its answer.
 */
#define STORE_APPS 16                   /* apps whose values are kept; a new one past that drops the oldest saved */
#define STORE_OBJECT_MAX APPS_STORE_MAX_BYTES  /* the runtime saves no more, and reads back no more (apps_rt.h): the map never holds a runaway */
#define STORE_POLL_MS 5                 /* how often a load looks for its answer */

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
        apps_port_free(r);
    }
}

char *apps_port_store_load(const char *app, size_t *len) {
    *len = 0;
    if (strlen(app) > APPS_ID_MAX) {
        return NULL;  /* an id the packages can't have */
    }
    store_load_t *r = apps_port_realloc(NULL, sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    atomic_init(&r->state, LOAD_WAITING);
    snprintf(r->app, sizeof r->app, "%s", app);
    r->json = NULL;
    r->len = 0;
    if (!apps_io_submit_op(store_get_op, r)) {
        apps_port_free(r);  /* refused: the worker never saw it (apps_io_submit_op says why in its log line) */
        return NULL;
    }
    int64_t until = apps_port_mono_ms() + APPS_STORE_WAIT_MS;
    while (atomic_load(&r->state) != LOAD_DONE) {
        if (apps_port_mono_ms() >= until) {
            int waiting = LOAD_WAITING;
            if (atomic_compare_exchange_strong(&r->state, &waiting, LOAD_ABANDONED)) {
                ESP_LOGW(TAG, "store: no answer from the I/O worker for %s within %d ms: launch refused", app, APPS_STORE_WAIT_MS);
                return NULL;  /* the worker frees the request when it comes to it */
            }
            break;  /* it answered as the wait ran out */
        }
        vTaskDelay(pdMS_TO_TICKS(STORE_POLL_MS));
    }
    char *json = r->json;
    *len = r->len;
    apps_port_free(r);
    return json;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    if (strlen(app) > APPS_ID_MAX || len > STORE_OBJECT_MAX) {
        ESP_LOGW(TAG, "store: refused %u bytes of values for %s: more than the runtime reads back", (unsigned)len, app);
        return false;
    }
    store_node_t *n = apps_port_realloc(NULL, sizeof *n + len + 1);
    if (n == NULL) {
        return false;  /* no memory for the copy: the caller asks again */
    }
    n->next = NULL;
    n->seq = 0;
    n->len = len;
    snprintf(n->app, sizeof n->app, "%s", app);
    memcpy(n->json, json, len);
    n->json[len] = '\0';
    if (!apps_io_submit_op(store_put_op, n)) {
        apps_port_free(n);
        return false;
    }
    return true;
}

/* sys.battery(): muse_state's, as the status bar reads it; a percentage outside 0 to 100 is clamped, none is -1. */
apps_battery_t apps_port_battery(void) {
    muse_power_t p = muse_state_power();
    int pct = p.battery_pct < 0 ? -1 : (p.battery_pct > 100 ? 100 : p.battery_pct);
    return (apps_battery_t){.pct = pct, .charging = p.charging, .usb = p.usb};
}

/* sys.wifi(): connected only when the station has joined; the name is the network's, and never logged (X14). */
bool apps_port_wifi(char *ssid, size_t cap) {
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool up = w.state == MUSE_WIFI_CONNECTED;
    w.ssid[sizeof w.ssid - 1] = '\0';
    if (cap > 0) {
        snprintf(ssid, cap, "%s", up ? w.ssid : "");
    }
    return up;
}

/* sys.show, sys.keep_awake and sys.exit are the app task's own (apps_task.h): they touch what only it holds. */
void apps_port_show(void) {
    apps_task_show();
}

void apps_port_keep_awake(bool on) {
    apps_task_keep_awake(on);
}

void apps_port_exit(void) {
    apps_task_exit();
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
