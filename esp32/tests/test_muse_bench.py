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


def pixels(w, h, seed=1):
    """w*h RGB565 pixels of noise."""
    return random.Random(seed).randbytes(w * h * 2)


def stream(w, h, raw, per_line=144, crc=True):
    """What send_snapshot() streams for raw: a blank line, SNAP BEGIN, base64 lines of per_line bytes, SNAP END."""
    lines = ["", f"SNAP BEGIN {w} {h} {per_line}"]
    for y in range(h):
        row = raw[y * w * 2:(y + 1) * w * 2]
        lines += [base64.b64encode(row[at:at + per_line]).decode() for at in range(0, len(row), per_line)]
    lines.append(f"SNAP END {zlib.crc32(raw):08x}" if crc else "SNAP END")
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
    """A serial port: write() records what is sent, and each 'p' queues the next scripted answer. read() hands out at
    most `piece` bytes, so a line can arrive in parts, and an empty read costs the port's 0.2 s timeout."""

    def __init__(self, clock, answers, piece=100):
        self.clock, self.answers, self.piece = clock, list(answers), piece
        self.writes, self.pending = [], b""

    def write(self, data):
        self.writes.append(bytes(data))
        if data == b"p" and self.answers:
            self.pending += self.answers.pop(0).encode("latin1")

    def read(self, size=1):
        if not self.pending:
            self.clock.now += 0.2
            return b""
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
            include = Path(tmp) / "include"
            include.mkdir()
            (include / "esp_err.h").write_text("typedef int esp_err_t;\n")
            (include / "sdkconfig.h").write_text("#define CONFIG_MUSE_BENCH 1\n")
            path = Path(tmp) / "eink_stubs.c"
            path.write_text(program)
            exe = apps_host.build("eink_stubs", [path], tmp, include_dirs=[include, MUSE])
            done = apps_host.run(exe)
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
static TaskHandle_t xTaskGetHandle(const char *name) { return strcmp(name, "lvgl") ? NULL : (TaskHandle_t)name; }
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 970; }
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
            include = Path(tmp) / "include"
            include.mkdir()
            (include / "esp_err.h").write_text("typedef int esp_err_t;\n")
            (include / "sdkconfig.h").write_text("#define CONFIG_MUSE_BENCH 1\n")
            path = Path(tmp) / "bench_block.c"
            path.write_text(program)
            exe = apps_host.build("bench_block", [path], tmp, include_dirs=[include, MUSE])
            done = apps_host.run(exe, input="\n".join(lines) + "\n")
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
        self.assertRegex(out[0], r'^@mem \{"int_free":1001,"int_min":101,"int_largest":11,"dma_free":1002,'
                                 r'"psram_free":1004,"psram_min":104,"lvgl_stack_free":\d+\}$')
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

