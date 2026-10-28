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
 * Checks apps_lv, the apply step of Muse apps, against a fake LVGL (tests/muse_apps_lv_fakes.h declares it, this
 * file is it). Usage: muse_apps_lv_harness <case>, one case per test of HarnessTest in tests/test_muse_apps_lv.py.
 * Exits 0 when the case holds and prints what it got when it doesn't.
 *
 * The widgets come from Lua through the real ui module and its table (apps_ui), and the pass is the real
 * apps_lv_apply; what is faked is LVGL, the lock and the heap. The fake LVGL logs each call as a line, keeps
 * every object it ever made (a deleted one is only marked) and counts a call on a deleted or NULL object, or on
 * an object of the wrong class, as an error, which on a device is a crash. The port below is the harness's own:
 * a counting allocator (LeakSanitizer doesn't run on macOS, so every case ends at 0 bytes held), and a UI lock
 * that records how it is used. The fake LVGL also counts an LVGL call made inside apps_lv_apply while the lock is
 * not held.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_json.h"
#include "apps_lv.h"
#include "apps_port.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"
#include "muse_apps_lv_fakes.h"

static int failures;

#define CHECK_AT(line, cond, ...)                                  \
    do {                                                           \
        if (!(cond)) {                                             \
            failures++;                                            \
            fprintf(stderr, "FAIL line %d: %s: ", (line), #cond);  \
            fprintf(stderr, __VA_ARGS__);                          \
            fputc('\n', stderr);                                   \
        }                                                          \
    } while (0)
#define CHECK(cond, ...) CHECK_AT(__LINE__, cond, __VA_ARGS__)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- The port ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;            /* bytes handed out and not freed */
static long growths;           /* growth requests so far */
static long refuse_from = -1;  /* refuse every growth request from this one on (counted in growths); -1: none */

static bool lock_held;
static long lock_takes;     /* locks taken: trylock calls that said yes, and lock calls */
static long lock_blocking;  /* apps_port_ui_lock calls made inside apps_lv_apply: the apply step only tries the lock */
static bool apply_active;   /* inside apps_lv_apply */
static int trylock_refusals;  /* the next calls of apps_port_ui_trylock say no */
static int trylock_ms;      /* what it was last asked to wait */
static long trylock_calls;
static int lock_errors;
static char lock_note[96];

static void lock_error(const char *what) {
    if (lock_errors++ == 0) {
        snprintf(lock_note, sizeof lock_note, "%s", what);
    }
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (lock_held) {
        lock_error("the heap was used while the lock was held");
    }
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old) {
        growths++;
        if (refuse_from >= 0 && growths >= refuse_from) {
            return NULL;
        }
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    held = held - old + n;
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        if (lock_held) {
            lock_error("a block was freed while the lock was held");
        }
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        free(b);
    }
}

int64_t apps_port_mono_ms(void) {
    return 0;
}

bool apps_port_epoch_ms(int64_t *out) {
    (void)out;
    return false;
}

void apps_port_log(const char *app_id, const char *line) {
    (void)app_id;
    (void)line;
}

void apps_port_ui_lock(void) {
    if (lock_held) {
        lock_error("the lock was taken while it was held");
    }
    lock_held = true;
    lock_takes++;
    if (apply_active) {
        lock_blocking++;
    }
}

bool apps_port_ui_trylock(int ms) {
    trylock_ms = ms;
    trylock_calls++;
    if (trylock_refusals > 0) {
        trylock_refusals--;
        return false;
    }
    if (lock_held) {
        lock_error("the lock was taken while it was held");
    }
    lock_held = true;
    lock_takes++;
    return true;
}

void apps_port_ui_unlock(void) {
    if (!lock_held) {
        lock_error("the lock was given back and wasn't held");
    }
    lock_held = false;
}

void apps_port_sha256(const void *data, size_t len, uint8_t out[32]) {
    (void)data;
    (void)len;
    memset(out, 0, 32);
}

/* ---- The logs ---- */

/* Two: every call into the fake LVGL, one line each (`log`), and what apps_lv.c wrote with ESP_LOGx (`esp`). A case
 * that says no LVGL call was made reads the first, and one about what the log says reads the second. */
typedef struct {
    char *data;
    size_t len, cap;
    char *taken;  /* what the last take returned */
} textbuf_t;

static textbuf_t lvgl_log, esp_log_text;

static void tb_append(textbuf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
        if (b->data == NULL) {
            fputs("out of memory in the harness\n", stderr);
            exit(2);
        }
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void tb_line(textbuf_t *b, const char *fmt, va_list ap) {
    char line[512];
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof line - 2) {
        n = (int)sizeof line - 2;
    }
    line[n++] = '\n';
    tb_append(b, line, (size_t)n);
}

/* What was logged since the last take: a string that stays until the next take. */
static const char *tb_take(textbuf_t *b) {
    free(b->taken);
    b->taken = b->data != NULL ? strdup(b->data) : strdup("");
    b->len = 0;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
    return b->taken;
}

static const char *tb_peek(const textbuf_t *b) {
    return b->data != NULL ? b->data : "";
}

/* How many lines start with prefix. */
static int tb_count(const textbuf_t *b, const char *prefix) {
    int n = 0;
    size_t plen = strlen(prefix);
    for (const char *p = tb_peek(b); *p != '\0';) {
        if (strncmp(p, prefix, plen) == 0) {
            n++;
        }
        const char *nl = strchr(p, '\n');
        p = nl != NULL ? nl + 1 : p + strlen(p);
    }
    return n;
}

/* Whether a line is exactly this. */
static bool tb_has_line(const textbuf_t *b, const char *line) {
    size_t len = strlen(line);
    for (const char *p = tb_peek(b); *p != '\0';) {
        const char *nl = strchr(p, '\n');
        size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if (n == len && strncmp(p, line, n) == 0) {
            return true;
        }
        p = nl != NULL ? nl + 1 : p + n;
    }
    return false;
}

/* The line number (from 0) of the first line that starts with prefix, or -1. */
static int tb_find(const textbuf_t *b, const char *prefix) {
    int i = 0;
    size_t plen = strlen(prefix);
    for (const char *p = tb_peek(b); *p != '\0'; i++) {
        if (strncmp(p, prefix, plen) == 0) {
            return i;
        }
        const char *nl = strchr(p, '\n');
        p = nl != NULL ? nl + 1 : p + strlen(p);
    }
    return -1;
}

static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tb_line(&lvgl_log, fmt, ap);
    va_end(ap);
}

static const char *log_take(void) {
    return tb_take(&lvgl_log);
}

static void log_clear(void) {
    (void)log_take();
}

static const char *log_peek(void) {
    return tb_peek(&lvgl_log);
}

static int log_count(const char *prefix) {
    return tb_count(&lvgl_log, prefix);
}

static bool log_has_line(const char *line) {
    return tb_has_line(&lvgl_log, line);
}

static int log_find(const char *prefix) {
    return tb_find(&lvgl_log, prefix);
}

static void log_is_at(int line, const char *want) {
    const char *got = log_take();
    CHECK_AT(line, strcmp(got, want) == 0, "the log is\n%s--- and was to be\n%s---", got, want);
}
#define LOG_IS(want) log_is_at(__LINE__, (want))

static void log_empty_at(int line) {
    const char *got = log_take();
    CHECK_AT(line, got[0] == '\0', "the log was to be empty and is\n%s---", got);
}
#define LOG_EMPTY() log_empty_at(__LINE__)

/* What apps_lv.c logged: "W muse_apps: apply 2001 us". */
static const char *esp_peek(void) {
    return tb_peek(&esp_log_text);
}

static int esp_count(const char *prefix) {
    return tb_count(&esp_log_text, prefix);
}

static bool esp_has_line(const char *line) {
    return tb_has_line(&esp_log_text, line);
}

static void esp_clear(void) {
    (void)tb_take(&esp_log_text);
}

/* ---- The fake LVGL ---- */

typedef struct {
    lv_event_cb_t cb;
    lv_event_code_t filter;
    void *ud;
} fake_cb_t;

struct lv_obj {
    int id;
    const char *cls;       /* "screen", "obj", "label", "button", "arc", "bar", "image" */
    lv_obj_t *parent;
    bool deleted;
    uint32_t flags;
    int32_t w, h, x, y;    /* LV_SIZE_CONTENT until set */
    lv_align_t align;
    char *text;
    int32_t min, max, value;
    const lv_image_dsc_t *src;
    uint32_t scale;
    lv_area_t area;
    bool forced;           /* the test set the area: the layout leaves it */
    fake_cb_t cbs[8];
    int n_cbs;
};

struct lv_indev {
    int unused;
};

struct lv_event {
    lv_event_code_t code;
    void *ud;
    lv_obj_t *target;
    bool stopped;
};

#define FAKE_OBJS_MAX 4096
static lv_obj_t *objs[FAKE_OBJS_MAX];
static int n_objs;
static int next_id;
static lv_obj_t screen_obj;
static lv_indev_t indev_obj;
static bool indev_present = true;
static lv_dir_t gesture_dir = LV_DIR_NONE;

static int fake_errors;
static char fake_note[240];

static long lvgl_calls;          /* every call into the fake LVGL, but the getters of events */
static int64_t clock_us = 1000000;
static int64_t call_cost_us;     /* every LVGL call takes this long */
static int64_t extra_us_next;    /* and the next one this much more, once */
static size_t psram_free = 4u * 1024 * 1024;

const lv_font_t lv_font_montserrat_14 = {"montserrat_14"};
const lv_font_t lv_font_montserrat_16 = {"montserrat_16"};
const lv_font_t lv_font_montserrat_20 = {"montserrat_20"};
const lv_font_t lv_font_montserrat_28 = {"montserrat_28"};
const lv_font_t lv_font_unscii_8 = {"unscii_8"};
const lv_font_t lv_font_unscii_16 = {"unscii_16"};
const lv_font_t apps_digits_48 = {"apps_digits_48"};
const lv_font_t apps_digits_96 = {"apps_digits_96"};

static void fake_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fake_error(const char *fmt, ...) {
    if (fake_errors++ == 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(fake_note, sizeof fake_note, fmt, ap);
        va_end(ap);
    }
}

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char line[400];
    snprintf(line, sizeof line, "%c %s: %s\n", level, tag, msg);
    tb_append(&esp_log_text, line, strlen(line));
}

size_t heap_caps_get_free_size(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) != 0 ? psram_free : 0;
}

int64_t esp_timer_get_time(void) {
    return clock_us;
}

/* Every LVGL call costs what the test says it does. */
static void tick(void) {
    lvgl_calls++;
    clock_us += call_cost_us + extra_us_next;
    extra_us_next = 0;
}

/* The object a call is made on, or NULL (and an error) when it isn't a live one of class cls (NULL: any). */
static lv_obj_t *use(lv_obj_t *o, const char *fn, const char *cls) {
    tick();
    if (apply_active && !lock_held) {
        fake_error("%s was called inside the apply step without the UI lock", fn);
    }
    if (o == NULL) {
        fake_error("%s(NULL)", fn);
        return NULL;
    }
    if (o->deleted) {
        fake_error("%s on the deleted object %d", fn, o->id);
        return NULL;
    }
    if (cls != NULL && strcmp(o->cls, cls) != 0) {
        fake_error("%s on %s %d, a %s call", fn, o->cls, o->id, cls);
        return NULL;
    }
    return o;
}

static const char *align_name(lv_align_t a) {
    switch (a) {
        case LV_ALIGN_DEFAULT: return "default";
        case LV_ALIGN_TOP_LEFT: return "top_left";
        case LV_ALIGN_TOP_MID: return "top_mid";
        case LV_ALIGN_TOP_RIGHT: return "top_right";
        case LV_ALIGN_BOTTOM_LEFT: return "bottom_left";
        case LV_ALIGN_BOTTOM_MID: return "bottom_mid";
        case LV_ALIGN_BOTTOM_RIGHT: return "bottom_right";
        case LV_ALIGN_LEFT_MID: return "left_mid";
        case LV_ALIGN_RIGHT_MID: return "right_mid";
        case LV_ALIGN_CENTER: return "center";
    }
    return "?";
}

static const char *flag_name(lv_obj_flag_t f) {
    switch (f) {
        case LV_OBJ_FLAG_HIDDEN: return "hidden";
        case LV_OBJ_FLAG_CLICKABLE: return "clickable";
        case LV_OBJ_FLAG_SCROLLABLE: return "scrollable";
        case LV_OBJ_FLAG_GESTURE_BUBBLE: return "gesture_bubble";
    }
    return "?";
}

static const char *code_name(lv_event_code_t c) {
    switch (c) {
        case LV_EVENT_ALL: return "all";
        case LV_EVENT_PRESSED: return "pressed";
        case LV_EVENT_PRESSING: return "pressing";
        case LV_EVENT_PRESS_LOST: return "press_lost";
        case LV_EVENT_SHORT_CLICKED: return "short_clicked";
        case LV_EVENT_SINGLE_CLICKED: return "single_clicked";
        case LV_EVENT_DOUBLE_CLICKED: return "double_clicked";
        case LV_EVENT_TRIPLE_CLICKED: return "triple_clicked";
        case LV_EVENT_LONG_PRESSED: return "long_pressed";
        case LV_EVENT_LONG_PRESSED_REPEAT: return "long_pressed_repeat";
        case LV_EVENT_CLICKED: return "clicked";
        case LV_EVENT_RELEASED: return "released";
        case LV_EVENT_GESTURE: return "gesture";
        default: break;
    }
    return "?";
}

static const char *part_suffix(lv_style_selector_t sel) {
    switch (sel) {
        case LV_PART_MAIN: return "";
        case LV_PART_INDICATOR: return " indicator";
        case LV_PART_KNOB: return " knob";
        default: return " ?part";
    }
}

static void coord_text(char *out, size_t cap, int32_t v) {
    if (v == LV_SIZE_CONTENT) {
        snprintf(out, cap, "content");
    } else if ((v & (1 << 29)) != 0) {
        snprintf(out, cap, "%d%%", (int)(v & ((1 << 29) - 1)));
    } else {
        snprintf(out, cap, "%d", (int)v);
    }
}

lv_color_t lv_color_hex(uint32_t c) {
    lv_color_t col = {(uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF)};
    return col;
}

int32_t lv_pct(int32_t x) {
    return x | (1 << 29);
}

static uint32_t rgb(lv_color_t c) {
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

static lv_obj_t *make_object(const char *cls, lv_obj_t *parent, const char *fn) {
    tick();
    if (apply_active && !lock_held) {
        fake_error("%s was called inside the apply step without the UI lock", fn);
    }
    if (parent == NULL || parent->deleted) {
        fake_error("%s under %s", fn, parent == NULL ? "NULL" : "a deleted object");
        return NULL;
    }
    if (strcmp(parent->cls, "screen") != 0 && strcmp(parent->cls, "obj") != 0 && strcmp(parent->cls, "button") != 0) {
        fake_error("%s under a %s, which holds no children here", fn, parent->cls);
        return NULL;
    }
    if (n_objs >= FAKE_OBJS_MAX) {
        fputs("the fake LVGL ran out of objects\n", stderr);
        exit(2);
    }
    lv_obj_t *o = calloc(1, sizeof *o);
    if (o == NULL) {
        exit(2);
    }
    o->id = ++next_id;
    o->cls = cls;
    o->parent = parent;
    o->w = o->h = LV_SIZE_CONTENT;
    o->align = LV_ALIGN_DEFAULT;
    o->scale = 256;
    o->area = (lv_area_t){0, 0, -1, -1};
    /* The flags LVGL 9.5.0's constructors leave: lv_obj is clickable and scrollable, a label and an image are
     * not clickable, and a button, an arc and a bar are not scrollable. */
    o->flags = LV_OBJ_FLAG_GESTURE_BUBBLE;
    if (strcmp(cls, "label") != 0 && strcmp(cls, "image") != 0) {
        o->flags |= LV_OBJ_FLAG_CLICKABLE;
    }
    if (strcmp(cls, "obj") == 0 || strcmp(cls, "label") == 0 || strcmp(cls, "image") == 0) {
        o->flags |= LV_OBJ_FLAG_SCROLLABLE;
    }
    objs[n_objs++] = o;
    char where[16];
    if (strcmp(parent->cls, "screen") == 0) {
        snprintf(where, sizeof where, "screen");
    } else {
        snprintf(where, sizeof where, "%d", parent->id);
    }
    logline("create %s %d in %s", cls, o->id, where);
    return o;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent) {
    return make_object("obj", parent, "lv_obj_create");
}

lv_obj_t *lv_label_create(lv_obj_t *parent) {
    lv_obj_t *o = make_object("label", parent, "lv_label_create");
    if (o != NULL) {
        o->text = strdup("Text");  /* LVGL's LV_LABEL_DEFAULT_TEXT */
    }
    return o;
}

lv_obj_t *lv_button_create(lv_obj_t *parent) {
    return make_object("button", parent, "lv_button_create");
}

lv_obj_t *lv_arc_create(lv_obj_t *parent) {
    lv_obj_t *o = make_object("arc", parent, "lv_arc_create");
    if (o != NULL) {
        o->max = 100;
    }
    return o;
}

lv_obj_t *lv_bar_create(lv_obj_t *parent) {
    lv_obj_t *o = make_object("bar", parent, "lv_bar_create");
    if (o != NULL) {
        o->max = 100;
    }
    return o;
}

lv_obj_t *lv_image_create(lv_obj_t *parent) {
    return make_object("image", parent, "lv_image_create");
}

static void mark_deleted(lv_obj_t *o) {
    o->deleted = true;
    for (int i = 0; i < n_objs; i++) {
        if (objs[i]->parent == o && !objs[i]->deleted) {
            mark_deleted(objs[i]);
        }
    }
}

void lv_obj_delete(lv_obj_t *obj) {
    if (use(obj, "lv_obj_delete", NULL) == NULL) {
        return;
    }
    if (strcmp(obj->cls, "screen") == 0) {
        fake_error("lv_obj_delete of the screen");
        return;
    }
    logline("delete %d", obj->id);
    mark_deleted(obj);
}

void lv_obj_clean(lv_obj_t *obj) {
    if (use(obj, "lv_obj_clean", NULL) == NULL) {
        return;
    }
    logline("clean %d", obj->id);
    for (int i = 0; i < n_objs; i++) {
        if (objs[i]->parent == obj && !objs[i]->deleted) {
            mark_deleted(objs[i]);
        }
    }
}

void lv_obj_remove_style(lv_obj_t *obj, const lv_style_t *style, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_remove_style", NULL) == NULL) {
        return;
    }
    if (style != NULL) {
        fake_error("lv_obj_remove_style with a style: the fake only knows to remove a part's");
    }
    logline("remove_style %d%s", obj->id, part_suffix(selector));
}

