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

#include "muse_clock.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_sntp.h"

static const char *TAG = "muse_clock";

/*
 * A pocket watch: a gold case round an ivory enamel dial, a railway minute
 * track, Roman numerals with the watchmaker's IIII, blued steel hands, and a
 * small seconds dial where the VI would be. Laid out for a 466 px circle and
 * scaled to the screen.
 */
#define COLOR_CASE 0xb8923f
#define COLOR_CASE_LIGHT 0xe6cc85
#define COLOR_CASE_DARK 0x6e5320
#define COLOR_DIAL 0xf4eddc
#define COLOR_SUBDIAL 0xebe2cc
#define COLOR_INK 0x231e18
#define COLOR_FAINT 0x8a7f6c
#define COLOR_BLUED 0x1f3a70

static lv_obj_t *s_hands;      /* over the whole tile, so its centre is the watch's */
static lv_obj_t *s_status;
static int s_r;                 /* the case's radius */

/* What the hands show, in degrees clockwise from XII; -1 before the first. */
static float s_hour_deg = -1, s_min_deg = -1, s_sec_deg = -1;
static int s_shown_sec = -1;
static int s_shown_valid = -1;

static int px(int v)
{
    return v * s_r / 233;
}

static lv_point_precise_t polar(int cx, int cy, float deg, float r)
{
    float a = deg * (float)M_PI / 180.0f;
    return (lv_point_precise_t){ (lv_value_precise_t)(cx + r * sinf(a)),
                                 (lv_value_precise_t)(cy - r * cosf(a)) };
}

static void ring(lv_layer_t *layer, int cx, int cy, int r, int width, uint32_t color)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){ cx, cy };
    d.radius = r;
    d.width = width;
    d.start_angle = 0;
    d.end_angle = 360;
    d.color = lv_color_hex(color);
    lv_draw_arc(layer, &d);
}

static void disc(lv_layer_t *layer, int cx, int cy, int r, uint32_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(color);
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &d, &a);
}

static void line(lv_layer_t *layer, lv_point_precise_t p1, lv_point_precise_t p2, int width,
                 uint32_t color, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = p1;
    d.p2 = p2;
    d.width = width;
    d.color = lv_color_hex(color);
    d.round_start = d.round_end = round;
    lv_draw_line(layer, &d);
}

/* Sixty ticks out to r2: from r1, and from r5 and heavier for each fifth. */
static void ticks(lv_layer_t *layer, int cx, int cy, float r1, float r2, float r5, int w1, int w5)
{
    for (int i = 0; i < 60; i++) {
        bool five = i % 5 == 0;
        line(layer, polar(cx, cy, i * 6.0f, five ? r5 : r1), polar(cx, cy, i * 6.0f, r2),
             five ? w5 : w1, COLOR_INK, false);
    }
}

/* The watch's centre, and the seconds dial's, in `area`'s coordinates. */
static void centres(const lv_area_t *area, int *cx, int *cy, int *sx, int *sy)
{
    *cx = (area->x1 + area->x2) / 2;
    *cy = (area->y1 + area->y2) / 2;
    *sx = *cx;
    *sy = *cy + px(96);
}

static void print(lv_layer_t *layer, const lv_font_t *font, const char *text, int x, int y, int spacing)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = lv_color_hex(COLOR_INK);
    d.letter_space = spacing;
    d.align = LV_TEXT_ALIGN_CENTER;
    int h = lv_font_get_line_height(font);
    lv_area_t a = { x - px(70), y - h / 2, x + px(70), y - h / 2 + h - 1 };
    lv_draw_label(layer, &d, &a);
}

/*
 * Everything but the hands, drawn once: case, enamel, minute track, numerals
 * and the seconds dial. A swipe redraws the whole screen every frame, and
 * these full circles drawn live mask every pixel of every row they cross,
 * several milliseconds each; as an image they're a copy.
 */
