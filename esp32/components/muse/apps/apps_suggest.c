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

#include "apps_suggest.h"

#include <stddef.h>
#include <string.h>

/* strlen(s), or APPS_SUGGEST_MAX_LEN + 1 for anything longer: reads no further than that. */
static size_t bounded_len(const char *s) {
    size_t n = 0;
    while (n <= APPS_SUGGEST_MAX_LEN && s[n] != '\0') {
        n++;
    }
    return n;
}

/* The Levenshtein distance between a and b, both at most APPS_SUGGEST_MAX_LEN bytes, or
 * bound + 1 as soon as it has to be more than bound. Each row takes lb cells from
 * *cells; -1 when they run out. Two rows on the stack: 130 bytes. */
static int edit_distance(const char *a, size_t la, const char *b, size_t lb, int bound, size_t *cells) {
    unsigned char prev[APPS_SUGGEST_MAX_LEN + 1], cur[APPS_SUGGEST_MAX_LEN + 1];
    for (size_t j = 0; j <= lb; j++) {
        prev[j] = (unsigned char)j;
    }
    for (size_t i = 1; i <= la; i++) {
        if (*cells < lb) {
            return -1;
        }
        *cells -= lb;
        cur[0] = (unsigned char)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= lb; j++) {
            int d = prev[j - 1] + (a[i - 1] != b[j - 1]);  /* keep or substitute */
            if (prev[j] + 1 < d) {
                d = prev[j] + 1;  /* delete a[i - 1] */
            }
            if (cur[j - 1] + 1 < d) {
                d = cur[j - 1] + 1;  /* insert b[j - 1] */
            }
            cur[j] = (unsigned char)d;
            if (d < row_min) {
                row_min = d;
            }
        }
        if (row_min > bound) {
            return bound + 1;  /* no row below can come back under it */
        }
        memcpy(prev, cur, lb + 1);
    }
    return prev[lb];
}

const char *apps_suggest(const char *name, const char *const *cands, int n) {
    if (name == NULL || cands == NULL) {
        return NULL;
    }
    size_t len = bounded_len(name);
    if (len == 0 || len > APPS_SUGGEST_MAX_LEN) {
        return NULL;
    }
    /* A suggestion must come in under this: at most 2 edits, and fewer than len. */
    int beat = (len - 1 < 2 ? (int)len - 1 : 2) + 1;
    size_t cells = APPS_SUGGEST_MAX_CELLS;
    const char *best = NULL;
    for (int i = 0; i < n; i++) {
        const char *cand = cands[i];
        if (cand == NULL) {
            continue;
        }
        size_t clen = bounded_len(cand);
        size_t apart = clen > len ? clen - len : len - clen;  /* at least this many edits */
        if (clen > APPS_SUGGEST_MAX_LEN || apart >= (size_t)beat) {
            continue;
        }
        int d = edit_distance(name, len, cand, clen, beat - 1, &cells);
        if (d < 0) {
            return NULL;  /* past the cap: no suggestion rather than unbounded work */
        }
        if (d > 0 && d < beat) {  /* d == 0 is the name itself */
            best = cand;
            beat = d;
        }
    }
    return best;
}
