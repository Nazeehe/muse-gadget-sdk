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
 * The host harness of Link's app.control tap and drag (components/muse/apps/apps_touch.c), for tests/test_muse_apps_touch.py: one
 * case a run, named on the command line. The real apps_touch.c runs over fakes:
 * - the I/O worker: apps_io_submit queues the job, which the case runs (run_jobs), on its own thread;
 * - the clock (apps_port_mono_ms) stands still but for what vTaskDelay sleeps (a tick is a millisecond); after each delay of the job
 *   comes one LVGL frame (apps_touch_supervise) and one input read of the touch player, as the LVGL task's come while the job polls;
 * - the touch player (muse_ui_guarded_drag and _withdraw): it keeps the guard and plays the drag as muse_ui.c's does
 *   (test_muse_bench.py has the real one): at its take a finger refuses it, then admit; at each read after, a finger or keep cuts it;
 *   the end at the read after the release. It plays LVGL's part too: the events a widget with a handler gets go through
 *   apps_touch_tag, as apps_task.c's on_widget_event sends them, and their tagged codes are kept for the case; and a drag that is
 *   LVGL's gesture is the app layer's swipe (apps_lv.c's on_gesture): the touch taken, and the close from the left edge, or the
 *   swipe the app hears;
 * - the screen's sleep (dark until a time on that clock, and a wake that lights it some ms later), the voice turn, the layer's place
 *   and its cards, what LVGL would press at a point (apps_lv_hit: the fake screen's widgets), the widgets' kinds, the scroll boxes
 *   (one that a drag on it scrolls, and that then settles), the app that runs and its app_gen, muse_input_wake.
 * Every block the touch takes (cJSON's, the port's) is counted, and every case ends with none held and nothing queued.
 */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_port.h"
#include "apps_task.h"
#include "apps_touch.h"
#include "apps_ui.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_apps.h"
#include "muse_input.h"
#include "muse_ui.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const char *current_case = "";

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void fail_at(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: line %d: ", current_case, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
#define CHECK(cond, ...)                    \
    do {                                    \
        if (!(cond)) {                      \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                   \
    } while (0)

/* ---- The blocks: cJSON's and the port's, counted; the case may refuse the Nth of them ---- */

static int blocks;
static long allocations;
static long refuse_at;   /* refuse the allocation of this number (1 = the first after arming); 0: none */
static bool refused;
static bool counting = true;

static bool take_one(void) {
    if (!counting) {
        return true;
    }
    allocations++;
    if (refuse_at != 0 && allocations == refuse_at) {
        refused = true;
        return false;
    }
    return true;
}

static void *cjson_malloc(size_t n) {
    void *p = take_one() ? malloc(n) : NULL;
    blocks += p != NULL;
    return p;
}

static void cjson_free(void *p) {
    if (p != NULL) {
        blocks--;
        free(p);
    }
}

void *apps_port_realloc(void *ptr, size_t n) {
    CHECK(n > 0, "apps_port_realloc with 0 bytes");
    void *p = take_one() ? realloc(ptr, n) : NULL;
    if (p != NULL && ptr == NULL) {
        blocks++;
    }
    return p;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        blocks--;
        free(ptr);
    }
}

/* apps_ui.c is linked for its direction names (apps_ui_dir_name): the touch never takes the UI lock. */
void apps_port_ui_lock(void) {
    fail_at(__LINE__, "the touch took the UI lock");
}

void apps_port_ui_unlock(void) {
    fail_at(__LINE__, "the touch let go of the UI lock");
}

/* ---- The log ---- */

static char logged[65536];
static size_t logged_len;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int n = snprintf(logged + logged_len, sizeof logged - logged_len, "%c %s: %s\n", level, tag, line);
    if (n > 0 && logged_len + (size_t)n < sizeof logged) {
        logged_len += (size_t)n;
    }
}

/* ---- The clock, the screen and the voice ---- */

static int64_t now_ms = 100000;
static int64_t dark_until;      /* dark while the clock is before it */
static bool dark_forever;
static int wake_lights_ms;      /* a wake of a dark screen lights it this long after (0: at once) */
static int wakes;
static const char *wake_why;
static bool in_turn;

int64_t apps_port_mono_ms(void) {
    return now_ms;
}

bool apps_port_dark(void) {
    return dark_forever || now_ms < dark_until;
}

bool apps_port_in_turn(void) {
    return in_turn;
}

void muse_input_wake(const char *why) {
    wakes++;
    wake_why = why;
    if (!dark_forever && apps_port_dark()) {
        dark_until = now_ms + wake_lights_ms;
    }
}

/* ---- The store (apps_result.c's): never asked by the touch ---- */

apps_storage_t apps_store_mode(void) {
    fail_at(__LINE__, "the touch asked the store's mode");
}

bool apps_store_has(const char *id) {
    fail_at(__LINE__, "the touch asked the store for %s", id);
}

/* ---- The app that runs, the layer and the screen's widgets ---- */

static const char *running_id = "list";
static uint32_t app_gen = 7;
static bool app_alive = true;              /* its table on the layer, no reset outstanding */
static apps_touch_target_t target = APPS_TOUCH_TARGET_OK;  /* what the names lock says beyond the id */
static bool layer_front = true, cards = false;
static int targets, alives;

bool apps_task_running(char *id, size_t cap) {
    snprintf(id, cap, "%s", running_id != NULL ? running_id : "");
    return running_id != NULL;
}

apps_touch_target_t apps_task_touch_target(const char *id, uint32_t *gen) {
    targets++;
    if (target != APPS_TOUCH_TARGET_OK) {
        return target;
    }
    if (running_id == NULL || strcmp(id, running_id) != 0 || !app_alive) {
        return APPS_TOUCH_TARGET_GONE;
    }
    *gen = app_gen;
    return APPS_TOUCH_TARGET_OK;
}

bool apps_task_touch_alive(uint32_t gen) {
    alives++;
    return gen == app_gen && app_alive && target != APPS_TOUCH_TARGET_HUNG;
}

bool apps_lv_front(void) {
    return layer_front;
}

bool apps_lv_cards_shown(void) {
    return cards;
}

/* The fake screen: a button (slot 4, a tap handler) across y 100 to 179, a scroll box (slot 7, no handler) across y 200 to 399
 * whose rows (slot 9 on, labels with a tap handler each, 40 px tall) scroll in it, and the layer elsewhere. covered says something
 * is over all of it from that read of a touch on (0: never). */
#define BUTTON 4
#define LIST 7
#define ROW0 9
static int covered_from;
static int reads_now;           /* reads of the touch that plays, from its take (1) on */
static int32_t list_y;          /* the box's scroll y */
static int32_t list_y_max = 600;
static int list_moving_until;   /* the box is scrolling until this clock (a throw), or while a drag scrolls it */
static bool list_dragging;

int apps_lv_hit(int32_t x, int32_t y) {
    CHECK(x >= 0 && x <= 465 && y >= 0 && y <= 465, "a hit off the screen: %d,%d", (int)x, (int)y);
    if (covered_from != 0 && reads_now >= covered_from) {
        return APPS_LV_HIT_OUTSIDE;
    }
    if (y >= 100 && y < 180) {
        return BUTTON;
    }
    if (y >= 200 && y < 400) {
        int row = (int)((y - 200 + list_y) / 40);
        return row < 20 ? ROW0 + row : LIST;
    }
    return APPS_LV_HIT_LAYER;
}

/* The 1.75C's screen is the apps' canvas: a point shows where it is. */
void apps_lv_to_screen(int32_t *x, int32_t *y) {
    (void)x, (void)y;
}

int apps_lv_kind(int slot) {
    if (slot == BUTTON) {
        return APPS_W_BUTTON;
    }
    if (slot == LIST) {
        return APPS_W_BOX;
    }
    if (slot >= ROW0 && slot < ROW0 + 20) {
        return APPS_W_LABEL;
    }
    return -1;
}

static bool has_handler(int slot) {
    return slot == BUTTON || (slot >= ROW0 && slot < ROW0 + 20);
}

int apps_lv_scrolls(apps_lv_scroll_t *out, int max) {
    if (max < 1) {
        return 0;
    }
    out[0] = (apps_lv_scroll_t){.slot = LIST, .y = list_y, .moving = list_dragging || now_ms < list_moving_until};
    return 1;
}

/* ---- The touch player, with LVGL's part ---- */

/* LVGL 9.5.0's gesture (indev_gesture), as tests/muse_apps_lv_harness.c's fake has it: test_muse_apps_touch.py pins these to that
 * fake's, which test_muse_apps_lv.py pins to LVGL's. */
#define FAKE_GESTURE_LIMIT 50         /* LV_INDEV_DEF_GESTURE_LIMIT: past this far on one axis a drag is a gesture */
#define FAKE_GESTURE_MIN_VELOCITY 3   /* LV_INDEV_DEF_GESTURE_MIN_VELOCITY: a read that moves less on both starts it again */

static const muse_ui_guard_t *guard;
static bool pending, playing, ending, no_input;
static int px1, py1, px2, py2, pms;
static int64_t pstart;
static muse_ui_guard_end_t end_how;
static uint32_t end_ms;
static int last_x, last_y;
static int pressed_slot;        /* what LVGL pressed at the take (a widget's slot, or -1) */
static bool scrolled, long_sent;
static int sum_x, sum_y;        /* the gesture's movement so far (pointer.gesture_sum) */
static bool gesture_sent;       /* pointer.gesture_sent */
static bool waiting;            /* the layer took the touch (lv_indev_wait_release): nothing more but the lift's PRESS_LOST */
static int closes;              /* the layer's closes (apps_task.c's on_close) */
static int finger_from;         /* a finger is on the screen from this read of a touch on (1: at the take; 0: never) */
static int takes, admits, keeps, ends, withdraws;
static bool reads_paused;
static uint8_t sent[64];        /* the codes apps_touch_tag gave back, in order */
static int n_sent;
static int heard_slots[64];

/* What apps_task.c's on_widget_event does with an event the layer sends the app: the tag, whose code is kept for the case. */
static void app_hears(int slot, uint8_t event) {
    uint8_t code = apps_touch_tag((uint16_t)slot, event);
    CHECK((code & ~APPS_UI_INJECTED) == event, "the tag changed the event: %#x for %#x", code, event);
    if (n_sent < (int)ARRAY_LEN(sent)) {
        heard_slots[n_sent] = slot;
        sent[n_sent++] = code;
    }
}

/* A widget's own event (apps_lv.c's on_touch): only a widget with a handler has the callbacks. */
static void lvgl_event(int slot, uint8_t bit) {
    if (slot < 0 || !has_handler(slot)) {
        return;
    }
    app_hears(slot, bit);
}

/* apps_task.c's on_close: the layer's right swipe from the left edge puts the app behind. */
static void on_close(void) {
    closes++;
    apps_touch_closed();
    layer_front = false;
}

/* apps_lv.c's in_left_edge: x < APPS_LV_CLOSE_EDGE_PX, inside the circle. */
static bool in_left_edge(int x, int y) {
    int dx = x - 233, dy = y - 233;
    return x < APPS_LV_CLOSE_EDGE_PX && dx * dx + dy * dy <= 233 * 233;
}

/* apps_lv.c's on_gesture: the touch is taken, whichever way the swipe goes; then nothing more while the layer is behind or a card
 * covers it; a right swipe whose press went down at the left edge closes, and any other is the app's, for the widget pressed (a
 * widget with a handler or the scroll box: the bare layer is nobody's). */
static void layer_swipe(int dir) {
    waiting = true;
    if (!layer_front || cards) {
        return;
    }
    if (dir == APPS_UI_RIGHT && in_left_edge(px1, py1)) {
        on_close();
    } else if (pressed_slot >= 0) {
        app_hears(pressed_slot, (uint8_t)APPS_UI_SWIPE_CODE(dir));
    }
}

/* indev_gesture: while nothing scrolls, the reads' movement adds up, and past FAKE_GESTURE_LIMIT on either axis it is a swipe, sent
 * once, its way the larger axis's. */
static void gesture(int vx, int vy) {
    if (scrolled || gesture_sent) {
        return;
    }
    if (abs(vx) < FAKE_GESTURE_MIN_VELOCITY && abs(vy) < FAKE_GESTURE_MIN_VELOCITY) {
        sum_x = sum_y = 0;
    }
    sum_x += vx;
    sum_y += vy;
    if (abs(sum_x) > FAKE_GESTURE_LIMIT || abs(sum_y) > FAKE_GESTURE_LIMIT) {
        gesture_sent = true;
        layer_swipe(abs(sum_x) > abs(sum_y) ? (sum_x > 0 ? APPS_UI_RIGHT : APPS_UI_LEFT) : (sum_y > 0 ? APPS_UI_DOWN : APPS_UI_UP));
    }
}

bool muse_ui_guarded_drag(int x1, int y1, int x2, int y2, int ms, const muse_ui_guard_t *g) {
    CHECK(g != NULL && g->admit != NULL && g->keep != NULL && g->end != NULL, "a guard without its checks");
    if (no_input || pending) {
        return false;
    }
    CHECK(ms >= MUSE_UI_DRAG_MS_MIN && ms <= MUSE_UI_DRAG_MS_MAX, "ms %d", ms);
    guard = g;
    px1 = x1, py1 = y1, px2 = x2, py2 = y2, pms = ms;
    pending = true;
    return true;
}

bool muse_ui_guarded_withdraw(void) {
    withdraws++;
    bool was = pending;
    pending = false;
    return was;
}

/* One input read, as the player makes it and LVGL handles it. */
static void fake_read(void) {
    if (reads_paused) {
        return;
    }
    if (ending) {
        ending = false;
        ends++;
        guard->end(end_how, end_ms);
    }
    if (!playing && pending) {
        pending = false;
        takes++;
        reads_now = 1;
        if (finger_from == 1) {
            guard->end(MUSE_UI_GUARD_FINGER, 0);
            return;
        }
        admits++;
        if (!guard->admit(px1, py1)) {
            guard->end(MUSE_UI_GUARD_REFUSED, 0);
            return;
        }
        playing = true;
        pstart = now_ms;
        last_x = px1;
        last_y = py1;
        scrolled = long_sent = gesture_sent = waiting = false;
        sum_x = sum_y = 0;
        pressed_slot = apps_lv_hit(px1, py1);
        lvgl_event(pressed_slot, APPS_UI_PRESS);
        return;
    }
    if (!playing) {
        return;
    }
    reads_now++;
    int t = (int)(now_ms - pstart);
    int x = t >= pms ? px2 : px1 + (px2 - px1) * t / pms;
    int y = t >= pms ? py2 : py1 + (py2 - py1) * t / pms;
    bool finger = finger_from != 0 && reads_now >= finger_from;
    keeps += !finger;
    if (finger || !guard->keep(x, y)) {
        playing = false;
        list_dragging = false;
        ending = true;
        end_how = finger ? MUSE_UI_GUARD_CUT_FINGER : MUSE_UI_GUARD_CUT;
        end_ms = (uint32_t)t;
        lvgl_event(pressed_slot, APPS_UI_RELEASE);  /* PRESS_LOST, which the app hears as a release, and no tap */
        return;
    }
    /* Until the layer takes the touch (after lv_indev_wait_release LVGL's indev_proc_press returns at once): a drag up or down of more
     * than LVGL's 10 px that began in the box scrolls it; a drag that doesn't is a gesture past 50 px (not at the release's read);
     * and a press past 400 ms is a long press, on a gesture's read too, as LVGL checks it after the gesture. */
    if (!waiting) {
        bool in_list = pressed_slot == LIST || (pressed_slot >= ROW0 && pressed_slot < ROW0 + 20);
        if (in_list && abs(y - py1) > 10) {
            int32_t want = list_y + (last_y - y);
            list_y = want < 0 ? 0 : want > list_y_max ? list_y_max : want;
            scrolled = list_dragging = true;
        }
        if (t < pms) {
            gesture(x - last_x, y - last_y);
        }
        if (!scrolled && !long_sent && t >= 400 && t < pms) {
            long_sent = true;
            lvgl_event(pressed_slot, APPS_UI_LONG_PRESS);
        }
    }
    last_x = x, last_y = y;
    if (t >= pms) {
        playing = false;
        ending = true;
        end_how = MUSE_UI_GUARD_PLAYED;
        end_ms = (uint32_t)t;
        lvgl_event(pressed_slot, APPS_UI_RELEASE);  /* PRESS_LOST once the layer has taken the touch */
        if (!waiting && !scrolled && !long_sent) {
            lvgl_event(pressed_slot, APPS_UI_TAP);
        }
        if (list_dragging) {
            list_dragging = false;
            list_moving_until = (int)now_ms + 200;  /* its throw */
            list_y = list_y + 30 > list_y_max ? list_y_max : list_y + 30;
        }
    }
}

/* ---- The I/O worker ---- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} job_t;

static job_t jobs[8];
static int n_jobs;
static bool io_refuses;
static int assists;
static int64_t run_late_ms;  /* the job waits this long in the queue before it runs */

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    if (io_refuses || n_jobs == (int)ARRAY_LEN(jobs)) {
        return false;
    }
    jobs[n_jobs++] = (job_t){fn, arg};
    return true;
}

void apps_io_assist(void) {
    assists++;
}

static void run_jobs(void) {
    now_ms += run_late_ms;
    while (n_jobs > 0) {
        job_t job = jobs[0];
        memmove(jobs, jobs + 1, sizeof jobs[0] * (size_t)--n_jobs);
        job.fn(job.arg);
    }
}

/* ---- The LVGL task: a frame and a read after every delay of the job; a case's hook runs first ---- */

static int frames;
static void (*each_step)(void);  /* a case's: the app task or the voice changes things, as time goes */

static void lvgl_step(void) {
    if (each_step != NULL) {
        each_step();
    }
    frames++;
    apps_touch_supervise();
    fake_read();
}

void vTaskDelay(TickType_t ticks) {
    CHECK(ticks > 0, "a delay of no ticks spins");
    now_ms += (int64_t)ticks;
    lvgl_step();
}

/* ---- The command ---- */

static int dones;
static char result[16384];

static void done_cb(void *ctx, cJSON *r) {
    CHECK(ctx == (void *)&dones, "done's context is not the caller's");
    dones++;
    counting = false;
    char *t = r != NULL ? cJSON_PrintUnformatted(r) : NULL;
    counting = true;
    snprintf(result, sizeof result, "%s", t != NULL ? t : "(null)");
    cJSON_free(t);
    cJSON_Delete(r);
}

static void reset(void) {
    refuse_at = 0;
    allocations = 0;
    refused = false;
    logged_len = 0;
    logged[0] = '\0';
    now_ms = 100000;
    dark_until = 0;
    dark_forever = false;
    wake_lights_ms = 0;
    wakes = 0;
    wake_why = NULL;
    in_turn = false;
    running_id = "list";
    app_gen = 7;
    app_alive = true;
    target = APPS_TOUCH_TARGET_OK;
    layer_front = true;
    cards = false;
    targets = alives = 0;
    covered_from = 0;
    reads_now = 0;
    list_y = 0;
    list_y_max = 600;
    list_moving_until = 0;
    list_dragging = false;
    guard = NULL;
    pending = playing = ending = no_input = false;
    finger_from = 0;
    sum_x = sum_y = 0;
    gesture_sent = waiting = false;
    closes = 0;
    takes = admits = keeps = ends = withdraws = 0;
    reads_paused = false;
    n_sent = 0;
    io_refuses = false;
    assists = 0;
    run_late_ms = 0;
    frames = 0;
    each_step = NULL;
    dones = 0;
    result[0] = '\0';
}

/* Every block given back, nothing queued, no touch under way. */
static void expect_clean_at(int line) {
    if (blocks != 0 || n_jobs != 0 || pending || playing || ending) {
        fail_at(line, "held at the end: %d blocks, %d jobs, pending %d, playing %d, ending %d", blocks, n_jobs, pending, playing,
                ending);
    }
}
#define EXPECT_CLEAN() expect_clean_at(__LINE__)

static muse_apps_at_t tap_at(int x, int y, int ms) {
    return (muse_apps_at_t){.x = x, .y = y, .x2 = -1, .y2 = -1, .ms = ms};
}

static muse_apps_at_t drag_at(int x, int y, int x2, int y2, int ms) {
    return (muse_apps_at_t){.x = x, .y = y, .x2 = x2, .y2 = y2, .ms = ms};
}

/* Asks for the touch and runs the job: true when it was queued (done ran once then); false with code and err. */
static bool touch(const char *id, bool drag, muse_apps_at_t at, const char **code, const char **err) {
    int before = dones;
    *code = "(unset)";
    *err = "(unset)";
    if (!muse_apps_touch(id, drag, &at, done_cb, &dones, code, err)) {
        CHECK(n_jobs == 0 && dones == before, "a refusal queued a job or called done");
        return false;
    }
    CHECK(n_jobs == 1 && dones == before, "queued: one job, and done not yet");
    run_jobs();
    CHECK(dones == before + 1, "done ran %d times", dones - before);
    return true;
}

static void expect_failure_at(int line, const char *id, bool drag, muse_apps_at_t at, const char *code, const char *message) {
    const char *c, *e;
    if (!touch(id, drag, at, &c, &e)) {
        fail_at(line, "refused at the call: %s %s", c, e);
    }
    char want[1024];
    snprintf(want, sizeof want, "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
    if (strcmp(result, want) != 0) {
        fail_at(line, "the result is\n  %s\nnot\n  %s", result, want);
    }
}
#define EXPECT_FAILURE(id, drag, at, code, message) expect_failure_at(__LINE__, (id), (drag), (at), (code), (message))

static void expect_refusal_at(int line, const char *id, bool drag, const muse_apps_at_t *at, muse_apps_done_cb done,
                              const char *code, const char *err) {
    const char *c = "(unset)", *e = "(unset)";
    int before = dones, jobs_before = n_jobs;
    bool queued = muse_apps_touch(id, drag, at, done, &dones, &c, &e);
    if (queued || n_jobs != jobs_before || dones != before) {
        fail_at(line, "not refused (queued %d, %d jobs)", queued, n_jobs);
    }
    if (strcmp(c, code) != 0 || strcmp(e, err) != 0) {
        fail_at(line, "refused with %s '%s', not %s '%s'", c, e, code, err);
    }
}
#define EXPECT_REFUSAL(id, drag, at, done, code, err) expect_refusal_at(__LINE__, (id), (drag), (at), (done), (code), (err))

/* The successful result's payload as compact JSON (with "warnings" and all), for a case to compare. */
static void expect_payload_at(int line, const char *want) {
    char full[16384];
    snprintf(full, sizeof full, "{\"ok\":true,\"payload\":%s}", want);
    if (strcmp(result, full) != 0) {
        fail_at(line, "the result is\n  %s\nnot\n  %s", result, full);
    }
}
#define EXPECT_PAYLOAD(want) expect_payload_at(__LINE__, (want))

/* Every code the touch sent the app carried the tag, and n of them came. */
static void expect_sent_tagged_at(int line, int n) {
    if (n_sent != n) {
        fail_at(line, "%d events sent, not %d", n_sent, n);
    }
    for (int i = 0; i < n_sent; i++) {
        if ((sent[i] & APPS_UI_INJECTED) == 0) {
            fail_at(line, "event %d (%#x) wasn't tagged as Muse's", i, sent[i]);
        }
    }
}
#define EXPECT_SENT_TAGGED(n) expect_sent_tagged_at(__LINE__, (n))

/* ---- The cases ---- */

#define NOT_RUNNING_LIST "'list' isn't running; launch it first (app.control launch)"
#define NOT_FRONT_LIST "'list' isn't on screen (the launcher, a card, another app or a voice turn covers it); nothing was touched"
#define COVERED_LIST \
    "something is over 'list' where the touch would land (a picture Muse showed, or a pairing code); nothing was touched"
#define BAD_ID "app id must match ^[a-z][a-z0-9_]{1,23}$"

/* At the call: the ruling's codes, nothing queued, done never called. */
static void refusals(void) {
    muse_apps_at_t ok = tap_at(233, 150, 80);
    EXPECT_REFUSAL("list", false, &ok, NULL, "invalid_params", "no callback for the result");
    EXPECT_REFUSAL(NULL, false, &ok, done_cb, "invalid_params", BAD_ID);
    EXPECT_REFUSAL("List", false, &ok, done_cb, "invalid_params", BAD_ID);
    EXPECT_REFUSAL("list", false, NULL, done_cb, "invalid_params", MUSE_APPS_TOUCH_BAD_TAP);
    EXPECT_REFUSAL("list", true, NULL, done_cb, "invalid_params", MUSE_APPS_TOUCH_BAD_DRAG);
    static const muse_apps_at_t bad_taps[] = {
        {-1, 150, 0, 0, 80}, {466, 150, 0, 0, 80}, {233, -1, 0, 0, 80}, {233, 466, 0, 0, 80}, {233, 150, 0, 0, 49},
        {233, 150, 0, 0, 2001}, {233, 150, 0, 0, 0},
    };
    for (size_t i = 0; i < ARRAY_LEN(bad_taps); i++) {
        EXPECT_REFUSAL("list", false, &bad_taps[i], done_cb, "invalid_params", MUSE_APPS_TOUCH_BAD_TAP);
    }
    static const muse_apps_at_t bad_drags[] = {
        {233, 300, -1, 100, 300}, {233, 300, 466, 100, 300}, {233, 300, 233, -1, 300}, {233, 300, 233, 466, 300},
        {-1, 300, 233, 100, 300}, {233, 300, 233, 100, 49}, {233, 300, 233, 100, 2001},
    };
    for (size_t i = 0; i < ARRAY_LEN(bad_drags); i++) {
        EXPECT_REFUSAL("list", true, &bad_drags[i], done_cb, "invalid_params", MUSE_APPS_TOUCH_BAD_DRAG);
    }
    /* The edges are in: 0 and 465, 50 and 2000 ms; a tap's x2 and y2 aren't looked at, and it is played as a drag to its own
     * point. One touch at a time: from the call to its done, a second is busy, a drag too. */
    static const muse_apps_at_t edges[] = {{0, 0, 999, -999, 50}, {465, 465, 0, 0, 2000}};
    for (size_t i = 0; i < ARRAY_LEN(edges); i++) {
        const char *c, *e;
        CHECK(muse_apps_touch("list", false, &edges[i], done_cb, &dones, &c, &e), "edge %zu refused: %s %s", i, c, e);
        EXPECT_REFUSAL("list", false, &ok, done_cb, "busy", MUSE_APPS_TOUCH_BUSY);
        muse_apps_at_t d = drag_at(0, 465, 465, 0, 2000);
        EXPECT_REFUSAL("list", true, &d, done_cb, "busy", MUSE_APPS_TOUCH_BUSY);
        run_jobs();
        CHECK(strstr(result, "\"ok\":true") != NULL, "edge %zu: %s", i, result);
        CHECK(px1 == edges[i].x && py1 == edges[i].y && px2 == px1 && py2 == py1 && pms == edges[i].ms,
              "edge %zu played as %d,%d to %d,%d over %d ms", i, px1, py1, px2, py2, pms);
    }
    muse_apps_at_t d = drag_at(0, 465, 465, 0, 2000);
    const char *c, *e;
    CHECK(touch("list", true, d, &c, &e) && strstr(result, "\"ok\":true") != NULL, "a drag on the edges: %s", result);
    CHECK(px1 == 0 && py1 == 465 && px2 == 465 && py2 == 0 && pms == 2000, "played as %d,%d to %d,%d over %d ms", px1, py1, px2,
          py2, pms);
    /* The worker's refusal, and no memory for the job: out_of_memory, and the claim is let go. */
    io_refuses = true;
    EXPECT_REFUSAL("list", false, &ok, done_cb, "out_of_memory", APPS_IO_REFUSED);
    io_refuses = false;
    refuse_at = allocations + 1;
    EXPECT_REFUSAL("list", false, &ok, done_cb, "out_of_memory", "out of memory: no room for the command");
    refuse_at = 0;
    CHECK(touch("list", false, ok, &c, &e), "after the refusals a touch is taken: %s %s", c, e);
    CHECK(strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* A tap on a button: woken first, admitted at the take, every read kept, and the reply names what was pressed and heard. The events
 * reach the app tagged as Muse's. */
static void tap_on_a_button(void) {
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 release\",\"button#4 tap\"],\"front\":true,\"cut\":false,\"played_ms\":80,\"scroll\":[],"
                   "\"warnings\":[]}");
    EXPECT_SENT_TAGGED(3);
    CHECK(wakes == 1 && strcmp(wake_why, "touch") == 0, "woken %d times, for %s", wakes, wake_why != NULL ? wake_why : "-");
    CHECK(takes == 1 && admits == 1 && keeps == 8 && ends == 1, "takes %d admits %d keeps %d ends %d", takes, admits, keeps, ends);
    CHECK(targets == 1 && alives >= keeps, "the names lock once (%d), the atomics every read (%d)", targets, alives);
    CHECK(assists > 0, "the job ran the app task's ops while it waited");
    CHECK(strstr(logged, "I muse_apps: touch list: tap at 233,150 for 80 ms\n") != NULL, "%s", logged);
    /* After the touch, the app's own events aren't tagged. */
    CHECK(apps_touch_tag(BUTTON, APPS_UI_TAP) == APPS_UI_TAP, "a person's tap after it was tagged");
    EXPECT_CLEAN();
}

/* A tap of 500 ms or more is a long press: press, long_press, release, no tap. One on the layer presses no widget. */
static void long_press_and_the_layer(void) {
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, MUSE_APPS_TOUCH_LONG_MS), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 long_press\",\"button#4 release\"],\"front\":true,\"cut\":false,\"played_ms\":500,\"scroll\":[],"
                   "\"warnings\":[]}");
    CHECK(touch("list", false, tap_at(233, 30, 80), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":null,\"heard\":[],\"front\":true,\"cut\":false,"
                   "\"played_ms\":80,\"scroll\":[],\"warnings\":[]}");
    EXPECT_CLEAN();
}

/* A drag on the list scrolls it: the row it began on hears press and release, no tap; the reply has the box's y once its throw has
 * come to rest: 194 px of the drag (its first read, 6 px in, is under LVGL's 10 px scroll limit) and the throw's 30. */
static void drag_scrolls_the_list(void) {
    const char *c, *e;
    CHECK(touch("list", true, drag_at(233, 350, 233, 150, 300), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"drag\",\"pressed\":\"label#12\",\"heard\":[\"label#12 press\","
                   "\"label#12 release\"],\"front\":true,\"cut\":false,\"played_ms\":300,\"scroll\":[{\"id\":\"box#7\",\"y\":224}],"
                   "\"warnings\":[]}");
    CHECK(now_ms >= list_moving_until, "answered before the list came to rest");
    EXPECT_SENT_TAGGED(2);
    CHECK(px1 == 233 && py1 == 350 && px2 == 233 && py2 == 150 && pms == 300, "played as %d,%d to %d,%d over %d ms", px1, py1,
          px2, py2, pms);
    /* A tap on a row after it: the list didn't move, so no scroll is reported. */
    CHECK(touch("list", false, tap_at(233, 210, 80), &c, &e), "%s %s", c, e);
    CHECK(strstr(result, "\"scroll\":[]") != NULL && strstr(result, "\"pressed\":\"label#14\"") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* The list keeps moving: the reply comes at the settle's bound, with a warning. */
static void keep_moving(void) {
    if (!playing) {
        list_moving_until = (int)now_ms + 100000;
    }
}

static void a_list_still_moving_is_reported(void) {
    const char *c, *e;
    each_step = keep_moving;
    int64_t began = now_ms;
    CHECK(touch("list", true, drag_at(233, 350, 233, 150, 300), &c, &e), "%s %s", c, e);
    CHECK(strstr(result, "\"warnings\":[\"box#7 was still scrolling 1000 ms after the lift: its y is where it was then\"]") != NULL,
          "%s", result);
    CHECK(now_ms - began >= 300 + MUSE_APPS_TOUCH_SETTLE_MS && now_ms - began < 300 + MUSE_APPS_TOUCH_SETTLE_MS + 100,
          "answered %d ms after the call", (int)(now_ms - began));
    each_step = NULL;
    list_moving_until = 0;
    EXPECT_CLEAN();
}

/* id isn't the app that runs: not_running, before any wake. */
static void not_running(void) {
    running_id = "other";
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "not_running", NOT_RUNNING_LIST);
    running_id = NULL;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "not_running", NOT_RUNNING_LIST);
    CHECK(wakes == 0 && takes == 0, "woken %d, taken %d", wakes, takes);
    EXPECT_CLEAN();
}

/* At the take: the layer behind, a card over it, a voice turn, the app gone or hung, the names lock busy, the first point not on
 * the app: each refused, nothing pressed, nothing sent. */
static void refused_at_the_take(void) {
    struct {
        bool *flag;
        apps_touch_target_t target;
        int covered;
        const char *code, *message;
    } ways[] = {
        {&cards, APPS_TOUCH_TARGET_OK, 0, "unavailable", NOT_FRONT_LIST},
        {&in_turn, APPS_TOUCH_TARGET_OK, 0, "unavailable", NOT_FRONT_LIST},
        {NULL, APPS_TOUCH_TARGET_GONE, 0, "unavailable", NOT_FRONT_LIST},
        {NULL, APPS_TOUCH_TARGET_HUNG, 0, "unavailable", APPS_TASK_MSG_HUNG},
        {NULL, APPS_TOUCH_TARGET_BUSY, 0, "busy", APPS_TASK_MSG_BUSY},
        {NULL, APPS_TOUCH_TARGET_OK, 1, "unavailable", COVERED_LIST},
    };
    for (size_t i = 0; i < ARRAY_LEN(ways); i++) {
        if (ways[i].flag != NULL) {
            *ways[i].flag = true;
        }
        target = ways[i].target;
        covered_from = ways[i].covered;
        EXPECT_FAILURE("list", false, tap_at(233, 150, 80), ways[i].code, ways[i].message);
        CHECK(n_sent == 0 && admits == (int)i + 1 && keeps == 0, "way %zu: %d sent, %d admits, %d keeps", i, n_sent, admits, keeps);
        if (ways[i].flag != NULL) {
            *ways[i].flag = false;
        }
        target = APPS_TOUCH_TARGET_OK;
        covered_from = 0;
    }
    layer_front = false;
    EXPECT_FAILURE("list", true, drag_at(233, 350, 233, 150, 300), "unavailable", NOT_FRONT_LIST);
    layer_front = true;
    /* A finger on the screen at the take: the player says so before admit is asked. */
    finger_from = 1;
    int asked = admits;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "unavailable", MUSE_APPS_TOUCH_FINGER);
    CHECK(admits == asked, "admit asked with a finger down");
    finger_from = 0;
    /* A board whose touch can't be played. */
    no_input = true;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "unavailable", MUSE_APPS_TOUCH_NO_INPUT);
    no_input = false;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e) && strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* A dark screen is woken, and the touch taken on its first lit frame. */
static void dark_then_lit(void) {
    dark_until = now_ms + 1000000;
    wake_lights_ms = 300;
    int64_t began = now_ms;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e), "%s %s", c, e);
    CHECK(strstr(result, "\"pressed\":\"button#4\"") != NULL, "%s", result);
    CHECK(wakes == 1 && now_ms - began >= 300 + 80, "woken %d, answered after %d ms", wakes, (int)(now_ms - began));
    CHECK(strstr(logged, "I muse_apps: touch list: waking the screen\n") != NULL, "%s", logged);
    EXPECT_CLEAN();
}

