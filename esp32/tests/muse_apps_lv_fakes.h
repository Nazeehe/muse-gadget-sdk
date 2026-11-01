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
 * A fake LVGL for apps_lv.c (tests/test_muse_apps_lv.py stages lvgl.h, esp_attr.h, esp_heap_caps.h, esp_log.h and
 * esp_timer.h as one line each that includes this, and an sdkconfig.h of its own: empty, or CONFIG_MUSE_BENCH for the bench
 * build of the harness). The declarations here are the real LVGL 9.5.0's: same names, same types,
 * same parameters (test_the_fakes_match_lvgl_9_5_0 compares them with the headers when they are on this machine),
 * so what compiles here compiles against the real headers too. Only the functions apps_lv.c calls are declared:
 * one it doesn't call, like lv_obj_set_parent, is a compile error if it starts to.
 *
 * The implementation is in tests/muse_apps_lv_harness.c. It keeps every object ever made (a deleted one is only
 * marked, so a call on it is caught: the fake counts it as an error, which a real LVGL would turn into a crash on
 * the device), logs each call as one line, and lets a test read the log, move the clock, set the free PSRAM, fire
 * an event at an object (a deleted one too) and set the objects' coordinates. A test can also touch the screen at a
 * point, move the finger and lift it, and run the animations' frames: the fake then does what LVGL 9.5.0's pointer
 * input does with that (lv_indev.c, lv_indev_scroll.c, lv_obj_scroll.c), from finding the object under the finger to a
 * scroll, a gesture or a click, and a fling's snap as an animation. The animations apps_lv.c starts itself (an image's spin)
 * run as lv_anim.c runs them: copied into a list at lv_anim_start, stepped by each frame's animation timer (the path, the
 * exec callback only when the value changes, a repeat), and deleted with lv_anim_delete or with their object (lv_obj_destructor),
 * the deleted callback called each time. LVGL's own widget with an animation, lv_spinner, is here as lv_spinner.c has it (the
 * constructor's defaults, lv_spinner_set_anim_params's two animations with the bezier path) over an arc whose indicator angles
 * invalidate the box of the arc, which is more than LVGL's changed sector, so what a test says of a spinner holds for LVGL too.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- esp_attr.h: a static the device keeps in PSRAM is a plain one here ---- */

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

/* ---- esp_heap_caps.h ---- */

#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)
size_t heap_caps_get_free_size(uint32_t caps);

/* ---- esp_timer.h ---- */

int64_t esp_timer_get_time(void);

/* ---- esp_log.h ---- */

void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fake_esp_log('D', tag, __VA_ARGS__)

/* ---- lvgl.h: types ---- */

typedef struct lv_obj lv_obj_t;
typedef struct lv_display lv_display_t;
typedef struct lv_indev lv_indev_t;
typedef struct lv_event lv_event_t;
typedef struct lv_event_dsc lv_event_dsc_t;
typedef struct lv_style lv_style_t;

typedef uint8_t lv_opa_t;
typedef int32_t lv_value_precise_t;  /* LV_USE_FLOAT is off in every Muse build */
typedef uint32_t lv_style_selector_t;
typedef bool lv_anim_enable_t;

typedef struct {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
} lv_color_t;

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} lv_area_t;

typedef struct {
    int32_t x;
    int32_t y;
} lv_point_t;

/* The fonts are only compared by address: the real lv_font_t is much bigger. */
typedef struct lv_font {
    const char *name;
} lv_font_t;

typedef enum {
    LV_ALIGN_DEFAULT = 0,
    LV_ALIGN_TOP_LEFT,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_RIGHT_MID,
    LV_ALIGN_CENTER,
} lv_align_t;

