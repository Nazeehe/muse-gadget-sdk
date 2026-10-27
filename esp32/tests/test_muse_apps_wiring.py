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

"""CONFIG_MUSE_APPS: the Lua component, the opt-in option, its overlay, CI, checks and licences."""

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import apps_host

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
MUSE = ROOT / "components/muse"
LUA = ROOT / "components/lua"
sys.path.insert(0, str(ROOT / "tools" / "muse"))   # tabular_digits, which gen_app_fonts.sh runs

# Lua's own build lists: the core, and the libraries apps get.
LUA_CORE = ("lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser "
            "lstate lstring ltable ltm lundump lvm lzio").split()
LUA_LIBS = "lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib".split()


def kconfig_block(kconfig: str, symbol: str) -> str:
    """One `config` entry: from its line to the next line indented less than its attributes."""
    start = kconfig.index(f"    config {symbol}\n")
    end = re.search(r"\n(?! {8}|\n)", kconfig[start:])
    return kconfig[start:start + end.start()] if end else kconfig[start:]


def lv_font_conv_points(args: list) -> set:
    """The code points an lv_font_conv command line asks for: each -r is a list like `0x20,0x25,0x30-0x3A`."""
    points = set()
    for flag, value in zip(args, args[1:]):
        if flag in ("-r", "--range"):
            for item in value.split(","):
                low, _, high = item.partition("-")
                points.update(range(int(low, 16), int(high or low, 16) + 1))
    return points


def lv_font_opts(text: str) -> list:
    """The arguments in the `Opts:` line at the top of an lv_font_conv file."""
    header = re.search(r"^ \* Opts: (.*)$", text, re.M)
    assert header, "no Opts: line, so not lv_font_conv's output"
    return header.group(1).split()


