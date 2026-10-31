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

#include "apps_icons.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_store.h"

/*
 * The table (apps_icons.h has the contract). APPS_ICONS_SLOTS slots, each EMPTY, LOADING, READY or FAILED, with the app and icon it
 * is for and the generation of the want that asked: plain data of the LVGL task's, in PSRAM. A LOADING slot is lent to its job,
 * which reads only its id and icon and writes only its result (block, len, fail). Slot k's hand-off is s_hand[k], the one word the
 * two tasks share, an internal static (S3 PSRAM has no read-modify-write, so no atomic is in the table): LENT while the job has the
 * slot; the job swaps LENT for DONE once its result is written, and a drop swaps LENT for ORPHANED. Whichever loses the swap frees
 * the block: a job that finds ORPHANED frees its own and then stores DONE; a drop that finds DONE frees it there. Until DONE the
 * LVGL task reads nothing of the slot and never empties it, so a job can't meet a slot used again; after DONE the job doesn't touch
 * it. So a drop frees every block, wherever it lands in a load, and no poll is needed after it (the launcher has closed).
 *
 * Bounds. READY and FAILED slots are only for wanted (id, icon) pairs, one slot each (a want frees the others at once, and a load
 * that ends unwanted is freed, not adopted), so there are at most APPS_ICONS_WINDOW of them; LOADING slots are at most
 * APPS_ICONS_IN_FLIGHT, as nothing is asked for while that many are out. So the table never fills (the assertion), and the blocks
 * held are at most APPS_ICONS_SLOTS.
 *
 * A drop. The generation goes up; every load still lent is orphaned, and every other slot is emptied with its block. A LOADING slot
 * of an older generation matches no want: an app asked for again after a drop gets a load of its own, and the old one, once its job
 * has stored DONE, is emptied (with no block: its job freed it).
 *
 * Failures. A file that isn't an icon, or can't be read, is FAILED: its tile, not read again while it stays in the window. A block
 * the floor or the heap refused (the store's APPS_STORE_MSG_NO_MEMORY) is a shortage that passes: its slot is emptied, and no load is
 * asked for until APPS_ICONS_RETRY_MS later, when it is asked for again if it is still wanted.
 */

static const char *TAG = "muse_apps";

#define IMAGE_HEADER 12  /* an LVGL v9 .bin: lv_image_header_t, then the pixels */

_Static_assert(sizeof(lv_image_header_t) == IMAGE_HEADER, "a .bin's header is an lv_image_header_t");
_Static_assert(APPS_ICONS_SLOTS <= 2 * APPS_ICONS_WINDOW, "the table is at most two windows");
_Static_assert(APPS_ICONS_SLOTS >= APPS_ICONS_WINDOW + APPS_ICONS_IN_FLIGHT, "a slot for each wanted app and each load in flight");

enum { EMPTY, LOADING, READY, FAILED };      /* a slot's state: the LVGL task's */
enum { HAND_FREE, HAND_LENT, HAND_DONE, HAND_ORPHANED };  /* s_hand[k] */
enum { FAIL_NONE, FAIL_FILE, FAIL_MEMORY };  /* why a load failed: a tile while it stays, or asked for again after the wait */

typedef struct {
    char id[APPS_ID_MAX + 1];
    char icon[APPS_PATH_MAX + 1];
    uint32_t gen;       /* the generation of the want that asked (the LVGL task's) */
    uint8_t state;      /* the LVGL task's */
    uint8_t fail;       /* the job's: FAIL_NONE with a block, or why it has none */
    uint8_t *block;     /* the file, from the job; READY: what dsc points into */
    size_t len;
    lv_image_dsc_t dsc; /* READY */
} slot_t;

typedef struct {
    char id[APPS_ID_MAX + 1];
    char icon[APPS_PATH_MAX + 1];
} wanted_t;

/* X12: the LVGL task's plain data, 1,432 bytes on the gadget (7 slots of 136, the window), in PSRAM; zeroed at boot. */
static EXT_RAM_BSS_ATTR struct {
    slot_t slot[APPS_ICONS_SLOTS];
    wanted_t want[APPS_ICONS_WINDOW];  /* the window, centre first */
    int want_n;
    uint32_t gen;       /* a drop's: the jobs never read it */
    int64_t retry_at;   /* no load is asked for before this (the worker or the floor refused one) */
    bool news;          /* an icon became ready since the last poll */
} S;

