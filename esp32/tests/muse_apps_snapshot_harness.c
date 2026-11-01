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
 * The host harness of Link's app.control snapshot (components/muse/apps/apps_snapshot.c), for
 * tests/test_muse_apps_snapshot.py: one case a run, named on the command line. The real apps_snapshot.c runs over fakes:
 * - the I/O worker: apps_io_submit queues the job, which the case runs (run_jobs), on its own thread or, for the abandoned
 *   drawing, while a second thread plays the LVGL task;
 * - the clock (apps_port_mono_ms) stands still but for what vTaskDelay sleeps (a tick is a millisecond) and what the drawing
 *   takes; each delay of the job is followed by one LVGL frame (apps_snapshot_take_pending), as the LVGL task's frames come
 *   while the job polls;
 * - the screen's sleep: dark until dark_until (a time on that clock);
 * - LVGL's snapshot: a screen of screen_w x screen_h whose pixel (x, y) is pattern(x, y), never 0;
 * - the JPEG encoder: it can refuse sizes that aren't multiples of 16, and writes a fake JPEG of jpeg_len bytes; what it was
 *   given (the configuration, the input's alignment and pixels) is kept for the case to check;
 * - the store, the app task's record and the app that runs, the layer's place, the VM's writer, muse_input_wake and
 *   apps_rt_steal_ms.
 * Every block the snapshot takes (cJSON's, the port's, the encoder's) is counted, and every case ends with none held.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_snapshot.h"
#include "apps_store.h"
#include "apps_task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "muse_apps.h"
#include "muse_input.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const char *current_case = "";

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void fail_at(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: line %d: ", current_case, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
#define CHECK(cond, ...)                   \
    do {                                   \
        if (!(cond)) {                     \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                  \
    } while (0)

/* ---- The blocks: cJSON's, the port's and the encoder's, counted; the case may refuse the Nth of them ---- */

static pthread_mutex_t heap_mu = PTHREAD_MUTEX_INITIALIZER;
static int blocks;              /* held now, of all three kinds */
static int port_blocks, jpeg_blocks, cjson_blocks;
static long allocations;        /* since the case armed refuse_at */
static long refuse_at;          /* refuse the allocation of this number (1 = the first after arming); 0: none */
static bool refused;            /* one was refused */
static bool port_refuses;       /* every port allocation is refused */

static bool counting = true;     /* false while the harness itself allocates (done's print) */

static bool take_one(void) {
    if (!counting) {
        return true;
    }
    allocations++;
    if (refuse_at != 0 && allocations == refuse_at) {
        refused = true;
        return false;
    }
    return true;
}

static void *cjson_malloc(size_t n) {
    pthread_mutex_lock(&heap_mu);
    void *p = take_one() ? malloc(n) : NULL;
    if (p != NULL) {
        blocks++;
        cjson_blocks++;
    }
    pthread_mutex_unlock(&heap_mu);
    return p;
}

static void cjson_free(void *p) {
    if (p == NULL) {
        return;
    }
    pthread_mutex_lock(&heap_mu);
    blocks--;
    cjson_blocks--;
    pthread_mutex_unlock(&heap_mu);
    free(p);
}

void *apps_port_realloc(void *ptr, size_t n) {
    CHECK(n > 0, "apps_port_realloc with 0 bytes");
    pthread_mutex_lock(&heap_mu);
    bool ok = !port_refuses && take_one();
    void *p = ok ? realloc(ptr, n) : NULL;
    if (p != NULL && ptr == NULL) {
        blocks++;
        port_blocks++;
    }
    pthread_mutex_unlock(&heap_mu);
    return p;
}

void apps_port_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    pthread_mutex_lock(&heap_mu);
    blocks--;
    port_blocks--;
    pthread_mutex_unlock(&heap_mu);
    free(ptr);
}

static size_t psram_free = 4 * 1024 * 1024;
static size_t psram_largest = 4 * 1024 * 1024;

size_t apps_port_psram_free(void) {
    return psram_free;
}

size_t apps_port_psram_largest(void) {
    return psram_largest;
}

/* The floor for the encoder's own buffers (the wrapped allocators): a case can say it refuses. */
static bool room_refuses;
static int room_asks;

bool apps_port_room(size_t n) {
    (void)n;
    room_asks++;
    return !room_refuses;
}

/* The snapshot's blocks (apps_port_snapshot_*): aligned to 16, counted with the rest and refusable as the port's are (take_one); a
 * shrink keeps the block where it is, as TLSF's realloc does, and recounts. What was asked is kept for the cases. */
static struct {
    void *p;
    size_t n;
} snaps[8];
static size_t snap_bytes, snap_peak, snap_sizes[8];
static int snap_allocs, snap_asks, snap_refuse_at;  /* snap_refuse_at: the snapshot block of this number is refused (0: none) */
static size_t take_refused_free;  /* when set, free PSRAM from that refusal on (the memory went meanwhile) */

void *apps_port_snapshot_alloc(size_t n) {
    CHECK(n > 0, "apps_port_snapshot_alloc with 0 bytes");
    pthread_mutex_lock(&heap_mu);
    bool ok = !port_refuses && take_one();
    if (ok && ++snap_asks == snap_refuse_at) {
        ok = false;
        refused = true;
        if (take_refused_free != 0) {
            psram_free = take_refused_free;
        }
    }
    void *p = ok ? aligned_alloc(16, (n + 15) / 16 * 16) : NULL;
    if (p != NULL) {
        size_t i = 0;
        while (i < ARRAY_LEN(snaps) && snaps[i].p != NULL) {
            i++;
        }
        CHECK(i < ARRAY_LEN(snaps), "more than 8 snapshot blocks held");
        snaps[i].p = p;
        snaps[i].n = n;
        snap_bytes += n;
        snap_peak = snap_bytes > snap_peak ? snap_bytes : snap_peak;
        if (snap_allocs < (int)ARRAY_LEN(snap_sizes)) {
            snap_sizes[snap_allocs] = n;
        }
        snap_allocs++;
        blocks++;
        port_blocks++;
    }
    pthread_mutex_unlock(&heap_mu);
    return p;
}

static size_t *snap_size_of(void *p) {
    for (size_t i = 0; i < ARRAY_LEN(snaps); i++) {
        if (snaps[i].p == p) {
            return &snaps[i].n;
        }
    }
    return NULL;
}

void *apps_port_snapshot_shrink(void *p, size_t n) {
    pthread_mutex_lock(&heap_mu);
    size_t *had = snap_size_of(p);
    CHECK(had != NULL, "a shrink of a block that isn't the snapshot's");
    if (n > 0 && n < *had) {
        snap_bytes -= *had - n;
        *had = n;
    }
    pthread_mutex_unlock(&heap_mu);
    return p;
}

void apps_port_snapshot_free(void *p) {
    if (p == NULL) {
        return;
    }
    pthread_mutex_lock(&heap_mu);
    size_t *had = snap_size_of(p);
    CHECK(had != NULL, "a free of a block that isn't the snapshot's");
    snap_bytes -= *had;
    for (size_t i = 0; i < ARRAY_LEN(snaps); i++) {
        if (snaps[i].p == p) {
            snaps[i].p = NULL;
        }
    }
    blocks--;
    port_blocks--;
    pthread_mutex_unlock(&heap_mu);
    free(p);
}

/* The encoder's blocks, adopted while it encodes (fix 1 A): counted with the snapshot's, their sizes as the heap has them. */
static int adopts, disowns;

void apps_port_snapshot_adopt(void *p) {
    if (p != NULL) {
        pthread_mutex_lock(&heap_mu);
        adopts++;
        snap_bytes += heap_caps_get_allocated_size(p);
        snap_peak = snap_bytes > snap_peak ? snap_bytes : snap_peak;
        pthread_mutex_unlock(&heap_mu);
    }
}

void apps_port_snapshot_disown(void *p) {
    if (p != NULL) {
        pthread_mutex_lock(&heap_mu);
        disowns++;
        size_t n = heap_caps_get_allocated_size(p);
        CHECK(n <= snap_bytes, "disowned %zu bytes of %zu", n, snap_bytes);
        snap_bytes -= n;
        pthread_mutex_unlock(&heap_mu);
    }
}

size_t apps_port_snapshot_bytes(void) {
    return snap_bytes;
}

/* The app task's waits for the snapshot's memory (apps_port.h), as the port would report them: a case sets them. */
static uint32_t fake_waits, fake_wait_ms;

uint32_t apps_port_snapshot_waits(uint32_t *ms) {
    *ms = fake_wait_ms;
    uint32_t n = fake_waits;
    fake_waits = fake_wait_ms = 0;
    return n;
}

/* ---- The clock and the screen ---- */

static _Atomic int64_t now_ms = 100000;
static _Atomic int64_t dark_until;   /* dark while the clock is before it */
static atomic_bool dark_forever;

int64_t apps_port_mono_ms(void) {
    return atomic_load(&now_ms);
}

static _Atomic int64_t idle_dark_at;  /* the idle timer: dark from then on (0: never), unless a wake pokes it first */

bool apps_port_dark(void) {
    const int64_t now = atomic_load(&now_ms), idle = atomic_load(&idle_dark_at);
    return atomic_load(&dark_forever) || now < atomic_load(&dark_until) || (idle != 0 && now >= idle);
}

static int wakes;
static const char *wake_why;

/* A wake pokes the idle timer, as muse_input_wake's does: a lit screen stays lit. */
void muse_input_wake(const char *why) {
    wakes++;
    wake_why = why;
    const int64_t idle = atomic_load(&idle_dark_at);
    if (idle != 0 && atomic_load(&now_ms) < idle) {
        atomic_store(&idle_dark_at, 0);
    }
}

static _Atomic uint32_t stolen_ms;
static int steals;

void apps_rt_steal_ms(uint32_t ms) {
    atomic_fetch_add(&stolen_ms, ms);
    steals++;
}

/* DMA RAM, for the log's lines. */
size_t heap_caps_get_free_size(uint32_t caps) {
    return caps & MALLOC_CAP_DMA ? 24000 : 0;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    return caps & MALLOC_CAP_DMA ? 12963 : 0;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    (void)task;
    return 4321;
}

/* A task is a thread here: its handle, one for each. */
static _Thread_local char this_task;

TaskHandle_t xTaskGetCurrentTaskHandle(void) {
    return &this_task;
}

/* ---- The log ---- */

static char logged[65536];
static size_t logged_len;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int n = snprintf(logged + logged_len, sizeof logged - logged_len, "%c %s: %s\n", level, tag, line);
    if (n > 0 && logged_len + (size_t)n < sizeof logged) {
        logged_len += (size_t)n;
    }
}

static bool logged_has(const char *text) {
    return strstr(logged, text) != NULL;
}

/* ---- The store, the record, the app that runs and the layer ---- */

static apps_storage_t store_mode = APPS_STORAGE_FLASH;
static const char *installed_ids[4] = {"timer", "hello"};

apps_storage_t apps_store_mode(void) {
    return store_mode;
}

bool apps_store_has(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(installed_ids); i++) {
        if (installed_ids[i] != NULL && strcmp(installed_ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static const char *running_id = "timer";
static const char *record_id = "timer";
static const char *after_take_running;  /* the app that runs (and has the record) from the frame after the drawing */
static const char *record_dump;
static int record_truncated;
static int statuses;

#define TIMER_DUMP                                                                                                        \
    "[{\"id\":\"arc#0\",\"kind\":\"arc\",\"text\":null,\"x\":13,\"y\":13,\"w\":440,\"h\":440,\"hidden\":false,"        \
    "\"overflow\":0},{\"id\":\"label#1\",\"kind\":\"label\",\"text\":\"Time's up\",\"x\":100,\"y\":-5,\"w\":80,\"h\":20," \
    "\"hidden\":false,\"overflow\":14},{\"id\":\"label#2\",\"kind\":\"label\",\"text\":\"tucked away\",\"x\":0,\"y\":0,"  \
    "\"w\":10,\"h\":10,\"hidden\":true,\"overflow\":30},{\"id\":\"box#3\",\"kind\":\"box\",\"text\":null,\"x\":-4,"      \
    "\"y\":200,\"w\":30,\"h\":30,\"hidden\":false,\"overflow\":7}]"

bool apps_task_running(char *id, size_t cap) {
    snprintf(id, cap, "%s", running_id != NULL ? running_id : "");
    return running_id != NULL;
}

void apps_task_status(apps_status_t *st) {
    statuses++;
    memset(st, 0, sizeof *st);
    st->state = APPS_STATE_RUNNING;
    snprintf(st->id, sizeof st->id, "%s", record_id);
    snprintf(st->ui_dump, sizeof st->ui_dump, "%s", record_dump);
    st->ui_truncated = record_truncated;
}

static bool layer_front = true, cards = false;

bool apps_lv_front(void) {
    return layer_front;
}

bool apps_lv_cards_shown(void) {
    return cards;
}

/* ---- LVGL's snapshot ---- */

static int screen_w = 466, screen_h = 466;
static int renders;               /* lv_snapshot_take_to_draw_buf calls */
static int render_ms;             /* what a drawing takes, on the clock */
static bool render_fails;
static struct lv_obj {
    int unused;
} the_screen;

static uint16_t pattern(int x, int y) {
    return (uint16_t)(0x0841u + (unsigned)((x * 7 + y * 13) & 0x0FFF));
}

lv_obj_t *lv_screen_active(void) {
    return &the_screen;
}

void lv_obj_update_layout(const lv_obj_t *obj) {
    CHECK(obj == &the_screen, "the layout of something else");
}

int32_t lv_obj_get_width(const lv_obj_t *obj) {
    CHECK(obj == &the_screen, "the width of something else");
    return screen_w;
}

int32_t lv_obj_get_height(const lv_obj_t *obj) {
    CHECK(obj == &the_screen, "the height of something else");
    return screen_h;
}

static uint32_t stride_pad;  /* bytes LVGL's alignment adds to each row (fix 1 H): none on the 1.75C */

uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t cf) {
    CHECK(cf == LV_COLOR_FORMAT_RGB565, "stride of format %d", (int)cf);
    return w * 2 + stride_pad;
}

lv_result_t lv_draw_buf_init(lv_draw_buf_t *buf, uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride, void *data,
                             uint32_t data_size) {
    memset(buf, 0, sizeof *buf);
    if (stride * h > data_size) {
        return LV_RESULT_INVALID;
    }
    buf->header.w = w;
    buf->header.h = h;
    buf->header.cf = cf;
    buf->header.stride = stride;
    buf->data = data;
    buf->unaligned_data = data;
    buf->data_size = data_size;
    return LV_RESULT_OK;
}

/* The abandoned drawing's: the drawing tells the case it has begun, and waits for the case to let it end. */
static pthread_mutex_t slow_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slow_cv = PTHREAD_COND_INITIALIZER;
static bool slow_render, slow_started, slow_released;

lv_result_t lv_snapshot_take_to_draw_buf(lv_obj_t *obj, lv_color_format_t cf, lv_draw_buf_t *buf) {
    CHECK(obj == &the_screen && cf == LV_COLOR_FORMAT_RGB565, "a snapshot of something else");
    renders++;
    if (slow_render) {
        pthread_mutex_lock(&slow_mu);
        slow_started = true;
        pthread_cond_broadcast(&slow_cv);
        while (!slow_released) {
            pthread_cond_wait(&slow_cv, &slow_mu);
        }
        pthread_mutex_unlock(&slow_mu);
    }
    atomic_fetch_add(&now_ms, render_ms);
    if (render_fails || (uint32_t)screen_w * 2 * (uint32_t)screen_h > buf->data_size) {
        return LV_RESULT_INVALID;
    }
    buf->header.w = (uint32_t)screen_w;
    buf->header.h = (uint32_t)screen_h;
    for (int y = 0; y < screen_h; y++) {
        uint16_t *row = (uint16_t *)(buf->data + (size_t)y * buf->header.stride);
        for (int x = 0; x < screen_w; x++) {
            row[x] = pattern(x, y);
        }
    }
    return LV_RESULT_OK;
}

/* ---- The JPEG encoder ---- */

static bool jpeg_refuses_unaligned;   /* sizes that aren't multiples of 16 are refused */
static bool jpeg_refuses_all;
static bool jpeg_no_memory;
static bool jpeg_process_fails;
static int jpeg_len = 1234;
static int opens, closes, processes;
static jpeg_enc_config_t last_config;
static int enc_w, enc_h;
static bool enc_in_aligned;
static uint16_t enc_corner, enc_centre, enc_pad;
static int enc_in_size;
static uint32_t enc_sum;          /* FNV-1a of the input's pixels, the picture's (the encoder's w x h) */
static size_t enc_held;           /* the snapshot's blocks held as it encoded */
/* Its own blocks, as esp_new_jpeg's: the encoder's state at the open, and at the encode a node and the Huffman buffer (sized from the
 * state), through the allocators the linker sends to apps_snapshot.c, all freed at the close. A case sets their sizes, or refuses the
 * encode's (JPEG_ERR_NO_MEM, with free PSRAM then what huff_refused_free says, when it is set). */
static size_t enc_state_bytes = 6000, enc_huff_bytes = 20000;
static bool huff_refused;
static size_t huff_refused_free;
static void *enc_blocks[3];
static size_t enc_counted_then;   /* the snapshot's bytes with all three held */
/* Another task's use of the same allocators during the encode (the library's decoder, for display.draw_file): whether its block was
 * counted with the snapshot's (-1: it didn't run). */
static bool decode_meanwhile;
static int decoder_counted = -1;

static void *decoder_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&heap_mu);
    const size_t before = snap_bytes;
    pthread_mutex_unlock(&heap_mu);
    void *p = __wrap_jpeg_calloc_inner(500);
    pthread_mutex_lock(&heap_mu);
    decoder_counted = p != NULL && snap_bytes != before;
    pthread_mutex_unlock(&heap_mu);
    __wrap_jpeg_free(p);
    return NULL;
}
static struct encoder {
    int w, h;
} the_encoder;

jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *info, jpeg_enc_handle_t *enc) {
    opens++;
    last_config = *info;
    if (jpeg_no_memory) {
        return JPEG_ERR_NO_MEM;
    }
    if (jpeg_refuses_all || (jpeg_refuses_unaligned && (info->width % 16 != 0 || info->height % 16 != 0))) {
        return JPEG_ERR_INVALID_PARAM;
    }
    enc_blocks[0] = __wrap_jpeg_calloc_align(enc_state_bytes, 16);
    if (enc_blocks[0] == NULL) {
        return JPEG_ERR_NO_MEM;
    }
    the_encoder.w = info->width;
    the_encoder.h = info->height;
    *enc = &the_encoder;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_enc_process(const jpeg_enc_handle_t enc, const uint8_t *in, int in_size, uint8_t *out, int out_cap,
                              int *out_len) {
    processes++;
    CHECK(enc == &the_encoder, "process with a handle that isn't open");
    struct encoder *e = enc;
    enc_w = e->w;
    enc_h = e->h;
    enc_in_size = in_size;
    enc_in_aligned = ((uintptr_t)in & 15) == 0;
    const uint16_t *px = (const uint16_t *)in;
    enc_corner = px[0];
    enc_centre = px[(e->h / 2) * e->w + e->w / 2];
    enc_pad = px[(e->h / 2) * e->w + e->w - 1];  /* the last column: padding when the size was padded */
    enc_sum = 2166136261u;
    for (int i = 0; i < e->w * e->h; i++) {
        enc_sum = (enc_sum ^ px[i]) * 16777619u;
    }
    enc_held = snap_bytes;
    if (huff_refused) {
        if (huff_refused_free != 0) {
            psram_free = huff_refused_free;
        }
        return JPEG_ERR_NO_MEM;
    }
    enc_blocks[1] = __wrap_jpeg_calloc_align(12, 16);
    enc_blocks[2] = __wrap_jpeg_calloc_align(enc_huff_bytes, 16);
    if (enc_blocks[1] == NULL || enc_blocks[2] == NULL) {
        return JPEG_ERR_NO_MEM;
    }
    enc_counted_then = snap_bytes;
    if (decode_meanwhile) {
        pthread_t t;
        CHECK(pthread_create(&t, NULL, decoder_thread, NULL) == 0, "no decoder thread");
        pthread_join(t, NULL);
    }
    if (jpeg_process_fails || jpeg_len > out_cap) {
        return JPEG_ERR_FAIL;
    }
    for (int i = 0; i < jpeg_len; i++) {
        out[i] = (uint8_t)(i == 0 ? 0xFF : i == 1 ? 0xD8 : (i * 31) & 0xFF);
    }
    *out_len = jpeg_len;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t enc) {
    CHECK(enc == &the_encoder, "close of a handle that isn't open");
    closes++;
    for (size_t i = 0; i < ARRAY_LEN(enc_blocks); i++) {
        __wrap_jpeg_free_align(enc_blocks[i]);
        enc_blocks[i] = NULL;
    }
    return JPEG_ERR_OK;
}

void *jpeg_calloc_align(size_t size, int aligned) {
    CHECK(aligned == 16, "the encoder's input aligned to %d", aligned);
    pthread_mutex_lock(&heap_mu);
    void *p = take_one() ? aligned_alloc(16, (size + 15) / 16 * 16) : NULL;
    if (p != NULL) {
        memset(p, 0, size);
        blocks++;
        jpeg_blocks++;
    }
    pthread_mutex_unlock(&heap_mu);
    return p;
}

/* The heap the wrapped esp_new_jpeg allocators ask (heap_caps_calloc, heap_caps_aligned_calloc): what they asked for, and a
 * PSRAM that can run out. */
static int caps_calls;
static uint32_t last_caps;
static size_t last_alignment;
static bool psram_out;

static struct {
    void *p;
    size_t n;
} caps_held[16];
static int caps_live;

static void *caps_block(size_t alignment, size_t size, uint32_t caps) {
    caps_calls++;
    last_caps = caps;
    last_alignment = alignment;
    if ((caps & MALLOC_CAP_SPIRAM) && psram_out) {
        return NULL;
    }
    const size_t n = (size + alignment - 1) / alignment * alignment;
    void *p = aligned_alloc(alignment, n);
    if (p != NULL) {
        memset(p, 0, size);
        size_t i = 0;
        while (i < ARRAY_LEN(caps_held) && caps_held[i].p != NULL) {
            i++;
        }
        CHECK(i < ARRAY_LEN(caps_held), "more than 16 encoder blocks held");
        caps_held[i].p = p;
        caps_held[i].n = n;
        caps_live++;
    }
    return p;
}

size_t heap_caps_get_allocated_size(void *ptr) {
    for (size_t i = 0; i < ARRAY_LEN(caps_held); i++) {
        if (caps_held[i].p == ptr) {
            return caps_held[i].n;
        }
    }
    CHECK(false, "the size of a block the heap didn't give");
    return 0;
}

void heap_caps_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    for (size_t i = 0; i < ARRAY_LEN(caps_held); i++) {
        if (caps_held[i].p == ptr) {
            caps_held[i].p = NULL;
            caps_live--;
            free(ptr);
            return;
        }
    }
    CHECK(false, "a free of a block the heap didn't give");
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    return caps_block(sizeof(void *), n * size, caps);
}

void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps) {
    return caps_block(alignment, n * size, caps);
}

