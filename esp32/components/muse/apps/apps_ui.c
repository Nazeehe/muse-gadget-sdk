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

/*
 * The `ui` module and its shadow table (apps_ui.h). Every binding has the same shape: apps_vm_check, then
 * everything that can raise or allocate (reading the app's table, validating, the handle, the text block),
 * then a few lines of plain C on the table under apps_port_ui_lock, then what has to happen after the lock
 * (freeing the old text, dropping handlers, pushing results). A property table is read raw (no metamethod, so no
 * app code runs inside a binding), in a fixed order, and a set that fails validation changes nothing.
 */

#include "apps_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "apps_json.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define NORETURN __attribute__((noreturn))
#define BIT(p) (1u << (p))

_Static_assert(APPS_UI_ASSETS_MAX >= APPS_MAX_FILES, "every file of a package may be an image");
_Static_assert(APPS_RT_IMAGES_MAX <= 32, "a runtime image is a bit of the pass's report");

/* A widget's image: an asset's index, or IMAGE_RT + a runtime image's slot (apps_ui.h). */
#define IMAGE_RT APPS_UI_ASSETS_MAX
#define IMAGES_ALL (APPS_UI_ASSETS_MAX + APPS_RT_IMAGES_MAX)
_Static_assert(IMAGES_ALL <= INT16_MAX, "a widget's image is an int16_t");

#define SCREEN 466
#define SCREEN_R 233
#define SCREEN_SAFE 330
#define WIDGET_MT "muse.widget"
/* Text an app supplied, as a message shows it: cut here, on a character boundary, with "..." after the cut. */
#define SHOWN_MAX 40

/* ---- The names ---- */

static const char *const apps_ui_kinds[] = {"label", "button", "box", "arc", "bar", "image"};
_Static_assert(ARRAY_LEN(apps_ui_kinds) == APPS_W_IMAGE + 1, "a name for each kind");

static const char *const apps_ui_aligns[] = {"center",   "top",       "bottom",      "left",         "right",
                                             "top_left", "top_right", "bottom_left", "bottom_right"};
_Static_assert(ARRAY_LEN(apps_ui_aligns) == APPS_ALIGN_BOTTOM_RIGHT + 1, "a name for each alignment");

static const char *const apps_ui_fonts[] = {"sans14", "sans16", "sans20",   "sans28",
                                            "mono8",  "mono16", "digits48", "digits96"};
_Static_assert(ARRAY_LEN(apps_ui_fonts) == APPS_FONT_DIGITS96 + 1, "a name for each font");

static const char *const apps_ui_text_aligns[] = {"left", "center", "right"};

/* An event's bit is 1 << its index here. "click" is "tap" under another name. */
static const char *const apps_ui_events[] = {"tap", "long_press", "press", "release"};
_Static_assert(ARRAY_LEN(apps_ui_events) == 4, "four events fit the events mask");
#define EVENT_COUNT ARRAY_LEN(apps_ui_events)

/* What each kind takes, in the order the unknown-property message lists them. */
static const char *const apps_ui_props_label[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "text", "font", "color", "text_align", "wrap",
};
static const char *const apps_ui_props_button[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "text", "font", "color", "bg", "radius", "border",
    "border_color",
};
static const char *const apps_ui_props_box[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "bg", "radius", "border", "border_color",
};
static const char *const apps_ui_props_arc[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "min", "max", "value", "start_angle", "end_angle",
    "thickness", "color", "track_color",
};
static const char *const apps_ui_props_bar[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "min", "max", "value", "color", "track_color", "radius",
};
static const char *const apps_ui_props_image[] = {
    "align", "x", "y", "w", "h", "hidden", "opacity", "parent", "src", "scale", "rotation", "spin",
};

static const struct {
    const char *const *names;
    int n;
} kind_props[] = {
    [APPS_W_LABEL] = {apps_ui_props_label, ARRAY_LEN(apps_ui_props_label)},
    [APPS_W_BUTTON] = {apps_ui_props_button, ARRAY_LEN(apps_ui_props_button)},
    [APPS_W_BOX] = {apps_ui_props_box, ARRAY_LEN(apps_ui_props_box)},
    [APPS_W_ARC] = {apps_ui_props_arc, ARRAY_LEN(apps_ui_props_arc)},
    [APPS_W_BAR] = {apps_ui_props_bar, ARRAY_LEN(apps_ui_props_bar)},
    [APPS_W_IMAGE] = {apps_ui_props_image, ARRAY_LEN(apps_ui_props_image)},
};

/* ---- The properties ---- */

typedef enum {
    P_ALIGN, P_X, P_Y, P_W, P_H, P_HIDDEN, P_OPACITY, P_PARENT,
    P_TEXT, P_FONT, P_COLOR, P_TEXT_ALIGN, P_WRAP,
    P_BG, P_RADIUS, P_BORDER, P_BORDER_COLOR,
    P_MIN, P_MAX, P_VALUE, P_START_ANGLE, P_END_ANGLE, P_THICKNESS, P_TRACK_COLOR,
    P_SRC, P_SCALE, P_ROTATION, P_SPIN,
    P_COUNT
} prop_t;
_Static_assert(P_COUNT <= 32, "a property is a bit of change_t's set");

static const char *const prop_names[P_COUNT] = {
    [P_ALIGN] = "align", [P_X] = "x", [P_Y] = "y", [P_W] = "w", [P_H] = "h", [P_HIDDEN] = "hidden",
    [P_OPACITY] = "opacity", [P_PARENT] = "parent", [P_TEXT] = "text", [P_FONT] = "font", [P_COLOR] = "color",
    [P_TEXT_ALIGN] = "text_align", [P_WRAP] = "wrap", [P_BG] = "bg", [P_RADIUS] = "radius", [P_BORDER] = "border",
    [P_BORDER_COLOR] = "border_color", [P_MIN] = "min", [P_MAX] = "max", [P_VALUE] = "value",
    [P_START_ANGLE] = "start_angle", [P_END_ANGLE] = "end_angle", [P_THICKNESS] = "thickness",
    [P_TRACK_COLOR] = "track_color", [P_SRC] = "src", [P_SCALE] = "scale", [P_ROTATION] = "rotation", [P_SPIN] = "spin",
};

typedef enum { T_NUM, T_BOOL, T_ENUM, T_COLOR, T_BG, T_TEXT, T_RADIUS, T_PARENT, T_SRC } ptype_t;

typedef struct {
    ptype_t type;
    uint32_t dirty;  /* the group of dirty bits a change lands in (apps_ui.h) */
    int32_t lo, hi;  /* T_NUM, T_RADIUS (the numbers' part) */
    const char *const *names;  /* T_ENUM */
    int n_names;
    const char *unit;  /* T_NUM: what a number of it counts, which a message of the wrong type names with the range; NULL: none */
} prop_info_t;

static const prop_info_t prop_info[P_COUNT] = {
    [P_ALIGN] = {T_ENUM, APPS_DIRTY_GEOM, 0, 0, apps_ui_aligns, ARRAY_LEN(apps_ui_aligns), NULL},
    [P_X] = {T_NUM, APPS_DIRTY_GEOM, -SCREEN, SCREEN, NULL, 0, NULL},
    [P_Y] = {T_NUM, APPS_DIRTY_GEOM, -SCREEN, SCREEN, NULL, 0, NULL},
    [P_W] = {T_NUM, APPS_DIRTY_GEOM, 1, SCREEN, NULL, 0, NULL},
    [P_H] = {T_NUM, APPS_DIRTY_GEOM, 1, SCREEN, NULL, 0, NULL},
    [P_HIDDEN] = {T_BOOL, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_OPACITY] = {T_NUM, APPS_DIRTY_STYLE, 0, 100, NULL, 0, NULL},
    [P_PARENT] = {T_PARENT, 0, 0, 0, NULL, 0, NULL},
    [P_TEXT] = {T_TEXT, APPS_DIRTY_TEXT, 0, 0, NULL, 0, NULL},
    [P_FONT] = {T_ENUM, APPS_DIRTY_STYLE, 0, 0, apps_ui_fonts, ARRAY_LEN(apps_ui_fonts), NULL},
    [P_COLOR] = {T_COLOR, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_TEXT_ALIGN] = {T_ENUM, APPS_DIRTY_STYLE, 0, 0, apps_ui_text_aligns, ARRAY_LEN(apps_ui_text_aligns), NULL},
    [P_WRAP] = {T_BOOL, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_BG] = {T_BG, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_RADIUS] = {T_RADIUS, APPS_DIRTY_STYLE, 0, SCREEN_R, NULL, 0, NULL},
    [P_BORDER] = {T_NUM, APPS_DIRTY_STYLE, 0, 20, NULL, 0, NULL},
    [P_BORDER_COLOR] = {T_COLOR, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_MIN] = {T_NUM, APPS_DIRTY_VALUE, -APPS_UI_VALUE_MAX, APPS_UI_VALUE_MAX, NULL, 0, NULL},
    [P_MAX] = {T_NUM, APPS_DIRTY_VALUE, -APPS_UI_VALUE_MAX, APPS_UI_VALUE_MAX, NULL, 0, NULL},
    [P_VALUE] = {T_NUM, APPS_DIRTY_VALUE, -APPS_UI_VALUE_MAX, APPS_UI_VALUE_MAX, NULL, 0, NULL},
    [P_START_ANGLE] = {T_NUM, APPS_DIRTY_VALUE, 0, 360, NULL, 0, NULL},
    [P_END_ANGLE] = {T_NUM, APPS_DIRTY_VALUE, 0, 360, NULL, 0, NULL},
    [P_THICKNESS] = {T_NUM, APPS_DIRTY_STYLE, 1, 64, NULL, 0, NULL},
    [P_TRACK_COLOR] = {T_COLOR, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_SRC] = {T_SRC, APPS_DIRTY_STYLE, 0, 0, NULL, 0, NULL},
    [P_SCALE] = {T_NUM, APPS_DIRTY_STYLE, 10, 400, NULL, 0, NULL},
    [P_ROTATION] = {T_NUM, APPS_DIRTY_STYLE, 0, 359, NULL, 0, "degrees"},
    [P_SPIN] = {T_NUM, APPS_DIRTY_STYLE, 0, APPS_SPIN_RPM_MAX, NULL, 0, "turns a minute"},
};

