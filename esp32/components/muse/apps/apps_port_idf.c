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
 * is the voice task's. An app's store is apps_values.c's (apps_values.h): apps_nvs on a gadget with the apps partition, a map in PSRAM
 * otherwise, behind the I/O worker; an alarm that is armed or cancelled is a name its app touched, for its record
 * (apps_values_alarm_touched); sys's battery and Wi-Fi are muse_state's and muse_wifi's, and its show, keep_awake and exit are the app
 * task's (apps_task.h).
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

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "apps_alarm.h"
#include "apps_echo.h"
#include "apps_pkg.h"
#include "apps_task.h"
#include "apps_values.h"
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

/*
 * The floor (C1, apps_port.h): growth that would leave the rest of the gadget less than APPS_RAM_RESERVE of PSRAM is refused,
 * so an app, a package's read and the install's blocks run out of memory cleanly and LVGL (which asserts) never does because
 * of them. Free PSRAM is a counter; the largest free block is a walk of the heap under its lock, so only a big request asks,
 * which is also the only kind that can take a big piece of the largest block. The check and the allocation aren't atomic:
 * another task's allocation in between can take a little of the reserve, which is why it is a reserve and not an edge.
 */
static bool room_to_grow(void *ptr, size_t n) {
    size_t had = ptr != NULL ? heap_caps_get_allocated_size(ptr) : 0;
    if (n <= had) {
        return true;  /* a shrink, or growth within the block: never refused (vm_alloc takes a refused shrink for running out) */
    }
    size_t grow = n - had, free_now = heap_caps_get_free_size(MUSE_BIG_CAPS);
    if (free_now < grow || free_now - grow < APPS_RAM_RESERVE) {
        return false;
    }
    if (n >= APPS_FLOOR_BIG_BLOCK) {
        size_t largest = heap_caps_get_largest_free_block(MUSE_BIG_CAPS);
        if (largest < n || largest - n < APPS_RAM_RESERVE) {
            return false;
        }
    }
    return true;
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (held_by_me()) {
        breach(BREACH_HEAP, "the heap was used with the lock held");
    }
    if (!room_to_grow(ptr, n)) {
        return NULL;
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

static EXT_RAM_BSS_ATTR apps_echo_t s_echo;    /* the app task's alone: PSRAM (I2) */
static uint32_t s_unechoed;                     /* lines not echoed since the last echoed one */
static EXT_RAM_BSS_ATTR char s_unechoed_id[APPS_ID_MAX + 1];
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

/* The alarms and the sound: apps_alarm.c keeps the first (its table, its timers and its ring), the voice task plays the second. An arming
 * or a cancel touches the name (apps_values.h): its record follows at a tick of the app task after the call, and a record the mount handed
 * over for it is armed again no more (the app's choice wins). Nothing here waits. */

apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    apps_alarm_result_t got = apps_alarm_arm(app, name, ms, sound, wake);
    apps_values_alarm_touched(app, name);  /* a refusal can have dropped a replaced arming too (apps_alarm.h) */
    return got;
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    apps_alarm_cancel(app, name);
    apps_values_alarm_touched(app, name);
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

/* The store of an app: apps_values.c keeps the objects (apps_values.h), behind the I/O worker. */

char *apps_port_store_load(const char *app, size_t *len) {
    char *json = apps_values_load(app, len);
    apps_task_call_restart();  /* what the load waited for is the gadget's: the call that makes the app is counted from here (apps_port.h) */
    return json;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    return apps_values_save(app, json, len);
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
