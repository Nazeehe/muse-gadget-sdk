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

#include "apps_snapshot.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_result.h"
#include "apps_rt.h"
#include "apps_task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "muse_apps.h"
#include "muse_input.h"

#if defined(ESP_PLATFORM) && !LV_USE_SNAPSHOT
#error "apps_snapshot.c draws the screen with lv_snapshot: set CONFIG_LV_USE_SNAPSHOT (devices/sdkconfig.muse-apps does)"
#endif

/*
 * app.control snapshot (apps_snapshot.h has how it runs, muse_apps.h the contract). The caller's side and the job are the I/O
 * worker's way of doing things (apps_control.c's): a job of its own in PSRAM, one done, the store read on the worker. The LVGL
 * task's side touches nothing but the handshake, LVGL and a block of PSRAM it allocates for the pixels.
 *
 * Memory, at the peak (a full-size snapshot): the screen's pixels (434 KB) and the masked copy (434 KB, or 460 KB padded to 480),
 * then the masked copy, the encoder's working buffers and the JPEG's 128 KB; the screen's block goes before the encoder runs. All of
 * it is PSRAM, the encoder's working buffers included (the wrapped allocators at the end of this file); the job logs the DMA heap
 * before and with the encoder open, so the Link's margin can be watched.
 */

static const char *TAG = "muse_apps";

#define BAD_ID "app id must match ^[a-z][a-z0-9_]{1,23}$"
#define ALREADY "another snapshot is being taken; try again in a few seconds"
#define SCREEN_BUSY "the screen is still drawing the last snapshot; try again in a few seconds"
#define DRAWING_SLOW "the screen took too long to draw the snapshot; try again"
#define NO_SIZE "the screen has no size"
#define NO_DRAWING "LVGL couldn't draw the screen into the snapshot"
#define NO_PIXELS "out of memory: no room for the screen's pixels"
#define SHOWN_MAX 40  /* text an app supplied, as a warning shows it (apps_task.c's overflow warnings) */
#define RESULT_SKELETON "{\"ok\":true,\"payload\":}"
#define UI_TRUNCATED_ROOM 24  /* ,"ui_truncated":NNNNN */
#define PATH_MAX_LEN (sizeof APPS_VM_DIR + APPS_ID_MAX + sizeof MUSE_APPS_SNAPSHOT_FILE)

/* ---- The handshake (apps_snapshot.h) ---- */

enum { CAP_IDLE, CAP_REQUESTED, CAP_TAKING, CAP_TAKEN, CAP_FAILED, CAP_ABANDONED };

/* The LVGL task's answer: the pixels (a block of apps_port_realloc, NULL when it failed, with why) and what it saw. */
typedef struct {
    uint16_t *px;
    int w, h, stride_px;
    bool layer_front;    /* the app layer was in front and no card covered it */
    char running[APPS_ID_MAX + 1];  /* the app that ran when it was drawn ("" for none) */
    uint32_t render_ms;  /* what the drawing took */
    const char *why;     /* a failure's, a literal */
    bool no_memory;
} shot_t;

static atomic_int s_cap;         /* the handshake's word */
static atomic_bool s_claimed;    /* a snapshot command is under way, from its admission to its done */
static _Atomic int64_t s_deadline_ms;  /* the job's, set before it says CAP_REQUESTED: a word the LVGL task can't read torn */
static shot_t s_shot;            /* the LVGL task's, set before it says CAP_TAKEN or CAP_FAILED */
static lv_draw_buf_t s_draw_buf; /* the LVGL task's: its pixels are s_shot's block */

