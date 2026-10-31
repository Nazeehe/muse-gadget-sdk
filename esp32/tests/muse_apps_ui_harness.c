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
 * Checks apps_ui, the ui module of Muse apps and its shadow widget table, through the C interface. Usage:
 * muse_apps_ui_harness <case>, one case per test of HarnessTest in tests/test_muse_apps_ui.py. Exits 0 when the
 * case holds and prints what it got when it doesn't. What a script of apphost can't reach is here: the dirty
 * bits and the slots the LVGL task's apply step sees, the lock, a heap that refuses at every step, and the
 * dump's cuts.
 *
 * Each case makes a VM and a table of its own (env_t) and runs Lua chunks in it. The apply step is played by
 * apply(): what Task 11's pass does to the bits and the flags, with no LVGL. The port below is the harness's own:
 * a counting allocator that can refuse every growth request from the Nth on (the leak check, since
 * LeakSanitizer doesn't run on macOS: every case ends at 0 bytes), and a UI lock that records how it is used.
 * After every chunk the harness checks the lock is free, that the heap was never used or given back to while it
 * was held (the heap is Lua's too, so a Lua call under the lock lands there), and the table's invariants
 * (apps_ui.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_json.h"
#include "apps_port.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

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

#define NEW_BITS (APPS_DIRTY_CREATE | APPS_DIRTY_GEOM | APPS_DIRTY_STYLE)

/* ---- The port ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;            /* bytes handed out and not freed */
static long growths;           /* growth requests so far */
static long refuse_from = -1;  /* refuse every growth request from this one on (counted in growths); -1: none */

static bool lock_held;
static long lock_takes;  /* times the lock was taken: a change that never takes it is a race on the device */
static int lock_errors;  /* a lock taken while held, a lock given back that wasn't held, the heap used or freed under it */
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
}

bool apps_port_ui_trylock(int ms) {
    (void)ms;
    apps_port_ui_lock();
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

/* ---- An environment: a VM with `ui` in it, and its table ---- */

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

/* The apply step, without LVGL: what a pass does to the table's bits and flags, and what it saw. */
typedef struct {
    bool cleared;
    bool bg_changed;
    uint32_t bg;
    uint32_t dirty[APPS_UI_SLOTS];
} pass_t;

static void apply(env_t *e, pass_t *seen) {
    pass_t scratch;
    if (seen == NULL) {
        seen = &scratch;
    }
    apps_port_ui_lock();
    seen->cleared = apps_ui_take_clear(e->ui);
    seen->bg = apps_ui_take_bg(e->ui, &seen->bg_changed);
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        apps_widget_t *w = apps_ui_slot(e->ui, i);
        seen->dirty[i] = w->dirty;
        w->dirty = 0;
    }
    apps_port_ui_unlock();
}

/* ---- What must hold after every chunk ---- */

static bool in_range(int64_t v, int64_t lo, int64_t hi) {
    return v >= lo && v <= hi;
}

/* The table's invariants (apps_ui.h), for every slot. */
static void check_invariants(int line, env_t *e) {
    int alive = 0;
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = apps_ui_slot(e->ui, i);
        CHECK_AT(line, (w->dirty & ~127u) == 0, "slot %d: dirty %u", i, w->dirty);
        if (!w->alive) {
            CHECK_AT(line, w->text == NULL && w->events == 0, "slot %d: a dead widget keeps text or events", i);
            continue;
        }
        alive++;
        CHECK_AT(line, w->kind <= APPS_W_IMAGE, "slot %d: kind %d", i, w->kind);
        CHECK_AT(line, (w->dirty & APPS_DIRTY_DELETE) == 0, "slot %d: alive and flagged DELETE", i);
        int depth = 0;
        for (int p = w->parent; p >= 0 && depth <= 5; p = apps_ui_slot(e->ui, p)->parent) {
            const apps_widget_t *pw = apps_ui_slot(e->ui, p);
            CHECK_AT(line, pw->alive && pw->kind == APPS_W_BOX, "slot %d: its parent %d is not an alive box", i, p);
            if (!pw->alive || pw->kind != APPS_W_BOX) {
                break;
            }
            depth++;
        }
        CHECK_AT(line, depth <= APPS_UI_BOX_DEPTH_MAX, "slot %d: %d box ancestors", i, depth);
        CHECK_AT(line, w->kind != APPS_W_BOX || depth < APPS_UI_BOX_DEPTH_MAX,
                 "slot %d: a box with %d box ancestors is the fifth box in a chain", i, depth);
        CHECK_AT(line, in_range(w->x, -466, 466) && in_range(w->y, -466, 466), "slot %d: x %d y %d", i, w->x, w->y);
        CHECK_AT(line, (w->w == -1 || in_range(w->w, 1, 466)) && (w->h == -1 || in_range(w->h, 1, 466)),
                 "slot %d: w %d h %d", i, w->w, w->h);
        CHECK_AT(line, w->opacity <= 100 && w->align <= APPS_ALIGN_BOTTOM_RIGHT && w->font <= APPS_FONT_DIGITS96 &&
                           w->text_align <= 2,
                 "slot %d: opacity %d align %d font %d text_align %d", i, w->opacity, w->align, w->font,
                 w->text_align);
        CHECK_AT(line, in_range(w->radius, -1, 233) && w->border <= 20 && in_range(w->thickness, 1, 64) &&
                           in_range(w->scale, 10, 400),
                 "slot %d: radius %d border %d thickness %d scale %d", i, w->radius, w->border, w->thickness,
                 w->scale);
        CHECK_AT(line, in_range(w->rotation, 0, 359) && w->spin <= APPS_SPIN_RPM_MAX, "slot %d: rotation %d spin %d", i,
                 w->rotation, w->spin);
        CHECK_AT(line, in_range(w->start_angle, 0, 360) && in_range(w->end_angle, 0, 360), "slot %d: angles %d %d", i,
                 w->start_angle, w->end_angle);
        CHECK_AT(line, w->color <= 0xFFFFFF && w->border_color <= 0xFFFFFF && w->track_color <= 0xFFFFFF &&
                           (w->bg <= 0xFFFFFF || w->bg == APPS_COLOR_NONE),
                 "slot %d: colours", i);
        CHECK_AT(line, w->min < w->max && w->min >= -APPS_UI_VALUE_MAX && w->max <= APPS_UI_VALUE_MAX &&
                           w->value >= w->min && w->value <= w->max,
                 "slot %d: min %d max %d value %d", i, w->min, w->max, w->value);
        CHECK_AT(line, w->events <= (APPS_UI_TAP | APPS_UI_LONG_PRESS | APPS_UI_PRESS | APPS_UI_RELEASE),
                 "slot %d: events %d", i, w->events);
        if (w->kind == APPS_W_IMAGE) {
            CHECK_AT(line, apps_ui_asset(e->ui, w->image) != NULL, "slot %d: image %d", i, w->image);
        }
        if (w->text != NULL) {
            size_t len = strlen(w->text);
            bool valid = len > 0 && len <= APPS_UI_TEXT_MAX;  /* NULL is "": a block is never empty */
            const unsigned char *p = (const unsigned char *)w->text, *end = p + len;
            while (valid && p < end) {
                if (*p < 0x80) {
                    p++;
                } else {
                    int n = apps_json_utf8_len(p, end);
                    valid = n > 0;
                    p += n > 0 ? n : 0;
                }
            }
            CHECK_AT(line, valid, "slot %d: text '%s'", i, w->text);
        }
    }
    CHECK_AT(line, alive == apps_ui_count(e->ui), "%d alive, apps_ui_count says %d", alive, apps_ui_count(e->ui));
}

static void after(int line, env_t *e) {
    CHECK_AT(line, !lock_held && lock_errors == 0, "the lock: held %d, %d errors, the first: %s", lock_held,
             lock_errors, lock_note);
    check_invariants(line, e);
}

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

/* The chunk raises an error with `want` in its message. Nothing but that may have changed: the caller checks. */
static void run_fails_at(int line, env_t *e, const char *src, const char *want) {
    char err[1024] = "";
    bool ok = run(e, src, err, sizeof err);
    CHECK_AT(line, !ok, "'%s' ran, and was to fail with '%s'", src, want);
    CHECK_AT(line, strstr(err, want) != NULL, "'%s' failed with '%s', not '%s'", src, err, want);
    after(line, e);
}
#define RUN_FAILS(e, src, want) run_fails_at(__LINE__, (e), (src), (want))

/* The same, and err's first line is exactly want (the message, with the position of the chunk's line). */
static void run_fails_exactly_at(int line, env_t *e, const char *src, const char *want) {
    char err[1024] = "";
    bool ok = run(e, src, err, sizeof err);
    char *newline = strchr(err, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    CHECK_AT(line, !ok && strcmp(err, want) == 0, "'%s' gave %d, '%s', want '%s'", src, ok, err, want);
    after(line, e);
}
#define RUN_FAILS_EXACTLY(e, src, want) run_fails_exactly_at(__LINE__, (e), (src), (want))

#define OPEN(e)                                  \
    env_t e;                                     \
    CHECK(env_open(&e, NULL, 0), "env_open")

static void finish(env_t *e) {
    env_close(e);
    CHECK(held == 0, "%zu bytes still held", held);
}

/* ---- Dirty bits: what each call tells the apply step ---- */

static const apps_asset_t TWO_IMAGES[] = {{"a.bin", (const uint8_t *)"aa", 2, 1, 1}, {"b.bin", (const uint8_t *)"bb", 2, 2, 2}};

static void dirty_bits(void) {
    env_t e;
    CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");

    /* A label: CREATE with the groups it needs. */
    RUN(&e, "a = ui.label{text = 'hi'}");
    apps_widget_t *w = W(&e, 0);
    CHECK(w->alive && w->kind == APPS_W_LABEL && w->gen == 0 && w->parent == -1 && w->events == 0 && w->rw == 0,
          "label: alive %d kind %d gen %d parent %d", w->alive, w->kind, w->gen, w->parent);
    CHECK(w->dirty == (NEW_BITS | APPS_DIRTY_TEXT), "label: dirty %u", w->dirty);
    CHECK(w->text != NULL && strcmp(w->text, "hi") == 0, "label text");
    CHECK(apps_ui_count(e.ui) == 1, "count %d", apps_ui_count(e.ui));
    pass_t seen;
    apply(&e, &seen);
    CHECK(w->dirty == 0 && seen.dirty[0] == (NEW_BITS | APPS_DIRTY_TEXT) && !seen.cleared, "the pass saw %u", seen.dirty[0]);

    /* Each group of properties sets its own bit, and a set to what is there sets none. */
    static const struct {
        const char *set;
        uint32_t want;
    } sets[] = {
        {"a:set{x = 5}", APPS_DIRTY_GEOM},
        {"a:set{x = 5}", 0},
        {"a:set{x = 5.2}", 0},  /* rounds to what is there */
        {"a:set{y = -9, align = 'top'}", APPS_DIRTY_GEOM},
        {"a:set{w = 100, h = 40}", APPS_DIRTY_GEOM},
        {"a:set{align = 'top'}", 0},
        {"a:set{text = 'yo'}", APPS_DIRTY_TEXT},
        {"a:set{text = 'yo'}", 0},
        {"a:set{text = ''}", APPS_DIRTY_TEXT},
        {"a:set{text = ''}", 0},
        {"a:set{text = 'yo'}", APPS_DIRTY_TEXT},
        {"a:set{color = '#ff0000'}", APPS_DIRTY_STYLE},
        {"a:set{color = 0xFF0000}", 0},
        {"a:set{color = '#f00'}", 0},
        {"a:set{font = 'mono16', text_align = 'left', wrap = true}", APPS_DIRTY_STYLE},
        {"a:set{hidden = true}", APPS_DIRTY_STYLE},
        {"a:set{opacity = 40}", APPS_DIRTY_STYLE},
        {"a:set{opacity = 40, hidden = true, wrap = true}", 0},
        {"a:set{x = 6, opacity = 41}", APPS_DIRTY_GEOM | APPS_DIRTY_STYLE},
        {"a:set{text = 'z', x = 7, font = 'sans16'}", APPS_DIRTY_TEXT | APPS_DIRTY_GEOM | APPS_DIRTY_STYLE},
        {"a:set{}", 0},
    };
    for (size_t i = 0; i < ARRAY_LEN(sets); i++) {
        RUN(&e, sets[i].set);
        CHECK(w->dirty == sets[i].want, "%s: dirty %u, want %u", sets[i].set, w->dirty, sets[i].want);
        apply(&e, NULL);
    }
    /* The same text is not copied again. */
    const char *block = w->text;
    RUN(&e, "a:set{text = 'z'}");
    CHECK(w->text == block && w->dirty == 0, "the same text allocated again");

    /* Events: the mask is what the apply step reads. */
    RUN(&e, "a:on('tap', function() end)");
    CHECK(w->events == APPS_UI_TAP && w->dirty == APPS_DIRTY_EVENTS, "tap: events %d dirty %u", w->events, w->dirty);
    apply(&e, NULL);
    RUN(&e, "a:on('click', function() end)");  /* the same event, a new handler */
    CHECK(w->events == APPS_UI_TAP && w->dirty == 0, "click: events %d dirty %u", w->events, w->dirty);
    RUN(&e, "a:on('release', function() end)");
    CHECK(w->events == (APPS_UI_TAP | APPS_UI_RELEASE) && w->dirty == APPS_DIRTY_EVENTS, "release: %d %u", w->events,
          w->dirty);
    apply(&e, NULL);
    RUN(&e, "a:on('long_press', function() end) a:on('press', function() end)");
    CHECK(w->events == 15, "all four: %d", w->events);
    apply(&e, NULL);
    RUN(&e, "a:on('tap', nil)");
    CHECK(w->events == 14 && w->dirty == APPS_DIRTY_EVENTS, "tap off: %d %u", w->events, w->dirty);
    apply(&e, NULL);
    RUN(&e, "a:on('tap', nil)");
    CHECK(w->events == 14 && w->dirty == 0, "tap off again: %d %u", w->events, w->dirty);

    /* The other kinds, and what a new one asks to have applied. */
    RUN(&e, "b = ui.box{}");
    CHECK(W(&e, 1)->kind == APPS_W_BOX && W(&e, 1)->dirty == NEW_BITS && W(&e, 1)->bg == APPS_COLOR_NONE &&
              W(&e, 1)->radius == 0,
          "box: dirty %u bg %x", W(&e, 1)->dirty, W(&e, 1)->bg);
    RUN(&e, "c = ui.button{text = 'Go'}");
    w = W(&e, 2);
    CHECK(w->kind == APPS_W_BUTTON && w->dirty == (NEW_BITS | APPS_DIRTY_TEXT) && w->bg == 0x262626 && w->radius == 16 &&
              w->font == APPS_FONT_SANS20 && w->color == 0xFFFFFF,
          "button: dirty %u bg %x radius %d", w->dirty, w->bg, w->radius);
    RUN(&e, "d = ui.arc{value = 30}");
    w = W(&e, 3);
    CHECK(w->kind == APPS_W_ARC && w->dirty == (NEW_BITS | APPS_DIRTY_VALUE) && w->min == 0 && w->max == 100 &&
              w->value == 30 && w->start_angle == 0 && w->end_angle == 360 && w->thickness == 12 &&
              w->track_color == 0x262626,
          "arc: dirty %u value %d", w->dirty, w->value);
    RUN(&e, "f = ui.bar{}");
    w = W(&e, 4);
    CHECK(w->kind == APPS_W_BAR && w->dirty == (NEW_BITS | APPS_DIRTY_VALUE) && w->min == 0 && w->max == 100 &&
              w->value == 0 && w->track_color == 0x262626 && w->radius == 0,
          "bar: dirty %u", w->dirty);
    RUN(&e, "g = ui.image{src = 'b.bin'}");
    w = W(&e, 5);
    CHECK(w->kind == APPS_W_IMAGE && w->dirty == NEW_BITS && w->image == 1 && w->scale == 100, "image: dirty %u image %d",
          w->dirty, w->image);
    apply(&e, NULL);

    /* An arc's value follows min and max, and a set that would cross them changes nothing. */
    RUN(&e, "d:set{value = 150}");
    CHECK(W(&e, 3)->value == 100 && W(&e, 3)->dirty == APPS_DIRTY_VALUE, "value clamped: %d", W(&e, 3)->value);
    apply(&e, NULL);
    RUN(&e, "d:set{min = 10, max = 20}");
    CHECK(W(&e, 3)->value == 20 && W(&e, 3)->min == 10 && W(&e, 3)->max == 20 && W(&e, 3)->dirty == APPS_DIRTY_VALUE,
          "min and max: value %d", W(&e, 3)->value);
    apply(&e, NULL);
    RUN(&e, "d:set{max = 12}");
    CHECK(W(&e, 3)->value == 12 && W(&e, 3)->max == 12, "value follows max: %d", W(&e, 3)->value);
    apply(&e, NULL);
    RUN(&e, "d:set{thickness = 5}");
    CHECK(W(&e, 3)->dirty == APPS_DIRTY_STYLE, "thickness: %u", W(&e, 3)->dirty);
    RUN(&e, "d:set{start_angle = 90, end_angle = 270}");
    CHECK(W(&e, 3)->dirty == (APPS_DIRTY_STYLE | APPS_DIRTY_VALUE), "angles: %u", W(&e, 3)->dirty);
    apply(&e, NULL);
    RUN(&e, "g:set{src = 'a.bin', scale = 50}");
    CHECK(W(&e, 5)->image == 0 && W(&e, 5)->scale == 50 && W(&e, 5)->dirty == APPS_DIRTY_STYLE, "src: image %d dirty %u",
          W(&e, 5)->image, W(&e, 5)->dirty);
    apply(&e, NULL);

    /* A set that fails changes nothing: not the fields before the bad one, and not the bits. */
    apps_widget_t before;
    memcpy(&before, W(&e, 0), sizeof before);
    RUN_FAILS(&e, "a:set{x = 99, text = 'changed', color = 'bad'}", "color 'bad' is not a colour");
    CHECK(memcmp(&before, W(&e, 0), sizeof before) == 0, "a failed set changed the label");
    memcpy(&before, W(&e, 3), sizeof before);
    RUN_FAILS(&e, "d:set{value = 11, min = 12}", "min must be less than max");
    RUN_FAILS(&e, "d:set{value = 11, bogus = 1}", "unknown property 'bogus'");
    CHECK(memcmp(&before, W(&e, 3), sizeof before) == 0, "a failed set changed the arc");

    /* Delete: after the apply step has made it, the object has to go; before, there is nothing to delete. */
    char *text = W(&e, 0)->text;
    CHECK(text != NULL, "text");
    W(&e, 0)->rx = 7;  /* what the apply step wrote back: a new widget in the slot must not inherit it */
    W(&e, 0)->ry = 8;
    W(&e, 0)->rw = 50;
    W(&e, 0)->rh = 20;
    W(&e, 0)->overflow = 3;
    RUN(&e, "a:delete()");
    w = W(&e, 0);
    CHECK(!w->alive && w->gen == 1 && w->dirty == APPS_DIRTY_DELETE && w->text == NULL && w->events == 0 &&
              apps_ui_count(e.ui) == 5,
          "deleted: alive %d gen %d dirty %u count %d", w->alive, w->gen, w->dirty, apps_ui_count(e.ui));
    RUN(&e, "h = ui.label{}");  /* the slot is not free until the apply step has deleted the object */
    CHECK(W(&e, 6)->alive && W(&e, 6)->kind == APPS_W_LABEL, "the new label went to slot 6");
    apply(&e, &seen);
    CHECK(seen.dirty[0] == APPS_DIRTY_DELETE, "the pass saw %u", seen.dirty[0]);
    RUN(&e, "i = ui.label{}");
    CHECK(W(&e, 0)->alive && W(&e, 0)->gen == 1 && W(&e, 0)->dirty == (NEW_BITS | APPS_DIRTY_TEXT),
          "the new label took slot 0 on gen 1");
    CHECK(W(&e, 0)->rx == 0 && W(&e, 0)->ry == 0 && W(&e, 0)->rw == 0 && W(&e, 0)->rh == 0 &&
              W(&e, 0)->overflow == 0 && W(&e, 0)->events == 0 && W(&e, 0)->text == NULL,
          "the new label has the old one's layout: rw %d overflow %d", W(&e, 0)->rw, W(&e, 0)->overflow);
    RUN(&e, "j = ui.label{} j:delete()");  /* made and deleted before a pass: nothing was ever made for it */
    CHECK(!W(&e, 7)->alive && W(&e, 7)->dirty == 0 && W(&e, 7)->gen == 1, "made and deleted: dirty %u", W(&e, 7)->dirty);
    RUN(&e, "k = ui.label{}");
    CHECK(W(&e, 7)->alive && W(&e, 7)->gen == 1, "slot 7 is free at once");
    finish(&e);
}

/* ---- Dirty bits: every property in its group ---- */

/* A widget of each kind and, for each property it has, a set to a value other than the one it holds: the bit that
 * raises is the property's group (apps_ui.h), and the same set again raises none. */
static void dirty_groups(void) {
    enum { G = APPS_DIRTY_GEOM, S = APPS_DIRTY_STYLE, T = APPS_DIRTY_TEXT, V = APPS_DIRTY_VALUE };
    static const struct {
        const char *make;      /* an expression that makes the widget, which gets slot 0 */
        const char *props;     /* inside w:set{...} */
        uint32_t want;
    } cases[] = {
        {"ui.label{}", "align = 'top'", G},          {"ui.label{}", "x = 3", G},
        {"ui.label{}", "y = 3", G},                  {"ui.label{}", "w = 30", G},
        {"ui.label{}", "h = 30", G},                 {"ui.label{}", "hidden = true", S},
        {"ui.label{}", "opacity = 30", S},           {"ui.label{}", "text = 't'", T},
        {"ui.label{}", "font = 'mono8'", S},         {"ui.label{}", "color = '#010203'", S},
        {"ui.label{}", "text_align = 'right'", S},   {"ui.label{}", "wrap = true", S},
        {"ui.button{}", "text = 't'", T},            {"ui.button{}", "bg = '#010203'", S},
        {"ui.button{}", "radius = 3", S},            {"ui.button{}", "radius = 'circle'", S},
        {"ui.button{}", "border = 3", S},            {"ui.button{}", "border_color = '#010203'", S},
        {"ui.button{}", "font = 'sans14'", S},       {"ui.button{}", "color = '#010203'", S},
        {"ui.box{}", "bg = '#010203'", S},           {"ui.box{}", "radius = 3", S},
        {"ui.box{}", "border = 3", S},               {"ui.box{}", "border_color = '#010203'", S},
        {"ui.box{}", "align = 'left', w = 9", G},    {"ui.box{}", "hidden = true, opacity = 9", S},
        {"ui.arc{}", "min = -5", V},                 {"ui.arc{}", "max = 500", V},
        {"ui.arc{}", "value = 9", V},                {"ui.arc{}", "start_angle = 9", V},
        {"ui.arc{}", "end_angle = 9", V},            {"ui.arc{}", "thickness = 9", S},
        {"ui.arc{}", "color = '#010203'", S},        {"ui.arc{}", "track_color = '#010203'", S},
        {"ui.arc{}", "x = 9, y = 9", G},             {"ui.bar{}", "min = -5", V},
        {"ui.bar{}", "max = 500", V},                {"ui.bar{}", "value = 9", V},
        {"ui.bar{}", "color = '#010203'", S},        {"ui.bar{}", "track_color = '#010203'", S},
        {"ui.bar{}", "radius = 3", S},               {"ui.bar{}", "radius = 'circle'", S},
        {"ui.bar{}", "w = 9, h = 9", G},             {"ui.image{src = 'a.bin'}", "src = 'b.bin'", S},
        {"ui.image{src = 'a.bin'}", "scale = 50", S}, {"ui.image{src = 'a.bin'}", "x = 9", G},
        {"ui.image{src = 'a.bin'}", "hidden = true", S}, {"ui.image{src = 'a.bin'}", "rotation = 90", S},
        {"ui.image{src = 'a.bin'}", "spin = 30", S},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        env_t e;
        char src[256];
        CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");
        snprintf(src, sizeof src, "w = %s", cases[i].make);
        RUN(&e, src);
        apply(&e, NULL);
        snprintf(src, sizeof src, "w:set{%s}", cases[i].props);
        RUN(&e, src);
        CHECK(W(&e, 0)->dirty == cases[i].want, "%s, %s: dirty %u, want %u", cases[i].make, cases[i].props,
              W(&e, 0)->dirty, cases[i].want);
        apply(&e, NULL);
        RUN(&e, src);
        CHECK(W(&e, 0)->dirty == 0, "%s, %s: the same set again raised %u", cases[i].make, cases[i].props,
              W(&e, 0)->dirty);
        finish(&e);
    }
}

/* The spin cap fails closed (connected apps Task 1, fix round 1, I2): a picture the table can't size, one whose asset has no size
 * or a src that is no asset of the package (Task 2's runtime images, until P9 sizes them), may not spin. The call that would leave
 * it spinning is refused and changes nothing; spin = 0, and the picture still, are fine. */
static void spin_refuses_an_unsized_src(void) {
    static const apps_asset_t assets[] = {{"a.bin", (const uint8_t *)"aa", 2, 1, 1}, {"u.bin", (const uint8_t *)"uu", 2, 0, 0}};
    static const char unknown[] = "t.lua:1: ui.image: spin: this image's size is unknown";
    env_t e;
    CHECK(env_open(&e, assets, 2), "env_open");
    RUN_FAILS_EXACTLY(&e, "ui.image{src = 'u.bin', spin = 10}", unknown);
    CHECK(apps_ui_count(e.ui) == 0, "a refused create made %d widgets", apps_ui_count(e.ui));
    RUN(&e, "u = ui.image{src = 'u.bin', spin = 0} a = ui.image{src = 'a.bin'} b = ui.image{src = 'a.bin', spin = 10}");
    RUN_FAILS_EXACTLY(&e, "u:set{spin = 10}", unknown);
    CHECK(W(&e, 0)->spin == 0, "u: spin %d", W(&e, 0)->spin);
    RUN_FAILS_EXACTLY(&e, "b:set{src = 'u.bin'}", unknown);
    CHECK(W(&e, 2)->image == 0 && W(&e, 2)->spin == 10, "b: image %d spin %d", W(&e, 2)->image, W(&e, 2)->spin);

    /* A src that is no asset: the table's index past the package's images. The invariants don't allow it, so the chunk runs
     * without the after-check, and the index is put back before anything else. */
    W(&e, 1)->image = APPS_UI_ASSETS_MAX + 1;
    char err[512] = "";
    bool ran = run(&e, "a:set{spin = 10}", err, sizeof err);
    char *newline = strchr(err, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    CHECK(!ran && strcmp(err, unknown) == 0, "a src that is no asset: ran %d, '%s'", ran, err);
    CHECK(W(&e, 1)->spin == 0, "a: spin %d", W(&e, 1)->spin);
    ran = run(&e, "a:set{spin = 0, rotation = 90}", err, sizeof err);
    CHECK(ran && W(&e, 1)->rotation == 90, "a, still: ran %d, '%s'", ran, err);
    W(&e, 1)->image = 0;
    RUN(&e, "a:set{spin = 10}");
    finish(&e);
}

/* rotation and spin (connected apps Task 1). A set of rotation turns the image to it, so the apply step must hear one even of
 * the value the slot holds once the image has spun since rotation was last set (a spin turns it away): rotation_sets moves on and
 * STYLE is set. Otherwise the same set again sets no bit, as for every property. A create with rotation counts one. */
static void rotation_sets(void) {
    env_t e;
    CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");
    RUN(&e, "a = ui.image{src = 'a.bin'} b = ui.image{src = 'a.bin', rotation = 0} c = ui.image{src = 'a.bin', spin = 33}");
    CHECK(W(&e, 0)->rotation_sets == 0 && W(&e, 1)->rotation_sets == 1 && W(&e, 2)->rotation_sets == 0,
          "made: %u %u %u", W(&e, 0)->rotation_sets, W(&e, 1)->rotation_sets, W(&e, 2)->rotation_sets);
    CHECK(W(&e, 2)->spin == 33 && W(&e, 2)->rotation == 0, "c: spin %d rotation %d", W(&e, 2)->spin, W(&e, 2)->rotation);
    apply(&e, NULL);

    /* Never spun: rotation is where the image is, so the same value is nothing, and another one a turn. */
    RUN(&e, "a:set{rotation = 0}");
    CHECK(W(&e, 0)->dirty == 0 && W(&e, 0)->rotation_sets == 0, "a, the same: dirty %u sets %u", W(&e, 0)->dirty,
          W(&e, 0)->rotation_sets);
    RUN(&e, "a:set{rotation = 30}");
    CHECK(W(&e, 0)->dirty == APPS_DIRTY_STYLE && W(&e, 0)->rotation_sets == 1 && W(&e, 0)->rotation == 30, "a, 30: dirty %u sets %u",
          W(&e, 0)->dirty, W(&e, 0)->rotation_sets);
    apply(&e, NULL);

    /* Spinning: every set of rotation counts, the value it holds too. */
    RUN(&e, "c:set{rotation = 0}");
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 1, "c, the same while it spins: dirty %u sets %u",
          W(&e, 2)->dirty, W(&e, 2)->rotation_sets);
    apply(&e, NULL);
    RUN(&e, "c:set{rotation = 0}");
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 2, "c, again: dirty %u sets %u", W(&e, 2)->dirty,
          W(&e, 2)->rotation_sets);
    apply(&e, NULL);
    /* Stopped: it stays where the spin got to, so the next set of rotation still counts; after that, it is at rotation. */
    RUN(&e, "c:set{spin = 0}");
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 2 && W(&e, 2)->spin == 0, "c stops: dirty %u",
          W(&e, 2)->dirty);
    apply(&e, NULL);
    RUN(&e, "c:set{rotation = 0}");
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 3, "c, stopped after a spin: dirty %u sets %u",
          W(&e, 2)->dirty, W(&e, 2)->rotation_sets);
    apply(&e, NULL);
    RUN(&e, "c:set{rotation = 0}");
    CHECK(W(&e, 2)->dirty == 0 && W(&e, 2)->rotation_sets == 3, "c, at rest at rotation: dirty %u sets %u", W(&e, 2)->dirty,
          W(&e, 2)->rotation_sets);
    /* One set with both, the image at rest at rotation: the spin goes on from there, no turn first; and it has spun again. */
    RUN(&e, "c:set{rotation = 0, spin = 45}");
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 3 && W(&e, 2)->spin == 45 && W(&e, 2)->spun,
          "c, both: dirty %u sets %u", W(&e, 2)->dirty, W(&e, 2)->rotation_sets);
    apply(&e, NULL);
    RUN(&e, "c:set{spin = 45}");
    CHECK(W(&e, 2)->dirty == 0, "c, the same spin: dirty %u", W(&e, 2)->dirty);
    RUN(&e, "c:set{rotation = 0, spin = 20}");  /* spinning: the turn counts, and the new rate goes on from it */
    CHECK(W(&e, 2)->dirty == APPS_DIRTY_STYLE && W(&e, 2)->rotation_sets == 4 && W(&e, 2)->spin == 20, "c, both while it spins: %u",
          W(&e, 2)->rotation_sets);
    apply(&e, NULL);
    /* A set that fails counts nothing. */
    RUN_FAILS(&e, "c:set{rotation = 10, spin = 'fast'}", "'spin' must be a number of turns a minute");
    CHECK(W(&e, 2)->dirty == 0 && W(&e, 2)->rotation_sets == 4 && W(&e, 2)->rotation == 0 && W(&e, 2)->spin == 20,
          "a failed set: dirty %u sets %u", W(&e, 2)->dirty, W(&e, 2)->rotation_sets);
    /* A new widget in a slot starts its count again. */
    RUN(&e, "c:delete()");
    apply(&e, NULL);
    RUN(&e, "d = ui.image{src = 'b.bin'}");
    CHECK(W(&e, 2)->alive && W(&e, 2)->rotation_sets == 0 && W(&e, 2)->spin == 0 && W(&e, 2)->rotation == 0,
          "the slot's next widget: sets %u spin %d rotation %d", W(&e, 2)->rotation_sets, W(&e, 2)->spin, W(&e, 2)->rotation);
    finish(&e);
}