/* ---- The table ---- */

struct apps_ui {
    apps_widget_t w[APPS_UI_SLOTS];
    int count;  /* alive widgets */
    uint32_t bg;
    bool bg_changed, clear_pending;
    apps_asset_t assets[APPS_UI_ASSETS_MAX];
    int n_assets;
    /* The runtime images by slot (apps_rt.h: the runtime sets them under the lock), data NULL for none; each one's life, as its
     * handles carry it; and the apply step's report, a bit a slot: the ones the layer's objects draw from. */
    apps_asset_t images[APPS_RT_IMAGES_MAX];
    uint32_t image_gen[APPS_RT_IMAGES_MAX];
    uint32_t drawn;
    char handle_key;  /* its address is the registry's key of the table that remembers the app's handles */
    char image_key;   /* and this one's, of the table that remembers the runtime images' handles */
};

/* What a handle holds. A slot that is gone, or whose gen has moved on, is a deleted widget. */
typedef struct {
    uint16_t slot, gen;
    uint8_t kind;
} handle_t;

#define NO_SLOT 0xFFFFu

/* The value a property is read as, for the properties of a widget. */
static int64_t fetch(const apps_widget_t *w, prop_t p) {
    switch (p) {
        case P_ALIGN: return w->align;
        case P_X: return w->x;
        case P_Y: return w->y;
        case P_W: return w->w;
        case P_H: return w->h;
        case P_HIDDEN: return w->hidden;
        case P_OPACITY: return w->opacity;
        case P_PARENT: return w->parent;
        case P_FONT: return w->font;
        case P_COLOR: return w->color;
        case P_TEXT_ALIGN: return w->text_align;
        case P_WRAP: return w->wrap;
        case P_BG: return w->bg;
        case P_RADIUS: return w->radius;
        case P_BORDER: return w->border;
        case P_BORDER_COLOR: return w->border_color;
        case P_MIN: return w->min;
        case P_MAX: return w->max;
        case P_VALUE: return w->value;
        case P_START_ANGLE: return w->start_angle;
        case P_END_ANGLE: return w->end_angle;
        case P_THICKNESS: return w->thickness;
        case P_TRACK_COLOR: return w->track_color;
        case P_SRC: return w->image;
        case P_SCALE: return w->scale;
        case P_ROTATION: return w->rotation;
        case P_SPIN: return w->spin;
        case P_TEXT:
        case P_COUNT: break;
    }
    return 0;
}

/* v is in the property's range: the readers made sure. */
static void store(apps_widget_t *w, prop_t p, int64_t v) {
    switch (p) {
        case P_ALIGN: w->align = (uint8_t)v; break;
        case P_X: w->x = (int16_t)v; break;
        case P_Y: w->y = (int16_t)v; break;
        case P_W: w->w = (int16_t)v; break;
        case P_H: w->h = (int16_t)v; break;
        case P_HIDDEN: w->hidden = v != 0; break;
        case P_OPACITY: w->opacity = (uint8_t)v; break;
        case P_PARENT: w->parent = (int16_t)v; break;
        case P_FONT: w->font = (uint8_t)v; break;
        case P_COLOR: w->color = (uint32_t)v; break;
        case P_TEXT_ALIGN: w->text_align = (uint8_t)v; break;
        case P_WRAP: w->wrap = v != 0; break;
        case P_BG: w->bg = (uint32_t)v; break;
        case P_RADIUS: w->radius = (int16_t)v; break;
        case P_BORDER: w->border = (uint8_t)v; break;
        case P_BORDER_COLOR: w->border_color = (uint32_t)v; break;
        case P_MIN: w->min = (int32_t)v; break;
        case P_MAX: w->max = (int32_t)v; break;
        case P_VALUE: w->value = (int32_t)v; break;
        case P_START_ANGLE: w->start_angle = (int16_t)v; break;
        case P_END_ANGLE: w->end_angle = (int16_t)v; break;
        case P_THICKNESS: w->thickness = (uint8_t)v; break;
        case P_TRACK_COLOR: w->track_color = (uint32_t)v; break;
        case P_SRC: w->image = (int16_t)v; break;
        case P_SCALE: w->scale = (uint16_t)v; break;
        case P_ROTATION: w->rotation = (int16_t)v; break;
        case P_SPIN: w->spin = (uint8_t)v; break;
        case P_TEXT:
        case P_COUNT: break;
    }
}

/* A new widget: every field has a value that is in range for any kind, so a table never holds a slot that the
 * apply step has to guess about. */
static void init_widget(apps_widget_t *w, int kind) {
    w->kind = (uint8_t)kind;
    w->parent = -1;
    w->w = w->h = -1;
    w->align = APPS_ALIGN_CENTER;
    w->font = APPS_FONT_SANS20;
    w->opacity = 100;
    w->text_align = 1;
    w->color = 0xFFFFFF;
    w->bg = APPS_COLOR_NONE;
    w->border_color = 0xFFFFFF;
    w->track_color = 0x262626;
    w->thickness = 12;
    w->min = 0;
    w->max = 100;
    w->end_angle = 360;
    w->image = -1;
    w->scale = 100;
    if (kind == APPS_W_BUTTON) {
        w->bg = 0x262626;
        w->radius = 16;
    }
}

/* The bits a widget has when it is made: all it needs to be applied from nothing. */
static uint32_t created_dirty(int kind) {
    uint32_t dirty = APPS_DIRTY_CREATE | APPS_DIRTY_GEOM | APPS_DIRTY_STYLE;
    if (kind == APPS_W_LABEL || kind == APPS_W_BUTTON) {
        dirty |= APPS_DIRTY_TEXT;
    }
    if (kind == APPS_W_ARC || kind == APPS_W_BAR) {
        dirty |= APPS_DIRTY_VALUE;
    }
    return dirty;
}

/* ---- Raising ---- */

/* Raises "<position><message>". */
static NORETURN void fail(lua_State *L, const char *fmt, ...) {
    va_list ap;
    luaL_where(L, 1);
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    lua_concat(L, 2);
    lua_error(L);
    __builtin_unreachable();
}

/* Raises "<position>ui.<kind>: <message>". */
static NORETURN void fail_kind(lua_State *L, int kind, const char *fmt, ...) {
    va_list ap;
    luaL_where(L, 1);
    lua_pushfstring(L, "ui.%s: ", apps_ui_kinds[kind]);
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    lua_concat(L, 3);
    lua_error(L);
    __builtin_unreachable();
}

typedef struct {
    char s[SHOWN_MAX + 4];
} shown_t;

/* s[0, len), which the app supplied, as a message may show it: valid UTF-8 as it is, a control byte or anything
 * else as '?', and cut at SHOWN_MAX bytes (a character is never split) with "..." after the cut. */
static const char *show(shown_t *out, const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    size_t used = 0;
    while (p < end) {
        size_t n = 1;
        bool keep = *p >= 0x20 && *p != 0x7F;
        if (*p >= 0x80) {
            int k = apps_json_utf8_len(p, end);
            keep = k > 0;
            n = keep ? (size_t)k : 1;
        }
        size_t take = keep ? n : 1;
        if (used + take > SHOWN_MAX) {
            memcpy(out->s + used, "...", 3);
            used += 3;
            break;
        }
        if (keep) {
            memcpy(out->s + used, p, n);
        } else {
            out->s[used] = '?';
        }
        used += take;
        p += n;
    }
    out->s[used] = '\0';
    return out->s;
}

/* "a, b, c" into out. The lists are short: what doesn't fit is dropped, whole. */
static void join_names(char *out, size_t cap, const char *const *names, int n) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t need = strlen(names[i]) + (i > 0 ? 2 : 0);
        if (used + need >= cap) {
            break;
        }
        used += (size_t)snprintf(out + used, cap - used, "%s%s", i > 0 ? ", " : "", names[i]);
    }
}

/* ---- Handles ---- */

static apps_ui_t *ui_of(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

static handle_t *push_handle(lua_State *L, int slot, uint16_t gen, int kind) {
    handle_t *h = lua_newuserdatauv(L, sizeof *h, 0);
    h->slot = (uint16_t)slot;
    h->gen = gen;
    h->kind = (uint8_t)kind;
    luaL_setmetatable(L, WIDGET_MT);
    return h;
}

/*
 * Handles are remembered, so that a widget is one object while the app holds its handle (a table can be keyed by
 * it, and rawequal holds): a registry table with a place for each slot, and weak values, so a handle the app has
 * let go of is not kept. A place holds the handle of that slot's current life, which is looked at before it is
 * used (slot, gen and kind). The table is made with all its places, so storing a handle in it can't allocate:
 * create stores its handle after the widget exists, where nothing may raise.
 */
static void push_widget(lua_State *L, apps_ui_t *ui, int slot, uint16_t gen, int kind) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &ui->handle_key) != LUA_TTABLE) {  /* never: ui is installed after it */
        lua_pop(L, 1);
        push_handle(L, slot, gen, kind);
        return;
    }
    if (lua_rawgeti(L, -1, slot + 1) == LUA_TUSERDATA) {
        const handle_t *known = luaL_testudata(L, -1, WIDGET_MT);
        if (known != NULL && known->slot == slot && known->gen == gen && known->kind == kind) {
            lua_remove(L, -2);
            return;
        }
    }
    lua_pop(L, 1);
    push_handle(L, slot, gen, kind);  /* may raise: nothing is held */
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, slot + 1);
    lua_remove(L, -2);
}