typedef enum {
    LV_DIR_NONE = 0x00,
    LV_DIR_LEFT = (1 << 0),
    LV_DIR_RIGHT = (1 << 1),
    LV_DIR_TOP = (1 << 2),
    LV_DIR_BOTTOM = (1 << 3),
    LV_DIR_HOR = LV_DIR_LEFT | LV_DIR_RIGHT,
    LV_DIR_VER = LV_DIR_TOP | LV_DIR_BOTTOM,
    LV_DIR_ALL = LV_DIR_HOR | LV_DIR_VER,
} lv_dir_t;

typedef enum {
    LV_TEXT_ALIGN_AUTO,
    LV_TEXT_ALIGN_LEFT,
    LV_TEXT_ALIGN_CENTER,
    LV_TEXT_ALIGN_RIGHT,
} lv_text_align_t;

typedef enum {
    LV_LABEL_LONG_MODE_WRAP,
    LV_LABEL_LONG_MODE_DOTS,
    LV_LABEL_LONG_MODE_SCROLL,
    LV_LABEL_LONG_MODE_SCROLL_CIRCULAR,
    LV_LABEL_LONG_MODE_CLIP,
} lv_label_long_mode_t;

typedef enum {
    LV_OBJ_FLAG_HIDDEN = (1u << 0),
    LV_OBJ_FLAG_CLICKABLE = (1u << 1),
    LV_OBJ_FLAG_SCROLLABLE = (1u << 4),
    LV_OBJ_FLAG_SNAPPABLE = (1u << 12),
    LV_OBJ_FLAG_GESTURE_BUBBLE = (1u << 15),
} lv_obj_flag_t;

/* lv_obj_scroll.h: when a scrollbar shows, and where a scroll that ends puts the snappable children. */
typedef enum {
    LV_SCROLLBAR_MODE_OFF,
    LV_SCROLLBAR_MODE_ON,
    LV_SCROLLBAR_MODE_ACTIVE,
    LV_SCROLLBAR_MODE_AUTO,
} lv_scrollbar_mode_t;

typedef enum {
    LV_SCROLL_SNAP_NONE,
    LV_SCROLL_SNAP_START,
    LV_SCROLL_SNAP_END,
    LV_SCROLL_SNAP_CENTER,
} lv_scroll_snap_t;

typedef enum {
    LV_PART_MAIN = 0x000000,
    LV_PART_INDICATOR = 0x020000,
    LV_PART_KNOB = 0x030000,
} lv_part_t;

/* lv_obj_style.h: the states a selector can name besides its part (the others aren't used). */
typedef enum {
    LV_STATE_DEFAULT = 0,
    LV_STATE_PRESSED = 1 << 7,
} lv_state_t;

typedef enum {
    LV_EVENT_ALL = 0,
    LV_EVENT_PRESSED,
    LV_EVENT_PRESSING,
    LV_EVENT_PRESS_LOST,
    LV_EVENT_SHORT_CLICKED,
    LV_EVENT_SINGLE_CLICKED,
    LV_EVENT_DOUBLE_CLICKED,
    LV_EVENT_TRIPLE_CLICKED,
    LV_EVENT_LONG_PRESSED,
    LV_EVENT_LONG_PRESSED_REPEAT,
    LV_EVENT_CLICKED,
    LV_EVENT_RELEASED,
    LV_EVENT_SCROLL_BEGIN,
    LV_EVENT_SCROLL_THROW_BEGIN,
    LV_EVENT_SCROLL_END,
    LV_EVENT_SCROLL,
    LV_EVENT_GESTURE,
    LV_EVENT_KEY,
    LV_EVENT_REFR_EXT_DRAW_SIZE = 27,  /* an object's: its param is the int32_t room it needs past its box, to raise (or cap) */
    LV_EVENT_REFR_READY = 59,  /* a display's: the end of each refresh, drawn or not (the codes between aren't used) */
    LV_EVENT_PREPROCESS = 0x8000,
} lv_event_code_t;

typedef void (*lv_event_cb_t)(lv_event_t *e);

