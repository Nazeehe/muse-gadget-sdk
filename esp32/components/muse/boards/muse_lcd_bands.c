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

#include "muse_lcd_bands.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_mem.h"
#if CONFIG_MUSE_APPS
#include <stdio.h>
#include "esp_async_memcpy.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "src/display/lv_display_private.h"   /* a refresh's areas and the band being drawn, for the pair (on_render_event) */
#endif

typedef struct {
    const uint8_t *data;   /* NULL: run s_call instead */
    int x1, y1, x2, y2;
#if CONFIG_MUSE_APPS
    bool last;             /* the refresh's last band (lv_display_flush_is_last) */
    bool pair;             /* the first of a refresh of two: held until LVGL has drawn the second, then sent with it */
    bool whole;            /* a band of a whole-screen refresh in frame mode: copied into the frame, the last sends it whole */
    uint32_t drawn;        /* s_drawn as LVGL flushed it */
    int32_t render_us;     /* the last band's: LVGL's drawing of the refresh, its waits for the wire left out (bench) */
#endif
} band_t;
#if CONFIG_MUSE_APPS

/* A write of the panel's rows [y1, y2), started at t0 and taking w (us). */
typedef struct {
    int y1, y2;
    int64_t t0, w;
} write_t;
#if CONFIG_MUSE_BENCH
/* The bench's detail of a band's hold: what te_hold said (0 clear at once, 1 after a wait, 2 never clear), or 3 if it
 * went with the band before, as a pair's second; and the write it expected. */
typedef struct {
    int how;
    int64_t est;
} hold_t;
#endif
#endif

static const char *TAG = "lcd_bands";

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static int s_gap_x, s_gap_y;
static QueueHandle_t s_bands;            /* room for LVGL's band and a call */
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_call_done;
static void (*s_call)(void *);
static void *s_call_arg;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Tearing effect (muse_lcd_bands_set_te): the last scan's start, the period, the blanking. */
static bool s_te_on;
static int s_te_gpio = -1;
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;

/* Writes timed against the scan, for the stats (te_hold, te_done). */
typedef struct {
    const char *what;
    uint32_t n, held, torn;
    int64_t sum_us;
} te_stats_t;
static te_stats_t s_te_ui = { .what = "UI band" }, s_te_video = { .what = "video frame" };
static int s_rows;                       /* the panel's */
#if CONFIG_MUSE_APPS
/* A band's write, learned from the ones before (pace_learn): about fixed_us +
 * bytes * ns_per_kb / 1024000, a smoothed least-squares line over the size. */
typedef struct {
    float mean_kb, mean_us, var_kb, cov;
    int64_t fixed_us, ns_per_kb;
} pace_t;
#define PACE_VAR_MIN 100.0f   /* KB^2: the sizes' spread, about 10 KB, a slope is learned from */
static pace_t s_pace;
#else
/* A band's write, learned from the ones before: about s_fixed_us + bytes *
 * s_ns_per_kb / 1024000 (a smoothed least-squares line over the size). */
static float s_mean_kb, s_mean_us, s_var_kb, s_cov;
static int64_t s_fixed_us, s_ns_per_kb;
#endif
static volatile int64_t s_band_end;      /* when the last band's last piece went */
static int64_t s_band_start, s_band_fall, s_band_p, s_band_scan;
static int s_band_y1, s_band_y2;
static size_t s_band_bytes;
#if CONFIG_MUSE_APPS

/*
 * A refresh's bands go out as LVGL draws them, and a scan that comes between two of them shows one refresh's top
 * over the last one's bottom. LVGL has two buffers, so a refresh of two bands can be sent whole: the first is held
 * (LVGL's flush of it left open) until LVGL has drawn the second into the other buffer and waits to flush it, then
 * the two go back to back in a moment that keeps both clear of the scan (te_band, frame_clear_in).
 */
#define FRAME_HOLD_MAX 2                 /* the bands held and sent together: LVGL's two buffers */
#define TE_SPREAD 50                     /* a band's write, as a percentage of its estimate, either way */
#define TE_JITTER_US 1000                /* how late a held write may start after its moment */
#define PAIR_WAIT_MS 200                 /* for LVGL to draw a pair's second band, at most */
static bool s_pairing;                   /* LVGL task: muse_lcd_bands_set_pairing; off, each band is held on its own */
static int s_parts, s_flushed;           /* LVGL task: the refresh's bands (refresh_parts), and those flushed so far */
static volatile uint32_t s_drawn;        /* LV_EVENT_FLUSH_WAIT_START's count: LVGL has drawn a band */
static lv_area_t s_drawn_area;           /* the band it has drawn (under s_lock) */
static SemaphoreHandle_t s_drawn_sem;    /* given with s_drawn */
static bool s_follows;                   /* send task: the next band is a pair's second, planned with the first */
static int s_follow_y1, s_follow_y2;
static int64_t s_gap_us = 300;           /* from a band's end to the next one's start, when it follows (learned) */
static esp_timer_handle_t s_wake_timer;  /* ends a hold to the microsecond (sleep_until) */
static SemaphoreHandle_t s_wake;         /* wakes the send task: a hold's timer (wake_send), or a copy's end by DMA (on_copied) */

/*
 * LVGL's wait for the band before to be flushed (flush_wait, its flush_wait_cb) blocks on s_flush_ready, which every
 * lv_display_flush_ready gives, rather than spinning on disp->flushing at the UI task's priority through a band's write, or in
 * frame mode through a frame's hold and write (about 34 ms). It checks the flag again at each wake, and every FLUSH_WAIT_MS.
 */
#define FLUSH_WAIT_MS 10
static SemaphoreHandle_t s_flush_ready;

/*
 * Frame mode (muse_lcd_bands_set_frame_mode), while the home screen's pages slide: each step redraws the whole screen, which
 * takes about two scans' time on the wire as LVGL's four bands, so the next scan always overtakes it between two of them. In
 * frame mode such a refresh's bands are copied into one frame as LVGL flushes them, by DMA where it can (dma_copy), each given
 * back to LVGL once its copy has ended, and the last sends the frame as one write through one window (send_frame's framing),
 * held to just after a scan's start: slower than the scan, the write stays behind the scan that has just started and ahead of
 * the next one, however long LVGL took to draw. The state is in PSRAM: only the LVGL and send tasks touch it, with plain loads
 * and stores.
 */
#define WHOLE_SPREAD 10                  /* a frame's write, as a percentage of the frames' learned time, either way */
#define WHOLE_RESERVE (1024 * 1024)      /* PSRAM the frame leaves free: the apps' floor (APPS_RAM_RESERVE), for Muse's session and LVGL */
typedef struct {
    uint8_t *px;               /* the frame: the screen, s_cols x s_rows at 2 bytes, made as frame mode registers, and kept */
    bool failed;               /* it couldn't be made: frame mode is off for the boot */
    bool made_untold;          /* set as the frame is made (muse_lcd_bands_make_frame), cleared by the send task as it logs that */
    bool failed_untold;        /* the same for its failure */
    int at;                    /* LVGL task: the rows of the refresh in frame mode flushed so far */
    int staged;                /* send task: the rows copied into px so far */
    int64_t us;                /* send task: a frame's write, learned from the ones before (0: none timed yet) */
} whole_t;
static EXT_RAM_BSS_ATTR whole_t s_whole;
static EXT_RAM_BSS_ATTR bool s_framing, s_framed;   /* LVGL task: frame mode on (with its frame); the refresh under way in it */
static EXT_RAM_BSS_ATTR te_stats_t s_te_whole;      /* the frames' writes against the scan ("UI frame") */
static EXT_RAM_BSS_ATTR int s_cols;                 /* the panel's */

