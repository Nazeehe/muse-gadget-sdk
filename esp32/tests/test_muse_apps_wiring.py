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


def strip_c_comments(source: str) -> str:
    """The code of a C file: its comments gone, and its string literals as they are."""
    return re.sub(r'("(?:\\.|[^"\\\n])*")|/\*.*?\*/|//[^\n]*', lambda m: m.group(1) or "", source, flags=re.S)


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
        # The FAT's long names (ids like stopwatch, paths like timer_face.lua), their buffers on the heap and in PSRAM (the
        # 2026-10-27 ruling's place for these two).
        lines = overlay.read_text().splitlines()
        self.assertIn("CONFIG_FATFS_LFN_HEAP=y", lines)
        self.assertIn("CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y", lines)
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

    def test_board_sh_refuses_the_apps_for_any_board_but_the_175c(self):  # G
        # The overlay's table and FAT lines apply on any board it is loaded on, apps or not: board.sh refuses the flag for every
        # other board, with one line, before it activates ESP-IDF or touches a build dir. The environment has no ESP-IDF, so a
        # board.sh that went past the guard would stop at "idf.py not found" (or mosaico's utils) and never build.
        board = ROOT / "tools/muse/board.sh"
        with tempfile.TemporaryDirectory() as home:
            env = {"HOME": home, "PATH": "/usr/bin:/bin", "MUSE_APPS": "1"}
            for name in ("s3n", "sticks3", "plus2", "mosaico"):
                with self.subTest(name):
                    done = subprocess.run(["bash", str(board), "build", name], env=env, capture_output=True, text=True,
                                          timeout=60)
                    self.assertNotEqual(done.returncode, 0)
                    self.assertEqual((done.stdout + done.stderr).strip(), "MUSE_APPS=1 is only for the 1.75C (s3)")
            # The 1.75C passes the guard (and stops at the missing ESP-IDF here).
            done = subprocess.run(["bash", str(board), "build", "s3"], env=env, capture_output=True, text=True, timeout=60)
            self.assertNotIn("is only for the 1.75C", done.stdout + done.stderr)
            self.assertIn("idf.py not found", done.stdout + done.stderr)

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

    def test_validate_config_warns_of_an_apps_build_without_the_apps_table(self):  # Y
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available")
        validate = (ROOT / "cmake/validate_config.cmake").read_text()
        start = validate.index("if(CONFIG_MUSE_APPS)\n")
        block = validate[start:validate.index("\nendif()", start) + len("\nendif()")]
        # (CONFIG_MUSE_APPS, the table's file, warned). The table is read where ESP-IDF reads it, from the project's folder.
        cases = (("ON", "partitions_muse.csv", True), ("ON", "partitions_muse_apps.csv", False),
                 ("OFF", "partitions_muse.csv", False), ("ON", "partitions_muse_8mb.csv", True))
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "warn.cmake"
            for apps, table, warned in cases:
                with self.subTest(apps=apps, table=table):
                    script.write_text(
                        f"set(CONFIG_MUSE_APPS {apps})\nset(CONFIG_PARTITION_TABLE_CUSTOM ON)\n"
                        f'set(CONFIG_PARTITION_TABLE_FILENAME "{table}")\n'
                        "function(idf_build_get_property var name)\n"
                        '    set(${var} "/work/build-muse-waveshare-s3-175c-apps/sdkconfig" PARENT_SCOPE)\n'
                        "endfunction()\n" + block)
                    done = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, cwd=ROOT)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    said = " ".join(done.stderr.split())  # CMake wraps a message's lines
                    if not warned:
                        self.assertEqual(said, "")
                        continue
                    self.assertIn("CMake Warning", said)
                    self.assertIn(f"CONFIG_MUSE_APPS with the partition table {table}, which has no apps partition", said)
                    self.assertIn("/work/build-muse-waveshare-s3-175c-apps/sdkconfig", said)
                    self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_muse_apps.csv" and '
                                  'CONFIG_PARTITION_TABLE_FILENAME="partitions_muse_apps.csv"', said)

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

    def test_a_build_without_the_apps_has_no_alarm_wake_in_the_input_task(self):  # X5, O
        # muse_input_wake and what it leaves for the input task (s_wake_why, apply_wake and its call) are only for the alarms of
        # Muse apps: with the option off the plain build has none of them, so every mention sits inside #if CONFIG_MUSE_APPS.
        source = (MUSE / "muse_input.c").read_text()
        mentions = [m.start() for m in re.finditer(r"s_wake_why|muse_input_wake|apply_wake", source)]
        self.assertGreaterEqual(len(mentions), 6)
        for index in mentions:
            line = source[source.rindex("\n", 0, index) + 1:source.index("\n", index)]
            with self.subTest(line.strip()):
                self.assertIn("if CONFIG_MUSE_APPS", open_conditions(source, index))

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
        # The queue's length is in the header (the harnesses size their floods by it), the rest are the task's own.
        defines = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", (MUSE / "apps/apps_task.h").read_text(), re.M))
        defines.update(dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", task, re.M)))

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
        # N1: it holds stdout's lock once around the whole queue, and not at all with nothing queued.
        self.assertRegex(drain, r"if \(s_echo_used == 0\) \{\s*return;[^}]*\}\s*flockfile\(stdout\);[^\n]*\n\s*for \("
                                r"[\s\S]*\}\s*funlockfile\(stdout\);")
        # The app task drains after every call, once in_call is clear, and in its housekeeping.
        task = (MUSE / "apps/apps_task.c").read_text()
        finish = function_source(task, "static void call_finish(void)")
        # What the alarms recorded to log goes out with it (C: the esp_timer task never prints), from one helper, outside any call.
        self.assertRegex(finish, r"atomic_store\(&s_rec->in_call, 0\);[\s\S]*drain_logs\(\);[^;]*\}\s*$")
        self.assertIn("drain_logs();", function_source(task, "static void housekeeping(void)"))
        drain_logs = function_source(task, "static void drain_logs(void)")
        self.assertRegex(drain_logs, r"apps_alarm_log_drain\(\);\s*apps_port_log_drain\(\);")
        for name in ("call_begin", "tell_alarm", "on_alarm_fired"):
            self.assertNotRegex(function_source(task, f"static void {name}("), r"log_drain|drain_logs")

    def test_the_apps_option_says_the_app_tasks_stack_as_it_is(self):  # S: it was 32 KB once, and is measured at 56
        task = (MUSE / "apps/apps_task.c").read_text()
        stack = int(re.search(r"^#define APPS_TASK_STACK\s+(\d+)", task, re.M).group(1))
        block = " ".join(kconfig_block((MUSE / "Kconfig").read_text(), "MUSE_APPS").split())
        self.assertEqual(stack % 1024, 0)
        self.assertIn(f"and a {stack // 1024} KB task stack", block)
        self.assertNotIn("32 KB", block)

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

    def test_one_compile_message(self):  # the pre-review round: check words a refusal as the install does
        # apps_pkg.c makes the message, "compile failed: ", Lua's own and the excerpt of the file it names; the install
        # pipeline and apphost both call it, so neither keeps a lookup or a wording of its own. (Fix round 1, E and F: the
        # step that compiles, its order, the heap's message and the compiled-chunk refusal are shared the same way.)
        pkg = (MUSE / "apps/apps_pkg.c").read_text()
        self.assertEqual(pkg.count('"compile failed: "'), 1)
        self.assertEqual(pkg.count("apps_vm_excerpt("), 1)
        for path in (MUSE / "apps/apps_install.c", ROOT / "tools/muse/apphost/apphost.c"):
            text = path.read_text()
            with self.subTest(path.name):
                self.assertEqual(text.count("apps_pkg_compile_message("), 1)
                self.assertNotIn('"compile failed: "', text)
                self.assertNotIn("apps_vm_excerpt(", text)
        self.one_compile_step_and_one_compiled_chunk_check()

    def one_compile_step_and_one_compiled_chunk_check(self):  # fix round 1, E and F
        # What the install compiles, apphost's check compiles with the same code: apps_rt.c makes the scratch VM and
        # loops over the files (and says what a heap too small to make says, as it does for the app's own), apps_pkg.c
        # puts main first and refuses a compiled chunk. The app task's compile and apphost call these, and neither has a
        # VM, a loop, an order or a wording of its own. Every C file of the build is searched, so a copy anywhere else
        # fails here too.
        sources = {path.name: strip_c_comments(path.read_text()) for folder in (MUSE / "apps", ROOT / "tools/muse/apphost")
                   for path in [*folder.glob("*.c"), *folder.glob("*.h")]}
        for name in ("apps_task.c", "apphost.c", "apps_rt.c", "apps_pkg.c", "apps_install.c"):
            self.assertIn(name, sources)

        def holders(text):
            return sorted(name for name, source in sources.items() if text in source)

        # One wording of a heap that can't be made, and of a compiled chunk.
        self.assertEqual(holders("can't make the app's heap"), ["apps_rt.c"])
        self.assertEqual(sources["apps_rt.c"].count("can't make the app's heap"), 1)
        self.assertEqual(holders("is a compiled chunk"), ["apps_pkg.c"])
        self.assertEqual(sources["apps_pkg.c"].count("is a compiled chunk"), 1)
        self.assertEqual(holders("binary chunks are not allowed"), ["apps_vm.c"])
        # The compile's steps are in the runtime: the VM is made by one function (the app's and the compile's), and each
        # file loaded into it and the VM freed by one function more.
        rt = sources["apps_rt.c"]
        self.assertEqual(rt.count("apps_vm_new("), 1)
        self.assertEqual(c_function(rt, "static apps_vm_t *make_vm(").count("apps_vm_new("), 1)
        compile_all = c_function(rt, "bool apps_rt_compile(")
        self.assertEqual(compile_all.count("make_vm("), 1)
        self.assertEqual(compile_all.count("apps_vm_load("), 1)
        self.assertEqual(compile_all.count("apps_vm_free("), 1)
        for name in ("apps_task.c", "apphost.c", "apps_install.c"):
            for call in ("apps_vm_new(", "apps_vm_load(", "apps_vm_load_on(", "apps_vm_free(", "apps_vm_config_t"):
                with self.subTest(name, call=call):
                    self.assertNotIn(call, sources[name])
        # The callers: each calls the shared pieces once and makes no loop or order of its own.
        for name, calls in (("apps_task.c", ("apps_rt_compile(", "apps_pkg_compile_order(")),
                            ("apphost.c", ("apps_rt_compile(", "apps_pkg_compile_order(", "apps_pkg_check_source("))):
            for call in calls:
                with self.subTest(name, call=call):
                    self.assertEqual(sources[name].count(call), 1)
        self.assertEqual(sources["apps_install.c"].count("apps_pkg_check_source("), 1)
        # The order, main first, is apps_pkg.c's alone: nothing else compares a path with the manifest's main to sort
        # the compile (the app task's message for a main it can't compile is its own business, after the order).
        order = c_function(sources["apps_pkg.c"], "int apps_pkg_compile_order(")
        self.assertIn("manifest.main", order)
        self.assertNotIn("manifest.main", c_function(sources["apphost.c"], "static bool compile_package("))
        self.assertEqual(c_function(sources["apps_task.c"], "bool apps_task_compile(").count("apps_pkg_compile_order("), 1)

    def test_the_io_worker_is_on_core_0(self):  # Task 13's F4: never in front of the app task or LVGL on core 1
        io = strip_c_comments((MUSE / "apps/apps_io.c").read_text())
        args = call_arguments(io, "xTaskCreatePinnedToCoreWithCaps")
        self.assertEqual(len(args), 8, args)
        self.assertEqual(args[1], '"muse_apps_io"')
        self.assertEqual(args[2], "APPS_IO_STACK")
        self.assertEqual(args[6], "APPS_IO_CORE")
        self.assertEqual(args[7], "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT")  # Task 20's R1: its stack in PSRAM (X18)
        header = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", (MUSE / "apps/apps_io.h").read_text(), re.M))
        self.assertEqual(header.get("APPS_IO_CORE"), "0")
        self.assertEqual(header.get("APPS_IO_STACK"), "8192")

    def test_the_io_worker_takes_no_internal_ram_to_be_made_or_to_go(self):  # Task 20's R1
        # No internal-RAM admission is left, and the worker never deletes itself: IDF v6.0.1's vTaskDeleteWithCaps(NULL) makes a
        # cleanup task in internal RAM and aborts when it can't. It parks, and the next submit deletes it, from that task.
        io = strip_c_comments((MUSE / "apps/apps_io.c").read_text())
        for gone in ("heap_caps_get_free_size", "heap_caps_get_largest_free_block", "APPS_IO_HEADROOM", "APPS_IO_BLOCK_MARGIN",
                     "vTaskDelete(", "vTaskDeleteWithCaps(NULL)", "xTaskCreatePinnedToCore("):
            with self.subTest(gone):
                self.assertNotIn(gone, io)
        self.assertIn("vTaskSuspend(NULL);", c_function(io, "static void worker(void *arg)"))
        reap = c_function(io, "static void reap_locked(void)")
        self.assertRegex(reap, r"eTaskGetState\(s_parked\) == eSuspended\)\s*\{\s*vTaskDeleteWithCaps\(s_parked\);")
        header = (MUSE / "apps/apps_io.h").read_text()
        for gone in ("APPS_IO_HEADROOM", "APPS_IO_BLOCK_MARGIN"):
            with self.subTest(gone):
                self.assertNotIn(gone, header)

    def test_a_device_build_of_the_io_worker_needs_code_and_rodata_in_psram(self):  # Task 20's R1, X18: the #error
        # The worker's stack is in PSRAM, and it writes flash: only safe while IDF keeps the cache on through a flash write, which
        # CONFIG_SPIRAM_FETCH_INSTRUCTIONS and CONFIG_SPIRAM_RODATA together make it do (SPI_FLASH_CACHE_NO_DISABLE). A device build
        # (ESP_PLATFORM) of apps_io.c stops at its #error without both; the host's builds don't look.
        headers = ["freertos/FreeRTOS.h", "freertos/idf_additions.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h",
                   "esp_attr.h", "esp_err.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "lvgl.h"]
        options = {"both": ["CONFIG_SPIRAM_FETCH_INSTRUCTIONS", "CONFIG_SPIRAM_RODATA"],
                   "fetch only": ["CONFIG_SPIRAM_FETCH_INSTRUCTIONS"], "rodata only": ["CONFIG_SPIRAM_RODATA"], "neither": []}
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            for name in headers:
                (stage / name).parent.mkdir(parents=True, exist_ok=True)
                (stage / name).write_text('#include "muse_apps_task_fakes.h"\n')
            base = [*apps_host.cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", f"-I{stage}", f"-I{ROOT / 'tests'}",
                    f"-I{MUSE / 'apps'}", str(MUSE / "apps/apps_io.c")]
            for case, defined in options.items():
                with self.subTest(case):
                    (stage / "sdkconfig.h").write_text("".join(f"#define {name} 1\n" for name in defined))
                    done = subprocess.run([*base, "-DESP_PLATFORM"], capture_output=True, text=True)
                    if case == "both":
                        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                    else:
                        self.assertNotEqual(done.returncode, 0, "built without both options")
                        self.assertIn("set CONFIG_SPIRAM_FETCH_INSTRUCTIONS and CONFIG_SPIRAM_RODATA (X18)", done.stderr)
            (stage / "sdkconfig.h").write_text("")
            done = subprocess.run(base, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, "the host's build: " + done.stdout + done.stderr)

    def test_the_175c_runs_code_and_rodata_from_psram(self):  # Task 20's R1, X18: what apps_io.c's #error asks for
        overlay = (ROOT / "devices/sdkconfig.muse-waveshare-s3-175c").read_text()
        for option in ("CONFIG_SPIRAM_FETCH_INSTRUCTIONS", "CONFIG_SPIRAM_RODATA"):
            with self.subTest(option):
                self.assertRegex(overlay, rf"(?m)^{option}=y$")
                # Nothing else the apps build loads (board.sh: defaults, muse, the board's, muse-apps, muse-bench) turns it off.
                for other in ("sdkconfig.defaults", "devices/sdkconfig.muse", "devices/sdkconfig.muse-apps", "devices/sdkconfig.muse-bench"):
                    self.assertNotRegex((ROOT / other).read_text(), rf"(?m)^(# {option} is not set|{option}=n)$", other)

    def test_sntp_left_the_clock(self):  # Task 16: the watch and the apps' time module share one SNTP helper
        clock = strip_c_comments((MUSE / "muse_clock.c").read_text())
        sntp_source = (MUSE / "muse_sntp.c").read_text()
        sntp = strip_c_comments(sntp_source)
        # SNTP starts in muse_sntp.c and nowhere else, and the year test is its muse_time_valid().
        self.assertEqual(sntp.count("esp_netif_sntp_init("), 1)
        for gone in ("esp_netif_sntp", "esp_sntp", "VALID_YEAR", "s_sntp", "muse_wifi"):
            self.assertNotIn(gone, clock, f"{gone} is still in muse_clock.c")
        self.assertIn("muse_time_valid()", clock)
        self.assertEqual(clock.count("muse_time_valid()"), 1)
        # The watch keeps its time zone (muse_clock_build), and the boot log keeps its line: `SNTP started`.
        build = c_function(clock, "void muse_clock_build(lv_obj_t *tile)")
        self.assertRegex(build, r'setenv\("TZ", CONFIG_MUSE_CLOCK_TZ, 1\);\s*tzset\(\);')
        self.assertIn('"SNTP %s, time zone %s", err == ESP_OK ? "started"', sntp)
        self.assertNotIn("setenv", sntp)
        # What the two functions are (the header), and where each may be called from.
        header = (MUSE / "muse_sntp.h").read_text()
        self.assertIn("void muse_sntp_poll(void);", header)
        self.assertIn("bool muse_time_valid(void);", header)
        self.assertIn("#include <stdbool.h>", header)
        # The CMake line sits with the watch's: muse_sntp.c exists exactly when CONFIG_MUSE_CLOCK does.
        cmake = (MUSE / "CMakeLists.txt").read_text()
        block = cmake[cmake.index("    if(CONFIG_MUSE_CLOCK)\n"):]
        block = block[:block.index("    endif()")]
        self.assertEqual(re.findall(r'"(muse_\w+\.c)"', block), ["muse_clock.c", "muse_sntp.c"])
        # muse_ui.c: polled in the LVGL task's frame, straight after update_sleep()'s early return (so it goes on while an
        # app covers the screen, which returns before the watch's tick), and guarded as the watch itself is (C19).
        ui = (MUSE / "muse_ui.c").read_text()
        frame = c_function(ui, "static void frame_tick(lv_timer_t *timer)")
        self.assertRegex(frame, r"if \(update_sleep\(\)\) \{\s*return;\s*\}\s*#if CONFIG_MUSE_CLOCK\s*muse_sntp_poll\(\);[^\n]*\s*#endif")
        for needle in ('#include "muse_sntp.h"', "muse_sntp_poll();"):
            with self.subTest(needle):
                self.assertEqual(ui.count(needle), 1)
                self.assertIn("if CONFIG_MUSE_CLOCK", open_conditions(ui, ui.index(needle)))
        # The apps' port asks the same two helpers: the wall clock counts when muse_time_valid() says so, and the screen is
        # dark when the UI says so (nothing in it keeps a year or a flag of its own).
        port = strip_c_comments((MUSE / "apps/apps_port_idf.c").read_text())
        self.assertRegex(c_function(port, "bool apps_port_epoch_ms(int64_t *out)"), r"muse_time_valid\(\)")
        self.assertRegex(c_function(port, "bool apps_port_dark(void)"), r"return muse_ui_dark\(\);")
        self.assertNotIn("EPOCH_2025", port)

    def test_the_store_health_and_heartbeat_are_wired(self):  # Task 20
        # The I/O worker mounts the store before its first job or op: set as the worker is made ready (muse_apps_start), before
        # anything can submit.
        task = strip_c_comments((MUSE / "apps/apps_task.c").read_text())
        self.assertRegex(c_function(task, "esp_err_t muse_apps_start(void)"),
                         r"apps_io_init\(\);\s*apps_io_set_first\(apps_store_mount_once\);")
        # The port says which name an app touched, after the arming or the cancel: its record follows, and a record the mount
        # handed over for that name is armed again no more (apps_values.h, P1).
        port = strip_c_comments((MUSE / "apps/apps_port_idf.c").read_text())
        self.assertRegex(c_function(port, "apps_alarm_result_t apps_port_alarm_arm("),
                         r"apps_alarm_arm\([^;]*;\s*apps_values_alarm_touched\(app, name\);")
        self.assertRegex(c_function(port, "void apps_port_alarm_cancel("),
                         r"apps_alarm_cancel\(app, name\);\s*apps_values_alarm_touched\(app, name\);")
        # The lock of the write budget and of the failures' notices is made before anything that takes it runs.
        self.assertRegex(c_function(task, "esp_err_t muse_apps_start(void)"),
                         r"apps_values_init\(\);[\s\S]*xTaskCreatePinnedToCoreWithCaps\(apps_task,")
        # device.health's "apps" and the heartbeat's " apps=<resident>/<state> lua=<KB>K": only in a build with the apps.
        health = (ROOT / "main/noise_control.cpp").read_text()
        for needle in ('#include "muse_apps.h"', "muse_apps_health()", 'cJSON_AddItemToObject(health, "apps", apps);'):
            with self.subTest(needle):
                self.assertEqual(health.count(needle), 1)
                self.assertIn("if CONFIG_MUSE_APPS", open_conditions(health, health.index(needle)))
        self.assertLess(health.index('"version"'), health.index('"apps"'), "after the version, in send_device_health")
        app = (ROOT / "main/app.c").read_text()
        for needle in ('#include "muse_apps.h"', "muse_apps_brief(", '" apps=%s/%s lua=%uK"'):
            with self.subTest(needle):
                self.assertEqual(app.count(needle), 1)
                self.assertIn("if CONFIG_MUSE_APPS", open_conditions(app, app.index(needle)))
        self.assertIn('psram=%uK%s "', app)


if __name__ == "__main__":
    unittest.main()
