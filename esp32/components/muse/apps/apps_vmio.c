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

#include "apps_vmio.h"

#include <stdio.h>
#include <string.h>

#include "apps_json.h"
#include "apps_port.h"
#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

/*
 * The `vm` module (apps_vmio.h has the contract). How the books go:
 *
 * - The app's own calls (read, write, command) are requests: a record each, from the call to its callback. A record waits with its
 *   job (R_WAITING), is at the worker (R_RUNNING), waits for its ack (R_ACK, a command written with a callback to tell), or holds its
 *   result for the callback (R_DONE). The waiting, running and acking ones are the queue's (at most APPS_VM_QUEUE + 1); a call past
 *   that, or one whose job the floor refuses, is an early answer: its callback and what to tell it go into the callbacks' table, and
 *   the next callbacks to run are those.
 * - The watches, the acks' poll and the heartbeat are the module's own polls: a due time each, and nothing queued. Each round's tick,
 *   when nothing is at the worker, sends one thing: an acks poll that is due, then the heartbeat, then the watch due first (only while
 *   active), then the app's oldest waiting request. So one job is at the worker at a time, the polls coalesce (a due poll is sent once
 *   when the turn comes), and the app's requests can't starve them.
 * - A job comes back to apps_vmio_take, which files it: a request's result waits for its callback (in the order the results came), a
 *   watch's calls back only for a change of outcome, an acks read answers the commands it lists. Its epoch says which books it was
 *   made from: one sent before a release (a stop) is freed.
 * - The callbacks live in one table of the app's registry: [id] for a request's, [-(slot + 1)] for a watch's, [0] the early answers
 *   (2i - 1: the callback, 2i: what to tell it). A release unrefs the table, so a stop or a crash lets go of every callback at once.
 *
 * Every binding allocates in the app's heap before it holds a block of apps_port_realloc, so a raise (no memory, a kill) never leaks
 * one: the callback goes into the table first, and a call that then fails sets it to nil, which allocates nothing.
 */

#define STR_(x) #x
#define STR(x) STR_(x)

#define PATH_RULE "(a-z 0-9 _ . / -, no '..', at most " STR(APPS_PATH_MAX) " characters)"
#define OPTS_EXAMPLE "{circle = true, max = 200}"
#define OUTBOX "outbox/"
#define ACKS_PATH "state/acks.json"
#define BEAT_PATH "outbox/active.json"
#define SHOWN_KEY_MAX 24

/* Whose a job is. */
enum { K_READ = 1, K_WRITE, K_COMMAND, K_WATCH, K_ACKS, K_BEAT };

/* What a read gives, by the path's ending. */
typedef enum { T_BAD = 0, T_JSON, T_TXT, T_JPEG, T_BIN } read_type_t;

/* An own request's record. */
enum { R_FREE = 0, R_WAITING, R_RUNNING, R_ACK, R_DONE };

typedef struct {
    uint32_t id;         /* its callback's key in the table; 0 for a free record */
    uint8_t kind;        /* K_READ, K_WRITE or K_COMMAND */
    uint8_t state;
    bool has_cb;
    uint32_t done_at;    /* R_DONE: the order the results came in */
    apps_vm_job_t *job;  /* R_WAITING: the job to send; R_DONE: the result, or NULL for a command answered by its ack or its time */
    int64_t seq;         /* a command's */
    int64_t ack_until;   /* R_ACK: when it times out */
    bool ok;             /* R_DONE without a job: acked, or why is the failure */
    const char *why;
    char *result;        /* acked: its result's JSON, a block of apps_port_realloc; NULL for nil */
    size_t result_len;
} req_t;

typedef struct {
    uint32_t gen;            /* its life, which its handles carry; 0 for a free slot */
    uint8_t every_s;
    bool reported;           /* it has called back once */
    bool last_ok;            /* what it last called back with: the text, or an error */
    uint16_t last_len;
    int64_t due;             /* its next read, while the app is in front and lit */
    uint32_t done_at;
    apps_vm_job_t *ready;    /* a result whose callback is due */
    char rel[APPS_PATH_MAX + 1];
    char last[APPS_VM_WATCH_MAX];  /* the text, or the error's words, it last called back with */
} watch_t;

#define REQS_MAX (2 * (APPS_VM_QUEUE + 1))

struct apps_vmio {
    apps_rt_t *rt;
    int ref;               /* the callbacks' table, a registry reference; LUA_NOREF until the first request */
    uint32_t epoch;        /* the books' life: a release begins new ones */
    uint32_t next_id;      /* the last request's id */
    uint32_t next_done;    /* the last result's place in the order */
    bool active;           /* in front and lit, at the last tick */
    bool running;          /* a job is at the worker */
    req_t reqs[REQS_MAX];
    watch_t watches[APPS_VM_WATCHES_MAX];
    uint32_t watch_gen;
    int64_t last_seq;      /* the last command's */
    int64_t acks_due;      /* the next acks poll; INT64_MAX when none waits */
    bool beating;          /* the heartbeat goes: a watch, and in front and lit */
    int64_t beat_due;
    lua_Integer early_head, early_tail;  /* the early answers waiting in the table's [0]: [head, tail) */
};

/* An early answer's message, and the first argument the callback gets: nil for a read's, false for a write's and a command's. */
static const char *const EARLY[] = {APPS_VM_MSG_TOO_MANY, APPS_VM_MSG_NO_MEMORY};
enum { EARLY_TOO_MANY = 0, EARLY_NO_MEMORY = 1 };

/* What a watch's handle holds: its slot and its life, so a handle of a cancelled watch names nothing, whatever took its slot. */
typedef struct {
    uint32_t gen;
    uint8_t slot;
} watch_handle_t;

/* ---- Jobs ---- */

void apps_vm_job_free(apps_vm_job_t *job) {
    if (job != NULL) {
        apps_port_free(job->data);
        apps_port_free(job);
    }
}

