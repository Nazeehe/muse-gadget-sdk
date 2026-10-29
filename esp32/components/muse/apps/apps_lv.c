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

#include "apps_lv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "apps_json.h"
#include "apps_pkg.h"
#include "apps_port.h"

/* The apply step, the layer and the cards over it; apps_lv.h has the contract. All of it runs on the LVGL task. */

LV_FONT_DECLARE(apps_digits_48)
LV_FONT_DECLARE(apps_digits_96)

static const char *TAG = "muse_apps";

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define SCREEN_PX 466
#define SCREEN_R 233
#define IMAGE_HEADER 12  /* the first bytes of a .bin file: an lv_image_header_t */

#define LOCK_MS 2
#define SLOW_US 2000
#define SLOW_LOG_US 1000000  /* a slow pass is logged at most this often */
#define MAX_CALLBACK_SKIPS 3

/* The bits that name a group of properties, as opposed to CREATE and DELETE. */
#define GROUPS (APPS_DIRTY_GEOM | APPS_DIRTY_STYLE | APPS_DIRTY_TEXT | APPS_DIRTY_VALUE | APPS_DIRTY_EVENTS)

_Static_assert(sizeof(lv_image_header_t) == IMAGE_HEADER, "an image's header is the first 12 bytes of its file");
_Static_assert(APPS_UI_SLOTS <= 0xFFFF, "a slot goes in 16 bits of a callback's user data");

/* What the pass knows of one slot's object. Slots are the table's, so a record says nothing the table has not
 * said once: the kind, gen and parent the object was made for, to tell a slot that is still that widget from one
 * that is not. */
typedef struct {
    lv_obj_t *obj;     /* NULL: the slot has no object */
    lv_obj_t *label;   /* a button's text; the button itself is obj */
    uint16_t gen;
    int16_t up;        /* the parent's slot + 1; 0: the layer */
    uint8_t kind;
    bool subscribed;   /* the touch callbacks are on obj */
    bool recolored;    /* an A8 image has the white recolor on it */
} node_t;

/* The records of the app on the layer, and the descriptors its images are drawn from: one block, from the apps'
 * heap (PSRAM on the device, so it takes no internal RAM), made by the first pass that has a table, with the lock
 * given back (the heap is not used under it), and freed by apps_lv_reset. */
typedef struct {
    node_t node[APPS_UI_SLOTS];
    lv_image_dsc_t image[APPS_UI_ASSETS_MAX];  /* per asset; data NULL until the asset is first drawn */
} state_t;

static lv_obj_t *s_layer;
static apps_ui_t *s_ui;          /* the app's table: written with the UI lock held, and read only with it held */
static state_t *s_state;
static apps_lv_event_cb_t s_event_cb;
static void (*s_close_cb)(void);
static volatile bool s_front;
static uint8_t s_skip_run;       /* frames skipped in a row for a callback */
static bool s_reject_logged;     /* the app's first reject is in the log */
static int64_t s_slow_next_log;  /* the esp_timer time a slow pass may be logged from (0: any) */
static uint32_t s_slow_unlogged; /* slow passes since the last line, and the longest of them */
static int64_t s_slow_worst_us;

static volatile uint32_t s_applies, s_skips, s_rejects, s_refused, s_slow;

/* The cards (made at the first show): their parent, the screen the layer is on, and what is shown on them. */
static lv_obj_t *s_screen;
static lv_obj_t *s_launcher, *s_crash, *s_crash_title, *s_crash_body;
static bool s_launcher_shown, s_crash_shown;
static void (*s_pick)(const char *id);
static char s_row_ids[APPS_LV_LAUNCHER_ROWS][APPS_ID_MAX + 1];

/* One pass: what it works on and what it found. */
typedef struct {
    apps_ui_t *ui;
    state_t *st;
    bool changed;            /* LVGL was called for a widget: lay out and write back */
    uint32_t rejects, refused;
    int reject_slot;         /* the first slot dropped, or -1 */
    const char *reject_why;
} pass_t;

typedef enum { MADE, REFUSED, BAD } made_t;

/* ---- Mappings ---- */

static const lv_font_t *font_of(uint8_t font) {
    switch (font) {
        case APPS_FONT_SANS14: return &lv_font_montserrat_14;
        case APPS_FONT_SANS16: return &lv_font_montserrat_16;
        case APPS_FONT_SANS28: return &lv_font_montserrat_28;
        case APPS_FONT_MONO8: return &lv_font_unscii_8;
        case APPS_FONT_MONO16: return &lv_font_unscii_16;
        case APPS_FONT_DIGITS48: return &apps_digits_48;
        case APPS_FONT_DIGITS96: return &apps_digits_96;
        case APPS_FONT_SANS20:
        default: return &lv_font_montserrat_20;
    }
}

static lv_align_t align_of(uint8_t align) {
    static const lv_align_t map[] = {LV_ALIGN_CENTER,      LV_ALIGN_TOP_MID,      LV_ALIGN_BOTTOM_MID,
                                     LV_ALIGN_LEFT_MID,    LV_ALIGN_RIGHT_MID,    LV_ALIGN_TOP_LEFT,
                                     LV_ALIGN_TOP_RIGHT,   LV_ALIGN_BOTTOM_LEFT,  LV_ALIGN_BOTTOM_RIGHT};
    return align < ARRAY_LEN(map) ? map[align] : LV_ALIGN_CENTER;
}

static lv_text_align_t text_align_of(uint8_t align) {
    static const lv_text_align_t map[] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT};
    return align < ARRAY_LEN(map) ? map[align] : LV_TEXT_ALIGN_CENTER;
}

static lv_color_t colour(uint32_t rgb) {
    return lv_color_hex(rgb & 0xFFFFFF);
}

