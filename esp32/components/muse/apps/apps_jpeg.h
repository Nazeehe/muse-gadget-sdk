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
 * The apps' own JPEG decoder (connected apps Task 2): a baseline JPEG in memory, decoded by the ESP32-S3 ROM's TJpgDec (rom/tjpgd.h)
 * into an LVGL image block, the runtime image apps_rt_image_add takes. Apps only: main/image_fetch.c keeps its own decode, which
 * draws to the panel as it goes.
 *
 * The scale. TJpgDec decodes at 1/1, 1/2, 1/4 or 1/8, and the decode takes the largest of those (the least reduction) at which both
 * sides fit max_side: an 800 px picture is 400 px for a max_side of 466. It never scales up: a picture that fits is decoded at 1/1, at
 * its own size. A side at a scale is the picture's side shifted right (TJpgDec's own rounding: 933 px is 466 at 1/2).
 *
 * The block. An LVGL v9 image file in memory: the 12-byte header (magic 0x19, the colour format, no flags, w, h, the stride, 0),
 * then the pixels, which apps_image_check passes:
 * - circle false: RGB565, w x h pixels of 2 bytes, as LVGL lays them in memory on the board (little-endian, red in the top 5 bits),
 *   from TJpgDec's RGB888.
 * - circle true: RGB565A8, the same colour plane, then a plane of alpha, a byte a pixel: 255 inside the circle inscribed in the
 *   picture (centred, of diameter min(w, h)) and 0 outside. The method: a pixel is opaque when its centre lies strictly inside the
 *   circle (a centre on it is outside), worked out in integers. It gives a record-like disc to spin.
 * The block comes from apps_port_realloc, through the apps' floor, as does TJpgDec's pool (APPS_JPEG_POOL_BYTES, freed before this
 * returns). A refusal of either is a clean error, never a crash.
 *
 * Tasks. It runs on whichever task calls it, with no state of its own outside the call (two tasks may decode at once). In Task 3
 * that is the network worker, never the LVGL task or the app task: a 466 px decode takes hundreds of ms. The stack it takes is the
 * session (about 130 bytes) and TJpgDec's own frames; the pool is in the heap.
 *
 * Errors (exact, and the guide's):
 *   unsupported JPEG: use baseline, not progressive    TJpgDec's JDR_FMT3: progressive, or another JPEG process it doesn't do
 *   not a valid JPEG                                   anything else TJpgDec refuses: not a JPEG, cut short, damaged, a sampling it
 *                                                      doesn't take, tables too big for its pool; and no bytes at all
 *   the picture is larger than <n> px even at 1/8      a side over max_side at 1/8; <n> is max_side
 *   the picture is too thin to scale down to <n> px    the scale that fits max_side leaves a side of 0 px (1000 x 3 at 1/4)
 *   the gadget is short of memory                      the floor or the heap refused the pool or the block
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TJpgDec's working pool: what main/image_fetch.c gives the same ROM decoder. */
#define APPS_JPEG_POOL_BYTES 3100

#define APPS_JPEG_MSG_PROGRESSIVE "unsupported JPEG: use baseline, not progressive"
#define APPS_JPEG_MSG_INVALID "not a valid JPEG"
#define APPS_JPEG_MSG_TOO_LARGE "the picture is larger than %d px even at 1/8"
#define APPS_JPEG_MSG_TOO_THIN "the picture is too thin to scale down to %d px"
#define APPS_JPEG_MSG_NO_MEMORY "the gadget is short of memory"

/*
 * Decodes jpg[0, len) (above). max_side is the longest side wanted, 1 to APPS_RT_IMAGE_SIDE_MAX (apps_rt.h); any other value is
 * APPS_RT_IMAGE_SIDE_MAX. True with the block in *out (the caller's: apps_port_free, or give it to apps_rt_image_add), its bytes in
 * *out_len and the picture's size in *w and *h (either may be NULL). False with *out NULL, *out_len, *w and *h 0, nothing held, and
 * the reason in err (cut to err_len; err may be NULL). jpg NULL or len 0 is "not a valid JPEG". out and out_len must not be NULL.
 */
bool apps_jpeg_decode(const uint8_t *jpg, size_t len, int max_side, bool circle, uint8_t **out, size_t *out_len, int *w, int *h,
                      char *err, size_t err_len);