/* A job for the app's file rel: NULL when the floor refuses its block. */
static apps_vm_job_t *new_job(const apps_vmio_t *v, uint8_t kind, uint8_t op, const char *rel, uint32_t max) {
    apps_vm_job_t *job = apps_port_realloc(NULL, sizeof *job);
    if (job == NULL) {
        return NULL;
    }
    memset(job, 0, sizeof *job);
    const char *id = apps_rt_id(v->rt);
    snprintf(job->path, sizeof job->path, "%s%s/%s", APPS_VM_DIR, id, rel);  /* a valid id and path: always fits */
    job->rel = (uint8_t)(strlen(APPS_VM_DIR) + strlen(id) + 1);
    job->kind = kind;
    job->op = op;
    job->max = max;
    job->epoch = v->epoch;
    return job;
}

static read_type_t read_type(const char *path) {
    static const struct {
        const char *end;
        read_type_t type;
    } ENDINGS[] = {{".json", T_JSON}, {".txt", T_TXT}, {".jpg", T_JPEG}, {".jpeg", T_JPEG}, {".bin", T_BIN}};
    size_t n = strlen(path);
    for (size_t i = 0; i < sizeof ENDINGS / sizeof ENDINGS[0]; i++) {
        size_t m = strlen(ENDINGS[i].end);
        if (n > m && memcmp(path + n - m, ENDINGS[i].end, m) == 0) {
            return ENDINGS[i].type;
        }
    }
    return T_BAD;
}

/* ---- The books ---- */

size_t apps_vmio_size(void) {
    return sizeof(apps_vmio_t);
}

void apps_vmio_init(apps_vmio_t *v, apps_rt_t *rt) {
    memset(v, 0, sizeof *v);
    v->rt = rt;
    v->ref = LUA_NOREF;
    v->acks_due = INT64_MAX;
    v->beat_due = INT64_MAX;
    v->early_head = v->early_tail = 1;
}

/* Every block the books hold goes, and new books begin (a job at the worker comes back to a later epoch, and is freed). */
static void drop_all(apps_vmio_t *v) {
    for (int i = 0; i < REQS_MAX; i++) {
        apps_vm_job_free(v->reqs[i].job);
        apps_port_free(v->reqs[i].result);
    }
    for (int i = 0; i < APPS_VM_WATCHES_MAX; i++) {
        apps_vm_job_free(v->watches[i].ready);
    }
    apps_rt_t *rt = v->rt;
    uint32_t epoch = v->epoch + 1;
    int64_t last_seq = v->last_seq;  /* a stopped runtime started again goes on from it */
    apps_vmio_init(v, rt);
    v->epoch = epoch;
    v->last_seq = last_seq;
}

void apps_vmio_release(apps_vmio_t *v, lua_State *L) {
    if (v == NULL) {
        return;
    }
    if (v->ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, v->ref);
        v->ref = LUA_NOREF;
    }
    drop_all(v);
}

void apps_vmio_free(apps_vmio_t *v) {
    if (v != NULL) {
        drop_all(v);
    }
}

static int own_pending(const apps_vmio_t *v) {
    int n = 0;
    for (int i = 0; i < REQS_MAX; i++) {
        uint8_t s = v->reqs[i].state;
        n += s == R_WAITING || s == R_RUNNING || s == R_ACK ? 1 : 0;
    }
    return n;
}

static req_t *free_record(apps_vmio_t *v) {
    for (int i = 0; i < REQS_MAX; i++) {
        if (v->reqs[i].state == R_FREE) {
            return &v->reqs[i];
        }
    }
    return NULL;
}

/* Whether another request of the app's may wait: one at the worker and APPS_VM_QUEUE more (P11). */
static bool room(apps_vmio_t *v) {
    return own_pending(v) < APPS_VM_QUEUE + 1 && free_record(v) != NULL;
}

static req_t *find_request(apps_vmio_t *v, uint32_t id, uint8_t state) {
    for (int i = 0; i < REQS_MAX; i++) {
        if (v->reqs[i].id == id && v->reqs[i].state == state) {
            return &v->reqs[i];
        }
    }
    return NULL;
}

static int watch_count(const apps_vmio_t *v) {
    int n = 0;
    for (int i = 0; i < APPS_VM_WATCHES_MAX; i++) {
        n += v->watches[i].gen != 0 ? 1 : 0;
    }
    return n;
}

/* ---- What comes back (the app task, outside a call) ---- */

/* A request's result: it waits for its callback. A command written with a callback waits for its ack instead. */
static void take_own(apps_vmio_t *v, apps_vm_job_t *job, int64_t now_ms) {
    req_t *r = find_request(v, job->req, R_RUNNING);
    if (r == NULL) {
        apps_vm_job_free(job);
        return;
    }
    if (r->kind == K_COMMAND && job->ok && r->has_cb) {
        apps_vm_job_free(job);
        r->state = R_ACK;
        r->ack_until = now_ms + APPS_VM_ACK_MS;
        if (v->acks_due > now_ms + APPS_VM_ACK_POLL_MS) {
            v->acks_due = now_ms + APPS_VM_ACK_POLL_MS;
        }
        return;
    }
    if (!r->has_cb) {
        apps_vm_job_free(job);
        *r = (req_t){0};
        return;
    }
    r->job = job;
    r->state = R_DONE;
    r->done_at = ++v->next_done;
}

/* A watch's read: a callback when the outcome changed since the last one (P15), or for the first; a newer outcome replaces one whose
 * callback hasn't run yet. Its next read is every_s from now (P12). */
