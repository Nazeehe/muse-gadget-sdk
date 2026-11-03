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

"""Compile muse_lcd_bands.c's tearing-effect timing on the host and check it against the CO5300's measured scan (16.8 to
17.0 ms refresh, 0.6 ms blanking, 466 rows): te_meets (one band against the scan), frame_meets (a refresh's bands
against it: torn if a scan reads some of its rows before their write and some after), frame_clear_in (when a refresh's
bands, sent back to back, stay clear of every scan) and refresh_parts (how many bands LVGL draws a refresh in); and the
clock and Muse's slide's frame mode (FrameModeTest, FrameModeSwitchTest): a whole-screen refresh copied into one frame and
sent as one write, held to just after a scan's start.

Those are connected apps Task 7's and the slide's, built only with the apps (CONFIG_MUSE_APPS): a build without them sends as
7e59ee0 did (PlainBuildTest)."""
import hashlib
import math
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_muse_apps_wiring import open_conditions

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/boards/muse_lcd_bands.c"
HEADER = ROOT / "components/muse/boards/muse_lcd_bands.h"
BOARD_H = ROOT / "components/muse/muse_board.h"
BOARD_175C = ROOT / "components/muse/boards/board_waveshare_s3_175c.c"


def lines_by_build(source):
    """Each line of source with whether a build without the apps has it and whether one with them does: every #if
    CONFIG_MUSE_APPS, #if !CONFIG_MUSE_APPS, #else and #endif resolved (those directive lines in neither), every other
    directive kept as a line both builds have when its region does."""
    out, stack = [], []
    for line in source.splitlines(keepends=True):
        text = line.strip()
        guard = re.match(r"#\s*if\s+(!?)\s*CONFIG_MUSE_APPS\s*$", text)
        if guard:
            stack.append(["apps", not guard.group(1)])  # its lines are the apps build's (True) or the plain build's
            continue
        under_apps = bool(stack) and stack[-1][0] == "apps"
        if re.match(r"#\s*if", text):
            stack.append(["other", None])
        elif under_apps and re.match(r"#\s*elif\b", text):
            raise ValueError("#elif under a CONFIG_MUSE_APPS guard")
        elif under_apps and re.match(r"#\s*else\b", text):
            stack[-1][1] = not stack[-1][1]
            continue
        elif re.match(r"#\s*endif\b", text):
            if stack.pop()[0] == "apps":
                continue
        apps = [taken for kind, taken in stack if kind == "apps"]
        out.append((line, not any(apps), all(apps)))
    assert not stack, "an #if without its #endif"
    return out


def variant(source, apps):
    """source as the preprocessor leaves it with CONFIG_MUSE_APPS 1 (apps) or 0, its other directives as they are."""
    return "".join(line for line, plain, with_apps in lines_by_build(source) if (with_apps if apps else plain))


def strip_c(source):
    """The code of a C source: comments and string literals gone."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    source = re.sub(r"//[^\n]*", " ", source)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', source)


def function(source, name):
    match = re.search(r"^static [\w ]+?\**\b" + name + r"\([^)]*\)\n\{", source, re.M)
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def typedef(source, name):
    return re.search(r"^typedef struct \{[^}]*\} " + name + ";", source, re.M).group(0)


# One query a line on stdin, one answer a line on stdout:
#   meets t0 w y1 y2 fall p scan
#   frame fall p scan n (y1 y2 t0 w)*n
#   clear now gap spread jitter fall p scan n (y1 y2 est)*n
#   parts buf_bytes n (x1 y1 x2 y2 joined)*n
#   pace n (bytes us)*n bytes: the samples learned (pace_learn, on one pace_t kept from query to query), then pace_est
#   pacenew: a new pace_t
#   pairs pairing flushed parts last: band_pairs
# and, built with frame mode's functions (WHOLE, WholeHarness):
#   whole w h n (x1 y1 x2 y2 joined)*n: refresh_whole
#   fband at x1 y1 x2 y2 w h last: frame_band
#   west us spread p scan: whole_est
HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
static int s_rows = 466;
#define FRAME_HOLD_MAX 4   /* the device holds 2 bands together; room here to ask about the whole screen's 4 */
%s
int main(void)
{
    char cmd[16];
    static pace_t pace;
    while (scanf("%%15s", cmd) == 1) {
        long long a[8];
        if (strcmp(cmd, "pairs") == 0) {
            for (int i = 0; i < 4; i++) scanf("%%lld", &a[i]);
            printf("%%d\n", band_pairs(a[0] != 0, (int)a[1], (int)a[2], a[3] != 0));
        } else if (strcmp(cmd, "pacenew") == 0) {
            memset(&pace, 0, sizeof pace);
        } else if (strcmp(cmd, "pace") == 0) {
            int n;
            scanf("%%d", &n);
            for (int i = 0; i < n; i++) {
                long long bytes, us;
                scanf("%%lld %%lld", &bytes, &us);
                pace_learn(&pace, (size_t)bytes, us);
            }
            long long bytes;
            scanf("%%lld", &bytes);
            printf("%%lld\n", (long long)pace_est(&pace, (size_t)bytes));
        } else if (strcmp(cmd, "meets") == 0) {
            for (int i = 0; i < 7; i++) scanf("%%lld", &a[i]);
            printf("%%d\n", te_meets(a[0], a[1], (int)a[2], (int)a[3], a[4], a[5], a[6]));
        } else if (strcmp(cmd, "frame") == 0 || strcmp(cmd, "clear") == 0) {
            bool frame = cmd[0] == 'f';
            int head = frame ? 3 : 7, n;
            for (int i = 0; i < head; i++) scanf("%%lld", &a[i]);
            scanf("%%d", &n);
            static write_t w[512];
            for (int i = 0; i < n; i++) {
                long long y1, y2, t0 = 0, d;
                scanf("%%lld %%lld", &y1, &y2);
                if (frame) scanf("%%lld", &t0);
                scanf("%%lld", &d);
                w[i] = (write_t){ (int)y1, (int)y2, t0, d };
            }
            if (frame) {
                printf("%%d\n", frame_meets(w, n, a[0], a[1], a[2]));
            } else {
                printf("%%lld\n", (long long)frame_clear_in(w, n, a[1], (int)a[2], a[3], a[0], a[4], a[5], a[6]));
            }
        } else if (strcmp(cmd, "parts") == 0) {
            long long buf;
            int n;
            scanf("%%lld %%d", &buf, &n);
            lv_area_t areas[8];
            uint8_t joined[8];
            for (int i = 0; i < n; i++) {
                long long x1, y1, x2, y2, j;
                scanf("%%lld %%lld %%lld %%lld %%lld", &x1, &y1, &x2, &y2, &j);
                areas[i] = (lv_area_t){ (int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2 };
                joined[i] = (uint8_t)j;
            }
            printf("%%d\n", refresh_parts(areas, joined, n, (uint32_t)buf));
#if WHOLE
        } else if (strcmp(cmd, "whole") == 0) {
            long long w, h;
            int n;
            scanf("%%lld %%lld %%d", &w, &h, &n);
            lv_area_t areas[8];
            uint8_t joined[8];
            for (int i = 0; i < n; i++) {
                long long x1, y1, x2, y2, j;
                scanf("%%lld %%lld %%lld %%lld %%lld", &x1, &y1, &x2, &y2, &j);
                areas[i] = (lv_area_t){ (int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2 };
                joined[i] = (uint8_t)j;
            }
            printf("%%d\n", refresh_whole(areas, joined, n, (int)w, (int)h));
        } else if (strcmp(cmd, "fband") == 0) {
            for (int i = 0; i < 8; i++) scanf("%%lld", &a[i]);
            printf("%%d\n", frame_band((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], a[7] != 0));
        } else if (strcmp(cmd, "west") == 0) {
            for (int i = 0; i < 4; i++) scanf("%%lld", &a[i]);
            printf("%%lld\n", (long long)whole_est(a[0], (int)a[1], a[2], a[3]));
#endif
        }
    }
    return 0;
}
'''

PERIOD, SCAN = 16800, 16200   # the CO5300 on the Waveshare 1.75C, as logged when te_meets was written
P2, SCAN2 = 16995, 16365      # and as logged on 2026-10-31 (Task 7's board runs)
BUF = 466 * 118 * 2           # one of LVGL's two draw buffers (board_waveshare_s3_175c.c: DRAW_BUF_LINES 118)
SPREAD, JITTER = 50, 1000     # what the send task asks of frame_clear_in for LVGL's bands


class Harness:
    exe = None
    whole = False   # frame mode's functions too (WholeHarness)

    @classmethod
    def build(cls, tmp):
        source = variant(SOURCE.read_text(), apps=True)
        parts = [typedef(source, "write_t"), typedef(source, "pace_t"), re.search(r"^#define PACE_VAR_MIN .*$", source, re.M).group(0)]
        names = ["te_meets", "floor_div", "frame_meets", "frame_clear_in", "refresh_parts", "pace_learn", "pace_est", "band_pairs"]
        if cls.whole:
            parts += [re.search(rf"^#define {macro} .*$", source, re.M).group(0) for macro in ("TE_JITTER_US", "WHOLE_SPREAD")]
            names += ["refresh_whole", "frame_band", "whole_est"]
        parts += [function(source, name) for name in names]
        name = "te_whole" if cls.whole else "te"
        src = Path(tmp) / f"{name}.c"
        src.write_text(HARNESS % "\n".join(parts))
        cls.exe = Path(tmp) / name
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", f"-DWHOLE={int(cls.whole)}", "-o", str(cls.exe), str(src)],
                       check=True)

    @classmethod
    def ask(cls, queries):
        out = subprocess.run([str(cls.exe)], input="\n".join(queries) + "\n", check=True, capture_output=True,
                             text=True).stdout
        return [int(v) for v in out.split()]


class WholeHarness(Harness):
    """The harness with frame mode's functions (refresh_whole, frame_band, whole_est) as well, in an executable of its own."""
    exe = None
    whole = True


def source_number(name):
    """A #define of muse_lcd_bands.c's apps build, as an int: a number, or the product of two in brackets."""
    text = re.search(rf"^#define {name} (\d+|\(\d+ \* \d+\))(?:\s|$)", variant(SOURCE.read_text(), apps=True), re.M).group(1)
    return math.prod(int(v) for v in re.findall(r"\d+", text))


def frame_query(writes, fall, p, scan):
    return f"frame {fall} {p} {scan} {len(writes)} " + " ".join(f"{y1} {y2} {t0} {w}" for y1, y2, t0, w in writes)


def clear_query(bands, now, gap, fall, p, scan, spread=SPREAD, jitter=JITTER):
    return (f"clear {now} {gap} {spread} {jitter} {fall} {p} {scan} {len(bands)} " +
            " ".join(f"{y1} {y2} {est}" for y1, y2, est in bands))


class TearingEffectTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        Harness.build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def meets(self, t0, w, y1, y2, fall=100000):
        return Harness.ask([f"meets {fall + t0} {w} {y1} {y2} {fall} {PERIOD} {SCAN}"]) == [1]

    def test_whole_screen_slower_than_the_scan(self):
        # A 26.4 ms write stays behind the scan that just started and ahead of the
        # next one only if it starts within about 6 ms of the scan's start.
        for t in (0, 2000, 5000):
            self.assertFalse(self.meets(t, 26400, 0, 466), t)
        for t in (7000, 10000, 16000):
            self.assertTrue(self.meets(t, 26400, 0, 466), t)

    def test_whole_screen_faster_than_the_scan(self):
        # A 5 ms write overtakes a scan it starts behind, so it has to start late
        # in the scan, below which nothing is left for it to catch.
        self.assertTrue(self.meets(1000, 5000, 0, 466))
        self.assertFalse(self.meets(13000, 5000, 0, 466))

    def test_small_band(self):
        # Rows 100-200 are scanned 3.5-7 ms into the refresh: a 2 ms write meets
        # the scan only around then.
        self.assertFalse(self.meets(0, 2000, 100, 200))
        self.assertTrue(self.meets(4000, 2000, 100, 200))
        self.assertFalse(self.meets(9000, 2000, 100, 200))

    def test_scans_after_the_last_one(self):
        # The next refresh's scan counts too: the same band, one period later.
        self.assertTrue(self.meets(PERIOD + 4000, 2000, 100, 200))


