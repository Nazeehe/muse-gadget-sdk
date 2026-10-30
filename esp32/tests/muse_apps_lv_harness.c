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
#include "apps_pkg.h"
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
static void (*trylock_hook)(void *);  /* run once, as the next call of apps_port_ui_trylock begins: another task's turn */
static void *trylock_hook_arg;
static void (*realloc_hook)(void *);  /* the same for the next call of apps_port_realloc */
static void *realloc_hook_arg;
static int lock_errors;
static char lock_note[96];

static void lock_error(const char *what) {
    if (lock_errors++ == 0) {
        snprintf(lock_note, sizeof lock_note, "%s", what);
    }
}

void *apps_port_realloc(void *ptr, size_t n) {
    if (realloc_hook != NULL) {
        /* Another task's turn, as this call begins: the heap is a place where a task can be overtaken. */
        void (*hook)(void *) = realloc_hook;
        realloc_hook = NULL;
        bool was_active = apply_active;
        apply_active = false;
        hook(realloc_hook_arg);
        apply_active = was_active;
    }
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
    if (trylock_hook != NULL) {
        /* Another task got the lock first, did its work and gave it back, while this call waited. */
        void (*hook)(void *) = trylock_hook;
        trylock_hook = NULL;
        bool was_active = apply_active;
        apply_active = false;
        hook(trylock_hook_arg);
        apply_active = was_active;
    }
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

/* The log has this line, whole. */
static void has_at(int line, const char *text) {
    CHECK_AT(line, log_has_line(text), "the log lacks \"%s\":\n%s", text, log_peek());
}
#define HAS(text) has_at(__LINE__, (text))

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
    lv_image_align_t inner_align;  /* where an image sits in its box: CENTER until lv_image_set_inner_align says otherwise */
    lv_area_t area;
    bool forced;           /* the test set the area: the layout leaves it */
    fake_cb_t cbs[8];
    int n_cbs;
    int order;             /* where it is among its parent's children (lv_obj_get_index): made last, unless moved */
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
static int next_order;
static lv_obj_t screen_obj;
static lv_indev_t indev_obj;
static bool indev_present = true;
static lv_dir_t gesture_dir = LV_DIR_NONE;
static lv_obj_t *pressed_obj;  /* the object the finger went down on (LVGL's act_obj), NULL when it is up */
static bool wait_released;     /* lv_indev_wait_release was called during this touch */
static bool long_sent;         /* LONG_PRESSED was sent during this touch */

static int fake_errors;
static char fake_note[240];

static long lvgl_calls;          /* every call into the fake LVGL, but the getters of events */
static int create_fail_in;       /* the Nth lv_*_create from now returns NULL, as LVGL does when it has no memory (0: none) */
static void (*image_hook)(void *);  /* run once, as the next lv_image_create begins: what changes after the pass has checked */
static void *image_hook_arg;
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
    if (create_fail_in > 0 && --create_fail_in == 0) {
        logline("create_failed %s", cls);  /* no memory: NULL, and nothing was made */
        return NULL;
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
    o->order = ++next_order;
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
    if (image_hook != NULL) {
        void (*hook)(void *) = image_hook;
        image_hook = NULL;
        hook(image_hook_arg);
    }
    lv_obj_t *o = make_object("image", parent, "lv_image_create");
    if (o != NULL) {
        o->inner_align = LV_IMAGE_ALIGN_CENTER;  /* lv_image_constructor: img->align = LV_IMAGE_ALIGN_CENTER */
    }
    return o;
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

/* parent's live children in their order (LVGL's child list), into kids; their count. */
static int fake_children(const lv_obj_t *parent, lv_obj_t **kids) {
    int n = 0;
    for (int i = 0; i < n_objs; i++) {
        if (objs[i]->parent == parent && !objs[i]->deleted) {
            int at = n++;
            while (at > 0 && kids[at - 1]->order > objs[i]->order) {
                kids[at] = kids[at - 1];
                at--;
            }
            kids[at] = objs[i];
        }
    }
    return n;
}

static lv_obj_t *fake_kids[FAKE_OBJS_MAX];

int32_t lv_obj_get_index(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_get_index", NULL) == NULL || obj->parent == NULL) {
        return -1;
    }
    int n = fake_children(obj->parent, fake_kids);
    for (int i = 0; i < n; i++) {
        if (fake_kids[i] == obj) {
            return i;
        }
    }
    return -1;
}

/* LVGL 9.5.0's lv_obj_move_to_index (lv_obj_tree.c): a negative index counts from the end; one out of range, or the
 * index it has, leaves it where it is. */
void lv_obj_move_to_index(lv_obj_t *obj, int32_t index) {
    if (use(obj, "lv_obj_move_to_index", NULL) == NULL || obj->parent == NULL) {
        return;
    }
    logline("move_to_index %d %d", obj->id, (int)index);
    int n = fake_children(obj->parent, fake_kids);
    int old = 0;
    while (old < n && fake_kids[old] != obj) {
        old++;
    }
    if (index < 0) {
        index += n;
    }
    if (index < 0 || index >= n || index == old) {
        return;
    }
    if (index < old) {
        memmove(&fake_kids[index + 1], &fake_kids[index], (size_t)(old - index) * sizeof fake_kids[0]);
    } else {
        memmove(&fake_kids[old], &fake_kids[old + 1], (size_t)(index - old) * sizeof fake_kids[0]);
    }
    fake_kids[index] = obj;
    for (int i = 0; i < n; i++) {
        fake_kids[i]->order = ++next_order;
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
        /* LVGL 9.5.0's LV_EVENT_GET_SELF_SIZE (lv_image.c) says the image as it is: the scale draws it larger or smaller
         * about the middle of its box, and does not change what the box lays out as. */
        *w = (int32_t)o->src->header.w;
        *h = (int32_t)o->src->header.h;
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

void lv_obj_set_style_image_recolor(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_image_recolor", NULL) != NULL) {
        style_log(obj, "image_recolor", selector, "#%06x", (unsigned)rgb(value));
    }
}

void lv_obj_set_style_image_recolor_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_image_recolor_opa", NULL) != NULL) {
        style_log(obj, "image_recolor_opa", selector, "%u", (unsigned)value);
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
    if ((dsc->header.flags & ~(unsigned)LV_IMAGE_FLAGS_PREMULTIPLIED) != 0) {
        /* LVGL 9.5.0 reads ALLOCATED as "this descriptor is an lv_draw_buf" and, as its unaligned_data and handlers (the
         * descriptor's reserved fields, NULL here) are not there, refuses it: the image never shows. The other flags say
         * what the memory is as well. Only premultiplied alpha means anything to a descriptor drawn in place, so any
         * other that gets this far is a mistake the fake reports. */
        fake_error("lv_image_set_src(%d): header flags 0x%04x, but only premultiplied alpha means anything to a descriptor "
                   "drawn in place", obj->id, (unsigned)dsc->header.flags);
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

void lv_image_set_inner_align(lv_obj_t *obj, lv_image_align_t align) {
    if (use(obj, "lv_image_set_inner_align", "image") == NULL) {
        return;
    }
    if (align != LV_IMAGE_ALIGN_DEFAULT && align != LV_IMAGE_ALIGN_CENTER) {
        fake_error("lv_image_set_inner_align(%d, %d): the fake only knows default and center", obj->id, (int)align);
        return;
    }
    obj->inner_align = align;
    logline("image_inner_align %d %s", obj->id, align == LV_IMAGE_ALIGN_CENTER ? "center" : "default");
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
    wait_released = true;
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

/* A touch the way LVGL 9.5.0's indev (lv_indev.c) sends it. The finger goes down on an object: PRESSED. */
static void fake_press(int id) {
    pressed_obj = fake_obj(id);
    wait_released = false;
    long_sent = false;
    fake_fire(id, LV_EVENT_PRESSED);
}

/* It stays down long enough: LONG_PRESSED. */
static void fake_hold(void) {
    long_sent = true;
    fake_fire(pressed_obj->id, LV_EVENT_LONG_PRESSED);
}

/* It moves far enough: indev_gesture sends the gesture, to the first object up from the pressed one that doesn't pass
 * gestures on (one with no GESTURE_BUBBLE), and to that one only. */
static void fake_swipe(lv_dir_t dir) {
    lv_obj_t *target = pressed_obj;
    while (target != NULL && (target->flags & LV_OBJ_FLAG_GESTURE_BUBBLE) != 0) {
        target = target->parent;
    }
    gesture_dir = dir;
    if (target != NULL) {
        fake_fire(target->id, LV_EVENT_GESTURE);
    }
}

/* It lifts. After lv_indev_wait_release the pressed object gets PRESS_LOST and nothing else (no RELEASED, no click).
 * Otherwise RELEASED, then SHORT_CLICKED (when no long press was sent), then CLICKED. */
static void fake_lift(void) {
    lv_obj_t *o = pressed_obj;
    pressed_obj = NULL;
    if (o == NULL) {
        return;
    }
    if (wait_released) {
        wait_released = false;
        fake_fire(o->id, LV_EVENT_PRESS_LOST);
        return;
    }
    fake_fire(o->id, LV_EVENT_RELEASED);
    if (!long_sent) {
        fake_fire(o->id, LV_EVENT_SHORT_CLICKED);
    }
    fake_fire(o->id, LV_EVENT_CLICKED);
}

/* A tap: down and up. */
static void fake_tap(int id) {
    fake_press(id);
    fake_lift();
}

/* Where LVGL 9.5.0's draw_image puts the picture of an image object: the image at its own size, placed in the box by the
 * inner align (lv_area_align), then scaled about its middle (the default pivot). To a pixel: the fake does the sums in
 * whole pixels. */
static lv_area_t fake_drawn(int id) {
    const lv_obj_t *o = fake_obj(id);
    int32_t iw = (int32_t)o->src->header.w, ih = (int32_t)o->src->header.h;
    int32_t bw = o->area.x2 - o->area.x1 + 1, bh = o->area.y2 - o->area.y1 + 1;
    int32_t x = o->area.x1, y = o->area.y1;
    if (o->inner_align == LV_IMAGE_ALIGN_CENTER) {
        x += bw / 2 - iw / 2;
        y += bh / 2 - ih / 2;
    }
    int32_t sw = iw * (int32_t)o->scale / 256, sh = ih * (int32_t)o->scale / 256;
    lv_area_t a;
    a.x1 = (2 * x + iw - sw) / 2;
    a.y1 = (2 * y + ih - sh) / 2;
    a.x2 = a.x1 + sw - 1;
    a.y2 = a.y1 + sh - 1;
    return a;
}

static void fake_free_all(void) {
    for (int i = 0; i < n_objs; i++) {
        free(objs[i]->text);
        free(objs[i]);
        objs[i] = NULL;
    }
    n_objs = 0;
    next_id = 0;
    next_order = 0;
    memset(&screen_obj, 0, sizeof screen_obj);
    screen_obj.cls = "screen";
    screen_obj.area = (lv_area_t){0, 0, 465, 465};
    indev_present = true;
    gesture_dir = LV_DIR_NONE;
    pressed_obj = NULL;
    wait_released = false;
    long_sent = false;
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

/* The app task's side of Task 12's C6: the table is published, and cleared, with the UI lock held. */
static void publish(const env_t *e) {
    apps_port_ui_lock();
    apps_lv_set_ui(e->ui);
    apps_port_ui_unlock();
}

static void unpublish(void) {
    apps_port_ui_lock();
    apps_lv_set_ui(NULL);
    apps_port_ui_unlock();
}

/* False when the heap refused (what was made is still there for env_close). Otherwise the table is published,
 * as a running app's is. */
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
    publish(e);
    return true;
}

/* An app's end, as C6 has it: the pointer is cleared with the lock held, and only then is the table freed. */
static void env_close(env_t *e) {
    unpublish();
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
    bool applied = apps_lv_apply(in_callback);
    apply_active = false;
    after(line, e);
    return applied;
}
#define APPLY(e) apply_at(__LINE__, (e), false)
#define APPLY_CB(e) apply_at(__LINE__, (e), true)

/* A fresh fake LVGL and a layer on its screen, an app's table with two images, and the first pass done (which
 * takes the table's background), so the log is empty and a case's first pass is its own. */
static void scene_raw_with(env_t *e, const apps_asset_t *assets, int n_assets) {
    fake_reset();
    refuse_from = -1;
    trylock_refusals = 0;
    create_fail_in = 0;
    CHECK(env_open(e, assets, n_assets), "env_open");
    apps_lv_build(fake_screen());
    log_clear();
    stats_base = stats_now();
}

static void scene_raw(env_t *e) {
    scene_raw_with(e, ASSETS, 2);
}

/* The same with a table of images of the case's own. */
static void scene_with(env_t *e, const apps_asset_t *assets, int n_assets) {
    scene_raw_with(e, assets, n_assets);
    apply_at(__LINE__, e, false);
    log_clear();
    stats_base = stats_now();
}

static void scene(env_t *e) {
    scene_with(e, ASSETS, 2);
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

static int closes;  /* times the layer's close callback has run */

typedef struct {
    uint16_t slot, gen;
    uint8_t event;
    int closes_before;  /* closes that had happened when it arrived */
} ev_t;

static ev_t evs[256];
static int n_evs;

static void on_event(uint16_t slot, uint16_t gen, uint8_t event) {
    if (n_evs < (int)ARRAY_LEN(evs)) {
        evs[n_evs++] = (ev_t){slot, gen, event, closes};
    }
}

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
/* A box counts itself in the four it may nest (apps_ui.h): the binding makes no box with four boxes above it, and no other
 * widget with five. These two build tables as only a corrupted one could be. Slot 4 is a box made on its own and slot 5 a
 * label made in it. Moving box 4 into box 3 gives it four boxes above it, which is a fifth box, and the label five; moving
 * only the label into box 3 gives the label four, the most a widget that isn't a box may have, and leaves box 4 alone. */
POKE(box_four_into_box_three, W(e, 4)->parent = 3)
POKE(parent_four_boxes_up, w->parent = 3)
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
    /* The box that is the fifth in its chain is refused, and the label in it with it (five boxes above it). */
    {"a box with four boxes above it", "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
                                       "b4 = ui.box{}", 4, box_four_into_box_three, 4, 1},
    {"five boxes above it", "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
                            "b4 = ui.box{} w = ui.label{parent = b4}", 5, box_four_into_box_three, 4, 2},
    {"a parent chain that loops", "b0 = ui.box{} b1 = ui.box{parent = b0}", 1, parent_loop, 0, 2},
    {"a box that became a label under a child", "b = ui.box{} w = ui.label{parent = b}", 1, parent_kind_changed, 1, 1},
    /* A box in a higher slot than its pending child is made for the child, so it is checked first: bad, it is not made,
     * and neither is the child. */
    {"a pending box above its child that is bad itself", "x = ui.label{} b = ui.box{} x:delete() c = ui.label{parent = b}", 1,
     opacity_101, 0, 2},
    {"a pending box two levels above it that is bad itself",
     "x = ui.label{} b0 = ui.box{} b1 = ui.box{parent = b0} x:delete() c = ui.label{parent = b1}", 1, opacity_101, 0, 3},
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

/* Four boxes above a widget that isn't a box is the most there may be, and slot 4, the fifth box made, stands on its own
 * with none above it: a table the rule allows, drawn whole. */
static void parent_chain_of_four_is_drawn(void) {
    env_t e;
    scene(&e);
    RUN(&e, "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
            "b4 = ui.box{} w = ui.label{parent = b4}");
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

/* An lv_*_create that gives NULL (LVGL had no memory) is a refused create: the bits stay, no pointer is kept, and the next
 * pass makes the widget. A button whose label can't be made is deleted again, whole, and is not recorded. */
static void a_create_that_fails_is_tried_again(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    uint32_t bits = W(&e, 0)->dirty;
    create_fail_in = 1;
    CHECK(APPLY(&e), "the pass ran");
    LOG_IS("create_failed label\n");  /* and nothing else: not a style on an object that isn't there, not a layout */
    CHECK(stats_since().refused_creates == 1 && stats_since().rejects == 0 && W(&e, 0)->dirty == bits && fake_alive() == 1,
          "refused %u, rejects %u, bits %u (wanted %u), %d objects", stats_since().refused_creates, stats_since().rejects,
          W(&e, 0)->dirty, bits, fake_alive());
    CHECK(APPLY(&e), "the next pass");
    CHECK(log_count("create label 2 in 1") == 1 && log_has_line("text 2 \"x\"") && W(&e, 0)->dirty == 0 && stats_since().refused_creates == 1,
          "made\n%s", log_peek());
    FINISH(&e);

    /* The label of a button is the second create. The button is deleted, with what is inside it, at once. */
    scene(&e);
    RUN(&e, "b = ui.button{text = 'go'}");
    bits = W(&e, 0)->dirty;
    create_fail_in = 2;
    CHECK(APPLY(&e), "the pass ran");
    LOG_IS(
        "create button 2 in 1\n"
        "remove_style_all 2\n"
        "remove_flag 2 scrollable\n"
        "remove_flag 2 clickable\n"
        "create_failed label\n"
        "delete 2\n");
    CHECK(fake_alive() == 1 && stats_since().refused_creates == 1 && stats_since().rejects == 0 && W(&e, 0)->dirty == bits,
          "no half-made button is left: %d objects, refused %u, bits %u (wanted %u)", fake_alive(), stats_since().refused_creates,
          W(&e, 0)->dirty, bits);
    /* No record of the button: the next pass finds no object to delete (which the fake would count as a call on a deleted one),
     * and makes a whole one. */
    CHECK(APPLY(&e), "the next pass");
    CHECK(log_count("delete ") == 0 && log_count("create button 3 in 1") == 1 && log_count("create label 4 in 3") == 1 &&
              log_has_line("text 4 \"go\"") && fake_alive() == 3,
          "a whole button\n%s", log_peek());
    FINISH(&e);

    /* A box made for a widget that is in it, and then that widget's create fails: the box stays (it is made, and its own slot
     * applies it), and the widget is made into it by the next pass. */
    scene(&e);
    RUN(&e, "x = ui.label{} b = ui.box{w = 50, h = 50} x:delete() c = ui.label{text = 'in', parent = b}");
    create_fail_in = 2;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_find("create obj 2 in 1") == 0 && log_count("create_failed label") == 1 && log_count("create label") == 0,
          "the box is made, the label is not\n%s", log_peek());
    HAS("size 2 50 50");
    CHECK(stats_since().refused_creates == 1 && stats_since().rejects == 0 && W(&e, 1)->dirty == 0 &&
              (W(&e, 0)->dirty & APPS_DIRTY_CREATE) != 0,
          "the label is refused and waits: refused %u, its bits %u", stats_since().refused_creates, W(&e, 0)->dirty);
    log_clear();
    CHECK(APPLY(&e), "the next pass");
    CHECK(log_count("create ") == 1 && log_has_line("create label 3 in 2") && W(&e, 0)->dirty == 0,
          "the label goes into the box that is there\n%s", log_peek());
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

    /* No table published: not a pass, and not a skip. */
    unpublish();
    CHECK(!APPLY(&e), "no table");
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

/* ---- Who holds the table ---- */

/* An app ends as Task 12's C6 has it: the pointer is cleared with the lock held, and then the table is freed. The
 * frames that come after find nothing, and nothing they do touches the freed table: this harness runs under
 * AddressSanitizer, which stops on a read of it. */
static void a_cleared_table_can_be_freed(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'} b = ui.arc{w = 100, h = 100, value = 3} c = ui.box{} d = ui.label{text = 'in', parent = c}");
    CHECK(APPLY(&e), "made");
    int alive = fake_alive();
    apps_lv_stats_t base = stats_since();
    log_clear();

    env_close(&e);
    long calls = lvgl_calls;
    for (int i = 0; i < 4; i++) {
        CHECK(!APPLY(&e), "frame %d: nothing is published", i);
    }
    apps_lv_stats_t s = stats_since();
    CHECK(lvgl_calls == calls, "%ld LVGL calls", lvgl_calls - calls);
    LOG_EMPTY();
    CHECK(s.applies == base.applies && s.skips == base.skips && s.rejects == base.rejects &&
              s.refused_creates == base.refused_creates && s.slow == base.slow,
          "nothing counted: applies %u skips %u rejects %u", s.applies - base.applies, s.skips - base.skips, s.rejects - base.rejects);
    CHECK(fake_alive() == alive, "the objects stay until the layer is reset: %d, was %d", fake_alive(), alive);
    apps_lv_reset();
    CHECK(fake_alive() == 1, "only the layer is left: %d", fake_alive());

    /* The next app: a new table, applied from nothing. */
    CHECK(env_open(&e, ASSETS, 2), "env_open");
    RUN(&e, "z = ui.label{text = 'new'}");
    CHECK(APPLY(&e), "the next app's table is applied");
    CHECK(log_count("create label") == 1 && log_has_line("bg_color 1 #000000"), "made from nothing\n%s", log_peek());
    FINISH(&e);
}

/* What the app task does while the tick waits for the lock. */
static void app_task_ends_first(void *arg) {
    env_close((env_t *)arg);
}

/* The pass has to read the pointer with the lock held. Read before it, the pointer is one the app task may clear and
 * free while the pass waits: here it does, in the lock call itself. */
static void the_table_is_read_under_the_lock(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    apps_lv_stats_t base = stats_since();
    long calls = lvgl_calls;
    trylock_hook = app_task_ends_first;
    trylock_hook_arg = &e;
    CHECK(!APPLY(&e), "the app ended while the pass waited: nothing to apply");
    CHECK(trylock_hook == NULL && e.ui == NULL && e.vm == NULL, "the app task's turn came");
    apps_lv_stats_t s = stats_since();
    CHECK(lvgl_calls == calls && s.applies == base.applies && s.skips == base.skips && s.rejects == base.rejects,
          "no call, nothing counted: %ld calls, applies %u skips %u rejects %u", lvgl_calls - calls, s.applies - base.applies,
          s.skips - base.skips, s.rejects - base.rejects);
    FINISH(&e);
}

/* The first pass of a layer lets go of the lock to make its records, and the app task can end in that gap: the
 * pointer read before it is a table that may be gone, so it is read again after. */
static void the_app_can_end_while_the_records_are_made(void) {
    env_t e;
    scene_raw(&e);  /* no pass yet: no records */
    RUN(&e, "a = ui.label{text = 'x'}");
    apps_lv_stats_t base = stats_since();
    long calls = lvgl_calls;
    realloc_hook = app_task_ends_first;
    realloc_hook_arg = &e;
    CHECK(!APPLY(&e), "the app ended while the records were made: nothing to apply");
    CHECK(realloc_hook == NULL && e.ui == NULL, "the app task's turn came");
    apps_lv_stats_t s = stats_since();
    CHECK(lvgl_calls == calls && s.applies == base.applies && s.skips == base.skips && s.rejects == base.rejects,
          "no call, nothing counted: %ld calls, applies %u skips %u rejects %u", lvgl_calls - calls, s.applies - base.applies,
          s.skips - base.skips, s.rejects - base.rejects);
    FINISH(&e);
}

static void refuse_the_next_lock(void *arg) {
    (void)arg;
    trylock_refusals = 1;
}

/* And the lock may not be free when the pass comes back for it: a skip, nothing left locked, and the next frame is the
 * pass, with the records already there. */
static void the_lock_may_be_refused_after_the_records_are_made(void) {
    env_t e;
    scene_raw(&e);
    RUN(&e, "a = ui.label{text = 'x'}");
    apps_lv_stats_t base = stats_since();
    long calls = lvgl_calls;
    realloc_hook = refuse_the_next_lock;
    CHECK(!APPLY(&e), "the lock was refused the second time");
    apps_lv_stats_t s = stats_since();
    CHECK(lvgl_calls == calls && s.skips == base.skips + 1 && s.applies == base.applies, "no call, a skip: %ld calls, skips %u applies %u",
          lvgl_calls - calls, s.skips - base.skips, s.applies - base.applies);
    CHECK(APPLY(&e) && log_count("create label") == 1, "the next frame applies it\n%s", log_peek());
    FINISH(&e);
}

/* No table published is a frame with nothing to do: not a skip, not a reject, not an apply, no LVGL call, and no
 * memory made for it. The pointer is read under the lock, so the lock was asked for. */
static void no_table_is_no_pass(void) {
    env_t e;
    scene_raw(&e);  /* the layer and a published table, but no pass yet: no records */
    unpublish();
    size_t before = held;
    long grown = growths, trylocks = trylock_calls, takes = lock_takes, calls = lvgl_calls;
    for (int i = 0; i < 5; i++) {
        CHECK(!APPLY(&e), "frame %d: nothing is published", i);
    }
    CHECK(trylock_calls == trylocks + 5 && lock_takes == takes + 5 && trylock_ms == 2,
          "the pointer is read under the lock: %ld tries, %ld taken, asked for %d ms", trylock_calls - trylocks, lock_takes - takes,
          trylock_ms);
    CHECK(lvgl_calls == calls && held == before && growths == grown, "no LVGL call, nothing allocated");
    apps_lv_stats_t s = stats_since();
    CHECK(s.applies == 0 && s.skips == 0 && s.rejects == 0 && s.refused_creates == 0 && s.slow == 0,
          "nothing counted: applies %u skips %u rejects %u refused %u slow %u", s.applies, s.skips, s.rejects, s.refused_creates, s.slow);
    /* Published, the table is applied: what the app made meanwhile is drawn. */
    RUN(&e, "a = ui.label{text = 'x'}");
    publish(&e);
    CHECK(APPLY(&e), "published");
    CHECK(log_count("create label") == 1, "made\n%s", log_peek());
    FINISH(&e);
}

/* A frame inside a callback is skipped before the lock is asked for and before the table is read, so with no table
 * published up to three such frames in a row still count as skips (Task 12 passes in_callback = false when no app is
 * published: no callback can run without one). The 4th goes on to read the pointer and finds nothing: not a skip. */
static void a_callback_frame_is_skipped_before_the_table_is_read(void) {
    env_t e;
    scene_raw(&e);
    unpublish();
    long trylocks = trylock_calls;
    for (int frame = 1; frame <= 3; frame++) {
        CHECK(!APPLY_CB(&e), "frame %d is skipped", frame);
    }
    CHECK(trylock_calls == trylocks && stats_since().skips == 3, "skipped before the lock: %ld trylocks, %u skips",
          trylock_calls - trylocks, stats_since().skips);
    CHECK(!APPLY_CB(&e), "the 4th has no table");
    CHECK(trylock_calls == trylocks + 1 && stats_since().skips == 3 && stats_since().applies == 0,
          "it read the pointer under the lock and counted nothing: %ld trylocks, %u skips, %u applies", trylock_calls - trylocks,
          stats_since().skips, stats_since().applies);
    CHECK(!APPLY_CB(&e) && trylock_calls == trylocks + 2 && stats_since().skips == 3, "and so does every frame after it");
    /* A frame outside a callback is no skip, and it ends the run: the next three in a callback are skipped again. */
    CHECK(!APPLY(&e) && stats_since().skips == 3, "outside a callback nothing is counted");
    CHECK(!APPLY_CB(&e) && stats_since().skips == 4 && trylock_calls == trylocks + 3, "a new run: skipped, before the lock");
    FINISH(&e);
}

/* Passes are the published table's, whichever it is; the one that isn't published is left as it was. */
static void the_published_table_is_the_one_applied(void) {
    env_t first, second;
    scene(&first);
    RUN(&first, "a = ui.label{text = 'one'}");
    CHECK(APPLY(&first), "the first app's label is made");
    RUN(&first, "a:set{text = 'changed'}");
    log_clear();

    CHECK(env_open(&second, ASSETS, 2), "env_open");  /* an empty table, published */
    CHECK(APPLY(&first), "a pass for the second table");
    LOG_IS("bg_color 1 #000000\n");  /* its first take of the background, and nothing of the first's change */
    CHECK(W(&first, 0)->dirty == APPS_DIRTY_TEXT, "the first table is untouched: bits %u", W(&first, 0)->dirty);

    publish(&first);
    CHECK(APPLY(&first), "a pass for the first again");
    LOG_IS(
        "text 2 \"changed\"\n"
        "update_layout 1\n");
    env_close(&second);
    FINISH(&first);
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
    CHECK(fake_obj(old_obj)->n_cbs == 5, "five callbacks: %d", fake_obj(old_obj)->n_cbs);
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
              log_find("add_event_cb 2 pressed") < log_find("add_event_cb 2 released") &&
              log_find("add_event_cb 2 released") < log_find("add_event_cb 2 press_lost"),
          "one callback each for the five events, in the order of the table\n%s", log_peek());
    CHECK(log_count("add_event_cb") == 5, "five registrations\n%s", log_peek());
    log_clear();

    /* A long press: LVGL sends PRESSED, LONG_PRESSED, and on the lift RELEASED and then CLICKED (it follows a long press
     * too, and no SHORT_CLICKED does): the app hears press, long_press, release. */
    n_evs = 0;
    fake_press(2);
    fake_hold();
    fake_lift();
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_LONG_PRESS && evs[2].event == APPS_UI_RELEASE,
          "press, long_press, release: %d events", n_evs);
    /* A short tap: PRESSED, and on the lift RELEASED, SHORT_CLICKED, CLICKED, in that order: press, release, tap. */
    n_evs = 0;
    fake_tap(2);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_RELEASE && evs[2].event == APPS_UI_TAP,
          "press, release, tap: %d events", n_evs);
    CHECK(evs[2].slot == 0 && evs[2].gen == 0, "slot %d gen %d", evs[2].slot, evs[2].gen);
    /* PRESS_LOST is the app's release: it is what LVGL sends instead of RELEASED when the touch was taken away. */
    n_evs = 0;
    fake_fire(2, LV_EVENT_PRESS_LOST);
    CHECK(n_evs == 1 && evs[0].event == APPS_UI_RELEASE && evs[0].slot == 0, "release from PRESS_LOST: %d events", n_evs);
    /* The other events LVGL sends a clickable object are not the app's. */
    n_evs = 0;
    static const lv_event_code_t others[] = {LV_EVENT_PRESSING, LV_EVENT_SINGLE_CLICKED, LV_EVENT_DOUBLE_CLICKED,
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
        /* A swipe that starts on it has to reach the layer: LVGL's indev_gesture climbs while the objects pass it up. */
        CHECK(fake_flag(id, LV_OBJ_FLAG_GESTURE_BUBBLE), "%s: it doesn't pass a swipe up to the layer", kinds[i]);
        CHECK(fake_obj(id)->n_cbs == 0, "%s: callbacks with no event", kinds[i]);
        RUN(&e, "w:on('release', function() end)");
        CHECK(APPLY(&e), "subscribed");
        int callbacks = i == 3 ? 6 : 5;  /* an arc has a guard against being turned by a finger as well */
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
    CHECK(fake_obj(guarded)->n_cbs == 6 && fake_obj(bare)->n_cbs == 0, "callbacks %d and %d", fake_obj(guarded)->n_cbs,
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
    fake_tap(guarded);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_RELEASE && evs[2].event == APPS_UI_TAP,
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
    /* A change of style makes no call for the size or the alignment: only an image's box follows its style (its src and scale). */
    RUN(&e, "a:set{opacity = 50}");
    CHECK(APPLY(&e), "restyled");
    HAS("opa 2 127");
    CHECK(log_count("size ") == 0 && log_count("align ") == 0, "no geometry for a style\n%s", log_peek());
    log_clear();
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

/* A right swipe that starts on a widget the app listens to. LVGL sends PRESSED when the finger goes down (the app hears
 * press); the swipe climbs to the layer (widgets keep GESTURE_BUBBLE), which closes the app and tells LVGL the rest of the
 * touch is nobody's (lv_indev_wait_release); and the lift then sends the widget PRESS_LOST, not RELEASED, and no click.
 * The app has to hear press, then release, with the close between them: no press left open, no tap made of a swipe. */
static void a_swipe_that_starts_on_a_widget_ends_its_press(void) {
    env_t e;
    scene(&e);
    apps_lv_set_event_cb(on_event);
    apps_lv_set_close_cb(on_close);
    closes = 0;
    n_evs = 0;
    /* A button in a box, as a watch app has them: the swipe has to climb past two objects. */
    RUN(&e, "g = ui.box{w = 200, h = 100} b = ui.button{text = 'go', parent = g}\n"
            "b:on('press', function() end) b:on('release', function() end) b:on('tap', function() end)");
    CHECK(APPLY(&e), "made");
    int button = fake_nth("button", 0);
    fake_press(button);
    CHECK(n_evs == 1 && evs[0].event == APPS_UI_PRESS && evs[0].slot == 1 && closes == 0, "the press: %d events, %d closes", n_evs, closes);
    fake_swipe(LV_DIR_RIGHT);
    CHECK(closes == 1 && n_evs == 1, "the swipe closes the app and says nothing to the widget: %d closes, %d events", closes, n_evs);
    HAS("wait_release");
    fake_lift();
    CHECK(n_evs == 2 && evs[1].event == APPS_UI_RELEASE && evs[1].slot == 1 && evs[1].gen == 0 && evs[1].closes_before == 1,
          "press, then release after the close: %d events", n_evs);
    CHECK(closes == 1, "and it is not closed twice: %d", closes);

    /* A swipe that is not to the right closes nothing, and the touch ends as any other: press, release, tap. */
    n_evs = 0;
    fake_press(button);
    fake_swipe(LV_DIR_LEFT);
    fake_lift();
    CHECK(closes == 1 && n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_RELEASE && evs[2].event == APPS_UI_TAP,
          "a left swipe: %d closes, %d events", closes, n_evs);
    /* The next touch is an ordinary one: the layer's wish to wait for the release doesn't outlive the touch. */
    n_evs = 0;
    fake_tap(button);
    CHECK(n_evs == 3 && evs[0].event == APPS_UI_PRESS && evs[1].event == APPS_UI_RELEASE && evs[2].event == APPS_UI_TAP,
          "the next tap: %d events", n_evs);
    FINISH(&e);
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
    static const uint8_t allocated[] = {0x19, 0x12, 0x10, 0, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t modifiable[] = {0x19, 0x12, 0x21, 0, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const uint8_t all_but_compressed[] = {0x19, 0x12, 0xF7, 0xFF, 2, 0, 1, 0, 4, 0, 0, 0, 1, 2, 3, 4};
    static const struct {
        const char *what;
        const uint8_t *bytes;
        size_t len;
    } cases[] = {
        {"the wrong magic", bad_magic, sizeof bad_magic},
        {"fewer bytes than the header says", short_data, sizeof short_data},
        {"a compressed image", compressed, sizeof compressed},
        {"the allocated flag (LVGL would take the descriptor for a draw buffer)", allocated, sizeof allocated},
        {"premultiplied with the modifiable flag", modifiable, sizeof modifiable},
        {"every flag but compressed", all_but_compressed, sizeof all_but_compressed},
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

/* ---- What an image shows ---- */

/* Images a case needs, all valid LVGL v9 files (apps_image_check passes them). */
static const uint8_t IMG_RGB565A8[] = {0x19, 0x14, 0, 0, 3, 0, 1, 0, 6, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};  /* 3x1: 6 colour bytes, 3 alpha */
static const apps_asset_t ASSETS_FORMATS[] = {{"a.bin", IMG_A, sizeof IMG_A, 2, 1},
                                              {"b.bin", IMG_B, sizeof IMG_B, 3, 2},
                                              {"d.bin", IMG_RGB565A8, sizeof IMG_RGB565A8, 3, 1}};

/* LVGL draws an A8 image as a mask filled with the image recolor, which is black unless it is set: invisible on the layer's
 * black. So an A8 image gets white, in full. RGB565 and RGB565A8 have their own colours and are left alone. */
static void a8_images_are_white(void) {
    env_t e;
    scene_with(&e, ASSETS_FORMATS, 3);
    RUN(&e, "a = ui.image{src = 'a.bin'} b = ui.image{src = 'b.bin'} d = ui.image{src = 'd.bin'}");
    CHECK(APPLY(&e), "made");
    int rgb565 = fake_nth("image", 0), a8 = fake_nth("image", 1), rgb565a8 = fake_nth("image", 2);
    CHECK(fake_obj(rgb565)->src->header.cf == 0x12 && fake_obj(a8)->src->header.cf == 0x0E && fake_obj(rgb565a8)->src->header.cf == 0x14,
          "the three formats");
    char line[64];
    snprintf(line, sizeof line, "image_recolor %d #ffffff", a8);
    HAS(line);
    snprintf(line, sizeof line, "image_recolor_opa %d 255", a8);
    HAS(line);
    CHECK(log_count("image_recolor") == 2, "no other image is recoloured\n%s", log_peek());
    /* It is the image's style, so a change of style that applies the style again gives the same. */
    log_clear();
    RUN(&e, "b:set{opacity = 50}");
    CHECK(APPLY(&e), "changed");
    snprintf(line, sizeof line, "image_recolor_opa %d 255", a8);
    HAS(line);

    /* A src that is not A8 any more is drawn as it is: the recolor that was set goes (opacity 0), and is not set again by a
     * later change of style. And when it is A8 again it is white again. */
    log_clear();
    RUN(&e, "b:set{src = 'a.bin'}");
    CHECK(APPLY(&e), "an RGB565 image now");
    snprintf(line, sizeof line, "image_recolor_opa %d 0", a8);
    HAS(line);
    CHECK(log_count("image_recolor ") == 0, "only the opacity is put back\n%s", log_peek());
    log_clear();
    RUN(&e, "b:set{opacity = 60}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_count("image_recolor") == 0, "nothing to recolor\n%s", log_peek());
    log_clear();
    RUN(&e, "b:set{src = 'b.bin'}");
    CHECK(APPLY(&e), "A8 again");
    snprintf(line, sizeof line, "image_recolor %d #ffffff", a8);
    HAS(line);
    snprintf(line, sizeof line, "image_recolor_opa %d 255", a8);
    HAS(line);
    FINISH(&e);
}

/* Images of the sizes a case needs: 40x20, 300x2 (up to the edge of the screen when it is scaled) and 5x1 (down to a pixel). */
static const uint8_t IMG_C[12 + 40 * 2 * 20] = {0x19, 0x12, 0, 0, 40, 0, 20, 0, 80, 0, 0, 0};
static const uint8_t IMG_WIDE[12 + 300 * 2 * 2] = {0x19, 0x12, 0, 0, 0x2C, 0x01, 2, 0, 0x58, 0x02, 0, 0};
static const uint8_t IMG_TINY[12 + 5 * 2] = {0x19, 0x12, 0, 0, 5, 0, 1, 0, 10, 0, 0, 0};
static const apps_asset_t ASSETS_SIZES[] = {{"c.bin", IMG_C, sizeof IMG_C, 40, 20},
                                            {"wide.bin", IMG_WIDE, sizeof IMG_WIDE, 300, 2},
                                            {"tiny.bin", IMG_TINY, sizeof IMG_TINY, 5, 1}};

/* The box the pass wrote back for the widget in slot (its object is id), and where the fake puts the picture in it. */
static void box_is_at(int line, env_t *e, int slot, int id, int w, int h, const char *what) {
    const apps_widget_t *s = W(e, slot);
    CHECK_AT(line, s->rw == w && s->rh == h, "%s: written back %dx%d, wanted %dx%d", what, s->rw, s->rh, w, h);
    lv_area_t d = fake_drawn(id);
    CHECK_AT(line,
             abs(d.x1 - s->rx) <= 1 && abs(d.y1 - s->ry) <= 1 && abs(d.x2 - (s->rx + s->rw - 1)) <= 1 &&
                 abs(d.y2 - (s->ry + s->rh - 1)) <= 1,
             "%s: the picture is drawn at %d,%d..%d,%d in a box at %d,%d %dx%d", what, d.x1, d.y1, d.x2, d.y2, s->rx, s->ry, s->rw,
             s->rh);
}
#define BOX_IS(e, slot, id, w, h) box_is_at(__LINE__, (e), (slot), (id), (w), (h), #w "x" #h)

/* In LVGL 9.5.0 an image's own size is the image as it is: a scale draws it larger or smaller about the middle of its box
 * and never changes the box. So for a side the app left to the content, the box is set to the scaled size, the picture
 * is centred in it and scaled about its middle: what the app reads back, and the overflow, are what is on the screen. */
static void scaled_images_have_the_scaled_box(void) {
    env_t e;
    scene_with(&e, ASSETS_SIZES, 3);
    RUN(&e, "i = ui.image{src = 'c.bin', scale = 200}");
    CHECK(APPLY(&e), "made");
    HAS("size 2 80 40");
    HAS("image_inner_align 2 center");
    HAS("image_scale 2 512");
    BOX_IS(&e, 0, 2, 80, 40);
    CHECK(W(&e, 0)->rx == 193 && W(&e, 0)->ry == 213, "centred on the screen: %d,%d", W(&e, 0)->rx, W(&e, 0)->ry);

    /* A change of scale alone sizes it again. */
    log_clear();
    RUN(&e, "i:set{scale = 50}");
    CHECK(APPLY(&e), "changed");
    HAS("size 2 20 10");
    BOX_IS(&e, 0, 2, 20, 10);
    log_clear();
    RUN(&e, "i:set{scale = 100}");
    CHECK(APPLY(&e), "back to its own size");
    HAS("size 2 content content");
    BOX_IS(&e, 0, 2, 40, 20);
    /* To the nearest pixel: 13.2 and 6.6. */
    log_clear();
    RUN(&e, "i:set{scale = 33}");
    CHECK(APPLY(&e), "changed");
    HAS("size 2 13 7");
    BOX_IS(&e, 0, 2, 13, 7);
    /* A side the app sized is its own; the other is the scaled one. */
    log_clear();
    RUN(&e, "i:set{scale = 200, w = 100}");
    CHECK(APPLY(&e), "changed");
    HAS("size 2 100 40");
    CHECK(W(&e, 0)->rw == 100 && W(&e, 0)->rh == 40, "the app's width, the scaled height: %dx%d", W(&e, 0)->rw, W(&e, 0)->rh);
    log_clear();
    RUN(&e, "j = ui.image{src = 'c.bin', scale = 200, h = 30}");  /* the other side: the app's height, the scaled width */
    CHECK(APPLY(&e), "made");
    HAS("size 3 80 30");
    CHECK(W(&e, 1)->rw == 80 && W(&e, 1)->rh == 30, "the scaled width, the app's height: %dx%d", W(&e, 1)->rw, W(&e, 1)->rh);
    log_clear();
    RUN(&e, "i:set{h = 30}");
    CHECK(APPLY(&e), "changed");
    HAS("size 2 100 30");
    CHECK(W(&e, 0)->rw == 100 && W(&e, 0)->rh == 30, "both the app's: %dx%d", W(&e, 0)->rw, W(&e, 0)->rh);
    /* In a box the app sized the picture is centred and scaled about its middle: 80x40 in 100x30, 5 px over at the top and bottom
     * (which is why the overflow of such a box is the box's). */
    {
        const apps_widget_t *s = W(&e, 0);
        lv_area_t d = fake_drawn(2);
        CHECK(d.x2 - d.x1 + 1 == 80 && d.y2 - d.y1 + 1 == 40 && d.x1 + d.x2 == 2 * s->rx + s->rw - 1 && d.y1 == s->ry - 5,
              "drawn %d,%d..%d,%d in a box at %d,%d %dx%d", d.x1, d.y1, d.x2, d.y2, s->rx, s->ry, s->rw, s->rh);
    }
    FINISH(&e);

    /* Up to the screen and down to a pixel. */
    scene_with(&e, ASSETS_SIZES, 3);
    RUN(&e, "a = ui.image{src = 'wide.bin', scale = 400} b = ui.image{src = 'tiny.bin', scale = 10}");
    CHECK(APPLY(&e), "made");
    HAS("size 2 466 8");  /* 1200 wide is clamped to the screen */
    HAS("size 3 1 1");    /* 0.5 and 0.1 pixels are one */
    FINISH(&e);

    /* The overflow is that of the box on the screen: 80x40 at (400, 213) pokes 15 px past the round edge, where the unscaled
     * 40x20 is inside it. */
    scene_with(&e, ASSETS_SIZES, 3);
    RUN(&e, "i = ui.image{src = 'c.bin', scale = 200, align = 'top_left', x = 400, y = 213}");
    CHECK(APPLY(&e), "made");
    CHECK(W(&e, 0)->rx == 400 && W(&e, 0)->rw == 80 && W(&e, 0)->rh == 40 && W(&e, 0)->overflow == 15,
          "the scaled box %d, %dx%d, overflow %d", W(&e, 0)->rx, W(&e, 0)->rw, W(&e, 0)->rh, W(&e, 0)->overflow);
    RUN(&e, "i:set{scale = 100}");
    CHECK(APPLY(&e), "back to its own size");
    CHECK(W(&e, 0)->rw == 40 && W(&e, 0)->overflow == 0, "unscaled: %d wide, overflow %d", W(&e, 0)->rw, W(&e, 0)->overflow);
    FINISH(&e);
}

/* Of an image's flags only premultiplied alpha belongs in the descriptor: the others (allocated, modifiable...) say what
 * the memory is, and LVGL takes an allocated descriptor for one of its draw buffers and refuses it. apps_image_check
 * refuses them; the descriptor is made with the rest masked off as well. The bytes here pass the check and change as the
 * object is made (what the check saw is not what is copied), so only the mask can keep them out. */
static uint8_t flagged_bytes[sizeof IMG_A];

static void set_stray_flags(void *arg) {
    (void)arg;
    flagged_bytes[2] = 0x31;  /* premultiplied, allocated, modifiable */
}

static void image_flags_are_masked(void) {
    memcpy(flagged_bytes, IMG_A, sizeof flagged_bytes);
    const apps_asset_t assets[] = {{"f.bin", flagged_bytes, sizeof flagged_bytes, 2, 1}};
    env_t e;
    scene_with(&e, assets, 1);
    RUN(&e, "i = ui.image{src = 'f.bin'}");
    image_hook = set_stray_flags;
    CHECK(APPLY(&e), "made");
    const lv_obj_t *img = fake_obj(2);
    CHECK(img->src != NULL && img->src->header.flags == LV_IMAGE_FLAGS_PREMULTIPLIED, "only premultiplied is kept: 0x%04x",
          img->src != NULL ? (unsigned)img->src->header.flags : 0xFFFFu);
    FINISH(&e);

    /* Premultiplied alone is a valid file, and stays. */
    memcpy(flagged_bytes, IMG_A, sizeof flagged_bytes);
    flagged_bytes[2] = 0x01;
    scene_with(&e, assets, 1);
    RUN(&e, "i = ui.image{src = 'f.bin'}");
    CHECK(APPLY(&e), "made");
    img = fake_obj(2);
    CHECK(stats_since().rejects == 0 && img->src != NULL && img->src->header.flags == LV_IMAGE_FLAGS_PREMULTIPLIED,
          "premultiplied is a valid file: %u rejects, flags 0x%04x", stats_since().rejects,
          img->src != NULL ? (unsigned)img->src->header.flags : 0xFFFFu);
    FINISH(&e);
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

    /* The delete in a higher slot than the create, where plain slot order would make first: b is made in slot 0, below the
     * slot of a's pending delete. */
    scene(&e);
    RUN(&e, "x = ui.label{} a = ui.label{text = 'a'}");
    CHECK(APPLY(&e), "made");
    RUN(&e, "x:delete()");
    CHECK(APPLY(&e), "slot 0 is free");
    log_clear();
    RUN(&e, "a:delete() b = ui.label{text = 'b'}");
    CHECK(W(&e, 1)->dirty == APPS_DIRTY_DELETE && W(&e, 0)->alive && W(&e, 0)->gen == 1,
          "b went to slot 0, below the slot of a's delete");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_find("delete 3") == 0 && log_find("create label 4 in 1") > 0, "the delete of the higher slot comes first\n%s", log_peek());
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

    /* Four deep, the most boxes there may be above a widget: the leaf is in the lowest slot and every box above it in a
     * higher one, so the boxes are made from inside the leaf's visit, four levels down. */
    scene(&e);
    RUN(&e, "p0 = ui.label{} p1 = ui.label{} p2 = ui.label{} p3 = ui.label{}\n"
            "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2}\n"
            "p0:delete() p1:delete() p2:delete() p3:delete()\n"
            "l = ui.label{text = 'deep', parent = b3}");
    CHECK(W(&e, 0)->parent == 7 && W(&e, 4)->parent == -1 && W(&e, 5)->parent == 4 && W(&e, 6)->parent == 5 && W(&e, 7)->parent == 6,
          "the leaf sits in slot 0, below the four boxes (slots 4 to 7)");
    CHECK(APPLY(&e), "the pass ran");
    CHECK(log_count("create ") == 5 && stats_since().rejects == 0 && fake_errors == 0, "five objects, no reject\n%s", log_peek());
    int top = log_find("create obj 2 in 1"), second = log_find("create obj 3 in 2"), third = log_find("create obj 4 in 3");
    int fourth = log_find("create obj 5 in 4"), leaf = log_find("create label 6 in 5");
    CHECK(top >= 0 && top < second && second < third && third < fourth && fourth < leaf, "made from the top down\n%s", log_peek());
    CHECK(W(&e, 0)->dirty == 0 && W(&e, 4)->dirty == 0 && W(&e, 7)->dirty == 0, "all clear");
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
    /* The first slow pass is logged at once, with how many there were since the last line (this one). */
    char want[96];
    snprintf(want, sizeof want, "W muse_apps: apply %ld us (1 slow since last)", calls * 500);
    CHECK(stats_since().slow == 1 && esp_has_line(want) && esp_count("") == 1, "slow %u, wanted \"%s\"\n%s", stats_since().slow, want,
          esp_peek());
    int64_t logged_at = clock_us;  /* the pass ended now */
    log_clear();
    esp_clear();

    /* Over 2 ms, not 2 ms: the border. A slow pass inside the second after a line is counted and not logged. */
    call_cost_us = 0;
    RUN(&e, "a:set{text = 'y'}");
    extra_us_next = 2000;
    CHECK(APPLY(&e), "2000 us");
    CHECK(stats_since().slow == 1 && esp_count("") == 0, "2000 us is not slow\n%s", esp_peek());
    RUN(&e, "a:set{text = 'z'}");
    extra_us_next = 2001;
    CHECK(APPLY(&e), "2001 us");
    CHECK(stats_since().slow == 2 && esp_count("") == 0, "2001 us is slow, counted, and not logged: the last line is under a second old\n%s",
          esp_peek());
    RUN(&e, "a:set{text = 'w'}");
    extra_us_next = 4000;
    CHECK(APPLY(&e), "4000 us");
    CHECK(stats_since().slow == 3 && esp_count("") == 0, "counted\n%s", esp_peek());
    /* A microsecond short of a second after the line: still not. */
    clock_us = logged_at + 1000000 - 1 - 3000;
    RUN(&e, "a:set{text = 'v'}");
    extra_us_next = 3000;
    CHECK(APPLY(&e), "3000 us, a microsecond early");
    CHECK(stats_since().slow == 4 && esp_count("") == 0, "not yet\n%s", esp_peek());
    /* A second after it: the line says the worst time and how many slow passes it stands for. */
    clock_us = logged_at + 1000000 - 2500;
    RUN(&e, "a:set{text = 'u'}");
    extra_us_next = 2500;
    CHECK(APPLY(&e), "2500 us, a second after the last line");
    CHECK(stats_since().slow == 5 && esp_count("") == 1 && esp_has_line("W muse_apps: apply 4000 us (4 slow since last)"),
          "one line, the worst of the four since the last\n%s", esp_peek());
    esp_clear();
    /* The count starts again after a line. */
    clock_us += 2000000;
    RUN(&e, "a:set{text = 't'}");
    extra_us_next = 2100;
    CHECK(APPLY(&e), "2100 us, long after");
    CHECK(stats_since().slow == 6 && esp_count("") == 1 && esp_has_line("W muse_apps: apply 2100 us (1 slow since last)"),
          "the count started over\n%s", esp_peek());
    log_clear();
    esp_clear();

    /* Waiting for the lock isn't the pass's time, and a pass that was skipped isn't slow. */
    clock_us += 2000000;
    RUN(&e, "a:set{text = 's'}");
    trylock_refusals = 1;
    extra_us_next = 5000;
    CHECK(!APPLY(&e), "skipped");
    CHECK(stats_since().slow == 6 && esp_count("") == 0, "not slow");
    extra_us_next = 0;
    CHECK(APPLY(&e), "applied");
    CHECK(stats_since().slow == 6 && esp_count("") == 0, "and fast");
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
    /* The table is read under the lock, and the heap isn't used with it held: the lock was taken once to find a table,
     * given back, and the pass stopped there. */
    CHECK(lvgl_calls == calls && trylock_calls == trylocks + 1 && !lock_held && W(&e, 0)->dirty != 0,
          "one look at the table, no call, the bits wait: %ld trylocks", trylock_calls - trylocks);
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

/* Random calls into the ui module, random passes (skipped, refused for PSRAM or for LVGL's own memory, in a callback),
 * touches of objects that may be gone, and, for the odd seeds, a table corrupted for one pass. The fake LVGL counts every call on a
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
                create_fail_in = rnd(10) == 0 ? (int)rnd(4) + 1 : 0;  /* LVGL has no memory for one of this pass's creates */
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
                create_fail_in = 0;
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
                                                            LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED,
                                                            LV_EVENT_GESTURE};
                    fake_fire(1 + (int)rnd((uint32_t)n_objs), codes[rnd(ARRAY_LEN(codes))]);
                    n_evs = 0;
                }
                break;
            case 98:  /* the app ends (the pointer cleared, the layer reset, the table freed) and the next one starts */
                unpublish();
                apply_at(__LINE__, &e, false);  /* a frame in between finds nothing */
                apps_lv_reset();
                env_close(&e);
                apply_at(__LINE__, &e, false);
                CHECK(env_open(&e, ASSETS, 2), "env_open");
                RUN(&e, "H = {} N = 0");
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

/* ---- The launcher and the crash card (Task 12) ---- */

/* The id of the live label whose text is text, or -1. */
static int fake_label(const char *text) {
    for (int i = 0; i < n_objs; i++) {
        if (!objs[i]->deleted && strcmp(objs[i]->cls, "label") == 0 && objs[i]->text != NULL && strcmp(objs[i]->text, text) == 0) {
            return objs[i]->id;
        }
    }
    return -1;
}

/* The id of an object's parent: 0 for the screen. */
static int fake_parent(int id) {
    const lv_obj_t *parent = fake_obj(id)->parent;
    return parent == NULL || parent == &screen_obj ? 0 : parent->id;
}

/* How many live children of class cls the object (0: the screen) has. */
static int fake_children_of(int id, const char *cls) {
    const lv_obj_t *parent = id == 0 ? &screen_obj : fake_obj(id);
    int n = 0;
    for (int i = 0; i < n_objs; i++) {
        n += objs[i]->parent == parent && !objs[i]->deleted && strcmp(objs[i]->cls, cls) == 0 ? 1 : 0;
    }
    return n;
}

static int fake_index(int id) {
    return (int)lv_obj_get_index(fake_obj(id));
}

static int picks;
static char picked[APPS_ID_MAX + 1];

static void pick(const char *id) {
    picks++;
    snprintf(picked, sizeof picked, "%s", id);
}

static int cards_image;  /* the stand-in for muse_ui's downloaded image, made after the layer as build_overlays() does */

/* A layer on a fresh screen, and an object made after it, as muse_ui's image is: the cards go between them. */
static void cards_scene(void) {
    fake_reset();
    apps_lv_build(fake_screen());
    cards_image = lv_image_create(fake_screen())->id;
    log_clear();
    picks = 0;
    picked[0] = '\0';
}

/* A card is full-screen, black and takes every touch on it: the tileview under it never sees one. */
static void check_card_at(int line, int card) {
    const lv_obj_t *o = fake_obj(card);
    CHECK_AT(line, fake_parent(card) == 0, "a card is a child of the screen: %d", fake_parent(card));
    CHECK_AT(line, fake_flag(card, LV_OBJ_FLAG_CLICKABLE) && !fake_flag(card, LV_OBJ_FLAG_SCROLLABLE) &&
                       !fake_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE),
             "a card's flags: clickable %d scrollable %d gesture_bubble %d", fake_flag(card, LV_OBJ_FLAG_CLICKABLE),
             fake_flag(card, LV_OBJ_FLAG_SCROLLABLE), fake_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE));
    CHECK_AT(line, o->w == lv_pct(100) && o->h == lv_pct(100), "a card covers the screen");
    /* Over the layer, under what muse_ui made after it. */
    CHECK_AT(line, fake_index(card) > fake_index(1) && fake_index(card) < fake_index(cards_image),
             "a card sits between the layer (%d) and the image (%d): %d", fake_index(1), fake_index(cards_image), fake_index(card));
}
#define CHECK_CARD(card) check_card_at(__LINE__, (card))

static void launcher_lists_apps_and_picks(void) {
    cards_scene();
    const char *ids[] = {"hello", "timer"};
    const char *names[] = {"Hello", "Kitchen timer"};
    apps_lv_show_launcher(ids, names, 2, pick);
    int hello = fake_label("Hello"), timer = fake_label("Kitchen timer"), title = fake_label("Apps");
    CHECK(hello > 0 && timer > 0 && title > 0, "a row for each app, and the title: %d %d %d\n%s", hello, timer, title, log_peek());
    int row = fake_parent(hello), launcher = fake_parent(row);
    CHECK_CARD(launcher);
    CHECK(fake_index(launcher) == fake_index(1) + 1, "the launcher is right over the layer: %d %d", fake_index(launcher), fake_index(1));
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(), "shown");
    CHECK(fake_parent(fake_parent(timer)) == launcher && fake_children_of(launcher, "obj") == 2, "two rows on the launcher");
    CHECK(fake_flag(row, LV_OBJ_FLAG_CLICKABLE) && !fake_flag(row, LV_OBJ_FLAG_SCROLLABLE), "a row is clickable");
    CHECK(!fake_flag(hello, LV_OBJ_FLAG_CLICKABLE) && !fake_flag(title, LV_OBJ_FLAG_CLICKABLE),
          "labels aren't: LVGL gives their touches to what is under them");
    fake_tap(row);
    CHECK(picks == 1 && strcmp(picked, "hello") == 0, "tapping a row runs its app: %d '%s'", picks, picked);
    CHECK(fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "and closes the launcher (C12)");
}

static void launcher_closes_on_a_tap_elsewhere(void) {
    cards_scene();
    const char *ids[] = {"hello"};
    const char *names[] = {"Hello"};
    apps_lv_show_launcher(ids, names, 1, pick);
    int launcher = fake_parent(fake_parent(fake_label("Hello")));
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(),
          "a tap on no row closes the launcher and runs nothing (C12): %d", picks);
    /* It opens again, rows and all. */
    apps_lv_show_launcher(ids, names, 1, pick);
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && fake_children_of(launcher, "obj") == 1, "opens again");
    fake_tap(fake_parent(fake_label("Hello")));
    CHECK(picks == 1 && strcmp(picked, "hello") == 0, "and picks");
}

static void launcher_empty_state(void) {
    cards_scene();
    apps_lv_show_launcher(NULL, NULL, 0, pick);
    int empty = fake_label("No apps yet. Ask Muse to make one.");
    CHECK(empty > 0, "the empty state\n%s", log_peek());
    int launcher = fake_parent(empty);
    CHECK_CARD(launcher);
    CHECK(fake_children_of(launcher, "obj") == 0, "no rows");
    /* LVGL gives a tap on the text, which isn't clickable, to the launcher under it: that closes it (C12). */
    CHECK(!fake_flag(empty, LV_OBJ_FLAG_CLICKABLE), "the text isn't clickable");
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN), "closed");
}

/* P6, O: until the store is mounted the launcher says "Loading apps..." (APPS_LV_LOADING: three ASCII dots, which the font has), with no
 * rows and not the empty state; shown again with the list, it has the rows and the text is gone. apps_lv_launcher_shown says whether the
 * launcher is up: not before it is shown, not after it closes, and not for a crash card. */
static void launcher_says_loading_then_lists_apps(void) {
    cards_scene();
    CHECK(!apps_lv_launcher_shown(), "nothing is shown yet");
    apps_lv_show_launcher(NULL, NULL, APPS_LV_LOADING, pick);
    int loading = fake_label("Loading apps...");
    CHECK(loading > 0, "the loading text\n%s", log_peek());
    int launcher = fake_parent(loading);
    CHECK_CARD(launcher);
    CHECK(fake_children_of(launcher, "obj") == 0 && fake_label("No apps yet. Ask Muse to make one.") < 0, "no rows, and not the empty state");
    CHECK(!fake_flag(loading, LV_OBJ_FLAG_CLICKABLE), "the text isn't clickable: a tap on it closes the launcher, as the empty state's");
    CHECK(apps_lv_launcher_shown() && apps_lv_cards_shown(), "the launcher is up");
    const char *ids[] = {"hello", "timer"};
    const char *names[] = {"Hello", "Kitchen timer"};
    apps_lv_show_launcher(ids, names, 2, pick);
    CHECK(fake_label("Loading apps...") < 0, "the loading text is gone");
    CHECK(fake_label("Hello") > 0 && fake_label("Kitchen timer") > 0 && fake_children_of(launcher, "obj") == 2, "the list's rows\n%s",
          log_peek());
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_launcher_shown(), "still up");
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_launcher_shown(), "closed: not shown");
    apps_lv_show_crash("Hello", "main.lua:1: boom");
    CHECK(apps_lv_cards_shown() && !apps_lv_launcher_shown(), "a crash card isn't the launcher");
}