static void take_watch(apps_vmio_t *v, apps_vm_job_t *job, int64_t now_ms) {
    watch_t *w = job->slot < APPS_VM_WATCHES_MAX ? &v->watches[job->slot] : NULL;
    if (w == NULL || w->gen == 0 || w->gen != job->req) {
        apps_vm_job_free(job);  /* cancelled since */
        return;
    }
    w->due = now_ms + (int64_t)w->every_s * 1000;
    const char *text = job->ok ? (const char *)job->data : job->err;
    size_t len = job->ok ? job->len : strlen(job->err);
    if (len > sizeof w->last) {
        len = sizeof w->last;  /* can't be: the read's max is the watch's */
    }
    bool same = w->reported && w->last_ok == job->ok && w->last_len == len && (len == 0 || memcmp(w->last, text, len) == 0);
    if (same) {
        apps_vm_job_free(job);
        return;
    }
    if (len > 0) {
        memcpy(w->last, text, len);
    }
    w->last_len = (uint16_t)len;
    w->last_ok = job->ok;
    w->reported = true;
    apps_vm_job_free(w->ready);
    w->ready = job;
    w->done_at = ++v->next_done;
}

/* The command waiting for its ack whose seq this is: acked, with its result's JSON (raw, as the hook wrote it), or NULL for none. */
static void ack(apps_vmio_t *v, int64_t seq, const char *result, size_t len) {
    for (int i = 0; i < REQS_MAX; i++) {
        req_t *r = &v->reqs[i];
        if (r->state != R_ACK || r->seq != seq) {
            continue;
        }
        r->state = R_DONE;
        r->done_at = ++v->next_done;
        r->ok = true;
        if (result != NULL) {
            r->result = apps_port_realloc(NULL, len);
            if (r->result != NULL) {
                memcpy(r->result, result, len);
                r->result_len = len;
            } else {
                r->ok = false;
                r->why = APPS_VM_MSG_NO_MEMORY;
            }
        }
    }
}

/* state/acks.json, {"acks":[{"seq":<int>,"result":<any JSON>}, ...]} (P3): each entry that names a waiting command acks it. A file
 * that doesn't read as that is read as far as it does: the commands it didn't ack wait for the next poll. */
static void take_acks(apps_vmio_t *v, const apps_vm_job_t *job) {
    if (job->data == NULL) {
        return;
    }
    apps_jr_t r;
    char key[8];
    apps_jr_init(&r, (const char *)job->data, job->len);
    if (!apps_jr_object(&r)) {
        return;
    }
    int k;
    while ((k = apps_jr_key_any(&r, key, sizeof key)) > 0) {
        if (k != 1 || strcmp(key, "acks") != 0) {
            if (!apps_jr_skip(&r)) {
                return;
            }
            continue;
        }
        if (!apps_jr_array(&r)) {
            return;
        }
        while (apps_jr_next(&r) == 1) {
            if (!apps_jr_object(&r)) {
                return;
            }
            int64_t seq = 0;
            bool have_seq = false;
            const char *result = NULL;
            size_t result_len = 0;
            int e;
            while ((e = apps_jr_key_any(&r, key, sizeof key)) > 0) {
                if (e == 1 && strcmp(key, "seq") == 0) {
                    if (!apps_jr_integer(&r, &seq)) {
                        return;
                    }
                    have_seq = true;
                } else if (e == 1 && strcmp(key, "result") == 0) {
                    result = apps_jr_raw_value(&r, &result_len);
                    if (result == NULL) {
                        return;
                    }
                } else if (!apps_jr_skip(&r)) {
                    return;
                }
            }
            if (e < 0) {
                return;
            }
            if (have_seq) {
                ack(v, seq, result, result_len);
            }
        }
    }
}

static bool any_acking(const apps_vmio_t *v) {
    for (int i = 0; i < REQS_MAX; i++) {
        if (v->reqs[i].state == R_ACK) {
            return true;
        }
    }
    return false;
}

void apps_vmio_take(apps_rt_t *rt, apps_vm_job_t *job, int64_t now_ms) {
    apps_vmio_t *v = apps_rt_vmio(rt);
    if (v == NULL || job->epoch != v->epoch) {
        apps_vm_job_free(job);  /* sent before a release */
        return;
    }
    v->running = false;
    switch (job->kind) {
    case K_READ:
    case K_WRITE:
    case K_COMMAND:
        take_own(v, job, now_ms);
        break;
    case K_WATCH:
        take_watch(v, job, now_ms);
        break;
    case K_ACKS:
        if (job->ok) {
            take_acks(v, job);
        }
        apps_vm_job_free(job);
        v->acks_due = any_acking(v) ? now_ms + APPS_VM_ACK_POLL_MS : INT64_MAX;
        break;
    default:
        apps_vm_job_free(job);  /* the heartbeat's: the next one says it again */
        break;
    }
}

/* ---- The tick (the app task, each round) ---- */

/* The job goes to the worker; one the port refuses comes back at once, failed in the port's words. */
static void send(apps_vmio_t *v, apps_vm_job_t *job, int64_t now_ms) {
    job->epoch = v->epoch;
    if (apps_port_vm_submit(job)) {
        v->running = true;
        return;
    }
    job->ok = false;
    if (job->err[0] == '\0') {
        snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NO_MEMORY);
    }
    apps_vmio_take(v->rt, job, now_ms);
}

/* The heartbeat's write: {"app":"<id>","n":<watches>} (an id needs no escape). NULL when the floor refuses. */
static apps_vm_job_t *beat_job(apps_vmio_t *v, int watches) {
    apps_vm_job_t *job = new_job(v, K_BEAT, APPS_VM_WRITE, BEAT_PATH, 0);
    char *text = job != NULL ? apps_port_realloc(NULL, APPS_ID_MAX + 32) : NULL;
    if (text == NULL) {
        apps_vm_job_free(job);
        return NULL;
    }
    int n = snprintf(text, APPS_ID_MAX + 32, "{\"app\":\"%s\",\"n\":%d}", apps_rt_id(v->rt), watches);
    job->data = (uint8_t *)text;
    job->len = (size_t)n;
    return job;
}

static watch_t *due_watch(apps_vmio_t *v, int64_t now_ms) {
    watch_t *pick = NULL;
    for (int i = 0; i < APPS_VM_WATCHES_MAX; i++) {
        watch_t *w = &v->watches[i];
        if (w->gen != 0 && w->due <= now_ms && (pick == NULL || w->due < pick->due)) {
            pick = w;
        }
    }
    return pick;
}

