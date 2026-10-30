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

/*
 * Link's app.control snapshot (Task 25): a JPEG of the gadget's screen, written to Muse's VM. muse_apps.h has the command's
 * contract (muse_apps_snapshot); this is how its three tasks meet.
 *
 * 1. The caller (the Noise session's task) checks the parameters and the admission (APPS_SNAPSHOT_PSRAM_MIN of PSRAM free, one
 *    snapshot at a time) and queues one job on the I/O worker (apps_io.h). It never waits.
 * 2. The job checks that the app is installed (the store is the worker's: X11), wakes the screen if it is dark (E9:
 *    muse_input_wake("snapshot")), and asks the LVGL task for the screen: it leaves a request with a deadline,
 *    MUSE_APPS_SNAPSHOT_SCREEN_MS from then, and polls for the answer, running the app task's ops as it waits (apps_io_assist).
 * 3. The LVGL task, every frame, in muse_apps_ui_supervise (apps_snapshot_take_pending): on the first frame the screen isn't dark
 *    and the deadline hasn't come, it draws the active screen, whatever is on it (D22), into a block of PSRAM of its own
 *    (lv_snapshot_take_to_draw_buf, RGB565) and hands the block to the job. A request it finds after its deadline is dropped,
 *    never taken: the job's own look at the deadline answers it, unavailable, MUSE_APPS_SNAPSHOT_NO_WAKE. The time the drawing
 *    took is the app task's core's, not the app's: apps_rt_steal_ms keeps it off the running app's duty cycle and 1 s backstop.
 * 4. The job masks the pixels to the round screen (apps_snapshot_mask) into a 16-byte aligned block (jpeg_calloc_align), frees
 *    the screen's block, encodes the JPEG (esp_new_jpeg: RGB565 little-endian, 4:2:0, quality APPS_SNAPSHOT_QUALITY) into
 *    APPS_SNAPSHOT_JPEG_MAX bytes of PSRAM, builds the result (the app's widgets as the app task's record has them, and the
 *    overflow warnings worked out again from them), and writes the JPEG to workspace/apps/<id>/snapshot.jpg through the VM ops
 *    (vm_file_write: POST /fs/upload). A write that fails fails the command: without the file Muse sees nothing.
 * The job calls done once, on every way, after it has let go of the admission (so a snapshot asked once the result is read
 * isn't busy).
 *
 * The handshake (one atomic word, in internal RAM): IDLE, REQUESTED (the job asked), TAKING (the LVGL task is drawing), TAKEN
 * or FAILED (it has answered: the job reads the answer and sets IDLE). At its deadline the job takes a REQUESTED back (IDLE) or,
 * for a drawing that runs APPS_SNAPSHOT_RENDER_MS past it, gives it up (ABANDONED): the LVGL task, which finds it so when it
 * is done, frees its block and sets IDLE. A job that finds the word not IDLE (an abandoned drawing still under way) answers busy.
 *
 * The encoder's sizes. A size the encoder refuses is padded with black to the next multiple of 16 (480 for 466, 240 for 233:
 * D22); the result's w and h stay the screen's (or half of it), and a warning says the file is larger.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_SNAPSHOT_PSRAM_MIN (1536 * 1024)  /* free PSRAM a snapshot is admitted with: the screen, the masked copy, the JPEG */
#define APPS_SNAPSHOT_JPEG_MAX (128 * 1024)    /* the encoder's output, PSRAM */
#define APPS_SNAPSHOT_QUALITY 80
#define APPS_SNAPSHOT_MIN_LEFT_MS 8000         /* a job that starts with less of its deadline left does nothing: busy */
#define APPS_SNAPSHOT_RENDER_MS 2000           /* a drawing begun before the screen's deadline is waited for this much longer */
#define APPS_SNAPSHOT_POLL_MS 10               /* how often the job looks for the LVGL task's answer */
#define APPS_SNAPSHOT_WARNINGS_MAX 1536        /* bytes of overflow warnings a result carries; the rest are counted */

/*
 * The LVGL task, every frame, the dark ones included (muse_apps_ui_supervise): takes the screen when the job has asked, the screen
 * isn't dark and the request's deadline hasn't come; nothing otherwise. It never waits for another task.
 */
void apps_snapshot_take_pending(void);

/*
 * src, w x h RGB565 pixels with rows of stride_px, masked to the round screen and scaled down by scale (1, or 2 for a pixel that is
 * the mean of a 2 x 2 block), into dst, whose rows are dst_stride_px pixels: (w / scale) x (h / scale) of them, and the rest of dst
 * is left as it is (the encoder's padding, black). A pixel is kept when its centre is inside the circle the screen's square
 * holds (diameter min(w, h), about the square's centre), and is 0 (black) otherwise: on the 466 x 466 screen, (0, 0) is black
 * and (465, 233), 232 px from the centre column, is kept. Any task; it allocates nothing.
 */
void apps_snapshot_mask(const uint16_t *src, int w, int h, int stride_px, int scale, uint16_t *dst, int dst_stride_px);

/*
 * esp_new_jpeg's internal-first allocators (jpeg_calloc_inner and jpeg_calloc_align_inner, its working buffers), which the linker
 * sends here in a build with the apps (components/muse/CMakeLists.txt): zeroed PSRAM, never internal RAM, which is the Link's DMA
 * heap. NULL when PSRAM is out (the encoder's JPEG_ERR_NO_MEM). Any task.
 */
void *__wrap_jpeg_calloc_inner(size_t size);
void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned);
