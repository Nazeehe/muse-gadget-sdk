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
 * is for and the generation of the want that asked. The LVGL task owns the table; a LOADING slot is lent to its job, which reads
 * only its id, icon and gen and writes only its result (block, len, ok), then done, last, with a store the LVGL task's load of done
 * pairs with. Until done the LVGL task reads nothing of it but done, and never empties it, so the job can't meet a slot used again;
 * after done the job doesn't touch it. done and gen are words with one writer each, loaded and stored whole (PSRAM promises no
 * read-modify-write).
 *
 * Bounds. READY and FAILED slots are only for wanted (id, icon) pairs, one slot each (a want frees the others at once, and a load
 * that ends unwanted is freed, not adopted), so there are at most APPS_ICONS_WINDOW of them; LOADING slots are at most
 * APPS_ICONS_IN_FLIGHT, as nothing is asked for while that many are out. So the table never fills (the assertion), and the blocks
 * held are at most APPS_ICONS_SLOTS.
 *
 * A drop. The generation goes up first, so a load that ends after it frees its own block on the worker; then every slot but a LOADING
 * one not yet done is emptied (a done one is the LVGL task's again, and its block goes with it). A LOADING slot of an older generation
 * matches no want: an app asked for again after a drop gets a load of its own, and the old one, once done, is emptied.
 */

static const char *TAG = "muse_apps";

#define IMAGE_HEADER 12  /* an LVGL v9 .bin: lv_image_header_t, then the pixels */

_Static_assert(sizeof(lv_image_header_t) == IMAGE_HEADER, "a .bin's header is an lv_image_header_t");
_Static_assert(APPS_ICONS_SLOTS <= 2 * APPS_ICONS_WINDOW, "the table is at most two windows");
_Static_assert(APPS_ICONS_SLOTS >= APPS_ICONS_WINDOW + APPS_ICONS_IN_FLIGHT, "a slot for each wanted app and each load in flight");

enum { EMPTY, LOADING, READY, FAILED };

typedef struct {
    char id[APPS_ID_MAX + 1];
    char icon[APPS_PATH_MAX + 1];
    uint32_t gen;       /* the generation of the want that asked */
    uint8_t state;      /* the LVGL task's */
    atomic_uint done;   /* LOADING: the job's last store; the slot is the LVGL task's again */
    uint8_t *block;     /* the file, from the job (NULL when it failed); READY: what dsc points into */
    size_t len;
    bool ok;
    lv_image_dsc_t dsc; /* READY */
} slot_t;

typedef struct {
    char id[APPS_ID_MAX + 1];
    char icon[APPS_PATH_MAX + 1];
} wanted_t;

/* X12: 1,488 bytes on the gadget (7 slots of 144 and the window), in PSRAM; zeroed at boot: every slot EMPTY, generation 0. */
static EXT_RAM_BSS_ATTR struct {
    slot_t slot[APPS_ICONS_SLOTS];
    wanted_t want[APPS_ICONS_WINDOW];  /* the window, centre first */
    int want_n;
    atomic_uint gen;    /* the LVGL task stores it (a drop); the jobs load it */
    int64_t retry_at;   /* no load is asked for before this (the worker refused one) */
    bool news;          /* an icon became ready since the last poll */
} S;

/* ---- The I/O worker's ---- */

static void load_job(void *arg) {
    slot_t *s = arg;
    uint8_t *block = NULL;
    size_t len = 0;
    char why[96];
    bool ok = apps_store_read_file(s->id, s->icon, APPS_ICON_MAX_BYTES, &block, &len, why, sizeof why) &&
              apps_icon_check(block, len, why, sizeof why);
    if (!ok) {
        apps_port_free(block);  /* a file that isn't an icon (the read's own refusals leave none) */
        block = NULL;
        ESP_LOGW(TAG, "icons: %s: %s: %s; its tile shows", s->id, s->icon, why);
    } else if (atomic_load(&S.gen) != s->gen) {
        apps_port_free(block);  /* dropped while it loaded: freed here, never shown */
        block = NULL;
        ok = false;
    }
    s->block = block;
    s->len = len;
    s->ok = ok;
    atomic_store(&s->done, 1u);  /* last: from here the slot is the LVGL task's */
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
    const uint32_t gen = atomic_load(&S.gen);
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state != EMPTY && s->gen == gen && strcmp(s->id, id) == 0 && strcmp(s->icon, icon) == 0) {
            return s;
        }
    }
    return NULL;
}

/* A slot the LVGL task holds (not one lent to a job) emptied, its block freed. */
static void release(slot_t *s) {
    apps_port_free(s->block);
    s->block = NULL;
    s->len = 0;
    s->ok = false;
    memset(&s->dsc, 0, sizeof s->dsc);
    s->state = EMPTY;
}

static bool lent(const slot_t *s) {
    return s->state == LOADING && atomic_load(&s->done) == 0u;
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

/* The loads that ended: a wanted one of this generation becomes READY (or FAILED, so it isn't asked for again while it stays);
 * any other is emptied, its block freed. */
static void collect(void) {
    const uint32_t gen = atomic_load(&S.gen);
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state != LOADING || lent(s)) {
            continue;
        }
        if (s->gen != gen || !wanted(s->id, s->icon)) {
            release(s);
        } else if (s->ok && s->block != NULL) {
            describe(s);
            s->state = READY;
            S.news = true;
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
    const uint32_t gen = atomic_load(&S.gen);
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
        s->gen = gen;
        s->block = NULL;
        s->len = 0;
        s->ok = false;
        atomic_store(&s->done, 0u);
        s->state = LOADING;
        if (!apps_io_submit(load_job, s)) {  /* it says why, in one W line */
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
    atomic_store(&S.gen, atomic_load(&S.gen) + 1u);  /* first: a load that ends from here on frees its own block */
    for (int i = 0; i < APPS_ICONS_SLOTS; i++) {
        slot_t *s = &S.slot[i];
        if (s->state != EMPTY && !lent(s)) {
            release(s);
        }
    }
    S.want_n = 0;
    S.news = false;
}