static req_t *oldest_waiting(apps_vmio_t *v) {
    req_t *pick = NULL;
    for (int i = 0; i < REQS_MAX; i++) {
        req_t *r = &v->reqs[i];
        if (r->state == R_WAITING && (pick == NULL || r->id < pick->id)) {
            pick = r;
        }
    }
    return pick;
}

void apps_vmio_tick(apps_rt_t *rt, int64_t now_ms, bool active) {
    apps_vmio_t *v = apps_rt_vmio(rt);
    if (v == NULL || apps_rt_state(rt) != APPS_STATE_RUNNING) {
        return;
    }
    v->active = active;
    for (int i = 0; i < REQS_MAX; i++) {
        req_t *r = &v->reqs[i];
        if (r->state == R_ACK && now_ms >= r->ack_until) {
            r->state = R_DONE;
            r->done_at = ++v->next_done;
            r->ok = false;
            r->why = APPS_VM_MSG_TIMED_OUT;
        }
    }
    if (!any_acking(v)) {
        v->acks_due = INT64_MAX;
    }
    int watches = watch_count(v);
    if (active && watches > 0) {
        if (!v->beating) {
            v->beating = true;
            v->beat_due = now_ms;  /* once at once */
        }
    } else {
        v->beating = false;
        v->beat_due = INT64_MAX;
    }
    if (v->running) {
        return;
    }
    apps_vm_job_t *job = NULL;
    watch_t *w;
    req_t *r;
    if (v->acks_due <= now_ms) {
        job = new_job(v, K_ACKS, APPS_VM_READ, ACKS_PATH, APPS_VM_JSON_MAX);
        v->acks_due = job != NULL ? INT64_MAX : now_ms + APPS_VM_ACK_POLL_MS;  /* set again when it comes back */
    } else if (v->beating && v->beat_due <= now_ms) {
        job = beat_job(v, watches);
        v->beat_due = now_ms + APPS_VM_HEARTBEAT_MS;
    } else if (active && (w = due_watch(v, now_ms)) != NULL) {
        job = new_job(v, K_WATCH, APPS_VM_READ, w->rel, APPS_VM_WATCH_MAX);
        if (job != NULL) {
            job->req = w->gen;
            job->slot = (uint8_t)(w - v->watches);
        } else {
            w->due = now_ms + (int64_t)w->every_s * 1000;
        }
    } else if ((r = oldest_waiting(v)) != NULL) {
        job = r->job;
        r->job = NULL;
        r->state = R_RUNNING;
    }
    if (job != NULL) {
        send(v, job, now_ms);
    }
}

static bool has_work(const apps_vmio_t *v) {
    if (v->early_head < v->early_tail) {
        return true;
    }
    for (int i = 0; i < REQS_MAX; i++) {
        if (v->reqs[i].state == R_DONE) {
            return true;
        }
    }
    for (int i = 0; i < APPS_VM_WATCHES_MAX; i++) {
        if (v->watches[i].ready != NULL) {
            return true;
        }
    }
    return false;
}

bool apps_vmio_has_work(const apps_rt_t *rt) {
    const apps_vmio_t *v = apps_rt_vmio(rt);
    return v != NULL && apps_rt_state(rt) == APPS_STATE_RUNNING && has_work(v);
}

static int64_t sooner(int64_t a, int64_t b) {
    return a < b ? a : b;
}

int64_t apps_vmio_next_deadline(const apps_rt_t *rt) {
    const apps_vmio_t *v = apps_rt_vmio(rt);
    if (v == NULL || apps_rt_state(rt) != APPS_STATE_RUNNING) {
        return INT64_MAX;
    }
    if (has_work(v)) {
        return 0;
    }
    int64_t due = INT64_MAX;
    bool waiting = false;
    for (int i = 0; i < REQS_MAX; i++) {
        due = v->reqs[i].state == R_ACK ? sooner(due, v->reqs[i].ack_until) : due;
        waiting = waiting || v->reqs[i].state == R_WAITING;
    }
    if (!v->running) {
        if (waiting) {
            return 0;
        }
        due = sooner(due, v->acks_due);
        due = v->beating ? sooner(due, v->beat_due) : due;
        for (int i = 0; v->active && i < APPS_VM_WATCHES_MAX; i++) {
            due = v->watches[i].gen != 0 ? sooner(due, v->watches[i].due) : due;
        }
    }
    return due;
}

/* ---- Callbacks ---- */

typedef enum { D_EARLY, D_REQUEST, D_WATCH } deliver_what_t;

/* One callback to run: what the trampoline needs, and what is left to free after it. */
typedef struct {
    apps_vmio_t *v;
    uint8_t what;           /* deliver_what_t */
    uint8_t kind;           /* a request's */
    uint32_t id;            /* a request's */
    int slot;               /* a watch's */
    lua_Integer early;      /* an early answer's place */
    apps_vm_job_t *job;     /* the result, or NULL */
    uint8_t *image;         /* a picture's block, until apps_rt_image_add takes it */
    size_t image_len;
    bool ok;                /* a command answered without a job */
    const char *why;
    char *result;
    size_t result_len;
} deliver_t;

/* A read's value (P14): JSON decoded here, text as it is, a picture added to the runtime's images (P2). */
static int push_read(lua_State *L, deliver_t *d) {
    const apps_vm_job_t *job = d->job;
    const char *rel = job->path + job->rel;
    const char *bytes = job->data != NULL ? (const char *)job->data : "";
    char err[APPS_VM_ERR_MAX];
    switch (read_type(rel)) {
    case T_JSON:
        if (!apps_json_push(L, bytes, job->len, err, sizeof err)) {
            lua_pushnil(L);
            lua_pushfstring(L, APPS_VM_MSG_NOT_JSON, rel, err);
            return 2;
        }
        return 1;
    case T_TXT:
        lua_pushlstring(L, bytes, job->len);
        return 1;
    default: {
        uint8_t *block = d->image;
        d->image = NULL;  /* the runtime's from here, whatever comes of the add */
        int slot = apps_rt_image_add(d->v->rt, block, d->image_len, err, sizeof err);
        if (slot < 0) {
            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }
        apps_rt_image_push(L, d->v->rt, slot);
        return 1;
    }
    }
}

