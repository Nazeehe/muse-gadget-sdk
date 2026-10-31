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

/* The fake TJpgDec: tests/fake_tjpgd.h has its format and what it gives the output function. */

#include "fake_tjpgd.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD 14
#define POOL_MIN 3100

int fake_tjpgd_scale = -1;
int fake_tjpgd_rects;

/* What jd_prepare read that the session's fields don't hold, kept at the start of the pool, as the ROM keeps its tables there. */
typedef struct {
    BYTE mode;
} fake_session_t;

static UINT le16(const BYTE *p) {
    return (UINT)p[0] | (UINT)p[1] << 8;
}

/* n bytes through the session's input function, in pieces of at most JD_SZBUF as the ROM's buffer takes them; into buf, or skipped
 * when buf is NULL. False when the stream ended first. */
static bool take(JDEC *jd, BYTE *buf, UINT n) {
    while (n > 0) {
        UINT piece = n < JD_SZBUF ? n : JD_SZBUF;
        UINT got = jd->infunc(jd, buf, piece);
        if (got != piece) {
            return false;
        }
        if (buf != NULL) {
            buf += got;
        }
        n -= got;
    }
    return true;
}

JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *pool, UINT sz_pool, void *dev) {
    if (jd == NULL || infunc == NULL || pool == NULL) {
        return JDR_PAR;
    }
    memset(jd, 0, sizeof *jd);
    jd->infunc = infunc;
    jd->device = dev;
    jd->pool = pool;
    jd->sz_pool = sz_pool;
    if (sz_pool < POOL_MIN) {
        return JDR_MEM1;
    }
    memset(pool, 0xA5, sz_pool);  /* every byte of the pool given is the decoder's: a short one is AddressSanitizer's */
    BYTE head[HEAD];
    if (!take(jd, head, HEAD)) {
        return JDR_INP;
    }
    if (memcmp(head, "FJPG", 4) != 0) {
        return JDR_FMT1;
    }
    BYTE mode = head[4];
    if (mode == 'P') {
        return JDR_FMT3;
    }
    if ((mode != 'B' && mode != 'C') || head[5] < 1 || head[5] > 2 || head[6] < 1 || head[6] > 2) {
        return JDR_FMT1;
    }
    UINT width = le16(head + 8), height = le16(head + 10);
    if (width == 0 || height == 0) {
        return JDR_FMT1;
    }
    UINT skip = le16(head + 12);
    if (skip > 0 && !take(jd, NULL, skip)) {
        return JDR_INP;
    }
    jd->msx = head[5];
    jd->msy = head[6];
    jd->width = width;
    jd->height = height;
    ((fake_session_t *)pool)->mode = mode;
    return JDR_OK;
}

/* The picture's pixel at (x, y), full size. */
static void pixel(UINT x, UINT y, BYTE *rgb) {
    rgb[0] = (BYTE)((7 * x + 3 * y) & 255);
    rgb[1] = (BYTE)(((5 * x) ^ (11 * y)) & 255);
    rgb[2] = (BYTE)((x + 13 * y) & 255);
}

JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale) {
    if (jd == NULL || outfunc == NULL || scale > (JD_USE_SCALE ? 3 : 0)) {
        return JDR_PAR;
    }
    fake_tjpgd_scale = scale;
    fake_tjpgd_rects = 0;
    jd->scale = scale;
    BYTE tail[4];
    if (!take(jd, tail, sizeof tail)) {
        return JDR_INP;
    }
    if (memcmp(tail, "END!", 4) != 0) {
        return JDR_FMT1;
    }
    const BYTE mode = ((const fake_session_t *)jd->pool)->mode;
    const UINT mx = jd->msx * 8u, my = jd->msy * 8u;
    for (UINT y = 0; y < jd->height; y += my) {
        for (UINT x = 0; x < jd->width; x += mx) {
            UINT rx = x + mx <= jd->width ? mx : jd->width - x;
            UINT ry = y + my <= jd->height ? my : jd->height - y;
            rx >>= scale;
            ry >>= scale;
            if (rx == 0 || ry == 0) {
                continue;  /* every pixel of it is rounded off */
            }
            UINT ox = x >> scale, oy = y >> scale;
            BYTE *bitmap = malloc((size_t)rx * ry * 3);
            if (bitmap == NULL) {
                fputs("fake tjpgd: out of memory\n", stderr);
                exit(2);
            }
            for (UINT j = 0; j < ry; j++) {
                for (UINT i = 0; i < rx; i++) {
                    pixel((ox + i) << scale, (oy + j) << scale, bitmap + ((size_t)j * rx + i) * 3);
                }
            }
            JRECT rect = {.left = (WORD)ox, .right = (WORD)(ox + rx - 1), .top = (WORD)oy, .bottom = (WORD)(oy + ry - 1)};
            UINT go_on = outfunc(jd, bitmap, &rect);
            free(bitmap);
            fake_tjpgd_rects++;
            if (go_on == 0) {
                return JDR_INTR;
            }
            if (mode == 'C') {
                return JDR_FMT1;
            }
        }
    }
    return JDR_OK;
}