void jpeg_free_align(void *p) {
    if (p == NULL) {
        return;
    }
    pthread_mutex_lock(&heap_mu);
    blocks--;
    jpeg_blocks--;
    pthread_mutex_unlock(&heap_mu);
    free(p);
}

/* ---- The VM's writer ---- */

static int writes;
static char written_path[160];
static uint8_t written[200000];
static size_t written_len;
static int64_t written_deadline_us;
static int64_t written_left_ms;  /* what was left of the deadline when the write began */
static size_t written_held;      /* the snapshot's blocks held as it wrote */
static const char *write_code;   /* NULL: the write works; else the code it fails with */
static const char *write_why = "the VM answered HTTP 507";
static int write_takes_ms;       /* the write's time on the clock: past the deadline, it times out there */

static bool vm_write(const char *path, const void *data, size_t len, int64_t deadline_us, const char **code, char *err,
                     size_t err_len) {
    writes++;
    written_held = snap_bytes;
    snprintf(written_path, sizeof written_path, "%s", path);
    written_len = len <= sizeof written ? len : 0;
    memcpy(written, data, written_len);
    written_deadline_us = deadline_us;
    written_left_ms = deadline_us / 1000 - atomic_load(&now_ms);
    if (write_takes_ms > written_left_ms) {
        atomic_store(&now_ms, deadline_us / 1000);
        *code = MUSE_APPS_VM_TIMED_OUT;
        snprintf(err, err_len, "no answer from the VM in time");
        return false;
    }
    atomic_fetch_add(&now_ms, write_takes_ms);
    if (write_code != NULL) {
        *code = write_code;
        snprintf(err, err_len, "%s", write_why);
        return false;
    }
    return true;
}

