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

"""apps_lv, the apply step of Muse apps: the shadow widget table turned into LVGL objects, once per frame.

tests/muse_apps_lv_harness.c runs the real apps_ui (Lua's `ui` module and its table) and the real apps_lv.c against
a fake LVGL (tests/muse_apps_lv_fakes.h, staged as lvgl.h, esp_heap_caps.h, esp_log.h and esp_timer.h, one line each,
as test_muse_ptt_ui.py does). The fake logs every call as a line, keeps every object it ever made so that a call on
a deleted one is caught, and counts an LVGL call made inside the pass without the UI lock; the harness's port is a
counting allocator (every case ends at 0 bytes held) and a lock that records how it is used.

A fake is only as good as its likeness to the real thing, so three tests compare it with LVGL 9.5.0's headers when
they are on this machine (the Espressif component cache, or managed_components/ after a device build): the
prototypes it declares, the constants the log shows, and apps_lv.c itself, compiled against the real headers.
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
TESTS = ROOT / "tests"
FAKES = TESTS / "muse_apps_lv_fakes.h"
HARNESS = TESTS / "muse_apps_lv_harness.c"
APPS_LV = APPS / "apps_lv.c"
HARNESS_SOURCES = [HARNESS, APPS_LV, APPS / "apps_ui.c", APPS / "apps_vm.c", APPS / "apps_json.c",
                   APPS / "apps_pkg.c", APPS / "apps_suggest.c"]

# What apps_lv.c includes of LVGL and ESP-IDF: each one is a line that includes the fakes.
FAKE_HEADERS = ["lvgl.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h"]


def stage_fakes(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name in FAKE_HEADERS:
        (directory / name).write_text('#include "muse_apps_lv_fakes.h"\n')
    return directory


def lvgl_dir():
    """LVGL 9.5.0's tree on this machine, or None: $MUSE_LVGL_DIR, what a device build left in managed_components/,
    or the Espressif component manager's cache ($IDF_COMPONENT_CACHE_PATH, else its usual places)."""
    candidates = []
    if os.environ.get("MUSE_LVGL_DIR"):
        candidates.append(Path(os.environ["MUSE_LVGL_DIR"]))
    candidates.append(ROOT / "managed_components/lvgl__lvgl")
    caches = [Path.home() / "Library/Caches/Espressif/ComponentManager", Path.home() / ".cache/Espressif/ComponentManager",
              Path.home() / ".espressif/ComponentManager"]
    if os.environ.get("IDF_COMPONENT_CACHE_PATH"):
        caches.insert(0, Path(os.environ["IDF_COMPONENT_CACHE_PATH"]))
    for cache in caches:
        candidates += sorted(cache.glob("*/lvgl__lvgl_9.5.0_*"))
    for path in candidates:
        version = path / "lv_version.h"
        if version.is_file():
            text = version.read_text()
            parts = [re.search(rf"#define\s+LVGL_VERSION_{part}\s+(\d+)", text) for part in ("MAJOR", "MINOR", "PATCH")]
            if all(parts) and tuple(int(m.group(1)) for m in parts) == (9, 5, 0):
                return path
    return None