/*
 * Frame mode's copies by DMA (esp_async_memcpy, ESP-IDF v6.0.1's GDMA driver): the send task starts a band's copy into the frame
 * and waits for its end, blocked, so the CPU is LVGL's to draw the next band meanwhile; the band goes back to LVGL only then. The
 * driver takes internal RAM (its context and channels, and each copy's descriptors and stash, kept until the next copy), so it is
 * installed at a slide's first copy and uninstalled once frame mode is off (dma_end): installed only while internal DMA RAM keeps
 * DMA_KEEP and DMA_TAKES free, and used only while it keeps DMA_KEEP. A band it can't take (no room, no channel, a copy refused or
 * never ended) is copied by CPU, as before.
 *
 * The driver keeps the caches right (async_memcpy_gdma.c): it writes the band back from the cache before the DMA reads it
 * (esp_cache_msync, C2M), invalidates the frame's cache lines it writes before it starts (M2C, esp_dma_split_rx_buffer_to_cache_aligned),
 * and copies the lines it shares at either end through a stash in internal RAM, by CPU in its interrupt
 * (esp_dma_merge_aligned_rx_buffers). Nothing reads or writes a band's rows of the frame while its copy is under way (the send task
 * waits for it, and the frame's write comes after the last), so no stale line comes back. The S3's GDMA reads PSRAM at any alignment
 * but writes it in whole bursts (GDMA_LL_AHB_RX_BURST_NEEDS_ALIGNMENT): the destination and the length must be whole DMA_BURSTs
 * (check_buffer_alignment), and a band's rows of the frame are 932 B apart. So a band's bulk, from its first byte on a burst of the
 * frame to its last whole burst, goes by DMA, and its ends, under a burst each and on cache lines of their own, by CPU meanwhile.
 * The panel's SPI DMA never reads the frame: whole_write copies it into the internal buffers.
 *
 * The driver's write-back is one critical section over the whole band (esp_cache_msync.c, with
 * CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS off, as here): up to the cache's 64 KB of dirty lines to PSRAM with this core's interrupts
 * held, the TE pin's among them, whose edges time every hold. So the band is written back first DMA_SYNC_BYTES at a time, interrupts
 * on between the pieces, and the driver's own finds nothing left to write.
 */
#define DMA_BURST 64                     /* bytes: the most a PSRAM burst takes (GDMA_LL_MAX_BURST_SIZE_PSRAM), as >memtest timed it */
#define DMA_WAIT_MS 100                  /* for a band's copy to end, at most: it takes about 3.6 ms */
#define DMA_KEEP (20 * 1024)             /* internal DMA RAM the copies leave: the Link's TX margin (16 KB) and a VM read's 4 KB */
#define DMA_TAKES (2 * 1024)             /* about what the driver and a copy take of it, installed */
#define DMA_SYNC_BYTES 4096              /* the band's write-back from the cache, a piece at a time */
typedef struct {
    async_memcpy_handle_t mcp; /* the driver, while frame mode is on (NULL otherwise) */
    bool off;                  /* the DMA is given up for the boot: no channel, a copy refused, or one that never ended */
    bool untold;               /* the first band copied by CPU, not logged yet (tell_frame) */
    const char *why;           /* its step: "room", "install", "copy" or "end" (NULL: none yet) */
    esp_err_t err;             /* and that step's error */
} dma_t;
static EXT_RAM_BSS_ATTR dma_t s_dma;   /* the send task's alone */
static volatile bool s_copied;         /* the copy under way has ended: set in the DMA's interrupt (on_copied) */
static void dma_end(void);
static void whole_band(const band_t *b);
static void whole_write(int rows, bool hold);
static void tell_frame(void);

/* A refresh of LVGL's (its bands, the last one marked) as it went to the panel, for the stats: torn if a scan passed
 * between its writes, so that one refresh of the panel showed some of it and the next the rest (frame_meets). In PSRAM:
 * the send task's alone, its interrupts never touch it (and it gives internal RAM back for the flush wait's semaphore). */
#define FRAME_WRITES_MAX 8
static EXT_RAM_BSS_ATTR struct {
    write_t w[FRAME_WRITES_MAX];
    int n;                     /* bands so far; past FRAME_WRITES_MAX only counted, and not judged */
    int64_t fall, p, scan;     /* the scans as te_now gave them at its first band; p 0: no TE then */
    int64_t held_us;           /* waited for the scan, all its bands */
    uint32_t frames, torn;     /* every refresh judged so far, and those torn */
#if CONFIG_MUSE_BENCH
    hold_t hold[FRAME_WRITES_MAX];
    int how, est;              /* the band being sent's */
    bool paired;               /* its first two bands went as a pair */
    bool framed;               /* it went through frame mode, as one write */
    int64_t copy_us;           /* its bands' copies into the frame */
    uint32_t cpu_bands;        /* of them, those copied by CPU, where the DMA couldn't (dma_copy) */
#endif
} s_frame;
#if CONFIG_MUSE_BENCH
/* LVGL's drawing of the refresh under way (on the LVGL task): its start, and the time it has waited for the wire. */
static int64_t s_render_t0, s_render_waited, s_wait_t0;
static EXT_RAM_BSS_ATTR bool s_copy_off;   /* the bench's '>frame copy off': frame mode's refreshes go as bands (set_frame_copy) */
/* The bench's line every FRAME_BATCH refreshes: each figure's sum and most. */
#define FRAME_BATCH 30
static struct {
    uint32_t n, torn, bands, paired, framed;
    uint32_t framed_torn, cpu; /* of the framed, those torn; frame mode's bands copied by CPU */
    int64_t render, render_max, copy, copy_max, held, held_max, wire, wire_max, span, span_max;
} s_batch;
#endif
#endif