static void draw_dial(lv_layer_t *layer, const lv_area_t *area)
{
    int cx, cy, sx, sy;
    centres(area, &cx, &cy, &sx, &sy);

    ring(layer, cx, cy, s_r - px(4), px(3), COLOR_CASE_LIGHT);
    ring(layer, cx, cy, s_r - px(10), px(2), COLOR_CASE_DARK);

    lv_draw_rect_dsc_t enamel;
    lv_draw_rect_dsc_init(&enamel);
    enamel.radius = LV_RADIUS_CIRCLE;
    enamel.bg_color = lv_color_hex(COLOR_DIAL);
    enamel.border_color = lv_color_hex(COLOR_CASE_DARK);
    enamel.border_width = px(3);
    int dr = s_r - px(16);
    lv_area_t d = { cx - dr, cy - dr, cx + dr - 1, cy + dr - 1 };
    lv_draw_rect(layer, &enamel, &d);

    int out = px(200), in = px(189);
    ring(layer, cx, cy, out, 2, COLOR_INK);
    ring(layer, cx, cy, in, 1, COLOR_INK);
    ticks(layer, cx, cy, in, out, in - px(4), 2, px(5));

    static const char *const NUMERALS[12] = {
        "XII", "I", "II", "III", "IIII", "V", NULL, "VII", "VIII", "IX", "X", "XI",
    };
    for (int i = 0; i < 12; i++) {
        if (NUMERALS[i]) {   /* the seconds dial sits where VI would */
            lv_point_precise_t p = polar(cx, cy, i * 30.0f, px(158));
            print(layer, &lv_font_montserrat_28, NUMERALS[i], (int)p.x, (int)p.y, 0);
        }
    }
    print(layer, &lv_font_montserrat_16, "MUSE", cx, cy - px(88), 4);

    int sr = px(46);
    disc(layer, sx, sy, sr, COLOR_SUBDIAL);
    ring(layer, sx, sy, sr, 1, COLOR_INK);
    ticks(layer, sx, sy, sr - px(4), sr - 1, sr - px(9), 1, 2);
}

static void triangle(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c)
{
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(COLOR_BLUED);
    t.p[0] = a;
    t.p[1] = b;
    t.p[2] = c;
    lv_draw_triangle(layer, &t);
}

/* Two points `half` either side of the hand at `r` from the centre. */
static void across(int cx, int cy, float deg, float r, float half, lv_point_precise_t *l, lv_point_precise_t *rt)
{
    lv_point_precise_t m = polar(cx, cy, deg, r), s = polar(0, 0, deg + 90, half);
    *l = (lv_point_precise_t){ m.x + s.x, m.y + s.y };
    *rt = (lv_point_precise_t){ m.x - s.x, m.y - s.y };
}

/*
 * A blued hand: a round-tailed stem into a leaf, widest 2w across at 0.6 of
 * its length. The stem ends where the leaf is as wide as it, and the leaf's
 * outer half starts a little inside the inner one, so no antialiased edge
 * shows where they join.
 */
static void hand(lv_layer_t *layer, int cx, int cy, float deg, int len, int tail, int w)
{
    float back = len * 0.4f, mid = len * 0.6f, lap = 2;
    line(layer, polar(cx, cy, deg + 180, tail), polar(cx, cy, deg, len * 0.5f), w, COLOR_BLUED, true);
    lv_point_precise_t l, r, il, ir;
    across(cx, cy, deg, mid, w, &l, &r);
    triangle(layer, polar(cx, cy, deg, back), l, r);
    across(cx, cy, deg, mid - lap, w * (mid - lap - back) / (mid - back), &il, &ir);
    triangle(layer, il, polar(cx, cy, deg, len), ir);
}

static void on_hands_draw(lv_event_t *e)
{
    if (s_min_deg < 0) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(s_hands, &c);
    int cx, cy, sx, sy;
    centres(&c, &cx, &cy, &sx, &sy);

    if (s_sec_deg >= 0) {
        line(layer, polar(sx, sy, s_sec_deg + 180, px(10)), polar(sx, sy, s_sec_deg, px(40)), 2,
             COLOR_BLUED, true);
        disc(layer, sx, sy, px(4), COLOR_BLUED);
    }
    hand(layer, cx, cy, s_hour_deg, px(112), px(20), px(9));
    hand(layer, cx, cy, s_min_deg, px(168), px(24), px(6));
    disc(layer, cx, cy, px(10), COLOR_BLUED);
    disc(layer, cx, cy, px(4), COLOR_CASE);
}