class FrameTest(unittest.TestCase):
    """Task 7: a refresh is torn if a scan passes between its writes (frame_meets), and the send task holds a refresh's
    bands for a moment their writes, back to back, stay clear of every scan (frame_clear_in)."""

    # The board's torn refresh (task-7-investigation.md, the disc): a spinning 200 px record's turned box, rows 90-374, in
    # LVGL's two bands; the second went out once LVGL had drawn it, in the next refresh. Times from the scan's start.
    A, B = (90, 282, 7485, 8311), (282, 374, P2 + 11570, 4121)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        Harness.build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_one_write_is_te_meets(self):
        queries = []
        for y1, y2, w in ((0, 466, 26400), (100, 200, 2000), (178, 288, 1927), (90, 282, 8300), (282, 374, 4100)):
            for t0 in range(-PERIOD, 2 * PERIOD, 250):
                queries.append(f"meets {t0} {w} {y1} {y2} 0 {PERIOD} {SCAN}")
                queries.append(frame_query([(y1, y2, t0, w)], 0, PERIOD, SCAN))
        answers = Harness.ask(queries)
        self.assertEqual(answers[0::2], answers[1::2])
        self.assertIn(0, answers)
        self.assertIn(1, answers)

    def test_bands_each_clear_can_make_a_torn_refresh(self):
        # Each band alone is clear (te_meets, what the old hold checked), but the scan after the first band's write reads
        # the second band's rows before its write: that refresh shows the new top over the old bottom.
        a, b = self.A, self.B
        answers = Harness.ask([f"meets {a[2]} {a[3]} {a[0]} {a[1]} 0 {P2} {SCAN2}", f"meets {b[2]} {b[3]} {b[0]} {b[1]} 0 {P2} {SCAN2}",
                               frame_query([a, b], 0, P2, SCAN2)])
        self.assertEqual(answers, [0, 0, 1])

    def test_the_same_bands_back_to_back_are_clear(self):
        a, b = self.A, self.B
        self.assertEqual(Harness.ask([frame_query([a, (b[0], b[1], a[2] + a[3] + 300, b[3])], 0, P2, SCAN2)]), [0])

    def test_a_scan_between_bands_of_different_areas_tears_the_refresh(self):
        # Two areas, far apart (a label at the top and a picture below): the scan reads the label's rows after its write and
        # the picture's before its write (rows 300-380 are read 10.5-13.3 ms in; the picture goes at 14 ms). Neither write
        # meets the scan, but the refresh is split.
        label, picture = (20, 60, 0, 600), (300, 380, 14000, 2000)
        self.assertEqual(Harness.ask([frame_query([label], 0, P2, SCAN2), frame_query([picture], 0, P2, SCAN2),
                                      frame_query([label, picture], 0, P2, SCAN2)]), [0, 0, 1])

    def timings(self, bands, start, gap):
        """Every timing of the bands sent back to back that frame_clear_in allows for: a start in [start, start + JITTER],
        each write (100 - SPREAD)% to (100 + SPREAD)% of its estimate + 400 us, the gap half to twice + 200 us."""
        out = []
        for s in range(start, start + JITTER + 1, 250):
            for scale in ("lo", "mid", "hi"):
                for gap_k in (gap // 2, gap, 2 * gap + 200):
                    t, writes = s, []
                    for y1, y2, est in bands:
                        lo, hi = est * (100 - SPREAD) // 100, est * (100 + SPREAD) // 100 + 400
                        w = {"lo": lo, "mid": (lo + hi) // 2, "hi": hi}[scale]
                        writes.append((y1, y2, t, w))
                        t += w + gap_k
                    out.append(writes)
        return out

    def clear_everywhere(self, bands, start, gap, p, scan, fall=0):
        """Each timing alone is clear (no tear), and all of them together are (a scan reads every written row of every
        timing on the same side of its write: whichever timing it has, the refresh shows whole in the same refresh)."""
        timings = self.timings(bands, start, gap)
        answers = Harness.ask([frame_query(t, fall, p, scan) for t in timings] +
                              [frame_query([w for t in timings for w in t], fall, p, scan)])
        return not any(answers)

    def test_the_scheduler_keeps_a_refresh_of_two_bands_clear(self):
        # The record's two bands, from every moment of a refresh: the delay frame_clear_in gives (in half milliseconds,
        # within two refreshes) is the first at which every timing it allows for stays clear.
        bands, gap = [(90, 282, 8300), (282, 374, 4100)], 300
        delays = Harness.ask([clear_query(bands, now, gap, 0, P2, SCAN2) for now in range(0, P2, 500)])
        for now, d in zip(range(0, P2, 500), delays):
            with self.subTest(now=now, d=d):
                self.assertGreaterEqual(d, 0)
                self.assertLess(d, 2 * P2)
                self.assertEqual(d % 500, 0)
                self.assertTrue(self.clear_everywhere(bands, now + d, gap, P2, SCAN2))
                if d > 0:
                    self.assertFalse(self.clear_everywhere(bands, now + d - 500, gap, P2, SCAN2))

    def test_the_scheduler_keeps_one_band_clear(self):
        # One band is the same question: the static body's 80 px label, the full screen's top band, the record's turned box.
        for bands in ([(178, 288, 1927)], [(0, 118, 6800)], [(90, 374, 12000)]):
            delays = Harness.ask([clear_query(bands, now, 300, 0, P2, SCAN2) for now in range(0, P2, 1000)])
            for now, d in zip(range(0, P2, 1000), delays):
                with self.subTest(bands=bands, now=now, d=d):
                    self.assertGreaterEqual(d, 0)
                    self.assertTrue(self.clear_everywhere(bands, now + d, 300, P2, SCAN2))

    def test_the_old_hold_started_its_check_too_early(self):
        # The board's torn single band (the static body's label): rows 178-288, 1.94 ms (1.93 expected), sent 6.27 ms into
        # the refresh. The old hold checked a start 300 us before now, ahead of the scan (row 178 at 6.24 ms), and found
        # it clear; the write really started after the scan had passed row 178 and overtook it. frame_clear_in checks the
        # starts the write can really have, and waits.
        now, est = 6267, 1927
        lo, hi = est // 2 + 400, est * 3 // 2 + 400
        old = Harness.ask([f"meets {now - 300} {w} 178 288 0 {P2} {SCAN2}" for w in (lo, (lo + hi) // 2, hi)])
        self.assertEqual(old, [0, 0, 0], "the old check passed it")
        self.assertEqual(Harness.ask([f"meets {now} 1940 178 288 0 {P2} {SCAN2}"]), [1], "the write met the scan")
        self.assertGreater(Harness.ask([clear_query([(178, 288, est)], now, 300, 0, P2, SCAN2)])[0], 0)

    def test_a_refresh_that_cant_be_kept_clear(self):
        # The whole screen in LVGL's four bands, 26 ms: no start keeps every timing of it clear, so it isn't held (-1) and
        # goes out at once; frame_done counts it as torn when its writes meet the scan.
        whole = [(0, 118, 6600), (118, 236, 6600), (236, 354, 6600), (354, 466, 6300)]
        self.assertEqual(Harness.ask([clear_query(whole, now, 300, 0, P2, SCAN2) for now in (0, 5000, 10000)]), [-1, -1, -1])
        sent = [(y1, y2, 10000 + i * 6700, w) for i, (y1, y2, w) in enumerate(whole)]
        self.assertEqual(Harness.ask([frame_query(sent, 0, P2, SCAN2)]), [1])
        done = function(variant(SOURCE.read_text(), apps=True), "frame_done")
        self.assertRegex(done, r"bool torn = judged && frame_meets\(")
        self.assertIn("s_frame.torn += torn;", done)


class RefreshPartsTest(unittest.TestCase):
    """refresh_parts: LVGL 9.5.0 draws each area that isn't joined into another in pieces of the rows that fit one draw
    buffer (lv_refr.c, refr_invalid_areas and get_max_row: buffer bytes / (width * 2) rows, at most the area's height),
    an even number of them as the board's rounder makes every height (round_area)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        Harness.build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def parts(self, *areas):
        return Harness.ask([f"parts {BUF} {len(areas)} " + " ".join(" ".join(str(v) for v in a) for a in areas)])[0]

    def test_a_spinning_record(self):
        self.assertEqual(self.parts((90, 90, 375, 375, 0)), 2)    # its turned box at 45 degrees, 286 px: 192 rows, then 94
        self.assertEqual(self.parts((132, 132, 345, 345, 0)), 1)  # its own square and a few px, 214: fits (256 rows)
        self.assertEqual(self.parts((132, 132, 333, 333, 0)), 1)  # 202 px square

    def test_the_whole_screen_and_wide_areas(self):
        self.assertEqual(self.parts((0, 0, 465, 465, 0)), 4)      # 118 rows a band
        self.assertEqual(self.parts((0, 0, 465, 235, 0)), 2)
        self.assertEqual(self.parts((0, 0, 465, 117, 0)), 1)

    def test_areas_add_up_and_joined_ones_dont_count(self):
        self.assertEqual(self.parts((20, 20, 99, 59, 0), (132, 132, 333, 333, 0)), 2)
        self.assertEqual(self.parts((20, 20, 99, 59, 1), (132, 132, 333, 333, 0)), 1)

    def test_the_rule_is_lvgls(self):
        source = variant(SOURCE.read_text(), apps=True)
        self.assertIn("buf_bytes / ((uint32_t)w * 2)", function(source, "refresh_parts"))
        self.assertIn("& ~1", function(source, "refresh_parts"))
        from test_muse_apps_lv import lvgl_dir  # LVGL 9.5.0 on this machine, if a device build has fetched it
        lvgl = lvgl_dir()
        if lvgl is None:
            self.skipTest("LVGL 9.5.0 isn't on this machine (set MUSE_LVGL_DIR, or build for a device once)")
        refr = (lvgl / "src/core/lv_refr.c").read_text(errors="replace")
        # get_max_row: the buffer's bytes over the stride (RGB565: no palette, width * 2 at a stride alignment of 1), at most the
        # area's height; refr_invalid_areas draws each unjoined area in pieces of that many rows.
        self.assertIn("int32_t max_row = (uint32_t)(disp->buf_act->data_size - overhead) / stride;", refr)
        self.assertIn("if(max_row > area_h) max_row = area_h;", refr)
        self.assertIn("if(disp_refr->inv_area_joined[i]) continue;", refr)


class PairingTest(unittest.TestCase):
    """Task 7, fix round 1 (R7a): pairing is a switch (muse_lcd_bands_set_pairing), off until the apps turn it on while a picture
    spins. Off, a refresh of two bands goes out as f0204bd sent it, each band held on its own as LVGL sends it; on, the first is
    held for the second and both go in one clear moment."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        Harness.build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_only_the_first_band_of_two_pairs_and_only_with_pairing_on(self):
        cases = [(pairing, flushed, parts, last) for pairing in (0, 1) for flushed in range(4) for parts in range(5)
                 for last in (0, 1)]
        answers = Harness.ask([f"pairs {p} {f} {n} {l}" for p, f, n, l in cases])
        for (pairing, flushed, parts, last), pairs in zip(cases, answers):
            with self.subTest(pairing=pairing, flushed=flushed, parts=parts, last=last):
                self.assertEqual(pairs, int(pairing == 1 and flushed == 0 and parts == 2 and last == 0))

    def test_off_holds_each_band_alone_and_on_holds_the_pair(self):
        # The board's two-band refresh (the turned 200 px record, before the cap): LVGL flushes A, draws B meanwhile and
        # flushes it once A has gone and B is drawn, 15 ms after A was flushed. Off, each band is held on its own as it comes, and
        # some refreshes are torn although each band is clear; on, the two are held together and none is.
        a, b, render, gap = (90, 282, 8300), (282, 374, 4100), 15000, 300
        nows = list(range(0, P2, 250))
        holds_a = Harness.ask([clear_query([a], now, gap, 0, P2, SCAN2) for now in nows])
        off = []
        for now, da in zip(nows, holds_a):
            start_a = now + max(da, 0)
            ready_b = max(start_a + a[2] + gap, now + render)
            off.append((start_a, ready_b))
        holds_b = Harness.ask([clear_query([b], ready_b, gap, 0, P2, SCAN2) for _, ready_b in off])
        off_frames = [frame_query([(a[0], a[1], start_a, a[2]), (b[0], b[1], ready_b + max(db, 0), b[2])], 0, P2, SCAN2)
                      for (start_a, ready_b), db in zip(off, holds_b)]
        holds_ab = Harness.ask([clear_query([a, b], now + render, gap, 0, P2, SCAN2) for now in nows])
        on_frames = [frame_query([(a[0], a[1], now + render + d, a[2]), (b[0], b[1], now + render + d + a[2] + gap, b[2])], 0, P2,
                                 SCAN2) for now, d in zip(nows, holds_ab)]
        self.assertTrue(all(d >= 0 for d in holds_ab), "the pair always has a clear moment")
        off_torn, on_torn = sum(Harness.ask(off_frames)), sum(Harness.ask(on_frames))
        self.assertGreater(off_torn, 0, "off: each band is clear, but some refreshes are torn (f0204bd)")
        self.assertEqual(on_torn, 0, "on: none is")


class FrameModeTest(unittest.TestCase):
    """Frame mode (muse_lcd_bands_set_frame_mode), on while the home screen's pages slide: a refresh of the whole screen, LVGL's
    four 118-row bands of it, is copied into one frame and sent as one write, held to just after a scan's start, where a write
    slower than the scan stays behind the scan that just started and ahead of the next one. Any other refresh takes the band
    path, and so does every refresh while there is no frame."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        WholeHarness.build(cls.tmp.name)
        cls.spread = source_number("WHOLE_SPREAD")

    @staticmethod
    def whole_query(areas, w=466, h=466):
        return f"whole {w} {h} {len(areas)} " + " ".join(" ".join(str(v) for v in a) for a in areas)

    def west(self, us, p=P2, scan=SCAN2):
        return WholeHarness.ask([f"west {us} {self.spread} {p} {scan}"])[0]

    def test_only_one_area_of_the_whole_screen_is_a_whole_refresh(self):
        cases = [
            ([(0, 0, 465, 465, 0)], 1),                         # a step of the slide: the tileview, all of the screen
            ([(0, 0, 465, 465, 0), (20, 20, 99, 59, 1)], 1),    # a label's change joined into it (the clock's hand)
            ([(20, 20, 99, 59, 1), (0, 0, 465, 465, 0)], 1),
            ([(0, 0, 465, 235, 0)], 0),                         # the top half: full-width bands, but not the screen
            ([(0, 118, 465, 465, 0)], 0),
            ([(0, 0, 463, 465, 0)], 0),                         # not the whole width
            ([(2, 0, 465, 465, 0)], 0),
            ([(0, 0, 465, 465, 0), (20, 20, 99, 59, 0)], 0),    # and an area drawn after it
            ([(132, 132, 333, 333, 0)], 0),                     # a spinning record
            # The screen in two areas. LVGL joins two such areas before it draws (lv_refr_join_area: their union is no bigger),
            # so a slide's step is one; frame mode takes only that.
            ([(0, 0, 465, 233, 0), (0, 234, 465, 465, 0)], 0),
            ([], 0),
        ]
        answers = WholeHarness.ask([self.whole_query(areas) for areas, _ in cases])
        for (areas, whole), answer in zip(cases, answers):
            with self.subTest(areas=areas):
                self.assertEqual(answer, whole)

    def test_only_full_width_bands_from_row_0_to_465_in_order_go_into_the_frame(self):
        # LVGL's bands of the whole screen, as refresh_parts' rule cuts it (118 rows of a draw buffer each), top to bottom: each
        # continues the screen from where the one before ended, and the last ends at its bottom.
        rows = BUF // (466 * 2) & ~1
        bands = [(y, min(y + rows, 466)) for y in range(0, 466, rows)]
        self.assertEqual(bands, [(0, 118), (118, 236), (236, 354), (354, 466)])
        good = [f"fband {y1} 0 {y1} 466 {y2} 466 466 {int(y2 == 466)}" for y1, y2 in bands]
        self.assertEqual(WholeHarness.ask(good), [1, 1, 1, 1])
        # Anything else doesn't: not the whole width, not from where the band before ended (a band again, a band skipped), a last
        # band that doesn't reach the bottom, a band at the bottom that isn't LVGL's last, no rows.
        bad = [(0, 0, 0, 464, 118, 0), (0, 2, 0, 466, 118, 0), (118, 0, 0, 466, 118, 0), (118, 0, 236, 466, 354, 0),
               (354, 0, 354, 466, 464, 1), (354, 0, 354, 466, 466, 0), (0, 0, 0, 466, 118, 1), (0, 0, 0, 466, 0, 0)]
        answers = WholeHarness.ask([f"fband {at} {x1} {y1} {x2} {y2} 466 466 {last}" for at, x1, y1, x2, y2, last in bad])
        for case, answer in zip(bad, answers):
            with self.subTest(case=case):
                self.assertEqual(answer, 0)

    def test_the_hold_is_for_the_frames_learned_write_but_never_more_than_a_hold_can_keep_clear(self):
        # The longest write frame_clear_in can still find a moment for (at WHOLE_SPREAD, starting up to TE_JITTER_US late, with
        # 400 us more and a millisecond to spare): a write as long or longer is held to a scan's start, its best moment. Before
        # the first is timed (0) the estimate is that longest.
        most = (P2 + SCAN2 - JITTER - 1400) * 100 // (100 + self.spread)
        self.assertEqual(self.west(0), most)
        for learned in (15000, 24000, 26000, most - 1):
            self.assertEqual(self.west(learned), learned)
        for learned in (most, most + 1, 40000):
            self.assertEqual(self.west(learned), most)
        self.assertEqual(self.west(0, PERIOD, SCAN), (PERIOD + SCAN - JITTER - 1400) * 100 // (100 + self.spread))

    def test_the_frame_is_held_to_just_after_a_scan_start_and_clear_of_the_next(self):
        # From every moment of a refresh: the frame's one write (rows 0-466) is held to a moment within a few ms of a scan's start,
        # where every timing the hold allows for (a start up to TE_JITTER_US late, a write of WHOLE_SPREAD either way and 400 us
        # more) stays behind that scan and ahead of the next one. Untimed, or as long as a hold can take, it goes within a
        # millisecond of the start.
        most = self.west(0)
        for learned in (0, 20000, 24000, 26000, 28000, most + 5000):
            est = self.west(learned)
            lo, hi = est * (100 - self.spread) // 100, est * (100 + self.spread) // 100 + 400
            nows = list(range(0, P2, 250))
            delays = WholeHarness.ask([clear_query([(0, 466, est)], now, 0, 0, P2, SCAN2, spread=self.spread) for now in nows])
            checks = []
            for now, d in zip(nows, delays):
                with self.subTest(learned=learned, now=now, d=d):
                    self.assertGreaterEqual(d, 0, "always held to a moment, never sent at once for want of one")
                    phase = (now + d) % P2
                    # After a scan's start (phase 0: that scan is ahead of it), and so soon after that the next scan doesn't
                    # catch it however long it takes; as long as a hold can take, within a millisecond of the start.
                    self.assertLess(phase + JITTER + hi, P2 + SCAN2, "ahead of the next scan")
                    if est == most:
                        self.assertLessEqual(phase, 1000, "just after a scan's start")
                    checks += [frame_query([(0, 466, now + d + late, w)], 0, P2, SCAN2) for late in (0, JITTER)
                               for w in (lo, est, hi)]
            self.assertEqual(sum(WholeHarness.ask(checks)), 0, f"a timing the hold allows for is torn (learned {learned})")

    def test_the_first_frame_held_untimed_is_clear_for_any_write_up_to_its_estimate(self):
        # The first frame mode's write has no time learned: held as the longest a hold can keep clear, it starts within a
        # millisecond of a scan's start, and a real write of 20 ms to that long, from any moment, stays clear.
        most = self.west(0)
        nows = list(range(0, P2, 500))
        delays = WholeHarness.ask([clear_query([(0, 466, most)], now, 0, 0, P2, SCAN2, spread=self.spread) for now in nows])
        writes = [frame_query([(0, 466, now + d + late, w)], 0, P2, SCAN2) for now, d in zip(nows, delays)
                  for late in (0, 200, JITTER) for w in range(20000, most + 1, 1000)]
        self.assertEqual(sum(WholeHarness.ask(writes)), 0)

    def test_a_frame_held_after_its_last_dma_copy_is_clear_from_every_phase(self):
        # With the copies by DMA, the last band's copy ends at any moment of the scan, and the send task wakes up to some 300 us after
        # the DMA's interrupt: whole_write takes its hold only then (FrameDmaTest: the write comes after the last copy's end). From
        # every phase in 100 us steps, the frame's write, learned at 26.4 ms (the board's, Task 3) and anywhere from 24 to 29 ms on
        # the wire, starting up to TE_JITTER_US late, meets no scan.
        est = self.west(26400)
        nows = list(range(0, P2, 100))
        delays = WholeHarness.ask([clear_query([(0, 466, est)], now, 0, 0, P2, SCAN2, spread=self.spread) for now in nows])
        self.assertTrue(all(d >= 0 for d in delays))
        writes = [frame_query([(0, 466, now + d + late, w)], 0, P2, SCAN2) for now, d in zip(nows, delays)
                  for late in (0, 300, JITTER) for w in (24000, 26400, 29000)]
        self.assertEqual(sum(WholeHarness.ask(writes)), 0)

    def test_the_slides_whole_screen_tears_as_bands_and_not_as_one_write(self):
        # A step of the slide on the board (the user's capture, 2026-11-01): LVGL's four bands of the whole screen, 8.2 ms each on
        # the band path. No moment keeps them clear sent back to back (FrameTest.test_a_refresh_that_cant_be_kept_clear), and the
        # band path held each on its own: the third went behind the next scan and the refresh split at row 236 (the map's line,
        # "0-118 +186 8410 h1, 118-236 +8779 8341 h0, 236-354 +8726 8056 h1", the third a refresh later). In frame mode the same
        # rows are one write of the frame, 26 ms through one window (video's whole screen on the 1.75C), held by the same
        # frame_clear_in: clear from every moment the refresh can be ready at.
        bands = [(0, 118, 8200), (118, 236, 8200), (236, 354, 8200), (354, 466, 7800)]
        self.assertEqual(WholeHarness.ask([clear_query(bands, now, 300, 0, P2, SCAN2) for now in (0, 4000, 8000, 12000)]),
                         [-1, -1, -1, -1])
        split = [(0, 118, 186, 8410), (118, 236, 186 + 8410 + 183, 8341), (236, 354, P2 + 8726, 8056)]
        self.assertEqual(WholeHarness.ask([frame_query(split, 0, P2, SCAN2)]), [1])
        nows = list(range(0, P2, 250))
        delays = WholeHarness.ask([clear_query([(0, 466, 26000)], now, 0, 0, P2, SCAN2, spread=self.spread) for now in nows])
        self.assertTrue(all(d >= 0 for d in delays))
        self.assertEqual(sum(WholeHarness.ask([frame_query([(0, 466, now + d, 26000)], 0, P2, SCAN2)
                                               for now, d in zip(nows, delays)])), 0)

    def test_the_refresh_is_chosen_at_its_start_and_frame_mode_wins_over_pairing(self):
        # LV_EVENT_RENDER_START decides, from LVGL's areas, whether the refresh goes through frame mode: only with frame mode on
        # (which it is only with a frame: FrameModeSwitchTest) and only for a whole screen. Each band LVGL then flushes goes into
        # the frame only while every one so far continued the screen; a band that doesn't ends frame mode for the rest of the
        # refresh. A band in the frame is never a pair's first: frame mode wins if the apps' pairing is on too.
        source = strip_c(variant(SOURCE.read_text(), apps=True))
        render = function(source, "on_render_event")
        self.assertRegex(render, r"(?s)case LV_EVENT_RENDER_START:.*?s_framed = s_framing && refresh_whole\(s_disp->inv_areas, "
                                 r"s_disp->inv_area_joined, \(int\)s_disp->inv_p, s_cols, s_rows\);", )
        queue = function(source, "queue_band")
        self.assertRegex(queue, r"if \(s_framed\) \{\s*b\.whole = frame_band\(s_whole\.at, x1, y1, x2, y2, s_cols, s_rows, b\.last\);"
                                r"\s*s_framed = b\.whole;\s*s_whole\.at = y2;\s*\}")
        self.assertIn("b.pair = !b.whole && band_pairs(s_pairing, flushed, s_parts, b.last);", queue)
        # The send task copies such a band into the frame (by DMA, or by CPU where the DMA can't: FrameDmaTest), gives it back to
        # LVGL once the copy has ended, and at the refresh's last sends the frame whole; one task sends everything, so the next
        # refresh's first copy comes after that write. A refresh that stopped being whole has the rows copied so far written first.
        send = function(source, "send_bands")
        self.assertRegex(send, r"if \(b\.whole\) \{\s*whole_band\(&b\);\s*continue;\s*\}\s*if \(s_whole\.staged\) \{\s*"
                               r"whole_write\(s_whole\.staged, false\);\s*s_whole\.staged = 0;\s*\}")
        band = function(source, "whole_band")
        self.assertIn("uint8_t *dst = s_whole.px + (size_t)b->y1 * row;", band)
        self.assertIn("size_t n = (size_t)(b->y2 - b->y1) * row;", band)
        copy = band.index("if (!dma_copy(dst, b->data, n)) {")
        self.assertLess(copy, band.index("memcpy(dst, b->data, n);"))
        ready = band.index("band_back();")
        send_it = band.index("whole_write(s_rows, true);")
        self.assertLess(band.index("memcpy(dst, b->data, n);"), ready)
        self.assertLess(ready, send_it)
        self.assertLess(send_it, band.index("frame_done(b);"))
        self.assertLess(band.index("frame_done(b);"), band.index("dma_end();"))

    def test_the_frame_is_one_window_and_counted_as_one_write_of_its_refresh(self):
        # send_frame's framing: the window once (2Ah, 2Bh at the panel's gap), then 2Ch and 3Ch pieces through the two internal
        # buffers, with nothing of LVGL's bands in flight. Held only when it is the whole frame, by whole_est at WHOLE_SPREAD.
        # The write is the refresh's record (s_frame) as one band, which frame_done judges (frame_meets) and counts in the
        # tearing effect lines like any other: a frame that meets a scan is a torn refresh.
        write = function(strip_c(variant(SOURCE.read_text(), apps=True)), "whole_write")
        self.assertRegex(write, r"^static void whole_write\(int rows, bool hold\)")
        self.assertLess(write.index("wire_idle(0);"), write.index("QSPI_CMD(0x2A)"))
        self.assertIn("QSPI_CMD(0x2B)", write)
        self.assertIn("QSPI_PIXELS(y == 0 ? 0x2C : 0x3C)", write)
        self.assertEqual(write.count("esp_lcd_panel_io_tx_color("), 1)
        self.assertNotIn("esp_lcd_panel_draw_bitmap", write)
        self.assertIn("int x1 = s_gap_x, x2 = s_cols - 1 + s_gap_x, y1 = s_gap_y, y2 = rows - 1 + s_gap_y;", write)
        self.assertRegex(write, r"if \(hold && te_now\(&fall, &p, &scan\)\) \{\s*est = whole_est\(s_whole\.us, WHOLE_SPREAD, p, scan\);"
                                r"\s*const write_t w = \{ 0, rows, 0, est \};\s*how = te_hold\(&s_te_whole, &w, 1, 0, WHOLE_SPREAD\);")
        self.assertIn("s_frame.w[s_frame.n] = (write_t){ 0, rows, t0, w };", write)
        self.assertIn("s_frame.n++;", write)
        self.assertIn("te_done(&s_te_whole, 0, rows, t0, w, fall, p, scan);", write)
        # The frames' time, learned as video's is (quick to rise), from whole frames only.
        self.assertRegex(write, r"if \(rows == s_rows\) \{\s*s_whole\.us = s_whole\.us \? \(s_whole\.us \* 7 \+ \(w > s_whole\.us \? "
                                r"2 \* w - s_whole\.us : w\)\) / 8 : w;")

    def test_the_frame_is_psram_made_once_and_kept(self):
        # 466 x 466 x 2 bytes, from PSRAM only (never internal or DMA RAM), made as frame mode registers (TC-m6) and never freed;
        # its state is in PSRAM too (only tasks touch it, with plain loads and stores). The switch never allocates.
        source = variant(SOURCE.read_text(), apps=True)
        make = strip_c(function_text(source, "bool muse_lcd_bands_make_frame(void)"))
        self.assertIn("heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)", make)
        self.assertNotRegex(make, r"MALLOC_CAP_(INTERNAL|DMA)|\bfree\(|heap_caps_free")
        self.assertNotRegex(strip_c(function_text(source, "void muse_lcd_bands_set_frame_mode(")), r"heap_caps|alloc\(")
        self.assertNotRegex(strip_c(source), r"\bfree\(s_whole|heap_caps_free\(s_whole")
        for line in ("static EXT_RAM_BSS_ATTR whole_t s_whole;", "static EXT_RAM_BSS_ATTR bool s_framing, s_framed;",
                     "static EXT_RAM_BSS_ATTR te_stats_t s_te_whole;", "static EXT_RAM_BSS_ATTR int s_cols;"):
            self.assertIn(line, source)
        self.assertEqual(466 * 466 * 2, 434312)


class FrameAtRegisterTest(unittest.TestCase):
    """muse_lcd_bands_make_frame and muse_lcd_bands_set_frame_mode, compiled with a fake heap (TC-m6): the frame is made from PSRAM
    as frame mode registers (the 1.75C's display_start, once), if PSRAM keeps WHOLE_RESERVE free after it, and kept, so the apps'
    PSRAM budget is the same before and after the first slide. With none, frame mode stays off for the boot (every refresh takes
    the band path) and says so once. The switch never allocates: never at the first slide."""

    PROGRAM = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)
typedef int esp_err_t;
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_FAIL"; }
static const char *TAG = "lcd_bands";
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
typedef struct async_memcpy_context_t *async_memcpy_handle_t;
static size_t g_free;
static bool g_fail;
static int g_allocs;
static unsigned g_caps;
static size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return g_free; }
static void *heap_caps_aligned_alloc(size_t align, size_t size, unsigned caps)
{
    g_allocs++;
    g_caps = caps;
    if (g_fail || size > g_free || align != 16) return NULL;
    g_free -= size;
    return malloc(size);
}
static int s_rows = 466;
PARTS
int main(void)
{
    char cmd[8];
    long long v;
    s_cols = 466;
    while (scanf("%7s %lld", cmd, &v) == 2) {
        if (strcmp(cmd, "free") == 0) {
            g_free = (size_t)v;
        } else if (strcmp(cmd, "fail") == 0) {
            g_fail = v != 0;
        } else if (strcmp(cmd, "tell") == 0) {
            tell_frame();   /* the send task's loop, at its next band */
        } else if (strcmp(cmd, "make") == 0) {
            bool made = muse_lcd_bands_make_frame();   /* as frame mode registers */
            printf("made %d allocs %d caps %u\n", made, g_allocs, g_caps);
        } else {
            muse_lcd_bands_set_frame_mode(v != 0);   /* a slide begins (1) or rests (0) */
            printf("framing %d allocs %d\n", s_framing, g_allocs);
        }
    }
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = variant(SOURCE.read_text(), apps=True)
        parts = [typedef(source, "te_stats_t"), typedef(source, "whole_t"), typedef(source, "dma_t"),
                 re.search(r"^#define WHOLE_RESERVE .*$", source, re.M).group(0)]
        parts += re.findall(r"^static EXT_RAM_BSS_ATTR [^;\n]*;", source, re.M)
        parts.append(function_text(source, "bool muse_lcd_bands_make_frame(void)"))
        parts.append(function_text(source, "void muse_lcd_bands_set_frame_mode("))
        parts.append(function(source, "tell_frame"))
        src = Path(cls.tmp.name) / "frame_mode.c"
        src.write_text(cls.PROGRAM.replace("PARTS", "\n".join(parts)))
        cls.exe = Path(cls.tmp.name) / "frame_mode"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-o", str(cls.exe),
                        str(src)], check=True)
        cls.reserve = source_number("WHOLE_RESERVE")

    def run_script(self, lines):
        out = subprocess.run([str(self.exe)], input="\n".join(lines) + "\n", check=True, capture_output=True, text=True).stdout
        return out.splitlines()

    def test_the_frame_is_made_at_register_and_the_switch_never_allocates(self):
        frame, psram = 466 * 466 * 2, (1 << 10) | (1 << 2)
        out = self.run_script([f"free {frame + self.reserve}", "on 1", "off 0", "make 0", "tell 0", "make 0", "on 1", "off 0",
                               "on 1", "tell 0"])
        self.assertEqual([line for line in out if not line.startswith(("I ", "W "))],
                         ["framing 0 allocs 0", "framing 0 allocs 0",              # before it registers: no frame, nothing taken
                          f"made 1 allocs 1 caps {psram}", f"made 1 allocs 1 caps {psram}",   # made once, from PSRAM, and kept
                          "framing 1 allocs 1", "framing 0 allocs 1", "framing 1 allocs 1"])
        infos = [line for line in out if line.startswith("I ")]
        self.assertEqual(infos, [f"I lcd_bands: tearing effect: frame mode's frame, {frame} B of PSRAM"])
        self.assertEqual(out.index(infos[0]), 3, "told at the send task's next band after it was made")
        self.assertFalse([line for line in out if line.startswith("W ")])

    def test_without_room_at_register_frame_mode_stays_off_for_the_boot(self):
        frame = 466 * 466 * 2
        out = self.run_script([f"free {frame + self.reserve - 1}", "make 0", "on 1", f"free {10 * frame}", "on 1", "make 0", "on 1",
                               "tell 0", "tell 0"])
        self.assertEqual([line for line in out if not line.startswith(("I ", "W "))],
                         ["made 0 allocs 0 caps 0", "framing 0 allocs 0", "framing 0 allocs 0", "made 0 allocs 0 caps 0",
                          "framing 0 allocs 0"], "short of the frame and the reserve: nothing taken, then or at a slide")
        warnings = [line for line in out if line.startswith("W ")]
        self.assertEqual(len(warnings), 1, out)
        self.assertIn("lcd_bands: tearing effect: ", warnings[0])   # an allowlisted line
        self.assertIn(f"{frame} B of PSRAM, {self.reserve} B kept free", warnings[0])
        self.assertFalse([line for line in out if line.startswith("I ")])
        # The allocation itself failing (no block that large) is the same: tried once, said once.
        out = self.run_script([f"free {10 * frame}", "fail 1", "make 0", "fail 0", "on 1", "make 0", "tell 0", "tell 0"])
        self.assertEqual([line for line in out if line.startswith(("made", "framing"))],
                         [f"made 0 allocs 1 caps {(1 << 10) | (1 << 2)}", "framing 0 allocs 1",
                          f"made 0 allocs 1 caps {(1 << 10) | (1 << 2)}"])
        self.assertEqual(sum(line.startswith("W ") for line in out), 1, out)

    def test_the_board_makes_it_as_frame_mode_registers(self):
        # The 1.75C's display_start, right after the bands register (s_cols and s_rows are set there) and before LVGL starts: an
        # apps build's alone, as frame mode is.
        board = BOARD_175C.read_text()
        start = function_text(board, "static lv_display_t *display_start(lv_indev_t **touch)")
        make = start.index("muse_lcd_bands_make_frame();")
        self.assertLess(start.index("muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);"), make)
        self.assertLess(make, start.index("esp_lv_adapter_start()"))
        self.assertEqual(open_conditions(board, board.index("muse_lcd_bands_make_frame();")), ["if CONFIG_MUSE_APPS"])
        header = HEADER.read_text()
        self.assertEqual(open_conditions(header, header.index("bool muse_lcd_bands_make_frame(void);")), ["if CONFIG_MUSE_APPS"])

    def test_the_switch_logs_nothing_and_the_send_loop_tells(self):
        # W1 fix C and Review Focus 1 (Task 1 of the perf plan): the switch runs on the LVGL task inside the slide's SCROLL_BEGIN
        # (muse_ui.c's slide_begins, deep in LVGL's input read), where vprintf's frame would sit on the thin stack, so it logs nothing;
        # the frame's making notes what happened, and the send task logs it at the top of its loop, at its next band, its shallowest
        # frame. Nothing of the bands logs from LVGL's flush or render callbacks either, nor the DMA's copies: only the send task's,
        # the registration and the TE pin's setup do.
        source = variant(SOURCE.read_text(), apps=True)
        for head in ("void muse_lcd_bands_set_frame_mode(", "bool muse_lcd_bands_make_frame(void)"):
            with self.subTest(head):
                self.assertNotRegex(strip_c(function_text(source, head)), r"ESP_LOG|printf")
        self.assertRegex(function(SOURCE.read_text(), "send_bands"),
                         r"xQueueReceive\(s_bands, &b, portMAX_DELAY\);\n#if CONFIG_MUSE_APPS\n\s*tell_frame\(\);")
        self.assertEqual(len(re.findall(r"\btell_frame\(\);", strip_c(source))), 1, "told from the send loop alone")
        self.assertEqual(logging_functions(source), {"te_done", "frame_done", "tell_frame", "muse_lcd_bands_register",
                                                     "muse_lcd_bands_set_te"})
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        for head in ("static void slide_begins(lv_event_t *e)", "static void slide_rests(lv_event_t *e)"):
            with self.subTest(head):
                self.assertNotRegex(strip_c(function_text(ui, head)), r"ESP_LOG|printf")
        self.assertIn("muse_board->frame_mode(true);", function_text(ui, "static void slide_begins(lv_event_t *e)"))
        self.assertIn(".frame_mode = muse_lcd_bands_set_frame_mode,", BOARD_175C.read_text())

    def test_the_reserve_is_the_apps_floor(self):
        # What the frame leaves the rest of the gadget is the apps' own floor (APPS_RAM_RESERVE): Muse's session and LVGL, which
        # aborts when it can't allocate.
        pkg = (ROOT / "components/muse/apps/apps_pkg.h").read_text()
        floor = re.search(r"^#define APPS_RAM_RESERVE \((\d+) \* (\d+)\)", pkg, re.M)
        self.assertIsNotNone(floor, "APPS_RAM_RESERVE's form changed")
        self.assertEqual(self.reserve, int(floor.group(1)) * int(floor.group(2)))


class FrameCopyTest(unittest.TestCase):
    """A bench build's switch of frame mode's copy (Task 2 of the perf plan, PF4): with it off (muse_lcd_bands_set_frame_copy(false),
    the console's '>frame copy off'), a slide's whole-screen refresh goes on the band path, each band sent as LVGL flushes it and
    nothing copied (it tears), so the batch line's render is the slide's without the copies. LV_EVENT_RENDER_START and queue_band
    compiled with the display and the queue faked."""

    PROGRAM = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MUSE_BENCH 1
#define EXT_RAM_BSS_ATTR
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef struct { uint32_t data_size; } lv_draw_buf_t;
typedef struct {
    lv_area_t inv_areas[8];
    uint8_t inv_area_joined[8];
    uint32_t inv_p;
    lv_draw_buf_t *buf_act;
    lv_area_t refreshed_area;
    bool last;
} lv_display_t;
typedef enum { LV_EVENT_RENDER_START = 1, LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH } lv_event_code_t;
typedef struct { lv_event_code_t code; } lv_event_t;
static lv_event_code_t lv_event_get_code(lv_event_t *e) { return e->code; }
static bool lv_display_flush_is_last(lv_display_t *disp) { return disp->last; }
typedef void *esp_lcd_panel_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef int portMUX_TYPE;
typedef struct async_memcpy_context_t *async_memcpy_handle_t;
#define portENTER_CRITICAL(lock) (void)(lock)
#define portEXIT_CRITICAL(lock) (void)(lock)
#define pdTRUE 1
static void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }
static int64_t esp_timer_get_time(void) { return 0; }
TYPES
static band_t g_band;
static int xQueueSend(QueueHandle_t q, const band_t *b, int wait) { (void)q; (void)wait; g_band = *b; return pdTRUE; }
static lv_draw_buf_t g_buf = { 466 * 118 * 2 };
static lv_display_t g_disp = { .buf_act = &g_buf };
static lv_display_t *s_disp = &g_disp;
static QueueHandle_t s_bands;
static portMUX_TYPE s_lock;
static int s_rows = 466;
static bool s_pairing;
static int s_parts, s_flushed;
static volatile uint32_t s_drawn;
static lv_area_t s_drawn_area;
static SemaphoreHandle_t s_drawn_sem;
static int64_t s_render_t0, s_render_waited, s_wait_t0;
PARTS
/* One step a line: "framing 0|1" (frame mode, with its frame), "copy 0|1" (the switch), "whole" (a refresh of the whole screen in
 * LVGL's four bands) or "part" (one of an area of 200 x 100): each band answered as it was queued. */
int main(void)
{
    char cmd[16];
    int v = 0;
    s_cols = 466;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "framing") && scanf("%d", &v) == 1) {
            s_framing = v != 0;
        } else if (!strcmp(cmd, "copy") && scanf("%d", &v) == 1) {
            muse_lcd_bands_set_frame_copy(v != 0);
        } else if (!strcmp(cmd, "whole") || !strcmp(cmd, "part")) {
            bool whole = !strcmp(cmd, "whole");
            g_disp.inv_areas[0] = whole ? (lv_area_t){ 0, 0, 465, 465 } : (lv_area_t){ 100, 100, 299, 199 };
            g_disp.inv_area_joined[0] = 0;
            g_disp.inv_p = 1;
            lv_event_t e = { LV_EVENT_RENDER_START };
            on_render_event(&e);
            static const int rows[][2] = { { 0, 118 }, { 118, 236 }, { 236, 354 }, { 354, 466 } };
            for (int i = 0; i < (whole ? 4 : 1); i++) {
                g_disp.last = !whole || i == 3;
                if (whole) {
                    queue_band(s_disp, NULL, 0, rows[i][0], 466, rows[i][1], "", NULL);
                } else {
                    queue_band(s_disp, NULL, 100, 100, 300, 200, "", NULL);
                }
                printf("%d-%d whole %d pair %d\n", g_band.y1, g_band.y2, g_band.whole, g_band.pair);
            }
        }
    }
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = variant(SOURCE.read_text(), apps=True)
        types = [typedef(source, "te_stats_t"), typedef(source, "whole_t"), typedef(source, "dma_t"), typedef(source, "band_t")]
        parts = re.findall(r"^static EXT_RAM_BSS_ATTR [^;\n]*;", source, re.M)
        parts += [function(source, name) for name in ("refresh_parts", "band_pairs", "refresh_whole", "frame_band", "queue_band",
                                                      "on_render_event")]
        parts.append(function_text(source, "void muse_lcd_bands_set_frame_copy("))
        src = Path(cls.tmp.name) / "frame_copy.c"
        src.write_text(cls.PROGRAM.replace("TYPES", "\n".join(types)).replace("PARTS", "\n".join(parts)))
        cls.exe = Path(cls.tmp.name) / "frame_copy"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-o", str(cls.exe),
                        str(src)], check=True)

    def run_script(self, lines):
        return subprocess.run([str(self.exe)], input="\n".join(lines) + "\n", check=True, capture_output=True,
                              text=True).stdout.splitlines()

    def test_copy_off_sends_a_slides_whole_screen_as_bands(self):
        framed = ["0-118 whole 1 pair 0", "118-236 whole 1 pair 0", "236-354 whole 1 pair 0", "354-466 whole 1 pair 0"]
        bands = [line.replace("whole 1", "whole 0") for line in framed]
        self.assertEqual(self.run_script(["framing 1", "whole", "copy 0", "whole", "part", "copy 1", "whole", "framing 0", "whole"]),
                         framed + bands + ["100-200 whole 0 pair 0"] + framed + bands)

    def test_the_switch_is_the_benchs_and_the_apps_alone(self):
        # Built only with the apps (frame mode's) and the bench: the plain build stays 7e59ee0's (PlainBuildTest), and the console's
        # line reaches it through muse_board_t frame_copy, a bench build's field that only the 1.75C's apps build sets.
        text = SOURCE.read_text()
        for name in ("void muse_lcd_bands_set_frame_copy(bool on)", "s_copy_off"):
            for m in re.finditer(re.escape(name), text):
                with self.subTest(name, at=m.start()):
                    self.assertEqual(open_conditions(text, m.start()), ["if CONFIG_MUSE_APPS", "if CONFIG_MUSE_BENCH"])
        header = HEADER.read_text()
        self.assertEqual(open_conditions(header, header.index("void muse_lcd_bands_set_frame_copy(bool on);")),
                         ["if CONFIG_MUSE_APPS", "if CONFIG_MUSE_BENCH"])
        board_h = BOARD_H.read_text()
        self.assertEqual(open_conditions(board_h, board_h.index("void (*frame_copy)(bool on);")), ["if CONFIG_MUSE_BENCH"])
        board = BOARD_175C.read_text()
        self.assertEqual(open_conditions(board, board.index(".frame_copy = muse_lcd_bands_set_frame_copy,")),
                         ["if CONFIG_MUSE_APPS && CONFIG_MUSE_BENCH"])
        # The switch logs nothing: the console's line answers it.
        self.assertNotRegex(strip_c(function_text(variant(text, True), "void muse_lcd_bands_set_frame_copy(")), r"ESP_LOG|printf")


class FlushWaitTest(unittest.TestCase):
    """TC-m5: LVGL's wait for the band before to be flushed (lv_display_set_flush_wait_cb) blocks on a semaphore that every
    lv_display_flush_ready gives (the band path's last piece, in the SPI interrupt; frame mode's copies, on the send task),
    instead of LVGL's busy loop on disp->flushing, which held the UI core through each frame's hold and write. flush_wait is
    compiled with the semaphore faked."""

    PROGRAM = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void *SemaphoreHandle_t;
typedef struct { volatile int flushing; } lv_display_t;
static lv_display_t g_disp;
static SemaphoreHandle_t s_flush_ready = (SemaphoreHandle_t)2;
static int g_takes, g_ready_at, g_stale;
/* The LVGL task blocks: a give left from a flush LVGL didn't wait for returns at once; the take numbered g_ready_at is woken by
 * the give that came with the flush's end; any other waits its ticks out with no give. */
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (s != s_flush_ready || ++g_takes > 50) abort();
    printf("take %u\n", (unsigned)ticks);
    if (g_stale) {
        g_stale = 0;
        return pdTRUE;
    }
    if (g_takes == g_ready_at) {
        g_disp.flushing = 0;
        return pdTRUE;
    }
    return pdFALSE;
}
PARTS
/* One wait a line: flushing ready_at stale. */
int main(void)
{
    int flushing;
    while (scanf("%d %d %d", &flushing, &g_ready_at, &g_stale) == 3) {
        g_disp.flushing = flushing;
        g_takes = 0;
        flush_wait(&g_disp);
        printf("back with flushing %d after %d takes\n", g_disp.flushing, g_takes);
    }
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = variant(SOURCE.read_text(), apps=True)
        parts = [re.search(r"^#define FLUSH_WAIT_MS .*$", source, re.M).group(0), function(source, "flush_wait")]
        src = Path(cls.tmp.name) / "flush_wait.c"
        src.write_text(cls.PROGRAM.replace("PARTS", "\n".join(parts)))
        cls.exe = Path(cls.tmp.name) / "flush_wait"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)
        cls.ms = source_number("FLUSH_WAIT_MS")

    def wait(self, flushing, ready_at, stale=0):
        out = subprocess.run([str(self.exe)], input=f"{flushing} {ready_at} {stale}\n", check=True, capture_output=True,
                             text=True).stdout.splitlines()
        return out

    def test_the_wait_blocks_until_the_flush_is_ready(self):
        self.assertGreater(self.ms, 0)
        take = f"take {self.ms}"
        self.assertEqual(self.wait(0, 0), ["back with flushing 0 after 0 takes"], "a flush already ready: no wait")
        self.assertEqual(self.wait(1, 1), [take, "back with flushing 0 after 1 takes"], "woken by the give at the flush's end")
        self.assertEqual(self.wait(1, 2, stale=1), [take, take, "back with flushing 0 after 2 takes"],
                         "a give from a flush LVGL didn't wait for returns at once: the flag is checked again, and it blocks again")
        self.assertEqual(self.wait(1, 4), [take] * 4 + ["back with flushing 0 after 4 takes"],
                         "each wake without the flush ready blocks again (FLUSH_WAIT_MS each), never a busy loop")

    def test_every_flush_ready_wakes_the_wait(self):
        source = variant(SOURCE.read_text(), apps=True)
        code = strip_c(source)
        register = strip_c(function_text(source, "lv_display_t *muse_lcd_bands_register("))
        self.assertIn("s_flush_ready = xSemaphoreCreateBinary();", register)
        self.assertRegex(register, r"if \(([^{]*?)!s_flush_ready[^{]*?xTaskCreatePinnedToCore\(send_bands", "made before the send task, "
                                   "or registering fails")
        self.assertLess(register.index("s_disp = esp_lv_adapter_register_display(&cfg);"),
                        register.index("lv_display_set_flush_wait_cb(s_disp, flush_wait);"))
        # Every flush_ready in the code gives the semaphore right after it: the send task's (band_back, a failed piece) and the SPI
        # interrupt's (the adapter's notify, at a band's last piece).
        readies = len(re.findall(r"lv_display_flush_ready\(s_disp\);", code))
        self.assertGreaterEqual(readies, 2)
        self.assertEqual(len(re.findall(r"lv_display_flush_ready\(s_disp\);\s*xSemaphoreGive\(s_flush_ready\);", code)), readies)
        sent = function(code, "on_chunk_sent")
        notify = sent.index("esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);")
        self.assertRegex(sent[notify:], r"^[^}]*?if \(last\) \{\s*xSemaphoreGiveFromISR\(s_flush_ready, &woken\);\s*\}")
        self.assertRegex(sent[notify:], r"if \(yield \|\| woken == pdTRUE\) \{\s*portYIELD_FROM_ISR\(\);")
        # The adapter's own flush_ready calls come from inside LVGL's flush callback, before LVGL waits: flush_wait isn't called
        # then (LVGL 9.5.0's wait_for_flushing calls it only while disp->flushing).
        self.assertIn("lv_display_set_flush_wait_cb", strip_c(variant(SOURCE.read_text(), apps=True)))
        self.assertNotIn("s_flush_ready", variant(SOURCE.read_text(), apps=False))


class FrameDmaTest(unittest.TestCase):
    """Frame mode's copies by DMA (PF4, ruling P10): whole_band and the DMA's helpers, compiled with esp_async_memcpy, the
    semaphores and LVGL faked. Each band is copied into the frame by the GDMA while the send task waits, blocked, so the CPU is
    LVGL's to draw the next band; the band goes back to LVGL only once its copy has ended, and the frame is written only once its
    last band's copy has. A band the DMA can't take is copied by CPU as before, and the first such is said once. The driver is
    installed at a slide's first copy and uninstalled once frame mode is off, so its internal RAM is back between slides."""

    PROGRAM = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_MUSE_BENCH 1
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_INTERNAL (1u << 11)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
static const char *esp_err_to_name(esp_err_t e)
{
    return e == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : e == ESP_ERR_INVALID_ARG ? "ESP_ERR_INVALID_ARG"
         : e == ESP_ERR_NOT_FOUND ? "ESP_ERR_NOT_FOUND" : e == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_FAIL";
}
static const char *TAG = "lcd_bands";
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void *SemaphoreHandle_t;
typedef struct { volatile int flushing; } lv_display_t;
static lv_display_t g_disp;
static lv_display_t *s_disp = &g_disp;
static int64_t esp_timer_get_time(void) { return 0; }
static size_t g_dma_free = 64 * 1024;
static size_t heap_caps_get_free_size(unsigned caps) { return caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) ? g_dma_free : 0; }

/* esp_async_memcpy, as ESP-IDF v6.0.1's esp_async_memcpy.h has it, faked: a copy lands, and its callback runs as its interrupt
 * would, once the send task blocks waiting for it (unless copies never end). One copy at a time, on the driver installed. */
typedef struct async_memcpy_context_t *async_memcpy_handle_t;
typedef struct { void *data; } async_memcpy_event_t;
typedef bool (*async_memcpy_isr_cb_t)(async_memcpy_handle_t mcp_hdl, async_memcpy_event_t *event, void *cb_args);
typedef struct { uint32_t backlog; uint32_t weight; size_t dma_burst_size; uint32_t flags; } async_memcpy_config_t;
#define ASYNC_MEMCPY_DEFAULT_CONFIG() { .backlog = 8, .weight = 0, .dma_burst_size = 16, .flags = 0 }
static int g_ctx;
static bool g_installed;
static esp_err_t g_install_err, g_copy_err;
static int g_ends = 1;
static uint8_t *g_frame, *g_px;   /* the block, and the frame in it */
static size_t g_burst;
/* esp_cache_msync's write-back (C2M), recorded: from where, how far, in how many pieces and the largest. */
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED (1 << 1)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 2)
static struct { const uint8_t *from, *to; int pieces; size_t most; bool bad; } g_sync;
static esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    const uint8_t *at = addr;
    g_sync.bad |= flags != (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) || (g_sync.to && at != g_sync.to);
    g_sync.from = g_sync.from ? g_sync.from : at;
    g_sync.to = at + size;
    g_sync.pieces++;
    g_sync.most = size > g_sync.most ? size : g_sync.most;
    return ESP_OK;
}
static struct { uint8_t *dst; const uint8_t *src; size_t n; async_memcpy_isr_cb_t cb; void *arg; bool on; } g_copy;
static esp_err_t esp_async_memcpy_install(const async_memcpy_config_t *config, async_memcpy_handle_t *mcp)
{
    printf("install backlog %u burst %u%s\n", (unsigned)config->backlog, (unsigned)config->dma_burst_size,
           g_install_err ? ": refused" : "");
    if (g_install_err) return g_install_err;
    if (g_installed) abort();
    g_installed = true;
    g_burst = config->dma_burst_size;
    *mcp = (async_memcpy_handle_t)&g_ctx;
    return ESP_OK;
}
static esp_err_t esp_async_memcpy_uninstall(async_memcpy_handle_t mcp)
{
    if (mcp != (async_memcpy_handle_t)&g_ctx || !g_installed) abort();
    if (g_copy.on) {
        printf("uninstall: refused, a copy is under way\n");
        return ESP_ERR_INVALID_STATE;
    }
    g_installed = false;
    printf("uninstall\n");
    return ESP_OK;
}
/* As ESP-IDF v6.0.1 checks on the S3 (check_buffer_alignment): the GDMA writes PSRAM in whole bursts (GDMA_LL_AHB_RX_BURST_NEEDS_ALIGNMENT),
 * so the destination and the length must be whole bursts; it reads at any alignment (AHB GDMA version 1). */
static esp_err_t esp_async_memcpy(async_memcpy_handle_t mcp, void *dst, void *src, size_t n, async_memcpy_isr_cb_t cb, void *arg)
{
    if (mcp != (async_memcpy_handle_t)&g_ctx || !g_installed || g_copy.on || !cb) abort();
    if (((uintptr_t)dst & (g_burst - 1)) || (n & (g_burst - 1))) {
        printf("dma: address|size not aligned\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (g_copy_err) {
        printf("dma: refused\n");
        return g_copy_err;
    }
    g_copy.dst = dst;
    g_copy.src = src;
    g_copy.n = n;
    g_copy.cb = cb;
    g_copy.arg = arg;
    g_copy.on = true;
    bool whole = !g_sync.bad && g_sync.from == src && g_sync.to == (uint8_t *)src + n;
    printf("dma %ld+%zu, written back first%s in %d pieces of at most %zu\n", (long)((uint8_t *)dst - g_px), n,
           whole ? "" : " NOT WHOLE", g_sync.pieces, g_sync.most);
    memset(&g_sync, 0, sizeof g_sync);
    return ESP_OK;
}
static SemaphoreHandle_t s_wake = (SemaphoreHandle_t)1;
static SemaphoreHandle_t s_flush_ready = (SemaphoreHandle_t)2;
static int g_wake;
static TickType_t g_ticks;
static TickType_t xTaskGetTickCount(void) { return g_ticks; }
static BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    if (s != s_wake) abort();
    g_wake = 1;
    *woken = pdTRUE;
    return pdTRUE;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s != s_flush_ready) abort();
    printf("flush wait woken\n");
    return pdTRUE;
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (s != s_wake) abort();
    if (g_copy.on && g_ends) {
        memcpy(g_copy.dst, g_copy.src, g_copy.n);
        g_copy.on = false;
        printf("dma end\n");
        async_memcpy_event_t e = { 0 };
        g_copy.cb((async_memcpy_handle_t)&g_ctx, &e, g_copy.arg);
    }
    if (g_wake) {
        g_wake = 0;
        return pdTRUE;
    }
    g_ticks += ticks;
    return pdFALSE;
}

/* LVGL's two draw buffers, the bands of the screen drawn into them by turns, each a pattern of its own. */
static uint8_t g_buf[2][466 * 118 * 2];
static int g_band;
static bool band_in_frame(int k);
static void lv_display_flush_ready(lv_display_t *disp)
{
    disp->flushing = 0;
    printf("flush ready: band %s\n", band_in_frame(g_band) ? "in the frame" : "NOT in the frame");
}
TYPES
static void whole_write(int rows, bool hold);
static void frame_done(const band_t *b);
PARTS
static const int g_rows_of[4][2] = { { 0, 118 }, { 118, 236 }, { 236, 354 }, { 354, 466 } };
static uint8_t pattern(int k, size_t i) { return (uint8_t)(k * 61 + i * 7 + (i >> 9)); }
static bool band_in_frame(int k)
{
    size_t row = 466 * 2, n = (size_t)(g_rows_of[k][1] - g_rows_of[k][0]) * row;
    const uint8_t *at = g_px + (size_t)g_rows_of[k][0] * row;
    for (size_t i = 0; i < n; i++) {
        if (at[i] != pattern(k, i)) return false;
    }
    return true;
}
static void whole_write(int rows, bool hold)
{
    bool all = true;
    for (int k = 0; k < 4; k++) all = all && band_in_frame(k);
    printf("write %d%s: %s\n", rows, hold ? " held" : "", all ? "every band in the frame" : "A BAND MISSING");
}
static void frame_done(const band_t *b) { printf("frame done%s\n", b->last ? "" : " (not last?)"); }
/* One step a line: "band K" (LVGL flushes band K of a whole screen, 0 to 3, 3 the last, drawn into buffer K % 2), "frame" (all
 * four), "item" (the send loop's next item, not a whole band), "tell", "framing 0|1", "free N" (internal DMA RAM), "install E",
 * "copy E" (the driver's next answers: 0 or an error), "ends 0|1", "cpu" (the bench's count of bands copied by CPU). */
int main(void)
{
    char cmd[16];
    long long v = 0;
    s_cols = 466;
    g_frame = aligned_alloc(64, (466 * 466 * 2 + 64 + 63) & ~63);
    s_whole.px = g_px = g_frame + 16;   /* as heap_caps_aligned_alloc(16, ...) may give it: 16-aligned, not 64 */
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "band") || !strcmp(cmd, "frame")) {
            int from = 0, to = 3;
            if (!strcmp(cmd, "band")) {
                if (scanf("%lld", &v) != 1) return 1;
                from = to = (int)v;
            }
            for (int k = from; k <= to; k++) {
                uint8_t *buf = g_buf[k % 2];
                size_t n = (size_t)(g_rows_of[k][1] - g_rows_of[k][0]) * 466 * 2;
                for (size_t i = 0; i < n; i++) buf[i] = pattern(k, i);   /* LVGL draws it */
                band_t b = { buf, 0, g_rows_of[k][0], 466, g_rows_of[k][1], k == 3, false, true, 0, 0 };
                g_band = k;
                g_disp.flushing = 1;
                whole_band(&b);
                if (g_disp.flushing) printf("band %d never went back to LVGL\n", k);
                memset(buf, 0xEE, n);   /* back with LVGL, which draws the band after next into it */
            }
        } else if (!strcmp(cmd, "item")) {
            tell_frame();
            dma_end();
        } else if (!strcmp(cmd, "tell")) {
            tell_frame();
        } else if (!strcmp(cmd, "cpu")) {
            printf("cpu bands %u\n", (unsigned)s_frame.cpu_bands);
        } else if (scanf("%lld", &v) == 1) {
            if (!strcmp(cmd, "framing")) s_framing = v != 0;
            else if (!strcmp(cmd, "free")) g_dma_free = (size_t)v;
            else if (!strcmp(cmd, "install")) g_install_err = (esp_err_t)v;
            else if (!strcmp(cmd, "copy")) g_copy_err = (esp_err_t)v;
            else if (!strcmp(cmd, "ends")) g_ends = (int)v;
        }
    }
    return 0;
}
'''

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = variant(SOURCE.read_text(), apps=True)
        types = [typedef(source, "te_stats_t"), typedef(source, "write_t"), typedef(source, "hold_t"), typedef(source, "whole_t"),
                 typedef(source, "dma_t"), typedef(source, "band_t")]
        parts = [re.search(rf"^#define {name} .*$", source, re.M).group(0)
                 for name in ("FRAME_WRITES_MAX", "WHOLE_RESERVE", "DMA_BURST", "DMA_WAIT_MS", "DMA_KEEP", "DMA_TAKES",
                              "DMA_SYNC_BYTES")]
        parts.append(re.search(r"^static EXT_RAM_BSS_ATTR struct \{\n    write_t w\[FRAME_WRITES_MAX\];.*?^\} s_frame;", source, re.M | re.S).group(0))
        parts += re.findall(r"^static EXT_RAM_BSS_ATTR [^;\n]*;", source, re.M)
        parts.append(re.search(r"^static volatile bool s_copied;", source, re.M).group(0))
        parts.append("static int s_rows = 466;")
        parts += [function(source, name) for name in ("on_copied", "dma_fell_back", "dma_copy", "dma_end", "band_back", "whole_band",
                                                      "tell_frame")]
        src = Path(cls.tmp.name) / "frame_dma.c"
        src.write_text(cls.PROGRAM.replace("TYPES", "\n".join(types)).replace("PARTS", "\n".join(parts)))
        cls.exe = Path(cls.tmp.name) / "frame_dma"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-variable", "-Wno-unused-function", "-o", str(cls.exe),
                        str(src)], check=True)
        cls.keep, cls.takes = source_number("DMA_KEEP"), source_number("DMA_TAKES")
        cls.wait_ms = source_number("DMA_WAIT_MS")

    def run_script(self, lines):
        return subprocess.run([str(self.exe)], input="\n".join(lines) + "\n", check=True, capture_output=True,
                              text=True).stdout.splitlines()

    @classmethod
    def dma_band(cls, k):
        """A band's events copied by DMA: the bulk from its first byte on a 64-byte burst of the frame (which is 16 bytes past one
        here) to its last whole burst, written back from the cache a piece at a time, its copy, its end, then (only then) back to
        LVGL and its flush wait woken. The ends, under a burst each, by CPU."""
        rows = [(0, 118), (118, 236), (236, 354), (354, 466)][k]
        start, end = rows[0] * 932, rows[1] * 932
        first = start + (-(16 + start)) % 64
        n = (end - first) - (end - first) % 64
        sync = source_number("DMA_SYNC_BYTES")
        return [f"dma {first}+{n}, written back first in {math.ceil(n / sync)} pieces of at most {sync}", "dma end",
                "flush ready: band in the frame", "flush wait woken"]

    @staticmethod
    def cpu_band():
        return ["flush ready: band in the frame", "flush wait woken"]

    WRITE = ["write 466 held: every band in the frame", "frame done"]

    def test_each_band_goes_back_to_lvgl_only_once_its_dma_copy_has_ended(self):
        out = self.run_script(["framing 1", "frame", "cpu", "frame", "cpu", "item"])
        slide = self.dma_band(0) + self.dma_band(1) + self.dma_band(2) + self.dma_band(3) + self.WRITE
        self.assertEqual(out, ["install backlog 1 burst 64"] + slide + ["cpu bands 0"] + slide + ["cpu bands 0"],
                         "installed at the slide's first copy, one copy at a time; kept while frame mode is on")

    def test_the_driver_goes_once_frame_mode_is_off(self):
        # A slide's last refresh: frame mode goes off at its LV_EVENT_REFR_READY, as LVGL flushes its last band, so the driver goes
        # after the frame's write; the next slide installs it again. Frame mode off mid-refresh leaves the refresh's copies as they
        # are (the driver goes at its end), and an item between slides takes it too.
        slide = self.dma_band(0) + self.dma_band(1) + self.dma_band(2) + self.dma_band(3) + self.WRITE
        out = self.run_script(["framing 1", "band 0", "band 1", "band 2", "framing 0", "band 3", "item",
                               "framing 1", "frame", "framing 0", "item", "item"])
        self.assertEqual(out, ["install backlog 1 burst 64"] + slide + ["uninstall"] +
                         ["install backlog 1 burst 64"] + slide + ["uninstall"])

    def test_no_channel_copies_by_cpu_and_says_so_once(self):
        # No GDMA channel (or no memory for the driver): every band by CPU, back to LVGL after its copy, the frame whole; the DMA is
        # given up for the boot (one try, so the driver's own error is logged once at most), and the first reason said once.
        out = self.run_script(["install 261", "framing 1", "frame", "cpu", "tell", "item", "framing 0", "item", "framing 1", "frame",
                               "tell", "item", "cpu"])
        cpu = self.cpu_band() * 4 + self.WRITE
        warning = "W lcd_bands: tearing effect: frame mode's copy by CPU (DMA install: ESP_ERR_NOT_FOUND)"
        self.assertEqual(out, ["install backlog 1 burst 64: refused"] + cpu + ["cpu bands 4", warning] + cpu + ["cpu bands 8"])

    def test_a_copy_the_dma_refuses_is_copied_by_cpu(self):
        # An alignment or argument the driver refuses: that band by CPU, the DMA given up for the boot and its driver gone after the
        # frame's write, every band in the frame.
        out = self.run_script(["copy 258", "framing 1", "band 0", "copy 0", "band 1", "band 2", "band 3", "cpu", "item", "item"])
        warning = "W lcd_bands: tearing effect: frame mode's copy by CPU (DMA copy: ESP_ERR_INVALID_ARG)"
        self.assertEqual(out, ["install backlog 1 burst 64", "dma: refused"] + self.cpu_band() * 4 + self.WRITE +
                         ["uninstall", "cpu bands 4", warning])

    def test_a_copy_that_never_ends_is_copied_by_cpu_and_the_dma_left_alone(self):
        # A copy whose end never comes (DMA_WAIT_MS): that band by CPU and back to LVGL, the DMA given up for the boot and its driver
        # left as it is (it can't be uninstalled with a copy under way), every band in the frame.
        out = self.run_script(["ends 0", "framing 1", "band 0", "ends 1", "band 1", "band 2", "band 3", "cpu", "item", "framing 0",
                               "item"])
        warning = "W lcd_bands: tearing effect: frame mode's copy by CPU (DMA end: ESP_ERR_TIMEOUT)"
        self.assertEqual(out, ["install backlog 1 burst 64", self.dma_band(0)[0]] + self.cpu_band() * 4 + self.WRITE +
                         ["cpu bands 4", warning])

    def test_short_of_internal_ram_the_copies_wait_for_room(self):
        # Installed only while internal DMA RAM keeps DMA_KEEP (the Link's TX margin and a VM read's) and DMA_TAKES (what the driver
        # and a copy take) free, and run while it keeps DMA_KEEP, the driver's own share then taken (the board's first run counted
        # it twice, and all but each slide's first band went by CPU): short of that, by CPU, and the DMA is tried again at the next
        # band.
        installs, runs, short = self.keep + self.takes, self.keep, self.keep - 1
        out = self.run_script([f"free {installs - 1}", "framing 1", "band 0", "band 1", f"free {installs}", "band 2", f"free {runs}",
                               "band 3", "cpu", "tell", f"free {short}", "frame", "cpu", "tell", f"free {runs}", "frame", "cpu"])
        warning = "W lcd_bands: tearing effect: frame mode's copy by CPU (DMA room: ESP_ERR_NO_MEM)"
        self.assertEqual(out, self.cpu_band() * 2 + ["install backlog 1 burst 64"] + self.dma_band(2) + self.dma_band(3) +
                         self.WRITE + ["cpu bands 2", warning] + self.cpu_band() * 4 + self.WRITE + ["cpu bands 6"] +
                         self.dma_band(0) + self.dma_band(1) + self.dma_band(2) + self.dma_band(3) + self.WRITE + ["cpu bands 6"])

    def test_the_band_is_written_back_a_piece_at_a_time(self):
        # The driver writes the band back from the cache itself in one critical section (ESP-IDF v6.0.1's esp_cache_msync, with
        # CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS off as in the 1.75C's builds): up to the cache's 64 KB of dirty lines with the
        # UI core's interrupts held, the TE pin's among them. Written back first a piece of at most DMA_SYNC_BYTES at a time,
        # the whole band from its first byte (dma_band's line says so), its own finds nothing to write.
        sync = source_number("DMA_SYNC_BYTES")
        self.assertLessEqual(sync, 4096)
        out = self.run_script(["framing 1", "band 0", "band 3"])
        self.assertEqual([line for line in out if line.startswith("dma ") and "end" not in line],
                         [self.dma_band(0)[0], self.dma_band(3)[0]])
        config = (ROOT / "devices/sdkconfig.muse-waveshare-s3-175c").read_text() + (ROOT / "devices/sdkconfig.muse-apps").read_text()
        self.assertNotIn("CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS=y", config)

    def test_the_copies_leave_the_links_margin_and_burst_as_memtest_timed(self):
        noise = re.search(r"^#define NOISE_TX_DMA_RESERVE_BYTES \((\d+) \* (\d+)\)", (ROOT / "main/noise_tunnel_internal.h").read_text(),
                          re.M)
        vm = re.search(r"^#define VM_RESERVE_BYTES\s+\((\d+) \* (\d+)\)", (ROOT / "main/vm_file.c").read_text(), re.M)
        self.assertEqual(self.keep, int(noise.group(1)) * int(noise.group(2)) + int(vm.group(1)) * int(vm.group(2)))
        bench = (ROOT / "components/muse/muse_bench_mem.h").read_text()
        self.assertEqual(source_number("DMA_BURST"), int(re.search(r"^#define MUSE_MEMTEST_ALIGN (\d+)", bench, re.M).group(1)))
        self.assertEqual(source_number("DMA_BURST"), 64)   # ESP-IDF v6.0.1's GDMA_LL_MAX_BURST_SIZE_PSRAM on the S3

    def test_the_refresh_record_is_in_psram_so_internal_ram_is_not_reduced(self):
        # The flush wait's semaphore and the copies' flag take internal RAM (about 100 B; the board's boot line "UI built: free
        # internal" fell by 116 B): the refresh's record (s_frame), the send task's alone (its interrupts never touch it), goes to
        # PSRAM for it, and internal RAM is no less than before.
        source = variant(SOURCE.read_text(), apps=True)
        self.assertRegex(source, r"(?m)^static EXT_RAM_BSS_ATTR struct \{\n    write_t w\[FRAME_WRITES_MAX\];")
        for isr in ("chunk_done", "on_chunk_sent", "on_te", "on_copied"):
            with self.subTest(isr):
                self.assertNotIn("s_frame", function(source, isr))

    def test_the_send_loop_gives_the_driver_back_between_slides(self):
        # Any item that isn't a band of a whole screen (a band, a call, video's frame) lets the driver go once frame mode is off;
        # the copies' state is the send task's alone, in PSRAM, but for the flag the DMA's interrupt sets (internal RAM).
        source = variant(SOURCE.read_text(), apps=True)
        send = function(source, "send_bands")
        self.assertRegex(send, r"tell_frame\(\);\n\s*if \(!b\.whole\) \{\n\s*dma_end\(\);")
        self.assertIn("static EXT_RAM_BSS_ATTR dma_t s_dma;", source)
        self.assertRegex(source, r"(?m)^static volatile bool s_copied;")
        # The bench's batch line counts the bands copied by CPU.
        done = strip_c(function(source, "frame_done"))
        self.assertIn("s_batch.cpu += s_frame.cpu_bands;", done)
        self.assertIn("s_frame.cpu_bands = 0;", done)
        self.assertIn("s_batch.framed_torn += torn && s_frame.framed;", done)
        self.assertIn('%u framed (%u torn, %u bands by CPU); "', source)


def logging_functions(source):
    """The names of source's functions (each opening brace on the line after its head, this file's style) that log."""
    names = set()
    for match in re.finditer(r"^(?:static )?[\w ]+?\**\b(\w+)\([^;{}]*\)\n\{", source, re.M):
        brace = source.index("\n{", match.start()) + 1
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if re.search(r"\bESP_LOG[EWID]\(|\bprintf\(", strip_c(source[brace:end])):
            names.add(match.group(1))
    return names


class TornLineTest(unittest.TestCase):
    """frame_done's torn-refresh line (a bench build's), compiled with what it calls stubbed: each line spins the console in the send
    task (priority 6, above LVGL), so it is printed for the first torn refresh of each batch of FRAME_BATCH refreshes alone; the batch
    line counts them all (Task 1 of the perf plan)."""

    PROGRAM = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MUSE_BENCH 1
#define EXT_RAM_BSS_ATTR
static const char *TAG = "lcd_bands";
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
static unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 596; }
static int64_t s_gap_us = 300;
static bool g_torn;
static void wire_idle(int held) { (void)held; }
static void band_gone(void) {}
PARTS
static bool frame_meets(const write_t *w, int n, int64_t fall, int64_t p, int64_t scan)
{
    (void)w; (void)n; (void)fall; (void)p; (void)scan;
    return g_torn;
}
FRAME_DONE
/* One refresh of two bands a line: 1 torn, 0 not. Each is announced, then judged. */
int main(void)
{
    int torn, k = 0;
    while (scanf("%d", &torn) == 1) {
        printf("refresh %d\n", k++);
        g_torn = torn != 0;
        s_frame.fall = 0;
        s_frame.p = 16800;
        s_frame.scan = 16200;
        s_frame.n = 2;
        s_frame.w[0] = (write_t){ 0, 118, 1000, 6000 };
        s_frame.w[1] = (write_t){ 118, 236, 9000, 6000 };
        band_t b = { 0 };
        b.last = true;
        b.render_us = 900;
        frame_done(&b);
    }
    return 0;
}
"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = variant(SOURCE.read_text(), apps=True)
        parts = [typedef(source, "band_t"), typedef(source, "write_t"), typedef(source, "hold_t"),
                 re.search(r"^#define FRAME_WRITES_MAX .*$", source, re.M).group(0),
                 re.search(r"^static EXT_RAM_BSS_ATTR struct \{\n    write_t w\[FRAME_WRITES_MAX\];.*?^\} s_frame;", source, re.M | re.S).group(0),
                 re.search(r"^#define FRAME_BATCH .*$", source, re.M).group(0),
                 re.search(r"^static struct \{\n    uint32_t n, torn, bands, paired, framed;.*?^\} s_batch;", source,
                           re.M | re.S).group(0),
                 function(source, "floor_div")]
        src = Path(cls.tmp.name) / "torn.c"
        src.write_text(cls.PROGRAM.replace("PARTS", "\n".join(parts)).replace("FRAME_DONE", function(source, "frame_done")))
        cls.exe = Path(cls.tmp.name) / "torn"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)
        cls.batch = source_number("FRAME_BATCH")

    def run_refreshes(self, torn):
        return subprocess.run([str(self.exe)], input=" ".join(str(int(t)) for t in torn) + "\n", check=True, capture_output=True,
                              text=True).stdout.splitlines()

    def test_a_batch_prints_its_first_torn_refresh_alone(self):
        self.assertEqual(self.batch, 30)
        first = [i in (2, 5, 6, 17, 29) for i in range(self.batch)]   # 5 torn
        clear = [False] * self.batch
        again = [i in (0, 1) for i in range(self.batch)]
        out = self.run_refreshes(first + clear + again)
        torn = [i for i, line in enumerate(out) if "tearing effect: torn refresh of " in line]
        batches = [line for line in out if re.search(r"tearing effect: \d+ refreshes, ", line)]
        self.assertEqual(len(torn), 2, out)
        self.assertEqual(out[torn[0] - 1], "refresh 2", "the first batch's line is its first torn refresh's")
        self.assertEqual(out[torn[1] - 1], f"refresh {2 * self.batch}", "the third's, at its first refresh")
        self.assertRegex(out[torn[0]], r"^I lcd_bands: tearing effect: torn refresh of 2 bands, scan 16200/16800 us: 0-118 \+1000 ")
        self.assertEqual(len(batches), 3, out)
        for line, count in zip(batches, (5, 0, 2)):
            self.assertIn(f"tearing effect: {self.batch} refreshes, {count} torn, ", line)

class FrameModeDocTest(unittest.TestCase):
    """esp32/AGENTS.md's frame mode paragraph (D12): what it quotes of the code is the code's (the board's bands, WHOLE_SPREAD,
    WHOLE_RESERVE, the frame's size, the switch, the log lines), and the numbers measured on the board are pinned as written."""

    def test_agents_md_has_the_frame_mode_paragraph(self):
        source = variant(SOURCE.read_text(), apps=True)
        board = BOARD_175C.read_text()
        rows = int(re.search(r"^#define DRAW_BUF_LINES (\d+)", board, re.M).group(1))
        self.assertIn("round 466 px CO5300", board)
        bands = math.ceil(466 / rows)
        spread, reserve = source_number("WHOLE_SPREAD"), source_number("WHOLE_RESERVE")
        keep, takes, burst, sync = (source_number(name) for name in ("DMA_KEEP", "DMA_TAKES", "DMA_BURST", "DMA_SYNC_BYTES"))
        frame = 466 * 466 * 2
        self.assertIn('ESP_LOGW(TAG, "tearing effect: frame mode\'s copy by CPU (DMA %s: %s)", s_dma.why, esp_err_to_name(s_dma.err));',
                      source)
        self.assertIn("lv_display_set_flush_wait_cb(s_disp, flush_wait);", source)
        self.assertIn("muse_lcd_bands_make_frame();", function(BOARD_175C.read_text(), "display_start"))
        self.assertIn('ESP_LOGW(TAG, "tearing effect: no frame for frame mode (%u B of PSRAM, %u B kept free): each band goes on '
                      'its own",', source)
        self.assertIn("%u paired, %u framed (%u torn, %u bands by CPU); \"\n                     \"us avg/max: render %d/%d, copy %d/%d,",
                      source)
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIn('s_page_frames.name = "page";', ui)
        self.assertIn("lv_obj_add_event_cb(s_tv, slide_begins, LV_EVENT_SCROLL_BEGIN, NULL);", ui)
        self.assertIn(".frame_mode = muse_lcd_bands_set_frame_mode,", board)
        agents = (ROOT / "AGENTS.md").read_text()
        section = " ".join(agents[agents.index("\n## Muse apps\n"):agents.index("\n## Configuration gotchas\n")].split())
        for phrase in (
                "and so are the slide's frame mode (below) and `muse_board_t`'s `.frame_pairing` and `.frame_mode`, which only the "
                "1.75C sets.",
                f"redraws the whole screen, LVGL's {bands} bands of {rows} rows,",
                "From the tileview's `LV_EVENT_SCROLL_BEGIN` to the end of the first refresh that finds it at rest (`muse_ui.c`), the "
                "1.75C's `.frame_mode` (`muse_lcd_bands_set_frame_mode`) sends such a refresh whole: each band is copied into one "
                "frame of the screen, by DMA while LVGL draws the next band, and given back to LVGL once its copy has ended,",
                f"(`te_hold` of the frames' learned write at `WHOLE_SPREAD`, {spread}%, and never more than the longest write a hold "
                "can keep clear).",
                f"The frame is {frame:,} bytes of PSRAM, made as the display registers (`muse_lcd_bands_make_frame`, from the 1.75C's "
                f"`display_start`) if {reserve // (1024 * 1024)} MB of PSRAM stays free after it (`WHOLE_RESERVE`, the apps' floor) and "
                "kept, so the switch never allocates;",
                "the bands log `tearing effect: no frame for frame mode` once.",
                "That line and `tearing effect: frame mode's frame, <bytes> B of PSRAM` come from the send task at its next band, never "
                "from the slide's scroll callback, deep in LVGL's input read.",
                "A bench build sums up each slide's frames, its drag's and its snap's together, in `page frames:` lines (the bench's "
                "frame summaries, under Build).",
                "The copies are ESP-IDF's `esp_async_memcpy` (GDMA): its driver is installed at a slide's first copy if internal DMA "
                f"RAM keeps `DMA_KEEP` ({keep // 1024} KB, the Link's TX margin and a VM read's) and `DMA_TAKES` ({takes // 1024} KB) "
                "free, used while it keeps `DMA_KEEP`, and uninstalled once frame mode is off.",
                f"The S3's GDMA writes PSRAM only in whole bursts of `DMA_BURST` ({burst} bytes), so a band's bulk goes by DMA from its "
                "first burst of the frame, and its two ends by CPU.",
                f"The send task writes the band back from the cache first, `DMA_SYNC_BYTES` ({sync:,}) at a time: the driver's own "
                "write-back is one critical section over the band, which would hold off the TE pin's interrupt.",
                "A band the DMA can't take is copied by CPU, and the send task logs `tearing effect: frame mode's copy by CPU (DMA "
                "<step>: <error>)` once.",
                "With the apps, LVGL waits for a band's flush blocked on a semaphore (`flush_wait`, its `flush_wait_cb`), not spinning "
                "at the UI task's priority.",
                "The bands' batch line then counts the refreshes `framed`, those of them torn and their bands copied by CPU (`framed "
                "(<t> torn, <c> bands by CPU)`), and their `copy` into the frame.",
                # Measured on the board, 2026-11-02 (the task's report): pinned as written.
                "none of the 527 refreshes in frame mode was torn, and no torn refresh came during a slide (the 57 torn were the Muse "
                "face's small ones at rest), where the band path, in the same loop on the firmware before, tore 562 whole-screen "
                "refreshes, 11 on each swipe left and 12 on each swipe right.",
                "395 page frames had a median of 68 ms and a 95th percentile of 75 ms (the longest 84): the slide is no quicker than "
                "the band path's, about 64 ms between its torn refreshes, and misses the 60 ms the design asked for.",
                "A refresh in frame mode spent 14.5 ms copying its bands, 7.2 ms held for the scan and 26.4 ms on the wire,",
                f"PSRAM fell by 434,292 bytes at the first slide, the frame less {frame - 434292}, and stayed there.",
                "The send task's unused stack fell to 596 bytes, and the LVGL task's from 3,868 to 2,956.",
                # With the copies by DMA, measured on the board, 2026-11-02 (perf plan Task 5's report): pinned as written.
                "none of the 429 refreshes in frame mode was torn and none of their bands went by CPU.",
                "360 page frames had a median of 68 ms (each slide's 66 to 70), 95th percentiles of 68 to 78 ms and the longest 78: "
                "still four of the panel's scans a frame, as each write waits for a scan's start, and short of the 60 ms.",
                "The DMA gives LVGL its core back (the batches' longest drawing fell from about 68 to 63 ms), but three scans would "
                "need the next frame's write to start within about 24 ms of the last one's end,",
                "A refresh in frame mode spent 19.5 ms waiting for its copies, the CPU LVGL's meanwhile.",
                "Internal RAM is no less than before: the refresh's record (`s_frame`) is in PSRAM now, and `UI built` said 52,247 "
                "bytes free at boot against 51,967 (`>mem` 27,095 internal and 23,255 DMA), 20 less from the first slide on (the DMA "
                "interrupt's descriptor, which ESP-IDF keeps); the driver took 1,176 more while a slide ran, all back after it.",
                "The send task's unused stack was 540 bytes at its lowest."):
            with self.subTest(phrase):
                self.assertIn(phrase, section)


class PaceTest(unittest.TestCase):
    """pace_learn: the wire's pace, a smoothed line of a band's write over its size, that the holds time bands by. A
    small band's estimate has to bracket its write, as frame_clear_in reads it ((100 - SPREAD)% of it to (100 + SPREAD)%
    and 400 us more), or the hold times it wrong."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        Harness.build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def wire(kb, load=0):
        return int(180 + 75 * kb + load)   # the board's fitted line (task-7-investigation.md, section 4), and more under load

    def mixed(self):
        """Bands of many sizes, as a screen's whole refreshes and small changes give (300 of them)."""
        sizes = [107.4, 107.4, 104.7, 13.4, 20.0, 40.0]
        return [(int(sizes[i % 6] * 1024), self.wire(sizes[i % 6], 100 * math.sin(i * 1.7))) for i in range(300)]

    @staticmethod
    def query(samples, bytes_):
        return f"pace {len(samples)} " + " ".join(f"{b} {us}" for b, us in samples) + f" {bytes_}"

    def test_it_learns_the_line_from_bands_of_many_sizes(self):
        small, large = int(13.4 * 1024), int(107.4 * 1024)
        est_small, est_large = Harness.ask(["pacenew", self.query(self.mixed(), small), self.query([], large)])
        self.assertAlmostEqual(est_small, self.wire(13.4), delta=150)
        self.assertAlmostEqual(est_large, self.wire(107.4), delta=300)

    def test_bands_of_about_one_size_keep_the_slope(self):
        # The board's spinning record, capped (task-7-report.md): band after band of its 200 to 206 px square, the size
        # following the angle, and the draw's load on PSRAM (up to 900 us more) following it too. A tap's 13.4 KB button band
        # then has to be estimated within what frame_clear_in allows for: the line's slope can't follow the load's noise.
        queries = ["pacenew", self.query(self.mixed(), 0)]
        for k in range(30):
            spin = []
            for i in range(100 * k, 100 * (k + 1)):
                side = 200 + (i % 11) * 6 // 10
                kb = side * side * 2 / 1024
                spin.append((side * side * 2, self.wire(kb, 500 + 400 * math.sin(i * 2 * math.pi / 11 + 1))))
            queries.append(self.query(spin, int(13.4 * 1024)))
        small = self.wire(13.4)
        for k, est in enumerate(Harness.ask(queries)[1:]):
            with self.subTest(after=100 * (k + 1), est=est):
                self.assertLessEqual(est * (100 - SPREAD) // 100, small)
                self.assertGreaterEqual(est * (100 + SPREAD) // 100 + 400, small + 500)


# The holds of both builds, compiled from their variants of muse_lcd_bands.c over a fake clock: vTaskDelay moves it on by its
# ticks (1 ms each). One query a line: hold now fall p scan y1 y2 est; the answer is the time the hold took and whether it counted
# the band as held. The apps build's hold has no timer here (its esp_timer couldn't be made: M32), so it sleeps in ticks.
HOLD = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *esp_timer_handle_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE 1
#define ESP_OK 0
static int64_t g_now;
static int64_t esp_timer_get_time(void) { return g_now; }
static void vTaskDelay(TickType_t ticks) { g_now += (int64_t)ticks * 1000; }
static int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) { (void)s; (void)ticks; return pdTRUE; }
static int esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; abort(); }
static int esp_timer_stop(esp_timer_handle_t t) { (void)t; abort(); }
static SemaphoreHandle_t s_wake;
static esp_timer_handle_t s_wake_timer;   /* none */
static bool s_te_on = true;
static int s_rows = 466;
static volatile int64_t s_te_rise, s_te_fall, s_te_period, s_te_blank;
%s
int main(void)
{
    char cmd[8];
    long long now, fall, p, scan, y1, y2, est;
    while (scanf("%%7s %%lld %%lld %%lld %%lld %%lld %%lld %%lld", cmd, &now, &fall, &p, &scan, &y1, &y2, &est) == 8) {
        te_stats_t st = { "UI band", 0, 0, 0, 0 };
        g_now = now;
        s_te_fall = fall;
        s_te_period = p;
        s_te_blank = p - scan;
        HOLD_ONE;
        printf("%%lld %%u\n", (long long)(g_now - now), (unsigned)st.held);
    }
    return 0;
}
'''