/* The widget h names, or NULL when it is gone. A gen has 16 bits and comes round again after 65536 deletes of one
 * slot, so a handle an app kept can match a later widget of that slot: the kind has to match as well (it never
 * changes from create to delete), or the handle would write one kind's state into another's. */
static apps_widget_t *widget_of(apps_ui_t *ui, const handle_t *h) {
    if (h->slot >= APPS_UI_SLOTS) {
        return NULL;
    }
    apps_widget_t *w = &ui->w[h->slot];
    return w->alive && w->gen == h->gen && w->kind == h->kind ? w : NULL;
}

static apps_widget_t *live_widget(lua_State *L, apps_ui_t *ui, const handle_t *h) {
    apps_widget_t *w = widget_of(ui, h);
    if (w == NULL) {
        fail(L, "widget was deleted");
    }
    return w;
}

/* The handle at arg 1 of a method. */
static handle_t *self_handle(lua_State *L, const char *method) {
    handle_t *h = luaL_testudata(L, 1, WIDGET_MT);
    if (h == NULL) {
        fail(L, "ui: %s needs a widget: call it as w:%s(...)", method, method);
    }
    return h;
}

/* ---- Reading the app's properties ---- */

/* What a create or a set asks for, validated and not yet applied. */
typedef struct {
    uint32_t set;  /* BIT(p) for each property given */
    int64_t v[P_COUNT];
    char text[APPS_UI_TEXT_MAX + 1];
    size_t text_len;
} change_t;

typedef struct {
    lua_State *L;
    apps_ui_t *ui;
    int kind;
    bool creating;
    const apps_widget_t *cur;  /* the widget a set changes; NULL for a create */
} ctx_t;

/* The property of kind that s[0, len) names, or -1. */
static int prop_of(int kind, const char *s, size_t len) {
    for (int i = 0; i < kind_props[kind].n; i++) {
        const char *name = kind_props[kind].names[i];
        if (strlen(name) == len && memcmp(name, s, len) == 0) {
            for (int p = 0; p < P_COUNT; p++) {
                if (strcmp(prop_names[p], name) == 0) {
                    return p;
                }
            }
        }
    }
    return -1;
}

/* Raises unknown property for the name at idx, whatever its type. */
static NORETURN void unknown_property(lua_State *L, int kind, int idx) {
    char list[192];
    join_names(list, sizeof list, kind_props[kind].names, kind_props[kind].n);
    shown_t shown;
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            size_t len;
            const char *s = lua_tolstring(L, idx, &len);
            show(&shown, s, len);
            break;
        }
        case LUA_TNUMBER: {  /* a copy on the stack, so converting it in place is harmless */
            lua_pushvalue(L, idx);
            size_t len;
            const char *s = lua_tolstring(L, -1, &len);
            show(&shown, s, len);
            break;
        }
        default: snprintf(shown.s, sizeof shown.s, "<%s>", luaL_typename(L, idx)); break;
    }
    fail_kind(L, kind, "unknown property '%s' (valid: %s)", shown.s, list);
}

static NORETURN void wrong_type(const ctx_t *x, prop_t p, const char *want, int idx) {
    fail_kind(x->L, x->kind, "'%s' must be %s, got %s", prop_names[p], want, luaL_typename(x->L, idx));
}

/* A number of a property with a unit says its range when it is the wrong type or NaN: "'spin' must be a number of
 * turns a minute, 0 to 60, got string". */
static NORETURN void not_a_number(const ctx_t *x, prop_t p, const char *got) {
    const prop_info_t *info = &prop_info[p];
    fail_kind(x->L, x->kind, "'%s' must be a number of %s, %d to %d, got %s", prop_names[p], info->unit, (int)info->lo,
              (int)info->hi, got);
}

static void read_number(const ctx_t *x, prop_t p, int idx, change_t *c) {
    if (lua_type(x->L, idx) != LUA_TNUMBER) {
        if (prop_info[p].unit != NULL) {
            not_a_number(x, p, luaL_typename(x->L, idx));
        }
        wrong_type(x, p, "a number", idx);
    }
    double d = (double)lua_tonumber(x->L, idx);
    if (isnan(d)) {
        if (prop_info[p].unit != NULL) {
            not_a_number(x, p, "NaN");
        }
        fail_kind(x->L, x->kind, "'%s' must be a number, got NaN", prop_names[p]);
    }
    /* Rounded and clamped as doubles: an int16 or int32 made of a NaN or an out of range double is undefined. */
    d = round(d);
    if (d < prop_info[p].lo) {
        d = prop_info[p].lo;
    } else if (d > prop_info[p].hi) {
        d = prop_info[p].hi;
    }
    c->v[p] = (int64_t)d;
}

static void read_bool(const ctx_t *x, prop_t p, int idx, change_t *c) {
    if (lua_type(x->L, idx) != LUA_TBOOLEAN) {
        wrong_type(x, p, "a boolean", idx);
    }
    c->v[p] = lua_toboolean(x->L, idx) ? 1 : 0;
}

static void read_enum(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    const prop_info_t *info = &prop_info[p];
    int type = lua_type(L, idx);
    shown_t shown;
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);  /* a number becomes its text: it is not one of the names either */
        for (int i = 0; i < info->n_names; i++) {
            if (strlen(info->names[i]) == len && memcmp(info->names[i], s, len) == 0) {
                c->v[p] = i;
                return;
            }
        }
        show(&shown, s, len);
    } else {  /* a boolean, a table ...: shown by its type, as unknown_property shows a key that isn't text */
        snprintf(shown.s, sizeof shown.s, "<%s>", luaL_typename(L, idx));
    }
    char list[128];
    join_names(list, sizeof list, info->names, info->n_names);
    fail_kind(L, x->kind, "%s '%s' is not one of %s", prop_names[p], shown.s, list);
}

static int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* "#rgb" or "#rrggbb" as 0xRRGGBB. */
static bool colour_from_text(const char *s, size_t len, uint32_t *out) {
    if ((len != 4 && len != 7) || s[0] != '#') {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 1; i < len; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) {
            return false;
        }
        v = v << 4 | (uint32_t)d;
        if (len == 4) {
            v = v << 4 | (uint32_t)d;  /* "#f80" is "#ff8800" */
        }
    }
    *out = v;
    return true;
}

/* The colour at idx, a string or a number (a copy on the stack: a number is converted in place for the message).
 * False when it isn't one, with its text in *shown. */
static bool colour_at(lua_State *L, int idx, bool allow_none, uint32_t *out, shown_t *shown) {
    int type = lua_type(L, idx);
    if (type == LUA_TNUMBER) {  /* 0xRRGGBB: an integer, or a float with an integer value */
        int isnum;
        lua_Integer v = lua_tointegerx(L, idx, &isnum);
        if (isnum && v >= 0 && v <= 0xFFFFFF) {
            *out = (uint32_t)v;
            return true;
        }
    }
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);
    if (type == LUA_TSTRING) {
        if (allow_none && len == 4 && memcmp(s, "none", 4) == 0) {
            *out = APPS_COLOR_NONE;
            return true;
        }
        if (colour_from_text(s, len, out)) {
            return true;
        }
    }
    show(shown, s, len);
    return false;
}

static const char colour_hint[] = "(use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)";

static void read_colour(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    int type = lua_type(L, idx);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
        fail_kind(L, x->kind, "%s must be a colour string or number %s, got %s", prop_names[p], colour_hint,
                  luaL_typename(L, idx));
    }
    uint32_t v;
    shown_t shown;
    if (!colour_at(L, idx, prop_info[p].type == T_BG, &v, &shown)) {
        fail_kind(L, x->kind, "%s '%s' is not a colour %s", prop_names[p], shown.s, colour_hint);
    }
    c->v[p] = v;
}

/* Valid text, in c->text. */
static void read_text(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    int type = lua_type(L, idx);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
        wrong_type(x, p, "a string", idx);
    }
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);  /* a number as tostring writes it */
    if (len > APPS_UI_TEXT_MAX) {
        fail_kind(L, x->kind, "text is longer than %d bytes", APPS_UI_TEXT_MAX);
    }
    if (memchr(s, '\0', len) != NULL) {
        fail_kind(L, x->kind, "text can't contain a NUL byte");
    }
    const unsigned char *q = (const unsigned char *)s, *end = q + len;
    while (q < end) {
        if (*q < 0x80) {
            q++;
            continue;
        }
        int n = apps_json_utf8_len(q, end);
        if (n <= 0) {
            fail_kind(L, x->kind, "text is not valid UTF-8");
        }
        q += n;
    }
    memcpy(c->text, s, len);
    c->text[len] = '\0';
    c->text_len = len;
}

static void read_radius(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    int type = lua_type(L, idx);
    if (type == LUA_TNUMBER) {
        read_number(x, p, idx, c);
        return;
    }
    if (type != LUA_TSTRING) {
        fail_kind(L, x->kind, "'radius' must be a number or \"circle\", got %s", luaL_typename(L, idx));
    }
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);
    if (len == 6 && memcmp(s, "circle", 6) == 0) {
        c->v[p] = -1;
        return;
    }
    shown_t shown;
    fail_kind(L, x->kind, "radius '%s' is not a number or \"circle\"", show(&shown, s, len));
}

/* How many box ancestors a widget made in the box at slot would have: the box and what is above it. */
static int ancestors_of(const apps_ui_t *ui, int slot) {
    int n = 0;
    for (int p = slot; p >= 0 && n <= APPS_UI_SLOTS; p = ui->w[p].parent) {
        n++;
    }
    return n;
}