void apps_snapshot_take_pending(void) {
    if (atomic_load(&s_cap) != CAP_REQUESTED || apps_port_dark() || apps_port_mono_ms() >= atomic_load(&s_deadline_ms)) {
        return;  /* nothing asked, the screen not lit yet (its first lit frame takes it), or too late: the job's deadline answers */
    }
    int expected = CAP_REQUESTED;
    if (!atomic_compare_exchange_strong(&s_cap, &expected, CAP_TAKING)) {
        return;  /* the job took it back at its deadline */
    }
    const int64_t began = apps_port_mono_ms();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_update_layout(screen);
    const int32_t w = lv_obj_get_width(screen), h = lv_obj_get_height(screen);
    const uint32_t stride = w > 0 && h > 0 ? lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_RGB565) : 0;
    const size_t size = (size_t)stride * (size_t)(h > 0 ? h : 0);
    shot_t shot = {.w = w, .h = h, .stride_px = (int)(stride / 2)};
    shot.px = size > 0 ? apps_port_realloc(NULL, size) : NULL;
    if (shot.px == NULL) {
        shot.why = size > 0 ? NO_PIXELS : NO_SIZE;
        shot.no_memory = size > 0;
    } else if (lv_draw_buf_init(&s_draw_buf, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, stride, shot.px, (uint32_t)size) !=
                   LV_RESULT_OK ||
               lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &s_draw_buf) != LV_RESULT_OK ||
               s_draw_buf.header.w != (uint32_t)w || s_draw_buf.header.h != (uint32_t)h) {
        apps_port_free(shot.px);
        shot.px = NULL;
        shot.why = NO_DRAWING;
    }
    shot.render_ms = (uint32_t)(apps_port_mono_ms() - began);
    apps_rt_steal_ms(shot.render_ms);  /* the drawing held the app task's core: not the app's time (apps_rt.h) */
    /* What the picture is of, read with it: the layer's place and the app that runs (its names lock is a mutex held only for
     * copies, so the wait is one copy's). */
    shot.layer_front = apps_lv_front() && !apps_lv_cards_shown();
    apps_task_running(shot.running, sizeof shot.running);
    s_shot = shot;
    expected = CAP_TAKING;
    if (!atomic_compare_exchange_strong(&s_cap, &expected, shot.px != NULL ? CAP_TAKEN : CAP_FAILED)) {
        /* The job gave it up (CAP_ABANDONED) while it was drawn: the pixels are nobody's but this task's. */
        apps_port_free(shot.px);
        s_shot.px = NULL;
        atomic_store(&s_cap, CAP_IDLE);
    }
}

/* ---- The mask ---- */

void apps_snapshot_mask(const uint16_t *src, int w, int h, int stride_px, int scale, uint16_t *dst, int dst_stride_px) {
    /* In doubled coordinates, so it is integers only: an output pixel (x, y) has its centre at (scale x + scale / 2) in the screen's
     * pixels, and is kept when that is within min(w, h) / 2 of the screen's centre (w / 2, h / 2). 32 bits hold it for sides up to
     * 16384. */
    const int32_t d = w < h ? w : h;
    const int32_t d2 = d * d;
    const int ow = w / scale, oh = h / scale;
    const unsigned n = (unsigned)(scale * scale);
    for (int y = 0; y < oh; y++) {
        const int32_t cy = 2 * scale * y + scale - h;
        const uint16_t *row = src + (size_t)y * (size_t)scale * (size_t)stride_px;
        uint16_t *out = dst + (size_t)y * (size_t)dst_stride_px;
        for (int x = 0; x < ow; x++) {
            const int32_t cx = 2 * scale * x + scale - w;
            if (cx * cx + cy * cy > d2) {
                out[x] = 0;
            } else if (scale == 1) {
                out[x] = row[x];
            } else {
                unsigned r = 0, g = 0, b = 0;  /* the block's mean, channel by channel, rounded */
                for (int j = 0; j < scale; j++) {
                    const uint16_t *p = row + (size_t)j * (size_t)stride_px + (size_t)x * (size_t)scale;
                    for (int i = 0; i < scale; i++) {
                        r += p[i] >> 11;
                        g += (p[i] >> 5) & 0x3Fu;
                        b += p[i] & 0x1Fu;
                    }
                }
                out[x] = (uint16_t)(((r + n / 2) / n) << 11 | ((g + n / 2) / n) << 5 | (b + n / 2) / n);
            }
        }
    }
}

/* ---- The job (the I/O worker's) ---- */

typedef struct {
    char id[APPS_ID_MAX + 1];
    int scale;
    const muse_apps_vm_ops_t *vm;
    muse_apps_done_cb done;
    void *ctx;
    int64_t stamp_ms, deadline_ms;  /* apps_port_mono_ms when it was asked for, and the whole snapshot's end */
} snap_job_t;