static void launcher_closes_on_a_right_swipe(void) {
    cards_scene();
    const char *ids[] = {"hello"};
    const char *names[] = {"Hello"};
    apps_lv_show_launcher(ids, names, 1, pick);
    int row = fake_parent(fake_label("Hello")), launcher = fake_parent(row);
    /* A swipe that starts on a row climbs to the launcher, which keeps it from the screen. */
    fake_press(row);
    fake_swipe(LV_DIR_LEFT);
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN), "a left swipe leaves it open");
    log_clear();
    fake_swipe(LV_DIR_RIGHT);
    CHECK(fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "a right swipe closes it (C12)");
    CHECK(log_has_line("wait_release"), "and the rest of the touch is nobody's\n%s", log_peek());
    fake_lift();
    CHECK(picks == 0, "the row the swipe began on isn't picked: %d", picks);
}

/* F4: only a right swipe does anything to the launcher. A left, up or down swipe that begins on a row, followed
 * through the lift, neither picks the row (LVGL would click it: nothing scrolled) nor closes the launcher. */
static void launcher_ignores_every_other_swipe(void) {
    static const struct {
        lv_dir_t dir;
        const char *name;
    } ways[] = {{LV_DIR_LEFT, "a left"}, {LV_DIR_TOP, "an up"}, {LV_DIR_BOTTOM, "a down"}};
    const char *ids[] = {"hello"};
    const char *names[] = {"Hello"};
    for (size_t i = 0; i < sizeof ways / sizeof ways[0]; i++) {
        cards_scene();
        apps_lv_show_launcher(ids, names, 1, pick);
        int row = fake_parent(fake_label("Hello")), launcher = fake_parent(row);
        fake_press(row);
        fake_swipe(ways[i].dir);
        fake_lift();
        CHECK(picks == 0, "%s swipe on a row picks it: %d", ways[i].name, picks);
        CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(), "%s swipe closes the launcher", ways[i].name);
    }
    cards_scene();
    apps_lv_show_launcher(ids, names, 1, pick);
    int row = fake_parent(fake_label("Hello")), launcher = fake_parent(row);
    fake_press(row);
    fake_swipe(LV_DIR_RIGHT);
    fake_lift();
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "a right swipe closes it, picking nothing");
}