void lv_obj_remove_style_all(lv_obj_t *obj) {
    if (use(obj, "lv_obj_remove_style_all", NULL) == NULL) {
        return;
    }
    logline("remove_style_all %d", obj->id);
}

void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t f) {
    if (use(obj, "lv_obj_add_flag", NULL) == NULL) {
        return;
    }
    obj->flags |= (uint32_t)f;
    logline("add_flag %d %s", obj->id, flag_name(f));
}

void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t f) {
    if (use(obj, "lv_obj_remove_flag", NULL) == NULL) {
        return;
    }
    obj->flags &= ~(uint32_t)f;
    logline("remove_flag %d %s", obj->id, flag_name(f));
}

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h) {
    if (use(obj, "lv_obj_set_size", NULL) == NULL) {
        return;
    }
    char a[24], b[24];
    coord_text(a, sizeof a, w);
    coord_text(b, sizeof b, h);
    if ((w != LV_SIZE_CONTENT && (w & (1 << 29)) == 0 && w < 0) || (h != LV_SIZE_CONTENT && (h & (1 << 29)) == 0 && h < 0)) {
        fake_error("lv_obj_set_size(%s, %s) on %d", a, b, obj->id);
    }
    obj->w = w;
    obj->h = h;
    logline("size %d %s %s", obj->id, a, b);
}

void lv_obj_align(lv_obj_t *obj, lv_align_t align, int32_t x_ofs, int32_t y_ofs) {
    if (use(obj, "lv_obj_align", NULL) == NULL) {
        return;
    }
    if ((int)align < LV_ALIGN_DEFAULT || (int)align > LV_ALIGN_CENTER) {
        fake_error("lv_obj_align with %d", (int)align);
    }
    obj->align = align;
    obj->x = x_ofs;
    obj->y = y_ofs;
    logline("align %d %s %d %d", obj->id, align_name(align), (int)x_ofs, (int)y_ofs);
}

/* ---- The fake layout: enough to place objects by their alignment and size ---- */

static int32_t px_of(int32_t v, int32_t whole, int32_t content) {
    if (v == LV_SIZE_CONTENT) {
        return content;
    }
    if ((v & (1 << 29)) != 0) {
        return whole * (v & ((1 << 29) - 1)) / 100;
    }
    return v;
}

static void content_size(lv_obj_t *o, int32_t *w, int32_t *h) {
    *w = *h = 0;
    if (strcmp(o->cls, "label") == 0) {
        *w = (int32_t)strlen(o->text != NULL ? o->text : "") * 10;
        *h = 20;
    } else if (strcmp(o->cls, "button") == 0) {
        for (int i = 0; i < n_objs; i++) {
            if (objs[i]->parent == o && !objs[i]->deleted) {
                content_size(objs[i], w, h);
                break;
            }
        }
    } else if (strcmp(o->cls, "image") == 0 && o->src != NULL) {
        *w = (int32_t)((uint64_t)o->src->header.w * o->scale / 256);
        *h = (int32_t)((uint64_t)o->src->header.h * o->scale / 256);
    }
}

static void layout_one(lv_obj_t *o) {
    if (o->forced) {
        return;
    }
    const lv_area_t *pa = &o->parent->area;
    int32_t pw = pa->x2 - pa->x1 + 1, ph = pa->y2 - pa->y1 + 1, cw, ch;
    content_size(o, &cw, &ch);
    int32_t w = px_of(o->w, pw, cw), h = px_of(o->h, ph, ch);
    int32_t x1 = pa->x1, y1 = pa->y1;
    switch (o->align) {
        case LV_ALIGN_DEFAULT:
        case LV_ALIGN_TOP_LEFT: break;
        case LV_ALIGN_TOP_MID: x1 += (pw - w) / 2; break;
        case LV_ALIGN_TOP_RIGHT: x1 += pw - w; break;
        case LV_ALIGN_BOTTOM_LEFT: y1 += ph - h; break;
        case LV_ALIGN_BOTTOM_MID: x1 += (pw - w) / 2; y1 += ph - h; break;
        case LV_ALIGN_BOTTOM_RIGHT: x1 += pw - w; y1 += ph - h; break;
        case LV_ALIGN_LEFT_MID: y1 += (ph - h) / 2; break;
        case LV_ALIGN_RIGHT_MID: x1 += pw - w; y1 += (ph - h) / 2; break;
        case LV_ALIGN_CENTER: x1 += (pw - w) / 2; y1 += (ph - h) / 2; break;
    }
    x1 += o->x;
    y1 += o->y;
    o->area = (lv_area_t){x1, y1, x1 + w - 1, y1 + h - 1};
}

void lv_obj_update_layout(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_update_layout", NULL) == NULL) {
        return;
    }
    logline("update_layout %d", obj->id);
    for (int i = 0; i < n_objs; i++) {
        if (!objs[i]->deleted) {
            layout_one(objs[i]);
        }
    }
}

void lv_obj_get_coords(const lv_obj_t *obj, lv_area_t *coords) {
    if (use((lv_obj_t *)obj, "lv_obj_get_coords", NULL) == NULL) {
        return;
    }
    if (coords == NULL) {
        fake_error("lv_obj_get_coords with no area");
        return;
    }
    *coords = obj->area;
}

/* ---- Styles ---- */

static void style_log(lv_obj_t *obj, const char *prop, lv_style_selector_t sel, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void style_log(lv_obj_t *obj, const char *prop, lv_style_selector_t sel, const char *fmt, ...) {
    char value[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof value, fmt, ap);
    va_end(ap);
    logline("%s %d %s%s", prop, obj->id, value, part_suffix(sel));
}

void lv_obj_set_style_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_opa", NULL) != NULL) {
        style_log(obj, "opa", selector, "%u", (unsigned)value);
    }
}

void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_bg_color", NULL) != NULL) {
        style_log(obj, "bg_color", selector, "#%06x", (unsigned)rgb(value));
    }
}

void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_bg_opa", NULL) != NULL) {
        style_log(obj, "bg_opa", selector, "%u", (unsigned)value);
    }
}

void lv_obj_set_style_radius(lv_obj_t *obj, int32_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_radius", NULL) != NULL) {
        if (value < 0) {
            fake_error("a negative radius %d on %d", (int)value, obj->id);
        }
        style_log(obj, "radius", selector, "%d", (int)value);
    }
}

void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_border_width", NULL) != NULL) {
        style_log(obj, "border_width", selector, "%d", (int)value);
    }
}

void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_border_color", NULL) != NULL) {
        style_log(obj, "border_color", selector, "#%06x", (unsigned)rgb(value));
    }
}

void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_text_font", NULL) != NULL) {
        if (value == NULL) {
            fake_error("a NULL font on %d", obj->id);
            return;
        }
        style_log(obj, "text_font", selector, "%s", value->name);
    }
}

void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_text_color", NULL) != NULL) {
        style_log(obj, "text_color", selector, "#%06x", (unsigned)rgb(value));
    }
}

void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_text_align", NULL) != NULL) {
        static const char *const names[] = {"auto", "left", "center", "right"};
        if ((unsigned)value >= ARRAY_LEN(names)) {
            fake_error("text_align %d on %d", (int)value, obj->id);
            return;
        }
        style_log(obj, "text_align", selector, "%s", names[value]);
    }
}

void lv_obj_set_style_arc_width(lv_obj_t *obj, int32_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_arc_width", NULL) != NULL) {
        style_log(obj, "arc_width", selector, "%d", (int)value);
    }
}

void lv_obj_set_style_arc_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_arc_color", NULL) != NULL) {
        style_log(obj, "arc_color", selector, "#%06x", (unsigned)rgb(value));
    }
}

/* ---- Widgets ---- */

void lv_label_set_text(lv_obj_t *obj, const char *text) {
    if (use(obj, "lv_label_set_text", "label") == NULL) {
        return;
    }
    if (text == NULL) {
        fake_error("lv_label_set_text(%d, NULL): LVGL asserts", obj->id);
        return;
    }
    free(obj->text);
    obj->text = strdup(text);
    logline("text %d \"%s\"", obj->id, text);
}

void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t long_mode) {
    if (use(obj, "lv_label_set_long_mode", "label") == NULL) {
        return;
    }
    const char *name = long_mode == LV_LABEL_LONG_MODE_WRAP ? "wrap" : long_mode == LV_LABEL_LONG_MODE_CLIP ? "clip" : "other";
    logline("long_mode %d %s", obj->id, name);
}

void lv_arc_set_rotation(lv_obj_t *obj, int32_t rotation) {
    if (use(obj, "lv_arc_set_rotation", "arc") != NULL) {
        logline("arc_rotation %d %d", obj->id, (int)rotation);
    }
}

void lv_arc_set_bg_angles(lv_obj_t *obj, lv_value_precise_t start, lv_value_precise_t end) {
    if (use(obj, "lv_arc_set_bg_angles", "arc") != NULL) {
        if (start < 0 || start > 360 || end < 0 || end > 360) {
            fake_error("arc angles %d %d on %d", (int)start, (int)end, obj->id);
        }
        logline("arc_bg_angles %d %d %d", obj->id, (int)start, (int)end);
    }
}

/* As LVGL's: a value is clamped to the range there is when it is set, and a new range clamps the value there is. */
static int32_t clamp_to(int32_t v, int32_t min, int32_t max) {
    return v > max ? max : v < min ? min : v;
}

void lv_arc_set_range(lv_obj_t *obj, int32_t min, int32_t max) {
    if (use(obj, "lv_arc_set_range", "arc") != NULL) {
        if (min >= max) {
            fake_error("arc range %d..%d on %d: LVGL divides by the span", (int)min, (int)max, obj->id);
        }
        obj->min = min;
        obj->max = max;
        obj->value = clamp_to(obj->value, min, max);
        logline("arc_range %d %d %d", obj->id, (int)min, (int)max);
    }
}

void lv_arc_set_value(lv_obj_t *obj, int32_t value) {
    if (use(obj, "lv_arc_set_value", "arc") != NULL) {
        obj->value = clamp_to(value, obj->min, obj->max);
        logline("arc_value %d %d", obj->id, (int)value);
    }
}

void lv_bar_set_range(lv_obj_t *obj, int32_t min, int32_t max) {
    if (use(obj, "lv_bar_set_range", "bar") != NULL) {
        if (min >= max) {
            fake_error("bar range %d..%d on %d: LVGL divides by the span", (int)min, (int)max, obj->id);
        }
        obj->min = min;
        obj->max = max;
        obj->value = clamp_to(obj->value, min, max);
        logline("bar_range %d %d %d", obj->id, (int)min, (int)max);
    }
}

void lv_bar_set_value(lv_obj_t *obj, int32_t value, lv_anim_enable_t anim) {
    if (use(obj, "lv_bar_set_value", "bar") != NULL) {
        obj->value = clamp_to(value, obj->min, obj->max);
        logline("bar_value %d %d anim=%s", obj->id, (int)value, anim ? "on" : "off");
    }
}

void lv_image_set_src(lv_obj_t *obj, const void *src) {
    if (use(obj, "lv_image_set_src", "image") == NULL) {
        return;
    }
    const lv_image_dsc_t *dsc = src;
    if (dsc == NULL || dsc->header.magic != 0x19 || dsc->data == NULL || dsc->header.w == 0 || dsc->header.h == 0 ||
        dsc->header.stride == 0) {
        fake_error("lv_image_set_src(%d) with something that is not an image descriptor", obj->id);
        return;
    }
    obj->src = dsc;
    logline("image_src %d %ux%u cf=0x%02x data=%u", obj->id, (unsigned)dsc->header.w, (unsigned)dsc->header.h,
         (unsigned)dsc->header.cf, (unsigned)dsc->data_size);
}

void lv_image_set_scale(lv_obj_t *obj, uint32_t zoom) {
    if (use(obj, "lv_image_set_scale", "image") == NULL) {
        return;
    }
    if (zoom == 0) {
        fake_error("lv_image_set_scale(%d, 0)", obj->id);
        return;
    }
    obj->scale = zoom;
    logline("image_scale %d %u", obj->id, (unsigned)zoom);
}

/* ---- Events and the input device ---- */

void *lv_event_get_user_data(lv_event_t *e) {
    return e->ud;
}

lv_event_code_t lv_event_get_code(lv_event_t *e) {
    return e->code;
}

void lv_event_stop_processing(lv_event_t *e) {
    e->stopped = true;
}

lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t event_cb, lv_event_code_t filter, void *user_data) {
    if (use(obj, "lv_obj_add_event_cb", NULL) == NULL) {
        return NULL;
    }
    if (event_cb == NULL || obj->n_cbs >= (int)ARRAY_LEN(obj->cbs)) {
        fake_error("lv_obj_add_event_cb on %d with cb %p, %d callbacks", obj->id, (void *)(uintptr_t)event_cb, obj->n_cbs);
        return NULL;
    }
    obj->cbs[obj->n_cbs++] = (fake_cb_t){event_cb, filter, user_data};
    logline("add_event_cb %d %s%s", obj->id, code_name((lv_event_code_t)(filter & ~LV_EVENT_PREPROCESS)),
         (filter & LV_EVENT_PREPROCESS) != 0 ? " preprocess" : "");
    return (lv_event_dsc_t *)&obj->cbs[obj->n_cbs - 1];
}

lv_indev_t *lv_indev_active(void) {
    return indev_present ? &indev_obj : NULL;
}

lv_dir_t lv_indev_get_gesture_dir(const lv_indev_t *indev) {
    if (indev == NULL) {
        fake_error("lv_indev_get_gesture_dir(NULL)");
        return LV_DIR_NONE;
    }
    return gesture_dir;
}

void lv_indev_wait_release(lv_indev_t *indev) {
    if (indev == NULL) {
        fake_error("lv_indev_wait_release(NULL)");
        return;
    }
    logline("wait_release");
}

/* ---- What the tests do to the fake ---- */

static lv_obj_t *fake_obj(int id) {
    for (int i = 0; i < n_objs; i++) {
        if (objs[i]->id == id) {
            return objs[i];
        }
    }
    fprintf(stderr, "no fake object %d\n", id);
    exit(2);
}

/* The id of the nth (from 0) object of class cls ever made, deleted ones too; -1 when there is none. */
static int fake_nth(const char *cls, int n) {
    for (int i = 0; i < n_objs; i++) {
        if (strcmp(objs[i]->cls, cls) == 0 && n-- == 0) {
            return objs[i]->id;
        }
    }
    return -1;
}

/* Every object is dead or alive as the fake sees it. */
static int fake_alive(void) {
    int n = 0;
    for (int i = 0; i < n_objs; i++) {
        n += objs[i]->deleted ? 0 : 1;
    }
    return n;
}

static int fake_alive_of(const char *cls) {
    int n = 0;
    for (int i = 0; i < n_objs; i++) {
        n += !objs[i]->deleted && strcmp(objs[i]->cls, cls) == 0 ? 1 : 0;
    }
    return n;
}

static bool fake_flag(int id, lv_obj_flag_t f) {
    return (fake_obj(id)->flags & (uint32_t)f) != 0;
}

/* An object's area as a test wants it, whatever the layout says. */
static void fake_place(int id, int x1, int y1, int x2, int y2) {
    lv_obj_t *o = fake_obj(id);
    o->area = (lv_area_t){x1, y1, x2, y2};
    o->forced = true;
}

static int arc_drags;  /* times the fake arc's own handler turned its value to a finger */

/* Sends an event the way LVGL does, to a deleted object too (an event that was already on its way when the object
 * went): the callbacks flagged PREPROCESS first, then the class's own handler (an arc follows a finger that
 * presses it, and that is all the fake gives a class), then the other callbacks. A callback that stops the
 * processing ends it. */
static void fake_fire(int id, lv_event_code_t code) {
    lv_obj_t *o = fake_obj(id);
    lv_event_t e = {code, NULL, o, false};
    for (int stage = 0; stage < 3 && !e.stopped; stage++) {
        if (stage == 1) {
            if (strcmp(o->cls, "arc") == 0 && code == LV_EVENT_PRESSING) {
                o->value++;
                arc_drags++;
            }
            continue;
        }
        for (int i = 0; i < o->n_cbs && !e.stopped; i++) {
            bool preprocess = (o->cbs[i].filter & LV_EVENT_PREPROCESS) != 0;
            lv_event_code_t filter = (lv_event_code_t)(o->cbs[i].filter & ~LV_EVENT_PREPROCESS);
            if (preprocess == (stage == 0) && (filter == LV_EVENT_ALL || filter == code)) {
                e.ud = o->cbs[i].ud;
                o->cbs[i].cb(&e);
            }
        }
    }
}

static void fake_free_all(void) {
    for (int i = 0; i < n_objs; i++) {
        free(objs[i]->text);
        free(objs[i]);
        objs[i] = NULL;
    }
    n_objs = 0;
    next_id = 0;
    memset(&screen_obj, 0, sizeof screen_obj);
    screen_obj.cls = "screen";
    screen_obj.area = (lv_area_t){0, 0, 465, 465};
    indev_present = true;
    gesture_dir = LV_DIR_NONE;
}

/* A fresh LVGL: nothing made, the log empty, the clock and the PSRAM as they start. Errors are not cleared: a
 * case reports them at its end. */
static void fake_reset(void) {
    fake_free_all();
    log_clear();
    esp_clear();
    call_cost_us = 0;
    extra_us_next = 0;
    psram_free = 4u * 1024 * 1024;
}

static lv_obj_t *fake_screen(void) {
    return &screen_obj;
}

/* ---- An environment: a VM with `ui` in it, its table, and the layer on the fake screen ---- */

