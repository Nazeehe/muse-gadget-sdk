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

#include "apps_canvas.h"

#include <math.h>
#include <string.h>

/* ---- Filling a polygon ---- */

/*
 * A scanline fill with coverage. Each px row is 4 sub-rows, at 1/8, 3/8, 5/8 and 7/8 of the px down. On each the edges'
 * crossings are found (in 1/256 px across), sorted, and walked with their winding; every span inside adds to the row's
 * coverage: a px it covers whole gets 64 (a quarter of 256, as it's one sub-row of 4), the px at each end its share. Whole px go
 * through a difference array (diff), so a wide span costs two adds; the ends go straight into part. After the 4 sub-rows a px's
 * coverage is the running sum of diff plus its part, out of 256.
 */

#define SUBROWS 4
#define FULL 256           /* a px's whole coverage */
#define ROW_SHARE (FULL / SUBROWS)
#define FRAC 256           /* crossings are in 1/256 px */
#define TO_FRAC (FRAC / APPS_CV_SUBPX)

size_t apps_canvas_poly_scratch(int n, int w) {
    return (size_t)(n > 0 ? n : 0) * 2 * sizeof(int32_t) + (size_t)(w + 2) * 2 * sizeof(int32_t);
}

static void add_span(int32_t *part, int32_t *diff, int w, int32_t a, int32_t b) {
    a = a < 0 ? 0 : a;
    b = b > w * FRAC ? w * FRAC : b;
    if (b <= a) {
        return;
    }
    int32_t ia = a / FRAC, ib = b / FRAC;
    if (ia == ib) {
        part[ia] += (b - a) * ROW_SHARE / FRAC;
        return;
    }
    part[ia] += (FRAC - a % FRAC) * ROW_SHARE / FRAC;
    if (ib > ia + 1) {
        diff[ia + 1] += ROW_SHARE;
        diff[ib] -= ROW_SHARE;
    }
    if (ib < w) {
        part[ib] += (b % FRAC) * ROW_SHARE / FRAC;
    }
}

/* v / 255 rounded, for v up to 255 * 255, with no division (the S3 divides slowly, and this runs for every px a shape covers). */
static uint32_t div255(uint32_t v) {
    v += 128;
    return (v + (v >> 8)) >> 8;
}

static uint8_t mix8(uint32_t s, uint32_t d, uint32_t a) {
    return (uint8_t)div255(s * a + d * (255 - a));
}

static void blend_px(uint8_t *px, bool argb, uint8_t r, uint8_t g, uint8_t b, uint32_t a) {
    if (a >= 255) {  /* covered whole and opaque, as most of a big shape is: the colour, as it is */
        if (argb) {
            px[0] = b, px[1] = g, px[2] = r, px[3] = 255;
        } else {
            uint16_t c = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
            px[0] = (uint8_t)c;
            px[1] = (uint8_t)(c >> 8);
        }
        return;
    }
    if (argb) {
        uint32_t da = px[3];
        uint32_t out = a + (da * (255 - a) + 127) / 255;  /* straight alpha: s over d */
        if (out == 0) {
            return;
        }
        uint32_t dw = da * (255 - a) / 255;  /* d's share, before dividing by out */
        px[0] = (uint8_t)((b * a + px[0] * dw + out / 2) / out);
        px[1] = (uint8_t)((g * a + px[1] * dw + out / 2) / out);
        px[2] = (uint8_t)((r * a + px[2] * dw + out / 2) / out);
        px[3] = (uint8_t)out;
        return;
    }
    uint16_t d = (uint16_t)(px[0] | px[1] << 8);
    uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    dr = dr << 3 | dr >> 2;
    dg = dg << 2 | dg >> 4;
    db = db << 3 | db >> 2;
    uint32_t nr = mix8(r, dr, a), ng = mix8(g, dg, a), nb = mix8(b, db, a);
    uint16_t c = (uint16_t)((nr & 0xF8) << 8 | (ng & 0xFC) << 3 | nb >> 3);
    px[0] = (uint8_t)c;
    px[1] = (uint8_t)(c >> 8);
}