/* The encoded picture: the JPEG (a block of apps_port_realloc) and its sizes. */
typedef struct {
    uint8_t *jpeg;
    int len;
    int w, h;            /* the picture's: the screen's, or half */
    int file_w, file_h;  /* the JPEG's: the same, or padded to multiples of 16 */
} encoded_t;

static void snapshot_path(const char *id, char *out, size_t cap) {
    snprintf(out, cap, APPS_VM_DIR "%s/" MUSE_APPS_SNAPSHOT_FILE, id);
}

/* Asks the LVGL task for the screen, waking it if it is dark (E9), and waits for its answer until the screen's deadline (and, for a
 * drawing begun before it, APPS_SNAPSHOT_RENDER_MS more), running the app task's ops as it looks. True with *shot the pixels (the
 * caller's to free); otherwise *r is the refusal. */
static bool capture(const snap_job_t *job, shot_t *shot, cJSON **r) {
    if (atomic_load(&s_cap) != CAP_IDLE) {
        *r = apps_result_failure(APPS_ERR_BUSY, SCREEN_BUSY);  /* a drawing given up on is still drawing into its pixels */
        return false;
    }
    if (apps_port_dark()) {
        ESP_LOGI(TAG, "snapshot %s: waking the screen", job->id);
    }
    /* Always: it lights a dark screen, and pokes a lit one's idle timer, so that the screen can't go dark before a frame takes
     * it (a wake only for a screen found dark would never come for that one, and the request would time out). */
    muse_input_wake("snapshot");
    const int64_t deadline = apps_port_mono_ms() + MUSE_APPS_SNAPSHOT_SCREEN_MS;
    atomic_store(&s_deadline_ms, deadline);
    memset(&s_shot, 0, sizeof s_shot);
    atomic_store(&s_cap, CAP_REQUESTED);
    const TickType_t ticks = pdMS_TO_TICKS(APPS_SNAPSHOT_POLL_MS) > 0 ? pdMS_TO_TICKS(APPS_SNAPSHOT_POLL_MS) : 1;
    for (;;) {
        int state = atomic_load(&s_cap);
        if (state == CAP_TAKEN || state == CAP_FAILED) {
            break;
        }
        const int64_t now = apps_port_mono_ms();
        if (now >= deadline) {
            int expected = state;
            if (state == CAP_REQUESTED && atomic_compare_exchange_strong(&s_cap, &expected, CAP_IDLE)) {
                ESP_LOGW(TAG, "snapshot %s: the screen wasn't lit and drawn within %d ms", job->id, MUSE_APPS_SNAPSHOT_SCREEN_MS);
                /* A frame after this finds nothing to take. */
                *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_SNAPSHOT_NO_WAKE);
                return false;
            }
            if (state == CAP_TAKING && now >= deadline + APPS_SNAPSHOT_RENDER_MS &&
                atomic_compare_exchange_strong(&s_cap, &expected, CAP_ABANDONED)) {
                ESP_LOGW(TAG, "snapshot %s: the drawing didn't end %d ms after the deadline: given up", job->id,
                         APPS_SNAPSHOT_RENDER_MS);
                *r = apps_result_failure(APPS_ERR_UNAVAILABLE, DRAWING_SLOW);  /* the LVGL task frees the pixels when it is done */
                return false;
            }
            if (expected != state) {
                continue;  /* it moved on as the deadline came: look again */
            }
        }
        apps_io_assist();
        vTaskDelay(ticks);
    }
    *shot = s_shot;
    const bool taken = atomic_load(&s_cap) == CAP_TAKEN;
    atomic_store(&s_cap, CAP_IDLE);
    if (!taken) {
        *r = apps_result_failure(shot->no_memory ? APPS_ERR_OUT_OF_MEMORY : APPS_ERR_UNAVAILABLE,
                                 shot->why != NULL ? shot->why : NO_DRAWING);
        return false;
    }
    return true;
}

static int pad16(int v) {
    return (v + 15) / 16 * 16;
}

