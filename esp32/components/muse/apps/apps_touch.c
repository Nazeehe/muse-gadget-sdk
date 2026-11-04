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

#include "apps_touch.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "apps_install.h"
#include "apps_io.h"
#include "apps_lv.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_result.h"
#include "apps_task.h"
#include "apps_ui.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_apps.h"
#include "muse_input.h"
#include "muse_ui.h"

/*
 * app.control tap and drag (apps_touch.h has how its tasks meet, muse_apps.h the contract). The caller's side and the job are the
 * I/O worker's way (apps_snapshot.c's): a job of its own in PSRAM, one done. The LVGL task's side is the guard the touch player
 * calls inside the input read, the tag on_widget_event asks for, and the frame's hand-off and settle; it touches nothing but the
 * handshake, the touch's own state and what LVGL and the app layer say.
 */

static const char *TAG = "muse_apps";

#define BAD_ID "app id must match ^[a-z][a-z0-9_]{1,23}$"
#define SCROLLS_MAX 8  /* the scroll boxes a touch follows: in slot order, the first this many */

_Static_assert(MUSE_APPS_TOUCH_MS_MIN == MUSE_UI_DRAG_MS_MIN && MUSE_APPS_TOUCH_MS_MAX == MUSE_UI_DRAG_MS_MAX,
               "a touch's ms is the player's to play as it is");
_Static_assert(MUSE_APPS_TOUCH_SCREEN_MS + MUSE_APPS_TOUCH_MS_MAX + MUSE_APPS_TOUCH_SETTLE_MS < APPS_TOUCH_MIN_LEFT_MS,
               "a job that starts with the least it may has room for the screen, the longest touch and its settle");
_Static_assert(APPS_TOUCH_MIN_LEFT_MS < MUSE_APPS_TOUCH_TOTAL_MS, "a job that starts at once always plays");

/* ---- The handshake (apps_touch.h) ---- */

enum { T_IDLE, T_ASKED, T_QUEUED, T_PLAYING, T_SETTLING, T_DONE, T_ABANDONED };

/* Why a touch wasn't played (nothing was pressed). */
typedef enum { WHY_NONE, WHY_DARK, WHY_NOT_FRONT, WHY_HUNG, WHY_BUSY, WHY_COVERED, WHY_FINGER, WHY_NO_INPUT } why_t;
/* Why a touch was cut short. */
typedef enum { CUT_NONE, CUT_LEFT, CUT_COVERED, CUT_FINGER, CUT_SWIPE, CUT_LATE } cut_t;

typedef struct {
    int16_t slot;
    int8_t kind;    /* apps_ui.h's APPS_W_*, -1 for none */
    uint8_t event;  /* its bit, or a swipe's code with its direction (apps_ui.h), without the tag */
} heard_t;

/* The LVGL task's answer, from the hand-off to DONE: then the job's. */
typedef struct {
    why_t why;
    cut_t cut;
    int pressed;          /* the slot the touch went down on, or APPS_LV_HIT_LAYER */
    int pressed_kind;
    heard_t heard[MUSE_APPS_TOUCH_HEARD_MAX];
    int n_heard, more;    /* listed, and heard past those */
    uint32_t played_ms;
    bool front;           /* id is the app on screen, once the touch is over */
    apps_lv_scroll_t before[SCROLLS_MAX], after[SCROLLS_MAX];
    int n_before, n_after;
} answer_t;

/* The touch's state. The request (id to ms) is the job's, written while the word is IDLE and read by the LVGL task only once it has
 * moved the word from ASKED; the rest is the LVGL task's from then. */
static EXT_RAM_BSS_ATTR struct {
    char id[APPS_ID_MAX + 1];
    int x1, y1, x2, y2, ms;
    uint32_t gen;      /* the app's app_gen, read with its id at admit */
    bool swiped;       /* its drag was the layer's right swipe from the left edge (apps_touch_closed) */
    bool ended;        /* the player's end came */
    int64_t lift_ms;
    answer_t a;
} s_t;

static atomic_int s_state;            /* the handshake's word, in internal RAM */
static atomic_bool s_claimed;         /* a touch command is under way, from its admission to its done */
static _Atomic int64_t s_deadline_ms; /* the screen's, set before ASKED: a word the LVGL task can't read torn */