/* Redraws only where a hand was or now is. */
static void invalidate_hand(float deg, int len, int tail, int w)
{
    if (deg < 0) {
        return;
    }
    lv_area_t c;
    lv_obj_get_coords(s_hands, &c);
    int cx, cy, sx, sy;
    centres(&c, &cx, &cy, &sx, &sy);
    lv_point_precise_t a = polar(cx, cy, deg + 180, tail), b = polar(cx, cy, deg, len);
    int m = w + px(10) + 2;   /* the leaf, the centre cap and antialiasing */
    lv_area_t r = {
        (int32_t)LV_MIN(a.x, b.x) - m, (int32_t)LV_MIN(a.y, b.y) - m,
        (int32_t)LV_MAX(a.x, b.x) + m, (int32_t)LV_MAX(a.y, b.y) + m,
    };
    lv_obj_invalidate_area(s_hands, &r);
}

static void invalidate_subdial(void)
{
    lv_area_t c;
    lv_obj_get_coords(s_hands, &c);
    int cx, cy, sx, sy;
    centres(&c, &cx, &cy, &sx, &sy);
    int r = px(42) + 2;
    lv_area_t a = { sx - r, sy - r, sx + r, sy + r };
    lv_obj_invalidate_area(s_hands, &a);
}

void muse_clock_build(lv_obj_t *tile)
{
    lv_obj_update_layout(tile);
    int w = lv_obj_get_width(tile), h = lv_obj_get_height(tile);
    s_r = (w < h ? w : h) / 2;
    setenv("TZ", CONFIG_MUSE_CLOCK_TZ, 1);
    tzset();

    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    void *buf = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (buf) {
        lv_obj_t *dial = lv_canvas_create(tile);
        lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_canvas_set_buffer(dial, buf, w, h, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(dial, lv_color_hex(COLOR_CASE), LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(dial, &layer);
        lv_area_t all = { 0, 0, w - 1, h - 1 };
        draw_dial(&layer, &all);
        lv_canvas_finish_layer(dial, &layer);
    } else {
        /* Hands on a bare case rather than no watch at all. */
        ESP_LOGE(TAG, "no PSRAM for the dial");
        lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_CASE), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    }

    s_status = lv_label_create(tile);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_FAINT), 0);
    lv_obj_set_style_text_letter_space(s_status, 1, 0);
    lv_label_set_text(s_status, "SETTING TIME");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -px(62));

    s_hands = lv_obj_create(tile);
    lv_obj_remove_style_all(s_hands);
    lv_obj_remove_flag(s_hands, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_hands, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s_hands, on_hands_draw, LV_EVENT_DRAW_MAIN, NULL);
    muse_clock_tick(true);
}

void muse_clock_tick(bool visible)
{
    if (!visible || !s_hands) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_sec == s_shown_sec) {
        return;
    }
    s_shown_sec = tm.tm_sec;
    bool valid = muse_time_valid();
    if ((int)valid != s_shown_valid) {
        lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, valid);
        s_shown_valid = valid;
    }

    /* Unset, the hands rest at ten past ten and the seconds stop. */
    float hour = valid ? (tm.tm_hour % 12) * 30.0f + tm.tm_min * 0.5f : 10 * 30.0f + 5.0f;
    float min = valid ? tm.tm_min * 6.0f : 10 * 6.0f;
    float sec = valid ? tm.tm_sec * 6.0f : -1;
    if (hour != s_hour_deg || min != s_min_deg) {
        invalidate_hand(s_hour_deg, px(112), px(20), px(9));
        invalidate_hand(s_min_deg, px(168), px(24), px(6));
        s_hour_deg = hour;
        s_min_deg = min;
        invalidate_hand(s_hour_deg, px(112), px(20), px(9));
        invalidate_hand(s_min_deg, px(168), px(24), px(6));
    }
    if (sec != s_sec_deg) {
        s_sec_deg = sec;
        invalidate_subdial();
    }
}
