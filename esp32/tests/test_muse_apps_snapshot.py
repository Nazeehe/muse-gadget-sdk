# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Link's app.control snapshot (Task 25): components/muse/apps/apps_snapshot.c.

MaskAndScale compiles apps_snapshot_mask on its own (style b) and checks the round mask and the half-size scale against the
same rule worked out here. Snapshot runs tests/muse_apps_snapshot_harness.c: the real apps_snapshot.c, with the real package
rules (apps_pkg.c, for the id and the shown text) and the real cJSON, over fakes of the I/O worker (jobs run when the case says),
the LVGL task (a frame at every poll of the job, on a clock the case moves), LVGL's snapshot, the JPEG encoder, the store, the
app task's record, the VM's writer and the screen's sleep. Every case checks that done ran exactly once (or not at all for a
refusal) and that every block it allocated, cJSON's, the port's and the encoder's, has been freed. Needs the cJSON sources:
CJSON_SOURCE_DIR (~/.cache/muse-cjson), or an IDF build's managed_components.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_link_ota import function_source

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_snapshot_harness.c"
SOURCES = [HARNESS, APPS / "apps_snapshot.c", APPS / "apps_result.c", APPS / "apps_pkg.c", APPS / "apps_json.c", APPS / "apps_vm.c",
           APPS / "apps_suggest.c"]

SIDE = 466


def kept(x, y, w, h, scale):
    """Whether the mask keeps the pixel (x, y) of the (w / scale) x (h / scale) image: its centre, in the screen's pixels, is
    inside the circle of diameter min(w, h) about the screen's centre. In doubled coordinates, so it is integers only."""
    d = min(w, h)
    cx = 2 * scale * x + scale - w
    cy = 2 * scale * y + scale - h
    return cx * cx + cy * cy <= d * d


MASK_MAIN = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@FUNCTION@

#define W 466
#define H 466
#define STRIDE 470  /* the screen's rows, wider than the image: the stride is honoured */

static uint16_t src[H * STRIDE];
static uint16_t dst[H * W];
static uint16_t half[240 * 240];
static uint16_t block[H * STRIDE];  /* the mask in place (N1): the drawing's block, masked where it is */
static uint16_t half_tight[233 * 233];

static uint16_t pattern(int x, int y) {
    return (uint16_t)(0x0841u + (unsigned)((x * 7 + y * 13) & 0x0FFF));  /* never 0 */
}