static int push_request(lua_State *L, deliver_t *d) {
    if (d->job != NULL && !d->job->ok) {
        if (d->kind == K_READ) {
            lua_pushnil(L);
        } else {
            lua_pushboolean(L, 0);
        }
        lua_pushstring(L, d->job->err);
        return 2;
    }
    if (d->kind == K_READ) {
        return push_read(L, d);
    }
    if (d->kind == K_WRITE) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!d->ok) {  /* a command that timed out, or whose result had no room */
        lua_pushboolean(L, 0);
        lua_pushstring(L, d->why);
        return 2;
    }
    lua_pushboolean(L, 1);
    char err[APPS_VM_ERR_MAX];
    if (d->result == NULL || !apps_json_push(L, d->result, d->result_len, err, sizeof err)) {
        lua_pushnil(L);  /* no result (it was checked as it was read, so it decodes) */
    }
    return 2;
}

/* Runs under apps_vm_call (apps_rt_callback): the callback, and what it gets. */
static int deliver_trampoline(lua_State *L) {
    deliver_t *d = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d->v->ref);  /* 2: the callbacks */
    int nargs;
    if (d->what == D_EARLY) {
        lua_rawgeti(L, 2, 0);  /* 3: the early answers */
        lua_rawgeti(L, 3, 2 * d->early - 1);
        lua_rawgeti(L, 3, 2 * d->early);
        lua_Integer code = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, 3, 2 * d->early - 1);
        lua_pushnil(L);
        lua_rawseti(L, 3, 2 * d->early);
        if ((code & 1) != 0) {
            lua_pushboolean(L, 0);
        } else {
            lua_pushnil(L);
        }
        lua_Integer msg = code >> 1;
        lua_pushstring(L, EARLY[msg >= 0 && msg <= EARLY_NO_MEMORY ? msg : EARLY_NO_MEMORY]);
        nargs = 2;
    } else if (d->what == D_REQUEST) {
        lua_rawgeti(L, 2, (lua_Integer)d->id);
        lua_pushnil(L);
        lua_rawseti(L, 2, (lua_Integer)d->id);  /* a request calls back once */
        nargs = push_request(L, d);
    } else {
        lua_rawgeti(L, 2, -(lua_Integer)(d->slot + 1));
        if (d->job->ok) {
            lua_pushlstring(L, d->job->data != NULL ? (const char *)d->job->data : "", d->job->len);
            nargs = 1;
        } else {
            lua_pushnil(L);
            lua_pushstring(L, d->job->err);
            nargs = 2;
        }
    }
    lua_call(L, nargs, 0);
    return 0;
}

/* The callback due first: an early answer, then the result that came first. Taken from the books before the call, which may change
 * them (a new request, a cancel) or end them (a crash releases them). */
static bool pick(apps_vmio_t *v, deliver_t *d) {
    *d = (deliver_t){.v = v};
    if (v->early_head < v->early_tail) {
        d->what = D_EARLY;
        d->early = v->early_head++;
        if (v->early_head == v->early_tail) {
            v->early_head = v->early_tail = 1;  /* the answers' places start again */
        }
        return true;
    }
    req_t *r = NULL;
    watch_t *w = NULL;
    for (int i = 0; i < REQS_MAX; i++) {
        if (v->reqs[i].state == R_DONE && (r == NULL || v->reqs[i].done_at < r->done_at)) {
            r = &v->reqs[i];
        }
    }
    for (int i = 0; i < APPS_VM_WATCHES_MAX; i++) {
        if (v->watches[i].ready != NULL && (w == NULL || v->watches[i].done_at < w->done_at)) {
            w = &v->watches[i];
        }
    }
    if (r != NULL && (w == NULL || r->done_at < w->done_at)) {
        d->what = D_REQUEST;
        d->kind = r->kind;
        d->id = r->id;
        d->job = r->job;
        d->ok = r->ok;
        d->why = r->why;
        d->result = r->result;
        d->result_len = r->result_len;
        *r = (req_t){0};
    } else if (w != NULL) {
        d->what = D_WATCH;
        d->slot = (int)(w - v->watches);
        d->job = w->ready;
        w->ready = NULL;
    } else {
        return false;
    }
    if (d->what == D_REQUEST && d->kind == K_READ && d->job != NULL && d->job->ok) {
        read_type_t type = read_type(d->job->path + d->job->rel);
        if (type == T_JPEG || type == T_BIN) {
            d->image = d->job->data;
            d->image_len = d->job->len;
            d->job->data = NULL;
        }
    }
    return true;
}

bool apps_vmio_run_one(apps_rt_t *rt) {
    apps_vmio_t *v = apps_rt_vmio(rt);
    deliver_t d;
    if (v == NULL || apps_rt_state(rt) != APPS_STATE_RUNNING || v->ref == LUA_NOREF || !pick(v, &d)) {
        return false;
    }
    (void)apps_rt_callback(rt, deliver_trampoline, &d);  /* a failure has crashed the app, and released the books */
    apps_port_free(d.image);
    apps_vm_job_free(d.job);
    apps_port_free(d.result);
    return true;
}

/* ---- The bindings ---- */

static apps_rt_t *rt_of(lua_State *L) {
    return lua_touserdata(L, lua_upvalueindex(1));
}

static void need_vm(lua_State *L) {
    if (!apps_rt_permits(rt_of(L), APPS_PERM_VM)) {
        luaL_error(L, "vm: add \"vm\" to \"permissions\" in manifest.json");
    }
}