/* lv_image.h: where an image sits in a box that is not its size (the others transform it, and are not used). */
typedef enum {
    LV_IMAGE_ALIGN_DEFAULT = 0,
    LV_IMAGE_ALIGN_TOP_LEFT,
    LV_IMAGE_ALIGN_TOP_MID,
    LV_IMAGE_ALIGN_TOP_RIGHT,
    LV_IMAGE_ALIGN_BOTTOM_LEFT,
    LV_IMAGE_ALIGN_BOTTOM_MID,
    LV_IMAGE_ALIGN_BOTTOM_RIGHT,
    LV_IMAGE_ALIGN_LEFT_MID,
    LV_IMAGE_ALIGN_RIGHT_MID,
    LV_IMAGE_ALIGN_CENTER,
} lv_image_align_t;

/* lv_color.h and lv_image_dsc.h: what an image's header says (only what apps_lv.c reads). */
enum {
    LV_COLOR_FORMAT_A8 = 0x0E,
    LV_COLOR_FORMAT_RGB565A8 = 0x14,  /* the colour, then a plane of alpha (stride / 2 a row) */
};
enum {
    LV_IMAGE_FLAGS_PREMULTIPLIED = 0x0001,
};

/* A variable image (what an .bin file is once its first 12 bytes are the header): lv_image_dsc.h. */
typedef struct {
    uint32_t magic : 8;
    uint32_t cf : 8;
    uint32_t flags : 16;

    uint32_t w : 16;
    uint32_t h : 16;
    uint32_t stride : 16;
    uint32_t reserved_2 : 16;
} lv_image_header_t;

typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;

/* lv_types.h and lv_anim.h: an animation (LV_USE_EXT_DATA is off in every Muse build, so it has no ext_data). lv_anim_start copies
 * one into LVGL's list; apps_lv.c's path callback reads its times and values. */
typedef struct _lv_anim_t lv_anim_t;
typedef int32_t (*lv_anim_path_cb_t)(const lv_anim_t *);
typedef void (*lv_anim_exec_xcb_t)(void *, int32_t);
typedef void (*lv_anim_custom_exec_cb_t)(lv_anim_t *, int32_t);
typedef void (*lv_anim_completed_cb_t)(lv_anim_t *);
typedef void (*lv_anim_start_cb_t)(lv_anim_t *);
typedef int32_t (*lv_anim_get_value_cb_t)(lv_anim_t *);
typedef void (*lv_anim_deleted_cb_t)(lv_anim_t *);

typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
} lv_anim_bezier3_para_t;

struct _lv_anim_t {
    void *var;
    lv_anim_exec_xcb_t exec_cb;
    lv_anim_custom_exec_cb_t custom_exec_cb;
    lv_anim_start_cb_t start_cb;
    lv_anim_completed_cb_t completed_cb;
    lv_anim_deleted_cb_t deleted_cb;
    lv_anim_get_value_cb_t get_value_cb;
    void *user_data;
    lv_anim_path_cb_t path_cb;
    int32_t start_value;
    int32_t current_value;
    int32_t end_value;
    int32_t duration;
    int32_t act_time;
    uint32_t reverse_delay;
    uint32_t reverse_duration;
    uint32_t repeat_delay;
    uint32_t repeat_cnt;
    union _lv_anim_path_para_t {
        lv_anim_bezier3_para_t bezier3;
    } parameter;
    uint32_t last_timer_run;
    uint32_t pause_time;
    uint32_t pause_duration;
    uint8_t is_paused : 1;
    uint8_t reverse_play_in_progress : 1;
    uint8_t run_round : 1;
    uint8_t start_cb_called : 1;
    uint8_t early_apply : 1;
};

/* ---- lvgl.h: constants ---- */

