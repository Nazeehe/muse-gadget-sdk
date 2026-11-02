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

#include "muse_bench_mem.h"

#if CONFIG_MUSE_BENCH
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#if CONFIG_SOC_ASYNC_MEMCPY_SUPPORTED && __has_include("esp_async_memcpy.h")
#include "esp_async_memcpy.h"
#define MEMTEST_DMA 1
#endif
#if !CONFIG_MUSE_UI_FREEINK
#include "lvgl.h"
#include "src/core/lv_global.h"
#include "src/draw/sw/lv_draw_sw_private.h"
#endif

#define FILL_WORD (0x01010101u * MUSE_STACK_FILL)
#define MEMTEST_WAIT_MS 1000

/* ---- The stack paint ---- */

/* The paint that is open, and the deepest path so far. The LVGL task's alone (every painted path runs on it). */
static EXT_RAM_BSS_ATTR struct {
    bool open;
    uint8_t *start;   /* the stack's lowest byte */
    uint8_t *mark;    /* where the paint began: the first byte that was not the fill */
    uint8_t *top;     /* where it ended */
    const char *path; /* the deepest path so far; NULL: none */
    int free_bytes;   /* what it left */
} s_paint;

/* The first byte in [p, end) that isn't the fill, or end: a word at a time once p is aligned. */
static uint8_t *first_used(uint8_t *p, uint8_t *end)
{
    while (p < end && ((uintptr_t)p & 3) != 0 && *p == MUSE_STACK_FILL) {
        p++;
    }
    while (end - p >= 4) {
        uint32_t word;
        memcpy(&word, p, sizeof word);
        if (word != FILL_WORD) {
            break;
        }
        p += 4;
    }
    while (p < end && *p == MUSE_STACK_FILL) {
        p++;
    }
    return p;
}

bool muse_stack_paint(void)
{
    if (s_paint.open) {
        return false;
    }
    uint8_t *start = (uint8_t *)xTaskGetStackStart(NULL);
    uint8_t *sp = (uint8_t *)esp_cpu_get_sp();
    if (start == NULL || sp - start <= MUSE_STACK_MARGIN) {
        return false;
    }
    uint8_t *top = sp - MUSE_STACK_MARGIN;
    if (s_paint.path != NULL && start + s_paint.free_bytes < top) {
        top = start + s_paint.free_bytes;   /* only a path deeper than the deepest is news */
    }
    uint8_t *mark = first_used(start, top);
    memset(mark, MUSE_STACK_FILL, (size_t)(top - mark));
    s_paint.start = start;
    s_paint.mark = mark;
    s_paint.top = top;
    s_paint.open = true;
    return true;
}

void muse_stack_note(const char *path)
{
    if (!s_paint.open) {
        return;
    }
    s_paint.open = false;
    uint8_t *used = first_used(s_paint.start, s_paint.top);
    if (used < s_paint.top && (s_paint.path == NULL || used - s_paint.start < s_paint.free_bytes)) {
        s_paint.path = path;
        s_paint.free_bytes = (int)(used - s_paint.start);
    }
    if (s_paint.mark < s_paint.top && used > s_paint.mark) {
        /* The path stayed above where FreeRTOS's fill ended: a byte that isn't the fill goes back there, so its high-water mark
         * doesn't take the paint for stack never used. */
        *s_paint.mark = (uint8_t)~MUSE_STACK_FILL;
    }
}

const char *muse_stack_peak(int *free_bytes)
{
    if (free_bytes != NULL) {
        *free_bytes = s_paint.path != NULL ? s_paint.free_bytes : -1;
    }
    return s_paint.path;
}

/* ---- The drawing tasks' stacks ---- */

void muse_bench_draw_stacks(int free_bytes[2])
{
    free_bytes[0] = free_bytes[1] = -1;
#if !CONFIG_MUSE_UI_FREEINK && LV_USE_DRAW_SW && LV_USE_OS == LV_OS_FREERTOS
    /* LVGL's draw units are made by lv_init and never change: the SW one's threads are the swdraw tasks. */
    for (lv_draw_unit_t *unit = LV_GLOBAL_DEFAULT()->draw_info.unit_head; unit != NULL; unit = unit->next) {
        if (unit->name == NULL || strcmp(unit->name, "SW") != 0) {
            continue;
        }
        const lv_draw_sw_unit_t *sw = (const lv_draw_sw_unit_t *)unit;
        for (int i = 0; i < 2 && i < LV_DRAW_SW_DRAW_UNIT_CNT; i++) {
            TaskHandle_t task = sw->thread_dscs[i].thread.xTaskHandle;
            if (task != NULL) {
                free_bytes[i] = (int)(uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t));
            }
        }
        return;
    }