/* A piece has gone, or failed to; true if it was the band's last. */
static bool IRAM_ATTR chunk_done(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool last = s_chunks_out > 0 && --s_chunks_out == 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return last;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    bool last = chunk_done();
    if (last) {
        s_band_end = esp_timer_get_time();
    }
    bool yield = last && esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
#if CONFIG_MUSE_APPS
    if (last) {
        xSemaphoreGiveFromISR(s_flush_ready, &woken);   /* the band's flush is ready (the notify): LVGL's flush wait goes on */
    }
#endif
    /* esp_lcd's SPI IO ignores this callback's return value, so the send
     * task, woken here, would wait for the next tick (up to 1 ms a piece,
     * 58 pieces a frame): ask for the switch at the interrupt's exit. */
    if (yield || woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return yield || woken == pdTRUE;
}

/*
 * Waits until no piece is on the wire, holding `held` of the two buffers.
 *
 * Every call into the panel IO acquires the SPI bus lock. If that happens
 * while a queued piece is still going out, the driver hands the lock over
 * from its interrupt when the piece is done; once in tens of thousands of
 * frames that hand-over is lost (ESP-IDF 6.0.1 spi_bus_lock: the device's
 * lock bit set, no acquiring device, the bus idle, the waiter never woken),
 * and the send task, the video and the UI hang for good. Acquiring only
 * with nothing in flight never takes that path. It costs the gap between
 * pieces (the next one is filled while the last is on the wire either way).
 */
static void wire_idle(int held)
{
    for (int i = held; i < 2; i++) {
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
    }
    for (int i = held; i < 2; i++) {
        xSemaphoreGive(s_chunk_free);
    }
}
#if CONFIG_MUSE_APPS

/*
 * How many bands LVGL 9.5.0 draws a refresh in (lv_refr.c: refr_invalid_areas and get_max_row): each of its areas
 * that isn't joined into another, in bands of as many rows as fit a draw buffer of buf_bytes (2 bytes a pixel, a
 * stride of the width), at most the area's height, and an even number of them as the board's rounder makes every
 * height (round_area).
 */
static int refresh_parts(const lv_area_t *areas, const uint8_t *joined, int n, uint32_t buf_bytes)
{
    int parts = 0;
    for (int i = 0; i < n; i++) {
        if (joined[i]) {
            continue;
        }
        int32_t w = areas[i].x2 - areas[i].x1 + 1, h = areas[i].y2 - areas[i].y1 + 1;
        if (w <= 0 || h <= 0) {
            continue;
        }
        int32_t rows = (int32_t)(buf_bytes / ((uint32_t)w * 2));
        rows = (rows < h ? rows : h) & ~1;
        rows = rows > 2 ? rows : 2;
        parts += (h + rows - 1) / rows;
    }
    return parts;
}

/*
 * Whether the band LVGL is flushing, the refresh's `flushed`th (from 0) of `parts`, is a pair's first: held until the band after
 * it is drawn, and sent with it. Only with pairing on (muse_lcd_bands_set_pairing), and only the first band of a refresh of
 * exactly two that isn't the refresh's last (a count that was wrong can't hold a band for one that never comes).
 */
static bool band_pairs(bool pairing, int flushed, int parts, bool last)
{
    return pairing && flushed == 0 && parts == 2 && !last;   /* two: LVGL draws the second in its other buffer */
}

/*
 * Whether LVGL draws the refresh of areas[0..n) (joined[i]: drawn as part of another) as the whole w x h screen: its one area
 * that isn't joined into another is all of it, which LVGL 9.5.0 draws in bands of every row, full width, top to bottom
 * (refr_invalid_areas). A slide's step is that: the tileview, the screen, with whatever else changed joined into it.
 */
static bool refresh_whole(const lv_area_t *areas, const uint8_t *joined, int n, int w, int h)
{
    int whole = 0, other = 0;
    for (int i = 0; i < n; i++) {
        if (joined[i]) {
            continue;
        }
        const lv_area_t *a = &areas[i];
        if (a->x1 <= 0 && a->y1 <= 0 && a->x2 >= w - 1 && a->y2 >= h - 1) {
            whole++;
        } else {
            other++;
        }
    }
    return whole == 1 && other == 0;
}

/*
 * Whether the band LVGL flushes, rows [y1, y2) from x1 to x2 (end exclusive), continues a whole w x h screen whose bands so far
 * ended at row `at`: the full width, from that row, and the screen's bottom if and only if it is the refresh's last.
 */
static bool frame_band(int at, int x1, int y1, int x2, int y2, int w, int h, bool last)
{
    return x1 == 0 && x2 == w && y1 == at && y2 > y1 && (last ? y2 == h : y2 < h);
}
#endif

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
#if !CONFIG_MUSE_APPS
    (void)disp;
#endif
    (void)panel;
    (void)ctx;
#if CONFIG_MUSE_APPS
    band_t b = { data, x1, y1, x2, y2, lv_display_flush_is_last(disp), false, false, s_drawn, 0 };
    int flushed = s_flushed++;
    if (s_framed) {
        /* A band that doesn't continue the screen (never, as LVGL draws one) ends frame mode for the rest of the refresh: the
         * send task writes the rows it has first. */
        b.whole = frame_band(s_whole.at, x1, y1, x2, y2, s_cols, s_rows, b.last);
        s_framed = b.whole;
        s_whole.at = y2;
    }
    b.pair = !b.whole && band_pairs(s_pairing, flushed, s_parts, b.last);   /* frame mode wins if pairing is on too */
#if CONFIG_MUSE_BENCH
    if (b.last) {
        b.render_us = (int32_t)(esp_timer_get_time() - s_render_t0 - s_render_waited);
    }
#endif
#else
    band_t b = { data, x1, y1, x2, y2 };
#endif
    return xQueueSend(s_bands, &b, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}
#if CONFIG_MUSE_APPS

/*
 * On the LVGL task. LV_EVENT_RENDER_START: the refresh's areas are settled, so how many bands it takes, and whether it is
 * the whole screen and goes through frame mode. And
 * LV_EVENT_FLUSH_WAIT_START: LVGL has drawn a band (disp->refreshed_area) and waits to flush it, for the send task
 * holding the band before. The bench's render time is from LV_EVENT_RENDER_START to the last band's flush, less
 * LVGL's waits for the wire (LV_EVENT_FLUSH_WAIT_START to _FINISH).
 */
static void on_render_event(lv_event_t *e)
{
#if CONFIG_MUSE_BENCH
    int64_t now = esp_timer_get_time();
#endif
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        s_parts = s_pairing ? refresh_parts(s_disp->inv_areas, s_disp->inv_area_joined, (int)s_disp->inv_p,
                                            s_disp->buf_act->data_size)
                            : 0;
        s_framed = s_framing && refresh_whole(s_disp->inv_areas, s_disp->inv_area_joined, (int)s_disp->inv_p, s_cols, s_rows);
#if CONFIG_MUSE_BENCH
        s_framed = s_framed && !s_copy_off;
#endif
        s_whole.at = 0;
        s_flushed = 0;
#if CONFIG_MUSE_BENCH
        s_render_t0 = now;
        s_render_waited = 0;
#endif
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (s_pairing) {
            portENTER_CRITICAL(&s_lock);
            s_drawn_area = s_disp->refreshed_area;
            s_drawn++;
            portEXIT_CRITICAL(&s_lock);
            xSemaphoreGive(s_drawn_sem);
        }
#if CONFIG_MUSE_BENCH
        s_wait_t0 = now;
#endif
        break;
#if CONFIG_MUSE_BENCH
    case LV_EVENT_FLUSH_WAIT_FINISH:
        s_render_waited += now - s_wait_t0;
        break;
#endif
    default:
        break;
    }
}

/*
 * LVGL's flush_wait_cb, on the LVGL task: LVGL 9.5.0 calls it while the band before is still being flushed (wait_for_flushing,
 * with disp->flushing set), before it flushes the next. Blocks until that band's lv_display_flush_ready, woken by its give of
 * s_flush_ready (band_back, on_chunk_sent); a give left from a flush LVGL didn't wait for wakes it at once, and the flag is checked again.
 */
static void flush_wait(lv_display_t *disp)
{
    while (disp->flushing) {
        xSemaphoreTake(s_flush_ready, pdMS_TO_TICKS(FLUSH_WAIT_MS));
    }
}
#endif

/*
 * The panel reads its memory top to bottom once a refresh, starting as TE
 * falls and taking the period less the blanking (16.2 of 16.8 ms on the
 * CO5300), while QSPI writes it at its own pace (26 ms for a whole 466 px
 * screen, a few ms for a small area). Where the scan and the writer pass each
 * other inside the area, that refresh shows the new picture on one side of
 * the meeting row and the old one on the other: a tear line. They pass each
 * other in rows [y1, y2) if one reaches y1 first and the other reaches y2
 * first. True if a write of those rows starting at t0 and taking w meets any
 * scan around it, the last scan having started at `fall`.
 */
static bool te_meets(int64_t t0, int64_t w, int y1, int y2, int64_t fall, int64_t p, int64_t scan)
{
    for (int k = -1; k <= 3; k++) {
        int64_t f = fall + k * p;
        int64_t ta = f + scan * y1 / s_rows, tb = f + scan * y2 / s_rows;
        if ((t0 < ta) != (t0 + w < tb)) {
            return true;
        }
    }
    return false;
}
#if CONFIG_MUSE_APPS

/* floor(a / b) for b > 0. */
static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/*
 * A refresh's writes w[0..n) (each a band of rows written at an even pace, as te_meets has them), against the scans
 * around them, the last having started at `fall`: true if a scan passes between them. A scan that reads every
 * written row after its write shows the whole refresh, one that reads every row before shows none of it; one that
 * reads some rows before their write and some after (within one band or across two) shows part of the refresh and
 * part of the one before: a tear line, wherever the two parts meet. Each band's rows are checked at its ends: within
 * one, the write and the scan pass each other at most once.
 */
static bool frame_meets(const write_t *w, int n, int64_t fall, int64_t p, int64_t scan)
{
    if (n <= 0 || p <= 0) {
        return false;
    }
    int64_t t_lo = w[0].t0, t_hi = w[0].t0 + w[0].w;
    for (int i = 1; i < n; i++) {
        t_lo = w[i].t0 < t_lo ? w[i].t0 : t_lo;
        t_hi = w[i].t0 + w[i].w > t_hi ? w[i].t0 + w[i].w : t_hi;
    }
    /* Only a scan under way during the writes can read some rows before them and some after. */
    for (int64_t k = floor_div(t_lo - scan - fall, p); k <= floor_div(t_hi - fall, p) + 1; k++) {
        int64_t f = fall + k * p;
        int before = 0, after = 0;
        for (int i = 0; i < n; i++) {
            int64_t ta = f + scan * w[i].y1 / s_rows, tb = f + scan * w[i].y2 / s_rows;
            /* At each end: is the row written before this scan reads it? */
            before += (w[i].t0 < ta) + (w[i].t0 + w[i].w < tb);
            after += (w[i].t0 >= ta) + (w[i].t0 + w[i].w >= tb);
        }
        if (before && after) {
            return true;
        }
    }
    return false;
}

/*
 * When a refresh's bands w[0..n) (each one's rows, and in .w its write as estimated), sent back to back from about
 * now + the result, stay clear of every scan (frame_meets), the last having started at `fall`: the first such delay
 * in half-millisecond steps over two refreshes, or -1 if none. Clear for every timing they can have: a start up to
 * `jitter` late, each write (100 - spread)% to (100 + spread)% of its estimate and 400 us more, and the gap between
 * two half to twice `gap` and 200 us more. Each band's ends come earliest when everything is quickest and latest when
 * everything is slowest, so a scan that reads all of those on one side of the writes reads every timing's there too:
 * those two timings, judged together, stand for all of them.
 */