static bool move(int from, int to) {
    return atomic_compare_exchange_strong(&s_state, &from, to);
}

/* ---- The LVGL task's side ---- */

/* id is the app on screen with its touches reaching it: its layer in front with no card over it, no voice turn, and the app the
 * touch began on still the one that runs with its table on the layer. Atomics and the LVGL task's own state only. */
static bool front_now(uint32_t gen) {
    return apps_lv_front() && !apps_lv_cards_shown() && !apps_port_in_turn() && apps_task_touch_alive(gen);
}

static bool guard_admit(int x, int y) {
    if (atomic_load(&s_state) != T_QUEUED) {
        return false;  /* given up on (ABANDONED): its end lets the next one in */
    }
    why_t why = WHY_NONE;
    uint32_t gen = 0;
    int hit = APPS_LV_HIT_OUTSIDE;
    if (apps_port_dark()) {
        why = WHY_DARK;
    } else if (apps_port_in_turn() || !apps_lv_front() || apps_lv_cards_shown()) {
        why = WHY_NOT_FRONT;
    } else {
        switch (apps_task_touch_target(s_t.id, &gen)) {
            case APPS_TOUCH_TARGET_OK: break;
            case APPS_TOUCH_TARGET_HUNG: why = WHY_HUNG; break;
            case APPS_TOUCH_TARGET_BUSY: why = WHY_BUSY; break;
            default: why = WHY_NOT_FRONT; break;
        }
    }
    if (why == WHY_NONE && (hit = apps_lv_hit(x, y)) == APPS_LV_HIT_OUTSIDE) {
        why = WHY_COVERED;
    }
    if (why != WHY_NONE) {
        s_t.a.why = why;
        return false;
    }
    s_t.gen = gen;
    s_t.a.pressed = hit;
    s_t.a.pressed_kind = hit >= 0 ? apps_lv_kind(hit) : -1;
    s_t.a.n_before = apps_lv_scrolls(s_t.a.before, SCROLLS_MAX);
    return move(T_QUEUED, T_PLAYING);  /* not when the job gave it up just now */
}

static bool guard_keep(int x, int y) {
    cut_t cut = CUT_NONE;
    if (atomic_load(&s_state) != T_PLAYING) {
        cut = CUT_LATE;  /* the job gave up on it: lift now */
    } else if (s_t.swiped) {
        cut = CUT_SWIPE;
    } else if (!front_now(s_t.gen)) {
        cut = CUT_LEFT;
    } else if (apps_lv_hit(x, y) == APPS_LV_HIT_OUTSIDE) {
        cut = CUT_COVERED;
    }
    s_t.a.cut = cut;
    return cut == CUT_NONE;
}

static void guard_end(muse_ui_guard_end_t how, uint32_t ms) {
    s_t.ended = true;
    s_t.lift_ms = apps_port_mono_ms();
    s_t.a.played_ms = ms;
    int to = T_SETTLING, from = T_PLAYING;
    switch (how) {
        case MUSE_UI_GUARD_FINGER:
            s_t.a.why = WHY_FINGER;
            from = T_QUEUED, to = T_DONE;
            break;
        case MUSE_UI_GUARD_REFUSED:
            from = T_QUEUED, to = T_DONE;
            break;
        case MUSE_UI_GUARD_CUT_FINGER:
            s_t.a.cut = CUT_FINGER;
            break;
        default:  /* played, or cut by keep (its cut says why) */
            break;
    }
    if (!move(from, to)) {
        move(T_ABANDONED, T_IDLE);  /* given up on: nobody waits for its answer */
    }
}

static const muse_ui_guard_t GUARD = {guard_admit, guard_keep, guard_end};