/* ---- A dying VM: every binding stops at apps_vm_check ---- */

static int probe_calls;

static int count_probe(lua_State *L) {
    (void)L;
    probe_calls++;
    return 0;
}

/* Each binding runs in a VM and a table of its own, killed first, from a chunk run outside apps_vm_call: a kill
 * re-arms the thread's hook to fire at every instruction, so only the first chunk after it could show the check.
 * Here a chunk is a few instructions, and only the check can stop it: nothing may run after the call (probe),
 * and the table must be as it was (not a widget more, not a bit, not a colour). The methods are called through
 * the functions saved before the kill: w:get would stop in widget_index, before get is reached. */
static void vm_check(void) {
    static const char *const chunks[] = {
        "local v = ui.label{} probe() return v",
        "local v = ui.button{text = 'b'} probe() return v",
        "local v = ui.box{} probe() return v",
        "local v = ui.arc{} probe() return v",
        "local v = ui.bar{} probe() return v",
        "local v = ui.image{src = 'a.bin'} probe() return v",
        "ui.bg('#102030') probe()",
        "ui.clear() probe()",
        "local x = ui.polar(90, 100) probe() return x",
        "local v = ui.nothing probe() return v",
        "local v = f_get(w, 'text') probe() return v",
        "f_set(w, {x = 9, text = 'changed'}) probe()",
        "f_on(w, 'tap', probe) probe()",
        "f_delete(w) probe()",
        "local v = tostring(w) probe() return v",
        "local v = (w == w2) probe() return v",
        "local v = w.nothing probe() return v",
        "w.x = 1 probe()",
        /* The check comes before the arguments are looked at: a call that would be refused for them stops at it. */
        "local v = ui.label('not a table') probe() return v",
        "local v = ui.image{} probe() return v",
        "ui.bg({}) probe()",
        "local x = ui.polar('a', 1) probe() return x",
        "local v = f_get(w, 5) probe() return v",
        "local v = f_get(w, 'nothing') probe() return v",
        "f_set(w, 'not a table') probe()",
        "f_set(w, {nothing = 1}) probe()",
        "f_on(w, 5, probe) probe()",
        "f_on(w, 'swipe', probe) probe()",
        "f_on(w, 'tap', 'not a function') probe()",
        "f_delete(5) probe()",
        "local v = f_get(5, 'x') probe() return v",
    };
    for (size_t i = 0; i < ARRAY_LEN(chunks); i++) {
        env_t e;
        CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");
        RUN(&e,
            "w = ui.label{text = 'a'} w2 = ui.label{}\n"
            "f_get, f_set, f_on, f_delete = w.get, w.set, w.on, w.delete");
        apply(&e, NULL);
        apps_widget_t before[3];
        for (int slot = 0; slot < 3; slot++) {
            memcpy(&before[slot], W(&e, slot), sizeof before[slot]);
        }
        lua_State *L = apps_vm_lua(e.vm);
        lua_pushcfunction(L, count_probe);
        lua_setglobal(L, "probe");
        probe_calls = 0;
        apps_vm_kill(e.vm, "stopped by test");
        char err[256] = "";
        CHECK(apps_vm_load(e.vm, "t.lua", chunks[i], strlen(chunks[i]), err, sizeof err), "%s: %s", chunks[i], err);
        int rc = lua_pcall(L, 0, 1, 0);
        CHECK(rc == LUA_ERRRUN && lua_type(L, -1) == LUA_TLIGHTUSERDATA && probe_calls == 0,
              "'%s': status %d, the error is a %s, %d probe calls", chunks[i], rc, luaL_typename(L, -1), probe_calls);
        lua_settop(L, 0);
        pass_t seen;
        apply(&e, &seen);
        bool same = !seen.cleared && !seen.bg_changed && seen.bg == 0 && apps_ui_count(e.ui) == 2;
        for (int slot = 0; slot < 3; slot++) {
            same = same && memcmp(&before[slot], W(&e, slot), sizeof before[slot]) == 0 && seen.dirty[slot] == 0;
        }
        CHECK(same, "'%s' changed the table of a dying app", chunks[i]);
        CHECK(!lock_held && lock_errors == 0, "'%s': the lock", chunks[i]);
        finish(&e);
    }
}

