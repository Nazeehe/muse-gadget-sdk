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
 *
 * The launcher's icons are the real apps_icons.c's. What it asks of the I/O worker and the store is faked here: a submit
 * queues the job and run_io runs what is queued, on this thread; the store is a few icon files in memory, read into the
 * counting heap as the real reader does.
 *
 * The runtime images' cases (connected apps Task 2) run the real runtime too (apps_rt.c, with apps_svc.c): what it asks of the
 * port beyond the heap and the lock is stubbed here, and the heap can watch a block for its free.
 */

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_ask.h"
#include "apps_icons.h"
#include "apps_io.h"
#include "apps_json.h"
#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_store.h"
#include "apps_ui.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"
#include "muse_apps_lv_fakes.h"
#include "sdkconfig.h"  /* the staged one: CONFIG_MUSE_BENCH in the bench build of this harness, nothing in the other */

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

/* Blocks a case watches (a runtime image's), and whether each has been freed since. */
#define WATCH_MAX 8
static const void *watch_ptr[WATCH_MAX];
static bool watch_gone[WATCH_MAX];
static int watch_n;
/* With recycle set, the next watched block freed is kept, and the next block of its size handed out is it again: a heap that gives a
 * freed block's address to the next request, which ASan's quarantine never does. */
static bool recycle;
static block_t *recycled;

void *apps_port_realloc(void *ptr, size_t n) {
    if (ptr == NULL && recycled != NULL && recycled->size == n) {
        block_t *again = recycled;
        recycled = NULL;
        held += n;
        return again + 1;
    }
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
        bool watched = false;
        for (int i = 0; i < watch_n; i++) {
            if (watch_ptr[i] == ptr && !watch_gone[i]) {
                watch_gone[i] = true;
                watched = true;
            }
        }
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        if (watched && recycle && recycled == NULL) {
            recycle = false;
            recycled = b;
            return;
        }
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

/* What the runtime and its services ask of the port (apps_rt.c and apps_svc.c, for the runtime images' cases): nothing any case here
 * uses, so each does the least it may. */
bool apps_port_dark(void) {
    return false;
}

apps_alarm_result_t apps_port_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    (void)app, (void)name, (void)ms, (void)sound, (void)wake;
    return APPS_ALARM_REFUSED;
}

void apps_port_alarm_cancel(const char *app, const char *name) {
    (void)app, (void)name;
}

int64_t apps_port_alarm_remaining(const char *app, const char *name) {
    (void)app, (void)name;
    return -1;
}

int apps_port_alarm_list(const char *app, char names[4][24], int64_t remaining[4]) {
    (void)app, (void)names, (void)remaining;
    return 0;
}

void apps_port_sound_chirp(void) {}

void apps_port_sound_stop(void) {}

char *apps_port_store_load(const char *app, size_t *len) {
    (void)app;
    *len = 0;
    return NULL;
}

bool apps_port_store_save(const char *app, const char *json, size_t len) {
    (void)app, (void)json, (void)len;
    return false;
}

apps_battery_t apps_port_battery(void) {
    return (apps_battery_t){.pct = -1};
}

bool apps_port_wifi(char *ssid, size_t cap) {
    if (cap > 0) {
        ssid[0] = '\0';
    }
    return false;
}

void apps_port_show(void) {}

void apps_port_keep_awake(bool on) {
    (void)on;
}

void apps_port_exit(void) {}

/* No VM here: the vm module's requests are refused (its callbacks say so; tests/muse_apps_vmio_harness.c is the module's). */
bool apps_port_vm_submit(struct apps_vm_job *job) {
    (void)job;
    return false;
}

/* No Muse here (tests/muse_apps_ask_harness.c is the muse module's): an ask's turn ends at once, as apphost's does. */
void apps_port_ask(const char *msg, size_t len, void *user) {
    (void)msg, (void)len;
    apps_ask_done(user, "not set up");
}

/* ---- The I/O worker and the store, for the launcher's icons ---- */

#define IO_JOBS 16
static void (*io_fn[IO_JOBS])(void *arg);
static void *io_arg[IO_JOBS];
static int io_queued;  /* submitted and not run */
static int io_submits; /* every submit, taken or not */

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    io_submits++;
    if (io_queued == IO_JOBS) {
        return false;
    }
    io_fn[io_queued] = fn;
    io_arg[io_queued] = arg;
    io_queued++;
    return true;
}

/* The worker's turn: every job queued, in order, those they queue too. */
static void run_io(void) {
    while (io_queued > 0) {
        void (*fn)(void *) = io_fn[0];
        void *arg = io_arg[0];
        memmove(&io_fn[0], &io_fn[1], (size_t)(io_queued - 1) * sizeof io_fn[0]);
        memmove(&io_arg[0], &io_arg[1], (size_t)(io_queued - 1) * sizeof io_arg[0]);
        io_queued--;
        fn(arg);
    }
}

apps_storage_t apps_store_mode(void) {
    return APPS_STORAGE_FLASH;
}

/* The icon files: an app's icon.bin, malloc's (not the apps' heap). */
typedef struct {
    char id[APPS_ID_MAX + 1];
    uint8_t *data;
    size_t len;
} icon_file_t;
static icon_file_t icon_files[16];
static int n_icon_files;
static int store_reads;

/* As apps_store.c's: the file into a block of the apps' heap, with a NUL after it, when it has at most max bytes. */
bool apps_store_read_file(const char *id, const char *path, size_t max, uint8_t **out, size_t *len, char *err, size_t n) {
    store_reads++;
    *out = NULL;
    *len = 0;
    const icon_file_t *f = NULL;
    for (int i = 0; i < n_icon_files && f == NULL; i++) {
        f = strcmp(icon_files[i].id, id) == 0 && strcmp(path, "icon.bin") == 0 ? &icon_files[i] : NULL;
    }
    if (f == NULL) {
        snprintf(err, n, "No such file or directory");
        return false;
    }
    if (f->len > max) {
        snprintf(err, n, "larger than %u KB", (unsigned)(max / 1024));
        return false;
    }
    uint8_t *block = apps_port_realloc(NULL, f->len + 1);
    if (block == NULL) {
        snprintf(err, n, "%s", APPS_STORE_MSG_NO_MEMORY);
        return false;
    }
    memcpy(block, f->data, f->len);
    block[f->len] = '\0';
    *out = block;
    *len = f->len;
    return true;
}

#define CF_A8 0x0E
#define CF_RGB565 0x12
#define CF_RGB565A8 0x14

/* id's icon.bin: an LVGL v9 image of w x h in cf (premultiplied when it has alpha), every byte after the header fill. */
static void put_icon(const char *id, unsigned cf, unsigned w, unsigned h, uint8_t fill) {
    unsigned stride = cf == CF_A8 ? w : w * 2;
    size_t n = 12 + (size_t)stride * h + (cf == CF_RGB565A8 ? (size_t)(stride / 2) * h : 0);
    uint8_t *b = malloc(n);
    if (b == NULL || n_icon_files == (int)ARRAY_LEN(icon_files)) {
        fputs("no room for an icon file in the harness\n", stderr);
        exit(2);
    }
    memset(b, fill, n);
    unsigned flags = cf == CF_RGB565A8 ? 1 : 0;
    const uint8_t header[12] = {0x19, (uint8_t)cf, (uint8_t)flags, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8),
                                (uint8_t)stride, (uint8_t)(stride >> 8), 0, 0};
    memcpy(b, header, sizeof header);
    icon_file_t *f = &icon_files[n_icon_files++];
    snprintf(f->id, sizeof f->id, "%s", id);
    f->data = b;
    f->len = n;
}

static void free_icon_files(void) {
    for (int i = 0; i < n_icon_files; i++) {
        free(icon_files[i].data);
    }
    n_icon_files = 0;
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
    lv_label_long_mode_t long_mode;  /* a label's: WRAP until set, as LVGL's */
    const lv_image_dsc_t *src;
    uint32_t scale;
    lv_image_align_t inner_align;  /* where an image sits in its box: CENTER until lv_image_set_inner_align says otherwise */
    int32_t rotation;      /* an image's angle in 0.1 degrees, 0..3599 (lv_image_set_rotation): 0 until set, as LVGL's */
    int turns;             /* times the angle changed */
    long turned_ms;        /* anim_clock_ms when it last changed */
    long turn_gap_ms;      /* the shortest time between two changes of it, by the animation timer's clock; -1 until two */
    lv_area_t area;
    bool forced;           /* the test set the area: the layout leaves it */
    int32_t pad_left, pad_right;  /* where its children's content area starts and ends (0 until set) */
    int32_t scroll_x;      /* how far its content is scrolled left (lv_obj_get_scroll_x): a test moves it */
    lv_dir_t scroll_dir;   /* LV_DIR_ALL until set, as LVGL's */
    lv_scroll_snap_t snap_x;
    lv_scrollbar_mode_t scrollbar_mode;  /* LV_SCROLLBAR_MODE_AUTO until set, as LVGL's */
    fake_cb_t cbs[8];
    int n_cbs;
    int order;             /* where it is among its parent's children (lv_obj_get_index): made last, unless moved */
    int32_t ext_draw;      /* the room past its box it draws in and is invalidated in (LV_EVENT_REFR_EXT_DRAW_SIZE): 0 until refreshed */
    lv_area_t inv;         /* what it invalidated since fake_inv_clear, as one box (empty: x2 < x1), on the screen */
    int32_t inv_widest;    /* the widest area of those, and the tallest */
    int32_t inv_tallest;
};

struct lv_indev {
    int unused;
};

/* The one display. Its callbacks outlive the objects (fake_reset leaves them), as a display outlives a screen's. */
struct lv_display {
    fake_cb_t cbs[4];
    int n_cbs;
};

struct lv_event {
    lv_event_code_t code;
    void *ud;
    lv_obj_t *target;
    bool stopped;
    void *param;           /* lv_event_get_param: LV_EVENT_REFR_EXT_DRAW_SIZE's int32_t */
};

#define FAKE_OBJS_MAX 4096
static lv_obj_t *objs[FAKE_OBJS_MAX];
static int n_objs;
static int next_id;
static int next_order;
static lv_obj_t screen_obj;
static lv_display_t display_obj;
static lv_indev_t indev_obj;
static bool indev_present = true;
static lv_dir_t gesture_dir = LV_DIR_NONE;
static lv_obj_t *pressed_obj;  /* the object the finger went down on (LVGL's act_obj), NULL when it is up */
static bool wait_released;     /* lv_indev_wait_release was called during this touch */
static bool long_sent;         /* LONG_PRESSED was sent during this touch */

/* The rest of the pointer, as LVGL 9.5.0's indev keeps it (lv_indev_private.h), for a touch at a point (fake_down, fake_move,
 * fake_up). The fake has one read of history where LVGL keeps a few: a throw is the last read's movement. */
static lv_point_t act_point, last_point, vect;  /* where the finger is, where it was at the last read, and the difference */
static lv_obj_t *scroll_obj;    /* what this touch's drag scrolls; after the lift, until its throw has taken its first step */
static lv_dir_t scroll_dir_now; /* pointer.scroll_dir: LV_DIR_HOR or LV_DIR_VER once a drag scrolls */
static lv_point_t scroll_sum, gesture_sum;
static bool gesture_sent;
static bool throw_pending;      /* indev->scroll_throw_anim: a drag's lift started it, and it hasn't run yet */
static int32_t throw_vect_x;    /* pointer.scroll_throw_vect_ori.x at the lift */
static lv_obj_t *active_obj;    /* indev_obj_act: the pressed object while a read is processed, NULL otherwise */

/* LVGL 9.5.0's numbers for all that (test_the_fakes_pointer_is_lvgl_9_5_0s reads them in its tree). */
#define FAKE_SCROLL_LIMIT 10          /* LV_INDEV_DEF_SCROLL_LIMIT: a drag scrolls once it has gone this far */
#define FAKE_SCROLL_THROW 10          /* LV_INDEV_DEF_SCROLL_THROW: a throw loses this % a step */
#define FAKE_GESTURE_LIMIT 50         /* LV_INDEV_DEF_GESTURE_LIMIT: a gesture is this far */
#define FAKE_GESTURE_MIN_VELOCITY 3   /* LV_INDEV_DEF_GESTURE_MIN_VELOCITY: a read that moves less starts the gesture again */
#define FAKE_ELASTIC_SLOWNESS 4       /* ELASTIC_SLOWNESS_FACTOR: a scroll past the last snap point goes this much slower */
#define FAKE_SCROLL_ANIM_MIN_MS 200   /* SCROLL_ANIM_TIME_MIN */
#define FAKE_SCROLL_ANIM_MAX_MS 400   /* SCROLL_ANIM_TIME_MAX */
#define FAKE_HOR_RES 466              /* the display: lv_obj_scroll_by animates at half its width a second */
#define FAKE_COORD_MIN (-LV_COORD_MAX)

/* A scroll animation (lv_obj_scroll_by with LV_ANIM_ON: scroll_x_anim), in LVGL's scroll.x, which is -lv_obj_get_scroll_x. */
typedef struct {
    lv_obj_t *obj;
    int32_t from, to, now;
    int32_t duration, elapsed;  /* ms */
    bool started;               /* its first step has run (lv_anim_t.start_cb_called): its end sends SCROLL_END */
} fake_anim_t;
static fake_anim_t anims[8];
static int n_anims;

/* The animations apps_lv.c starts (lv_anim_start): LVGL's list, newest first as lv_ll_ins_head puts them, each a copy of what it was
 * given, at an address of its own until it is deleted (freed: a pointer kept past that is an AddressSanitizer report). */
#define FAKE_LV_ANIMS_MAX 160
static lv_anim_t *lv_anims[FAKE_LV_ANIMS_MAX];
static int n_lv_anims;
static bool anim_list_changed;  /* state.anim_list_changed: the list changed while the timer walked it */
static bool anim_run_round;     /* state.anim_run_round, flipped at each run of the timer */
static long anim_clock_ms;      /* lv_tick_get() as the animation timer sees it: fake_frame moves it */
static void lv_anims_of_deleted(const lv_obj_t *o);

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
        case LV_OBJ_FLAG_SNAPPABLE: return "snappable";
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
        case LV_EVENT_SCROLL: return "scroll";
        case LV_EVENT_GESTURE: return "gesture";
        case LV_EVENT_REFR_EXT_DRAW_SIZE: return "refr_ext_draw_size";
        case LV_EVENT_REFR_READY: return "refr_ready";
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
    o->turn_gap_ms = -1;
    o->area = (lv_area_t){0, 0, -1, -1};
    o->inv = (lv_area_t){0, 0, -1, -1};
    o->scroll_dir = LV_DIR_ALL;
    o->scrollbar_mode = LV_SCROLLBAR_MODE_AUTO;
    /* The flags LVGL 9.5.0's constructors leave: lv_obj is clickable, scrollable and snappable (every class keeps
     * the last), a label and an image are not clickable, and a button, an arc and a bar are not scrollable. */
    o->flags = LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE;
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
    /* LVGL's obj_delete_core: its animations go with it (lv_obj_destructor's lv_anim_delete(obj, NULL), each deleted callback
     * called), and an input device that presses or scrolls it is reset (obj_indev_reset): a finger that is down waits for its
     * release, and nothing more of the touch is sent. */
    for (int i = 0; i < n_anims; i++) {
        if (anims[i].obj == o) {
            anims[i--] = anims[--n_anims];
        }
    }
    lv_anims_of_deleted(o);
    if (o == pressed_obj || o == scroll_obj) {
        if (pressed_obj != NULL) {
            wait_released = true;
        }
        pressed_obj = scroll_obj = NULL;
        throw_pending = false;
        scroll_dir_now = LV_DIR_NONE;
    }
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

static int32_t ext_of(lv_obj_t *o);

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

/* As LVGL 9.5.0's lv_obj_refr_pos and lv_obj_move_to: an object is placed in its parent's content area (the parent's
 * box less its left and right padding), moved left by how far the parent is scrolled. */
static void layout_one(lv_obj_t *o) {
    if (o->forced) {
        return;
    }
    const lv_obj_t *parent = o->parent;
    const lv_area_t *pa = &parent->area;
    int32_t pw = pa->x2 - pa->x1 + 1 - parent->pad_left - parent->pad_right, ph = pa->y2 - pa->y1 + 1, cw, ch;
    content_size(o, &cw, &ch);
    int32_t w = px_of(o->w, pw, cw), h = px_of(o->h, ph, ch);
    int32_t x1 = pa->x1 + parent->pad_left - parent->scroll_x, y1 = pa->y1;
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
    bool resized = o->area.x2 - o->area.x1 + 1 != w || o->area.y2 - o->area.y1 + 1 != h;
    o->area = (lv_area_t){x1, y1, x1 + w - 1, y1 + h - 1};
    if (resized && strcmp(o->cls, "image") == 0) {
        o->ext_draw = ext_of(o);  /* lv_obj_refr_size sends for the room again whenever the size changes */
    }
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

/* ---- Scrolling: what is set is kept and logged; a test moves the scroll (fake_scroll) ---- */

void lv_obj_set_scrollbar_mode(lv_obj_t *obj, lv_scrollbar_mode_t mode) {
    if (use(obj, "lv_obj_set_scrollbar_mode", NULL) == NULL) {
        return;
    }
    static const char *const names[] = {"off", "on", "active", "auto"};
    if ((unsigned)mode >= ARRAY_LEN(names)) {
        fake_error("lv_obj_set_scrollbar_mode(%d, %d)", obj->id, (int)mode);
        return;
    }
    obj->scrollbar_mode = mode;
    logline("scrollbar_mode %d %s", obj->id, names[mode]);
}

void lv_obj_set_scroll_dir(lv_obj_t *obj, lv_dir_t dir) {
    if (use(obj, "lv_obj_set_scroll_dir", NULL) == NULL) {
        return;
    }
    if ((dir & ~LV_DIR_ALL) != 0) {
        fake_error("lv_obj_set_scroll_dir(%d, 0x%x)", obj->id, (unsigned)dir);
        return;
    }
    obj->scroll_dir = dir;
    logline("scroll_dir %d %s", obj->id, dir == LV_DIR_HOR ? "hor" : dir == LV_DIR_VER ? "ver" : dir == LV_DIR_ALL ? "all" : "other");
}

void lv_obj_set_scroll_snap_x(lv_obj_t *obj, lv_scroll_snap_t align) {
    if (use(obj, "lv_obj_set_scroll_snap_x", NULL) == NULL) {
        return;
    }
    static const char *const names[] = {"none", "start", "end", "center"};
    if ((unsigned)align >= ARRAY_LEN(names)) {
        fake_error("lv_obj_set_scroll_snap_x(%d, %d)", obj->id, (int)align);
        return;
    }
    obj->snap_x = align;
    logline("scroll_snap_x %d %s", obj->id, names[align]);
}

int32_t lv_obj_get_scroll_x(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_get_scroll_x", NULL) == NULL) {
        return 0;
    }
    return obj->scroll_x;
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

void lv_obj_set_style_pad_left(lv_obj_t *obj, int32_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_pad_left", NULL) != NULL) {
        if (selector == LV_PART_MAIN) {
            obj->pad_left = value;
        }
        style_log(obj, "pad_left", selector, "%d", (int)value);
    }
}