static bool whole(lua_State *L, int idx, lua_Integer *out) {
    int isnum = 0;
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return false;
    }
    *out = lua_tointegerx(L, idx, &isnum);
    return isnum != 0;
}

/* The path at idx, by the package's rules: fn names the call in the message. */
static const char *check_path(lua_State *L, int idx, const char *fn) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        luaL_error(L, "%s: path must be a string, got %s", fn, luaL_typename(L, idx));
    }
    size_t len;
    const char *path = lua_tolstring(L, idx, &len);
    if (strlen(path) != len || !apps_path_valid(path)) {
        char shown[APPS_PATH_MAX + 4];
        apps_text_shown(path, len, APPS_PATH_MAX, shown);
        luaL_error(L, "%s: path '%s' is not an allowed path " PATH_RULE, fn, shown);
    }
    return path;
}

static void check_cb(lua_State *L, int idx, const char *fn) {
    if (lua_type(L, idx) != LUA_TFUNCTION) {
        luaL_error(L, "%s: cb must be a function, got %s", fn, luaL_typename(L, idx));
    }
}

/* Pushes the callbacks' table, made with its early answers' table the first time (in the app's heap: a refusal raises). */
static void push_callbacks(lua_State *L, apps_vmio_t *v) {
    if (v->ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, v->ref);
        return;
    }
    lua_createtable(L, 0, 4);
    lua_createtable(L, 4, 0);
    lua_rawseti(L, -2, 0);
    lua_pushvalue(L, -1);
    v->ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* A request in the making: its callback (cb, an absolute index; 0 for none) is in the table under the id it will take. */
typedef struct {
    lua_State *L;
    apps_vmio_t *v;
    int cb;
    uint32_t id;
    bool false_first;  /* a write's or a command's callback gets false, not nil, with an error */
} making_t;

static void making_begin(making_t *m, lua_State *L, int cb, bool false_first) {
    m->L = L;
    m->v = apps_rt_vmio(rt_of(L));
    m->cb = cb != 0 ? lua_absindex(L, cb) : 0;
    m->false_first = false_first;
    m->id = m->v->next_id + 1 != 0 ? m->v->next_id + 1 : 1;
    push_callbacks(L, m->v);
    if (m->cb != 0) {
        lua_pushvalue(L, m->cb);
        lua_rawseti(L, -2, (lua_Integer)m->id);
    }
    lua_pop(L, 1);
}

/* It doesn't go: its callback leaves the table (a nil allocates nothing). */
static void making_drop(making_t *m) {
    if (m->cb != 0) {
        lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->v->ref);
        lua_pushnil(m->L);
        lua_rawseti(m->L, -2, (lua_Integer)m->id);
        lua_pop(m->L, 1);
    }
}

/* It is refused: its callback gets the answer later, the next callbacks to run (P10). */
static void making_refuse(making_t *m, int msg) {
    making_drop(m);
    if (m->cb == 0) {
        return;
    }
    apps_vmio_t *v = m->v;
    lua_State *L = m->L;
    push_callbacks(L, v);
    lua_rawgeti(L, -1, 0);
    lua_pushvalue(L, m->cb);
    lua_rawseti(L, -2, 2 * v->early_tail - 1);
    lua_pushinteger(L, 2 * msg + (m->false_first ? 1 : 0));
    lua_rawseti(L, -2, 2 * v->early_tail);
    lua_pop(L, 2);
    v->early_tail++;
}

/* It goes: the record, waiting for its turn at the worker. */
static void making_commit(making_t *m, apps_vm_job_t *job, uint8_t kind, int64_t seq) {
    req_t *r = free_record(m->v);  /* room() said there is one, and nothing between took it */
    m->v->next_id = m->id;
    job->req = m->id;
    *r = (req_t){.id = m->id, .kind = kind, .state = R_WAITING, .has_cb = m->cb != 0, .job = job, .seq = seq};
}

/* vm.read's opts, the table at idx: circle (a boolean) and max (1 to the screen's side). Read raw, and charged by the slots it visits. */
static void read_opts(lua_State *L, int idx, bool *circle, int *max_px) {
    apps_vm_charge(L, apps_vm_table_slots(L, idx) + 1);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        size_t len = 0;
        const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &len) : NULL;
        lua_Integer n = 0;
        if (key != NULL && len == 6 && memcmp(key, "circle", 6) == 0) {
            if (lua_type(L, -1) != LUA_TBOOLEAN) {
                luaL_error(L, "vm.read: circle must be true or false, got %s", luaL_typename(L, -1));
            }
            *circle = lua_toboolean(L, -1) != 0;
        } else if (key != NULL && len == 3 && memcmp(key, "max", 3) == 0) {
            if (!whole(L, -1, &n) || n < 1 || n > APPS_RT_IMAGE_SIDE_MAX) {
                luaL_error(L, "vm.read: max must be a whole number from 1 to " STR(APPS_RT_IMAGE_SIDE_MAX));
            }
            *max_px = (int)n;
        } else {
            char shown[SHOWN_KEY_MAX + 4];
            if (key != NULL) {
                apps_text_shown(key, len, SHOWN_KEY_MAX, shown);
            } else {
                snprintf(shown, sizeof shown, "<%s>", luaL_typename(L, -2));
            }
            luaL_error(L, "vm.read: unknown option '%s' (valid: circle, max)", shown);
        }
        lua_pop(L, 1);  /* the value; the key stays for the next step */
    }
}