static lv_indev_read_cb_t g_cb;
static uint32_t g_tick = 5000;
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
    bench_touch_install();
    if (!strcmp(what, "no_driver")) {
        printf("installed=%d\n", g_cb != NULL);
        muse_ui_bench_touch(MUSE_UI_TOUCH_TAP, 1, 1);
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
        for (;;) {
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
            include = Path(tmp) / "include"
            include.mkdir()
            (include / "esp_err.h").write_text("typedef int esp_err_t;\n")
            (include / "sdkconfig.h").write_text("#define CONFIG_MUSE_BENCH 1\n")
            path = Path(tmp) / "bench_touch.c"
            path.write_text(program)
            exe = apps_host.build("bench_touch", [path], tmp, include_dirs=[include, MUSE])
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
        # A left swipe starts at the right and goes left along y=233 in 9 steps over 240 ms, then lets go at the end.
        left = self.reads(out["left"][2])
        self.assertEqual((left[0], left[-1]), (("P", 406, 233, 0), ("R", 60, 233, 240)))
        self.assertTrue(all(r[2] == 233 for r in left))
        xs = [r[1] for r in left]
        self.assertEqual(xs, sorted(xs, reverse=True))
        self.assertEqual(len({x for x in xs}), 9)
        self.assertEqual(left[-2][1], 60)   # the finger has reached the end before it lifts
        self.assertEqual(left[-2][3], 225)
        slow = self.reads(out["left_slow"][2])   # a read every 100 ms: still ends where it should, at the end
        self.assertEqual((slow[0], slow[-1]), (("P", 406, 233, 0), ("R", 60, 233, 300)))
        right = self.reads(out["right"][2])
        self.assertEqual((right[0], right[-1]), (("P", 60, 233, 0), ("R", 406, 233, 240)))
        xs = [r[1] for r in right]
        self.assertEqual(xs, sorted(xs))
        self.assertEqual(len({x for x in xs}), 9)
        # Out of the screen is clamped to its edge; a kind that doesn't exist is dropped (the real touch goes on).
        self.assertEqual(self.reads(out["clamp"][2])[0], ("P", 465, 0, 0))
        self.assertEqual(out["invalid"][2], "invalid: P7,9@0 real=1")

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
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)   # the standard check value, as zlib gives it

    def test_send_snapshot_streams_what_snap_parses(self):
        # The firmware's own stream (send_snapshot, its chunks and its SNAP END line) read by the tool's parser.
        snap = load_snap()
        source = (MUSE / "muse_ui.c").read_text()
        program = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { struct { uint32_t w, h, stride; } header; uint8_t *data; } lv_draw_buf_t;
typedef struct lv_obj lv_obj_t;
#define LV_COLOR_FORMAT_RGB565 18
static lv_draw_buf_t g_buf;
static lv_obj_t *lv_screen_active(void) { return NULL; }
static lv_draw_buf_t *lv_snapshot_take(lv_obj_t *obj, int format) { (void)obj; (void)format; return &g_buf; }
static void lv_draw_buf_destroy(lv_draw_buf_t *buf) { (void)buf; }
static void muse_console_write(const void *buf, size_t n) { fwrite(buf, 1, n, stdout); }
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
""" + c_function(source, "static uint32_t snap_crc32(") + "\n" + \
    c_function(source, "static void send_snapshot(void)") + r"""

int main(int argc, char **argv)
{
    if (argc < 5) {
        return 2;
    }
    uint32_t w = (uint32_t)atoi(argv[1]), h = (uint32_t)atoi(argv[2]), pad = (uint32_t)atoi(argv[3]);
    g_buf.header.w = w;
    g_buf.header.h = h;
    g_buf.header.stride = w * 2 + pad;
    g_buf.data = malloc((size_t)g_buf.header.stride * h);
    uint32_t seed = 12345;
    FILE *dump = fopen(argv[4], "wb");
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < g_buf.header.stride; x++) {
            seed = seed * 1103515245u + 12345u;
            uint8_t b = (uint8_t)(seed >> 16);
            g_buf.data[y * g_buf.header.stride + x] = b;
            if (x < w * 2) {
                fputc(b, dump);
            }
        }
    }
    fclose(dump);
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
                    self.assertRegex(log, r"\nSNAP END [0-9a-f]{8}\n$")
                    got_w, got_h, raw, ok = snap.parse(log)
                    self.assertEqual((got_w, got_h), (w, h))
                    self.assertEqual(raw, dump.read_bytes())
                    self.assertTrue(ok)
                    self.assertFalse(snap.parse(flip_first_data_char(log))[3])

    def test_snapshot_streaming_stays_on_lvgl_snapshot(self):
        ui = (MUSE / "muse_ui.c").read_text()
        for text in ("static uint32_t snap_crc32(", "lv_snapshot_take(", '"SNAP END %08x\\n"'):
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
        # in front of the first line read after opening; nowhere else.
        snap = load_snap()
        raw = pixels(5, 4, seed=4)
        begin = "SNAP BEGIN 5 4 144\n"
        body = stream(5, 4, raw)[len("\n" + begin):]
        self.assertEqual(snap.parse("ws=up ra" + begin + body), (5, 4, raw, True))
        self.assertEqual(snap.parse("x" * 64 + begin + body), (5, 4, raw, True))
        with self.assertRaises(ValueError):
            snap.parse("x" * 65 + begin + body)
        with self.assertRaises(ValueError):
            snap.parse("I (1) muse: up\nws=up ra" + begin + body)
        with self.assertRaises(ValueError):
            snap.parse("ws=up ra\n" + "x" * 3 + begin + body)

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

    def capture(self, snap, answers, keys="", settle=0.35, piece=100):
        clock = FakeClock()
        board = FakeBoard(clock, answers, piece)
        errors = io.StringIO()
        with mock.patch.object(snap, "time", clock), contextlib.redirect_stderr(errors):
            try:
                result = snap.capture(board, keys, settle)
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
        self.assertEqual(clock.slept[:2], [0.35, 0.5])   # a key's pace, then the settle after the command

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
            pixels_out = zlib.decompress(idat)
            row = pixels_out[1:1 + 18]
            self.assertEqual(row[:3], bytes([0xF8, 0x00, 0x00]))      # red, three times across
            self.assertEqual(row[9:12], bytes([0x00, 0xFC, 0x00]))    # green
            # A capture that came up short is padded, never refused.
            snap.write_png(path, 2, 2, raw[:4])
            self.assertTrue(path.read_bytes().startswith(b"\x89PNG"))


if __name__ == "__main__":
    unittest.main()
