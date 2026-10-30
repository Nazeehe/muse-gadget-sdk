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
 * 1. The caller (the Noise session's task) checks the parameters and one snapshot at a time, and admits it before anything is drawn
 *    (apps_snapshot_admit, below), then queues one job on the I/O worker (apps_io.h). It never waits.
 * 2. The job checks that the app is installed (the store is the worker's: X11), admits it again (the memory may have gone while it
 *    waited in the queue), wakes the screen (E9: muse_input_wake("snapshot"), which lights a dark screen and keeps a lit one from
 *    going dark before the take), and asks the LVGL task for the screen at its scale: it leaves a request with a deadline,
 *    MUSE_APPS_SNAPSHOT_SCREEN_MS from then, and polls for the answer, running the app task's ops as it waits (apps_io_assist).
 * 3. The LVGL task, every frame, in muse_apps_ui_supervise (apps_snapshot_take_pending): on the first frame the screen isn't dark
 *    and the deadline hasn't come, it draws the active screen, whatever is on it (D22), into one block of its own
 *    (apps_port_snapshot_alloc: PSRAM, through the floor, aligned for the encoder; RGB565, lv_snapshot_take_to_draw_buf), sized for
 *    the whole sequence below, and hands the block to the job, with what the picture is of: the app layer's place and the app that
 *    runs, read as it is drawn. A request it finds after its deadline is dropped, never taken: the job's own look at the deadline
 *    answers it, unavailable, MUSE_APPS_SNAPSHOT_NO_WAKE. The time the drawing took is the app task's core's, not the app's:
 *    apps_rt_steal_ms keeps it off the running app's duty cycle and 1 s backstop.
 * 4. The job opens the encoder (esp_new_jpeg: RGB565 little-endian, 4:2:0, quality APPS_SNAPSHOT_QUALITY), masks the pixels to the
 *    round screen in place (apps_snapshot_mask: at scale 2 the half-size picture takes the block's first quarter), and encodes the
 *    JPEG into the block's room after the picture (APPS_SNAPSHOT_JPEG_MAX bytes); then it moves the JPEG to the block's start and
 *    cuts the block to the JPEG's size (apps_port_snapshot_shrink), so the write holds only that. It builds the result (the app's
 *    widgets as the app task's record has them, and the overflow warnings worked out again from them), and writes the JPEG to
 *    workspace/apps/<id>/snapshot.jpg through the VM ops (vm_file_write: POST /fs/upload). A write that fails fails the command:
 *    without the file Muse sees nothing.
 * The job calls done once, on every way, after it has let go of the admission (so a snapshot asked once the result is read
 * isn't busy).
 *
 * Memory (N1). Every block of a snapshot goes through the apps' floor (apps_port.h: APPS_RAM_RESERVE stays free, and a big block
 * leaves the largest free block at least that): the screen's block, the padded copy below and the encoder's own buffers (the
 * wrapped allocators at the end of apps_snapshot.c). So a snapshot never takes free PSRAM under the reserve. Its peak is one block
 * and the encoder's buffers held at once (apps_snapshot_peak): at scale 1 the screen's pixels and the JPEG's room after them
 * (434,320 + 131,072 bytes), at scale 2 the screen's pixels alone (434,312: the half-size picture and the JPEG fit in them once the
 * picture is masked). It is admitted, before anything is drawn, only when free PSRAM and the largest free block each hold the peak
 * above the reserve (apps_snapshot_admit): the encoder's buffers are counted in the block too, as if they came out of it. While
 * the job encodes, the encoder's buffers count with the block (apps_port_snapshot_adopt: the wrapped allocators and frees), and
 * the encode's log line says their peak against the allowance. An encoder that finds no room even so is out_of_memory, in the
 * admission's words then, as is a block the take finds no room for. While the snapshot holds its blocks, an app task growth the
 * floor refuses waits for them (apps_port.h), so the snapshot doesn't kill the app that runs unless that wait runs out.
 *
 * The handshake (one atomic word, in internal RAM): IDLE, REQUESTED (the job asked), TAKING (the LVGL task is drawing), TAKEN
 * or FAILED (it has answered: the job reads the answer and sets IDLE). At its deadline the job takes a REQUESTED back (IDLE) or,
 * for a drawing that runs APPS_SNAPSHOT_RENDER_MS past it, gives it up (ABANDONED): the LVGL task, which finds it so when it
 * is done, frees its block and sets IDLE. A job that finds the word not IDLE (an abandoned drawing still under way) answers busy.
 *
 * The encoder's sizes. A size the encoder refuses is padded with black to the next multiple of 16 (480 for 466, 240 for 233:
 * D22); the result's w and h stay the screen's (or half of it), and a warning says the file is larger. A padded picture can't be
 * masked in place: it goes into a zeroed copy of its own (apps_port_snapshot_alloc, so through the floor), and the JPEG into the
 * screen's block, whose pixels are done with by then. The 1.75C's encoder takes 466 and 233 (Task 25), so its snapshots never pad.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_SNAPSHOT_JPEG_MAX (128 * 1024)    /* the encoder's output, PSRAM */