/* vm.read(path, [opts,] cb). */
static int vm_read(lua_State *L) {
    apps_vm_check(L);
    need_vm(L);
    const char *path = check_path(L, 1, "vm.read");
    read_type_t type = read_type(path);
    if (type == T_BAD) {
        luaL_error(L, "vm.read: path must end in .json, .txt, .jpg, .jpeg or .bin");
    }
    bool circle = false;
    int max_px = APPS_RT_IMAGE_SIDE_MAX;
    int cb = 2;
    if (lua_type(L, 2) == LUA_TTABLE) {
        read_opts(L, 2, &circle, &max_px);
        cb = 3;
    } else if (lua_isnil(L, 2) && lua_gettop(L) >= 3) {
        cb = 3;
    } else if (!lua_isnoneornil(L, 2) && lua_type(L, 2) != LUA_TFUNCTION) {
        luaL_error(L, "vm.read: opts must be a table like " OPTS_EXAMPLE ", got %s", luaL_typename(L, 2));
    }
    check_cb(L, cb, "vm.read");
    making_t m;
    making_begin(&m, L, cb, false);
    if (!room(m.v)) {
        making_refuse(&m, EARLY_TOO_MANY);
        return 0;
    }
    static const uint32_t MAX[] = {[T_JSON] = APPS_VM_JSON_MAX, [T_TXT] = APPS_VM_TXT_MAX, [T_JPEG] = APPS_VM_JPEG_MAX,
                                   [T_BIN] = APPS_VM_BIN_MAX};
    apps_vm_job_t *job = new_job(m.v, K_READ, APPS_VM_READ, path, MAX[type]);
    if (job == NULL) {
        making_refuse(&m, EARLY_NO_MEMORY);
        return 0;
    }
    job->as = type == T_JPEG ? APPS_VM_AS_JPEG : APPS_VM_AS_BYTES;
    job->circle = circle;
    job->max_px = (uint16_t)max_px;
    making_commit(&m, job, K_READ, 0);
    return 0;
}

/* vm.write(path, value, [cb]). */
static int vm_write(lua_State *L) {
    apps_vm_check(L);
    need_vm(L);
    const char *path = check_path(L, 1, "vm.write");
    if (strncmp(path, OUTBOX, strlen(OUTBOX)) != 0) {
        luaL_error(L, "vm.write: " APPS_VM_MSG_OUTBOX);
    }
    lua_settop(L, 3);  /* a value left out is nil: null */
    if (!lua_isnil(L, 3)) {
        check_cb(L, 3, "vm.write");
    }
    making_t m;
    making_begin(&m, L, lua_isnil(L, 3) ? 0 : 3, true);
    size_t len = 0;
    char err[96] = "";
    char *text = apps_json_encode_value_max(L, 2, apps_port_realloc, APPS_VM_WRITE_MAX, &len, err, sizeof err);
    if (text == NULL) {
        if (strcmp(err, "out of memory") == 0) {  /* the floor refused the text's block */
            making_refuse(&m, EARLY_NO_MEMORY);
            return 0;
        }
        making_drop(&m);
        if (strcmp(err, APPS_JSON_TOO_LARGE) == 0) {
            return luaL_error(L, "vm.write: value is larger than %d KB as JSON", APPS_VM_WRITE_MAX / 1024);
        }
        return luaL_error(L, "vm.write: value can't be encoded: %s", err);
    }
    bool has_room = room(m.v);
    apps_vm_job_t *job = has_room ? new_job(m.v, K_WRITE, APPS_VM_WRITE, path, 0) : NULL;
    if (job == NULL) {
        apps_port_free(text);
        making_refuse(&m, has_room ? EARLY_NO_MEMORY : EARLY_TOO_MANY);
        return 0;
    }
    job->data = (uint8_t *)text;
    job->len = len;
    making_commit(&m, job, K_WRITE, 0);
    return 0;
}