static bool vm_read(const char *path, size_t max, bool (*sink)(void *, const uint8_t *, size_t), void *user, int64_t deadline_us,
                    const char **code, char *err, size_t err_len) {
    (void)path, (void)max, (void)sink, (void)user, (void)deadline_us;
    *code = MUSE_APPS_VM_FAILED;
    snprintf(err, err_len, "no reads in this harness");
    return false;
}

static const muse_apps_vm_ops_t VM_OPS = {vm_read, vm_write, NULL};
static const muse_apps_vm_ops_t *vm_ops = &VM_OPS;

const muse_apps_vm_ops_t *muse_apps_vm_ops(void) {
    return vm_ops;
}

/* ---- The I/O worker ---- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} job_t;

static job_t jobs[8];
static int n_jobs;
static bool io_refuses;
static int assists;

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    if (io_refuses || n_jobs == (int)ARRAY_LEN(jobs)) {
        return false;
    }
    jobs[n_jobs++] = (job_t){fn, arg};
    return true;
}

void apps_io_assist(void) {
    assists++;
}

static void run_jobs(void) {
    while (n_jobs > 0) {
        job_t job = jobs[0];
        memmove(jobs, jobs + 1, sizeof jobs[0] * (size_t)--n_jobs);
        job.fn(job.arg);
    }
}

/* ---- The LVGL task's frames ---- */

static bool frames_on = true;    /* a frame after every delay of the job, on the job's thread */
static int frames;
static bool threaded;            /* the abandoned drawing's: frames are a second thread's */
static pthread_t lvgl_thread;
static pthread_mutex_t frame_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cv = PTHREAD_COND_INITIALIZER;
static bool frame_go, frame_quit, frame_done;

static void *lvgl_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&frame_mu);
    for (;;) {
        while (!frame_go && !frame_quit) {
            pthread_cond_wait(&frame_cv, &frame_mu);
        }
        if (frame_quit) {
            break;
        }
        frame_go = false;
        pthread_mutex_unlock(&frame_mu);
        apps_snapshot_take_pending();
        pthread_mutex_lock(&frame_mu);
        frame_done = true;
        pthread_cond_broadcast(&frame_cv);
    }
    pthread_mutex_unlock(&frame_mu);
    return NULL;
}

void vTaskDelay(TickType_t ticks) {
    CHECK(ticks > 0, "a delay of no ticks spins");
    atomic_fetch_add(&now_ms, (int64_t)ticks);
    if (threaded) {
        /* One frame on the LVGL thread, the first time the screen is lit: it starts the drawing, which then waits for the case.
         * The job's thread goes on only once the drawing has begun. */
        if (!apps_port_dark() && renders == 0 && !slow_started) {
            pthread_mutex_lock(&frame_mu);
            frame_go = true;
            pthread_cond_broadcast(&frame_cv);
            pthread_mutex_unlock(&frame_mu);
            pthread_mutex_lock(&slow_mu);
            while (!slow_started) {
                pthread_cond_wait(&slow_cv, &slow_mu);
            }
            pthread_mutex_unlock(&slow_mu);
        }
        return;
    }
    if (frames_on) {
        frames++;
        const int drawn = renders;
        apps_snapshot_take_pending();
        if (renders != drawn && after_take_running != NULL) {  /* the app task switches apps once the screen is drawn */
            running_id = after_take_running;
            record_id = after_take_running;
            after_take_running = NULL;
        }
    }
}

/* ---- The command ---- */

static int dones;
static char result[16384];
static bool result_null;

static void done_cb(void *ctx, cJSON *r) {
    CHECK(ctx == (void *)&dones, "done's context is not the caller's");
    dones++;
    result_null = r == NULL;
    counting = false;  /* the harness's own print is never refused */
    char *text = r != NULL ? cJSON_PrintUnformatted(r) : NULL;
    counting = true;
    CHECK(r == NULL || text != NULL, "no memory to print the result");
    snprintf(result, sizeof result, "%s", text != NULL ? text : "(null)");
    cJSON_free(text);
    cJSON_Delete(r);
}

static void reset(void) {
    refuse_at = 0;
    allocations = 0;
    refused = false;
    port_refuses = false;
    psram_free = 4 * 1024 * 1024;
    psram_largest = 4 * 1024 * 1024;
    room_refuses = false;
    room_asks = 0;
    snap_peak = 0;
    snap_allocs = snap_asks = snap_refuse_at = 0;
    take_refused_free = 0;
    decode_meanwhile = false;
    decoder_counted = -1;
    memset(snap_sizes, 0, sizeof snap_sizes);
    fake_waits = fake_wait_ms = 0;
    enc_sum = 0;
    enc_held = 0;
    written_held = 0;
    atomic_store(&dark_until, 0);
    atomic_store(&dark_forever, false);
    wakes = 0;
    wake_why = NULL;
    atomic_store(&stolen_ms, 0);
    steals = 0;
    logged_len = 0;
    logged[0] = '\0';
    store_mode = APPS_STORAGE_FLASH;
    running_id = "timer";
    record_id = "timer";
    record_dump = TIMER_DUMP;
    record_truncated = 0;
    statuses = 0;
    layer_front = true;
    cards = false;
    screen_w = screen_h = 466;
    renders = 0;
    render_ms = 0;
    render_fails = false;
    jpeg_refuses_unaligned = jpeg_refuses_all = jpeg_no_memory = jpeg_process_fails = false;
    jpeg_len = 1234;
    opens = closes = processes = 0;
    enc_state_bytes = 6000;
    enc_huff_bytes = 20000;
    huff_refused = false;
    huff_refused_free = 0;
    enc_counted_then = 0;
    adopts = disowns = 0;
    stride_pad = 0;
    caps_calls = 0;
    last_caps = 0;
    atomic_store(&idle_dark_at, 0);
    after_take_running = NULL;
    written_left_ms = 0;
    write_takes_ms = 0;
    last_alignment = 0;
    psram_out = false;
    memset(&last_config, 0, sizeof last_config);
    enc_w = enc_h = 0;
    writes = 0;
    written_path[0] = '\0';
    written_len = 0;
    write_code = NULL;
    write_why = "the VM answered HTTP 507";
    vm_ops = &VM_OPS;
    io_refuses = false;
    assists = 0;
    frames_on = true;
    frames = 0;
    dones = 0;
    result[0] = '\0';
    result_null = false;
}

/* Every block given back, nothing queued. */
static void expect_clean_at(int line) {
    if (blocks != 0 || n_jobs != 0 || snap_bytes != 0 || caps_live != 0) {
        fail_at(line, "held at the end: %d blocks (port %d, encoder %d, cJSON %d), %d of the heap's, %zu snapshot bytes, %d jobs",
                blocks, port_blocks, jpeg_blocks, cjson_blocks, caps_live, snap_bytes, n_jobs);
    }
}
#define EXPECT_CLEAN() expect_clean_at(__LINE__)

/* Asks for a snapshot of id at scale and runs the job: true when it was queued (done ran once then), false with code and err
 * (nothing queued, done not called). */
