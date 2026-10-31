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

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Bring up the display and build the UI: the avatar on the first tile,
 * settings one swipe to the left, and with CONFIG_MUSE_CLOCK a pocket watch
 * one swipe to the right, where the screen starts. Also owns screen sleep
 * and brightness.
 */
esp_err_t muse_ui_start(void);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* From any task: the face tile is the currently visible tile. */
bool muse_ui_face_active(void);
/* From any task: the pocket watch is the page showing, asleep or not. */
bool muse_ui_clock_showing(void);
/* From any task: slides to Muse's face at the next frame. */
void muse_ui_request_face(void);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image or before the UI is up.
 */
bool muse_ui_image_size(int *w, int *h);
/* What the panel shows of those pixels: 16 (colour), or 2 on a four-gray e-paper. */
int muse_ui_image_bits(void);
/* The image is complete: a four-gray e-paper dithers it to its grays. */
void muse_ui_image_done(void);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/* Bench testing, from any task: streams the screen over USB serial (SNAP OFF
 * where the build leaves it out: CONFIG_LV_USE_SNAPSHOT). */
void muse_ui_request_snapshot(void);
#if CONFIG_MUSE_BENCH
typedef enum {
    MUSE_UI_PAGE_FACE,
    MUSE_UI_PAGE_SETTINGS,
    MUSE_UI_PAGE_CLOCK,     /* the face, without CONFIG_MUSE_CLOCK */
} muse_ui_page_t;
/* Bench-only, from any task: select the touch tile without pretending a/s
 * are touch-menu navigation. No effect on non-touch boards. */
void muse_ui_bench_page(muse_ui_page_t page);
/* Display-only demo name for privacy-safe screenshots; no provisioning changes. */
void muse_ui_bench_demo(bool enabled);

/*
 * Bench-only, from any task: a scripted touch, played through LVGL's input
 * read so the widgets see presses, clicks, scrolls and gestures as from a
 * finger. A tap and a long press hold the point (x, y) for 80 and 900 ms; a
 * swipe ignores x and y and drags along y=233 between x=406 and x=60 over
 * 240 ms, its point following the clock on every read, a left swipe from the
 * right; an up swipe drags from 8 px above the bottom, at the middle (inside
 * the launcher's close edge), straight up 200 px in as long. While a script
 * plays the real touch isn't read. A request made while
 * one plays waits for it, and the latest of several waits. LVGL doesn't read
 * its input while the screen sleeps on battery: wake it first. No effect on
 * boards without a touch screen, or whose touch reads only on its interrupt.
 */
typedef enum {
    MUSE_UI_TOUCH_TAP,
    MUSE_UI_TOUCH_LONG,
    MUSE_UI_TOUCH_SWIPE_LEFT,
    MUSE_UI_TOUCH_SWIPE_RIGHT,
    MUSE_UI_TOUCH_SWIPE_UP,
} muse_ui_touch_t;
void muse_ui_bench_touch(muse_ui_touch_t kind, int x, int y);
#endif