static int64_t frame_clear_in(const write_t *w, int n, int64_t gap, int spread, int64_t jitter, int64_t now,
                              int64_t fall, int64_t p, int64_t scan)
{
    write_t t[2 * FRAME_HOLD_MAX];
    if (n <= 0 || n > FRAME_HOLD_MAX || p <= 0) {
        return n <= 0 || p <= 0 ? 0 : -1;
    }
    for (int64_t d = 0; d < 2 * p; d += 500) {
        int64_t early = now + d, late = now + d + jitter;
        for (int i = 0; i < n; i++) {
            int64_t lo = w[i].w * (100 - spread) / 100, hi = w[i].w * (100 + spread) / 100 + 400;
            t[i] = (write_t){ w[i].y1, w[i].y2, early, lo };
            t[n + i] = (write_t){ w[i].y1, w[i].y2, late, hi };
            early += lo + gap / 2;
            late += hi + 2 * gap + 200;
        }
        if (!frame_meets(t, 2 * n, fall, p, scan)) {
            return d;
        }
    }
    return -1;
}
#endif

/* The scan timing, read whole (the interrupt writes each value as two words). */
static bool te_now(int64_t *fall, int64_t *p, int64_t *scan)
{
    int64_t f, q;
    do {
        f = s_te_fall;
        q = s_te_period;
    } while (f != s_te_fall || q != s_te_period);
    *fall = f;
    *p = q;
    *scan = q - s_te_blank;
    return s_te_on && s_rows && q && esp_timer_get_time() - f < 4 * q;   /* else no TE: the panel is asleep */
}

#if CONFIG_MUSE_APPS
static void wake_send(void *arg)
{
    (void)arg;
    xSemaphoreGive(s_wake);
}

/* On the send task: sleeps until esp_timer's time t, to within the timer's latency (a tick could be a millisecond
 * late, and the hold's moment is to half of one). */
static void sleep_until(int64_t t)
{
    int64_t us = t - esp_timer_get_time();
    if (us <= 0) {
        return;
    }
    xSemaphoreTake(s_wake, 0);   /* a wake left from a sleep that gave up */
    if (!s_wake_timer || esp_timer_start_once(s_wake_timer, (uint64_t)us) != ESP_OK) {   /* no timer: in ticks */
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
        return;
    }
    if (xSemaphoreTake(s_wake, pdMS_TO_TICKS(us / 1000) + 2) != pdTRUE) {
        esp_timer_stop(s_wake_timer);
    }
}

/*
 * Before a refresh's writes w[0..n) (rows, and each one's expected time), sent back to back about `gap` apart: waits
 * for the first moment they would all stay clear of every scan, however long each takes within spread percent, and
 * starting up to TE_JITTER_US after it (frame_clear_in). 0 if that is now, 1 after a wait, 2 if no moment over two
 * refreshes is clear (they go at once).
 */
static int te_hold(te_stats_t *st, const write_t *w, int n, int64_t gap, int spread)
{
    int64_t fall, p, scan, now = esp_timer_get_time();
    if (!te_now(&fall, &p, &scan)) {
        return 0;
    }
    int64_t d = frame_clear_in(w, n, gap, spread, TE_JITTER_US, now, fall, p, scan);
    if (d <= 0) {
        return d < 0 ? 2 : 0;
    }
    st->held++;
    sleep_until(now + d);
    return 1;
}
#else
/*
 * Before a write of rows [y1, y2) expected to take w, give or take
 * spread percent: waits for the first moment it would stay clear of every
 * scan (now, usually; looked for in half-millisecond steps over two
 * refreshes). A write that meets a scan wherever it starts isn't held.
 */
static void te_hold(te_stats_t *st, int y1, int y2, int64_t w, int spread)
{
    for (int tries = 0; tries < 3; tries++) {
        int64_t fall, p, scan, now = esp_timer_get_time();
        if (!te_now(&fall, &p, &scan)) {
            return;
        }
        /* The write starts a little after this (the first piece is copied
         * first) and may take more or less than w: clear for all of it. */
        int64_t lo = w * (100 - spread) / 100 + 400, hi = w * (100 + spread) / 100 + 400, d = 0;
        while (d < 2 * p && (te_meets(now + d - 300, lo, y1, y2, fall, p, scan) ||
                             te_meets(now + d - 300, (lo + hi) / 2, y1, y2, fall, p, scan) ||
                             te_meets(now + d - 300, hi, y1, y2, fall, p, scan))) {
            d += 500;
        }
        if (d == 0 || d >= 2 * p) {
            return;   /* clear now, or never */
        }
        vTaskDelay(pdMS_TO_TICKS((d + 999) / 1000));   /* then check again */
        st->held += tries == 0;
    }
}
#endif

/* After the write of rows [y1, y2) that started at t0 and took w, the scans
 * then as te_now gave them: did one meet it? */
static void te_done(te_stats_t *st, int y1, int y2, int64_t t0, int64_t w, int64_t fall, int64_t p, int64_t scan)
{
    st->sum_us += w;
    st->torn += te_meets(t0, w, y1, y2, fall, p, scan);
    if (++st->n % 1000 == 1) {
#if CONFIG_MUSE_APPS
        ESP_LOGI(TAG, "tearing effect: refresh %d us (blank %d us); %s %d us avg (%d us + %d us/KB), %u of %u held "
                 "for the scan, %u met it; %u of %u refreshes torn", (int)p, (int)(p - scan), st->what,
                 (int)(st->sum_us / st->n), (int)s_pace.fixed_us, (int)(s_pace.ns_per_kb / 1000), (unsigned)st->held,
                 (unsigned)st->n, (unsigned)st->torn, (unsigned)s_frame.torn, (unsigned)s_frame.frames);
#else
        ESP_LOGI(TAG, "tearing effect: refresh %d us (blank %d us); %s %d us avg (%d us + %d us/KB), %u of %u held "
                 "for the scan, %u met it", (int)p, (int)(p - scan), st->what, (int)(st->sum_us / st->n),
                 (int)s_fixed_us, (int)(s_ns_per_kb / 1000), (unsigned)st->held, (unsigned)st->n, (unsigned)st->torn);
#endif
    }
}

#if CONFIG_MUSE_APPS
/* A band of `bytes` took w (us): the line learned from it. */
static void pace_learn(pace_t *pc, size_t bytes, int64_t w)
{
    float kb = bytes / 1024.0f, us = (float)w, a = pc->mean_kb ? 1 / 16.0f : 1;
    pc->mean_kb += a * (kb - pc->mean_kb);
    pc->mean_us += a * (us - pc->mean_us);
    pc->var_kb += a * ((kb - pc->mean_kb) * (kb - pc->mean_kb) - pc->var_kb);
    pc->cov += a * ((kb - pc->mean_kb) * (us - pc->mean_us) - pc->cov);
    /* A slope needs sizes that differ. Over bands of about one size (a picture spinning, its turned square a few px
     * more or less with the angle) the times' noise, and the draw's load on PSRAM, which follows the angle too, would
     * make it anything (the board's: 6.1 ms + 10 us a KB): the last one stands, and only the offset follows. */
    float slope = pc->var_kb > PACE_VAR_MIN ? pc->cov / pc->var_kb
                  : pc->ns_per_kb ? pc->ns_per_kb / 1000.0f : pc->mean_us / pc->mean_kb;
    slope = slope > 1 ? slope : 1;
    float fixed = pc->mean_us - slope * pc->mean_kb;
    pc->fixed_us = fixed > 0 ? (int64_t)fixed : 0;
    pc->ns_per_kb = (int64_t)(slope * 1000);
}

/* A band's write of `bytes`, at the pace learned so far. */
static int64_t pace_est(const pace_t *pc, size_t bytes)
{
    return pc->fixed_us + (int64_t)bytes * pc->ns_per_kb / 1024 / 1000;
}

/* The band before has gone (its last piece's interrupt noted when): its write into the stats, the wire's pace
 * learned from it, and the refresh's record. */