void apps_touch_supervise(void) {
    const int state = atomic_load(&s_state);
    const int64_t now = apps_port_mono_ms();
    if (state == T_ASKED) {
        if (apps_port_dark() || now >= atomic_load(&s_deadline_ms) || !move(T_ASKED, T_QUEUED)) {
            return;  /* the screen not lit yet (its first lit frame hands it on), or the job's deadline answers */
        }
        memset(&s_t.a, 0, sizeof s_t.a);
        s_t.a.pressed = APPS_LV_HIT_LAYER;
        s_t.swiped = s_t.ended = false;
        /* The job's points are the apps' canvas's; the touch is played, and admitted and kept (apps_lv_hit), on the screen. */
        int32_t x1 = s_t.x1, y1 = s_t.y1, x2 = s_t.x2, y2 = s_t.y2;
        apps_lv_to_screen(&x1, &y1);
        apps_lv_to_screen(&x2, &y2);
        if (!muse_ui_guarded_drag((int)x1, (int)y1, (int)x2, (int)y2, s_t.ms, &GUARD)) {
            s_t.a.why = WHY_NO_INPUT;
            s_t.ended = true;
            if (!move(T_QUEUED, T_DONE)) {
                move(T_ABANDONED, T_IDLE);
            }
        }
        return;
    }
    if (state == T_SETTLING) {
        /* The scroll the touch began comes to rest (its throw, an elastic end), at most MUSE_APPS_TOUCH_SETTLE_MS. */
        apps_lv_scroll_t after[SCROLLS_MAX];
        const int n = apps_lv_scrolls(after, SCROLLS_MAX);
        bool moving = false;
        for (int i = 0; i < n; i++) {
            moving |= after[i].moving;
        }
        if (moving && now < s_t.lift_ms + MUSE_APPS_TOUCH_SETTLE_MS) {
            return;
        }
        memcpy(s_t.a.after, after, sizeof after[0] * (size_t)n);
        s_t.a.n_after = n;
        s_t.a.front = !s_t.swiped && front_now(s_t.gen);
        if (!move(T_SETTLING, T_DONE)) {
            move(T_ABANDONED, T_IDLE);
        }
        return;
    }
    if (state == T_ABANDONED && s_t.ended) {
        move(T_ABANDONED, T_IDLE);  /* given up on as it settled */
    }
}

uint8_t apps_touch_tag(uint16_t slot, uint8_t event) {
    const int state = atomic_load(&s_state);
    if (state != T_PLAYING && !(state == T_ABANDONED && !s_t.ended)) {
        return event;
    }
    answer_t *a = &s_t.a;
    if (a->n_heard < MUSE_APPS_TOUCH_HEARD_MAX) {
        a->heard[a->n_heard++] = (heard_t){.slot = (int16_t)slot, .kind = (int8_t)apps_lv_kind(slot), .event = event};
    } else {
        a->more++;
    }
    return (uint8_t)(event | APPS_UI_INJECTED);
}

void apps_touch_closed(void) {
    if (atomic_load(&s_state) == T_PLAYING) {
        s_t.swiped = true;
    }
}

/* ---- The job (the I/O worker's) ---- */

typedef struct {
    char id[APPS_ID_MAX + 1];
    bool drag;
    int x1, y1, x2, y2, ms;
    muse_apps_done_cb done;
    void *ctx;
    int64_t stamp_ms, deadline_ms;  /* when it was asked for, and the whole touch's end */
} touch_job_t;

static const char *const KINDS[] = {"label", "button", "box", "arc", "bar", "image", "spinner"};
_Static_assert(sizeof KINDS / sizeof KINDS[0] == APPS_W_SPINNER + 1, "a name for each kind, as status names it");

static const char *kind_name(int kind) {
    return kind >= 0 && kind <= APPS_W_SPINNER ? KINDS[kind] : "widget";
}

/* An event's name as heard lists it; a swipe's direction (apps_ui_dir_name) goes after it. */
static const char *event_name(uint8_t code) {
    switch (code & ~APPS_UI_DIR_MASK) {
        case APPS_UI_TAP: return "tap";
        case APPS_UI_LONG_PRESS: return "long_press";
        case APPS_UI_PRESS: return "press";
        case APPS_UI_SWIPE: return "swipe";
        default: return "release";
    }
}

/* Asks the LVGL task for the touch, the screen woken, and waits for its answer: true with *a what it saw; otherwise *r is the
 * refusal. */