/* A screen that stays dark: MUSE_APPS_TOUCH_NO_WAKE at the screen's deadline, nothing handed to the player; the next touch is
 * taken. */
static void dark_past_the_deadline(void) {
    dark_forever = true;
    int64_t began = now_ms;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "unavailable", MUSE_APPS_TOUCH_NO_WAKE);
    CHECK(now_ms - began >= MUSE_APPS_TOUCH_SCREEN_MS && now_ms - began < MUSE_APPS_TOUCH_SCREEN_MS + 50, "after %d ms",
          (int)(now_ms - began));
    CHECK(takes == 0 && withdraws == 0, "taken %d, withdrawn %d", takes, withdraws);
    apps_touch_supervise();  /* a late frame finds nothing */
    CHECK(!pending, "a frame after the deadline handed it over");
    dark_forever = false;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e) && strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* Handed to the player, which never reads (the input is blocked): withdrawn at the screen's deadline, MUSE_APPS_TOUCH_NO_WAKE. */
static void withdrawn_at_the_deadline(void) {
    reads_paused = true;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "unavailable", MUSE_APPS_TOUCH_NO_WAKE);
    CHECK(withdraws == 1 && !pending && takes == 0, "withdrawn %d, pending %d, taken %d", withdraws, pending, takes);
    reads_paused = false;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e) && strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* Mid-touch, the app leaves the front, the app changes, something covers it, a finger comes down: cut, with a release and no tap,
 * and the reply says why. */