/* ---- Slots: the quota, and what holds a slot ---- */

static void slot_reuse(void) {
    OPEN(e);
    RUN(&e, "t = {} for i = 1, 128 do t[i] = ui.label{} end");
    CHECK(apps_ui_count(e.ui) == 128, "count %d", apps_ui_count(e.ui));
    RUN_FAILS_EXACTLY(&e, "ui.label{}", "t.lua:1: ui: no more than 128 widgets per app");
    RUN_FAILS(&e, "ui.box{}", "ui: no more than 128 widgets per app");
    apply(&e, NULL);

    /* A widget the apply step has made keeps its slot until the step has deleted its object. */
    RUN(&e, "t[6]:delete()");
    CHECK(apps_ui_count(e.ui) == 127, "count %d", apps_ui_count(e.ui));
    RUN_FAILS_EXACTLY(&e, "ui.label{}",
                      "t.lua:1: ui: no more than 128 widgets per app (a deleted widget keeps its slot until the "
                      "screen next updates)");
    apply(&e, NULL);
    RUN(&e, "n = ui.label{text = 'new'}");
    CHECK(W(&e, 5)->alive && W(&e, 5)->gen == 1 && strcmp(W(&e, 5)->text, "new") == 0, "slot 5 on gen 1");
    RUN_FAILS_EXACTLY(&e, "t[6]:get('text')", "t.lua:1: widget was deleted");
    RUN_FAILS(&e, "ui.label{}", "ui: no more than 128 widgets per app");

    /* One that it never made has no object to wait for: its slot is free at once. */
    apply(&e, NULL);
    RUN(&e, "t[7]:delete() t[8]:delete()");  /* made and applied */
    apply(&e, NULL);
    RUN(&e, "p = ui.label{} q = ui.label{}");  /* the two slots again, not yet applied */
    RUN(&e, "p:delete() r = ui.label{}");
    CHECK(W(&e, 6)->alive && W(&e, 6)->gen == 2 && W(&e, 7)->alive, "slots 6 and 7 are taken again");
    finish(&e);
}