static void read_parent(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    if (!x->creating) {
        fail_kind(L, x->kind, "parent can only be set when the widget is created");
    }
    const handle_t *h = luaL_testudata(L, idx, WIDGET_MT);
    if (h == NULL || h->kind != APPS_W_BOX) {
        fail_kind(L, x->kind, "parent must be a ui.box");
    }
    const apps_widget_t *parent = widget_of(x->ui, h);
    if (parent == NULL) {
        fail_kind(L, x->kind, "parent was deleted");
    }
    /* Boxes nest at most APPS_UI_BOX_DEPTH_MAX deep (R9.5, and the message says it): a box counts itself, so it may have three
     * box ancestors and a fifth one is refused; a widget that isn't a box may sit in the fourth, with four above it. */
    if (ancestors_of(x->ui, h->slot) + (x->kind == APPS_W_BOX) > APPS_UI_BOX_DEPTH_MAX) {
        fail_kind(L, x->kind, "boxes nest at most %d deep", APPS_UI_BOX_DEPTH_MAX);
    }
    c->v[p] = h->slot;
}

static void read_src(const ctx_t *x, prop_t p, int idx, change_t *c) {
    lua_State *L = x->L;
    const apps_image_handle_t *img = luaL_testudata(L, idx, APPS_IMAGE_MT);
    if (img != NULL) {  /* a runtime image (apps_rt.h): one that hasn't been freed, and is still this handle's */
        if (img->slot >= APPS_RT_IMAGES_MAX || x->ui->images[img->slot].data == NULL || x->ui->image_gen[img->slot] != img->gen) {
            fail_kind(L, x->kind, "src image#%d was freed", (int)img->slot);
        }
        c->v[p] = IMAGE_RT + img->slot;
        return;
    }
    if (lua_type(L, idx) != LUA_TSTRING) {
        wrong_type(x, p, "a string or an image", idx);
    }
    size_t len;
    const char *s = lua_tolstring(L, idx, &len);
    for (int i = 0; i < x->ui->n_assets; i++) {
        const char *path = x->ui->assets[i].path;
        if (strlen(path) == len && memcmp(path, s, len) == 0) {
            c->v[p] = i;
            return;
        }
    }
    shown_t shown;
    fail_kind(L, x->kind, "src '%s' is not one of this app's images (manifest.json \"files\")", show(&shown, s, len));
}

/* ---- The spin cap (connected apps Task 1: R1, R1a; Task 7: R1b) ----
 * The board's spin gate drew a turned picture at about 1 us a pixel each step: 300 and 400 px didn't hold the frame rate, and
 * taps waited behind them. Task 7 found Task 1's 200 px pass was a scaled picture LVGL drew less of; a whole 200 px record,
 * drawn as its own square, takes 34 ms a frame, and 180 px (32,400 px) holds 30 a second. So a spinning picture is at most
 * APPS_SPIN_SIDE_MAX a side as drawn, and the pictures that spin at once and show cover at most APPS_SPIN_AREA_MAX together.
 * The checks run on the app task, the table's only writer, so they read it without the lock. */

_Static_assert(APPS_SPIN_AREA_MAX == APPS_SPIN_SIDE_MAX * APPS_SPIN_SIDE_MAX, "the total's message names it as one picture's sides");

/* One side of a picture drawn at scale percent, to the nearest px, as apps_lv.c's scaled_side has it (without its screen clamp). */
static uint32_t drawn_side(uint16_t side, int64_t scale) {
    return ((uint32_t)side * (uint32_t)scale + 50) / 100;
}

/* The size of the picture src names, from its header: a package image's, or a runtime image's (P9); 0 x 0 when the table can't size
 * it (no image of the package and no runtime image, or one with no size). */
static void picture_size(const apps_ui_t *ui, int64_t src, uint16_t *w, uint16_t *h) {
    const apps_asset_t *image = src >= 0 && src < IMAGES_ALL ? apps_ui_asset(ui, (int)src) : NULL;
    *w = image != NULL ? image->w : 0;
    *h = image != NULL ? image->h : 0;
}

/* Raises unless a picture of w x h px at scale percent may spin: sized (a side of 0 is one the table couldn't size, so this fails
 * closed) and both sides as drawn at most APPS_SPIN_SIDE_MAX. Every image that may spin comes through here, whatever its src. */
static void check_spin_side(lua_State *L, int kind, uint16_t w, uint16_t h, uint16_t scale) {
    if (w == 0 || h == 0) {
        fail_kind(L, kind, "spin: this image's size is unknown");
    }
    uint32_t drawn_w = drawn_side(w, scale);
    uint32_t drawn_h = drawn_side(h, scale);
    if (drawn_w > APPS_SPIN_SIDE_MAX || drawn_h > APPS_SPIN_SIDE_MAX) {
        fail_kind(L, kind, "spin: an image may spin only up to %d x %d px on screen (this one is %d x %d)", APPS_SPIN_SIDE_MAX,
                  APPS_SPIN_SIDE_MAX, (int)drawn_w, (int)drawn_h);
    }
}

/* A create or a set not yet made, as the spin total counts it: c's values over those of the widget at slot `at`, or (at -1) the
 * widget of kind being made. */
typedef struct {
    int at;
    int kind;
    const change_t *c;
} pending_t;

/* Property p of the widget at slot i, with pend's change over it (pend NULL: as the table has it). */
static int64_t value_of(const apps_ui_t *ui, int i, prop_t p, const pending_t *pend) {
    if (pend != NULL && i == pend->at && (pend->c->set & BIT(p)) != 0) {
        return pend->c->v[p];
    }
    return fetch(&ui->w[i], p);
}

/* Whether a widget hidden or not, in the box at slot parent (-1: the layer), shows: neither it nor a box it is in is hidden. A
 * parent chain is at most APPS_UI_BOX_DEPTH_MAX boxes, and the walk stops there whatever the table says. */
static bool shows(const apps_ui_t *ui, bool hidden, int64_t parent, const pending_t *pend) {
    for (int steps = 0; !hidden && parent >= 0 && parent < APPS_UI_SLOTS && steps <= APPS_UI_BOX_DEPTH_MAX; steps++) {
        hidden = value_of(ui, (int)parent, P_HIDDEN, pend) != 0;
        parent = ui->w[parent].parent;
    }
    return !hidden;
}

/* The px a spinning picture covers as drawn; more than APPS_SPIN_AREA_MAX for one that can't be sized (closed: it can't be
 * spinning, since check_spin_side refused its spin). */
static uint64_t spin_area(const apps_ui_t *ui, int64_t src, int64_t scale) {
    uint16_t w, h;
    picture_size(ui, src, &w, &h);
    return w == 0 || h == 0 ? APPS_SPIN_AREA_MAX + 1u : (uint64_t)drawn_side(w, scale) * drawn_side(h, scale);
}

/* The px the images that spin and show cover together, with pend (NULL: none) counted as made. */
static uint64_t spin_total(const apps_ui_t *ui, const pending_t *pend) {
    uint64_t total = 0;
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = &ui->w[i];
        if (w->alive && w->kind == APPS_W_IMAGE && value_of(ui, i, P_SPIN, pend) != 0 &&
            shows(ui, value_of(ui, i, P_HIDDEN, pend) != 0, w->parent, pend)) {
            total += spin_area(ui, value_of(ui, i, P_SRC, pend), value_of(ui, i, P_SCALE, pend));
        }
    }
    if (pend != NULL && pend->at < 0 && pend->kind == APPS_W_IMAGE) {  /* the image being made, with its defaults */
        const change_t *c = pend->c;
        bool hidden = (c->set & BIT(P_HIDDEN)) != 0 && c->v[P_HIDDEN] != 0;
        int64_t parent = (c->set & BIT(P_PARENT)) != 0 ? c->v[P_PARENT] : -1;
        int64_t scale = (c->set & BIT(P_SCALE)) != 0 ? c->v[P_SCALE] : 100;
        if ((c->set & BIT(P_SPIN)) != 0 && c->v[P_SPIN] != 0 && shows(ui, hidden, parent, pend)) {
            total += spin_area(ui, c->v[P_SRC], scale);
        }
    }
    return total;
}

/* Raises when the create or set c, on the widget x->cur (NULL: one being made), would leave a picture spinning past the cap: one
 * that is too large itself (a spin given, or a scale or src while it spins), or the pictures that spin at once and show covering
 * more than APPS_SPIN_AREA_MAX together where they covered less (a spin made or given, a scale or src while it spins, or a hidden
 * spinner, or a box with one in it, shown). spin = 0, a smaller picture and a hide never fail. */
static void check_spin(const ctx_t *x, const change_t *c) {
    if (x->kind == APPS_W_IMAGE) {
        int64_t spin = (c->set & BIT(P_SPIN)) != 0 ? c->v[P_SPIN] : (x->cur != NULL ? x->cur->spin : 0);
        int64_t src = (c->set & BIT(P_SRC)) != 0 ? c->v[P_SRC] : (x->cur != NULL ? x->cur->image : -1);
        int64_t scale = (c->set & BIT(P_SCALE)) != 0 ? c->v[P_SCALE] : (x->cur != NULL ? x->cur->scale : 100);
        if (spin != 0 && (c->set & (BIT(P_SPIN) | BIT(P_SCALE) | BIT(P_SRC))) != 0) {
            uint16_t w, h;
            picture_size(x->ui, src, &w, &h);
            check_spin_side(x->L, x->kind, w, h, (uint16_t)scale);
        }
    }
    uint32_t grows = x->kind == APPS_W_IMAGE                ? BIT(P_SPIN) | BIT(P_SCALE) | BIT(P_SRC) | BIT(P_HIDDEN)
                     : x->kind == APPS_W_BOX && !x->creating ? BIT(P_HIDDEN)
                                                             : 0;
    if ((c->set & grows) != 0) {
        pending_t pend = {.at = x->cur != NULL ? (int)(x->cur - x->ui->w) : -1, .kind = x->kind, .c = c};
        uint64_t before = spin_total(x->ui, NULL);
        uint64_t after = spin_total(x->ui, &pend);
        if (after > before && after > APPS_SPIN_AREA_MAX) {
            fail_kind(x->L, x->kind, "spin: images spinning at once may cover at most %d x %d px together (these would cover %d px)",
                      APPS_SPIN_SIDE_MAX, APPS_SPIN_SIDE_MAX, (int)after);
        }
    }
}