static bool snap(const char *id, int scale, const char **code, const char **err) {
    int before = dones;
    *code = "(unset)";
    *err = "(unset)";
    bool queued = muse_apps_snapshot(id, scale, done_cb, &dones, code, err);
    if (!queued) {
        CHECK(n_jobs == 0 && dones == before, "a refusal queued a job or called done");
        return false;
    }
    CHECK(n_jobs == 1 && dones == before, "queued: one job, and done not yet");
    run_jobs();
    CHECK(dones == before + 1, "done ran %d times", dones - before);
    return true;
}

/* A snapshot that is answered with the failure code and message. */
static void expect_failure_at(int line, const char *id, const char *code, const char *message) {
    const char *c, *e;
    if (!snap(id, 1, &c, &e)) {
        fail_at(line, "refused at the call: %s %s", c, e);
    }
    char want[1024];
    snprintf(want, sizeof want, "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
    if (strcmp(result, want) != 0) {
        fail_at(line, "the result is\n  %s\nnot\n  %s", result, want);
    }
}
#define EXPECT_FAILURE(id, code, message) expect_failure_at(__LINE__, (id), (code), (message))

/* A refusal at the call, with its code and message. */
static void expect_refusal_at(int line, const char *id, int scale, muse_apps_done_cb done, const char *code, const char *err) {
    const char *c = "(unset)", *e = "(unset)";
    int before = dones, jobs_before = n_jobs;
    bool queued = muse_apps_snapshot(id, scale, done, &dones, &c, &e);
    if (queued || n_jobs != jobs_before || dones != before) {
        fail_at(line, "not refused (queued %d, %d jobs)", queued, n_jobs);
    }
    if (strcmp(c, code) != 0 || strcmp(e, err) != 0) {
        fail_at(line, "refused with %s '%s', not %s '%s'", c, e, code, err);
    }
}
#define EXPECT_REFUSAL(id, scale, done, code, err) expect_refusal_at(__LINE__, (id), (scale), (done), (code), (err))

/* The successful result's payload, parsed (the caller deletes it). */
static cJSON *payload_at(int line) {
    cJSON *r = cJSON_Parse(result);
    if (r == NULL || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"))) {
        fail_at(line, "not a success: %s", result);
    }
    cJSON *p = cJSON_DetachItemFromObjectCaseSensitive(r, "payload");
    cJSON_Delete(r);
    if (!cJSON_IsObject(p)) {
        fail_at(line, "no payload: %s", result);
    }
    return p;
}
#define PAYLOAD() payload_at(__LINE__)

static double number(const cJSON *p, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static const char *text(const cJSON *p, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, key));
    return s != NULL ? s : "(none)";
}

/* The payload's warnings, joined by "|". */
static void warnings_of(const cJSON *p, char *out, size_t cap) {
    out[0] = '\0';
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(p, "warnings");
    CHECK(cJSON_IsArray(w), "no warnings array");
    const cJSON *item;
    size_t used = 0;
    cJSON_ArrayForEach(item, w) {
        CHECK(cJSON_IsString(item), "a warning that isn't text");
        used += (size_t)snprintf(out + used, cap - used, "%s%s", used > 0 ? "|" : "", item->valuestring);
        if (used >= cap) {
            break;
        }
    }
}

/* ---- The cases ---- */

#define ASYNC_OK_PATH "workspace/apps/timer/snapshot.jpg"
#define NOT_FOUND_NOSUCH "no app 'nosuch' on this gadget (app.control list shows the installed ones)"

static void refusals(void) {
    EXPECT_REFUSAL("timer", 1, NULL, "invalid_params", "no callback for the result");
    EXPECT_REFUSAL(NULL, 1, done_cb, "invalid_params", "app id must match ^[a-z][a-z0-9_]{1,23}$");
    EXPECT_REFUSAL("Timer", 1, done_cb, "invalid_params", "app id must match ^[a-z][a-z0-9_]{1,23}$");
    EXPECT_REFUSAL("a", 1, done_cb, "invalid_params", "app id must match ^[a-z][a-z0-9_]{1,23}$");
    static const int bad_scales[] = {0, 3, -1, 4, 100};
    for (size_t i = 0; i < ARRAY_LEN(bad_scales); i++) {
        EXPECT_REFUSAL("timer", bad_scales[i], done_cb, "invalid_params", MUSE_APPS_SNAPSHOT_BAD_SCALE);
    }
    vm_ops = NULL;
    EXPECT_REFUSAL("timer", 1, done_cb, "unavailable", "the gadget can't write files on the VM");
    vm_ops = &VM_OPS;
    /* The admission (N1; admission_before_drawing has the rest): the reserve and the peak, no more. */
    running_id = NULL;
    psram_free = APPS_RAM_RESERVE + apps_snapshot_peak(2) - 1;
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_NO_MEMORY);
    CHECK(strcmp(MUSE_APPS_SNAPSHOT_NO_MEMORY, "not enough memory for a snapshot right now") == 0, "the brief's words");
    running_id = "timer";
    psram_free = APPS_RAM_RESERVE + apps_snapshot_peak(1);
    /* One at a time: a second while one is pending is busy, and the first's admission goes with its done. */
    const char *c, *e;
    CHECK(muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e), "the first refused: %s %s", c, e);
    EXPECT_REFUSAL("timer", 1, done_cb, "busy", "another snapshot is being taken; try again in a few seconds");
    EXPECT_REFUSAL("hello", 2, done_cb, "busy", "another snapshot is being taken; try again in a few seconds");
    CHECK(n_jobs == 1, "one job queued: %d", n_jobs);
    run_jobs();
    CHECK(dones == 1 && strstr(result, "\"ok\":true") != NULL, "the first: %s", result);
    CHECK(snap("timer", 2, &c, &e), "after the first's done, the next is taken: %s %s", c, e);
    /* The worker's refusal and no memory for the job: out_of_memory, and the admission is let go. */
    io_refuses = true;
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", APPS_IO_REFUSED);
    io_refuses = false;
    port_refuses = true;
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", "out of memory: no room for the command");
    port_refuses = false;
    CHECK(snap("timer", 1, &c, &e), "and then one is taken: %s %s", c, e);
    CHECK(wakes == 3 && dones == 3, "three done, each with its wake (a lit screen's idle timer poked): %d", wakes);
    EXPECT_CLEAN();
}

static void not_installed(void) {
    EXPECT_FAILURE("nosuch", "not_found", NOT_FOUND_NOSUCH);
    store_mode = APPS_STORAGE_NONE;
    EXPECT_FAILURE("timer", "unavailable", "storage not ready");
    store_mode = APPS_STORAGE_RAM;
    CHECK(renders == 0 && opens == 0 && writes == 0 && wakes == 0, "nothing drawn, encoded or written");
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "RAM mode: %s", result);
    EXPECT_CLEAN();
}

static void in_front(void) {
    render_ms = 37;
    jpeg_len = 23456;
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    cJSON *p = PAYLOAD();
    /* D2's shape, no base64 anywhere. */
    CHECK(strcmp(text(p, "path"), ASYNC_OK_PATH) == 0, "path %s", text(p, "path"));
    CHECK(strcmp(text(p, "format"), "jpeg") == 0, "format %s", text(p, "format"));
    CHECK(number(p, "w") == 466 && number(p, "h") == 466, "%gx%g", number(p, "w"), number(p, "h"));
    CHECK(number(p, "bytes") == 23456, "bytes %g", number(p, "bytes"));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "front")), "front");
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "data_base64") == NULL && strstr(result, "base64") == NULL, "no base64");
    CHECK(cJSON_GetObjectItemCaseSensitive(p, "ui_truncated") == NULL, "nothing truncated");
    /* The widgets as the record has them, and the overflow warnings worked out again from them: the hidden one has none. */
    cJSON *ui = cJSON_GetObjectItemCaseSensitive(p, "ui");
    cJSON *want_ui = cJSON_Parse(TIMER_DUMP);
    CHECK(cJSON_Compare(ui, want_ui, true), "ui is the record's dump: %s", result);
    cJSON_Delete(want_ui);
    char w[2048];
    warnings_of(p, w, sizeof w);
    CHECK(strcmp(w, "label#1 'Time's up' extends 14 px past the round edge|box#3 'box' extends 7 px past the round edge") == 0,
          "warnings: %s", w);
    cJSON_Delete(p);
    /* The file: the JPEG's bytes, at the path, with the command's deadline. */
    CHECK(writes == 1 && strcmp(written_path, ASYNC_OK_PATH) == 0, "written once to %s", written_path);
    CHECK(written_len == 23456 && written[0] == 0xFF && written[1] == 0xD8 && written[100] == (uint8_t)((100 * 31) & 0xFF),
          "the JPEG's bytes: %zu", written_len);
    CHECK(written_deadline_us == (100000 + MUSE_APPS_SNAPSHOT_TOTAL_MS) * 1000LL, "deadline %lld", (long long)written_deadline_us);
    /* The encoder: RGB565 little-endian, 4:2:0, quality 80, the screen's size; a 16-byte aligned input, masked. */
    CHECK(opens == 1 && processes == 1 && closes == 1, "opened %d, processed %d, closed %d", opens, processes, closes);
    CHECK(last_config.width == 466 && last_config.height == 466 && last_config.src_type == JPEG_PIXEL_FORMAT_RGB565_LE &&
              last_config.subsampling == JPEG_SUBSAMPLE_420 && last_config.quality == 80 && !last_config.task_enable &&
              last_config.rotate == JPEG_ROTATE_0D,
          "config %dx%d type %x sub %d q %d", last_config.width, last_config.height, (unsigned)last_config.src_type,
          (int)last_config.subsampling, last_config.quality);
    CHECK(enc_in_aligned && enc_in_size == 466 * 466 * 2, "input aligned %d, %d bytes", enc_in_aligned, enc_in_size);
    CHECK(enc_corner == 0 && enc_centre == pattern(233, 233), "masked: corner %04x, centre %04x", enc_corner, enc_centre);
    /* One drawing, its time stolen from the app's books, the record read once, and the logs. */
    CHECK(renders == 1 && steals == 1 && atomic_load(&stolen_ms) == 37, "renders %d, stolen %u", renders,
          (unsigned)atomic_load(&stolen_ms));
    CHECK(statuses == 1, "the record read %d times", statuses);
    CHECK(wakes == 1 && !logged_has("waking the screen"), "a lit screen's idle timer poked, and not said to be woken: %d",
          wakes);
    CHECK(logged_has("I muse_apps: snapshot timer: scale 1\n"), "the start line:\n%s", logged);
    CHECK(logged_has("I muse_apps: snapshot timer: 466x466 jpeg 23456 bytes (q80), drawn in 37 ms, encoded in "), "the encode "
          "line:\n%s", logged);
    CHECK(logged_has("io stack 4321 of 8192 free; DMA free 24000 before, 24000 with the encoder open, lowest 12963"),
          "the measurements:\n%s", logged);
    CHECK(logged_has("I muse_apps: snapshot timer: wrote " ASYNC_OK_PATH " (23456 bytes) in "), "the write line:\n%s", logged);
    EXPECT_CLEAN();
}