/* ---- Deleting a box ---- */

static void cascade(void) {
    OPEN(e);
    RUN(&e,
        "a = ui.box{}\n"
        "b = ui.box{parent = a}\n"
        "c = ui.label{parent = b, text = 'deep'}\n"
        "d = ui.button{parent = a, text = 'Go'}\n"
        "d:on('tap', function() end)\n"
        "c:on('press', function() end)\n"
        "e = ui.label{text = 'apart'}\n"
        "e:on('tap', function() end)");
    apply(&e, NULL);
    RUN(&e, "late = ui.label{parent = b, text = 'late'}");  /* in the box, and no object yet */
    CHECK(W(&e, 5)->dirty != 0 && W(&e, 5)->parent == 1, "late: dirty %u", W(&e, 5)->dirty);
    RUN(&e, "a:delete()");
    for (int i = 0; i < 4; i++) {
        const apps_widget_t *w = W(&e, i);
        CHECK(!w->alive && w->gen == 1 && w->dirty == APPS_DIRTY_DELETE && w->text == NULL && w->events == 0,
              "slot %d: alive %d gen %d dirty %u", i, w->alive, w->gen, w->dirty);
    }
    CHECK(W(&e, 1)->parent == 0 && W(&e, 2)->parent == 1 && W(&e, 3)->parent == 0 && W(&e, 1)->kind == APPS_W_BOX &&
              W(&e, 2)->kind == APPS_W_LABEL,
          "a deleted widget keeps its kind and parent for the apply step");
    CHECK(!W(&e, 5)->alive && W(&e, 5)->dirty == 0 && W(&e, 5)->gen == 1, "late: dirty %u", W(&e, 5)->dirty);
    CHECK(W(&e, 4)->alive && W(&e, 4)->gen == 0 && W(&e, 4)->events == APPS_UI_TAP && W(&e, 4)->dirty == 0 &&
              strcmp(W(&e, 4)->text, "apart") == 0,
          "the label outside is as it was");
    CHECK(apps_ui_count(e.ui) == 1, "count %d", apps_ui_count(e.ui));
    for (const char *name = "bcd"; *name != '\0'; name++) {
        char src[64];
        snprintf(src, sizeof src, "%c:get('x')", *name);
        RUN_FAILS(&e, src, "widget was deleted");
        snprintf(src, sizeof src, "%c:set{x = 1}", *name);
        RUN_FAILS(&e, src, "widget was deleted");
    }
    RUN_FAILS(&e, "ui.label{parent = b}", "parent was deleted");
    RUN(&e, "e:set{text = 'still here'}");
    pass_t seen;
    apply(&e, &seen);
    CHECK(seen.dirty[0] == APPS_DIRTY_DELETE && seen.dirty[1] == APPS_DIRTY_DELETE && seen.dirty[2] == APPS_DIRTY_DELETE &&
              seen.dirty[3] == APPS_DIRTY_DELETE && seen.dirty[5] == 0 && seen.dirty[4] == APPS_DIRTY_TEXT,
          "the pass saw %u %u %u %u %u %u", seen.dirty[0], seen.dirty[1], seen.dirty[2], seen.dirty[3], seen.dirty[4],
          seen.dirty[5]);

    /* A tree no pass has seen leaves nothing to delete. */
    RUN(&e, "x = ui.box{} y = ui.box{parent = x} z = ui.label{parent = y} x:delete()");
    for (int i = 0; i < 6; i++) {
        CHECK(W(&e, i)->dirty == 0, "slot %d: dirty %u", i, W(&e, i)->dirty);
    }
    finish(&e);
}