static jpeg_error_t open_encoder(int w, int h, jpeg_enc_handle_t *enc) {
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;  /* as LVGL keeps RGB565: the low byte first */
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = APPS_SNAPSHOT_QUALITY;
    return jpeg_enc_open(&cfg, enc);
}

/* The pixels masked, scaled and encoded: true with *e the JPEG; otherwise *r is the refusal. The screen's block (shot->px) is freed
 * either way, as soon as it has been masked. answered_ms is how long the LVGL task took to answer, for the log. */
static bool encode(const snap_job_t *job, shot_t *shot, uint32_t answered_ms, encoded_t *e, cJSON **r) {
    memset(e, 0, sizeof *e);
    const int w = shot->w / job->scale, h = shot->h / job->scale;
    const size_t dma_before = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const int64_t began = apps_port_mono_ms();
    char msg[96];
    jpeg_enc_handle_t enc = NULL;
    int fw = w, fh = h;
    jpeg_error_t err = open_encoder(fw, fh, &enc);
    if (err != JPEG_ERR_OK && err != JPEG_ERR_NO_MEM && (pad16(w) != w || pad16(h) != h)) {
        fw = pad16(w);  /* D22: a size the encoder refuses, padded with black to the next multiple of 16 */
        fh = pad16(h);
        err = open_encoder(fw, fh, &enc);
    }
    if (err != JPEG_ERR_OK) {
        apps_port_free(shot->px);
        shot->px = NULL;
        snprintf(msg, sizeof msg, "the JPEG encoder refused %dx%d (error %d)", fw, fh, (int)err);
        *r = apps_result_failure(err == JPEG_ERR_NO_MEM ? APPS_ERR_OUT_OF_MEMORY : APPS_ERR_UNAVAILABLE, msg);
        return false;
    }
    const size_t dma_open = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t in_len = (size_t)fw * (size_t)fh * 2;
    uint16_t *in = jpeg_calloc_align(in_len, 16);  /* zeroed: the padding is black */
    if (in == NULL) {
        jpeg_enc_close(enc);
        apps_port_free(shot->px);
        shot->px = NULL;
        *r = apps_result_failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the masked pixels");
        return false;
    }
    apps_snapshot_mask(shot->px, shot->w, shot->h, shot->stride_px, job->scale, in, fw);
    apps_port_free(shot->px);
    shot->px = NULL;
    uint8_t *out = apps_port_realloc(NULL, APPS_SNAPSHOT_JPEG_MAX);
    int len = 0;
    if (out != NULL) {
        err = jpeg_enc_process(enc, (const uint8_t *)in, (int)in_len, out, APPS_SNAPSHOT_JPEG_MAX, &len);
    }
    const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);  /* the worker's, with the encoder's frames on it */
    jpeg_enc_close(enc);
    jpeg_free_align(in);
    if (out == NULL) {
        *r = apps_result_failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the JPEG");
        return false;
    }
    if (err != JPEG_ERR_OK || len <= 0 || len > APPS_SNAPSHOT_JPEG_MAX) {
        apps_port_free(out);
        snprintf(msg, sizeof msg, "the JPEG encoder failed (error %d); try scale 2", (int)err);
        *r = apps_result_failure(APPS_ERR_UNAVAILABLE, msg);
        return false;
    }
    char padded[32] = "";
    if (fw != w || fh != h) {
        snprintf(padded, sizeof padded, ", padded to %dx%d", fw, fh);
    }
    ESP_LOGI(TAG,
             "snapshot %s: %dx%d jpeg %d bytes (q%d%s), drawn in %u ms, encoded in %u ms, answered %u ms after the request; "
             "io stack %u of %u free; DMA free %u before, %u with the encoder open, lowest %u",
             job->id, w, h, len, APPS_SNAPSHOT_QUALITY, padded, (unsigned)shot->render_ms,
             (unsigned)(apps_port_mono_ms() - began), (unsigned)answered_ms, (unsigned)stack_free, (unsigned)APPS_IO_STACK,
             (unsigned)dma_before, (unsigned)dma_open, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
    *e = (encoded_t){.jpeg = out, .len = len, .w = w, .h = h, .file_w = fw, .file_h = fh};
    return true;
}

