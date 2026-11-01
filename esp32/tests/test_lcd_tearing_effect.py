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
bands, sent back to back, stay clear of every scan) and refresh_parts (how many bands LVGL draws a refresh in)."""
import math
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/boards/muse_lcd_bands.c"


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

    @classmethod
    def build(cls, tmp):
        source = SOURCE.read_text()
        parts = [typedef(source, "write_t"), typedef(source, "pace_t"), re.search(r"^#define PACE_VAR_MIN .*$", source, re.M).group(0)]
        parts += [function(source, name) for name in
                  ("te_meets", "floor_div", "frame_meets", "frame_clear_in", "refresh_parts", "pace_learn", "pace_est",
                   "band_pairs")]
        src = Path(tmp) / "te.c"
        src.write_text(HARNESS % "\n".join(parts))
        cls.exe = Path(tmp) / "te"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def ask(cls, queries):
        out = subprocess.run([str(cls.exe)], input="\n".join(queries) + "\n", check=True, capture_output=True,
                             text=True).stdout
        return [int(v) for v in out.split()]


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
        done = function(SOURCE.read_text(), "frame_done")
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
        source = SOURCE.read_text()
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


if __name__ == "__main__":
    unittest.main()