/* ---- ui.clear ---- */

static void clear(void) {
    OPEN(e);
    RUN(&e, "ui.bg('#102030') a = ui.label{text = 'a'} b = ui.button{text = 'b'} b:on('tap', function() end)");
    apply(&e, NULL);
    RUN(&e, "c = ui.label{text = 'c'} a:delete()");  /* c is not applied, and a is waiting for its delete */
    RUN(&e, "ui.clear()");
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = W(&e, i);
        CHECK(!w->alive && w->dirty == 0 && w->text == NULL && w->events == 0, "slot %d: %d %u", i, w->alive, w->dirty);
    }
    CHECK(W(&e, 0)->gen == 1 && W(&e, 1)->gen == 1 && W(&e, 2)->gen == 1, "gens %d %d %d", W(&e, 0)->gen, W(&e, 1)->gen,
          W(&e, 2)->gen);
    CHECK(apps_ui_count(e.ui) == 0, "count %d", apps_ui_count(e.ui));
    for (const char *name = "abc"; *name != '\0'; name++) {
        char src[64];
        snprintf(src, sizeof src, "%c:get('x')", *name);
        RUN_FAILS_EXACTLY(&e, src, "t.lua:1: widget was deleted");
    }
    pass_t seen;
    apply(&e, &seen);
    CHECK(seen.cleared && seen.bg == 0x102030 && !seen.bg_changed, "cleared %d bg %x changed %d", seen.cleared, seen.bg,
          seen.bg_changed);  /* the colour was taken by the first pass: a clear keeps it */
    apply(&e, &seen);
    CHECK(!seen.cleared && !seen.bg_changed, "a second pass: cleared %d changed %d", seen.cleared, seen.bg_changed);

    /* Every slot is free the moment it has run, and a new widget can follow it in the same chunk. */
    RUN(&e, "for i = 1, 128 do ui.label{} end ui.clear() for i = 1, 128 do ui.label{} end");
    CHECK(apps_ui_count(e.ui) == 128 && W(&e, 0)->gen == 2, "count %d gen %d", apps_ui_count(e.ui), W(&e, 0)->gen);
    apply(&e, &seen);
    CHECK(seen.cleared && seen.dirty[0] != 0, "cleared %d, and the widgets made after it are there to apply", seen.cleared);

    /* Nothing to clear is still a clear. */
    RUN(&e, "ui.clear() ui.clear()");
    apply(&e, &seen);
    CHECK(seen.cleared, "an empty table: cleared %d", seen.cleared);
    finish(&e);
}

/* ---- ui.bg ---- */

static void bg(void) {
    OPEN(e);
    bool changed = false;
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0 && changed, "the first take: changed %d", changed);
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0 && !changed, "the second: changed %d", changed);
    CHECK(apps_ui_take_bg(e.ui, NULL) == 0, "no changed");
    RUN(&e, "ui.bg('#123456')");
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0x123456 && changed, "changed %d", changed);
    RUN(&e, "ui.bg('#123456') ui.bg(0x123456) ui.bg(1193046.0)");  /* the same colour, however it is written */
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0x123456 && !changed, "unchanged: changed %d", changed);
    RUN(&e, "ui.bg('#f80')");
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0xFF8800 && changed, "#f80");
    RUN_FAILS_EXACTLY(&e, "ui.bg('#12')", "t.lua:1: ui.bg: '#12' is not a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)");
    RUN_FAILS_EXACTLY(&e, "ui.bg('none')", "t.lua:1: ui.bg: 'none' is not a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)");
    RUN_FAILS_EXACTLY(&e, "ui.bg(0x1000000)", "t.lua:1: ui.bg: '16777216' is not a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)");
    RUN_FAILS_EXACTLY(&e, "ui.bg(1.5)", "t.lua:1: ui.bg: '1.5' is not a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)");
    RUN_FAILS_EXACTLY(&e, "ui.bg(-1)", "t.lua:1: ui.bg: '-1' is not a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB)");
    RUN_FAILS_EXACTLY(&e, "ui.bg({})", "t.lua:1: ui.bg: needs a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB), got table");
    RUN_FAILS_EXACTLY(&e, "ui.bg()", "t.lua:1: ui.bg: needs a colour (use \"#rgb\", \"#rrggbb\" or 0xRRGGBB), got no value");
    CHECK(apps_ui_take_bg(e.ui, &changed) == 0xFF8800 && !changed, "a refused colour changed nothing");
    finish(&e);
}

/* ---- Handlers ---- */

typedef struct {
    apps_ui_t *ui;
    uint16_t slot, gen;
    uint8_t event;
    int fill;  /* nils pushed first, to leave the call LUA_MINSTACK - fill free slots */
    bool result;
    int top_before, top_after;
} probe_t;

/* Under apps_vm_call: push_handler, and call what it pushed. */
static int probe_trampoline(lua_State *L) {
    probe_t *p = lua_touserdata(L, 1);
    for (int i = 0; i < p->fill; i++) {
        lua_pushnil(L);
    }
    p->top_before = lua_gettop(L);
    p->result = apps_ui_push_handler(L, p->ui, p->slot, p->gen, p->event);
    p->top_after = lua_gettop(L);
    if (p->result) {
        lua_call(L, 2, 0);
    }
    return 0;
}

/* One touch's dispatch, as apps_rt_touch does it: inside a call, with fill nils under it. *status is how the call
 * ended. */
static probe_t probe_filled(env_t *e, int slot, int gen, uint8_t event, int fill, apps_call_t *status, char *err,
                            size_t err_len) {
    probe_t p = {.ui = e->ui, .slot = (uint16_t)slot, .gen = (uint16_t)gen, .event = event, .fill = fill};
    lua_State *L = apps_vm_lua(e->vm);
    lua_pushcfunction(L, probe_trampoline);
    lua_pushlightuserdata(L, &p);
    *status = apps_vm_call(e->vm, 1, 0, APPS_BUDGET_CALL, err, err_len);
    return p;
}

static probe_t probe_status(env_t *e, int slot, int gen, uint8_t event, apps_call_t *status, char *err, size_t err_len) {
    return probe_filled(e, slot, gen, event, 0, status, err, err_len);
}

static probe_t probe_at(int line, env_t *e, int slot, int gen, uint8_t event) {
    char err[256] = "";
    apps_call_t status;
    probe_t p = probe_status(e, slot, gen, event, &status, err, sizeof err);
    CHECK_AT(line, status == APPS_CALL_OK, "probe: %s", err);
    after(line, e);
    return p;
}
#define PROBE(e, slot, gen, event) probe_at(__LINE__, (e), (slot), (gen), (event))