# connected apps Task 7's names in muse_lcd_bands.c (7e59ee0..b9d8070): its types, state, functions and constants, and the IDF and
# LVGL calls it added. A build without the apps has none of them.
TASK7_NAMES = ["write_t", "hold_t", "pace_t", "PACE_VAR_MIN", "s_pace", "FRAME_HOLD_MAX", "TE_SPREAD", "TE_JITTER_US", "PAIR_WAIT_MS",
               "s_pairing", "s_parts", "s_flushed", "s_drawn", "s_drawn_area", "s_drawn_sem", "s_follows", "s_follow_y1", "s_follow_y2",
               "s_gap_us", "s_wake_timer", "s_wake", "FRAME_WRITES_MAX", "s_frame", "FRAME_BATCH", "s_batch", "s_render_t0",
               "s_render_waited", "s_wait_t0", "refresh_parts", "band_pairs", "on_render_event", "floor_div", "frame_meets",
               "frame_clear_in", "wake_send", "sleep_until", "pace_learn", "pace_est", "band_gone", "wait_drawn", "frame_done",
               "muse_lcd_bands_set_pairing", "esp_timer_create", "esp_timer_start_once", "esp_timer_stop", "lv_display_add_event_cb",
               "lv_display_flush_is_last", "LV_EVENT_RENDER_START", "LV_EVENT_FLUSH_WAIT_START", "LV_EVENT_FLUSH_WAIT_FINISH",
               "uxTaskGetStackHighWaterMark", "xTaskGetTickCount"]