static bool play(const touch_job_t *job, answer_t *a, cJSON **r) {
    if (atomic_load(&s_state) != T_IDLE) {
        *r = apps_result_failure(APPS_ERR_BUSY, MUSE_APPS_TOUCH_BUSY);  /* a touch given up on is still ending */
        return false;
    }
    if (apps_port_dark()) {
        ESP_LOGI(TAG, "touch %s: waking the screen", job->id);
    }
    /* Always, as a snapshot's: it lights a dark screen and keeps a lit one lit until a frame hands the touch on. */
    muse_input_wake("touch");
    const int64_t deadline = apps_port_mono_ms() + MUSE_APPS_TOUCH_SCREEN_MS;
    snprintf(s_t.id, sizeof s_t.id, "%s", job->id);
    s_t.x1 = job->x1, s_t.y1 = job->y1, s_t.x2 = job->x2, s_t.y2 = job->y2, s_t.ms = job->ms;
    atomic_store(&s_deadline_ms, deadline);
    atomic_store(&s_state, T_ASKED);
    const TickType_t ticks = pdMS_TO_TICKS(APPS_TOUCH_POLL_MS) > 0 ? pdMS_TO_TICKS(APPS_TOUCH_POLL_MS) : 1;
    char msg[160];
    for (;;) {
        const int state = atomic_load(&s_state);
        if (state == T_DONE) {
            break;
        }
        const int64_t now = apps_port_mono_ms();
        /* Not lit, or not taken, in time: taken back as IDLE, unless the player has taken it (then its answer comes). A
         * withdrawn request never reaches the guard, so nothing of the LVGL task's follows it. */
        bool taken_back = false;
        if (now >= deadline && state == T_ASKED) {
            taken_back = move(T_ASKED, T_IDLE);
        } else if (now >= deadline && state == T_QUEUED && muse_ui_guarded_withdraw()) {
            atomic_store(&s_state, T_IDLE);
            taken_back = true;
        }
        if (taken_back) {
            ESP_LOGW(TAG, "touch %s: the screen wasn't lit and the touch taken within %d ms", job->id, MUSE_APPS_TOUCH_SCREEN_MS);
            *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_TOUCH_NO_WAKE);
            return false;
        }
        if (now >= job->deadline_ms && state != T_ASKED && state != T_IDLE && move(state, T_ABANDONED)) {
            /* The LVGL task stopped reading the touch: its next read cuts it, and its end lets the next one in. */
            ESP_LOGW(TAG, "touch %s: it didn't end within %d ms: given up", job->id, MUSE_APPS_TOUCH_TOTAL_MS);
            *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_TOUCH_SLOW);
            return false;
        }
        apps_io_assist();
        vTaskDelay(ticks);
    }
    *a = s_t.a;
    atomic_store(&s_state, T_IDLE);
    switch (a->why) {
        case WHY_NONE: return true;
        case WHY_DARK: *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_TOUCH_NO_WAKE); break;
        case WHY_HUNG: *r = apps_result_failure(APPS_ERR_UNAVAILABLE, APPS_TASK_MSG_HUNG); break;
        case WHY_BUSY: *r = apps_result_failure(APPS_ERR_BUSY, APPS_TASK_MSG_BUSY); break;
        case WHY_FINGER: *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_TOUCH_FINGER); break;
        case WHY_NO_INPUT: *r = apps_result_failure(APPS_ERR_UNAVAILABLE, MUSE_APPS_TOUCH_NO_INPUT); break;
        case WHY_COVERED:
            snprintf(msg, sizeof msg, MUSE_APPS_TOUCH_COVERED, job->id);
            *r = apps_result_failure(APPS_ERR_UNAVAILABLE, msg);
            break;
        default:
            snprintf(msg, sizeof msg, MUSE_APPS_TOUCH_NOT_FRONT, job->id);
            *r = apps_result_failure(APPS_ERR_UNAVAILABLE, msg);
            break;
    }
    return false;
}

static bool add_text(cJSON *array, const char *text) {
    return cJSON_AddItemToArray(array, cJSON_CreateString(text));
}

/* The boxes whose scroll the touch moved, or that were still moving at the settle's bound (a warning each), into scroll. */
static bool add_scrolls(const touch_job_t *job, const answer_t *a, cJSON *scroll, cJSON *warnings) {
    (void)job;
    char item[96];
    for (int i = 0; i < a->n_after; i++) {
        const apps_lv_scroll_t *s = &a->after[i];
        bool moved = s->moving;
        for (int j = 0; j < a->n_before; j++) {
            moved |= a->before[j].slot == s->slot && a->before[j].y != s->y;
        }
        if (!moved) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        snprintf(item, sizeof item, "box#%d", (int)s->slot);
        if (!cJSON_AddItemToArray(scroll, o) || cJSON_AddStringToObject(o, "id", item) == NULL ||
            cJSON_AddNumberToObject(o, "y", (double)s->y) == NULL) {
            return false;
        }
        if (s->moving) {
            snprintf(item, sizeof item, "box#%d was still scrolling %d ms after the lift: its y is where it was then", (int)s->slot,
                     MUSE_APPS_TOUCH_SETTLE_MS);
            if (!add_text(warnings, item)) {
                return false;
            }
        }
    }
    return true;
}