static void half_size(void) {
    jpeg_refuses_unaligned = true;
    const char *c, *e;
    CHECK(snap("timer", 2, &c, &e), "refused: %s %s", c, e);
    cJSON *p = PAYLOAD();
    CHECK(number(p, "w") == 233 && number(p, "h") == 233, "the result's size: %gx%g", number(p, "w"), number(p, "h"));
    char w[2048];
    warnings_of(p, w, sizeof w);
    CHECK(strstr(w, "the JPEG is 240x240: the encoder takes sizes in steps of 16, so its edges are black padding") != NULL,
          "the padding's warning: %s", w);
    cJSON_Delete(p);
    CHECK(opens == 2 && last_config.width == 240 && last_config.height == 240 && enc_w == 240 && enc_h == 240,
          "233 refused, 240 encoded: %d opens, %dx%d", opens, enc_w, enc_h);
    CHECK(enc_in_size == 240 * 240 * 2 && enc_pad == 0 && enc_corner == 0 && enc_centre != 0, "the padded input: %d bytes, pad "
          "%04x, centre %04x", enc_in_size, enc_pad, enc_centre);
    CHECK(closes == 1, "one encoder opened, one closed: %d", closes);
    CHECK(logged_has("snapshot timer: 233x233 jpeg 1234 bytes (q80, padded to 240x240)"), "the log:\n%s", logged);
    EXPECT_CLEAN();
}

static void padded_full_size(void) {
    jpeg_refuses_unaligned = true;
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    cJSON *p = PAYLOAD();
    CHECK(number(p, "w") == 466 && number(p, "h") == 466, "%gx%g", number(p, "w"), number(p, "h"));
    cJSON_Delete(p);
    CHECK(opens == 2 && enc_w == 480 && enc_h == 480 && enc_in_size == 480 * 480 * 2 && enc_pad == 0, "466 refused, 480 "
          "encoded: %dx%d", enc_w, enc_h);
    /* With no padding to try (both sides multiples of 16 already), a refusal is the command's failure. */
    jpeg_refuses_all = true;
    screen_w = screen_h = 480;
    EXPECT_FAILURE("timer", "unavailable", "the JPEG encoder refused 480x480 (error -4)");
    EXPECT_CLEAN();
}

static void dark_then_lit(void) {
    atomic_store(&dark_until, atomic_load(&now_ms) + 800);
    const int64_t t0 = atomic_load(&now_ms);
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(wakes == 1 && wake_why != NULL && strcmp(wake_why, "snapshot") == 0, "woken once, as \"snapshot\": %d", wakes);
    CHECK(strstr(result, "\"ok\":true") != NULL, "%s", result);
    CHECK(renders == 1, "drawn once: %d", renders);
    /* Taken on the first lit frame: the job's polls are 10 ms apart. */
    int64_t took = atomic_load(&now_ms) - t0;
    CHECK(took >= 800 && took <= 800 + APPS_SNAPSHOT_POLL_MS, "taken %lld ms after the request", (long long)took);
    CHECK(frames >= 80, "the dark frames took nothing: %d frames", frames);
    CHECK(assists >= 80, "the app task's ops were run while the job waited: %d", assists);
    CHECK(logged_has("snapshot timer: waking the screen"), "the wake's line:\n%s", logged);
    EXPECT_CLEAN();
}

static void dark_past_deadline(void) {
    atomic_store(&dark_forever, true);
    const int64_t t0 = atomic_load(&now_ms);
    EXPECT_FAILURE("timer", "unavailable", MUSE_APPS_SNAPSHOT_NO_WAKE);
    CHECK(strcmp(MUSE_APPS_SNAPSHOT_NO_WAKE, "the gadget's screen didn't wake for the snapshot; try again") == 0, "E9's words");
    int64_t took = atomic_load(&now_ms) - t0;
    CHECK(took >= MUSE_APPS_SNAPSHOT_SCREEN_MS && took <= MUSE_APPS_SNAPSHOT_SCREEN_MS + APPS_SNAPSHOT_POLL_MS,
          "answered %lld ms after the request", (long long)took);
    CHECK(wakes == 1 && renders == 0 && opens == 0 && writes == 0, "woken, nothing drawn, encoded or written");
    /* The screen lights after: a frame finds nothing to take (the flag went with the deadline). */
    atomic_store(&dark_forever, false);
    for (int i = 0; i < 5; i++) {
        apps_snapshot_take_pending();
    }
    CHECK(renders == 0 && dones == 1, "a late frame drew nothing, and done stayed at once");
    /* And the next snapshot is taken. */
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "the next: %s", result);
    CHECK(renders == 1, "drawn once: %d", renders);
    EXPECT_CLEAN();
}

static void lit_at_deadline(void) {
    /* Dark until the very millisecond of the deadline: the frame then is too late, and the job's deadline answers. */
    atomic_store(&dark_until, atomic_load(&now_ms) + MUSE_APPS_SNAPSHOT_SCREEN_MS);
    EXPECT_FAILURE("timer", "unavailable", MUSE_APPS_SNAPSHOT_NO_WAKE);
    CHECK(renders == 0, "nothing drawn: %d", renders);
    /* Lit one poll (APPS_SNAPSHOT_POLL_MS) before the deadline is in time. */
    atomic_store(&dark_until, atomic_load(&now_ms) + MUSE_APPS_SNAPSHOT_SCREEN_MS - APPS_SNAPSHOT_POLL_MS);
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "in time: %s", result);
    EXPECT_CLEAN();
}

static void not_in_front(void) {
    const char *c, *e;
    char w[2048];
    /* Behind: the screen is drawn all the same, front false, a warning, and the app's widgets as the record has them. */
    layer_front = false;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    cJSON *p = PAYLOAD();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "front")), "front false: %s", result);
    warnings_of(p, w, sizeof w);
    CHECK(strncmp(w, "'timer' isn't on screen: the snapshot shows what is|label#1 ", 60) == 0, "warnings: %s", w);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "ui")) == 4, "the app's four widgets");
    cJSON_Delete(p);
    /* A card over the layer: not in front either. */
    layer_front = true;
    cards = true;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    p = PAYLOAD();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "front")), "a card covers it: %s", result);
    cJSON_Delete(p);
    cards = false;
    /* Another app runs: front false (the layer is that app's), no widgets of this one, and no record read. */
    running_id = "hello";
    record_id = "hello";
    statuses = 0;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    p = PAYLOAD();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "front")), "another app: %s", result);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "ui")) == 0, "no widgets: %s", result);
    warnings_of(p, w, sizeof w);
    CHECK(strcmp(w, "'timer' isn't running: the snapshot shows what is on screen") == 0, "warnings: %s", w);
    CHECK(statuses == 0, "the record isn't read for an app that doesn't run");
    cJSON_Delete(p);
    /* None runs. */
    running_id = NULL;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    p = PAYLOAD();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "front")) && renders == 4, "none runs: %s", result);
    cJSON_Delete(p);
    /* The app runs but the record is another's (it has just begun): no widgets rather than another app's. */
    running_id = "timer";
    record_id = "hello";
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    p = PAYLOAD();
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "ui")) == 0, "the record is hello's: %s", result);
    cJSON_Delete(p);
    EXPECT_CLEAN();
}

static void write_fails(void) {
    static const struct {
        const char *vm_code, *code;
    } cases[] = {
        {MUSE_APPS_VM_BUSY, "busy"},           {MUSE_APPS_VM_OUT_OF_MEMORY, "out_of_memory"},
        {MUSE_APPS_VM_DROPPED, "unavailable"}, {MUSE_APPS_VM_FAILED, "unavailable"},
        {MUSE_APPS_VM_TIMED_OUT, "unavailable"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        write_code = cases[i].vm_code;
        write_why = i == 2 ? "the VM connection dropped" : "the VM answered HTTP 403";
        char message[256];
        snprintf(message, sizeof message, "couldn't save the snapshot to " ASYNC_OK_PATH ": %s", write_why);
        EXPECT_FAILURE("timer", cases[i].code, message);
        CHECK(logged_has("W muse_apps: snapshot timer: couldn't write " ASYNC_OK_PATH ": "), "the log:\n%s", logged);
        EXPECT_CLEAN();
    }
    CHECK(writes == (int)ARRAY_LEN(cases), "one write each: %d", writes);
}

static void encoder_failures(void) {
    jpeg_no_memory = true;
    EXPECT_FAILURE("timer", "out_of_memory", "the JPEG encoder refused 466x466 (error -2)");
    CHECK(opens == 1, "no memory isn't tried again padded: %d opens", opens);
    jpeg_no_memory = false;
    jpeg_process_fails = true;
    EXPECT_FAILURE("timer", "unavailable", "the JPEG encoder failed (error -1); try scale 2");
    CHECK(opens == 2 && closes == 1, "the encoder closed after a failed encode: %d opens, %d closes", opens, closes);
    jpeg_process_fails = false;
    jpeg_len = APPS_SNAPSHOT_JPEG_MAX + 1;
    EXPECT_FAILURE("timer", "unavailable", "the JPEG encoder failed (error -1); try scale 2");
    jpeg_len = 1234;
    render_fails = true;
    EXPECT_FAILURE("timer", "unavailable", "LVGL couldn't draw the screen into the snapshot");
    render_fails = false;
    screen_w = 0;
    EXPECT_FAILURE("timer", "unavailable", "the screen has no size");
    screen_w = 466;
    CHECK(writes == 0, "nothing written");
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "then one works: %s", result);
    EXPECT_CLEAN();
}