# The clock and Muse's slide's (frame mode) names in muse_lcd_bands.c: its state, constants and functions, and the IDF calls it
# added. A build without the apps has none of them either.
FRAME_MODE_NAMES = ["whole_t", "s_whole", "s_framing", "s_framed", "s_te_whole", "s_cols", "WHOLE_SPREAD", "WHOLE_RESERVE",
                    "refresh_whole", "frame_band", "whole_est", "whole_band", "whole_write", "muse_lcd_bands_set_frame_mode",
                    "EXT_RAM_BSS_ATTR", "MALLOC_CAP_SPIRAM", "heap_caps_get_free_size",
                    # Task 5 of the perf plan: the frame made as frame mode registers, the blocking flush wait, the copies by DMA.
                    "muse_lcd_bands_make_frame", "FLUSH_WAIT_MS", "s_flush_ready", "flush_wait", "lv_display_set_flush_wait_cb",
                    "band_back", "DMA_BURST", "DMA_WAIT_MS", "DMA_KEEP", "DMA_TAKES", "dma_t", "s_dma", "s_copied", "on_copied",
                    "dma_fell_back", "dma_copy", "dma_end", "esp_async_memcpy", "esp_async_memcpy_install",
                    "esp_async_memcpy_uninstall", "DMA_SYNC_BYTES", "esp_cache_msync"]