static int cut_at_read;
static void (*cut_by)(void);

static void at_the_read(void) {
    if (playing && reads_now + 1 == cut_at_read) {
        cut_by();
    }
}

static void go_behind(void) {
    layer_front = false;
}
static void new_app(void) {
    app_gen++;
}
static void a_card(void) {
    cards = true;
}
static void a_turn(void) {
    in_turn = true;
}
static void swiped(void) {
    on_close();  /* the layer's right swipe from the left edge, inside LVGL's gesture */
}

static void cut_mid_touch(void) {
    struct {
        void (*by)(void);
        bool front_after;
        const char *warning;
    } ways[] = {
        {go_behind, false, "cut short: 'list' left the front (it ended, or another app, the launcher, a card or a voice turn took "
                           "the screen); the press ended with a release and no tap, and nothing else was touched"},
        {new_app, false, "cut short: 'list' left the front (it ended, or another app, the launcher, a card or a voice turn took the "
                         "screen); the press ended with a release and no tap, and nothing else was touched"},
        {a_card, false, "cut short: 'list' left the front (it ended, or another app, the launcher, a card or a voice turn took the "
                        "screen); the press ended with a release and no tap, and nothing else was touched"},
        {a_turn, false, "cut short: 'list' left the front (it ended, or another app, the launcher, a card or a voice turn took the "
                        "screen); the press ended with a release and no tap, and nothing else was touched"},
        {swiped, false, "the drag was a right swipe from the left edge: 'list' went behind, as a finger's would"},
    };
    each_step = at_the_read;
    for (size_t i = 0; i < ARRAY_LEN(ways); i++) {
        cut_at_read = 4;
        cut_by = ways[i].by;
        n_sent = 0;
        const char *c, *e;
        CHECK(touch("list", false, tap_at(233, 150, 300), &c, &e), "%s %s", c, e);
        char want[1024];
        snprintf(want, sizeof want, "{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                                    "\"button#4 release\"],\"front\":false,\"cut\":true,\"played_ms\":30,\"scroll\":[],"
                                    "\"warnings\":[\"%s\"]}", ways[i].warning);
        EXPECT_PAYLOAD(want);
        EXPECT_SENT_TAGGED(2);
        layer_front = true;
        cards = in_turn = false;
    }
    /* Something comes over it where the touch is. */
    cut_at_read = 0;
    covered_from = 3;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 300), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 release\"],\"front\":true,\"cut\":true,\"played_ms\":20,\"scroll\":[],\"warnings\":[\"cut short: "
                   "something came over 'list' where the touch was (a picture Muse showed, or a pairing code); the press ended with "
                   "a release and no tap, and nothing else was touched\"]}");
    covered_from = 0;
    /* A finger comes down: it wins. */
    finger_from = 3;
    CHECK(touch("list", false, tap_at(233, 150, 300), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 release\"],\"front\":true,\"cut\":true,\"played_ms\":20,\"scroll\":[],\"warnings\":[\"cut short: "
                   "someone touched the screen; the press ended with a release and no tap\"]}");
    finger_from = 0;
    each_step = NULL;
    /* The swiped way, played: a right swipe from the left edge (the board's drag of 10,233 to 300,233 over 250 ms) is LVGL's gesture
     * at the read 58 px in, 50 ms on; the layer closes, and the next read cuts the touch: the same answer. It pressed the list's
     * first row, at y 233. */
    n_sent = 0;
    const int closed = closes;  /* the swiped way's one */
    CHECK(touch("list", true, drag_at(10, 233, 300, 233, 250), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"drag\",\"pressed\":\"label#9\",\"heard\":[\"label#9 press\","
                   "\"label#9 release\"],\"front\":false,\"cut\":true,\"played_ms\":60,\"scroll\":[],\"warnings\":[\"the drag was "
                   "a right swipe from the left edge: 'list' went behind, as a finger's would\"]}");
    EXPECT_SENT_TAGGED(2);
    CHECK(closes == closed + 1, "closed %d times", closes - closed);
    layer_front = true;
    /* Its twin, from x = APPS_LV_CLOSE_EDGE_PX: no edge, so the swipe is the app's, and the touch plays to its end. */
    n_sent = 0;
    CHECK(touch("list", true, drag_at(APPS_LV_CLOSE_EDGE_PX, 233, 330, 233, 250), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"drag\",\"pressed\":\"label#9\",\"heard\":[\"label#9 press\","
                   "\"label#9 swipe right\",\"label#9 release\"],\"front\":true,\"cut\":false,\"played_ms\":250,\"scroll\":[],"
                   "\"warnings\":[]}");
    EXPECT_SENT_TAGGED(3);
    CHECK(closes == closed + 1, "the twin closed: %d", closes - closed - 1);
    EXPECT_CLEAN();
}

/* A drag that is a swipe (more than 50 px at 3 px or more a read) is heard as press, swipe and its way, release, and no tap, each of
 * the four ways, on the widget it pressed; a swipe from the middle leaves the app in front. The code the app gets has the direction
 * and the tag together. A drag slower than 3 px a read is no swipe, and taps at the lift. */
static void drag_that_swipes_is_heard_with_its_direction(void) {
    static const struct {
        muse_apps_at_t at;
        int dir;
        const char *name;
    } swipes[] = {
        {{330, 140, 130, 140, 200}, APPS_UI_LEFT, "left"},
        {{130, 140, 330, 140, 200}, APPS_UI_RIGHT, "right"},
        {{233, 170, 233, 20, 150}, APPS_UI_UP, "up"},
        {{233, 110, 233, 260, 150}, APPS_UI_DOWN, "down"},  /* over the list, which doesn't scroll: the button keeps the press */
    };
    const char *c, *e;
    for (size_t i = 0; i < ARRAY_LEN(swipes); i++) {
        n_sent = 0;
        CHECK(touch("list", true, swipes[i].at, &c, &e), "%s %s", c, e);
        char want[1024];
        snprintf(want, sizeof want, "{\"id\":\"list\",\"action\":\"drag\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                                    "\"button#4 swipe %s\",\"button#4 release\"],\"front\":true,\"cut\":false,\"played_ms\":%d,"
                                    "\"scroll\":[],\"warnings\":[]}", swipes[i].name, swipes[i].at.ms);
        EXPECT_PAYLOAD(want);
        EXPECT_SENT_TAGGED(3);
        CHECK(sent[1] == (uint8_t)(APPS_UI_SWIPE_CODE(swipes[i].dir) | APPS_UI_INJECTED), "the swipe %s reached the app as %#x",
              swipes[i].name, sent[1]);
    }
    CHECK(closes == 0, "a swipe from the middle closed the app %d times", closes);
    /* 2 px a read: no swipe, and a tap. */
    n_sent = 0;
    CHECK(touch("list", true, drag_at(300, 140, 240, 140, 300), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"drag\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 release\",\"button#4 tap\"],\"front\":true,\"cut\":false,\"played_ms\":300,\"scroll\":[],"
                   "\"warnings\":[]}");
    EXPECT_SENT_TAGGED(3);
    EXPECT_CLEAN();
}

/* The tag: only while a touch of Muse's plays (from its admit to its end), never before or after. */
static void the_tag_is_only_the_touchs(void) {
    CHECK(apps_touch_tag(BUTTON, APPS_UI_PRESS) == APPS_UI_PRESS, "tagged with no touch under way");
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e), "%s %s", c, e);
    EXPECT_SENT_TAGGED(3);
    CHECK(apps_touch_tag(BUTTON, APPS_UI_RELEASE) == APPS_UI_RELEASE, "tagged after the touch");
    /* A refused touch tags nothing: a finger that was down keeps its own events. */
    finger_from = 1;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "unavailable", MUSE_APPS_TOUCH_FINGER);
    CHECK(apps_touch_tag(BUTTON, APPS_UI_PRESS) == APPS_UI_PRESS, "the finger's press was tagged");
    finger_from = 0;
    EXPECT_CLEAN();
}