/* Valid LVGL v9 .bin images, as apps_image_check wants them: an RGB565 one of 2x1 and an A8 one of 3x2. */
static const uint8_t IMG_A[] = {0x19, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
static const uint8_t IMG_B[] = {0x19, 0x0E, 0, 0, 3, 0, 2, 0, 4, 0, 0, 0, 1, 2, 3, 0, 4, 5, 6, 0};
static const apps_asset_t ASSETS[] = {{"a.bin", IMG_A, sizeof IMG_A, 2, 1}, {"b.bin", IMG_B, sizeof IMG_B, 3, 2}};

typedef struct {
    apps_vm_t *vm;
    apps_ui_t *ui;
} env_t;

/* False when the heap refused (what was made is still there for env_close). */
static bool env_open(env_t *e, const apps_asset_t *assets, int n_assets) {
    apps_vm_config_t cfg = {.mem_limit = 1024 * 1024,
                            .realloc_fn = apps_port_realloc,
                            .free_fn = apps_port_free,
                            .now_ms = apps_port_mono_ms};
    e->vm = apps_vm_new(&cfg);
    e->ui = apps_ui_new();
    if (e->vm == NULL || e->ui == NULL) {
        return false;
    }
    apps_ui_set_assets(e->ui, assets, n_assets);
    apps_ui_open(apps_vm_lua(e->vm), e->ui);
    return true;
}

static void env_close(env_t *e) {
    apps_vm_free(e->vm);  /* the VM first: no handle is left to use the table */
    apps_ui_free(e->ui);
    e->vm = NULL;
    e->ui = NULL;
}

static apps_widget_t *W(env_t *e, int slot) {
    return apps_ui_slot(e->ui, slot);
}

static apps_lv_stats_t stats_base;

static apps_lv_stats_t stats_now(void) {
    apps_lv_stats_t s;
    apps_lv_stats(&s);
    return s;
}

/* The stats since the scene began. */
static apps_lv_stats_t stats_since(void) {
    apps_lv_stats_t s = stats_now();
    s.applies -= stats_base.applies;
    s.skips -= stats_base.skips;
    s.rejects -= stats_base.rejects;
    s.refused_creates -= stats_base.refused_creates;
    s.slow -= stats_base.slow;
    return s;
}

static void after(int line, env_t *e) {
    (void)e;
    CHECK_AT(line, !lock_held && lock_errors == 0, "the lock: held %d, %d errors, the first: %s", lock_held,
             lock_errors, lock_note);
    CHECK_AT(line, fake_errors == 0, "the fake LVGL counted %d errors, the first: %s", fake_errors, fake_note);
}

static bool apply_at(int line, env_t *e, bool in_callback) {
    apply_active = true;
    bool applied = apps_lv_apply(e->ui, in_callback);
    apply_active = false;
    after(line, e);
    return applied;
}
#define APPLY(e) apply_at(__LINE__, (e), false)
#define APPLY_CB(e) apply_at(__LINE__, (e), true)

/* A fresh fake LVGL and a layer on its screen, an app's table with two images, and the first pass done (which
 * takes the table's background), so the log is empty and a case's first pass is its own. */
static void scene_raw(env_t *e) {
    fake_reset();
    refuse_from = -1;
    trylock_refusals = 0;
    CHECK(env_open(e, ASSETS, 2), "env_open");
    apps_lv_build(fake_screen());
    log_clear();
    stats_base = stats_now();
}

static void scene(env_t *e) {
    scene_raw(e);
    apply_at(__LINE__, e, false);
    log_clear();
    stats_base = stats_now();
}

/* The end of a case: the layer let go, the table gone, nothing held. */
static void finish_at(int line, env_t *e) {
    apps_lv_reset();
    env_close(e);
    CHECK_AT(line, held == 0, "%zu bytes still held", held);
    CHECK_AT(line, fake_errors == 0, "the fake LVGL counted %d errors, the first: %s", fake_errors, fake_note);
    CHECK_AT(line, !lock_held && lock_errors == 0, "the lock: held %d, %d errors, the first: %s", lock_held,
             lock_errors, lock_note);
    CHECK_AT(line, lock_blocking == 0, "apps_port_ui_lock was called %ld times inside the apply step: it only tries the lock",
             lock_blocking);
}
#define FINISH(e) finish_at(__LINE__, (e))

/* ---- Running Lua ---- */

/* An exact-size heap copy of src[0, len): a read past it is an AddressSanitizer report. */
static char *exact_copy(const char *src, size_t len) {
    char *p = malloc(len != 0 ? len : 1);
    if (p == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    memcpy(p, src, len);
    return p;
}

/* Runs src as a chunk with a call's budget. True when it ran; else err has the message and the traceback. */
static bool run(env_t *e, const char *src, char *err, size_t err_len) {
    size_t len = strlen(src);
    char *copy = exact_copy(src, len);
    char load[256];
    bool loaded = apps_vm_load(e->vm, "t.lua", copy, len, load, sizeof load);
    free(copy);
    if (!loaded) {
        snprintf(err, err_len, "load: %s", load);
        return false;
    }
    return apps_vm_call(e->vm, 0, 0, APPS_BUDGET_CALL, err, err_len) == APPS_CALL_OK;
}

static void run_ok_at(int line, env_t *e, const char *src) {
    char err[512] = "";
    CHECK_AT(line, run(e, src, err, sizeof err), "'%s' failed: %s", src, err);
    after(line, e);
}
#define RUN(e, src) run_ok_at(__LINE__, (e), (src))

/* ---- Events from the layer ---- */

typedef struct {
    uint16_t slot, gen;
    uint8_t event;
} ev_t;

static ev_t evs[256];
static int n_evs;

static void on_event(uint16_t slot, uint16_t gen, uint8_t event) {
    if (n_evs < (int)ARRAY_LEN(evs)) {
        evs[n_evs++] = (ev_t){slot, gen, event};
    }
}

static int closes;

static void on_close(void) {
    closes++;
}

/* What apps_rt_touch does with an event, for one touch: apps_ui_push_handler inside a call, and the handler it
 * pushes called. result: whether a handler ran. */
typedef struct {
    apps_ui_t *ui;
    uint16_t slot, gen;
    uint8_t event;
    bool result;
} probe_t;

static int probe_trampoline(lua_State *L) {
    probe_t *p = lua_touserdata(L, 1);
    p->result = apps_ui_push_handler(L, p->ui, p->slot, p->gen, p->event);
    if (p->result) {
        lua_call(L, 2, 0);
    }
    return 0;
}

static bool dispatch(env_t *e, const ev_t *ev) {
    probe_t p = {.ui = e->ui, .slot = ev->slot, .gen = ev->gen, .event = ev->event};
    lua_State *L = apps_vm_lua(e->vm);
    char err[256] = "";
    lua_pushcfunction(L, probe_trampoline);
    lua_pushlightuserdata(L, &p);
    apps_call_t status = apps_vm_call(e->vm, 1, 0, APPS_BUDGET_CALL, err, sizeof err);
    CHECK(status == APPS_CALL_OK, "dispatch: %s", err);
    return p.result;
}

/* ---- The brief's tests ---- */

static void create_update_delete_sequence(void) {
    env_t e;
    scene_raw(&e);
    RUN(&e, "a = ui.label{text = '5:00', y = -35, font = 'sans28'}");
    CHECK(APPLY(&e), "the first pass ran");
    LOG_IS(
        "bg_color 1 #000000\n"
        "create label 2 in 1\n"
        "remove_style_all 2\n"
        "remove_flag 2 scrollable\n"
        "remove_flag 2 clickable\n"
        "size 2 content content\n"
        "align 2 center 0 -35\n"
        "remove_flag 2 hidden\n"
        "opa 2 255\n"
        "text_font 2 montserrat_28\n"
        "text_color 2 #ffffff\n"
        "text_align 2 center\n"
        "long_mode 2 clip\n"
        "text 2 \"5:00\"\n"
        "update_layout 1\n");
    apps_widget_t *w = W(&e, 0);
    CHECK(w->dirty == 0 && w->alive, "the slot's bits are cleared: %u", w->dirty);
    /* Written back: the fake lays "5:00" out as 40x20, centred 35 px above the middle. */
    CHECK(w->rx == 213 && w->ry == 188 && w->rw == 40 && w->rh == 20 && w->overflow == 0, "geometry %d %d %d %d, overflow %d",
          w->rx, w->ry, w->rw, w->rh, w->overflow);

    /* Nothing changed: the pass takes the lock, finds nothing, and calls LVGL not at all. */
    CHECK(APPLY(&e), "an idle pass runs");
    LOG_EMPTY();

    RUN(&e, "a:set{text = '4:59'}");
    CHECK(APPLY(&e), "the second pass ran");
    LOG_IS(
        "text 2 \"4:59\"\n"
        "update_layout 1\n");

    RUN(&e, "a:set{text = '4:59'}");  /* the same text sets no bit */
    CHECK(APPLY(&e), "the third pass ran");
    LOG_EMPTY();

    RUN(&e, "a:delete()");
    CHECK(APPLY(&e), "the fourth pass ran");
    LOG_IS(
        "delete 2\n"
        "update_layout 1\n");
    CHECK(W(&e, 0)->dirty == 0 && !W(&e, 0)->alive, "the delete is cleared: %u", W(&e, 0)->dirty);
    CHECK(fake_alive() == 1, "only the layer is left: %d", fake_alive());
    apps_lv_stats_t s = stats_since();
    CHECK(s.applies == 5 && s.skips == 0 && s.rejects == 0 && s.refused_creates == 0 && s.slow == 0,
          "applies %u skips %u rejects %u refused %u slow %u", s.applies, s.skips, s.rejects, s.refused_creates, s.slow);
    FINISH(&e);
}

/* ---- Re-validation ---- */

/* Gives the widget a text block of its own, from the port's heap (apps_ui_free frees it). */
static void poke_text(apps_widget_t *w, const char *bytes, size_t len) {
    char *block = apps_port_realloc(NULL, len + 1);
    CHECK(block != NULL, "a block of %zu", len + 1);
    if (block == NULL) {
        return;
    }
    memcpy(block, bytes, len);
    block[len] = '\0';
    apps_port_free(w->text);
    w->text = block;
}

typedef struct {
    const char *why;
    const char *lua;     /* makes the widgets: the one that is corrupted has slot `slot` */
    int slot;
    void (*poke)(env_t *e, apps_widget_t *w);
    int creates;         /* the objects the other widgets still make: "create" lines in the log */
    int rejects;         /* the slots the pass drops */
} bad_t;

#define POKE(name, stmt)                              \
    static void name(env_t *e, apps_widget_t *w) {    \
        (void)e;                                      \
        (void)w;                                      \
        stmt;                                         \
    }

POKE(kind_past_the_last, w->kind = APPS_W_IMAGE + 1)
POKE(kind_255, w->kind = 255)
POKE(align_past_the_last, w->align = 9)
POKE(opacity_101, w->opacity = 101)
POKE(x_467, w->x = 467)
POKE(y_minus_467, w->y = -467)
POKE(w_zero, w->w = 0)
POKE(w_467, w->w = 467)
POKE(h_minus_2, w->h = -2)
POKE(font_8, w->font = 8)
POKE(text_align_3, w->text_align = 3)
POKE(color_past_white, w->color = 0x1000000)
POKE(events_16, w->events = 16)
POKE(text_257, {
    char big[257];
    memset(big, 'a', sizeof big);
    poke_text(w, big, sizeof big);
})
POKE(text_not_utf8, poke_text(w, "ab\xff" "cd", 5))
POKE(text_cut_utf8, poke_text(w, "ab\xe2\x82", 4))
POKE(text_overlong_utf8, poke_text(w, "\xc0\xaf", 2))
POKE(radius_234, w->radius = 234)
POKE(radius_minus_2, w->radius = -2)
POKE(border_21, w->border = 21)
POKE(border_color_past_white, w->border_color = 0x1000000)
POKE(bg_past_white, w->bg = 0x1000000)
POKE(arc_min_is_max, {
    w->min = 5;
    w->max = 5;
    w->value = 5;
})
POKE(arc_min_past_max, {
    w->min = 50;
    w->max = 10;
    w->value = 20;
})
POKE(value_below_min, w->value = w->min - 1)
POKE(value_above_max, w->value = w->max + 1)
POKE(max_over_the_range, w->max = APPS_UI_VALUE_MAX + 1)
POKE(min_under_the_range, w->min = -APPS_UI_VALUE_MAX - 1)
POKE(end_angle_361, w->end_angle = 361)
POKE(start_angle_minus_1, w->start_angle = -1)
POKE(thickness_0, w->thickness = 0)
POKE(thickness_65, w->thickness = 65)
POKE(track_color_past_white, w->track_color = 0x1000000)
POKE(image_minus_1, w->image = -1)
POKE(image_past_the_table, w->image = 2)
POKE(scale_0, w->scale = 0)
POKE(scale_9, w->scale = 9)
POKE(scale_401, w->scale = 401)
POKE(parent_a_label, w->parent = 1)                  /* slot 1 is a label */
POKE(parent_itself, w->parent = 0)
POKE(parent_past_the_table, w->parent = APPS_UI_SLOTS)
POKE(parent_minus_2, w->parent = -2)
POKE(parent_dead, {
    W(e, 1)->alive = false;
    W(e, 1)->dirty = 0;
})
POKE(parent_five_boxes_up, w->parent = 4)            /* box 4 has four boxes above it: five in all */
POKE(parent_four_boxes_up, w->parent = 3)            /* four: the most there may be */
POKE(parent_loop, W(e, 0)->parent = 1)               /* 0 inside 1 inside 0 */
POKE(parent_kind_changed, W(e, 0)->kind = APPS_W_LABEL)

static const bad_t BAD[] = {
    {"a kind past the last", "w = ui.label{text = 'hi'}", 0, kind_past_the_last, 0, 1},
    {"kind 255", "w = ui.arc{}", 0, kind_255, 0, 1},
    {"an alignment past the last", "w = ui.label{}", 0, align_past_the_last, 0, 1},
    {"opacity 101", "w = ui.box{}", 0, opacity_101, 0, 1},
    {"x 467", "w = ui.bar{}", 0, x_467, 0, 1},
    {"y -467", "w = ui.image{src = 'a.bin'}", 0, y_minus_467, 0, 1},
    {"a width of 0", "w = ui.label{}", 0, w_zero, 0, 1},
    {"a width of 467", "w = ui.box{}", 0, w_467, 0, 1},
    {"a height of -2", "w = ui.button{}", 0, h_minus_2, 0, 1},
    {"font 8", "w = ui.label{}", 0, font_8, 0, 1},
    {"font 8 on a button", "w = ui.button{text = 'go'}", 0, font_8, 0, 1},
    {"text_align 3", "w = ui.label{}", 0, text_align_3, 0, 1},
    {"a colour past white", "w = ui.label{}", 0, color_past_white, 0, 1},
    {"an events mask of 16", "w = ui.label{}", 0, events_16, 0, 1},
    {"a text of 257 bytes", "w = ui.label{}", 0, text_257, 0, 1},
    {"a button's text of 257 bytes", "w = ui.button{}", 0, text_257, 0, 1},
    {"text that isn't UTF-8", "w = ui.label{}", 0, text_not_utf8, 0, 1},
    {"text cut inside a character", "w = ui.label{}", 0, text_cut_utf8, 0, 1},
    {"an overlong UTF-8 form", "w = ui.button{}", 0, text_overlong_utf8, 0, 1},
    {"a radius of 234", "w = ui.button{}", 0, radius_234, 0, 1},
    {"a radius of -2", "w = ui.box{}", 0, radius_minus_2, 0, 1},
    {"a border of 21", "w = ui.box{}", 0, border_21, 0, 1},
    {"a border colour past white", "w = ui.button{}", 0, border_color_past_white, 0, 1},
    {"a background past white", "w = ui.box{}", 0, bg_past_white, 0, 1},
    {"an arc whose min is its max", "w = ui.arc{}", 0, arc_min_is_max, 0, 1},
    {"an arc whose min is past its max", "w = ui.arc{}", 0, arc_min_past_max, 0, 1},
    {"a bar whose min is its max", "w = ui.bar{}", 0, arc_min_is_max, 0, 1},
    {"a bar whose min is past its max", "w = ui.bar{}", 0, arc_min_past_max, 0, 1},
    {"an arc's value below its min", "w = ui.arc{}", 0, value_below_min, 0, 1},
    {"an arc's value above its max", "w = ui.arc{value = 50}", 0, value_above_max, 0, 1},
    {"a bar's value below its min", "w = ui.bar{}", 0, value_below_min, 0, 1},
    {"a bar's value above its max", "w = ui.bar{}", 0, value_above_max, 0, 1},
    {"a max over the range LVGL can map", "w = ui.arc{}", 0, max_over_the_range, 0, 1},
    {"a min under it", "w = ui.bar{}", 0, min_under_the_range, 0, 1},
    {"an end angle of 361", "w = ui.arc{}", 0, end_angle_361, 0, 1},
    {"a start angle of -1", "w = ui.arc{}", 0, start_angle_minus_1, 0, 1},
    {"a thickness of 0", "w = ui.arc{}", 0, thickness_0, 0, 1},
    {"a thickness of 65", "w = ui.arc{}", 0, thickness_65, 0, 1},
    {"a track colour past white", "w = ui.bar{}", 0, track_color_past_white, 0, 1},
    {"an image index of -1", "w = ui.image{src = 'a.bin'}", 0, image_minus_1, 0, 1},
    {"an image index past the table", "w = ui.image{src = 'b.bin'}", 0, image_past_the_table, 0, 1},
    {"a scale of 0", "w = ui.image{src = 'a.bin'}", 0, scale_0, 0, 1},
    {"a scale of 9", "w = ui.image{src = 'a.bin'}", 0, scale_9, 0, 1},
    {"a scale of 401", "w = ui.image{src = 'a.bin'}", 0, scale_401, 0, 1},
    {"a parent that is a label", "b = ui.box{} l = ui.label{} w = ui.label{parent = b}", 2, parent_a_label, 2, 1},
    {"a parent that is itself", "w = ui.box{}", 0, parent_itself, 0, 1},
    {"a parent past the table", "w = ui.label{}", 0, parent_past_the_table, 0, 1},
    {"a parent of -2", "w = ui.label{}", 0, parent_minus_2, 0, 1},
    {"a parent that is not alive", "l = ui.label{} b = ui.box{} w = ui.label{parent = b}", 2, parent_dead, 1, 1},
    {"five boxes above it", "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
                            "b4 = ui.box{parent = b3} w = ui.label{}", 5, parent_five_boxes_up, 5, 1},
    {"a parent chain that loops", "b0 = ui.box{} b1 = ui.box{parent = b0}", 1, parent_loop, 0, 2},
    {"a box that became a label under a child", "b = ui.box{} w = ui.label{parent = b}", 1, parent_kind_changed, 1, 1},
};

static void revalidation_drops_bad_slot(void) {
    for (size_t i = 0; i < ARRAY_LEN(BAD); i++) {
        const bad_t *bad = &BAD[i];
        env_t e;
        scene(&e);
        RUN(&e, bad->lua);
        bad->poke(&e, W(&e, bad->slot));
        apps_lv_stats_t before = stats_now();
        CHECK(APPLY(&e), "%s: the pass ran", bad->why);
        apps_lv_stats_t after_stats = stats_now();
        CHECK(after_stats.rejects - before.rejects == (uint32_t)bad->rejects, "%s: %u rejects, want %d", bad->why,
              after_stats.rejects - before.rejects, bad->rejects);
        CHECK(W(&e, bad->slot)->dirty == 0, "%s: the slot's bits are %u", bad->why, W(&e, bad->slot)->dirty);
        CHECK(log_count("create ") == bad->creates, "%s: %d objects made, want %d\n%s", bad->why, log_count("create "),
              bad->creates, log_peek());
        if (bad->creates == 0) {
            LOG_EMPTY();  /* no LVGL call for it at all: not a style, not a size, not the layout */
        } else {
            log_clear();
        }
        FINISH(&e);
    }
}

/* A chain of four boxes above a widget is the most there may be: the same table with one box fewer is fine. */
static void parent_chain_of_four_is_drawn(void) {
    env_t e;
    scene(&e);
    RUN(&e, "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
            "b4 = ui.box{parent = b3} w = ui.label{}");
    parent_four_boxes_up(&e, W(&e, 5));
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 6 && stats_since().rejects == 0, "six objects, no reject\n%s", log_peek());
    CHECK(log_has_line("create label 7 in 5"), "the label is inside box 3 (object 5)\n%s", log_peek());
    FINISH(&e);
}

/* A slot that is fine when it is made and bad afterwards: no call, its bits are dropped, its object stays. */
static void revalidation_after_the_object_exists(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    CHECK(APPLY(&e), "made");
    log_clear();
    W(&e, 0)->font = 8;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE | APPS_DIRTY_GEOM;
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(stats_since().rejects == 1 && W(&e, 0)->dirty == 0, "rejects %u dirty %u", stats_since().rejects, W(&e, 0)->dirty);
    CHECK(fake_alive_of("label") == 1, "the object is still there");
    /* Put right, it is drawn again. */
    W(&e, 0)->font = APPS_FONT_SANS16;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_has_line("text_font 2 montserrat_16") && stats_since().rejects == 1, "drawn again\n%s", log_peek());
    FINISH(&e);
}