#endif
}

/* ---- >memtest ---- */

#if MEMTEST_DMA
static EXT_RAM_BSS_ATTR volatile int64_t s_dma_end_us;
static EXT_RAM_BSS_ATTR volatile bool s_dma_done;

/* The copy's end, in the DMA's interrupt: the time, then the flag the task waits on. */
static bool on_copied(async_memcpy_handle_t mcp, async_memcpy_event_t *event, void *arg)
{
    (void)mcp;
    (void)event;
    (void)arg;
    s_dma_end_us = esp_timer_get_time();
    s_dma_done = true;
    return false;
}

/* The DMA copy from src to dst, timed, into why: true when the blocks must be left (the copy never ended). */
static bool dma_copy(uint8_t *src, uint8_t *dst, char *why, size_t cap)
{
    async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
    config.backlog = 1;
    config.dma_burst_size = MUSE_MEMTEST_ALIGN;
    async_memcpy_handle_t mcp = NULL;
    esp_err_t err = esp_async_memcpy_install(&config, &mcp);
    if (err != ESP_OK) {
        snprintf(why, cap, "async_memcpy skipped (install: %s)", esp_err_to_name(err));
        return false;
    }
    /* A new pattern in the source: a copy that writes nothing leaves the CPU's copy of the old one behind. */
    memset(src, 0xC3, MUSE_MEMTEST_DMA_BYTES);
    s_dma_done = false;
    int64_t t0 = esp_timer_get_time();
    err = esp_async_memcpy(mcp, dst, src, MUSE_MEMTEST_DMA_BYTES, on_copied, NULL);
    if (err != ESP_OK) {
        esp_async_memcpy_uninstall(mcp);
        snprintf(why, cap, "async_memcpy skipped (copy: %s)", esp_err_to_name(err));
        return false;
    }
    for (TickType_t waited = 0; !s_dma_done && waited < pdMS_TO_TICKS(MEMTEST_WAIT_MS); waited++) {
        vTaskDelay(1);
    }
    if (!s_dma_done) {
        snprintf(why, cap, "async_memcpy skipped (no end in %d ms)", MEMTEST_WAIT_MS);
        return true;
    }
    int64_t took = s_dma_end_us - t0;
    esp_async_memcpy_uninstall(mcp);
    if (memcmp(dst, src, MUSE_MEMTEST_DMA_BYTES) != 0) {
        snprintf(why, cap, "async_memcpy skipped (the copy differs)");
        return false;
    }
    snprintf(why, cap, "async_memcpy %u B %lld us", (unsigned)MUSE_MEMTEST_DMA_BYTES, (long long)took);
    return false;
}
#endif

void muse_bench_memtest(void)
{
    const size_t n = MUSE_MEMTEST_BYTES;
    const size_t need = 2 * n + MUSE_MEMTEST_RESERVE;
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint8_t *src = NULL, *dst = NULL;
    if (free_now >= need) {
        src = heap_caps_aligned_alloc(MUSE_MEMTEST_ALIGN, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        dst = src != NULL ? heap_caps_aligned_alloc(MUSE_MEMTEST_ALIGN, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    }
    if (dst == NULL) {
        heap_caps_free(src);
        printf("memtest: no room (%u B of PSRAM free, %u needed)\n", (unsigned)free_now, (unsigned)need);
        fflush(stdout);
        return;
    }
    memset(src, 0x5A, n);
    memset(dst, 0, n);
    /* The blocks escape here, so the copy stays between the two clock reads and isn't dropped as a store nothing reads. */
    __asm__ volatile("" : : "r"(src), "r"(dst) : "memory");
    int64_t t0 = esp_timer_get_time();
    memcpy(dst, src, n);
    __asm__ volatile("" : : "r"(dst) : "memory");
    int64_t cpu_us = esp_timer_get_time() - t0;
    char dma[64];
    bool keep = false;
#if MEMTEST_DMA
    keep = dma_copy(src, dst, dma, sizeof dma);
#elif !CONFIG_SOC_ASYNC_MEMCPY_SUPPORTED
    snprintf(dma, sizeof dma, "async_memcpy skipped (none on this chip)");
#else
    snprintf(dma, sizeof dma, "async_memcpy skipped (not in this build)");
#endif
    printf("memtest: memcpy psram %u B %lld us; %s\n", (unsigned)n, (long long)cpu_us, dma);
    fflush(stdout);
    if (!keep) {
        heap_caps_free(dst);
        heap_caps_free(src);
    }
}
#endif