static int32_t radius_of(int16_t radius) {
    return radius < 0 ? LV_RADIUS_CIRCLE : radius;
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- Re-validation ---- */

/* Whether the widget's image is one of the asset table that apps_image_check passes; its size (from the file's header)
 * goes to *iw and *ih (either may be NULL). */
static bool image_size(const pass_t *p, const apps_widget_t *w, uint16_t *iw, uint16_t *ih) {
    const apps_asset_t *asset = apps_ui_asset(p->ui, w->image);
    return asset != NULL && w->image < APPS_UI_ASSETS_MAX && apps_image_check(asset->data, asset->len, iw, ih, NULL, 0);
}

/* Why the widget at slot can't be given to LVGL, or NULL when it can. This is the table's invariants (apps_ui.h)
 * checked again, field by field, because LVGL asserts abort() and a loop here would hang the UI. The text is
 * read at most APPS_UI_TEXT_MAX + 1 bytes in. */
static const char *check_widget(const pass_t *p, int slot, const apps_widget_t *w) {
    if (w->kind > APPS_W_IMAGE) {
        return "kind";
    }

    /* The parent chain: alive boxes, at most APPS_UI_BOX_DEPTH_MAX of them, and a box counts itself (the rule of
     * apps_ui.h: a box has at most APPS_UI_BOX_DEPTH_MAX - 1 boxes above it, any other widget at most
     * APPS_UI_BOX_DEPTH_MAX), so it is walked in at most one step more than that and a loop ends in a reject. */
    if (w->parent < -1 || w->parent >= APPS_UI_SLOTS || w->parent == slot) {
        return "parent";
    }
    int boxes = w->kind == APPS_W_BOX;
    for (int up = w->parent; up != -1;) {
        const apps_widget_t *a = apps_ui_slot(p->ui, up);
        if (a == NULL || !a->alive || a->kind != APPS_W_BOX || ++boxes > APPS_UI_BOX_DEPTH_MAX || a->parent < -1) {
            return "parent";
        }
        up = a->parent;
    }

    if (w->x < -SCREEN_PX || w->x > SCREEN_PX || w->y < -SCREEN_PX || w->y > SCREEN_PX) {
        return "position";
    }
    if (w->w != -1 && (w->w < 1 || w->w > SCREEN_PX)) {
        return "width";
    }
    if (w->h != -1 && (w->h < 1 || w->h > SCREEN_PX)) {
        return "height";
    }
    if (w->align > APPS_ALIGN_BOTTOM_RIGHT) {
        return "align";
    }
    if (w->opacity > 100) {
        return "opacity";
    }
    if (w->font > APPS_FONT_DIGITS96) {
        return "font";
    }
    if (w->text_align > 2) {
        return "text_align";
    }
    if (w->color > 0xFFFFFF || w->border_color > 0xFFFFFF || w->track_color > 0xFFFFFF ||
        (w->bg > 0xFFFFFF && w->bg != APPS_COLOR_NONE)) {
        return "colour";
    }
    if (w->radius < -1 || w->radius > SCREEN_R) {
        return "radius";
    }
    if (w->border > 20) {
        return "border";
    }
    if (w->thickness < 1 || w->thickness > 64) {
        return "thickness";
    }
    if (w->scale < 10 || w->scale > 400) {
        return "scale";
    }
    if (w->events > (APPS_UI_TAP | APPS_UI_LONG_PRESS | APPS_UI_PRESS | APPS_UI_RELEASE)) {
        return "events";
    }
    if (w->min < -APPS_UI_VALUE_MAX || w->min > APPS_UI_VALUE_MAX || w->max < -APPS_UI_VALUE_MAX ||
        w->max > APPS_UI_VALUE_MAX || w->value < -APPS_UI_VALUE_MAX || w->value > APPS_UI_VALUE_MAX) {
        return "range";
    }
    if (w->start_angle < 0 || w->start_angle > 360 || w->end_angle < 0 || w->end_angle > 360) {
        return "angle";
    }
    if ((w->kind == APPS_W_ARC || w->kind == APPS_W_BAR) && (w->min >= w->max || w->value < w->min || w->value > w->max)) {
        return "range";
    }

    if (w->text != NULL) {
        const unsigned char *text = (const unsigned char *)w->text;
        const unsigned char *end = memchr(text, '\0', APPS_UI_TEXT_MAX + 1);
        if (end == NULL) {
            return "text";
        }
        for (const unsigned char *c = text; c < end;) {
            if (*c < 0x80) {
                c++;
                continue;
            }
            int n = apps_json_utf8_len(c, end);
            if (n <= 0) {  /* not UTF-8, or cut short */
                return "text";
            }
            c += n;
        }
    }

    if (w->kind == APPS_W_IMAGE && !image_size(p, w, NULL, NULL)) {
        return "image";
    }
    return NULL;
}

static void reject(pass_t *p, int slot, const char *why) {
    p->rejects++;
    if (p->reject_slot < 0) {
        p->reject_slot = slot;
        p->reject_why = why;
    }
}

/* ---- Records ---- */

static bool record_fits(const node_t *n, const apps_widget_t *w) {
    return n->obj != NULL && n->kind == w->kind && n->gen == w->gen && n->up == w->parent + 1;
}

/* Forgets the record of slot and every record inside it: LVGL deletes a parent's children with it. Which are
 * inside is worked out before anything is cleared, from the records' own parents, and the walk is bounded so a
 * set of records that loops (it can't, from this code) still ends. */
static void forget(state_t *st, int slot) {
    bool inside[APPS_UI_SLOTS] = {false};
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        int cur = i;
        for (int steps = 0; steps <= APPS_UI_SLOTS && cur >= 0 && st->node[cur].obj != NULL; steps++) {
            if (cur == slot) {
                inside[i] = true;
                break;
            }
            cur = st->node[cur].up - 1;
        }
    }
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        if (inside[i]) {
            memset(&st->node[i], 0, sizeof st->node[i]);
        }
    }
}

/* ---- Making objects ---- */