/* The reject is logged once per app, counted every time. */
static void reject_logged_once_per_app(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{} b = ui.label{}");
    W(&e, 0)->font = 9;
    W(&e, 1)->font = 9;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_since().rejects == 2, "two rejects");
    CHECK(esp_count("W muse_apps:") == 1, "one line in the log\n%s", esp_peek());
    const char *line = strstr(esp_peek(), "W muse_apps:");
    CHECK(line != NULL && strchr(line, '\n') != NULL, "a line");
    esp_clear();
    log_clear();
    W(&e, 0)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_since().rejects == 3 && esp_count("W") == 0, "a third, not logged again\n%s", esp_peek());
    /* Another app: the first reject of it is logged again. */
    apps_lv_reset();
    env_close(&e);
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "a = ui.label{}");
    W(&e, 0)->font = 9;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_since().rejects == 4 && esp_count("W muse_apps:") == 1, "logged again\n%s", esp_peek());
    FINISH(&e);
}

/* NULL text is "": a label made with no text is made, and a set to "Paused" is applied. */
static void empty_text_is_made_and_set(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{}");
    CHECK(W(&e, 0)->text == NULL, "the table keeps no block for \"\"");
    CHECK(APPLY(&e), "made");
    CHECK(log_has_line("text 2 \"\"") && stats_since().rejects == 0, "text \"\" is set, not rejected\n%s", log_peek());
    log_clear();
    RUN(&e, "a:set{text = 'Paused'}");
    CHECK(APPLY(&e), "set");
    LOG_IS(
        "text 2 \"Paused\"\n"
        "update_layout 1\n");
    RUN(&e, "a:set{text = ''}");
    CHECK(APPLY(&e), "cleared");
    LOG_IS(
        "text 2 \"\"\n"
        "update_layout 1\n");
    /* A button the same, in its label. */
    RUN(&e, "b = ui.button{}");
    CHECK(APPLY(&e), "a button with no text");
    CHECK(log_has_line("text 4 \"\"") && stats_since().rejects == 0, "the button's label\n%s", log_peek());
    /* The longest text, and every length of character. */
    char src[400];
    char big[257];
    memset(big, 'z', 256);
    big[256] = '\0';
    snprintf(src, sizeof src, "a:set{text = '%s'}", big);
    log_clear();
    RUN(&e, src);
    CHECK(APPLY(&e), "256 bytes");
    CHECK(log_find("text 2 \"zzzz") == 0 && stats_since().rejects == 0, "256 bytes are fine\n%.60s", log_peek());
    RUN(&e, "a:set{text = 'a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80'}");
    log_clear();
    CHECK(APPLY(&e), "multi-byte characters");
    CHECK(log_has_line("text 2 \"a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"") && stats_since().rejects == 0, "UTF-8 of every length\n%s",
          log_peek());
    FINISH(&e);
}

/* ---- The PSRAM floor ---- */

static void psram_floor_refuses_creates(void) {
    env_t e;
    scene(&e);
    psram_free = 200 * 1024;
    RUN(&e, "a = ui.label{text = 'x'}");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 0, "nothing is made at 200 KB\n%s", log_peek());
    LOG_EMPTY();
    CHECK(stats_since().refused_creates == 1 && stats_since().rejects == 0, "refused %u rejects %u",
          stats_since().refused_creates, stats_since().rejects);
    /* The bits stay, and the slot comes again. */
    uint32_t bits = APPS_DIRTY_CREATE | APPS_DIRTY_GEOM | APPS_DIRTY_STYLE | APPS_DIRTY_TEXT;
    CHECK(W(&e, 0)->dirty == bits, "the bits are %u, want %u", W(&e, 0)->dirty, bits);
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_since().refused_creates == 2 && W(&e, 0)->dirty == bits, "refused again: %u", stats_since().refused_creates);
    psram_free = 300 * 1024;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create label 2 in 1") == 1 && W(&e, 0)->dirty == 0 && stats_since().refused_creates == 2,
          "made at 300 KB\n%s", log_peek());
    log_clear();

    /* Only a create is refused: a widget that is there is still changed when the memory is low. */
    psram_free = 100 * 1024;
    RUN(&e, "a:set{text = 'y'}");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_has_line("text 2 \"y\"") && stats_since().refused_creates == 2, "the set is applied\n%s", log_peek());
    log_clear();

    /* The floor is APPS_LV_MIN_PSRAM: under it refuses, at it makes. */
    RUN(&e, "b = ui.box{}");
    psram_free = APPS_LV_MIN_PSRAM - 1;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 0 && stats_since().refused_creates == 3, "one byte under: refused\n%s", log_peek());
    psram_free = APPS_LV_MIN_PSRAM;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create obj 3 in 1") == 1 && stats_since().refused_creates == 3, "at the floor: made\n%s", log_peek());
    log_clear();

    /* A box and what is in it: both are refused, both are counted, and each is made once there is room. */
    RUN(&e, "c = ui.box{} d = ui.label{parent = c}");
    psram_free = 100 * 1024;
    uint32_t refused = stats_since().refused_creates;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 0 && stats_since().refused_creates == refused + 2, "two refused: %u\n%s",
          stats_since().refused_creates - refused, log_peek());
    psram_free = 4u * 1024 * 1024;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 2 && W(&e, 2)->dirty == 0 && W(&e, 3)->dirty == 0, "both made\n%s", log_peek());
    FINISH(&e);
}

/* A widget made and deleted while its create is refused leaves nothing for the pass: it had no object. */
static void create_pending_has_no_object(void) {
    env_t e;
    scene(&e);
    psram_free = 100 * 1024;
    RUN(&e, "a = ui.label{text = 'x'}");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_since().refused_creates == 1, "refused once");
    RUN(&e, "a:delete()");
    CHECK(W(&e, 0)->dirty == 0 && !W(&e, 0)->alive, "Task 9 frees the slot at once, without DELETE: %u", W(&e, 0)->dirty);
    psram_free = 4u * 1024 * 1024;
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();  /* not made, not deleted */
    /* The slot is free at once, and the next widget in it is made as a new one. */
    RUN(&e, "b = ui.label{text = 'y'}");
    CHECK(W(&e, 0)->alive && W(&e, 0)->gen == 1 && W(&e, 0)->dirty != 0, "the slot lives again");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create label 2 in 1") == 1 && log_has_line("text 2 \"y\""), "made fresh\n%s", log_peek());
    log_clear();

    /* A refused create keeps no pointer: the object that gets made later is the slot's one object. */
    psram_free = 100 * 1024;
    RUN(&e, "c = ui.label{text = 'z'}");
    CHECK(APPLY(&e), "the pass ran");
    RUN(&e, "b:delete()");
    psram_free = 4u * 1024 * 1024;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("delete 2") == 1 && log_count("create label 3 in 1") == 1, "slot 0 deleted, slot 1 made\n%s", log_peek());
    CHECK(fake_alive_of("label") == 1, "one label is alive");
    FINISH(&e);
}

/* ---- Frames: the lock and the callback ---- */

static void skips_mid_callback_at_most_three_frames(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    long trylocks = trylock_calls;
    for (int frame = 1; frame <= 3; frame++) {
        CHECK(!APPLY_CB(&e), "frame %d is skipped", frame);
    }
    CHECK(stats_since().skips == 3 && stats_since().applies == 0, "skips %u applies %u", stats_since().skips, stats_since().applies);
    LOG_EMPTY();
    CHECK(W(&e, 0)->dirty != 0 && trylock_calls == trylocks, "a skipped frame reads and changes nothing, and doesn't ask for the lock: bits %u, %ld trylocks",
          W(&e, 0)->dirty, trylock_calls - trylocks);
    CHECK(APPLY_CB(&e), "the 4th frame applies anyway");
    CHECK(log_count("create label 2 in 1") == 1 && stats_since().applies == 1 && stats_since().skips == 3, "applied\n%s", log_peek());
    log_clear();

    /* The run starts over after an apply: three more skipped, then one applied. */
    RUN(&e, "a:set{text = 'y'}");
    CHECK(!APPLY_CB(&e) && !APPLY_CB(&e) && !APPLY_CB(&e), "three skipped again");
    CHECK(APPLY_CB(&e) && log_has_line("text 2 \"y\""), "and the fourth applies\n%s", log_peek());
    CHECK(stats_since().skips == 6 && stats_since().applies == 2, "skips %u applies %u", stats_since().skips, stats_since().applies);
    log_clear();

    /* A frame outside a callback is never skipped, and it ends the run. */
    RUN(&e, "a:set{text = 'z'}");
    CHECK(!APPLY_CB(&e) && !APPLY_CB(&e), "two skipped");
    CHECK(APPLY(&e) && log_has_line("text 2 \"z\""), "a frame outside a callback applies at once\n%s", log_peek());
    log_clear();
    RUN(&e, "a:set{text = 'w'}");
    CHECK(!APPLY_CB(&e) && !APPLY_CB(&e) && !APPLY_CB(&e), "the run starts over: three skipped");
    CHECK(APPLY_CB(&e) && log_has_line("text 2 \"w\""), "and the fourth applies\n%s", log_peek());
    log_clear();

    /* The 4th frame that can't get the lock is a skip too, but the run is over: the next frame applies. */
    RUN(&e, "a:set{text = 'v'}");
    CHECK(!APPLY_CB(&e) && !APPLY_CB(&e) && !APPLY_CB(&e), "three skipped");
    trylock_refusals = 1;
    uint32_t skips = stats_since().skips;
    CHECK(!APPLY_CB(&e), "the lock is not free at the 4th frame");
    CHECK(stats_since().skips == skips + 1, "a skip, counted");
    CHECK(APPLY_CB(&e) && log_has_line("text 2 \"v\""), "the next frame applies at once\n%s", log_peek());
    FINISH(&e);
}

static void lock_discipline(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'} b = ui.arc{} c = ui.button{text = 'go'}");
    long calls = trylock_calls, takes = lock_takes;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(trylock_calls == calls + 1 && lock_takes == takes + 1 && trylock_ms == 2, "one trylock of 2 ms: %ld calls, %d ms",
          trylock_calls - calls, trylock_ms);
    CHECK(lock_blocking == 0, "the blocking lock was never taken");
    CHECK(!lock_held, "and it is given back");

    /* A lock that isn't free: nothing is read, nothing is called, and the bits wait. */
    RUN(&e, "a:set{text = 'y'}");
    log_clear();
    long before = lvgl_calls;
    trylock_refusals = 1;
    CHECK(!APPLY(&e), "the pass is skipped");
    CHECK(lvgl_calls == before && W(&e, 0)->dirty == APPS_DIRTY_TEXT && stats_since().skips == 1, "calls %ld bits %u skips %u",
          lvgl_calls - before, W(&e, 0)->dirty, stats_since().skips);
    CHECK(!lock_held, "nothing was left locked");
    CHECK(APPLY(&e) && log_has_line("text 2 \"y\""), "the next frame applies it\n%s", log_peek());

    /* No table, no layer: not a pass, and not a skip. */
    apply_active = true;
    CHECK(!apps_lv_apply(NULL, false), "no table");
    apply_active = false;
    CHECK(stats_since().skips == 1 && stats_since().applies == 2, "not counted: skips %u applies %u", stats_since().skips, stats_since().applies);
    FINISH(&e);
}

/* Before the layer is built there is nothing to draw on: no pass, no lock, no call, and no skip. */
static void no_layer_no_pass(void) {
    env_t e;
    fake_reset();
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "a = ui.label{}");
    long trylocks = trylock_calls;
    CHECK(!APPLY(&e), "no layer: no pass");
    CHECK(trylock_calls == trylocks && lvgl_calls == 0 && W(&e, 0)->dirty != 0, "nothing was asked, called or changed");
    apps_lv_stats_t s = stats_now();
    CHECK(s.applies == 0 && s.skips == 0, "not a skip: %u %u", s.applies, s.skips);
    apps_lv_set_front(true);
    CHECK(!apps_lv_front(), "nothing to bring to the front");
    apps_lv_reset();  /* harmless */
    apps_lv_stats(NULL);
    FINISH(&e);
}

/* ---- Round-screen feedback ---- */

/* The overflow of a widget made by `make`, whose object is then given the area x1,y1..x2,y2 (inclusive, LVGL's). */
static int overflow_with(const char *make, int x1, int y1, int x2, int y2, apps_widget_t *out) {
    env_t e;
    scene(&e);
    char src[300];
    snprintf(src, sizeof src, "w = %s", make);
    RUN(&e, src);
    CHECK(APPLY(&e), "made");
    fake_place(2, x1, y1, x2, y2);
    RUN(&e, "w:set{x = 1}");  /* any change: the pass lays the layer out again and writes the geometry back */
    CHECK(APPLY(&e), "laid out");
    int overflow = W(&e, 0)->overflow;
    if (out != NULL) {
        *out = *W(&e, 0);
    }
    FINISH(&e);
    return overflow;
}

static void overflow_from_coords(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'Time up', align = 'top_left'}");
    CHECK(APPLY(&e), "made");
    fake_place(2, 0, 0, 99, 39);  /* 100x40 in the screen's corner */
    RUN(&e, "a:set{x = 1}");
    CHECK(APPLY(&e), "laid out");
    apps_widget_t *w = W(&e, 0);
    CHECK(w->rx == 0 && w->ry == 0 && w->rw == 100 && w->rh == 40, "geometry %d %d %d %d", w->rx, w->ry, w->rw, w->rh);
    /* The corner (0, 0) is 329.5 from the centre: 96.5 past the edge, so 97 px. */
    CHECK(w->overflow == 97, "overflow %d", w->overflow);
    char dump[512];
    int dropped = -1;
    size_t n = apps_ui_dump(e.ui, dump, sizeof dump, &dropped);
    CHECK(n > 0 && dropped == 0, "dump %zu dropped %d", n, dropped);
    CHECK(strstr(dump, "\"x\":0,\"y\":0,\"w\":100,\"h\":40,\"hidden\":false,\"overflow\":97") != NULL, "the dump: %s", dump);

    /* Fully inside: none. */
    fake_place(2, 183, 213, 282, 252);
    RUN(&e, "a:set{x = 2}");
    CHECK(APPLY(&e), "laid out");
    CHECK(W(&e, 0)->overflow == 0, "inside: %d", W(&e, 0)->overflow);
    /* A hidden widget is not drawn, so it has no overflow; shown again, it has. */
    fake_place(2, 0, 0, 99, 39);
    RUN(&e, "a:set{hidden = true}");
    CHECK(APPLY(&e), "hidden");
    CHECK(W(&e, 0)->overflow == 0 && W(&e, 0)->rw == 100, "hidden: overflow %d, rw %d", W(&e, 0)->overflow, W(&e, 0)->rw);
    RUN(&e, "a:set{hidden = false}");
    CHECK(APPLY(&e), "shown");
    CHECK(W(&e, 0)->overflow == 97, "shown: %d", W(&e, 0)->overflow);
    FINISH(&e);
}

static void overflow_corner_rule(void) {
    /* The corner rule: the largest distance of a corner from (233, 233), less 233, to the nearest px. The corners are
     * the rectangle's edges: an area of 100 px from x1 = 0 ends at 100. */
    static const struct {
        const char *what;
        const char *make;
        int x1, y1, x2, y2, want;
    } cases[] = {
        {"a label in the corner: (0, 0) is 329.5 away", "ui.label{text = 'x'}", 0, 0, 99, 39, 97},
        {"a label inside: (283, 253) is 53.9 away", "ui.label{text = 'x'}", 183, 213, 282, 252, 0},
        {"a box over the screen's square", "ui.box{}", 0, 0, 465, 465, 97},
        {"a box 466x200 with its corners rounded less than half: (0, 133) is 253.5 away", "ui.box{radius = 99}", 0, 133, 465, 332, 21},
        {"a small box in the middle: 141.4 away", "ui.box{radius = 5}", 133, 133, 332, 332, 0},
        {"a button at the right edge: (466, 273) is 236.4 away", "ui.button{text = 'go'}", 366, 233, 465, 272, 3},
        {"a bar across the middle: (0, 226) is 233.1 away, which is no px", "ui.bar{}", 0, 226, 465, 236, 0},
        {"an image in the corner of the square: (466, 466) is 329.5 away", "ui.image{src = 'a.bin'}", 400, 400, 465, 465, 97},
        {"a label with no width draws nothing", "ui.label{text = 'x'}", 233, 233, 232, 232, 0},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        int got = overflow_with(cases[i].make, cases[i].x1, cases[i].y1, cases[i].x2, cases[i].y2, NULL);
        CHECK(got == cases[i].want, "%s: overflow %d, want %d", cases[i].what, got, cases[i].want);
    }
}

