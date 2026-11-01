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

"""apps_jpeg, the apps' own JPEG decoder (connected apps Task 2): a baseline JPEG in memory, through the ROM's TJpgDec, to an
LVGL image block that apps_rt_image_add takes.

tests/muse_apps_jpeg_harness.c runs the real apps_jpeg.c over a fake TJpgDec (tests/fake_tjpgd.h, staged as rom/tjpgd.h: the
ROM header's declarations, and a test format that says which picture to make and gives the output function the rectangles
TJpgDec's mcu_output gives). The harness prints the block, and the tests here check it against the picture the fake makes, worked
out again in Python. The real decode is checked on the board (Task 6). Its heap counts every byte, and the decode runs with ASan and
UBSan where the compiler has them.
"""

import re
import struct
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_rt import header_number

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
TESTS = ROOT / "tests"
SOURCES = [TESTS / "muse_apps_jpeg_harness.c", TESTS / "fake_tjpgd.c", APPS / "apps_jpeg.c", APPS / "apps_pkg.c",
           APPS / "apps_json.c", APPS / "apps_vm.c", APPS / "apps_suggest.c"]

# The errors, exactly (apps_jpeg.h).
PROGRESSIVE = "unsupported JPEG: use baseline, not progressive"
UNSUPPORTED = "unsupported JPEG: write baseline with 4:2:0 colour (ffmpeg -pix_fmt yuvj420p)"
INVALID = "not a valid JPEG"
NO_MEMORY = "the gadget is short of memory"


def too_large(n):
    return f"the picture is larger than {n} px even at 1/8"


def too_thin(n):
    return f"the picture is too thin to scale down to {n} px"


def fake_jpeg(w, h, mode=b"B", msx=1, msy=1, skip=0, tail=b"END!"):
    """A picture in the fake TJpgDec's format (tests/fake_tjpgd.h)."""
    return b"FJPG" + mode + bytes([msx, msy, 0]) + struct.pack("<HHH", w, h, skip) + b"\x5a" * skip + tail


def segment(marker, body):
    """A JPEG marker segment: FF, the marker, its length (2 more than the body), big-endian, and the body."""
    return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body


def real_jpeg(w, h, sof=0xC0, sampling=(0x22, 0x11, 0x11), before=b"", between=b"", after=b"END!"):
    """A JPEG's header as an encoder writes one (SOI, APP0 JFIF, the quantization tables, the frame, the Huffman tables, the scan),
    which the fake TJpgDec reads as the ROM's jd_prepare does for what it refuses (tests/fake_tjpgd.h); then after, the scan's data
    (to the fake, "END!" and the picture it makes). sof is the frame's marker (C0 baseline, C2 progressive), sampling each
    component's (one value: a grey picture); before goes between APP0 and the frame, between after the frame. ffmpeg 9.0.1 writes
    its 4:2:0 (-pix_fmt yuvj420p) as 22 11 11, its 4:4:4 (its default for a colour picture) as 12 12 12 and its 4:2:2 as 22 12 12."""
    components = b"".join(bytes([i + 1, s, 0 if i == 0 else 1]) for i, s in enumerate(sampling))
    frame = bytes([8]) + struct.pack(">HH", h, w) + bytes([len(sampling)]) + components
    scan = bytes([len(sampling)]) + b"".join(bytes([i + 1, 0x00 if i == 0 else 0x11]) for i in range(len(sampling))) + bytes([0, 63, 0])
    return (b"\xff\xd8" + segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00") + before +
            segment(0xDB, bytes([0]) + bytes(range(1, 65))) + segment(sof, frame) + between + segment(0xC4, bytes([0]) + bytes(16) + b"") +
            segment(0xDA, scan) + after)


def source_rgb(x, y):
    """The fake's picture at (x, y), full size."""
    return (7 * x + 3 * y) & 255, ((5 * x) ^ (11 * y)) & 255, (x + 13 * y) & 255