/* More events than the result lists: the first MUSE_APPS_TOUCH_HEARD_MAX, and a warning that counts the rest. The fake LVGL sends
 * nine more presses once the touch plays: twelve in all, with the press, the release and the tap. */
static bool more_sent;

static void nine_more(void) {
    if (playing && !more_sent) {
        more_sent = true;
        for (int i = 0; i < 9; i++) {
            lvgl_event(BUTTON, APPS_UI_PRESS);
        }
    }
}

static void heard_more_than_listed(void) {
    more_sent = false;
    each_step = nine_more;
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 200), &c, &e), "%s %s", c, e);
    EXPECT_PAYLOAD("{\"id\":\"list\",\"action\":\"tap\",\"pressed\":\"button#4\",\"heard\":[\"button#4 press\","
                   "\"button#4 press\",\"button#4 press\",\"button#4 press\",\"button#4 press\",\"button#4 press\","
                   "\"button#4 press\",\"button#4 press\"],\"front\":true,\"cut\":false,\"played_ms\":200,\"scroll\":[],"
                   "\"warnings\":[\"4 more events came than heard lists (it keeps the first 8)\"]}");
    EXPECT_SENT_TAGGED(12);
    each_step = NULL;
    EXPECT_CLEAN();
}

/* The job waited in the worker's queue past what a touch needs: busy, with nothing woken or touched. */
static void waited_too_long(void) {
    run_late_ms = MUSE_APPS_TOUCH_TOTAL_MS - APPS_TOUCH_MIN_LEFT_MS + 1;
    char want[128];
    snprintf(want, sizeof want, "the gadget was busy for %d ms; the touch wasn't played, so try again", (int)run_late_ms);
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "busy", want);
    CHECK(wakes == 0 && takes == 0, "woken %d, taken %d", wakes, takes);
    run_late_ms = 0;
    EXPECT_CLEAN();
}