static void overflow_round_shapes(void) {
    static const struct {
        const char *what;
        const char *make;
        int x1, y1, x2, y2, want;
    } cases[] = {
        /* An arc is the circle of its box. */
        {"a centred 400x400 arc", "ui.arc{}", 33, 33, 432, 432, 0},
        {"a centred 480x480 arc", "ui.arc{}", -7, -7, 472, 472, 7},
        {"a centred 466x466 arc", "ui.arc{}", 0, 0, 465, 465, 0},
        {"an arc pushed 10 px right", "ui.arc{}", 10, 0, 475, 465, 10},
        {"a wide arc: the circle of its shorter side", "ui.arc{}", 0, 0, 465, 299, 0},
        {"a wide arc, off centre", "ui.arc{}", 0, -10, 465, 289, 10},
        /* A box with radius "circle". */
        {"a circle box 466x466", "ui.box{radius = 'circle'}", 0, 0, 465, 465, 0},
        {"a circle box 480x480", "ui.box{radius = 'circle'}", -7, -7, 472, 472, 7},
        {"a radius of half the side is a circle", "ui.box{radius = 233}", 0, 0, 465, 465, 0},
        /* A stadium: a segment along the long axis, and a circle of half the shorter side round it. */
        {"a stadium 466x200 across the middle", "ui.box{radius = 'circle'}", 0, 133, 465, 332, 0},
        {"a stadium 480x200", "ui.box{radius = 'circle'}", -7, 133, 472, 332, 7},
        {"a stadium 200x466 up and down", "ui.box{radius = 'circle'}", 133, 0, 332, 465, 0},
        {"a stadium 200x480", "ui.box{radius = 'circle'}", 133, -7, 332, 472, 7},
        {"a stadium pushed 5 px to one side", "ui.box{radius = 'circle'}", 5, 133, 470, 332, 5},
        {"a radius of half the shorter side is a stadium", "ui.box{radius = 100}", 0, 133, 465, 332, 0},
        {"a radius under half the shorter side keeps the corners", "ui.box{radius = 99}", 0, 133, 465, 332, 21},
        {"a box with radius 0 keeps the corners", "ui.box{}", 0, 0, 465, 465, 97},
        /* Under 2 px there is no corner to round: LVGL draws a rectangle, so the corner rule holds (a stadium would
         * say 96 here). */
        {"a box one px wide, however round", "ui.box{radius = 'circle'}", 0, 0, 0, 465, 97},
        {"a box one px high, however round", "ui.box{radius = 'circle'}", 0, 0, 465, 0, 97},
        /* The other kinds keep the corner rule, even with a radius that would round them. */
        {"a circle button", "ui.button{radius = 'circle', text = 'go'}", 0, 133, 465, 332, 21},
        {"a bar with a round end", "ui.bar{radius = 'circle'}", 0, 133, 465, 332, 21},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        int got = overflow_with(cases[i].make, cases[i].x1, cases[i].y1, cases[i].x2, cases[i].y2, NULL);
        CHECK(got == cases[i].want, "%s: overflow %d, want %d", cases[i].what, got, cases[i].want);
    }
}

/* A hidden box hides what is inside it. */
static void overflow_of_hidden_and_inside(void) {
    env_t e;
    scene(&e);
    RUN(&e, "b = ui.box{hidden = true} l = ui.label{text = 'x', parent = b} v = ui.label{text = 'y'}");
    CHECK(APPLY(&e), "made");
    fake_place(3, 0, 0, 99, 39);   /* the label inside the hidden box */
    fake_place(4, 0, 0, 99, 39);   /* a label that is shown */
    RUN(&e, "v:set{x = 1}");
    CHECK(APPLY(&e), "laid out");
    CHECK(W(&e, 1)->overflow == 0 && W(&e, 1)->rw == 100, "inside a hidden box: %d", W(&e, 1)->overflow);
    CHECK(W(&e, 2)->overflow == 97, "shown: %d", W(&e, 2)->overflow);
    RUN(&e, "b:set{hidden = false}");
    CHECK(APPLY(&e), "shown");
    CHECK(W(&e, 1)->overflow == 97, "the box shown: %d", W(&e, 1)->overflow);
    FINISH(&e);
}

/* The geometry of every widget is written back when any changes: a change in one place moves another. */
static void layout_written_back_for_every_widget(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'ab', align = 'top_left'} b = ui.label{text = 'cd', align = 'top_right'} "
            "c = ui.button{text = 'go', w = 100, h = 50, y = 100}");
    CHECK(APPLY(&e), "made");
    CHECK(W(&e, 0)->rx == 0 && W(&e, 0)->rw == 20 && W(&e, 0)->rh == 20, "a: %d %d", W(&e, 0)->rx, W(&e, 0)->rw);
    CHECK(W(&e, 1)->rx == 446 && W(&e, 1)->rw == 20, "b at the right: %d %d", W(&e, 1)->rx, W(&e, 1)->rw);
    CHECK(W(&e, 2)->rw == 100 && W(&e, 2)->rh == 50 && W(&e, 2)->rx == 183 && W(&e, 2)->ry == 308, "c: %d %d %d %d", W(&e, 2)->rx,
          W(&e, 2)->ry, W(&e, 2)->rw, W(&e, 2)->rh);
    /* b's text grows: its box grows to the left from the right edge; a's number is not touched by the pass over b. */
    W(&e, 0)->rx = 77;  /* what a stale write-back would leave */
    RUN(&e, "b:set{text = 'abcd'}");
    CHECK(APPLY(&e), "laid out");
    CHECK(W(&e, 1)->rx == 426 && W(&e, 1)->rw == 40, "b: %d %d", W(&e, 1)->rx, W(&e, 1)->rw);
    CHECK(W(&e, 0)->rx == 0, "a was written back too: %d", W(&e, 0)->rx);
    FINISH(&e);
}

/* ---- Events ---- */

static void has_at(int line, const char *text) {
    CHECK_AT(line, log_has_line(text), "the log lacks \"%s\":\n%s", text, log_peek());
}
#define HAS(text) has_at(__LINE__, (text))

static void event_after_delete_carries_stale_gen(void) {
    env_t e;
    scene(&e);
    apps_lv_set_event_cb(on_event);
    n_evs = 0;
    RUN(&e, "hits = 0 pad0 = ui.label{} pad1 = ui.label{} pad2 = ui.label{}\n"
            "b = ui.button{text = 'one'}\n"
            "b:on('tap', function(w, ev) hits = hits + 1 end)");
    CHECK(W(&e, 3)->kind == APPS_W_BUTTON && W(&e, 3)->gen == 0, "the button is in slot 3");
    CHECK(APPLY(&e), "made");
    int old_obj = fake_nth("button", 0);
    CHECK(old_obj > 0, "the button's object");
    CHECK(fake_obj(old_obj)->n_cbs == 4, "four callbacks: %d", fake_obj(old_obj)->n_cbs);
    CHECK(fake_obj(old_obj)->cbs[0].ud == (void *)(uintptr_t)3u, "the user data is slot | gen << 16: %p", fake_obj(old_obj)->cbs[0].ud);

    /* A tap on the live widget gets through. */
    fake_fire(old_obj, LV_EVENT_SHORT_CLICKED);
    CHECK(n_evs == 1 && evs[0].slot == 3 && evs[0].gen == 0 && evs[0].event == APPS_UI_TAP, "the event: %d", n_evs);
    CHECK(dispatch(&e, &evs[0]), "and the app's handler runs");
    RUN(&e, "assert(hits == 1, hits)");

    /* Deleted, and the slot made again on gen 1. */
    RUN(&e, "b:delete()");
    CHECK(APPLY(&e), "deleted");
    RUN(&e, "c = ui.button{text = 'two'} c:on('tap', function(w, ev) hits = hits + 10 end)");
    CHECK(W(&e, 3)->alive && W(&e, 3)->gen == 1, "the slot lives again on gen 1");
    CHECK(APPLY(&e), "made again");
    int new_obj = fake_nth("button", 1);
    CHECK(new_obj > old_obj && fake_obj(new_obj)->cbs[0].ud == (void *)(uintptr_t)(3u | 1u << 16), "the new object's user data: %p",
          fake_obj(new_obj)->cbs[0].ud);

    /* An event that was already on its way when the old object went: its callback is still called, with the gen it
     * was made for, and the app's side drops it. */
    n_evs = 0;
    fake_fire(old_obj, LV_EVENT_SHORT_CLICKED);
    CHECK(n_evs == 1 && evs[0].slot == 3 && evs[0].gen == 0 && evs[0].event == APPS_UI_TAP, "the stale event: %d, gen %d", n_evs,
          n_evs > 0 ? evs[0].gen : -1);
    CHECK(!dispatch(&e, &evs[0]), "Task 9's dispatch drops it");
    RUN(&e, "assert(hits == 1, hits)");
    n_evs = 0;
    fake_fire(new_obj, LV_EVENT_SHORT_CLICKED);
    CHECK(n_evs == 1 && evs[0].gen == 1 && dispatch(&e, &evs[0]), "the new widget's tap gets through");
    RUN(&e, "assert(hits == 11, hits)");
    FINISH(&e);
}

static void tap_events(void) {
    env_t e;
    scene(&e);
    apps_lv_set_event_cb(on_event);
    RUN(&e, "a = ui.box{w = 50, h = 50}\n"
            "a:on('tap', function() end) a:on('long_press', function() end)\n"
            "a:on('press', function() end) a:on('release', function() end)");
    CHECK(APPLY(&e), "made");
    HAS("add_flag 2 clickable");
    CHECK(log_find("add_event_cb 2 short_clicked") >= 0 && log_find("add_event_cb 2 short_clicked") < log_find("add_event_cb 2 long_pressed") &&
              log_find("add_event_cb 2 long_pressed") < log_find("add_event_cb 2 pressed") &&
              log_find("add_event_cb 2 pressed") < log_find("add_event_cb 2 released"),
          "one callback each for the four events, in the order of the table\n%s", log_peek());
    CHECK(log_count("add_event_cb") == 4, "four registrations\n%s", log_peek());
    log_clear();

    /* A long press, then LVGL's CLICKED on the release (it follows a long press too): the app hears long_press only. */
    n_evs = 0;
    fake_fire(2, LV_EVENT_PRESSED);
    fake_fire(2, LV_EVENT_LONG_PRESSED);
    fake_fire(2, LV_EVENT_CLICKED);
    fake_fire(2, LV_EVENT_RELEASED);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_LONG_PRESS && evs[2].event == APPS_UI_RELEASE,
          "press, long_press, release: %d events", n_evs);
    /* A short tap: SHORT_CLICKED is the tap. */
    n_evs = 0;
    fake_fire(2, LV_EVENT_PRESSED);
    fake_fire(2, LV_EVENT_SHORT_CLICKED);
    fake_fire(2, LV_EVENT_CLICKED);
    fake_fire(2, LV_EVENT_RELEASED);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_TAP && evs[2].event == APPS_UI_RELEASE,
          "press, tap, release: %d events", n_evs);
    CHECK(evs[1].slot == 0 && evs[1].gen == 0, "slot %d gen %d", evs[1].slot, evs[1].gen);
    /* The other events LVGL sends a clickable object are not the app's. */
    n_evs = 0;
    static const lv_event_code_t others[] = {LV_EVENT_PRESSING, LV_EVENT_PRESS_LOST, LV_EVENT_SINGLE_CLICKED, LV_EVENT_DOUBLE_CLICKED,
                                             LV_EVENT_TRIPLE_CLICKED, LV_EVENT_LONG_PRESSED_REPEAT, LV_EVENT_CLICKED,
                                             LV_EVENT_GESTURE, LV_EVENT_KEY};
    for (size_t i = 0; i < ARRAY_LEN(others); i++) {
        fake_fire(2, others[i]);
    }
    CHECK(n_evs == 0, "%d events from the others", n_evs);

    /* No event callback set: nowhere to send it, and no crash. */
    apps_lv_set_event_cb(NULL);
    fake_fire(2, LV_EVENT_SHORT_CLICKED);
    CHECK(n_evs == 0, "nowhere to send");
    FINISH(&e);
}

/* Every kind is clickable only while it has an event; what is clickable by default is made not to be. */
static void clickable_only_when_subscribed(void) {
    static const char *const kinds[] = {"label", "button", "box", "arc", "bar", "image"};
    static const char *const objects[] = {"label", "button", "obj", "arc", "bar", "image"};
    for (size_t i = 0; i < ARRAY_LEN(kinds); i++) {
        env_t e;
        scene(&e);
        char src[200];
        snprintf(src, sizeof src, "w = ui.%s{%s}", kinds[i], i == 5 ? "src = 'a.bin'" : "");
        RUN(&e, src);
        CHECK(APPLY(&e), "made");
        int id = fake_nth(objects[i], i == 2 ? 1 : 0);  /* the layer is the first lv_obj */
        CHECK(id > 0 && !fake_flag(id, LV_OBJ_FLAG_CLICKABLE), "%s: clickable with no event", kinds[i]);
        CHECK(!fake_flag(id, LV_OBJ_FLAG_SCROLLABLE), "%s: scrollable", kinds[i]);
        CHECK(fake_obj(id)->n_cbs == 0, "%s: callbacks with no event", kinds[i]);
        RUN(&e, "w:on('release', function() end)");
        CHECK(APPLY(&e), "subscribed");
        int callbacks = i == 3 ? 5 : 4;  /* an arc has a guard against being turned by a finger as well */
        CHECK(fake_flag(id, LV_OBJ_FLAG_CLICKABLE) && fake_obj(id)->n_cbs == callbacks, "%s: clickable %d with %d callbacks", kinds[i],
              fake_flag(id, LV_OBJ_FLAG_CLICKABLE), fake_obj(id)->n_cbs);
        RUN(&e, "w:on('release', nil)");
        CHECK(APPLY(&e), "unsubscribed");
        CHECK(!fake_flag(id, LV_OBJ_FLAG_CLICKABLE), "%s: still clickable", kinds[i]);
        RUN(&e, "w:on('tap', function() end)");
        CHECK(APPLY(&e), "subscribed again");
        CHECK(fake_flag(id, LV_OBJ_FLAG_CLICKABLE) && fake_obj(id)->n_cbs == callbacks, "%s: %d callbacks the second time", kinds[i],
              fake_obj(id)->n_cbs);
        FINISH(&e);
    }
}

/* An arc turns its value to a finger on it. One the app has subscribed to is clickable, so that would change what
 * the app set, behind its back: the guard stops it, and the events the app asked for still arrive. */
static void subscribed_arc_cannot_be_scrubbed(void) {
    env_t e;
    scene(&e);
    apps_lv_set_event_cb(on_event);
    n_evs = 0;
    arc_drags = 0;
    RUN(&e, "a = ui.arc{w = 100, h = 100, value = 40} a:on('tap', function() end)  b = ui.arc{w = 100, h = 100, value = 40}");
    CHECK(APPLY(&e), "made");
    int guarded = fake_nth("arc", 0), bare = fake_nth("arc", 1);
    HAS("add_event_cb 2 pressing preprocess");
    CHECK(fake_obj(guarded)->n_cbs == 5 && fake_obj(bare)->n_cbs == 0, "callbacks %d and %d", fake_obj(guarded)->n_cbs,
          fake_obj(bare)->n_cbs);
    for (int i = 0; i < 5; i++) {
        fake_fire(guarded, LV_EVENT_PRESSING);
    }
    CHECK(fake_obj(guarded)->value == 40 && arc_drags == 0 && n_evs == 0, "the guarded arc: value %d, dragged %d times, %d events",
          (int)fake_obj(guarded)->value, arc_drags, n_evs);
    /* The fake does turn an arc that nothing guards (so the check above says something). */
    fake_fire(bare, LV_EVENT_PRESSING);
    CHECK(fake_obj(bare)->value == 41 && arc_drags == 1, "the fake's arc follows a finger: %d", (int)fake_obj(bare)->value);
    /* A touch as the app asked for it. */
    fake_fire(guarded, LV_EVENT_PRESSED);
    fake_fire(guarded, LV_EVENT_SHORT_CLICKED);
    fake_fire(guarded, LV_EVENT_RELEASED);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_TAP && evs[2].event == APPS_UI_RELEASE,
          "the touches arrive: %d events", n_evs);
    FINISH(&e);
}

/* ---- Many sets, one apply ---- */

static void many_sets_while_dark_apply_once(void) {
    env_t e;
    scene(&e);
    /* A widget made, and 100 texts before the pass: one text call, the last. */
    RUN(&e, "a = ui.label{text = 'first'} for i = 1, 100 do a:set{text = 't' .. i} end");
    CHECK(APPLY(&e), "made");
    CHECK(log_count("text ") == 1 && log_has_line("text 2 \"t100\""), "one text call, with the last\n%s", log_peek());
    log_clear();
    /* A widget there already. */
    RUN(&e, "for i = 1, 100 do a:set{text = 'u' .. i} end");
    CHECK(APPLY(&e), "changed");
    LOG_IS(
        "text 2 \"u100\"\n"
        "update_layout 1\n");
    /* And all the other properties too: one call of each. */
    RUN(&e, "for i = 1, 100 do a:set{x = i, y = -i, w = 20 + i, color = i * 1000, font = i % 2 == 0 and 'sans14' or 'sans16'} end");
    CHECK(APPLY(&e), "changed");
    CHECK(log_count("size ") == 1 && log_count("align ") == 1 && log_count("text_color ") == 1 && log_count("text_font ") == 1,
          "one of each\n%s", log_peek());
    HAS("size 2 120 content");
    HAS("align 2 center 100 -100");
    HAS("text_font 2 montserrat_14");
    FINISH(&e);
}

/* ---- The swipe ---- */