static void on_touch(lv_event_t *e) {
    uint8_t bit = 0;
    switch (lv_event_get_code(e)) {
        case LV_EVENT_SHORT_CLICKED: bit = APPS_UI_TAP; break;
        case LV_EVENT_LONG_PRESSED: bit = APPS_UI_LONG_PRESS; break;
        case LV_EVENT_PRESSED: bit = APPS_UI_PRESS; break;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST: bit = APPS_UI_RELEASE; break;  /* the lift after lv_indev_wait_release */
        default: return;
    }
    apps_lv_event_cb_t cb = s_event_cb;
    if (cb != NULL) {
        uintptr_t user = (uintptr_t)lv_event_get_user_data(e);
        cb((uint16_t)(user & 0xFFFF), (uint16_t)((user >> 16) & 0xFFFF), bit);
    }
}

/* An arc turns its value to follow a finger on it. The app's value is the app's: stop the arc's own handling of a
 * press that moves (this runs before it, see LV_EVENT_PREPROCESS). That also keeps lv_arc's drag code from ever
 * running on an app's arc, and it divides by the radius: an arc with no w and h, or a side of 1 px, has a radius of 0
 * and is still pressable (its click area reaches past its box), so the second PRESSING of a drag would be a division
 * by zero, which is a CPU exception on the ESP32-S3. Don't drop this as cosmetic. */
static void on_arc_press(lv_event_t *e) {
    lv_event_stop_processing(e);
}

/* A bare object of the kind, with LVGL's own style taken off it (every value a widget shows is then set by the
 * pass, none is the theme's) and neither scrollable nor clickable. NULL when LVGL gives none. */
static lv_obj_t *new_object(uint8_t kind, lv_obj_t *parent, lv_obj_t **label) {
    lv_obj_t *obj = NULL;
    switch (kind) {
        case APPS_W_LABEL: obj = lv_label_create(parent); break;
        case APPS_W_BUTTON: obj = lv_button_create(parent); break;
        case APPS_W_BOX: obj = lv_obj_create(parent); break;
        case APPS_W_ARC: obj = lv_arc_create(parent); break;
        case APPS_W_BAR: obj = lv_bar_create(parent); break;
        case APPS_W_IMAGE: obj = lv_image_create(parent); break;
        default: break;
    }
    if (obj == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    if (kind == APPS_W_IMAGE) {
        /* In a box that is not its size an image is centred, and a scale is drawn about its middle: so a box of the
         * scaled size is exactly filled (apply_geom). That is LVGL's default for an image, said here because the
         * box depends on it. */
        lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_CENTER);
    }
    if (kind == APPS_W_ARC) {
        lv_arc_set_rotation(obj, 270);  /* 0 is 12 o'clock, as for the app */
        lv_obj_remove_style(obj, NULL, LV_PART_KNOB);  /* no knob: there is nothing on it to grab */
    } else if (kind == APPS_W_BUTTON) {
        lv_obj_t *text = lv_label_create(obj);
        if (text == NULL) {
            lv_obj_delete(obj);
            return NULL;
        }
        lv_obj_remove_style_all(text);
        lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_align(text, LV_ALIGN_CENTER, 0, 0);
        *label = text;
    }
    return obj;
}

/* The groups a new widget has to apply, whatever bits it came with. */
static uint32_t groups_of_new(const apps_widget_t *w) {
    uint32_t groups = APPS_DIRTY_GEOM | APPS_DIRTY_STYLE;
    if (w->kind == APPS_W_LABEL || w->kind == APPS_W_BUTTON) {
        groups |= APPS_DIRTY_TEXT;
    }
    if (w->kind == APPS_W_ARC || w->kind == APPS_W_BAR) {
        groups |= APPS_DIRTY_VALUE;
    }
    if (w->events != 0) {
        groups |= APPS_DIRTY_EVENTS;
    }
    return groups;
}

static made_t make_object(pass_t *p, int slot, int depth);

/* The object of the box at slot for a widget to go inside, made first when its CREATE is still pending (a box may
 * sit in a higher slot than what is in it). */
static made_t box_object(pass_t *p, int slot, int depth, lv_obj_t **out) {
    apps_widget_t *w = apps_ui_slot(p->ui, slot);
    const node_t *n = &p->st->node[slot];
    if (w == NULL || !w->alive || w->kind != APPS_W_BOX) {
        return BAD;
    }
    if ((w->dirty & APPS_DIRTY_CREATE) != 0) {
        /* Its own slot comes later in the pass (or came, and said no): a box that is bad is not made, and what is in it
         * is rejected for having no parent. */
        if (check_widget(p, slot, w) != NULL) {
            return BAD;
        }
        made_t made = make_object(p, slot, depth);
        if (made != MADE) {
            return made;
        }
    } else if (!record_fits(n, w)) {
        return BAD;
    }
    *out = n->obj;
    return MADE;
}

/*
 * Makes the object of the widget at slot, which is alive and has CREATE. REFUSED when it can't be made now (the
 * PSRAM is low, LVGL had none) and the bits are to stay; BAD when it can't be made at all. Only on MADE is CREATE
 * cleared, and the groups the widget has to apply are put in its bits, for its own visit (or the visit that is
 * under way) to apply. The caller has checked the widget, and box_object has checked a box made for what is in it.
 */
static made_t make_object(pass_t *p, int slot, int depth) {
    apps_widget_t *w = apps_ui_slot(p->ui, slot);
    node_t *n = &p->st->node[slot];
    if (w == NULL || !w->alive || (w->dirty & APPS_DIRTY_CREATE) == 0 || depth > APPS_UI_BOX_DEPTH_MAX + 1) {
        return BAD;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < APPS_LV_MIN_PSRAM) {
        return REFUSED;
    }
    if (n->obj != NULL) {
        /* An object of an app that was never reset (or a table that lost its way): it goes, with what is in it. */
        lv_obj_t *stale = n->obj;
        forget(p->st, slot);
        lv_obj_delete(stale);
        p->changed = true;
    }
    lv_obj_t *parent = s_layer;
    if (w->parent >= 0) {
        made_t made = box_object(p, w->parent, depth + 1, &parent);
        if (made != MADE) {
            return made;
        }
    }
    lv_obj_t *label = NULL;
    lv_obj_t *obj = new_object(w->kind, parent, &label);
    if (obj == NULL) {
        return REFUSED;
    }
    n->obj = obj;
    n->label = label;
    n->gen = w->gen;
    n->up = (int16_t)(w->parent + 1);
    n->kind = w->kind;
    n->subscribed = false;
    n->recolored = false;
    w->dirty = (w->dirty & ~(uint32_t)APPS_DIRTY_CREATE) | groups_of_new(w);
    p->changed = true;
    return MADE;
}

