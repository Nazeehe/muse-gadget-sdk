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

#include "muse_frame_stats.h"

#if CONFIG_MUSE_BENCH
#include <limits.h>
#include <stddef.h>

#include "esp_log.h"

static const char *TAG = "muse_frames";

#define MUSE_FRAME_STATS_WINDOW_US 1000000   /* a summary covers at most a second of motion */
#define MUSE_FRAME_STATS_OVER_MS 50          /* over50: the frame gate's longest frame */
#define MUSE_FRAME_STATS_BINS 128            /* the counting sort's: a bin a ms below it, the frames at or above it ranked apart */

void muse_frame_stats_add(muse_frame_stats_t *s, int ms, int64_t now_us)
{
    if (s->n == 0) {
        s->first_us = now_us;
    }
    s->ms[s->n++] = (uint16_t)(ms < 0 ? 0 : ms > UINT16_MAX ? UINT16_MAX : ms);
    if ((size_t)s->n == sizeof s->ms / sizeof s->ms[0] || now_us - s->first_us >= MUSE_FRAME_STATS_WINDOW_US) {
        muse_frame_stats_flush(s);
    }
}

/*
 * The rank-th smallest of the frames held (1-based), from count[], the frames of each ms below MUSE_FRAME_STATS_BINS. A rank past
 * those falls among the longer frames, which are rare (a stall): it is the least of them with at least rank frames at or under it.
 */
static int nth(const muse_frame_stats_t *s, const uint8_t *count, int rank)
{
    int seen = 0;
    for (int v = 0; v < MUSE_FRAME_STATS_BINS; v++) {
        seen += count[v];
        if (seen >= rank) {
            return v;
        }
    }
    int best = INT_MAX;
    for (int i = 0; i < s->n; i++) {
        int x = s->ms[i];
        if (x >= MUSE_FRAME_STATS_BINS && x < best) {
            int under = 0;
            for (int j = 0; j < s->n; j++) {
                under += s->ms[j] <= x;
            }
            if (under >= rank) {
                best = x;
            }
        }
    }
    return best;
}

void muse_frame_stats_flush(muse_frame_stats_t *s)
{
    if (s->n == 0) {
        return;
    }
    uint8_t count[MUSE_FRAME_STATS_BINS] = { 0 };   /* at most 128 frames: a byte a bin holds them, on the caller's stack */
    int max = 0, over = 0;
    for (int i = 0; i < s->n; i++) {
        int ms = s->ms[i];
        if (ms < MUSE_FRAME_STATS_BINS) {
            count[ms]++;
        }
        max = ms > max ? ms : max;
        over += ms > MUSE_FRAME_STATS_OVER_MS;
    }
    /* Nearest rank: the ceil(p * n / 100)-th smallest. */
    int n = s->n;
    ESP_LOGI(TAG, "%s frames: n=%d p50=%d p95=%d max=%d over50=%d", s->name, n, nth(s, count, (n + 1) / 2),
             nth(s, count, (95 * n + 99) / 100), max, over);
    s->n = 0;
    s->first_us = 0;
}
#endif