void lv_obj_set_style_pad_right(lv_obj_t *obj, int32_t value, lv_style_selector_t selector) {
    if (use(obj, "lv_obj_set_style_pad_right", NULL) != NULL) {
        if (selector == LV_PART_MAIN) {
            obj->pad_right = value;
        }
        style_log(obj, "pad_right", selector, "%d", (int)value);
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
    const char *name = long_mode == LV_LABEL_LONG_MODE_WRAP   ? "wrap"
                       : long_mode == LV_LABEL_LONG_MODE_CLIP ? "clip"
                       : long_mode == LV_LABEL_LONG_MODE_DOTS ? "dots"
                                                              : "other";
    obj->long_mode = long_mode;
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

static void invalidate_turned(lv_obj_t *o);

void lv_image_set_src(lv_obj_t *obj, const void *src) {
    if (use(obj, "lv_image_set_src", "image") == NULL) {
        return;
    }
    const lv_image_dsc_t *dsc = src;
    if (src == NULL) {
        /* LVGL 9.5.0 takes NULL for an unknown source: reset_image_attributes forgets the image, and nothing is drawn. The
         * launcher does that to an icon it takes down; an app's image always has its asset, so from the apply step it is a
         * mistake. */
        if (apply_active) {
            fake_error("lv_image_set_src(%d, NULL) in the apply step: an app's image always has a source", obj->id);
        }
        obj->src = NULL;
        logline("image_src %d none", obj->id);
        return;
    }
    if (dsc->header.magic != 0x19 || dsc->data == NULL || dsc->header.w == 0 || dsc->header.h == 0 ||
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
    logline("image_scale %d %u", obj->id, (unsigned)zoom);
    if (zoom == obj->scale) {
        return;  /* LVGL returns early for the same scale */
    }
    /* scale_update (lv_image.c): as lv_image_set_rotation does for an angle. */
    invalidate_turned(obj);
    obj->scale = zoom;
    obj->ext_draw = ext_of(obj);
    invalidate_turned(obj);
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

/* ---- An image's room for its turn, and what it invalidates (connected apps Task 7) ---- */

static void fake_layout(void);

/* LVGL 9.5.0's lv_image_buf_get_transformed_area (lv_draw_image.c), relative to the box: the box (the object's size, w x h)
 * turned by angle (0.1 degrees) and scaled (256 = 1) about its pivot, LV_PCT(50) of each side (lv_image_constructor), the
 * scale first (lv_point_transform's zoom_first); the corners' extremes, the right and bottom less 1. LVGL turns in integer
 * steps of its sine table, the fake in doubles: within a px of it. */
static lv_area_t turned_area(const lv_obj_t *o, int32_t angle, uint32_t scale) {
    int32_t w = o->area.x2 - o->area.x1 + 1, h = o->area.y2 - o->area.y1 + 1;
    if (angle == 0 && scale == 256) {
        return (lv_area_t){0, 0, w - 1, h - 1};
    }
    double c = cos(angle * M_PI / 1800.0), sn = sin(angle * M_PI / 1800.0), k = scale / 256.0;
    double px = w * 50 / 100, py = h * 50 / 100, x_lo = 1e9, x_hi = -1e9, y_lo = 1e9, y_hi = -1e9;
    const double corner[4][2] = {{0, 0}, {w, 0}, {0, h}, {w, h}};
    for (int i = 0; i < 4; i++) {
        double x = (corner[i][0] - px) * k, y = (corner[i][1] - py) * k;
        double xr = c * x - sn * y + px, yr = sn * x + c * y + py;
        x_lo = xr < x_lo ? xr : x_lo;
        x_hi = xr > x_hi ? xr : x_hi;
        y_lo = yr < y_lo ? yr : y_lo;
        y_hi = yr > y_hi ? yr : y_hi;
    }
    return (lv_area_t){(int32_t)floor(x_lo), (int32_t)floor(y_lo), (int32_t)floor(x_hi) - 1, (int32_t)floor(y_hi) - 1};
}

/* LV_EVENT_REFR_EXT_DRAW_SIZE as LVGL 9.5.0 sends it (lv_obj_refresh_ext_draw_size, lv_obj_draw.c): from 0, the class's
 * handler first (event_send_core: lv_obj_event_base before the object's own callbacks) and lv_image's raises it to the
 * turned box's overhang when the image is turned or scaled (lv_image_event), then the object's callbacks. */
static int32_t ext_of(lv_obj_t *o) {
    int32_t s = 0;
    if (strcmp(o->cls, "image") == 0 && (o->rotation != 0 || o->scale != 256)) {
        lv_area_t a = turned_area(o, o->rotation, o->scale);
        int32_t w = o->area.x2 - o->area.x1 + 1, h = o->area.y2 - o->area.y1 + 1;
        s = -a.x1 > s ? -a.x1 : s;
        s = -a.y1 > s ? -a.y1 : s;
        s = a.x2 - w > s ? a.x2 - w : s;
        s = a.y2 - h > s ? a.y2 - h : s;
    }
    lv_event_t e = {LV_EVENT_REFR_EXT_DRAW_SIZE, NULL, o, false, &s};
    for (int i = 0; i < o->n_cbs && !e.stopped; i++) {
        lv_event_code_t f = (lv_event_code_t)(o->cbs[i].filter & ~LV_EVENT_PREPROCESS);
        if (f == LV_EVENT_ALL || f == LV_EVENT_REFR_EXT_DRAW_SIZE) {
            e.ud = o->cbs[i].ud;
            o->cbs[i].cb(&e);
        }
    }
    return s;
}

/* An area of o invalidated (lv_obj_invalidate_area): nothing if o is hidden; else cut to its box grown by its room
 * (lv_obj_area_is_visible) and to the screen (lv_inv_area), and added to what o has invalidated. */
static void fake_invalidate(lv_obj_t *o, lv_area_t a) {
    if ((o->flags & LV_OBJ_FLAG_HIDDEN) != 0) {
        return;
    }
    lv_area_t box = {o->area.x1 - o->ext_draw, o->area.y1 - o->ext_draw, o->area.x2 + o->ext_draw, o->area.y2 + o->ext_draw};
    a.x1 = a.x1 > box.x1 ? a.x1 : box.x1;
    a.y1 = a.y1 > box.y1 ? a.y1 : box.y1;
    a.x2 = a.x2 < box.x2 ? a.x2 : box.x2;
    a.y2 = a.y2 < box.y2 ? a.y2 : box.y2;
    a.x1 = a.x1 > 0 ? a.x1 : 0;
    a.y1 = a.y1 > 0 ? a.y1 : 0;
    a.x2 = a.x2 < 465 ? a.x2 : 465;
    a.y2 = a.y2 < 465 ? a.y2 : 465;
    if (a.x2 < a.x1 || a.y2 < a.y1) {
        return;
    }
    if (o->inv.x2 < o->inv.x1) {
        o->inv = a;
    } else {
        o->inv.x1 = a.x1 < o->inv.x1 ? a.x1 : o->inv.x1;
        o->inv.y1 = a.y1 < o->inv.y1 ? a.y1 : o->inv.y1;
        o->inv.x2 = a.x2 > o->inv.x2 ? a.x2 : o->inv.x2;
        o->inv.y2 = a.y2 > o->inv.y2 ? a.y2 : o->inv.y2;
    }
    o->inv_widest = a.x2 - a.x1 + 1 > o->inv_widest ? a.x2 - a.x1 + 1 : o->inv_widest;
    o->inv_tallest = a.y2 - a.y1 + 1 > o->inv_tallest ? a.y2 - a.y1 + 1 : o->inv_tallest;
}

static void fake_inv_clear(lv_obj_t *o) {
    o->inv = (lv_area_t){0, 0, -1, -1};
    o->inv_widest = o->inv_tallest = 0;
}

/* An image's turned area on the screen, invalidated: lv_image_set_rotation and lv_image_set_scale do it for the old angle
 * and scale and again for the new (lv_image.c). */
static void invalidate_turned(lv_obj_t *o) {
    lv_area_t a = turned_area(o, o->rotation, o->scale);
    fake_invalidate(o, (lv_area_t){a.x1 + o->area.x1, a.y1 + o->area.y1, a.x2 + o->area.x1, a.y2 + o->area.y1});
}

void *lv_event_get_param(lv_event_t *e) {
    return e->param;
}

lv_obj_t *lv_event_get_target_obj(lv_event_t *e) {
    return e->target;
}

int32_t lv_obj_get_width(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_get_width", NULL) == NULL) {
        return 0;
    }
    return obj->area.x2 - obj->area.x1 + 1;
}

int32_t lv_obj_get_height(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_get_height", NULL) == NULL) {
        return 0;
    }
    return obj->area.y2 - obj->area.y1 + 1;
}

/* LVGL 9.5.0's (lv_obj_draw.c): the room sent for again, kept, and the object invalidated with it when it changed. */
void lv_obj_refresh_ext_draw_size(lv_obj_t *obj) {
    if (use(obj, "lv_obj_refresh_ext_draw_size", NULL) == NULL) {
        return;
    }
    logline("refresh_ext_draw_size %d", obj->id);
    int32_t s = ext_of(obj);
    bool changed = s != obj->ext_draw;
    obj->ext_draw = s;
    if (changed) {
        fake_invalidate(obj, obj->area);
    }
}

/* LVGL 9.5.0's lv_image_set_rotation (lv_image.c): 0.1 degrees, brought into 0..3599, about the pivot (the image's centre, LVGL's
 * default: apps_lv.c never moves it); the same angle again does nothing. An image aligned to transform itself (stretch, fit ...)
 * would take 0, but the fake knows only default and center. Every call is logged, with the angle as given. */
void lv_image_set_rotation(lv_obj_t *obj, int32_t angle) {
    if (use(obj, "lv_image_set_rotation", "image") == NULL) {
        return;
    }
    logline("image_rotation %d %d", obj->id, (int)angle);
    while (angle >= 3600) {
        angle -= 3600;
    }
    while (angle < 0) {
        angle += 3600;
    }
    if (angle == obj->rotation) {
        return;
    }
    if (obj->turns > 0) {
        long gap = anim_clock_ms - obj->turned_ms;
        obj->turn_gap_ms = obj->turn_gap_ms < 0 || gap < obj->turn_gap_ms ? gap : obj->turn_gap_ms;
    }
    /* LVGL: the layout brought up to date, the old turned area invalidated, the angle set, the room sent for again with
     * invalidation off, and the new turned area invalidated. */
    fake_layout();
    invalidate_turned(obj);
    obj->rotation = angle;
    obj->ext_draw = ext_of(obj);
    invalidate_turned(obj);
    obj->turns++;
    obj->turned_ms = anim_clock_ms;
}

int32_t lv_image_get_rotation(lv_obj_t *obj) {
    if (use(obj, "lv_image_get_rotation", "image") == NULL) {
        return 0;
    }
    return obj->rotation;
}

/* ---- Animations, as LVGL 9.5.0's lv_anim.c runs them, for what apps_lv.c starts (test_the_fakes_animations_are_lvgl_9_5_0s reads
 * the lines this follows in its tree). What the fake doesn't model (a delay, a reverse play, a speed for a time, the start, completed
 * and get-value callbacks, a custom exec callback, pausing) is an error if apps_lv.c ever asks for it. ---- */

#define FAKE_ANIM_SPEED_MASK 0x80000000u  /* LV_ANIM_SPEED_MASK: a time with this bit is a speed */

/* An LVGL call that isn't on an object: counted, and inside the apply step only with the UI lock. */
static void fake_call(const char *fn) {
    tick();
    if (apply_active && !lock_held) {
        fake_error("%s was called inside the apply step without the UI lock", fn);
    }
}

/* The id of the object an animation's var is, or 0 when it isn't one of the fake's. */
static int anim_var_id(const void *var) {
    for (int i = 0; i < n_objs; i++) {
        if (objs[i] == var) {
            return objs[i]->id;
        }
    }
    return 0;
}

/* lv_anim_path_linear: lv_map(act_time, 0, duration, 0, LV_ANIM_RESOLUTION), then the share of the way from start to end. */
static int32_t fake_path_linear(const lv_anim_t *a) {
    int32_t t = a->act_time <= 0 ? 0 : a->act_time >= a->duration ? a->duration : a->act_time;
    int32_t step = a->duration > 0 ? (int32_t)((int64_t)t * 1024 / a->duration) : 1024;
    return ((step * (a->end_value - a->start_value)) >> 10) + a->start_value;
}

void lv_anim_init(lv_anim_t *a) {
    fake_call("lv_anim_init");
    memset(a, 0, sizeof *a);
    a->duration = 500;
    a->start_value = 0;
    a->end_value = 100;
    a->repeat_cnt = 1;
    a->path_cb = fake_path_linear;
    a->early_apply = 1;
}

void lv_anim_set_var(lv_anim_t *a, void *var) {
    fake_call("lv_anim_set_var");
    a->var = var;
}

void lv_anim_set_exec_cb(lv_anim_t *a, lv_anim_exec_xcb_t exec_cb) {
    fake_call("lv_anim_set_exec_cb");
    a->exec_cb = exec_cb;
}

void lv_anim_set_values(lv_anim_t *a, int32_t start, int32_t end) {
    fake_call("lv_anim_set_values");
    a->start_value = start;
    a->current_value = INT32_MIN;
    a->end_value = end;
}

void lv_anim_set_duration(lv_anim_t *a, uint32_t duration) {
    fake_call("lv_anim_set_duration");
    a->duration = (int32_t)duration;
}

void lv_anim_set_repeat_count(lv_anim_t *a, uint32_t cnt) {
    fake_call("lv_anim_set_repeat_count");
    a->repeat_cnt = cnt;
}

void lv_anim_set_path_cb(lv_anim_t *a, lv_anim_path_cb_t path_cb) {
    fake_call("lv_anim_set_path_cb");
    a->path_cb = path_cb;
}

void lv_anim_set_deleted_cb(lv_anim_t *a, lv_anim_deleted_cb_t deleted_cb) {
    fake_call("lv_anim_set_deleted_cb");
    a->deleted_cb = deleted_cb;
}

/* remove_anim: out of the list, the deleted callback, freed. */
static void remove_lv_anim(int i) {
    lv_anim_t *a = lv_anims[i];
    memmove(&lv_anims[i], &lv_anims[i + 1], (size_t)(n_lv_anims - i - 1) * sizeof lv_anims[0]);
    n_lv_anims--;
    anim_list_changed = true;
    if (a->deleted_cb != NULL) {
        a->deleted_cb(a);
    }
    free(a);
}

/* remove_concurrent_anims: the others of the same var and exec callback that run (or applied their start) go. */
static void remove_concurrent_lv_anims(const lv_anim_t *current) {
    if (current->exec_cb == NULL) {
        return;
    }
    for (int i = 0; i < n_lv_anims;) {
        lv_anim_t *a = lv_anims[i];
        if (a != current && (a->act_time >= 0 || a->early_apply) && a->var == current->var && a->exec_cb == current->exec_cb) {
            remove_lv_anim(i);
            i = 0;  /* from the head again, as LVGL does after a delete */
        } else {
            i++;
        }
    }
}

lv_anim_t *lv_anim_start(const lv_anim_t *a) {
    fake_call("lv_anim_start");
    int id = anim_var_id(a->var);
    if (id != 0 && ((const lv_obj_t *)a->var)->deleted) {
        fake_error("lv_anim_start on the deleted object %d", id);
        return NULL;
    }
    if (a->custom_exec_cb != NULL || a->start_cb != NULL || a->completed_cb != NULL || a->get_value_cb != NULL ||
        a->reverse_duration != 0 || a->repeat_delay != 0 || a->act_time != 0 || a->duration <= 0 ||
        ((uint32_t)a->duration & FAKE_ANIM_SPEED_MASK) != 0 || !a->early_apply || a->path_cb == NULL) {
        fake_error("lv_anim_start(%d) with what the fake doesn't model", id);
        return NULL;
    }
    if (n_lv_anims >= FAKE_LV_ANIMS_MAX) {
        fake_error("more animations than the fake holds");
        return NULL;
    }
    if (a->early_apply && a->exec_cb != NULL) {
        remove_concurrent_lv_anims(a);
    }
    lv_anim_t *copy = malloc(sizeof *copy);
    if (copy == NULL) {
        exit(2);
    }
    *copy = *a;
    if (a->var == a) {
        copy->var = copy;
    }
    copy->run_round = anim_run_round;
    copy->last_timer_run = (uint32_t)anim_clock_ms;
    copy->is_paused = 0;
    memmove(&lv_anims[1], &lv_anims[0], (size_t)n_lv_anims * sizeof lv_anims[0]);
    lv_anims[0] = copy;
    n_lv_anims++;
    char repeat[16];
    if (copy->repeat_cnt == LV_ANIM_REPEAT_INFINITE) {
        snprintf(repeat, sizeof repeat, "inf");
    } else {
        snprintf(repeat, sizeof repeat, "%u", (unsigned)copy->repeat_cnt);
    }
    logline("anim_start %d %d %d %d ms repeat=%s", id, (int)copy->start_value, (int)copy->end_value, (int)copy->duration, repeat);
    if (copy->early_apply) {  /* the start value at once: the path at time 0, and the exec callback with it */
        copy->current_value = copy->path_cb(copy);
        if (copy->exec_cb != NULL) {
            copy->exec_cb(copy->var, copy->current_value);
        }
    }
    anim_list_changed = true;
    return copy;
}

bool lv_anim_delete(void *var, lv_anim_exec_xcb_t exec_cb) {
    fake_call("lv_anim_delete");
    bool any = false;
    for (int i = 0; i < n_lv_anims;) {
        lv_anim_t *a = lv_anims[i];
        if ((a->var == var || var == NULL) && (a->exec_cb == exec_cb || exec_cb == NULL)) {
            remove_lv_anim(i);
            any = true;
            i = 0;
        } else {
            i++;
        }
    }
    logline("anim_delete %d%s", anim_var_id(var), any ? "" : " none");
    return any;
}

/* lv_obj_destructor's lv_anim_delete(obj, NULL), for an object the fake deletes: no call of apps_lv.c's, so not logged. */
static void lv_anims_of_deleted(const lv_obj_t *o) {
    for (int i = 0; i < n_lv_anims;) {
        if (lv_anims[i]->var == o) {
            remove_lv_anim(i);
            i = 0;
        } else {
            i++;
        }
    }
}

/* anim_completed_handler, for an animation with no reverse play: one repeat less (none for an infinite one); with none left it goes,
 * else it starts again with its overtime kept. */
static void lv_anim_completed(lv_anim_t *a) {
    if (a->repeat_cnt > 0 && a->repeat_cnt != LV_ANIM_REPEAT_INFINITE) {
        a->repeat_cnt--;
    }
    if (a->repeat_cnt == 0) {
        for (int i = 0; i < n_lv_anims; i++) {
            if (lv_anims[i] == a) {
                remove_lv_anim(i);
                return;
            }
        }
        return;
    }
    int32_t over_time = a->act_time > a->duration ? a->act_time - a->duration : 0;
    a->start_cb_called = 0;
    a->act_time = over_time - (int32_t)a->repeat_delay;
}

/* anim_timer, one run ms after the last: each animation's time moves on; at its first step after a start or a repeat the concurrent
 * ones go; its time, held to its duration, gives the path's value; the exec callback hears it only when it changed; at its end it
 * repeats or goes. A change of the list (a delete in a callback) sends the walk back to the head, and the run round keeps an
 * animation from being stepped twice. */
static void lv_anim_timer(int ms) {
    anim_clock_ms += ms;
    anim_run_round = !anim_run_round;
    for (int i = 0; i < n_lv_anims;) {
        lv_anim_t *a = lv_anims[i];
        uint32_t elaps = (uint32_t)anim_clock_ms - a->last_timer_run;
        a->act_time += (int32_t)elaps;
        a->last_timer_run = (uint32_t)anim_clock_ms;
        anim_list_changed = false;
        if (a->run_round != anim_run_round) {
            a->run_round = anim_run_round;
            if (!a->start_cb_called && a->act_time >= 0) {
                a->start_cb_called = 1;
                remove_concurrent_lv_anims(a);
            }
            if (a->act_time >= 0) {
                int32_t act_time_original = a->act_time;
                if (a->act_time > a->duration) {
                    a->act_time = a->duration;
                }
                int32_t act_time_before_exec = a->act_time;
                int32_t new_value = a->path_cb(a);
                if (new_value != a->current_value) {
                    a->current_value = new_value;
                    if (a->exec_cb != NULL) {
                        a->exec_cb(a->var, new_value);
                    }
                }
                if (!anim_list_changed) {
                    if (a->act_time == act_time_before_exec && act_time_original < a->duration * 2) {
                        a->act_time = act_time_original;
                    }
                    if (a->act_time >= a->duration) {
                        lv_anim_completed(a);
                    }
                }
            }
        }
        i = anim_list_changed ? 0 : i + 1;
    }
}

/* The animations running now (lv_anim_count_running), and the one of the object id, or NULL. */
static int fake_spins(void) {
    return n_lv_anims;
}

static const lv_anim_t *fake_anim_of(int id) {
    for (int i = 0; i < n_lv_anims; i++) {
        if (anim_var_id(lv_anims[i]->var) == id) {
            return lv_anims[i];
        }
    }
    return NULL;
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

/* Every object is on the one display (the screen's). */
lv_display_t *lv_obj_get_display(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_get_display", NULL) == NULL) {
        return NULL;
    }
    return &display_obj;
}

void lv_display_add_event_cb(lv_display_t *disp, lv_event_cb_t event_cb, lv_event_code_t filter, void *user_data) {
    tick();
    if (disp != &display_obj || event_cb == NULL || disp->n_cbs >= (int)ARRAY_LEN(disp->cbs)) {
        fake_error("lv_display_add_event_cb on %p with cb %p, %d callbacks", (void *)disp, (void *)(uintptr_t)event_cb,
                   disp == &display_obj ? disp->n_cbs : -1);
        return;
    }
    disp->cbs[disp->n_cbs++] = (fake_cb_t){event_cb, filter, user_data};
    logline("display_add_event_cb %s", code_name(filter));
}

/* One refresh of the display as lv_display_refr_timer ends it (lv_refr.c): LV_EVENT_REFR_READY to the display's callbacks, sent
 * whether or not anything was drawn. LVGL runs it at most every LV_DEF_REFR_PERIOD while something is invalid; a case says when. */
static void fake_refresh(void) {
    lv_event_t e = {LV_EVENT_REFR_READY, NULL, NULL, false, NULL};
    for (int i = 0; i < display_obj.n_cbs && !e.stopped; i++) {
        if (display_obj.cbs[i].filter == LV_EVENT_ALL || display_obj.cbs[i].filter == LV_EVENT_REFR_READY) {
            e.ud = display_obj.cbs[i].ud;
            display_obj.cbs[i].cb(&e);
        }
    }
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

/* LVGL's: (0, 0) for no device. */
void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *point) {
    if (point == NULL) {
        fake_error("lv_indev_get_point with no point");
        return;
    }
    *point = indev != NULL ? act_point : (lv_point_t){0, 0};
}

/* The object pressed, while a read is processed (LVGL clears it at the end of lv_indev_read): NULL for an event sent from code. */
lv_obj_t *lv_indev_get_active_obj(void) {
    return active_obj;
}

/* ---- Scrolling as LVGL 9.5.0 does it (lv_obj_scroll.c, lv_indev_scroll.c), for what apps_lv makes: a row that snaps its children to
 * its centre. What the fake doesn't model (a vertical scroll, a throw without snapping, the other snap modes) is an error. ---- */

static void fake_layout(void);
static void fake_fire(int id, lv_event_code_t code);

static bool anim_on(const lv_obj_t *o) {
    for (int i = 0; i < n_anims; i++) {
        if (anims[i].obj == o) {
            return true;
        }
    }
    return false;
}

/* lv_obj_scroll_by_raw: the content moves x to the right (scroll.x += x, so lv_obj_get_scroll_x falls by x), and SCROLL. */
static void scroll_by_raw(lv_obj_t *o, int32_t x, int32_t y) {
    if (x == 0 && y == 0) {
        return;
    }
    if (y != 0) {
        fake_error("a vertical scroll of %d by %d: the fake doesn't model one", o->id, (int)y);
        return;
    }
    o->scroll_x -= x;
    fake_layout();
    fake_fire(o->id, LV_EVENT_SCROLL);
}

/* lv_anim_delete(obj, scroll_x_anim): its deleted_cb (scroll_end_cb) sends SCROLL_END when it had begun. */
static void anim_delete(lv_obj_t *o) {
    for (int i = 0; i < n_anims; i++) {
        if (anims[i].obj == o) {
            bool started = anims[i].started;
            anims[i] = anims[--n_anims];
            if (started) {
                fake_fire(o->id, LV_EVENT_SCROLL_END);
            }
            return;  /* there is one at most: a new one takes the place of the old (remove_concurrent_anims) */
        }
    }
}

/* lv_obj_scroll_by. Animated: SCROLL_BEGIN, then an ease-out from where it is to dx further, over the distance at half the screen's
 * width a second, 200 to 400 ms (lv_anim_speed_clamped), one step a frame (fake_frame), SCROLL_END at its end. It takes the place of
 * one that was running (LVGL's remove_concurrent_anims, at the new one's first step). Not animated: the ones running stop, then
 * SCROLL_BEGIN, the move and SCROLL_END. */
static void scroll_by(lv_obj_t *o, int32_t dx, int32_t dy, bool anim) {
    if (dx == 0 && dy == 0) {
        return;
    }
    if (dy != 0) {
        fake_error("a vertical scroll of %d by %d: the fake doesn't model one", o->id, (int)dy);
        return;
    }
    if (!anim) {
        anim_delete(o);
        fake_fire(o->id, LV_EVENT_SCROLL_BEGIN);
        scroll_by_raw(o, dx, 0);
        fake_fire(o->id, LV_EVENT_SCROLL_END);
        return;
    }
    int32_t speed = ((FAKE_HOR_RES >> 1) + 5) / 10;  /* lv_anim_speed_clamped keeps it in 10 px/s */
    int32_t d = dx < 0 ? -dx : dx, ms = d * 100 / speed;
    ms = ms < FAKE_SCROLL_ANIM_MIN_MS ? FAKE_SCROLL_ANIM_MIN_MS : ms > FAKE_SCROLL_ANIM_MAX_MS ? FAKE_SCROLL_ANIM_MAX_MS : ms;
    fake_fire(o->id, LV_EVENT_SCROLL_BEGIN);
    anim_delete(o);
    if (n_anims >= (int)ARRAY_LEN(anims)) {
        fake_error("more scroll animations than the fake holds");
        return;
    }
    int32_t from = -o->scroll_x;
    anims[n_anims++] = (fake_anim_t){o, from, from + dx, from, ms, 0, false};
}

/* An object's children that LVGL counts for scrolling and snapping: alive and not hidden. */
static bool counted_child(const lv_obj_t *c, const lv_obj_t *parent) {
    return c->parent == parent && !c->deleted && (c->flags & LV_OBJ_FLAG_HIDDEN) == 0;
}

/* find_snap_point_x: of the snappable children whose centre, moved by ofs, is in min..max, the distance of the nearest to the
 * row's centre, negated (what to scroll by); LV_COORD_MAX when there is none. */
static int32_t find_snap_point_x(const lv_obj_t *o, int32_t min, int32_t max, int32_t ofs) {
    if (o->snap_x == LV_SCROLL_SNAP_NONE) {
        return LV_COORD_MAX;
    }
    if (o->snap_x != LV_SCROLL_SNAP_CENTER) {
        fake_error("snapping %d to its start or end: the fake models the centre only", o->id);
        return LV_COORD_MAX;
    }
    int32_t dist = LV_COORD_MAX;
    int32_t x_parent = o->area.x1 + o->pad_left + (o->area.x2 - o->area.x1 + 1 - o->pad_left - o->pad_right) / 2;
    for (int i = 0; i < n_objs; i++) {
        const lv_obj_t *c = objs[i];
        if (!counted_child(c, o) || (c->flags & LV_OBJ_FLAG_SNAPPABLE) == 0) {
            continue;
        }
        int32_t x_child = c->area.x1 + (c->area.x2 - c->area.x1 + 1) / 2 + ofs;
        if (x_child >= min && x_child <= max) {
            int32_t x = x_child - x_parent;
            if ((x < 0 ? -x : x) < (dist < 0 ? -dist : dist)) {
                dist = x;
            }
        }
    }
    return dist == LV_COORD_MAX ? LV_COORD_MAX : -dist;
}

/* has_more_snap_points, sideways: a snap point left of the centre, and one right of it. */
static void more_snap_points(const lv_obj_t *o, bool *start, bool *end) {
    int32_t x = o->area.x1 + (o->area.x2 - o->area.x1 + 1 - o->pad_left - o->pad_right) / 2 + o->pad_left;
    *end = find_snap_point_x(o, x + 1, LV_COORD_MAX, 0) != LV_COORD_MAX;
    *start = find_snap_point_x(o, FAKE_COORD_MIN, x - 1, 0) != LV_COORD_MAX;
}

/* lv_obj_get_scroll_left/right/top/bottom (left to right, no self size, no vertical padding or scroll): how far the content goes
 * past each side of the content area. */
static int32_t scroll_left(const lv_obj_t *o) {
    return o->scroll_x;
}

static int32_t scroll_right(const lv_obj_t *o) {
    int32_t right = FAKE_COORD_MIN;
    for (int i = 0; i < n_objs; i++) {
        if (counted_child(objs[i], o) && objs[i]->area.x2 > right) {
            right = objs[i]->area.x2;
        }
    }
    int32_t self = -(o->area.x2 - o->area.x1 + 1 - o->pad_left - o->pad_right) - o->scroll_x;
    right = right == FAKE_COORD_MIN ? right : right - (o->area.x2 - o->pad_right);
    return right > self ? right : self;
}

static int32_t scroll_top(const lv_obj_t *o) {
    (void)o;
    return 0;  /* -scroll.y: nothing scrolls vertically here */
}

static int32_t scroll_bottom(const lv_obj_t *o) {
    int32_t bottom = FAKE_COORD_MIN;
    for (int i = 0; i < n_objs; i++) {
        if (counted_child(objs[i], o) && objs[i]->area.y2 - o->area.y2 > bottom) {
            bottom = objs[i]->area.y2 - o->area.y2;
        }
    }
    int32_t self = -(o->area.y2 - o->area.y1 + 1);
    return bottom > self ? bottom : self;
}

/* elastic_diff, sideways, with SCROLL_ELASTIC (every object has it): past the last snap point either way (or the content's end,
 * without snapping), a quarter of the finger's movement. */
static int32_t elastic_diff_x(const lv_obj_t *o, int32_t diff) {
    bool out;
    if (o->snap_x == LV_SCROLL_SNAP_NONE) {
        out = scroll_right(o) < 0 || scroll_left(o) < 0;
    } else {
        bool start, end;
        more_snap_points(o, &start, &end);
        out = !start || !end;
    }
    if (!out || diff == 0) {
        return diff;
    }
    diff += diff < 0 ? -FAKE_ELASTIC_SLOWNESS / 2 : FAKE_ELASTIC_SLOWNESS / 2;
    return diff / FAKE_ELASTIC_SLOWNESS;
}

/* lv_indev_find_scroll_obj: up from the pressed object, the one that can scroll the way the drag goes, once it has gone
 * FAKE_SCROLL_LIMIT; an object that can't scroll passes the drag to its parent (SCROLL_CHAIN, which LVGL gives every object made with a
 * parent: the screen has none). */
static lv_obj_t *find_scroll_obj(void) {
    lv_obj_t *candidate = NULL;
    lv_dir_t candidate_dir = LV_DIR_NONE;
    bool hor_en = false, ver_en = false;
    scroll_sum.x += vect.x;
    scroll_sum.y += vect.y;
    for (lv_obj_t *o = pressed_obj; o != NULL; o = o->parent) {
        int32_t ax = scroll_sum.x < 0 ? -scroll_sum.x : scroll_sum.x, ay = scroll_sum.y < 0 ? -scroll_sum.y : scroll_sum.y;
        if (ax > ay) {
            hor_en = true;
        } else {
            ver_en = true;
        }
        bool chain = o->parent != NULL;
        if ((o->flags & LV_OBJ_FLAG_SCROLLABLE) == 0) {
            if (!chain) {
                break;
            }
            continue;
        }
        bool up_en = ver_en, down_en = ver_en, left_en = hor_en, right_en = hor_en;
        if ((o->scroll_dir & LV_DIR_LEFT) == 0) left_en = false;
        if ((o->scroll_dir & LV_DIR_RIGHT) == 0) right_en = false;
        if ((o->scroll_dir & LV_DIR_TOP) == 0) up_en = false;
        if ((o->scroll_dir & LV_DIR_BOTTOM) == 0) down_en = false;
        int32_t sl, sr;
        if (o->snap_x == LV_SCROLL_SNAP_NONE) {
            sl = scroll_left(o);
            sr = scroll_right(o);
        } else {
            bool start, end;
            more_snap_points(o, &start, &end);
            sl = start ? 1 : -1;
            sr = end ? 1 : -1;
        }
        int32_t st = scroll_top(o), sb = scroll_bottom(o);
        const int32_t L = FAKE_SCROLL_LIMIT;
        if ((st > 0 || sb > 0) && ((up_en && scroll_sum.y >= L) || (down_en && scroll_sum.y <= -L))) {
            candidate = o;
            candidate_dir = LV_DIR_VER;
        }
        if ((sl > 0 || sr > 0) && ((left_en && scroll_sum.x >= L) || (right_en && scroll_sum.x <= -L))) {
            candidate = o;
            candidate_dir = LV_DIR_HOR;
        }
        if (st <= 0) up_en = false;
        if (sb <= 0) down_en = false;
        if (sl <= 0) left_en = false;
        if (sr <= 0) right_en = false;
        if ((left_en && scroll_sum.x >= L) || (right_en && scroll_sum.x <= -L) || (up_en && scroll_sum.y >= L) ||
            (down_en && scroll_sum.y <= -L)) {
            break;
        }
        if (!chain) {
            break;
        }
    }
    if (candidate != NULL) {
        scroll_dir_now = candidate_dir;
        scroll_obj = candidate;
        scroll_sum = (lv_point_t){0, 0};
    }
    return candidate;
}

/* lv_indev_scroll_handler: a read that moves scrolls what the drag scrolls (found on the first read that can: SCROLL_BEGIN). */
static void indev_scroll(void) {
    if (vect.x == 0 && vect.y == 0) {
        return;
    }
    lv_obj_t *o = scroll_obj;
    if (o == NULL) {
        o = find_scroll_obj();
        if (o == NULL) {
            return;
        }
        fake_fire(o->id, LV_EVENT_SCROLL_BEGIN);
    }
    if (scroll_dir_now != LV_DIR_HOR) {
        fake_error("a vertical drag of %d: the fake doesn't model one", o->id);
        return;
    }
    int32_t dx = elastic_diff_x(o, vect.x);
    if ((o->scroll_dir & LV_DIR_LEFT) == 0 && dx > 0) dx = 0;
    if ((o->scroll_dir & LV_DIR_RIGHT) == 0 && dx < 0) dx = 0;
    scroll_by_raw(o, dx, 0);
    scroll_sum.x += dx;
}

/* indev_gesture: while nothing scrolls, the reads' movement adds up (a read under FAKE_GESTURE_MIN_VELOCITY starts it again), and
 * past FAKE_GESTURE_LIMIT it is a gesture, its way the larger of the two, sent once, to the first object up from the pressed one that
 * doesn't pass gestures on (no GESTURE_BUBBLE). */
static void indev_gesture(void) {
    if (scroll_obj != NULL || gesture_sent) {
        return;
    }
    lv_obj_t *target = pressed_obj;
    while (target != NULL && (target->flags & LV_OBJ_FLAG_GESTURE_BUBBLE) != 0) {
        target = target->parent;
    }
    if (target == NULL) {
        return;
    }
    if ((vect.x < 0 ? -vect.x : vect.x) < FAKE_GESTURE_MIN_VELOCITY && (vect.y < 0 ? -vect.y : vect.y) < FAKE_GESTURE_MIN_VELOCITY) {
        gesture_sum = (lv_point_t){0, 0};
    }
    gesture_sum.x += vect.x;
    gesture_sum.y += vect.y;
    int32_t ax = gesture_sum.x < 0 ? -gesture_sum.x : gesture_sum.x, ay = gesture_sum.y < 0 ? -gesture_sum.y : gesture_sum.y;
    if (ax > FAKE_GESTURE_LIMIT || ay > FAKE_GESTURE_LIMIT) {
        gesture_sent = true;
        gesture_dir = ax > ay ? (gesture_sum.x > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT) : (gesture_sum.y > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP);
        fake_fire(target->id, LV_EVENT_GESTURE);
    }
}

/* lv_indev_scroll_throw_predict: where the throw would carry the row, losing FAKE_SCROLL_THROW % a step. */
static int32_t throw_predict(int32_t v) {
    int32_t sum = 0;
    while (v != 0) {
        sum += v;
        v = v * (100 - FAKE_SCROLL_THROW) / 100;
    }
    return sum;
}

/* The throw's first step (lv_indev_scroll_throw_handler, on a snapping row): an animated scroll to the snap point nearest where the
 * throw would carry it, SCROLL_END, and the touch's scroll is over. Its later steps find no scroll object and stop. */
static void indev_throw(void) {
    throw_pending = false;
    lv_obj_t *o = scroll_obj;
    if (o == NULL || scroll_dir_now == LV_DIR_NONE) {
        return;
    }
    if (scroll_dir_now != LV_DIR_HOR || o->snap_x == LV_SCROLL_SNAP_NONE) {
        fake_error("a throw of %d that isn't a snapping row's: the fake doesn't model one", o->id);
    } else {
        int32_t diff = throw_predict(throw_vect_x);
        int32_t x = find_snap_point_x(o, FAKE_COORD_MIN, LV_COORD_MAX, diff);
        if (x == LV_COORD_MAX) {
            fake_error("a throw of %d, which has no snap point", o->id);
        } else {
            scroll_by(o, x + diff, 0, true);
        }
    }
    fake_fire(o->id, LV_EVENT_SCROLL_END);
    scroll_dir_now = LV_DIR_NONE;
    scroll_obj = NULL;
}

/* One run of LVGL's animation timer, ms after the last: a throw a lift started takes its first step, the animations apps_lv.c started
 * take theirs (lv_anim_timer), then each scroll animation moves on (an ease-out: LVGL's is a cubic Bezier, and only the ends and the
 * time matter here), sending SCROLL, and one that has run its time ends, with SCROLL_END. */
static void fake_frame(int ms) {
    if (throw_pending) {
        indev_throw();
    }
    lv_anim_timer(ms);
    lv_obj_t *moving[ARRAY_LEN(anims)];
    int n = n_anims;
    for (int i = 0; i < n; i++) {
        moving[i] = anims[i].obj;
    }
    for (int i = 0; i < n; i++) {
        fake_anim_t *a = NULL;
        for (int k = 0; k < n_anims; k++) {
            if (anims[k].obj == moving[i]) {
                a = &anims[k];
            }
        }
        if (a == NULL) {
            continue;  /* a SCROLL before it stopped it */
        }
        a->started = true;
        a->elapsed += ms;
        int32_t t = a->elapsed < a->duration ? a->elapsed : a->duration;
        int32_t p = t * 1024 / a->duration, eased = 1024 - (1024 - p) * (1024 - p) / 1024;
        int32_t v = a->from + (a->to - a->from) * eased / 1024;
        lv_obj_t *o = a->obj;
        bool done = a->elapsed >= a->duration;
        if (v != a->now) {
            a->now = v;
            scroll_by_raw(o, v + o->scroll_x, 0);  /* scroll_x_anim: scroll.x becomes v */
        }
        if (done && anim_on(o)) {
            for (int k = 0; k < n_anims; k++) {
                if (anims[k].obj == o) {
                    anims[k] = anims[--n_anims];
                    break;
                }
            }
            fake_fire(o->id, LV_EVENT_SCROLL_END);
        }
    }
}

bool lv_obj_is_scrolling(const lv_obj_t *obj) {
    if (use((lv_obj_t *)obj, "lv_obj_is_scrolling", NULL) == NULL) {
        return false;
    }
    return scroll_obj == obj || anim_on(obj);
}

/* scroll_area_into_view for the object's own box, in its parent: as little scroll as shows it all, then, for a parent that snaps its
 * children to its centre, the scroll that puts its centre there. The parent's animations stop first. */
void lv_obj_scroll_to_view(lv_obj_t *obj, lv_anim_enable_t anim_en) {
    if (use(obj, "lv_obj_scroll_to_view", NULL) == NULL) {
        return;
    }
    logline("scroll_to_view %d%s", obj->id, anim_en ? " anim" : "");
    fake_layout();  /* lv_obj_update_layout */
    lv_obj_t *parent = obj->parent;
    if (parent == NULL || (parent->flags & LV_OBJ_FLAG_SCROLLABLE) == 0) {
        return;
    }
    const lv_area_t *c = &obj->area, *p = &parent->area;
    int32_t y_scroll = 0, top_diff = p->y1 - c->y1, bottom_diff = -(p->y2 - c->y2);
    if (!(top_diff >= 0 && bottom_diff >= 0)) {
        if (top_diff > 0) {
            y_scroll = scroll_top(parent) - top_diff < 0 ? 0 : top_diff;
        } else if (bottom_diff > 0) {
            y_scroll = scroll_bottom(parent) - bottom_diff < 0 ? 0 : -bottom_diff;
        }
    }
    int32_t x_scroll = 0, left_diff = p->x1 + parent->pad_left - c->x1, right_diff = -(p->x2 - parent->pad_right - c->x2);
    if (!(left_diff >= 0 && right_diff >= 0)) {
        if (left_diff > 0) {
            x_scroll = scroll_left(parent) - left_diff < 0 ? 0 : left_diff;
        } else if (right_diff > 0) {
            x_scroll = scroll_right(parent) - right_diff < 0 ? 0 : -right_diff;
        }
    }
    if (parent->snap_x == LV_SCROLL_SNAP_CENTER) {
        int32_t parent_w = p->x2 - p->x1 + 1 - parent->pad_left - parent->pad_right;
        x_scroll += p->x1 + parent->pad_left + parent_w / 2 - ((c->x2 - c->x1 + 1) / 2 + c->x1 + x_scroll);
    } else if (parent->snap_x != LV_SCROLL_SNAP_NONE) {
        fake_error("scroll_to_view in %d, which snaps to its start or end: the fake models the centre only", parent->id);
    }
    anim_delete(parent);
    if ((parent->scroll_dir & LV_DIR_LEFT) == 0 && x_scroll < 0) x_scroll = 0;
    if ((parent->scroll_dir & LV_DIR_RIGHT) == 0 && x_scroll > 0) x_scroll = 0;
    if ((parent->scroll_dir & LV_DIR_TOP) == 0 && y_scroll < 0) y_scroll = 0;
    if ((parent->scroll_dir & LV_DIR_BOTTOM) == 0 && y_scroll > 0) y_scroll = 0;
    scroll_by(parent, x_scroll, y_scroll, anim_en);
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
    lv_event_t e = {code, NULL, o, false, NULL};
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

/* lv_indev_search_obj: the topmost object under the point that is clickable, children before their parent and the last made
 * first; a hidden object hides what is in it, and a child outside its parent's box can't be found. */
static lv_obj_t *search_obj(lv_obj_t *o, int32_t x, int32_t y) {
    if ((o->flags & LV_OBJ_FLAG_HIDDEN) != 0) {
        return NULL;
    }
    bool on = x >= o->area.x1 && x <= o->area.x2 && y >= o->area.y1 && y <= o->area.y2;
    if (on) {
        for (int below = INT_MAX;;) {
            lv_obj_t *child = NULL;
            for (int i = 0; i < n_objs; i++) {
                lv_obj_t *c = objs[i];
                if (c->parent == o && !c->deleted && c->order < below && (child == NULL || c->order > child->order)) {
                    child = c;
                }
            }
            if (child == NULL) {
                break;
            }
            below = child->order;
            lv_obj_t *found = search_obj(child, x, y);
            if (found != NULL) {
                return found;
            }
        }
    }
    return on && (o->flags & LV_OBJ_FLAG_CLICKABLE) != 0 ? o : NULL;
}

/* A touch the way LVGL 9.5.0's indev (lv_indev.c) sends it. A new touch on an object: a throw still waiting for its first step takes
 * it (indev_proc_press stops the throw so: the snap's animation it starts goes on), the touch's state starts afresh, and the object gets
 * PRESSED. */
static void begin_touch(lv_obj_t *o) {
    if (scroll_obj != NULL) {
        indev_throw();
    }
    pressed_obj = active_obj = o;
    last_point = act_point;
    vect = scroll_sum = gesture_sum = (lv_point_t){0, 0};
    scroll_obj = NULL;
    scroll_dir_now = gesture_dir = LV_DIR_NONE;
    gesture_sent = wait_released = long_sent = false;
    if (o != NULL) {
        fake_fire(o->id, LV_EVENT_PRESSED);
    }
    active_obj = NULL;
}

/* The finger goes down on an object (at the middle of its box). */
static void fake_press(int id) {
    fake_layout();
    lv_obj_t *o = fake_obj(id);
    act_point = (lv_point_t){(o->area.x1 + o->area.x2 + 1) / 2, (o->area.y1 + o->area.y2 + 1) / 2};
    begin_touch(o);
}

/* The finger goes down at (x, y), on what lv_indev_search_obj finds there (it searches before it stops a throw). Its id, 0 for none. */
static int fake_down(int32_t x, int32_t y) {
    fake_layout();
    act_point = (lv_point_t){x, y};
    lv_obj_t *o = search_obj(&screen_obj, x, y);
    begin_touch(o);
    return o != NULL ? o->id : 0;
}

/* It stays down long enough: LONG_PRESSED. */
static void fake_hold(void) {
    long_sent = true;
    active_obj = pressed_obj;
    fake_fire(pressed_obj->id, LV_EVENT_LONG_PRESSED);
    active_obj = NULL;
}

/* It moves far enough: the gesture (indev_gesture), to the first object up from the pressed one that doesn't pass gestures on (one
 * with no GESTURE_BUBBLE), and to that one only. Whatever a real drag would have scrolled, this one doesn't: fake_move does that. */
static void fake_swipe(lv_dir_t dir) {
    lv_obj_t *target = pressed_obj;
    while (target != NULL && (target->flags & LV_OBJ_FLAG_GESTURE_BUBBLE) != 0) {
        target = target->parent;
    }
    gesture_dir = dir;
    gesture_sent = true;
    active_obj = pressed_obj;
    if (target != NULL) {
        fake_fire(target->id, LV_EVENT_GESTURE);
    }
    active_obj = NULL;
}

/* One read of a finger that is down, now at (x, y) (indev_proc_press): nothing once lv_indev_wait_release was called; otherwise
 * PRESSING, the scroll's animation stopped (lv_obj_stop_scroll_anim) when a drag already scrolls it, the drag (indev_scroll) and the
 * gesture (indev_gesture). */
static void fake_move(int32_t x, int32_t y) {
    act_point = (lv_point_t){x, y};
    if (pressed_obj != NULL && !wait_released) {
        active_obj = pressed_obj;
        vect = (lv_point_t){act_point.x - last_point.x, act_point.y - last_point.y};
        fake_fire(pressed_obj->id, LV_EVENT_PRESSING);
        if (pressed_obj != NULL && !wait_released) {
            if (scroll_obj != NULL) {
                anim_delete(scroll_obj);
            }
            indev_scroll();
            indev_gesture();
        }
        active_obj = NULL;
    }
    last_point = act_point;
}

/* It lifts (indev_proc_release). After lv_indev_wait_release the pressed object gets PRESS_LOST and nothing else (no RELEASED, no
 * click). Otherwise RELEASED, then, when no drag of the touch scrolled, SHORT_CLICKED (when no long press was sent) and CLICKED; a drag
 * that scrolled gets SCROLL_THROW_BEGIN on what it scrolled instead, and its throw starts: its first step is at the next frame
 * (fake_frame), or at the next touch. */
static void fake_lift(void) {
    lv_obj_t *o = pressed_obj, *s = scroll_obj;
    pressed_obj = NULL;
    active_obj = o;
    if (wait_released) {
        wait_released = false;
        if (o != NULL) {
            fake_fire(o->id, LV_EVENT_PRESS_LOST);
        }
    } else if (o != NULL) {
        fake_fire(o->id, LV_EVENT_RELEASED);
        if (s == NULL) {
            if (!long_sent) {
                fake_fire(o->id, LV_EVENT_SHORT_CLICKED);
            }
            fake_fire(o->id, LV_EVENT_CLICKED);
        } else {
            fake_fire(s->id, LV_EVENT_SCROLL_THROW_BEGIN);
        }
    }
    active_obj = NULL;
    if (s != NULL && scroll_obj == s) {
        throw_pending = true;
        throw_vect_x = vect.x;
    }
}

/* A tap: down and up. */
static void fake_tap(int id) {
    fake_press(id);
    fake_lift();
}

/* A tap at a point; the id of what it landed on, 0 for nothing. */
static int fake_tap_at(int32_t x, int32_t y) {
    int id = fake_down(x, y);
    fake_lift();
    return id;
}

/* Down at (x0, y0), then steps reads on the line to (x1, y1), then still reads more that don't move (a finger that stops before it
 * lifts throws nothing). The finger stays down. Its id, 0 for nothing. */
static int fake_drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int steps, int still) {
    int id = fake_down(x0, y0);
    for (int i = 1; i <= steps; i++) {
        fake_move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    }
    for (int i = 0; i < still; i++) {
        fake_move(x1, y1);
    }
    return id;
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
    act_point = last_point = vect = scroll_sum = gesture_sum = (lv_point_t){0, 0};
    scroll_obj = active_obj = NULL;
    scroll_dir_now = LV_DIR_NONE;
    gesture_sent = throw_pending = false;
    throw_vect_x = 0;
    n_anims = 0;
    while (n_lv_anims > 0) {  /* lv_anim_delete_all: each deleted callback called */
        remove_lv_anim(n_lv_anims - 1);
    }
    anim_list_changed = false;
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
    switch (rnd(22)) {
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
        case 19: snprintf(out, cap, "rotation = %d", (int)rnd(400) - 20); break;
        case 20: snprintf(out, cap, "spin = %d", rnd(3) == 0 ? 0 : (int)rnd(80) - 5); break;
        default: snprintf(out, cap, "scale = %d", (int)rnd(500)); break;
    }
    (void)kind;
}

/* Overwrites one field of the slot with garbage (the harness puts the widget back after the pass). */
static void corrupt(env_t *e, int slot) {
    apps_widget_t *w = W(e, slot);
    switch (rnd(26)) {
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
        case 23: w->rotation = (int16_t)((int)rnd(800) - 200); break;
        case 24: w->spin = (uint8_t)rnd(255); break;
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
    int spinning = 0;
    for (int i = 0; i < APPS_UI_SLOTS; i++) {
        const apps_widget_t *w = W(e, i);
        spinning += w->alive && w->kind == APPS_W_IMAGE && w->spin != 0;
    }
    CHECK(fake_spins() == spinning && stats_now().spinning == (uint32_t)spinning, "seed %u: %d animations, spinning %u, the table has %d",
          seed, fake_spins(), stats_now().spinning, spinning);
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
                fake_frame((int)rnd(40) + 1);  /* the spins step: on objects that may have gone since */
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

/* ---- The launcher and the crash card (Task 12; the carousel, launcher carousel Task 4) ---- */

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

static int fake_index(int id) {
    return (int)lv_obj_get_index(fake_obj(id));
}

/* Whether the object is drawn: it and every object up to the screen are not hidden. */
static bool fake_shown(int id) {
    for (const lv_obj_t *o = fake_obj(id); o != NULL && o != &screen_obj; o = o->parent) {
        if ((o->flags & LV_OBJ_FLAG_HIDDEN) != 0 || o->deleted) {
            return false;
        }
    }
    return true;
}

/* LVGL's layout of everything, as a refresh does it before it draws (no LVGL call of the code under test). */
static void fake_layout(void) {
    for (int i = 0; i < n_objs; i++) {
        if (!objs[i]->deleted) {
            layout_one(objs[i]);
        }
    }
}

/* The finger (or a fling, or the snap's animation) scrolls the object to x: LVGL moves its children and sends SCROLL. */
static void fake_scroll(int id, int32_t x) {
    fake_obj(id)->scroll_x = x;
    fake_layout();
    fake_fire(id, LV_EVENT_SCROLL);
}

/* A frame: LVGL draws every image that shows, from its descriptor, every byte of its pixels. A descriptor that is no
 * longer an image (its slot let go) is an error, and pixels whose block was freed are ASan's. How many were drawn. */
static volatile unsigned drawn_bytes;
static int fake_draw(void) {
    int drawn = 0;
    for (int i = 0; i < n_objs; i++) {
        const lv_obj_t *o = objs[i];
        if (o->deleted || strcmp(o->cls, "image") != 0 || o->src == NULL || !fake_shown(o->id)) {
            continue;
        }
        const lv_image_dsc_t *d = o->src;
        if (d->header.magic != 0x19 || d->data == NULL || d->header.w == 0 || d->header.h == 0 || d->data_size == 0) {
            fake_error("image %d is drawn from a descriptor that is no longer an image", o->id);
            continue;
        }
        unsigned sum = 0;
        for (uint32_t k = 0; k < d->data_size; k++) {
            sum += d->data[k];
        }
        drawn_bytes += sum;
        drawn++;
    }
    return drawn;
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
    run_io();  /* the worker ends what a scene before left it */
    fake_reset();
    apps_lv_build(fake_screen());
    cards_image = lv_image_create(fake_screen())->id;
    log_clear();
    picks = 0;
    picked[0] = '\0';
}

/* The end of a launcher case: it closes, the worker ends what it has, and the icons hold nothing. */
static void launcher_end_at(int line) {
    apps_lv_hide_cards();
    run_io();
    free_icon_files();
    CHECK_AT(line, held == 0, "%zu bytes of icons still held after the launcher closed", held);
    CHECK_AT(line, fake_errors == 0, "the fake LVGL counted %d errors, the first: %s", fake_errors, fake_note);
}
#define LAUNCHER_END() launcher_end_at(__LINE__)

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

/* ---- The carousel, as the fake sees it ---- */

#define SCREEN_CENTRE 233

/* The carousel's row: the object that snaps its children to the centre. -1 when there is none. */
static int carousel_row(void) {
    for (int i = 0; i < n_objs; i++) {
        if (!objs[i]->deleted && objs[i]->snap_x == LV_SCROLL_SNAP_CENTER) {
            return objs[i]->id;
        }
    }
    return -1;
}

/* The row's items (its children), in order, into ids; how many. */
static int carousel_items(int *ids, int cap) {
    int row = carousel_row();
    if (row < 0) {
        return 0;
    }
    int n = fake_children(fake_obj(row), fake_kids);
    for (int i = 0; i < n && i < cap; i++) {
        ids[i] = fake_kids[i]->id;
    }
    return n < cap ? n : cap;
}

static int carousel_item(int k) {
    int ids[64];
    int n = carousel_items(ids, 64);
    return k >= 0 && k < n ? ids[k] : -1;
}

/* What an item holds: its tile (a circle with its letter in it), its image (made once an icon came), its running dot. */
typedef struct {
    int tile, letter, image, dot;
} item_parts_t;

static item_parts_t item_parts(int item) {
    item_parts_t p = {-1, -1, -1, -1};
    int n = fake_children(fake_obj(item), fake_kids);
    lv_obj_t *kids[16];
    memcpy(kids, fake_kids, (size_t)(n < 16 ? n : 16) * sizeof kids[0]);
    for (int i = 0; i < n && i < 16; i++) {
        if (strcmp(kids[i]->cls, "image") == 0) {
            p.image = kids[i]->id;
        } else if (strcmp(kids[i]->cls, "obj") == 0) {
            int inside = fake_children(kids[i], fake_kids);
            if (inside == 1 && strcmp(fake_kids[0]->cls, "label") == 0) {
                p.tile = kids[i]->id;
                p.letter = fake_kids[0]->id;
            } else if (inside == 0) {
                p.dot = kids[i]->id;
            }
        }
    }
    return p;
}

/* What the item shows (whether the launcher does or not): 'T' its tile, 'I' its icon, '-' nothing (a blank), '2' both. */
static char item_face(int item) {
    item_parts_t p = item_parts(item);
    bool tile = p.tile > 0 && !fake_flag(p.tile, LV_OBJ_FLAG_HIDDEN);
    bool image = p.image > 0 && !fake_flag(p.image, LV_OBJ_FLAG_HIDDEN) && fake_obj(p.image)->src != NULL;
    return tile && image ? '2' : tile ? 'T' : image ? 'I' : '-';
}

/* No item's image is left on a descriptor apps_icons doesn't give it now, shown or hidden: a tile's image has no source. */
static void check_sources_at(int line) {
    int ids[64];
    int n = carousel_items(ids, 64);
    for (int k = 0; k < n; k++) {
        item_parts_t p = item_parts(ids[k]);
        if (p.image < 0) {
            continue;
        }
        const lv_image_dsc_t *src = fake_obj(p.image)->src;
        bool tile = item_face(ids[k]) == 'T';
        CHECK_AT(line, tile ? src == NULL : src != NULL, "item %d: a %s with a source %p", k, tile ? "tile" : "icon", (const void *)src);
    }
}
#define CHECK_SOURCES() check_sources_at(__LINE__)

/* Every item's face, "TTI..." (at most 63). */
static const char *faces(void) {
    static char out[64];
    int ids[63];
    int n = carousel_items(ids, 63);
    for (int i = 0; i < n; i++) {
        out[i] = item_face(ids[i]);
    }
    out[n] = '\0';
    return out;
}

/* The side of a tile drawn at scale, and the scale of an icon side px wide drawn at scale (apps_lv.h). */
static int32_t tile_side(int scale) {
    return (APPS_LV_ICON_PX * scale + 128) / 256;
}

static uint32_t icon_zoom(int scale, unsigned px) {
    return (uint32_t)(((uint32_t)scale * APPS_LV_ICON_PX + px / 2) / px);
}

/* Whether the item is drawn at scale: its tile that size, or its icon at the zoom that makes it that. */
static bool item_at_scale(int item, int scale) {
    item_parts_t p = item_parts(item);
    char face = item_face(item);
    if (face == 'T') {
        const lv_obj_t *t = fake_obj(p.tile);
        return t->w == tile_side(scale) && t->h == tile_side(scale);
    }
    if (face == 'I') {
        const lv_obj_t *im = fake_obj(p.image);
        return im->scale == icon_zoom(scale, im->src->header.w);
    }
    return false;
}

/* Every item is drawn at the scale its distance from the centre gives it. */
static void check_scales_at(int line) {
    int ids[64];
    int n = carousel_items(ids, 64);
    int32_t sx = fake_obj(carousel_row())->scroll_x;
    for (int k = 0; k < n; k++) {
        int want = apps_lv_carousel_scale(k * APPS_LV_ITEM_PITCH - (int)sx);
        CHECK_AT(line, item_at_scale(ids[k], want), "item %d at scroll %d: not drawn at %d (its face %c)", k, (int)sx, want,
                 item_face(ids[k]));
    }
}
#define CHECK_SCALES() check_scales_at(__LINE__)

/* The carousel's name label: the card's one label that is neither the title nor in the row. -1 when there is none; more than one is
 * an error. */
static int name_label(void) {
    int row = carousel_row();
    if (row < 0) {
        return -1;
    }
    int card = fake_parent(row), found = -1, n = 0;
    for (int i = 0; i < n_objs; i++) {
        const lv_obj_t *o = objs[i];
        if (!o->deleted && strcmp(o->cls, "label") == 0 && fake_parent(o->id) == card && strcmp(o->text, "Apps") != 0) {
            found = o->id;
            n++;
        }
    }
    CHECK(n <= 1, "%d name labels", n);
    return found;
}

static const char *name_shown(void) {
    int id = name_label();
    return id > 0 ? fake_obj(id)->text : "(none)";
}

/* The screen row through the middle of the name, as it is laid out under the carousel of a launcher that shows (on the card: the label
 * isn't clickable). */
static int32_t name_middle_y(void) {
    fake_layout();
    const lv_area_t *a = &fake_obj(name_label())->area;
    return (a->y1 + a->y2 + 1) / 2;
}

/* The centre of an object's laid-out box, in screen x. */
static int32_t centre_x(int id) {
    const lv_area_t *a = &fake_obj(id)->area;
    return (a->x1 + a->x2 + 1) / 2;
}

/* Every corner of the box is inside the round screen. */
static bool inside_circle(int id) {
    const lv_area_t *a = &fake_obj(id)->area;
    const int32_t xs[2] = {a->x1, a->x2 + 1}, ys[2] = {a->y1, a->y2 + 1};
    for (int i = 0; i < 4; i++) {
        int32_t dx = xs[i & 1] - SCREEN_CENTRE, dy = ys[i >> 1] - SCREEN_CENTRE;
        if (dx * dx + dy * dy > SCREEN_CENTRE * SCREEN_CENTRE) {
            return false;
        }
    }
    return true;
}

static const apps_lv_app_t FIVE[] = {
    {"alpha", "Alpha", ""}, {"bravo", "Bravo", ""}, {"charlie", "Charlie", ""}, {"delta", "Delta", ""}, {"echo", "Echo", ""},
};

/* ---- Fingers on the launcher (launcher carousel Task 5), through the fake's pointer: LVGL 9.5.0's way of making PRESSED, a scroll, a
 * gesture or a click of them ---- */

#define FRAME_MS 40                                            /* the 1.75C's frame */
#define EDGE_TOP (2 * SCREEN_CENTRE - APPS_LV_CLOSE_EDGE_PX)  /* the bottom band's first row */

static bool in_area(const lv_area_t *a, lv_point_t p) {
    return p.x >= a->x1 && p.x <= a->x2 && p.y >= a->y1 && p.y <= a->y2;
}

/* What lv_obj_is_scrolling says, without a call of the code under test's: a drag scrolls it, or an animation. */
static bool fake_scrolling(int id) {
    const lv_obj_t *o = fake_obj(id);
    return scroll_obj == o || anim_on(o);
}

/* Frames until nothing moves: no throw waiting for its first step, no animation. */
static void settle_at(int line) {
    for (int i = 0; i < 100 && (throw_pending || n_anims > 0); i++) {
        fake_frame(FRAME_MS);
    }
    CHECK_AT(line, !throw_pending && n_anims == 0, "the row never came to rest");
}
#define SETTLE() settle_at(__LINE__)

static int32_t scroll_now(void) {
    return fake_obj(carousel_row())->scroll_x;
}

/* The middle of item k's face (its tile's place, where an icon is drawn too), and of its box, as they are now. */
static lv_point_t face_at(int k) {
    fake_layout();
    const lv_area_t *a = &fake_obj(item_parts(carousel_item(k)).tile)->area;
    return (lv_point_t){(a->x1 + a->x2 + 1) / 2, (a->y1 + a->y2 + 1) / 2};
}

static lv_point_t box_at(int k) {
    fake_layout();
    const lv_area_t *a = &fake_obj(carousel_item(k))->area;
    return (lv_point_t){(a->x1 + a->x2 + 1) / 2, (a->y1 + a->y2 + 1) / 2};
}

/* A swipe from (x, y) by (dx, dy), in 8 reads, the finger still down; the id of what it began on. */
static int swipe_from(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    return fake_drag(x, y, x + dx, y + dy, 8, 0);
}

/* ---- The pure parts ---- */

static void scale_curve(void) {
    const int P = APPS_LV_ITEM_PITCH, SIDE = APPS_LV_ICON_SIDE_SCALE;
    CHECK(SIDE > 0 && SIDE < 256, "a neighbour is smaller than the centre: %d", SIDE);
    CHECK(apps_lv_carousel_scale(0) == 256, "the centre is full size: %d", apps_lv_carousel_scale(0));
    CHECK(apps_lv_carousel_scale(P) == SIDE && apps_lv_carousel_scale(-P) == SIDE, "a pitch away is a neighbour's: %d %d",
          apps_lv_carousel_scale(P), apps_lv_carousel_scale(-P));
    const int beyond[] = {P + 1, -P - 1, 2 * P, -3 * P, 100000, -100000, INT_MAX, INT_MIN, INT_MIN + 1};
    for (size_t i = 0; i < ARRAY_LEN(beyond); i++) {
        CHECK(apps_lv_carousel_scale(beyond[i]) == SIDE, "%d away: %d", beyond[i], apps_lv_carousel_scale(beyond[i]));
    }
    int last = 256;
    for (int d = 0; d <= P; d++) {
        int s = apps_lv_carousel_scale(d);
        CHECK(s <= last && s >= SIDE && s <= 256, "monotonic: %d at %d, after %d", s, d, last);
        CHECK(apps_lv_carousel_scale(-d) == s, "the same either side: %d %d at %d", s, apps_lv_carousel_scale(-d), d);
        int line = 256 - (256 - SIDE) * d / P;  /* linear, within a unit of rounding */
        CHECK(s - line <= 1 && line - s <= 1, "linear: %d at %d, the line %d", s, d, line);
        last = s;
    }
    CHECK(apps_lv_carousel_scale(P / 2) < 256 && apps_lv_carousel_scale(P / 2) > SIDE, "between, it is between: %d",
          apps_lv_carousel_scale(P / 2));
    /* Task 6a's: a neighbour at most 40% of the centre (102 of 256), half way at half a pitch, and the boxes apart at rest. */
    CHECK(SIDE <= 102, "a neighbour's scale is at most 102 of 256: %d", SIDE);
    int half = apps_lv_carousel_scale(P / 2), mid = (256 + SIDE) / 2;
    CHECK(half - mid <= 1 && mid - half <= 1, "half a pitch away, half way: %d, the middle %d", half, mid);
    CHECK(P > APPS_LV_ICON_PX, "the row shows between two items' boxes at rest: pitch %d, box %d", P, APPS_LV_ICON_PX);
}

/* Printed for the test, which checks them against the palette and zlib's CRC-32: "color <id> <rrggbb>", each asked twice. */
static void tile_colors(void) {
    static const char *const ids[] = {"timer", "stopwatch1", "stopwatch2", "stopwatch3", "mtg_life", "prayer_times", "hello",
                                      "a", "b", "c", "d", "e", "f", "g", "h", "zz_9", "app00", "app01", "app02", "app03"};
    for (size_t i = 0; i < ARRAY_LEN(ids); i++) {
        uint32_t c = apps_lv_tile_color(ids[i]);
        CHECK(c == apps_lv_tile_color(ids[i]), "%s: the same colour each time", ids[i]);
        printf("color %s %06x\n", ids[i], (unsigned)c);
    }
    printf("color (null) %06x\n", (unsigned)apps_lv_tile_color(NULL));
    printf("color (empty) %06x\n", (unsigned)apps_lv_tile_color(""));
}

static void tile_letter(void) {
    static const struct {
        const char *name;
        char letter;
    } cases[] = {{"timer", 'T'},   {"Timer", 'T'},   {"9lives", '9'}, {"zed", 'Z'}, {"\xc3\x9c" "ber", '?'}, {"\xe2\x8f\xb1 Timer", '?'},
                 {"_x", '?'},      {" x", '?'},      {"", '?'},       {"-", '?'},   {"\x7f", '?'},        {"\xff", '?'}};
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        char got = apps_lv_tile_letter(cases[i].name);
        CHECK(got == cases[i].letter, "case %zu: '%c' and not '%c'", i, got, cases[i].letter);
    }
    CHECK(apps_lv_tile_letter(NULL) == '?', "NULL: '%c'", apps_lv_tile_letter(NULL));
}

/* ---- The carousel ---- */

static void opens_centred_on_first_app(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    fake_layout();
    int row = carousel_row();
    CHECK(row > 0, "a carousel\n%s", log_peek());
    if (row < 0) {
        return;
    }
    const lv_obj_t *r = fake_obj(row);
    CHECK(r->w == lv_pct(100) && r->scroll_dir == LV_DIR_HOR && r->scrollbar_mode == LV_SCROLLBAR_MODE_OFF &&
              fake_flag(row, LV_OBJ_FLAG_SCROLLABLE),
          "the row: full width, scrolls sideways, no scrollbar: w %d dir %d bar %d", (int)r->w, (int)r->scroll_dir,
          (int)r->scrollbar_mode);
    CHECK(r->pad_left == (466 - APPS_LV_ICON_PX) / 2 && r->pad_right == r->pad_left,
          "the first and the last can come to the centre: pads %d %d", (int)r->pad_left, (int)r->pad_right);
    CHECK(r->scroll_x == 0, "not scrolled: %d", (int)r->scroll_x);
    int ids[8];
    int n = carousel_items(ids, 8);
    CHECK(n == 5, "an item each: %d", n);
    for (int k = 0; k < n; k++) {
        const lv_obj_t *o = fake_obj(ids[k]);
        CHECK(o->w == APPS_LV_ICON_PX && fake_flag(ids[k], LV_OBJ_FLAG_SNAPPABLE) && !fake_flag(ids[k], LV_OBJ_FLAG_SCROLLABLE),
              "item %d: %d wide, snappable %d, scrollable %d", k, (int)o->w, fake_flag(ids[k], LV_OBJ_FLAG_SNAPPABLE),
              fake_flag(ids[k], LV_OBJ_FLAG_SCROLLABLE));
        CHECK(centre_x(ids[k]) == SCREEN_CENTRE + k * APPS_LV_ITEM_PITCH, "item %d's centre: %d", k, (int)centre_x(ids[k]));
        item_parts_t p = item_parts(ids[k]);
        CHECK(p.tile > 0 && !fake_flag(p.tile, LV_OBJ_FLAG_CLICKABLE), "item %d's tile isn't clickable: its taps are the item's", k);
    }
    CHECK(strcmp(name_shown(), "Alpha") == 0, "the first app is named: %s", name_shown());
    CHECK(strcmp(faces(), "TTTTT") == 0, "tiles: %s", faces());
    CHECK(item_at_scale(ids[0], 256), "the first is full size");
    CHECK_SCALES();
    /* The snap brings item 3 to the centre: LVGL scrolls by three pitches. */
    fake_scroll(row, 3 * APPS_LV_ITEM_PITCH);
    CHECK(centre_x(ids[3]) == SCREEN_CENTRE && strcmp(name_shown(), "Delta") == 0, "item 3 centred: %d %s", (int)centre_x(ids[3]),
          name_shown());
    CHECK_SCALES();
    LAUNCHER_END();
}

static void one_name_label_shows_the_centre_app(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    const int P = APPS_LV_ITEM_PITCH;
    static const struct {
        int32_t x;
        const char *name;
    } steps[] = {{0, "Alpha"},       {P / 3, "Alpha"},          {2 * P, "Charlie"}, {2 * P + 2 * P / 5, "Charlie"},
                 {2 * P + 3 * P / 5, "Delta"}, {-40, "Alpha"}, {4 * P + 60, "Echo"}, {P, "Bravo"}};
    for (size_t i = 0; i < ARRAY_LEN(steps); i++) {
        fake_scroll(row, steps[i].x);
        CHECK(strcmp(name_shown(), steps[i].name) == 0, "at %d: %s, not %s", (int)steps[i].x, name_shown(), steps[i].name);
        CHECK_SCALES();
    }
    int label = name_label();
    CHECK(label > 0 && !fake_flag(label, LV_OBJ_FLAG_CLICKABLE), "the name isn't clickable");
    /* The letters are the tiles', not names. */
    CHECK(fake_label("A") > 0 && fake_label("E") > 0, "a letter on each tile");
    LAUNCHER_END();
}

static void running_dot_under_running_app(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, "charlie", pick);
    fake_layout();
    int ids[8], dots = 0;
    int n = carousel_items(ids, 8);
    for (int k = 0; k < n; k++) {
        item_parts_t p = item_parts(ids[k]);
        if (p.dot > 0) {
            dots++;
            CHECK(k == 2, "the dot is under item %d, not charlie's", k);
            const lv_obj_t *d = fake_obj(p.dot);
            const lv_area_t face = fake_obj(p.tile)->area;
            CHECK(d->area.y1 > face.y2 && d->w > 0 && d->w == d->h, "under its face, round: y %d, the face to %d", (int)d->area.y1,
                  (int)face.y2);
        }
    }
    CHECK(dots == 1, "one dot: %d", dots);
    char line[64];
    item_parts_t p = item_parts(ids[2]);
    snprintf(line, sizeof line, "radius %d %d", p.dot, LV_RADIUS_CIRCLE);
    CHECK(log_has_line(line), "a round dot\n%s", log_peek());
    /* No app runs, or one the list hasn't: no dot. */
    const char *runs[] = {NULL, "zulu", ""};
    for (size_t i = 0; i < ARRAY_LEN(runs); i++) {
        apps_lv_show_launcher(FIVE, 5, runs[i], pick);
        n = carousel_items(ids, 8);
        dots = 0;
        for (int k = 0; k < n; k++) {
            dots += item_parts(ids[k]).dot > 0 ? 1 : 0;
        }
        CHECK(n == 5 && dots == 0, "running '%s': %d dots", runs[i] != NULL ? runs[i] : "(null)", dots);
    }
    LAUNCHER_END();
}

/* Before the worker's pixels are polled in, every item is its tile; a ready icon shows its image; an item whose icon left the window,
 * failed or never was is its tile again; at no point is an item blank, or an image drawn from a block that was freed. */
static void icon_or_tile_never_blank(void) {
    cards_scene();
    put_icon("alpha", CF_RGB565A8, 120, 120, 0x11);
    put_icon("bravo", CF_A8, 120, 120, 0x22);
    put_icon("delta", CF_RGB565A8, 120, 100, 0x44);  /* not square: a tile */
    put_icon("echo", CF_RGB565A8, 160, 160, 0x55);   /* the largest: drawn in the same box */
    put_icon("golf", CF_RGB565A8, 120, 120, 0x77);
    const apps_lv_app_t apps[] = {{"alpha", "Alpha", "icon.bin"}, {"bravo", "Bravo", "icon.bin"}, {"charlie", "Charlie", ""},
                                  {"delta", "Delta", "icon.bin"}, {"echo", "Echo", "icon.bin"},   {"foxtrot", "Foxtrot", "icon.bin"},
                                  {"golf", "Golf", "icon.bin"}};
    apps_lv_show_launcher(apps, 7, NULL, pick);
    int row = carousel_row();
    CHECK(strcmp(faces(), "TTTTTTT") == 0, "tiles until the pixels come: %s", faces());
    CHECK(io_queued == 2 && store_reads == 0, "the centre's and the next asked for on the worker: %d queued, %d read", io_queued,
          store_reads);
    run_io();
    CHECK(strcmp(faces(), "TTTTTTT") == 0, "read, not yet polled: tiles: %s", faces());
    apps_lv_launcher_poll();
    CHECK(strcmp(faces(), "IITTTTT") == 0, "polled: the icons: %s", faces());
    CHECK(fake_draw() == 2, "two icons drawn");
    item_parts_t a = item_parts(carousel_item(0)), b = item_parts(carousel_item(1));
    CHECK(fake_obj(a.image)->src == apps_icons_get("alpha") && fake_obj(b.image)->src == apps_icons_get("bravo"),
          "each its own app's");
    char line[64];
    snprintf(line, sizeof line, "image_recolor %d #ffffff", b.image);
    CHECK(log_has_line(line), "an A8 icon is drawn white\n%s", log_peek());
    snprintf(line, sizeof line, "image_recolor %d #ffffff", a.image);
    CHECK(!log_has_line(line), "a colour one isn't recoloured");
    CHECK_SCALES();
    /* To delta: alpha leaves the window (freed at once), delta's file is bad, echo is the largest icon. */
    fake_scroll(row, 3 * APPS_LV_ITEM_PITCH);
    CHECK(fake_draw() == 1 && strcmp(faces(), "TITTTTT") == 0, "alpha is its tile again, bravo stays: %s", faces());
    CHECK_SOURCES();
    for (int frame = 0; frame < 4; frame++) {
        run_io();
        apps_lv_launcher_poll();
        CHECK(strchr(faces(), '-') == NULL && strchr(faces(), '2') == NULL, "frame %d: %s", frame, faces());
        fake_draw();
        CHECK_SOURCES();
    }
    CHECK(strcmp(faces(), "TITTITT") == 0, "delta's bad file and foxtrot's missing one are tiles, echo is its icon: %s", faces());
    CHECK(esp_count("W muse_apps: icons: delta: icon.bin: ") == 1 && esp_count("W muse_apps: icons: foxtrot: icon.bin: ") == 1,
          "a line each\n%s", esp_peek());
    CHECK_SCALES();
    fake_scroll(row, 4 * APPS_LV_ITEM_PITCH);
    CHECK(fake_obj(item_parts(carousel_item(4)).image)->scale == icon_zoom(256, 160), "the 160 px icon fills the box at the centre");
    CHECK_SCALES();
    /* To golf, at the end: bravo leaves. */
    fake_scroll(row, 6 * APPS_LV_ITEM_PITCH);
    run_io();
    apps_lv_launcher_poll();
    CHECK(fake_draw() == 2 && strcmp(faces(), "TTTTITI") == 0, "at the end: %s", faces());
    CHECK_SOURCES();
    /* Closed: every block goes, and no image is left on one. */
    int card = fake_parent(row);
    fake_tap(card);
    CHECK(!apps_lv_launcher_shown() && strcmp(faces(), "TTTTTTT") == 0, "closed, every item its tile: %s", faces());
    CHECK_SOURCES();
    run_io();
    CHECK(held == 0, "closed: %zu bytes held", held);
    /* Opened again: the tiles first, then the icons. */
    apps_lv_show_launcher(apps, 7, NULL, pick);
    CHECK(strcmp(faces(), "TTTTTTT") == 0, "tiles first: %s", faces());
    run_io();
    apps_lv_launcher_poll();
    CHECK(strcmp(faces(), "IITTTTT") == 0, "then the icons: %s", faces());
    LAUNCHER_END();
}

static void every_app_is_an_item(void) {
    cards_scene();
    static char ids_text[40][12], names_text[40][12];
    apps_lv_app_t apps[40];
    for (int i = 0; i < 40; i++) {
        snprintf(ids_text[i], sizeof ids_text[i], "app%02d", i);
        snprintf(names_text[i], sizeof names_text[i], "%c%02d", 'A' + i % 26, i);
        apps[i] = (apps_lv_app_t){ids_text[i], names_text[i], ""};
    }
    apps_lv_show_launcher(apps, 12, NULL, pick);
    int ids[64];
    int n = carousel_items(ids, 64);
    CHECK(n == 12, "12 apps, 12 items: %d", n);
    for (int k = 0; k < n; k++) {
        item_parts_t p = item_parts(ids[k]);
        char letter[2] = {(char)('A' + k % 26), '\0'};
        CHECK(p.tile > 0 && strcmp(fake_obj(p.letter)->text, letter) == 0, "item %d is its app's: '%s'", k,
              p.letter > 0 ? fake_obj(p.letter)->text : "");
    }
    fake_scroll(carousel_row(), 11 * APPS_LV_ITEM_PITCH);
    CHECK(strcmp(name_shown(), "L11") == 0, "the last can be centred: %s", name_shown());
    /* More than the launcher holds: the first APPS_LV_LAUNCHER_MAX. */
    apps_lv_show_launcher(apps, 40, NULL, pick);
    n = carousel_items(ids, 64);
    CHECK(n == APPS_LV_LAUNCHER_MAX, "%d items of 40", n);
    CHECK(fake_label("N39") < 0 && strcmp(name_shown(), "A00") == 0, "the rest left out");
    /* An item with no id is left out. */
    const apps_lv_app_t gaps[] = {{"one", "One", ""}, {NULL, "Nobody", ""}, {"", "Empty", ""}, {"two", "Two", ""}};
    apps_lv_show_launcher(gaps, 4, NULL, pick);
    CHECK(carousel_items(ids, 64) == 2, "the two with ids");
    fake_tap(carousel_item(1));  /* a side app comes to the centre (Task 5), and a tap there opens it */
    SETTLE();
    CHECK(picks == 0 && strcmp(name_shown(), "Two") == 0, "the second item is the second app: %s", name_shown());
    fake_tap(carousel_item(1));
    CHECK(picks == 1 && strcmp(picked, "two") == 0, "and picks the right one: %s", picked);
    LAUNCHER_END();
}

/* Review Focus 1: opened during the deferred start, the launcher says "Loading apps..." (APPS_LV_LOADING: three ASCII dots, which the
 * font has), with no carousel and not the empty state; shown again with the list it is the carousel, centred on the first, and the
 * text is gone; shown again with another list, the old items go with no call on them. apps_lv_launcher_shown says whether the
 * launcher is up: not before it is shown, not after it closes, and not for a crash card. */
static void loading_then_list_rebuilds(void) {
    cards_scene();
    CHECK(!apps_lv_launcher_shown(), "nothing is shown yet");
    apps_lv_show_launcher(NULL, APPS_LV_LOADING, NULL, pick);
    int loading = fake_label("Loading apps...");
    CHECK(loading > 0, "the loading text\n%s", log_peek());
    int launcher = fake_parent(loading);
    CHECK_CARD(launcher);
    CHECK(carousel_row() < 0 && fake_label("No apps yet. Ask Muse to make one.") < 0, "no carousel, and not the empty state");
    CHECK(!fake_flag(loading, LV_OBJ_FLAG_CLICKABLE), "the text isn't clickable: a tap on it closes the launcher, as the empty state's");
    CHECK(apps_lv_launcher_shown() && apps_lv_cards_shown(), "the launcher is up");
    for (int frame = 0; frame < 3; frame++) {
        apps_lv_launcher_poll();
    }
    CHECK(io_queued == 0, "nothing asked of the worker while it loads");
    /* The mount's list comes. */
    put_icon("hello", CF_RGB565A8, 120, 120, 0x10);
    put_icon("timer", CF_RGB565A8, 120, 120, 0x20);
    const apps_lv_app_t apps[] = {{"hello", "Hello", "icon.bin"}, {"timer", "Kitchen timer", "icon.bin"}, {"quiet", "Quiet", ""}};
    apps_lv_show_launcher(apps, 3, "timer", pick);
    CHECK(fake_label("Loading apps...") < 0, "the loading text is gone");
    int row = carousel_row();
    CHECK(row > 0 && fake_parent(row) == launcher && carousel_items((int[8]){0}, 8) == 3, "the carousel, on the same card\n%s",
          log_peek());
    CHECK(fake_obj(row)->scroll_x == 0 && strcmp(name_shown(), "Hello") == 0 && strcmp(faces(), "TTT") == 0,
          "centred on the first, tiles: %s %s", name_shown(), faces());
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_launcher_shown(), "still up");
    run_io();
    apps_lv_launcher_poll();
    CHECK(strcmp(faces(), "IIT") == 0 && fake_draw() == 2, "the icons come: %s", faces());
    fake_scroll(row, APPS_LV_ITEM_PITCH);
    /* The list changes while it shows (an install, and hello reinstalled with a new icon): rebuilt, centred on the first, the old
     * icons dropped, so hello's is the new file's. */
    int old_row = row, old_item = carousel_item(0);
    put_icon("new", CF_RGB565A8, 120, 120, 0x30);
    free(icon_files[0].data);  /* hello's: the same path, new pixels */
    n_icon_files--;
    memmove(&icon_files[0], &icon_files[1], (size_t)n_icon_files * sizeof icon_files[0]);
    put_icon("hello", CF_RGB565A8, 120, 120, 0x40);
    const apps_lv_app_t changed[] = {{"new", "New", "icon.bin"}, {"hello", "Hello", "icon.bin"}};
    apps_lv_show_launcher(changed, 2, NULL, pick);
    CHECK(fake_obj(old_row)->deleted && fake_obj(old_item)->deleted, "the old carousel is gone");
    row = carousel_row();
    CHECK(row > 0 && row != old_row && fake_obj(row)->scroll_x == 0 && strcmp(name_shown(), "New") == 0 && strcmp(faces(), "TT") == 0,
          "the new list, from its first, tiles first: %s %s", name_shown(), faces());
    CHECK(fake_draw() == 0, "nothing is drawn from the old icons");
    run_io();
    apps_lv_launcher_poll();
    CHECK(strcmp(faces(), "II") == 0 && fake_draw() == 2, "the new icons: %s", faces());
    const lv_image_dsc_t *hello = fake_obj(item_parts(carousel_item(1)).image)->src;
    CHECK(hello == apps_icons_get("hello") && hello->data[0] == 0x40, "hello's own, the new file's: 0x%02x", hello->data[0]);
    CHECK_SOURCES();
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_launcher_shown(), "closed: not shown");
    apps_lv_show_crash("Hello", "main.lua:1: boom");
    CHECK(apps_lv_cards_shown() && !apps_lv_launcher_shown(), "a crash card isn't the launcher");
    LAUNCHER_END();
}