/* The widgets that extend past the round edge, as the run's warnings say them, from the dump's entries (visible ones with an
 * overflow): at most APPS_SNAPSHOT_WARNINGS_MAX bytes of them, then how many more. False without memory. */
static bool add_overflow_warnings(cJSON *warnings, const cJSON *all) {
    size_t used = 0;
    int more = 0;
    char item[APPS_ID_MAX + SHOWN_MAX + 96];
    const cJSON *w;
    cJSON_ArrayForEach(w, all) {
        const cJSON *overflow = cJSON_GetObjectItemCaseSensitive(w, "overflow");
        if (!cJSON_IsNumber(overflow) || overflow->valuedouble <= 0 || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "hidden"))) {
            continue;
        }
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(w, "id"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(w, "text"));
        if (text == NULL || text[0] == '\0') {
            text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(w, "kind"));
        }
        char shown[SHOWN_MAX + 4];
        text = text != NULL ? text : "widget";
        apps_text_shown(text, strlen(text), SHOWN_MAX, shown);
        snprintf(item, sizeof item, "%s '%s' extends %d px past the round edge", id != NULL ? id : "widget", shown,
                 (int)overflow->valuedouble);
        size_t len = strlen(item);
        if (more > 0 || used + len > APPS_SNAPSHOT_WARNINGS_MAX) {
            more++;
            continue;
        }
        if (!cJSON_AddItemToArray(warnings, cJSON_CreateString(item))) {
            return false;
        }
        used += len;
    }
    if (more > 0) {
        snprintf(item, sizeof item, more == 1 ? "%d more widget extends past the round edge" : "%d more widgets extend past the round edge",
                 more);
        return cJSON_AddItemToArray(warnings, cJSON_CreateString(item));
    }
    return true;
}

/* Moves the dump's entries, from its start and whole, into ui while the result p will make still prints in APPS_RESULT_MAX bytes;
 * *left is how many stay behind. False without memory. */
static bool fill_ui(const cJSON *p, cJSON *ui, cJSON *all, int *left) {
    char *text = cJSON_PrintUnformatted(p);
    if (text == NULL) {
        return false;
    }
    long room = (long)APPS_RESULT_MAX - (long)strlen(text) - (long)(sizeof RESULT_SKELETON - 1) - UI_TRUNCATED_ROOM;
    cJSON_free(text);
    *left = cJSON_GetArraySize(all);
    for (int kept = 0; all->child != NULL; kept++) {
        cJSON *entry = all->child;
        char *s = cJSON_PrintUnformatted(entry);
        if (s == NULL) {
            return false;
        }
        long need = (long)strlen(s) + (kept > 0 ? 1 : 0);
        cJSON_free(s);
        if (need > room) {
            break;
        }
        cJSON_DetachItemViaPointer(all, entry);
        cJSON_AddItemToArray(ui, entry);
        room -= need;
        (*left)--;
    }
    return true;
}