static void abandoned_drawing(void) {
    threaded = true;
    slow_render = true;
    slow_started = slow_released = false;
    frame_quit = frame_done = frame_go = false;
    CHECK(pthread_create(&lvgl_thread, NULL, lvgl_main, NULL) == 0, "no LVGL thread");
    const int64_t t0 = atomic_load(&now_ms);
    EXPECT_FAILURE("timer", "unavailable", "the screen took too long to draw the snapshot; try again");
    int64_t took = atomic_load(&now_ms) - t0;
    CHECK(took >= MUSE_APPS_SNAPSHOT_SCREEN_MS + APPS_SNAPSHOT_RENDER_MS &&
              took <= MUSE_APPS_SNAPSHOT_SCREEN_MS + APPS_SNAPSHOT_RENDER_MS + APPS_SNAPSHOT_POLL_MS,
          "given up %lld ms after the request", (long long)took);
    CHECK(slow_started && renders == 1, "the drawing had begun");
    /* While the drawing is still under way, a new snapshot is busy (its pixels are still being drawn into). */
    threaded = false;
    frames_on = false;
    EXPECT_FAILURE("timer", "busy", "the screen is still drawing the last snapshot; try again in a few seconds");
    /* The drawing ends: the LVGL task finds it given up and frees its pixels. */
    pthread_mutex_lock(&slow_mu);
    slow_released = true;
    pthread_cond_broadcast(&slow_cv);
    pthread_mutex_unlock(&slow_mu);
    pthread_mutex_lock(&frame_mu);
    while (!frame_done) {
        pthread_cond_wait(&frame_cv, &frame_mu);
    }
    frame_quit = true;
    pthread_cond_broadcast(&frame_cv);
    pthread_mutex_unlock(&frame_mu);
    CHECK(pthread_join(lvgl_thread, NULL) == 0, "the LVGL thread didn't end");
    slow_render = false;
    CHECK(dones == 2 && opens == 0 && writes == 0, "two answers, nothing encoded or written");
    EXPECT_CLEAN();
    /* And then a snapshot is taken. */
    frames_on = true;
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "after it: %s", result);
    EXPECT_CLEAN();
}

static void waited_too_long(void) {
    const char *c, *e;
    CHECK(muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e), "refused: %s %s", c, e);
    /* The job waits in the worker's queue (an install ahead of it) until less than MIN_LEFT is left. */
    atomic_fetch_add(&now_ms, MUSE_APPS_SNAPSHOT_TOTAL_MS - APPS_SNAPSHOT_MIN_LEFT_MS + 1);
    atomic_store(&dark_forever, true);
    run_jobs();
    char want[256];
    snprintf(want, sizeof want, "{\"ok\":false,\"error\":{\"code\":\"busy\",\"message\":\"the gadget was busy for %d ms; take the "
             "snapshot again\"}}", MUSE_APPS_SNAPSHOT_TOTAL_MS - APPS_SNAPSHOT_MIN_LEFT_MS + 1);
    CHECK(dones == 1 && strcmp(result, want) == 0, "%s", result);
    CHECK(wakes == 0 && renders == 0 && statuses == 0, "no wake, no drawing");
    /* Exactly MIN_LEFT left is enough. */
    atomic_store(&dark_forever, false);
    CHECK(muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e), "refused: %s %s", c, e);
    atomic_fetch_add(&now_ms, MUSE_APPS_SNAPSHOT_TOTAL_MS - APPS_SNAPSHOT_MIN_LEFT_MS);
    run_jobs();
    CHECK(dones == 2 && strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* A record dump of n widgets, each with a long text and overflow, into out. */
static void big_dump(char *out, size_t cap, int n) {
    size_t used = (size_t)snprintf(out, cap, "[");
    for (int i = 0; i < n && used < cap; i++) {
        char textbuf[201];
        memset(textbuf, 'a' + i % 26, 200);
        textbuf[200] = '\0';
        used += (size_t)snprintf(out + used, cap - used,
                                 "%s{\"id\":\"label#%d\",\"kind\":\"label\",\"text\":\"%s\",\"x\":0,\"y\":%d,\"w\":466,\"h\":20,"
                                 "\"hidden\":false,\"overflow\":%d}",
                                 i > 0 ? "," : "", i, textbuf, i, 10 + i);
    }
    CHECK(used + 2 <= cap, "the dump doesn't fit its buffer");
    snprintf(out + used, cap - used, "]");
}

static void ui_trimmed(void) {
    static char dump[16384];
    big_dump(dump, sizeof dump, 50);
    CHECK(strlen(dump) < sizeof dump - 1 && strlen(dump) > 14000, "the dump is %zu bytes", strlen(dump));
    record_dump = dump;
    record_truncated = 68;  /* the record's own dump left 68 out */
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(strlen(result) <= APPS_RESULT_MAX, "the result is %zu bytes, over %d", strlen(result), APPS_RESULT_MAX);
    cJSON *p = PAYLOAD();
    cJSON *ui = cJSON_GetObjectItemCaseSensitive(p, "ui");
    int kept = cJSON_GetArraySize(ui);
    CHECK(kept > 0 && kept < 50, "kept %d of 50", kept);
    CHECK(number(p, "ui_truncated") == 68 + 50 - kept, "ui_truncated %g, kept %d", number(p, "ui_truncated"), kept);
    /* Whole entries, from the start of the dump. */
    for (int i = 0; i < kept; i++) {
        char id[16];
        snprintf(id, sizeof id, "label#%d", i);
        CHECK(strcmp(text(cJSON_GetArrayItem(ui, i), "id"), id) == 0, "entry %d is %s", i, text(cJSON_GetArrayItem(ui, i), "id"));
    }
    /* The warnings, within their bytes, and the rest counted. */
    char w[8192];
    warnings_of(p, w, sizeof w);
    size_t sum = 0;
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(p, "warnings")) {
        sum += strlen(item->valuestring);
        n++;
    }
    CHECK(sum <= APPS_SNAPSHOT_WARNINGS_MAX + 64, "warnings are %zu bytes", sum);
    const char *last = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "warnings"), n - 1)->valuestring;
    char want[96];
    snprintf(want, sizeof want, "%d more widgets extend past the round edge", 50 - (n - 1));
    CHECK(strcmp(last, want) == 0, "the last warning is '%s', not '%s'", last, want);
    static const char first[] = "label#0 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa...' extends 10 px past the round edge|";
    CHECK(strncmp(w, first, sizeof first - 1) == 0, "a long text as messages show it: %s", w);
    cJSON_Delete(p);
    EXPECT_CLEAN();
}

static void every_allocation_refused(void) {
    /* Each allocation of a snapshot refused in turn, from its first: done runs exactly once (or the call refuses), and nothing is
     * held after. The count goes up until a snapshot needs no refusal. */
    for (long n = 1;; n++) {
        reset();
        refuse_at = n;
        const char *c = "", *e = "";
        int before = dones;
        bool queued = muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e);
        if (queued) {
            run_jobs();
            CHECK(dones == before + 1, "allocation %ld refused: done ran %d times", n, dones - before);
            if (!refused) {
                CHECK(strstr(result, "\"ok\":true") != NULL, "allocation %ld: nothing refused, yet %s", n, result);
                CHECK(n > 10, "only %ld allocations", n);
                break;
            }
            CHECK(result_null || strstr(result, "\"ok\":false") != NULL, "allocation %ld refused, yet %s", n, result);
        } else {
            CHECK(refused && strcmp(c, "out_of_memory") == 0, "allocation %ld: refused with %s %s", n, c, e);
        }
        if (blocks != 0) {
            fail_at(__LINE__, "allocation %ld refused: %d blocks held (port %d, encoder %d, cJSON %d)", n, blocks, port_blocks,
                    jpeg_blocks, cjson_blocks);
        }
        CHECK(n < 5000, "never done");
    }
    EXPECT_CLEAN();
}

static void drawing_time_is_stolen(void) {
    render_ms = 212;
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(steals == 1 && atomic_load(&stolen_ms) == 212, "stolen %u in %d", (unsigned)atomic_load(&stolen_ms), steals);
    /* A drawing that fails took the core all the same. */
    render_fails = true;
    render_ms = 50;
    EXPECT_FAILURE("timer", "unavailable", "LVGL couldn't draw the screen into the snapshot");
    CHECK(steals == 2 && atomic_load(&stolen_ms) == 262, "stolen %u in %d", (unsigned)atomic_load(&stolen_ms), steals);
    EXPECT_CLEAN();
}

static void lit_then_dark_before_the_take(void) {
    /* Lit when the job asks, but the idle timer would darken it before the next frame: the request's wake keeps it lit (a wake
     * only once dark would never come, and the job would time out with "didn't wake"). */
    atomic_store(&idle_dark_at, atomic_load(&now_ms) + 1);
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(strstr(result, "\"ok\":true") != NULL, "taken: %s", result);
    CHECK(wakes == 1 && wake_why != NULL && strcmp(wake_why, "snapshot") == 0, "woken once, as \"snapshot\": %d", wakes);
    CHECK(renders == 1, "drawn once: %d", renders);
    CHECK(!logged_has("waking the screen"), "a lit screen isn't said to be woken:\n%s", logged);
    EXPECT_CLEAN();
}

static void app_switched_after_the_take(void) {
    const char *c, *e;
    /* hello was on screen when it was drawn; timer runs by the time the job looks: the picture isn't timer's. */
    running_id = "hello";
    record_id = "hello";
    after_take_running = "timer";
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    cJSON *p = PAYLOAD();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "front")), "front false, as drawn: %s", result);
    char w[2048];
    warnings_of(p, w, sizeof w);
    CHECK(strcmp(w, "'timer' isn't running: the snapshot shows what is on screen") == 0, "warnings: %s", w);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "ui")) == 0, "no widgets: %s", result);
    cJSON_Delete(p);
    /* timer was drawn; hello runs by the time the job looks: the picture is timer's (front), and the record is hello's (no
     * widgets rather than another app's). */
    running_id = "timer";
    record_id = "timer";
    after_take_running = "hello";
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    p = PAYLOAD();
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "front")), "front true, as drawn: %s", result);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "ui")) == 0, "no widgets of another app: %s", result);
    cJSON_Delete(p);
    EXPECT_CLEAN();
}

static void write_gets_whats_left(void) {
    const char *c, *e;
    const int64_t asked = atomic_load(&now_ms);
    CHECK(muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e), "refused: %s %s", c, e);
    /* The job starts with MIN_LEFT (8 s) left, the screen lights 4.9 s later and takes 2 s to draw: 1.1 s is left for the
     * write, which takes 3 s. */
    atomic_fetch_add(&now_ms, MUSE_APPS_SNAPSHOT_TOTAL_MS - APPS_SNAPSHOT_MIN_LEFT_MS);
    atomic_store(&dark_until, atomic_load(&now_ms) + 4900);
    render_ms = 2000;
    write_takes_ms = 3000;
    run_jobs();
    CHECK(dones == 1, "done once: %d", dones);
    CHECK(renders == 1 && writes == 1, "drawn %d, written %d", renders, writes);
    CHECK(written_deadline_us == (asked + MUSE_APPS_SNAPSHOT_TOTAL_MS) * 1000LL, "the write's deadline is the command's: %lld",
          (long long)written_deadline_us);
    CHECK(written_left_ms > 1000 && written_left_ms <= 1100, "the write had what was left: %lld ms",
          (long long)written_left_ms);
    CHECK(strcmp(result, "{\"ok\":false,\"error\":{\"code\":\"unavailable\",\"message\":\"couldn't save the snapshot to "
                         ASYNC_OK_PATH ": no answer from the VM in time\"}}") == 0,
          "%s", result);
    CHECK(atomic_load(&now_ms) == asked + MUSE_APPS_SNAPSHOT_TOTAL_MS, "answered at the deadline");
    EXPECT_CLEAN();
}

