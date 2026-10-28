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
from test_link_ota import function_source

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


def c_function(source: str, signature: str) -> str:
    """A C function's text, from its signature to its closing brace (braces counted: none in its strings)."""
    start = source.index(signature)
    pos, depth = source.index("{", start) + 1, 1
    while depth:
        depth += {"{": 1, "}": -1}.get(source[pos], 0)
        pos += 1
    return source[start:pos]


def open_conditions(source: str, index: int) -> list:
    """The preprocessor branches open at source[index], outermost first: "if CONFIG_X", or "else" once #else is met."""
    stack = []
    for match in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b[ \t]*(.*)$", source[:index], re.M):
        kind, rest = match.group(1), match.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(f"{kind} {rest}")
        elif kind == "endif":
            stack.pop()
        else:
            stack[-1] = f"{kind} {rest}".strip()
    return stack


def call_arguments(source: str, name: str) -> list:
    """The arguments of the first call of name(...) in source, split at its top-level commas."""
    pos = source.index(name + "(") + len(name) + 1
    args, depth, current = [], 0, ""
    while depth or source[pos] != ")":
        ch = source[pos]
        depth += {"(": 1, ")": -1}.get(ch, 0)
        if ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
        pos += 1
    return [*args, current.strip()]


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

    def test_option_selects_every_font_an_app_can_name(self):
        # font = "sans14" ... "mono16" are lv_font_montserrat_14/16/20/28 and lv_font_unscii_8/16, which a build
        # only has when its sdkconfig enables them; the option does, so no board's overlay has to remember.
        apps = kconfig_block((MUSE / "Kconfig").read_text(), "MUSE_APPS")
        selected = set(re.findall(r"^\s*select (\w+)", apps, re.M))
        wanted = {f"LV_FONT_MONTSERRAT_{size}" for size in (14, 16, 20, 28)} | {f"LV_FONT_UNSCII_{size}" for size in (8, 16)}
        self.assertEqual(wanted - selected, set(), "LVGL fonts an app can name that the option doesn't select")
        # They are the ones apps_lv.c draws with, no more and no fewer.
        source = (MUSE / "apps/apps_lv.c").read_text()
        used = set(re.findall(r"&lv_font_((?:montserrat|unscii)_\d+)", source))
        self.assertEqual({font.removeprefix("LV_FONT_").lower() for font in wanted}, used)

    def test_every_apps_source_is_built_by_the_option(self):
        # A .c under apps/ that isn't in CONFIG_MUSE_APPS's block never links: its functions are missing when the
        # task that calls them lands, and the host tests (which compile it by path) can't tell.
        cmake = (MUSE / "CMakeLists.txt").read_text()
        block = cmake[cmake.index("    if(CONFIG_MUSE_APPS)\n"):]
        block = block[:block.index("    endif()")]
        listed = set(re.findall(r'"(apps/[\w/]+\.c)"', block))
        on_disk = {str(path.relative_to(MUSE)) for path in (MUSE / "apps").rglob("*.c")}
        self.assertEqual(sorted(on_disk - listed), [], "apps/ sources that CMakeLists.txt doesn't build")
        self.assertEqual(sorted(listed - on_disk), [], "CMakeLists.txt lists sources that don't exist")
        self.assertIn("apps/apps_lv.c", listed)

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

    # ---- Task 12: the app task, the device port and the UI hooks ----

    def test_ui_hooks_in_order(self):
        ui = (MUSE / "muse_ui.c").read_text()
        # build_overlays(): the app layer over the pages and their dots and under the downloaded image, so the z-order
        # is tileview, dots, app layer, image, lv_layer_top.
        build = ui.index("muse_apps_ui_build(scr)")
        self.assertLess(ui.index("s_dots[i] = d;"), build)
        self.assertLess(build, ui.index("s_image = lv_image_create(scr)"))
        frame = c_function(ui, "static void frame_tick(lv_timer_t *timer)")
        # The tick (the apply step, and whether an app covers the screen) straight after the chrome, which keeps the
        # pairing card and the icons alive over an app (C16), so after the video block and before the menu and face.
        self.assertRegex(frame, r"update_chrome\(now\);\s*#if CONFIG_MUSE_APPS\s*if \(muse_apps_ui_tick\(\)\)")
        tick = frame.index("if (muse_apps_ui_tick())")
        self.assertLess(frame.index("muse_video_tick()"), tick)
        self.assertLess(tick, frame.index("muse_menu_tick("))
        # The supervisor and the reset before update_sleep(), which returns before the tick while the screen sleeps
        # (C8 and the reset's invariants).
        supervise = frame.index("muse_apps_ui_supervise();")
        self.assertLess(supervise, frame.index("if (update_sleep())"))
        # A voice turn, from the mode-change block, which runs before the supervisor in the same frame.
        block = frame.index("if (mode != s_last_mode)")
        turn = frame.index("muse_apps_ui_turn(mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING || "
                           "mode == MUSE_MODE_SPEAKING)")
        self.assertLess(block, turn)
        self.assertLess(turn, frame.index("s_last_mode = mode;"))
        self.assertLess(turn, supervise)
        # The launcher: a long press on the pocket watch.
        press = ui.index("lv_obj_add_event_cb(s_clock, on_clock_long_press, LV_EVENT_LONG_PRESSED, NULL)")
        self.assertLess(ui.index("s_clock = lv_tileview_add_tile("), press)
        # With the option off nothing changes (X5): every mention of the apps sits inside #if CONFIG_MUSE_APPS.
        mentions = [m.start() for m in re.finditer(r"muse_apps|on_clock_long_press", ui)]
        self.assertGreaterEqual(len(mentions), 8)
        for index in mentions:
            line = ui[ui.rindex("\n", 0, index) + 1:ui.index("\n", index)]
            with self.subTest(line.strip()):
                self.assertIn("if CONFIG_MUSE_APPS", open_conditions(ui, index))

    def test_clock_showing_yields_to_apps(self):
        # An app in front or a card (the launcher, a crash card) covers the watch, whatever tile is under it: muse_input's
        # talk button then talks instead of turning to the face (F7). With the option off it is s_on_clock, as before.
        body = c_function((MUSE / "muse_ui.c").read_text(), "bool muse_ui_clock_showing(void)")
        source = ("#include <stdbool.h>\n#include <stdio.h>\n"
                  "static bool s_on_clock, covering;\n"
                  "__attribute__((unused)) static bool muse_apps_covering(void) { return covering; }\n" + body +
                  "\nint main(void) {\n"
                  "    for (int on = 0; on < 2; on++) {\n"
                  "        for (int f = 0; f < 2; f++) {\n"
                  "            s_on_clock = on;\n"
                  "            covering = f;\n"
                  "            printf(\"%d%d%d\\n\", on, f, muse_ui_clock_showing());\n"
                  "        }\n"
                  "    }\n"
                  "    return 0;\n}\n")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "clock_showing.c"
            path.write_text(source)
            for name, defines, expected in (("apps_on", ["CONFIG_MUSE_APPS=1"], ["000", "010", "101", "110"]),
                                            ("apps_off", [], ["000", "010", "101", "111"])):
                with self.subTest(name):
                    exe = apps_host.build(f"clock_showing_{name}", [path], tmp, defines=defines, sanitize=False)
                    done = apps_host.run(exe)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    self.assertEqual(done.stdout.split(), expected)

    def test_started_before_input(self):
        # After the UI (the app layer exists, built under the display lock), before input (nothing can ask for an app
        # before its task is up), and only with the option on (X5).
        app = (MUSE / "muse_app.c").read_text()
        start = app.index("muse_apps_start()")
        self.assertLess(app.index("muse_ui_start()"), start)
        self.assertLess(start, app.index("muse_input_start(q)"))
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(app, start))
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(app, app.index('#include "muse_apps.h"')))

    def test_task_shape(self):
        task = (MUSE / "apps/apps_task.c").read_text()
        defines = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", task, re.M))

        def value(arg):
            while arg in defines:
                arg = defines[arg]
            return arg

        args = call_arguments(task, "xTaskCreatePinnedToCoreWithCaps")
        self.assertEqual(len(args), 8, args)
        self.assertEqual(args[1], '"muse_apps"')    # E21: the spec's "muse_app" is muse_app.c's
        self.assertEqual(args[2], "APPS_TASK_STACK")
        self.assertEqual(value("APPS_TASK_STACK"), "57344")    # 56 KB of PSRAM (the amended constraint)
        self.assertEqual(value(args[4]), "2")    # under the draw threads, at 3
        self.assertEqual(value(args[6]), "1")
        self.assertIn("MALLOC_CAP_SPIRAM", args[7])
        queue = call_arguments(task, "xQueueCreateWithCaps")
        self.assertEqual([value(queue[0]), queue[1], queue[2]], ["16", "sizeof(apps_event_t)", "MALLOC_CAP_SPIRAM"])
        # T6: the spec's waits, as literals here: the harness moves its clock by these numbers, not by the macros.
        header = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", (MUSE / "apps/apps_task.h").read_text(), re.M))
        for name, ms in (("APPS_TASK_HUNG_MS", "3000"),  # the brief's supervisor, the global constraint
                         ("APPS_TASK_LAYOUT_MS", "500"),  # C8
                         ("APPS_TASK_RESET_MS", "500"),  # the reset's acknowledgement (the rulings)
                         ("APPS_TASK_WAIT_MS", "2000"),  # C7: compile and stop
                         ("APPS_TASK_RUN_EXTRA_MS", "7000")):  # C7: a run's settle_ms + 7000
            with self.subTest(name):
                self.assertEqual(header.get(name), ms)

    def test_task_header_compiles_on_the_host(self):
        # C29: apps_task.h stays portable C (the host harness and later tasks' host tests include it): the runtime's and
        # the package's headers and the standard ones, no FreeRTOS. A file that includes only it compiles with nothing
        # but the apps directory on the include path, so no type can come in through whatever is included first (T2).
        header = (MUSE / "apps/apps_task.h").read_text()
        includes = re.findall(r'^#include\s+[<"]([^>"]+)[>"]', header, re.M)
        self.assertEqual(sorted(name for name in includes if not name.startswith("std")), ["apps_pkg.h", "apps_rt.h"])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "only_apps_task.c"
            path.write_text('#include "apps_task.h"\n')
            args = [*apps_host.cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", f"-I{MUSE / 'apps'}",
                    str(path)]
            done = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_device_port(self):
        port = (MUSE / "apps/apps_port_idf.c").read_text()
        # C21: PSA's SHA-256, as IDF v6's mbedTLS has it, not the old mbedtls/sha256.h.
        self.assertHasText(port, "psa_crypto_init()", "apps_port_idf.c")
        self.assertHasText(port, "psa_hash_compute(PSA_ALG_SHA_256,", "apps_port_idf.c")
        self.assertHasText(port, "mbedtls/sha256.h", "apps_port_idf.c", absent=True)
        # The apps' heap is PSRAM, and a line of an app's log is one console line, within the echo's budget (F8:
        # tests/test_muse_apps_port.py runs it).
        self.assertHasText(port, "MUSE_BIG_CAPS", "apps_port_idf.c")
        self.assertHasText(port, '"%s@app.log %s %s\\n"', "apps_port_idf.c")
        self.assertHasText(port, "apps_echo_take(&s_echo, apps_port_mono_ms(), bytes)", "apps_port_idf.c")
        # Task 13: a console reply ends with fflush, then fsync. ESP-IDF's console forwards the fsync to the
        # USB-Serial-JTAG port, which sends the zero-length packet that ends a line landing on a 64-byte boundary
        # (tests/test_muse_apps_console.py checks that every reply calls it).
        self.assertHasText(port, "void apps_port_console_flush(void) {", "apps_port_idf.c")
        flush = function_source(port, "void apps_port_console_flush(void)")
        self.assertRegex(flush, r"fflush\(stdout\);\s*\(void\)fsync\(fileno\(stdout\)\);")
        # Task 13's F3: an app's print only queues, never touching the console (the app task is inside a call, its wall
        # clock running); the drain, after the call, prints each queued line and ends it as a reply ends (the pre-review
        # fix's P3). tests/test_muse_apps_port.py runs both.
        echo = function_source(port, "void apps_port_log(const char *app_id, const char *line)")
        for call in ("printf", "puts", "fputs", "fwrite", "putchar", "fflush", "fsync", "apps_port_console_flush"):
            with self.subTest(call):
                self.assertNotRegex(echo, rf"(?<![\w.]){call}\(")
        drain = function_source(port, "void apps_port_log_drain(void)")
        self.assertRegex(drain, r'printf\("%s", \w+\);\s*apps_port_console_flush\(\);')
        # The app task drains after every call, once in_call is clear, and in its housekeeping.
        task = (MUSE / "apps/apps_task.c").read_text()
        finish = function_source(task, "static void call_finish(void)")
        self.assertRegex(finish, r"atomic_store\(&s_rec->in_call, 0\);[\s\S]*apps_port_log_drain\(\);[^;]*\}\s*$")
        self.assertIn("apps_port_log_drain();", function_source(task, "static void housekeeping(void)"))
        self.assertNotIn("apps_port_log_drain", function_source(task, "static void call_begin(void)"))

    def test_one_table_of_limits_and_one_bad_id_message(self):  # Task 13's F9
        # The stage and the pipeline agree on limits because both read apps_pkg.c's apps_file_max, and say a bad id the
        # same way (APPS_MSG_BAD_ID).
        for name in ("apps_console.c", "apps_install.c"):
            text = (MUSE / "apps" / name).read_text()
            with self.subTest(name):
                self.assertNotIn("kind_max", text)
                self.assertNotRegex(text, r"#define\s+MSG_BAD_ID\b")
                self.assertNotRegex(text, r"\bAPPS_(?:LUA|JSON_FILE|IMAGE)_MAX\b")
                self.assertIn("apps_file_max(", text)
                self.assertIn("APPS_MSG_BAD_ID", text)

    def test_the_io_worker_is_on_core_0(self):  # Task 13's F4: never in front of the app task or LVGL on core 1
        io = (MUSE / "apps/apps_io.c").read_text()
        args = call_arguments(io, "xTaskCreatePinnedToCore")
        self.assertEqual(len(args), 7, args)
        self.assertEqual(args[1], '"muse_apps_io"')
        self.assertEqual(args[6], "APPS_IO_CORE")
        header = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", (MUSE / "apps/apps_io.h").read_text(), re.M))
        self.assertEqual(header.get("APPS_IO_CORE"), "0")


if __name__ == "__main__":
    unittest.main()