/* The slots' hand-off words: shared by the LVGL task and the jobs, and swapped, so in internal RAM (28 bytes), as the apps' atomics
 * are. */
static atomic_uint s_hand[APPS_ICONS_SLOTS];

/* ---- The I/O worker's ---- */

static void load_job(void *arg) {
    slot_t *s = arg;
    atomic_uint *hand = &s_hand[s - S.slot];
    uint8_t *block = NULL;
    size_t len = 0;
    uint8_t fail = FAIL_NONE;
    char why[96];
    if (!apps_store_read_file(s->id, s->icon, APPS_ICON_MAX_BYTES, &block, &len, why, sizeof why)) {
        fail = strcmp(why, APPS_STORE_MSG_NO_MEMORY) == 0 ? FAIL_MEMORY : FAIL_FILE;
    } else if (!apps_icon_check(block, len, why, sizeof why)) {
        fail = FAIL_FILE;
        apps_port_free(block);
        block = NULL;
    }
    if (fail != FAIL_NONE) {
        ESP_LOGW(TAG, "icons: %s: %s: %s; its tile shows", s->id, s->icon, why);
    }
    s->block = block;
    s->len = len;
    s->fail = fail;
    unsigned lent = HAND_LENT;
    if (!atomic_compare_exchange_strong(hand, &lent, HAND_DONE)) {
        /* Orphaned: a drop came while it loaded, and the block is this job's to free. Never shown. */
        apps_port_free(block);
        s->block = NULL;
        atomic_store(hand, HAND_DONE);
    }
    /* From here the slot is the LVGL task's. */
}

/* ---- The LVGL task's ---- */

static bool wanted(const char *id, const char *icon) {
    for (int i = 0; i < S.want_n; i++) {
        if (strcmp(S.want[i].id, id) == 0 && strcmp(S.want[i].icon, icon) == 0) {
            return true;
        }
    }
    return false;
}

static bool in_window(const char *id) {
    for (int i = 0; i < S.want_n; i++) {
        if (strcmp(S.want[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

/* The slot of this generation for the pair, loading, ready or failed; NULL when there is none. */
static slot_t *find(const char *id, const char *icon) {
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state != EMPTY && s->gen == S.gen && strcmp(s->id, id) == 0 && strcmp(s->icon, icon) == 0) {
            return s;
        }
    }
    return NULL;
}

static atomic_uint *hand_of(const slot_t *s) {
    return &s_hand[s - S.slot];
}

/* A slot the LVGL task holds (not one lent to a job) emptied, its block freed. */
static void release(slot_t *s) {
    apps_port_free(s->block);
    s->block = NULL;
    s->len = 0;
    s->fail = FAIL_NONE;
    memset(&s->dsc, 0, sizeof s->dsc);
    s->state = EMPTY;
    atomic_store(hand_of(s), HAND_FREE);
}

/* A LOADING slot whose job hasn't stored DONE: the job's still (LENT or ORPHANED). */
static bool lent(const slot_t *s) {
    return s->state == LOADING && atomic_load(hand_of(s)) != HAND_DONE;
}

/* The descriptor of a ready block, as apps_lv.c makes an image's (APPS_W_IMAGE): the header is the file's first bytes, the pixels the
 * rest, in place. apps_icon_check passed the block, so it holds the header and the bytes it says. */
static void describe(slot_t *s) {
    memcpy(&s->dsc.header, s->block, sizeof s->dsc.header);
    /* Only premultiplied alpha means anything to an image drawn in place; the others say what the memory is (apps_lv.c). */
    s->dsc.header.flags &= LV_IMAGE_FLAGS_PREMULTIPLIED;
    s->dsc.data_size = (uint32_t)(s->len - IMAGE_HEADER);
    s->dsc.data = s->block + IMAGE_HEADER;
    s->dsc.reserved = NULL;
    s->dsc.reserved_2 = NULL;
}

/* The loads that ended: a wanted one of this generation becomes READY, FAILED for a bad file (not asked for again while it stays),
 * or, refused for want of memory, EMPTY again with no load asked for until APPS_ICONS_RETRY_MS later; any other is emptied, its block
 * freed. */
static void collect(void) {
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state != LOADING || lent(s)) {
            continue;
        }
        if (s->gen != S.gen || !wanted(s->id, s->icon)) {
            release(s);
        } else if (s->fail == FAIL_NONE && s->block != NULL) {
            describe(s);
            s->state = READY;
            S.news = true;
        } else if (s->fail == FAIL_MEMORY) {
            release(s);
            S.retry_at = apps_port_mono_ms() + APPS_ICONS_RETRY_MS;
        } else {
            release(s);
            s->state = FAILED;
        }
    }
}

/* Asks the worker for the window's icons that have no slot, in the window's order, while fewer than APPS_ICONS_IN_FLIGHT are out. */
static void ask(void) {
    if (apps_store_mode() != APPS_STORAGE_FLASH) {
        return;  /* RAM mode keeps no files, and before the mount there are none: tiles */
    }
    int in_flight = 0;
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        in_flight += S.slot[i].state == LOADING ? 1 : 0;
    }
    const int64_t now = apps_port_mono_ms();
    if (now < S.retry_at) {
        return;  /* the worker refused one: not at every frame (each refusal is a W line) */
    }
    for (int i = 0; i < S.want_n && in_flight < APPS_ICONS_IN_FLIGHT; i++) {
        const wanted_t *w = &S.want[i];
        if (find(w->id, w->icon) != NULL) {
            continue;
        }
        slot_t *s = NULL;
        for (int k = 0; k < APPS_ICONS_SLOTS && s == NULL; k++) {
            s = S.slot[k].state == EMPTY ? &S.slot[k] : NULL;
        }
        if (s == NULL) {
            return;  /* can't be: the table has a slot for each wanted app and each load in flight */
        }
        memcpy(s->id, w->id, sizeof s->id);
        memcpy(s->icon, w->icon, sizeof s->icon);
        s->gen = S.gen;
        s->block = NULL;
        s->len = 0;
        s->fail = FAIL_NONE;
        atomic_store(hand_of(s), HAND_LENT);  /* before the submit, whose lock hands the slot to the job */
        s->state = LOADING;
        if (!apps_io_submit(load_job, s)) {  /* it says why, in one W line */
            atomic_store(hand_of(s), HAND_FREE);
            s->state = EMPTY;
            S.retry_at = now + APPS_ICONS_RETRY_MS;
            return;
        }
        in_flight++;
    }
}