#define LV_ATTRIBUTE_EXTERN_DATA
#define LV_BEZIER_VAL_SHIFT 10
#define LV_BEZIER_VAL_MAX (1L << LV_BEZIER_VAL_SHIFT)
#define LV_BEZIER_VAL_FLOAT(f) ((int32_t)((f) * LV_BEZIER_VAL_MAX))
#define LV_FONT_DECLARE(font_name) LV_ATTRIBUTE_EXTERN_DATA extern const lv_font_t font_name;
LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_montserrat_20)
LV_FONT_DECLARE(lv_font_montserrat_28)
LV_FONT_DECLARE(lv_font_unscii_8)
LV_FONT_DECLARE(lv_font_unscii_16)

#define LV_COORD_MAX ((1 << 29) - 1)
#define LV_SIZE_CONTENT ((LV_COORD_MAX) | (1 << 29))
#define LV_RADIUS_CIRCLE 0x7FFF
#define LV_OPA_TRANSP 0
#define LV_OPA_30 76
#define LV_OPA_COVER 255
#define LV_ANIM_OFF false
#define LV_ANIM_ON true
#define LV_ANIM_REPEAT_INFINITE 0xFFFFFFFF

/* ---- lvgl.h: functions ---- */

lv_color_t lv_color_hex(uint32_t c);
int32_t lv_pct(int32_t x);

lv_obj_t *lv_obj_create(lv_obj_t *parent);
lv_obj_t *lv_label_create(lv_obj_t *parent);
lv_obj_t *lv_button_create(lv_obj_t *parent);
lv_obj_t *lv_arc_create(lv_obj_t *parent);
lv_obj_t *lv_spinner_create(lv_obj_t *parent);
lv_obj_t *lv_bar_create(lv_obj_t *parent);
lv_obj_t *lv_image_create(lv_obj_t *parent);
void lv_obj_delete(lv_obj_t *obj);
void lv_obj_clean(lv_obj_t *obj);
int32_t lv_obj_get_index(const lv_obj_t *obj);
void lv_obj_move_to_index(lv_obj_t *obj, int32_t index);

void lv_obj_remove_style(lv_obj_t *obj, const lv_style_t *style, lv_style_selector_t selector);
void lv_obj_remove_style_all(lv_obj_t *obj);
bool lv_obj_has_flag(const lv_obj_t *obj, lv_obj_flag_t f);
lv_obj_t *lv_obj_get_parent(const lv_obj_t *obj);
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t f);
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t f);

void lv_obj_set_size(lv_obj_t *obj, int32_t w, int32_t h);
void lv_obj_align(lv_obj_t *obj, lv_align_t align, int32_t x_ofs, int32_t y_ofs);
void lv_obj_update_layout(const lv_obj_t *obj);
void lv_obj_get_coords(const lv_obj_t *obj, lv_area_t *coords);

void lv_obj_set_scrollbar_mode(lv_obj_t *obj, lv_scrollbar_mode_t mode);
void lv_obj_set_scroll_dir(lv_obj_t *obj, lv_dir_t dir);
void lv_obj_set_scroll_snap_x(lv_obj_t *obj, lv_scroll_snap_t align);
int32_t lv_obj_get_scroll_x(const lv_obj_t *obj);
bool lv_obj_is_scrolling(const lv_obj_t *obj);
void lv_obj_scroll_to_view(lv_obj_t *obj, lv_anim_enable_t anim_en);

void lv_obj_set_style_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector);
void lv_obj_set_style_bg_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector);
void lv_obj_set_style_radius(lv_obj_t *obj, int32_t value, lv_style_selector_t selector);
void lv_obj_set_style_pad_left(lv_obj_t *obj, int32_t value, lv_style_selector_t selector);
void lv_obj_set_style_pad_right(lv_obj_t *obj, int32_t value, lv_style_selector_t selector);
void lv_obj_set_style_border_width(lv_obj_t *obj, int32_t value, lv_style_selector_t selector);
void lv_obj_set_style_border_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector);
void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *value, lv_style_selector_t selector);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector);
void lv_obj_set_style_text_align(lv_obj_t *obj, lv_text_align_t value, lv_style_selector_t selector);
void lv_obj_set_style_arc_width(lv_obj_t *obj, int32_t value, lv_style_selector_t selector);
void lv_obj_set_style_arc_color(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector);
void lv_obj_set_style_image_recolor(lv_obj_t *obj, lv_color_t value, lv_style_selector_t selector);
void lv_obj_set_style_image_recolor_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t selector);