# sha256 of esp32/components/muse/boards/muse_lcd_bands.c at 7e59ee0, the connected apps' base: what a build without the apps
# compiles. On a mismatch, diff variant(SOURCE, apps=False) with `git show 7e59ee0:esp32/components/muse/boards/muse_lcd_bands.c`.
PLAIN_SHA256 = "058c9a2e4f76a56f2d15096a29cbc04dda748abf2bd17d161c03c4636768a530"


class PlainBuildTest(unittest.TestCase):
    """The final review's I8 (the user's decision, "Only with apps"): Task 7's display changes are built only with the apps. Without
    CONFIG_MUSE_APPS, muse_lcd_bands.c is 7e59ee0's, byte for byte: the per-band hold that sleeps in ticks, no esp_timer, no pairing
    and no display callbacks of its own, and a 2560 B send stack. With it the code is Task 7's, its hold's timer no longer needed."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        Harness.build(cls.tmp.name)
        cls.source = SOURCE.read_text()
        cls.plain, cls.apps = variant(cls.source, False), variant(cls.source, True)
        cls.holds = {}

    def hold(self, build, queries):
        """Each query's answer from the hold of the build ("plain" or "apps"), compiled from its variant the first time."""
        if build not in self.holds:
            text = self.apps if build == "apps" else self.plain
            parts = [typedef(text, "te_stats_t")]
            if build == "apps":
                parts += [typedef(text, "write_t")]
                parts += [re.search(rf"^#define {macro} .*$", text, re.M).group(0) for macro in ("FRAME_HOLD_MAX", "TE_JITTER_US")]
                parts += [function(text, f) for f in ("te_meets", "floor_div", "frame_meets", "frame_clear_in", "te_now", "sleep_until",
                                                      "te_hold")]
                one = "const write_t w = { (int)y1, (int)y2, 0, est }; te_hold(&st, &w, 1, 0, 50)"
            else:
                parts += [function(text, f) for f in ("te_meets", "te_now", "te_hold")]
                one = "te_hold(&st, (int)y1, (int)y2, est, 50)"
            src = Path(self.tmp.name) / f"hold_{build}.c"
            src.write_text(HOLD.replace("HOLD_ONE", one) % "\n".join(parts))
            exe = Path(self.tmp.name) / f"hold_{build}"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-o", str(exe),
                            str(src)], check=True)
            self.holds[build] = exe
        out = subprocess.run([str(self.holds[build])], input="\n".join(queries) + "\n", check=True, capture_output=True,
                             text=True).stdout
        return [tuple(int(v) for v in line.split()) for line in out.splitlines()]

    def test_the_plain_build_is_7e59ee0s(self):
        self.assertEqual(hashlib.sha256(self.plain.encode()).hexdigest(), PLAIN_SHA256,
                         "muse_lcd_bands.c without the apps isn't 7e59ee0's")
        # What that means, said plainly: 7e59ee0's queue item, its hold, its stack, and nothing of Task 7's.
        code = strip_c(self.plain)
        self.assertRegex(code, r"typedef struct \{\s*const uint8_t \*data;\s*int x1, y1, x2, y2;\s*\} band_t;")
        self.assertRegex(code, r"static void te_hold\(te_stats_t \*st, int y1, int y2, int64_t w, int spread\)")
        self.assertRegex(code, r'xTaskCreatePinnedToCore\(send_bands, "", 2560,')
        self.assertRegex(code, r"vTaskDelay\(pdMS_TO_TICKS\(\(d \+ 999\) / 1000\)\);")

    def test_every_task7_name_is_inside_the_apps_guard(self):
        plain, apps = strip_c(self.plain), strip_c(self.apps)
        for name in TASK7_NAMES:
            with self.subTest(name):
                self.assertRegex(apps, rf"\b{name}\b", "the apps build lost it")
                self.assertNotRegex(plain, rf"\b{name}\b", "outside the CONFIG_MUSE_APPS guard")
        self.assertIn('xTaskCreatePinnedToCore(send_bands, "", 3072,', apps)
        self.assertNotIn("lv_display_private.h", self.plain)
        # The switch, the board's op and the 1.75C's setting of it are apps-only too (AGENTS.md: .frame_pairing is the apps').
        for path, names in ((HEADER, ["muse_lcd_bands_set_pairing"]), (BOARD_H, ["frame_pairing"]),
                            (BOARD_175C, ["frame_pairing", "muse_lcd_bands_set_pairing"])):
            text = path.read_text()
            for name in names:
                with self.subTest(path.name, name=name):
                    self.assertRegex(strip_c(variant(text, True)), rf"\b{name}\b")
                    self.assertNotRegex(strip_c(variant(text, False)), rf"\b{name}\b")
        self.assertIn('#include "sdkconfig.h"', HEADER.read_text(), "the header's guard reads the option")

    def test_the_holds_compiled_both_ways(self):
        # The board's torn single band (test_the_old_hold_started_its_check_too_early): rows 178-288, 1.93 ms expected, sent 6.27 ms
        # into the refresh. 7e59ee0's hold checks a start 300 us early, finds it clear and sends at once: the plain build keeps
        # that. Task 7's waits for a moment every start it can have is clear at.
        query = f"hold 6267 0 {P2} {SCAN2} 178 288 1927"
        self.assertEqual(self.hold("plain", [query]), [(0, 0)])
        (waited, held), = self.hold("apps", [query])
        self.assertEqual(held, 1)
        delay = Harness.ask([clear_query([(178, 288, 1927)], 6267, 0, 0, P2, SCAN2)])[0]
        self.assertGreater(delay, 0)
        self.assertGreaterEqual(waited, delay)

    def test_frame_mode_is_the_apps_alone(self):
        # The clock and Muse's slide (frame mode) is built only with the apps, as Task 7's changes are: its names, the switch, the
        # board's op and the 1.75C's setting of it.
        plain, apps = strip_c(self.plain), strip_c(self.apps)
        for name in FRAME_MODE_NAMES:
            with self.subTest(name):
                self.assertRegex(apps, rf"\b{name}\b", "the apps build lost it")
                self.assertNotRegex(plain, rf"\b{name}\b", "outside the CONFIG_MUSE_APPS guard")
        for path, names in ((HEADER, ["muse_lcd_bands_set_frame_mode", "muse_lcd_bands_make_frame"]), (BOARD_H, ["frame_mode"]),
                            (BOARD_175C, ["frame_mode", "muse_lcd_bands_set_frame_mode", "muse_lcd_bands_make_frame"])):
            text = path.read_text()
            for name in names:
                with self.subTest(path.name, name=name):
                    self.assertRegex(strip_c(variant(text, True)), rf"\b{name}\b")
                    self.assertNotRegex(strip_c(variant(text, False)), rf"\b{name}\b")

    def test_videos_write_is_as_it_was(self):
        # Frame mode has a write of its own (whole_write): video's send_frame, its job and its entry point are 739cd75's, in both
        # builds, so a video build sends its frames as before.
        for build, text, digest in (("plain", self.plain, "19f99ccaa3f212920774eebdcc9ece4902b1b4c593a79bfa551c5d2ab0578b38"),
                                    ("apps", self.apps, "8e372aea42e943223113aae4e425b65589aa7eb8e159328abd53ae0cbdd74090")):
            with self.subTest(build):
                self.assertEqual(hashlib.sha256(function(text, "send_frame").encode()).hexdigest(), digest)
                self.assertEqual(hashlib.sha256(typedef(text, "frame_job_t").encode()).hexdigest(),
                                 "a93c1b6c66a6239ea44da7a68882e1bc5e1a837af7c387db3d5496b891cab900")
                self.assertIn("void muse_lcd_bands_frame(int x1, int y1, int x2, int y2, int rows, muse_lcd_fill_fn fill, void *arg)\n"
                              "{\n    frame_job_t j = { x1, y1, x2, y2, rows, fill, arg };\n    muse_lcd_bands_run(send_frame, &j);\n}",
                              text)

    def test_the_apps_hold_needs_no_timer(self):
        # M32: without its esp_timer the apps build still registers (its absence isn't a failure) and each hold sleeps in ticks,
        # from its delay to 2 ms past it, as sleep_until's fallback rounds it up.
        register = strip_c(function_text(self.apps, "lv_display_t *muse_lcd_bands_register("))
        refusal = re.search(r"if \(([^{]*?)xTaskCreatePinnedToCore\(send_bands", register, re.S).group(1)
        self.assertIn("!s_drawn_sem", refusal)
        self.assertNotIn("s_wake_timer", refusal)
        self.assertRegex(function(self.apps, "sleep_until"), r"if \(!s_wake_timer \|\| esp_timer_start_once\(")
        nows = list(range(100000, 100000 + P2, 500))
        bands = [(178, 288, 1927), (90, 374, 6000)]
        queries = [f"hold {now} 100000 {P2} {SCAN2} {y1} {y2} {est}" for now in nows for y1, y2, est in bands]
        delays = Harness.ask([clear_query([band], now, 0, 100000, P2, SCAN2) for now in nows for band in bands])
        for query, (waited, held), delay in zip(queries, self.hold("apps", queries), delays):
            with self.subTest(query):
                if delay <= 0:
                    self.assertEqual((waited, held), (0, 0))
                else:
                    self.assertEqual(held, 1)
                    self.assertGreaterEqual(waited, delay)
                    self.assertLess(waited, delay + 2000)


def function_text(source, head):
    """The function of source that starts with the line head, through its closing brace."""
    start = source.index("\n" + head) + 1
    brace = source.index("\n{", start) + 1
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


if __name__ == "__main__":
    unittest.main()
