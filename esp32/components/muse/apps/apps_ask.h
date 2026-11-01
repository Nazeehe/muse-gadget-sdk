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
 * The apps' `muse` module (connected apps Task 4): muse.ask(text, cb) asks Muse a question, after a tap. Portable C, like the runtime
 * it belongs to (apps_rt.h): the call runs on the app task and hands the turn to the platform (apps_port_ask, apps_port.h), which on
 * the gadget sends it to the user's Muse chat as a typed turn (muse_hatch_text_turn_to, muse_chat.h), the one a console's chat.py
 * sends: the user sees the app's question in their chat.
 *
 * The call (apps_muse_funcs[], with the "muse" permission, APPS_PERM_MUSE; sys.has("muse") is true). muse.ask(text, cb) sends
 * "(app <id>) <text>" and returns at once; cb(reply) or cb(nil, err) comes later, on the app task, never during the call: as one call
 * of the app with APPS_BUDGET_CALL (apps_rt_callback), whose error crashes the app as a timer's does.
 * - Raised at the call (P18), and nothing is sent: no "muse" permission, text not a string of 1 to APPS_ASK_TEXT_MAX bytes (a NUL in
 *   it included), cb not a function, no tap on the app within APPS_SHOW_TAP_MS (APPS_ASK_MSG_TAP; only a tap counts, not message() or
 *   alarm()), an ask that waits already (APPS_ASK_MSG_ONE), and less than APPS_ASK_GAP_MS since the last accepted ask began
 *   (APPS_ASK_MSG_WAIT, with the seconds left rounded up).
 * - Through the callback: the reply, or the gadget short of memory (APPS_ASK_MSG_NO_MEMORY), the turn's end in the chat's words
 *   (muse_chat.h: "interrupted", "busy with a voice turn", "not set up", or the turn's own, like "CAN'T REACH MUSE"), and the
 *   time running out (APPS_ASK_MSG_TIMED_OUT, after APPS_ASK_TIMEOUT_MS: P19).
 * - The reply is the turn's text, its messages joined by a blank line, at most APPS_ASK_REPLY_MAX bytes: one that runs past them is
 *   cut where a character starts, and ends with "…" (U+2026). Text that isn't clean UTF-8 is cleaned before it goes (apps_text_clean,
 *   newlines and tabs kept).
 * - The gap and the one at a time are the gadget's, not the app's: the last accepted ask's start, of any app, and a turn still at
 *   the chat, of any app, count (P17).
 *
 * The turn and the app (P17). The slot below is the module's, static, and the runtime doesn't own it: the chat task may answer after
 * the ask has timed out or its app has ended, and finds valid memory then. One turn of the apps' is at the chat at a time: the slot's
 * in-flight flag is set when the turn goes, and cleared only by the turn's end (apps_ask_done), never by a timeout, so an ask while a
 * turn the apps have let go of still runs is "one ask at a time". A timeout or the app's end only detaches the app: the turn isn't
 * cancelled, its later pieces are dropped (its generation is no longer the one wanted), and its end comes back as a result that the
 * app task frees.
 *
 * What the chat task and the app task share has one writer each, so no lock is needed: the in-flight flag (the app task sets it
 * while no turn is out, the turn's end clears it), the generation wanted (the app task's), the turn's block (the app task hands it
 * over before the turn goes; the chat task fills it and gives it back by the results list) and the results list (the chat task
 * pushes, the app task takes it whole). They are atomics, in internal RAM (S3 PSRAM has no compare-and-swap). The reply's block,
 * APPS_ASK_REPLY_MAX + 4 bytes with its header, comes from apps_port_realloc, through the apps' floor, when the ask starts; it is
 * freed when its result has been delivered or dropped.
 *
 * Results (P29, Task 3's shape). The turn's end puts its block on the results list and calls the app task's notify hook, which posts
 * APPS_EV_ASK_DONE (apps_task.h) with no payload: a post that finds the queue full loses nothing, as the loop takes the list every
 * round. Each result carries its ask's generation, given by the module, unique since boot: the loop hands it to the app that runs
 * (apps_ask_take), which keeps it only while that ask waits for it; any other, for an app that ended or an ask that timed out, is
 * freed and never runs (Review Focus 1).
 *
 * Logging. The reply's text and the ask's are never logged. Each ask's outcome is one line through the log hook, just before its
 * callback runs: "ask <id>: done (<n> bytes)" or "ask <id>: <err>" (the gadget prints it under the muse_apps tag).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps_rt.h"

#define APPS_ASK_GAP_MS 30000        /* from one accepted ask's start to the next, any app's */
#define APPS_ASK_TEXT_MAX 512        /* an ask's text, in bytes */
#define APPS_ASK_REPLY_MAX 2048      /* the reply's bytes kept: past them it is cut, and "…" added */
#define APPS_ASK_TIMEOUT_MS 120000   /* an ask's wait for its turn's end */
#define APPS_ASK_ERR_MAX 64          /* an error's words, NUL included */

/* What the call raises (the brief's words, which the guide's Common errors quote). */
#define APPS_ASK_MSG_TAP "muse.ask: allowed only within %d s of a tap"   /* APPS_SHOW_TAP_MS in seconds */
#define APPS_ASK_MSG_ONE "muse.ask: one ask at a time"
#define APPS_ASK_MSG_WAIT "muse.ask: wait %d s between asks"             /* the seconds left, rounded up */
/* What the callback gets, beside the chat's words. */
#define APPS_ASK_MSG_NO_MEMORY "the gadget is short of memory"
#define APPS_ASK_MSG_TIMED_OUT "timed out"

/*
 * A turn's block: its reply as it comes, then its end. Made by the call (apps_port_realloc), filled by the chat task through the sink
 * (apps_ask_piece, apps_ask_done), and handed back on the results list; the app task delivers it or frees it (apps_ask_result_free).
 */
typedef struct apps_ask_result {
    struct apps_ask_result *next;       /* the results list */
    uint32_t gen;                       /* its ask's generation */
    bool ok;                            /* it ended with no error: text is the reply */
    bool cut;                           /* the reply ran past APPS_ASK_REPLY_MAX: cut, and "…" added */
    uint16_t len;                       /* text's bytes */
    char err[APPS_ASK_ERR_MAX];         /* why it failed */
    char text[APPS_ASK_REPLY_MAX + 4];  /* the reply: APPS_ASK_REPLY_MAX bytes, then room for "…" and a NUL */
} apps_ask_result_t;

/* The app's side, which lives in its runtime (apps_rt.c, apps_rt_ask) from apps_rt_new to apps_rt_free: the ask that waits, if any.
 * The app task alone. */
typedef struct apps_ask_app {
    int ref;                    /* its callback, a registry reference: LUA_NOREF when none waits */
    uint32_t gen;               /* its generation; 0 when none waits */
    int64_t until;              /* when it times out (apps_port_mono_ms) */
    apps_ask_result_t *ready;   /* its result, whose callback is due */
    const char *why;            /* or what its callback is told without a result: a refusal, or the timeout */
} apps_ask_app_t;

/* The app task's hooks, set once before any ask (muse_apps_start): notify, on the chat task, when a result is on the list (posts
 * APPS_EV_ASK_DONE; it must not wait); log, on the app task, with each ask's one line. NULL for either is none. */
void apps_ask_setup(void (*notify)(void), void (*log)(const char *line));

/* The runtime's books, at apps_rt_new. */
void apps_ask_init(apps_ask_app_t *a);

/* Installs the global `muse`. apps_svc_open's protected call runs it, with the other modules. */
void apps_ask_open(lua_State *L, apps_rt_t *rt);

/* The app stopped or crashed: the callback is released, the turn detached, and a result that waits freed. No callback of an ask runs
 * after it. luaL_unref allocates nothing, so it is safe outside a call. */
void apps_ask_release(apps_ask_app_t *a, lua_State *L);

/* The runtime is freed (the VM goes with it): the turn detached, and a result that waits freed. */
void apps_ask_free(apps_ask_app_t *a);

/*
 * The sink (muse_chat.h's muse_hatch_text_sink_t, with user the ask's generation): on the chat task, or done at once on the app task
 * when the platform can't send the turn. piece adds text[0, len) to the reply, while the ask is still wanted; done ends the turn with
 * err NULL (the reply is in) or why, puts the block on the results list, clears the in-flight flag and calls the notify hook.
 */
void apps_ask_piece(void *user, const char *text, size_t len);
void apps_ask_done(void *user, const char *err);

/* ---- The app task's loop (apps_task.c; apphost too) ---- */

/* Takes the results list whole, oldest first (each ->next): the loop gives each to apps_ask_take. NULL when it is empty. */
apps_ask_result_t *apps_ask_results(void);

/* Frees a result. NULL is ignored. */
void apps_ask_result_free(apps_ask_result_t *r);

/* A result, for the app that runs (rt; NULL when none does): kept when it is the one its waiting ask wants, freed otherwise. Takes r. */
void apps_ask_take(apps_rt_t *rt, apps_ask_result_t *r);

/* Each round, outside a call: an ask whose time has run out (now_ms) is detached, and its callback is due with "timed out" (P19). */
void apps_ask_tick(apps_rt_t *rt, int64_t now_ms);

/* Whether a callback is due (apps_ask_run_one would make a call). */
bool apps_ask_has_work(const apps_rt_t *rt);

/* The monotonic time the loop should come back by: now (0) when a callback is due, the waiting ask's time, or INT64_MAX. */
int64_t apps_ask_next_deadline(const apps_rt_t *rt);

/* Runs the callback that is due, as one call of the app, after its line in the log: true when one ran; then the caller checks
 * apps_rt_state, as after any call. The app task, bracketed like any call. */
bool apps_ask_run_one(apps_rt_t *rt);