/* Review Focus 5: a name of 24 bytes, the most a manifest has, is shown whole and ellipsised by LVGL inside the circle; a name that
 * starts with a byte that isn't ASCII has the tile '?'; a longer name is cut at 24 bytes; an app with no name shows its id. */
static void long_and_non_ascii_names(void) {
    cards_scene();
    const apps_lv_app_t apps[] = {{"long", "WWWWWWWWWWWWWWWWWWWWWWWW", ""}, {"umlaut", "\xc3\x9c" "ber timer", ""},
                                  {"longer", "abcdefghijklmnopqrstuvwxyz0123", ""}, {"noname", "", ""}, {"nullname", NULL, ""}};
    apps_lv_show_launcher(apps, 5, NULL, pick);
    fake_layout();
    int label = name_label();
    const lv_obj_t *l = label > 0 ? fake_obj(label) : NULL;
    CHECK(l != NULL && strcmp(l->text, "WWWWWWWWWWWWWWWWWWWWWWWW") == 0, "the whole name: %s", l != NULL ? l->text : "");
    CHECK(l != NULL && l->long_mode == LV_LABEL_LONG_MODE_DOTS && l->w != LV_SIZE_CONTENT && l->h != LV_SIZE_CONTENT,
          "LVGL ellipsises it in a box of its own");
    CHECK(label > 0 && inside_circle(label), "the box is inside the circle: %d,%d - %d,%d", (int)l->area.x1, (int)l->area.y1,
          (int)l->area.x2, (int)l->area.y2);
    CHECK(strcmp(fake_obj(item_parts(carousel_item(1)).letter)->text, "?") == 0, "the umlaut's tile is '?'");
    CHECK(strcmp(fake_obj(item_parts(carousel_item(0)).letter)->text, "W") == 0, "W");
    fake_scroll(carousel_row(), APPS_LV_ITEM_PITCH);
    CHECK(strcmp(name_shown(), "\xc3\x9c" "ber timer") == 0, "the name as it is: %s", name_shown());
    fake_scroll(carousel_row(), 2 * APPS_LV_ITEM_PITCH);
    CHECK(strcmp(name_shown(), "abcdefghijklmnopqrstuvwx") == 0, "cut at 24 bytes: %s", name_shown());
    fake_scroll(carousel_row(), 3 * APPS_LV_ITEM_PITCH);
    CHECK(strcmp(name_shown(), "noname") == 0 && strcmp(fake_obj(item_parts(carousel_item(3)).letter)->text, "N") == 0,
          "no name: the id, and its letter: %s", name_shown());
    fake_scroll(carousel_row(), 4 * APPS_LV_ITEM_PITCH);
    CHECK(strcmp(name_shown(), "nullname") == 0, "NULL: the id: %s", name_shown());
    /* At rest, the centre's tile and its neighbours' are inside the circle too. */
    fake_scroll(carousel_row(), 2 * APPS_LV_ITEM_PITCH);
    for (int k = 1; k <= 3; k++) {
        int tile = item_parts(carousel_item(k)).tile;
        CHECK(inside_circle(tile), "item %d's tile: %d,%d - %d,%d", k, (int)fake_obj(tile)->area.x1, (int)fake_obj(tile)->area.y1,
              (int)fake_obj(tile)->area.x2, (int)fake_obj(tile)->area.y2);
    }
    LAUNCHER_END();
}