static void swipe_right_closes(void) {
    fake_reset();
    apps_lv_build(fake_screen());
    LOG_IS(
        "create obj 1 in screen\n"
        "remove_style_all 1\n"
        "size 1 100% 100%\n"
        "bg_color 1 #000000\n"
        "bg_opa 1 255\n"
        "add_flag 1 clickable\n"
        "remove_flag 1 scrollable\n"
        "remove_flag 1 gesture_bubble\n"
        "add_event_cb 1 gesture\n"
        "add_flag 1 hidden\n");
    /* B13: the layer stops the gesture before it reaches the screen, and only then listens for it. */
    CHECK(!fake_flag(1, LV_OBJ_FLAG_GESTURE_BUBBLE) && fake_flag(1, LV_OBJ_FLAG_CLICKABLE) && !fake_flag(1, LV_OBJ_FLAG_SCROLLABLE) &&
              fake_flag(1, LV_OBJ_FLAG_HIDDEN),
          "the layer's flags");
    CHECK(!apps_lv_front(), "behind");

    closes = 0;
    apps_lv_set_close_cb(on_close);
    gesture_dir = LV_DIR_RIGHT;
    fake_fire(1, LV_EVENT_GESTURE);
    CHECK(closes == 1, "a right swipe closes: %d", closes);
    LOG_IS("wait_release\n");  /* the rest of the touch is not for whatever comes up under the layer */
    static const lv_dir_t others[] = {LV_DIR_LEFT, LV_DIR_TOP, LV_DIR_BOTTOM, LV_DIR_NONE};
    for (size_t i = 0; i < ARRAY_LEN(others); i++) {
        gesture_dir = others[i];
        fake_fire(1, LV_EVENT_GESTURE);
    }
    CHECK(closes == 1, "no other direction does: %d", closes);
    LOG_EMPTY();
    /* Other events on the layer are not gestures. */
    gesture_dir = LV_DIR_RIGHT;
    fake_fire(1, LV_EVENT_SHORT_CLICKED);
    fake_fire(1, LV_EVENT_PRESSED);
    CHECK(closes == 1, "a tap isn't a swipe");
    /* No input device (a gesture sent by code): nothing to ask, no crash. */
    indev_present = false;
    fake_fire(1, LV_EVENT_GESTURE);
    CHECK(closes == 1, "no device, no close");
    indev_present = true;
    /* No close callback: nothing happens. */
    apps_lv_set_close_cb(NULL);
    fake_fire(1, LV_EVENT_GESTURE);
    CHECK(closes == 1 && fake_errors == 0, "no callback: %s", fake_note);
    log_clear();
    apps_lv_reset();
    log_clear();
}

/* ---- Front and back, build and reset ---- */

static void front_and_back(void) {
    env_t e;
    scene(&e);
    CHECK(!apps_lv_front(), "the layer starts behind");
    apps_lv_set_front(true);
    LOG_IS("remove_flag 1 hidden\n");
    CHECK(apps_lv_front() && !fake_flag(1, LV_OBJ_FLAG_HIDDEN), "in front");
    apps_lv_set_front(false);
    LOG_IS("add_flag 1 hidden\n");
    CHECK(!apps_lv_front() && fake_flag(1, LV_OBJ_FLAG_HIDDEN), "behind");
    /* Behind, the pass still runs: the layout is there when the app comes to the front. */
    RUN(&e, "a = ui.label{text = 'x'}");
    CHECK(APPLY(&e) && log_count("create label") == 1 && W(&e, 0)->rw == 10, "applied while behind\n%s", log_peek());
    /* A reset leaves the layer where it was. */
    apps_lv_set_front(true);
    apps_lv_reset();
    CHECK(apps_lv_front() && !fake_flag(1, LV_OBJ_FLAG_HIDDEN), "reset doesn't hide the layer");
    FINISH(&e);
}

static void reset_deletes_every_object(void) {
    env_t e;
    scene(&e);
    RUN(&e, "ui.bg('#102030') b = ui.box{} l = ui.label{text = 'x', parent = b} c = ui.button{text = 'go'}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_alive() == 1 + 4, "the layer, a box, a label, a button and its label: %d", fake_alive());
    log_clear();
    size_t before = held;
    apps_lv_reset();
    LOG_IS(
        "clean 1\n"
        "bg_color 1 #000000\n");
    CHECK(fake_alive() == 1, "only the layer is left: %d", fake_alive());
    /* The pass's records go with them: what is held now is the VM's and the table's. */
    CHECK(held < before, "held %zu, then %zu", before, held);
    env_close(&e);
    CHECK(held == 0, "held %zu", held);
    /* The next app starts from nothing: its widgets are made new, in slots the last one had. */
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "a = ui.label{text = 'again'}");
    CHECK(APPLY(&e), "made");
    CHECK(log_count("create label 6 in 1") == 1 && log_has_line("bg_color 1 #000000") && log_has_line("text 6 \"again\""),
          "a new object, a new table's first take of the background\n%s", log_peek());
    /* Reset twice, and with nothing: harmless. */
    apps_lv_reset();
    apps_lv_reset();
    FINISH(&e);
}

static void stale_records_after_a_missed_reset(void) {
    /* A runtime freed without apps_lv_reset (it shouldn't be): the next table's creates replace what is in the slots,
     * and never touch a pointer the layer doesn't have. */
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'old'} b = ui.box{} c = ui.label{text = 'in', parent = b}");
    CHECK(APPLY(&e), "made");
    log_clear();
    env_close(&e);
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "x = ui.label{text = 'new'} y = ui.box{} z = ui.label{text = 'in', parent = y}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_errors == 0, "%s", fake_note);
    CHECK(log_count("delete ") == 2 && log_count("create ") == 3, "the old label and the old box deleted, three made\n%s", log_peek());
    /* The old box took its label with it: nothing was called on that label. */
    CHECK(fake_alive_of("label") == 2 && fake_alive_of("obj") == 2, "labels %d, boxes (the layer too) %d", fake_alive_of("label"),
          fake_alive_of("obj"));
    FINISH(&e);
}

static void build_makes_the_layer_again(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    CHECK(APPLY(&e), "made");
    /* A second layer (the screen was made again): the old objects belong to a screen that is gone. */
    fake_free_all();
    log_clear();
    apps_lv_build(fake_screen());
    CHECK(log_count("create obj 1 in screen") == 1, "the layer again\n%s", log_peek());
    log_clear();
    env_close(&e);
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "b = ui.label{text = 'y'}");
    CHECK(APPLY(&e), "made");
    CHECK(log_has_line("create label 2 in 1") && fake_errors == 0, "made on the new layer, no call on the old objects: %s", fake_note);
    FINISH(&e);
}

/* ---- Style, fonts and values ---- */

typedef struct {
    const char *lua;
    const char *lines[12];  /* what the log has, NULL at the end */
    const char *absent[3];  /* the line prefixes it doesn't have, NULL at the end */
} style_t;

static const style_t STYLES[] = {
    /* Fonts: the eight names, on a label each. */
    {"ui.label{font = 'sans14'}", {"text_font 2 montserrat_14", NULL}, {NULL}},
    {"ui.label{font = 'sans16'}", {"text_font 2 montserrat_16", NULL}, {NULL}},
    {"ui.label{font = 'sans20'}", {"text_font 2 montserrat_20", NULL}, {NULL}},
    {"ui.label{font = 'sans28'}", {"text_font 2 montserrat_28", NULL}, {NULL}},
    {"ui.label{font = 'mono8'}", {"text_font 2 unscii_8", NULL}, {NULL}},
    {"ui.label{font = 'mono16'}", {"text_font 2 unscii_16", NULL}, {NULL}},
    {"ui.label{font = 'digits48'}", {"text_font 2 apps_digits_48", NULL}, {NULL}},
    {"ui.label{font = 'digits96'}", {"text_font 2 apps_digits_96", NULL}, {NULL}},
    {"ui.label{}", {"text_font 2 montserrat_20", "text_color 2 #ffffff", "text_align 2 center", "long_mode 2 clip", NULL}, {NULL}},
    /* A button's font and colour are its label's. */
    {"ui.button{font = 'digits96', color = '#102030', text = '1'}",
     {"text_font 3 apps_digits_96", "text_color 3 #102030", "text 3 \"1\"", NULL}, {"text_font 2", "text_color 2", NULL}},
    /* Opacity: opacity * 255 / 100. */
    {"ui.box{opacity = 0}", {"opa 2 0", NULL}, {NULL}},
    {"ui.box{opacity = 1}", {"opa 2 2", NULL}, {NULL}},
    {"ui.box{opacity = 50}", {"opa 2 127", NULL}, {NULL}},
    {"ui.box{opacity = 99}", {"opa 2 252", NULL}, {NULL}},
    {"ui.box{opacity = 100}", {"opa 2 255", NULL}, {NULL}},
    {"ui.label{opacity = 40}", {"opa 2 102", NULL}, {NULL}},
    /* Shape and colour. */
    {"ui.box{radius = 'circle'}", {"radius 2 32767", NULL}, {NULL}},
    {"ui.box{radius = 0}", {"radius 2 0", NULL}, {NULL}},
    {"ui.box{radius = 233}", {"radius 2 233", NULL}, {NULL}},
    {"ui.box{bg = 'none'}", {"bg_opa 2 0", NULL}, {"bg_color 2", NULL}},
    {"ui.box{}", {"bg_opa 2 0", "radius 2 0", "border_width 2 0", "border_color 2 #ffffff", NULL}, {"bg_color 2", NULL}},
    {"ui.box{bg = '#112233', border = 4, border_color = 0xff0000}",
     {"bg_color 2 #112233", "bg_opa 2 255", "border_width 2 4", "border_color 2 #ff0000", NULL}, {NULL}},
    {"ui.button{}", {"bg_color 2 #262626", "bg_opa 2 255", "radius 2 16", "border_width 2 0", NULL}, {NULL}},
    {"ui.button{bg = 'none', radius = 'circle'}", {"bg_opa 2 0", "radius 2 32767", NULL}, {"bg_color 2", NULL}},
    /* Flags. */
    {"ui.label{hidden = true}", {"add_flag 2 hidden", NULL}, {"remove_flag 2 hidden", NULL}},
    {"ui.label{hidden = false}", {"remove_flag 2 hidden", NULL}, {"add_flag 2 hidden", NULL}},
    {"ui.label{wrap = true, w = 100}", {"long_mode 2 wrap", "size 2 100 content", NULL}, {"long_mode 2 clip", NULL}},
    {"ui.label{wrap = false}", {"long_mode 2 clip", NULL}, {"long_mode 2 wrap", NULL}},
    {"ui.label{text_align = 'left'}", {"text_align 2 left", NULL}, {NULL}},
    {"ui.label{text_align = 'right'}", {"text_align 2 right", NULL}, {NULL}},
    {"ui.label{color = 0x123456}", {"text_color 2 #123456", NULL}, {NULL}},
    /* Layout. */
    {"ui.label{align = 'top', x = 5, y = 6}", {"align 2 top_mid 5 6", NULL}, {NULL}},
    {"ui.label{align = 'bottom'}", {"align 2 bottom_mid 0 0", NULL}, {NULL}},
    {"ui.label{align = 'left'}", {"align 2 left_mid 0 0", NULL}, {NULL}},
    {"ui.label{align = 'right'}", {"align 2 right_mid 0 0", NULL}, {NULL}},
    {"ui.label{align = 'top_left'}", {"align 2 top_left 0 0", NULL}, {NULL}},
    {"ui.label{align = 'top_right'}", {"align 2 top_right 0 0", NULL}, {NULL}},
    {"ui.label{align = 'bottom_left'}", {"align 2 bottom_left 0 0", NULL}, {NULL}},
    {"ui.label{align = 'bottom_right'}", {"align 2 bottom_right 0 0", NULL}, {NULL}},
    {"ui.box{w = 100, h = 40}", {"size 2 100 40", NULL}, {NULL}},
    {"ui.box{w = 100}", {"size 2 100 content", NULL}, {NULL}},
    {"ui.box{h = 40}", {"size 2 content 40", NULL}, {NULL}},
    {"ui.box{}", {"size 2 content content", NULL}, {NULL}},
    /* Arcs and bars. */
    {"ui.arc{thickness = 20, color = '#ff0000', track_color = '#00ff00'}",
     {"arc_width 2 20", "arc_color 2 #00ff00", "arc_width 2 20 indicator", "arc_color 2 #ff0000 indicator", NULL}, {NULL}},
    {"ui.arc{min = -50, max = 50, value = 20, start_angle = 90, end_angle = 270}",
     {"arc_range 2 -50 50", "arc_bg_angles 2 90 270", "arc_value 2 20", NULL}, {NULL}},
    {"ui.arc{}", {"arc_rotation 2 270", "remove_style 2 knob", "arc_range 2 0 100", "arc_bg_angles 2 0 360", "arc_value 2 0",
                  "arc_width 2 12", "arc_color 2 #262626", "arc_color 2 #ffffff indicator", NULL}, {NULL}},
    {"ui.bar{min = 10, max = 20, value = 15}", {"bar_range 2 10 20", "bar_value 2 15 anim=off", NULL}, {NULL}},
    {"ui.bar{color = '#ff0000', track_color = '#00ff00', radius = 'circle'}",
     {"bg_color 2 #00ff00", "bg_opa 2 255", "radius 2 32767", "bg_color 2 #ff0000 indicator", "bg_opa 2 255 indicator",
      "radius 2 32767 indicator", NULL}, {NULL}},
    {"ui.bar{}", {"bg_color 2 #262626", "bg_color 2 #ffffff indicator", "radius 2 0", "radius 2 0 indicator", "bar_range 2 0 100",
                  "bar_value 2 0 anim=off", NULL}, {NULL}},
    /* Images: src is the asset's descriptor, scale pct * 256 / 100. */
    {"ui.image{src = 'a.bin'}", {"image_src 2 2x1 cf=0x12 data=4", "image_scale 2 256", NULL}, {NULL}},
    {"ui.image{src = 'b.bin', scale = 150}", {"image_src 2 3x2 cf=0x0e data=8", "image_scale 2 384", NULL}, {NULL}},
    {"ui.image{src = 'a.bin', scale = 10}", {"image_scale 2 25", NULL}, {NULL}},
    {"ui.image{src = 'a.bin', scale = 400}", {"image_scale 2 1024", NULL}, {NULL}},
};

static void style_and_font_mapping(void) {
    for (size_t i = 0; i < ARRAY_LEN(STYLES); i++) {
        env_t e;
        scene(&e);
        char src[300];
        snprintf(src, sizeof src, "w = %s", STYLES[i].lua);
        RUN(&e, src);
        CHECK(APPLY(&e), "%s: the pass ran", STYLES[i].lua);
        for (size_t k = 0; STYLES[i].lines[k] != NULL; k++) {
            CHECK(log_has_line(STYLES[i].lines[k]), "%s: the log lacks \"%s\":\n%s", STYLES[i].lua, STYLES[i].lines[k], log_peek());
        }
        for (size_t k = 0; STYLES[i].absent[k] != NULL; k++) {
            CHECK(log_count(STYLES[i].absent[k]) == 0, "%s: the log has \"%s\":\n%s", STYLES[i].lua, STYLES[i].absent[k], log_peek());
        }
        CHECK(stats_since().rejects == 0, "%s: a reject", STYLES[i].lua);
        FINISH(&e);
    }
}

static void kinds_make_their_objects(void) {
    static const struct {
        const char *lua;
        const char *log;
    } cases[] = {
        {"ui.label{text = 'x'}", "create label 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\n"},
        {"ui.button{text = 'go'}",
         "create button 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\n"
         "create label 3 in 2\nremove_style_all 3\nremove_flag 3 scrollable\nlong_mode 3 clip\nalign 3 center 0 0\n"},
        {"ui.box{}", "create obj 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\n"},
        {"ui.arc{}",
         "create arc 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\narc_rotation 2 270\n"
         "remove_style 2 knob\n"},
        {"ui.bar{}", "create bar 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\n"},
        {"ui.image{src = 'a.bin'}", "create image 2 in 1\nremove_style_all 2\nremove_flag 2 scrollable\nremove_flag 2 clickable\n"},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        env_t e;
        scene(&e);
        char src[200];
        snprintf(src, sizeof src, "w = %s", cases[i].lua);
        RUN(&e, src);
        CHECK(APPLY(&e), "made");
        const char *got = log_peek();
        size_t want = strlen(cases[i].log);
        CHECK(strncmp(got, cases[i].log, want) == 0, "%s: the log starts\n%.*s--- and was to start\n%s---", cases[i].lua, (int)want, got, cases[i].log);
        FINISH(&e);
    }
}

/* LVGL clamps a value to the range it has: the range goes first, so a value outside the default 0..100 lands. */
static void values_land_in_their_range(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.arc{min = 100, max = 200, value = 150, w = 50, h = 50} b = ui.bar{min = -300, max = -100, value = -150, w = 50, h = 5}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *arc = fake_obj(fake_nth("arc", 0)), *bar = fake_obj(fake_nth("bar", 0));
    CHECK(arc->min == 100 && arc->max == 200 && arc->value == 150, "the arc: %d..%d at %d", (int)arc->min, (int)arc->max, (int)arc->value);
    CHECK(bar->min == -300 && bar->max == -100 && bar->value == -150, "the bar: %d..%d at %d", (int)bar->min, (int)bar->max,
          (int)bar->value);
    /* A range that moves with the value. */
    RUN(&e, "a:set{min = 0, max = 1000, value = 900} b:set{max = 100, min = 50, value = 75}");
    CHECK(APPLY(&e), "changed");
    CHECK(arc->min == 0 && arc->max == 1000 && arc->value == 900 && bar->min == 50 && bar->max == 100 && bar->value == 75,
          "arc %d..%d at %d, bar %d..%d at %d", (int)arc->min, (int)arc->max, (int)arc->value, (int)bar->min, (int)bar->max,
          (int)bar->value);
    FINISH(&e);
}

static void button_text_is_its_labels(void) {
    env_t e;
    scene(&e);
    RUN(&e, "b = ui.button{text = 'Go', w = 120, h = 48}");
    CHECK(APPLY(&e), "made");
    LOG_IS(
        "create button 2 in 1\n"
        "remove_style_all 2\n"
        "remove_flag 2 scrollable\n"
        "remove_flag 2 clickable\n"
        "create label 3 in 2\n"
        "remove_style_all 3\n"
        "remove_flag 3 scrollable\n"
        "long_mode 3 clip\n"
        "align 3 center 0 0\n"
        "size 2 120 48\n"
        "align 2 center 0 0\n"
        "remove_flag 2 hidden\n"
        "opa 2 255\n"
        "bg_color 2 #262626\n"
        "bg_opa 2 255\n"
        "radius 2 16\n"
        "border_width 2 0\n"
        "border_color 2 #ffffff\n"
        "text_font 3 montserrat_20\n"
        "text_color 3 #ffffff\n"
        "text 3 \"Go\"\n"
        "update_layout 1\n");
    RUN(&e, "b:set{text = 'Stop'}");
    CHECK(APPLY(&e), "text");
    LOG_IS(
        "text 3 \"Stop\"\n"
        "update_layout 1\n");
    RUN(&e, "b:delete()");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 2\n"
        "update_layout 1\n");  /* the label inside went with it: no call of its own */
    FINISH(&e);
}

