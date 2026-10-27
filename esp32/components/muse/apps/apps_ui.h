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
 * The `ui` module Muse apps draw with, and the shadow widget table behind it. Portable C over apps_vm: the
 * platform comes in through apps_port.h, and nothing here touches LVGL. A binding validates what the app asked
 * for, writes it into this table and sets dirty bits; the LVGL task's apply step (apps_lv, Task 11) reads the
 * table once per frame and makes the real objects. Lua never sees an lv_obj_t*, and LVGL never sees a value
 * this module has not checked: LVGL asserts abort().
 *
 * The Lua module (the guide copies this).
 * - ui.label{...} ui.button{...} ui.box{...} ui.arc{...} ui.bar{...} ui.image{...} make a widget and return its
 *   handle. w:set{...} changes properties, w:get(name) reads one, w:on(event, fn) subscribes (fn nil removes it),
 *   w:delete() removes the widget and everything inside it. set and on return w, so calls chain. tostring(w) is
 *   "label#3": the kind and the slot. Two handles of one widget are equal (==).
 * - ui.bg(color) is the screen's background (black at first); ui.clear() removes every widget; ui.polar(deg, r)
 *   returns the x and y offsets of a point r pixels from the centre at deg clock degrees, as integers: ui.polar(90,
 *   100) is 100, 0, and ui.polar(0, 100) is 0, -100. They go straight into the x and y of a widget whose align is
 *   "center". ui.W = ui.H = 466, ui.R = 233 and ui.SAFE = 330 (the inscribed square).
 * - Properties. All: align x y w h hidden opacity parent. label: text font color text_align wrap. button: text
 *   font color bg radius border border_color. box: bg radius border border_color. arc: min max value start_angle
 *   end_angle thickness color track_color. bar: min max value color track_color radius. image: src scale.
 *   (apps_ui_props_<kind> in apps_ui.c are the lists; the unknown-property message prints them.)
 * - Values. align is one of center top bottom left right top_left top_right bottom_left bottom_right, and x y are
 *   offsets from that anchor in the parent. Numbers are rounded to the nearest integer (halves away from zero)
 *   and then clamped: x, y to +-466; w, h to 1..466 (a widget with no w or h sizes to its content); opacity 0..100;
 *   radius 0..233, or "circle"; border 0..20; thickness 1..64; start_angle and end_angle 0..360 in clock degrees
 *   (0 is 12 o'clock, clockwise); scale 10..400 (percent); min and max +-APPS_UI_VALUE_MAX; value to [min, max]. An
 *   infinity clamps to the end of the range, and NaN is an error. hidden and wrap are booleans. text is a string
 *   or a number (as tostring makes it) of at most APPS_UI_TEXT_MAX bytes, valid UTF-8 (apps_json_utf8_len) and
 *   with no NUL byte; "" is no text. font is one of sans14 sans16 sans20 sans28 mono8 mono16 digits48 digits96
 *   (the digit fonts hold "0-9:.-+% "), text_align one of left center right. Colours are "#rgb", "#rrggbb" or
 *   0xRRGGBB, and bg also takes "none". src is the path of an image of the app's package (manifest.json "files").
 *   parent is a box, and can be given only when the widget is made. A widget may have at most
 *   APPS_UI_BOX_DEPTH_MAX box ancestors.
 * - Defaults. Every widget: align center, x y 0, w h to content, visible, opacity 100, border_color white. label:
 *   font sans20, text_align center, color white. button: bg "#262626", radius 16, font sans20, color white.
 *   box: bg none, radius 0. arc: min 0, max 100, value 0, start_angle 0, end_angle 360, thickness 12, color
 *   white, track_color "#262626". bar: min 0, max 100, value 0, color white, track_color "#262626", radius 0.
 *   image: scale 100, and src is required.
 * - min < max is kept: a create or a set that would leave min >= max on an arc or a bar is an error, and value
 *   follows min and max (it is clamped again when they change).
 * - Events: tap (and click, the same), long_press, press and release, on a widget of any kind. A handler gets
 *   (w, event) with the event's own name, "tap" for a click too. w:on replaces the event's handler. Deleting a
 *   widget drops its handlers.
 * - w:get returns what set takes: colours as "#rrggbb" (lower case) or "none"; align, font and text_align as
 *   names; radius "circle" or a number; w and h nil while they size to content (the declared values: the laid-out
 *   size comes from the LVGL task, later); parent as the parent's handle or nil; src as the path.
 *
 * Messages (the position "main.lua:12: " comes first; ui.<kind> is ui.label ...; <p> is a property name):
 *   ui.<kind>: '<p>' must be a number, got <type>          also "got NaN"; must be a boolean, got <type>
 *   ui.<kind>: 'text' must be a string, got <type>
 *   ui.<kind>: unknown property '<name>' (valid: align, x, y, w, h, hidden, opacity, parent, text, ...)
 *   ui.<kind>: font 'sans24' is not one of sans14, sans16, ...      (align and text_align the same way)
 *   ui.<kind>: bg '#12' is not a colour (use "#rgb", "#rrggbb" or 0xRRGGBB)
 *   ui.<kind>: radius '<s>' is not a number or "circle"
 *   ui.<kind>: text is longer than 256 bytes | text is not valid UTF-8 | text can't contain a NUL byte
 *   ui.<kind>: min must be less than max
 *   ui.<kind>: parent must be a ui.box | parent was deleted | boxes nest at most 4 deep
 *   ui.<kind>: parent can only be set when the widget is created
 *   ui.image: src is required (a .bin image of this app's manifest.json "files") | src '<s>' is not one of this
 *             app's images (manifest.json "files")
 *   ui.<kind>: unknown method '<name>' (valid: set, get, on, delete)
 *   ui.<kind>: unknown event '<s>' (valid: tap, long_press, press, release)
 *   ui.<kind>: handler for '<event>' must be a function (or nil to remove it), got <type>
 *   ui: no more than 128 widgets per app            (a widget deleted in this callback keeps its slot until
 *                                                    the next screen update: the message then adds that)
 *   widget was deleted                                        any use of a handle of a deleted widget
 *   ui.<name> doesn't exist (ui has: label, button, box, arc, bar, image, bg, clear, polar, W, H, R, SAFE)
 *   ui.bg: '<s>' is not a colour (...)
 * Text an app supplied is shown cut at 40 bytes and with its control bytes as '?'.
 *
 * The table. APPS_UI_SLOTS widgets in a block from apps_port_realloc; the slot count is also LVGL's object quota
 * for the app. A handle is userdata {slot, gen, kind}, and a slot's gen is bumped when its widget is deleted, so
 * a handle (and a touch event, which carries the gen it saw) of a widget that is gone finds the slot dead or on
 * another gen, and is dropped. Deleting a box deletes everything inside it. The invariants, which hold whenever
 * the lock is free:
 * - an alive widget's parent is alive and a box, and has at most APPS_UI_BOX_DEPTH_MAX box ancestors, so the
 *   parent chain ends and the apply step may walk it;
 * - an arc or bar has min < max and min <= value <= max, and all of them are within +-APPS_UI_VALUE_MAX;
 * - every number and enum is in its range, text is NULL (for "") or a valid, NUL-terminated block of at most
 *   APPS_UI_TEXT_MAX bytes, and a widget of kind image has an image in the asset table;
 * - apps_ui_count() is the number of alive widgets.
 *
 * Threads and the lock. Everything that changes the table runs on the app task (the VM's), and the apply step
 * on the LVGL task. apps_port_ui_lock guards what both touch: every change takes it, the apply step holds it for
 * its whole pass (apps_port_ui_trylock) and reads, clears dirty bits and writes rx..overflow back. A binding
 * does all its work before it takes the lock (calls apps_vm_check, validates, allocates text and the handle) and
 * under the lock does plain C on the table, so it never raises, and never allocates, while it holds the lock; the
 * old text block is freed after the lock is released. The table's fields the apply step doesn't write
 * (everything but dirty and rx..overflow) are read by the app task without the lock.
 *
 * The apply step's side (Task 11). Per pass, with the lock held:
 * 1. apps_ui_take_clear: when it says true ui.clear() ran since the last pass: every object of the app is
 *    gone (lv_obj_clean), so drop every object pointer, then go on with the dirty slots below.
 * 2. apps_ui_take_bg: the layer's colour, and whether it changed. The first take of a table says changed,
 *    whatever the layer was left at by the app before.
 * 3. Each slot 0..APPS_UI_SLOTS-1, apps_ui_slot, by its dirty bits. A bit it handled is cleared; one it
 *    couldn't (a create refused for want of memory) stays set, and the slot comes again next pass.
 *    - APPS_DIRTY_DELETE (alive is false): delete the slot's object if it has one (a widget inside a box that
 *      is deleted in the same pass goes with it: kind and parent are still there to see that), then clear
 *      dirty. Only then is the slot free for a new widget, so a delete that isn't applied yet holds its slot.
 *    - alive with APPS_DIRTY_CREATE: make the object. A widget is made before the widgets inside it, but the
 *      parent's slot may be the higher: slots are reused. A new widget has CREATE and also GEOM and STYLE, TEXT
 *      for a label and button, VALUE for an arc and bar, so one that applies each bit in turn after making the
 *      object applies everything.
 *    - alive with other bits: GEOM is align, x, y, w, h; STYLE is hidden, opacity, font, color, text_align, wrap,
 *      bg, radius, border, border_color, thickness, track_color, src and scale; TEXT is text; VALUE is min, max,
 *      value, start_angle and end_angle; EVENTS is the events mask (a widget with any is clickable). A change
 *      to what a widget sizes to (a text, a font) moves its neighbours: lay the layer out after any change and
 *      write rx, ry, rw, rh (screen coordinates) and overflow back.
 *    A set to the value a slot already has sets no bit. A widget made and deleted before a pass has no bits.
 *
 * Handlers live in a table in the Lua registry, keyed by slot and event, and are looked up (apps_ui_push_handler)
 * against the slot's gen at dispatch, as well as dropped at delete.
 *
 * apps_ui_dump is the dump Muse sees with a snapshot: one object per alive widget, in slot order, as a JSON
 * array: {"id":"label#3","kind":"label","text":"Time","x":0,"y":0,"w":null,"h":null,"hidden":false,"overflow":0}.
 * text is null for a kind that has none, and x y w h are the laid-out geometry once rw > 0 (else the declared
 * values, with null for a size to content).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* lua.h declares this too; the firmware outside apps/ includes this header, not Lua's. */
typedef struct lua_State lua_State;

#define APPS_UI_SLOTS 128
#define APPS_UI_TEXT_MAX 256
#define APPS_UI_VALUE_MAX 1000000  /* an arc's or bar's min and max: lv_map multiplies by up to 466 in int32 */
#define APPS_UI_BOX_DEPTH_MAX 4
#define APPS_UI_ASSETS_MAX 32      /* the package's most files */
#define APPS_COLOR_NONE 0xFF000000u

typedef enum { APPS_W_LABEL, APPS_W_BUTTON, APPS_W_BOX, APPS_W_ARC, APPS_W_BAR, APPS_W_IMAGE } apps_wkind_t;
typedef enum {
    APPS_FONT_SANS14, APPS_FONT_SANS16, APPS_FONT_SANS20, APPS_FONT_SANS28,
    APPS_FONT_MONO8, APPS_FONT_MONO16, APPS_FONT_DIGITS48, APPS_FONT_DIGITS96
} apps_font_t;
typedef enum {
    APPS_ALIGN_CENTER, APPS_ALIGN_TOP, APPS_ALIGN_BOTTOM, APPS_ALIGN_LEFT, APPS_ALIGN_RIGHT,
    APPS_ALIGN_TOP_LEFT, APPS_ALIGN_TOP_RIGHT, APPS_ALIGN_BOTTOM_LEFT, APPS_ALIGN_BOTTOM_RIGHT
} apps_align_t;

/* Widget events; APPS_EV_* are task events. An event's bit is 1 << its index in {tap, long_press, press, release}. */
enum { APPS_UI_TAP = 1, APPS_UI_LONG_PRESS = 2, APPS_UI_PRESS = 4, APPS_UI_RELEASE = 8 };

enum {
    APPS_DIRTY_CREATE = 1, APPS_DIRTY_GEOM = 2, APPS_DIRTY_STYLE = 4, APPS_DIRTY_TEXT = 8,
    APPS_DIRTY_VALUE = 16, APPS_DIRTY_EVENTS = 32, APPS_DIRTY_DELETE = 64
};

typedef struct {
    uint16_t gen;
    uint8_t kind;
    bool alive;
    int16_t parent;                            /* slot, or -1: the layer */
    int16_t x, y, w, h;                        /* w/h -1: size to content */
    uint8_t align, font, opacity, text_align;  /* text_align 0 left, 1 center, 2 right */
    bool hidden, wrap;
    uint32_t color, bg, border_color, track_color;  /* 0xRRGGBB, or (bg only) APPS_COLOR_NONE */
    uint8_t border, thickness;
    int16_t radius;                            /* radius -1: circle */
    int32_t min, max, value;
    int16_t start_angle, end_angle;
    int16_t image;                             /* asset index or -1 */
    uint16_t scale;                            /* percent */
    char *text;                                /* PSRAM, NULL = "" */
    uint8_t events;                            /* APPS_UI_* subscribed */
    uint32_t dirty;                            /* APPS_DIRTY_*; the apply step clears it */
    int16_t rx, ry, rw, rh, overflow;          /* written back by the apply step; rw 0 = not yet laid out */
} apps_widget_t;

/* An image of the package: its path (manifest.json "files"), its .bin bytes (an LVGL v9 file that apps_pkg.h's
 * apps_image_check passed) and its size. The strings and the bytes stay the caller's: they must outlive the table. */
typedef struct {
    const char *path;
    const uint8_t *data;
    size_t len;
    uint16_t w, h;
} apps_asset_t;

typedef struct apps_ui apps_ui_t;

/* An empty table, from apps_port_realloc; NULL when that refuses. The background is black, and nothing is
 * pending but the first take of it. */
apps_ui_t *apps_ui_new(void);

/* Frees the table and its text. NULL is ignored. Nothing may read it any more (the apply step has let go), and
 * no lock is taken. */
void apps_ui_free(apps_ui_t *ui);

/* The images ui.image may name, copied (at most APPS_UI_ASSETS_MAX; the strings and bytes stay the caller's).
 * Before main.lua runs: apps_rt_new calls it with the package's .bin files, so a top-level ui.image works. */
void apps_ui_set_assets(apps_ui_t *ui, const apps_asset_t *assets, int n);

/* Installs the global `ui`. It allocates, but it is safe outside a protected call: if the heap refuses, `ui` isn't
 * installed and the VM is out of memory (apps_vm.h), so the next apps_vm_call ends NOMEM. The stack is as it was. */
void apps_ui_open(lua_State *L, apps_ui_t *ui);

/* ---- The apply step (the LVGL task, lock held; The apply step's side, above) ---- */

/* The slot's widget, alive or not: its dirty bits say what is to be done. NULL for a slot out of range. */
apps_widget_t *apps_ui_slot(apps_ui_t *ui, int slot);
/* The image at index, or NULL. */
const apps_asset_t *apps_ui_asset(const apps_ui_t *ui, int index);
/* The background colour, and in *changed (may be NULL) whether it changed since the last take. */
uint32_t apps_ui_take_bg(apps_ui_t *ui, bool *changed);
/* Whether ui.clear() ran since the last take: true once. */
bool apps_ui_take_clear(apps_ui_t *ui);

/* ---- The app task ---- */

/*
 * For apps_vm_call, inside a C function the call runs, with 4 free stack slots: when the widget at slot is
 * alive on that gen and subscribed to event (one APPS_UI_* bit) with a handler, pushes the handler, the widget
 * (a handle) and the event's name, returns true, and the caller calls it with nargs 2. Otherwise (a slot that is
 * gone, a gen that isn't its, an event nobody asked for, or no room) returns false and pushes nothing. It
 * allocates (the handle, the name), so it raises when the app's heap refuses: only inside a protected call.
 */
bool apps_ui_push_handler(lua_State *L, apps_ui_t *ui, uint16_t slot, uint16_t gen, uint8_t event);

/* ---- Any task ---- */

/*
 * The dump (above) into out, which gets at most cap bytes with the NUL, and the length written. Whole entries
 * only, in slot order: the first that doesn't fit and every one after it are dropped, counted in *dropped (may be
 * NULL), and the array is always closed, so out is valid JSON whenever cap is 3 or more. For a cap under 3 (or a
 * NULL out) there is no room for "[]": out is "" and every widget is dropped. It takes the lock itself, so the
 * caller must not hold it, and the laid-out geometry it reads is one pass's. An entry is under 2 KB.
 */
size_t apps_ui_dump(const apps_ui_t *ui, char *out, size_t cap, int *dropped);

/* The number of alive widgets. Not under the lock: it may be a moment stale on another task. */
int apps_ui_count(const apps_ui_t *ui);