/* The success's payload (muse_apps.h): NULL without memory. runs says id is the app that runs, front that it is on screen. */
static cJSON *payload(const snap_job_t *job, const encoded_t *e, bool runs, bool front) {
    char path[PATH_MAX_LEN];
    snapshot_path(job->id, path, sizeof path);
    apps_status_t *st = NULL;
    cJSON *all = NULL, *warnings = NULL, *ui = NULL;
    cJSON *p = cJSON_CreateObject();
    bool ok = p != NULL && cJSON_AddStringToObject(p, "path", path) != NULL && cJSON_AddStringToObject(p, "format", "jpeg") != NULL &&
              cJSON_AddNumberToObject(p, "w", e->w) != NULL && cJSON_AddNumberToObject(p, "h", e->h) != NULL &&
              cJSON_AddNumberToObject(p, "bytes", e->len) != NULL && cJSON_AddBoolToObject(p, "front", front) != NULL &&
              (warnings = cJSON_AddArrayToObject(p, "warnings")) != NULL;
    char note[APPS_ID_MAX + 112];
    if (ok && !front) {
        snprintf(note, sizeof note, runs ? "'%s' isn't on screen: the snapshot shows what is"
                                         : "'%s' isn't running: the snapshot shows what is on screen",
                 job->id);
        ok = cJSON_AddItemToArray(warnings, cJSON_CreateString(note));
    }
    if (ok && (e->file_w != e->w || e->file_h != e->h)) {
        snprintf(note, sizeof note, "the JPEG is %dx%d: the encoder takes sizes in steps of 16, so its edges are black padding",
                 e->file_w, e->file_h);
        ok = cJSON_AddItemToArray(warnings, cJSON_CreateString(note));
    }
    /* The app's widgets: the record's dump, for the app that runs, whose record it is (not one that has just been replaced). */
    const char *dump = "[]";
    int before = 0;
    if (ok && runs) {
        st = apps_port_realloc(NULL, sizeof *st);  /* several KB: never on a stack (X12) */
        ok = st != NULL;
        if (ok) {
            apps_task_status(st);
            if (strcmp(st->id, job->id) == 0 && st->ui_dump[0] == '[') {
                dump = st->ui_dump;
                before = st->ui_truncated;
            }
        }
    }
    int left = 0;
    ok = ok && (all = cJSON_Parse(dump)) != NULL && cJSON_IsArray(all) && add_overflow_warnings(warnings, all) &&
         (ui = cJSON_AddArrayToObject(p, "ui")) != NULL && fill_ui(p, ui, all, &left);
    if (ok && before + left > 0) {
        ok = cJSON_AddNumberToObject(p, "ui_truncated", before + left) != NULL;
    }
    cJSON_Delete(all);
    apps_port_free(st);
    if (!ok) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}

/* The JPEG to workspace/apps/<id>/snapshot.jpg on the VM, by the whole snapshot's deadline: true once written; otherwise *r is the
 * failure, with the writer's code (busy, out_of_memory, else unavailable) and words. */