class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = stage_fakes(Path(tmp.name) / "fake_headers")
        cls.harness = apps_host.build("muse_apps_lv_harness", [*HARNESS_SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[apps_host.LUA_SRC, APPS, stage, TESTS])

    def run_case(self, case, timeout=300):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-3000:] + done.stderr[-6000:])
        return done

    # ---- The brief's tests ----

    def test_create_update_delete_sequence(self):
        self.run_case("create_update_delete_sequence")

    def test_revalidation_drops_bad_slot(self):
        self.run_case("revalidation_drops_bad_slot")

    def test_psram_floor_refuses_creates(self):
        self.run_case("psram_floor_refuses_creates")

    def test_skips_mid_callback_at_most_three_frames(self):
        self.run_case("skips_mid_callback_at_most_three_frames")

    def test_overflow_from_coords(self):
        self.run_case("overflow_from_coords")

    def test_event_after_delete_carries_stale_gen(self):
        self.run_case("event_after_delete_carries_stale_gen")

    def test_many_sets_while_dark_apply_once(self):
        self.run_case("many_sets_while_dark_apply_once")

    def test_swipe_right_closes(self):
        self.run_case("swipe_right_closes")

    # ---- The rulings' tests ----

    def test_overflow_round_shapes(self):  # D1, C9
        self.run_case("overflow_round_shapes")

    def test_style_and_font_mapping(self):  # B3
        self.run_case("style_and_font_mapping")

    def test_empty_text_is_made_and_set(self):  # B2, E3
        self.run_case("empty_text_is_made_and_set")

    def test_deleting_a_box_makes_no_call_for_its_children(self):  # B4
        self.run_case("deleting_a_box_makes_no_call_for_its_children")

    def test_tap_events(self):  # B16
        self.run_case("tap_events")

    def test_a_create_that_is_pending_has_no_object(self):  # Task 9's review
        self.run_case("create_pending_has_no_object")

    def test_a_live_slot_whose_kind_is_not_its_objects_is_a_reject(self):  # Task 9's review
        self.run_case("kind_check")

    # ---- Re-validation ----

    def test_a_chain_of_four_boxes_is_drawn(self):
        self.run_case("parent_chain_of_four_is_drawn")

    def test_a_slot_that_goes_bad_after_its_object_exists_is_dropped(self):
        self.run_case("revalidation_after_the_object_exists")

    def test_a_reject_is_logged_once_per_app_and_counted_every_time(self):
        self.run_case("reject_logged_once_per_app")

    def test_bits_that_cannot_go_together_are_rejected(self):
        self.run_case("impossible_bit_combinations")

    def test_an_image_whose_bytes_are_not_an_image_is_rejected(self):
        self.run_case("bad_image_bytes_are_rejected")

    # ---- Creates, deletes and the order of work ----

    def test_deletes_come_before_creates(self):
        self.run_case("deletes_come_before_creates")

    def test_a_slot_is_free_once_its_delete_is_applied(self):
        self.run_case("slot_reuse_across_passes")

    def test_a_box_in_a_higher_slot_is_made_before_what_is_in_it(self):
        self.run_case("parent_in_a_higher_slot_is_made_first")

    def test_a_create_over_an_object_replaces_it(self):
        self.run_case("a_create_over_an_object_replaces_it")

    def test_every_kind_makes_its_object(self):
        self.run_case("kinds_make_their_objects")

    def test_values_land_in_their_range(self):
        self.run_case("values_land_in_their_range")

    def test_a_buttons_text_is_its_labels(self):
        self.run_case("button_text_is_its_labels")

    def test_images_are_drawn_from_the_assets_bytes(self):
        self.run_case("images_use_the_assets_bytes")

    def test_many_widgets(self):
        self.run_case("many_widgets")

    # ---- Clear, background, layout ----

    def test_clear_cleans_the_layer(self):
        self.run_case("clear_cleans_the_layer")

    def test_the_background_follows_the_table(self):
        self.run_case("background_follows_the_table")

    def test_the_layout_is_written_back_for_every_widget(self):
        self.run_case("layout_written_back_for_every_widget")

    def test_overflow_corner_rule(self):
        self.run_case("overflow_corner_rule")

    def test_overflow_of_hidden_and_inside(self):
        self.run_case("overflow_of_hidden_and_inside")

    # ---- Frames, the lock, timing and memory ----

    def test_lock_discipline(self):
        self.run_case("lock_discipline")

    def test_no_layer_no_pass(self):
        self.run_case("no_layer_no_pass")

    def test_a_slow_apply_is_logged_and_counted(self):
        self.run_case("slow_apply_is_logged_and_counted")

    def test_a_pass_with_no_memory_for_its_records_is_a_skip(self):
        self.run_case("a_pass_with_no_memory_is_a_skip")

    # ---- Events ----

    def test_a_widget_is_clickable_only_while_it_is_subscribed(self):
        self.run_case("clickable_only_when_subscribed")

    def test_a_subscribed_arc_cannot_be_turned_by_a_finger(self):
        self.run_case("subscribed_arc_cannot_be_scrubbed")

    # ---- The layer, build and reset ----

    def test_front_and_back(self):
        self.run_case("front_and_back")

    def test_reset_deletes_every_object(self):
        self.run_case("reset_deletes_every_object")

    def test_records_of_an_app_that_was_never_reset_are_replaced(self):
        self.run_case("stale_records_after_a_missed_reset")

    def test_build_makes_the_layer_again(self):
        self.run_case("build_makes_the_layer_again")

    # ---- Random tables ----

    def test_random_tables_and_passes_make_no_call_on_a_dead_object(self):
        # 24 seeds; MUSE_LV_FUZZ_SEEDS=2000 in the environment runs that many.
        self.run_case("fuzz", timeout=900)

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