static void launcher_rows_are_its_own(void) {
    cards_scene();
    char first[] = "alpha";
    const char *ids[] = {first, "bravo", "charlie", "delta", "echo", "foxtrot"};
    const char *names[] = {"Alpha", "Bravo", "Charlie", "Delta", "Echo", NULL};
    apps_lv_show_launcher(ids, names, 6, pick);
    int launcher = fake_parent(fake_parent(fake_label("Alpha")));
    CHECK(fake_children_of(launcher, "obj") == APPS_LV_LAUNCHER_ROWS, "at most %d rows: %d", APPS_LV_LAUNCHER_ROWS,
          fake_children_of(launcher, "obj"));
    CHECK(fake_label("Echo") < 0, "the fifth isn't shown");
    memcpy(first, "zulu!", sizeof first);  /* the caller's text changes: the launcher kept a copy */
    fake_tap(fake_parent(fake_label("Alpha")));
    CHECK(picks == 1 && strcmp(picked, "alpha") == 0, "the copy is picked: '%s'", picked);
    /* Shown again, it has the new rows only: the old ones are gone, and no call is made on them. */
    const char *one_id[] = {"solo"};
    const char *one_name[] = {NULL};  /* no name: the id stands in */
    apps_lv_show_launcher(one_id, one_name, 1, pick);
    CHECK(fake_children_of(launcher, "obj") == 1 && fake_label("solo") > 0 && fake_label("Alpha") < 0, "rebuilt\n%s", log_peek());
    fake_tap(fake_parent(fake_label("solo")));
    CHECK(picks == 2 && strcmp(picked, "solo") == 0, "picked: '%s'", picked);
}

