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
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import apps_host

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
MUSE = ROOT / "components/muse"
LUA = ROOT / "components/lua"

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


class MuseAppsWiringTest(unittest.TestCase):
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
                header = re.search(r"^ \* Opts: (.*)$", text, re.M)
                self.assertIsNotNone(header, "no Opts: line, so not lv_font_conv's output")
                opts = header.group(1).split()
                self.assertEqual(lv_font_conv_points(opts), spec)
                drawn = {int(point, 16) for point in re.findall(r"^\s*/\* U\+([0-9A-F]+) ", text, re.M)}
                self.assertEqual(drawn, spec)
                self.assertEqual(opts[opts.index("--bpp") + 1], "4")
                self.assertIn("--no-compress", opts)
                self.assertEqual(opts[opts.index("--size") + 1], str(size))
                self.assertEqual(opts[opts.index("--lv-font-name") + 1], name)
                self.assertIn(f'"apps/fonts/{name}.c"', block)
                # The generator's sed: the project's include, and no path of the machine that ran it.
                self.assertIn('#include "lvgl.h"', text)
                self.assertNotIn("lvgl/lvgl.h", text)
                self.assertEqual([arg for arg in opts if arg.startswith("/")], [])
                self.assertNotIn("/", opts[opts.index("-o") + 1])
        # What regenerates them asks for the same glyphs.
        script = (ROOT / "tools/muse/gen_app_fonts.sh").read_text()
        self.assertEqual(lv_font_conv_points([word for word in script.split() if word != "\\"]), spec)

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