def lv_font_glyphs(text: str) -> dict:
    """The glyphs of an lv_font_conv file by code point, found the way LVGL finds them: the code point's glyph id in
    `cmaps`, then that entry of `glyph_dsc`. Each is the descriptor's fields (adv_w is in 1/16 px) plus `columns`,
    the 4 bpp coverage of the glyph's pixels summed down each column."""
    table = re.search(r"glyph_dsc\[\] = \{\n(.*?)\n\};", text, re.S).group(1)
    fields = ("index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y")
    descriptors = [dict(zip(fields, map(int, match.groups())))
                   for match in re.finditer(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
                                            r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", table)]
    body = re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", text, re.S).group(1)
    bitmap = bytes(int(byte, 16) for byte in re.findall(r"0x[0-9a-fA-F]+", re.sub(r"/\*.*?\*/", "", body)))
    lists = {name: [int(item, 16) for item in re.findall(r"0x[0-9a-fA-F]+", items)]
             for name, items in re.findall(r"(unicode_list_\d+)\[\] = \{(.*?)\};", text, re.S)}
    glyphs = {}
    for cmap in re.findall(r"\{\s*(\.range_start = .*?\.type = \w+)\s*\}", text, re.S):
        field = dict(re.findall(r"\.(\w+) = (\w+)", cmap))
        start, first = int(field["range_start"]), int(field["glyph_id_start"])
        if field["type"] == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
            ids = {start + i: first + i for i in range(int(field["range_length"]))}
        elif field["type"] == "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
            ids = {start + offset: first + i for i, offset in enumerate(lists[field["unicode_list"]])}
        else:
            raise AssertionError(f"this test doesn't read {field['type']}")
        for code, glyph_id in ids.items():
            glyph = descriptors[glyph_id]
            pixels = bitmap[glyph["index"]:glyph["index"] + (glyph["box_w"] * glyph["box_h"] + 1) // 2]
            columns = [0] * glyph["box_w"]
            for n in range(glyph["box_w"] * glyph["box_h"]):
                columns[n % glyph["box_w"]] += pixels[n // 2] >> 4 if n % 2 == 0 else pixels[n // 2] & 15
            glyphs[code] = {**glyph, "columns": columns}
    return glyphs


def synthetic_font(glyphs: dict) -> str:
    """The parts of an lv_font_conv file that tabular_digits reads, for {code point: (adv_w, ofs_x)}."""
    codes = sorted(glyphs)
    comments = "".join(f'    /* U+{code:04X} "?" */\n    0x0,\n\n' for code in codes)
    rows = ",\n".join(f"    {{.bitmap_index = 0, .adv_w = {adv}, .box_w = 1, .box_h = 1, .ofs_x = {ofs}, .ofs_y = 0}}"
                      for adv, ofs in (glyphs[code] for code in codes))
    return ("/*******\n * Size: 96 px\n * Opts: --font x.ttf --size 96 -o font.c\n *******/\n"
            f"static const uint8_t glyph_bitmap[] = {{\n{comments}}};\n\n"
            "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n"
            "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} "
            "/* id = 0 reserved */,\n"
            f"{rows}\n}};\n")


class MuseAppsWiringTest(unittest.TestCase):
    def assertHasText(self, text, needle, where, absent=False):
        """assertIn (or assertNotIn) for a file too big to print: a failure names the needle and the file."""
        self.assertEqual(needle in text, not absent, f"{needle!r} is{' ' if absent else ' not '}in {where}")

    def test_lua_component_is_gated_and_trimmed(self):
        cmake = (LUA / "CMakeLists.txt").read_text()
        self.assertIn("if(CONFIG_MUSE_APPS)", cmake)
        # Without the option the component registers empty, as tinyh264 does without video.
        self.assertIn("idf_component_register()", cmake[cmake.index("else()"):])
        self.assertIn("-O2 -w", cmake)
        for name in ("liolib", "loslib", "ldblib", "loadlib", "linit", "lua", "luac"):
            self.assertNotIn(f'"src/{name}.c"', cmake)
        sources = apps_host.lua_sources()
        self.assertEqual(sorted(path.stem for path in sources), sorted(LUA_CORE + LUA_LIBS))
        for path in sources:
            self.assertTrue(path.is_file(), path)
        # src/ is upstream's minus the programs and its Makefile.
        for name in ("lua.c", "luac.c", "Makefile"):
            self.assertFalse((LUA / "src" / name).exists(), name)

    def test_host_parser_refuses_cmake_it_cannot_follow(self):
        # tests/apps_host.py doesn't evaluate if(). A second call, or one the muse component
        # would inherit, would change the device build and not the host's.
        defs = "target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=40)\n"
        opts = "target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)\n"
        extra = "if(CONFIG_FOO)\n    target_compile_{}(${{COMPONENT_LIB}} PRIVATE FOO)\nendif()\n"

        def parse(cmake):
            # Whatever apps_host reads as components/lua/CMakeLists.txt.
            with mock.patch.object(Path, "read_text", return_value=cmake):
                return apps_host.lua_defines(), apps_host.lua_options()

        self.assertEqual(parse(defs + opts), (["LUAI_MAXCCALLS=40"], ["-O2", "-w"]))
        # Comments are ignored, and CMake commands are case-insensitive.
        self.assertEqual(parse("# " + defs + defs.replace("target_compile", "Target_Compile") + opts),
                         (["LUAI_MAXCCALLS=40"], ["-O2", "-w"]))
        for why, cmake in (
            ("no definitions call", opts),
            ("no options call", defs),
            ("a second definitions call", defs + extra.format("definitions") + opts),
            ("a second options call", defs + opts + extra.format("options")),
            ("PUBLIC definitions", defs.replace("PRIVATE", "PUBLIC") + opts),
            ("INTERFACE options", defs + opts.replace("PRIVATE", "INTERFACE")),
            ("PUBLIC after PRIVATE", defs.replace("40)", "40 PUBLIC B)") + opts),
            ("another target", defs.replace("${COMPONENT_LIB}", "other") + opts),
            ("BEFORE", defs + opts.replace("PRIVATE", "BEFORE PRIVATE")),
        ):
            with self.subTest(why), self.assertRaises(AssertionError):
                parse(cmake)

    def test_muse_requires_lua_unconditionally(self):
        # The empty component costs nothing; a conditional requirement is resolved before Kconfig.
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, re.compile(r"set\(muse_priv_requires[^)]*\blua\b", re.S))

    def test_option_is_opt_in(self):
        kconfig = (MUSE / "Kconfig").read_text()
        apps = kconfig_block(kconfig, "MUSE_APPS")
        self.assertIn("default n", apps)
        self.assertIn("depends on MUSE_ENABLED && SPIRAM && HOMEHUB_DAEMON_REQUESTS"
                      " && !MUSE_UI_FREEINK && MUSE_CLOCK", apps)
        heap = kconfig_block(kconfig, "MUSE_APPS_HEAP_MAX_KB")
        self.assertIn("range 256 1024", heap)
        self.assertIn("default 1024", heap)
        # Always defined, so idf_component.yml can key on it in every build (as MUSE_VIDEO_MDNS).
        jpeg = kconfig_block(kconfig, "MUSE_APPS_JPEG")
        self.assertNotIn("depends on", jpeg)
        self.assertIn('default "y" if MUSE_APPS', jpeg)
        self.assertIn('default "n"', jpeg)
        # Between MUSE_CLOCK_TZ and MUSE_BENCH.
        self.assertLess(kconfig.index("config MUSE_CLOCK_TZ"), kconfig.index("config MUSE_APPS\n"))
        self.assertLess(kconfig.index("config MUSE_APPS_JPEG"), kconfig.index("config MUSE_BENCH"))

        overlay = ROOT / "devices/sdkconfig.muse-apps"
        self.assertIn("CONFIG_MUSE_APPS=y", overlay.read_text())
        # Nothing else turns it on, the 1.75C's overlay included.
        for other in [*(ROOT / "devices").glob("sdkconfig.*"), *ROOT.glob("sdkconfig.defaults*")]:
            if other != overlay:
                self.assertNotIn("CONFIG_MUSE_APPS=y", other.read_text(), other.name)

    def test_board_sh_and_ci(self):
        board = (ROOT / "tools/muse/board.sh").read_text()
        flag = 'B=$B-apps; defaults="$defaults;devices/sdkconfig.muse-apps"'
        self.assertIn(flag, board)
        # After MUSE_VIDEO, so MUSE_BENCH stays the last suffix.
        self.assertLess(board.index('if [ -n "${MUSE_VIDEO:-}" ]'), board.index(flag))
        self.assertLess(board.index(flag), board.index('if [ -n "${MUSE_BENCH:-}" ]'))

        ci = (REPO / ".github/workflows/esp32.yml").read_text()
        row = re.search(r'- board: muse-waveshare-s3-175c-apps\n\s+target: esp32s3\n\s+overlays: ("[^"\n]*")', ci)
        self.assertIsNotNone(row, "no muse-waveshare-s3-175c-apps row in esp32.yml")
        self.assertEqual(row.group(1), '";devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c'
                                       ';devices/sdkconfig.muse-apps"')

    def test_license_rows(self):
        self.assertIn("esp32/components/lua", (REPO / "README.md").read_text())
        self.assertIn("components/lua", (ROOT / "README.md").read_text())
        # The digit fonts are Montserrat's, under the SIL OFL 1.1, and both lists say so.
        row = next((line for line in (REPO / "README.md").read_text().splitlines()
                    if "esp32/components/muse/apps/fonts" in line), "")
        self.assertIn("Montserrat Medium", row)
        self.assertIn("SIL Open Font License 1.1", row)
        readme = (ROOT / "README.md").read_text()
        section = readme[readme.index("\n## License"):]
        bullet = next((item for item in section.split("\n- ") if "components/muse/apps/fonts" in item), "")
        self.assertIn("Montserrat", bullet)
        self.assertIn("SIL OFL 1.1", bullet)
        # The OFL asks for the copyright line and its text with every copy: they sit beside the fonts, and the
        # lists and the folder's README point to them.
        ofl = (MUSE / "apps/fonts/OFL.txt").read_text()
        self.assertTrue(ofl.startswith("Copyright 2011 The Montserrat Project Authors"))
        self.assertIn("SIL OPEN FONT LICENSE Version 1.1", ofl)
        self.assertIn("esp32/components/muse/apps/fonts/OFL.txt", row)
        self.assertIn("components/muse/apps/fonts/OFL.txt", bullet)
        self.assertIn("OFL.txt", (MUSE / "apps/fonts/README.md").read_text())

    def test_digit_fonts_cover_exactly_the_spec_glyphs(self):
        # font = "digits48" and "digits96" show "0-9:.-+% " and nothing else. The Opts: line of a generated file
        # is the command that made it, and its bitmaps are commented with their code points: both must agree.
        spec = {ord(c) for c in "0123456789:.-+% "}
        cmake = (MUSE / "CMakeLists.txt").read_text()
        block = re.search(r"if\(CONFIG_MUSE_APPS\)(.*?)endif\(\)", cmake, re.S).group(1)
        for size in (48, 96):
            name = f"apps_digits_{size}"
            with self.subTest(name):
                text = (MUSE / "apps/fonts" / f"{name}.c").read_text()
                opts = lv_font_opts(text)
                self.assertEqual(lv_font_conv_points(opts), spec)
                drawn = {int(point, 16) for point in re.findall(r"^\s*/\* U\+([0-9A-F]+) ", text, re.M)}
                self.assertEqual(drawn, spec)
                self.assertEqual(opts[opts.index("--bpp") + 1], "4")
                self.assertIn("--no-compress", opts)
                self.assertEqual(opts[opts.index("--size") + 1], str(size))
                self.assertEqual(opts[opts.index("--lv-font-name") + 1], name)
                self.assertIn(f'"apps/fonts/{name}.c"', block)
                # The generator's sed: the project's include, and no path of the machine that ran it.
                self.assertHasText(text, '#include "lvgl.h"', f"{name}.c")
                self.assertHasText(text, "lvgl/lvgl.h", f"{name}.c", absent=True)
                self.assertEqual([arg for arg in opts if arg.startswith("/")], [])
                self.assertNotIn("/", opts[opts.index("-o") + 1])
        # What regenerates them asks for the same glyphs, and for what the Opts: lines and the files show.
        words = [word for word in (ROOT / "tools/muse/gen_app_fonts.sh").read_text().split() if word != "\\"]
        self.assertEqual(lv_font_conv_points(words), spec)
        for option in ("--no-kerning", "--lv-fallback lv_font_montserrat_28", "tools/muse/tabular_digits.py"):
            self.assertIn(option, " ".join(words))

    def test_digit_fonts_are_tabular_and_unkerned(self):
        # The timer and the clock centre their digits, so a figure that changes must not move the rest: 0-9 share
        # one advance, each digit's ink is centred in it, and no kerning pair moves a digit by its neighbour.
        digits = range(0x30, 0x3A)
        for size in (48, 96):
            with self.subTest(size):
                text = (MUSE / f"apps/fonts/apps_digits_{size}.c").read_text()
                glyphs = lv_font_glyphs(text)
                advances = {chr(code): glyphs[code]["adv_w"] for code in digits}
                self.assertEqual(len(set(advances.values())), 1, advances)
                cell = glyphs[0x30]["adv_w"] / 16
                for code in digits:
                    glyph = glyphs[code]
                    # Where the ink sits in the cell, by weight. Montserrat's own 1 sits at the cell's left, and
                    # moving by the whole difference instead of half would put it as far to the right.
                    weight = sum(glyph["columns"])
                    ink = glyph["ofs_x"] + sum((x + 0.5) * c for x, c in enumerate(glyph["columns"])) / weight
                    self.assertAlmostEqual(ink, cell / 2, delta=size / 16, msg=chr(code))
                    self.assertGreaterEqual(glyph["ofs_x"], 0, chr(code))
                    self.assertLessEqual(glyph["ofs_x"] + glyph["box_w"], cell + 1, chr(code))
                self.assertIn("--no-kerning", lv_font_opts(text))
                self.assertHasText(text, "    .kern_dsc = NULL,\n", f"apps_digits_{size}.c")
                for table in ("kern_class_values", "kern_pair"):
                    self.assertHasText(text, table, f"apps_digits_{size}.c", absent=True)
                self.assertHasText(text, "tools/muse/tabular_digits.py", f"apps_digits_{size}.c")

    def test_tabular_digits_script(self):
        from tabular_digits import MARK, tabular_digits

        # The 96 px font's own numbers, as lv_font_conv wrote them: code point -> (adv_w in 1/16 px, ofs_x).
        before = {0x20: (413, 0), 0x25: (1295, 3), 0x30: (1025, 4), 0x31: (568, 0), 0x32: (882, 1),
                  0x33: (879, 0), 0x34: (1028, 3), 0x35: (882, 1), 0x36: (948, 4), 0x37: (919, 2),
                  0x38: (989, 4), 0x39: (948, 2), 0x3A: (349, 4)}
        # Every digit takes the widest one's advance (the 4's) and moves by half of what it gained, rounded half up:
        # a 1 gains 28.75 px and moves 14, and a 6 and a 9 gain 5 px and move 3.
        moved = {0x30: 4, 0x31: 14, 0x32: 6, 0x33: 5, 0x34: 3, 0x35: 6, 0x36: 7, 0x37: 5, 0x38: 5, 0x39: 5}
        after = {**before, **{code: (1028, ofs_x) for code, ofs_x in moved.items()}}
        done = tabular_digits(synthetic_font(before))
        self.assertIn(MARK, done)
        self.assertEqual(done.replace(MARK, ""), synthetic_font(after))
        self.assertEqual(tabular_digits(done), done)    # nothing left to do the second time
        # A file it doesn't understand is refused, and the command line says so and leaves the file alone.
        text = synthetic_font(before)
        no_five = synthetic_font({code: value for code, value in before.items() if code != 0x35})
        space = "    {.bitmap_index = 0, .adv_w = 413, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},\n"
        short = text.replace(space, "")
        self.assertNotEqual(short, text)
        far = synthetic_font({**before, 0x34: (4095, 3), 0x35: (100, 4)})    # a 5 that would move 129 px
        for why, bad, message in (("no digit 5", no_five, "U+0035"),
                                  ("a glyph with no descriptor", short, "descriptors"),
                                  ("an offset that can't be stored", far, "129 px"),
                                  ("not a font", "int x;\n", "glyph_dsc")):
            with self.subTest(why), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(ValueError, re.escape(message)):
                    tabular_digits(bad)
                path = Path(tmp) / "font.c"
                path.write_text(bad)
                run = subprocess.run([sys.executable, "-I", str(ROOT / "tools/muse/tabular_digits.py"), str(path)],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 1, run.stderr)
                self.assertIn(message, run.stderr)
                self.assertEqual(path.read_text(), bad)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "font.c"
            path.write_text(text)
            run = subprocess.run([sys.executable, "-I", str(ROOT / "tools/muse/tabular_digits.py"), str(path)],
                                 capture_output=True, text=True)
            self.assertEqual((run.returncode, run.stderr), (0, ""))
            self.assertEqual(path.read_text(), done)

    def test_digit_fonts_fall_back_to_montserrat_28(self):
        # A letter in a digits label draws in the 28 px text font, not as placeholder boxes. The fallback is the
        # font the Muse overlay (which every apps build is built on) already has.
        self.assertIn("CONFIG_LV_FONT_MONTSERRAT_28=y", (ROOT / "devices/sdkconfig.muse").read_text().splitlines())
        for size in (48, 96):
            with self.subTest(size):
                text = (MUSE / f"apps/fonts/apps_digits_{size}.c").read_text()
                self.assertHasText(text, "    .fallback = &lv_font_montserrat_28,\n", f"apps_digits_{size}.c")
                self.assertTrue("LV_FONT_DECLARE(lv_font_montserrat_28)" in text
                                or "extern const lv_font_t lv_font_montserrat_28;" in text,
                                f"apps_digits_{size}.c doesn't declare lv_font_montserrat_28")
                self.assertIn("--lv-fallback lv_font_montserrat_28", " ".join(lv_font_opts(text)))

    def test_validate_config(self):
        validate = (ROOT / "cmake/validate_config.cmake").read_text()
        self.assertIn("CONFIG_MUSE_APPS AND NOT CONFIG_SPIRAM", validate)
        self.assertIn("CONFIG_MUSE_APPS_HEAP_MAX_KB", validate)

    def test_validate_config_guards_behave(self):
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available")
        validate = (ROOT / "cmake/validate_config.cmake").read_text()
        # Both guards: from the first to the end of the second `endif()`.
        start = validate.index("if(CONFIG_MUSE_APPS AND NOT CONFIG_SPIRAM)")
        first_end = validate.index("endif()", start) + len("endif()")
        guards = validate[start:validate.index("endif()", first_end) + len("endif()")]
        self.assertEqual(guards.count("GADGET_CONFIG_REGEN_HINT"), 2)
        # (CONFIG_MUSE_APPS, CONFIG_SPIRAM, CONFIG_MUSE_APPS_HEAP_MAX_KB, accepted). Any other
        # build has the option off and no heap setting.
        cases = (
            ("OFF", "OFF", None, True), ("OFF", "ON", None, True),
            ("ON", "ON", 1024, True), ("ON", "ON", 256, True),
            ("ON", "ON", 255, False), ("ON", "ON", 1025, False),
            ("ON", "OFF", 1024, False),
        )
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "guard.cmake"
            for apps, spiram, heap, accepted in cases:
                with self.subTest(apps=apps, spiram=spiram, heap=heap):
                    script.write_text(
                        f"set(CONFIG_MUSE_APPS {apps})\nset(CONFIG_SPIRAM {spiram})\n"
                        + (f"set(CONFIG_MUSE_APPS_HEAP_MAX_KB {heap})\n" if heap is not None else "")
                        + "set(GADGET_CONFIG_REGEN_HINT x)\n" + guards)
                    done = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
                    if accepted:
                        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                    else:
                        self.assertNotEqual(done.returncode, 0)
                        self.assertIn("CONFIG_MUSE_APPS", done.stderr)


if __name__ == "__main__":
    unittest.main()
