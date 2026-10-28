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
 * The apply step of Muse apps: the one place that turns an app's shadow widget table (apps_ui.h) into LVGL
 * objects, once per frame, on the LVGL task. The app task only ever edits the table; Lua never sees an lv_obj_t*.
 * LVGL asserts abort(), and a hang here stops the whole UI, so this reads nothing it has not checked: every dirty
 * slot is validated again, whatever apps_ui promised, and a slot that fails is dropped (its bits are cleared and
 * it counts a reject, logged once per app), never drawn.
 *
 * One pass, with the UI lock held for all of it (apps_port_ui_trylock(2): a pass that can't get it is skipped):
 * 1. The table's clear flag: lv_obj_clean(layer), every object pointer dropped. The background, when it changed
 *    (the first take of a table says so).
 * 2. Deletes first. A widget inside a box that is deleted in the same pass goes with the box (LVGL deletes a
 *    parent's children), so for it the pass makes no LVGL call and only forgets its pointer, whichever of the two
 *    slots comes first.
 * 3. Then each slot with dirty bits, in slot order, by group: GEOM (lv_obj_set_size, lv_obj_align), STYLE, TEXT,
 *    VALUE, EVENTS. A box is made before what is inside it, even when its slot is the higher one (slots are
 *    reused); a new widget gets every group its kind has, so one pass applies it from nothing.
 * 4. When a widget was made, changed or deleted: lv_obj_update_layout(layer), and rx ry rw rh overflow of every
 *    widget that has an object are written back (screen coordinates; rw 0 is "not laid out"). A pass with nothing
 *    to do (no dirty bit, no clear, no new background) makes no LVGL call at all.
 * What a pass does not do: reparent (a widget's parent is set when it is made), or call LVGL for a slot it
 * rejected.
 *
 * Objects. LVGL's own style is taken off each (every value shown is one the table has), and none is scrollable or
 * clickable but a widget with an event. An arc and a bar have no size of their own, so one with no w and h is
 * nothing on screen. What a widget lays out in is its parent's content area: a box's border counts.
 *
 * CREATE. A slot whose CREATE bit is set has no object: it is cleared only once the object exists, and the pass
 * keeps no pointer for a slot it hasn't finished making. A create is refused while the PSRAM free is under
 * APPS_LV_MIN_PSRAM (LVGL asserts when it can't allocate): the bits stay set, refused_creates counts it, and the
 * slot comes again next pass. The pass also records the kind, gen and parent each object was made for: a live
 * slot that is none of them is rejected. A CREATE that finds an object already recorded (the records of an app
 * that was never reset) deletes it first, and what is inside it, and makes a new one.
 *
 * Re-validation checks the shape of what LVGL is told: kind, parent chain (alive boxes, at most
 * APPS_UI_BOX_DEPTH_MAX of them, and it ends: it is walked in at most 5 steps, so a loop can't hang the LVGL
 * task), every number and enum in its range, text (NULL is ""; at most APPS_UI_TEXT_MAX bytes of valid UTF-8),
 * min < max with value between, and an image of the asset table that apps_image_check passes.
 *
 * Fonts: sans14 sans16 sans20 sans28 are lv_font_montserrat_14/16/20/28, mono8 and mono16 are lv_font_unscii_8/16,
 * digits48 and digits96 are apps_digits_48 and apps_digits_96 (fonts/). CONFIG_MUSE_APPS selects every LVGL font
 * this names. Opacity is opacity * 255 / 100, a scale of pct is pct * 256 / 100, radius -1 is LV_RADIUS_CIRCLE.
 * An image is drawn from the asset's bytes in place (its descriptor's header is the file's first 12 bytes), so
 * they must outlive the objects, and be 4-byte aligned, as a heap block is.
 *
 * Events. A widget with a subscribed event is clickable and has one callback for each of LV_EVENT_SHORT_CLICKED
 * (tap), LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSED and LV_EVENT_RELEASED, registered once. LV_EVENT_CLICKED is not
 * used: it follows a long press too. The callback's user data is slot | gen << 16, and it calls the event callback
 * with that slot and gen, and the APPS_UI_* bit. It doesn't check the gen: an object deleted since can't send one,
 * and an event already queued meets Task 9's dispatch, which drops a slot that is gone or on another gen. An arc
 * also gets a guard that stops its own handling of a moving press: LVGL turns an arc's value to a finger on it,
 * and the value is the app's.
 *
 * Round-screen feedback: overflow is how far the widget goes past the round edge, in px, to the nearest (0 when it
 * fits, and for a hidden widget or one inside a hidden box). A rectangle goes as far as its farthest corner from
 * the centre (233, 233), less 233; what is round uses its outline: an arc is the circle of its box, and a box with
 * radius "circle" (or one at least half its shorter side) is a stadium, a segment of the box's long axis with a
 * circle of half the shorter side round it. It is integer arithmetic, so a host test and the device agree.
 *
 * The layer. apps_lv_build makes it once, a child of the screen: black, clickable, not scrollable, hidden. It is
 * the only parent of the app's objects, and nothing else is put on it: apps_lv_reset cleans it. While it is
 * hidden (the app is behind) the pass still runs, so the layout is current when it comes to the front. A swipe
 * right on it (one that starts on a widget too: widgets pass a swipe up) calls the close callback.
 *
 * Threads. Everything here runs on the LVGL task (the pass, build, reset, set_front, the callbacks LVGL calls)
 * except apps_lv_front and apps_lv_stats, which any task may call. The pass keeps no pointer to the table between
 * calls, and a pointer to the table is used only inside apps_lv_apply, so once apps_lv_reset has run on the LVGL
 * task nothing here refers to the app any more and its runtime may be freed (apps_rt_free; the package's images
 * too: an image widget draws from their bytes, so the objects go first). The records are one block from the apps'
 * heap (apps_port_realloc, PSRAM on the device), made before the pass takes the lock and freed by apps_lv_reset.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "apps_ui.h"

/* A create is refused while this much PSRAM is not free. */
#define APPS_LV_MIN_PSRAM (256 * 1024)

/* A touch: the widget's slot and gen (what the object was made for), and one APPS_UI_* bit. Called on the LVGL
 * task: post it and return. */
typedef void (*apps_lv_event_cb_t)(uint16_t slot, uint16_t gen, uint8_t event);

/* Makes the app layer on screen: full-screen, black, clickable, not scrollable and hidden. Once. */
void apps_lv_build(lv_obj_t *screen);

/* Where touches on widgets go (NULL: nowhere), and what a right swipe on the layer does (NULL: nothing). */
void apps_lv_set_event_cb(apps_lv_event_cb_t cb);
void apps_lv_set_close_cb(void (*cb)(void));

/*
 * One pass over the table, the apply step above. in_callback says the app task is inside a callback (its widgets
 * may be half changed): the pass is skipped for up to 3 frames in a row, and the 4th applies anyway. False when
 * the pass was skipped (the lock was not free within 2 ms, the app is mid-callback, or there is no table or no
 * layer): nothing was read or changed. A pass that took more than 2 ms logs "apply %u us" and counts slow.
 * ui may be NULL (no app is running).
 */
bool apps_lv_apply(apps_ui_t *ui, bool in_callback);

/* Deletes every app object and forgets them, and puts the layer back to black. Call it when an app ends (before
 * its runtime is freed), on the LVGL task. Whether the layer is in front is not changed. */
void apps_lv_reset(void);

/* The layer in front (shown) or behind (hidden). apps_lv_front: any task. */
void apps_lv_set_front(bool front);
bool apps_lv_front(void);

/* Since boot (a reset doesn't clear them). applies: passes that ran; skips: passes that didn't (the lock, a
 * callback, or no memory for the pass's records); rejects: slots dropped by re-validation; refused_creates:
 * widgets not made for want of PSRAM; slow: passes over 2 ms. Any task; a value may be a frame old. */
typedef struct {
    uint32_t applies, skips, rejects, refused_creates, slow;
} apps_lv_stats_t;
void apps_lv_stats(apps_lv_stats_t *out);
