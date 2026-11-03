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

#pragma once

#include "sdkconfig.h"

#if CONFIG_MUSE_BENCH
#include <stdbool.h>

/*
 * The bench's memory probes (perf plan Task 2), in a bench build only.
 *
 * The stack paint names the deepest of a few paths on the LVGL task, which FreeRTOS's high-water mark can't: before a path,
 * muse_stack_paint fills the calling task's stack with MUSE_STACK_FILL from just above the lowest byte ever used up to
 * MUSE_STACK_MARGIN bytes under the stack pointer (the margin is for the calls it makes, and an interrupt's frame), and after it,
 * muse_stack_note scans for the first byte the path changed. The deepest path so far is kept, with the bytes it left free: the since-boot record of
 * the painted paths alone (the apply summary's stack_peak); once there is one, a paint goes no higher than it, since only a deeper
 * path is news. The paint begins after the first byte FreeRTOS's own fill (the same byte) didn't reach, which it leaves as it was,
 * so its high-water mark (>mem's lvgl_stack_free) stays the least free since boot, even while a paint is open (rulings P6, P8). An
 * interrupt's frame that lands during a path counts toward it, as it does toward the high-water mark: two paths within one such
 * frame of each other may swap places. One paint at a time: a path painted inside another counts toward the outer one. Call the
 * note where the paint was called, at the same depth, on the same task. Neither logs; neither may run under the UI lock (the paint
 * is a memset).
 */
#define MUSE_STACK_MARGIN 256
#define MUSE_STACK_FILL 0xA5   /* FreeRTOS's tskSTACK_FILL_BYTE */

/* False when a paint is open, or the stack pointer is within the margin of the stack's start. */
bool muse_stack_paint(void);

/* The paint open since muse_stack_paint is scanned and closed; path (a string that lives for ever) is kept when it went deeper than
 * the deepest so far. Nothing when no paint is open. */
void muse_stack_note(const char *path);

/* The deepest path so far and, in free_bytes, what it left of its stack; NULL and -1 when there is none yet. */
const char *muse_stack_peak(int *free_bytes);

/* The unused stack of LVGL's two drawing tasks (its SW draw unit's threads, "swdraw"), in bytes: their high-water marks, each -1
 * when there is no such thread. For >mem. */
void muse_bench_draw_stacks(int free_bytes[2]);

/*
 * >memtest: a CPU memcpy of MUSE_MEMTEST_BYTES (a band of the slide, LVGL's draw buffer: 466 x 118 x 2) from one PSRAM block to
 * another, then an esp_async_memcpy (GDMA) of MUSE_MEMTEST_DMA_BYTES (105 KiB, whole 64-byte bursts) between the same blocks,
 * each timed, and one line:
 *
 *     memtest: memcpy psram 109976 B <us> us; async_memcpy 107520 B <us> us
 *
 * with "async_memcpy skipped (<why>)" when the DMA copy can't be set up, fails, doesn't end within a second (its blocks and its
 * driver are then left, since the DMA may still write them) or doesn't match. Nothing is copied unless the PSRAM has room for both
 * blocks and MUSE_MEMTEST_RESERVE besides. The driver takes some internal RAM while it is installed (its context and descriptors).
 * Blocks the calling task for the copies, and up to a second for the DMA's end.
 */
#define MUSE_MEMTEST_BYTES 109976
#define MUSE_MEMTEST_DMA_BYTES 107520
#define MUSE_MEMTEST_ALIGN 64            /* the blocks' alignment and the DMA's burst: PSRAM DMA wants both */
#define MUSE_MEMTEST_RESERVE (1024 * 1024)
void muse_bench_memtest(void);
#endif