/* The value at idx (a copy on the stack) as property p. */
static void read_prop(const ctx_t *x, prop_t p, int idx, change_t *c) {
    switch (prop_info[p].type) {
        case T_NUM: read_number(x, p, idx, c); break;
        case T_BOOL: read_bool(x, p, idx, c); break;
        case T_ENUM: read_enum(x, p, idx, c); break;
        case T_COLOR:
        case T_BG: read_colour(x, p, idx, c); break;
        case T_TEXT: read_text(x, p, idx, c); break;
        case T_RADIUS: read_radius(x, p, idx, c); break;
        case T_PARENT: read_parent(x, p, idx, c); break;
        case T_SRC: read_src(x, p, idx, c); break;
    }
    c->set |= BIT(p);
}

/*
 * Reads and checks the properties in the table at t (an absolute index), into c, and raises on the first
 * problem. Nothing is changed. An unknown key is found first, whatever else is wrong; then the properties are
 * read in a fixed order, so which error comes first doesn't depend on the table's own order.
 */
static void read_props(const ctx_t *x, int t, change_t *c) {
    lua_State *L = x->L;
    uint32_t present = 0;
    /* A walk C does where no count hook fires: charged by the slots lua_next visits. */
    apps_vm_charge(L, apps_vm_table_slots(L, t) + 1);
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        lua_pop(L, 1);  /* the value; the key stays for the next step */
        int p = -1;
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t len;
            const char *name = lua_tolstring(L, -1, &len);
            p = prop_of(x->kind, name, len);
        }
        if (p < 0) {
            unknown_property(L, x->kind, -1);
        }
        present |= BIT(p);
    }
    for (int p = 0; p < P_COUNT; p++) {
        if ((present & BIT(p)) != 0) {
            lua_pushstring(L, prop_names[p]);
            lua_rawget(L, t);  /* raw: no app code runs in here */
            read_prop(x, (prop_t)p, -1, c);
            lua_pop(L, 1);
        }
    }

    if (x->kind == APPS_W_ARC || x->kind == APPS_W_BAR) {
        /* min < max always, and value inside them: what a set leaves is checked, not just what it gave. */
        int64_t mn = (c->set & BIT(P_MIN)) != 0 ? c->v[P_MIN] : (x->cur != NULL ? x->cur->min : 0);
        int64_t mx = (c->set & BIT(P_MAX)) != 0 ? c->v[P_MAX] : (x->cur != NULL ? x->cur->max : 100);
        if (mn >= mx) {
            fail_kind(L, x->kind, "min must be less than max");
        }
        int64_t v = (c->set & BIT(P_VALUE)) != 0 ? c->v[P_VALUE] : (x->cur != NULL ? x->cur->value : 0);
        c->v[P_MIN] = mn;
        c->v[P_MAX] = mx;
        c->v[P_VALUE] = v < mn ? mn : (v > mx ? mx : v);
        c->set |= BIT(P_MIN) | BIT(P_MAX) | BIT(P_VALUE);
    }
    if (x->kind == APPS_W_IMAGE && x->creating && (c->set & BIT(P_SRC)) == 0) {
        fail_kind(L, x->kind, "src is required (a .bin image of this app's manifest.json \"files\")");
    }
    check_spin(x, c);
}

/* ---- Changing the table ---- */

/* Everything below the lock's comment is plain C: no Lua call, no allocation, no raise. */

/* Copies c's text into a block of its own (NULL for ""), before the lock. */
static char *copy_text(lua_State *L, int kind, const change_t *c) {
    if ((c->set & BIT(P_TEXT)) == 0 || c->text_len == 0) {
        return NULL;
    }
    char *block = apps_port_realloc(NULL, c->text_len + 1);
    if (block == NULL) {
        fail_kind(L, kind, "out of memory");
    }
    memcpy(block, c->text, c->text_len + 1);
    return block;
}

/* Takes the first slot that is neither alive nor waiting for the apply step to delete its object. The slot
 * and its gen, or -1. */
static int commit_create(apps_ui_t *ui, int kind, const change_t *c, char *text, uint16_t *gen) {
    int slot = -1;
    apps_port_ui_lock();
    for (int i = 0; i < APPS_UI_SLOTS && slot < 0; i++) {
        if (!ui->w[i].alive && (ui->w[i].dirty & APPS_DIRTY_DELETE) == 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        apps_widget_t *w = &ui->w[slot];
        *gen = w->gen;
        memset(w, 0, sizeof *w);
        w->gen = *gen;
        init_widget(w, kind);
        for (int p = 0; p < P_COUNT; p++) {
            if ((c->set & BIT(p)) != 0 && p != P_TEXT) {
                store(w, (prop_t)p, c->v[p]);
            }
        }
        w->text = text;
        w->rotation_sets = (c->set & BIT(P_ROTATION)) != 0 ? 1u : 0u;
        w->spun = w->spin != 0;
        w->alive = true;
        w->dirty = created_dirty(kind);
        ui->count++;
    }
    apps_port_ui_unlock();
    return slot;
}

/* Applies c to the widget at slot: only what differs is written, and only that sets a dirty bit. The old text
 * block, if the text changed, is left in *old for the caller to free once the lock is released. */
static void commit_set(apps_ui_t *ui, int slot, const change_t *c, char *text, char **old) {
    apps_port_ui_lock();
    apps_widget_t *w = &ui->w[slot];
    uint32_t dirty = 0;
    /* A set of rotation turns the image to it: of another angle, or of the one it holds once a spin has turned it away (spun).
     * The apply step turns it when rotation_sets moves. */
    if ((c->set & BIT(P_ROTATION)) != 0 && (w->rotation != c->v[P_ROTATION] || w->spun)) {
        w->rotation_sets++;
        w->spun = false;
        dirty |= APPS_DIRTY_STYLE;
    }
    for (int p = 0; p < P_COUNT; p++) {
        if ((c->set & BIT(p)) != 0 && p != P_TEXT && p != P_PARENT && fetch(w, (prop_t)p) != c->v[p]) {
            store(w, (prop_t)p, c->v[p]);
            dirty |= prop_info[p].dirty;
        }
    }
    w->spun = w->spun || w->spin != 0;
    if ((c->set & BIT(P_TEXT)) != 0) {
        *old = w->text;
        w->text = text;
        dirty |= APPS_DIRTY_TEXT;
    }
    w->dirty |= dirty;
    apps_port_ui_unlock();
}

/* What a delete or a clear takes out of the table, to be let go of after the lock. */
typedef struct {
    char *text[APPS_UI_SLOTS];
    uint8_t handler_slot[APPS_UI_SLOTS];
    int n_text, n_handlers;
} leaving_t;

/* The widget at slot is over (under the lock). Its gen moves on, which ends every handle and queued event of
 * it. Its object, if it has one, is for the apply step to delete: DELETE, and the slot stays taken until that is
 * done. A widget the apply step never made has no object, so its slot is free at once. */
static void leave(apps_ui_t *ui, int slot, leaving_t *gone) {
    apps_widget_t *w = &ui->w[slot];
    if (w->text != NULL) {
        gone->text[gone->n_text++] = w->text;
        w->text = NULL;
    }
    if (w->events != 0) {
        gone->handler_slot[gone->n_handlers++] = (uint8_t)slot;
        w->events = 0;
    }
    w->alive = false;
    w->gen++;
    ui->count--;
    w->dirty = (w->dirty & APPS_DIRTY_CREATE) != 0 ? 0 : APPS_DIRTY_DELETE;
}

/* Whether the widget at slot is somewhere inside the widget at root. */
static bool inside(const apps_ui_t *ui, int slot, int root) {
    int p = ui->w[slot].parent;
    for (int steps = 0; p >= 0 && steps < APPS_UI_SLOTS; steps++) {
        if (p == root) {
            return true;
        }
        p = ui->w[p].parent;
    }
    return false;
}

/* The widget at slot and everything inside it. */
static void commit_delete(apps_ui_t *ui, int slot, leaving_t *gone) {
    apps_port_ui_lock();
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        if (ui->w[i].alive && (i == slot || inside(ui, i, slot))) {
            leave(ui, i, gone);
        }
    }
    apps_port_ui_unlock();
}

/* Every widget. The apply step gets one clean of the layer instead of a delete for each, so a slot is free
 * at once, whatever it was waiting for. */
static void commit_clear(apps_ui_t *ui, leaving_t *gone) {
    apps_port_ui_lock();
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        if (ui->w[i].alive) {
            leave(ui, i, gone);
        }
        ui->w[i].dirty = 0;
    }
    ui->clear_pending = true;
    apps_port_ui_unlock();
}

/* Subscribes to the event whose bit it is, or (on false) unsubscribes. */
static void commit_events(apps_ui_t *ui, int slot, uint8_t bit, bool on) {
    apps_port_ui_lock();
    apps_widget_t *w = &ui->w[slot];
    uint8_t events = on ? (uint8_t)(w->events | bit) : (uint8_t)(w->events & ~bit);
    if (w->events != events) {
        w->events = events;
        w->dirty |= APPS_DIRTY_EVENTS;
    }
    apps_port_ui_unlock();
}