static bool save(const snap_job_t *job, const encoded_t *e, cJSON **r) {
    char path[PATH_MAX_LEN];
    snapshot_path(job->id, path, sizeof path);
    char why[192] = "";
    const char *code = MUSE_APPS_VM_FAILED;
    const int64_t began = apps_port_mono_ms();
    if (job->vm->write(path, e->jpeg, (size_t)e->len, job->deadline_ms * 1000, &code, why, sizeof why)) {
        ESP_LOGI(TAG, "snapshot %s: wrote %s (%d bytes) in %u ms (DMA free %u, lowest %u)", job->id, path, e->len,
                 (unsigned)(apps_port_mono_ms() - began), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
        return true;
    }
    code = code != NULL ? code : MUSE_APPS_VM_FAILED;
    ESP_LOGW(TAG, "snapshot %s: couldn't write %s: %s (%s, DMA lowest %u)", job->id, path, why, code,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
    char msg[PATH_MAX_LEN + sizeof why + 40];
    snprintf(msg, sizeof msg, "couldn't save the snapshot to %s: %s", path, why);
    *r = apps_result_failure(apps_result_vm_code(code), msg);
    return false;
}

/* The whole snapshot, on the worker: its result (NULL without memory for it). */
static cJSON *snapshot_result(const snap_job_t *job) {
    const int64_t at = apps_port_mono_ms();
    if (job->deadline_ms - at < APPS_SNAPSHOT_MIN_LEFT_MS) {  /* it waited in the worker's queue: nothing is done */
        char msg[80];
        snprintf(msg, sizeof msg, "the gadget was busy for %d ms; take the snapshot again", (int)(at - job->stamp_ms));
        return apps_result_failure(APPS_ERR_BUSY, msg);
    }
    cJSON *r = NULL;
    shot_t shot;
    if (!apps_result_installed(job->id, &r) || !capture(job, &shot, &r)) {
        return r;
    }
    const uint32_t answered_ms = (uint32_t)(apps_port_mono_ms() - at);
    const bool runs = strcmp(shot.running, job->id) == 0;  /* as drawn: an app that has begun since isn't in the picture */
    const bool front = runs && shot.layer_front;
    encoded_t e;
    if (!encode(job, &shot, answered_ms, &e, &r)) {
        return r;
    }
    cJSON *p = payload(job, &e, runs, front);
    bool saved = p != NULL && save(job, &e, &r);
    apps_port_free(e.jpeg);
    if (!saved) {
        cJSON_Delete(p);
        return r;  /* the write's failure, or NULL without memory for the payload */
    }
    return apps_result_success(p);
}

static void snap_run(void *arg) {
    snap_job_t *job = arg;
    ESP_LOGI(TAG, "snapshot %s: scale %d", job->id, job->scale);
    cJSON *r = snapshot_result(job);
    if (r == NULL) {
        r = apps_result_failure(APPS_ERR_OUT_OF_MEMORY, APPS_NO_ROOM_RESULT);
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(r, "error");
    if (error != NULL) {
        ESP_LOGI(TAG, "snapshot %s: %s: %s", job->id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code")),
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message")));
    }
    atomic_store(&s_claimed, false);  /* before done: a snapshot asked once the result is read isn't busy */
    job->done(job->ctx, r);
    apps_port_free(job);
}

bool muse_apps_snapshot(const char *id, int scale, muse_apps_done_cb done, void *ctx, const char **code, const char **err) {
    const char *unused;
    code = code != NULL ? code : &unused;
    err = err != NULL ? err : &unused;
    *code = APPS_ERR_INVALID_PARAMS;
    if (done == NULL) {
        *err = "no callback for the result";
        return false;
    }
    if (id == NULL || !apps_id_valid(id)) {
        *err = BAD_ID;
        return false;
    }
    if (scale != 1 && scale != 2) {
        *err = MUSE_APPS_SNAPSHOT_BAD_SCALE;
        return false;
    }
    const muse_apps_vm_ops_t *vm = muse_apps_vm_ops();
    if (vm == NULL || vm->write == NULL) {
        *code = APPS_ERR_UNAVAILABLE;
        *err = "the gadget can't write files on the VM";
        return false;
    }
    if (atomic_exchange(&s_claimed, true)) {
        *code = APPS_ERR_BUSY;
        *err = ALREADY;
        return false;
    }
    *code = APPS_ERR_OUT_OF_MEMORY;
    if (apps_port_psram_free() < APPS_SNAPSHOT_PSRAM_MIN) {
        atomic_store(&s_claimed, false);
        *err = MUSE_APPS_SNAPSHOT_NO_MEMORY;
        return false;
    }
    snap_job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job == NULL) {
        atomic_store(&s_claimed, false);
        *err = "out of memory: no room for the command";
        return false;
    }
    memset(job, 0, sizeof *job);
    snprintf(job->id, sizeof job->id, "%s", id);
    job->scale = scale;
    job->vm = vm;
    job->done = done;
    job->ctx = ctx;
    job->stamp_ms = apps_port_mono_ms();
    job->deadline_ms = job->stamp_ms + MUSE_APPS_SNAPSHOT_TOTAL_MS;  /* stamped here: the queue's wait is spent too */
    if (!apps_io_submit(snap_run, job)) {  /* it says why, in one log line */
        apps_port_free(job);
        atomic_store(&s_claimed, false);
        *err = APPS_IO_REFUSED;
        return false;
    }
    return true;
}

/* ---- esp_new_jpeg's working buffers ---- */

/*
 * esp_new_jpeg asks for its working buffers with jpeg_calloc_inner and jpeg_calloc_align_inner: internal RAM first, PSRAM when
 * that is out. Internal RAM is the Link's DMA heap: on the 1.75C the encoder's 8 KB there took it from 24211 bytes to 16127, under
 * the Link's 16 KB margin, for as long as the encoder was open. The linker sends the library's calls here
 * (components/muse/CMakeLists.txt): PSRAM only, so a PSRAM that is out is the encoder's JPEG_ERR_NO_MEM (the snapshot's
 * out_of_memory), never the Link's margin. The library frees both kinds with heap_caps_free, which takes PSRAM blocks too.
 */
void *__wrap_jpeg_calloc_inner(size_t size) {
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned) {
    return heap_caps_aligned_calloc((size_t)aligned, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