/* The LVGL task stops reading mid-touch (its input blocked): at the whole touch's deadline the job gives up, MUSE_APPS_TOUCH_SLOW;
 * a touch asked meanwhile is busy; when the reads come back keep says no, which cuts it, and its end lets the next touch in. */
static void pause_reads(void) {
    if (playing && reads_now >= 3) {
        reads_paused = true;
    }
}

static void abandoned(void) {
    each_step = pause_reads;
    int64_t began = now_ms;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 300), "unavailable", MUSE_APPS_TOUCH_SLOW);
    CHECK(now_ms - began >= MUSE_APPS_TOUCH_TOTAL_MS && now_ms - began < MUSE_APPS_TOUCH_TOTAL_MS + 50, "after %d ms",
          (int)(now_ms - began));
    each_step = NULL;
    EXPECT_FAILURE("list", false, tap_at(233, 150, 80), "busy", MUSE_APPS_TOUCH_BUSY);
    reads_paused = false;
    int kept = keeps;
    fake_read();  /* keep says no: cut */
    CHECK(keeps == kept + 1 && ending && !playing, "not cut: keeps %d ending %d", keeps, ending);
    fake_read();  /* its end */
    CHECK(!ending, "no end");
    const char *c, *e;
    CHECK(touch("list", false, tap_at(233, 150, 80), &c, &e) && strstr(result, "\"ok\":true") != NULL, "%s", result);
    EXPECT_CLEAN();
}

