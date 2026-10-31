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

/* The apps' JPEG decoder over the ROM's TJpgDec (apps_jpeg.h has the contract). */

#include "apps_jpeg.h"

#include <stdio.h>
#include <string.h>

#include "apps_port.h"
#include "apps_rt.h"
#include "rom/tjpgd.h"

/* LVGL v9's image header (apps_pkg.c checks the same): magic, colour format, flags, w, h, stride, reserved, little-endian. */
#define HEADER 12
#define MAGIC 0x19
#define CF_RGB565 0x12
#define CF_RGB565A8 0x14

/* What TJpgDec's callbacks are given (its session's device): the JPEG being read, and the colour plane being written. */
typedef struct {
    const uint8_t *jpg;
    size_t len, at;
    uint8_t *pixels;  /* w x h RGB565, 2 bytes a pixel */
    unsigned w, h;
} decode_t;

/* TJpgDec's input: n bytes into buf, or skipped when buf is NULL; how many there were (fewer at the end of the JPEG). */
static UINT read_jpeg(JDEC *jd, BYTE *buf, UINT n) {
    decode_t *d = jd->device;
    size_t left = d->len - d->at;
    size_t take = n < left ? n : left;
    if (buf != NULL) {
        memcpy(buf, d->jpg + d->at, take);
    }
    d->at += take;
    return (UINT)take;
}

/* TJpgDec's output: a rectangle of RGB888 pixels into the colour plane as LVGL's RGB565 (little-endian, red on top). A part of it
 * outside the picture, which TJpgDec never gives, is left out rather than written past the block. */
static UINT put_rect(JDEC *jd, void *bitmap, JRECT *rect) {
    decode_t *d = jd->device;
    const BYTE *rgb = bitmap;
    for (unsigned y = rect->top; y <= rect->bottom; y++) {
        for (unsigned x = rect->left; x <= rect->right; x++, rgb += 3) {
            if (x < d->w && y < d->h) {
                unsigned px = (rgb[0] & 0xF8u) << 8 | (rgb[1] & 0xFCu) << 3 | rgb[2] >> 3;
                uint8_t *at = d->pixels + ((size_t)y * d->w + x) * 2;
                at[0] = (uint8_t)px;
                at[1] = (uint8_t)(px >> 8);
            }
        }
    }
    return 1;
}

/* The circle (apps_jpeg.h): a pixel is opaque when its centre lies strictly inside the circle inscribed in the picture. In doubled
 * coordinates every number is an integer: pixel (x, y)'s centre is (2x + 1, 2y + 1), the circle's centre (w, h) and its radius
 * min(w, h). */
static void fill_circle(uint8_t *alpha, unsigned w, unsigned h) {
    long r = (long)(w < h ? w : h);
    for (unsigned y = 0; y < h; y++) {
        long dy = 2 * (long)y + 1 - (long)h;
        for (unsigned x = 0; x < w; x++) {
            long dx = 2 * (long)x + 1 - (long)w;
            alpha[(size_t)y * w + x] = dx * dx + dy * dy < r * r ? 255 : 0;
        }
    }
}

static void put_le16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* message into err, cut to err_len (the messages are ASCII), and false. */
static bool fail(char *err, size_t err_len, const char *message) {
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", message);
    }
    return false;
}

bool apps_jpeg_decode(const uint8_t *jpg, size_t len, int max_side, bool circle, uint8_t **out, size_t *out_len, int *w, int *h,
                      char *err, size_t err_len) {
    *out = NULL;
    *out_len = 0;
    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (jpg == NULL || len == 0) {
        return fail(err, err_len, APPS_JPEG_MSG_INVALID);
    }
    if (max_side < 1 || max_side > APPS_RT_IMAGE_SIDE_MAX) {
        max_side = APPS_RT_IMAGE_SIDE_MAX;
    }
    void *pool = apps_port_realloc(NULL, APPS_JPEG_POOL_BYTES);
    if (pool == NULL) {
        return fail(err, err_len, APPS_JPEG_MSG_NO_MEMORY);
    }
    decode_t d = {.jpg = jpg, .len = len};
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, read_jpeg, pool, APPS_JPEG_POOL_BYTES, &d);
    if (rc != JDR_OK) {
        apps_port_free(pool);
        return fail(err, err_len, rc == JDR_FMT3 ? APPS_JPEG_MSG_PROGRESSIVE : APPS_JPEG_MSG_INVALID);
    }
    /* The largest scale (the least reduction) at which both sides fit: never larger than the picture. */
    BYTE scale = 0;
    while (scale < 3 && ((jd.width >> scale) > (UINT)max_side || (jd.height >> scale) > (UINT)max_side)) {
        scale++;
    }
    unsigned ow = jd.width >> scale, oh = jd.height >> scale;
    if (ow > (unsigned)max_side || oh > (unsigned)max_side || ow == 0 || oh == 0) {
        apps_port_free(pool);
        char text[64];
        if (ow == 0 || oh == 0) {
            snprintf(text, sizeof text, APPS_JPEG_MSG_TOO_THIN, max_side);
        } else {
            snprintf(text, sizeof text, APPS_JPEG_MSG_TOO_LARGE, max_side);
        }
        return fail(err, err_len, text);
    }
    size_t colour = (size_t)ow * oh * 2, alpha = circle ? (size_t)ow * oh : 0;
    size_t total = HEADER + colour + alpha;
    uint8_t *block = apps_port_realloc(NULL, total);
    if (block == NULL) {
        apps_port_free(pool);
        return fail(err, err_len, APPS_JPEG_MSG_NO_MEMORY);
    }
    block[0] = MAGIC;
    block[1] = circle ? CF_RGB565A8 : CF_RGB565;
    put_le16(block + 2, 0);       /* flags: none (the alpha isn't premultiplied) */
    put_le16(block + 4, ow);
    put_le16(block + 6, oh);
    put_le16(block + 8, ow * 2);  /* the colour plane's stride; the alpha plane's is half of it */
    put_le16(block + 10, 0);
    memset(block + HEADER, 0, colour);  /* a pixel TJpgDec never gave (it gives every one) is black, not the heap's */
    d.pixels = block + HEADER;
    d.w = ow;
    d.h = oh;
    rc = jd_decomp(&jd, put_rect, scale);
    apps_port_free(pool);
    if (rc != JDR_OK) {
        apps_port_free(block);
        return fail(err, err_len, rc == JDR_FMT3 ? APPS_JPEG_MSG_PROGRESSIVE : APPS_JPEG_MSG_INVALID);
    }
    if (circle) {
        fill_circle(block + HEADER + colour, ow, oh);
    }
    *out = block;
    *out_len = total;
    if (w != NULL) {
        *w = (int)ow;
    }
    if (h != NULL) {
        *h = (int)oh;
    }
    return true;
}
