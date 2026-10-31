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

"""The launcher's icons (apps_icons.c): pixels read on the I/O worker into the apps' heap, handed to the LVGL task as image
descriptors, for a window of apps around the carousel's centre.

tests/muse_apps_icons_harness.c runs the real apps_icons.c and the real package rules (apps_pkg.c: the icon's check) against the
fake LVGL of the apps_lv tests (tests/muse_apps_lv_fakes.h, staged as lvgl.h and esp_log.h, one line each, as
test_muse_apps_lv.py stages it). The LVGL task is the harness's main thread; the I/O worker is a thread of its own, which runs the
jobs queued when a case says. The store's reader (apps_store_read_file, whose own cases are test_muse_apps_store.py's) is a fake
over files in memory that records the thread of every call and takes its block from the apps' heap: a counting allocator whose
floor a case can close. Every case ends with every block freed.
"""

import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_link_ota import function_source
from test_muse_apps_lv import lvgl_dir

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_icons_harness.c"
ICONS_C = APPS / "apps_icons.c"
ICONS_H = APPS / "apps_icons.h"
SOURCES = [HARNESS, ICONS_C, APPS / "apps_pkg.c", APPS / "apps_json.c", APPS / "apps_vm.c", APPS / "apps_suggest.c"]

# What the sources include of LVGL and ESP-IDF, each a line that includes the fakes; esp_attr.h says where a static goes.
STAGED = {
    "lvgl.h": '#include "muse_apps_lv_fakes.h"\n',
    "esp_log.h": '#include "muse_apps_lv_fakes.h"\n',
    "esp_heap_caps.h": '#include "muse_apps_lv_fakes.h"\n',
    "esp_attr.h": "#pragma once\n#define EXT_RAM_BSS_ATTR\n",
}


def stage(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in STAGED.items():
        (directory / name).write_text(text)
    return directory


def strip_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)


def define(name, text):
    match = re.search(rf"^#define\s+{name}\s+(.+?)\s*(?:/\*.*)?$", text, re.M)
    assert match, f"{name} isn't defined"
    return match.group(1)


