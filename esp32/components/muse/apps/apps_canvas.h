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
 * ui.canvas's draw list, and the filled-polygon rasterizer the apply step draws poly and ellipse with. Portable C: no
 * LVGL, no Lua. apps_ui.c reads c:draw{...} into one block (apps_cv_list_t), checking every value, and keeps it on the
 * widget; the apply step (apps_lv.c) draws the block into the canvas's pixels. The names follow the HN runtime's canvas
 * (rect, circle, line, arc, text, image, poly), plus ellipse, a rect's gradient and a text's letter spacing.
 *
 * The block: the header, then n commands, then n_points points (x, y pairs of int32 in 1/16 px: APPS_CV_SUBPX), then the
 * texts (each NUL-terminated, at the offset its command says). One apps_port_realloc block, which the app task frees.
 *
 * Coordinates are px in the canvas, (0, 0) its top left; x and y may be negative or past its size (what falls outside is
 * cut), within +-APPS_CV_COORD_MAX. A command's opacity is 0..100.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_CV_CMDS_MAX 512          /* commands in one c:draw */
#define APPS_CV_POINTS_MAX 4096       /* points (x, y pairs) of all its lines and polys together */
#define APPS_CV_TEXT_BYTES_MAX 8192   /* bytes of all its texts together, each with its NUL */
#define APPS_CV_COORD_MAX 932         /* +- this many px: twice the screen */
#define APPS_CV_SUBPX 16              /* a point's coordinates are in 1/16 px */
#define APPS_CV_WIDTH_MAX 64          /* a line's or an arc's width, a border, px */
/* The px all canvases may cover together, as bytes of their pixels: one opaque canvas the size of the screen (466 x 466 RGB565).
 * A canvas with bg "none" is ARGB8888, 4 bytes a px, so half the area. */
#define APPS_CANVAS_BYTES_MAX (466 * 466 * 2)

typedef enum {
    APPS_CV_RECT, APPS_CV_CIRCLE, APPS_CV_ELLIPSE, APPS_CV_LINE, APPS_CV_ARC, APPS_CV_TEXT, APPS_CV_IMAGE, APPS_CV_POLY
} apps_cv_op_t;

typedef enum { APPS_CV_GRAD_NONE, APPS_CV_GRAD_V, APPS_CV_GRAD_H } apps_cv_grad_t;

/*
 * One command. What each uses (the rest is 0):
 * - rect: x y w h; color (APPS_COLOR_NONE: no fill), color2 and grad (a gradient from color to color2, top to bottom or left to
 *   right), radius (-1: a circle's), border and border_color.
 * - circle: x y (its centre) r; color (none: no fill), border, border_color.
 * - ellipse: x y (its centre) rx ry (in w and h); color. Filled only.
 * - line: points (pts, n_pts >= 2), width, color, rounded, dash and gap (px; 0: solid).
 * - arc: x y (its centre) r, start_angle end_angle (clock degrees 0..360), width, color, rounded.
 * - text: x y (its top left), w (0: as wide as its text, else it wraps there and aligns in it), text, font (apps_font_t),
 *   align (0 left, 1 center, 2 right), spacing (px between letters), color.
 * - image: x y (its top left), image (an asset index: apps_ui.h), scale (percent).
 * - poly: points (n_pts >= 3), color. Filled, non-zero winding, anti-aliased.
 */
typedef struct {
    uint8_t op, opacity, font, align, grad, rounded, border, width;
    int16_t x, y, w, h, r, radius;
    int16_t start_angle, end_angle, dash, gap, spacing, image;
    uint16_t scale, n_pts;
    uint32_t color, color2, border_color;
    uint32_t pts;   /* index of its first point in the points */
    uint32_t text;  /* offset of its text in the texts */
} apps_cv_cmd_t;

typedef struct apps_cv_list {
    uint32_t n, n_points, text_bytes;
    apps_cv_cmd_t *cmds;
    int32_t *points;  /* 2 * n_points */
    char *texts;
} apps_cv_list_t;

/* The bytes of a block holding n commands, points and text bytes, or 0 when they're past the limits above. */
static inline size_t apps_canvas_list_bytes(uint32_t n, uint32_t n_points, uint32_t text_bytes) {
    if (n > APPS_CV_CMDS_MAX || n_points > APPS_CV_POINTS_MAX || text_bytes > APPS_CV_TEXT_BYTES_MAX) {
        return 0;
    }
    return sizeof(apps_cv_list_t) + n * sizeof(apps_cv_cmd_t) + (size_t)n_points * 2 * sizeof(int32_t) + text_bytes;
}

/* Lays the block out in mem (apps_canvas_list_bytes of these, from apps_port_realloc): the pointers set, the counts kept. */
static inline apps_cv_list_t *apps_canvas_list_init(void *mem, uint32_t n, uint32_t n_points, uint32_t text_bytes) {
    apps_cv_list_t *list = (apps_cv_list_t *)mem;
    uint8_t *at = (uint8_t *)mem + sizeof *list;
    list->n = n;
    list->n_points = n_points;
    list->text_bytes = text_bytes;
    list->cmds = (apps_cv_cmd_t *)at;
    at += n * sizeof(apps_cv_cmd_t);
    list->points = (int32_t *)at;
    at += (size_t)n_points * 2 * sizeof(int32_t);
    list->texts = (char *)at;
    return list;
}

/*
 * Fills the polygon pts[0, 2n) (x, y pairs in 1/16 px, n >= 3, closed from the last point back to the first) with rgb at opacity
 * opa (0..255) into the w x h pixels at buf, a row each stride bytes: RGB565 (LVGL's lv_color16_t, 2 bytes a px), or ARGB8888
 * when argb (LVGL's lv_color32_t: blue, green, red, alpha; straight alpha). Non-zero winding; each px gets the share of it the
 * polygon covers, in 4 sub-rows a px, to the 1/256 px across. scratch holds apps_canvas_poly_scratch(n, w) bytes. What falls
 * outside the pixels is cut.
 */
size_t apps_canvas_poly_scratch(int n, int w);
void apps_canvas_fill_poly(uint8_t *buf, int stride, int w, int h, bool argb, const int32_t *pts, int n, uint32_t rgb, uint8_t opa,
                           void *scratch);

/* The points of an ellipse about (cx, cy) with radii rx, ry (px), as a polygon in 1/16 px into out (2 * the count it returns
 * ints, at most APPS_CV_ELLIPSE_POINTS_MAX points): more points for a bigger one, so its outline stays smooth. */
#define APPS_CV_ELLIPSE_POINTS_MAX 128
int apps_canvas_ellipse(int32_t *out, int cx, int cy, int rx, int ry);