static void band_gone(void)
{
    if (!s_band_bytes) {
        return;
    }
    int64_t w = s_band_end - s_band_start;
    if (w > 0) {
        if (s_band_p) {
            te_done(&s_te_ui, s_band_y1, s_band_y2, s_band_start, w, s_band_fall, s_band_p, s_band_scan);
        }
        pace_learn(&s_pace, s_band_bytes, w);
        if (s_frame.n < FRAME_WRITES_MAX) {
            s_frame.w[s_frame.n] = (write_t){ s_band_y1, s_band_y2, s_band_start, w };
#if CONFIG_MUSE_BENCH
            s_frame.hold[s_frame.n] = (hold_t){ s_frame.how, s_frame.est };
#endif
        }
        s_frame.n++;
    }
    s_band_bytes = 0;
}


/*
 * Each band LVGL sends has been drawn whole, so its write takes its size at
 * the wire's pace, learned from the bands before (the interrupt notes when
 * each band's last piece went): hold it until it stays clear of the scan, and
 * with it the band after when that one is drawn and waiting (`next`, a pair).
 * A pair's second goes at once, its moment the pair's. Bands of a refresh of
 * more than two go out one at a time as LVGL draws them, so a band boundary
 * can still show for such a refresh: each band is whole, though.
 */
static void te_band(int y1, int y2, size_t bytes, const lv_area_t *next)
{
    band_gone();
    bool follows = s_follows && y1 == s_follow_y1 && y2 == s_follow_y2;
    s_follows = false;
    if (!s_te_on) {
        return;
    }
    if (s_pace.ns_per_kb && !follows) {
        write_t w[FRAME_HOLD_MAX] = { { y1, y2, 0, pace_est(&s_pace, bytes) } };
        int n = 1;
        if (next) {
            size_t next_bytes = (size_t)(next->x2 - next->x1 + 1) * 2 * (size_t)(next->y2 - next->y1 + 1);
            w[1] = (write_t){ next->y1, next->y2 + 1, 0, pace_est(&s_pace, next_bytes) };
            n = 2;
        }
        int64_t t = esp_timer_get_time();
        int how = te_hold(&s_te_ui, w, n, s_gap_us, TE_SPREAD);
        if (how == 2 && n == 2) {
            n = 1;   /* no moment keeps the pair clear: this band, then, on its own */
            how = te_hold(&s_te_ui, w, 1, 0, TE_SPREAD);
        }
        s_follows = n == 2;
        s_follow_y1 = w[1].y1;
        s_follow_y2 = w[1].y2;
        s_frame.held_us += esp_timer_get_time() - t;
#if CONFIG_MUSE_BENCH
        s_frame.how = how;
        s_frame.est = (int)w[0].w;
        s_frame.paired |= n == 2;
#else
        (void)how;
#endif
    }
    s_band_p = te_now(&s_band_fall, &s_band_p, &s_band_scan) ? s_band_p : 0;
    s_band_start = esp_timer_get_time();
    if (follows) {
        /* The gap the pair was timed with, learned: from the first band's last piece to this one's start. An
         * outlier (the LVGL task held up) is cut, so as not to make every pair after it look unclear. */
        int64_t g = s_band_start - s_band_end;
        g = g < 0 ? 0 : g > 2000 ? 2000 : g;
        s_gap_us = (s_gap_us * 7 + (g > s_gap_us ? 2 * g - s_gap_us : g)) / 8;   /* quick to rise */
#if CONFIG_MUSE_BENCH
        s_frame.how = 3;
        s_frame.est = (int)pace_est(&s_pace, bytes);
#endif
    }
    s_band_y1 = y1;
    s_band_y2 = y2;
    s_band_bytes = bytes;
    if (s_frame.n == 0) {
        s_frame.fall = s_band_fall;
        s_frame.p = s_band_p;
        s_frame.scan = s_band_scan;
    }
}

/*
 * On the send task, with a pair's first band b, which LVGL can't take back until
 * it is flushed: waits (PAIR_WAIT_MS at most) for LVGL to have drawn the band
 * after it into its other buffer, and says which. Not with no scan to keep
 * clear of, or no pace yet to time the two by: the band goes alone, at once.
 */
static bool wait_drawn(const band_t *b, lv_area_t *next)
{
    int64_t fall, p, scan;
    if (!s_pace.ns_per_kb || !te_now(&fall, &p, &scan)) {
        return false;
    }
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(PAIR_WAIT_MS);
    while (s_drawn == b->drawn) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0 || xSemaphoreTake(s_drawn_sem, until - now) != pdTRUE) {
            return false;
        }
    }
    portENTER_CRITICAL(&s_lock);
    *next = s_drawn_area;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