/* The success's payload (muse_apps.h): NULL without memory. */
static cJSON *payload(const touch_job_t *job, const answer_t *a) {
    char item[APPS_ID_MAX + 240];
    cJSON *heard = NULL, *scroll = NULL, *warnings = NULL;
    cJSON *p = cJSON_CreateObject();
    bool ok = p != NULL && cJSON_AddStringToObject(p, "id", job->id) != NULL &&
              cJSON_AddStringToObject(p, "action", job->drag ? "drag" : "tap") != NULL;
    if (ok && a->pressed >= 0) {
        snprintf(item, sizeof item, "%s#%d", kind_name(a->pressed_kind), a->pressed);
        ok = cJSON_AddStringToObject(p, "pressed", item) != NULL;
    } else if (ok) {
        ok = cJSON_AddNullToObject(p, "pressed") != NULL;
    }
    ok = ok && (heard = cJSON_AddArrayToObject(p, "heard")) != NULL;
    for (int i = 0; ok && i < a->n_heard; i++) {
        const uint8_t code = a->heard[i].event;
        const bool swipe = (code & ~APPS_UI_DIR_MASK) == APPS_UI_SWIPE;
        snprintf(item, sizeof item, "%s#%d %s%s%s", kind_name(a->heard[i].kind), (int)a->heard[i].slot, event_name(code),
                 swipe ? " " : "", swipe ? apps_ui_dir_name(code) : "");
        ok = add_text(heard, item);
    }
    ok = ok && cJSON_AddBoolToObject(p, "front", a->front) != NULL &&
         cJSON_AddBoolToObject(p, "cut", a->cut != CUT_NONE) != NULL &&
         cJSON_AddNumberToObject(p, "played_ms", (double)a->played_ms) != NULL &&
         (scroll = cJSON_AddArrayToObject(p, "scroll")) != NULL && (warnings = cJSON_AddArrayToObject(p, "warnings")) != NULL;
    const char *cut = NULL;
    switch (a->cut) {
        case CUT_LEFT:
            cut = "cut short: '%s' left the front (it ended, or another app, the launcher, a card or a voice turn took the screen); "
                  "the press ended with a release and no tap, and nothing else was touched";
            break;
        case CUT_COVERED:
            cut = "cut short: something came over '%s' where the touch was (a picture Muse showed, or a pairing code); the press "
                  "ended with a release and no tap, and nothing else was touched";
            break;
        case CUT_FINGER: cut = "cut short: someone touched the screen; the press ended with a release and no tap"; break;
        case CUT_SWIPE: cut = "the drag was a right swipe from the left edge: '%s' went behind, as a finger's would"; break;
        default: break;
    }
    if (ok && cut != NULL) {
        snprintf(item, sizeof item, cut, job->id);
        ok = add_text(warnings, item);
    }
    if (ok && a->more > 0) {
        snprintf(item, sizeof item, "%d more events came than heard lists (it keeps the first %d)", a->more,
                 MUSE_APPS_TOUCH_HEARD_MAX);
        ok = add_text(warnings, item);
    }
    ok = ok && add_scrolls(job, a, scroll, warnings);
    if (!ok) {
        cJSON_Delete(p);
        return NULL;
    }
    return p;
}