class Icons(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        staged = stage(Path(tmp.name) / "fake_headers")
        cls.harness = apps_host.build("muse_apps_icons_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[staged, TESTS, APPS, apps_host.LUA_SRC])

    def run_case(self, case):
        try:
            done = apps_host.run(self.harness, case, timeout=120)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 120 s")
        self.assertEqual(done.returncode, 0, f"{case}:\n{done.stdout[-3000:]}{done.stderr[-8000:]}")
        return done.stdout

    # ---- The brief's ----

    def test_window_loads_and_frees(self):  # centre first, two loads at once, a scroll frees what left the window
        self.run_case("window_loads_and_frees")

    def test_get_is_null_until_loaded(self):  # a tile until poll has adopted the pixels; the descriptor is the file's
        self.run_case("get_is_null_until_loaded")

    def test_refused_load_keeps_tile(self):  # Review Focus 3: the floor refuses, get stays NULL, nothing leaks; asked again later
        self.run_case("refused_load_keeps_tile")

    def test_list_change_drops_stale_icons(self):  # Review Focus 2: drop_all while a load is in flight; never shown
        self.run_case("list_change_drops_stale_icons")

    def test_keep_frees_all_but_the_kept(self):  # Task 6a fix round 1 (I1): the hold keeps the drawn icons, frees and orphans the rest
        self.run_case("keep_frees_all_but_the_kept")

    def test_bad_icon_file_is_a_tile(self):  # not square, too big, not an image, cut short, missing: a W line, a tile
        self.run_case("bad_icon_file_is_a_tile")

    def test_no_flash_on_the_lvgl_task(self):  # X11: every read of the store is the worker thread's
        self.run_case("no_flash_on_the_lvgl_task")

    # ---- Past the brief ----

    def test_a_drop_racing_the_worker_frees_every_block(self):  # whichever side loses frees it; nothing polls after a close
        self.run_case("a_drop_racing_the_worker_frees_every_block")

    def test_a_refused_submit_is_tried_again_later(self):  # the worker's queue full: no slot lost, asked again after the wait
        self.run_case("a_refused_submit_is_tried_again_later")

    def test_a_fling_keeps_the_worker_to_two_loads(self):  # windows that pass quickly don't queue a load for every app
        self.run_case("a_fling_keeps_the_worker_to_two_loads")

    def test_ram_mode_is_a_tile(self):  # no FAT: nothing is asked of the worker
        self.run_case("ram_mode_is_a_tile")

    def test_odd_windows(self):  # NULLs, duplicates, bad ids and paths, more than a window, no icon
        self.run_case("odd_windows")

    def test_any_order_of_calls_keeps_the_bounds(self):  # a long pseudo-random run: the slots and blocks stay bounded
        self.run_case("any_order_of_calls_keeps_the_bounds")

    def test_every_case_of_the_harness_is_run_here(self):
        done = apps_host.run(self.harness, "--cases")
        self.assertEqual(done.returncode, 0, done.stderr)
        called = set(re.findall(r'self\.run_case\("([a-z_]+)"', Path(__file__).read_text()))
        cases = set(done.stdout.split())
        self.assertEqual(cases - called, set(), "a case of the harness that no test runs")
        self.assertEqual(called - cases, set(), "a test that runs a case the harness doesn't have")


class Source(unittest.TestCase):
    """What the cases can't see: where apps_icons.c's memory and flash come from."""

    @classmethod
    def setUpClass(cls):
        cls.code = strip_comments(ICONS_C.read_text())
        cls.header = ICONS_H.read_text()

    def test_the_window_is_the_centre_and_two_each_side(self):
        self.assertEqual(define("APPS_ICONS_WINDOW", self.header), "5")

    def test_the_table_is_at_most_two_windows(self):
        window = int(define("APPS_ICONS_WINDOW", self.header))
        in_flight = int(define("APPS_ICONS_IN_FLIGHT", self.header))
        self.assertEqual(define("APPS_ICONS_SLOTS", self.header), "(APPS_ICONS_WINDOW + APPS_ICONS_IN_FLIGHT)")
        self.assertLessEqual(window + in_flight, 2 * window)

    def test_the_table_is_in_psram(self):  # X12
        self.assertRegex(self.code, r"static\s+EXT_RAM_BSS_ATTR\s+struct\s*\{")

    def test_no_atomic_is_in_psram(self):
        # S3 PSRAM has no read-modify-write (S32C1I): what another task loads, stores or swaps is an internal static (apps_alarm.c,
        # apps_values.c: "atomics and locks stay internal"). The table and the window are plain data, in PSRAM.
        atomic = r"\b(_Atomic|atomic_\w+_t|atomic_(bool|char|schar|uchar|short|ushort|int|uint|long|ulong|llong|ullong|size_t))\b"
        for match in re.finditer(r"typedef struct \{(.*?)\n\} (\w+);", self.code, re.S):
            self.assertNotRegex(match.group(1), atomic, f"{match.group(2)} holds an atomic, and lives in the PSRAM table")
        for match in re.finditer(r"static\s+EXT_RAM_BSS_ATTR\s+struct\s*\{(.*?)\n\}\s*(\w+);", self.code, re.S):
            self.assertNotRegex(match.group(1), atomic, f"{match.group(2)}: an atomic in PSRAM")
        for line in self.code.splitlines():
            if re.search(atomic, line) and line.startswith("static"):
                self.assertNotIn("EXT_RAM_BSS_ATTR", line, line)
        self.assertRegex(self.code, r"(?m)^static atomic_uint s_hand\[APPS_ICONS_SLOTS\];", "the slots' hand-off words, internal")

    def test_a_refusal_by_the_floor_is_the_stores_words(self):
        # The job tells a shortage (asked again after the wait) from a bad file (a tile while it stays) by the store's own words.
        self.assertTrue("APPS_STORE_MSG_NO_MEMORY" in self.code, "apps_icons.c doesn't tell a shortage by the store's words")
        store = strip_comments((APPS / "apps_store.c").read_text())
        reader = function_source(store, "static bool read_file(")
        self.assertTrue("APPS_STORE_MSG_NO_MEMORY" in reader, "read_file doesn't say APPS_STORE_MSG_NO_MEMORY")
        self.assertTrue('"out of memory"' not in reader, "read_file spells the words out")

    def test_no_block_bypasses_the_floor(self):
        # The pixels' block is the store's reader's (apps_port_realloc, the floor): apps_icons.c allocates nothing of its own.
        self.assertNotRegex(self.code, r"\b(malloc|calloc|realloc|heap_caps_\w+|free)\s*\(")
        self.assertIn("apps_port_free(", self.code)

    def test_the_only_flash_is_the_workers_job(self):  # X11
        self.assertNotRegex(self.code, r"\b(open|fopen|read|fread|stat|fstat|opendir|nvs_\w+)\s*\(")
        self.assertEqual(len(re.findall(r"\bapps_store_read_file\s*\(", self.code)), 1)
        job = function_source(self.code, "static void load_job(void *arg)")
        self.assertIn("apps_store_read_file(", job)
        self.assertIn("APPS_ICON_MAX_BYTES", job)
        self.assertIn("apps_icon_check(", job, "the square-and-size rule, on top of apps_image_check")
        self.assertIn("apps_io_submit(load_job,", self.code)
        # apps_io_submit_op asserts on any task but the app task: the LVGL task submits jobs.
        self.assertNotIn("apps_io_submit_op", self.code)

    def test_the_descriptor_is_made_as_apps_lv_makes_an_images(self):
        self.assertIn("header.flags &= LV_IMAGE_FLAGS_PREMULTIPLIED", self.code)
        self.assertRegex(self.code, r"reserved\s*=\s*NULL")
        self.assertRegex(self.code, r"reserved_2\s*=\s*NULL")


class RealLvgl(unittest.TestCase):
    """apps_icons.c compiled against LVGL 9.5.0's own headers, when they are on this machine: the descriptor's fields and the
    flag it keeps are LVGL's."""

    def test_apps_icons_compiles_against_the_real_headers(self):
        lvgl = lvgl_dir()
        if lvgl is None:
            self.skipTest("LVGL 9.5.0 isn't on this machine (set MUSE_LVGL_DIR, or build for a device once)")
        with tempfile.TemporaryDirectory() as tmp:
            staged = Path(tmp)
            (staged / "esp_attr.h").write_text("#pragma once\n#define EXT_RAM_BSS_ATTR\n")
            (staged / "esp_log.h").write_text(
                "#pragma once\nvoid esp_log_write(int level, const char *tag, const char *fmt, ...) "
                "__attribute__((format(printf, 3, 4)));\n#define ESP_LOGW(tag, ...) esp_log_write(1, tag, __VA_ARGS__)\n")
            args = [*apps_host.cc(), "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-DLV_CONF_SKIP=1", "-fsyntax-only",
                    "-isystem", str(lvgl), "-isystem", str(lvgl / "src"), f"-I{staged}", f"-I{APPS}", str(ICONS_C)]
            done = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, shlex.join(args) + "\n" + done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