/* After a refresh's last band: once it has gone, judge the refresh (frame_meets), for the stats. */
static void frame_done(const band_t *b)
{
    (void)b;
    wire_idle(0);
    band_gone();
    bool judged = s_frame.p && s_frame.n > 0 && s_frame.n <= FRAME_WRITES_MAX;
    bool torn = judged && frame_meets(s_frame.w, s_frame.n, s_frame.fall, s_frame.p, s_frame.scan);
    s_frame.frames += judged;
    s_frame.torn += torn;
#if CONFIG_MUSE_BENCH
    if (judged) {
        int n = s_frame.n;
        int64_t wire = 0, span = s_frame.w[n - 1].t0 + s_frame.w[n - 1].w - s_frame.w[0].t0;
        for (int i = 0; i < n; i++) {
            wire += s_frame.w[i].w;
        }
        if (torn && s_batch.torn == 0) {
            /* The batch's first torn refresh alone (the batch line counts them all): each line spins the console in this task,
             * above LVGL's. Each band (at most 3): its rows, its start after the scan's start (0 to the period), its write and
             * the one expected, and how its hold went. */
            char line[160];
            int at = 0;
            for (int i = 0; i < n && i < 3 && at < (int)sizeof line; i++) {
                const write_t *x = &s_frame.w[i];
                int64_t phase = x->t0 - s_frame.fall;
                phase -= floor_div(phase, s_frame.p) * s_frame.p;
                at += snprintf(line + at, sizeof line - at, " %d-%d +%d %d/%d h%d", x->y1, x->y2, (int)phase, (int)x->w,
                               (int)s_frame.hold[i].est, s_frame.hold[i].how);
            }
            ESP_LOGI(TAG, "tearing effect: torn refresh of %d bands, scan %d/%d us:%s", n, (int)s_frame.scan,
                     (int)s_frame.p, line);
        }
        s_batch.n++;
        s_batch.torn += torn;
        s_batch.bands += n;
        s_batch.paired += s_frame.paired;
        s_batch.framed += s_frame.framed;
        s_batch.framed_torn += torn && s_frame.framed;
        s_batch.cpu += s_frame.cpu_bands;
        s_batch.render += b->render_us;
        s_batch.render_max = b->render_us > s_batch.render_max ? b->render_us : s_batch.render_max;
        s_batch.copy += s_frame.copy_us;
        s_batch.copy_max = s_frame.copy_us > s_batch.copy_max ? s_frame.copy_us : s_batch.copy_max;
        s_batch.held += s_frame.held_us;
        s_batch.held_max = s_frame.held_us > s_batch.held_max ? s_frame.held_us : s_batch.held_max;
        s_batch.wire += wire;
        s_batch.wire_max = wire > s_batch.wire_max ? wire : s_batch.wire_max;
        s_batch.span += span;
        s_batch.span_max = span > s_batch.span_max ? span : s_batch.span_max;
        if (s_batch.n == FRAME_BATCH) {
            ESP_LOGI(TAG, "tearing effect: %u refreshes, %u torn, %u bands, %u paired, %u framed (%u torn, %u bands by CPU); "
                     "us avg/max: render %d/%d, copy %d/%d, held %d/%d, wire %d/%d, span %d/%d; gap %d us, send stack %u B free",
                     (unsigned)s_batch.n, (unsigned)s_batch.torn, (unsigned)s_batch.bands, (unsigned)s_batch.paired,
                     (unsigned)s_batch.framed, (unsigned)s_batch.framed_torn, (unsigned)s_batch.cpu,
                     (int)(s_batch.render / s_batch.n), (int)s_batch.render_max, (int)(s_batch.copy / s_batch.n),
                     (int)s_batch.copy_max, (int)(s_batch.held / s_batch.n),
                     (int)s_batch.held_max, (int)(s_batch.wire / s_batch.n), (int)s_batch.wire_max,
                     (int)(s_batch.span / s_batch.n), (int)s_batch.span_max, (int)s_gap_us,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            memset(&s_batch, 0, sizeof s_batch);
        }
    }
    s_frame.paired = false;
    s_frame.framed = false;
    s_frame.copy_us = 0;
    s_frame.cpu_bands = 0;
#endif
    s_frame.n = 0;
    s_frame.p = 0;
    s_frame.held_us = 0;
}
#else
/*
 * Each band LVGL sends has been drawn whole, so its write takes its size at
 * the wire's pace, learned from the bands before (the interrupt notes when
 * each band's last piece went): hold it until it stays clear of the scan.
 * Bands of one refresh go out one at a time as LVGL draws them, so a band
 * boundary can still show for a refresh: each band is whole, though.
 */
static void te_band(int y1, int y2, size_t bytes)
{
    if (s_band_bytes) {
        int64_t w = s_band_end - s_band_start;
        if (w > 0) {
            if (s_band_p) {
                te_done(&s_te_ui, s_band_y1, s_band_y2, s_band_start, w, s_band_fall, s_band_p, s_band_scan);
            }
            float kb = s_band_bytes / 1024.0f, us = (float)w, a = s_mean_kb ? 1 / 16.0f : 1;
            s_mean_kb += a * (kb - s_mean_kb);
            s_mean_us += a * (us - s_mean_us);
            s_var_kb += a * ((kb - s_mean_kb) * (kb - s_mean_kb) - s_var_kb);
            s_cov += a * ((kb - s_mean_kb) * (us - s_mean_us) - s_cov);
            float slope = s_var_kb > 0.01f ? s_cov / s_var_kb : s_mean_us / s_mean_kb;
            slope = slope > 1 ? slope : 1;
            float fixed = s_mean_us - slope * s_mean_kb;
            s_fixed_us = fixed > 0 ? (int64_t)fixed : 0;
            s_ns_per_kb = (int64_t)(slope * 1000);
        }
        s_band_bytes = 0;
    }
    if (!s_te_on) {
        return;
    }
    if (s_ns_per_kb) {
        te_hold(&s_te_ui, y1, y2, s_fixed_us + (int64_t)bytes * s_ns_per_kb / 1024 / 1000, 50);
    }
    s_band_p = te_now(&s_band_fall, &s_band_p, &s_band_scan) ? s_band_p : 0;
    s_band_start = esp_timer_get_time();
    s_band_y1 = y1;
    s_band_y2 = y2;
    s_band_bytes = bytes;
}
#endif

/*
 * The panel's draw waits for the piece before to finish, so each piece is
 * copied while the one before is on the wire. Even row counts keep every
 * piece's window starting on an even row, which the CO5300 needs.
 */
static void send_bands(void *arg)
{
    (void)arg;
    int k = 0;
    band_t b;
    for (;;) {
        xQueueReceive(s_bands, &b, portMAX_DELAY);
#if CONFIG_MUSE_APPS
        tell_frame();
        if (!b.whole) {
            dma_end();   /* frame mode's copies are over if it is off: their driver goes */
        }
#endif
        if (!b.data) {
            wire_idle(0);
            s_call(s_call_arg);
            xSemaphoreGive(s_call_done);
#if CONFIG_MUSE_APPS
            s_follows = false;   /* a pair's second after this isn't on time */
#endif
            continue;
        }
        size_t row = (size_t)(b.x2 - b.x1) * 2;
#if CONFIG_MUSE_APPS
        if (b.whole) {
            whole_band(&b);
            continue;
        }
        if (s_whole.staged) {
            whole_write(s_whole.staged, false);   /* a refresh that stopped being whole: the rows copied so far go first */
            s_whole.staged = 0;
        }
        lv_area_t next;
        bool pair = b.pair && wait_drawn(&b, &next);
        te_band(b.y1, b.y2, row * (size_t)(b.y2 - b.y1), pair ? &next : NULL);
#else
        te_band(b.y1, b.y2, row * (size_t)(b.y2 - b.y1));
#endif
        int rows = s_chunk_bytes / row & ~1;
        /* LVGL sends no more until this band is done, so nothing is still
         * counting down. */
        s_chunks_out = (b.y2 - b.y1 + rows - 1) / rows;
        for (int y = b.y1; y < b.y2; y += rows) {
            int h = b.y2 - y < rows ? b.y2 - y : rows;
            xSemaphoreTake(s_chunk_free, portMAX_DELAY);
            memcpy(s_chunk[k], b.data + (size_t)(y - b.y1) * row, h * row);
            wire_idle(1);
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, b.x1, y, b.x2, y + h, s_chunk[k]);
            if (err != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
                if (chunk_done()) {
                    lv_display_flush_ready(s_disp);
#if CONFIG_MUSE_APPS
                    xSemaphoreGive(s_flush_ready);
#endif
                }
            }
            k ^= 1;
        }
#if CONFIG_MUSE_APPS
        if (b.last) {
            frame_done(&b);
        }
#endif
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_io = cfg.panel_io;
    s_chunk_bytes = chunk_bytes;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
#if CONFIG_MUSE_APPS
    s_drawn_sem = xSemaphoreCreateBinary();
    s_wake = xSemaphoreCreateBinary();
    s_flush_ready = xSemaphoreCreateBinary();
    /* The hold's timer. Without it each hold sleeps in ticks instead (sleep_until), up to 2 ms late. */
    const esp_timer_create_args_t wake = { .callback = wake_send, .name = "lcd_hold" };
    esp_err_t timer_err = esp_timer_create(&wake, &s_wake_timer);
    if (timer_err != ESP_OK) {
        s_wake_timer = NULL;
        ESP_LOGW(TAG, "the hold's timer: %s; holds sleep in ticks", esp_err_to_name(timer_err));
    }
#endif
    /* 16-byte aligned: video fills them with the S3's SIMD stores (muse_lcd_bands_frame). */
    s_chunk[0] = heap_caps_aligned_alloc(16, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_aligned_alloc(16, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#if CONFIG_MUSE_APPS
    /* The send task's stack: 2560 B left 180 B free on the board with the bench's lines (vprintf is the deepest). */
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_drawn_sem || !s_wake || !s_flush_ready ||
        !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", 3072, NULL, MUSE_UI_PRIORITY + 1, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
#else
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", 2560, NULL, MUSE_UI_PRIORITY + 1, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
#endif
    const esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_chunk_sent };
    esp_lcd_panel_io_register_event_callbacks(cfg.panel_io, &io_cbs, NULL);
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);

    cfg.profile.buffer_height = lines;
    cfg.profile.use_psram = true;
    cfg.profile.require_double_buffer = true;
    s_disp = esp_lv_adapter_register_display(&cfg);
    if (!s_disp) {
        return NULL;
    }
    s_rows = cfg.profile.ver_res;
#if CONFIG_MUSE_APPS
    s_cols = cfg.profile.hor_res;
    s_te_whole.what = "UI frame";
    lv_display_set_flush_wait_cb(s_disp, flush_wait);   /* LVGL blocks for a band's flush rather than spinning (TC-m5) */
#endif
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = { .custom_draw_bitmap = queue_band };
    esp_lv_adapter_set_draw_bitmap_callbacks(s_disp, &draw_cbs, NULL);
#if CONFIG_MUSE_APPS
    lv_display_add_event_cb(s_disp, on_render_event, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, on_render_event, LV_EVENT_FLUSH_WAIT_START, NULL);
#if CONFIG_MUSE_BENCH
    lv_display_add_event_cb(s_disp, on_render_event, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
#endif
#endif
    return s_disp;
}

void muse_lcd_bands_run(void (*fn)(void *arg), void *arg)
{
    if (!s_bands) {
        fn(arg);
        return;
    }
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    s_call = fn;
    s_call_arg = arg;
    const band_t call = { 0 };
    xQueueSend(s_bands, &call, portMAX_DELAY);
    xSemaphoreTake(s_call_done, portMAX_DELAY);
    xSemaphoreGive(s_call_lock);
}

typedef struct {
    int x1, y1, x2, y2, rows;
    muse_lcd_fill_fn fill;
    void *arg;
} frame_job_t;

/* On the send task: each piece is filled straight into a free DMA buffer
 * while the one before is on the wire. */
void muse_lcd_bands_set_gap(int x, int y)
{
    s_gap_x = x;
    s_gap_y = y;
}

/* QSPI framing of the CO5300 family (and most QSPI AMOLED controllers):
 * opcode 0x02 for a command, 0x32 for pixels on four lines. */
#define QSPI_CMD(c) ((0x02 << 24) | ((c) << 8))
#define QSPI_PIXELS(c) ((0x32 << 24) | ((c) << 8))

/*
 * On the send task: one window for the whole frame, then the pieces as
 * Memory Write and Memory Write Continue (2Ch, 3Ch), each filled straight
 * into a free DMA buffer while the one before is on the wire. Setting the
 * window per piece (as esp_lcd_panel_draw_bitmap does) costs two polled
 * commands that wait for the bus each time: about a third of the frame.
 */
static void send_frame(void *arg)
{
    const frame_job_t *j = arg;
    /* Let LVGL's last band finish first, so its count of pieces in flight
     * (chunk_done) never sees ours. */
    xSemaphoreTake(s_chunk_free, portMAX_DELAY);
    xSemaphoreTake(s_chunk_free, portMAX_DELAY);
    xSemaphoreGive(s_chunk_free);
    xSemaphoreGive(s_chunk_free);
    /* A whole frame is slower to write than the panel scans (26 ms on the
     * 1.75C): it stays clear started within a few ms of a scan's start. */
    static int64_t avg;
    if (s_te_on && avg) {
#if CONFIG_MUSE_APPS
        const write_t frame = { j->y1, j->y2, 0, avg };
        te_hold(&s_te_video, &frame, 1, 0, 3);   /* steady: the same size each time */
#else
        te_hold(&s_te_video, j->y1, j->y2, avg, 3);   /* steady: the same size each time */
#endif
    }
    int64_t fall = 0, p = 0, scan = 0;
    bool timed = te_now(&fall, &p, &scan);
    int64_t t_start = esp_timer_get_time();
    size_t row = (size_t)(j->x2 - j->x1) * 2;
    int rows = j->rows;
    if ((size_t)rows * row > s_chunk_bytes) {
        rows = (int)(s_chunk_bytes / row) & ~1;
    }
    int x1 = j->x1 + s_gap_x, x2 = j->x2 - 1 + s_gap_x, y1 = j->y1 + s_gap_y, y2 = j->y2 - 1 + s_gap_y;
    bool ok = esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2A), (uint8_t[]){ x1 >> 8, x1, x2 >> 8, x2 }, 4) == ESP_OK &&
              esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2B), (uint8_t[]){ y1 >> 8, y1, y2 >> 8, y2 }, 4) == ESP_OK;
    int k = 0;
    for (int y = j->y1; ok && y < j->y2; y += rows) {
        int h = j->y2 - y < rows ? j->y2 - y : rows;
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        j->fill(s_chunk[k], (int)row, y - j->y1, h, j->arg);
        wire_idle(1);
        if (esp_lcd_panel_io_tx_color(s_io, QSPI_PIXELS(y == j->y1 ? 0x2C : 0x3C), s_chunk[k], (size_t)h * row) !=
            ESP_OK) {
            xSemaphoreGive(s_chunk_free);
            ok = false;
        }
        k ^= 1;
    }
    /* Both buffers back: the caller's picture is no longer read. */
    xSemaphoreTake(s_chunk_free, portMAX_DELAY);
    xSemaphoreTake(s_chunk_free, portMAX_DELAY);
    xSemaphoreGive(s_chunk_free);
    xSemaphoreGive(s_chunk_free);
    int64_t w = esp_timer_get_time() - t_start;
    avg = avg ? (avg * 7 + (w > avg ? 2 * w - avg : w)) / 8 : w;   /* quick to rise */
    if (timed) {
        te_done(&s_te_video, j->y1, j->y2, t_start, w, fall, p, scan);
    }
}
#if CONFIG_MUSE_APPS