static bool name_valid(const char *name, size_t len) {
    if (name == NULL || len < 1 || len > APPS_VM_NAME_MAX || strlen(name) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* vm.command(name, args, [cb]). */
static int vm_command(lua_State *L) {
    apps_vm_check(L);
    need_vm(L);
    size_t name_len = 0;
    const char *name = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &name_len) : NULL;
    if (!name_valid(name, name_len)) {
        luaL_error(L, "vm.command: name must be 1-" STR(APPS_VM_NAME_MAX) " characters of a-z, 0-9 and _");
    }
    lua_settop(L, 3);
    if (!lua_isnil(L, 3)) {
        check_cb(L, 3, "vm.command");
    }
    int64_t epoch_ms;
    if (!apps_port_epoch_ms(&epoch_ms)) {
        luaL_error(L, "vm.command: " APPS_VM_MSG_NO_CLOCK);
    }
    making_t m;
    making_begin(&m, L, lua_isnil(L, 3) ? 0 : 3, true);
    size_t args_len = 0;
    char err[96] = "";
    char *args = apps_json_encode_value_max(L, 2, apps_port_realloc, APPS_VM_ARGS_MAX, &args_len, err, sizeof err);
    if (args == NULL) {
        if (strcmp(err, "out of memory") == 0) {
            making_refuse(&m, EARLY_NO_MEMORY);
            return 0;
        }
        making_drop(&m);
        if (strcmp(err, APPS_JSON_TOO_LARGE) == 0) {
            return luaL_error(L, "vm.command: args are larger than %d KB as JSON", APPS_VM_ARGS_MAX / 1024);
        }
        return luaL_error(L, "vm.command: args can't be encoded: %s", err);
    }
    if (!room(m.v)) {
        apps_port_free(args);
        making_refuse(&m, EARLY_TOO_MANY);
        return 0;
    }
    /* seq: the wall clock's ms, or one more than the last if that is later (two in a ms, or a clock set back). */
    int64_t seq = m.v->last_seq + 1 > epoch_ms ? m.v->last_seq + 1 : epoch_ms;
    char rel[APPS_PATH_MAX + 1];
    snprintf(rel, sizeof rel, OUTBOX "cmd-%lld.json", (long long)seq);
    size_t cap = args_len + name_len + 64;
    apps_vm_job_t *job = new_job(m.v, K_COMMAND, APPS_VM_WRITE, rel, 0);
    char *text = job != NULL ? apps_port_realloc(NULL, cap) : NULL;
    if (text == NULL) {
        apps_port_free(args);
        apps_vm_job_free(job);
        making_refuse(&m, EARLY_NO_MEMORY);
        return 0;
    }
    int n = snprintf(text, cap, "{\"seq\":%lld,\"name\":\"%.*s\",\"args\":%s}", (long long)seq, (int)name_len, name, args);
    apps_port_free(args);
    job->data = (uint8_t *)text;
    job->len = (size_t)n;
    m.v->last_seq = seq;
    making_commit(&m, job, K_COMMAND, seq);
    return 0;
}

/* vm.watch(path, [every_s,] cb): the handle. */
static int vm_watch(lua_State *L) {
    apps_vm_check(L);
    need_vm(L);
    const char *path = check_path(L, 1, "vm.watch");
    lua_Integer every = APPS_VM_EVERY_DEFAULT_S;
    int cb = 3;
    if (lua_type(L, 2) == LUA_TFUNCTION) {
        cb = 2;
    } else if (!lua_isnoneornil(L, 2) &&
               (!whole(L, 2, &every) || every < APPS_VM_EVERY_MIN_S || every > APPS_VM_EVERY_MAX_S)) {
        luaL_error(L, "vm.watch: every_s must be a whole number from " STR(APPS_VM_EVERY_MIN_S) " to " STR(APPS_VM_EVERY_MAX_S));
    }
    check_cb(L, cb, "vm.watch");
    apps_vmio_t *v = apps_rt_vmio(rt_of(L));
    int slot = -1;
    for (int i = 0; i < APPS_VM_WATCHES_MAX && slot < 0; i++) {
        slot = v->watches[i].gen == 0 ? i : -1;
    }
    if (slot < 0) {
        luaL_error(L, "vm.watch: no more than " STR(APPS_VM_WATCHES_MAX) " watches per app");
    }
    /* The handle and the callback's place first (the app's heap may refuse); the watch is taken last, which can't fail. */
    watch_handle_t *h = lua_newuserdatauv(L, sizeof *h, 0);
    luaL_setmetatable(L, APPS_WATCH_MT);
    push_callbacks(L, v);
    lua_pushvalue(L, cb);
    lua_rawseti(L, -2, -(lua_Integer)(slot + 1));
    lua_pop(L, 1);
    if (++v->watch_gen == 0) {
        v->watch_gen = 1;  /* never 0: a free slot */
    }
    watch_t *w = &v->watches[slot];
    memset(w, 0, sizeof *w);
    w->gen = v->watch_gen;
    w->every_s = (uint8_t)every;
    w->due = apps_port_mono_ms();  /* the first read at once, while the app is in front and lit */
    snprintf(w->rel, sizeof w->rel, "%s", path);
    h->gen = w->gen;
    h->slot = (uint8_t)slot;
    return 1;
}

/* h:cancel(): nothing for a watch that is gone. A read of it at the worker comes back to nobody. */
static int watch_cancel(lua_State *L) {
    apps_vm_check(L);
    const watch_handle_t *h = luaL_testudata(L, 1, APPS_WATCH_MT);
    if (h == NULL) {
        return luaL_error(L, "watch: cancel needs a watch: call it as h:cancel()");
    }
    apps_vmio_t *v = apps_rt_vmio(rt_of(L));
    watch_t *w = h->slot < APPS_VM_WATCHES_MAX ? &v->watches[h->slot] : NULL;
    if (w == NULL || w->gen == 0 || w->gen != h->gen) {
        return 0;
    }
    if (v->ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, v->ref);
        lua_pushnil(L);
        lua_rawseti(L, -2, -(lua_Integer)(h->slot + 1));
        lua_pop(L, 1);
    }
    apps_vm_job_free(w->ready);
    memset(w, 0, sizeof *w);
    return 0;
}

/* h.name: a method, or an error that lists them. */
static int watch_index(lua_State *L) {
    apps_vm_check(L);
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushvalue(L, 2);
        if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TNIL) {  /* upvalue 1: the methods */
            return 1;
        }
    }
    char shown[SHOWN_KEY_MAX + 4];
    if (lua_type(L, 2) == LUA_TSTRING) {
        size_t len;
        const char *name = lua_tolstring(L, 2, &len);
        apps_text_shown(name, len, SHOWN_KEY_MAX, shown);
    } else {
        snprintf(shown, sizeof shown, "<%s>", luaL_typename(L, 2));
    }
    return luaL_error(L, "watch: unknown method '%s' (valid: cancel)", shown);
}

static int watch_newindex(lua_State *L) {
    apps_vm_check(L);
    return luaL_error(L, "watch: a watch has no fields: h:cancel() stops it");
}

static int watch_tostring(lua_State *L) {
    apps_vm_check(L);
    const watch_handle_t *h = luaL_testudata(L, 1, APPS_WATCH_MT);
    if (h == NULL) {
        return luaL_error(L, "watch: tostring needs a watch");
    }
    lua_pushfstring(L, "watch#%d", (int)h->slot);
    return 1;
}

static const luaL_Reg apps_vm_funcs[] = {
    {"read", vm_read},
    {"write", vm_write},
    {"command", vm_command},
    {"watch", vm_watch},
    {NULL, NULL},
};

static const luaL_Reg apps_watch_methods[] = {
    {"cancel", watch_cancel},
    {NULL, NULL},
};

void apps_vmio_open(lua_State *L, apps_rt_t *rt) {
    /* Protected: getmetatable gives the name, never the table (X17: a finalizer put on it would run with the hooks off). */
    luaL_newmetatable(L, APPS_WATCH_MT);
    lua_pushliteral(L, APPS_WATCH_MT);
    lua_setfield(L, -2, "__metatable");
    lua_createtable(L, 0, 1);  /* the methods */
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_watch_methods, 1);
    lua_pushcclosure(L, watch_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, watch_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, watch_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    lua_createtable(L, 0, 4);
    lua_pushlightuserdata(L, rt);
    luaL_setfuncs(L, apps_vm_funcs, 1);
    lua_setglobal(L, "vm");
}