int main(void) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < STRIDE; x++) {
            src[y * STRIDE + x] = x < W ? pattern(x, y) : 0xBEEF;
        }
    }
    apps_snapshot_mask(src, W, H, STRIDE, 1, dst, W);
    long n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t v = dst[y * W + x];
            if (v != 0) {
                n++;
                if (v != pattern(x, y)) {
                    printf("changed %d %d %04x %04x\n", x, y, v, pattern(x, y));
                    return 1;
                }
            }
        }
    }
    printf("kept1 %ld\n", n);
    printf("p00 %04x\n", dst[0]);
    printf("centre %d\n", dst[233 * W + 233] == pattern(233, 233));
    printf("edge %d\n", dst[233 * W + 465] == pattern(465, 233));
    printf("left %d\n", dst[233 * W + 0] == pattern(0, 233));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            printf("%c", dst[y * W + x] != 0 ? '1' : '0');
        }
    }
    printf("\n");

    /* Half size, into rows of 240 (the encoder's padding): the padding is left as it is. */
    for (int i = 0; i < 240 * 240; i++) {
        half[i] = 0x1234;
    }
    /* A 2 x 2 block of known colours at the centre: (232, 232) (233, 232) (232, 233) (233, 233) go to (116, 116). */
    src[232 * STRIDE + 232] = 0xF800;  /* red 31 */
    src[232 * STRIDE + 233] = 0x07E0;  /* green 63 */
    src[233 * STRIDE + 232] = 0x001F;  /* blue 31 */
    src[233 * STRIDE + 233] = 0xFFFF;  /* white */
    apps_snapshot_mask(src, W, H, STRIDE, 2, half, 240);
    long n2 = 0;
    int pad_ok = 1;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 240; x++) {
            uint16_t v = half[y * 240 + x];
            if (x >= 233 || y >= 233) {
                pad_ok &= v == 0x1234;
            } else if (v != 0) {
                n2++;
            }
        }
    }
    printf("kept2 %ld\n", n2);
    printf("pad %d\n", pad_ok);
    printf("mean %04x\n", half[116 * 240 + 116]);
    printf("h00 %04x\n", half[0]);
    for (int y = 0; y < 233; y++) {
        for (int x = 0; x < 233; x++) {
            printf("%c", half[y * 240 + x] != 0 ? '1' : '0');
        }
    }
    printf("\n");

    /* In place, rows of the picture's own width in the block of the screen's rows: the same picture as out of place, both scales. */
    apps_snapshot_mask(src, W, H, STRIDE, 1, dst, W);
    memcpy(block, src, sizeof block);
    apps_snapshot_mask(block, W, H, STRIDE, 1, block, W);
    printf("inplace1 %d\n", memcmp(block, dst, sizeof dst) == 0);
    apps_snapshot_mask(src, W, H, STRIDE, 2, half_tight, 233);
    memcpy(block, src, sizeof block);
    apps_snapshot_mask(block, W, H, STRIDE, 2, block, 233);
    printf("inplace2 %d\n", memcmp(block, half_tight, sizeof half_tight) == 0);
    return 0;
}
"""


class MaskAndScale(unittest.TestCase):
    def test_mask_and_scale(self):
        source = (APPS / "apps_snapshot.c").read_text()
        function = function_source(source, "void apps_snapshot_mask(")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mask.c"
            path.write_text(MASK_MAIN.replace("@FUNCTION@", function))
            exe = apps_host.build("snapshot_mask", [path], tmp)
            done = apps_host.run(exe)
        self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr[-2000:])
        lines = done.stdout.split("\n")
        values = dict(line.split(" ", 1) for line in lines if " " in line)
        # Full size: (0, 0) is black, the centre is kept, and so is (465, 233), 232 px right of the centre column.
        self.assertEqual(values["p00"], "0000")
        self.assertEqual(values["centre"], "1")
        self.assertEqual(values["edge"], "1")
        self.assertEqual(values["left"], "1")
        want1 = "".join("1" if kept(x, y, SIDE, SIDE, 1) else "0" for y in range(SIDE) for x in range(SIDE))
        self.assertEqual(int(values["kept1"]), want1.count("1"))
        self.assertEqual(lines[5], want1, "the full-size mask isn't the circle")
        self.assertAlmostEqual(want1.count("1") / (3.14159265 * 233 * 233), 1.0, delta=0.01)
        # Half size: 233 x 233 (the padding to 240 untouched), the same circle, each pixel the mean of its 2 x 2 block.
        self.assertEqual(values["pad"], "1")
        self.assertEqual(values["h00"], "0000")
        want2 = "".join("1" if kept(x, y, SIDE, SIDE, 2) else "0" for y in range(233) for x in range(233))
        self.assertEqual(int(values["kept2"]), want2.count("1"))
        self.assertEqual(lines[10], want2, "the half-size mask isn't the circle")
        # red 31 + 0 + 0 + 31 -> 16 (rounded); green 0 + 63 + 0 + 63 -> 32; blue 0 + 0 + 31 + 31 -> 16.
        self.assertEqual(values["mean"], f"{(16 << 11) | (32 << 5) | 16:04x}")
        # N1: masked in place (the job's way: one block for the whole snapshot), the picture is the out-of-place one at both scales.
        self.assertEqual(values["inplace1"], "1")
        self.assertEqual(values["inplace2"], "1")


# What apps_snapshot.c and the headers it includes need of ESP-IDF, FreeRTOS, LVGL, the encoder and the input task: the little
# they use, each declared here and defined by the harness.
STAGED = {
    "sdkconfig.h": "#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_MUSE_APPS_HEAP_MAX_KB 1024\n",
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n",
    "esp_log.h": ("#pragma once\n"
                  "void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));\n"
                  "#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)\n"
                  "#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)\n"),
    "esp_heap_caps.h": ("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_8BIT (1u << 2)\n"
                        "#define MALLOC_CAP_DMA (1u << 3)\n#define MALLOC_CAP_SPIRAM (1u << 10)\n"
                        "#define MALLOC_CAP_INTERNAL (1u << 11)\n"
                        "size_t heap_caps_get_free_size(uint32_t caps);\nsize_t heap_caps_get_minimum_free_size(uint32_t caps);\n"
                        "void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);\n"
                        "void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);\n"),
    "freertos/FreeRTOS.h": ("#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\ntypedef int BaseType_t;\n"
                            "typedef unsigned UBaseType_t;\ntypedef void *TaskHandle_t;\n#define pdTRUE 1\n#define pdFALSE 0\n"),
    # A tick is a millisecond here, and the harness's clock moves on by the ticks a delay sleeps.
    "freertos/task.h": ("#pragma once\n#include \"freertos/FreeRTOS.h\"\n#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n"
                        "void vTaskDelay(TickType_t ticks);\nUBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);\n"),
    "muse_input.h": "#pragma once\nvoid muse_input_wake(const char *why);\n",
    "lvgl.h": ("#pragma once\n#include <stdint.h>\n#define LV_USE_SNAPSHOT 1\n"
               "typedef struct lv_obj lv_obj_t;\n"
               "typedef enum { LV_RESULT_INVALID = 0, LV_RESULT_OK } lv_result_t;\n"
               "typedef enum { LV_COLOR_FORMAT_UNKNOWN = 0, LV_COLOR_FORMAT_RGB565 = 0x12 } lv_color_format_t;\n"
               "typedef struct { uint32_t magic, cf, flags, w, h, stride; } lv_image_header_t;\n"
               "typedef struct { lv_image_header_t header; uint32_t data_size; uint8_t *data; void *unaligned_data; } lv_draw_buf_t;\n"
               "lv_obj_t *lv_screen_active(void);\nvoid lv_obj_update_layout(const lv_obj_t *obj);\n"
               "int32_t lv_obj_get_width(const lv_obj_t *obj);\nint32_t lv_obj_get_height(const lv_obj_t *obj);\n"
               "uint32_t lv_draw_buf_width_to_stride(uint32_t w, lv_color_format_t cf);\n"
               "lv_result_t lv_draw_buf_init(lv_draw_buf_t *draw_buf, uint32_t w, uint32_t h, lv_color_format_t cf, uint32_t stride,\n"
               "                             void *data, uint32_t data_size);\n"
               "lv_result_t lv_snapshot_take_to_draw_buf(lv_obj_t *obj, lv_color_format_t cf, lv_draw_buf_t *draw_buf);\n"),
    # esp_new_jpeg 1.0.2's encoder, as much of esp_jpeg_enc.h and esp_jpeg_common.h as the snapshot uses (same names and values).
    "esp_jpeg_enc.h": ("#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n"
                       "#define JPEG_FOURCC_TO_INT(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | "
                       "((uint32_t)(d) << 24))\n"
                       "typedef enum { JPEG_PIXEL_FORMAT_YCbYCr = JPEG_FOURCC_TO_INT('Y', 'U', 'Y', 'V'),\n"
                       "  JPEG_PIXEL_FORMAT_RGB565_LE = JPEG_FOURCC_TO_INT('R', 'G', 'B', 'L') } jpeg_pixel_format_t;\n"
                       "typedef enum { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_ERR_NO_MEM = -2, JPEG_ERR_NO_MORE_DATA = -3,\n"
                       "  JPEG_ERR_INVALID_PARAM = -4, JPEG_ERR_BAD_DATA = -5, JPEG_ERR_UNSUPPORT_FMT = -6,\n"
                       "  JPEG_ERR_UNSUPPORT_STD = -7 } jpeg_error_t;\n"
                       "typedef enum { JPEG_SUBSAMPLE_GRAY = 0, JPEG_SUBSAMPLE_444 = 1, JPEG_SUBSAMPLE_422 = 2,\n"
                       "  JPEG_SUBSAMPLE_420 = 3 } jpeg_subsampling_t;\n"
                       "typedef enum { JPEG_ROTATE_0D = 0 } jpeg_rotate_t;\n"
                       "typedef void *jpeg_enc_handle_t;\n"
                       "typedef struct jpeg_info { int width; int height; jpeg_pixel_format_t src_type;\n"
                       "  jpeg_subsampling_t subsampling; uint8_t quality; jpeg_rotate_t rotate; bool task_enable;\n"
                       "  uint8_t hfm_task_priority; uint8_t hfm_task_core; } jpeg_enc_config_t;\n"
                       "#define DEFAULT_JPEG_ENC_CONFIG() { .width = 320, .height = 240, .src_type = JPEG_PIXEL_FORMAT_YCbYCr, "
                       ".subsampling = JPEG_SUBSAMPLE_420, .quality = 40, .rotate = JPEG_ROTATE_0D, .task_enable = false, "
                       ".hfm_task_priority = 13, .hfm_task_core = 1, }\n"
                       "jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *info, jpeg_enc_handle_t *jpeg_enc);\n"
                       "jpeg_error_t jpeg_enc_process(const jpeg_enc_handle_t jpeg_enc, const uint8_t *in_buf, int inbuf_size,\n"
                       "  uint8_t *out_buf, int outbuf_size, int *out_size);\n"
                       "jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t jpeg_enc);\n"
                       "void *jpeg_calloc_align(size_t size, int aligned);\nvoid jpeg_free_align(void *data);\n"),
}


class Snapshot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        staged = Path(tmp.name) / "staged"
        for name, text in STAGED.items():
            (staged / name).parent.mkdir(parents=True, exist_ok=True)
            (staged / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_snapshot_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[staged, APPS, MUSE, ROOT / "components/lua/src", apps_host.CJSON_DIR],
                                      objects=[cjson])

    def run_case(self, case):
        try:
            done = apps_host.run(self.harness, case, timeout=120)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 120 s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-4000:])
        return done.stdout

    def test_refusals(self):  # the ruling's codes at the call: nothing queued, done never called, the admission let go
        self.run_case("refusals")

    def test_not_installed(self):  # not_found in MUSE_APPS_NOT_FOUND's words; a store that isn't mounted is unavailable
        self.run_case("not_installed")

    def test_snapshot_of_the_app_in_front(self):  # D2's result, the file on the VM, the encoder's settings, the mask, the logs
        self.run_case("in_front")

    def test_half_size_is_padded_when_refused(self):  # D22: 233 refused, 240 encoded, w and h 233
        self.run_case("half_size")

    def test_full_size_is_padded_when_refused(self):  # D22: 466 refused, 480 encoded, w and h 466
        self.run_case("padded_full_size")

    def test_a_dark_screen_wakes_and_is_taken_when_lit(self):  # E9: the wake, then the first lit frame
        self.run_case("dark_then_lit")

    def test_a_screen_that_stays_dark_times_out(self):  # E9: 5 s, unavailable, done once, a late frame takes nothing
        self.run_case("dark_past_deadline")

    def test_a_screen_lit_at_the_deadline_is_dropped(self):  # E9: a flag found after the deadline is dropped
        self.run_case("lit_at_deadline")

    def test_an_app_that_is_not_in_front(self):  # D22: whatever is on screen, front false, and a warning that says so
        self.run_case("not_in_front")

    def test_a_failed_write_fails_the_command(self):  # the ruling: the writer's code (busy, out_of_memory, else unavailable)
        self.run_case("write_fails")

    def test_encoder_and_screen_failures(self):  # each is the command's failure, and every block is freed
        self.run_case("encoder_failures")

    def test_a_drawing_past_its_deadline_is_given_up(self):  # the handshake's ABANDONED: the LVGL task frees its block
        self.run_case("abandoned_drawing")

    def test_a_job_that_waited_too_long_does_nothing(self):  # busy, before any wake or drawing
        self.run_case("waited_too_long")

    def test_the_widgets_and_warnings_fit_a_result(self):  # the dump trimmed whole entries from its end, ui_truncated counts
        self.run_case("ui_trimmed")

    def test_every_allocation_refused_in_turn(self):  # done exactly once, nothing leaked, whichever allocation fails
        self.run_case("every_allocation_refused")

    def test_the_drawing_is_not_the_apps_time(self):  # apps_rt_steal_ms gets the drawing's time (the Task 19 carry)
        self.run_case("drawing_time_is_stolen")

    def test_a_screen_that_darkens_before_the_take_is_kept_lit(self):  # fix 1 C: the request's wake pokes the idle timer
        self.run_case("lit_then_dark_before_the_take")

    def test_front_is_what_was_drawn(self):  # fix 1 E: the app that runs is read at the take, with the layer's place
        self.run_case("app_switched_after_the_take")

    def test_the_write_gets_what_is_left_of_the_deadline(self):  # fix 1 H: a slow capture leaves the write little
        self.run_case("write_gets_whats_left")

    def test_the_encoders_buffers_are_psram(self):  # the Link's DMA margin: esp_new_jpeg's internal-first allocators, wrapped
        self.run_case("encoder_buffers_are_psram")

    def test_admission_before_drawing(self):  # N1: the peak and the block against free and largest; scale 2 suggested; job too
        self.run_case("admission_before_drawing")

    def test_one_block_through_the_floor(self):  # N1: the drawing, the in-place mask and the JPEG in one floored block
        self.run_case("one_block_through_the_floor")

    def test_the_padded_copy_is_floored(self):  # N1: the masked copy is the port's; refused, out_of_memory and nothing held
        self.run_case("the_padded_copy_is_floored")

    def test_the_apps_waits_are_logged(self):  # N1: how long the app waited for the snapshot's memory
        self.run_case("the_apps_waits_are_logged")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])


if __name__ == "__main__":
    unittest.main()