/* The whole touch, on the worker: its result (NULL without memory for it). */
static cJSON *touch_result(const touch_job_t *job) {
    const int64_t at = apps_port_mono_ms();
    char msg[96];
    if (job->deadline_ms - at < APPS_TOUCH_MIN_LEFT_MS) {  /* it waited in the worker's queue: nothing is done */
        snprintf(msg, sizeof msg, "the gadget was busy for %d ms; the touch wasn't played, so try again", (int)(at - job->stamp_ms));
        return apps_result_failure(APPS_ERR_BUSY, msg);
    }
    char running[APPS_ID_MAX + 1];
    if (!apps_task_running(running, sizeof running) || strcmp(running, job->id) != 0) {
        snprintf(msg, sizeof msg, MUSE_APPS_TOUCH_NOT_RUNNING, job->id);
        return apps_result_failure(APPS_ERR_NOT_RUNNING, msg);
    }
    answer_t a;
    cJSON *r = NULL;
    if (!play(job, &a, &r)) {
        return r;
    }
    char pressed[32] = "the layer";
    if (a.pressed >= 0) {
        snprintf(pressed, sizeof pressed, "%s#%d", kind_name(a.pressed_kind), a.pressed);
    }
    ESP_LOGI(TAG, "touch %s: pressed %s, %d events, played %u ms%s%s", job->id, pressed, a.n_heard + a.more, (unsigned)a.played_ms,
             a.cut != CUT_NONE ? ", cut" : "", a.front ? "" : ", not in front after");
    cJSON *p = payload(job, &a);
    return p != NULL ? apps_result_success(p) : NULL;
}

static void touch_run(void *arg) {
    touch_job_t *job = arg;
    if (job->drag) {
        ESP_LOGI(TAG, "touch %s: drag %d,%d to %d,%d over %d ms", job->id, job->x1, job->y1, job->x2, job->y2, job->ms);
    } else {
        ESP_LOGI(TAG, "touch %s: tap at %d,%d for %d ms", job->id, job->x1, job->y1, job->ms);
    }
    cJSON *r = touch_result(job);
    if (r == NULL) {
        r = apps_result_failure(APPS_ERR_OUT_OF_MEMORY, APPS_NO_ROOM_RESULT);
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(r, "error");
    if (error != NULL) {
        ESP_LOGI(TAG, "touch %s: %s: %s", job->id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code")),
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message")));
    }
    atomic_store(&s_claimed, false);  /* before done: a touch asked once the result is read isn't busy */
    job->done(job->ctx, r);
    apps_port_free(job);
}

static bool on_screen(int v) {
    return v >= 0 && v <= MUSE_APPS_TOUCH_XY_MAX;
}

bool muse_apps_touch(const char *id, bool drag, const muse_apps_at_t *at, muse_apps_done_cb done, void *ctx, const char **code,
                     const char **err) {
    const char *unused;
    code = code != NULL ? code : &unused;
    err = err != NULL ? err : &unused;
    *code = APPS_ERR_INVALID_PARAMS;
    if (done == NULL) {
        *err = "no callback for the result";
        return false;
    }
    if (id == NULL || !apps_id_valid(id)) {
        *err = BAD_ID;
        return false;
    }
    if (at == NULL || !on_screen(at->x) || !on_screen(at->y) || at->ms < MUSE_APPS_TOUCH_MS_MIN || at->ms > MUSE_APPS_TOUCH_MS_MAX ||
        (drag && (!on_screen(at->x2) || !on_screen(at->y2)))) {
        *err = drag ? MUSE_APPS_TOUCH_BAD_DRAG : MUSE_APPS_TOUCH_BAD_TAP;
        return false;
    }
    if (atomic_exchange(&s_claimed, true)) {
        *code = APPS_ERR_BUSY;
        *err = MUSE_APPS_TOUCH_BUSY;
        return false;
    }
    *code = APPS_ERR_OUT_OF_MEMORY;
    touch_job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job == NULL) {
        atomic_store(&s_claimed, false);
        *err = "out of memory: no room for the command";
        return false;
    }
    memset(job, 0, sizeof *job);
    snprintf(job->id, sizeof job->id, "%s", id);
    job->drag = drag;
    job->x1 = at->x;
    job->y1 = at->y;
    job->x2 = drag ? at->x2 : at->x;  /* a tap is a drag to its own point */
    job->y2 = drag ? at->y2 : at->y;
    job->ms = at->ms;
    job->done = done;
    job->ctx = ctx;
    job->stamp_ms = apps_port_mono_ms();
    job->deadline_ms = job->stamp_ms + MUSE_APPS_TOUCH_TOTAL_MS;  /* stamped here: the queue's wait is spent too */
    if (!apps_io_submit(touch_run, job)) {  /* it says why, in one log line */
        apps_port_free(job);
        atomic_store(&s_claimed, false);
        *err = APPS_IO_REFUSED;
        return false;
    }
    return true;
}