#define APPS_SNAPSHOT_SIDE 466                 /* the 1.75C's screen, as the admission counts it (the drawing takes the screen's size) */
/* What the admission allows for the encoder's own buffers (apps_snapshot_encoder_bytes): their peak, from the encode's log line on the
 * 1.75C, is 55,208 bytes at scale 1 and 32,168 at scale 2 (its state grows with the picture's width), and the allowances keep about a
 * fifth more than that. */
#define APPS_SNAPSHOT_ENCODER_BYTES_1 (64 * 1024)
#define APPS_SNAPSHOT_ENCODER_BYTES_2 (40 * 1024)
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
/* dst may be src (the mask in place, as the job does it) when dst_stride_px is at most stride_px: an output pixel is written at or
 * before the first input pixel it is made of, after it has been read, and no later one is read from there. The rest of the block
 * then still holds what was drawn: only the (w / scale) x (h / scale) picture is the mask's. */

/*
 * The admission (N1, above). apps_snapshot_block is the block a snapshot at scale takes of a side x side screen (the drawing's block:
 * the pixels in rows of LVGL's RGB565 stride, as the take lays them out, and at scale 1 the JPEG's room after the picture, 16-byte
 * aligned); apps_snapshot_encoder_bytes is the allowance for the encoder's buffers at scale (APPS_SNAPSHOT_ENCODER_BYTES_1 or _2),
 * and apps_snapshot_peak is the two together.
 * apps_snapshot_admit: NULL when free_bytes and largest (the largest free block) each hold the peak at scale above APPS_RAM_RESERVE
 * for the 1.75C's screen; else the refusal, out_of_memory's words: MUSE_APPS_SNAPSHOT_TRY_SCALE_2 for scale 1 when scale 2 fits,
 * otherwise MUSE_APPS_SNAPSHOT_STOP_APP when an app runs (app_runs) and MUSE_APPS_SNAPSHOT_NO_MEMORY when none does. Any task.
 */
size_t apps_snapshot_block(int side, int scale);
size_t apps_snapshot_encoder_bytes(int scale);
size_t apps_snapshot_peak(int scale);
const char *apps_snapshot_admit(int scale, size_t free_bytes, size_t largest, bool app_runs);

/*
 * esp_new_jpeg's allocators (jpeg_calloc_inner and jpeg_calloc_align_inner, internal RAM first in the library, and jpeg_calloc and
 * jpeg_calloc_align: its working buffers), which the linker sends here in a build with the apps (components/muse/CMakeLists.txt):
 * zeroed PSRAM, never internal RAM, which is the Link's DMA heap, and through the apps' floor (apps_port_room). NULL when the floor
 * refuses or PSRAM is out (the encoder's JPEG_ERR_NO_MEM). Its frees (jpeg_free, jpeg_free_align) come here too: heap_caps_free.
 * While the I/O worker encodes a snapshot, that task's blocks are counted with the snapshot's (apps_port_snapshot_adopt, _disown)
 * from allocation to free, and their peak is in the encode's log line. Any task.
 */
void *__wrap_jpeg_calloc_inner(size_t size);
void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned);
void *__wrap_jpeg_calloc(size_t size);
void *__wrap_jpeg_calloc_align(size_t size, int aligned);
void __wrap_jpeg_free(void *p);
void __wrap_jpeg_free_align(void *p);