static void launcher_lists_apps_and_picks(void) {
    cards_scene();
    const apps_lv_app_t apps[] = {{"hello", "Hello", ""}, {"timer", "Kitchen timer", ""}};
    apps_lv_show_launcher(apps, 2, NULL, pick);
    int title = fake_label("Apps"), row = carousel_row();
    CHECK(title > 0 && row > 0, "the title and the carousel: %d %d\n%s", title, row, log_peek());
    int launcher = fake_parent(row);
    CHECK_CARD(launcher);
    CHECK(fake_index(launcher) == fake_index(1) + 1, "the launcher is right over the layer: %d %d", fake_index(launcher), fake_index(1));
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(), "shown");
    int item = carousel_item(0);
    CHECK(carousel_items((int[4]){0}, 4) == 2 && fake_flag(item, LV_OBJ_FLAG_CLICKABLE), "two items, clickable");
    CHECK(!fake_flag(title, LV_OBJ_FLAG_CLICKABLE) && !fake_flag(name_label(), LV_OBJ_FLAG_CLICKABLE),
          "labels aren't: LVGL gives their touches to what is under them");
    fake_tap(item);
    CHECK(picks == 1 && strcmp(picked, "hello") == 0, "tapping an app runs it: %d '%s'", picks, picked);
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown() && apps_lv_launcher_holding(),
          "and holds the launcher for the app's first frame, when it closes (C12; Task 6a)");
    LAUNCHER_END();
}

static void launcher_closes_on_a_tap_elsewhere(void) {
    cards_scene();
    const apps_lv_app_t apps[] = {{"hello", "Hello", ""}};
    apps_lv_show_launcher(apps, 1, NULL, pick);
    int launcher = fake_parent(carousel_row());
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(),
          "a tap on no app closes the launcher and runs nothing (C12): %d", picks);
    /* It opens again, items and all. */
    apps_lv_show_launcher(apps, 1, NULL, pick);
    CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && carousel_items((int[4]){0}, 4) == 1, "opens again");
    fake_tap(carousel_item(0));
    CHECK(picks == 1 && strcmp(picked, "hello") == 0, "and picks");
    LAUNCHER_END();
}

static void launcher_empty_state(void) {
    cards_scene();
    apps_lv_show_launcher(NULL, 0, NULL, pick);
    int empty = fake_label("No apps yet. Ask Muse to make one.");
    CHECK(empty > 0, "the empty state\n%s", log_peek());
    int launcher = fake_parent(empty);
    CHECK_CARD(launcher);
    CHECK(carousel_row() < 0, "no carousel");
    /* LVGL gives a tap on the text, which isn't clickable, to the launcher under it: that closes it (C12). */
    CHECK(!fake_flag(empty, LV_OBJ_FLAG_CLICKABLE), "the text isn't clickable");
    fake_tap(launcher);
    CHECK(picks == 0 && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN), "closed");
    LAUNCHER_END();
}

/* P3, and Task 5's edge rule: a swipe that begins on an app closes nothing, whichever way it goes (the bottom-edge close is the card's,
 * and an app is never in the bottom band). Followed through the lift, it picks nothing either: the rest of the touch is nobody's. */
static void launcher_a_swipe_on_an_app_closes_nothing(void) {
    static const struct {
        lv_dir_t dir;
        const char *name;
    } ways[] = {{LV_DIR_LEFT, "a left"}, {LV_DIR_RIGHT, "a right"}, {LV_DIR_TOP, "an up"}, {LV_DIR_BOTTOM, "a down"}};
    const apps_lv_app_t apps[] = {{"hello", "Hello", ""}};
    for (size_t i = 0; i < ARRAY_LEN(ways); i++) {
        cards_scene();
        apps_lv_show_launcher(apps, 1, NULL, pick);
        int item = carousel_item(0), launcher = fake_parent(carousel_row());
        fake_press(item);
        fake_swipe(ways[i].dir);
        CHECK(log_has_line("wait_release"), "%s swipe: the rest of the touch is nobody's\n%s", ways[i].name, log_peek());
        fake_lift();
        CHECK(picks == 0, "%s swipe on an app picks it: %d", ways[i].name, picks);
        CHECK(!fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(), "%s swipe closes the launcher", ways[i].name);
        LAUNCHER_END();
    }
}

static void launcher_keeps_its_own_copy(void) {
    cards_scene();
    char first[] = "alpha", first_name[] = "Alpha";
    const apps_lv_app_t apps[] = {{first, first_name, ""}, {"bravo", "Bravo", ""}};
    apps_lv_show_launcher(apps, 2, first, pick);
    memcpy(first, "zulu!", sizeof first);  /* the caller's text changes: the launcher kept a copy */
    memcpy(first_name, "Zulu!", sizeof first_name);
    fake_scroll(carousel_row(), APPS_LV_ITEM_PITCH);
    fake_scroll(carousel_row(), 0);
    CHECK(strcmp(name_shown(), "Alpha") == 0, "its own name: %s", name_shown());
    CHECK(item_parts(carousel_item(0)).dot > 0, "its own running id");
    fake_tap(carousel_item(0));
    CHECK(picks == 1 && strcmp(picked, "alpha") == 0, "the copy is picked: '%s'", picked);
    /* Shown again, it has the new items only: the old ones are gone, and no call is made on them. */
    int launcher = fake_parent(carousel_row());
    const apps_lv_app_t one[] = {{"solo", NULL, ""}};  /* no name: the id stands in */
    apps_lv_show_launcher(one, 1, NULL, pick);
    CHECK(carousel_items((int[4]){0}, 4) == 1 && fake_parent(carousel_row()) == launcher && strcmp(name_shown(), "solo") == 0 &&
              fake_label("Alpha") < 0,
          "rebuilt\n%s", log_peek());
    fake_tap(carousel_item(0));
    CHECK(picks == 2 && strcmp(picked, "solo") == 0, "picked: '%s'", picked);
    LAUNCHER_END();
}

/* LVGL had no memory for one of the launcher's objects, wherever in the build: what was made stays, no call is made on NULL, and an
 * item that is there shows its tile. */
static void launcher_survives_a_create_that_fails(void) {
    for (int at = 1; at <= 30; at++) {
        cards_scene();
        apps_lv_show_launcher(FIVE, 1, NULL, pick);  /* the cards, made once */
        apps_lv_hide_cards();
        log_clear();
        create_fail_in = at;
        apps_lv_show_launcher(FIVE, 5, "bravo", pick);
        create_fail_in = 0;
        CHECK(strchr(faces(), '-') == NULL && strchr(faces(), '2') == NULL, "failing at create %d: %s", at, faces());
        int row = carousel_row();
        if (row > 0) {
            fake_scroll(row, 2 * APPS_LV_ITEM_PITCH);
            fake_scroll(row, 0);
        }
        apps_lv_launcher_poll();
        CHECK(fake_errors == 0, "failing at create %d: %s", at, fake_note);
        LAUNCHER_END();
    }
}

/* Fix round 1, I1: a fling's snap animation (or an elastic end, or a drag still under the finger) goes on scrolling the row after
 * the launcher closes, whichever way it closes. Those scrolls ask nothing of the icons: no load is submitted, no block is held while
 * the launcher is closed, no image is given a source, and the hidden card isn't touched. */
static void a_scroll_after_a_close_asks_nothing(void) {
    static const char *const ways[] = {"a tap on the card", "a pick, and its hold's bound", "hide_cards", "a swipe up from the bottom edge",
                                       "close_launcher (the screen went dark)"};
    const apps_lv_app_t apps[] = {{"a1", "A1", "icon.bin"}, {"a2", "A2", "icon.bin"}, {"a3", "A3", "icon.bin"}, {"a4", "A4", "icon.bin"},
                                  {"a5", "A5", "icon.bin"}, {"a6", "A6", "icon.bin"}, {"a7", "A7", "icon.bin"}, {"a8", "A8", "icon.bin"}};
    for (size_t w = 0; w < ARRAY_LEN(ways); w++) {
        cards_scene();
        for (size_t i = 0; i < ARRAY_LEN(apps); i++) {
            put_icon(apps[i].id, CF_RGB565A8, 120, 120, (uint8_t)(0x10 + i));
        }
        apps_lv_show_launcher(apps, 8, NULL, pick);
        int row = carousel_row(), card = fake_parent(row);
        run_io();
        apps_lv_launcher_poll();
        fake_scroll(row, APPS_LV_ITEM_PITCH / 2);  /* the fling begins: its animation is on its way to a5 (fake_scroll moves the row
                                                    * with no animation the fake can see, so a tap on a2 still opens it) */
        CHECK(strcmp(faces(), "IITTTTTT") == 0 && held > 0, "%s: icons up before the close: %s", ways[w], faces());
        if (w == 0) {
            fake_tap(card);
        } else if (w == 1) {
            fake_tap(carousel_item(1));
            CHECK(picks == 1 && strcmp(picked, "a2") == 0 && apps_lv_launcher_holding(), "%s: picked, and held: '%s'", ways[w], picked);
            clock_us += (int64_t)APPS_LV_LAUNCH_HOLD_MS * 1000;  /* the app showed nothing: the hold ends at its bound */
            apps_lv_launcher_poll();
        } else if (w == 2) {
            apps_lv_hide_cards();  /* an app to the front (an alarm), or a voice turn */
        } else if (w == 3) {
            CHECK(swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, 0, -80) == card, "%s: on the card", ways[w]);
            fake_lift();
        } else {
            apps_lv_close_launcher();  /* the glue's, as the screen goes dark (the final review's I1) */
        }
        CHECK(!apps_lv_launcher_shown(), "%s: closed", ways[w]);
        run_io();
        int submits = io_submits, reads = store_reads;
        const char *name = name_shown();
        char before[32];
        snprintf(before, sizeof before, "%s", name);
        log_clear();
        for (int k = 1; k <= 8; k++) {  /* the animation's steps, on to the last app and an elastic end past it */
            fake_scroll(row, k * 5 * APPS_LV_ITEM_PITCH / 8);
            apps_lv_launcher_poll();
        }
        CHECK(io_submits == submits && io_queued == 0 && store_reads == reads, "%s: %d submits and %d reads after the close", ways[w],
              io_submits - submits, store_reads - reads);
        CHECK(log_peek()[0] == '\0', "%s: the hidden carousel was touched:\n%s", ways[w], log_peek());
        CHECK(strcmp(name_shown(), before) == 0 && strcmp(faces(), "TTTTTTTT") == 0, "%s: %s %s", ways[w], name_shown(), faces());
        CHECK_SOURCES();
        run_io();
        CHECK(held == 0, "%s: %zu bytes of icons held while the launcher is closed", ways[w], held);
        LAUNCHER_END();
    }
}

/* ---- Launcher carousel Task 5: the taps and the bottom-edge close ---- */

/* A tap on the app at the centre, with the row at rest, opens it and closes the launcher. The whole of its item is its: the face, and
 * under it, where the dot goes. Which app that is follows the scroll. */
static void tap_centre_launches(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int card = fake_parent(carousel_row());
    lv_point_t c = face_at(0);
    CHECK(c.x == SCREEN_CENTRE, "the first app is at the centre: %d", (int)c.x);
    CHECK(fake_tap_at(c.x, c.y) == carousel_item(0), "the tap lands on the first app");
    CHECK(picks == 1 && strcmp(picked, "alpha") == 0, "it opens: %d '%s'", picks, picked);
    CHECK(apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN), "and the launcher holds for the app's first frame");
    /* Under the face, in the item's box (the dot's place): the same app. */
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    fake_layout();
    lv_area_t box = fake_obj(carousel_item(0))->area, face = fake_obj(item_parts(carousel_item(0)).tile)->area;
    lv_point_t under = {SCREEN_CENTRE, box.y2 - 2};
    CHECK(under.y > face.y2, "under the face: %d, the face to %d", (int)under.y, (int)face.y2);
    CHECK(fake_tap_at(under.x, under.y) == carousel_item(0) && picks == 2 && strcmp(picked, "alpha") == 0,
          "a tap under the centre's face opens it: %d '%s'", picks, picked);
    /* Two apps along, brought there by a drag and at rest: a tap at the centre opens the third. */
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    c = face_at(0);
    fake_drag(c.x, c.y, c.x - 2 * APPS_LV_ITEM_PITCH, c.y, 20, 2);
    fake_lift();
    SETTLE();
    CHECK(scroll_now() == 2 * APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Charlie") == 0, "at rest on the third: %d %s",
          (int)scroll_now(), name_shown());
    c = face_at(2);
    CHECK(c.x == SCREEN_CENTRE && fake_tap_at(c.x, c.y) == carousel_item(2), "the tap lands on the third: %d", (int)c.x);
    CHECK(picks == 3 && strcmp(picked, "charlie") == 0, "and opens it: %d '%s'", picks, picked);
    /* The launcher closes under a finger on the centre (an app to the front, a voice turn: LVGL doesn't end a touch for a hide): its
     * lift opens nothing, and scrolls nothing. */
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    c = face_at(0);
    CHECK(fake_down(c.x, c.y) == carousel_item(0), "the press lands on the centre");
    apps_lv_hide_cards();
    log_clear();
    fake_lift();
    CHECK(picks == 3 && !apps_lv_launcher_shown() && log_peek()[0] == '\0', "a tap the launcher closed under does nothing: %d\n%s",
          picks, log_peek());
    LAUNCHER_END();
}

/* A tap on a side app brings it to the centre (lv_obj_scroll_to_view, animated) and opens nothing; at rest there, a tap opens it.
 * M2: an item's box is APPS_LV_ICON_PX wide and a neighbour's face about 70 px, and a tap in the box beside or above the face is the
 * neighbour's too: it only scrolls, so it can't open the wrong app. Between two boxes is the row, where a tap does nothing at all. */
