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

"""The bench's console commands (>mem, >ui=, >tap, >longpress, >swipe), the touch scripts played through LVGL's
read callback, the screenshot stream's CRC and tools/muse/snap.py, and the size of link.register in the log."""

import base64
import contextlib
import importlib
import inspect
import io
import random
import re
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

import apps_host
from test_muse_apps_wiring import c_function, open_conditions

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
TOOLS = ROOT / "tools/muse"


def load_snap():
    """tools/muse/snap.py, imported with pyserial blocked: parse(), capture() and write_png() must not need it (C14)."""
    sys.path.insert(0, str(TOOLS))
    try:
        with mock.patch.dict(sys.modules, {"serial": None}):
            sys.modules.pop("snap", None)
            return importlib.import_module("snap")
    finally:
        sys.path.remove(str(TOOLS))


def build_bench(name, program, tmp):
    """`program` compiled against the real headers with CONFIG_MUSE_BENCH on (the two ESP-IDF headers muse_ui.h
    includes are faked); the executable's path."""
    include = Path(tmp) / "include"
    include.mkdir()
    (include / "esp_err.h").write_text("typedef int esp_err_t;\n")
    (include / "sdkconfig.h").write_text("#define CONFIG_MUSE_BENCH 1\n")
    path = Path(tmp) / f"{name}.c"
    path.write_text(program)
    return apps_host.build(name, [path], tmp, include_dirs=[include, MUSE])


SNAPSHOT_PRELUDE = r"""
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define UNUSED __attribute__((unused))
typedef struct { struct { uint32_t w, h, stride; } header; uint8_t *data; } lv_draw_buf_t;
typedef struct lv_obj lv_obj_t;
#define LV_COLOR_FORMAT_RGB565 18
static lv_draw_buf_t g_buf;
static lv_obj_t *lv_screen_active(void) { return NULL; }
static lv_draw_buf_t *lv_snapshot_take(lv_obj_t *obj, int format) { (void)obj; (void)format; return &g_buf; }
static void lv_draw_buf_destroy(lv_draw_buf_t *buf) { (void)buf; }
static UNUSED const char *TAG = "muse_ui";
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
static int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 4 * ((slen + 2) / 3);
    if (dlen < n + 1) {
        return -1;
    }
    for (size_t i = 0, o = 0; i < slen; i += 3, o += 4) {
        uint32_t v = (uint32_t)src[i] << 16 | (i + 1 < slen ? (uint32_t)src[i + 1] << 8 : 0) |
                     (i + 2 < slen ? src[i + 2] : 0);
        dst[o] = (unsigned char)t[v >> 18 & 63];
        dst[o + 1] = (unsigned char)t[v >> 12 & 63];
        dst[o + 2] = i + 1 < slen ? (unsigned char)t[v >> 6 & 63] : '=';
        dst[o + 3] = i + 2 < slen ? (unsigned char)t[v & 63] : '=';
    }
    *olen = n;
    return 0;
}
/* The picture: noise in g_buf, rows padded to the stride, and its pixels (no padding) written to the file `dump`. */
static void make_picture(uint32_t w, uint32_t h, uint32_t pad, const char *dump)
{
    g_buf.header.w = w;
    g_buf.header.h = h;
    g_buf.header.stride = w * 2 + pad;
    g_buf.data = malloc((size_t)g_buf.header.stride * h);
    uint32_t seed = 12345;
    FILE *f = fopen(dump, "wb");
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < g_buf.header.stride; x++) {
            seed = seed * 1103515245u + 12345u;
            uint8_t b = (uint8_t)(seed >> 16);
            g_buf.data[y * g_buf.header.stride + x] = b;
            if (x < w * 2) {
                fputc(b, f);
            }
        }
    }
    fclose(f);
}
"""


def code_only(source):
    """C source without its comments, for a search for calls (a comment may name what it explains)."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def snap_defines(source):
    """The SNAP_* limits muse_ui.c's snapshot code is written with (the #define lines), as C."""
    return "\n".join(re.findall(r"^#define SNAP_\w+ .*$", source, re.M))


def console_variant(source, name):
    """muse_console.c from the line that starts the USB-Serial-JTAG variant ("usb") or the UART's ("uart") on."""
    head = source.index("#if !CONFIG_MUSE_CONSOLE_UART")
    split = source.index("\n#else", head)
    return source[head:split] if name == "usb" else source[split:]


def pixels(w, h, seed=1):
    """w*h RGB565 pixels of noise."""
    return random.Random(seed).randbytes(w * h * 2)


def stream(w, h, raw, per_line=144, crc=True):
    """What send_snapshot() streams for raw: a blank line, SNAP BEGIN, base64 lines of per_line bytes, a blank line and
    SNAP END with the CRC (both lines start with a newline of their own). crc=False is older firmware's: its END
    follows the last row at once and carries nothing."""
    lines = ["", f"SNAP BEGIN {w} {h} {per_line}"]
    for y in range(h):
        row = raw[y * w * 2:(y + 1) * w * 2]
        lines += [base64.b64encode(row[at:at + per_line]).decode() for at in range(0, len(row), per_line)]
    lines += ["", f"SNAP END {zlib.crc32(raw):08x}"] if crc else ["SNAP END"]
    return "\n".join(lines) + "\n"


def flip_first_data_char(log):
    """The same log with its first base64 character changed."""
    at = log.index("\n", log.index("SNAP BEGIN")) + 1
    return log[:at] + ("B" if log[at] == "A" else "A") + log[at + 1:]


class FakeClock:
    """snap.time: a clock that only moves when the script sleeps or a read times out."""

    def __init__(self):
        self.now = 1000.0
        self.slept = []

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds


class FakeBoard:
    """A serial port: write() records what is sent and when, and each 'p' queues the next scripted answer: a string,
    or (now, later, delay) with `later` arriving `delay` seconds after the 'p'. read() hands out at most `piece` bytes,
    so a line can arrive in parts, and an empty read costs the port's 0.2 s timeout. With `chatter` the board never
    goes quiet: the line comes in place of an empty read, as from an app logging on a timer."""

    def __init__(self, clock, answers, piece=100, chatter=""):
        self.clock, self.answers, self.piece, self.chatter = clock, list(answers), piece, chatter
        self.writes, self.write_times, self.pending, self.later = [], [], b"", []
        self.outstanding_at_write = []   # (bytes pending, parts still to come) as each write goes

    def write(self, data):
        self.outstanding_at_write.append((len(self.pending), len(self.later)))
        self.writes.append(bytes(data))
        self.write_times.append(self.clock.now)
        if data == b"p" and self.answers:
            answer = self.answers.pop(0)
            now, later, delay = (answer, "", 0) if isinstance(answer, str) else answer
            self.pending += now.encode("latin1")
            if later:
                self.later.append((self.clock.now + delay, later.encode("latin1")))

    def read(self, size=1):
        for due in [item for item in self.later if item[0] <= self.clock.now]:
            self.later.remove(due)
            self.pending += due[1]
        if not self.pending:
            self.clock.now += 0.2
            return self.chatter.encode("latin1")
        chunk, self.pending = self.pending[:min(size, self.piece)], self.pending[min(size, self.piece):]
        return chunk


class MuseBenchTest(unittest.TestCase):
    def test_bench_option_and_memory_report(self):
        kc = (MUSE / "Kconfig").read_text()
        self.assertIn("config MUSE_BENCH", kc)
        self.assertIn("CONFIG_MUSE_BENCH=y", (ROOT / "devices/sdkconfig.muse-bench").read_text())
        inp = (MUSE / "muse_input.c").read_text()
        block = inp[inp.index("#if CONFIG_MUSE_BENCH"):]
        self.assertIn('!strcmp(line, "mem")', block[:block.index("#endif")])
        self.assertIn('"lvgl"', inp)
        self.assertIn('xTaskGetHandle("muse_serial")', inp)

    def test_the_docs_of_the_memory_report_name_each_stack_it_reports(self):
        # The Kconfig help and the overlay's comment are where a bench user learns what >mem prints: each *_stack_free
        # field of the line the firmware prints is named in both, with the task it is of (S).
        inp = (MUSE / "muse_input.c").read_text()
        printed = inp[inp.index('printf("@mem '):]
        fields = re.findall(r'\\"(\w+_stack_free)\\"', printed[:printed.index(");")])
        self.assertEqual(fields, ["lvgl_stack_free", "serial_stack_free"])
        kconfig = (MUSE / "Kconfig").read_text()
        help_text = kconfig[kconfig.index("    config MUSE_BENCH\n"):]
        help_text = help_text[:help_text.index("    config MUSE_JC3248W535_MIC_GPIO")]
        overlay = (ROOT / "devices/sdkconfig.muse-bench").read_text()
        for name, text in (("the Kconfig help", " ".join(help_text.split())), ("the overlay", " ".join(
                line.lstrip("# ") for line in overlay.splitlines()))):
            for field in fields:
                self.assertIn(field, text, f"{name} doesn't name {field}")
            self.assertIn("muse_serial", text, f"{name} doesn't say which task the second stack is")
            self.assertIn("console", text)

    def test_register_size_is_logged(self):
        # The size comes from the log line every session already prints when it sends
        # the registration: "sent link.register (N bytes)".
        source = (ROOT / "main/noise_control.cpp").read_text()
        self.assertIn('"sent %s (%u bytes)"', source)
        self.assertIn('is_registration ? "link.register"', source)

    # ---- Task 14: the bench commands sit on their own option ----

    def test_bench_commands_gated_by_bench(self):
        # E19: mem, ui.demo, ui=, tap, longpress and swipe are one #if CONFIG_MUSE_BENCH block of console_command, with
        # no nested #if, and nothing about them depends on LVGL's snapshot option.
        source = (MUSE / "muse_input.c").read_text()
        self.assertEqual(source.count("#if CONFIG_MUSE_BENCH"), 1)
        command = c_function(source, "static bool console_command(char *line, bool whole)")
        base = source.index(command)
        for text in ('"mem"', '"ui.demo=1"', '"ui=settings"', '"ui=clock"', '"tap "', '"longpress "', '"swipe "'):
            with self.subTest(text):
                self.assertIn(text, command)
                self.assertEqual(open_conditions(source, base + command.index(text)), ["if CONFIG_MUSE_BENCH"])
        self.assertNotIn("LV_USE_SNAPSHOT", command)
        self.assertNotIn("LV_USE_SNAPSHOT", source)

    def test_page_demo_and_touch_api_sit_on_the_bench_option(self):
        header = (MUSE / "muse_ui.h").read_text()
        for text in ("muse_ui_page_t;", "void muse_ui_bench_page(", "void muse_ui_bench_demo(", "muse_ui_touch_t;",
                     "void muse_ui_bench_touch("):
            with self.subTest(text):
                self.assertEqual(open_conditions(header, header.index(text)), ["if CONFIG_MUSE_BENCH"])
        self.assertRegex(header, r"MUSE_UI_TOUCH_TAP,\s*MUSE_UI_TOUCH_LONG,\s*MUSE_UI_TOUCH_SWIPE_LEFT,\s*"
                                 r"MUSE_UI_TOUCH_SWIPE_RIGHT")
        # Streaming a screenshot is not a bench option's: it stays declared for every build.
        self.assertEqual(open_conditions(header, header.index("void muse_ui_request_snapshot(void)")), [])
        ui = (MUSE / "muse_ui.c").read_text()
        for text in ("void muse_ui_bench_page(", "void muse_ui_bench_demo(", "void muse_ui_bench_touch("):
            with self.subTest(text):
                self.assertEqual(open_conditions(ui, ui.index(text)), ["if CONFIG_MUSE_BENCH"])
        uses = [m.start() for m in re.finditer(r"s_bench_(?:demo|page)", ui)]
        self.assertGreaterEqual(len(uses), 6)
        for index in uses:
            with self.subTest(ui[ui.rindex("\n", 0, index) + 1:ui.index("\n", index)].strip()):
                self.assertEqual(open_conditions(ui, index), ["if CONFIG_MUSE_BENCH"])

    def test_with_the_option_off_the_ui_is_as_before(self):
        # Every mention of the touch script, its wrapper and the install is inside #if CONFIG_MUSE_BENCH, and the
        # wrapper goes in after the display exists, under the display lock muse_ui_start() already holds.
        ui = (MUSE / "muse_ui.c").read_text()
        mentions = [m.start() for m in re.finditer(r"bench_touch_install|bench_read|s_real_read|s_touch\b|"
                                                   r"s_touch_request|BENCH_[A-Z_]+|muse_ui_bench_touch", ui)]
        self.assertGreaterEqual(len(mentions), 20)
        for index in mentions:
            with self.subTest(ui[ui.rindex("\n", 0, index) + 1:ui.index("\n", index)].strip()):
                self.assertEqual(open_conditions(ui, index), ["if CONFIG_MUSE_BENCH"])
        start = c_function(ui, "esp_err_t muse_ui_start(void)")
        self.assertLess(start.index("display_start(&s_indev)"), start.index("bench_touch_install();"))
        self.assertLess(start.index("display_lock(-1)"), start.index("bench_touch_install();"))
        self.assertLess(start.index("bench_touch_install();"), start.index("build_screen();"))

    def test_epaper_ui_answers_the_bench_calls(self):
        # FreeInk boards have no LVGL touch screen but link the same console: with the option on they get no-ops.
        eink = (MUSE / "eink/muse_eink_ui.cpp").read_text()
        for text in ('extern "C" void muse_ui_bench_page(', 'extern "C" void muse_ui_bench_demo(',
                     'extern "C" void muse_ui_bench_touch('):
            with self.subTest(text):
                self.assertEqual(open_conditions(eink, eink.index(text)), ["if CONFIG_MUSE_BENCH"])
        # They are the header's declarations' twins. Compiled as C against muse_ui.h, a signature that drifts from
        # the header fails here, not in a FreeInk build with the option on (no board.sh target builds one).
        anchor = eink.index('extern "C" void muse_ui_request_snapshot(void)')
        start = eink.index("#if CONFIG_MUSE_BENCH", anchor)
        stubs = eink[start:eink.index("\n#endif", start) + len("\n#endif")].replace('extern "C" ', "")
        program = '#include "muse_ui.h"\n' + stubs + r"""

int main(void)
{
    muse_ui_bench_page(MUSE_UI_PAGE_CLOCK);
    muse_ui_bench_demo(true);
    muse_ui_bench_touch(MUSE_UI_TOUCH_SWIPE_LEFT, 1, 2);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            done = apps_host.run(build_bench("eink_stubs", program, tmp))
        self.assertEqual((done.returncode, done.stdout), (0, ""), done.stderr)

    def run_bench_block(self, lines):
        """console_command's bench block, compiled on its own: what each line calls and prints."""
        source = (MUSE / "muse_input.c").read_text()
        command = c_function(source, "static bool console_command(char *line, bool whole)")
        start = command.index("#if CONFIG_MUSE_BENCH") + len("#if CONFIG_MUSE_BENCH\n")
        block = command[start:command.index("\n#endif", start)]
        program = r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_ui.h"