static void images_use_the_assets_bytes(void) {
    env_t e;
    scene(&e);
    RUN(&e, "i = ui.image{src = 'a.bin'} j = ui.image{src = 'b.bin'} k = ui.image{src = 'a.bin'}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *a = fake_obj(2), *b = fake_obj(3), *c = fake_obj(4);
    CHECK(a->src != NULL && b->src != NULL && c->src != NULL, "descriptors");
    if (a->src != NULL && b->src != NULL && c->src != NULL) {
        /* The header is the file's first 12 bytes, the data the rest: no copy. */
        CHECK(a->src->data == IMG_A + 12 && a->src->data_size == sizeof IMG_A - 12, "a: data %p size %u", (const void *)a->src->data,
              (unsigned)a->src->data_size);
        CHECK(a->src->header.magic == 0x19 && a->src->header.cf == 0x12 && a->src->header.w == 2 && a->src->header.h == 1 &&
                  a->src->header.stride == 4 && a->src->header.flags == 0,
              "a's header");
        CHECK(b->src->data == IMG_B + 12 && b->src->data_size == sizeof IMG_B - 12 && b->src->header.w == 3 && b->src->header.h == 2 &&
                  b->src->header.cf == 0x0E,
              "b");
        CHECK(a->src == c->src && a->src != b->src, "one descriptor for an image, however many widgets show it");
    }
    /* A new src: the other descriptor. */
    log_clear();
    RUN(&e, "i:set{src = 'b.bin'}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("image_src 2 3x2 cf=0x0e data=8") && fake_obj(2)->src == fake_obj(3)->src, "now b\n%s", log_peek());
    FINISH(&e);
}

/* An image whose bytes are not an image (the package was checked, but this doesn't depend on it) is a reject. */
static void bad_image_bytes_are_rejected(void) {
    static const uint8_t bad_magic[] = {0x18, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t short_data[] = {0x19, 0x12, 0, 0, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2};
    static const uint8_t compressed[] = {0x19, 0x12, 8, 0, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t zero_size[] = {0x19, 0x12, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t bad_stride[] = {0x19, 0x12, 0, 0, 2, 0, 1, 0, 2, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t tiny[] = {0x19, 0x12, 0};
    static const struct {
        const char *what;
        const uint8_t *bytes;
        size_t len;
    } cases[] = {
        {"the wrong magic", bad_magic, sizeof bad_magic},
        {"fewer bytes than the header says", short_data, sizeof short_data},
        {"a compressed image", compressed, sizeof compressed},
        {"no pixels", zero_size, sizeof zero_size},
        {"a stride under a row", bad_stride, sizeof bad_stride},
        {"less than a header", tiny, sizeof tiny},
        {"no bytes", NULL, 0},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        env_t e;
        fake_reset();
        const apps_asset_t assets[] = {{"x.bin", cases[i].bytes, cases[i].len, 2, 1}};
        CHECK(env_open(&e, assets, 1), "env_open");
        apps_lv_build(fake_screen());
        RUN(&e, "w = ui.image{src = 'x.bin'}");
        log_clear();
        apps_lv_stats_t before = stats_now();
        CHECK(APPLY(&e), "the pass ran");
        CHECK(log_count("create ") == 0 && log_count("image_src") == 0 && W(&e, 0)->dirty == 0, "%s: drawn\n%s", cases[i].what, log_peek());
        CHECK(stats_now().rejects == before.rejects + 1, "%s: %u rejects, not 1", cases[i].what, stats_now().rejects - before.rejects);
        FINISH(&e);
    }
}

/* ---- Deletes, and the order of work ---- */

static void deleting_a_box_makes_no_call_for_its_children(void) {
    env_t e;
    scene(&e);
    /* The children's slots are above the box's: the box is deleted first. */
    RUN(&e, "b = ui.box{} c = ui.label{text = 'c', parent = b} d = ui.box{parent = b} f = ui.label{text = 'f', parent = d}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_alive() == 5, "the layer and four");
    log_clear();
    RUN(&e, "b:delete()");
    CHECK(W(&e, 1)->dirty == APPS_DIRTY_DELETE && W(&e, 3)->dirty == APPS_DIRTY_DELETE, "the whole box is flagged");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 2\n"
        "update_layout 1\n");
    CHECK(W(&e, 0)->dirty == 0 && W(&e, 1)->dirty == 0 && W(&e, 2)->dirty == 0 && W(&e, 3)->dirty == 0, "every slot is clear");
    CHECK(fake_alive() == 1 && fake_errors == 0, "only the layer is left: %d, %s", fake_alive(), fake_note);
    /* The slots are free, and what comes into them is made new. */
    RUN(&e, "n1 = ui.label{text = 'n'} n2 = ui.box{} n3 = ui.label{parent = n2, text = 'm'}");
    CHECK(APPLY(&e), "made");
    CHECK(log_count("create label 6 in 1") == 1 && log_count("create obj 7 in 1") == 1 && log_count("create label 8 in 7") == 1,
          "three new objects\n%s", log_peek());
    FINISH(&e);

    /* The children's slots are below the box's: they are visited first, and still no call is made for them. */
    scene(&e);
    RUN(&e, "x0 = ui.label{} x1 = ui.label{} b = ui.box{}");
    CHECK(APPLY(&e), "made");
    RUN(&e, "x0:delete() x1:delete()");
    CHECK(APPLY(&e), "the two slots are free");
    RUN(&e, "c = ui.label{text = 'c', parent = b} d = ui.box{parent = b} f = ui.label{text = 'f', parent = d}");
    CHECK(W(&e, 0)->parent == 2 && W(&e, 1)->parent == 2 && W(&e, 3)->parent == 1, "c and d sit below b, f below d");
    CHECK(APPLY(&e), "made");
    log_clear();
    RUN(&e, "b:delete()");
    CHECK(W(&e, 0)->dirty == APPS_DIRTY_DELETE && W(&e, 1)->dirty == APPS_DIRTY_DELETE && W(&e, 2)->dirty == APPS_DIRTY_DELETE &&
              W(&e, 3)->dirty == APPS_DIRTY_DELETE,
          "the whole box is flagged, wherever it sits");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 4\n"
        "update_layout 1\n");
    CHECK(fake_alive() == 1 && fake_errors == 0, "only the layer is left: %d, %s", fake_alive(), fake_note);
    FINISH(&e);

    /* A child alone. */
    scene(&e);
    RUN(&e, "b = ui.box{} c = ui.label{parent = b} d = ui.label{parent = b}");
    CHECK(APPLY(&e), "made");
    log_clear();
    RUN(&e, "c:delete()");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 3\n"
        "update_layout 1\n");
    CHECK(fake_alive() == 3, "the layer, the box and one label");
    /* And what is left is still the box's: deleting the box now takes d, and calls nothing for it. */
    RUN(&e, "b:delete()");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 2\n"
        "update_layout 1\n");
    FINISH(&e);
}

static void deletes_come_before_creates(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'a'}");
    CHECK(APPLY(&e), "made");
    log_clear();
    RUN(&e, "a:delete() b = ui.label{text = 'b'} c = ui.box{}");
    CHECK(W(&e, 0)->dirty == APPS_DIRTY_DELETE && W(&e, 1)->alive, "b went to slot 1: slot 0 waits for its delete");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_find("delete 2") == 0 && log_find("create label 3 in 1") > 0, "the delete comes first\n%s", log_peek());
    CHECK(log_find("create obj 4 in 1") > log_find("create label 3 in 1"), "slot order among the creates\n%s", log_peek());
    FINISH(&e);
}

/* A slot is free only once its delete has been applied; one that is applied is free for the next widget. */
static void slot_reuse_across_passes(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'a'} b = ui.label{text = 'b'}");
    CHECK(APPLY(&e), "made");
    for (int round = 0; round < 5; round++) {
        log_clear();
        RUN(&e, "a:delete() a = ui.label{text = 'again'}");
        CHECK(W(&e, 0)->dirty == APPS_DIRTY_DELETE, "a's slot waits: it is in slot 2 now");
        CHECK(APPLY(&e), "the pass ran");
        CHECK(log_count("delete ") == 1 && log_count("create ") == 1, "one delete, one create\n%s", log_peek());
        CHECK(fake_alive_of("label") == 2, "two labels");
        log_clear();
        RUN(&e, "a:delete() a = ui.label{text = 'again'}");  /* now in slot 0 again */
        CHECK(APPLY(&e), "the pass ran");
        CHECK(fake_alive_of("label") == 2, "two labels");
    }
    FINISH(&e);
}

/* A box's slot above its child's: the box is made first, once, and gets everything its slot has to apply. */
static void parent_in_a_higher_slot_is_made_first(void) {
    env_t e;
    scene(&e);
    RUN(&e, "x = ui.label{} b = ui.box{w = 100, h = 100, bg = '#112233'} x:delete() c = ui.label{text = 'in', parent = b}");
    CHECK(W(&e, 0)->alive && W(&e, 0)->parent == 1 && W(&e, 1)->kind == APPS_W_BOX, "c is in slot 0, inside the box of slot 1");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create obj") == 1 && log_count("create label") == 1, "one box, one label\n%s", log_peek());
    CHECK(log_find("create obj 2 in 1") >= 0 && log_find("create obj 2 in 1") < log_find("create label 3 in 2"),
          "the box is made before what is inside it\n%s", log_peek());
    /* The box got its own groups when its slot came: size, colours. */
    HAS("size 2 100 100");
    HAS("bg_color 2 #112233");
    HAS("text 3 \"in\"");
    CHECK(W(&e, 0)->dirty == 0 && W(&e, 1)->dirty == 0, "both are clear");
    CHECK(W(&e, 1)->rw == 100 && W(&e, 0)->rw == 20, "both are laid out: %d %d", W(&e, 1)->rw, W(&e, 0)->rw);
    FINISH(&e);

    /* The same with the box's own create refused: the label waits for it, and neither is lost. */
    scene(&e);
    RUN(&e, "x = ui.label{} b = ui.box{} x:delete() c = ui.label{text = 'in', parent = b}");
    psram_free = 100 * 1024;
    CHECK(APPLY(&e), "refused");
    CHECK(log_count("create ") == 0 && stats_since().refused_creates == 2, "both refused: %u\n%s", stats_since().refused_creates, log_peek());
    psram_free = 4u * 1024 * 1024;
    CHECK(APPLY(&e), "made");
    CHECK(log_count("create obj 2 in 1") == 1 && log_count("create label 3 in 2") == 1, "both made\n%s", log_peek());
    FINISH(&e);

    /* Three deep, the deepest in the lowest slot. */
    scene(&e);
    RUN(&e, "p = ui.label{} q = ui.label{} r = ui.label{} b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1}\n"
            "p:delete() q:delete() r:delete()\n"
            "l = ui.label{text = 'deep', parent = b2} m = ui.box{parent = b1} n = ui.label{text = 'n', parent = m}");
    CHECK(W(&e, 0)->parent == 5 && W(&e, 1)->parent == 4 && W(&e, 2)->parent == 1, "the new ones sit below their parents");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 6 && fake_errors == 0, "six objects\n%s", log_peek());
    int box0 = log_find("create obj 2 in 1"), box1 = log_find("create obj 3 in 2"), box2 = log_find("create obj 4 in 3");
    int label = log_find("create label ");
    CHECK(box0 >= 0 && box0 < box1 && box1 < box2 && box2 < label, "the chain is made from the top down\n%s", log_peek());
    FINISH(&e);
}

/* ---- Identity ---- */

static void kind_check(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'} b = ui.arc{} c = ui.box{} d = ui.label{}");
    CHECK(APPLY(&e), "made");
    log_clear();

    /* A live slot whose kind isn't its object's: nothing is called on the object (a label call on an arc). */
    W(&e, 0)->kind = APPS_W_ARC;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE | APPS_DIRTY_VALUE;
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(stats_since().rejects == 1 && W(&e, 0)->dirty == 0, "rejected: %u, bits %u", stats_since().rejects, W(&e, 0)->dirty);
    W(&e, 0)->kind = APPS_W_LABEL;
    W(&e, 0)->dirty = APPS_DIRTY_TEXT;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_has_line("text 2 \"x\"") && stats_since().rejects == 1, "put back, it is applied\n%s", log_peek());
    log_clear();

    /* Another gen: the slot has been given to another widget that the pass has not been told to make. */
    W(&e, 1)->gen++;
    W(&e, 1)->dirty = APPS_DIRTY_VALUE;
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(stats_since().rejects == 2, "rejected: %u", stats_since().rejects);
    W(&e, 1)->gen--;

    /* Another parent: parents are set at creation, so a change is not one the pass knows how to make. */
    W(&e, 3)->parent = 2;
    W(&e, 3)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(stats_since().rejects == 3, "rejected: %u", stats_since().rejects);
    W(&e, 3)->parent = -1;

    /* A live slot with an object but no bit that says it is new and none of the object: no object, no call. */
    RUN(&e, "e = ui.label{text = 'e'}");
    W(&e, 4)->dirty = APPS_DIRTY_TEXT;  /* its CREATE is lost: a slot with no object and no way to get one */
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(stats_since().rejects == 4 && W(&e, 4)->dirty == 0, "rejected: %u", stats_since().rejects);
    FINISH(&e);
}

/* A dead slot with bits that only a live one has, and a live one marked for deletion: neither is for LVGL. */
static void impossible_bit_combinations(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'a'} b = ui.label{text = 'b'}");
    CHECK(APPLY(&e), "made");
    log_clear();
    W(&e, 0)->dirty = APPS_DIRTY_DELETE;  /* alive, and told to be deleted */
    CHECK(APPLY(&e), "the pass ran");
    LOG_EMPTY();
    CHECK(W(&e, 0)->dirty == 0 && stats_since().rejects == 1 && fake_alive_of("label") == 2, "the object stays: %u", W(&e, 0)->dirty);
    RUN(&e, "b:delete()");
    W(&e, 1)->dirty |= APPS_DIRTY_STYLE | APPS_DIRTY_CREATE;  /* dead, and with bits that only a live one has */
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("delete 3") == 1 && log_count("create ") == 0 && W(&e, 1)->dirty == 0, "deleted, not made\n%s", log_peek());
    CHECK(stats_since().rejects == 2, "counted: %u", stats_since().rejects);
    FINISH(&e);
}

/* A slot that is made again (CREATE) while the pass still holds an object for it: a table that lost its way. The
 * old object goes and a new one comes, so there is one. */
static void a_create_over_an_object_replaces_it(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'a'}");
    CHECK(APPLY(&e), "made");
    log_clear();
    W(&e, 0)->dirty = APPS_DIRTY_CREATE | APPS_DIRTY_GEOM | APPS_DIRTY_STYLE | APPS_DIRTY_TEXT;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_find("delete 2") == 0 && log_find("create label 3 in 1") > 0 && fake_alive_of("label") == 1, "replaced\n%s", log_peek());
    CHECK(stats_since().rejects == 0 && W(&e, 0)->dirty == 0, "not a reject");
    FINISH(&e);
}

/* ---- Clear and the background ---- */

static void clear_cleans_the_layer(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'a'} b = ui.box{} c = ui.label{parent = b}");
    CHECK(APPLY(&e), "made");
    log_clear();
    RUN(&e, "ui.clear()");
    CHECK(APPLY(&e), "cleared");
    LOG_IS(
        "clean 1\n"
        "update_layout 1\n");
    CHECK(fake_alive() == 1, "only the layer");
    /* The widgets made in the same callback as the clear are made, and the ones before it never were. */
    RUN(&e, "q = ui.label{text = 'before'} ui.clear() r = ui.label{text = 'after'}");
    CHECK(APPLY(&e), "cleared and made");
    CHECK(log_find("clean 1") == 0 && log_count("create label") == 1 && log_has_line("text 5 \"after\"") && log_count("text ") == 1,
          "clean first, then the one new label (object 5: 2 to 4 were the first three)\n%s", log_peek());
    CHECK(fake_alive() == 2 && fake_errors == 0, "the layer and the label");
    /* Every pointer is gone: what is deleted next is the new object, and nothing else. */
    log_clear();
    RUN(&e, "r:delete()");
    CHECK(APPLY(&e), "deleted");
    LOG_IS(
        "delete 5\n"
        "update_layout 1\n");
    /* A table that says clear and has nothing: a clean of an empty layer. */
    RUN(&e, "ui.clear()");
    CHECK(APPLY(&e), "cleared");
    LOG_IS(
        "clean 1\n"
        "update_layout 1\n");
    FINISH(&e);
}

static void background_follows_the_table(void) {
    env_t e;
    scene_raw(&e);
    CHECK(APPLY(&e), "the first pass");
    LOG_IS("bg_color 1 #000000\n");  /* the first take of a table always says changed */
    RUN(&e, "ui.bg('#112233')");
    CHECK(APPLY(&e), "changed");
    LOG_IS("bg_color 1 #112233\n");
    RUN(&e, "ui.bg(0x112233)");  /* the same colour */
    CHECK(APPLY(&e), "the same");
    LOG_EMPTY();
    RUN(&e, "ui.bg('#fff')");
    CHECK(APPLY(&e), "changed");
    LOG_IS("bg_color 1 #ffffff\n");
    /* A skipped pass leaves it for the next. */
    RUN(&e, "ui.bg('#000')");
    CHECK(!APPLY_CB(&e), "skipped");
    LOG_EMPTY();
    CHECK(APPLY(&e), "applied");
    LOG_IS("bg_color 1 #000000\n");
    FINISH(&e);
}

/* ---- Timing ---- */

static void slow_apply_is_logged_and_counted(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    call_cost_us = 500;
    long before = lvgl_calls;
    CHECK(APPLY(&e), "made");
    long calls = lvgl_calls - before;
    CHECK(calls >= 10, "a create is %ld calls", calls);
    char want[80];
    snprintf(want, sizeof want, "W muse_apps: apply %ld us", calls * 500);
    CHECK(stats_since().slow == 1 && esp_has_line(want), "slow %u, wanted \"%s\"\n%s", stats_since().slow, want, esp_peek());
    log_clear();
    esp_clear();

    /* Over 2 ms, not 2 ms: the border. */
    call_cost_us = 0;
    RUN(&e, "a:set{text = 'y'}");
    extra_us_next = 2000;
    CHECK(APPLY(&e), "2000 us");
    CHECK(stats_since().slow == 1 && esp_count("") == 0, "2000 us is not slow\n%s", esp_peek());
    RUN(&e, "a:set{text = 'z'}");
    extra_us_next = 2001;
    CHECK(APPLY(&e), "2001 us");
    CHECK(stats_since().slow == 2 && esp_has_line("W muse_apps: apply 2001 us"), "2001 us is\n%s", esp_peek());
    log_clear();
    esp_clear();
    /* Waiting for the lock isn't the pass's time, and a pass that was skipped isn't slow. */
    RUN(&e, "a:set{text = 'w'}");
    trylock_refusals = 1;
    extra_us_next = 5000;
    CHECK(!APPLY(&e), "skipped");
    CHECK(stats_since().slow == 2 && esp_count("") == 0, "not slow");
    extra_us_next = 0;
    CHECK(APPLY(&e), "applied");
    CHECK(stats_since().slow == 2, "and fast");
    FINISH(&e);
}