static void encoder_buffers_are_psram(void) {
    /* esp_new_jpeg's internal-first allocators, which the linker sends here: zeroed PSRAM, never the Link's DMA heap. */
    unsigned char *a = __wrap_jpeg_calloc_inner(100);
    CHECK(a != NULL && caps_calls == 1, "jpeg_calloc_inner: %p in %d calls", (void *)a, caps_calls);
    CHECK(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "jpeg_calloc_inner asked for caps 0x%x", (unsigned)last_caps);
    unsigned char *b = __wrap_jpeg_calloc_align_inner(100, 16);
    CHECK(b != NULL && caps_calls == 2, "jpeg_calloc_align_inner: %p in %d calls", (void *)b, caps_calls);
    CHECK(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && last_alignment == 16 && (uintptr_t)b % 16 == 0,
          "jpeg_calloc_align_inner asked for caps 0x%x aligned to %zu", (unsigned)last_caps, last_alignment);
    for (int i = 0; i < 100; i++) {
        CHECK(a[i] == 0 && b[i] == 0, "byte %d isn't zero", i);
    }
    heap_caps_free(a);
    heap_caps_free(b);
    /* Out of PSRAM, the encoder hears no (jpeg_enc_open: JPEG_ERR_NO_MEM, the snapshot's out_of_memory): no internal RAM. */
    psram_out = true;
    CHECK(__wrap_jpeg_calloc_inner(100) == NULL && __wrap_jpeg_calloc_align_inner(100, 16) == NULL && caps_calls == 4,
          "out of PSRAM: %d calls", caps_calls);
    psram_out = false;
    /* N1: the library's other pair (PSRAM first, internal RAM when it is out) is PSRAM only too, and all four keep the apps' floor:
     * refused there, the heap isn't asked. */
    unsigned char *c = __wrap_jpeg_calloc(100);
    CHECK(c != NULL && caps_calls == 5 && last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "jpeg_calloc: caps 0x%x", (unsigned)last_caps);
    unsigned char *d = __wrap_jpeg_calloc_align(100, 16);
    CHECK(d != NULL && caps_calls == 6 && last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && last_alignment == 16 &&
              (uintptr_t)d % 16 == 0,
          "jpeg_calloc_align: caps 0x%x aligned to %zu", (unsigned)last_caps, last_alignment);
    for (int i = 0; i < 100; i++) {
        CHECK(c[i] == 0 && d[i] == 0, "byte %d isn't zero", i);
    }
    heap_caps_free(c);
    heap_caps_free(d);
    CHECK(room_asks == 6, "each asked the floor once: %d", room_asks);
    room_refuses = true;
    CHECK(__wrap_jpeg_calloc_inner(100) == NULL && __wrap_jpeg_calloc_align_inner(100, 16) == NULL && __wrap_jpeg_calloc(100) == NULL &&
              __wrap_jpeg_calloc_align(100, 16) == NULL && caps_calls == 6,
          "the floor refused, yet the heap was asked: %d calls", caps_calls);
    EXPECT_CLEAN();
}

/* ---- N1: the admission, the one block, the floor ---- */

/* What the encoder should see at scale: the screen's pattern masked out of place, into a picture of its own (FNV-1a, as the fake
 * encoder sums its input). */
static uint32_t expected_sum(int scale) {
    static uint16_t src[466 * 466], dst[466 * 466];
    for (int y = 0; y < 466; y++) {
        for (int x = 0; x < 466; x++) {
            src[y * 466 + x] = pattern(x, y);
        }
    }
    const int w = 466 / scale, h = 466 / scale;
    apps_snapshot_mask(src, 466, 466, 466, scale, dst, w);
    uint32_t sum = 2166136261u;
    for (int i = 0; i < w * h; i++) {
        sum = (sum ^ dst[i]) * 16777619u;
    }
    return sum;
}

/*
 * A snapshot is admitted before anything is drawn, against free PSRAM and the largest free block, each of which must hold its peak at
 * the asked scale above APPS_RAM_RESERVE (apps_snapshot.h): refused at the call, out_of_memory, with scale 2 suggested for scale 1
 * when scale 2 fits, stopping the app when one runs and nothing fits, and the plain words when none runs. The job admits it again,
 * so memory that went while it waited in the queue is a refusal before the drawing too. Nothing is drawn, encoded or written for a
 * refusal, and done runs once for a queued one.
 */