static void push_handler(void) {
    OPEN(e);
    RUN(&e,
        "hits = 0 last_event = '' last_widget = ''\n"
        "a = ui.button{text = 'Go'}\n"
        "a:on('click', function(w, ev) hits = hits + 1 last_event = ev last_widget = tostring(w) end)\n"
        "a:on('long_press', function(w, ev) hits = hits + 10 last_event = ev end)\n"
        "b = ui.label{}");
    probe_t p = PROBE(&e, 0, 0, APPS_UI_TAP);
    CHECK(p.result && p.top_after == p.top_before + 3, "tap: %d, %d values pushed", p.result, p.top_after - p.top_before);
    RUN(&e, "assert(hits == 1 and last_event == 'tap' and last_widget == 'button#0', last_event)");
    p = PROBE(&e, 0, 0, APPS_UI_LONG_PRESS);
    CHECK(p.result, "long_press");
    RUN(&e, "assert(hits == 11 and last_event == 'long_press')");

    /* Nothing is pushed for what isn't there to run. */
    static const struct {
        const char *why;
        int slot, gen;
        uint8_t event;
    } nothing[] = {
        {"an event nobody asked for", 0, 0, APPS_UI_PRESS},
        {"another event, not subscribed", 0, 0, APPS_UI_RELEASE},
        {"a gen that isn't the widget's", 0, 1, APPS_UI_TAP},
        {"a widget with no handlers", 1, 0, APPS_UI_TAP},
        {"a slot that was never used", 2, 0, APPS_UI_TAP},
        {"a slot out of range", APPS_UI_SLOTS, 0, APPS_UI_TAP},
        {"no event", 0, 0, 0},
        {"two events", 0, 0, APPS_UI_TAP | APPS_UI_LONG_PRESS},
        {"an event that isn't one", 0, 0, 16},
    };
    for (size_t i = 0; i < ARRAY_LEN(nothing); i++) {
        probe_t q = PROBE(&e, nothing[i].slot, nothing[i].gen, nothing[i].event);
        CHECK(!q.result && q.top_after == q.top_before, "%s: %d, %d values pushed", nothing[i].why, q.result,
              q.top_after - q.top_before);
    }
    RUN(&e, "assert(hits == 11)");

    /* Each check stands alone: a slot whose flags and registry disagree, as no Lua call can make one (so the
     * invariants are not checked while it does), is refused all the same. */
    char err[256] = "";
    apps_call_t status;
    W(&e, 0)->alive = false;
    p = probe_status(&e, 0, 0, APPS_UI_TAP, &status, err, sizeof err);
    CHECK(status == APPS_CALL_OK && !p.result && p.top_after == p.top_before,
          "a dead slot with its handler still in the registry: %d, %s", p.result, err);
    W(&e, 0)->alive = true;
    uint8_t subscribed = W(&e, 0)->events;
    W(&e, 0)->events = 0;
    p = probe_status(&e, 0, 0, APPS_UI_TAP, &status, err, sizeof err);
    CHECK(status == APPS_CALL_OK && !p.result && p.top_after == p.top_before,
          "a mask that says no, with the handler still in the registry: %d, %s", p.result, err);
    W(&e, 0)->events = subscribed;
    p = PROBE(&e, 0, 0, APPS_UI_TAP);
    CHECK(p.result, "the same slot, put back");
    RUN(&e, "assert(hits == 12)");

    /* Deleted, and the slot lived again: the old gen finds nothing, the new one finds the new handler. */
    RUN(&e, "a:delete()");
    p = PROBE(&e, 0, 0, APPS_UI_TAP);
    CHECK(!p.result, "a deleted widget's tap");
    p = PROBE(&e, 0, 1, APPS_UI_TAP);
    CHECK(!p.result, "the dead slot on its new gen");
    apply(&e, NULL);
    RUN(&e, "c = ui.button{} c:on('tap', function(w, ev) hits = 100 end)");
    CHECK(W(&e, 0)->gen == 1 && W(&e, 0)->alive, "the slot lives again on gen 1");
    p = PROBE(&e, 0, 0, APPS_UI_TAP);
    CHECK(!p.result, "the old gen on the new widget");
    p = PROBE(&e, 0, 1, APPS_UI_TAP);
    CHECK(p.result, "the new widget's tap");
    RUN(&e, "assert(hits == 100)");

    /* Unsubscribed: gone from the mask and from the registry. */
    RUN(&e, "c:on('tap', nil)");
    p = PROBE(&e, 0, 1, APPS_UI_TAP);
    CHECK(!p.result, "after on(tap, nil)");
    finish(&e);
}

/* push_handler reserves what it pushes, whatever room its caller left: a C function starts with LUA_MINSTACK free
 * slots, and the probe fills them down to `left`. Its highest point is a handle the app already holds (create keeps
 * it): luaL_testudata's two metatables over the handler, the handle table and the handle, 5 slots. The test builds
 * this harness on a Lua with LUA_USE_APICHECK, which aborts on a push past the slots reserved. */
static void push_handler_reserves_its_slots(void) {
    OPEN(e);
    RUN(&e, "hits = 0 a = ui.button{text = 'Go'} a:on('tap', function(w) assert(w == a) hits = hits + 1 end)");
    for (int left = 0; left <= 6; left++) {
        char err[256] = "";
        apps_call_t status;
        probe_t p = probe_filled(&e, 0, 0, APPS_UI_TAP, LUA_MINSTACK - left, &status, err, sizeof err);
        CHECK(status == APPS_CALL_OK && p.result && p.top_after == p.top_before + 3, "%d slots left: %d, %s", left,
              p.result, err);
    }
    RUN(&e, "assert(hits == 7, hits)");
    finish(&e);
}

/* The memory a handler's closure holds (a string of 200 KB, here) is let go of when the widget goes: the
 * registry is a root, so a handler that stayed in it would be kept for good. */
static void handlers_dropped(void) {
    static const struct {
        const char *name, *drop;
    } ways[] = {
        {"delete", "w:delete()"},
        {"a box's delete", "box:delete()"},
        {"clear", "ui.clear()"},
        {"set to nil", "w:on('tap', nil)"},
    };
    for (size_t i = 0; i < ARRAY_LEN(ways); i++) {
        OPEN(e);
        RUN(&e,
            "do local big = string.rep('x', 200000)\n"
            "   box = ui.box{}\n"
            "   w = ui.button{parent = box, text = 'Go'}\n"
            "   w:on('tap', function() return #big end)\n"
            "end");
        lua_State *L = apps_vm_lua(e.vm);
        lua_gc(L, LUA_GCCOLLECT);
        size_t held_by_handler = apps_vm_mem_used(e.vm);
        CHECK(held_by_handler > 200000, "%s: the handler holds %zu bytes", ways[i].name, held_by_handler);
        RUN(&e, ways[i].drop);
        lua_gc(L, LUA_GCCOLLECT);
        size_t after_drop = apps_vm_mem_used(e.vm);
        CHECK(after_drop < 100000, "%s: %zu bytes after the handler is dropped", ways[i].name, after_drop);
        finish(&e);
    }
}

/* ---- Images ---- */

static void assets(void) {
    apps_ui_t *ui = apps_ui_new();
    CHECK(ui != NULL, "apps_ui_new");
    CHECK(apps_ui_asset(ui, 0) == NULL && apps_ui_asset(ui, -1) == NULL, "no images yet");
    apps_asset_t list[APPS_UI_ASSETS_MAX + 5];
    static char paths[APPS_UI_ASSETS_MAX + 5][16];
    for (size_t i = 0; i < ARRAY_LEN(list); i++) {
        snprintf(paths[i], sizeof paths[i], "i%zu.bin", i);
        list[i] = (apps_asset_t){.path = paths[i], .data = (const uint8_t *)"x", .len = 1, .w = (uint16_t)(i + 1), .h = 2};
    }
    apps_ui_set_assets(ui, list, 3);
    memset(list, 0, sizeof list);  /* the table has its own copy */
    CHECK(apps_ui_asset(ui, 2) != NULL && strcmp(apps_ui_asset(ui, 2)->path, "i2.bin") == 0 && apps_ui_asset(ui, 2)->w == 3,
          "a copy of the list");
    CHECK(apps_ui_asset(ui, 3) == NULL && apps_ui_asset(ui, -1) == NULL, "past the end");
    for (size_t i = 0; i < ARRAY_LEN(list); i++) {
        list[i] = (apps_asset_t){.path = paths[i], .data = (const uint8_t *)"x", .len = 1, .w = 1, .h = 1};
    }
    apps_ui_set_assets(ui, list, (int)ARRAY_LEN(list));  /* more than a package has files */
    CHECK(apps_ui_asset(ui, APPS_UI_ASSETS_MAX - 1) != NULL && apps_ui_asset(ui, APPS_UI_ASSETS_MAX) == NULL, "cut at %d",
          APPS_UI_ASSETS_MAX);
    apps_ui_set_assets(ui, NULL, 4);
    CHECK(apps_ui_asset(ui, 0) == NULL, "no list, no images");
    apps_ui_set_assets(ui, list, -1);
    CHECK(apps_ui_asset(ui, 0) == NULL, "a negative count");
    apps_ui_set_assets(NULL, list, 1);  /* ignored */
    CHECK(apps_ui_slot(ui, -1) == NULL && apps_ui_slot(ui, APPS_UI_SLOTS) == NULL && apps_ui_slot(ui, 0) != NULL &&
              apps_ui_slot(ui, APPS_UI_SLOTS - 1) != NULL && apps_ui_slot(NULL, 0) == NULL,
          "slot bounds");
    CHECK(apps_ui_count(NULL) == 0 && apps_ui_count(ui) == 0, "count");
    apps_ui_free(ui);
    apps_ui_free(NULL);
    CHECK(held == 0, "%zu bytes held", held);
}

/* ---- The dump ---- */

static const char *const DUMP_SRC =
    "a = ui.label{text = 'Time', x = 5, y = -10}\n"
    "b = ui.button{text = 'OK', w = 120, h = 48, hidden = true}\n"
    "c = ui.box{w = 200}\n"
    "d = ui.arc{}\n"
    "e = ui.label{text = 'say \"hi\"\\\\ \\n\\t\\1 \\xE2\\x80\\x99 \\xF0\\x9F\\x98\\x80'}\n"
    "f = ui.bar{h = 10}\n"
    "g = ui.label{text = string.rep('long text ', 25)}\n"
    "ui.label{text = 'gone'}:delete()\n";

/* What the apply step writes back: rw > 0 puts the dump on the laid-out geometry. */
static void dump_resolved(void) {
    OPEN(e);
    RUN(&e, DUMP_SRC);
    apps_widget_t *a = W(&e, 0);
    apps_port_ui_lock();
    a->rx = 117;
    a->ry = 200;
    a->rw = 232;
    a->rh = 24;
    a->overflow = 14;
    W(&e, 1)->rx = 0;  /* laid out only when rw > 0: the other three are no evidence */
    W(&e, 1)->ry = 99;
    W(&e, 1)->rh = 99;
    apps_port_ui_unlock();
    char out[8192];
    int dropped = -1;
    size_t n = apps_ui_dump(e.ui, out, sizeof out, &dropped);
    printf("%s\n", out);
    CHECK(n == strlen(out) && dropped == 0 && out[0] == '[' && out[n - 1] == ']', "n %zu dropped %d", n, dropped);
    CHECK(apps_json_validate(out, n, (char[96]){0}, 96), "valid JSON");
    CHECK(!lock_held && lock_errors == 0, "the dump left the lock: %d", lock_held);
    finish(&e);
}

/* Every cap, from nothing to more than the whole: each result printed for the test, which checks the JSON
 * parses and what was dropped, and that the checks of the buffer's bounds pass (the block is exactly cap bytes). */