/* Done exactly once and nothing leaked, whichever allocation fails, at the call or in the job. */
static void every_allocation_refused(void) {
    for (long n = 1; n < 400; n++) {
        reset();
        refuse_at = n;
        const char *c = NULL, *e = NULL;
        int before = dones;
        bool queued = muse_apps_touch("list", true, &(muse_apps_at_t){233, 350, 233, 150, 300}, done_cb, &dones, &c, &e);
        if (queued) {
            run_jobs();
            CHECK(dones == before + 1, "allocation %ld: done ran %d times", n, dones - before);
        } else {
            CHECK(strcmp(c, "out_of_memory") == 0, "allocation %ld: refused with %s", n, c);
        }
        CHECK(blocks == 0 && n_jobs == 0, "allocation %ld: %d blocks held", n, blocks);
        if (!refused) {
            CHECK(strstr(result, "\"ok\":true") != NULL, "nothing refused, and %s", result);
            return;
        }
    }
    fail_at(__LINE__, "never got through");
}

static const struct {
    const char *name;
    void (*fn)(void);
} CASES[] = {
    {"refusals", refusals},
    {"tap_on_a_button", tap_on_a_button},
    {"long_press_and_the_layer", long_press_and_the_layer},
    {"drag_scrolls_the_list", drag_scrolls_the_list},
    {"a_list_still_moving_is_reported", a_list_still_moving_is_reported},
    {"not_running", not_running},
    {"refused_at_the_take", refused_at_the_take},
    {"dark_then_lit", dark_then_lit},
    {"dark_past_the_deadline", dark_past_the_deadline},
    {"withdrawn_at_the_deadline", withdrawn_at_the_deadline},
    {"cut_mid_touch", cut_mid_touch},
    {"drag_that_swipes_is_heard_with_its_direction", drag_that_swipes_is_heard_with_its_direction},
    {"the_tag_is_only_the_touchs", the_tag_is_only_the_touchs},
    {"heard_more_than_listed", heard_more_than_listed},
    {"waited_too_long", waited_too_long},
    {"abandoned", abandoned},
    {"every_allocation_refused", every_allocation_refused},
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    cJSON_Hooks hooks = {cjson_malloc, cjson_free};
    cJSON_InitHooks(&hooks);
    for (size_t i = 0; i < ARRAY_LEN(CASES); i++) {
        if (strcmp(argv[1], CASES[i].name) == 0) {
            current_case = CASES[i].name;
            reset();
            CASES[i].fn();
            printf("%s: ok\n", current_case);
            return 0;
        }
    }
    fprintf(stderr, "no case %s\n", argv[1]);
    return 2;
}