/*
 * What a frame's write of the whole screen is held as taking (te_hold, at `spread` percent): the frames' learned time `us`, but
 * never more than the longest write a hold can still keep clear of the scan (frame_clear_in: a start up to TE_JITTER_US late,
 * the write spread percent and 400 us longer, and a millisecond to spare). A write that long or longer is held to just after a
 * scan's start, its best moment. Before the first is timed (us 0) it is that longest: any write up to it then stays clear.
 */
static int64_t whole_est(int64_t us, int spread, int64_t p, int64_t scan)
{
    int64_t most = (p + scan - TE_JITTER_US - 1400) * 100 / (100 + spread);
    return us > 0 && us < most ? us : most;
}

/*
 * On the send task: rows [0, rows) of the frame as one write, the window once and then the pieces as Memory Write and Memory
 * Write Continue through the two internal buffers (send_frame's framing), recorded as one band of the refresh (s_frame) for
 * frame_done to judge. With hold, at the first moment it stays clear of every scan (te_hold of whole_est at WHOLE_SPREAD).
 */
static void whole_write(int rows, bool hold)
{
    wire_idle(0);   /* nothing of LVGL's bands still in flight: on_chunk_sent's count of a band's pieces is theirs alone */
    band_gone();
    int64_t t = esp_timer_get_time(), fall = 0, p = 0, scan = 0, est = 0;
    int how = 0;
    if (hold && te_now(&fall, &p, &scan)) {
        est = whole_est(s_whole.us, WHOLE_SPREAD, p, scan);
        const write_t w = { 0, rows, 0, est };
        how = te_hold(&s_te_whole, &w, 1, 0, WHOLE_SPREAD);
    }
    s_frame.held_us += esp_timer_get_time() - t;
    bool timed = te_now(&fall, &p, &scan);
    if (s_frame.n == 0) {
        s_frame.fall = fall;
        s_frame.p = timed ? p : 0;
        s_frame.scan = scan;
    }
    size_t row = (size_t)s_cols * 2;
    int step = (int)(s_chunk_bytes / row) & ~1;
    int x1 = s_gap_x, x2 = s_cols - 1 + s_gap_x, y1 = s_gap_y, y2 = rows - 1 + s_gap_y;
    int64_t t0 = esp_timer_get_time();
    bool ok = esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2A), (uint8_t[]){ x1 >> 8, x1, x2 >> 8, x2 }, 4) == ESP_OK &&
              esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(0x2B), (uint8_t[]){ y1 >> 8, y1, y2 >> 8, y2 }, 4) == ESP_OK;
    int k = 0;
    for (int y = 0; ok && y < rows; y += step) {
        int h = rows - y < step ? rows - y : step;
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        memcpy(s_chunk[k], s_whole.px + (size_t)y * row, (size_t)h * row);
        wire_idle(1);
        if (esp_lcd_panel_io_tx_color(s_io, QSPI_PIXELS(y == 0 ? 0x2C : 0x3C), s_chunk[k], (size_t)h * row) != ESP_OK) {
            xSemaphoreGive(s_chunk_free);
            ok = false;
        }
        k ^= 1;
    }
    wire_idle(0);   /* both buffers back: the last piece has gone */
    int64_t w = esp_timer_get_time() - t0;
    if (rows == s_rows) {
        s_whole.us = s_whole.us ? (s_whole.us * 7 + (w > s_whole.us ? 2 * w - s_whole.us : w)) / 8 : w;   /* quick to rise */
    }
    if (timed) {
        te_done(&s_te_whole, 0, rows, t0, w, fall, p, scan);
    }
    if (s_frame.n < FRAME_WRITES_MAX) {
        s_frame.w[s_frame.n] = (write_t){ 0, rows, t0, w };
#if CONFIG_MUSE_BENCH
        s_frame.hold[s_frame.n] = (hold_t){ how, est };
#endif
    }
    s_frame.n++;
#if CONFIG_MUSE_BENCH
    s_frame.framed = true;
#else
    (void)how;
#endif
}

/* The copy's end, in the DMA's interrupt (on MUSE_UI_CORE, where the send task installed the driver): the send task waits for it. */
static bool on_copied(async_memcpy_handle_t mcp, async_memcpy_event_t *event, void *arg)
{
    (void)mcp;
    (void)event;
    (void)arg;
    BaseType_t woken = pdFALSE;
    s_copied = true;
    xSemaphoreGiveFromISR(s_wake, &woken);
    return woken == pdTRUE;
}

/* On the send task: a band goes by CPU for `why` (err); the first such is logged once, at the send loop's top (tell_frame). */
static void dma_fell_back(const char *why, esp_err_t err)
{
    if (!s_dma.why) {
        s_dma.why = why;
        s_dma.err = err;
        s_dma.untold = true;
    }
}