static void dump_caps(void) {
    OPEN(e);
    RUN(&e, DUMP_SRC);
    char whole[8192];
    int dropped = -1;
    size_t full = apps_ui_dump(e.ui, whole, sizeof whole, &dropped);
    CHECK(full > 0 && dropped == 0, "the whole dump: %zu, dropped %d", full, dropped);
    printf("FULL %zu %s\n", full, whole);
    for (size_t cap = 0; cap <= full + 4; cap++) {
        char *out = malloc(cap != 0 ? cap : 1);
        if (out == NULL) {
            exit(2);
        }
        if (cap > 0) {
            memset(out, 'X', cap);
        }
        dropped = -1;
        size_t n = apps_ui_dump(e.ui, out, cap, &dropped);
        CHECK(cap == 0 ? n == 0 : n < cap && out[n] == '\0', "cap %zu: n %zu", cap, n);
        printf("CAP %zu %zu %d %s\n", cap, n, dropped, cap > 0 ? out : "");
        free(out);
    }
    /* No buffer at all, and no count wanted. */
    CHECK(apps_ui_dump(e.ui, NULL, 100, &dropped) == 0 && dropped == 7, "NULL out: dropped %d", dropped);
    CHECK(apps_ui_dump(e.ui, whole, 0, NULL) == 0, "cap 0, no count");
    CHECK(apps_ui_dump(NULL, whole, sizeof whole, &dropped) == 0 && dropped == 0 && whole[0] == '\0', "NULL table");
    CHECK(!lock_held && lock_errors == 0, "the dump left the lock");
    finish(&e);
}

/* ---- The lock, and a heap that refuses ---- */

/* The source the sweeps run: every binding, success and failure. */
static const char *const EVERYTHING =
    "local b = ui.box{bg = '#102030', w = 300, h = 200}\n"
    "local l = ui.label{parent = b, text = 'hello'}\n"
    "local c = ui.button{parent = b, text = 'Go'}\n"
    "c:on('tap', function() end)\n"
    "c:on('release', function() end)\n"
    "l:set{text = 'world', color = '#fff', x = 3}\n"
    "l:set{text = ''}\n"
    "l:set{text = 'again'}\n"
    "local a = ui.arc{value = 10}\n"
    "a:set{value = 20, max = 50, thickness = 4}\n"
    "local t = ui.label{text = 'x'}\n"
    "t:delete()\n"
    "pcall(ui.label, {x = 'no'})\n"
    "pcall(l.set, l, {text = '\\255'})\n"
    "pcall(l.get, l, 'nope')\n"
    "pcall(function() return l.nope end)\n"
    "assert(l:get('text') == 'again' and tostring(l) == 'label#1')\n"
    "ui.polar(30, 100)\n"
    "ui.bg('#010203')\n"
    "local deep = ui.box{parent = b}\n"
    "ui.label{parent = deep, text = 'in'}\n"
    "b:delete()\n"
    "ui.label{text = 'y'}\n"
    "ui.clear()\n"
    "ui.label{text = 'z'}\n"
    "assert(select('#', ui.polar(0, 1)) == 2)\n";

static void lock_discipline(void) {
    OPEN(e);
    RUN(&e, EVERYTHING);  /* RUN checks the lock and the heap after the chunk; the port counts every violation */
    CHECK(lock_errors == 0, "%d violations: %s", lock_errors, lock_note);
    char out[4096];
    apps_ui_dump(e.ui, out, sizeof out, NULL);
    after(__LINE__, &e);
    finish(&e);
}

/* Every growth request from the Nth on refused, for each N until none is reached, over the whole life of a
 * table: nothing aborts, nothing leaks, the lock is always given back, the heap is not used under it and the
 * invariants hold wherever the run was cut short. */
static void refused(void) {
    long total = 0;
    for (long k = 1; k < 100000; k++) {
        growths = 0;
        refuse_from = k;
        env_t e = {0};
        bool opened = env_open(&e, TWO_IMAGES, 2);
        char err[512] = "";
        bool ran = opened && run(&e, EVERYTHING, err, sizeof err);
        bool was_refused = growths >= k;
        refuse_from = -1;
        if (!was_refused) {
            CHECK(opened && ran, "k=%ld: nothing was refused, but the run failed: %s", k, err);
            total = k;
        } else {
            CHECK(!opened || !ran, "k=%ld: something was refused and the run went on", k);
        }
        CHECK(!lock_held && lock_errors == 0, "k=%ld: the lock: held %d, %d violations: %s", k, lock_held, lock_errors,
              lock_note);
        if (e.ui != NULL && opened) {
            check_invariants(__LINE__, &e);
        }
        env_close(&e);
        CHECK(held == 0, "k=%ld: %zu bytes held", k, held);
        if (!was_refused) {
            break;
        }
    }
    CHECK(total > 100, "only %ld growth requests in the run", total);
}

/* The same for a touch: the handle and the event's name are made in the app's heap, inside the call. */
static void touch_refused(void) {
    long reached = 0;
    for (long k = 1; k < 1000; k++) {
        env_t e;
        CHECK(env_open(&e, NULL, 0), "env_open");
        /* No handle is held (the app let go of it, and a collection took it): the touch has to make one. */
        RUN(&e, "ui.button{}:on('tap', function() end)");
        lua_gc(apps_vm_lua(e.vm), LUA_GCCOLLECT);
        long before = growths;
        refuse_from = growths + k;
        char err[256] = "";
        apps_call_t status;
        probe_t p = probe_status(&e, 0, 0, APPS_UI_TAP, &status, err, sizeof err);
        bool was_refused = growths >= refuse_from;
        refuse_from = -1;
        CHECK(was_refused ? status == APPS_CALL_NOMEM : status == APPS_CALL_OK && p.result,
              "k=%ld: refused %d, the call ended %d (%s)", k, was_refused, status, err);
        CHECK(!lock_held && lock_errors == 0, "k=%ld: the lock", k);
        if (!was_refused) {
            reached = growths - before;
            finish(&e);
            break;
        }
        finish(&e);
    }
    CHECK(reached >= 1, "the touch made no allocation to refuse (%ld)", reached);
}

/* ---- A box deleted over a lower slot ---- */

/* Slots are reused, so a widget can sit in a lower slot than the box it is in. Deleting the box must still take it:
 * nothing may stay alive under a dead box (the apply step walks parent chains, and LVGL deletes a box's children
 * with it). Once with objects the apply step has made, which it has to delete, and once with none. */
static void cascade_lower_slot(void) {
    for (int applied = 0; applied < 2; applied++) {
        OPEN(e);
        RUN(&e, "x = ui.label{} p = ui.box{} x:delete() c = ui.label{parent = p, text = 'in'}");
        CHECK(W(&e, 0)->alive && W(&e, 0)->gen == 1 && W(&e, 0)->parent == 1 && W(&e, 1)->kind == APPS_W_BOX,
              "the child is in slot 0, the box in slot 1: alive %d gen %d parent %d", W(&e, 0)->alive, W(&e, 0)->gen,
              W(&e, 0)->parent);
        if (applied) {
            apply(&e, NULL);
        }
        RUN(&e, "p:delete()");
        const uint32_t want = applied ? APPS_DIRTY_DELETE : 0;
        for (int slot = 0; slot < 2; slot++) {
            const apps_widget_t *w = W(&e, slot);
            CHECK(!w->alive && w->dirty == want && w->text == NULL && w->events == 0,
                  "applied %d, slot %d: alive %d dirty %u, want %u", applied, slot, w->alive, w->dirty, want);
        }
        CHECK(W(&e, 0)->gen == 2 && W(&e, 1)->gen == 1, "applied %d: gens %d and %d", applied, W(&e, 0)->gen,
              W(&e, 1)->gen);
        CHECK(apps_ui_count(e.ui) == 0, "applied %d: %d alive", applied, apps_ui_count(e.ui));
        RUN_FAILS(&e, "c:get('x')", "widget was deleted");
        RUN_FAILS(&e, "p:get('x')", "widget was deleted");
        finish(&e);
    }
}

/* ---- Every change takes the lock ---- */

static void takes_lock_at(int line, env_t *e, const char *src) {
    long before = lock_takes;
    run_ok_at(line, e, src);
    CHECK_AT(line, lock_takes > before, "'%s' changed the table and never took the lock", src);
}
#define TAKES_LOCK(e, src) takes_lock_at(__LINE__, (e), (src))

/* The port checks that the lock is kept and given back in order; this is the other half of the rule, that each
 * change takes it at all (the apply step reads, clears and writes the same fields on the LVGL task). */
static void lock_taken(void) {
    env_t e;
    CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");
    TAKES_LOCK(&e, "a = ui.label{text = 'a'}");
    TAKES_LOCK(&e, "b = ui.button{}");
    TAKES_LOCK(&e, "c = ui.box{}");
    TAKES_LOCK(&e, "d = ui.arc{}");
    TAKES_LOCK(&e, "f = ui.bar{}");
    TAKES_LOCK(&e, "g = ui.image{src = 'a.bin'}");
    TAKES_LOCK(&e, "a:set{x = 5}");
    TAKES_LOCK(&e, "a:set{text = 'changed'}");
    TAKES_LOCK(&e, "a:set{x = 5, text = 'changed'}");  /* nothing differs */
    TAKES_LOCK(&e, "a:on('tap', function() end)");
    TAKES_LOCK(&e, "a:on('tap', nil)");
    TAKES_LOCK(&e, "ui.bg('#123456')");
    TAKES_LOCK(&e, "ui.bg('#123456')");
    TAKES_LOCK(&e, "child = ui.label{parent = c}");
    TAKES_LOCK(&e, "child:delete()");
    TAKES_LOCK(&e, "c:delete()");
    TAKES_LOCK(&e, "ui.clear()");
    long before = lock_takes;
    char out[64];
    apps_ui_dump(e.ui, out, sizeof out, NULL);
    CHECK(lock_takes > before, "the dump took no lock");
    finish(&e);
}

/* ---- The metatables apps can reach ---- */

/* F1: a finalizer runs with the hooks off, beyond the budget and the kill. The handle's metatable is shared and C
 * puts it on every new handle, so an app that could add __gc to it would give every later handle one. It can't:
 * getmetatable gives a string, and nothing it does reaches the real table. A full collection runs every finalizer
 * that was ever set. */