static void tap_side_scrolls_not_launches(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row(), item1 = carousel_item(1);
    char line[48];
    snprintf(line, sizeof line, "scroll_to_view %d anim", item1);
    lv_point_t p = face_at(1);
    CHECK(fake_tap_at(p.x, p.y) == item1, "the tap lands on the second app");
    CHECK(picks == 0 && apps_lv_launcher_shown(), "a side app opens nothing: %d '%s'", picks, picked);
    CHECK(log_has_line(line) && fake_scrolling(row), "it is brought to the centre\n%s", log_peek());
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Bravo") == 0, "at the centre: %d %s", (int)scroll_now(),
          name_shown());
    CHECK_SCALES();
    /* And back, to the left. */
    p = face_at(0);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(0) && picks == 0, "the first app, at the left, opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == 0 && strcmp(name_shown(), "Alpha") == 0, "back: %d %s", (int)scroll_now(), name_shown());
    /* M2: beside the right neighbour's face, in its box. */
    fake_layout();
    lv_area_t box = fake_obj(item1)->area, face = fake_obj(item_parts(item1).tile)->area;
    lv_point_t beside = {box.x1 + 3, (face.y1 + face.y2 + 1) / 2};
    CHECK(in_area(&box, beside) && !in_area(&face, beside), "beside the face, in the box: %d of %d..%d, the face %d..%d", (int)beside.x,
          (int)box.x1, (int)box.x2, (int)face.x1, (int)face.x2);
    CHECK(fake_tap_at(beside.x, beside.y) == item1 && picks == 0 && fake_scrolling(row), "it scrolls, and opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH, "the second app at the centre: %d", (int)scroll_now());
    /* Above the left neighbour's face, in its box. */
    fake_layout();
    box = fake_obj(carousel_item(0))->area;
    face = fake_obj(item_parts(carousel_item(0)).tile)->area;
    lv_point_t above = {(face.x1 + face.x2 + 1) / 2, box.y1 + 3};
    CHECK(in_area(&box, above) && !in_area(&face, above), "above the face, in the box: %d", (int)above.y);
    CHECK(fake_tap_at(above.x, above.y) == carousel_item(0) && picks == 0, "it scrolls, and opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == 0, "the first app at the centre: %d", (int)scroll_now());
    /* Between the centre's box and the next: the row. Nothing opens, nothing scrolls, nothing closes. */
    fake_layout();
    lv_area_t box0 = fake_obj(carousel_item(0))->area;
    box = fake_obj(item1)->area;
    lv_point_t gap = {(box0.x2 + box.x1) / 2, (box0.y1 + box0.y2 + 1) / 2};
    CHECK(!in_area(&box0, gap) && !in_area(&box, gap), "between the boxes: %d", (int)gap.x);
    log_clear();
    CHECK(fake_tap_at(gap.x, gap.y) == row, "a tap between two apps is the row's");
    CHECK(picks == 0 && apps_lv_launcher_shown() && !fake_scrolling(row) && log_find("scroll_to_view") < 0,
          "and does nothing: %d picks, shown %d\n%s", picks, apps_lv_launcher_shown(), log_peek());
    /* At rest on the second app, a tap at the centre opens it. */
    p = face_at(1);
    fake_tap_at(p.x, p.y);
    SETTLE();
    p = face_at(1);
    CHECK(p.x == SCREEN_CENTRE && fake_tap_at(p.x, p.y) == item1 && picks == 1 && strcmp(picked, "bravo") == 0,
          "the second app, at rest at the centre, opens: %d '%s'", picks, picked);
    LAUNCHER_END();
}

/* Review Focus 4, and M3: a tap never opens an app while the row moves, or that lands while it moves (LVGL doesn't stop the snap's
 * animation for a tap, so the app under the finger at the press goes on moving). It brings the app it landed on to the centre instead.
 * During a drag (LVGL sends no click at the lift of a drag that scrolled), a fling's snap animation, an lv_obj_scroll_to_view animation
 * from a side tap, a press that lands while the row moves and lifts once it is at rest, and the moment between a drag's lift and its
 * throw's first step. */
static void tap_while_scrolling_does_not_launch(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    /* A drag from the second app to the centre. */
    lv_point_t p = face_at(1);
    fake_drag(p.x, p.y, p.x - APPS_LV_ITEM_PITCH, p.y, 10, 2);
    CHECK(fake_scrolling(row), "the drag scrolls the row");
    fake_lift();
    CHECK(picks == 0 && apps_lv_launcher_shown(), "a drag that ends at the centre opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Bravo") == 0, "at rest on the second: %d %s", (int)scroll_now(),
          name_shown());
    /* A drag on the centre app, away and back. */
    p = face_at(1);
    fake_down(p.x, p.y);
    fake_move(p.x - 15, p.y);
    fake_move(p.x - 30, p.y);
    fake_move(p.x - 15, p.y);
    fake_move(p.x, p.y);
    fake_move(p.x, p.y);
    fake_lift();
    CHECK(picks == 0, "a drag that comes back to where it began opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH, "still on the second: %d", (int)scroll_now());

    /* A fling: the lift's throw snaps the row on to the third app. A tap on it as it comes, then on the second as it goes. */
    p = face_at(1);
    fake_drag(p.x, p.y, p.x - 60, p.y, 4, 0);
    fake_lift();
    CHECK(throw_pending && picks == 0, "a fling: its throw waits for the next frame");
    fake_frame(FRAME_MS);
    CHECK(fake_scrolling(row) && n_anims == 1, "the snap animates");
    p = box_at(2);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(2), "the tap lands on the third app, on its way");
    CHECK(picks == 0 && apps_lv_launcher_shown(), "a tap during the snap opens nothing: %d '%s'", picks, picked);
    fake_frame(FRAME_MS);
    p = box_at(1);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(1) && picks == 0, "nor one on the app it leaves: %d", picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Bravo") == 0, "the app tapped last is at the centre: %d %s",
          (int)scroll_now(), name_shown());

    /* lv_obj_scroll_to_view's animation, from a tap on the right neighbour: a tap on it on its way, and on the app it leaves. */
    p = face_at(2);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(2) && fake_scrolling(row) && picks == 0, "a side tap scrolls");
    fake_frame(FRAME_MS);
    p = box_at(2);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(2) && picks == 0, "a tap on it on its way opens nothing: %d", picks);
    fake_frame(FRAME_MS);
    p = box_at(1);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(1) && picks == 0, "nor one on the app it leaves: %d", picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH, "back on the second: %d", (int)scroll_now());

    /* A press that lands while the row moves and lifts once it is at rest, on the app now at the centre. */
    p = face_at(2);
    fake_tap_at(p.x, p.y);
    fake_frame(FRAME_MS);
    p = box_at(2);
    CHECK(fake_down(p.x, p.y) == carousel_item(2), "the press lands on the third app, on its way");
    SETTLE();
    CHECK(scroll_now() == 2 * APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Charlie") == 0, "at rest under the finger: %d %s",
          (int)scroll_now(), name_shown());
    fake_lift();
    CHECK(picks == 0 && apps_lv_launcher_shown() && !fake_scrolling(row), "a press made while the row moved opens nothing: %d", picks);

    /* Between a drag's lift and its throw's first step, LVGL still holds the row as the touch's scroll: a tap then takes that step
     * (the snap animation starts, and goes on), and opens nothing. */
    p = face_at(2);
    fake_drag(p.x, p.y, p.x - 60, p.y, 4, 0);
    fake_lift();
    CHECK(throw_pending && fake_scrolling(row), "the throw waits");
    p = box_at(3);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(3) && picks == 0, "a tap before the throw's first step opens nothing: %d", picks);
    CHECK(!throw_pending, "the press took the throw's first step");
    SETTLE();
    CHECK(scroll_now() == 3 * APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Delta") == 0, "at rest on the fourth: %d %s",
          (int)scroll_now(), name_shown());

    /* The row starts moving under a finger that went down on the centre at rest: code that scrolls it (none does yet; a re-centre
     * when the list changes would) makes the tap at the lift one made while the row moves, which opens nothing. */
    p = face_at(3);
    CHECK(fake_down(p.x, p.y) == carousel_item(3), "the press lands on the centre, at rest");
    lv_obj_scroll_to_view(fake_obj(carousel_item(4)), LV_ANIM_ON);
    fake_lift();
    CHECK(picks == 0 && apps_lv_launcher_shown(), "a tap that lifts while the row moves opens nothing: %d", picks);
    SETTLE();
    CHECK(scroll_now() == 3 * APPS_LV_ITEM_PITCH, "the tapped app was brought back: %d", (int)scroll_now());

    /* At rest, a tap at the centre opens what is there. */
    p = face_at(3);
    CHECK(fake_tap_at(p.x, p.y) == carousel_item(3) && picks == 1 && strcmp(picked, "delta") == 0, "at rest, it opens: %d '%s'", picks,
          picked);
    LAUNCHER_END();
}

/* The bottom-edge close: a swipe up whose press went down within APPS_LV_CLOSE_EDGE_PX of the screen's bottom, inside the circle,
 * closes the launcher as the gesture comes (before the lift), drops its icons and opens nothing; the lift is the card's PRESS_LOST
 * alone. The empty and the loading launcher close so too. */
static void bottom_edge_swipe_up_closes(void) {
    static const struct {
        int32_t x, y;
        const char *what;
    } starts[] = {
        {SCREEN_CENTRE, EDGE_TOP + 14, "the band's middle"},
        {SCREEN_CENTRE, EDGE_TOP, "its top row"},
        {SCREEN_CENTRE, 2 * SCREEN_CENTRE - 1, "the screen's last row"},
        {150, EDGE_TOP + 14, "towards the band's left end"},
        {316, EDGE_TOP + 14, "towards its right end"},
    };
    const apps_lv_app_t apps[] = {{"a1", "A1", "icon.bin"}, {"a2", "A2", "icon.bin"}, {"a3", "A3", ""}};
    for (size_t i = 0; i < ARRAY_LEN(starts); i++) {
        cards_scene();
        put_icon("a1", CF_RGB565A8, 120, 120, 0x11);
        put_icon("a2", CF_RGB565A8, 120, 120, 0x22);
        apps_lv_show_launcher(apps, 3, NULL, pick);
        run_io();
        apps_lv_launcher_poll();
        int card = fake_parent(carousel_row());
        CHECK(strcmp(faces(), "IIT") == 0 && held > 0, "%s: icons up first: %s", starts[i].what, faces());
        log_clear();
        CHECK(swipe_from(starts[i].x, starts[i].y, 0, -80) == card, "%s: the band is the card's", starts[i].what);
        CHECK(!apps_lv_launcher_shown() && fake_flag(card, LV_OBJ_FLAG_HIDDEN), "%s: a swipe up closes it, before the lift",
              starts[i].what);
        CHECK(log_has_line("wait_release"), "%s: and the rest of the touch is nobody's\n%s", starts[i].what, log_peek());
        CHECK(strcmp(faces(), "TTT") == 0, "%s: tiles again: %s", starts[i].what, faces());
        CHECK_SOURCES();
        fake_lift();
        CHECK(picks == 0, "%s: nothing opens: %d", starts[i].what, picks);
        run_io();
        CHECK(held == 0, "%s: %zu bytes of icons held after the close", starts[i].what, held);
        LAUNCHER_END();
    }
    static const int ns[] = {0, APPS_LV_LOADING};
    for (size_t i = 0; i < ARRAY_LEN(ns); i++) {
        cards_scene();
        apps_lv_show_launcher(NULL, ns[i], NULL, pick);
        int card = fake_parent(fake_label(ns[i] == 0 ? "No apps yet. Ask Muse to make one." : "Loading apps..."));
        CHECK(swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, 0, -80) == card && !apps_lv_launcher_shown(), "n %d: a swipe up closes it", ns[i]);
        fake_lift();
        CHECK(picks == 0, "n %d: nothing opens", ns[i]);
        LAUNCHER_END();
    }
}

/* A swipe up from anywhere but the bottom band closes nothing, and nothing of the touch is heard after it (no tap, no app opened): a
 * row above the band, the name, the app at the centre, between two apps, the top; and in the band, outside the circle. Each has a twin
 * that starts a row, or 10 px, inside the rule and closes, so that what is tested is the rule and not that nothing closes. A swipe from
 * the band that isn't up leaves nothing behind for the next touch. */
static void swipe_up_from_middle_does_nothing(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    lv_point_t centre = face_at(0);
    lv_area_t box0 = fake_obj(carousel_item(0))->area, box1 = fake_obj(carousel_item(1))->area;
    const int32_t gap_x = (box0.x2 + box1.x1) / 2, band = EDGE_TOP + 14, name_y = name_middle_y();
    LAUNCHER_END();
    const struct {
        int32_t x, y, twin_x, twin_y;
        const char *what;
    } starts[] = {
        {SCREEN_CENTRE, EDGE_TOP - 1, SCREEN_CENTRE, EDGE_TOP, "a row above the band"},
        {SCREEN_CENTRE, name_y, SCREEN_CENTRE, band, "the name"},
        {centre.x, centre.y, SCREEN_CENTRE, band, "the app at the centre"},
        {gap_x, centre.y, SCREEN_CENTRE, band, "between two apps"},
        {SCREEN_CENTRE, 100, SCREEN_CENTRE, band, "the top"},
        {120, band, 130, band, "the band, outside the circle on the left"},
        {346, band, 336, band, "the band, outside the circle on the right"},
    };
    for (size_t i = 0; i < ARRAY_LEN(starts); i++) {
        cards_scene();
        apps_lv_show_launcher(FIVE, 5, NULL, pick);
        log_clear();
        swipe_from(starts[i].x, starts[i].y, 0, -80);
        CHECK(apps_lv_launcher_shown() && log_has_line("wait_release"), "%s: a swipe up is a gesture that closes nothing (shown %d)\n%s",
              starts[i].what, apps_lv_launcher_shown(), log_peek());
        fake_lift();
        CHECK(apps_lv_launcher_shown() && picks == 0, "%s: nor does its lift: shown %d, %d picks", starts[i].what,
              apps_lv_launcher_shown(), picks);
        swipe_from(starts[i].twin_x, starts[i].twin_y, 0, -80);
        CHECK(!apps_lv_launcher_shown(), "%s: its twin, from (%d, %d), closes", starts[i].what, (int)starts[i].twin_x,
              (int)starts[i].twin_y);
        fake_lift();
        LAUNCHER_END();
    }
    /* The press that counts is this touch's own. */
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    swipe_from(SCREEN_CENTRE, band, -80, 0);
    fake_lift();
    swipe_from(SCREEN_CENTRE, band, 80, 0);
    fake_lift();
    CHECK(apps_lv_launcher_shown() && scroll_now() == 0, "sideways from the band: nothing closes or scrolls");
    swipe_from(centre.x, centre.y, 0, -80);
    fake_lift();
    CHECK(apps_lv_launcher_shown() && picks == 0, "the next touch, up from the app at the centre, closes nothing: %d", picks);
    swipe_from(gap_x, centre.y, 0, -80);
    fake_lift();
    CHECK(apps_lv_launcher_shown(), "nor one up from between two apps (the card hears no press of the row's)");
    swipe_from(SCREEN_CENTRE, band, 0, -80);
    CHECK(!apps_lv_launcher_shown(), "and one up from the band closes");
    fake_lift();
    LAUNCHER_END();
}

/* P3's pin: the right-swipe close is gone. On the row a right swipe scrolls back; on the card (under the name, or from the bottom band) it
 * is a gesture, and closes nothing. */
static void right_swipe_no_longer_closes(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    lv_point_t p = face_at(0);
    fake_drag(p.x, p.y, p.x - 2 * APPS_LV_ITEM_PITCH, p.y, 20, 2);
    fake_lift();
    SETTLE();
    CHECK(scroll_now() == 2 * APPS_LV_ITEM_PITCH, "two apps along, so that a right swipe has somewhere to go: %d", (int)scroll_now());
    p = face_at(2);
    fake_drag(p.x, p.y, p.x + 100, p.y, 10, 1);
    CHECK(fake_scrolling(row), "on the row, a right swipe scrolls");
    fake_lift();
    CHECK(apps_lv_launcher_shown() && picks == 0, "and closes nothing: shown %d, %d picks", apps_lv_launcher_shown(), picks);
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Bravo") == 0, "it went back an app: %d %s", (int)scroll_now(),
          name_shown());
    static const lv_point_t on_card[] = {{SCREEN_CENTRE, 360}, {SCREEN_CENTRE, EDGE_TOP + 14}};
    for (size_t i = 0; i < ARRAY_LEN(on_card); i++) {
        log_clear();
        swipe_from(on_card[i].x, on_card[i].y, 80, 0);
        CHECK(apps_lv_launcher_shown() && log_has_line("wait_release"), "from (%d, %d): a gesture that closes nothing",
              (int)on_card[i].x, (int)on_card[i].y);
        fake_lift();
        CHECK(apps_lv_launcher_shown() && picks == 0 && scroll_now() == APPS_LV_ITEM_PITCH, "from (%d, %d): nothing more",
              (int)on_card[i].x, (int)on_card[i].y);
    }
    LAUNCHER_END();
}

/* A sideways swipe on the row only scrolls: the items follow the finger at their scales, LVGL sends no gesture while it scrolls and
 * no click at the lift, and the snap brings the next app to the centre. Off the row it moves nothing and closes nothing. And from the
 * bottom band only up closes, which its twin shows. */
static void horizontal_swipe_only_scrolls(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    lv_point_t p = face_at(0);
    log_clear();
    fake_down(p.x, p.y);
    for (int i = 1; i <= 16; i++) {
        fake_move(p.x - 10 * i, p.y);
        if (i == 8) {
            CHECK(fake_scrolling(row) && scroll_now() > 0, "the row follows the finger: %d", (int)scroll_now());
            CHECK_SCALES();
        }
    }
    fake_move(p.x - 160, p.y);
    CHECK(log_find("wait_release") < 0, "no gesture while the row scrolls\n%s", log_peek());
    fake_lift();
    CHECK(picks == 0 && apps_lv_launcher_shown(), "no tap at the lift, no close: %d picks, shown %d", picks, apps_lv_launcher_shown());
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "Bravo") == 0, "the next app came to the centre: %d %s",
          (int)scroll_now(), name_shown());
    CHECK_SCALES();
    /* Off the row: the name, the title, under them. */
    const lv_point_t off_row[] = {{SCREEN_CENTRE, name_middle_y()}, {SCREEN_CENTRE, 66}, {SCREEN_CENTRE, 360}};
    for (size_t i = 0; i < ARRAY_LEN(off_row); i++) {
        for (int way = -1; way <= 1; way += 2) {
            log_clear();
            swipe_from(off_row[i].x, off_row[i].y, way * 80, 0);
            fake_lift();
            CHECK(apps_lv_launcher_shown() && picks == 0 && scroll_now() == APPS_LV_ITEM_PITCH && log_has_line("wait_release"),
                  "a swipe %d from (%d, %d): a gesture that moves and closes nothing", way, (int)off_row[i].x, (int)off_row[i].y);
        }
    }
    /* From the bottom band, sideways closes nothing; up closes. */
    swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, -80, 0);
    fake_lift();
    swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, 80, 0);
    fake_lift();
    CHECK(apps_lv_launcher_shown(), "sideways from the band closes nothing");
    swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, 0, -80);
    CHECK(!apps_lv_launcher_shown(), "up from the band closes");
    fake_lift();
    LAUNCHER_END();
}

/* ---- Task 5 fix round 1, I1: a tap is a touch that lifts where it went down ---- */

/* A slide off the row: from (x, y), dx sideways in steps reads (at 10 px a read it is under LVGL's 50 px gesture; at 2 px a read every
 * read starts the gesture again, lv_indev.c:1787), then the lift. Neither becomes a gesture, so LVGL sends the card CLICKED. */
static void slide_from(int32_t x, int32_t y, int32_t dx, int32_t dy, int steps) {
    fake_drag(x, y, x + dx, y + dy, steps, 0);
    fake_lift();
}

/* The sideways drags I1 names, short and fast or long and slow, each way, from a point on the card: none closes the launcher. */
static void slides_close_nothing_at(int line, int32_t x, int32_t y, const char *where) {
    static const struct {
        int32_t dx;
        int steps;
        const char *what;
    } slides[] = {{40, 4, "40 px at 10 px a read"}, {-40, 4, "40 px left"}, {80, 40, "80 px at 2 px a read"}, {-80, 40, "80 px left, slowly"},
                  {FAKE_SCROLL_LIMIT, 5, "the scroll limit, slowly"}};
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int card = fake_parent(carousel_row());
    for (size_t i = 0; i < ARRAY_LEN(slides); i++) {
        log_clear();
        CHECK_AT(line, fake_down(x, y) == card, "%s is the card's", where);
        for (int k = 1; k <= slides[i].steps; k++) {
            fake_move(x + slides[i].dx * k / slides[i].steps, y);
        }
        CHECK_AT(line, log_find("wait_release") < 0, "%s, %s: no gesture (so LVGL clicks the card at the lift)\n%s", where,
                 slides[i].what, log_peek());
        fake_lift();
        CHECK_AT(line, apps_lv_launcher_shown() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN) && picks == 0,
                 "%s, %s: the slide closes nothing (shown %d, %d picks)", where, slides[i].what, apps_lv_launcher_shown(), picks);
        CHECK_AT(line, scroll_now() == 0, "%s, %s: and scrolls nothing: %d", where, slides[i].what, (int)scroll_now());
    }
    LAUNCHER_END();
}

static void a_short_slow_sideways_drag_on_the_background_does_not_close(void) {
    slides_close_nothing_at(__LINE__, SCREEN_CENTRE, 360, "the background under the name");
    slides_close_nothing_at(__LINE__, SCREEN_CENTRE, 110, "the background over the row");
}

static void a_short_slow_sideways_drag_on_the_name_does_not_close(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    fake_layout();
    const lv_area_t *name = &fake_obj(name_label())->area;
    int32_t y = (name->y1 + name->y2 + 1) / 2;
    CHECK(in_area(name, (lv_point_t){SCREEN_CENTRE, y}) && !fake_flag(name_label(), LV_OBJ_FLAG_CLICKABLE),
          "(233, %d) is on the name, which passes its touches to the card", (int)y);
    LAUNCHER_END();
    slides_close_nothing_at(__LINE__, SCREEN_CENTRE, y, "the name");
}

/* A real tap on the background still closes (C12): one that lifts where it went down, and one whose finger rolled less than LVGL's
 * scroll limit on each axis (the movement at which LVGL makes a drag a scroll and no click). At the limit it is a slide, and closes
 * nothing. */
static void a_tap_on_the_background_still_closes(void) {
    static const struct {
        int32_t dx, dy;
        bool closes;
    } lifts[] = {{0, 0, true},
                 {FAKE_SCROLL_LIMIT - 1, 0, true},
                 {0, -(FAKE_SCROLL_LIMIT - 1), true},
                 {-(FAKE_SCROLL_LIMIT - 1), FAKE_SCROLL_LIMIT - 1, true},
                 {FAKE_SCROLL_LIMIT, 0, false},
                 {0, FAKE_SCROLL_LIMIT, false},
                 {-FAKE_SCROLL_LIMIT, -3, false}};
    /* Under the name and at the title (a tap on the name opens the centre app: a_tap_on_the_name_opens_the_centre_app). */
    const lv_point_t at[] = {{SCREEN_CENTRE, 360}, {SCREEN_CENTRE, 66}};
    for (size_t a = 0; a < ARRAY_LEN(at); a++) {
        for (size_t i = 0; i < ARRAY_LEN(lifts); i++) {
            cards_scene();
            apps_lv_show_launcher(FIVE, 5, NULL, pick);
            int card = fake_parent(carousel_row());
            CHECK(fake_down(at[a].x, at[a].y) == card, "(%d, %d) is the card's", (int)at[a].x, (int)at[a].y);
            if (lifts[i].dx != 0 || lifts[i].dy != 0) {
                fake_move(at[a].x + lifts[i].dx, at[a].y + lifts[i].dy);
            }
            fake_lift();
            CHECK(apps_lv_launcher_shown() == !lifts[i].closes && picks == 0, "a lift %d, %d from (%d, %d) %s (shown %d)", (int)lifts[i].dx,
                  (int)lifts[i].dy, (int)at[a].x, (int)at[a].y, lifts[i].closes ? "is a tap, and closes" : "is a slide, and closes nothing",
                  apps_lv_launcher_shown());
            LAUNCHER_END();
        }
    }
    /* The empty and the loading launcher: the same. */
    static const int ns[] = {0, APPS_LV_LOADING};
    for (size_t i = 0; i < ARRAY_LEN(ns); i++) {
        cards_scene();
        apps_lv_show_launcher(NULL, ns[i], NULL, pick);
        slide_from(SCREEN_CENTRE, SCREEN_CENTRE, 40, 0, 4);
        CHECK(apps_lv_launcher_shown(), "n %d: a slide on the text closes nothing", ns[i]);
        fake_tap_at(SCREEN_CENTRE, SCREEN_CENTRE);
        CHECK(!apps_lv_launcher_shown(), "n %d: a tap on it closes", ns[i]);
        LAUNCHER_END();
    }
}

/* The same root on the apps: a slide up or down on an app (the row scrolls only sideways, so LVGL sends the app CLICKED when the slide
 * is short or slow) opens nothing and scrolls nothing. A tap whose finger rolled less than the scroll limit still opens the centre. */
static void a_slide_on_an_app_opens_nothing(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    static const struct {
        int k;
        int32_t dy;
        int steps;
        const char *what;
    } slides[] = {{0, -40, 4, "40 px up the centre app"}, {0, 40, 20, "40 px down it, slowly"}, {0, -60, 30, "60 px up it at 2 px a read"},
                  {1, -40, 4, "40 px up the side app"}, {1, 60, 30, "60 px down it, slowly"}};
    for (size_t i = 0; i < ARRAY_LEN(slides); i++) {
        lv_point_t p = face_at(slides[i].k);
        log_clear();
        slide_from(p.x, p.y, 0, slides[i].dy, slides[i].steps);
        CHECK(picks == 0 && apps_lv_launcher_shown() && !fake_scrolling(row) && log_find("scroll_to_view") < 0 && scroll_now() == 0,
              "%s: opens and scrolls nothing (%d picks, scroll %d)\n%s", slides[i].what, picks, (int)scroll_now(), log_peek());
    }
    lv_point_t c = face_at(0);
    fake_down(c.x, c.y);
    fake_move(c.x + FAKE_SCROLL_LIMIT - 1, c.y - (FAKE_SCROLL_LIMIT - 1));
    fake_lift();
    CHECK(picks == 1 && strcmp(picked, "alpha") == 0, "a tap that rolled under the scroll limit opens the centre: %d '%s'", picks, picked);
    LAUNCHER_END();
}

/* ---- Launcher carousel Task 6a: a clearly larger centre, and the launch hold ---- */

/* What item k's face covers on the screen now: its tile's box, or the picture its icon's image draws (fake_drawn: LVGL's image scale,
 * about the middle of the box). */
static lv_area_t face_area(int k) {
    fake_layout();
    int item = carousel_item(k);
    item_parts_t p = item_parts(item);
    return item_face(item) == 'I' ? fake_drawn(p.image) : fake_obj(p.tile)->area;
}

static int32_t area_w(lv_area_t a) {
    return a.x2 - a.x1 + 1;
}

/* Every corner of a is inside the round screen. */
static bool area_inside_circle(lv_area_t a) {
    const int32_t xs[2] = {a.x1, a.x2 + 1}, ys[2] = {a.y1, a.y2 + 1};
    for (int i = 0; i < 4; i++) {
        int32_t dx = xs[i & 1] - SCREEN_CENTRE, dy = ys[i >> 1] - SCREEN_CENTRE;
        if (dx * dx + dy * dy > SCREEN_CENTRE * SCREEN_CENTRE) {
            return false;
        }
    }
    return true;
}

/* Nothing of a is on the screen's columns. */
static bool area_off_screen(lv_area_t a) {
    return a.x1 >= 2 * SCREEN_CENTRE || a.x2 < 0;
}

/* The user's word on preview 2: the app at the centre is clearly larger than the two beside it. At rest on the first app, the third and
 * the last, with tiles and with icons of 160 px (the size the guide asks for since Task 6: the centre's own) and of 120 px (what apps were
 * made at before it, drawn larger at the centre): the centre's face is 150 to 160 px
 * across and a neighbour's at most 40% of it; each neighbour is wholly on the screen and inside its circle, at least 12 px from the
 * centre's face; the apps two away are off the screen; the centre's face, its running dot, the name and the title are inside the circle,
 * and the title is clear above the face. */