typedef void *TaskHandle_t;
typedef unsigned StackType_t;
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 4
static unsigned heap_caps_get_free_size(int caps) { return 1000u + (unsigned)caps; }
static unsigned heap_caps_get_minimum_free_size(int caps) { return 100u + (unsigned)caps; }
static unsigned heap_caps_get_largest_free_block(int caps) { return 10u + (unsigned)caps; }
static TaskHandle_t xTaskGetHandle(const char *name)
{
    return strcmp(name, "lvgl") && strcmp(name, "muse_serial") ? NULL : (TaskHandle_t)name;
}
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { return strcmp((const char *)task, "lvgl") ? 321 : 970; }
static const struct { int width, height; } board = { 466, 466 };
#define muse_board (&board)
void muse_ui_bench_page(muse_ui_page_t page) { printf("page %d\n", (int)page); }
void muse_ui_bench_demo(bool enabled) { printf("demo %d\n", enabled); }
void muse_ui_bench_touch(muse_ui_touch_t kind, int x, int y) { printf("touch %d %d %d\n", (int)kind, x, y); }

static bool bench_commands(char *line)
{
""" + block + r"""
    return false;
}

int main(void)
{
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        printf("[%s] taken=%d\n", line, bench_commands(line));
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            done = apps_host.run(build_bench("bench_block", program, tmp), input="\n".join(lines) + "\n")
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout.splitlines()

    def test_touch_and_page_commands_parse_and_validate(self):
        usage = "@touch.error usage: tap X Y | longpress X Y (X 0..465, Y 0..465) | swipe left|right"
        out = self.run_bench_block([
            "tap 233 300", "longpress 10 20", "tap  5   6", "swipe left", "swipe right", "tap 0 465",
            "ui=settings", "ui=face", "ui=clock", "ui.demo=1", "ui.demo=0",
            "tap 466 0", "tap 0 466", "tap -1 5", "tap 5", "tap ", "tap a b", "tap 1 2 3", "tap 1 2x", "tap 1 ",
            "tap 99999999999999999999 1", "longpress 1", "swipe up", "swipe ", "swipe leftx", "swipe left ",
            "tapx 1 2", "swipes left", "taps", "swipe", "other",
        ])
        self.assertEqual(out, [
            "touch 0 233 300", "[tap 233 300] taken=1",
            "touch 1 10 20", "[longpress 10 20] taken=1",
            "touch 0 5 6", "[tap  5   6] taken=1",
            "touch 2 0 0", "[swipe left] taken=1",
            "touch 3 0 0", "[swipe right] taken=1",
            "touch 0 0 465", "[tap 0 465] taken=1",
            "page 1", "[ui=settings] taken=1",
            "page 0", "[ui=face] taken=1",
            "page 2", "[ui=clock] taken=1",
            "demo 1", "[ui.demo=1] taken=1",
            "demo 0", "[ui.demo=0] taken=1",
            *[x for line in ("tap 466 0", "tap 0 466", "tap -1 5", "tap 5", "tap ", "tap a b", "tap 1 2 3", "tap 1 2x",
                             "tap 1 ", "tap 99999999999999999999 1", "longpress 1", "swipe up", "swipe ",
                             "swipe leftx", "swipe left ")
              for x in (usage, f"[{line}] taken=1")],
            "[tapx 1 2] taken=0", "[swipes left] taken=0", "[taps] taken=0", "[swipe] taken=0", "[other] taken=0",
        ])

    def test_mem_stays_in_the_block(self):
        out = self.run_bench_block(["mem"])
        self.assertEqual(len(out), 2, out)
        # Each task's unused stack, in bytes: the LVGL task's (970 words of 4 bytes here) and muse_serial's, the task
        # that runs the console and so every >app.* command (Task 15 reads it after a session of pushes).
        self.assertEqual(out[0], '@mem {"int_free":1001,"int_min":101,"int_largest":11,"dma_free":1002,'
                                 '"psram_free":1004,"psram_min":104,"lvgl_stack_free":3880,"serial_stack_free":1284}')
        self.assertEqual(out[1], "[mem] taken=1")

    # ---- Task 14: touch scripts through LVGL's read callback ----

    def run_touch_harness(self, scenario):
        source = (MUSE / "muse_ui.c").read_text()
        start = source.index("/*\n * Bench touch")
        block = source[start:source.index("\n#endif", start)]
        program = r"""
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_ui.h"

typedef struct { int32_t x, y; } lv_point_t;
typedef enum { LV_INDEV_STATE_RELEASED = 0, LV_INDEV_STATE_PRESSED } lv_indev_state_t;
typedef struct { lv_indev_state_t state; lv_point_t point; } lv_indev_data_t;
typedef struct lv_indev lv_indev_t;
typedef void (*lv_indev_read_cb_t)(lv_indev_t *indev, lv_indev_data_t *data);
typedef enum { LV_INDEV_MODE_NONE = 0, LV_INDEV_MODE_TIMER, LV_INDEV_MODE_EVENT } lv_indev_mode_t;

static lv_indev_read_cb_t g_cb;
static lv_indev_mode_t g_mode = LV_INDEV_MODE_TIMER;
static uint32_t g_tick = 5000;
__attribute__((unused)) static lv_indev_mode_t lv_indev_get_mode(lv_indev_t *indev) { (void)indev; return g_mode; }
static lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t *indev) { (void)indev; return g_cb; }
static void lv_indev_set_read_cb(lv_indev_t *indev, lv_indev_read_cb_t cb) { (void)indev; g_cb = cb; }
static uint32_t lv_tick_get(void) { return g_tick; }
static uint32_t lv_tick_elaps(uint32_t prev) { return g_tick - prev; }
#define ESP_LOGW(tag, ...) do { printf("W %s: ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
static const char *TAG = "muse_ui";
static int s_w = 466, s_h = 466;
static lv_indev_t *s_indev;

static int real_calls;
static void real_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    real_calls++;
    data->state = LV_INDEV_STATE_PRESSED;   /* a finger on the glass at (7, 9) */
    data->point.x = 7;
    data->point.y = 9;
}

""" + block + r"""

/* One read, the way LVGL makes it: the data starts zeroed. */
static lv_indev_data_t read_now(void)
{
    lv_indev_data_t d;
    memset(&d, 0, sizeof d);
    g_cb(NULL, &d);
    return d;
}

static void show(const char *what, lv_indev_data_t d, uint32_t t0)
{
    printf(" %s%c%d,%d@%u", what, d.state == LV_INDEV_STATE_PRESSED ? 'P' : 'R', (int)d.point.x, (int)d.point.y,
           (unsigned)(g_tick - t0));
}

/* Reads every `step` ms from the first read of the script until its release; then one more read. */
static void play(const char *label, int step)
{
    real_calls = 0;
    uint32_t t0 = g_tick;
    printf("%s:", label);
    for (int i = 0; i < 400; i++) {
        lv_indev_data_t d = read_now();
        show("", d, t0);
        if (d.state == LV_INDEV_STATE_RELEASED) {
            break;
        }
        g_tick += (uint32_t)step;
    }
    printf(" real=%d\n", real_calls);
    g_tick += (uint32_t)step;
    lv_indev_data_t after = read_now();
    show("after:", after, t0);
    printf(" real=%d\n", real_calls);
    g_tick += 1000;
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    int step = argc > 2 ? atoi(argv[2]) : 15;
    s_indev = (lv_indev_t *)&g_tick;
    g_cb = strcmp(what, "no_driver") ? real_read : NULL;
    g_mode = strcmp(what, "event_mode") ? LV_INDEV_MODE_TIMER : LV_INDEV_MODE_EVENT;
    bench_touch_install();
    if (!strcmp(what, "no_driver")) {
        printf("installed=%d\n", g_cb != NULL);
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 1, 1);
        return 0;
    }
    if (!strcmp(what, "event_mode")) {
        printf("installed=%d\n", g_cb != real_read);
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 1, 1);
        real_calls = 0;
        lv_indev_data_t d = read_now();
        printf("read:");
        show("", d, g_tick);
        printf(" real=%d\n", real_calls);
        return 0;
    }
    printf("installed=%d\n", g_cb != real_read);
    real_calls = 0;
    lv_indev_data_t idle = read_now();
    printf("idle:");
    show("", idle, g_tick);
    printf(" real=%d\n", real_calls);
    if (!strcmp(what, "tap")) {
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 233, 300);
        play("tap", step);
    } else if (!strcmp(what, "long")) {
        muse_ui_bench_touch(MUSE_UI_TOUCH_LONG, 233, 233);
        play("long", step);
    } else if (!strcmp(what, "swipe_left")) {
        muse_ui_bench_touch(MUSE_UI_TOUCH_SWIPE_LEFT, 0, 0);
        play("swipe_left", step);
    } else if (!strcmp(what, "swipe_right")) {
        muse_ui_bench_touch(MUSE_UI_TOUCH_SWIPE_RIGHT, 99, 99);
        play("swipe_right", step);
    } else if (!strcmp(what, "clamp")) {
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 9999, -5);
        play("clamp", step);
    } else if (!strcmp(what, "invalid")) {
        muse_ui_bench_touch((muse_ui_touch_t)9, 1, 1);
        real_calls = 0;
        uint32_t t0 = g_tick;
        lv_indev_data_t d = read_now();
        printf("invalid:");
        show("", d, t0);
        printf(" real=%d\n", real_calls);
    } else if (!strcmp(what, "queued")) {
        /* A request that comes in mid-script waits for the release, and the very next read starts it. */
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 100, 100);
        real_calls = 0;
        uint32_t t0 = g_tick;
        printf("tap:");
        show("", read_now(), t0);
        g_tick += (uint32_t)step;
        muse_ui_bench_touch(MUSE_UI_TOUCH_SWIPE_LEFT, 0, 0);
        muse_ui_bench_touch(MUSE_UI_TOUCH_LONG, 50, 60);   /* the later request replaces the earlier one */
        for (int i = 0; i < 400; i++) {
            lv_indev_data_t d = read_now();
            show("", d, t0);
            if (d.state == LV_INDEV_STATE_RELEASED) {
                break;
            }
            g_tick += (uint32_t)step;
        }
        printf(" real=%d\n", real_calls);
        g_tick += (uint32_t)step;
        t0 = g_tick;
        printf("then:");
        for (int i = 0; i < 400; i++) {
            lv_indev_data_t d = read_now();
            show("", d, t0);
            if (d.state == LV_INDEV_STATE_RELEASED) {
                break;
            }
            g_tick += (uint32_t)step;
        }
        printf(" real=%d\n", real_calls);
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = build_bench("bench_touch", program, tmp)
            cases = {name: apps_host.run(exe, *args) for name, args in scenario.items()}
        for name, done in cases.items():
            self.assertEqual(done.returncode, 0, f"{name}: {done.stderr}")
        return {name: done.stdout.splitlines() for name, done in cases.items()}

    @staticmethod
    def reads(line):
        """[(state, x, y, ms)] from a harness line such as 'tap: P233,300@0 ... R233,300@90 real=0'."""
        return [(m[0], int(m[1]), int(m[2]), int(m[3]))
                for m in re.findall(r"([PR])(\d+),(\d+)@(\d+)", line.split("after:")[0])]

    def test_touch_scripts_play_through_the_read_callback(self):
        out = self.run_touch_harness({
            "tap": ("tap", 15), "tap_fast": ("tap", 1), "long": ("long", 15), "left": ("swipe_left", 15),
            "left_slow": ("swipe_left", 100), "right": ("swipe_right", 15), "clamp": ("clamp", 15),
            "invalid": ("invalid", 15),
        })
        # No request: the real touch is read, as it is (a finger at 7,9), and the wrapper is in place.
        self.assertEqual(out["tap"][:2], ["installed=1", "idle: P7,9@0 real=1"])
        # A tap presses for 80 ms: reads at 0..75 press, the first read at or after 80 releases, at the same point.
        tap = self.reads(out["tap"][2])
        self.assertEqual(tap, [("P", 233, 300, t) for t in range(0, 80, 15)] + [("R", 233, 300, 90)])
        self.assertTrue(out["tap"][2].endswith("real=0"))   # the real touch isn't read while the script runs
        self.assertEqual(out["tap"][3], " after:P7,9@105 real=1")   # and is again right after the release
        fast = self.reads(out["tap_fast"][2])
        self.assertEqual((fast[0], fast[-2], fast[-1]), (("P", 233, 300, 0), ("P", 233, 300, 79), ("R", 233, 300, 80)))
        # A long press holds 900 ms, past LVGL's 400 ms long-press time with room for a busy frame.
        long = self.reads(out["long"][2])
        self.assertEqual((long[0], long[-2], long[-1]),
                         (("P", 233, 233, 0), ("P", 233, 233, 885), ("R", 233, 233, 900)))
        self.assertTrue(all(r[:3] == ("P", 233, 233) for r in long[:-1]))
        # A left swipe starts at the right and goes left along y=233 for 240 ms, then lets go at the end (a right swipe
        # the other way; test_a_swipe_moves_on_every_read_and_fires_lvgls_gesture has the rest).
        left = self.reads(out["left"][2])
        self.assertEqual((left[0], left[-1]), (("P", 406, 233, 0), ("R", 60, 233, 240)))
        self.assertTrue(all(r[2] == 233 for r in left))
        slow = self.reads(out["left_slow"][2])   # a read every 100 ms: still ends where it should, at the end
        self.assertEqual((slow[0], slow[-1]), (("P", 406, 233, 0), ("R", 60, 233, 300)))
        right = self.reads(out["right"][2])
        self.assertEqual((right[0], right[-1]), (("P", 60, 233, 0), ("R", 406, 233, 240)))
        # Out of the screen is clamped to its edge; a kind that doesn't exist is dropped (the real touch goes on).
        self.assertEqual(self.reads(out["clamp"][2])[0], ("P", 465, 0, 0))
        self.assertEqual(out["invalid"][2], "invalid: P7,9@0 real=1")

    @staticmethod
    def lvgl_gesture(reads):
        """What LVGL 9.5 makes of a pointer's reads (lv_indev.c: indev_proc_press and indev_gesture): the vector is the
        move since the read before, the first read of a press has none, a read that moves less than the minimum
        velocity (3 px) forgets the distance so far, and the gesture fires once the distance passes 50 px. -> the
        direction and the read's ms, or None. (The points stay on y=233, so only x counts.)"""
        last, total = None, 0
        for state, x, _, ms in reads:
            if state != "P":
                return None   # a release isn't processed as a press
            vect = 0 if last is None else x - last
            last = x
            if abs(vect) < 3:
                total = 0
            total += vect
            if abs(total) > 50:
                return ("left" if total < 0 else "right"), ms
        return None

    def test_a_swipe_moves_on_every_read_and_fires_lvgls_gesture(self):
        # S6: the swipe's position follows the clock on every read, so no read sees the finger standing still, whatever
        # LV_DEF_REFR_PERIOD is. LVGL forgets a gesture's distance on a read that moves under 3 px, and a 43 px step
        # on a 26.7 ms clock left every other read standing still at 15 ms, and every one but the odd one at 10.
        periods = (5, 10, 15, 20, 33, 100)
        out = self.run_touch_harness({f"{d}_{p}": (f"swipe_{d}", p) for d in ("left", "right") for p in periods})
        for p in periods:
            for d, start, end in (("left", 406, 60), ("right", 60, 406)):
                with self.subTest(direction=d, period=p):
                    reads = self.reads(out[f"{d}_{p}"][2])
                    pressed, release = reads[:-1], reads[-1]
                    # released at the end, on the first read at or after 240 ms
                    self.assertEqual(release, ("R", end, 233, -(-240 // p) * p))
                    self.assertTrue(all(r[2] == 233 for r in reads))
                    delta = end - start   # x is the clock's share of the way, truncated as C divides
                    self.assertEqual([r[1] for r in pressed],
                                     [start + (delta * r[3] // 240 if delta > 0 else -(-delta * r[3] // 240))
                                      for r in pressed])
                    xs = [r[1] for r in pressed]
                    self.assertTrue(all((b - a) * delta > 0 for a, b in zip(xs, xs[1:])), xs)   # every read moves
                    self.assertEqual(self.lvgl_gesture(reads)[0], d)

    def test_a_touch_input_in_event_mode_gets_no_wrapper_and_says_why(self):
        # S7: in LV_INDEV_MODE_EVENT LVGL reads the touch only when its driver says so, so a script couldn't advance
        # (and a stored request would play at the next real touch). No wrapper goes in, the log says why once, and a
        # request says there is nothing to drive. The real touch is read as always.
        out = self.run_touch_harness({"event": ("event_mode",)})["event"]
        self.assertRegex(out[0], r"^W muse_ui: bench touch: .*event mode")
        self.assertEqual(out[1], "installed=0")
        self.assertRegex(out[2], r"^W muse_ui: .*no touch")
        self.assertEqual(out[3], "read: P7,9@0 real=1")
        self.assertEqual(sum("event mode" in line for line in out), 1)

    def test_a_request_in_mid_script_waits_and_the_latest_wins(self):
        out = self.run_touch_harness({"queued": ("queued", 15)})["queued"]
        tap, then = out[2], out[3]
        # The tap plays out (it was 15 ms in when the others came) and the real touch isn't read in between.
        self.assertTrue(tap.startswith("tap: P100,100@0"), tap)
        self.assertEqual(self.reads(tap)[-1], ("R", 100, 100, 90))
        self.assertTrue(tap.endswith("real=0"))
        # The next read starts the long press that replaced the swipe, at once, and holds it its 900 ms.
        self.assertTrue(then.startswith("then: P50,60@0"), then)
        self.assertEqual(self.reads(then)[-1], ("R", 50, 60, 900))
        self.assertTrue(all(r[:3] == ("P", 50, 60) for r in self.reads(then)[:-1]))
        self.assertTrue(then.endswith("real=0"))

    def test_no_touch_driver_means_no_wrapper(self):
        out = self.run_touch_harness({"none": ("no_driver",)})["none"]
        self.assertEqual(out[0], "installed=0")
        self.assertRegex(out[1], r"^W muse_ui: .*no touch")

    # ---- Task 14: the screenshot stream's CRC ----

    def test_snap_crc_matches_zlib(self):
        source = (MUSE / "muse_ui.c").read_text()
        program = "\n".join([
            "#include <stddef.h>", "#include <stdint.h>", "#include <stdio.h>", "#include <stdlib.h>",
            c_function(source, "static uint32_t snap_crc32("),
            "int main(int argc, char **argv)", "{",
            "    size_t chunk = argc > 1 ? (size_t)atoi(argv[1]) : 0;",
            "    static uint8_t data[4096];",
            "    size_t n = fread(data, 1, sizeof data, stdin);",
            "    uint32_t crc = 0;",
            "    if (chunk == 0) {",
            "        chunk = n ? n : 1;",
            "    }",
            "    for (size_t at = 0; at < n; at += chunk) {",
            "        crc = snap_crc32(crc, data + at, n - at < chunk ? n - at : chunk);",
            "    }",
            "    if (n == 0) {",
            "        crc = snap_crc32(crc, data, 0);",
            "    }",
            '    printf("%08x\\n", (unsigned)crc);',
            "    return 0;", "}",
        ])
        rng = random.Random(14)
        samples = [b"", b"123456789", rng.randbytes(1000), bytes(1000), b"\xff" * 1000, rng.randbytes(4096)]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snap_crc.c"
            path.write_text(program)
            exe = apps_host.build("snap_crc", [path], tmp)
            for data in samples:
                # In one call, and in pieces the way send_snapshot() feeds it (144 bytes, then any other size).
                for chunk in (0, 1, 144, 999):
                    with self.subTest(length=len(data), chunk=chunk):
                        done = apps_host.run(exe, chunk, input=data, text=False)
                        self.assertEqual(done.returncode, 0, done.stderr)
                        self.assertEqual(done.stdout.decode().strip(), f"{zlib.crc32(data):08x}")
            # The standard check value for CRC-32, from the firmware's code and not from zlib.
            done = apps_host.run(exe, 0, input=b"123456789", text=False)
            self.assertEqual(done.stdout.decode().strip(), "cbf43926")

    # The console as send_snapshot() sees it, for a stream read back by the tool's parser (hold, release, write and
    # flush do nothing but write to stdout).
    SNAPSHOT_CONSOLE = r"""
static UNUSED void muse_console_write(const void *buf, size_t n) { fwrite(buf, 1, n, stdout); }
static UNUSED void muse_console_hold(void) {}
static UNUSED void muse_console_release(void) {}
static UNUSED bool muse_console_write_timeout(const void *buf, size_t n, uint32_t ms)
{
    (void)ms;
    fwrite(buf, 1, n, stdout);
    return true;
}
static UNUSED bool muse_console_flush(uint32_t ms) { (void)ms; return true; }
"""

    @staticmethod
    def snapshot_functions(source):
        """What muse_ui.c's screenshot stream is made of, as C: its limits, the CRC, snap_write(), send_snapshot()."""
        return "\n".join([snap_defines(source), c_function(source, "static uint32_t snap_crc32("),
                          c_function(source, "static bool snap_write("),
                          c_function(source, "static void send_snapshot(void)")])

    def test_send_snapshot_streams_what_snap_parses(self):
        # The firmware's own stream (send_snapshot, its chunks and its SNAP END line) read by the tool's parser.
        snap = load_snap()
        source = (MUSE / "muse_ui.c").read_text()
        program = SNAPSHOT_PRELUDE + self.SNAPSHOT_CONSOLE + self.snapshot_functions(source) + r"""

int main(int argc, char **argv)
{
    if (argc < 5) {
        return 2;
    }
    make_picture((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), argv[4]);
    send_snapshot();
    free(g_buf.data);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snapshot_stream.c"
            path.write_text(program)
            exe = apps_host.build("snapshot_stream", [path], tmp, defines=["LV_USE_SNAPSHOT=1"])
            for w, h, pad in ((466, 3, 0), (200, 5, 8), (7, 4, 2), (1, 1, 0)):
                with self.subTest(w=w, h=h, pad=pad):
                    dump = Path(tmp) / f"dump_{w}_{h}"
                    done = apps_host.run(exe, w, h, pad, dump, text=False)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    log = done.stdout.decode("latin1")
                    self.assertTrue(log.startswith(f"\nSNAP BEGIN {w} {h} 144\n"), log[:40])
                    self.assertRegex(log, r"\n\nSNAP END [0-9a-f]{8}\n$")   # END has a newline of its own, as BEGIN has
                    got_w, got_h, raw, ok = snap.parse(log)
                    self.assertEqual((got_w, got_h), (w, h))
                    self.assertEqual(raw, dump.read_bytes())
                    self.assertTrue(ok)
                    self.assertFalse(snap.parse(flip_first_data_char(log))[3])

    # The console shared by two writers. While a screenshot streams, text printed with printf or the log by another
    # task must wait: the harness runs the real send_snapshot() and the real muse_console_hold(), a task that prints
    # one line in the middle of the stream's third row (a call of the stream's write), a task that has stdout when the
    # stream starts, and a "cable" that records what arrives in what order. events: L the hold taken, W a row written,
    # w a write that gave up, F the flush, f a flush that timed out, U the hold given back.
    SNAPSHOT_CABLE = r"""
static char g_events[8192];
static size_t g_nevents;
static int g_calls, g_inject_at, g_vanish_at = -1, g_flush_ok = 1, g_bad_ms, g_log_blocked = -1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static int g_go, g_done, g_held;

static void event(char c)
{
    if (g_nevents + 1 < sizeof g_events) {
        g_events[g_nevents++] = c;
    }
}

static void put(const void *buf, size_t n)
{
    for (size_t off = 0; off < n;) {
        ssize_t r = write(1, (const char *)buf + off, n - off);
        if (r <= 0) {
            abort();
        }
        off += (size_t)r;
    }
}

/* Another task: prints one line the moment it is told to, with printf and a flush, as a log line is. */
static void *log_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mutex);
    while (!g_go) {
        pthread_cond_wait(&g_cond, &g_mutex);
    }
    pthread_mutex_unlock(&g_mutex);
    printf("I (1) app: LOG LINE\n");
    fflush(stdout);
    pthread_mutex_lock(&g_mutex);
    g_done = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

/* Tells that task to print and waits up to 300 ms for it: it can't finish while the console is held. */
static void trigger_log_and_wait(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 300 * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&g_mutex);
    g_go = 1;
    pthread_cond_broadcast(&g_cond);
    while (!g_done) {
        if (pthread_cond_timedwait(&g_cond, &g_mutex, &until) != 0) {
            break;
        }
    }
    g_log_blocked = !g_done;
    pthread_mutex_unlock(&g_mutex);
}

/* A task in the middle of printing when the stream starts: it has stdout for 100 ms, prints its line, lets go. */
static void *holder_thread(void *arg)
{
    (void)arg;
    (flockfile)(stdout);
    pthread_mutex_lock(&g_mutex);
    g_held = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    usleep(100 * 1000);
    fputs("I (1) app: HOLDER LINE\n", stdout);
    fflush(stdout);
    (funlockfile)(stdout);
    return NULL;
}

/* The hold and its release are the host's flockfile and funlockfile, as the board's libc has them, noted. */
static UNUSED void harness_lock(FILE *f)
{
    (flockfile)(f);
    event('L');
}
static UNUSED void harness_unlock(FILE *f)
{
    event('U');
    (funlockfile)(f);
}
#define flockfile(f) harness_lock(f)
#define funlockfile(f) harness_unlock(f)

/* The cable: a write of the stream; the one that is the injected call gets the other task's line in the middle. */
static UNUSED void muse_console_write(const void *buf, size_t n)
{
    event('W');
    g_calls++;
    if (g_calls == g_inject_at) {
        put(buf, n / 2);
        trigger_log_and_wait();
        put((const char *)buf + n / 2, n - n / 2);
    } else {
        put(buf, n);
    }
}
static UNUSED bool muse_console_write_timeout(const void *buf, size_t n, uint32_t ms)
{
    if (ms == 0 || ms > 10000) {
        g_bad_ms++;   /* every wait is bounded, and not by much */
    }
    if (g_vanish_at >= 0 && g_calls >= g_vanish_at) {
        event('w');   /* a host that has stopped reading: the console takes nothing */
        return false;
    }
    muse_console_write(buf, n);
    return true;
}
static UNUSED bool muse_console_flush(uint32_t ms)
{
    if (ms == 0 || ms > 10000) {
        g_bad_ms++;
    }
    event(g_flush_ok ? 'F' : 'f');
    return g_flush_ok;
}
"""

    SNAPSHOT_CABLE_MAIN = r"""
int main(int argc, char **argv)
{
    /* w h pad dump inject_at vanish_at flush_ok holder */
    if (argc < 9) {
        return 2;
    }
    alarm(20);   /* a hold never given back leaves the other task waiting for ever: the run ends, and the test fails */
    make_picture((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), argv[4]);
    g_inject_at = atoi(argv[5]);
    g_vanish_at = atoi(argv[6]);
    g_flush_ok = atoi(argv[7]);
    int holder = atoi(argv[8]);
    pthread_t log_task, holder_task;
    if (g_inject_at) {
        pthread_create(&log_task, NULL, log_thread, NULL);
    }
    if (holder) {
        pthread_create(&holder_task, NULL, holder_thread, NULL);
        pthread_mutex_lock(&g_mutex);
        while (!g_held) {
            pthread_cond_wait(&g_cond, &g_mutex);
        }
        pthread_mutex_unlock(&g_mutex);
    }
    send_snapshot();
    if (holder) {
        pthread_join(holder_task, NULL);
    }
    if (g_inject_at) {
        pthread_join(log_task, NULL);
    }
    g_events[g_nevents] = 0;
    fprintf(stderr, "events=%s blocked=%d bad_ms=%d\n", g_events, g_log_blocked, g_bad_ms);
    free(g_buf.data);
    return 0;
}
"""

    def run_snapshot_cable(self, *, inject_at=0, vanish_at=-1, flush_ok=1, holder=0, hold=True, console="usb",
                           w=200, h=6, pad=8):
        """send_snapshot() on the shared cable: (what arrived on the cable, the raw pixels, the harness's report as
        a dict, the warnings it logged). hold=False takes the muse_console_hold() call out, as a stream without it;
        console says whose muse_console_hold() and muse_console_release() run: muse_console.c's "usb" or "uart" one."""
        ui = (MUSE / "muse_ui.c").read_text()
        console = console_variant((MUSE / "muse_console.c").read_text(), console)
        functions = self.snapshot_functions(ui)
        if not hold:
            # The stream without the lock: no hold taken, so none given back.
            self.assertIn("muse_console_hold();", functions)
            self.assertIn("muse_console_release();", functions)
            functions = functions.replace("muse_console_hold();", ";").replace("muse_console_release();", ";")
        program = SNAPSHOT_PRELUDE + self.SNAPSHOT_CABLE + "\n".join([
            c_function(console, "void muse_console_hold("), c_function(console, "void muse_console_release(")
        ]) + "\n" + functions + self.SNAPSHOT_CABLE_MAIN
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snapshot_cable.c"
            path.write_text(program)
            exe = apps_host.build("snapshot_cable", [path], tmp, defines=["LV_USE_SNAPSHOT=1"])
            dump = Path(tmp) / "dump"
            done = apps_host.run(exe, w, h, pad, dump, inject_at, vanish_at, flush_ok, holder, text=False)
            self.assertEqual(done.returncode, 0, done.stderr)
            stderr = done.stderr.decode("latin1")
            report = dict(re.findall(r"(\w+)=(\S*)", stderr.splitlines()[-1]))
            warnings = [line for line in stderr.splitlines() if line.startswith("W ")]
            return done.stdout.decode("latin1"), dump.read_bytes(), report, warnings

    def test_the_stream_holds_the_console_so_another_tasks_log_waits_for_the_end(self):
        snap = load_snap()
        w, h = 200, 6
        rows = h * -(-w * 2 // 144)
        # Another task prints one line in the middle of the third row: it can't (it waits for the console), and its
        # line comes out after END, with the stream whole.
        log, raw, report, warnings = self.run_snapshot_cable(inject_at=4)
        self.assertEqual(report["blocked"], "1")
        self.assertEqual(log.count("LOG LINE"), 1)
        self.assertGreater(log.index("LOG LINE"), log.index("SNAP END"))
        self.assertEqual(snap.parse(log), (w, h, raw, True))
        self.assertTrue(log.endswith("I (1) app: LOG LINE\n"))
        # What the stream did: took the hold, wrote the header, every row and the END line, flushed, gave the hold back.
        self.assertEqual(report["events"], "L" + "W" * (rows + 2) + "FU")
        self.assertEqual((report["bad_ms"], warnings), ("0", []))
        # With nobody else writing it is the same stream.
        log, raw, report, warnings = self.run_snapshot_cable()
        self.assertEqual(report["events"], "L" + "W" * (rows + 2) + "FU")
        self.assertEqual(snap.parse(log), (w, h, raw, True))

    def test_without_the_hold_another_tasks_log_lands_inside_the_stream(self):
        # The same harness with the hold taken out of send_snapshot(): the line goes through, inside a row, and the
        # CRC fails. So the test above has teeth: it fails for a stream that doesn't hold the console.
        snap = load_snap()
        log, raw, report, _ = self.run_snapshot_cable(inject_at=4, hold=False)
        self.assertEqual(report["blocked"], "0")
        self.assertNotIn("L", report["events"])
        self.assertLess(log.index("SNAP BEGIN"), log.index("LOG LINE"))
        self.assertLess(log.index("LOG LINE"), log.index("SNAP END"))
        self.assertFalse(snap.parse(log)[3])

    def test_the_stream_stops_and_lets_go_when_the_host_stops_reading(self):
        # The console takes nothing after the 5th write (the host has gone): the stream stops there, with no END and
        # no flush, the hold is given back, and the log says why.
        log, raw, report, warnings = self.run_snapshot_cable(vanish_at=5)
        self.assertEqual(report["events"], "L" + "W" * 5 + "wU")
        self.assertEqual(report["bad_ms"], "0")
        self.assertNotIn("SNAP END", log)
        self.assertEqual(len(warnings), 1)
        self.assertRegex(warnings[0], r"snapshot.*(stopped|took nothing)")
        # Gone before the first byte: nothing was written, and the hold is still given back.
        log, raw, report, warnings = self.run_snapshot_cable(vanish_at=0)
        self.assertEqual(report["events"], "LwU")
        self.assertEqual(log, "")

    def test_the_hold_is_given_back_when_the_last_bytes_do_not_leave(self):
        w, h = 200, 6
        rows = h * -(-w * 2 // 144)
        log, raw, report, warnings = self.run_snapshot_cable(flush_ok=0)
        self.assertEqual(report["events"], "L" + "W" * (rows + 2) + "fU")
        self.assertEqual(len(warnings), 1)
        self.assertRegex(warnings[0], r"snapshot.*(drain|left)")
        self.assertTrue(log.endswith("\n"))

    def test_the_stream_waits_for_a_task_that_has_the_console_and_then_goes_whole(self):
        snap = load_snap()
        w, h = 200, 6
        rows = h * -(-w * 2 // 144)
        # Another task has stdout when the stream starts and prints its line before it lets go: the stream's hold
        # waits for that, so the line comes out before BEGIN and the stream is whole.
        log, raw, report, warnings = self.run_snapshot_cable(holder=1)
        self.assertEqual(log.count("HOLDER LINE"), 1)
        self.assertLess(log.index("HOLDER LINE"), log.index("SNAP BEGIN"))
        self.assertEqual(report["events"], "L" + "W" * (rows + 2) + "FU")
        self.assertEqual(snap.parse(log[log.index("\nSNAP BEGIN"):]), (w, h, raw, True))
        self.assertEqual((report["bad_ms"], warnings), ("0", []))
        # The same harness without the hold: the stream doesn't wait, and the line comes after it.
        log, raw, report, _ = self.run_snapshot_cable(holder=1, hold=False)
        self.assertLess(log.index("SNAP END"), log.index("HOLDER LINE"))

    def test_a_uart_console_streams_without_the_hold(self):
        # The hold applies where the stream goes out over USB-Serial-JTAG. A UART's stream lasts as long as its bytes
        # take at the baud rate (about 40 s for the Watcher's screen at 115200), and for that long no task could log,
        # Link and the voice pipeline among them. There the stream goes out as it did before the hold: nothing is taken
        # or given back, and snap.py retakes a capture the log broke into. Same send_snapshot(), the UART's hold.
        snap = load_snap()
        w, h = 200, 6
        rows = h * -(-w * 2 // 144)
        log, raw, report, warnings = self.run_snapshot_cable(console="uart")
        self.assertEqual(report["events"], "W" * (rows + 2) + "F")   # the rows, END and the flush; no L, no U
        self.assertEqual(snap.parse(log), (w, h, raw, True))
        self.assertEqual((report["bad_ms"], warnings), ("0", []))
        # Another task's line isn't kept out: it prints at once, inside the stream.
        log, raw, report, _ = self.run_snapshot_cable(console="uart", inject_at=4)
        self.assertEqual(report["blocked"], "0")
        self.assertLess(log.index("SNAP BEGIN"), log.index("LOG LINE"))
        self.assertLess(log.index("LOG LINE"), log.index("SNAP END"))
        # A task that has stdout doesn't delay it either: the stream is over before that task is.
        log, raw, report, _ = self.run_snapshot_cable(console="uart", holder=1)
        self.assertLess(log.index("SNAP END"), log.index("HOLDER LINE"))
        self.assertEqual(report["events"], "W" * (rows + 2) + "F")
        # And the same run on the USB console's hold does hold: the harness tells the two apart.
        log, raw, report, _ = self.run_snapshot_cable(console="usb", inject_at=4)
        self.assertEqual(report["blocked"], "1")
        # The UART's hold and release are empty, and take no lock.
        uart = console_variant((MUSE / "muse_console.c").read_text(), "uart")
        for name in ("void muse_console_hold(", "void muse_console_release("):
            with self.subTest(name):
                body = code_only(c_function(uart, name))
                self.assertNotIn("lockfile", body)
                self.assertEqual(body.split("{")[1].split("}")[0].strip(), "")

    def test_the_limits_of_the_stream_are_finite(self):
        ui = (MUSE / "muse_ui.c").read_text()
        limits = {name: int(value) for name, value in re.findall(r"^#define (SNAP_\w+_MS) (\d+)", ui, re.M)}
        self.assertEqual(sorted(limits), ["SNAP_DRAIN_MS", "SNAP_WRITE_MS"])
        for name, value in limits.items():
            with self.subTest(name):
                self.assertTrue(0 < value <= 5000, value)   # seconds, not minutes, and not zero
        # Everything the stream waits for is one of the two: its writes, through snap_write(), and its flush. The plain
        # muse_console_write() waits for ever.
        stream = c_function(ui, "static void send_snapshot(void)")
        stream = stream[:stream.index("#else")]
        self.assertNotIn("muse_console_write(", stream)
        self.assertIn("muse_console_flush(SNAP_DRAIN_MS)", stream)
        self.assertIn("muse_console_write_timeout(data, n, SNAP_WRITE_MS)", c_function(ui, "static bool snap_write("))

    # The two consoles' side of it: a write that gives up and a flush that waits, each for at most ms. The functions are
    # muse_console.c's own text, compiled against stand-ins for the driver's calls.
    CONSOLE_STUBS = {
        "usb": r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms) / portTICK_PERIOD_MS)
static int g_takes = 1;
static uint32_t g_ticks;
static int usb_serial_jtag_write_bytes(const void *src, size_t size, uint32_t ticks)
{
    (void)src;
    g_ticks = ticks;
    return g_takes == 2 ? (int)size - 1 : g_takes ? (int)size : 0;   /* the whole write, none of it, or less */
}
static esp_err_t usb_serial_jtag_wait_tx_done(uint32_t ticks)
{
    g_ticks = ticks;
    return g_takes ? ESP_OK : ESP_ERR_TIMEOUT;
}
#define PORT_NOTE 0
""",
        "uart": r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms) / portTICK_PERIOD_MS)
#define CONFIG_ESP_CONSOLE_UART_NUM 1
#define PORT CONFIG_ESP_CONSOLE_UART_NUM
static int g_takes = 1;
static uint32_t g_ticks;
static int g_port = -1;
static int uart_write_bytes(int port, const void *src, size_t size)
{
    (void)src;
    g_port = port;
    return g_takes == 2 ? (int)size - 1 : g_takes ? (int)size : -1;
}
static esp_err_t uart_wait_tx_done(int port, uint32_t ticks)
{
    g_port = port;
    g_ticks = ticks;
    return g_takes ? ESP_OK : ESP_ERR_TIMEOUT;
}
#define PORT_NOTE g_port
""",
    }
    CONSOLE_MAIN = r"""
int main(void)
{
    for (int takes = 1; takes >= 0; takes--) {
        g_takes = takes;
        g_ticks = 0;
        bool wrote = muse_console_write_timeout("hello", 5, 250);
        printf("write takes=%d ok=%d ticks=%u port=%d\n", takes, wrote, (unsigned)g_ticks, PORT_NOTE);
        g_ticks = 0;
        bool wrote_now = muse_console_write_timeout("hello", 5, 0);
        printf("write0 takes=%d ok=%d ticks=%u\n", takes, wrote_now, (unsigned)g_ticks);
        g_ticks = 0;
        bool flushed = muse_console_flush(300);
        printf("flush takes=%d ok=%d ticks=%u port=%d\n", takes, flushed, (unsigned)g_ticks, PORT_NOTE);
    }
    g_takes = 2;
    printf("short ok=%d\n", muse_console_write_timeout("hello", 5, 250));
    return 0;
}
"""

    def test_each_console_writes_with_a_timeout_and_flushes_with_one(self):
        console = (MUSE / "muse_console.c").read_text()
        for variant in ("usb", "uart"):
            with self.subTest(variant), tempfile.TemporaryDirectory() as tmp:
                part = console_variant(console, variant)
                program = self.CONSOLE_STUBS[variant] + "\n".join(
                    [c_function(part, "bool muse_console_write_timeout("), c_function(part, "bool muse_console_flush(")]
                ) + self.CONSOLE_MAIN
                path = Path(tmp) / f"console_{variant}.c"
                path.write_text(program)
                exe = apps_host.build(f"console_{variant}", [path], tmp)
                done = apps_host.run(exe)
                self.assertEqual(done.returncode, 0, done.stderr)
                lines = dict(line.split(" ", 1) for line in done.stdout.splitlines())
                if variant == "usb":
                    # The ring's wait is the timeout in ticks (10 ms each): 250 ms is 25, and 0 doesn't wait.
                    self.assertEqual(lines["write"], "takes=0 ok=0 ticks=25 port=0")   # the last of the two passes
                    self.assertEqual(lines["write0"], "takes=0 ok=0 ticks=0")
                    self.assertEqual(lines["flush"], "takes=0 ok=0 ticks=30 port=0")
                else:
                    # A UART takes its bytes at the baud rate whether or not anyone reads, so a write has no timeout to
                    # give up by; a flush waits for the FIFO for the time it is given, on the console's port.
                    self.assertEqual(lines["write"], "takes=0 ok=0 ticks=0 port=1")
                    self.assertEqual(lines["write0"], "takes=0 ok=0 ticks=0")
                    self.assertEqual(lines["flush"], "takes=0 ok=0 ticks=30 port=1")
                self.assertEqual(lines["short"], "ok=0")   # less than all of it taken is not sent
                # The same calls when the console takes the bytes: true each time.
                first = [line for line in done.stdout.splitlines() if " takes=1 " in f" {line} "]
                self.assertEqual(len(first), 3)
                for line in first:
                    self.assertIn("ok=1", line)

    def test_the_snapshot_console_calls_exist_for_both_consoles_and_only_with_snapshots(self):
        header = (MUSE / "muse_console.h").read_text()
        source = (MUSE / "muse_console.c").read_text()
        ui = (MUSE / "muse_ui.c").read_text()
        with_snapshots = "if CONFIG_LV_USE_SNAPSHOT"
        for name in ("muse_console_hold", "muse_console_release", "muse_console_write_timeout", "muse_console_flush"):
            with self.subTest(name):
                self.assertEqual(open_conditions(header, header.index(name)), [with_snapshots])
                defined = [m.start() for m in re.finditer(rf"^(?:bool|void) {name}\(", source, re.M)]
                self.assertEqual([open_conditions(source, at) for at in defined],
                                 [["if !CONFIG_MUSE_CONSOLE_UART", with_snapshots], ["else", with_snapshots]])
        # Which console holds is decided here, where the consoles are told apart, and nowhere else.
        for path in [*MUSE.rglob("*.[ch]"), *MUSE.rglob("*.cpp"), *(ROOT / "main").rglob("*.[ch]")]:
            if path.name not in ("muse_console.c", "muse_console.h", "Kconfig"):
                with self.subTest(path.name):
                    self.assertNotIn("MUSE_CONSOLE_UART", code_only(path.read_text()))
        # Only the snapshot stream calls them, and only where there is one: no plain build refers to them.
        names = re.compile(r"\bmuse_console_(?:hold|release|write_timeout|flush)\b")
        for path in [*MUSE.rglob("*.[ch]"), *MUSE.rglob("*.cpp"), *(ROOT / "main").rglob("*.[ch]")]:
            if path.name not in ("muse_console.c", "muse_console.h", "muse_ui.c"):
                with self.subTest(path.name):
                    found = names.search(code_only(path.read_text()))
                    self.assertIsNone(found, f"{path} calls the snapshot's console API")
        calls = [m.start() for m in re.finditer(r"\bmuse_console_(?:hold|release|write_timeout|flush)\(", ui)]
        self.assertGreaterEqual(len(calls), 4)
        for at in calls:
            with self.subTest(at=ui[at:at + 24]):
                self.assertIn("if LV_USE_SNAPSHOT", open_conditions(ui, at))

    def test_the_hold_takes_stdouts_lock_with_flockfile_and_nothing_calls_ftrylockfile(self):
        # The toolchain's picolibc has an ftrylockfile that takes a global lock, never the FILE's, and returns 0: a
        # hold polling it holds nothing (the log landed inside the stream on the board) and the global lock is never
        # given back. The host's ftrylockfile is POSIX's and works, so no host run can tell. This keeps it out of the
        # source: flockfile and funlockfile are the real ones.
        usb = console_variant((MUSE / "muse_console.c").read_text(), "usb")
        self.assertIn("flockfile(stdout)", c_function(usb, "void muse_console_hold("))
        self.assertIn("funlockfile(stdout)", c_function(usb, "void muse_console_release("))
        for path in [*MUSE.rglob("*.[ch]"), *MUSE.rglob("*.cpp"), *(ROOT / "main").rglob("*.[ch]")]:
            with self.subTest(path.name):
                self.assertNotIn("ftrylockfile", code_only(path.read_text()))

    # The apps port's console flush as the stream sees it. fsync takes none of stdio's locks and its flushes reach the
    # USB port's FIFO from the task that calls it, so one that began just before the stream took the console would
    # still be flushing while the first rows go out (on the board it cost the stream a byte, about one capture in forty
    # with an app echoing 70 lines a second). The real apps_port_console_flush() is run with an fsync that stays in
    # flight until the "stream" is taking the console, and the stream's hold asks whether it still is when it gets it.
    CONSOLE_FLUSH = r"""
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static int g_in_fsync, g_entered, g_taking;
static int fake_fsync(int fd)
{
    (void)fd;
    __atomic_store_n(&g_in_fsync, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_entered, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && !__atomic_load_n(&g_taking, __ATOMIC_SEQ_CST); i++) {
        usleep(1000);
    }
    usleep(50 * 1000);   /* the host picking up the packet */
    __atomic_store_n(&g_in_fsync, 0, __ATOMIC_SEQ_CST);
    return 0;
}
#define fsync(fd) fake_fsync(fd)
"""
    CONSOLE_FLUSH_MAIN = r"""
static void *flusher(void *arg)
{
    (void)arg;
    apps_port_console_flush();
    return NULL;
}

int main(void)
{
    pthread_t task;
    pthread_create(&task, NULL, flusher, NULL);
    while (!__atomic_load_n(&g_entered, __ATOMIC_SEQ_CST)) {
        usleep(100);
    }
    __atomic_store_n(&g_taking, 1, __ATOMIC_SEQ_CST);
    flockfile(stdout);   /* the stream's hold */
    int overlapped = __atomic_load_n(&g_in_fsync, __ATOMIC_SEQ_CST);
    funlockfile(stdout);
    pthread_join(task, NULL);
    printf("overlapped=%d\n", overlapped);
    return 0;
}
"""

    def run_console_flush(self, flush=None):
        port = (MUSE / "apps" / "apps_port_idf.c").read_text()
        flush = flush or c_function(port, "void apps_port_console_flush(void)")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "console_flush.c"
            path.write_text(self.CONSOLE_FLUSH + flush + self.CONSOLE_FLUSH_MAIN)
            exe = apps_host.build("console_flush", [path], tmp)
            done = apps_host.run(exe)
            self.assertEqual(done.returncode, 0, done.stderr)
            return done.stdout.strip()

    def test_a_console_flush_that_began_before_the_stream_is_over_when_the_stream_has_the_console(self):
        self.assertEqual(self.run_console_flush(), "overlapped=0")
        # Without the lock around fflush and fsync the fsync is still flushing when the hold is taken: the same harness
        # with the two calls taken out has teeth.
        port = (MUSE / "apps" / "apps_port_idf.c").read_text()
        flush = c_function(port, "void apps_port_console_flush(void)")
        self.assertIn("flockfile(stdout);", flush)
        self.assertIn("funlockfile(stdout);", flush)
        unlocked = flush.replace("flockfile(stdout);", "").replace("funlockfile(stdout);", "")
        self.assertEqual(self.run_console_flush(unlocked), "overlapped=1")

    def test_the_simulator_stands_in_for_the_snapshot_console_calls(self):
        # The simulator compiles muse_ui.c, snapshot stream included, and links its own console (sim_services.c); it
        # isn't part of this suite, so what it needs is read here: a definition of every call, and
        # CONFIG_LV_USE_SNAPSHOT set wherever its lv_conf.h turns LV_USE_SNAPSHOT on.
        sim = ROOT / "simulator"
        services = (sim / "src/sim_services.c").read_text()
        for name in ("muse_console_hold", "muse_console_release", "muse_console_write_timeout", "muse_console_flush"):
            with self.subTest(name):
                self.assertRegex(services, rf"(?m)^(?:bool|void) {name}\(")
        self.assertRegex((sim / "lv_conf.h").read_text(), r"(?m)^#define LV_USE_SNAPSHOT 1\b")
        self.assertRegex((sim / "compat/sdkconfig.h").read_text(), r"(?m)^#define CONFIG_LV_USE_SNAPSHOT 1\b")

    def test_snapshot_streaming_stays_on_lvgl_snapshot(self):
        ui = (MUSE / "muse_ui.c").read_text()
        for text in ("static uint32_t snap_crc32(", "lv_snapshot_take(", '"\\nSNAP END %08x\\n"', "muse_console_hold(",
                     "muse_console_flush(", "static bool snap_write("):
            with self.subTest(text):
                self.assertIn("if LV_USE_SNAPSHOT", open_conditions(ui, ui.index(text)))
        self.assertIn('"\\nSNAP OFF\\n"', ui)   # a build without it still says why

    # ---- Task 14: tools/muse/snap.py ----

    def test_snap_imports_without_pyserial(self):
        snap = load_snap()
        for name in ("parse", "capture", "write_png", "main", "prepare"):
            with self.subTest(name):
                self.assertTrue(callable(getattr(snap, name, None)))
        source = (TOOLS / "snap.py").read_text()
        # pyserial is imported inside the functions that open a port, never at the top.
        self.assertEqual(len(re.findall(r"^import serial|^from serial", source, re.M)), 0)
        self.assertIn('if __name__ == "__main__":', source)
        for command in (">tap", ">longpress", ">swipe"):
            with self.subTest(command):
                self.assertIn(command, snap.__doc__)

    def test_snap_parse_detects_corruption(self):
        snap = load_snap()
        raw = bytes(range(8))   # 2x2 RGB565
        log = stream(2, 2, raw)
        self.assertTrue(log.startswith("\nSNAP BEGIN 2 2 144\n"))
        w, h, got, ok = snap.parse(log)
        self.assertEqual((w, h, got), (2, 2, raw))
        self.assertTrue(ok)
        w, h, got, ok = snap.parse(flip_first_data_char(log))
        self.assertEqual((w, h), (2, 2))
        self.assertNotEqual(got, raw)
        self.assertFalse(ok)
        # A wrong CRC value on a good stream is just as bad.
        self.assertFalse(snap.parse(log.replace(f"{zlib.crc32(raw):08x}", "00000000"))[3])

    def test_snap_parse_firmware_without_a_crc_works_as_before(self):
        snap = load_snap()
        raw = pixels(13, 6)
        log = stream(13, 6, raw, per_line=144, crc=False)
        self.assertEqual(snap.parse(log), (13, 6, raw, True))
        # And a BEGIN line from older firmware, which doesn't say how long its lines are (144).
        old = log.replace("SNAP BEGIN 13 6 144", "SNAP BEGIN 13 6")
        self.assertEqual(snap.parse(old), (13, 6, raw, True))

    def test_snap_parse_reads_every_row_of_a_real_size(self):
        snap = load_snap()
        raw = pixels(466, 20, seed=7)
        self.assertEqual(snap.parse(stream(466, 20, raw)), (466, 20, raw, True))
        raw = pixels(37, 9, seed=8)
        self.assertEqual(snap.parse(stream(37, 9, raw, per_line=32)), (37, 9, raw, True))
        # CRLF line ends, as a terminal in between would make them.
        self.assertEqual(snap.parse(stream(37, 9, raw).replace("\n", "\r\n")), (37, 9, raw, True))

    def test_snap_parse_matches_lines_at_column_zero(self):
        # H1 (Task 13), applied here: an app's @app.log echo carries its own text after its prefix, so a SNAP line
        # inside another line is never the stream's.
        snap = load_snap()
        raw = pixels(5, 4, seed=3)
        good = stream(5, 4, raw)
        forged_begin = "@app.log x SNAP BEGIN 9 9 144\n"
        forged_end = "@app.log x SNAP END 00000000\n"
        lines = good.split("\n")
        # An echoed BEGIN before the real one, and an echoed END between rows.
        log = "I (1) muse: noise\r\n" + forged_begin + good
        self.assertEqual(snap.parse(log), (5, 4, raw, True))
        log = "\n".join(lines[:4]) + "\n" + forged_end + "\n".join(lines[4:])
        self.assertEqual(snap.parse(log), (5, 4, raw, True))
        # Without the real END there is no stream to read, however many forged ones there are.
        with self.assertRaises(ValueError):
            snap.parse("\n".join(lines[:-2]) + "\n" + forged_end)
        with self.assertRaises(ValueError):
            snap.parse("I (1) muse: noise\n" + forged_begin + "\n".join(lines[2:]))

    def test_snap_parse_tolerates_one_stale_fragment_before_the_first_line(self):
        # A USB packet left in the FIFO while no program had the port open (at most 64 bytes, no newline) can be glued
        # in front of the first line read after opening; nowhere else. fresh=True says the log starts there. What
        # capture() parses, the reply to a 'p', never does (S1).
        snap = load_snap()
        raw = pixels(5, 4, seed=4)
        begin = "SNAP BEGIN 5 4 144\n"
        body = stream(5, 4, raw)[len("\n" + begin):]
        self.assertEqual(snap.parse("ws=up ra" + begin + body, fresh=True), (5, 4, raw, True))
        self.assertEqual(snap.parse("x" * 64 + begin + body, fresh=True), (5, 4, raw, True))
        for log in ("x" * 65 + begin + body, "I (1) muse: up\nws=up ra" + begin + body,
                    "ws=up ra\n" + "x" * 3 + begin + body):
            with self.subTest(log=log[:24]):
                with self.assertRaises(ValueError):
                    snap.parse(log, fresh=True)
        for fragment in ("ws=up ra", "x" * 64, "x"):
            with self.subTest(fragment=fragment):
                with self.assertRaises(ValueError):
                    snap.parse(fragment + begin + body)
        self.assertFalse(snap.parse.__defaults__[0])   # not fresh unless the caller says so

    def test_snap_parse_a_stream_cut_in_its_end_line_is_cut_short(self):
        # S3: "SNAP END" at the end of the text, before its CRC or its newline has come in, is a stream that stopped,
        # not firmware that sends no CRC (which would take a corrupt picture for a good one).
        snap = load_snap()
        raw = pixels(5, 4, seed=12)
        good = stream(5, 4, raw)
        bad = flip_first_data_char(good)
        head, bad_head = good[:good.index("SNAP END")], bad[:bad.index("SNAP END")]
        for name, log in (("good", head), ("corrupt", bad_head)):
            for cut in ("SNAP END", "SNAP END ", "SNAP END 0123", "SNAP END 0123456"):
                with self.subTest(stream=name, cut=cut):
                    with self.assertRaises(ValueError):
                        snap.parse(log + cut)
        # A whole CRC is a whole line, newline or not.
        crc = f"SNAP END {zlib.crc32(raw):08x}"
        self.assertEqual(snap.parse(head + crc), (5, 4, raw, True))
        self.assertFalse(snap.parse(bad_head + crc)[3])

    def test_snap_parse_finds_the_end_behind_a_log_fragment_on_the_blank_line(self):
        # S2: END starts a line of its own, so log text cut in front of it sits on the line before and END keeps
        # column 0.
        snap = load_snap()
        raw = pixels(5, 4, seed=20)
        log = stream(5, 4, raw)
        at = log.index("SNAP END") - 1
        self.assertEqual(log[at - 1:at + 1], "\n\n")
        self.assertEqual(snap.parse(log[:at] + "ws=up ra" + log[at:]), (5, 4, raw, True))

    def test_snap_parse_survives_log_lines_between_the_rows(self):
        snap = load_snap()
        raw = pixels(5, 4, seed=5)
        lines = stream(5, 4, raw).split("\n")
        # A whole log line between two rows, and log text cut into the front of another (a UART console writes a
        # character at a time): the row is still the tail of its line.
        noisy = lines[:3] + ["I (123) muse: hb t=5 wifi=up\r"] + lines[3:5] + ["ws=up ra" + lines[5]] + lines[6:]
        self.assertEqual(snap.parse("\n".join(noisy)), (5, 4, raw, True))
        # A log line that lands inside a row loses it: the CRC says so.
        w, h, got, ok = snap.parse("\n".join(lines[:3] + [lines[3][:6] + "I (9) x\r\n" + lines[3][6:]] + lines[4:]))
        self.assertFalse(ok)

    def test_snap_parse_rejects_what_is_not_a_stream(self):
        snap = load_snap()
        for log in ("", "SNAP OFF\n", "I (1) muse: up\n", "\nSNAP BEGIN 5 4 144\n", "\nSNAP BEGIN 0 4 144\nSNAP END\n",
                    "\nSNAP BEGIN 5 99999 144\nSNAP END\n"):
            with self.subTest(log=log):
                with self.assertRaises(ValueError):
                    snap.parse(log)

    def capture(self, snap, answers, keys="", settle=None, piece=100, pending=b"", chatter=""):
        """snap.capture() on a FakeBoard (`settle` only when given, so the default applies): the result or the
        SnapError, the board, the clock and what went to stderr. `pending` is what was in the port before."""
        clock = FakeClock()
        board = FakeBoard(clock, answers, piece, chatter)
        board.pending = pending
        errors = io.StringIO()
        with mock.patch.object(snap, "time", clock), contextlib.redirect_stderr(errors):
            try:
                result = snap.capture(board, keys, *([] if settle is None else [settle]))
            except snap.SnapError as e:
                result = e
        return result, board, clock, errors.getvalue()

    def test_snap_capture_retakes_a_corrupt_capture(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=6)
        good = stream(30, 8, raw)
        result, board, clock, errors = self.capture(snap, [flip_first_data_char(good), good], keys="a>face=happy",
                                                    settle=0.5)
        self.assertEqual(result, (30, 8, raw))
        # The keys go once; only the 'p' that asks for the picture is sent again.
        self.assertEqual(board.writes, [b"a", b">face=happy\n", b"p", b"p"])
        self.assertEqual(errors, "crc mismatch, retaking\n")
        self.assertEqual(clock.slept, [0.35, 0.5])   # a key's pace, then the settle after the command; no more

    def test_snap_capture_gives_up_after_three_corrupt_captures(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=6)
        bad = flip_first_data_char(stream(30, 8, raw))
        result, board, clock, errors = self.capture(snap, [bad, bad, bad, stream(30, 8, raw)])
        self.assertIsInstance(result, snap.SnapError)
        self.assertIn("crc", str(result))
        self.assertEqual(board.writes.count(b"p"), 3)   # three tries, not four
        self.assertEqual(errors, "crc mismatch, retaking\n" * 2)

    def test_snap_capture_reads_a_stream_that_arrives_in_pieces(self):
        # Including an END line that is cut before its CRC has come in.
        snap = load_snap()
        raw = pixels(30, 8, seed=9)
        for piece in (1, 7, 100, 4096):
            with self.subTest(piece=piece):
                result, board, _, errors = self.capture(snap, [stream(30, 8, raw)], piece=piece)
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.writes, [b"p"])
                self.assertEqual(errors, "")

    def test_snap_capture_firmware_without_a_crc_is_not_retaken(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=10)
        result, board, _, errors = self.capture(snap, [stream(30, 8, raw, crc=False)])
        self.assertEqual(result, (30, 8, raw))
        self.assertEqual(board.writes, [b"p"])
        self.assertEqual(errors, "")
        # Short of bytes without a CRC to say so: kept and warned about, as before, the missing rows zero.
        lines = stream(30, 8, raw, crc=False).split("\n")
        result, board, _, errors = self.capture(snap, ["\n".join(lines[:5] + lines[6:])])
        self.assertEqual(result[:2], (30, 8))
        self.assertEqual(len(result[2]), 30 * 8 * 2)
        self.assertEqual(board.writes, [b"p"])
        self.assertIn("bytes lost", errors)

    def test_snap_capture_says_why_it_has_no_picture(self):
        snap = load_snap()
        result, board, _, _ = self.capture(snap, ["\nSNAP OFF\n"])
        self.assertIsInstance(result, snap.SnapError)
        self.assertIn("screenshots are off in this build", str(result))
        self.assertIn("MUSE_BENCH=1", str(result))
        result, board, _, _ = self.capture(snap, [])
        self.assertIsInstance(result, snap.SnapError)
        self.assertIn("the board never answered", str(result))
        # An @app.log echo that says SNAP OFF is not the firmware saying it.
        result, board, _, _ = self.capture(snap, ["@app.log x SNAP OFF\n"])
        self.assertIn("never answered", str(result))

    def test_snap_capture_says_the_screenshot_arrived_damaged_when_end_comes_without_a_usable_begin(self):
        # A stream whose END arrives without a BEGIN a screen can be read from lost its head: bytes were lost on the
        # way, which is not a board that never answered. It is retaken like a CRC mismatch, at once, its END having
        # been read, and three of them say so.
        snap = load_snap()
        raw = pixels(30, 8, seed=23)
        good = stream(30, 8, raw)
        begin = "SNAP BEGIN 30 8 144"
        for name, log in (("no BEGIN line", good.replace(begin + "\n", "")),
                          ("a character lost from it", good.replace(begin, "SNAP BEGI 30 8 144")),
                          ("log text in front of it", good.replace("\n" + begin, "\nI (5) x: " + begin)),
                          ("sizes that are no screen", good.replace(begin, "SNAP BEGIN 30000 8 144"))):
            with self.subTest(name):
                result, board, _, errors = self.capture(snap, [log, good])
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.writes, [b"p", b"p"])
                self.assertEqual(errors, "the screenshot arrived damaged, retaking\n")
                self.assertEqual(board.write_times[1], board.write_times[0])
                result, board, _, errors = self.capture(snap, [log, log, log, good])
                self.assertIsInstance(result, snap.SnapError)
                self.assertIn("the screenshot arrived damaged", str(result))
                self.assertNotIn("never answered", str(result))
                self.assertEqual(board.writes.count(b"p"), 3)
                self.assertEqual(errors, "the screenshot arrived damaged, retaking\n" * 2)
        # No END to say a stream ended is still a board that said nothing, and a BEGIN without its END a stream cut
        # short, whatever else is wrong with the rest.
        lines = good.split("\n")
        rows_only = "\n".join(lines[:1] + lines[2:-3]) + "\n"
        result, board, _, errors = self.capture(snap, [rows_only])
        self.assertIn("the board never answered", str(result))
        self.assertEqual(errors, "")
        cut = good[:good.index("\nSNAP END")] + "\n"
        result, board, _, errors = self.capture(snap, [cut, good])
        self.assertEqual(result, (30, 8, raw))
        self.assertEqual(errors, "snapshot cut short, retaking\n")

    def test_snap_capture_never_takes_the_start_of_a_read_for_a_line_start(self):
        # S1: a drain can stop in the middle of an @app.log echo, so the first thing read after the 'p' can be the rest
        # of that line, whatever SNAP line it holds. It isn't column 0: the reply to 'p' starts with a newline of its
        # own.
        snap = load_snap()
        raw = pixels(30, 8, seed=11)
        good = stream(30, 8, raw)
        for tail in ("SNAP BEGIN 9 9 144\n", "SNAP BEGIN 9 9 144\r\n", "SNAP END 00000000\n", "SNAP END\n",
                     "SNAP OFF\n"):
            for piece in (1, 100, 4096):
                with self.subTest(tail=tail, piece=piece):
                    result, board, _, errors = self.capture(snap, [tail + good], piece=piece)
                    self.assertEqual(result, (30, 8, raw))
                    self.assertEqual(board.writes, [b"p"])   # no retake, and no giving up
                    self.assertEqual(errors, "")

    def test_snap_capture_takes_no_stale_fragment_for_a_stream_start(self):
        # S1: only a log that starts where the port was opened gets parse()'s tolerance for text glued in front of
        # BEGIN. capture() reads what follows a 'p', whose reply starts with a newline, so a BEGIN behind log text is an
        # app's line, never the firmware's: a stream that starts that way is not read.
        snap = load_snap()
        raw = pixels(30, 8, seed=22)
        glued = "junk\n@app.log x " + stream(30, 8, raw)[1:]   # the BEGIN line without its own line, behind an echo
        result, board, _, _ = self.capture(snap, [glued] * 3)   # its END arrives: a damaged stream, retaken twice
        self.assertIsInstance(result, snap.SnapError)
        self.assertIn("arrived damaged", str(result))
        self.assertEqual(board.writes, [b"p"] * 3)

    def test_snap_capture_drains_what_was_in_the_port_before_it(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=15)
        for pending in (b"ws=up ra", b"SNAP BEGIN 7 7 144\nSNAP END 00000000\n", b"@app.log x SNAP OFF\nSNAP OFF\n"):
            with self.subTest(pending=pending):
                result, board, _, errors = self.capture(snap, [stream(30, 8, raw)], pending=pending)
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.writes, [b"p"])
                self.assertEqual(errors, "")

    def test_a_retake_after_a_crc_mismatch_goes_at_once(self):
        # S4: the corrupt stream had ended (its END was read), so there is nothing to drain, however chatty the board.
        snap = load_snap()
        raw = pixels(30, 8, seed=16)
        good = stream(30, 8, raw)
        for chatter in ("", "@app.log x tick\n"):
            with self.subTest(chatter=chatter):
                result, board, _, errors = self.capture(snap, [flip_first_data_char(good), good], chatter=chatter)
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.write_times[1], board.write_times[0])
                self.assertEqual(errors, "crc mismatch, retaking\n")

    def test_settle_down_waits_for_a_short_quiet_and_never_long(self):
        # S4: 0.3 s without a byte, or 3 s in all: an app logging on a timer (every 200 ms here) can't hold it for 40.
        snap = load_snap()
        for chatter, low, high in (("", 0.3, 0.7), ("@app.log x tick\n", 3.0, 3.6)):
            with self.subTest(chatter=chatter):
                clock = FakeClock()
                board = FakeBoard(clock, [], chatter=chatter)
                board.pending = b"the tail of a stream"
                with mock.patch.object(snap, "time", clock):
                    snap.settle_down(board)
                self.assertTrue(low <= clock.now - 1000.0 <= high, clock.now - 1000.0)

    def test_snap_capture_retakes_a_stream_that_was_cut_short(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=13)
        good = stream(30, 8, raw)
        cut, rest = good[:len(good) // 2], good[len(good) // 2:]
        # Cut short, then whole: one retake, and the picture is the whole one.
        result, board, clock, errors = self.capture(snap, [cut, good])
        self.assertEqual(result, (30, 8, raw))
        self.assertEqual(board.writes, [b"p", b"p"])
        self.assertEqual(errors, "snapshot cut short, retaking\n")
        # The rest of the first arrives after the capture gave up on it (the fetch's two waits end 20.2 s after the
        # 'p'; this comes at 20.3), while the port is drained: nothing of it may be left for the next 'p' to read as
        # the end of its own stream.
        result, board, clock, errors = self.capture(snap, [(cut, rest, 20.3), good])
        self.assertEqual(result, (30, 8, raw))
        self.assertEqual(board.writes, [b"p", b"p"])
        self.assertEqual(errors, "snapshot cut short, retaking\n")
        self.assertEqual(board.outstanding_at_write[1], (0, 0))
        # Three that stop short say so, after three tries.
        result, board, clock, errors = self.capture(snap, [cut, cut, cut, good])
        self.assertIsInstance(result, snap.SnapError)
        self.assertIn("snapshot cut short", str(result))
        self.assertEqual(board.writes.count(b"p"), 3)
        self.assertEqual(errors, "snapshot cut short, retaking\n" * 2)
        # On a board that never goes quiet the drain between tries is 3 s, not 40.
        result, board, clock, errors = self.capture(snap, [cut, good], chatter="@app.log x tick\n")
        self.assertEqual(result, (30, 8, raw))
        self.assertLess(board.write_times[1] - board.write_times[0], 20.3 + 3.6)

    def test_snap_capture_paces_plain_keys_and_settles_after_the_last(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=17)
        good = stream(30, 8, raw)
        for keys in ("asa", "aaa"):   # a repeated key is paced like any other
            with self.subTest(keys=keys):
                result, board, clock, _ = self.capture(snap, [good], keys=keys, settle=2.0)
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.writes, [k.encode() for k in keys] + [b"p"])
                self.assertEqual(clock.slept, [0.35, 0.35, 2.0])
        _, _, clock, _ = self.capture(snap, [good], keys="as")   # no settle given: the default
        self.assertEqual(clock.slept, [0.35, 0.35])
        _, board, clock, _ = self.capture(snap, [good])   # no keys: nothing to wait for
        self.assertEqual((board.writes, clock.slept), ([b"p"], []))

    def test_snap_capture_wakes_first_and_waits_for_a_touch_script(self):
        # S5: capture() applies prepare() itself, so every caller (Task 15's app.py too) gets C13's 'w' and wait for a
        # touch command; a settle given on purpose replaces the wait, even 0.35.
        snap = load_snap()
        raw = pixels(30, 8, seed=18)
        good = stream(30, 8, raw)
        for command, ms in (("tap 1 2", 80), ("longpress 233 233", 900), ("swipe left", 240), ("swipe right", 240)):
            with self.subTest(command):
                result, board, clock, _ = self.capture(snap, [good], keys=">" + command)
                self.assertEqual(result, (30, 8, raw))
                self.assertEqual(board.writes, [b"w", f">{command}\n".encode(), b"p"])
                self.assertEqual(clock.slept[0], 0.35)   # the 'w' is paced like any key
                self.assertAlmostEqual(clock.slept[1], ms / 1000 + 0.6)
                self.assertEqual(len(clock.slept), 2)
        _, board, _, _ = self.capture(snap, [good], keys="a>tap 1 2")   # keys in front stay in front of the wake
        self.assertEqual(board.writes, [b"a", b"w", b">tap 1 2\n", b"p"])
        _, board, _, _ = self.capture(snap, [good], keys="w>tap 1 2")   # one wake is enough
        self.assertEqual(board.writes, [b"w", b">tap 1 2\n", b"p"])
        for settle in (0.2, 0.35):
            with self.subTest(settle=settle):
                _, _, clock, _ = self.capture(snap, [good], keys=">tap 1 2", settle=settle)
                self.assertEqual(clock.slept, [0.35, settle])
        _, board, clock, _ = self.capture(snap, [good], keys=">ui=clock")   # not a touch command: no wake, the default
        self.assertEqual((board.writes, clock.slept), ([b">ui=clock\n", b"p"], [0.35]))

    def test_snap_main_runs_the_whole_exchange_on_the_port_it_opens(self):
        snap = load_snap()
        raw = pixels(30, 8, seed=19)
        good = stream(30, 8, raw)

        def run(argv, answers):
            clock = FakeClock()
            board = FakeBoard(clock, answers)
            out = io.StringIO()
            with mock.patch.object(snap, "time", clock), mock.patch.object(snap, "open_port", return_value=board), \
                    contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                try:
                    snap.main(argv)
                    code = None
                except SystemExit as e:
                    code = e.code
            return board, clock, out.getvalue(), code

        with tempfile.TemporaryDirectory() as tmp:
            shot = Path(tmp) / "shot.png"
            board, clock, out, code = run(["/dev/x", ">tap 1 2", str(shot)], [good])
            self.assertIsNone(code)
            self.assertEqual(board.writes, [b"w", b">tap 1 2\n", b"p"])
            self.assertAlmostEqual(clock.slept[-1], 0.68)
            self.assertEqual(out, f"{shot}: 30x8\n")
            expected = Path(tmp) / "expected.png"
            snap.write_png(expected, 30, 8, raw)   # 30 wide and 8 high, not the other way round
            self.assertEqual(shot.read_bytes(), expected.read_bytes())
            # --settle replaces the wait; a reset first gives the board that long to boot.
            board, clock, out, code = run(["--settle", "0.2", "/dev/x", ">tap 1 2", str(shot), "1.5"], [good])
            self.assertEqual(clock.slept[-1], 0.2)
            self.assertEqual((board.dtr, board.rts), (False, False))
            self.assertGreaterEqual(board.write_times[0] - 1000.0, 1.5)
            # No picture: the reason is the exit message, and no file.
            gone = Path(tmp) / "gone.png"
            board, clock, out, code = run(["/dev/x", "", str(gone)], [])
            self.assertIn("never answered", code)
            self.assertFalse(gone.exists())

    def test_snap_capture_defaults_are_c15s(self):
        snap = load_snap()
        params = inspect.signature(snap.capture).parameters
        self.assertEqual(list(params), ["ser", "keys", "settle"])
        self.assertEqual((params["keys"].default, params["settle"].default), ("", 0.35))
        clock = FakeClock()
        board = FakeBoard(clock, [stream(30, 8, pixels(30, 8, seed=21))])
        with mock.patch.object(snap, "time", clock):
            w, h, raw = snap.capture(board)   # no keys, no settle
        self.assertEqual((w, h, len(raw), board.writes), (30, 8, 30 * 8 * 2, [b"p"]))

    def test_touch_commands_wake_first_and_wait_for_the_script(self):
        # C13 and the controller's "wake first": the read callback doesn't run while LVGL is paused, so a touch
        # command is preceded by 'w', and 'p' waits the script's length (80, 900 or 240 ms) plus 600 ms.
        snap = load_snap()
        for command, ms in (("tap 233 233", 80), ("longpress 233 233", 900), ("swipe left", 240),
                            ("swipe right", 240)):
            with self.subTest(command):
                keys, settle = snap.prepare(">" + command)
                self.assertEqual(keys, "w>" + command)
                self.assertAlmostEqual(settle, ms / 1000 + 0.6)
        keys, settle = snap.prepare("a>swipe left")
        self.assertEqual(keys, "aw>swipe left")   # the keys in front stay in front of the wake
        # Everything else keeps its pace and its keys.
        for keys in ("", "a", "as", ">face=happy", "a>ui=clock", ">mem", ">tapx 1 2", ">ui=face"):
            with self.subTest(keys):
                self.assertEqual(snap.prepare(keys), (keys, 0.35))
        # --settle S replaces the wait, touch or not.
        self.assertEqual(snap.prepare(">tap 1 2", 0.1), ("w>tap 1 2", 0.1))
        self.assertEqual(snap.prepare(">mem", 2.0), (">mem", 2.0))
        self.assertEqual(snap.prepare("", 3.0), ("", 3.0))

    def test_snap_command_line(self):
        snap = load_snap()
        args = snap.parse_args(["--settle", "2.5", "/dev/cu.x", ">swipe left", "/tmp/o.png", "5"])
        self.assertEqual((args.port, args.keys, args.out, args.reset_wait, args.settle),
                         ("/dev/cu.x", ">swipe left", "/tmp/o.png", 5.0, 2.5))
        args = snap.parse_args(["/dev/cu.x", "", "/tmp/o.png"])
        self.assertEqual((args.keys, args.reset_wait, args.settle), ("", None, None))
        args = snap.parse_args(["/dev/cu.x", ">tap 1 2", "/tmp/o.png", "--settle", "0.1"])
        self.assertEqual((args.keys, args.settle), (">tap 1 2", 0.1))

    def test_snap_writes_the_png_it_always_did(self):
        snap = load_snap()
        # RGB565 as LVGL keeps it in memory, low byte first: red 0xF800, green 0x07E0, blue 0x001F, white.
        raw = bytes([0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.png"
            snap.write_png(path, 2, 2, raw)
            data = path.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            width, height = int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
            self.assertEqual((width, height), (6, 6))   # scaled 3x so small screens are readable
            idat = data[data.index(b"IDAT") + 4:data.index(b"IEND") - 8]
            red, green, blue, white = (bytes([0xF8, 0x00, 0x00]), bytes([0x00, 0xFC, 0x00]), bytes([0x00, 0x00, 0xF8]),
                                       bytes([0xF8, 0xFC, 0xF8]))
            # Six scanlines (each pixel three across and three down), each a filter byte 0 and 18 bytes of pixels.
            self.assertEqual(zlib.decompress(idat),
                             (b"\0" + red * 3 + green * 3) * 3 + (b"\0" + blue * 3 + white * 3) * 3)
            # A capture that came up short is padded, never refused.
            snap.write_png(path, 2, 2, raw[:4])
            self.assertTrue(path.read_bytes().startswith(b"\x89PNG"))


if __name__ == "__main__":
    unittest.main()
