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

tests/muse_apps_lv_harness.c runs the real apps_ui (Lua's `ui` module and its table), the real apps_lv.c and the real
apps_icons.c (the launcher's icons) against a fake LVGL (tests/muse_apps_lv_fakes.h, staged as lvgl.h, esp_attr.h,
esp_heap_caps.h, esp_log.h and esp_timer.h, one line each, as test_muse_ptt_ui.py does), a fake I/O worker that runs its
jobs when a case says, a store of icon files in memory, and (for the runtime images' cases, connected apps Task 2) the real
runtime, apps_rt.c with its services. The fake logs every call as a line, keeps every object it ever made so that a call on
a deleted one is caught, and counts an LVGL call made inside the pass without the UI lock; the harness's port is a
counting allocator (every case ends at 0 bytes held) and a lock that records how it is used.

A fake is only as good as its likeness to the real thing, so five tests compare it with LVGL 9.5.0's tree when
it is on this machine (the Espressif component cache, or managed_components/ after a device build): the
prototypes it declares, the constants the log shows, the one default of an image the fake copies (it is centred in
its box), the numbers and object defaults its touch works from (a drag, a throw, a gesture, a scroll's animation),
and apps_lv.c itself, compiled against the real headers.
"""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

import apps_host
from test_muse_apps_rt import header_number
from test_muse_apps_wiring import c_function, open_conditions, strip_c_comments

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
TESTS = ROOT / "tests"
FAKES = TESTS / "muse_apps_lv_fakes.h"
HARNESS = TESTS / "muse_apps_lv_harness.c"
APPS_LV = APPS / "apps_lv.c"
HARNESS_SOURCES = [HARNESS, APPS_LV, APPS / "apps_icons.c", APPS / "apps_ui.c", APPS / "apps_vm.c", APPS / "apps_json.c",
                   APPS / "apps_pkg.c", APPS / "apps_suggest.c", APPS / "apps_rt.c", APPS / "apps_svc.c",
                   APPS / "apps_vmio.c", APPS / "apps_ask.c"]

# What apps_lv.c and apps_icons.c include of LVGL and ESP-IDF: each one is a line that includes the fakes.
FAKE_HEADERS = ["lvgl.h", "esp_attr.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h"]


def stage_fakes(directory, bench=False):
    """The fake headers in directory, and an sdkconfig.h: CONFIG_MUSE_BENCH for a bench build (bench), else nothing."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name in FAKE_HEADERS:
        (directory / name).write_text('#include "muse_apps_lv_fakes.h"\n')
    (directory / "sdkconfig.h").write_text("#pragma once\n" + ("#define CONFIG_MUSE_BENCH 1\n" if bench else ""))
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


def build_harness(tmp, bench=False):
    stage = stage_fakes(Path(tmp) / "fake_headers", bench=bench)
    return apps_host.build("muse_apps_lv_harness_bench" if bench else "muse_apps_lv_harness",
                           [*HARNESS_SOURCES, *apps_host.lua_sources()], tmp, include_dirs=[apps_host.LUA_SRC, APPS, stage, TESTS])


class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = build_harness(tmp.name)

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

    def test_a_button_shows_its_pressed_state(self):  # P41
        self.run_case("button_shows_its_pressed_state")

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

    def test_an_images_flags_are_masked_to_premultiplied(self):  # F5
        self.run_case("image_flags_are_masked")

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

    def test_an_a8_image_is_drawn_white(self):  # F1
        self.run_case("a8_images_are_white")

    def test_a_scaled_image_has_the_box_that_is_drawn(self):  # F2
        self.run_case("scaled_images_have_the_scaled_box")

    def test_a_create_that_fails_is_tried_again(self):  # F9
        self.run_case("a_create_that_fails_is_tried_again")

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

    # ---- Who holds the table: the apply step does (apps_lv_set_ui, read under the lock) ----

    def test_a_table_cleared_under_the_lock_can_be_freed(self):
        # The app task's end: clear the pointer with the lock held, free the table. What the harness's ASan watches for.
        self.run_case("a_cleared_table_can_be_freed")

    def test_the_table_is_read_under_the_lock(self):
        # The app task ends while the pass waits for the lock: a pointer read before the lock would be a freed table.
        self.run_case("the_table_is_read_under_the_lock")

    def test_the_app_can_end_while_the_records_are_made(self):
        # The first pass lets go of the lock to make its records; the pointer has to be read again after.
        self.run_case("the_app_can_end_while_the_records_are_made")

    def test_the_lock_may_be_refused_after_the_records_are_made(self):
        self.run_case("the_lock_may_be_refused_after_the_records_are_made")

    def test_no_table_is_neither_a_skip_nor_a_reject(self):
        self.run_case("no_table_is_no_pass")

    def test_a_callback_frame_is_skipped_before_the_table_is_read(self):  # F14
        self.run_case("a_callback_frame_is_skipped_before_the_table_is_read")

    def test_the_published_table_is_the_one_applied(self):
        self.run_case("the_published_table_is_the_one_applied")

    def test_a_slow_apply_is_logged_and_counted(self):
        self.run_case("slow_apply_is_logged_and_counted")

    def test_a_pass_with_no_memory_for_its_records_is_a_skip(self):
        self.run_case("a_pass_with_no_memory_is_a_skip")

    # ---- Events ----

    def test_a_widget_is_clickable_only_while_it_is_subscribed(self):
        self.run_case("clickable_only_when_subscribed")

    def test_a_subscribed_arc_cannot_be_turned_by_a_finger(self):
        self.run_case("subscribed_arc_cannot_be_scrubbed")

    def test_a_swipe_that_starts_on_a_widget_ends_its_press(self):  # F3
        self.run_case("a_swipe_that_starts_on_a_widget_ends_its_press")

    # ---- The layer, build and reset ----

    def test_front_and_back(self):
        self.run_case("front_and_back")

    def test_reset_deletes_every_object(self):
        self.run_case("reset_deletes_every_object")

    def test_records_of_an_app_that_was_never_reset_are_replaced(self):
        self.run_case("stale_records_after_a_missed_reset")

    def test_build_makes_the_layer_again(self):
        self.run_case("build_makes_the_layer_again")

    # ---- The launcher and the crash card (Task 12; the carousel, launcher carousel Task 4) ----

    def test_scale_curve(self):  # 256 at the centre, falling linearly to the side scale a pitch away, and that beyond
        self.run_case("scale_curve")

    def test_tile_color_is_stable_and_in_palette(self):  # one of 8 colours, by the CRC-32 of the id (zlib's)
        source = _strip_comments(APPS_LV.read_text())
        match = re.search(r"static const uint32_t TILE_PALETTE\[(\d+)\] = \{([^}]*)\};", source)
        self.assertIsNotNone(match, "no TILE_PALETTE in apps_lv.c")
        palette = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", match.group(2))]
        self.assertEqual(int(match.group(1)), 8)
        self.assertEqual(len(palette), 8)
        self.assertEqual(len(set(palette)), 8, "eight different colours")
        out = self.run_case("tile_colors").stdout
        colours = dict(line.split()[1:] for line in out.splitlines() if line.startswith("color "))
        self.assertGreater(len(colours), 10)
        for name, hexed in colours.items():
            key = {"(null)": "", "(empty)": ""}.get(name, name)
            with self.subTest(name):
                self.assertEqual(int(hexed, 16), palette[zlib.crc32(key.encode()) % 8])
        self.assertGreaterEqual(len(set(colours.values())), 4, "the ids spread over the palette")

    def test_tile_letter(self):  # "timer" T, "9lives" 9, a UTF-8 first byte ?
        self.run_case("tile_letter")

    def test_opens_centred_on_first_app(self):
        self.run_case("opens_centred_on_first_app")

    def test_one_name_label_shows_the_centre_app(self):
        self.run_case("one_name_label_shows_the_centre_app")

    def test_running_dot_under_running_app(self):
        self.run_case("running_dot_under_running_app")

    def test_icon_or_tile_never_blank(self):  # a tile until the pixels are polled in; the icon then; a tile again once it goes
        self.run_case("icon_or_tile_never_blank")

    def test_every_app_is_an_item(self):  # 12 apps, 12 items; more than APPS_LV_LAUNCHER_MAX, that many
        self.run_case("every_app_is_an_item")

    def test_loading_then_list_rebuilds(self):  # Review Focus 1 (and P6, O): "Loading apps...", then the carousel; rebuilt on a change
        self.run_case("loading_then_list_rebuilds")

    def test_long_and_non_ascii_names(self):  # Review Focus 5: inside the circle, ellipsised; the tile is '?'
        self.run_case("long_and_non_ascii_names")

    def test_the_launcher_lists_apps_and_a_tap_runs_one(self):
        self.run_case("launcher_lists_apps_and_picks")

    def test_a_tap_on_no_app_closes_the_launcher(self):  # C12
        self.run_case("launcher_closes_on_a_tap_elsewhere")

    def test_the_launcher_with_no_apps(self):  # C12: the empty-state text, unchanged, closes it too
        self.run_case("launcher_empty_state")

    def test_a_swipe_that_begins_on_an_app_closes_nothing(self):  # P3, and the edge rule: the bottom band is the card's
        self.run_case("launcher_a_swipe_on_an_app_closes_nothing")

    def test_the_launcher_keeps_its_own_copy(self):
        self.run_case("launcher_keeps_its_own_copy")

    def test_the_launcher_survives_a_create_that_fails(self):
        self.run_case("launcher_survives_a_create_that_fails")

    def test_a_scroll_after_a_close_asks_nothing(self):  # fix round 1, I1: a snap animation after a close loads no icon
        self.run_case("a_scroll_after_a_close_asks_nothing")

    # ---- Taps and the bottom-edge close (launcher carousel Task 5), through the fake's pointer ----

    def test_tap_centre_launches(self):  # the app at the centre, at rest: anywhere in its item
        self.run_case("tap_centre_launches")

    def test_tap_side_scrolls_not_launches(self):  # and M2: the whole of a side item's box scrolls; between items, nothing
        self.run_case("tap_side_scrolls_not_launches")

    def test_tap_while_scrolling_does_not_launch(self):  # Review Focus 4, M3: a drag, a fling's snap, scroll_to_view's animation
        self.run_case("tap_while_scrolling_does_not_launch")

    def test_bottom_edge_swipe_up_closes(self):  # within APPS_LV_CLOSE_EDGE_PX of the bottom, inside the circle; icons dropped
        self.run_case("bottom_edge_swipe_up_closes")

    def test_swipe_up_from_middle_does_nothing(self):  # each beside a twin that starts inside the rule and closes
        self.run_case("swipe_up_from_middle_does_nothing")

    def test_right_swipe_no_longer_closes(self):  # P3's pin
        self.run_case("right_swipe_no_longer_closes")

    def test_horizontal_swipe_only_scrolls(self):
        self.run_case("horizontal_swipe_only_scrolls")

    # ---- Fix round 1, I1: a tap is a touch that lifts within LVGL's scroll limit of where it went down ----

    def test_a_short_slow_sideways_drag_on_the_background_does_not_close(self):  # under 50 px, or under 3 px a read: no gesture
        self.run_case("a_short_slow_sideways_drag_on_the_background_does_not_close")

    def test_a_short_slow_sideways_drag_on_the_name_does_not_close(self):
        self.run_case("a_short_slow_sideways_drag_on_the_name_does_not_close")

    def test_a_tap_on_the_background_still_closes(self):  # C12, kept: a lift under the scroll limit away on each axis
        self.run_case("a_tap_on_the_background_still_closes")

    def test_a_slide_on_an_app_opens_nothing(self):  # the same root on the apps: a slow or short vertical slide
        self.run_case("a_slide_on_an_app_opens_nothing")

    # ---- The final review's fixes ----

    def test_close_launcher_closes_it_alone(self):  # I1: the glue's close as the screen goes dark: icons dropped, a crash card kept
        self.run_case("close_launcher_closes_it_alone")

    def test_a_tap_on_the_name_opens_the_centre_app(self):  # m1: the strip from the row through the name; the tap rules; C12 around it
        self.run_case("a_tap_on_the_name_opens_the_centre_app")

    def test_running_dot_follows_its_face(self):  # hardening (a), 6a m2: under the face as drawn, at every scale
        self.run_case("running_dot_follows_its_face")

    # ---- Task 6a: the user's feedback on preview 2: a clearly larger centre, and a launch with no clock flash ----

    def test_centre_is_clearly_larger(self):  # 150-160 px at the centre, a neighbour at most 40% of it, all inside the circle
        self.run_case("centre_is_clearly_larger")

    def test_the_hold_drops_offscreen_icons(self):  # fix round 1 (I1): a pick keeps the three icons on the screen, frees the rest, loads none
        self.run_case("the_hold_drops_offscreen_icons")

    def test_launch_holds_the_launcher_until_the_first_frame(self):  # nothing in apps_lv ends it: the glue does, at the first frame
        self.run_case("launch_holds_the_launcher_until_the_first_frame")

    def test_no_input_while_the_launch_holds(self):  # no tap, scroll or swipe up; its icons as they were
        self.run_case("no_input_while_the_launch_holds")

    def test_the_hold_times_out(self):  # APPS_LV_LAUNCH_HOLD_MS after the pick
        self.run_case("the_hold_times_out")

    def test_a_voice_turn_ends_the_hold(self):  # apps_lv_hide_cards closes a launcher that holds, at once
        self.run_case("a_voice_turn_ends_the_hold")

    def test_a_touch_the_launcher_closed_under_does_nothing(self):  # Task 5's m3: the card's tap and swipe up, after a hide
        self.run_case("a_touch_the_launcher_closed_under_does_nothing")

    def test_the_launchers_records_are_in_psram_and_the_rows_are_gone(self):
        # X12 and the follow-up round's internal RAM: the carousel's copy of the list (32 ids, names and icon paths) and its
        # objects are one static of the LVGL task's, in PSRAM; the old card's row count is gone.
        source = _strip_comments(APPS_LV.read_text())
        self.assertRegex(source, r"static EXT_RAM_BSS_ATTR struct \{[^}]*\bitem_t item\[APPS_LV_LAUNCHER_MAX\];",
                         "the carousel's records aren't one PSRAM static")
        self.assertNotIn("APPS_LV_LAUNCHER_ROWS", source + (APPS / "apps_lv.h").read_text())
        self.assertIn('#include "esp_attr.h"', APPS_LV.read_text())

    # ---- Task 6: the frame gate's measurement is a bench build's alone ----

    def test_a_build_without_the_bench_logs_no_frames(self):  # nothing on the display, nothing logged, whatever scrolls
        self.run_case("launcher_frame_log")

    def test_the_frame_log_is_the_bench_builds_alone(self):
        # Every part of it is inside #if CONFIG_MUSE_BENCH: the callback, its state, the line and the registration in apps_lv_build.
        # The one exception is Task 7's frame pairing (R7a), which follows the spin in every build, but registers its refresh callback
        # only once the board gives one (apps_lv_set_pairing_cb): a build with neither adds nothing to the display.
        source = APPS_LV.read_text()
        self.assertIn('#include "sdkconfig.h"', source)
        pairing_start = source.index("/* An image spins on the layer in front, with no launcher or card over it:")
        watch_spin = c_function(source, "static void watch_spin(lv_obj_t *screen) {")
        pairing_end = source.index(watch_spin) + len(watch_spin)
        self.assertIn("s_pairing_cb != NULL && screen != NULL ? lv_obj_get_display(screen) : NULL", watch_spin)
        mentions = [m.start() for m in re.finditer(r"on_refresh|watch_frames|s_frames|launcher frame|spin frame|LV_EVENT_REFR_READY|"
                                                   r"lv_display_add_event_cb|lv_obj_get_display", source)
                    if not pairing_start <= m.start() < pairing_end]
        self.assertGreaterEqual(len(mentions), 7)
        self.assertIn("spin frame", " ".join(source[i:i + 12] for i in mentions))
        for index in mentions:
            with self.subTest(source[source.rindex("\n", 0, index) + 1:source.index("\n", index)].strip()):
                self.assertEqual(open_conditions(source, index), ["if CONFIG_MUSE_BENCH"])
        build = c_function(source, "void apps_lv_build(lv_obj_t *screen)")
        self.assertIn("watch_frames(screen);", build)

    def test_the_crash_card(self):
        self.run_case("crash_card_text_and_tap")

    def test_a_reset_leaves_the_cards(self):
        self.run_case("cards_survive_a_reset")

    def test_no_layer_no_cards(self):
        self.run_case("cards_need_a_layer")

    # ---- Connected apps Task 1: an image's rotation and spin ----

    def test_image_rotation_applies(self):  # rotation 90 is lv_image_set_rotation(obj, 900): the image's own angle, about its centre
        self.run_case("image_rotation_applies")

    def test_image_spin_animates_and_stops_in_place(self):  # one animation, 2,000 ms a turn at 30; spin 0 keeps the angle; resumes
        self.run_case("image_spin_animates_and_stops_in_place")

    def test_rotation_sets_the_angle_a_spin_continues_from(self):
        self.run_case("rotation_sets_the_angle_a_spin_continues_from")

    def test_a_spinning_circle_invalidates_its_square(self):  # Task 7: a record's room for its turn is its own; corners keep LVGL's
        self.run_case("spin_invalidates_what_it_draws")

    def test_spinner_is_made_and_styled(self):  # connected apps Task 8: lv_spinner_create, an arc's style, APPS_SPINNER_SIDE, no value
        self.run_case("spinner_is_made_and_styled")

    def test_spinner_animates_only_while_shown(self):
        # Task 8: LVGL's own animation, whose steps invalidate nothing while the spinner or a box above it is hidden; no spin count
        # (pairing follows it: test_pairing_follows_a_shown_spinner).
        self.run_case("spinner_animates_only_while_shown")

    def test_pairing_follows_a_shown_spinner(self):
        # Task 8, R8a: the frame pairing is on while a spinner is shown on the layer in front (not hidden, no hidden box above, no
        # launcher or card over it) and off when the last is hidden, deleted or behind. A spinner is still no spinning image.
        self.run_case("pairing_follows_a_shown_spinner")

    def test_spinner_is_made_by_lvgl_alone(self):
        # Task 8: the animation is lv_spinner's (1,000 ms a turn, 200 degrees in 9.5.0), so apps_lv.c's only mention of a spinner is
        # lv_spinner_create in new_object: it sets no animation parameters of its own, starts no lv_anim for it (the spin test above
        # still holds: apply_turn and start_spin own every lv_anim_ call), and a spinner is no spinning image (s_spinning).
        source = strip_c_comments(APPS_LV.read_text())
        calls = re.findall(r"\blv_spinner_\w+", source)
        self.assertEqual(calls, ["lv_spinner_create"], "the apply step only makes the spinner")
        make = re.search(r"static lv_obj_t \*new_object\(.*?\n\}\n", source, re.S).group(0)
        self.assertRegex(make, r"case APPS_W_SPINNER:\s*obj = lv_spinner_create\(parent\);")
        self.assertRegex(make, r"kind == APPS_W_SPINNER")  # its knob is taken off, as an arc's is
        for path in sorted(APPS.glob("*.c")):
            if path != APPS_LV:
                self.assertNotIn("lv_spinner", strip_c_comments(path.read_text()), path.name)

    def test_pairing_follows_the_spin(self):
        # Task 7, fix round 1 (R7a): the board's frame pairing is on only while an image spins on the layer in front (no launcher or
        # card over it): a spin turns it on, and the last spin stopped or deleted, the app behind, the launcher or a card turn it off.
        self.run_case("pairing_follows_the_spin")
        # And only the 1.75C pairs, the board it was measured on: muse_ui.c hands the app layer the board's op, the 1.75C's is
        # muse_lcd_bands_set_pairing, and no other board has one, so the plain build and the other boards send band by band.
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertRegex(ui, r"muse_apps_ui_build\(scr\);\s*muse_apps_ui_pairing\(muse_board->frame_pairing\);")
        boards = {path.name: path.read_text() for path in (ROOT / "components/muse/boards").glob("board_*.c")}
        pairing = sorted(name for name, text in boards.items() if ".frame_pairing" in text)
        self.assertEqual(pairing, ["board_waveshare_s3_175c.c"])
        self.assertIn(".frame_pairing = muse_lcd_bands_set_pairing,", boards["board_waveshare_s3_175c.c"])
        self.assertIn("apps_lv_set_pairing_cb(pairing);", (APPS / "apps_task.c").read_text())

    # ---- Connected apps Task 2: runtime images ----

    def test_runtime_image_shows_in_ui_image(self):  # drawn from its block in place; rotation, spin and scale as a package image's
        self.run_case("runtime_image_shows_in_ui_image")

    def test_freed_runtime_image_is_never_drawn(self):  # Review Focus 3: the bytes stay until a pass that no longer draws them
        self.run_case("freed_runtime_image_is_never_drawn")

    def test_images_free_at_app_end_after_the_reset(self):
        self.run_case("images_free_at_app_end_after_the_reset")

    def test_the_pass_reports_what_it_draws(self):  # a rejected widget's object keeps its picture, and its image
        self.run_case("the_pass_reports_what_it_draws")

    def test_a_slots_next_image_can_be_at_the_same_address(self):  # its descriptor is made from its own header
        self.run_case("a_slots_next_image_can_be_at_the_same_address")

    def test_spin_ends_with_the_widget(self):  # a delete, ui.clear(), a box's delete, the layer's reset: no animation is left
        self.run_case("spin_ends_with_the_widget")

    def test_a_build_without_the_bench_logs_no_spin_frames(self):
        self.run_case("spin_frame_log")

    def test_spin_is_on_the_lvgl_task_only(self):
        # The animation is the apply step's: every lv_anim_ call of the apps' sources is apps_lv.c's, in apply_turn or start_spin,
        # which only the apply step runs (apply_style calls apply_turn, and apply_turn start_spin). LVGL runs it on its own task, and
        # its callbacks call no lv_anim_ function.
        call = re.compile(r"\blv_anim_\w+\(")
        for path in sorted(APPS.glob("*.c")):
            if path != APPS_LV:
                with self.subTest(path.name):
                    self.assertIsNone(call.search(strip_c_comments(path.read_text())), f"{path.name} calls lv_anim")
        source = strip_c_comments(APPS_LV.read_text())
        definitions = [(m.group(1), m.start()) for m in
                       re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \*]*?\b(\w+)\([^;{}]*\)\s*\{", source, re.M)]
        bodies = {name: (start, start + len(c_function(source[start:], source[start:source.index("(", start)])))
                  for name, start in definitions}

        def owner(index):
            inside = [name for name, (start, end) in bodies.items() if start <= index < end]
            return inside[0] if inside else None

        owners = {owner(m.start()) for m in call.finditer(source)}
        self.assertEqual(owners, {"apply_turn", "start_spin"})
        for name, callers in (("apply_turn", {"apply_style"}), ("start_spin", {"apply_turn"})):
            with self.subTest(name):
                calls = {owner(m.start()) for m in re.finditer(rf"\b{name}\(", source)} - {None}
                self.assertEqual(calls - {name}, callers, f"what calls {name}")
        self.assertEqual({owner(m.start()) for m in re.finditer(r"\bapply_style\(", source)} - {None, "apply_style"},
                         {"visit_live"}, "apply_style is the pass's")
        self.assertIn("lv_anim_set_var(&a, obj);", c_function(source, "static bool start_spin("))

    def test_spin_step_is_the_board_gates(self):
        # connected apps Task 1, Step 5: the step of a spinning image's angle, in its animation's time, as the board's spin gate left it.
        # 66 drew a 400 px disc no faster (its frames are the transform's time) and halved a 200 px disc's rate, which passes at 33
        # (task-1-report.md has the runs). A retune is a new gate run.
        self.assertEqual(header_number("apps_lv.h", "APPS_SPIN_STEP_MS"), 33)

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


class BenchHarnessTest(unittest.TestCase):
    """The harness built as a MUSE_BENCH build is (its sdkconfig.h has CONFIG_MUSE_BENCH): the frame gate's log line."""

    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = build_harness(tmp.name, bench=True)

    run_case = HarnessTest.run_case

    def test_the_launcher_logs_its_frames_while_it_scrolls(self):  # Task 6: "launcher frame <ms> ms", only while the row moves
        self.run_case("launcher_frame_log")

    def test_an_image_logs_its_frames_while_it_spins(self):  # connected apps Task 1: "spin frame <ms> ms", only while one spins in front
        self.run_case("spin_frame_log")


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
        LV_OBJ_FLAG_HIDDEN LV_OBJ_FLAG_CLICKABLE LV_OBJ_FLAG_SCROLLABLE LV_OBJ_FLAG_SNAPPABLE LV_OBJ_FLAG_GESTURE_BUBBLE
        LV_SCROLLBAR_MODE_OFF LV_SCROLLBAR_MODE_ON LV_SCROLLBAR_MODE_ACTIVE LV_SCROLLBAR_MODE_AUTO
        LV_SCROLL_SNAP_NONE LV_SCROLL_SNAP_START LV_SCROLL_SNAP_END LV_SCROLL_SNAP_CENTER LV_DIR_HOR LV_DIR_VER LV_DIR_ALL
        LV_PART_MAIN LV_PART_INDICATOR LV_PART_KNOB LV_STATE_DEFAULT LV_STATE_PRESSED
        LV_EVENT_ALL LV_EVENT_PRESSED LV_EVENT_PRESSING LV_EVENT_PRESS_LOST LV_EVENT_SHORT_CLICKED
        LV_EVENT_SINGLE_CLICKED LV_EVENT_DOUBLE_CLICKED LV_EVENT_TRIPLE_CLICKED LV_EVENT_LONG_PRESSED LV_EVENT_REFR_EXT_DRAW_SIZE
        LV_EVENT_LONG_PRESSED_REPEAT LV_EVENT_CLICKED LV_EVENT_RELEASED LV_EVENT_SCROLL_BEGIN
        LV_EVENT_SCROLL_THROW_BEGIN LV_EVENT_SCROLL_END LV_EVENT_SCROLL LV_EVENT_GESTURE LV_EVENT_KEY
        LV_EVENT_REFR_READY LV_EVENT_PREPROCESS
        LV_COORD_MAX LV_SIZE_CONTENT LV_RADIUS_CIRCLE LV_OPA_TRANSP LV_OPA_30 LV_OPA_COVER LV_ANIM_OFF LV_ANIM_ON
        LV_IMAGE_ALIGN_DEFAULT LV_IMAGE_ALIGN_CENTER LV_COLOR_FORMAT_A8 LV_COLOR_FORMAT_RGB565A8 LV_IMAGE_FLAGS_PREMULTIPLIED
        sizeof(lv_image_header_t) sizeof(lv_image_dsc_t) sizeof(lv_color_t) sizeof(lv_area_t)
        offsetof(lv_image_dsc_t,header) offsetof(lv_image_dsc_t,data_size) offsetof(lv_image_dsc_t,data)
        offsetof(lv_area_t,x1) offsetof(lv_area_t,x2) offsetof(lv_area_t,y1) offsetof(lv_area_t,y2)
        sizeof(lv_point_t) offsetof(lv_point_t,x) offsetof(lv_point_t,y)
        LV_BEZIER_VAL_SHIFT LV_BEZIER_VAL_MAX LV_BEZIER_VAL_FLOAT(0.42) LV_BEZIER_VAL_FLOAT(0.58) LV_BEZIER_VAL_FLOAT(1)
        offsetof(lv_anim_t,parameter) sizeof(lv_anim_bezier3_para_t)
        LV_ANIM_REPEAT_INFINITE sizeof(lv_anim_t) offsetof(lv_anim_t,var) offsetof(lv_anim_t,exec_cb) offsetof(lv_anim_t,deleted_cb)
        offsetof(lv_anim_t,path_cb) offsetof(lv_anim_t,start_value) offsetof(lv_anim_t,current_value) offsetof(lv_anim_t,end_value)
        offsetof(lv_anim_t,duration) offsetof(lv_anim_t,act_time) offsetof(lv_anim_t,repeat_cnt) offsetof(lv_anim_t,last_timer_run)
    """.split()

    def test_an_lvgl_image_is_centred_in_its_box_until_told_otherwise(self):
        """The fake makes an image centred (a scaled image fills a box of the scaled size): lv_image_constructor says so."""
        text = _strip_comments((self.lvgl / "src/widgets/image/lv_image.c").read_text(errors="replace"))
        constructor = re.search(r"static void lv_image_constructor\([^)]*\)\s*\{.*?\n\}", text, re.S)
        self.assertIsNotNone(constructor, "lv_image_constructor isn't in LVGL 9.5.0's lv_image.c")
        self.assertRegex(constructor.group(0), r"img->align\s*=\s*LV_IMAGE_ALIGN_CENTER;")

    def test_the_fakes_rotation_and_animations_are_lvgl_9_5_0s(self):
        """What the fake's lv_image_set_rotation and its animations (tests/muse_apps_lv_harness.c) follow, in LVGL 9.5.0's own lines: an
        image's angle in 0.1 degrees brought into 0..3599 about a pivot at its centre by default; an animation's defaults, its copy at
        lv_anim_start with the start applied at once, its timer (the time held to the duration, the exec callback only for a new value,
        the repeat with its overtime kept), the deleted callback at every delete, an object's animations deleted with it, and the
        timer's period (the display's refresh period)."""
        def code(path):
            return _strip_comments((self.lvgl / path).read_text(errors="replace"))

        def definition(text, signature):  # the function's body, not a forward declaration of it: LVGL opens one on the next line
            return c_function(text, signature + "\n{")

        image = code("src/widgets/image/lv_image.c")
        constructor = re.search(r"static void lv_image_constructor\([^)]*\)\s*\{.*?\n\}", image, re.S).group(0)
        self.assertRegex(constructor, r"lv_point_set\(&img->pivot, LV_PCT\(50\), LV_PCT\(50\)\);")
        self.assertRegex(constructor, r"img->rotation\s*=\s*0;")
        rotation = definition(image, "void lv_image_set_rotation(lv_obj_t * obj, int32_t angle)")
        for line in ("while(angle >= 3600) angle -= 3600;", "while(angle < 0) angle += 3600;",
                     "if((uint32_t)angle == img->rotation) return;", "if(img->align > _LV_IMAGE_ALIGN_AUTO_TRANSFORM) {"):
            with self.subTest(line):
                self.assertIn(line, rotation)
        self.assertIn("return img->rotation;", definition(image, "int32_t lv_image_get_rotation(lv_obj_t * obj)"))

        anim = code("src/misc/lv_anim.c")
        init = definition(anim, "void lv_anim_init(lv_anim_t * a)")
        for line in ("a->duration = 500;", "a->start_value = 0;", "a->end_value = 100;", "a->repeat_cnt = 1;",
                     "a->path_cb = lv_anim_path_linear;", "a->early_apply = 1;"):
            with self.subTest(line):
                self.assertIn(line, init)
        self.assertIn("a->current_value = INT32_MIN;", definition(anim, "void lv_anim_set_values(lv_anim_t * a, int32_t start, int32_t end)"))
        start = " ".join(definition(anim, "lv_anim_t * lv_anim_start(const lv_anim_t * a)").split())
        for line in ("if(a->early_apply && (a->exec_cb || a->custom_exec_cb)) { remove_concurrent_anims(a); }",
                     "lv_anim_t * new_anim = lv_ll_ins_head(anim_ll_p);", "lv_memcpy(new_anim, a, sizeof(lv_anim_t));",
                     "new_anim->current_value = new_anim->path_cb(new_anim);",
                     "if(new_anim->exec_cb) { new_anim->exec_cb(new_anim->var, new_anim->current_value); }"):
            with self.subTest(line):
                self.assertIn(line, start)
        timer = " ".join(definition(anim, "static void anim_timer(lv_timer_t * param)").split())
        for line in ("a->act_time += elaps;", "if(a->act_time > a->duration) a->act_time = a->duration;",
                     "if(new_value != a->current_value) { a->current_value = new_value;",
                     "if(a->exec_cb) a->exec_cb(a->var, new_value);", "if(a->act_time >= a->duration) { anim_completed_handler(a); }",
                     "if(a->act_time == act_time_before_exec && act_time_original < a->duration * 2) {"):
            with self.subTest(line):
                self.assertIn(line, timer)
        completed = " ".join(definition(anim, "static void anim_completed_handler(lv_anim_t * a)").split())
        for line in ("a->repeat_cnt != LV_ANIM_REPEAT_INFINITE) { a->repeat_cnt--; }", "if(a->deleted_cb != NULL) a->deleted_cb(a);",
                     "if(a->act_time > a->duration) over_time = a->act_time - a->duration;",
                     "a->act_time = over_time - (int32_t)(a->repeat_delay);"):
            with self.subTest(line):
                self.assertIn(line, completed)
        self.assertIn("if(anim->deleted_cb != NULL) anim->deleted_cb(anim);", definition(anim, "static void remove_anim(void * a)"))
        delete = definition(anim, "bool lv_anim_delete(void * var, lv_anim_exec_xcb_t exec_cb)")
        self.assertIn("if((a->var == var || var == NULL) && (a->exec_cb == exec_cb || exec_cb == NULL)) {", delete)
        self.assertIn("remove_anim(a);", delete)
        self.assertIn("state.timer = lv_timer_create(anim_timer, LV_DEF_REFR_PERIOD, NULL);", anim)
        destructor = definition(code("src/core/lv_obj.c"), "static void lv_obj_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj)")
        self.assertIn("lv_anim_delete(obj, NULL);", destructor)
        # And the fake says the same (the harness's own lines).
        harness = HARNESS.read_text()
        for line in ("a->duration = 500;", "a->repeat_cnt = 1;", "a->early_apply = 1;", "a->current_value = INT32_MIN;",
                     "angle -= 3600;", "angle += 3600;", "if (new_value != a->current_value) {",
                     "a->act_time = over_time - (int32_t)a->repeat_delay;", "lv_anims_of_deleted(o);"):
            with self.subTest(fake=line):
                self.assertIn(line, harness)

    def test_the_fakes_spinner_is_lvgl_9_5_0s(self):
        """Task 8: what the fake's lv_spinner (tests/muse_apps_lv_harness.c) follows, in lv_spinner.c's own lines: the defaults the
        constructor passes (1,000 ms a turn, 200 degrees), the two animations
        lv_spinner_set_anim_params starts, the bezier path they use (lv_anim_path_custom_bezier3 over lv_cubic_bezier, with the
        constants of lv_math.c), the arc's start and end angles it moves, and an invalidated area that a hidden parent hides."""
        def code(path):
            return _strip_comments((self.lvgl / path).read_text(errors="replace"))

        def definition(text, signature):
            return " ".join(c_function(text, signature + "\n{").split())

        spinner = code("src/widgets/spinner/lv_spinner.c")
        flat = " ".join(spinner.split())
        for line in ("#define DEF_ARC_ANGLE 200", "#define DEF_TIME 1000"):
            with self.subTest(line):
                self.assertIn(line, flat)
        constructor = " ".join(re.search(r"static void lv_spinner_constructor\([^)]*\)\s*\{.*?\n\}", spinner, re.S).group(0).split())
        for line in ("lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);", "lv_spinner_set_anim_params(obj, DEF_TIME, DEF_ARC_ANGLE);"):
            with self.subTest(constructor=line):
                self.assertIn(line, constructor)
        params = definition(spinner, "void lv_spinner_set_anim_params(lv_obj_t * obj, uint32_t t, uint32_t angle)")
        ordered = ("spinner->duration = t;", "spinner->angle = angle;", "lv_anim_delete(obj, NULL);", "lv_anim_init(&a);",
                   "lv_anim_set_var(&a, obj);", "lv_anim_set_exec_cb(&a, arc_anim_end_angle);",
                   "lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);", "lv_anim_set_duration(&a, t);",
                   "lv_anim_set_values(&a, angle, 360 + angle);", "lv_anim_start(&a);",
                   "lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);",
                   "lv_anim_set_bezier3_param(&a, LV_BEZIER_VAL_FLOAT(0.42), LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_FLOAT(0), "
                   "LV_BEZIER_VAL_FLOAT(1));", "lv_anim_set_values(&a, 0, 360);", "lv_anim_set_exec_cb(&a, arc_anim_start_angle);",
                   "lv_anim_start(&a);", "lv_arc_set_bg_angles(obj, 0, 360);", "lv_arc_set_rotation(obj, 270);")
        at = -1
        for line in ordered:
            with self.subTest(params=line):
                found = params.find(line, at + 1)
                self.assertGreater(found, at, f"{line} is not in order in lv_spinner_set_anim_params")
                at = found
        self.assertIn("lv_arc_set_start_angle(obj, (uint32_t) v);", definition(spinner, "static void arc_anim_start_angle(void * obj, int32_t v)"))
        self.assertIn("lv_arc_set_end_angle(obj, (uint32_t) v);", definition(spinner, "static void arc_anim_end_angle(void * obj, int32_t v)"))
        self.assertIn("lv_spinner_class", spinner)
        self.assertIn(".base_class = &lv_arc_class,", flat)  # so every arc call is good on a spinner

        anim = code("src/misc/lv_anim.c")
        self.assertIn("return lv_anim_path_cubic_bezier(a, para->x1, para->y1, para->x2, para->y2);",
                      definition(anim, "int32_t lv_anim_path_custom_bezier3(const lv_anim_t * a)"))
        cubic = definition(anim, "static int32_t lv_anim_path_cubic_bezier(const lv_anim_t * a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)")
        for line in ("uint32_t t = lv_map(a->act_time, 0, a->duration, 0, LV_BEZIER_VAL_MAX);",
                     "int32_t step = lv_cubic_bezier(t, x1, y1, x2, y2);", "new_value = step * (a->end_value - a->start_value);",
                     "new_value = new_value >> LV_BEZIER_VAL_SHIFT;", "new_value += a->start_value;"):
            with self.subTest(cubic=line):
                self.assertIn(line, cubic)
        self.assertIn("lv_anim_set_bezier3_param(lv_anim_t * a, int16_t x1, int16_t y1, int16_t x2, int16_t y2)", " ".join(anim.split()))
        math = code("src/misc/lv_math.c")
        flat_math = " ".join(math.split())
        self.assertIn("#define CUBIC_NEWTON_ITERATIONS 8", flat_math)
        self.assertIn("#define CUBIC_PRECISION_BITS 10", flat_math)
        bezier = definition(math, "int32_t lv_cubic_bezier(int32_t x, int32_t x1, int32_t y1, int32_t x2, int32_t y2)")
        for line in ("if(x == 0 || x == LV_BEZIER_VAL_MAX) return x;", "cx = 3 * x1;", "bx = 3 * (x2 - x1) - cx;",
                     "ax = (1L << CUBIC_PRECISION_BITS) - cx - bx;", "cy = 3 * y1;", "by = 3 * (y2 - y1) - cy;",
                     "ay = (1L << CUBIC_PRECISION_BITS) - cy - by;", "xs = do_cubic_bezier(t, ax, bx, cx) - x;",
                     "d = ((int64_t)xs * (1L << CUBIC_PRECISION_BITS)) / d;", "tl = 0, tr = 1L << CUBIC_PRECISION_BITS, t = x;",
                     "x > xs ? (tl = t) : (tr = t);", "t = (tr - tl) / 2 + tl;"):
            with self.subTest(bezier=line):
                self.assertIn(line, bezier)
        self.assertIn("ret = ((ret + c) * t) >> CUBIC_PRECISION_BITS;", definition(math, "static int32_t do_cubic_bezier(int32_t t, int32_t a, int32_t b, int32_t c)"))

        arc = code("src/widgets/arc/lv_arc.c")
        for name, var in (("start", "start"), ("end", "end")):
            body = definition(arc, f"void lv_arc_set_{name}_angle(lv_obj_t * obj, lv_value_precise_t {var})")
            with self.subTest(arc=name):
                self.assertIn(f"if({var} > 360) {var} -= 360;", body)
                self.assertIn(f"arc->indic_angle_{name} = {var};", body)
                self.assertIn("else if(new_delta < old_delta) inv_arc_area(", body)
        self.assertIn("if(lv_obj_is_visible(obj) == false) return;", definition(arc, "static void inv_arc_area(lv_obj_t * obj, lv_value_precise_t start_angle, lv_value_precise_t end_angle, lv_part_t part)"))
        visible = definition(code("src/core/lv_obj_pos.c"), "bool lv_obj_area_is_visible(const lv_obj_t * obj, lv_area_t * area)")
        self.assertIn("if(lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return false;", visible)  # the child of a hidden parent is hidden
        harness = HARNESS.read_text()
        for line in ("lv_spinner_set_anim_params(o, 1000, 200);", "lv_anim_delete(obj, NULL);",
                     "lv_anim_set_values(&a, (int32_t)angle, 360 + (int32_t)angle);", "lv_anim_set_values(&a, 0, 360);",
                     "LV_BEZIER_VAL_FLOAT(0.42), LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_FLOAT(0), LV_BEZIER_VAL_FLOAT(1)",
                     "lv_arc_set_bg_angles(obj, 0, 360);", "lv_arc_set_rotation(obj, 270);", "#define FAKE_CUBIC_NEWTON_ITERATIONS 8",
                     "#define FAKE_CUBIC_PRECISION_BITS 10", "o->flags &= ~(uint32_t)LV_OBJ_FLAG_CLICKABLE;",
                     "if (angle > 360) {\n        angle -= 360;", "up = up->parent"):
            with self.subTest(fake=line):
                self.assertIn(line, harness)

    def test_the_fakes_turned_room_and_invalidation_are_lvgl_9_5_0s(self):
        """Task 7: what the fake's turned images invalidate (tests/muse_apps_lv_harness.c), in LVGL 9.5.0's own lines: the turned
        box (the object's size turned and scaled about the pivot, the right and bottom less 1), an image's room for it
        (LV_EVENT_REFR_EXT_DRAW_SIZE from 0, the class's handler before the object's callbacks), lv_image_set_rotation's and
        scale_update's invalidations of the old and new turned areas with the room sent for between them, and an invalidated
        area cut to the box grown by the room."""
        def code(path):
            return _strip_comments((self.lvgl / path).read_text(errors="replace"))

        def definition(text, signature):
            return " ".join(c_function(text, signature + "\n{").split())

        image = code("src/widgets/image/lv_image.c")
        event = " ".join(re.search(r"else if\(code == LV_EVENT_REFR_EXT_DRAW_SIZE\) \{.*?\n    \}", image, re.S).group(0).split())
        for line in ("if(img->rotation || img->scale_x != LV_SCALE_NONE || img->scale_y != LV_SCALE_NONE) {",
                     "int32_t w = lv_obj_get_width(obj);",
                     "lv_image_buf_get_transformed_area(&a, w, h, img->rotation, img->scale_x, img->scale_y, &pivot_px);",
                     "*s = LV_MAX(*s, -a.x1);", "*s = LV_MAX(*s, -a.y1);", "*s = LV_MAX(*s, a.x2 - w);", "*s = LV_MAX(*s, a.y2 - h);"):
            with self.subTest(line):
                self.assertIn(line, event)
        rotation = definition(image, "void lv_image_set_rotation(lv_obj_t * obj, int32_t angle)")
        steps = ["lv_obj_update_layout(obj);", "lv_obj_invalidate_area(obj, &a);", "img->rotation = angle;",
                 "lv_display_enable_invalidation(disp, false);", "lv_obj_refresh_ext_draw_size(obj);",
                 "lv_display_enable_invalidation(disp, true);", "lv_obj_invalidate_area(obj, &a);"]
        at = 0
        for line in steps:
            with self.subTest(rotation=line):
                at = rotation.index(line, at) + len(line)
        scale = definition(image, "static void scale_update(lv_obj_t * obj, int32_t scale_x, int32_t scale_y)")
        for line in ("lv_obj_invalidate_area(obj, &a);", "img->scale_x = scale_x;", "lv_obj_refresh_ext_draw_size(obj);"):
            with self.subTest(scale=line):
                self.assertIn(line, scale)
        self.assertIn("if(zoom == img->scale_x && zoom == img->scale_y) return;", definition(image, "void lv_image_set_scale(lv_obj_t * obj, uint32_t zoom)"))
        transformed = " ".join(re.search(r"void lv_image_buf_get_transformed_area\(.*?\n\}", code("src/draw/lv_draw_image.c"),
                                         re.S).group(0).split())
        for line in ("lv_point_transform(&p[0], angle, scale_x, scale_y, pivot, true);",
                     "res->x2 = LV_MAX4(p[0].x, p[1].x, p[2].x, p[3].x) - 1;", "res->y1 = LV_MIN4(p[0].y, p[1].y, p[2].y, p[3].y);"):
            with self.subTest(transformed=line):
                self.assertIn(line, transformed)
        self.assertRegex(re.search(r"static void lv_image_constructor\([^)]*\)\s*\{.*?\n\}", image, re.S).group(0),
                         r"lv_point_set\(&img->pivot, LV_PCT\(50\), LV_PCT\(50\)\);")
        draw = code("src/core/lv_obj_draw.c")
        refresh = definition(draw, "void lv_obj_refresh_ext_draw_size(lv_obj_t * obj)")
        for line in ("int32_t s_new = 0;", "lv_obj_send_event(obj, LV_EVENT_REFR_EXT_DRAW_SIZE, &s_new);",
                     "if(s_new != s_old) lv_obj_invalidate(obj);"):
            with self.subTest(refresh=line):
                self.assertIn(line, refresh)
        send = definition(code("src/core/lv_obj_event.c"), "static lv_result_t event_send_core(lv_event_t * e)")
        self.assertLess(send.index("res = lv_event_send(list, e, true);"), send.index("res = lv_obj_event_base(NULL, e);"))
        self.assertLess(send.index("res = lv_obj_event_base(NULL, e);"), send.index("res = lv_event_send(list, e, false);"))
        visible = definition(code("src/core/lv_obj_pos.c"), "bool lv_obj_area_is_visible(const lv_obj_t * obj, lv_area_t * area)")
        for line in ("if(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;", "int32_t ext_size = lv_obj_get_ext_draw_size(obj);",
                     "lv_area_increase(&obj_coords, ext_size, ext_size);", "if(!lv_area_intersect(area, area, &obj_coords)) return false;"):
            with self.subTest(visible=line):
                self.assertIn(line, visible)
        self.assertIn("lv_obj_refresh_ext_draw_size(obj);", definition(code("src/core/lv_obj_pos.c"), "bool lv_obj_refr_size(lv_obj_t * obj)"))
        redraw = definition(code("src/core/lv_refr.c"), "void lv_obj_redraw(lv_layer_t * layer, lv_obj_t * obj)")
        self.assertIn("lv_area_increase(&obj_coords_ext, ext_draw_size, ext_draw_size);", redraw)  # and the draw is cut there too
        harness = HARNESS.read_text()
        for line in ("double px = w * 50 / 100, py = h * 50 / 100", "(int32_t)floor(x_hi) - 1", "lv_event_t e = {LV_EVENT_REFR_EXT_DRAW_SIZE, NULL, o, false, &s};",
                     "if (changed) {", "invalidate_turned(obj);\n    obj->rotation = angle;\n    obj->ext_draw = ext_of(obj);\n    invalidate_turned(obj);",
                     "if (zoom == obj->scale) {", "o->ext_draw = ext_of(o);  /* lv_obj_refr_size"):
            with self.subTest(fake=line):
                self.assertIn(line, harness)

    def test_the_fakes_pressed_state_is_lvgl_9_5_0s(self):
        """What the fake's state (fake_set_state, fake_style) follows, in LVGL 9.5.0's own lines: the base class's handler gives an
        object the pressed state at PRESSED and takes it at RELEASED and PRESS_LOST, a drag that starts to scroll takes it too; a
        state change that a style tells apart redraws the object, and its children with it; a property's value is that of the
        highest state the object is in; lv_obj_remove_style_all removes every part's and state's styles; and an unset background
        is white and transparent."""
        def code(path):
            return _strip_comments((self.lvgl / path).read_text(errors="replace"))

        def squeeze(text):
            return re.sub(r"\s+", " ", text)

        obj = code("src/core/lv_obj.c")
        event = squeeze(c_function(obj, "static void lv_obj_event(const lv_obj_class_t * class_p, lv_event_t * e)\n{"))
        for line in ("if(code == LV_EVENT_PRESSED) { lv_obj_add_state(obj, LV_STATE_PRESSED); }",
                     "else if(code == LV_EVENT_RELEASED) { lv_obj_remove_state(obj, LV_STATE_PRESSED);",
                     "else if(code == LV_EVENT_PRESS_LOST) { lv_obj_remove_state(obj, LV_STATE_PRESSED); }"):
            with self.subTest(lv_obj_event=line):
                self.assertIn(line, event)
        update = squeeze(c_function(obj, "static void update_obj_state(lv_obj_t * obj, lv_state_t new_state)\n{"))
        for line in ("if(cmp_res == LV_STYLE_STATE_CMP_SAME) { obj->state = new_state;", "lv_obj_invalidate(obj); obj->state = new_state;",
                     "if(cmp_res == LV_STYLE_STATE_CMP_DIFF_REDRAW) {", "lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);"):
            with self.subTest(update_obj_state=line):
                self.assertIn(line, update)
        scroll = squeeze(code("src/indev/lv_indev_scroll.c"))
        self.assertIn("lv_obj_remove_state(indev->pointer.act_obj, LV_STATE_PRESSED); "
                      "lv_obj_send_event(scroll_obj, LV_EVENT_SCROLL_BEGIN, NULL);", scroll)
        style = code("src/core/lv_obj_style.c")
        core = squeeze(c_function(style, "static lv_style_res_t get_prop_core(const lv_obj_t * obj, lv_style_selector_t selector, "
                                         "lv_style_prop_t prop,\n                                    lv_style_value_t * v)\n{"))
        for line in ("if((state_act & state_inv)) continue;", "if((int32_t)state_act <= weight) continue;", "weight = state_act;"):
            with self.subTest(get_prop_core=line):
                self.assertIn(line, core)
        compare = squeeze(c_function(style, "lv_style_state_cmp_t lv_obj_style_state_compare(lv_obj_t * obj, lv_state_t state1, "
                                            "lv_state_t state2)\n{"))
        self.assertIn("bool valid1 = state_act & (~state1) ? false : true; bool valid2 = state_act & (~state2) ? false : true; "
                      "if(valid1 != valid2) {", compare)
        refresh = squeeze(c_function(style, "static void refresh_children_style(lv_obj_t * obj)\n{"))
        self.assertIn("lv_obj_invalidate(child);", refresh)
        self.assertIn("lv_obj_remove_style(obj, NULL, LV_PART_ANY | LV_STATE_ANY);",
                      c_function(style, "void lv_obj_remove_style_all(lv_obj_t * obj)\n{"))
        default = squeeze(c_function(code("src/misc/lv_style.c"), "lv_style_value_t lv_style_prop_get_default(lv_style_prop_t prop)\n{"))
        self.assertIn("case LV_STYLE_BG_COLOR: return (lv_style_value_t) { .color = white };", default)
        self.assertNotIn("case LV_STYLE_BG_OPA:", default)  # so the default's: 0, transparent
        self.assertIn("default: return (lv_style_value_t) { .ptr = NULL };", default)
        harness = HARNESS.read_text()
        for line in ("fake_set_state(o, (lv_state_t)(o->state | LV_STATE_PRESSED));",
                     "} else if (!o->deleted && (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {",
                     "fake_set_state(pressed_obj, (lv_state_t)(pressed_obj->state & ~LV_STATE_PRESSED));  /* the scroll's, not a press */\n"
                     "        fake_fire(o->id, LV_EVENT_SCROLL_BEGIN);",
                     "if (differs) {\n        invalidate_tree(o);",
                     "(state & ~(uint32_t)o->state) == 0 &&\n            (int32_t)state > weight)",
                     "obj->n_styles = 0;  /* every part's and state's, the local ones too */",
                     'return "#ffffff";', 'return "0";'):
            with self.subTest(fake=line):
                self.assertIn(line, harness)

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

    def test_the_fakes_pointer_is_lvgl_9_5_0s(self):
        """The fake's touch (fake_down, fake_move, fake_lift, fake_frame) uses LVGL 9.5.0's numbers and the defaults it gives an object:
        the drag, throw and gesture limits, the elastic slowness, the scroll animation's times and speed, and the flags lv_obj_constructor
        sets (press lock, scroll chain and gesture bubble on an object with a parent; elastic and momentum on every one), which the fake
        takes as given (apps_lv.c can't change them: the fake declares none of them)."""
        harness = HARNESS.read_text()

        def fake(name):
            match = re.search(rf"^#define {name} (\d+)\b", harness, re.M)
            self.assertIsNotNone(match, f"no #define {name} in the harness")
            return int(match.group(1))

        def real(path, name):
            match = re.search(rf"^\s*#define\s+{name}\s+(\d+)\b", (self.lvgl / path).read_text(errors="replace"), re.M)
            self.assertIsNotNone(match, f"no #define {name} in {path}")
            return int(match.group(1))

        for fake_name, path, name in (("FAKE_SCROLL_LIMIT", "src/indev/lv_indev.c", "LV_INDEV_DEF_SCROLL_LIMIT"),
                                      ("FAKE_SCROLL_THROW", "src/indev/lv_indev.c", "LV_INDEV_DEF_SCROLL_THROW"),
                                      ("FAKE_GESTURE_LIMIT", "src/indev/lv_indev.c", "LV_INDEV_DEF_GESTURE_LIMIT"),
                                      ("FAKE_GESTURE_MIN_VELOCITY", "src/indev/lv_indev.c", "LV_INDEV_DEF_GESTURE_MIN_VELOCITY"),
                                      ("FAKE_ELASTIC_SLOWNESS", "src/indev/lv_indev_scroll.c", "ELASTIC_SLOWNESS_FACTOR"),
                                      ("FAKE_SCROLL_ANIM_MIN_MS", "src/core/lv_obj_scroll.c", "SCROLL_ANIM_TIME_MIN"),
                                      ("FAKE_SCROLL_ANIM_MAX_MS", "src/core/lv_obj_scroll.c", "SCROLL_ANIM_TIME_MAX")):
            with self.subTest(name):
                self.assertEqual(fake(fake_name), real(path, name))
        scroll = _strip_comments((self.lvgl / "src/core/lv_obj_scroll.c").read_text(errors="replace"))
        self.assertRegex(scroll, r"lv_anim_speed_clamped\(\(lv_display_get_horizontal_resolution\(d\)\) >> 1, SCROLL_ANIM_TIME_MIN,")
        anim = _strip_comments((self.lvgl / "src/misc/lv_anim.c").read_text(errors="replace"))
        self.assertIn("speed = (speed + 5) / 10;", anim)
        self.assertIn("uint32_t time = (d * 100) / speed;", anim)
        self.assertIn("int32_t speed = ((FAKE_HOR_RES >> 1) + 5) / 10;", harness)
        self.assertEqual(fake("FAKE_HOR_RES"), 466)
        obj = _strip_comments((self.lvgl / "src/core/lv_obj.c").read_text(errors="replace"))
        constructor = re.search(r"static void lv_obj_constructor\([^)]*\)\s*\{.*?\n\}", obj, re.S)
        self.assertIsNotNone(constructor, "lv_obj_constructor isn't in LVGL 9.5.0's lv_obj.c")
        for line in ("if(parent) obj->flags |= LV_OBJ_FLAG_PRESS_LOCK;", "if(parent) obj->flags |= LV_OBJ_FLAG_SCROLL_CHAIN;",
                     "if(parent) obj->flags |= LV_OBJ_FLAG_GESTURE_BUBBLE;", "obj->flags |= LV_OBJ_FLAG_SCROLL_ELASTIC;",
                     "obj->flags |= LV_OBJ_FLAG_SCROLL_MOMENTUM;"):
            with self.subTest(line):
                self.assertIn(line, constructor.group(0))

    def test_apps_lv_compiles_against_the_real_headers(self):
        """The device build compiles apps_lv.c against these: every name, type and argument it uses is checked, a bench build's
        (CONFIG_MUSE_BENCH: the frame log) too."""
        for bench in (False, True):
            with self.subTest(bench=bench), tempfile.TemporaryDirectory() as tmp:
                stage = Path(tmp)
                # The ESP-IDF headers the real LVGL doesn't bring: just what apps_lv.c uses of them.
                (stage / "sdkconfig.h").write_text("#pragma once\n" + ("#define CONFIG_MUSE_BENCH 1\n" if bench else ""))
                (stage / "esp_attr.h").write_text("#pragma once\n#define EXT_RAM_BSS_ATTR\n")
                (stage / "esp_heap_caps.h").write_text(
                    "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_SPIRAM (1u << 10)\n"
                    "size_t heap_caps_get_free_size(uint32_t caps);\n")
                (stage / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
                (stage / "esp_log.h").write_text(
                    "#pragma once\nvoid esp_log_write(int level, const char *tag, const char *fmt, ...) "
                    "__attribute__((format(printf, 3, 4)));\n#define ESP_LOGW(tag, ...) esp_log_write(1, tag, __VA_ARGS__)\n"
                    "#define ESP_LOGI(tag, ...) esp_log_write(2, tag, __VA_ARGS__)\n")
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