void lv_label_set_text(lv_obj_t *obj, const char *text);
void lv_label_set_long_mode(lv_obj_t *obj, lv_label_long_mode_t long_mode);

void lv_arc_set_rotation(lv_obj_t *obj, int32_t rotation);
void lv_arc_set_start_angle(lv_obj_t *obj, lv_value_precise_t start);
void lv_arc_set_end_angle(lv_obj_t *obj, lv_value_precise_t end);
void lv_arc_set_bg_angles(lv_obj_t *obj, lv_value_precise_t start, lv_value_precise_t end);
void lv_arc_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void lv_arc_set_value(lv_obj_t *obj, int32_t value);

void lv_spinner_set_anim_params(lv_obj_t *obj, uint32_t t, uint32_t angle);

void lv_bar_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void lv_bar_set_value(lv_obj_t *obj, int32_t value, lv_anim_enable_t anim);

void lv_image_set_src(lv_obj_t *obj, const void *src);
void lv_image_set_scale(lv_obj_t *obj, uint32_t zoom);
void lv_image_set_inner_align(lv_obj_t *obj, lv_image_align_t align);
void lv_image_set_rotation(lv_obj_t *obj, int32_t angle);
int32_t lv_image_get_rotation(lv_obj_t *obj);

void lv_anim_init(lv_anim_t *a);
void lv_anim_set_var(lv_anim_t *a, void *var);
void lv_anim_set_exec_cb(lv_anim_t *a, lv_anim_exec_xcb_t exec_cb);
void lv_anim_set_values(lv_anim_t *a, int32_t start, int32_t end);
void lv_anim_set_duration(lv_anim_t *a, uint32_t duration);
void lv_anim_set_repeat_count(lv_anim_t *a, uint32_t cnt);
void lv_anim_set_path_cb(lv_anim_t *a, lv_anim_path_cb_t path_cb);
void lv_anim_set_deleted_cb(lv_anim_t *a, lv_anim_deleted_cb_t deleted_cb);
void lv_anim_set_bezier3_param(lv_anim_t *a, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
int32_t lv_anim_path_custom_bezier3(const lv_anim_t *a);
lv_anim_t *lv_anim_start(const lv_anim_t *a);
bool lv_anim_delete(void *var, lv_anim_exec_xcb_t exec_cb);

void *lv_event_get_user_data(lv_event_t *e);
void *lv_event_get_param(lv_event_t *e);
lv_obj_t *lv_event_get_target_obj(lv_event_t *e);
lv_event_code_t lv_event_get_code(lv_event_t *e);
void lv_event_stop_processing(lv_event_t *e);
lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t event_cb, lv_event_code_t filter, void *user_data);

lv_display_t *lv_obj_get_display(const lv_obj_t *obj);
int32_t lv_obj_get_width(const lv_obj_t *obj);
int32_t lv_obj_get_height(const lv_obj_t *obj);
void lv_obj_refresh_ext_draw_size(lv_obj_t *obj);
void lv_display_add_event_cb(lv_display_t *disp, lv_event_cb_t event_cb, lv_event_code_t filter, void *user_data);

lv_indev_t *lv_indev_active(void);
lv_dir_t lv_indev_get_gesture_dir(const lv_indev_t *indev);
void lv_indev_wait_release(lv_indev_t *indev);
void lv_indev_get_point(const lv_indev_t *indev, lv_point_t *point);
lv_obj_t *lv_indev_get_active_obj(void);