void apps_canvas_fill_poly(uint8_t *buf, int stride, int w, int h, bool argb, const int32_t *pts, int n, uint32_t rgb, uint8_t opa,
                           void *scratch) {
    if (buf == NULL || pts == NULL || scratch == NULL || n < 3 || w <= 0 || h <= 0 || opa == 0) {
        return;
    }
    int32_t *xs = scratch;
    int32_t *dirs = xs + n;
    int32_t *part = dirs + n;
    int32_t *diff = part + (w + 2);
    int32_t ymin = pts[1], ymax = pts[1];
    for (int i = 1; i < n; i++) {
        ymin = pts[2 * i + 1] < ymin ? pts[2 * i + 1] : ymin;
        ymax = pts[2 * i + 1] > ymax ? pts[2 * i + 1] : ymax;
    }
    int y0 = (int)floorf((float)ymin / APPS_CV_SUBPX), y1 = (int)ceilf((float)ymax / APPS_CV_SUBPX);
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > h ? h : y1;
    uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
    int bpp = argb ? 4 : 2;
    for (int y = y0; y < y1; y++) {
        memset(part, 0, (size_t)(w + 2) * 2 * sizeof(int32_t));  /* part and diff */
        int32_t xmin = w * FRAC, xmax = 0;
        for (int s = 0; s < SUBROWS; s++) {
            /* The sub-row's y in 1/16 px: (y + (2s + 1) / 8) px. */
            int32_t sy = y * APPS_CV_SUBPX + (2 * s + 1) * APPS_CV_SUBPX / (2 * SUBROWS);
            int k = 0;
            for (int i = 0; i < n; i++) {
                int j = i + 1 == n ? 0 : i + 1;
                int32_t xa = pts[2 * i], ya = pts[2 * i + 1], xb = pts[2 * j], yb = pts[2 * j + 1];
                int dir = ya <= sy && yb > sy ? 1 : (yb <= sy && ya > sy ? -1 : 0);
                if (dir == 0) {
                    continue;
                }
                int64_t x = (int64_t)xa * TO_FRAC + ((int64_t)(sy - ya) * (xb - xa) * TO_FRAC) / (yb - ya);
                int32_t xi = (int32_t)(x < -FRAC * (int64_t)APPS_CV_COORD_MAX * 4 ? -FRAC * APPS_CV_COORD_MAX * 4
                                       : x > FRAC * (int64_t)APPS_CV_COORD_MAX * 4 ? FRAC * APPS_CV_COORD_MAX * 4 : x);
                int m = k++;
                while (m > 0 && xs[m - 1] > xi) {  /* insertion: a row crosses few edges */
                    xs[m] = xs[m - 1];
                    dirs[m] = dirs[m - 1];
                    m--;
                }
                xs[m] = xi;
                dirs[m] = dir;
            }
            int wind = 0;
            int32_t start = 0;
            for (int i = 0; i < k; i++) {
                int was = wind;
                wind += dirs[i];
                if (was == 0 && wind != 0) {
                    start = xs[i];
                } else if (was != 0 && wind == 0) {
                    add_span(part, diff, w, start, xs[i]);
                    xmin = start < xmin ? start : xmin;
                    xmax = xs[i] > xmax ? xs[i] : xmax;
                }
            }
        }
        int xa = xmin / FRAC, xb = (xmax + FRAC - 1) / FRAC;
        xa = xa < 0 ? 0 : xa;
        xb = xb > w ? w : xb;
        int32_t run = 0;
        for (int x = 0; x < xa; x++) {
            run += diff[x];
        }
        uint8_t *row = buf + (size_t)y * (size_t)stride;
        for (int x = xa; x < xb; x++) {
            run += diff[x];
            int32_t cov = run + part[x];
            cov = cov < 0 ? 0 : cov > FULL ? FULL : cov;
            if (cov == 0) {
                continue;
            }
            uint32_t a = (uint32_t)cov * opa / FULL;
            if (a != 0) {
                blend_px(row + x * bpp, argb, r, g, b, a);
            }
        }
    }
}

int apps_canvas_ellipse(int32_t *out, int cx, int cy, int rx, int ry) {
    rx = rx < 0 ? 0 : rx;
    ry = ry < 0 ? 0 : ry;
    float around = 2.0f * (float)M_PI * sqrtf(((float)rx * rx + (float)ry * ry) / 2.0f);  /* about its perimeter, px */
    int n = (int)(around / 3.0f);  /* a point every 3 px or so */
    n = n < 24 ? 24 : n > APPS_CV_ELLIPSE_POINTS_MAX ? APPS_CV_ELLIPSE_POINTS_MAX : n;
    for (int i = 0; i < n; i++) {
        float t = 2.0f * (float)M_PI * (float)i / (float)n;
        out[2 * i] = (int32_t)lroundf(((float)cx + (float)rx * cosf(t)) * APPS_CV_SUBPX);
        out[2 * i + 1] = (int32_t)lroundf(((float)cy + (float)ry * sinf(t)) * APPS_CV_SUBPX);
    }
    return n;
}