static void admission_before_drawing(void) {
    CHECK(apps_snapshot_block(466, 1) == 434320 + APPS_SNAPSHOT_JPEG_MAX, "scale 1's block: %zu", apps_snapshot_block(466, 1));
    CHECK(apps_snapshot_block(466, 2) == 434312, "scale 2's block: %zu", apps_snapshot_block(466, 2));
    CHECK(apps_snapshot_encoder_bytes(1) == APPS_SNAPSHOT_ENCODER_BYTES_1 && apps_snapshot_encoder_bytes(2) == APPS_SNAPSHOT_ENCODER_BYTES_2,
          "the encoder's allowances: %zu, %zu", apps_snapshot_encoder_bytes(1), apps_snapshot_encoder_bytes(2));
    CHECK(APPS_SNAPSHOT_ENCODER_BYTES_1 == 64 * 1024 && APPS_SNAPSHOT_ENCODER_BYTES_2 == 40 * 1024,
          "the allowances the board's measurement set (55,208 and 32,168 bytes at their peak, about a fifth kept over)");
    CHECK(apps_snapshot_peak(1) == apps_snapshot_block(466, 1) + APPS_SNAPSHOT_ENCODER_BYTES_1 &&
              apps_snapshot_peak(2) == apps_snapshot_block(466, 2) + APPS_SNAPSHOT_ENCODER_BYTES_2,
          "the peaks: %zu, %zu", apps_snapshot_peak(1), apps_snapshot_peak(2));
    const size_t need1 = APPS_RAM_RESERVE + apps_snapshot_peak(1), need2 = APPS_RAM_RESERVE + apps_snapshot_peak(2);
    const char *c, *e;
    /* Free PSRAM between the two: scale 1 is refused at the call with scale 2 suggested, and scale 2 is taken. */
    psram_free = need1 - 1;
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_TRY_SCALE_2);
    CHECK(strcmp(MUSE_APPS_SNAPSHOT_TRY_SCALE_2, "not enough memory for a full-size snapshot right now; take it at scale 2") == 0,
          "the words the guide quotes");
    CHECK(renders == 0 && wakes == 0 && opens == 0 && writes == 0, "a refusal drew, woke, encoded or wrote");
    CHECK(logged_has("snapshot timer: scale 1 refused: PSRAM free "), "the refusal's line:\n%s", logged);
    CHECK(snap("timer", 2, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale 2 where scale 1 isn't: %s %s %s", c, e, result);
    CHECK(renders == 1, "drawn once: %d", renders);
    /* The largest free block decides as well. */
    psram_free = 4 * 1024 * 1024;
    psram_largest = need1 - 1;
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_TRY_SCALE_2);
    psram_largest = need2;
    CHECK(snap("timer", 2, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale 2 at its block: %s", result);
    psram_largest = need2 - 1;
    /* Neither fits: stop the app that runs, or the plain words when none does. */
    EXPECT_REFUSAL("timer", 2, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_STOP_APP);
    EXPECT_REFUSAL("timer", 1, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_STOP_APP);
    running_id = NULL;
    EXPECT_REFUSAL("hello", 1, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_NO_MEMORY);
    running_id = "timer";
    psram_largest = 4 * 1024 * 1024;
    psram_free = need2 - 1;
    EXPECT_REFUSAL("timer", 2, done_cb, "out_of_memory", MUSE_APPS_SNAPSHOT_STOP_APP);
    /* Exactly the need is enough. */
    psram_free = psram_largest = need1;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale 1 at its need: %s %s %s", c, e, result);
    /* The job's own admission: the memory went while it waited in the queue. */
    psram_free = psram_largest = 4 * 1024 * 1024;
    CHECK(muse_apps_snapshot("timer", 1, done_cb, &dones, &c, &e), "queued: %s %s", c, e);
    psram_free = need1 - 1;
    const int drawn = renders, woken = wakes, before = dones;
    run_jobs();
    CHECK(dones == before + 1, "done ran %d times", dones - before);
    CHECK(strcmp(result, "{\"ok\":false,\"error\":{\"code\":\"out_of_memory\",\"message\":\"" MUSE_APPS_SNAPSHOT_TRY_SCALE_2 "\"}}") == 0,
          "the job's refusal: %s", result);
    CHECK(renders == drawn && wakes == woken, "the job's refusal drew or woke");
    psram_free = 4 * 1024 * 1024;
    CHECK(snap("timer", 1, &c, &e) && strstr(result, "\"ok\":true") != NULL, "and then one is taken: %s", result);
    EXPECT_CLEAN();
}

/*
 * One block, through the floor (the port's snapshot blocks), holds the whole sequence: the drawing, the picture masked in place at its
 * start (the encoder sees what an out-of-place mask makes, at both scales), the JPEG after it; and then only the JPEG, while it is
 * written. The block is what the admission counts (apps_snapshot_block), and nothing else of the snapshot is the encoder's library's.
 */
static void one_block_through_the_floor(void) {
    static const int scales[] = {1, 2};
    for (size_t i = 0; i < ARRAY_LEN(scales); i++) {
        const int scale = scales[i];
        snap_allocs = 0;
        snap_peak = 0;
        jpeg_len = 5000 + scale;
        const char *c, *e;
        CHECK(snap("timer", scale, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale %d: %s %s %s", scale, c, e, result);
        CHECK(snap_allocs == 1 && snap_sizes[0] == apps_snapshot_block(466, scale) && snap_peak == snap_sizes[0] + 6000 + 16 + 20000,
              "scale %d: %d blocks, the first %zu bytes (the admission's %zu), the peak %zu with the encoder's", scale, snap_allocs,
              snap_sizes[0], apps_snapshot_block(466, scale), snap_peak);
        CHECK(enc_held == snap_sizes[0] + 6000, "scale %d: %zu held as it encoded (the block and the encoder's state)", scale, enc_held);
        CHECK(enc_sum == expected_sum(scale), "scale %d: the encoder's input isn't the masked picture (%08x, not %08x)", scale, enc_sum,
              expected_sum(scale));
        CHECK(written_held == (size_t)jpeg_len, "scale %d: %zu held as it wrote, not the JPEG's %d", scale, written_held, jpeg_len);
        CHECK(written_len == (size_t)jpeg_len && written[0] == 0xFF && written[1] == 0xD8 && written[100] == (uint8_t)((100 * 31) & 0xFF),
              "scale %d: the JPEG's bytes moved to the block's start: %zu", scale, written_len);
        EXPECT_CLEAN();
    }
    CHECK(room_asks == 6, "each of the encoder's blocks asks the floor: %d asks", room_asks);
}

/* A padded picture (an encoder that refuses the size) can't be masked in place: its zeroed copy is the port's, through the floor, and
 * a refusal of it is out_of_memory with nothing held; the JPEG then goes into the drawing's block. */
static void the_padded_copy_is_floored(void) {
    jpeg_refuses_unaligned = true;
    const char *c, *e;
    CHECK(snap("timer", 2, &c, &e) && strstr(result, "\"ok\":true") != NULL, "padded: %s", result);
    CHECK(snap_allocs == 2 && snap_sizes[1] == 240 * 240 * 2, "the drawing's block and the copy: %d blocks, %zu", snap_allocs,
          snap_sizes[1]);
    CHECK(enc_pad == 0 && enc_corner == 0, "the copy is zeroed: pad %04x", enc_pad);
    CHECK(written_held == (size_t)jpeg_len, "%zu held as it wrote", written_held);
    EXPECT_CLEAN();
    for (long n = 1; n <= 2; n++) {  /* each of the two snapshot blocks refused: the drawing's, then the copy */
        reset();
        jpeg_refuses_unaligned = true;
        snap_refuse_at = (int)n;
        EXPECT_FAILURE("timer", "out_of_memory", n == 1 ? "out of memory: no room for the screen's pixels" :
                                                          "out of memory: no room for the masked pixels");
        CHECK(refused && opens == (n == 1 ? 0 : 2) && writes == 0, "refusal %ld: %d opens, %d writes", n, opens, writes);
        EXPECT_CLEAN();
    }
}

/* The app's waits for the snapshot's memory, as the port counts them, are said once the snapshot ends; none, nothing is said. */
static void the_apps_waits_are_logged(void) {
    const char *c, *e;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(!logged_has("the app waited"), "a wait was said with none:\n%s", logged);
    fake_waits = 2;
    fake_wait_ms = 120;
    CHECK(snap("timer", 1, &c, &e), "refused: %s %s", c, e);
    CHECK(logged_has("I muse_apps: snapshot timer: the app waited 120 ms for the snapshot's memory (2 times)\n"), "the line:\n%s",
          logged);
    EXPECT_CLEAN();
}

/* ---- Fix 1 ---- */

/* A: the encoder's blocks, from its open to its close, count with the snapshot's (apps_port_snapshot_adopt, _disown), so the app task's
 * wait and fit see them; their peak is in the encode's line, with free PSRAM after the encode (every buffer still held) and the
 * admission's allowance. Another task's use of the same allocators during the encode (the decoder, for display.draw_file), and any use
 * outside an encode, isn't counted. */
static void the_encoders_blocks_count_with_the_snapshot(void) {
    static const int scales[] = {1, 2};
    char want[200];
    for (size_t i = 0; i < ARRAY_LEN(scales); i++) {
        const int scale = scales[i];
        adopts = disowns = 0;
        logged_len = 0;
        logged[0] = '\0';
        decode_meanwhile = true;
        const char *c, *e;
        CHECK(snap("timer", scale, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale %d: %s %s %s", scale, c, e, result);
        CHECK(adopts == 3 && disowns == 3, "scale %d: %d adopted, %d disowned", scale, adopts, disowns);
        CHECK(decoder_counted == 0, "scale %d: the decoder's block was counted with the snapshot's (%d)", scale, decoder_counted);
        CHECK(enc_counted_then == apps_snapshot_block(466, scale) + 6000 + 16 + 20000, "scale %d: %zu counted with all three held", scale,
              enc_counted_then);
        snprintf(want, sizeof want, "PSRAM free 4194304 before, 4194304 with the encoder open, 4194304 after the encode; encoder peak "
                 "26016 bytes (%u allowed)\n", (unsigned)apps_snapshot_encoder_bytes(scale));
        CHECK(logged_has(want), "scale %d: the encode's line:\n%s", scale, logged);
        EXPECT_CLEAN();
    }
    void *outside = __wrap_jpeg_calloc_align(100, 16);
    CHECK(outside != NULL && adopts == 3 && snap_bytes == 0, "a block outside an encode was counted: %d adopts", adopts);
    __wrap_jpeg_free_align(outside);
    CHECK(disowns == 3, "a free outside an encode was disowned");
    EXPECT_CLEAN();
}

/* A: an encoder whose buffers find no room at the encode (after the admission) is out_of_memory, not unavailable: in the admission's
 * words then (scale 2 hinted when it fits), or its own when the admission would pass now. Nothing is written, and nothing is held. */
static void an_encoder_out_of_memory_is_out_of_memory(void) {
    huff_refused = true;
    EXPECT_FAILURE("timer", "out_of_memory", "out of memory: no room for the JPEG encoder's buffers");
    CHECK(closes == 1 && writes == 0, "%d closes, %d writes", closes, writes);
    EXPECT_CLEAN();
    huff_refused_free = APPS_RAM_RESERVE + apps_snapshot_peak(1) - 1;  /* the memory went meanwhile: scale 2 fits, scale 1 doesn't */
    EXPECT_FAILURE("timer", "out_of_memory", MUSE_APPS_SNAPSHOT_TRY_SCALE_2);
    CHECK(logged_has("snapshot timer: scale 1 refused: PSRAM free "), "the refusal's line:\n%s", logged);
    CHECK(writes == 0, "%d writes", writes);
    EXPECT_CLEAN();
}

/* E: no room for the block at the take, which can come up to MUSE_APPS_SNAPSHOT_SCREEN_MS after the admission (the screen's wake):
 * out_of_memory in the admission's words then, or the take's own when the admission would pass now. */
static void a_refusal_at_the_take_says_what_the_admission_says(void) {
    snap_refuse_at = 1;
    take_refused_free = APPS_RAM_RESERVE + apps_snapshot_peak(1) - 1;
    EXPECT_FAILURE("timer", "out_of_memory", MUSE_APPS_SNAPSHOT_TRY_SCALE_2);
    CHECK(renders == 0 && opens == 0 && writes == 0, "%d drawings, %d opens, %d writes", renders, opens, writes);
    CHECK(logged_has("snapshot timer: scale 1 refused: PSRAM free "), "the refusal's line:\n%s", logged);
    EXPECT_CLEAN();
    reset();
    snap_refuse_at = 1;
    EXPECT_FAILURE("timer", "out_of_memory", "out of memory: no room for the screen's pixels");
    EXPECT_CLEAN();
}

/* H: the admission counts the block the take asks for, whatever LVGL's row stride: with 64 bytes of alignment on each row the screen's
 * block at scale 2 is 996 x 466 bytes, and that is what the take takes; the picture is still masked in place. */
static void a_wider_stride_is_admitted_as_taken(void) {
    stride_pad = 64;
    CHECK(apps_snapshot_block(466, 2) == 996 * 466, "scale 2's block with the wider rows: %zu", apps_snapshot_block(466, 2));
    CHECK(apps_snapshot_block(466, 1) == 434320 + APPS_SNAPSHOT_JPEG_MAX, "scale 1's: %zu", apps_snapshot_block(466, 1));
    static const int scales[] = {1, 2};
    for (size_t i = 0; i < ARRAY_LEN(scales); i++) {
        const int scale = scales[i];
        snap_allocs = 0;
        const char *c, *e;
        CHECK(snap("timer", scale, &c, &e) && strstr(result, "\"ok\":true") != NULL, "scale %d: %s %s %s", scale, c, e, result);
        CHECK(snap_allocs == 1 && snap_sizes[0] == apps_snapshot_block(466, scale), "scale %d: the take's %zu, the admission's %zu", scale,
              snap_sizes[0], apps_snapshot_block(466, scale));
        CHECK(enc_sum == expected_sum(scale), "scale %d: the picture isn't the masked screen (%08x, not %08x)", scale, enc_sum,
              expected_sum(scale));
        EXPECT_CLEAN();
    }
}

static const struct {
    const char *name;
    void (*fn)(void);
} CASES[] = {
    {"refusals", refusals},
    {"not_installed", not_installed},
    {"in_front", in_front},
    {"half_size", half_size},
    {"padded_full_size", padded_full_size},
    {"dark_then_lit", dark_then_lit},
    {"dark_past_deadline", dark_past_deadline},
    {"lit_at_deadline", lit_at_deadline},
    {"not_in_front", not_in_front},
    {"write_fails", write_fails},
    {"encoder_failures", encoder_failures},
    {"abandoned_drawing", abandoned_drawing},
    {"waited_too_long", waited_too_long},
    {"ui_trimmed", ui_trimmed},
    {"every_allocation_refused", every_allocation_refused},
    {"drawing_time_is_stolen", drawing_time_is_stolen},
    {"encoder_buffers_are_psram", encoder_buffers_are_psram},
    {"lit_then_dark_before_the_take", lit_then_dark_before_the_take},
    {"app_switched_after_the_take", app_switched_after_the_take},
    {"write_gets_whats_left", write_gets_whats_left},
    {"admission_before_drawing", admission_before_drawing},
    {"one_block_through_the_floor", one_block_through_the_floor},
    {"the_padded_copy_is_floored", the_padded_copy_is_floored},
    {"the_apps_waits_are_logged", the_apps_waits_are_logged},
    {"the_encoders_blocks_count_with_the_snapshot", the_encoders_blocks_count_with_the_snapshot},
    {"an_encoder_out_of_memory_is_out_of_memory", an_encoder_out_of_memory_is_out_of_memory},
    {"a_refusal_at_the_take_says_what_the_admission_says", a_refusal_at_the_take_says_what_the_admission_says},
    {"a_wider_stride_is_admitted_as_taken", a_wider_stride_is_admitted_as_taken},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    cJSON_Hooks hooks = {cjson_malloc, cjson_free};
    cJSON_InitHooks(&hooks);
    for (size_t i = 0; i < ARRAY_LEN(CASES); i++) {
        if (strcmp(argv[1], CASES[i].name) == 0) {
            current_case = CASES[i].name;
            reset();
            CASES[i].fn();
            printf("%s: ok\n", current_case);
            return 0;
        }
    }
    fprintf(stderr, "no case %s\n", argv[1]);
    return 2;
}