static void crash_card_text_and_tap(void) {
    cards_scene();
    apps_lv_show_crash("Timer", "main.lua:3: boom");
    int title = fake_label("Timer stopped"), body = fake_label("main.lua:3: boom"), hint = fake_label("Tap to close");
    CHECK(title > 0 && body > 0 && hint > 0, "the card's text: %d %d %d\n%s", title, body, hint, log_peek());
    int card = fake_parent(title);
    CHECK_CARD(card);
    CHECK(fake_parent(body) == card && fake_parent(hint) == card && !fake_flag(body, LV_OBJ_FLAG_HIDDEN), "one card");
    CHECK(apps_lv_cards_shown() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN), "shown");
    fake_tap(card);
    CHECK(fake_flag(card, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "a tap closes it");
    /* The supervisor's card: a hung app has no error to show. */
    apps_lv_show_crash("Timer", NULL);
    CHECK(fake_label("Timer stopped responding") == title && fake_flag(body, LV_OBJ_FLAG_HIDDEN) && !fake_flag(card, LV_OBJ_FLAG_HIDDEN),
          "stopped responding\n%s", log_peek());
    apps_lv_show_crash(NULL, "x");
    CHECK(fake_label("The app stopped") == title && fake_label("x") == body && !fake_flag(body, LV_OBJ_FLAG_HIDDEN), "no name");
    /* The launcher, made with the card, sits under it. */
    const char *ids[] = {"hello"};
    const char *names[] = {"Hello"};
    apps_lv_show_launcher(ids, names, 1, pick);
    int launcher = fake_parent(fake_parent(fake_label("Hello")));
    CHECK(fake_index(launcher) < fake_index(card), "a crash card over the launcher: %d %d", fake_index(launcher), fake_index(card));
    apps_lv_hide_cards();
    CHECK(fake_flag(card, LV_OBJ_FLAG_HIDDEN) && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "both closed");
    CHECK(fake_alive_of("obj") == 3 + 1, "the layer, two cards and a row: %d", fake_alive_of("obj"));
}

