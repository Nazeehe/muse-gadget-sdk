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
 * One pass, with the UI lock held for all of it (apps_port_ui_trylock(2): a pass that can't get it is skipped).
 * The table is the one apps_lv_set_ui published, read once the lock is held:
 * 1. The table's clear flag: lv_obj_clean(layer), every object pointer dropped. The background, when it changed
 *    (the first take of a table says so).
 * 2. Deletes first. A widget inside a box that is deleted in the same pass goes with the box (LVGL deletes a
 *    parent's children), so for it the pass makes no LVGL call and only forgets its pointer, whichever of the two
 *    slots comes first.
 * 3. Then each slot with dirty bits, in slot order, by group: GEOM (lv_obj_set_size, lv_obj_align), STYLE, TEXT,
 *    VALUE, EVENTS (an image's GEOM goes with its STYLE: see Images). A box is made before what is inside it,
 *    even when its slot is the higher one (slots are reused), and checked first: a box that fails is not made,
 *    and what is in it is rejected for having no parent. A new widget gets every group its kind has, so one pass
 *    applies it from nothing.
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
 * Images. LVGL lays an image out at its own size and draws a scale as a transform about the middle of its box, which
 * it never resizes. An image is centred in its box (LVGL's default, set with lv_image_set_inner_align all the
 * same), and for a side of a scaled image that the app left to the content the box is made the size that is drawn,
 * side * scale / 100 to the nearest pixel, 1 to 466: align, x and y place what is on the screen, and the layout
 * written back and the overflow are of it (LVGL's own rounding leaves the picture within a pixel of its box, two
 * for an odd size at 400%). A side the app sized is its box, and the picture is centred in it and scaled about
 * its middle, so it can spill (the overflow is the box's). An A8 image is a mask that LVGL fills with the image's
 * recolor, black unless it is set, and an app has no colour to give an image: an A8 image is drawn white, in full
 * (image_recolor white, image_recolor_opa LV_OPA_COVER), so a coloured image has to be RGB565A8. The other
 * formats get no recolor.
 *
 * CREATE. A slot whose CREATE bit is set has no object: it is cleared only once the object exists, and the pass
 * keeps no pointer for a slot it hasn't finished making. A create is refused while the PSRAM free is under
 * APPS_LV_MIN_PSRAM (LVGL asserts when it can't allocate): the bits stay set, refused_creates counts it, and the
 * slot comes again next pass. The pass also records the kind, gen and parent each object was made for: a live
 * slot that is none of them is rejected. A CREATE that finds an object already recorded (the records of an app
 * that was never reset) deletes it first, and what is inside it, and makes a new one.
 *
 * Re-validation checks the shape of what LVGL is told: kind, parent chain (alive boxes, at most
 * APPS_UI_BOX_DEPTH_MAX of them with the widget itself counted if it is a box, as apps_ui.h says, and it ends: it
 * is walked in at most 5 steps, so a loop can't hang the LVGL task), every number and enum in its range, text
 * (NULL is ""; at most APPS_UI_TEXT_MAX bytes of valid UTF-8), min < max with value between, and an image of the
 * asset table that apps_image_check passes.
 *
 * Fonts: sans14 sans16 sans20 sans28 are lv_font_montserrat_14/16/20/28, mono8 and mono16 are lv_font_unscii_8/16,
 * digits48 and digits96 are apps_digits_48 and apps_digits_96 (fonts/). CONFIG_MUSE_APPS selects every LVGL font
 * this names. Opacity is opacity * 255 / 100, a scale of pct is pct * 256 / 100, radius -1 is LV_RADIUS_CIRCLE.
 * An image is drawn from the asset's bytes in place (its descriptor's header is the file's first 12 bytes, with
 * only the premultiplied flag kept: apps_image_check refuses the others), so they must outlive the objects, and be
 * 4-byte aligned, as a heap block is.
 *
 * Events. A widget with a subscribed event is clickable and has one callback for each of LV_EVENT_SHORT_CLICKED
 * (tap), LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSED, LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST (release), registered
 * once. LV_EVENT_CLICKED is not used: it follows a long press too. LVGL's order for a touch is PRESSED when the
 * finger goes down, then on the lift RELEASED, SHORT_CLICKED (when no long press was sent) and CLICKED: the app
 * hears press, release, tap, or press, long_press, release. When a swipe has taken the touch (the layer's
 * lv_indev_wait_release) the lift sends PRESS_LOST and nothing else, which is why that is a release too: a press
 * is never left open. The callback's user data is slot | gen << 16, and it calls the event callback with that slot
 * and gen, and the APPS_UI_* bit. It doesn't check the gen: an object deleted since can't send one, and an event
 * already queued meets Task 9's dispatch, which drops a slot that is gone or on another gen. An arc also gets a
 * guard that stops its own handling of a moving press: LVGL turns an arc's value to a finger on it, and the value
 * is the app's. It also keeps lv_arc's drag code, which divides by the radius, from ever running on an app's arc: an
 * arc with no w and h has a radius of 0 and can still be pressed, and the division would be a CPU exception.
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
 * The table's owner. This file holds the pointer to the app's table, and the pass reads it only with the UI lock
 * held, so the app task can end safely: with the lock held it publishes NULL (apps_lv_set_ui), then frees the table.
 * A pass that already holds the lock finishes first, and any later one finds NULL and does nothing. A caller that
 * kept the pointer itself and passed it in would have no such guarantee: it could read it, lose the lock to the app
 * task, and pass a table that is gone. Nothing else here keeps a pointer into the table past the end of a pass.
 *
 * Threads. The pass and the callbacks LVGL calls run on the LVGL task. apps_lv_build, apps_lv_reset and
 * apps_lv_set_front need the LVGL task or any task that holds the display lock (the LVGL lock): muse_ui builds its
 * overlays, this layer among them, in build_overlays(), on its own task, under the display lock and before the
 * frame timer exists, so no pass can run yet. apps_lv_set_ui runs on any task with the UI lock held, and
 * apps_lv_front and apps_lv_stats on any task. The table may be freed as soon as its pointer has been cleared.
 * What an object refers to is not the table but the package's images, which an image widget draws from in place:
 * free those only after apps_lv_reset has run on the LVGL task, which takes the objects down. The records are one
 * block from the apps' heap (apps_port_realloc, PSRAM on the device), made by the first pass that has a table and
 * freed by apps_lv_reset; the heap is not used with the lock held, so that pass lets go of the lock to make them
 * and reads the pointer again after.
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
 * Publishes the app's table (NULL: there is none), the one apps_lv_apply works on. The caller holds apps_port_ui_lock,
 * so a pass is never half way through reading the pointer: the app task publishes its table when the app starts and
 * clears it, with the lock held, before it frees the table. Any task. Only the pointer is kept (no memory, no LVGL
 * call), and apps_lv_reset leaves it alone.
 */
void apps_lv_set_ui(apps_ui_t *ui);

/*
 * One pass over the published table, the apply step above. in_callback says the app task is inside a callback (its
 * widgets may be half changed): the pass is skipped for up to 3 frames in a row, and the 4th applies anyway. It
 * takes the lock, reads the pointer with it held, and applies. False when no pass ran: it was skipped (the lock
 * was not free within 2 ms, the app is mid-callback, or the pass's records couldn't be made: each counted as a
 * skip), or there was nothing to apply (no layer, or no table published: neither counted, not a reject either).
 * The mid-callback skip comes first, before the lock is asked for and the pointer read, so with no table
 * published up to 3 in-callback frames in a row are still counted as skips: with no app published pass false, as
 * no callback can run without one.
 * A pass that took more than 2 ms counts slow, every time, and logs at most one line a second:
 * "apply %u us (%u slow since last)", the longest of the slow passes since the last line and how many there were.
 */
bool apps_lv_apply(bool in_callback);

/* Deletes every app object and forgets them, and puts the layer back to black. Call it when an app ends, with its
 * table no longer published, on the LVGL task. Whether the layer is in front is not changed. */
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

/*
 * The launcher and the crash card: two cards the firmware draws over the layer (Task 12), full-screen and black, made
 * the first time either is shown, on the screen apps_lv_build was given, just above the layer: over the app, under
 * what muse_ui puts above the layer (the downloaded image, the top layer). They are not on the layer, so
 * apps_lv_reset leaves them. Each takes every touch on it (clickable, not scrollable, and a swipe goes no further),
 * so the pages under them see none. The crash card sits over the launcher. Like apps_lv_reset, these need the LVGL
 * task, or a holder of the display lock: other tasks ask the app task's glue (muse_apps.h), which calls them at the
 * next frame. Without a layer they do nothing.
 */

/*
 * The launcher is a carousel: the apps in a row, most recently used first, that scrolls sideways and snaps an app to the
 * centre (LV_SCROLL_SNAP_CENTER), with no scrollbar and room at each end for the first and the last to come there. It opens
 * on the first. The app at the centre is drawn APPS_LV_ICON_PX square and named under it, in one label (LONG_MODE_DOTS, in a
 * box inside the circle); its neighbours, APPS_LV_ITEM_PITCH to each side, are smaller (APPS_LV_ICON_SIDE_SCALE), and each
 * app's size follows its distance from the centre as the row scrolls (apps_lv_carousel_scale, at every LV_EVENT_SCROLL). The app
 * that runs has a small dot under its icon.
 *
 * Each app is an item of APPS_LV_ICON_PX across that shows one of two faces, never neither: its icon, once apps_icons has its
 * pixels (apps_icons.h: the window of APPS_ICONS_WINDOW apps around the centre, asked for as the centre moves and polled in at
 * every frame), or else its tile, a circle in apps_lv_tile_color with apps_lv_tile_letter in it. A tile of scale s is
 * APPS_LV_ICON_PX * s / 256 across, to the nearest pixel; an icon w pixels square is drawn at s * APPS_LV_ICON_PX / w (LVGL's
 * image scale, about its middle), so every icon fills the same box; an A8 icon is drawn white, as an app's A8 image is. An
 * icon's descriptor is asked for again after every apps_icons_want and apps_icons_drop_all, so an item never shows a block
 * that was freed: one whose icon left the window, or went, is its tile again before LVGL draws.
 *
 * Touches, as LVGL makes them (a drag that scrolls the row sends no tap and no gesture). A tap is a touch that lifts less than LVGL's
 * scroll limit (10 px) from where it went down, on each axis; a slide that is neither a scroll nor a swipe does nothing:
 * - A tap on the app at the centre, with the row at rest (lv_obj_is_scrolling false when the finger went down and when it came up),
 *   holds the launcher (below) and calls pick with its id, on the LVGL task inside the tap (pick posts and returns). Anywhere in its
 *   item counts, the face or under it.
 * - A tap on any other app, anywhere in its item's box (APPS_LV_ICON_PX wide, wider than a neighbour's face), or on any app while
 *   the row moves (a drag, a fling's snap, the animation of an earlier tap), brings that app to the centre (lv_obj_scroll_to_view,
 *   animated) and opens nothing. A tap on the row between two apps does nothing.
 * - A tap on the card around the row (the title, the name, the empty state's text: none of them is clickable) closes it.
 * - A swipe sideways on the row scrolls it. A swipe up (LVGL's LV_DIR_TOP) whose press went down on the card itself within
 *   APPS_LV_CLOSE_EDGE_PX of the screen's bottom, inside the circle, closes it; no other swipe does. Whatever a swipe began on gets
 *   no tap.
 * Each close drops the icons. A touch that was down when the launcher closed does nothing more.
 *
 * The launch hold: a pick doesn't close the launcher. It stays on the screen as it was, over the clock, until the app's first frame,
 * so the clock never shows between the two: the app task's glue fronts the layer under it and closes it (apps_lv_hide_cards) in the
 * frame whose apply step first runs a pass with the layer in front, before LVGL draws that frame. The glue also closes it when the
 * launch is refused or the app fails before that frame (its crash card comes in the same frame), and a voice turn closes it as it
 * closes any card. While it holds it is shown (apps_lv_launcher_shown, apps_lv_cards_shown) but takes no input: no tap, no scroll
 * (the row isn't scrollable) and no swipe does anything. Its icons on the screen stay as they are (the centre's and its neighbours'),
 * and the rest are freed at the pick (apps_icons_keep), loads in flight too, so the launch's admission doesn't count them; none is asked
 * for or swapped in while it holds. A launch refused for memory nonetheless closes it, which frees those too, and the glue tries it once
 * more (apps_task.c). If nothing has closed it APPS_LV_LAUNCH_HOLD_MS after the pick, apps_lv_launcher_poll does. Showing it again ends
 * the hold.
 */

#define APPS_LV_ICON_PX 160          /* the centre's icon, and every item's box */
#define APPS_LV_ICON_SIDE_SCALE 100  /* a neighbour's scale, of 256: about 39%, 63 px */
#define APPS_LV_ITEM_PITCH 170       /* from one app's centre to the next's: the neighbours inside the circle, the next ones off it */
#define APPS_LV_LAUNCHER_MAX 32      /* apps the carousel holds (the store's most); more are left out */
#define APPS_LV_CLOSE_EDGE_PX 40     /* a swipe up closes the launcher when its press went down this close to the bottom */
#define APPS_LV_LAUNCH_HOLD_MS 3000  /* a pick holds the launcher at most this long for the app's first frame */

/* The launcher's n while the store hasn't been mounted (apps_store.h): "Loading apps..." in place of the carousel (the font has
 * no ellipsis). */
#define APPS_LV_LOADING (-1)

/* An app in the launcher: its id, the name it shows (NULL or "": the id) and its icon's path in its folder ("" or NULL: none,
 * its tile). The launcher's own type, so apps_lv doesn't depend on the app task's. */
typedef struct {
    const char *id, *name, *icon;
} apps_lv_app_t;

/* The launcher: "Apps" over the carousel of apps[0..n-1] (at most APPS_LV_LAUNCHER_MAX; one with a NULL or "" id is left out),
 * with the dot under running_id's (NULL or an id the list hasn't: none); for n 0 (or any other n under 1, or apps NULL), "No apps
 * yet. Ask Muse to make one.", or for APPS_LV_LOADING "Loading apps...". Everything is copied (a name at most 24 bytes, as a
 * manifest's). Showing it again makes the carousel again, from the first app (the app task's glue does when the store's list
 * changes while it is shown), and drops the icons the last one had. The LVGL task, outside the UI lock (apps_icons.h). */
void apps_lv_show_launcher(const apps_lv_app_t *apps, int n, const char *running_id, void (*pick)(const char *id));

/* Each frame, on the LVGL task, outside the UI lock: while the launcher shows, its icons that came are swapped in for their tiles
 * (apps_icons_poll). Nothing while it is closed. While it holds for a launch, no icon is swapped in, and at APPS_LV_LAUNCH_HOLD_MS
 * after the pick it closes. */
void apps_lv_launcher_poll(void);

/* The scale (256 = 100%) of an app whose centre is dx_px from the screen's: 256 at 0, falling linearly to APPS_LV_ICON_SIDE_SCALE
 * at APPS_LV_ITEM_PITCH either side, and that beyond. */
int apps_lv_carousel_scale(int dx_px);

/* A tile's colour, 0xRRGGBB: one of 8, by the CRC-32 of the id (zlib's; NULL is ""), so an app's never changes. */
uint32_t apps_lv_tile_color(const char *id);

/* A tile's letter: name's first byte, upper case, when it is an ASCII letter or digit; '?' otherwise (NULL, "", a symbol, the
 * first byte of a UTF-8 character). */
char apps_lv_tile_letter(const char *name);

/* Whether the launcher is showing. */
bool apps_lv_launcher_shown(void);

/* Whether it holds for a launch: a pick of its centre app, and no close since (it is shown, too). */
bool apps_lv_launcher_holding(void);

/* The crash card: "<name> stopped" over first_line (the crash record's first line), or "<name> stopped responding"
 * when first_line is NULL (the supervisor's card for a hung app); "Tap to close" under them. NULL or "" name is "The
 * app". A tap closes it. */
void apps_lv_show_crash(const char *name, const char *first_line);

/* Closes both cards: an app came to the front, or a voice turn began. The launcher's icons are dropped (apps_icons_drop_all). */
void apps_lv_hide_cards(void);

/* Whether a card is showing. */
bool apps_lv_cards_shown(void);