void apps_icons_want(const char *const *ids, const char *const *icons, int n) {
    S.want_n = 0;
    for (int i = 0; ids != NULL && icons != NULL && i < n && i < APPS_ICONS_WINDOW; i++) {
        const char *id = ids[i], *icon = icons[i];
        if (!apps_id_valid(id) || icon == NULL || !apps_path_valid(icon) || in_window(id)) {
            continue;  /* apps_path_valid refuses "": an app with no icon is its tile */
        }
        wanted_t *w = &S.want[S.want_n++];
        memcpy(w->id, id, strlen(id) + 1);  /* the checks passed: at most APPS_ID_MAX and APPS_PATH_MAX bytes */
        memcpy(w->icon, icon, strlen(icon) + 1);
    }
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if ((s->state == READY || s->state == FAILED) && !wanted(s->id, s->icon)) {
            release(s);  /* it left the window: its block goes at once */
        }
    }
    collect();
    ask();
}

const lv_image_dsc_t *apps_icons_get(const char *id) {
    for (int i = 0; id != NULL && i < APPS_ICONS_SLOTS; i++) {
        const slot_t *s = &S.slot[i];
        if (s->state == READY && strcmp(s->id, id) == 0) {
            return &s->dsc;
        }
    }
    return NULL;
}

bool apps_icons_poll(void) {
    collect();
    ask();
    const bool news = S.news;
    S.news = false;
    return news;
}

void apps_icons_drop_all(void) {
    S.gen++;
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state == EMPTY) {
            continue;
        }
        unsigned lent = HAND_LENT;
        if (s->state == LOADING && atomic_compare_exchange_strong(hand_of(s), &lent, HAND_ORPHANED)) {
            continue;  /* its job frees the block as it ends; the slot is emptied after (an older generation's) */
        }
        if (s->state == LOADING && lent == HAND_ORPHANED) {
            continue;  /* orphaned by an earlier drop, and still loading */
        }
        release(s);  /* ready, failed, or a load that has ended: its block goes now */
    }
    S.want_n = 0;
    S.news = false;
}