static void commit_bg(apps_ui_t *ui, uint32_t bg) {
    apps_port_ui_lock();
    if (ui->bg != bg) {
        ui->bg = bg;
        ui->bg_changed = true;
    }
    apps_port_ui_unlock();
}

/* ---- Handlers ---- */

/* The registry table of handlers is keyed by slot and event: one integer each, so the table is an array. */
static lua_Integer handler_key(int slot, int event) {
    return (lua_Integer)slot * (lua_Integer)EVENT_COUNT + event + 1;
}

/* Lets go of the text blocks (not under the lock), and of the handlers of the widgets that had any: a nil in the
 * registry's table never allocates, so this can't raise. */
static void release(lua_State *L, const apps_ui_t *ui, const leaving_t *gone) {
    for (int i = 0; i < gone->n_text; i++) {
        apps_port_free(gone->text[i]);
    }
    if (gone->n_handlers == 0) {
        return;
    }
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, ui) == LUA_TTABLE) {
        for (int i = 0; i < gone->n_handlers; i++) {
            for (int e = 0; e < EVENT_COUNT; e++) {
                lua_pushnil(L);
                lua_rawseti(L, -2, handler_key(gone->handler_slot[i], e));
            }
        }
    }
    lua_pop(L, 1);
}

/* ---- Creating ---- */

static NORETURN void quota_error(lua_State *L, const apps_ui_t *ui) {
    if (ui->count < APPS_UI_SLOTS) {  /* the free slots are those of widgets deleted since the last screen update */
        fail(L, "ui: no more than %d widgets per app (a deleted widget keeps its slot until the screen next updates)",
             APPS_UI_SLOTS);
    }
    fail(L, "ui: no more than %d widgets per app", APPS_UI_SLOTS);
}

static int create(lua_State *L, int kind) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    if (lua_type(L, 1) != LUA_TTABLE) {
        fail_kind(L, kind, "needs a table of properties, like ui.%s{...}, got %s", apps_ui_kinds[kind],
                  luaL_typename(L, 1));
    }
    lua_settop(L, 1);
    handle_t *h = push_handle(L, NO_SLOT, 0, kind);  /* before anything is held outside the Lua heap */
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &ui->handle_key) != LUA_TTABLE) {  /* the same: nothing may raise later */
        fail(L, "ui: the handle table is gone");
    }
    change_t c = {0};
    ctx_t x = {.L = L, .ui = ui, .kind = kind, .creating = true, .cur = NULL};
    read_props(&x, 1, &c);
    char *text = copy_text(L, kind, &c);
    uint16_t gen = 0;
    int slot = commit_create(ui, kind, &c, text, &gen);
    if (slot < 0) {
        apps_port_free(text);
        quota_error(L, ui);
    }
    h->slot = (uint16_t)slot;
    h->gen = gen;
    lua_pushvalue(L, 2);
    lua_rawseti(L, 3, slot + 1);  /* the table has a place for every slot: no allocation, no raise */
    lua_settop(L, 2);
    return 1;
}

static int ui_label(lua_State *L) {
    return create(L, APPS_W_LABEL);
}
static int ui_button(lua_State *L) {
    return create(L, APPS_W_BUTTON);
}
static int ui_box(lua_State *L) {
    return create(L, APPS_W_BOX);
}
static int ui_arc(lua_State *L) {
    return create(L, APPS_W_ARC);
}
static int ui_bar(lua_State *L) {
    return create(L, APPS_W_BAR);
}
static int ui_image(lua_State *L) {
    return create(L, APPS_W_IMAGE);
}

/* ---- The module's helpers ---- */

static int ui_bg(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    int type = lua_type(L, 1);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
        fail(L, "ui.bg: needs a colour %s, got %s", colour_hint, luaL_typename(L, 1));
    }
    lua_settop(L, 1);
    uint32_t colour;
    shown_t shown;
    if (!colour_at(L, 1, false, &colour, &shown)) {
        fail(L, "ui.bg: '%s' is not a colour %s", shown.s, colour_hint);
    }
    commit_bg(ui, colour);
    return 0;
}

static int ui_clear(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    leaving_t gone;
    gone.n_text = gone.n_handlers = 0;
    commit_clear(ui, &gone);
    release(L, ui, &gone);
    return 0;
}

/* The number at arg, or an error that names it. Not NaN; an infinity is the caller's to clamp. */
static double polar_arg(lua_State *L, int arg, const char *name) {
    if (lua_type(L, arg) != LUA_TNUMBER) {
        fail(L, "ui.polar: '%s' must be a number, got %s", name, luaL_typename(L, arg));
    }
    double d = (double)lua_tonumber(L, arg);
    if (isnan(d)) {
        fail(L, "ui.polar: '%s' must be a number, got NaN", name);
    }
    return d;
}

/* ui.polar(deg, r): the offsets of the point r from the centre at deg clock degrees, rounded. */
static int ui_polar(lua_State *L) {
    apps_vm_check(L);
    double deg = polar_arg(L, 1, "deg"), r = polar_arg(L, 2, "r");
    if (isinf(deg)) {
        fail(L, "ui.polar: 'deg' must be a finite number, got %s", deg > 0 ? "inf" : "-inf");
    }
    if (r > APPS_UI_VALUE_MAX) {
        r = APPS_UI_VALUE_MAX;
    } else if (r < -APPS_UI_VALUE_MAX) {
        r = -APPS_UI_VALUE_MAX;
    }
    const double pi = 3.14159265358979323846;
    double rad = fmod(deg, 360.0) * pi / 180.0;  /* 0 is 12 o'clock, and the angle runs clockwise */
    lua_pushinteger(L, (lua_Integer)round(r * sin(rad)));
    lua_pushinteger(L, (lua_Integer)round(-r * cos(rad)));
    return 2;
}

static const struct {
    const char *name;
    lua_Integer value;
} apps_ui_consts[] = {{"W", SCREEN}, {"H", SCREEN}, {"R", SCREEN_R}, {"SAFE", SCREEN_SAFE}};

static const luaL_Reg apps_ui_funcs[] = {
    {"label", ui_label}, {"button", ui_button}, {"box", ui_box}, {"arc", ui_arc}, {"bar", ui_bar},
    {"image", ui_image}, {"bg", ui_bg},         {"clear", ui_clear}, {"polar", ui_polar},
    {NULL, NULL},
};

/* ui.<name> for a name the module doesn't have. */
static int ui_missing(lua_State *L) {
    apps_vm_check(L);
    const char *names[ARRAY_LEN(apps_ui_funcs) + ARRAY_LEN(apps_ui_consts)];
    int n = 0;
    for (const luaL_Reg *f = apps_ui_funcs; f->name != NULL; f++) {
        names[n++] = f->name;
    }
    for (int i = 0; i < ARRAY_LEN(apps_ui_consts); i++) {
        names[n++] = apps_ui_consts[i].name;
    }
    char list[160];
    join_names(list, sizeof list, names, n);
    shown_t shown;
    int type = lua_type(L, 2);
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        size_t len;
        const char *s = lua_tolstring(L, 2, &len);
        show(&shown, s, len);
    } else {
        snprintf(shown.s, sizeof shown.s, "<%s>", luaL_typename(L, 2));
    }
    fail(L, "ui.%s doesn't exist (ui has: %s)", shown.s, list);
}

/* ---- Widget methods ---- */

static int w_set(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    handle_t *h = self_handle(L, "set");
    apps_widget_t *w = live_widget(L, ui, h);
    if (lua_type(L, 2) != LUA_TTABLE) {
        fail_kind(L, h->kind, "set needs a table of properties, like w:set{...}, got %s", luaL_typename(L, 2));
    }
    lua_settop(L, 2);
    change_t c = {0};
    ctx_t x = {.L = L, .ui = ui, .kind = h->kind, .creating = false, .cur = w};
    read_props(&x, 2, &c);
    char *text = NULL;
    if ((c.set & BIT(P_TEXT)) != 0) {
        size_t have = w->text != NULL ? strlen(w->text) : 0;
        if (have == c.text_len && (have == 0 || memcmp(w->text, c.text, have) == 0)) {
            c.set &= ~BIT(P_TEXT);  /* the same text: nothing to allocate and nothing for the apply step */
        } else {
            text = copy_text(L, h->kind, &c);
        }
    }
    char *old = NULL;
    commit_set(ui, h->slot, &c, text, &old);
    apps_port_free(old);
    lua_settop(L, 1);
    return 1;
}

static void push_colour(lua_State *L, uint32_t colour) {
    char text[8];
    if (colour == APPS_COLOR_NONE) {
        lua_pushliteral(L, "none");
        return;
    }
    snprintf(text, sizeof text, "#%02x%02x%02x", (unsigned)(colour >> 16) & 0xFF, (unsigned)(colour >> 8) & 0xFF,
             (unsigned)colour & 0xFF);
    lua_pushstring(L, text);
}