/* ---- Applying a group ---- */

/* One side of an image drawn at scale percent, to the nearest pixel and within the screen. */
static int32_t scaled_side(uint16_t side, uint16_t scale) {
    return clamp_i32((int32_t)(((uint32_t)side * scale + 50) / 100), 1, SCREEN_PX);
}

/*
 * LVGL lays an image out at its own size and draws a scale as a transform about the middle of its box, which never
 * changes the box. So for a side of a scaled image that the app left to the content, the box is made the size that is
 * drawn (new_object centres the image in it): align, x and y place what is on the screen, and the layout written back,
 * and the overflow, describe it. A side the app sized is its box, as for any widget.
 */
static void apply_geom(const pass_t *p, const apps_widget_t *w, lv_obj_t *obj) {
    int32_t width = w->w < 0 ? LV_SIZE_CONTENT : w->w, height = w->h < 0 ? LV_SIZE_CONTENT : w->h;
    uint16_t iw, ih;
    if (w->kind == APPS_W_IMAGE && w->scale != 100 && image_size(p, w, &iw, &ih)) {
        width = w->w < 0 ? scaled_side(iw, w->scale) : width;
        height = w->h < 0 ? scaled_side(ih, w->scale) : height;
    }
    lv_obj_set_size(obj, width, height);
    lv_obj_align(obj, align_of(w->align), w->x, w->y);
}

static void style_text(const apps_widget_t *w, lv_obj_t *text) {
    lv_obj_set_style_text_font(text, font_of(w->font), LV_PART_MAIN);
    lv_obj_set_style_text_color(text, colour(w->color), LV_PART_MAIN);
}