/* ---- The pass's memory ---- */

static void a_pass_with_no_memory_is_a_skip(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    apps_lv_reset();  /* the records are freed: the next pass has to make them */
    log_clear();
    refuse_from = growths + 1;
    long trylocks = trylock_calls, calls = lvgl_calls;
    CHECK(!APPLY(&e), "no memory for the pass's records");
    CHECK(lvgl_calls == calls && trylock_calls == trylocks && W(&e, 0)->dirty != 0, "no lock taken, no call, the bits wait");
    CHECK(stats_since().skips == 1 && stats_since().applies == 0, "a skip: %u", stats_since().skips);
    CHECK(!APPLY(&e) && stats_since().skips == 2, "and again");
    refuse_from = -1;
    CHECK(APPLY(&e), "there is room");
    CHECK(log_count("create label") == 1, "made\n%s", log_peek());
    FINISH(&e);
}

static void many_widgets(void) {
    env_t e;
    scene(&e);
    RUN(&e, "for i = 1, 128 do ui.label{text = tostring(i), x = i - 64, y = i - 64} end");
    CHECK(apps_ui_count(e.ui) == 128, "128 alive");
    CHECK(APPLY(&e), "made");
    CHECK(log_count("create label") == 128 && fake_errors == 0 && stats_since().rejects == 0, "128 objects\n%.200s", log_peek());
    for (int i = 0; i < 128; i++) {
        CHECK(W(&e, i)->dirty == 0 && W(&e, i)->rw > 0, "slot %d: bits %u rw %d", i, W(&e, i)->dirty, W(&e, i)->rw);
    }
    log_clear();
    RUN(&e, "ui.clear()");
    CHECK(APPLY(&e), "cleared");
    LOG_IS(
        "clean 1\n"
        "update_layout 1\n");
    RUN(&e, "for i = 1, 128 do ui.box{w = 5, h = 5} end");
    CHECK(APPLY(&e), "made again");
    CHECK(log_count("create obj") == 128 && fake_alive() == 129, "128 boxes\n%.200s", log_peek());
    FINISH(&e);
}

/* ---- Random tables ---- */

static uint32_t rng_state;

static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return n != 0 ? rng_state % n : 0;
}

static const char *const KIND_NAMES[] = {"label", "button", "box", "arc", "bar", "image"};
static const char *const ALIGN_NAMES[] = {"center", "top", "bottom", "left", "right", "top_left", "top_right", "bottom_left", "bottom_right"};
static const char *const FONT_NAMES[] = {"sans14", "sans16", "sans20", "sans28", "mono8", "mono16", "digits48", "digits96"};
static const char *const COLOUR_NAMES[] = {"'#fff'", "'#102030'", "0xff8800", "'#00000'", "'red'", "-1", "0x1000000", "1.5"};

/* One property, as a Lua field: mostly right for the kind, sometimes not (the binding raises and the pcall eats it). */
static void random_prop(int kind, char *out, size_t cap) {
    char text[40];
    switch (rnd(20)) {
        case 0: snprintf(out, cap, "x = %d", (int)rnd(1000) - 500); break;
        case 1: snprintf(out, cap, "y = %d", (int)rnd(1000) - 500); break;
        case 2: snprintf(out, cap, "w = %d", (int)rnd(500) - 10); break;
        case 3: snprintf(out, cap, "h = %d", (int)rnd(500) - 10); break;
        case 4: snprintf(out, cap, "align = '%s'", ALIGN_NAMES[rnd(ARRAY_LEN(ALIGN_NAMES))]); break;
        case 5: snprintf(out, cap, "hidden = %s", rnd(2) ? "true" : "false"); break;
        case 6: snprintf(out, cap, "opacity = %d", (int)rnd(120)); break;
        case 7:
        case 8:
            for (size_t i = 0; i < sizeof text - 1; i++) {
                text[i] = (char)('a' + rnd(26));
            }
            text[rnd(sizeof text - 1)] = '\0';
            snprintf(out, cap, "text = '%s'", text);
            break;
        case 9: snprintf(out, cap, "font = '%s'", FONT_NAMES[rnd(ARRAY_LEN(FONT_NAMES))]); break;
        case 10: snprintf(out, cap, "color = %s", COLOUR_NAMES[rnd(ARRAY_LEN(COLOUR_NAMES))]); break;
        case 11: snprintf(out, cap, "bg = %s", rnd(4) == 0 ? "'none'" : COLOUR_NAMES[rnd(ARRAY_LEN(COLOUR_NAMES))]); break;
        case 12: snprintf(out, cap, "radius = %s", rnd(2) ? "'circle'" : "7"); break;
        case 13: snprintf(out, cap, "border = %d", (int)rnd(30)); break;
        case 14: snprintf(out, cap, "value = %d", (int)rnd(200) - 50); break;
        case 15: snprintf(out, cap, "min = %d, max = %d", (int)rnd(100) - 20, (int)rnd(100) + 40); break;
        case 16: snprintf(out, cap, "start_angle = %d, end_angle = %d", (int)rnd(400), (int)rnd(400)); break;
        case 17: snprintf(out, cap, "thickness = %d", (int)rnd(70)); break;
        case 18: snprintf(out, cap, "src = '%s'", rnd(3) == 0 ? "c.bin" : (rnd(2) ? "a.bin" : "b.bin")); break;
        default: snprintf(out, cap, "scale = %d", (int)rnd(500)); break;
    }
    (void)kind;
}

/* Overwrites one field of the slot with garbage (the harness puts the widget back after the pass). */
static void corrupt(env_t *e, int slot) {
    apps_widget_t *w = W(e, slot);
    switch (rnd(24)) {
        case 0: w->kind = (uint8_t)rnd(10); break;
        case 1: w->parent = (int16_t)((int)rnd(140) - 6); break;
        case 2: w->x = (int16_t)((int)rnd(2000) - 1000); break;
        case 3: w->w = (int16_t)((int)rnd(1200) - 100); break;
        case 4: w->h = (int16_t)((int)rnd(1200) - 100); break;
        case 5: w->align = (uint8_t)rnd(20); break;
        case 6: w->font = (uint8_t)rnd(20); break;
        case 7: w->opacity = (uint8_t)rnd(255); break;
        case 8: w->text_align = (uint8_t)rnd(8); break;
        case 9: w->radius = (int16_t)((int)rnd(600) - 100); break;
        case 10: w->border = (uint8_t)rnd(255); break;
        case 11: w->thickness = (uint8_t)rnd(255); break;
        case 12: w->min = (int32_t)rnd(4000000) - 2000000; break;
        case 13: w->max = (int32_t)rnd(4000000) - 2000000; break;
        case 14: w->value = (int32_t)rnd(4000000) - 2000000; break;
        case 15: w->start_angle = (int16_t)((int)rnd(800) - 200); break;
        case 16: w->end_angle = (int16_t)((int)rnd(800) - 200); break;
        case 17: w->image = (int16_t)((int)rnd(10) - 3); break;
        case 18: w->scale = (uint16_t)rnd(1000); break;
        case 19: w->color = rnd(2) ? 0xFFFFFFFFu : rnd(0x2000000); break;
        case 20: w->events = (uint8_t)rnd(255); break;
        case 21: w->dirty = (uint32_t)rnd(256); break;
        case 22: w->alive = !w->alive; break;
        default: {  /* a block of its own: the widget's own is kept by the caller, who puts it back */
            const char *bytes = rnd(2) != 0 ? "\xff\xfe" : "plain";
            size_t len = bytes[0] == 'p' ? 5 : 2;
            char *block = apps_port_realloc(NULL, len + 1);
            CHECK(block != NULL, "a block of %zu", len + 1);
            if (block != NULL) {
                memcpy(block, bytes, len);
                block[len] = '\0';
                w->text = block;
            }
            break;
        }
    }
}

/* What the table says should be on the layer, against what is: every live widget clean and made once. */
static void check_in_sync(env_t *e, uint32_t seed) {
    int labels = 0, buttons = 0, boxes = 1, arcs = 0, bars = 0, images = 0;  /* the layer is an lv_obj */
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = W(e, i);
        CHECK(w->dirty == 0, "seed %u: slot %d still has bits %u", seed, i, w->dirty);
        if (!w->alive) {
            continue;
        }
        switch (w->kind) {
            case APPS_W_LABEL: labels++; break;
            case APPS_W_BUTTON: buttons++; labels++; break;
            case APPS_W_BOX: boxes++; break;
            case APPS_W_ARC: arcs++; break;
            case APPS_W_BAR: bars++; break;
            case APPS_W_IMAGE: images++; break;
        }
    }
    CHECK(fake_alive_of("label") == labels && fake_alive_of("button") == buttons && fake_alive_of("obj") == boxes &&
              fake_alive_of("arc") == arcs && fake_alive_of("bar") == bars && fake_alive_of("image") == images,
          "seed %u: the layer has labels %d buttons %d boxes %d arcs %d bars %d images %d, the table wants %d %d %d %d %d %d", seed,
          fake_alive_of("label"), fake_alive_of("button"), fake_alive_of("obj"), fake_alive_of("arc"), fake_alive_of("bar"),
          fake_alive_of("image"), labels, buttons, boxes, arcs, bars, images);
}

/* Random calls into the ui module, random passes (skipped, refused for PSRAM, in a callback), touches of objects
 * that may be gone, and, for the odd seeds, a table corrupted for one pass. The fake LVGL counts every call on a
 * deleted or NULL object, and ASan every wild read; an even seed also checks the layer ends up as the table says. */
static void fuzz_seed(uint32_t seed) {
    env_t e;
    bool corrupting = (seed & 1) != 0;
    scene(&e);
    rng_state = seed * 2654435761u + 12345u;
    apps_lv_set_event_cb(on_event);
    RUN(&e, "H = {} N = 0");
    char src[700], props[160];
    for (int step = 0; step < 400; step++) {
        src[0] = '\0';
        switch (rnd(100)) {
            case 0 ... 29: {  /* a widget */
                int kind = (int)rnd(ARRAY_LEN(KIND_NAMES));
                char fields[500] = "";
                for (uint32_t i = 0, n = rnd(5); i < n; i++) {
                    random_prop(kind, props, sizeof props);
                    strncat(fields, props, sizeof fields - strlen(fields) - 2);
                    strncat(fields, ", ", sizeof fields - strlen(fields) - 1);
                }
                if (kind == APPS_W_IMAGE && rnd(8) != 0) {
                    strncat(fields, "src = 'a.bin', ", sizeof fields - strlen(fields) - 1);
                }
                if (rnd(3) == 0) {
                    snprintf(props, sizeof props, "parent = H[%u], ", (unsigned)rnd(12) + 1);
                    strncat(fields, props, sizeof fields - strlen(fields) - 1);
                }
                snprintf(src, sizeof src, "pcall(function() local w = ui.%s{%s} N = N + 1 H[N] = w end)", KIND_NAMES[kind], fields);
                break;
            }
            case 30 ... 54:  /* a change */
                random_prop(0, props, sizeof props);
                snprintf(src, sizeof src, "pcall(function() H[%u]:set{%s} end)", (unsigned)rnd(14) + 1, props);
                break;
            case 55 ... 64: snprintf(src, sizeof src, "pcall(function() H[%u]:delete() end)", (unsigned)rnd(14) + 1); break;
            case 65 ... 68: snprintf(src, sizeof src, "ui.clear() H = {} N = 0"); break;
            case 69 ... 71: snprintf(src, sizeof src, "ui.bg(%s)", COLOUR_NAMES[rnd(3)]); break;
            case 72 ... 79:
                snprintf(src, sizeof src, "pcall(function() H[%u]:on('%s', %s) end)", (unsigned)rnd(14) + 1,
                         rnd(2) ? "tap" : "long_press", rnd(4) == 0 ? "nil" : "function() end");
                break;
            case 80 ... 92: {  /* a pass */
                psram_free = rnd(4) == 0 ? (size_t)rnd(600 * 1024) : 4u * 1024 * 1024;
                trylock_refusals = rnd(8) == 0 ? 1 : 0;
                int slot = (int)rnd(APPS_UI_SLOTS);
                apps_widget_t saved;
                char *saved_text = NULL;
                bool damaged = corrupting && rnd(3) == 0;
                if (damaged) {
                    saved = *W(&e, slot);
                    saved_text = saved.text;
                    corrupt(&e, slot);
                }
                bool in_callback = rnd(4) == 0;
                apply_at(__LINE__, &e, in_callback);
                if (damaged) {
                    /* Put the widget back as the app's side has it (the pass may have cleared its bits, and may have
                     * written its geometry back: those are the pass's to say). */
                    apps_widget_t *w = W(&e, slot);
                    if (w->text != saved_text) {
                        apps_port_free(w->text);
                    }
                    uint32_t dirty = w->dirty;
                    *w = saved;
                    w->dirty = dirty | (saved.dirty & ~APPS_DIRTY_DELETE & ~APPS_DIRTY_CREATE);
                    if (saved.alive == false) {
                        w->dirty = saved.dirty;
                    }
                }
                break;
            }
            case 93 ... 97:  /* a touch, of an object that may be long gone */
                if (n_objs > 1) {
                    static const lv_event_code_t codes[] = {LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSED,
                                                            LV_EVENT_RELEASED, LV_EVENT_CLICKED, LV_EVENT_GESTURE};
                    fake_fire(1 + (int)rnd((uint32_t)n_objs), codes[rnd(ARRAY_LEN(codes))]);
                    n_evs = 0;
                }
                break;
            default: snprintf(src, sizeof src, "collectgarbage()"); break;
        }
        if (src[0] != '\0') {
            char err[300];
            run(&e, src, err, sizeof err);  /* a call that raises is no failure: the pcall eats most, and some are not wrapped */
            after(__LINE__, &e);
        }
    }
    /* Everything the app asked for is drawn once the passes run with room and the lock. */
    psram_free = 4u * 1024 * 1024;
    trylock_refusals = 0;
    if (!corrupting) {
        for (int i = 0; i < 3; i++) {
            apply_at(__LINE__, &e, false);
        }
        check_in_sync(&e, seed);
        CHECK(stats_since().rejects == 0, "seed %u: %u rejects in a table that was never corrupted", seed, stats_since().rejects);
    } else {
        for (int i = 0; i < 3; i++) {
            apply_at(__LINE__, &e, false);
        }
    }
    FINISH(&e);
}

/* 24 seeds, or as many as MUSE_LV_FUZZ_SEEDS says (a longer run, by hand). */
static void fuzz(void) {
    const char *more = getenv("MUSE_LV_FUZZ_SEEDS");
    uint32_t seeds = more != NULL && atoi(more) > 0 ? (uint32_t)atoi(more) : 24;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        fuzz_seed(seed);
    }
}

/* ---- The cases ---- */

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"create_update_delete_sequence", create_update_delete_sequence},
        {"revalidation_drops_bad_slot", revalidation_drops_bad_slot},
        {"parent_chain_of_four_is_drawn", parent_chain_of_four_is_drawn},
        {"revalidation_after_the_object_exists", revalidation_after_the_object_exists},
        {"reject_logged_once_per_app", reject_logged_once_per_app},
        {"empty_text_is_made_and_set", empty_text_is_made_and_set},
        {"psram_floor_refuses_creates", psram_floor_refuses_creates},
        {"create_pending_has_no_object", create_pending_has_no_object},
        {"skips_mid_callback_at_most_three_frames", skips_mid_callback_at_most_three_frames},
        {"lock_discipline", lock_discipline},
        {"no_layer_no_pass", no_layer_no_pass},
        {"overflow_from_coords", overflow_from_coords},
        {"overflow_corner_rule", overflow_corner_rule},
        {"overflow_round_shapes", overflow_round_shapes},
        {"overflow_of_hidden_and_inside", overflow_of_hidden_and_inside},
        {"layout_written_back_for_every_widget", layout_written_back_for_every_widget},
        {"event_after_delete_carries_stale_gen", event_after_delete_carries_stale_gen},
        {"tap_events", tap_events},
        {"clickable_only_when_subscribed", clickable_only_when_subscribed},
        {"subscribed_arc_cannot_be_scrubbed", subscribed_arc_cannot_be_scrubbed},
        {"many_sets_while_dark_apply_once", many_sets_while_dark_apply_once},
        {"swipe_right_closes", swipe_right_closes},
        {"front_and_back", front_and_back},
        {"reset_deletes_every_object", reset_deletes_every_object},
        {"stale_records_after_a_missed_reset", stale_records_after_a_missed_reset},
        {"build_makes_the_layer_again", build_makes_the_layer_again},
        {"style_and_font_mapping", style_and_font_mapping},
        {"kinds_make_their_objects", kinds_make_their_objects},
        {"values_land_in_their_range", values_land_in_their_range},
        {"button_text_is_its_labels", button_text_is_its_labels},
        {"images_use_the_assets_bytes", images_use_the_assets_bytes},
        {"bad_image_bytes_are_rejected", bad_image_bytes_are_rejected},
        {"deleting_a_box_makes_no_call_for_its_children", deleting_a_box_makes_no_call_for_its_children},
        {"deletes_come_before_creates", deletes_come_before_creates},
        {"slot_reuse_across_passes", slot_reuse_across_passes},
        {"parent_in_a_higher_slot_is_made_first", parent_in_a_higher_slot_is_made_first},
        {"kind_check", kind_check},
        {"impossible_bit_combinations", impossible_bit_combinations},
        {"a_create_over_an_object_replaces_it", a_create_over_an_object_replaces_it},
        {"clear_cleans_the_layer", clear_cleans_the_layer},
        {"background_follows_the_table", background_follows_the_table},
        {"slow_apply_is_logged_and_counted", slow_apply_is_logged_and_counted},
        {"a_pass_with_no_memory_is_a_skip", a_pass_with_no_memory_is_a_skip},
        {"many_widgets", many_widgets},
        {"fuzz", fuzz},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(held == 0, "%s ended with %zu bytes held", argv[1], held);
            fake_reset();
            free(lvgl_log.data);
            free(lvgl_log.taken);
            free(esp_log_text.data);
            free(esp_log_text.taken);
            return failures != 0 ? 1 : 0;
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