# ---- The fakes against the real headers ----

def _strip_comments(text):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", text, flags=re.S))


def _type(text):
    text = re.sub(r"\s+", " ", text.strip())
    return text.replace(" *", "*").replace("* ", "*")


def _params(text):
    """The parameter types of a prototype's parameter list, names dropped."""
    text = text.strip()
    if text in ("", "void"):
        return []
    types = []
    for param in text.split(","):
        param = _type(param)
        match = re.match(r"^(.*?)([A-Za-z_]\w*)$", param)
        keep = match and match.group(1).strip() and not match.group(1).strip().endswith(("struct", "enum", "const", "unsigned"))
        types.append(_type(match.group(1)) if keep else param)
    return types


def prototypes(text):
    """name -> (return type, parameter types) of every `type name(params);` in text."""
    found = {}
    for match in re.finditer(r"^([A-Za-z_][\w \*]*?[ \*])([a-z_][a-z_0-9]*)\(([^;{}]*)\);", _strip_comments(text), re.M):
        found[match.group(2)] = (_type(match.group(1)), _params(match.group(3)))
    return found


class FakesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lvgl = lvgl_dir()
        if cls.lvgl is None:
            raise unittest.SkipTest("LVGL 9.5.0 isn't on this machine (set MUSE_LVGL_DIR, or build for a device once)")

    def real_prototypes(self, names):
        wanted, found = set(names), {}
        for header in (self.lvgl / "src").rglob("*.h"):
            text = _strip_comments(header.read_text(errors="replace"))
            for name in wanted:
                if name not in text:
                    continue
                for match in re.finditer(r"(?<![\w])([A-Za-z_][\w \*]*?[ \*])" + re.escape(name) + r"\(([^;{}]*)\)\s*;", text):
                    returns = _type(match.group(1).split(";")[-1].split("}")[-1])
                    found.setdefault(name, []).append((returns, _params(match.group(2))))
        return found

    def test_the_fakes_declare_the_prototypes_of_lvgl_9_5_0(self):
        fakes = {name: proto for name, proto in prototypes(FAKES.read_text()).items() if name.startswith("lv_")}
        self.assertGreater(len(fakes), 30, "the fakes' prototypes weren't found")
        real = self.real_prototypes(fakes)
        for name, (returns, params) in sorted(fakes.items()):
            with self.subTest(name):
                self.assertIn(name, real, f"{name} isn't in LVGL 9.5.0's headers")
                self.assertTrue(any(r[0].endswith(returns) and r[1] == params for r in real[name]),
                                f"{name}: the fake says {returns} ({', '.join(params)}), LVGL says {real[name]}")

    # The constants and sizes the log and the harness rely on.
    CONSTANTS = """
        LV_ALIGN_DEFAULT LV_ALIGN_TOP_LEFT LV_ALIGN_TOP_MID LV_ALIGN_TOP_RIGHT LV_ALIGN_BOTTOM_LEFT LV_ALIGN_BOTTOM_MID
        LV_ALIGN_BOTTOM_RIGHT LV_ALIGN_LEFT_MID LV_ALIGN_RIGHT_MID LV_ALIGN_CENTER
        LV_DIR_NONE LV_DIR_LEFT LV_DIR_RIGHT LV_DIR_TOP LV_DIR_BOTTOM
        LV_TEXT_ALIGN_AUTO LV_TEXT_ALIGN_LEFT LV_TEXT_ALIGN_CENTER LV_TEXT_ALIGN_RIGHT
        LV_LABEL_LONG_MODE_WRAP LV_LABEL_LONG_MODE_DOTS LV_LABEL_LONG_MODE_SCROLL LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
        LV_LABEL_LONG_MODE_CLIP
        LV_OBJ_FLAG_HIDDEN LV_OBJ_FLAG_CLICKABLE LV_OBJ_FLAG_SCROLLABLE LV_OBJ_FLAG_GESTURE_BUBBLE
        LV_PART_MAIN LV_PART_INDICATOR LV_PART_KNOB
        LV_EVENT_ALL LV_EVENT_PRESSED LV_EVENT_PRESSING LV_EVENT_PRESS_LOST LV_EVENT_SHORT_CLICKED
        LV_EVENT_SINGLE_CLICKED LV_EVENT_DOUBLE_CLICKED LV_EVENT_TRIPLE_CLICKED LV_EVENT_LONG_PRESSED
        LV_EVENT_LONG_PRESSED_REPEAT LV_EVENT_CLICKED LV_EVENT_RELEASED LV_EVENT_SCROLL_BEGIN
        LV_EVENT_SCROLL_THROW_BEGIN LV_EVENT_SCROLL_END LV_EVENT_SCROLL LV_EVENT_GESTURE LV_EVENT_KEY
        LV_EVENT_PREPROCESS
        LV_COORD_MAX LV_SIZE_CONTENT LV_RADIUS_CIRCLE LV_OPA_TRANSP LV_OPA_COVER LV_ANIM_OFF
        sizeof(lv_image_header_t) sizeof(lv_image_dsc_t) sizeof(lv_color_t) sizeof(lv_area_t)
        offsetof(lv_image_dsc_t,header) offsetof(lv_image_dsc_t,data_size) offsetof(lv_image_dsc_t,data)
        offsetof(lv_area_t,x1) offsetof(lv_area_t,x2) offsetof(lv_area_t,y1) offsetof(lv_area_t,y2)
    """.split()

    def test_the_constants_the_fakes_log_are_lvgl_9_5_0s(self):
        printer = "#include <stddef.h>\n#include <stdio.h>\n#include \"lvgl.h\"\nint main(void) {\n" + "".join(
            f'    printf("%s %lld\\n", "{name}", (long long)({name}));\n' for name in self.CONSTANTS) + "    return 0;\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "constants.c").write_text(printer)
            stage = stage_fakes(tmp / "fake_headers")
            outputs = []
            for label, includes, defines in (("fake", [stage, TESTS], []),
                                             ("real", [self.lvgl, self.lvgl / "src"], ["-DLV_CONF_SKIP=1"])):
                exe = tmp / f"constants_{label}"
                args = [*apps_host.cc(), "-std=gnu17", "-w", *defines, *(f"-I{d}" for d in includes), str(tmp / "constants.c"),
                        "-o", str(exe)]
                done = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, shlex.join(args) + "\n" + done.stdout + done.stderr)
                outputs.append(dict(line.rsplit(" ", 1) for line in subprocess.run([str(exe)], capture_output=True, text=True).stdout.splitlines()))
        fake, real = outputs
        self.assertEqual(sorted(fake), sorted(self.CONSTANTS))
        self.assertEqual(fake, real)

    def test_apps_lv_compiles_against_the_real_headers(self):
        """The device build compiles apps_lv.c against these: every name, type and argument it uses is checked."""
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            # The three ESP-IDF headers the real LVGL doesn't bring: just what apps_lv.c uses of them.
            (stage / "esp_heap_caps.h").write_text(
                "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_SPIRAM (1u << 10)\n"
                "size_t heap_caps_get_free_size(uint32_t caps);\n")
            (stage / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
            (stage / "esp_log.h").write_text(
                "#pragma once\nvoid esp_log_write(int level, const char *tag, const char *fmt, ...) "
                "__attribute__((format(printf, 3, 4)));\n#define ESP_LOGW(tag, ...) esp_log_write(1, tag, __VA_ARGS__)\n")
            # LVGL's defaults, and the fonts CONFIG_MUSE_APPS selects (components/muse/Kconfig).
            fonts = [f"-DLV_FONT_{font}=1" for font in ("MONTSERRAT_14", "MONTSERRAT_16", "MONTSERRAT_20", "MONTSERRAT_28",
                                                        "UNSCII_8", "UNSCII_16")]
            args = [*apps_host.cc(), "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-DLV_CONF_SKIP=1", *fonts, "-fsyntax-only",
                    "-isystem", str(self.lvgl), "-isystem", str(self.lvgl / "src"), f"-I{stage}", f"-I{APPS}",
                    f"-I{apps_host.LUA_SRC}", str(APPS_LV)]
            done = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, shlex.join(args) + "\n" + done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