static int w_get(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    handle_t *h = self_handle(L, "get");
    apps_widget_t *w = live_widget(L, ui, h);
    if (lua_type(L, 2) != LUA_TSTRING) {
        fail_kind(L, h->kind, "get needs the name of a property, got %s", luaL_typename(L, 2));
    }
    size_t len;
    const char *name = lua_tolstring(L, 2, &len);
    int p = prop_of(h->kind, name, len);
    if (p < 0) {
        unknown_property(L, h->kind, 2);
    }
    int64_t v = fetch(w, (prop_t)p);
    switch (prop_info[p].type) {
        case T_NUM:
            if (v < 0 && (p == P_W || p == P_H)) {  /* sizes to content */
                lua_pushnil(L);
            } else {
                lua_pushinteger(L, (lua_Integer)v);
            }
            break;
        case T_BOOL: lua_pushboolean(L, v != 0); break;
        case T_ENUM: lua_pushstring(L, prop_info[p].names[v]); break;
        case T_COLOR:
        case T_BG: push_colour(L, (uint32_t)v); break;
        case T_TEXT: lua_pushstring(L, w->text != NULL ? w->text : ""); break;
        case T_RADIUS:
            if (v < 0) {
                lua_pushliteral(L, "circle");
            } else {
                lua_pushinteger(L, (lua_Integer)v);
            }
            break;
        case T_PARENT:
            if (v < 0) {
                lua_pushnil(L);
            } else {
                push_widget(L, ui, (int)v, ui->w[(int)v].gen, APPS_W_BOX);
            }
            break;
        case T_SRC:
            if (v >= 0 && v < ui->n_assets) {
                lua_pushstring(L, ui->assets[v].path);
            } else if (v >= IMAGE_RT && v < IMAGES_ALL) {
                apps_ui_push_image(L, ui, (int)(v - IMAGE_RT));  /* the handle the app holds, or one of the same image */
            } else {
                lua_pushnil(L);
            }
            break;
    }
    return 1;
}

static int w_on(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    handle_t *h = self_handle(L, "on");
    live_widget(L, ui, h);
    if (lua_type(L, 2) != LUA_TSTRING) {
        fail_kind(L, h->kind, "on needs the name of an event, got %s", luaL_typename(L, 2));
    }
    size_t len;
    const char *name = lua_tolstring(L, 2, &len);
    int event = -1;
    if (len == 5 && memcmp(name, "click", 5) == 0) {
        event = 0;  /* tap, as LLMs write it */
    }
    for (int i = 0; i < (int)EVENT_COUNT && event < 0; i++) {
        if (strlen(apps_ui_events[i]) == len && memcmp(apps_ui_events[i], name, len) == 0) {
            event = i;
        }
    }
    if (event < 0) {
        char list[64];
        join_names(list, sizeof list, apps_ui_events, (int)EVENT_COUNT);
        shown_t shown;
        fail_kind(L, h->kind, "unknown event '%s' (valid: %s)", show(&shown, name, len), list);
    }
    int type = lua_type(L, 3);
    if (type != LUA_TFUNCTION && type != LUA_TNIL) {
        fail_kind(L, h->kind, "handler for '%s' must be a function (or nil to remove it), got %s",
                  apps_ui_events[event], luaL_typename(L, 3));
    }
    lua_settop(L, 3);
    /* The handler goes into the registry first: it may allocate, and raise. Then the events mask, under the lock. */
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, ui) != LUA_TTABLE) {
        fail(L, "ui: the handler table is gone");
    }
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, handler_key(h->slot, event));
    lua_pop(L, 1);
    commit_events(ui, h->slot, (uint8_t)(1u << event), type == LUA_TFUNCTION);
    lua_settop(L, 1);
    return 1;
}

static int w_delete(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    handle_t *h = self_handle(L, "delete");
    live_widget(L, ui, h);
    leaving_t gone;
    gone.n_text = gone.n_handlers = 0;
    commit_delete(ui, h->slot, &gone);
    release(L, ui, &gone);
    return 0;
}

static const luaL_Reg apps_ui_methods[] = {
    {"set", w_set}, {"get", w_get}, {"on", w_on}, {"delete", w_delete}, {NULL, NULL},
};

/* ---- The handle's metamethods ---- */

/* w.name: a method, or an error that lists them (reading a property this way is a mistake worth explaining). */
static int widget_index(lua_State *L) {
    apps_vm_check(L);
    const handle_t *h = luaL_testudata(L, 1, WIDGET_MT);
    if (h != NULL && lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(2)) != LUA_TNIL) {  /* upvalue 2: the methods */
            return 1;
        }
    }
    int kind = h != NULL ? h->kind : APPS_W_LABEL;
    char list[64];
    const char *names[ARRAY_LEN(apps_ui_methods)];
    int n = 0;
    for (const luaL_Reg *f = apps_ui_methods; f->name != NULL; f++) {
        names[n++] = f->name;
    }
    join_names(list, sizeof list, names, n);
    shown_t shown;
    if (lua_type(L, 2) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, 2, &len);
        show(&shown, s, len);
    } else {
        snprintf(shown.s, sizeof shown.s, "<%s>", luaL_typename(L, 2));
    }
    fail_kind(L, kind, "unknown method '%s' (valid: %s)", shown.s, list);
}

/* w.name = v: widgets have no fields; the way to change one is w:set. */
static int widget_newindex(lua_State *L) {
    apps_vm_check(L);
    const handle_t *h = luaL_testudata(L, 1, WIDGET_MT);
    int kind = h != NULL ? h->kind : APPS_W_LABEL;
    if (lua_type(L, 2) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, 2, &len);
        shown_t shown;
        show(&shown, s, len);
        fail_kind(L, kind, "can't assign w.%s: change a widget with w:set{%s = ...}", shown.s, shown.s);
    }
    fail_kind(L, kind, "can't assign to a widget: change it with w:set{...}");
}

static int widget_tostring(lua_State *L) {
    apps_vm_check(L);
    apps_ui_t *ui = ui_of(L);
    const handle_t *h = luaL_testudata(L, 1, WIDGET_MT);
    if (h == NULL) {
        fail(L, "ui: tostring needs a widget");
    }
    lua_pushfstring(L, "%s#%d%s", apps_ui_kinds[h->kind], (int)h->slot, widget_of(ui, h) != NULL ? "" : " (deleted)");
    return 1;
}

/* Two handles are equal when they name the same widget of the same life. (While the app holds a handle it is the
 * one object push_widget gives, so this only matters for handles of widgets that are gone.) */
static int widget_eq(lua_State *L) {
    apps_vm_check(L);
    const handle_t *a = luaL_testudata(L, 1, WIDGET_MT), *b = luaL_testudata(L, 2, WIDGET_MT);
    lua_pushboolean(L, a != NULL && b != NULL && a->slot == b->slot && a->gen == b->gen && a->kind == b->kind);
    return 1;
}

static const luaL_Reg apps_ui_meta[] = {
    {"__newindex", widget_newindex}, {"__tostring", widget_tostring}, {"__eq", widget_eq}, {NULL, NULL},
};

/* ---- The module ---- */

/* Runs under lua_pcall: installs the registry's table of handlers, the handle's metatable and the global. */
static int open_ui(lua_State *L) {
    apps_ui_t *ui = lua_touserdata(L, 1);
    lua_settop(L, 0);

    lua_createtable(L, 0, 0);  /* the handlers, keyed by slot and event */
    lua_rawsetp(L, LUA_REGISTRYINDEX, ui);

    lua_createtable(L, APPS_UI_SLOTS, 0);  /* the handles the app may hold, a place for each slot: see push_widget */
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &ui->handle_key);

    lua_createtable(L, APPS_RT_IMAGES_MAX, 0);  /* the runtime images' handles, the same way: see apps_ui_push_image */
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &ui->image_key);

    /* Protected: getmetatable gives the name, never the table. push_handle's luaL_setmetatable registers a
     * finalizer whenever the metatable holds __gc and skips the sandbox's setmetatable, which refuses one, so an
     * app that could write __gc into it would give every later handle a finalizer, and Lua runs those with the
     * hooks off, beyond the budget and the kill. */
    luaL_newmetatable(L, WIDGET_MT);
    lua_pushliteral(L, WIDGET_MT);
    lua_setfield(L, -2, "__metatable");
    lua_createtable(L, 0, 4);  /* the methods */
    lua_pushlightuserdata(L, ui);
    luaL_setfuncs(L, apps_ui_methods, 1);
    lua_pushlightuserdata(L, ui);
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, widget_index, 2);
    lua_setfield(L, -3, "__index");
    lua_pop(L, 1);
    lua_pushlightuserdata(L, ui);
    luaL_setfuncs(L, apps_ui_meta, 1);
    lua_pop(L, 1);

    lua_createtable(L, 0, 16);
    lua_pushlightuserdata(L, ui);
    luaL_setfuncs(L, apps_ui_funcs, 1);
    for (int i = 0; i < ARRAY_LEN(apps_ui_consts); i++) {
        lua_pushinteger(L, apps_ui_consts[i].value);
        lua_setfield(L, -2, apps_ui_consts[i].name);
    }
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, ui_missing);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "muse.ui");  /* protected, like the handles' */
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "ui");
    return 0;
}

void apps_ui_open(lua_State *L, apps_ui_t *ui) {
    int top = lua_gettop(L);
    if (lua_checkstack(L, 3)) {
        lua_pushcfunction(L, open_ui);
        lua_pushlightuserdata(L, ui);
        /* Protected, so a heap that refuses doesn't abort: the VM is then out of memory and the next apps_vm_call
         * says so. */
        (void)lua_pcall(L, 1, 0, 0);
    }
    lua_settop(L, top);
}

/* ---- The table's life, and the apply step's side ---- */

apps_ui_t *apps_ui_new(void) {
    apps_ui_t *ui = apps_port_realloc(NULL, sizeof *ui);
    if (ui != NULL) {
        memset(ui, 0, sizeof *ui);
        ui->bg_changed = true;  /* the layer takes the app's colour (black) whatever the last app left on it */
    }
    return ui;
}

void apps_ui_free(apps_ui_t *ui) {
    if (ui == NULL) {
        return;
    }
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        apps_port_free(ui->w[i].text);
    }
    apps_port_free(ui);
}

void apps_ui_set_assets(apps_ui_t *ui, const apps_asset_t *assets, int n) {
    if (ui == NULL) {
        return;
    }
    if (assets == NULL || n < 0) {
        n = 0;
    }
    if (n > APPS_UI_ASSETS_MAX) {
        n = APPS_UI_ASSETS_MAX;
    }
    if (n > 0) {
        memcpy(ui->assets, assets, (size_t)n * sizeof *assets);
    }
    ui->n_assets = n;
}