/* The surface of a box or button: its fill, corners and border. */
static void style_surface(const apps_widget_t *w, lv_obj_t *obj) {
    if (w->bg == APPS_COLOR_NONE) {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(obj, colour(w->bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    lv_obj_set_style_radius(obj, radius_of(w->radius), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, w->border, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, colour(w->border_color), LV_PART_MAIN);
}

/* A bar's track (LV_PART_MAIN) or its indicator: a filled, rounded bar. */
static void style_bar_part(lv_obj_t *obj, uint32_t rgb, int32_t radius, lv_style_selector_t part) {
    lv_obj_set_style_bg_color(obj, colour(rgb), part);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, part);
    lv_obj_set_style_radius(obj, radius, part);
}

static void apply_style(pass_t *p, const apps_widget_t *w, node_t *n) {
    lv_obj_t *obj = n->obj;
    if (w->hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_opa(obj, (lv_opa_t)((unsigned)w->opacity * 255 / 100), LV_PART_MAIN);
    switch (w->kind) {
        case APPS_W_LABEL:
            style_text(w, obj);
            lv_obj_set_style_text_align(obj, text_align_of(w->text_align), LV_PART_MAIN);
            lv_label_set_long_mode(obj, w->wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_CLIP);
            break;
        case APPS_W_BUTTON:
            style_surface(w, obj);
            style_text(w, n->label);  /* the text is the button's label's */
            break;
        case APPS_W_BOX:
            style_surface(w, obj);
            break;
        case APPS_W_ARC:
            lv_obj_set_style_arc_width(obj, w->thickness, LV_PART_MAIN);
            lv_obj_set_style_arc_color(obj, colour(w->track_color), LV_PART_MAIN);
            lv_obj_set_style_arc_width(obj, w->thickness, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(obj, colour(w->color), LV_PART_INDICATOR);
            break;
        case APPS_W_BAR:
            style_bar_part(obj, w->track_color, radius_of(w->radius), LV_PART_MAIN);
            style_bar_part(obj, w->color, radius_of(w->radius), LV_PART_INDICATOR);
            break;
        case APPS_W_IMAGE: {
            const apps_asset_t *asset = apps_ui_asset(p->ui, w->image);
            if (asset == NULL || w->image >= APPS_UI_ASSETS_MAX) {
                break;
            }
            /* One descriptor per asset: its header is the file's first bytes, the pixels are the rest, in place. */
            lv_image_dsc_t *dsc = &p->st->image[w->image];
            if (dsc->data != asset->data + IMAGE_HEADER) {
                memcpy(&dsc->header, asset->data, sizeof dsc->header);
                /* Only premultiplied alpha means anything to an image drawn in place; the others say what the memory is
                 * (ALLOCATED: an lv_draw_buf, which this isn't, so LVGL refuses the image and it never shows).
                 * apps_image_check refuses them already: this keeps out of LVGL what the header said when it was
                 * copied, which may not be what the check saw. */
                dsc->header.flags &= LV_IMAGE_FLAGS_PREMULTIPLIED;
                dsc->data_size = (uint32_t)(asset->len - IMAGE_HEADER);
                dsc->data = asset->data + IMAGE_HEADER;
                dsc->reserved = NULL;
                dsc->reserved_2 = NULL;
            }
            lv_image_set_src(obj, dsc);
            lv_image_set_scale(obj, (uint32_t)w->scale * 256 / 100);
            /* LVGL draws an A8 image as a mask filled with the image recolor, which is black unless it is set: nothing
             * on the layer's black. An app has no colour for an image, so white, in full. The other two formats have
             * colours of their own (and one that was A8 and is not now has the recolor taken off). */
            if (dsc->header.cf == LV_COLOR_FORMAT_A8) {
                lv_obj_set_style_image_recolor(obj, colour(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
                n->recolored = true;
            } else if (n->recolored) {
                lv_obj_set_style_image_recolor_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
                n->recolored = false;
            }
            break;
        }
        default:
            break;
    }
}

static void apply_text(const apps_widget_t *w, const node_t *n) {
    lv_obj_t *label = w->kind == APPS_W_BUTTON ? n->label : w->kind == APPS_W_LABEL ? n->obj : NULL;
    if (label != NULL) {
        lv_label_set_text(label, w->text != NULL ? w->text : "");
    }
}

static void apply_value(const apps_widget_t *w, lv_obj_t *obj) {
    if (w->kind == APPS_W_ARC) {
        lv_arc_set_range(obj, w->min, w->max);
        lv_arc_set_bg_angles(obj, w->start_angle, w->end_angle);
        lv_arc_set_value(obj, w->value);
    } else if (w->kind == APPS_W_BAR) {
        lv_bar_set_range(obj, w->min, w->max);
        lv_bar_set_value(obj, w->value, LV_ANIM_OFF);
    }
}

static void apply_events(int slot, const apps_widget_t *w, node_t *n) {
    if (w->events == 0) {
        lv_obj_remove_flag(n->obj, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    if (!n->subscribed) {
        /* The slot and gen the widget was made for travel with the callback: the app's side drops a touch whose
         * widget is gone, and an object can't send one once it is deleted. SHORT_CLICKED is the tap; CLICKED also
         * follows a long press. PRESS_LOST is what the lift sends instead of RELEASED once a swipe has taken the touch
         * (lv_indev_wait_release), and it is the app's release: a press is never left open. */
        void *user = (void *)(uintptr_t)((uint32_t)slot | ((uint32_t)w->gen << 16));
        lv_obj_add_event_cb(n->obj, on_touch, LV_EVENT_SHORT_CLICKED, user);
        lv_obj_add_event_cb(n->obj, on_touch, LV_EVENT_LONG_PRESSED, user);
        lv_obj_add_event_cb(n->obj, on_touch, LV_EVENT_PRESSED, user);
        lv_obj_add_event_cb(n->obj, on_touch, LV_EVENT_RELEASED, user);
        lv_obj_add_event_cb(n->obj, on_touch, LV_EVENT_PRESS_LOST, user);
        if (w->kind == APPS_W_ARC) {
            lv_obj_add_event_cb(n->obj, on_arc_press, (lv_event_code_t)(LV_EVENT_PRESSING | LV_EVENT_PREPROCESS), NULL);
        }
        n->subscribed = true;
    }
    lv_obj_add_flag(n->obj, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- The slots ---- */

/* Whether an object that is to be deleted in this pass holds the one at slot: an ancestor (by the table, which
 * keeps a deleted widget's parent for this) that is dead, waiting for its delete, and has an object. LVGL
 * deletes what is inside with it, so a call for the inner one would be on an object that is gone. */
static bool inside_pending_delete(const pass_t *p, int slot) {
    const apps_widget_t *w = apps_ui_slot(p->ui, slot);
    int up = w != NULL ? w->parent : -1;
    for (int steps = 0; up >= 0 && steps <= APPS_UI_BOX_DEPTH_MAX + 1; steps++) {
        const apps_widget_t *a = apps_ui_slot(p->ui, up);
        if (a == NULL) {
            break;
        }
        if (!a->alive && (a->dirty & APPS_DIRTY_DELETE) != 0 && p->st->node[up].obj != NULL) {
            return true;
        }
        up = a->parent;
    }
    return false;
}

/* A slot that is not alive and has bits: a delete (and its object, if it has one, goes), or bits that only a live
 * widget has (dropped, counted). */
static void visit_dead(pass_t *p, int slot, apps_widget_t *w) {
    if ((w->dirty & ~(uint32_t)APPS_DIRTY_DELETE) != 0) {
        reject(p, slot, "bits of a dead widget");
    }
    if ((w->dirty & APPS_DIRTY_DELETE) != 0) {
        node_t *n = &p->st->node[slot];
        lv_obj_t *obj = n->obj;
        if (obj != NULL) {
            bool covered = inside_pending_delete(p, slot);
            forget(p->st, slot);
            if (!covered) {
                lv_obj_delete(obj);
                p->changed = true;
            }
        }
    }
    w->dirty = 0;
}

static void visit_live(pass_t *p, int slot, apps_widget_t *w) {
    node_t *n = &p->st->node[slot];
    const char *why = check_widget(p, slot, w);
    if (why == NULL && (w->dirty & APPS_DIRTY_DELETE) != 0) {
        why = "delete of a live widget";
    }
    if (why == NULL && (w->dirty & APPS_DIRTY_CREATE) == 0 && !record_fits(n, w)) {
        why = "no object of this widget";
    }
    if (why != NULL) {
        reject(p, slot, why);
        w->dirty = 0;
        return;
    }

    if ((w->dirty & APPS_DIRTY_CREATE) != 0) {
        made_t made = make_object(p, slot, 0);
        if (made == REFUSED) {
            p->refused++;  /* the bits stay: the slot comes again */
            return;
        }
        if (made == BAD) {
            reject(p, slot, "no parent object");
            w->dirty = 0;
            return;
        }
    }
    uint32_t todo = w->dirty & GROUPS;
    if (w->kind == APPS_W_IMAGE && (todo & APPS_DIRTY_STYLE) != 0) {
        todo |= APPS_DIRTY_GEOM;  /* a src or a scale (both are STYLE) changes the size an image's box is made */
    }
    if ((todo & APPS_DIRTY_GEOM) != 0) {
        apply_geom(p, w, n->obj);
    }
    if ((todo & APPS_DIRTY_STYLE) != 0) {
        apply_style(p, w, n);
    }
    if ((todo & APPS_DIRTY_TEXT) != 0) {
        apply_text(w, n);
    }
    if ((todo & APPS_DIRTY_VALUE) != 0) {
        apply_value(w, n->obj);
    }
    if ((todo & APPS_DIRTY_EVENTS) != 0) {
        apply_events(slot, w, n);
    }
    if (todo != 0) {
        p->changed = true;
    }
    w->dirty = 0;
}

/* ---- Laying out, and what it says to the app ---- */

static bool is_hidden(const pass_t *p, const apps_widget_t *w) {
    for (int steps = 0; w != NULL && steps <= APPS_UI_BOX_DEPTH_MAX + 1; steps++) {
        if (w->hidden) {
            return true;
        }
        w = w->parent >= 0 ? apps_ui_slot(p->ui, w->parent) : NULL;
    }
    return false;
}

/* The integer square root of n, rounded down. */
static uint32_t isqrt(uint64_t n) {
    uint64_t root = 0, bit = (uint64_t)1 << 62;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/*
 * How far the widget goes past the round edge (a circle of radius SCREEN_R about (SCREEN_R, SCREEN_R)), in px,
 * to the nearest, or 0. The widget covers x1.. x1 + w, y1 .. y1 + h in continuous coordinates. A rectangle goes as
 * far as its farthest corner; the real outline is used for what is round: an arc is the circle that fits its box,
 * and a box whose radius is at least half its shorter side is a stadium (a segment along its long axis, with a
 * circle of half the shorter side round it). The farthest point of such a shape is its circle's, about the
 * segment's end that is farther from the centre: that distance and the radius, less SCREEN_R.
 *
 * All in integers, in half pixels, so a centre is a whole number: with s the farthest distance (doubled) and two_r
 * the doubled radius (0 for a corner), the overflow is floor((s + two_r - 2 * SCREEN_R + 1) / 2), which is
 * round(distance + r - SCREEN_R), halves up.
 */
static int16_t overflow_of(const pass_t *p, const apps_widget_t *w, int32_t x1, int32_t y1, int32_t rw, int32_t rh) {
    if (rw <= 0 || rh <= 0 || is_hidden(p, w)) {
        return 0;
    }
    const int64_t c = 2 * SCREEN_R;  /* the screen's centre, doubled */
    int64_t far2 = 0;                /* the farthest distance, squared, doubled */
    int32_t two_r = 0;
    int32_t half = (rw < rh ? rw : rh) / 2;  /* the most LVGL rounds a corner by */
    bool round_shape = w->kind == APPS_W_ARC || (w->kind == APPS_W_BOX && half > 0 && (w->radius < 0 || w->radius >= half));
    if (round_shape) {
        int64_t cx = 2 * (int64_t)x1 + rw, cy = 2 * (int64_t)y1 + rh;  /* the box's centre, doubled */
        int64_t reach = w->kind == APPS_W_ARC ? 0 : (rw > rh ? rw - rh : rh - rw);
        two_r = rw < rh ? rw : rh;
        for (int end = -1; end <= 1; end += 2) {
            int64_t ex = cx + (rw >= rh ? end * reach : 0), ey = cy + (rw >= rh ? 0 : end * reach);
            int64_t d2 = (ex - c) * (ex - c) + (ey - c) * (ey - c);
            far2 = d2 > far2 ? d2 : far2;
        }
    } else {
        for (int corner = 0; corner < 4; corner++) {
            int64_t ex = 2 * (int64_t)x1 + ((corner & 1) != 0 ? 2 * (int64_t)rw : 0);
            int64_t ey = 2 * (int64_t)y1 + ((corner & 2) != 0 ? 2 * (int64_t)rh : 0);
            int64_t d2 = (ex - c) * (ex - c) + (ey - c) * (ey - c);
            far2 = d2 > far2 ? d2 : far2;
        }
    }
    int64_t over = (int64_t)isqrt((uint64_t)far2) + two_r - (c - 1);
    return over <= 0 ? 0 : (int16_t)(over / 2 > INT16_MAX ? INT16_MAX : over / 2);
}

/* Lays the layer out and writes where each widget with an object is, and how far it goes over the edge, back. */
static void write_back(pass_t *p) {
    lv_obj_update_layout(s_layer);
    for (int slot = 0; slot < APPS_UI_SLOTS; slot++) {
        apps_widget_t *w = apps_ui_slot(p->ui, slot);
        const node_t *n = &p->st->node[slot];
        if (!w->alive || n->obj == NULL || n->kind != w->kind || n->gen != w->gen) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(n->obj, &area);
        int32_t x1 = clamp_i32(area.x1, -30000, 30000), y1 = clamp_i32(area.y1, -30000, 30000);
        int32_t rw = clamp_i32(area.x2 - area.x1 + 1, 0, 30000), rh = clamp_i32(area.y2 - area.y1 + 1, 0, 30000);
        w->rx = (int16_t)x1;
        w->ry = (int16_t)y1;
        w->rw = (int16_t)rw;
        w->rh = (int16_t)rh;
        w->overflow = overflow_of(p, w, x1, y1, rw, rh);
    }
}

/* ---- The pass ---- */

/* Everything the pass does, with the UI lock held. */
static void run_pass(pass_t *p) {
    if (apps_ui_take_clear(p->ui)) {
        lv_obj_clean(s_layer);
        memset(p->st->node, 0, sizeof p->st->node);
        p->changed = true;
    }
    bool bg_changed = false;
    uint32_t bg = apps_ui_take_bg(p->ui, &bg_changed);
    if (bg_changed) {
        lv_obj_set_style_bg_color(s_layer, colour(bg), LV_PART_MAIN);
    }

    for (int slot = 0; slot < APPS_UI_SLOTS; slot++) {
        apps_widget_t *w = apps_ui_slot(p->ui, slot);
        if (!w->alive && w->dirty != 0) {
            visit_dead(p, slot, w);
        }
    }
    for (int slot = 0; slot < APPS_UI_SLOTS; slot++) {
        apps_widget_t *w = apps_ui_slot(p->ui, slot);
        if (w->alive && w->dirty != 0) {
            visit_live(p, slot, w);
        }
    }
    if (p->changed) {
        write_back(p);
    }
}

/* The records, once per layer. Called without the lock: the heap is not used with it held. */
static bool make_state(void) {
    s_state = apps_port_realloc(NULL, sizeof *s_state);
    if (s_state == NULL) {
        return false;
    }
    memset(s_state, 0, sizeof *s_state);
    return true;
}

void apps_lv_set_ui(apps_ui_t *ui) {
    s_ui = ui;
}

bool apps_lv_apply(bool in_callback) {
    if (s_layer == NULL) {
        return false;
    }
    if (!in_callback) {
        s_skip_run = 0;
    } else if (s_skip_run < MAX_CALLBACK_SKIPS) {
        s_skip_run++;
        s_skips++;
        return false;
    }

    /* The table is read with the lock held. The app task clears the pointer under it before it frees the table, so
     * a table seen here stays until the unlock: a pass that holds the lock finishes first, a later one sees NULL. */
    if (!apps_port_ui_trylock(LOCK_MS)) {
        s_skips++;
        return false;
    }
    apps_ui_t *ui = s_ui;
    if (ui != NULL && s_state == NULL) {
        /* The first pass of a layer needs its records. Let go of the lock to make them, and look again: the app may
         * have ended meanwhile. */
        apps_port_ui_unlock();
        if (!make_state() || !apps_port_ui_trylock(LOCK_MS)) {
            s_skips++;
            return false;
        }
        ui = s_ui;
    }
    if (ui == NULL) {
        apps_port_ui_unlock();
        return false;
    }
    pass_t pass = {.ui = ui, .st = s_state, .reject_slot = -1};
    int64_t start = esp_timer_get_time();
    run_pass(&pass);
    int64_t end = esp_timer_get_time();
    int64_t took = end - start;
    apps_port_ui_unlock();

    s_skip_run = 0;
    s_applies++;
    s_rejects += pass.rejects;
    s_refused += pass.refused;
    if (pass.reject_slot >= 0 && !s_reject_logged) {
        s_reject_logged = true;
        ESP_LOGW(TAG, "widget %d dropped: bad %s (more are only counted)", pass.reject_slot, pass.reject_why);
    }
    if (took > SLOW_US) {
        s_slow++;
        /* Every slow pass is counted, and the log has one line a second: a console at 115200 baud that is written to
         * at the frame rate blocks the LVGL task, which makes the passes slower. The line says how many there were
         * since the last and the longest of them. */
        s_slow_unlogged++;
        s_slow_worst_us = took > s_slow_worst_us ? took : s_slow_worst_us;
        if (end >= s_slow_next_log) {
            ESP_LOGW(TAG, "apply %u us (%u slow since last)", (unsigned)s_slow_worst_us, (unsigned)s_slow_unlogged);
            s_slow_next_log = end + SLOW_LOG_US;
            s_slow_unlogged = 0;
            s_slow_worst_us = 0;
        }
    }
    return true;
}

/* ---- The layer ---- */

static void on_gesture(lv_event_t *e) {
    (void)e;
    void (*close_cb)(void) = s_close_cb;
    lv_indev_t *indev = lv_indev_active();
    if (close_cb == NULL || indev == NULL || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) {
        return;
    }
    lv_indev_wait_release(indev);  /* the rest of the touch is not for what shows under the layer */
    close_cb();
}

void apps_lv_build(lv_obj_t *screen) {
    if (screen == NULL) {
        return;
    }
    /* Records belong to the layer they were made on, and the cards to the screen. */
    apps_port_free(s_state);
    s_state = NULL;
    s_skip_run = 0;
    s_reject_logged = false;
    s_front = false;
    s_screen = screen;
    s_launcher = s_crash = s_crash_title = s_crash_body = NULL;
    s_launcher_shown = s_crash_shown = false;

    lv_obj_t *layer = lv_obj_create(screen);
    if (layer == NULL) {
        s_layer = NULL;
        return;
    }
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(layer, colour(0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);  /* a touch on no widget is the layer's, not the screen's */
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    /* A swipe bubbles up past an object to the screen unless the object stops it. */
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(layer, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    s_layer = layer;
}

void apps_lv_set_event_cb(apps_lv_event_cb_t cb) {
    s_event_cb = cb;
}

void apps_lv_set_close_cb(void (*cb)(void)) {
    s_close_cb = cb;
}

void apps_lv_reset(void) {
    if (s_layer != NULL) {
        lv_obj_clean(s_layer);
        lv_obj_set_style_bg_color(s_layer, colour(0), LV_PART_MAIN);
    }
    apps_port_free(s_state);
    s_state = NULL;
    s_skip_run = 0;
    s_reject_logged = false;
}

void apps_lv_set_front(bool front) {
    if (s_layer == NULL) {
        return;
    }
    if (front) {
        lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }
    s_front = front;
}

bool apps_lv_front(void) {
    return s_front;
}

void apps_lv_stats(apps_lv_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->applies = s_applies;
    out->skips = s_skips;
    out->rejects = s_rejects;
    out->refused_creates = s_refused;
    out->slow = s_slow;
}

/* ---- The launcher and the crash card ---- */

#define COLOR_TEXT 0xf2efff     /* muse_ui's, the menu's and the settings' */
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_DIM 0x8b84a8
#define COLOR_ROW 0x1a1530      /* the settings' cards */
#define CARD_TEXT_W 330         /* ui.SAFE: text this wide stays inside the circle near the middle */
#define ROW_W 300
#define ROW_H 64
#define ROW_GAP 12
#define ROWS_BELOW 20           /* the rows sit this much under the middle, clear of the title */
#define TITLE_MAX 96            /* "<name> stopped responding": a manifest's name is at most 24 bytes */

static lv_obj_t *new_label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb) {
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour(rgb), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

/* Full-screen and black, it takes every touch on it (labels on it aren't clickable, so theirs are its own), and a
 * swipe stops at it: the pages under it see nothing. Hidden until shown. */
static lv_obj_t *new_card(void) {
    lv_obj_t *card = lv_obj_create(s_screen);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(card, colour(0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    return card;
}

static void close_launcher(void) {
    if (s_launcher != NULL) {
        lv_obj_add_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
    }
    s_launcher_shown = false;
}

static void close_crash(void) {
    if (s_crash != NULL) {
        lv_obj_add_flag(s_crash, LV_OBJ_FLAG_HIDDEN);
    }
    s_crash_shown = false;
}

/* A tap that lands on no row: on the launcher itself, or on its text, which isn't clickable. */
static void on_launcher_tap(lv_event_t *e) {
    (void)e;
    close_launcher();
}

/* Any swipe: the row it began on gets no click (LVGL would click it at the lift, as nothing scrolled). Only a right
 * swipe closes the launcher (C12); the others do nothing. */
static void on_launcher_gesture(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_indev_wait_release(indev);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        close_launcher();
    }
}

/* The row's own copy of its id: the launcher closes first (pick may open another card), and pick only posts. Nothing
 * is deleted here, inside the row's event: the rows go when the launcher is next shown. */
static void on_row_tap(lv_event_t *e) {
    intptr_t row = (intptr_t)lv_event_get_user_data(e);
    if (row < 0 || row >= APPS_LV_LAUNCHER_ROWS || s_row_ids[row][0] == '\0') {
        return;
    }
    char id[APPS_ID_MAX + 1];
    memcpy(id, s_row_ids[row], sizeof id);
    void (*pick)(const char *id) = s_pick;
    close_launcher();
    if (pick != NULL) {
        pick(id);
    }
}

static void on_crash_tap(lv_event_t *e) {
    (void)e;
    close_crash();
}

/* Both cards, once, just above the layer: over the app, under what muse_ui made after the layer. False when there is
 * no layer, or LVGL had no memory for them (what was made goes, and the next show tries again). */
static bool make_cards(void) {
    if (s_launcher != NULL) {
        return true;
    }
    if (s_layer == NULL || s_screen == NULL) {
        return false;
    }
    lv_obj_t *launcher = new_card();
    lv_obj_t *crash = launcher != NULL ? new_card() : NULL;
    lv_obj_t *title = crash != NULL ? new_label(crash, &lv_font_montserrat_20, COLOR_TEXT) : NULL;
    lv_obj_t *body = title != NULL ? new_label(crash, &lv_font_montserrat_16, COLOR_CAPTION) : NULL;
    lv_obj_t *hint = body != NULL ? new_label(crash, &lv_font_montserrat_14, COLOR_DIM) : NULL;
    if (hint == NULL) {
        if (launcher != NULL) {
            lv_obj_delete(launcher);
        }
        if (crash != NULL) {
            lv_obj_delete(crash);
        }
        return false;
    }
    lv_obj_add_event_cb(launcher, on_launcher_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(launcher, on_launcher_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(crash, on_crash_tap, LV_EVENT_CLICKED, NULL);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(title, CARD_TEXT_W, LV_SIZE_CONTENT);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -72);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(body, CARD_TEXT_W, 80);  /* four lines of the error at most */
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 12);
    lv_label_set_text(hint, "Tap to close");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 112);
    lv_obj_move_to_index(launcher, lv_obj_get_index(s_layer) + 1);
    lv_obj_move_to_index(crash, lv_obj_get_index(s_layer) + 2);
    s_launcher = launcher;
    s_crash = crash;
    s_crash_title = title;
    s_crash_body = body;
    return true;
}

void apps_lv_show_launcher(const char *const *ids, const char *const *names, int n, void (*pick)(const char *id)) {
    if (!make_cards()) {
        return;
    }
    lv_obj_clean(s_launcher);  /* the last showing's rows */
    memset(s_row_ids, 0, sizeof s_row_ids);
    s_pick = pick;
    lv_obj_t *title = new_label(s_launcher, &lv_font_montserrat_20, COLOR_DIM);
    if (title != NULL) {
        lv_label_set_text(title, "Apps");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);
    }
    int rows = ids == NULL || n <= 0 ? 0 : n < APPS_LV_LAUNCHER_ROWS ? n : APPS_LV_LAUNCHER_ROWS;
    if (rows == 0) {
        lv_obj_t *empty = new_label(s_launcher, &lv_font_montserrat_16, COLOR_DIM);
        if (empty != NULL) {
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_size(empty, ROW_W, LV_SIZE_CONTENT);
            lv_label_set_text(empty, "No apps yet. Ask Muse to make one.");
            lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        }
    }
    /* The first row's centre, so the list is centred a little under the middle. */
    int32_t first = ROWS_BELOW - (rows * ROW_H + (rows - 1) * ROW_GAP) / 2 + ROW_H / 2;
    for (int i = 0; i < rows; i++) {
        if (ids[i] == NULL || ids[i][0] == '\0') {
            continue;
        }
        snprintf(s_row_ids[i], sizeof s_row_ids[i], "%s", ids[i]);
        lv_obj_t *row = lv_obj_create(s_launcher);
        if (row == NULL) {
            s_row_ids[i][0] = '\0';
            break;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, ROW_W, ROW_H);
        lv_obj_set_style_bg_color(row, colour(COLOR_ROW), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, ROW_H / 2, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_CENTER, 0, first + i * (ROW_H + ROW_GAP));
        lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *label = new_label(row, &lv_font_montserrat_20, COLOR_TEXT);
        if (label != NULL) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_size(label, ROW_W - 40, 24);  /* one line */
            bool named = names != NULL && names[i] != NULL && names[i][0] != '\0';
            lv_label_set_text(label, named ? names[i] : s_row_ids[i]);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        }
    }
    lv_obj_remove_flag(s_launcher, LV_OBJ_FLAG_HIDDEN);
    s_launcher_shown = true;
}

void apps_lv_show_crash(const char *name, const char *first_line) {
    if (!make_cards()) {
        return;
    }
    char title[TITLE_MAX];
    snprintf(title, sizeof title, "%s %s", name != NULL && name[0] != '\0' ? name : "The app",
             first_line != NULL ? "stopped" : "stopped responding");
    lv_label_set_text(s_crash_title, title);
    if (first_line != NULL) {
        lv_label_set_text(s_crash_body, first_line);
        lv_obj_remove_flag(s_crash_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_crash_body, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_crash, LV_OBJ_FLAG_HIDDEN);
    s_crash_shown = true;
}

void apps_lv_hide_cards(void) {
    if (s_launcher_shown) {
        close_launcher();
    }
    if (s_crash_shown) {
        close_crash();
    }
}

bool apps_lv_cards_shown(void) {
    return s_launcher_shown || s_crash_shown;
}
