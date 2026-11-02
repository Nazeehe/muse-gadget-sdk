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
#include <stdint.h>

/*
 * The bench's frame summaries: one line for a motion's frames instead of one a frame, since every line the console prints
 * spins the task that prints it until the UART has sent it, and a line a frame stalled the screen it measured.
 *
 *     <name> frames: n=<n> p50=<ms> p95=<ms> max=<ms> over50=<k>
 *
 * (the muse_frames tag), p50 and p95 nearest-rank, over50 the frames over 50 ms. A summary is its caller's, on one task: the
 * callers keep theirs in PSRAM (EXT_RAM_BSS_ATTR, so zeroed at boot) and name it before the first frame. Both calls may print,
 * so call them only where a log line may go: LV_EVENT_REFR_READY or frame_tick, never a scroll, input or flush callback.
 */
typedef struct {
    const char *name;   /* in the line: "scroll", "page", "launcher" or "spin" */
    uint16_t ms[128];   /* the frames held, each to the nearest ms */
    uint16_t n;         /* how many */
    int64_t first_us;   /* esp_timer_get_time() at the first of them */
} muse_frame_stats_t;

/* A frame of ms (clamped to 0..65535) at now_us. Prints the summary, and starts the next, when this frame fills it or comes a
 * second or more after its first. */
void muse_frame_stats_add(muse_frame_stats_t *s, int ms, int64_t now_us);

/* Prints the summary of the frames held and lets them go; nothing when it holds none. For the motion's end. */
void muse_frame_stats_flush(muse_frame_stats_t *s);
#endif