static void metatables(void) {
    OPEN(e);
    RUN(&e,
        "ran = 0\n"
        "w = ui.label{}\n"
        "assert(type(getmetatable(w)) ~= 'table' and type(getmetatable(ui)) ~= 'table')\n"
        "assert(not pcall(function() getmetatable(w).__gc = function() ran = ran + 1 end end))\n"
        "assert(not pcall(function() getmetatable(ui).__gc = function() ran = ran + 1 end end))\n"
        "assert(not pcall(setmetatable, ui, {__gc = function() ran = ran + 1 end}))\n"
        "assert(not pcall(setmetatable, ui, {}))\n"
        "for i = 1, 300 do ui.label{}:delete() ui.box{}:delete() end\n"
        "ui.clear()\n"
        "for i = 1, 100 do ui.label{} end\n");
    lua_State *L = apps_vm_lua(e.vm);
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCCOLLECT);
    RUN(&e, "assert(ran == 0, ran)");
    finish(&e);
}

/* ---- One handle for one widget ---- */

/* F5: the app holds a handle for as long as it likes, and finds the same object again: as a table key, with
 * rawequal. The table that remembers them is weak, so it does not keep a handle the app has let go of. */
static void identity(void) {
    OPEN(e);
    RUN(&e,
        "box = ui.box{} a = ui.label{parent = box} b = ui.label{parent = box}\n"
        "assert(rawequal(a:get('parent'), box) and rawequal(a:get('parent'), b:get('parent')))\n"
        "keyed = {} keyed[box] = 'box' keyed[a] = 'a'\n"
        "assert(keyed[a:get('parent')] == 'box' and keyed[b:get('parent')] == 'box')\n"
        "weak = setmetatable({}, {__mode = 'v'})\n"
        "weak[1] = ui.label{text = 'nothing but the app holds this handle'}\n");
    lua_State *L = apps_vm_lua(e.vm);
    lua_gc(L, LUA_GCCOLLECT);
    RUN(&e, "assert(weak[1] == nil, 'the cache kept a handle alive')");
    CHECK(apps_ui_count(e.ui) == 4, "the widget outlives its handle: %d alive", apps_ui_count(e.ui));
    /* A handle let go of is made again, as the same widget, and then it is the one every call gives. */
    RUN(&e, "box = nil keyed = nil");
    lua_gc(L, LUA_GCCOLLECT);
    RUN(&e,
        "p, q = a:get('parent'), b:get('parent')\n"
        "assert(rawequal(p, q) and tostring(p) == 'box#0' and p:get('radius') == 0)\n"
        "assert(select('#', p:set{radius = 3}) == 1 and rawequal(a:get('parent'), p))");
    finish(&e);
}

/* The table that remembers handles holds one for each slot and looks at it (its gen and its kind) before it gives
 * it out. No Lua call leaves the table disagreeing with a widget, because every create stores the handle of what
 * it made, so the widget table is rewritten here as only the apply side's own code could (a slot on its next
 * life, a slot of another kind) to show that the look is made: the handle the app holds is not handed out for a
 * widget it doesn't name. */
static void identity_checks_the_life(void) {
    OPEN(e);
    RUN(&e, "a = ui.button{} a:on('tap', function(w) seen = w end)");
    probe_t p = PROBE(&e, 0, 0, APPS_UI_TAP);
    CHECK(p.result, "the tap");
    RUN(&e, "assert(rawequal(seen, a))");  /* the handle the app holds, while it names that widget */
    W(&e, 0)->gen = 1;
    p = PROBE(&e, 0, 1, APPS_UI_TAP);
    CHECK(p.result, "the tap on the next life");
    RUN(&e, "assert(not rawequal(seen, a) and tostring(seen) == 'button#0' and tostring(a) == 'button#0 (deleted)')");
    finish(&e);

    OPEN(f);
    RUN(&f, "a = ui.button{} a:on('tap', function(w) seen = w end)");
    W(&f, 0)->kind = APPS_W_LABEL;
    p = PROBE(&f, 0, 0, APPS_UI_TAP);
    CHECK(p.result, "the tap on a slot of another kind");
    RUN(&f, "assert(not rawequal(seen, a) and tostring(seen) == 'label#0' and tostring(a) == 'button#0 (deleted)')");
    finish(&f);
}

/* ---- No table ---- */

/* F7: the apply side may be asked about an app that isn't there: nothing is pending, nothing is changed. */
static void null_table(void) {
    bool changed = true;
    CHECK(apps_ui_take_bg(NULL, &changed) == 0 && !changed, "take_bg(NULL): changed %d", changed);
    CHECK(apps_ui_take_bg(NULL, NULL) == 0, "take_bg(NULL, NULL)");
    CHECK(!apps_ui_take_clear(NULL), "take_clear(NULL)");
    CHECK(apps_ui_slot(NULL, 0) == NULL && apps_ui_asset(NULL, 0) == NULL && apps_ui_count(NULL) == 0, "NULL table");
    char out[8] = "xxxxxxx";
    int dropped = 99;
    CHECK(apps_ui_dump(NULL, out, sizeof out, &dropped) == 0 && out[0] == '\0' && dropped == 0, "dump(NULL)");
    apps_ui_free(NULL);
    apps_ui_set_assets(NULL, NULL, 0);
}

/* ---- A create that raises ---- */

/* An app that is told a create failed has no handle to a widget made anyway, and the slot would be lost to it for
 * good. So nothing may be able to raise once the widget is in the table: every growth request of each kind of
 * create is refused in turn, and wherever one was refused the call has failed and no widget is left. (The table
 * that remembers handles is made with a place for each slot, so that storing a handle can't ask for memory.) */
static void create_all_or_nothing(void) {
    static const char *const makes[] = {
        "x = ui.label{text = 'hello', x = 3}", "x = ui.button{text = 'go'}", "x = ui.box{}",
        "x = ui.arc{value = 5}",               "x = ui.bar{}",               "x = ui.image{src = 'a.bin'}",
    };
    for (size_t i = 0; i < ARRAY_LEN(makes); i++) {
        long refusals = 0;
        for (long k = 1; k < 1000; k++) {
            env_t e;
            CHECK(env_open(&e, TWO_IMAGES, 2), "env_open");
            refuse_from = growths + k;
            char err[256] = "";
            bool ran = run(&e, makes[i], err, sizeof err);
            bool refused = growths >= refuse_from;
            refuse_from = -1;
            CHECK(refused ? !ran && apps_ui_count(e.ui) == 0 : ran && apps_ui_count(e.ui) == 1,
                  "'%s', request %ld: refused %d, ran %d, %d widgets alive (%s)", makes[i], k, refused, ran,
                  apps_ui_count(e.ui), err);
            CHECK(!lock_held && lock_errors == 0, "'%s', request %ld: the lock", makes[i], k);
            check_invariants(__LINE__, &e);
            finish(&e);
            if (!refused) {
                break;
            }
            refusals++;
        }
        CHECK(refusals >= 2, "'%s': only %ld requests to refuse", makes[i], refusals);
    }
}

/* ---- How deep boxes nest ---- */

/* Boxes nest at most APPS_UI_BOX_DEPTH_MAX deep, and a box counts itself: four boxes in a chain with a label in the fourth is
 * the most there may be, and a fifth box is refused with the rule in the message. A refusal changes nothing: no slot is
 * taken and no bit is flagged for the apply step. What isn't a box still goes in the fourth, and a box goes in the third.
 * check_invariants holds every table to the same rule after every chunk (a box has at most three boxes above it), so a
 * binding that let the fifth box in fails here twice: the refusal doesn't come, and the table has a fifth box. */
static void box_depth(void) {
    OPEN(e);
    RUN(&e, "b0 = ui.box{} b1 = ui.box{parent = b0} b2 = ui.box{parent = b1} b3 = ui.box{parent = b2} "
            "l = ui.label{parent = b3, text = 'in'}");
    CHECK(apps_ui_count(e.ui) == 5, "%d widgets alive", apps_ui_count(e.ui));
    pass_t seen;
    apply(&e, &seen);

    RUN_FAILS(&e, "ui.box{parent = b3}", "ui.box: boxes nest at most 4 deep");
    CHECK(apps_ui_count(e.ui) == 5, "the refused box took a slot: %d alive", apps_ui_count(e.ui));
    apply(&e, &seen);
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        CHECK(seen.dirty[i] == 0, "slot %d: bits %u flagged by a refused box", i, seen.dirty[i]);
    }

    RUN(&e, "ui.button{parent = b3, text = 'Go'} ui.arc{parent = b3} x = ui.box{parent = b2}");
    CHECK(apps_ui_count(e.ui) == 8, "%d widgets alive, want 8", apps_ui_count(e.ui));
    RUN_FAILS(&e, "ui.box{parent = x}", "ui.box: boxes nest at most 4 deep");  /* x is the fourth box of its chain, too */
    CHECK(apps_ui_count(e.ui) == 8, "the refused box took a slot: %d alive", apps_ui_count(e.ui));
    finish(&e);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"dirty_bits", dirty_bits},
        {"dirty_groups", dirty_groups},
        {"rotation_sets", rotation_sets},
        {"spin_refuses_an_unsized_src", spin_refuses_an_unsized_src},
        {"vm_check", vm_check},
        {"slot_reuse", slot_reuse},
        {"cascade", cascade},
        {"cascade_lower_slot", cascade_lower_slot},
        {"lock_taken", lock_taken},
        {"metatables", metatables},
        {"identity", identity},
        {"identity_checks_the_life", identity_checks_the_life},
        {"null_table", null_table},
        {"clear", clear},
        {"bg", bg},
        {"push_handler", push_handler},
        {"push_handler_reserves_its_slots", push_handler_reserves_its_slots},
        {"handlers_dropped", handlers_dropped},
        {"assets", assets},
        {"dump_resolved", dump_resolved},
        {"dump_caps", dump_caps},
        {"lock_discipline", lock_discipline},
        {"refused", refused},
        {"touch_refused", touch_refused},
        {"create_all_or_nothing", create_all_or_nothing},
        {"box_depth", box_depth},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            CHECK(held == 0, "%s ended with %zu bytes held", argv[1], held);
            return failures != 0 ? 1 : 0;
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