/*
 * On the send task: n bytes of a band at src copied into the frame at dst, the bulk by DMA (the ends by CPU), the task blocked until
 * the copy has ended: true then, false if the DMA can't take it (the caller copies it by CPU). The driver is installed here at a
 * slide's first copy, with room for one copy at a time (LVGL flushes the next band only once this one is back). A copy refused or
 * one that never ends gives the DMA up for the boot, as no channel does (each tried once, so the driver's own error lines come once
 * at most); short of room, the next band tries again.
 */
static bool dma_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t head = (size_t)(-(uintptr_t)dst & (DMA_BURST - 1));   /* to dst's first burst */
    size_t bulk = n > head ? (n - head) & ~(size_t)(DMA_BURST - 1) : 0;
    if (s_dma.off || !bulk) {
        return false;
    }
    /* Room for the driver to go in, and once in (its share then taken), for the copies to go on. */
    if (heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) < DMA_KEEP + (s_dma.mcp ? 0 : DMA_TAKES)) {
        dma_fell_back("room", ESP_ERR_NO_MEM);
        return false;
    }
    esp_err_t err;
    if (!s_dma.mcp) {
        async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
        config.backlog = 1;
        config.dma_burst_size = DMA_BURST;
        err = esp_async_memcpy_install(&config, &s_dma.mcp);
        if (err != ESP_OK) {
            s_dma.mcp = NULL;
            s_dma.off = true;
            dma_fell_back("install", err);
            return false;
        }
    }
    for (size_t at = 0; at < bulk; at += DMA_SYNC_BYTES) {
        esp_cache_msync((void *)(src + head + at), bulk - at < DMA_SYNC_BYTES ? bulk - at : DMA_SYNC_BYTES,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    s_copied = false;
    err = esp_async_memcpy(s_dma.mcp, dst + head, (void *)(src + head), bulk, on_copied, NULL);
    if (err != ESP_OK) {
        s_dma.off = true;   /* its driver goes at dma_end */
        dma_fell_back("copy", err);
        return false;
    }
    memcpy(dst, src, head);   /* the ends, meanwhile: under a burst each, on cache lines the DMA's bulk doesn't touch */
    memcpy(dst + head + bulk, src + head + bulk, n - head - bulk);
    /* s_wake may hold a give from a hold's timer that fired as its sleep gave up (sleep_until): the flag says. */
    for (TickType_t start = xTaskGetTickCount(); !s_copied;) {
        TickType_t waited = xTaskGetTickCount() - start;
        if (waited >= pdMS_TO_TICKS(DMA_WAIT_MS) || xSemaphoreTake(s_wake, pdMS_TO_TICKS(DMA_WAIT_MS) - waited) != pdTRUE) {
            break;
        }
    }
    if (!s_copied) {
        /* The copy never ended. Its driver can't go with it under way: left as it is, never used again. */
        s_dma.mcp = NULL;
        s_dma.off = true;
        dma_fell_back("end", ESP_ERR_TIMEOUT);
        return false;
    }
    return true;
}

/* On the send task, between copies: the driver goes once frame mode is off (or the DMA is given up), its internal RAM back. One that
 * won't go yet is tried again at the next item. */
static void dma_end(void)
{
    if (s_dma.mcp && (!s_framing || s_dma.off) && esp_async_memcpy_uninstall(s_dma.mcp) == ESP_OK) {
        s_dma.mcp = NULL;
    }
}

/* On the send task: the band LVGL flushed is back with it, and LVGL's flush wait is woken (flush_wait). */
static void band_back(void)
{
    lv_display_flush_ready(s_disp);
    xSemaphoreGive(s_flush_ready);
}

/*
 * On the send task, a band of a refresh in frame mode: copied into the frame (by DMA, the CPU LVGL's to draw the next band into its
 * other buffer meanwhile; by CPU where the DMA can't), and given back to LVGL once the copy has ended; the refresh's last sends the
 * frame whole. This task sends one thing at a time, so the next refresh's first copy waits for that write.
 */
static void whole_band(const band_t *b)
{
    size_t row = (size_t)s_cols * 2;   /* the band's: the whole width (frame_band) */
    uint8_t *dst = s_whole.px + (size_t)b->y1 * row;
    size_t n = (size_t)(b->y2 - b->y1) * row;
#if CONFIG_MUSE_BENCH
    int64_t t = esp_timer_get_time();
#endif
    if (!dma_copy(dst, b->data, n)) {
        memcpy(dst, b->data, n);
#if CONFIG_MUSE_BENCH
        s_frame.cpu_bands++;
#endif
    }
#if CONFIG_MUSE_BENCH
    s_frame.copy_us += esp_timer_get_time() - t;
#endif
    s_whole.staged = b->y2;
    band_back();
    if (b->last) {
        whole_write(s_rows, true);
        s_whole.staged = 0;
        frame_done(b);
        dma_end();
    }
}
#endif

static void IRAM_ATTR on_te(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    if (gpio_get_level(s_te_gpio)) {
        if (s_te_rise) {
            s_te_period = t - s_te_rise;
        }
        s_te_rise = t;
    } else {
        s_te_blank = t - s_te_rise;
        s_te_fall = t;
    }
}

void muse_lcd_bands_set_te(int gpio)
{
    s_te_gpio = gpio;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        err = err == ESP_ERR_INVALID_STATE ? ESP_OK : err;   /* someone else installed it */
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(gpio, on_te, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tearing effect on GPIO%d: %s", gpio, esp_err_to_name(err));
        return;
    }
    s_te_on = true;
}
#if CONFIG_MUSE_APPS

void muse_lcd_bands_set_pairing(bool on)
{
    s_pairing = on;
}

/*
 * As frame mode registers (the 1.75C's display_start, after muse_lcd_bands_register), once: the frame, from PSRAM alone, and only
 * while it leaves the rest of the gadget its reserve; kept. Tried once: with none, frame mode stays off for the boot. Nothing is
 * logged here: what became of the frame is noted, and the send task logs it at its next band (tell_frame), as before.
 */
bool muse_lcd_bands_make_frame(void)
{
    size_t bytes = (size_t)s_cols * (size_t)s_rows * 2;
    if (!s_whole.px && !s_whole.failed && bytes) {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + WHOLE_RESERVE) {
            s_whole.px = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        s_whole.failed = !s_whole.px;
        s_whole.made_untold = !s_whole.failed;
        s_whole.failed_untold = s_whole.failed;
    }
    return s_whole.px != NULL;
}

/*
 * On the LVGL task, from the home tileview's LV_EVENT_SCROLL_BEGIN (muse_ui.c's slide_begins), deep in LVGL's input read: it never
 * allocates or logs here, where vprintf's frame would sit on the LVGL task's thin stack. On only with the frame made at register.
 */
void muse_lcd_bands_set_frame_mode(bool on)
{
    s_framing = on && s_whole.px;
}

#if CONFIG_MUSE_BENCH
void muse_lcd_bands_set_frame_copy(bool on)
{
    s_copy_off = !on;
}
#endif

/* On the send task, at the top of its loop (its shallowest frame): what muse_lcd_bands_make_frame noted of the frame, and the first
 * band frame mode copied by CPU (dma_fell_back), each once. Each flag is set once and cleared here only after it was seen set, so no
 * line is lost. */
static void tell_frame(void)
{
    if (s_whole.made_untold) {
        s_whole.made_untold = false;
        ESP_LOGI(TAG, "tearing effect: frame mode's frame, %u B of PSRAM", (unsigned)((size_t)s_cols * (size_t)s_rows * 2));
    }
    if (s_whole.failed_untold) {
        s_whole.failed_untold = false;
        ESP_LOGW(TAG, "tearing effect: no frame for frame mode (%u B of PSRAM, %u B kept free): each band goes on its own",
                 (unsigned)((size_t)s_cols * (size_t)s_rows * 2), (unsigned)WHOLE_RESERVE);
    }
    if (s_dma.untold) {
        s_dma.untold = false;
        ESP_LOGW(TAG, "tearing effect: frame mode's copy by CPU (DMA %s: %s)", s_dma.why, esp_err_to_name(s_dma.err));
    }
}
#endif

void muse_lcd_bands_frame(int x1, int y1, int x2, int y2, int rows, muse_lcd_fill_fn fill, void *arg)
{
    frame_job_t j = { x1, y1, x2, y2, rows, fill, arg };
    muse_lcd_bands_run(send_frame, &j);
}