/* The reset takes down the app's objects; the cards aren't on its layer, so a card shown before it stays. */
static void cards_survive_a_reset(void) {
    cards_scene();
    apps_lv_show_crash("Timer", "main.lua:3: boom");
    int card = fake_parent(fake_label("Timer stopped"));
    int before = fake_alive();
    apps_lv_reset();
    CHECK(fake_alive() == before && !fake_flag(card, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(), "the card is still up: %d of %d",
          fake_alive(), before);
}

/* No layer, no cards: nothing is made, and nothing breaks. */
static void cards_need_a_layer(void) {
    fake_reset();
    long before = lvgl_calls;
    const char *ids[] = {"hello"};
    const char *names[] = {"Hello"};
    apps_lv_show_launcher(ids, names, 1, pick);
    apps_lv_show_crash("Timer", "boom");
    apps_lv_hide_cards();
    CHECK(!apps_lv_cards_shown() && lvgl_calls == before, "no LVGL call: %ld\n%s", lvgl_calls - before, log_peek());
}

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
        {"a_create_that_fails_is_tried_again", a_create_that_fails_is_tried_again},
        {"skips_mid_callback_at_most_three_frames", skips_mid_callback_at_most_three_frames},
        {"lock_discipline", lock_discipline},
        {"no_layer_no_pass", no_layer_no_pass},
        {"a_cleared_table_can_be_freed", a_cleared_table_can_be_freed},
        {"the_table_is_read_under_the_lock", the_table_is_read_under_the_lock},
        {"the_app_can_end_while_the_records_are_made", the_app_can_end_while_the_records_are_made},
        {"the_lock_may_be_refused_after_the_records_are_made", the_lock_may_be_refused_after_the_records_are_made},
        {"no_table_is_no_pass", no_table_is_no_pass},
        {"a_callback_frame_is_skipped_before_the_table_is_read", a_callback_frame_is_skipped_before_the_table_is_read},
        {"the_published_table_is_the_one_applied", the_published_table_is_the_one_applied},
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
        {"a_swipe_that_starts_on_a_widget_ends_its_press", a_swipe_that_starts_on_a_widget_ends_its_press},
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
        {"a8_images_are_white", a8_images_are_white},
        {"scaled_images_have_the_scaled_box", scaled_images_have_the_scaled_box},
        {"image_flags_are_masked", image_flags_are_masked},
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
        {"launcher_lists_apps_and_picks", launcher_lists_apps_and_picks},
        {"launcher_closes_on_a_tap_elsewhere", launcher_closes_on_a_tap_elsewhere},
        {"launcher_empty_state", launcher_empty_state},
        {"launcher_says_loading_then_lists_apps", launcher_says_loading_then_lists_apps},
        {"launcher_closes_on_a_right_swipe", launcher_closes_on_a_right_swipe},
        {"launcher_ignores_every_other_swipe", launcher_ignores_every_other_swipe},
        {"launcher_rows_are_its_own", launcher_rows_are_its_own},
        {"crash_card_text_and_tap", crash_card_text_and_tap},
        {"cards_survive_a_reset", cards_survive_a_reset},
        {"cards_need_a_layer", cards_need_a_layer},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(held == 0, "%s ended with %zu bytes held", argv[1], held);
            CHECK(fake_errors == 0, "%s ended with %d fake LVGL errors, the first: %s", argv[1], fake_errors, fake_note);
            CHECK(lock_errors == 0 && !lock_held, "%s ended with %d lock errors, the lock held %d, the first: %s", argv[1], lock_errors,
                  lock_held, lock_note);
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