static void centre_is_clearly_larger(void) {
    CHECK(APPS_LV_ICON_PX >= 150 && APPS_LV_ICON_PX <= 160, "the centre's box is 150 to 160 px: %d", APPS_LV_ICON_PX);
    CHECK(APPS_LV_ICON_SIDE_SCALE <= 102, "a neighbour's scale is at most 102 of 256 (40%%): %d", APPS_LV_ICON_SIDE_SCALE);
    const apps_lv_app_t tiles[] = {{"c1", "C1", ""}, {"c2", "C2", ""}, {"c3", "C3", ""}, {"c4", "C4", ""}, {"c5", "C5", ""}};
    const apps_lv_app_t icons[] = {{"c1", "C1", "icon.bin"}, {"c2", "C2", "icon.bin"}, {"c3", "C3", "icon.bin"}, {"c4", "C4", "icon.bin"},
                                   {"c5", "C5", "icon.bin"}};
    for (int kind = 0; kind < 3; kind++) {
        const char *what = kind == 0 ? "tiles" : kind == 1 ? "icons of 160 px" : "icons of 120 px";
        const unsigned px = kind == 1 ? 160 : 120;
        for (int k = 0; k < 5; k += 2) {
            cards_scene();
            if (kind > 0) {
                for (int i = 0; i < 5; i++) {
                    put_icon(icons[i].id, CF_RGB565A8, px, px, (uint8_t)(0x10 + i));
                }
            }
            apps_lv_show_launcher(kind == 0 ? tiles : icons, 5, (kind == 0 ? tiles : icons)[k].id, pick);
            fake_scroll(carousel_row(), k * APPS_LV_ITEM_PITCH);  /* at rest there, as the snap leaves it */
            for (int round = 0; round < 4; round++) {
                run_io();
                apps_lv_launcher_poll();
            }
            CHECK_SCALES();
            const char face = kind == 0 ? 'T' : 'I';
            lv_area_t c = face_area(k);
            CHECK(item_face(carousel_item(k)) == face, "%s, at %d: the centre's face is its %c: %s", what, k, face, faces());
            CHECK(area_w(c) >= 150 && area_w(c) <= 160 && area_inside_circle(c), "%s, at %d: the centre's face is %d px, %d,%d - %d,%d",
                  what, k, (int)area_w(c), (int)c.x1, (int)c.y1, (int)c.x2, (int)c.y2);
            for (int way = -1; way <= 1; way += 2) {
                int n = k + way, far = k + 2 * way;
                if (n < 0 || n >= 5) {
                    continue;
                }
                lv_area_t a = face_area(n);
                CHECK(item_face(carousel_item(n)) == face, "%s, at %d: the neighbour %d's face is its %c: %s", what, k, n, face, faces());
                CHECK(area_w(a) * 10 <= 4 * area_w(c), "%s, at %d: the neighbour %d is %d px of the centre's %d: over 40%%", what, k, n,
                      (int)area_w(a), (int)area_w(c));
                CHECK(area_inside_circle(a), "%s, at %d: the neighbour %d is inside the circle: %d,%d - %d,%d", what, k, n, (int)a.x1,
                      (int)a.y1, (int)a.x2, (int)a.y2);
                int32_t gap = way > 0 ? a.x1 - c.x2 - 1 : c.x1 - a.x2 - 1;
                CHECK(gap >= 12, "%s, at %d: %d px between the neighbour %d and the centre", what, k, (int)gap, n);
                if (far >= 0 && far < 5) {
                    lv_area_t f = face_area(far);
                    CHECK(area_off_screen(f), "%s, at %d: the app two away (%d) is off the screen: %d - %d", what, k, far, (int)f.x1,
                          (int)f.x2);
                }
            }
            int title = fake_label("Apps"), name = name_label(), dot = item_parts(carousel_item(k)).dot;
            CHECK(title > 0 && name > 0 && dot > 0, "%s, at %d: the title, the name and the dot: %d %d %d", what, k, title, name, dot);
            if (title > 0 && name > 0 && dot > 0) {
                CHECK(inside_circle(name) && inside_circle(dot) && inside_circle(title), "%s, at %d: the name %d, the dot %d, the title %d "
                      "inside the circle", what, k, inside_circle(name), inside_circle(dot), inside_circle(title));
                CHECK(fake_obj(title)->area.y2 < c.y1, "%s, at %d: the title (to %d) clear of the centre's face (from %d)", what, k,
                      (int)fake_obj(title)->area.y2, (int)c.y1);
                CHECK(fake_obj(dot)->area.y1 > c.y2 && fake_obj(name)->area.y1 > fake_obj(dot)->area.y2,
                      "%s, at %d: the face, then the dot, then the name", what, k);
            }
            LAUNCHER_END();
        }
    }
}

static const apps_lv_app_t HOLD_APPS[] = {{"h1", "H1", "icon.bin"}, {"h2", "H2", "icon.bin"}, {"h3", "H3", "icon.bin"},
                                          {"h4", "H4", ""},         {"h5", "H5", "icon.bin"}};

/* The launcher over the clock, its first two icons in and two more of its window loading on the worker, looked at for browse_us; then a
 * tap on the app at the centre, which picks it. before and *drawn are what it showed before the tap. */
static void hold_scene_at(int line, int64_t browse_us, char before[8], int *drawn) {
    cards_scene();
    for (size_t i = 0; i < ARRAY_LEN(HOLD_APPS); i++) {
        if (HOLD_APPS[i].icon[0] != '\0') {
            put_icon(HOLD_APPS[i].id, CF_RGB565A8, 120, 120, (uint8_t)(0x10 + i));
        }
    }
    apps_lv_show_launcher(HOLD_APPS, 5, NULL, pick);
    run_io();
    apps_lv_launcher_poll();
    CHECK_AT(line, strcmp(faces(), "IITTT") == 0 && io_queued == 2, "two icons in, the next two of the window (h3, h5) on their way: %s, "
             "%d queued", faces(), io_queued);
    snprintf(before, 8, "%s", faces());
    *drawn = fake_draw();
    clock_us += browse_us;
    lv_point_t c = face_at(0);
    CHECK_AT(line, fake_tap_at(c.x, c.y) == carousel_item(0) && picks == 1 && strcmp(picked, "h1") == 0, "the centre is picked: %d '%s'",
             picks, picked);
}
#define HOLD_SCENE(before, drawn) hold_scene_at(__LINE__, 0, (before), (drawn))

/* A pick doesn't close the launcher: it holds it as it was, over the clock, while the app loads, and nothing apps_lv does by itself ends
 * that before the app's first frame: the app's table coming, passes of the apply step behind it and in front of it, a pass skipped for a
 * callback, the layer coming to the front under it, frames of the icons' poll. The app task's glue ends it at that frame
 * (tests/test_muse_apps_task.py), with apps_lv_hide_cards, which closes it as any close does in the same call: the icons dropped, every
 * item its tile, and the app's widget, on the layer in front, in its place. */
static void launch_holds_the_launcher_until_the_first_frame(void) {
    char before[8];
    int drawn;
    HOLD_SCENE(before, &drawn);
    int card = fake_parent(carousel_row());
    CHECK(apps_lv_launcher_shown() && apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN) && apps_lv_cards_shown(),
          "the pick holds the launcher: shown %d, holding %d", apps_lv_launcher_shown(), apps_lv_launcher_holding());
    CHECK(strcmp(faces(), before) == 0 && fake_draw() == drawn, "as it was: %s, %d icons drawn", faces(), fake_draw());
    CHECK_SOURCES();
    env_t e;
    CHECK(env_open(&e, ASSETS, 2), "env_open");  /* the app's table, published */
    RUN(&e, "ui.label{text = 'Hi'}");
    CHECK(APPLY(&e) && apps_lv_launcher_holding(), "a pass behind the launcher: it holds");
    apps_lv_set_front(true);
    CHECK(apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN), "the layer to the front, under it: it holds");
    CHECK(!APPLY_CB(&e) && apps_lv_launcher_holding(), "a pass skipped for a callback: it holds");
    CHECK(APPLY(&e) && apps_lv_launcher_holding(), "a pass with the layer in front: it holds (the glue's to end)");
    for (int frame = 0; frame < 5; frame++) {
        apps_lv_launcher_poll();
    }
    CHECK(apps_lv_launcher_holding() && strcmp(faces(), before) == 0 && fake_draw() == drawn, "frames of the poll: it holds, as it was: %s",
          faces());
    int label = fake_label("Hi");
    CHECK(label > 0 && fake_index(card) > fake_index(1), "the app's widget is on the layer, under the launcher");
    apps_lv_hide_cards();  /* the glue, at the app's first frame */
    CHECK(!apps_lv_launcher_shown() && !apps_lv_launcher_holding() && fake_flag(card, LV_OBJ_FLAG_HIDDEN), "it closes");
    CHECK(label > 0 && fake_shown(label), "the app's widget shows in its place");
    CHECK(strcmp(faces(), "TTTTT") == 0 && apps_icons_get("h1") == NULL && apps_icons_get("h2") == NULL,
          "its icons dropped, every item its tile: %s", faces());
    CHECK_SOURCES();
    apps_lv_reset();
    env_close(&e);
    LAUNCHER_END();
}

/* While the launcher holds it takes no input and changes nothing: a tap on the app at the centre picks nothing more, a tap on a side app
 * brings nothing to the centre, a drag along the row scrolls nothing (the row doesn't scroll while it holds), and a swipe up from the
 * bottom edge, a tap on the card and a slide on it close nothing. Its icons on the screen stay as they are, none is asked for, and the
 * two loads that were in flight at the pick (h3, h5: off the screen) end freed on the worker, never swapped in (fix round 1). The close
 * then drops the rest. */
static void no_input_while_the_launch_holds(void) {
    char before[8];
    int drawn;
    HOLD_SCENE(before, &drawn);
    int row = carousel_row(), card = fake_parent(row), submits = io_submits;
    lv_point_t c = face_at(0), side = face_at(1);
    CHECK(fake_tap_at(c.x, c.y) == carousel_item(0) && picks == 1, "a tap on the centre again picks nothing: %d", picks);
    log_clear();
    CHECK(fake_tap_at(side.x, side.y) == carousel_item(1) && log_find("scroll_to_view") < 0 && !fake_scrolling(row),
          "a tap on a side app brings nothing to the centre\n%s", log_peek());
    fake_drag(c.x, c.y, c.x - 2 * APPS_LV_ITEM_PITCH, c.y, 20, 2);
    CHECK(!fake_scrolling(row) && scroll_now() == 0, "a drag along the row scrolls nothing: %d", (int)scroll_now());
    fake_lift();
    SETTLE();
    CHECK(scroll_now() == 0 && strcmp(name_shown(), "H1") == 0 && picks == 1, "still on the first: %d %s, %d picks", (int)scroll_now(),
          name_shown(), picks);
    CHECK_SCALES();
    CHECK(swipe_from(SCREEN_CENTRE, EDGE_TOP + 14, 0, -80) == card && apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN),
          "a swipe up from the bottom edge closes nothing");
    fake_lift();
    CHECK(fake_tap_at(SCREEN_CENTRE, 360) == card && apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN),
          "nor a tap on the card");
    slide_from(SCREEN_CENTRE, 360, 40, 0, 4);
    CHECK(apps_lv_launcher_holding() && picks == 1, "nor a slide on it");
    run_io();  /* the two loads that were in flight end: orphaned at the pick, they free their blocks */
    for (int frame = 0; frame < 3; frame++) {
        apps_lv_launcher_poll();
    }
    CHECK(strcmp(faces(), before) == 0 && fake_draw() == drawn && io_submits == submits,
          "its icons as they were: %s, %d drawn, %d more asked for", faces(), fake_draw(), io_submits - submits);
    CHECK_SOURCES();
    CHECK(apps_lv_launcher_holding() && picks == 1, "and it holds still");
    LAUNCHER_END();  /* hide_cards, and every block goes */
}

/* A launch whose app shows nothing and whose end the glue doesn't hear of doesn't leave the launcher holding: APPS_LV_LAUNCH_HOLD_MS
 * after the pick (not after the launcher opened) the poll closes it, as any close. Nothing of the hold is left after. */
static void the_hold_times_out(void) {
    CHECK(APPS_LV_LAUNCH_HOLD_MS == 3000, "the brief's bound: %d ms", APPS_LV_LAUNCH_HOLD_MS);
    char before[8];
    int drawn;
    hold_scene_at(__LINE__, 5 * 1000000, before, &drawn);  /* looked at for 5 s before the pick */
    int card = fake_parent(carousel_row());
    clock_us += (int64_t)APPS_LV_LAUNCH_HOLD_MS * 1000 - 1;
    apps_lv_launcher_poll();
    CHECK(apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN) && strcmp(faces(), before) == 0,
          "a microsecond short of the bound: it holds, as it was: %s", faces());
    clock_us += 1;
    apps_lv_launcher_poll();
    CHECK(!apps_lv_launcher_shown() && !apps_lv_launcher_holding() && fake_flag(card, LV_OBJ_FLAG_HIDDEN) && strcmp(faces(), "TTTTT") == 0,
          "at %d ms it closes: shown %d, %s", APPS_LV_LAUNCH_HOLD_MS, apps_lv_launcher_shown(), faces());
    CHECK_SOURCES();
    run_io();
    CHECK(held == 0, "its icons dropped: %zu bytes held", held);
    log_clear();
    clock_us += 10 * 1000000;
    apps_lv_launcher_poll();
    LOG_EMPTY();
    LAUNCHER_END();
}

/* A voice turn closes a launcher that holds at once (muse_apps_ui_turn's apps_lv_hide_cards), as any close; nothing of the hold is left
 * after. A list that changes while it holds can show it again (it is then a launcher as ever: its row scrolls, and a pick holds again). */