apps_widget_t *apps_ui_slot(apps_ui_t *ui, int slot) {
    return ui != NULL && slot >= 0 && slot < APPS_UI_SLOTS ? &ui->w[slot] : NULL;
}

const apps_asset_t *apps_ui_asset(const apps_ui_t *ui, int index) {
    if (ui == NULL || index < 0) {
        return NULL;
    }
    if (index < ui->n_assets) {
        return &ui->assets[index];
    }
    if (index >= IMAGE_RT && index < IMAGES_ALL && ui->images[index - IMAGE_RT].data != NULL) {
        return &ui->images[index - IMAGE_RT];
    }
    return NULL;
}

void apps_ui_report_drawn(apps_ui_t *ui, uint32_t drawn) {
    if (ui != NULL) {
        ui->drawn = drawn;
    }
}

/* ---- Runtime images: the runtime's side ---- */

void apps_ui_set_image(apps_ui_t *ui, int slot, uint32_t gen, const uint8_t *block, size_t len, uint16_t w, uint16_t h) {
    if (ui == NULL || slot < 0 || slot >= APPS_RT_IMAGES_MAX) {
        return;
    }
    apps_port_ui_lock();
    ui->images[slot] = block != NULL ? (apps_asset_t){.path = NULL, .data = block, .len = len, .w = w, .h = h} : (apps_asset_t){0};
    ui->image_gen[slot] = block != NULL ? gen : 0;
    apps_port_ui_unlock();
}

bool apps_ui_image_shown(const apps_ui_t *ui, int slot) {
    if (ui == NULL || slot < 0 || slot >= APPS_RT_IMAGES_MAX) {
        return false;
    }
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = &ui->w[i];
        if (w->alive && w->kind == APPS_W_IMAGE && w->image == IMAGE_RT + slot) {
            return true;
        }
    }
    return false;
}

uint32_t apps_ui_images_drawn(apps_ui_t *ui) {
    if (ui == NULL) {
        return 0;
    }
    apps_port_ui_lock();
    uint32_t drawn = ui->drawn;
    apps_port_ui_unlock();
    return drawn;
}

/* A new handle of the runtime image at slot, on the stack. */
static void push_image_handle(lua_State *L, int slot, uint32_t gen) {
    apps_image_handle_t *h = lua_newuserdatauv(L, sizeof *h, 0);
    h->gen = gen;
    h->slot = (uint16_t)slot;
    luaL_setmetatable(L, APPS_IMAGE_MT);
}

/* As push_widget does for widgets: a registry table with weak values and a place for each slot, which holds the handle of that slot's
 * image as the app last had it, looked at (slot and gen) before it is given again. Made with its places, so storing a handle can't
 * allocate. */
void apps_ui_push_image(lua_State *L, apps_ui_t *ui, int slot) {
    luaL_checkstack(L, 4, "image");
    if (ui == NULL || slot < 0 || slot >= APPS_RT_IMAGES_MAX || ui->images[slot].data == NULL) {
        lua_pushnil(L);
        return;
    }
    const uint32_t gen = ui->image_gen[slot];
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &ui->image_key) != LUA_TTABLE) {  /* never: ui is installed after it */
        lua_pop(L, 1);
        push_image_handle(L, slot, gen);
        return;
    }
    if (lua_rawgeti(L, -1, slot + 1) == LUA_TUSERDATA) {
        const apps_image_handle_t *known = luaL_testudata(L, -1, APPS_IMAGE_MT);
        if (known != NULL && known->slot == slot && known->gen == gen) {
            lua_remove(L, -2);
            return;
        }
    }
    lua_pop(L, 1);
    push_image_handle(L, slot, gen);  /* may raise: nothing is held */
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, slot + 1);
    lua_remove(L, -2);
}

uint32_t apps_ui_take_bg(apps_ui_t *ui, bool *changed) {
    if (ui == NULL) {
        if (changed != NULL) {
            *changed = false;
        }
        return 0;
    }
    if (changed != NULL) {
        *changed = ui->bg_changed;
    }
    ui->bg_changed = false;
    return ui->bg;
}

bool apps_ui_take_clear(apps_ui_t *ui) {
    if (ui == NULL) {
        return false;
    }
    bool pending = ui->clear_pending;
    ui->clear_pending = false;
    return pending;
}

int apps_ui_count(const apps_ui_t *ui) {
    return ui != NULL ? ui->count : 0;
}

bool apps_ui_push_handler(lua_State *L, apps_ui_t *ui, uint16_t slot, uint16_t gen, uint8_t event) {
    int index = -1;
    for (int i = 0; i < (int)EVENT_COUNT; i++) {
        if (event == (1u << i)) {
            index = i;
        }
    }
    if (ui == NULL || index < 0 || slot >= APPS_UI_SLOTS) {
        return false;
    }
    const apps_widget_t *w = &ui->w[slot];
    if (!w->alive || w->gen != gen || (w->events & event) == 0 || !lua_checkstack(L, 5)) {
        return false;
    }
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, ui) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }
    int type = lua_rawgeti(L, -1, handler_key(slot, index));
    lua_remove(L, -2);
    if (type != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return false;
    }
    push_widget(L, ui, slot, gen, w->kind);
    lua_pushstring(L, apps_ui_events[index]);
    return true;
}

/* ---- The dump ---- */

/* The output, with room kept for what closes the array. */
typedef struct {
    char *p;
    size_t cap;  /* bytes the entries may take, from the start */
    size_t len;
    bool full;   /* something didn't fit: nothing more is written */
} out_t;

static void put(out_t *o, const char *s, size_t n) {
    if (o->full || n > o->cap - o->len) {
        o->full = true;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void put_str(out_t *o, const char *s) {
    put(o, s, strlen(s));
}

static void put_int(out_t *o, int v) {
    char text[16];
    int n = snprintf(text, sizeof text, "%d", v);
    put(o, text, (size_t)n);
}

/* s as a JSON string: valid UTF-8 as it is, anything else as U+FFFD. */
static void put_json_string(out_t *o, const char *s) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s, *end = p + strlen(s);
    put(o, "\"", 1);
    while (p < end && !o->full) {
        unsigned char ch = *p;
        if (ch >= 0x80) {
            int n = apps_json_utf8_len(p, end);
            if (n > 0) {
                put(o, (const char *)p, (size_t)n);
                p += n;
            } else {
                put(o, "\\ufffd", 6);
                p++;
            }
            continue;
        }
        switch (ch) {
            case '"': put(o, "\\\"", 2); break;
            case '\\': put(o, "\\\\", 2); break;
            case '\n': put(o, "\\n", 2); break;
            case '\r': put(o, "\\r", 2); break;
            case '\t': put(o, "\\t", 2); break;
            default:
                if (ch < 0x20) {
                    char esc[6] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15]};
                    put(o, esc, sizeof esc);
                } else {
                    put(o, (const char *)p, 1);
                }
        }
        p++;
    }
    put(o, "\"", 1);
}

static void put_size(out_t *o, int v) {  /* a size to content is null */
    if (v < 0) {
        put_str(o, "null");
    } else {
        put_int(o, v);
    }
}

static void put_entry(out_t *o, const apps_widget_t *w, int slot) {
    const bool laid_out = w->rw > 0;
    char id[24];
    snprintf(id, sizeof id, "%s#%d", apps_ui_kinds[w->kind], slot);
    put_str(o, "{\"id\":");
    put_json_string(o, id);
    put_str(o, ",\"kind\":");
    put_json_string(o, apps_ui_kinds[w->kind]);
    put_str(o, ",\"text\":");
    if (w->kind == APPS_W_LABEL || w->kind == APPS_W_BUTTON) {
        put_json_string(o, w->text != NULL ? w->text : "");
    } else {
        put_str(o, "null");
    }
    put_str(o, ",\"x\":");
    put_int(o, laid_out ? w->rx : w->x);
    put_str(o, ",\"y\":");
    put_int(o, laid_out ? w->ry : w->y);
    put_str(o, ",\"w\":");
    put_size(o, laid_out ? w->rw : w->w);
    put_str(o, ",\"h\":");
    put_size(o, laid_out ? w->rh : w->h);
    put_str(o, w->hidden ? ",\"hidden\":true" : ",\"hidden\":false");
    put_str(o, ",\"overflow\":");
    put_int(o, w->overflow);
    put_str(o, "}");
}

size_t apps_ui_dump(const apps_ui_t *ui, char *out, size_t cap, int *dropped) {
    int alive = 0, written = 0;
    size_t len = 0;
    if (ui == NULL) {
        if (out != NULL && cap > 0) {
            out[0] = '\0';
        }
        if (dropped != NULL) {
            *dropped = 0;
        }
        return 0;
    }
    apps_port_ui_lock();
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        alive += ui->w[i].alive ? 1 : 0;
    }
    if (out != NULL && cap >= 3) {  /* "[" and "]" and the NUL */
        out_t o = {.p = out, .cap = cap - 2, .len = 1, .full = false};
        out[0] = '[';
        for (int i = 0; i < APPS_UI_SLOTS && !o.full; i++) {
            if (!ui->w[i].alive) {
                continue;
            }
            size_t mark = o.len;
            if (written > 0) {
                put(&o, ",", 1);
            }
            put_entry(&o, &ui->w[i], i);
            if (o.full) {
                o.len = mark;  /* whole entries only */
            } else {
                written++;
            }
        }
        out[o.len++] = ']';
        out[o.len] = '\0';
        len = o.len;
    } else if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    apps_port_ui_unlock();
    if (dropped != NULL) {
        *dropped = alive - written;
    }
    return len;
}