def rgb565_le(r, g, b):
    """LVGL's RGB565 as it lies in memory on the board: the 16-bit pixel (red in the top 5 bits), low byte first."""
    return struct.pack("<H", (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3)


def inside_circle(x, y, w, h):
    """The decision for the circle: a pixel is opaque when its centre is strictly inside the circle inscribed in the picture."""
    r = min(w, h) / 2
    return (x + 0.5 - w / 2) ** 2 + (y + 0.5 - h / 2) ** 2 < r * r


class Decoded:
    def __init__(self, text):
        lines = dict(line.split(" ", 1) for line in text.strip().split("\n"))
        self.held = int(lines["held"])
        self.ok = "ok" in lines
        if self.ok:
            w, h, length, scale, rects = (int(v) for v in lines["ok"].split())
            self.w, self.h, self.len, self.scale, self.rects = w, h, length, scale, rects
            self.check = lines["check"]
            self.block = bytes.fromhex(lines["block"])
            self.err = lines.get("err")
        else:
            self.error = lines["error"]
            self.out = lines["out"]


class JpegTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = Path(tmp.name) / "fake_headers"
        (stage / "rom").mkdir(parents=True)
        (stage / "rom/tjpgd.h").write_text('#include "fake_tjpgd.h"\n')
        cls.harness = apps_host.build("muse_apps_jpeg_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[stage, TESTS, APPS, apps_host.LUA_SRC])

    def decode(self, jpg, max_side=466, circle=False, refuse_from=0):
        done = apps_host.run(self.harness, "decode", max_side, int(circle), refuse_from, input=jpg, text=False)
        self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr[-4000:])
        self.assertEqual(done.stderr, b"")
        result = Decoded(done.stdout.decode())
        self.assertEqual(result.held, 0, "the decode left bytes in the heap")
        return result

    def assert_ok(self, result, w, h, scale):
        self.assertTrue(result.ok, getattr(result, "error", ""))
        self.assertEqual((result.w, result.h, result.scale), (w, h, scale))
        self.assertEqual(result.check, "ok", "apps_image_check refuses the block")
        self.assertIsNone(result.err, "err is set on a success")

    def assert_error(self, result, message):
        self.assertFalse(result.ok, "decoded")
        self.assertEqual(result.error, message)
        self.assertEqual(result.out, "NULL 0 0 0", "a failure leaves no block and no size")

    def assert_pixels(self, result, scale):
        w, h = result.w, result.h
        header = result.block[:12]
        cf = header[1]
        self.assertEqual(header, struct.pack("<BBHHHHH", 0x19, cf, 0, w, h, w * 2, 0))
        colour = b"".join(rgb565_le(*source_rgb(x << scale, y << scale)) for y in range(h) for x in range(w))
        self.assertEqual(result.block[12:12 + 2 * w * h], colour, "the colour plane")
        return result.block[12 + 2 * w * h:]

    # ---- The brief's cases ----

    def test_decode_picks_the_largest_scale_that_fits(self):
        # 800 px is 400 at 1/2, which fits 466: 1/2 is the largest scale that fits, and never 1/4.
        result = self.decode(fake_jpeg(800, 600))
        self.assert_ok(result, 400, 300, 1)
        self.assert_pixels(result, 1)
        for side, max_side, want, scale in ((932, 466, 466, 1), (933, 466, 466, 1), (934, 466, 233, 2),
                                            (1000, 466, 250, 2), (3728, 466, 466, 3), (800, 200, 200, 2), (800, 199, 100, 3),
                                            (466, 466, 466, 0)):
            with self.subTest(side=side, max_side=max_side):
                self.assert_ok(self.decode(fake_jpeg(side, 40), max_side=max_side), want, 40 >> scale, scale)
        # Each side counts, the taller as much as the wider.
        self.assert_ok(self.decode(fake_jpeg(300, 900)), 150, 450, 1)
        # Past 1/8, it says so, with the most it may be.
        self.assert_error(self.decode(fake_jpeg(3736, 20)), too_large(466))
        self.assert_error(self.decode(fake_jpeg(800, 1700), max_side=200), too_large(200))
        # A max_side that is no side at all is the screen's: 1 to 466 is taken as it is.
        for max_side in (0, -5, 467, 100000):
            with self.subTest(max_side=max_side):
                self.assert_ok(self.decode(fake_jpeg(800, 600), max_side=max_side), 400, 300, 1)
        self.assert_ok(self.decode(fake_jpeg(8, 8), max_side=1), 1, 1, 3)
        self.assertEqual(header_number("apps_rt.h", "APPS_RT_IMAGE_SIDE_MAX"), 466)

    def test_decode_never_scales_up(self):
        for w, h in ((100, 50), (1, 1), (466, 466), (465, 3)):
            with self.subTest(w=w, h=h):
                result = self.decode(fake_jpeg(w, h))
                self.assert_ok(result, w, h, 0)
                self.assert_pixels(result, 0)

    def test_pixels_are_rgb565_in_lvgl_byte_order(self):
        # RGB888 from the decoder, as LVGL's RGB565 lies in memory (little-endian, red on top): the format LVGL's own .bin files
        # have, not the panel's byte-swapped one (main/image_fetch.c's). Edge MCUs that the picture cuts, of every shape the ROM's
        # sampling gives (8 x 8, 16 x 8, 8 x 16, 16 x 16), and at a scale, land where they belong.
        for msx, msy in ((1, 1), (2, 1), (1, 2), (2, 2)):
            for (w, h), max_side, scale in (((37, 21), 466, 0), ((75, 43), 40, 1), ((600, 290), 200, 2)):
                with self.subTest(mcu=(msx, msy), size=(w, h)):
                    result = self.decode(fake_jpeg(w, h, msx=msx, msy=msy), max_side=max_side)
                    self.assert_ok(result, w >> scale, h >> scale, scale)
                    self.assertEqual(result.len, 12 + 2 * result.w * result.h)
                    self.assertEqual(result.block[1], 0x12, "RGB565")
                    self.assertEqual(self.assert_pixels(result, scale), b"", "no alpha plane without the circle")
        # The skip of the input function (TJpgDec skips a JPEG's segments it doesn't read) moves over exactly what it is asked to.
        self.assert_pixels(self.decode(fake_jpeg(30, 20, skip=1500)), 0)

    def test_circle_alpha_is_255_inside_and_0_outside(self):
        # RGB565A8: the colour plane as without the circle, then a plane of alpha, a byte a pixel, 255 where the pixel's centre is
        # strictly inside the inscribed circle and 0 elsewhere.
        result = self.decode(fake_jpeg(16, 16), circle=True)
        self.assert_ok(result, 16, 16, 0)
        self.assertEqual(result.block[1], 0x14, "RGB565A8")
        self.assertEqual(result.len, 12 + 16 * 16 * 3)
        alpha = self.assert_pixels(result, 0)
        self.assertEqual(len(alpha), 16 * 16)
        self.assertEqual(alpha[0], 0, "a corner is clear")
        self.assertEqual(alpha[8 * 16 + 8], 255, "the centre is opaque")
        # The edge rows: the top row's centres are 7.5 px above the middle, so those within sqrt(64 - 56.25) = 2.78 px of the middle
        # column are inside: x 5 to 10 (centres 2.5 px out at most), and x 4 (3.5 px) isn't. The middle rows reach both sides.
        self.assertEqual([x for x in range(16) if alpha[x] == 255], list(range(5, 11)))
        self.assertEqual([x for x in range(16) if alpha[7 * 16 + x] == 255], list(range(16)))
        for (w, h), max_side, scale in (((16, 16), 466, 0), ((15, 15), 466, 0), ((20, 10), 466, 0), ((7, 30), 466, 0),
                                        ((800, 600), 466, 1), ((6, 5), 466, 0)):
            with self.subTest(size=(w, h)):
                result = self.decode(fake_jpeg(w, h), max_side=max_side, circle=True)
                ow, oh = w >> scale, h >> scale
                self.assert_ok(result, ow, oh, scale)
                alpha = self.assert_pixels(result, scale)
                want = bytes(255 if inside_circle(x, y, ow, oh) else 0 for y in range(oh) for x in range(ow))
                self.assertEqual(alpha, want)
        # On the circle is outside: in 6 x 5 the circle's radius is 2.5 about (3, 2.5), and the centres of pixels (0, 2) and (5, 2)
        # are 2.5 from it, as is (1, 0)'s, at (1.5, 0.5): 1.5 across and 2 up.
        result = self.decode(fake_jpeg(6, 5), circle=True)
        alpha = self.assert_pixels(result, 0)
        self.assertEqual([alpha[2 * 6 + x] for x in range(6)], [0, 255, 255, 255, 255, 0])
        self.assertEqual([alpha[x] for x in range(6)], [0, 0, 255, 255, 0, 0])

    def test_progressive_is_refused_with_its_message(self):
        # The final review's I2 (M7): TJpgDec's JDR_FMT3 says "use baseline, not progressive" only for a JPEG whose frame is SOF2.
        for jpg in (real_jpeg(800, 600, sof=0xC2), real_jpeg(800, 600, sof=0xC2, before=segment(0xFE, b"a comment")),
                    real_jpeg(40, 30, sof=0xC2, sampling=(0x22, 0x22, 0x22))):
            for circle in (False, True):
                with self.subTest(jpg=jpg[:40], circle=circle):
                    self.assert_error(self.decode(jpg, circle=circle), PROGRESSIVE)

    def test_any_other_unsupported_jpeg_is_told_the_recipe_that_works(self):
        # Every other JDR_FMT3 gets the recipe that works: a baseline JPEG in 4:2:0. ffmpeg's default JPEG of a colour picture is
        # 4:4:4, written as a baseline (SOF0) frame the ROM refuses; and so are its 4:2:2, a grey picture, another process (SOF1
        # extended, SOF3 lossless), and an SOF2 that only the scan's data holds (the first frame marker is what counts).
        for name, jpg in (("ffmpeg's 4:4:4", real_jpeg(300, 300, sampling=(0x12, 0x12, 0x12))),
                          ("4:4:4 as 2x2", real_jpeg(300, 300, sampling=(0x22, 0x22, 0x22))),
                          ("Cr sampled 2x2", real_jpeg(300, 300, sampling=(0x11, 0x11, 0x22))),
                          ("ffmpeg's 4:2:2", real_jpeg(300, 300, sampling=(0x22, 0x12, 0x12))),
                          ("grey", real_jpeg(300, 300, sampling=(0x11,))),
                          ("SOF1", real_jpeg(300, 300, sof=0xC1)), ("SOF3", real_jpeg(300, 300, sof=0xC3)),
                          ("an SOF2 after the frame", real_jpeg(300, 300, sampling=(0x22, 0x22, 0x22),
                                                                between=segment(0xC2, bytes(15)))),
                          ("an SOF2 in the scan", real_jpeg(300, 300, sampling=(0x22, 0x22, 0x22), after=b"\xff\xc2\x00\x11END!")),
                          ("the fake's own", fake_jpeg(800, 600, mode=b"P"))):
            for circle in (False, True):
                with self.subTest(name, circle=circle):
                    self.assert_error(self.decode(jpg, circle=circle), UNSUPPORTED)
        # The recipe's JPEG decodes: a baseline 4:2:0 frame, and a 4:2:2 the ROM takes (Y 2x1, Cb and Cr 1x1).
        self.assert_ok(self.decode(real_jpeg(300, 200)), 300, 200, 0)
        self.assert_ok(self.decode(real_jpeg(64, 32, sampling=(0x21, 0x11, 0x11))), 64, 32, 0)
        # The message is the header's, beside the other five.
        self.assertIn(f'#define APPS_JPEG_MSG_UNSUPPORTED "{UNSUPPORTED}"', (APPS / "apps_jpeg.h").read_text())

    def test_a_refused_block_is_a_clean_error(self):
        # The pool (TJpgDec's 3,100 bytes), then the block: either refused by the floor is the same clean error, with nothing
        # held after it and no block given. With both allowed the decode works.
        for circle in (False, True):
            for refuse_from in (1, 2):
                with self.subTest(circle=circle, refuse_from=refuse_from):
                    self.assert_error(self.decode(fake_jpeg(800, 600), circle=circle, refuse_from=refuse_from), NO_MEMORY)
            self.assert_ok(self.decode(fake_jpeg(800, 600), circle=circle, refuse_from=3), 400, 300, 1)

    # ---- The rest ----

    def test_what_is_no_jpeg_is_not_a_valid_jpeg(self):
        for name, jpg in (("garbage", b"\xff\xd8\xff\xe0 not really"), ("one byte", b"F"), ("empty", b""),
                          ("a JPEG cut in its header", real_jpeg(40, 30)[:30]), ("no frame before the scan", b"\xff\xd8" +
                                                                                    segment(0xDA, bytes([3, 1, 0, 2, 17, 3, 17, 0, 63, 0]))),
                          ("cut in its header", fake_jpeg(40, 30)[:9]), ("cut in what it skips", fake_jpeg(40, 30, skip=100)[:60]),
                          ("cut before the data", fake_jpeg(40, 30)[:-2]), ("no data", fake_jpeg(40, 30, tail=b"NOPE")),
                          ("broken after its first MCU", fake_jpeg(40, 30, mode=b"C")), ("an MCU of 24 px", fake_jpeg(40, 30, msx=3)),
                          ("no pixels", fake_jpeg(0, 30))):
            for circle in (False, True):
                with self.subTest(name, circle=circle):
                    self.assert_error(self.decode(jpg, circle=circle), INVALID)

    def test_a_picture_too_thin_to_scale(self):
        # 1000 x 3 fits 466 only at 1/4, where its height is 0 px: there is no picture to give at a size that fits.
        self.assert_error(self.decode(fake_jpeg(1000, 3)), too_thin(466))
        self.assert_error(self.decode(fake_jpeg(2, 3000), max_side=200), too_thin(200))
        self.assert_ok(self.decode(fake_jpeg(1000, 4)), 250, 1, 2)

    def test_null_outputs(self):
        done = apps_host.run(self.harness, "nulls")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_the_pool_is_tjpgds_3100_bytes(self):
        # What main/image_fetch.c gives the same ROM decoder, and what the fake asks for at least.
        self.assertEqual(header_number("apps_jpeg.h", "APPS_JPEG_POOL_BYTES"), 3100)
        source = (APPS / "apps_jpeg.c").read_text()
        self.assertIn('#include "rom/tjpgd.h"', source)
        self.assertRegex(source, r"apps_port_realloc\(NULL, APPS_JPEG_POOL_BYTES\)")
        self.assertIsNone(re.search(r"\bmalloc\(|\bcalloc\(|heap_caps_", source), "every buffer through the apps' floor")


if __name__ == "__main__":
    unittest.main()