static void a_voice_turn_ends_the_hold(void) {
    char before[8];
    int drawn;
    HOLD_SCENE(before, &drawn);
    int card = fake_parent(carousel_row());
    apps_lv_hide_cards();
    CHECK(!apps_lv_launcher_shown() && !apps_lv_launcher_holding() && fake_flag(card, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown() &&
              strcmp(faces(), "TTTTT") == 0,
          "the turn closes it: shown %d, %s", apps_lv_launcher_shown(), faces());
    run_io();
    CHECK(held == 0, "its icons dropped: %zu bytes held", held);
    log_clear();
    clock_us += (int64_t)APPS_LV_LAUNCH_HOLD_MS * 1000;
    apps_lv_launcher_poll();
    LOG_EMPTY();
    /* Shown again over a hold: a launcher as ever. */
    apps_lv_show_launcher(HOLD_APPS, 5, NULL, pick);
    lv_point_t c = face_at(0);
    fake_tap_at(c.x, c.y);
    CHECK(apps_lv_launcher_holding(), "a pick holds");
    apps_lv_show_launcher(HOLD_APPS, 5, NULL, pick);
    CHECK(apps_lv_launcher_shown() && !apps_lv_launcher_holding() && fake_flag(carousel_row(), LV_OBJ_FLAG_SCROLLABLE),
          "shown again, it doesn't hold, and its row scrolls");
    c = face_at(0);
    fake_drag(c.x, c.y, c.x - APPS_LV_ITEM_PITCH, c.y, 10, 2);
    fake_lift();
    SETTLE();
    CHECK(scroll_now() == APPS_LV_ITEM_PITCH && strcmp(name_shown(), "H2") == 0, "a drag scrolls it: %d %s", (int)scroll_now(),
          name_shown());
    LAUNCHER_END();
}

/* Task 5's m3: a touch on the card that the launcher closed under (an app to the front, a voice turn: LVGL doesn't end a touch for a
 * hide) does nothing at its lift or at its swipe up from the bottom edge: no second close, no call on the hidden card. */
static void a_touch_the_launcher_closed_under_does_nothing(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int card = fake_parent(carousel_row());
    CHECK(fake_down(SCREEN_CENTRE, 360) == card, "the press is the card's");
    apps_lv_hide_cards();
    log_clear();
    fake_lift();
    CHECK(picks == 0 && !apps_lv_launcher_shown(), "the lift opens nothing");
    LOG_EMPTY();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    CHECK(fake_down(SCREEN_CENTRE, EDGE_TOP + 14) == card, "the press in the band is the card's");
    apps_lv_hide_cards();
    log_clear();
    for (int i = 1; i <= 8; i++) {
        fake_move(SCREEN_CENTRE, EDGE_TOP + 14 - 10 * i);
    }
    fake_lift();
    CHECK(!apps_lv_launcher_shown() && log_count("add_flag") == 0 && log_count("remove_flag") == 0,
          "the swipe up closes nothing again\n%s", log_peek());
    LAUNCHER_END();
}

/* Task 6a's fix round 1 (I1): a pick must not leave the launcher's own memory in the way of the launch's admission. Its hold keeps only the
 * icons it draws on the screen (the centre and its two neighbours, at rest) and frees the rest at once: a ready icon off the screen, and
 * a load in flight, which frees its block on the worker as it ends. It asks for nothing more while it holds: no load, however many frames
 * pass. Eight apps, the pick in the middle: of four icons ready (the centre, its neighbours, one two away) and one loading, three are
 * held. The faces on the screen don't change (the same descriptors); the one off it is its tile again. */
static void the_hold_drops_offscreen_icons(void) {
    cards_scene();
    static const apps_lv_app_t eight[] = {{"d1", "D1", "icon.bin"}, {"d2", "D2", "icon.bin"}, {"d3", "D3", "icon.bin"},
                                          {"d4", "D4", "icon.bin"}, {"d5", "D5", "icon.bin"}, {"d6", "D6", "icon.bin"},
                                          {"d7", "D7", "icon.bin"}, {"d8", "D8", "icon.bin"}};
    for (int i = 0; i < 8; i++) {
        put_icon(eight[i].id, CF_RGB565A8, 120, 120, (uint8_t)(0x20 + i));
    }
    apps_lv_show_launcher(eight, 8, NULL, pick);
    fake_scroll(carousel_row(), 4 * APPS_LV_ITEM_PITCH);  /* at rest on d5: the window is d5, d6, d4, d7, d3 */
    for (int round = 0; round < 3; round++) {
        run_io();
        apps_lv_launcher_poll();
    }
    CHECK(strcmp(faces(), "TTTIIIIT") == 0 && io_queued == 1, "d4 to d7 in, d3 on its way: %s, %d queued", faces(), io_queued);
    const size_t one = held / 4;
    CHECK(held == 4 * one && one > 43200, "four icons held: %zu bytes", held);
    const void *shown[3];
    for (int k = 3; k <= 5; k++) {
        shown[k - 3] = fake_obj(item_parts(carousel_item(k)).image)->src;
    }
    const int submits = io_submits;
    lv_point_t c = face_at(4);
    CHECK(fake_tap_at(c.x, c.y) == carousel_item(4) && picks == 1 && strcmp(picked, "d5") == 0 && apps_lv_launcher_holding(),
          "d5 is picked, and the launcher holds: %d '%s'", picks, picked);
    CHECK(strcmp(faces(), "TTTIIITT") == 0, "the screen's three icons stay, the one off it is its tile: %s", faces());
    for (int k = 3; k <= 5; k++) {
        CHECK(fake_obj(item_parts(carousel_item(k)).image)->src == shown[k - 3], "item %d draws the same icon", k);
    }
    CHECK(held == 3 * one, "the off-screen icon freed at once: %zu bytes held, three icons are %zu", held, 3 * one);
    CHECK_SOURCES();
    run_io();  /* d3's load, orphaned, ends on the worker */
    CHECK(held == 3 * one, "the load in flight freed its own block: %zu bytes held", held);
    for (int frame = 0; frame < 5; frame++) {
        apps_lv_launcher_poll();
    }
    run_io();
    CHECK(io_submits == submits && held == 3 * one && strcmp(faces(), "TTTIIITT") == 0,
          "nothing asked for while it holds: %d more loads, %zu bytes held, %s", io_submits - submits, held, faces());
    LAUNCHER_END();  /* the close frees the three */
}

/* ---- Launcher carousel Task 6: the frame gate's measurement, in a MUSE_BENCH build only ---- */

#if CONFIG_MUSE_BENCH
#define FRAMES_LOGGED true
#else
#define FRAMES_LOGGED false
#endif

/* The ms of each "launcher frame <ms> ms" line apps_lv.c logged since the last esp_clear, in order, into ms (at most cap of them);
 * how many there were. A line of any other shape after the words is a failure. */
static int frame_lines_at(int line, int *ms, int cap) {
    static const char prefix[] = "I muse_apps: launcher frame ";
    int n = 0;
    for (const char *at = esp_peek(); (at = strstr(at, prefix)) != NULL; at += sizeof prefix - 1) {
        char *end;
        long v = strtol(at + sizeof prefix - 1, &end, 10);
        CHECK_AT(line, end != at + sizeof prefix - 1 && strncmp(end, " ms\n", 4) == 0, "a frame line of another shape:\n%s", at);
        if (n < cap) {
            ms[n] = (int)v;
        }
        n++;
    }
    return n;
}
#define FRAME_LINES(ms, cap) frame_lines_at(__LINE__, (ms), (cap))

/* A refresh gap_us after the last one. */
static void refresh_after(int64_t gap_us) {
    clock_us += gap_us;
    fake_refresh();
}

/* While the carousel scrolls (a drag, a fling's snap), each display refresh logs "launcher frame <ms> ms", the time since the refresh
 * before it, rounded to the ms, when that one found it scrolling too: the first refresh of a scroll follows a rest of any length and
 * isn't timed. Nothing at rest, nothing from a row that moves on under a closed launcher, and one callback on the display however
 * often the layer is built. A build without the bench registers nothing and logs nothing, whatever scrolls. */
static void launcher_frame_log(void) {
    cards_scene();
    cards_scene();  /* the layer built again (a new screen on the same display) */
    if (FRAMES_LOGGED) {
        CHECK(display_obj.n_cbs == 1 && display_obj.cbs[0].filter == LV_EVENT_REFR_READY,
              "a bench build watches the display's refreshes, once: %d callbacks", display_obj.n_cbs);
    } else {
        CHECK(display_obj.n_cbs == 0, "a build without the bench adds nothing to the display: %d callbacks", display_obj.n_cbs);
    }
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    int row = carousel_row();
    int ms[64];
    esp_clear();
    for (int i = 0; i < 3; i++) {
        refresh_after(1000000);  /* at rest: a clock tick, an icon swapped in */
    }
    CHECK(FRAME_LINES(ms, 64) == 0, "nothing at rest:\n%s", esp_peek());

    /* A drag. Its first refresh follows the rest; the rest are timed, to the nearest ms. */
    lv_point_t p = face_at(0);
    fake_down(p.x, p.y);
    fake_move(p.x - 12, p.y);
    CHECK(fake_scrolling(row), "the drag scrolls the row");
    refresh_after(15000);
    static const int64_t gaps_us[] = {20000, 33000, 16400, 16600, 51000};
    static const int want_drag[] = {20, 33, 16, 17, 51};
    for (size_t i = 0; i < ARRAY_LEN(gaps_us); i++) {
        fake_move(p.x - 12 * (int32_t)(i + 2), p.y);
        refresh_after(gaps_us[i]);
    }
    int n = FRAME_LINES(ms, 64);
    if (FRAMES_LOGGED) {
        CHECK(n == 5, "the drag's frames after its first: %d lines\n%s", n, esp_peek());
        for (int i = 0; i < 5 && i < n; i++) {
            CHECK(ms[i] == want_drag[i], "frame %d of the drag: %d ms, want %d", i, ms[i], want_drag[i]);
        }
    } else {
        CHECK(n == 0, "no frame line without the bench:\n%s", esp_peek());
    }

    /* The fling: after the lift, the snap's animation, a refresh after each of its frames, each timed until it has ended. */
    esp_clear();
    fake_lift();
    int frames = 0;
    for (; frames < 100 && (throw_pending || n_anims > 0); frames++) {
        clock_us += 16000;
        fake_frame(16);
        fake_refresh();
    }
    CHECK(!fake_scrolling(row) && frames >= 6, "the snap ran %d frames and ended", frames);
    n = FRAME_LINES(ms, 64);
    if (FRAMES_LOGGED) {
        CHECK(n == frames - 1, "every frame of the snap but the one that ended it: %d lines, %d frames\n%s", n, frames, esp_peek());
        for (int i = 0; i < n && i < 64; i++) {
            CHECK(ms[i] == 16, "frame %d of the snap: %d ms", i, ms[i]);
        }
    } else {
        CHECK(n == 0, "no frame line without the bench:\n%s", esp_peek());
    }

    /* At rest again, for 5 s: nothing, and the next drag's first refresh isn't a 5,000 ms frame. */
    esp_clear();
    for (int i = 0; i < 5; i++) {
        refresh_after(1000000);
    }
    p = (lv_point_t){SCREEN_CENTRE, face_at(0).y};  /* the app at the centre, wherever the fling left it */
    int way = scroll_now() < 4 * APPS_LV_ITEM_PITCH ? -1 : 1;  /* towards an app there is */
    fake_down(p.x, p.y);
    fake_move(p.x + way * 12, p.y);
    refresh_after(15000);
    fake_move(p.x + way * 24, p.y);
    refresh_after(18000);
    n = FRAME_LINES(ms, 64);
    CHECK(n == (FRAMES_LOGGED ? 1 : 0) && (!FRAMES_LOGGED || ms[0] == 18), "after a rest, only the frame after the first: %d lines\n%s",
          n, esp_peek());
    fake_lift();
    SETTLE();

    /* A launcher closed under a fling: LVGL animates its hidden row to the snap point all the same, and nothing is logged. */
    way = scroll_now() < 4 * APPS_LV_ITEM_PITCH ? -1 : 1;
    swipe_from(p.x, p.y, way * 120, 0);
    fake_lift();
    fake_frame(16);
    CHECK(fake_scrolling(row), "the snap runs");
    apps_lv_hide_cards();
    esp_clear();
    for (int i = 0; i < 100 && (throw_pending || n_anims > 0); i++) {
        clock_us += 16000;
        fake_frame(16);
        fake_refresh();
    }
    CHECK(FRAME_LINES(ms, 64) == 0, "nothing from a closed launcher's row:\n%s", esp_peek());
    LAUNCHER_END();
}

/* ---- Connected apps Task 1: an image's rotation and spin ---- */

/* n ms of frames of the animation timer, step ms each (the last one shorter, to land on n). */
static void frames(long n, int step) {
    for (long done = 0; done < n;) {
        int ms = n - done < step ? (int)(n - done) : step;
        fake_frame(ms);
        done += ms;
    }
}

/* The angle, in 0.1 degrees, of a spin from `from` at rpm turns a minute ms after it began, as apps_lv.c's path makes it: a turn of
 * 60000 / rpm ms in that over APPS_SPIN_STEP_MS whole steps of equal time, each angle held for its step. */
static int32_t spin_angle(int32_t from, int rpm, long ms) {
    int32_t duration = 60000 / rpm, steps = duration / APPS_SPIN_STEP_MS;
    if (steps < 1) {
        steps = 1;
    }
    int32_t k = (int32_t)((ms % duration) * steps / duration);
    return (from + 3600 * k / steps) % 3600;
}

/* Rotation is the image's own angle (lv_image_set_rotation, in 0.1 degrees, about its pivot: the centre, LVGL's default), applied
 * with the style. A new image at 0 makes no call (LVGL's own angle is 0), a style change that isn't the angle leaves it, and an
 * angle or a spin out of range in the table is a reject, never LVGL's. */
static void image_rotation_applies(void) {
    env_t e;
    scene(&e);
    RUN(&e, "i = ui.image{src = 'a.bin', rotation = 90} j = ui.image{src = 'b.bin'}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *i = fake_obj(2), *j = fake_obj(3);
    CHECK(log_has_line("image_rotation 2 900") && i->rotation == 900, "rotation 90 is lv_image_set_rotation(obj, 900)\n%s", log_peek());
    CHECK(log_count("image_rotation 3 ") == 0 && j->rotation == 0, "an image at 0 makes no call\n%s", log_peek());
    CHECK(log_count("anim_") == 0 && fake_spins() == 0 && stats_now().spinning == 0, "no animation without a spin\n%s", log_peek());

    log_clear();
    RUN(&e, "i:set{rotation = 359}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("image_rotation 2 3590") && i->rotation == 3590, "359 degrees\n%s", log_peek());
    log_clear();
    RUN(&e, "i:set{opacity = 50, scale = 200} j:set{hidden = true}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_count("image_rotation") == 0 && log_count("anim_") == 0 && i->rotation == 3590, "a style change that isn't the angle\n%s",
          log_peek());
    log_clear();
    RUN(&e, "i:set{rotation = 0}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("image_rotation 2 0") && i->rotation == 0, "back to 0\n%s", log_peek());

    /* Out of range in the table: dropped, and LVGL hears nothing of it. */
    log_clear();
    apps_lv_stats_t before = stats_now();
    W(&e, 0)->rotation = 360;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_now().rejects == before.rejects + 1 && log_count("image_rotation") == 0, "rotation 360: %u rejects\n%s",
          stats_now().rejects - before.rejects, log_peek());
    W(&e, 0)->rotation = -1;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_now().rejects == before.rejects + 2 && log_count("image_rotation") == 0, "rotation -1\n%s", log_peek());
    W(&e, 0)->rotation = 0;
    W(&e, 0)->spin = APPS_SPIN_RPM_MAX + 1;
    W(&e, 0)->dirty = APPS_DIRTY_STYLE;
    CHECK(APPLY(&e), "the pass ran");
    CHECK(stats_now().rejects == before.rejects + 3 && log_count("anim_") == 0 && fake_spins() == 0, "spin %d\n%s",
          APPS_SPIN_RPM_MAX + 1, log_peek());
    W(&e, 0)->spin = 0;
    FINISH(&e);
}

/* A spin is one animation of the image's angle, a turn in 60000 / rpm ms repeated without end, from the angle the image is at. Its
 * angle changes at most once per APPS_SPIN_STEP_MS of the animation's time. spin = 0 deletes it and leaves the angle where it got
 * to; a spin set again goes on from there, and a new rate goes on from where the old one got to. */
static void image_spin_animates_and_stops_in_place(void) {
    env_t e;
    scene(&e);
    RUN(&e, "d = ui.image{src = 'a.bin', spin = 30}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *d = fake_obj(2);
    const lv_anim_t *a = fake_anim_of(2);
    CHECK(fake_spins() == 1 && a != NULL, "one animation: %d", fake_spins());
    if (a != NULL) {
        CHECK(a->duration == 2000 && a->repeat_cnt == LV_ANIM_REPEAT_INFINITE && a->start_value == 0 && a->end_value == 3600 &&
                  a->var == d,
              "a turn of 2,000 ms from 0, without end: %d ms, %d..%d, repeat %u", (int)a->duration, (int)a->start_value,
              (int)a->end_value, (unsigned)a->repeat_cnt);
    }
    CHECK(log_has_line("anim_start 2 0 3600 2000 ms repeat=inf"), "the animation\n%s", log_peek());
    CHECK(stats_now().spinning == 1, "spinning %u", stats_now().spinning);

    /* The fake's clock moves on: a quarter of a turn in 500 ms. */
    frames(500, 10);
    CHECK(d->rotation == 900 && d->rotation == spin_angle(0, 30, 500), "a quarter turn: %d", (int)d->rotation);

    /* Stopped: the animation goes, and the angle stays where it got to, however long. */
    log_clear();
    RUN(&e, "d:set{spin = 0}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("anim_delete 2") && log_count("image_rotation") == 0 && fake_spins() == 0 && stats_now().spinning == 0,
          "stopped in place\n%s", log_peek());
    frames(1000, 10);
    CHECK(d->rotation == 900, "it stays at 900: %d", (int)d->rotation);
    RUN(&e, "assert(d:get('spin') == 0 and d:get('rotation') == 0, 'get says what the app set')");

    /* Again: on from 900, and a whole turn brings it back there, one angle per step at the most. */
    log_clear();
    RUN(&e, "d:set{spin = 30}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("anim_start 2 900 4500 2000 ms repeat=inf") && fake_spins() == 1 && d->rotation == 900,
          "resumed from where it stopped\n%s", log_peek());
    int turns = d->turns;
    d->turn_gap_ms = -1;
    frames(2000, 1);
    int steps = 2000 / APPS_SPIN_STEP_MS;
    CHECK(d->rotation == 900 && d->turns - turns == steps, "a whole turn: at %d, %d changes, want %d", (int)d->rotation,
          d->turns - turns, steps);
    CHECK(d->turn_gap_ms >= APPS_SPIN_STEP_MS, "two changes %ld ms apart, under APPS_SPIN_STEP_MS (%d)", d->turn_gap_ms,
          APPS_SPIN_STEP_MS);
    /* At the board's rate of the animation timer (LV_DEF_REFR_PERIOD, 15 ms) it is one change per step it passes, never two. */
    turns = d->turns;
    frames(1000, 15);
    CHECK(d->turns - turns <= 1000 / (2000 / steps) + 1 && d->rotation == spin_angle(900, 30, 1000), "15 ms frames: %d changes, at %d",
          d->turns - turns, (int)d->rotation);
    frames(250, 10);
    int32_t at = spin_angle(900, 30, 1250);
    CHECK(d->rotation == at, "on: %d, want %d", (int)d->rotation, (int)at);

    /* A new rate: one animation still, at 1,000 ms a turn, from where the old one got to. */
    log_clear();
    RUN(&e, "d:set{spin = 60}");
    CHECK(APPLY(&e), "changed");
    char line[64];
    snprintf(line, sizeof line, "anim_start 2 %d %d 1000 ms repeat=inf", (int)at, (int)at + 3600);
    CHECK(log_has_line("anim_delete 2") && log_has_line(line) && fake_spins() == 1 && stats_now().spinning == 1,
          "the new rate from %d\n%s", (int)at, log_peek());
    frames(100, 10);
    CHECK(d->rotation == spin_angle(at, 60, 100), "60 turns a minute: %d, want %d", (int)d->rotation, (int)spin_angle(at, 60, 100));
    /* What the app set is what get says, whatever the angle. */
    RUN(&e, "assert(d:get('spin') == 60 and d:get('rotation') == 0)");
    FINISH(&e);
}

/* Pictures for Task 7's case, at the most an image may spin (SPIN_PX) and half that: side x side, RGB565A8 whose alpha is 255 where
 * a pixel's centre is strictly inside the circle inscribed in the picture and 0 outside (apps_jpeg's circle), or RGB565 with no
 * alpha (a square that is all picture). */
#define SPIN_PX APPS_SPIN_SIDE_MAX
static uint8_t CIRCLE_FULL[12 + SPIN_PX * SPIN_PX * 3], CIRCLE_HALF[12 + (SPIN_PX / 2) * (SPIN_PX / 2) * 3],
    SQUARE_FULL[12 + SPIN_PX * SPIN_PX * 2];

static void make_picture(uint8_t *b, int side, bool circle) {
    const uint8_t head[12] = {0x19, circle ? 0x14 : 0x12, 0, 0, side & 0xFF, side >> 8, side & 0xFF, side >> 8,
                              (side * 2) & 0xFF, (side * 2) >> 8, 0, 0};
    memcpy(b, head, sizeof head);
    for (int i = 0; i < side * side; i++) {
        b[12 + 2 * i] = (uint8_t)i;
        b[13 + 2 * i] = (uint8_t)(i >> 8);
    }
    if (circle) {
        uint8_t *alpha = b + 12 + side * side * 2;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                long dx = 2L * x + 1 - side, dy = 2L * y + 1 - side;
                alpha[y * side + x] = dx * dx + dy * dy < (long)side * side ? 255 : 0;
            }
        }
    }
}

/* Task 7: what a turned picture invalidates. LVGL makes room past an image's box for its turned corners (lv_image's
 * LV_EVENT_REFR_EXT_DRAW_SIZE) and invalidates the turned box at every step of a spin: a square at 45 degrees is a box 1.41 times
 * its side. A picture whose alpha is a circle draws nothing outside its own box at any angle, so apps_lv caps the room at its
 * farthest pixel that isn't transparent, drawn, plus 2 px: a spinning record invalidates its own square and looks the same.
 * A picture with corners keeps LVGL's room, and a scaled one gets the room its drawn picture needs, not LVGL's for the box
 * scaled again. The pictures are at the spin cap, SPIN_PX (180 px since Task 7's R1b), and half of it at 200%. */
static void spin_invalidates_what_it_draws(void) {
    make_picture(CIRCLE_FULL, SPIN_PX, true);
    make_picture(CIRCLE_HALF, SPIN_PX / 2, true);
    make_picture(SQUARE_FULL, SPIN_PX, false);
    const apps_asset_t assets[] = {{"c.bin", CIRCLE_FULL, sizeof CIRCLE_FULL, SPIN_PX, SPIN_PX},
                                   {"s.bin", SQUARE_FULL, sizeof SQUARE_FULL, SPIN_PX, SPIN_PX},
                                   {"k.bin", CIRCLE_HALF, sizeof CIRCLE_HALF, SPIN_PX / 2, SPIN_PX / 2}};
    const int diagonal = (int)(SPIN_PX * 1.41421356 + 0.5);  /* the square's turned box at 45 degrees */
    env_t e;
    scene_with(&e, assets, 3);
    RUN(&e, "c = ui.image{src = 'c.bin', spin = 30}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *c = fake_obj(2);
    CHECK(log_count("add_event_cb 2 refr_ext_draw_size") == 1, "the cap is on the image\n%s", log_peek());
    fake_inv_clear(c);
    frames(2000, 10);  /* a whole turn */
    lv_area_t box = c->area;
    CHECK(box.x2 - box.x1 == SPIN_PX - 1 && box.y2 - box.y1 == SPIN_PX - 1, "a %d px box", SPIN_PX);
    CHECK(c->turns > 50 && c->inv.x1 >= box.x1 - 3 && c->inv.y1 >= box.y1 - 3 && c->inv.x2 <= box.x2 + 3 && c->inv.y2 <= box.y2 + 3,
          "the record invalidated (%d,%d)-(%d,%d) in %d turns: its box (%d,%d)-(%d,%d) and 3 px at most", (int)c->inv.x1,
          (int)c->inv.y1, (int)c->inv.x2, (int)c->inv.y2, c->turns, (int)box.x1, (int)box.y1, (int)box.x2, (int)box.y2);
    CHECK(c->ext_draw <= 3 && c->inv_widest <= SPIN_PX + 6, "room %d, widest %d", (int)c->ext_draw, (int)c->inv_widest);

    /* All picture, no alpha: its corners swing out, and LVGL's room stays (the turned box). */
    RUN(&e, "c:set{spin = 0, hidden = true} s = ui.image{src = 's.bin', spin = 30}");
    CHECK(APPLY(&e), "changed");
    lv_obj_t *sq = fake_obj(3);
    fake_inv_clear(sq);
    frames(2000, 10);
    CHECK(sq->inv_widest >= diagonal - 3 && sq->inv_widest <= diagonal + 3, "the square: widest %d, its diagonal %d",
          (int)sq->inv_widest, diagonal);

    /* Scaled: half the side at 200% draws the side. LVGL turns the box at 200% (a square of twice the side, 2.83 times it at 45
     * degrees); the picture drawn needs its box and 4 px. */
    RUN(&e, "s:set{spin = 0, hidden = true} k = ui.image{src = 'k.bin', scale = 200, spin = 30}");
    CHECK(APPLY(&e), "changed");
    lv_obj_t *k = fake_obj(4);
    fake_inv_clear(k);
    frames(2000, 10);
    box = k->area;
    CHECK(box.x2 - box.x1 == SPIN_PX - 1, "a box of %d px: %d", SPIN_PX, (int)(box.x2 - box.x1 + 1));
    CHECK(k->inv.x1 >= box.x1 - 4 && k->inv.y1 >= box.y1 - 4 && k->inv.x2 <= box.x2 + 4 && k->inv.y2 <= box.y2 + 4 &&
              k->ext_draw <= 4,
          "the scaled record invalidated (%d,%d)-(%d,%d), room %d", (int)k->inv.x1, (int)k->inv.y1, (int)k->inv.x2,
          (int)k->inv.y2, (int)k->ext_draw);

    /* Still, at an angle: capped the same. Its picture changed to one with corners: LVGL's room again, at once. */
    RUN(&e, "k:set{spin = 0, rotation = 45, scale = 100}");
    CHECK(APPLY(&e), "changed");
    CHECK(k->ext_draw <= 3, "a still record at 45 degrees: room %d", (int)k->ext_draw);
    RUN(&e, "k:set{src = 's.bin'}");
    CHECK(APPLY(&e), "changed");
    CHECK(k->ext_draw >= (diagonal - SPIN_PX) / 2 - 3, "the square at 45 degrees: room %d", (int)k->ext_draw);
    CHECK(log_count("add_event_cb 4 refr_ext_draw_size") == 1, "one cap an object\n%s", log_peek());

    /* Unturned images aren't capped, and get no callback. */
    RUN(&e, "u = ui.image{src = 'c.bin', scale = 50}");
    CHECK(APPLY(&e), "made");
    CHECK(log_count("add_event_cb 5 ") == 0, "an image that doesn't turn\n%s", log_peek());
    FINISH(&e);
}

/* Setting rotation turns the image to it, spinning or not, and a spin goes on from there: the same value again too, once the image
 * has spun away from it. */
static void rotation_sets_the_angle_a_spin_continues_from(void) {
    env_t e;
    scene(&e);
    RUN(&e, "d = ui.image{src = 'a.bin', spin = 30}");
    CHECK(APPLY(&e), "made");
    lv_obj_t *d = fake_obj(2);
    frames(500, 10);
    CHECK(d->rotation == 900, "a quarter turn: %d", (int)d->rotation);

    log_clear();
    RUN(&e, "d:set{rotation = 45}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("anim_delete 2") && log_has_line("image_rotation 2 450") && log_has_line("anim_start 2 450 4050 2000 ms repeat=inf"),
          "turned to 45 and on from there\n%s", log_peek());
    CHECK(log_find("anim_delete 2") < log_find("image_rotation 2 450") && log_find("image_rotation 2 450") < log_find("anim_start 2"),
          "stopped, turned, started\n%s", log_peek());
    CHECK(d->rotation == 450 && fake_spins() == 1, "at %d, %d animations", (int)d->rotation, fake_spins());
    frames(500, 10);
    CHECK(d->rotation == spin_angle(450, 30, 500), "on from 45: %d", (int)d->rotation);

    /* The value it holds, after the spin moved the image on: back to it. */
    log_clear();
    RUN(&e, "d:set{rotation = 45}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("anim_start 2 450 4050 2000 ms repeat=inf") && d->rotation == 450, "back to 45\n%s", log_peek());
    frames(300, 10);
    int32_t at = spin_angle(450, 30, 300);
    CHECK(d->rotation == at, "on: %d", (int)d->rotation);

    /* Stopped, it stays; a rotation then turns it, and no spin starts. */
    RUN(&e, "d:set{spin = 0}");
    CHECK(APPLY(&e), "changed");
    frames(300, 10);
    CHECK(d->rotation == at && fake_spins() == 0, "stopped at %d: %d", (int)at, (int)d->rotation);
    log_clear();
    RUN(&e, "d:set{rotation = 180}");
    CHECK(APPLY(&e), "changed");
    CHECK(d->rotation == 1800 && log_count("anim_") == 0 && fake_spins() == 0, "turned, still\n%s", log_peek());
    log_clear();
    RUN(&e, "d:set{rotation = 180}");  /* where it is: nothing */
    CHECK(APPLY(&e), "a pass");
    LOG_EMPTY();

    /* Both in one set: the turn first, and the spin from it. */
    log_clear();
    RUN(&e, "d:set{rotation = 270, spin = 60}");
    CHECK(APPLY(&e), "changed");
    CHECK(log_has_line("image_rotation 2 2700") && log_has_line("anim_start 2 2700 6300 1000 ms repeat=inf"), "from 270\n%s",
          log_peek());
    frames(100, 10);
    CHECK(d->rotation == spin_angle(2700, 60, 100), "on from 270: %d", (int)d->rotation);
    RUN(&e, "d:set{rotation = 300, spin = 0}");
    CHECK(APPLY(&e), "changed");
    CHECK(d->rotation == 3000 && fake_spins() == 0 && stats_now().spinning == 0, "stopped at 300: %d", (int)d->rotation);
    FINISH(&e);
}

/* A spin is the image object's own animation: it ends with the object, whatever ends that (a delete, a box's delete, ui.clear(), the
 * layer's reset as the app ends), and LVGL never steps it on an object that is gone (the fake counts any call on one). */
static void spin_ends_with_the_widget(void) {
    env_t e;
    scene(&e);
    RUN(&e, "a = ui.image{src = 'a.bin', spin = 30}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_spins() == 1 && stats_now().spinning == 1, "one spin");
    frames(100, 10);
    RUN(&e, "a:delete()");
    CHECK(APPLY(&e), "deleted");
    CHECK(fake_spins() == 0 && stats_now().spinning == 0, "a delete: %d animations, spinning %u", fake_spins(), stats_now().spinning);
    frames(500, 10);

    RUN(&e, "a = ui.image{src = 'a.bin', spin = 30} b = ui.image{src = 'b.bin', spin = 60, rotation = 10}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_spins() == 2 && stats_now().spinning == 2, "two spins: %d", fake_spins());
    frames(100, 10);
    RUN(&e, "ui.clear()");
    CHECK(APPLY(&e), "cleared");
    CHECK(fake_spins() == 0 && stats_now().spinning == 0, "ui.clear(): %d animations, spinning %u", fake_spins(), stats_now().spinning);
    frames(500, 10);

    RUN(&e, "box = ui.box{w = 300, h = 300} c = ui.image{src = 'a.bin', parent = box, spin = 10}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_spins() == 1, "a spin in a box");
    log_clear();
    RUN(&e, "box:delete()");
    CHECK(APPLY(&e), "deleted");
    CHECK(fake_spins() == 0 && stats_now().spinning == 0 && log_count("anim_") == 0, "the box's delete took it\n%s", log_peek());
    frames(500, 10);

    RUN(&e, "d = ui.image{src = 'a.bin', spin = 45}");
    CHECK(APPLY(&e), "made");
    CHECK(fake_spins() == 1, "a spin");
    unpublish();  /* the app ends: its table let go, then the layer reset */
    apps_lv_reset();
    CHECK(fake_spins() == 0 && stats_now().spinning == 0, "the reset: %d animations, spinning %u", fake_spins(), stats_now().spinning);
    frames(500, 10);
    FINISH(&e);
}

/* The ms of each "spin frame <ms> ms" line apps_lv.c logged since the last esp_clear, as frame_lines_at reads the launcher's. */
static int spin_lines_at(int line, int *ms, int cap) {
    static const char prefix[] = "I muse_apps: spin frame ";
    int n = 0;
    for (const char *at = esp_peek(); (at = strstr(at, prefix)) != NULL; at += sizeof prefix - 1) {
        char *end;
        long v = strtol(at + sizeof prefix - 1, &end, 10);
        CHECK_AT(line, end != at + sizeof prefix - 1 && strncmp(end, " ms\n", 4) == 0, "a spin frame line of another shape:\n%s", at);
        if (n < cap) {
            ms[n] = (int)v;
        }
        n++;
    }
    return n;
}
#define SPIN_LINES(ms, cap) spin_lines_at(__LINE__, (ms), (cap))

/* The spin gate's measurement, in a bench build: while an image spins on the layer in front, each display refresh logs "spin frame
 * <ms> ms", the time since the refresh before it, when that one found an image spinning there too (the first after a rest isn't
 * timed). Nothing while none spins, while the layer is behind or a card is over it, and nothing at all without the bench. */
static void spin_frame_log(void) {
    env_t e;
    scene(&e);
    if (FRAMES_LOGGED) {
        CHECK(display_obj.n_cbs == 1 && display_obj.cbs[0].filter == LV_EVENT_REFR_READY, "one callback on the display: %d",
              display_obj.n_cbs);
    } else {
        CHECK(display_obj.n_cbs == 0, "a build without the bench adds nothing to the display: %d callbacks", display_obj.n_cbs);
    }
    apps_lv_set_front(true);
    int ms[64];
    esp_clear();
    refresh_after(1000000);
    refresh_after(16000);
    CHECK(SPIN_LINES(ms, 64) == 0, "nothing while no image spins:\n%s", esp_peek());

    RUN(&e, "d = ui.image{src = 'a.bin', spin = 33}");
    CHECK(APPLY(&e), "made");
    esp_clear();
    refresh_after(500000);  /* after a rest: not timed */
    static const int64_t gaps_us[] = {33000, 30000, 45000, 16400, 16600};
    static const int want[] = {33, 30, 45, 16, 17};
    for (size_t i = 0; i < ARRAY_LEN(gaps_us); i++) {
        fake_frame(15);
        refresh_after(gaps_us[i]);
    }
    int n = SPIN_LINES(ms, 64);
    if (FRAMES_LOGGED) {
        CHECK(n == 5, "the spin's frames after its first: %d lines\n%s", n, esp_peek());
        for (int i = 0; i < 5 && i < n; i++) {
            CHECK(ms[i] == want[i], "frame %d: %d ms, want %d", i, ms[i], want[i]);
        }
    } else {
        CHECK(n == 0, "no spin frame line without the bench:\n%s", esp_peek());
    }
    CHECK(FRAME_LINES(ms, 64) == 0, "no launcher frame line for a spin:\n%s", esp_peek());

    /* Behind (another page shows): nothing; in front again, the first refresh isn't timed. */
    esp_clear();
    apps_lv_set_front(false);
    refresh_after(33000);
    refresh_after(33000);
    CHECK(SPIN_LINES(ms, 64) == 0, "nothing while the layer is behind:\n%s", esp_peek());
    apps_lv_set_front(true);
    refresh_after(33000);
    refresh_after(20000);
    n = SPIN_LINES(ms, 64);
    CHECK(n == (FRAMES_LOGGED ? 1 : 0) && (!FRAMES_LOGGED || ms[0] == 20), "in front again: %d lines\n%s", n, esp_peek());

    /* A card over it: nothing. */
    esp_clear();
    apps_lv_show_crash("Spin", NULL);
    refresh_after(33000);
    refresh_after(33000);
    CHECK(SPIN_LINES(ms, 64) == 0, "nothing under a card:\n%s", esp_peek());
    apps_lv_hide_cards();

    /* Stopped, or gone: nothing. */
    RUN(&e, "d:set{spin = 0}");
    CHECK(APPLY(&e), "stopped");
    esp_clear();
    for (int i = 0; i < 3; i++) {
        refresh_after(33000);
    }
    CHECK(SPIN_LINES(ms, 64) == 0, "nothing once it stops:\n%s", esp_peek());
    RUN(&e, "d:set{spin = 10}");
    CHECK(APPLY(&e), "again");
    esp_clear();
    refresh_after(33000);
    refresh_after(33000);
    CHECK(SPIN_LINES(ms, 64) == (FRAMES_LOGGED ? 1 : 0), "spinning again:\n%s", esp_peek());
    RUN(&e, "d:delete()");
    CHECK(APPLY(&e), "deleted");
    esp_clear();
    refresh_after(33000);
    refresh_after(33000);
    CHECK(SPIN_LINES(ms, 64) == 0, "nothing once its widget is gone:\n%s", esp_peek());
    FINISH(&e);
}

/* Task 7 (R7a): the board's frame pairing, which the layer turns on while an image spins on it in front with no launcher or card
 * over it, and off when that stops: a spin stopped or deleted, the app behind, the launcher or a card shown. Each change once, at
 * the end of the refresh that saw it; one callback on the display once a pairing callback is set, and none before. */
static int pairing_calls;
static bool pairing_on;
static void note_pairing(bool on) {
    pairing_calls++;
    pairing_on = on;
}

static void pairing_follows_the_spin(void) {
    env_t e;
    scene(&e);
    int before = display_obj.n_cbs;
    apps_lv_set_pairing_cb(note_pairing);
    apps_lv_set_pairing_cb(note_pairing);  /* set again: still one callback */
    CHECK(display_obj.n_cbs == before + 1 && display_obj.cbs[before].filter == LV_EVENT_REFR_READY,
          "one refresh callback for the pairing: %d, %d before", display_obj.n_cbs, before);
    apps_lv_set_front(true);
    refresh_after(33000);
    refresh_after(33000);
    CHECK(pairing_calls == 0, "no spin, no call: %d", pairing_calls);

    RUN(&e, "d = ui.image{src = 'a.bin', spin = 33}");
    CHECK(APPLY(&e), "made");
    refresh_after(33000);
    CHECK(pairing_calls == 1 && pairing_on, "the first spin turns it on: %d calls, on %d", pairing_calls, pairing_on);
    refresh_after(33000);
    CHECK(pairing_calls == 1, "each change once: %d calls", pairing_calls);

    RUN(&e, "d:set{spin = 0}");
    CHECK(APPLY(&e), "stopped");
    refresh_after(33000);
    CHECK(pairing_calls == 2 && !pairing_on, "the last spin stopped turns it off: %d calls, on %d", pairing_calls, pairing_on);

    RUN(&e, "d:set{spin = 10}");
    CHECK(APPLY(&e), "again");
    refresh_after(33000);
    CHECK(pairing_calls == 3 && pairing_on, "spinning again: %d calls, on %d", pairing_calls, pairing_on);
    RUN(&e, "d:delete()");
    CHECK(APPLY(&e), "deleted");
    refresh_after(33000);
    CHECK(pairing_calls == 4 && !pairing_on, "the last spinner deleted: %d calls, on %d", pairing_calls, pairing_on);

    RUN(&e, "d = ui.image{src = 'a.bin', spin = 33} k = ui.image{src = 'b.bin', spin = 20}");
    CHECK(APPLY(&e), "two");
    refresh_after(33000);
    CHECK(pairing_calls == 5 && pairing_on, "two spinning: %d calls, on %d", pairing_calls, pairing_on);
    RUN(&e, "d:set{spin = 0}");
    CHECK(APPLY(&e), "one stopped");
    refresh_after(33000);
    CHECK(pairing_calls == 5 && pairing_on, "one still spins: %d calls, on %d", pairing_calls, pairing_on);

    apps_lv_set_front(false);  /* the app behind (another page, or the screen dark) */
    refresh_after(33000);
    CHECK(pairing_calls == 6 && !pairing_on, "behind: %d calls, on %d", pairing_calls, pairing_on);
    apps_lv_set_front(true);
    refresh_after(33000);
    CHECK(pairing_calls == 7 && pairing_on, "in front again: %d calls, on %d", pairing_calls, pairing_on);

    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    refresh_after(33000);
    CHECK(pairing_calls == 8 && !pairing_on, "the launcher over it: %d calls, on %d", pairing_calls, pairing_on);
    apps_lv_close_launcher();
    refresh_after(33000);
    CHECK(pairing_calls == 9 && pairing_on, "the launcher closed: %d calls, on %d", pairing_calls, pairing_on);

    apps_lv_show_crash("Spin", NULL);
    refresh_after(33000);
    CHECK(pairing_calls == 10 && !pairing_on, "a card over it: %d calls, on %d", pairing_calls, pairing_on);
    apps_lv_hide_cards();
    refresh_after(33000);
    CHECK(pairing_calls == 11 && pairing_on, "the card gone: %d calls, on %d", pairing_calls, pairing_on);

    apps_lv_set_pairing_cb(NULL);  /* replaced: the old one is left off */
    CHECK(pairing_calls == 12 && !pairing_on, "the callback replaced: %d calls, on %d", pairing_calls, pairing_on);
    refresh_after(33000);
    CHECK(pairing_calls == 12, "none after: %d calls", pairing_calls);
    FINISH(&e);
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
    const apps_lv_app_t apps[] = {{"hello", "Hello", ""}};
    apps_lv_show_launcher(apps, 1, NULL, pick);
    int launcher = fake_parent(carousel_row());
    CHECK(fake_index(launcher) < fake_index(card), "a crash card over the launcher: %d %d", fake_index(launcher), fake_index(card));
    apps_lv_hide_cards();
    CHECK(fake_flag(card, LV_OBJ_FLAG_HIDDEN) && fake_flag(launcher, LV_OBJ_FLAG_HIDDEN) && !apps_lv_cards_shown(), "both closed");
    CHECK(fake_alive_of("obj") == 3 + 3, "the layer, two cards, and the carousel's row, item and tile: %d", fake_alive_of("obj"));
    LAUNCHER_END();
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
    const apps_lv_app_t apps[] = {{"hello", "Hello", ""}};
    apps_lv_show_launcher(apps, 1, NULL, pick);
    apps_lv_launcher_poll();
    apps_lv_show_crash("Timer", "boom");
    apps_lv_hide_cards();
    CHECK(!apps_lv_cards_shown() && lvgl_calls == before, "no LVGL call: %ld\n%s", lvgl_calls - before, log_peek());
}

/* ---- The final review's fixes ---- */

/* I1: apps_lv_close_launcher, which the glue calls when the screen goes dark, closes the launcher alone, as every close does: its icons
 * dropped (the loads in flight free theirs on the worker), every item its tile before LVGL draws. A crash card over it stays. Closed
 * already, it does nothing. */
static void close_launcher_closes_it_alone(void) {
    cards_scene();
    const apps_lv_app_t apps[] = {{"a1", "A1", "icon.bin"}, {"a2", "A2", "icon.bin"}, {"a3", "A3", "icon.bin"}, {"a4", "A4", "icon.bin"}};
    for (size_t i = 0; i < ARRAY_LEN(apps); i++) {
        put_icon(apps[i].id, CF_RGB565A8, 160, 160, (uint8_t)(0x20 + i));
    }
    apps_lv_show_launcher(apps, 4, NULL, pick);
    run_io();
    apps_lv_launcher_poll();
    int card = fake_parent(carousel_row());
    CHECK(strcmp(faces(), "IITT") == 0 && io_queued > 0 && held > 0, "two icons in, two on their way: %s, %d queued, %zu bytes", faces(),
          io_queued, held);
    apps_lv_show_crash("Boom", "main.lua:1: boom");
    int crash = fake_parent(fake_label("Boom stopped"));
    apps_lv_close_launcher();
    CHECK(!apps_lv_launcher_shown() && fake_flag(card, LV_OBJ_FLAG_HIDDEN), "the launcher closes");
    CHECK(apps_lv_cards_shown() && crash > 0 && !fake_flag(crash, LV_OBJ_FLAG_HIDDEN), "the crash card stays");
    CHECK(strcmp(faces(), "TTTT") == 0 && fake_draw() == 0, "every item its tile, nothing drawn from an icon: %s", faces());
    CHECK_SOURCES();
    run_io();
    CHECK(held == 0, "no icon held: %zu bytes", held);
    log_clear();
    apps_lv_close_launcher();
    CHECK(log_peek()[0] == '\0' && apps_lv_cards_shown() && !apps_lv_launcher_shown(), "closed already: nothing\n%s", log_peek());
    LAUNCHER_END();
}

/* m1: a tap in the strip from the row's bottom through the name, as wide as the name's box, is a tap on the app at the centre, which the
 * name names: it opens it, and the launcher holds, as for a tap on its icon. Under the same rules: only when the finger lifts in place
 * (a slide there does nothing), and not while the row moves (the centre app is brought to rest at the centre instead). Elsewhere on the
 * background a tap still closes (C12): beside the name, under it, at the title. */
static void a_tap_on_the_name_opens_the_centre_app(void) {
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    fake_layout();
    const lv_area_t row = fake_obj(carousel_row())->area, name = fake_obj(name_label())->area;
    const int32_t mid = (name.y1 + name.y2 + 1) / 2;
    CHECK(name.y1 > row.y2 + 1 && !fake_flag(name_label(), LV_OBJ_FLAG_CLICKABLE), "a gap from the row (to %d) to the name (from %d), "
          "which isn't clickable: its touches are the card's", (int)row.y2, (int)name.y1);
    LAUNCHER_END();
    const struct {
        lv_point_t p;
        const char *what;
    } on[] = {{{SCREEN_CENTRE, mid}, "the name's middle"}, {{SCREEN_CENTRE, row.y2 + 1}, "just under the row"},
              {{SCREEN_CENTRE, name.y1 - 1}, "the gap over the name"}, {{name.x1, name.y2}, "the name's box, bottom left"},
              {{name.x2, name.y1}, "its top right"}};
    for (size_t i = 0; i < ARRAY_LEN(on); i++) {
        cards_scene();
        apps_lv_show_launcher(FIVE, 5, NULL, pick);
        int card = fake_parent(carousel_row());
        CHECK(fake_down(on[i].p.x, on[i].p.y) == card, "%s (%d, %d) is the card's", on[i].what, (int)on[i].p.x, (int)on[i].p.y);
        fake_lift();
        CHECK(picks == 1 && strcmp(picked, "alpha") == 0 && apps_lv_launcher_holding() && !fake_flag(card, LV_OBJ_FLAG_HIDDEN),
              "a tap on %s opens the centre app, and holds: %d '%s', holding %d", on[i].what, picks, picked, apps_lv_launcher_holding());
        LAUNCHER_END();
    }
    /* The app it opens is the one at the centre: two along, at rest, the third. A finger that rolled under the scroll limit still taps. */
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    fake_scroll(carousel_row(), 2 * APPS_LV_ITEM_PITCH);
    fake_down(SCREEN_CENTRE, mid);
    fake_move(SCREEN_CENTRE + FAKE_SCROLL_LIMIT - 1, mid - (FAKE_SCROLL_LIMIT - 1));
    fake_lift();
    CHECK(picks == 1 && strcmp(picked, "charlie") == 0 && strcmp(name_shown(), "Charlie") == 0, "two along: %d '%s'", picks, picked);
    LAUNCHER_END();
    /* A slide on the name, sideways or down, opens nothing and closes nothing. */
    cards_scene();
    apps_lv_show_launcher(FIVE, 5, NULL, pick);
    slide_from(SCREEN_CENTRE, mid, FAKE_SCROLL_LIMIT, 0, 5);
    slide_from(SCREEN_CENTRE, mid, 0, 40, 4);
    slide_from(SCREEN_CENTRE, mid, -40, 0, 20);
    CHECK(picks == 0 && apps_lv_launcher_shown() && scroll_now() == 0, "slides on the name do nothing: %d picks, shown %d, scroll %d",
          picks, apps_lv_launcher_shown(), (int)scroll_now());
    /* While the row moves (a fling's snap) it opens nothing: the app at the centre comes to rest there, and then a tap opens it. */
    int row_id = carousel_row();
    lv_point_t p = face_at(0);
    fake_drag(p.x, p.y, p.x - 60, p.y, 4, 0);
    fake_lift();
    fake_frame(FRAME_MS);
    CHECK(fake_scrolling(row_id), "the snap animates");
    log_clear();
    fake_tap_at(SCREEN_CENTRE, mid);
    CHECK(picks == 0 && apps_lv_launcher_shown() && log_find("scroll_to_view") >= 0, "a tap on the name during the snap opens nothing and "
          "brings the centre app to the centre: %d picks\n%s", picks, log_peek());
    SETTLE();
    /* A press on the name while the row moves, lifted once it is at rest: one made while it moved, which opens nothing. */
    p = face_at(1);
    fake_tap_at(p.x, p.y);  /* the right neighbour: its scroll_to_view animates */
    fake_frame(FRAME_MS);
    CHECK(fake_scrolling(row_id), "the side tap's animation");
    fake_down(SCREEN_CENTRE, mid);
    SETTLE();
    fake_lift();
    CHECK(picks == 0 && apps_lv_launcher_shown() && !fake_scrolling(row_id), "a press on the name made while the row moved opens nothing at "
          "its lift, at rest: %d picks", picks);
    const char *resting = "?";
    for (size_t i = 0; i < ARRAY_LEN(FIVE); i++) {
        resting = strcmp(FIVE[i].name, name_shown()) == 0 ? FIVE[i].id : resting;
    }
    fake_tap_at(SCREEN_CENTRE, mid);
    CHECK(picks == 1 && strcmp(picked, resting) == 0, "at rest, a tap on the name opens the app it names: '%s', '%s' (%s)", picked, resting,
          name_shown());
    LAUNCHER_END();
    /* Just outside the strip it is the background, and a tap closes (C12). */
    const struct {
        lv_point_t p;
        const char *what;
    } off[] = {{{name.x1 - 1, mid}, "left of the name"}, {{name.x2 + 1, mid}, "right of it"}, {{SCREEN_CENTRE, name.y2 + 1}, "under it"},
               {{SCREEN_CENTRE, 66}, "the title"}};
    for (size_t i = 0; i < ARRAY_LEN(off); i++) {
        cards_scene();
        apps_lv_show_launcher(FIVE, 5, NULL, pick);
        int card = fake_parent(carousel_row());
        CHECK(fake_down(off[i].p.x, off[i].p.y) == card, "%s is the card's", off[i].what);
        fake_lift();
        CHECK(picks == 0 && !apps_lv_launcher_shown(), "a tap %s closes, and opens nothing: %d picks, shown %d", off[i].what, picks,
              apps_lv_launcher_shown());
        LAUNCHER_END();
    }
    /* With no carousel (no apps yet, or loading) there is no name: a tap there closes. */
    static const int ns[] = {0, APPS_LV_LOADING};
    for (size_t i = 0; i < ARRAY_LEN(ns); i++) {
        cards_scene();
        apps_lv_show_launcher(NULL, ns[i], NULL, pick);
        fake_tap_at(SCREEN_CENTRE, mid);
        CHECK(picks == 0 && !apps_lv_launcher_shown(), "n %d: a tap where the name would be closes", ns[i]);
        LAUNCHER_END();
    }
}

/* Hardening (a), 6a m2: the running app's dot is under its face as the face is drawn, a neighbour's small face too, not at the bottom of
 * its item's box: the same gap under the face at every scale as at the centre (where it is at the box's bottom, as before), and centred
 * under it, with tiles and with icons of 160 and 120 px. */
static void running_dot_follows_its_face(void) {
    const apps_lv_app_t tiles[] = {{"d1", "D1", ""}, {"d2", "D2", ""}, {"d3", "D3", ""}, {"d4", "D4", ""}, {"d5", "D5", ""}};
    const apps_lv_app_t icons[] = {{"d1", "D1", "icon.bin"}, {"d2", "D2", "icon.bin"}, {"d3", "D3", "icon.bin"}, {"d4", "D4", "icon.bin"},
                                   {"d5", "D5", "icon.bin"}};
    const int P = APPS_LV_ITEM_PITCH;
    for (int kind = 0; kind < 3; kind++) {
        const char *what = kind == 0 ? "tiles" : kind == 1 ? "icons of 160 px" : "icons of 120 px";
        const unsigned px = kind == 1 ? 160 : 120;
        cards_scene();
        if (kind > 0) {
            for (int i = 0; i < 5; i++) {
                put_icon(icons[i].id, CF_RGB565A8, px, px, (uint8_t)(0x10 + i));
            }
        }
        apps_lv_show_launcher(kind == 0 ? tiles : icons, 5, "d3", pick);
        int row = carousel_row(), item = carousel_item(2), dot = item_parts(item).dot;
        CHECK(dot > 0, "%s: d3 runs: its dot", what);
        if (dot <= 0) {
            LAUNCHER_END();
            continue;
        }
        /* At the centre: under the face, at the bottom of the box, as before. */
        fake_scroll(row, 2 * P);
        for (int round = 0; round < 4; round++) {
            run_io();
            apps_lv_launcher_poll();
        }
        lv_area_t f = face_area(2);
        fake_layout();
        const lv_area_t *d = &fake_obj(dot)->area, *box = &fake_obj(item)->area;
        const int32_t gap = d->y1 - f.y2 - 1;
        CHECK(item_face(item) == (kind == 0 ? 'T' : 'I'), "%s: d3's face: %s", what, faces());
        CHECK(gap >= 1 && gap <= 10 && d->y2 == box->y2, "%s: at the centre the dot is %d px under the face, at the box's bottom (%d, %d)",
              what, (int)gap, (int)d->y2, (int)box->y2);
        /* Across the scroll: d3 from two pitches to the right, through the centre, to two to the left. */
        for (int32_t sx = 0; sx <= 4 * P; sx += P / 10) {
            fake_scroll(row, sx);
            run_io();
            apps_lv_launcher_poll();
            f = face_area(2);
            fake_layout();
            d = &fake_obj(dot)->area;
            box = &fake_obj(item)->area;
            const int32_t g = d->y1 - f.y2 - 1, dx = (d->x1 + d->x2 + 1) / 2 - (f.x1 + f.x2 + 1) / 2;
            CHECK(g - gap <= 1 && gap - g <= 1, "%s, scroll %d: the dot is %d px under its %d px face, not %d", what, (int)sx, (int)g,
                  (int)(f.x2 - f.x1 + 1), (int)gap);
            CHECK(dx <= 1 && dx >= -1, "%s, scroll %d: the dot is %d px off the face's middle", what, (int)sx, (int)dx);
            CHECK(d->y1 >= box->y1 && d->y2 <= box->y2, "%s, scroll %d: the dot is inside its item (%d-%d, the box %d-%d)", what, (int)sx,
                  (int)d->y1, (int)d->y2, (int)box->y1, (int)box->y2);
        }
        LAUNCHER_END();
    }
}

/* ---- Runtime images (connected apps Task 2): the real runtime, its table and the real pass ----
 *
 * A runtime image is a block the app task holds (apps_rt_image_add), which an image object draws from in place through the pass's
 * descriptor of its slot. Never blank: its bytes are freed only once a pass has reported that no object draws them, and at an app's
 * end only after the layer's reset. These cases run the runtime (apps_rt.c, with apps_svc.c, its services) on this fake LVGL, draw
 * every image object after each step, every byte of its pixels whether it shows or not (so a freed block drawn is ASan's), and
 * watch each block for its free. */

typedef struct {
    apps_pkg_t *pkg;
    apps_rt_t *rt;
    env_t e;  /* the runtime's VM and table, for RUN and APPLY */
} rtenv_t;

/* Every image object that isn't deleted, drawn from its descriptor: every byte of its pixels. How many. */
static int draw_every_image(void) {
    int drawn = 0;
    for (int i = 0; i < n_objs; i++) {
        const lv_obj_t *o = objs[i];
        if (o->deleted || strcmp(o->cls, "image") != 0 || o->src == NULL) {
            continue;
        }
        unsigned sum = 0;
        for (uint32_t k = 0; k < o->src->data_size; k++) {
            sum += o->src->data[k];
        }
        drawn_bytes += sum;
        drawn++;
    }
    return drawn;
}

/* A block of w x h in cf, in the apps' heap as a decode makes one, given to the runtime: its slot, and the block's watch in *watched. */
static int rt_add(rtenv_t *r, unsigned cf, unsigned w, unsigned h, int *watched) {
    unsigned stride = cf == CF_A8 ? w : w * 2;
    size_t n = 12 + (size_t)stride * h + (cf == CF_RGB565A8 ? (size_t)(stride / 2) * h : 0);
    uint8_t *b = apps_port_realloc(NULL, n);
    if (b == NULL) {
        return -1;
    }
    for (size_t i = 12; i < n; i++) {
        b[i] = (uint8_t)(i * 7);
    }
    const uint8_t header[12] = {0x19, (uint8_t)cf, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8),
                                (uint8_t)stride, (uint8_t)(stride >> 8), 0, 0};
    memcpy(b, header, sizeof header);
    if (watched != NULL && watch_n < WATCH_MAX) {
        watch_ptr[watch_n] = b;
        watch_gone[watch_n] = false;
        *watched = watch_n++;
    }
    char err[200] = "";
    int slot = apps_rt_image_add(r->rt, b, n, err, sizeof err);
    CHECK(slot >= 0, "apps_rt_image_add: %s", err);
    return slot;
}

typedef struct {
    apps_rt_t *rt;
    int slot;
    const char *name;
} rt_push_t;

static int rt_push_trampoline(lua_State *L) {
    const rt_push_t *p = lua_touserdata(L, 1);
    apps_rt_image_push(L, p->rt, p->slot);
    lua_setglobal(L, p->name);
    return 0;
}

/* The handle of the image at slot, pushed inside a call as a callback's trampoline will push it, into the global name. */
static void rt_push(rtenv_t *r, int slot, const char *name) {
    rt_push_t p = {r->rt, slot, name};
    lua_State *L = apps_vm_lua(r->e.vm);
    lua_pushcfunction(L, rt_push_trampoline);
    lua_pushlightuserdata(L, &p);
    char err[256] = "";
    CHECK(apps_vm_call(r->e.vm, 1, 0, APPS_BUDGET_CALL, err, sizeof err) == APPS_CALL_OK, "push %s: %s", name, err);
}

/* A fresh fake LVGL with its layer in front, and a running app with a.bin in its package, its table published and the first pass
 * done. */
static void rt_scene(rtenv_t *r) {
    fake_reset();
    refuse_from = -1;
    trylock_refusals = 0;
    create_fail_in = 0;
    watch_n = 0;
    recycle = false;
    r->pkg = calloc(1, sizeof *r->pkg);
    if (r->pkg == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    static const char main_lua[] = "return {}";
    snprintf(r->pkg->manifest.id, sizeof r->pkg->manifest.id, "pics");
    snprintf(r->pkg->manifest.name, sizeof r->pkg->manifest.name, "Pics");
    snprintf(r->pkg->manifest.main, sizeof r->pkg->manifest.main, "main.lua");
    r->pkg->manifest.memory_kb = 256;
    r->pkg->manifest.file_count = 2;
    snprintf(r->pkg->manifest.files[0], sizeof r->pkg->manifest.files[0], "main.lua");
    snprintf(r->pkg->manifest.files[1], sizeof r->pkg->manifest.files[1], "a.bin");
    snprintf(r->pkg->files[0].path, sizeof r->pkg->files[0].path, "main.lua");
    r->pkg->files[0].data = (uint8_t *)exact_copy(main_lua, sizeof main_lua - 1);
    r->pkg->files[0].len = sizeof main_lua - 1;
    snprintf(r->pkg->files[1].path, sizeof r->pkg->files[1].path, "a.bin");
    r->pkg->files[1].data = (uint8_t *)exact_copy((const char *)IMG_A, sizeof IMG_A);
    r->pkg->files[1].len = sizeof IMG_A;
    r->pkg->count = 2;
    char err[256] = "";
    r->rt = apps_rt_new(r->pkg, err, sizeof err);
    if (r->rt == NULL || !apps_rt_start(r->rt)) {
        fprintf(stderr, "the runtime: %s\n", err);
        exit(2);
    }
    r->e.vm = apps_rt_vm(r->rt);
    r->e.ui = apps_rt_ui(r->rt);
    publish(&r->e);
    apps_lv_build(fake_screen());
    apps_lv_set_front(true);
    apply_at(__LINE__, &r->e, false);
    log_clear();
    stats_base = stats_now();
}

/* An app's end, in apps_task.c's order (end_app): the table unpublished, the runtime's images taken, the runtime freed; then the
 * caller draws, resets the layer and frees what was taken. */
static int rt_end(rtenv_t *r, void *taken[APPS_RT_IMAGES_MAX]) {
    unpublish();
    int n = apps_rt_images_take(r->rt, taken);
    apps_rt_free(r->rt);
    apps_pkg_free(r->pkg, free);
    free(r->pkg);
    r->rt = NULL;
    r->e.vm = NULL;
    r->e.ui = NULL;
    return n;
}

static void rt_finish_at(int line, rtenv_t *r) {
    void *taken[APPS_RT_IMAGES_MAX];
    int n = rt_end(r, taken);
    apps_lv_reset();
    for (int i = 0; i < n; i++) {
        apps_port_free(taken[i]);
    }
    CHECK_AT(line, held == 0, "%zu bytes still held", held);
    CHECK_AT(line, fake_errors == 0, "the fake LVGL counted %d errors, the first: %s", fake_errors, fake_note);
    CHECK_AT(line, !lock_held && lock_errors == 0, "the lock: held %d, %d errors, the first: %s", lock_held, lock_errors, lock_note);
}
#define RT_FINISH(r) rt_finish_at(__LINE__, (r))

/* ui.image{src = img}: the pass draws a runtime image from its block in place, as it draws a package image, and rotation, spin and
 * scale apply to it as they do to a package image's (P1: the angle and the spin don't care where the descriptor came from). */
static void runtime_image_shows_in_ui_image(void) {
    rtenv_t r;
    rt_scene(&r);
    int img_watch = 0, disc_watch = 0;
    int slot = rt_add(&r, CF_RGB565A8, 6, 4, &img_watch);
    int disc = rt_add(&r, CF_RGB565, 200, 200, &disc_watch);
    CHECK(slot == 0 && disc == 1, "slots %d %d", slot, disc);
    rt_push(&r, slot, "img");
    rt_push(&r, disc, "disc");
    RUN(&r.e, "w = ui.image{src = img} p = ui.image{src = 'a.bin'}");
    CHECK(APPLY(&r.e), "made");
    lv_obj_t *w = fake_obj(2), *p = fake_obj(3);
    CHECK(log_has_line("image_src 2 6x4 cf=0x14 data=72"), "the runtime image's descriptor\n%s", log_peek());
    CHECK(log_has_line("image_src 3 2x1 cf=0x12 data=4"), "the package's\n%s", log_peek());
    CHECK(w->src != NULL && w->src->data == (const uint8_t *)watch_ptr[img_watch] + 12, "drawn from the block in place");
    CHECK(w->area.x2 - w->area.x1 + 1 == 6 && w->area.y2 - w->area.y1 + 1 == 4, "laid out at its size");
    CHECK(draw_every_image() == 2, "both drawn");

    /* The same calls as a package image's: the angle, the spin, the scale. */
    log_clear();
    RUN(&r.e, "w:set{src = disc, rotation = 90, spin = 30, scale = 50}");
    CHECK(APPLY(&r.e), "changed");
    CHECK(log_has_line("image_src 2 200x200 cf=0x12 data=80000") && log_has_line("image_rotation 2 900") &&
              log_has_line("image_scale 2 128") && fake_spins() == 1 && stats_now().spinning == 1,
          "rotation, spin and scale on a runtime image\n%s", log_peek());
    CHECK(w->src->data == (const uint8_t *)watch_ptr[disc_watch] + 12 && w->area.x2 - w->area.x1 + 1 == 100,
          "the other block, its box the drawn size");
    CHECK(draw_every_image() == 2, "drawn");
    fake_frame(500);
    CHECK(w->rotation != 900, "it turns: %d", (int)w->rotation);
    log_clear();
    RUN(&r.e, "w:set{src = img}");
    CHECK(APPLY(&r.e), "changed");
    CHECK(log_has_line("image_src 2 6x4 cf=0x14 data=72") && fake_spins() == 1 && log_count("anim_") == 0,
          "a src change leaves the spin alone\n%s", log_peek());
    (void)p;
    RT_FINISH(&r);
}

/* Review Focus 3. The widget is deleted, then img:free(): the object is still on the layer until the next pass, so the bytes stay,
 * and the screen draws them; after the pass the collect frees them, and nothing draws them again. A free while the widget shows it is
 * an error. The same for a widget moved to another image, for ui.clear(), and for a box deleted with the image in it. */
static void freed_runtime_image_is_never_drawn(void) {
    rtenv_t r;
    rt_scene(&r);
    int a_watch = 0;
    int a = rt_add(&r, CF_RGB565, 8, 8, &a_watch);
    rt_push(&r, a, "img");
    RUN(&r.e, "w = ui.image{src = img}");
    CHECK(APPLY(&r.e), "made");
    CHECK(draw_every_image() == 1, "drawn");
    char err[512] = "";
    CHECK(!run(&r.e, "img:free()", err, sizeof err) && strstr(err, "image is in use by image#0") != NULL, "free while shown: '%s'", err);
    CHECK(!watch_gone[a_watch], "nothing freed");

    RUN(&r.e, "w:delete() img:free()");
    CHECK(!watch_gone[a_watch], "the object is still there: the bytes stay");
    CHECK(draw_every_image() == 1, "and the screen draws them");
    apps_rt_image_collect(r.rt);
    CHECK(!watch_gone[a_watch], "a collect before the pass frees nothing");
    CHECK(draw_every_image() == 1, "drawn again");
    CHECK(APPLY(&r.e), "the pass takes the object down");
    CHECK(draw_every_image() == 0 && !watch_gone[a_watch], "nothing draws it, and the app task hasn't looked yet");
    apps_rt_image_collect(r.rt);
    CHECK(watch_gone[a_watch], "freed after the pass");
    CHECK(draw_every_image() == 0, "never drawn again");

    /* A widget moved to another image. */
    int b_watch = 0, c_watch = 0;
    int b = rt_add(&r, CF_RGB565, 4, 4, &b_watch), c = rt_add(&r, CF_A8, 4, 4, &c_watch);
    rt_push(&r, b, "b");
    rt_push(&r, c, "c");
    RUN(&r.e, "v = ui.image{src = b}");
    CHECK(APPLY(&r.e), "made");
    RUN(&r.e, "v:set{src = c} b:free()");
    CHECK(draw_every_image() == 1 && !watch_gone[b_watch], "the object still draws b until the pass");
    apps_rt_image_collect(r.rt);
    CHECK(!watch_gone[b_watch], "kept");
    CHECK(APPLY(&r.e), "the pass moves it to c");
    apps_rt_image_collect(r.rt);
    CHECK(watch_gone[b_watch] && !watch_gone[c_watch] && draw_every_image() == 1, "b freed, c drawn");

    /* ui.clear(). */
    RUN(&r.e, "ui.clear() c:free()");
    apps_rt_image_collect(r.rt);
    CHECK(!watch_gone[c_watch] && draw_every_image() == 1, "kept until the pass cleans the layer");
    CHECK(APPLY(&r.e), "cleaned");
    apps_rt_image_collect(r.rt);
    CHECK(watch_gone[c_watch] && draw_every_image() == 0, "freed");

    /* A box deleted with the image in it (the pass deletes the box's object, and LVGL the image's with it). */
    int d_watch = 0;
    int d = rt_add(&r, CF_RGB565, 4, 4, &d_watch);
    rt_push(&r, d, "d");
    RUN(&r.e, "box = ui.box{w = 100, h = 100} u = ui.image{src = d, parent = box}");
    CHECK(APPLY(&r.e), "made");
    RUN(&r.e, "box:delete() d:free()");
    apps_rt_image_collect(r.rt);
    CHECK(!watch_gone[d_watch] && draw_every_image() == 1, "kept");
    CHECK(APPLY(&r.e), "the box goes");
    apps_rt_image_collect(r.rt);
    CHECK(watch_gone[d_watch] && draw_every_image() == 0, "freed");

    /* An image no pass ever drew goes at once. */
    int e_watch = 0;
    int e = rt_add(&r, CF_RGB565, 4, 4, &e_watch);
    rt_push(&r, e, "e");
    RUN(&r.e, "t = ui.image{src = e} t:delete() e:free()");
    CHECK(watch_gone[e_watch], "made and deleted before a pass: no object, freed at once");
    CHECK(APPLY(&r.e), "nothing to make");
    CHECK(draw_every_image() == 0, "nothing drawn");
    RT_FINISH(&r);
}

/* An app's end: the runtime is freed after its table is unpublished, but its images' bytes, taken from it, stay until the layer's
 * reset has taken the objects down; the screen draws them until then. A freed image the screen still drew goes the same way. */
static void images_free_at_app_end_after_the_reset(void) {
    rtenv_t r;
    rt_scene(&r);
    int a_watch = 0, b_watch = 0, c_watch = 0;
    int a = rt_add(&r, CF_RGB565A8, 10, 10, &a_watch), b = rt_add(&r, CF_RGB565, 4, 4, &b_watch);
    int c = rt_add(&r, CF_RGB565, 2, 2, &c_watch);
    rt_push(&r, a, "a");
    rt_push(&r, b, "b");
    rt_push(&r, c, "c");
    RUN(&r.e, "w = ui.image{src = a, spin = 20} v = ui.image{src = b} c:free()");
    CHECK(watch_gone[c_watch], "c, never drawn, is gone already");
    CHECK(APPLY(&r.e), "made");
    RUN(&r.e, "v:delete() b:free()");  /* still drawn */
    CHECK(draw_every_image() == 2, "both drawn");
    void *taken[APPS_RT_IMAGES_MAX];
    int n = rt_end(&r, taken);
    CHECK(n == 2, "a and b taken: %d", n);
    CHECK(!watch_gone[a_watch] && !watch_gone[b_watch], "the runtime freed neither");
    CHECK(draw_every_image() == 2, "the objects still draw them: the reset hasn't run");
    apps_lv_reset();
    CHECK(draw_every_image() == 0 && fake_spins() == 0, "the reset took them down");
    for (int i = 0; i < n; i++) {
        apps_port_free(taken[i]);
    }
    CHECK(watch_gone[a_watch] && watch_gone[b_watch], "freed after the reset");
    CHECK(held == 0, "%zu bytes still held", held);
    CHECK(fake_errors == 0, "the fake LVGL counted %d errors, the first: %s", fake_errors, fake_note);
}

/* A slot's next image can be another picture in a block at the address the last one had (the heap gives a freed block's address to the
 * next request): the descriptor is made again from the new header, not kept because its pixels' address is the same. */
static void a_slots_next_image_can_be_at_the_same_address(void) {
    rtenv_t r;
    rt_scene(&r);
    int a_watch = 0, b_watch = 0;
    int a = rt_add(&r, CF_RGB565, 8, 4, &a_watch);
    rt_push(&r, a, "a");
    RUN(&r.e, "w = ui.image{src = a}");
    CHECK(APPLY(&r.e), "made");
    CHECK(log_has_line("image_src 2 8x4 cf=0x12 data=64"), "8 x 4\n%s", log_peek());
    recycle = true;
    RUN(&r.e, "w:delete() a:free()");
    CHECK(APPLY(&r.e), "deleted");
    apps_rt_image_collect(r.rt);
    CHECK(watch_gone[a_watch] && recycled != NULL, "a's block is freed, and kept for the next request");
    log_clear();
    int b = rt_add(&r, CF_RGB565, 4, 8, &b_watch);
    CHECK(b == a && watch_ptr[b_watch] == watch_ptr[a_watch], "b is in a's slot, at a's address");
    rt_push(&r, b, "b");
    RUN(&r.e, "v = ui.image{src = b}");
    CHECK(APPLY(&r.e), "made");
    lv_obj_t *v = fake_obj(3);
    CHECK(log_has_line("image_src 3 4x8 cf=0x12 data=64") && v->src->header.w == 4 && v->src->header.h == 8,
          "the descriptor is b's: 4 x 8\n%s", log_peek());
    CHECK(draw_every_image() == 1, "drawn");
    RT_FINISH(&r);
}

/* The pass's report is the slots its objects draw from, made once a pass changes something: a rejected widget's object keeps its
 * picture, so its bit stays, and the image isn't freed under it. */
static void the_pass_reports_what_it_draws(void) {
    rtenv_t r;
    rt_scene(&r);
    int a_watch = 0;
    int a = rt_add(&r, CF_RGB565, 4, 4, &a_watch), b = rt_add(&r, CF_RGB565, 2, 2, NULL);
    rt_push(&r, a, "a");
    rt_push(&r, b, "b");
    CHECK(apps_ui_images_drawn(r.e.ui) == 0, "nothing drawn before a pass");
    RUN(&r.e, "w = ui.image{src = a} v = ui.image{src = b} p = ui.image{src = 'a.bin'}");
    CHECK(APPLY(&r.e), "made");
    CHECK(apps_ui_images_drawn(r.e.ui) == 3u, "slots 0 and 1: 0x%x", (unsigned)apps_ui_images_drawn(r.e.ui));
    RUN(&r.e, "v:delete()");
    CHECK(APPLY(&r.e), "deleted");
    CHECK(apps_ui_images_drawn(r.e.ui) == 1u, "slot 0: 0x%x", (unsigned)apps_ui_images_drawn(r.e.ui));
    CHECK(APPLY(&r.e), "a pass with nothing to do");
    CHECK(apps_ui_images_drawn(r.e.ui) == 1u, "the same: 0x%x", (unsigned)apps_ui_images_drawn(r.e.ui));

    /* w is moved to the package's image but goes bad in the table before the pass: rejected, its object keeps drawing a. */
    RUN(&r.e, "w:set{src = 'a.bin'} a:free()");
    W(&r.e, 0)->opacity = 101;
    CHECK(APPLY(&r.e), "the pass ran");
    CHECK(stats_since().rejects == 1, "rejected: %u", stats_since().rejects);
    CHECK(apps_ui_images_drawn(r.e.ui) == 1u, "its object still draws slot 0: 0x%x", (unsigned)apps_ui_images_drawn(r.e.ui));
    apps_rt_image_collect(r.rt);
    CHECK(!watch_gone[a_watch] && draw_every_image() == 2, "kept while drawn");
    W(&r.e, 0)->opacity = 100;
    RT_FINISH(&r);
    CHECK(watch_gone[a_watch], "freed after the reset");
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"runtime_image_shows_in_ui_image", runtime_image_shows_in_ui_image},
        {"freed_runtime_image_is_never_drawn", freed_runtime_image_is_never_drawn},
        {"images_free_at_app_end_after_the_reset", images_free_at_app_end_after_the_reset},
        {"the_pass_reports_what_it_draws", the_pass_reports_what_it_draws},
        {"a_slots_next_image_can_be_at_the_same_address", a_slots_next_image_can_be_at_the_same_address},
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
        {"scale_curve", scale_curve},
        {"tile_colors", tile_colors},
        {"tile_letter", tile_letter},
        {"opens_centred_on_first_app", opens_centred_on_first_app},
        {"one_name_label_shows_the_centre_app", one_name_label_shows_the_centre_app},
        {"running_dot_under_running_app", running_dot_under_running_app},
        {"icon_or_tile_never_blank", icon_or_tile_never_blank},
        {"every_app_is_an_item", every_app_is_an_item},
        {"loading_then_list_rebuilds", loading_then_list_rebuilds},
        {"long_and_non_ascii_names", long_and_non_ascii_names},
        {"launcher_lists_apps_and_picks", launcher_lists_apps_and_picks},
        {"launcher_closes_on_a_tap_elsewhere", launcher_closes_on_a_tap_elsewhere},
        {"launcher_empty_state", launcher_empty_state},
        {"launcher_a_swipe_on_an_app_closes_nothing", launcher_a_swipe_on_an_app_closes_nothing},
        {"launcher_keeps_its_own_copy", launcher_keeps_its_own_copy},
        {"launcher_survives_a_create_that_fails", launcher_survives_a_create_that_fails},
        {"a_scroll_after_a_close_asks_nothing", a_scroll_after_a_close_asks_nothing},
        {"tap_centre_launches", tap_centre_launches},
        {"tap_side_scrolls_not_launches", tap_side_scrolls_not_launches},
        {"tap_while_scrolling_does_not_launch", tap_while_scrolling_does_not_launch},
        {"bottom_edge_swipe_up_closes", bottom_edge_swipe_up_closes},
        {"swipe_up_from_middle_does_nothing", swipe_up_from_middle_does_nothing},
        {"right_swipe_no_longer_closes", right_swipe_no_longer_closes},
        {"horizontal_swipe_only_scrolls", horizontal_swipe_only_scrolls},
        {"a_short_slow_sideways_drag_on_the_background_does_not_close", a_short_slow_sideways_drag_on_the_background_does_not_close},
        {"a_short_slow_sideways_drag_on_the_name_does_not_close", a_short_slow_sideways_drag_on_the_name_does_not_close},
        {"a_tap_on_the_background_still_closes", a_tap_on_the_background_still_closes},
        {"a_slide_on_an_app_opens_nothing", a_slide_on_an_app_opens_nothing},
        {"close_launcher_closes_it_alone", close_launcher_closes_it_alone},
        {"a_tap_on_the_name_opens_the_centre_app", a_tap_on_the_name_opens_the_centre_app},
        {"running_dot_follows_its_face", running_dot_follows_its_face},
        {"centre_is_clearly_larger", centre_is_clearly_larger},
        {"the_hold_drops_offscreen_icons", the_hold_drops_offscreen_icons},
        {"launch_holds_the_launcher_until_the_first_frame", launch_holds_the_launcher_until_the_first_frame},
        {"no_input_while_the_launch_holds", no_input_while_the_launch_holds},
        {"the_hold_times_out", the_hold_times_out},
        {"a_voice_turn_ends_the_hold", a_voice_turn_ends_the_hold},
        {"a_touch_the_launcher_closed_under_does_nothing", a_touch_the_launcher_closed_under_does_nothing},
        {"launcher_frame_log", launcher_frame_log},
        {"image_rotation_applies", image_rotation_applies},
        {"image_spin_animates_and_stops_in_place", image_spin_animates_and_stops_in_place},
        {"spin_invalidates_what_it_draws", spin_invalidates_what_it_draws},
        {"rotation_sets_the_angle_a_spin_continues_from", rotation_sets_the_angle_a_spin_continues_from},
        {"spin_ends_with_the_widget", spin_ends_with_the_widget},
        {"spin_frame_log", spin_frame_log},
        {"pairing_follows_the_spin", pairing_follows_the_spin},
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
