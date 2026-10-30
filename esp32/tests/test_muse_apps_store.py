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

"""The store of Muse apps (apps_store.c): the FAT partition's folders, apps_nvs, the RAM mode, the quarantine and the alarms'
records.

tests/muse_apps_store_harness.c runs the real apps_store.c over the host's own files, as the brief's style (a): built with
-DAPPS_STORE_BASE="apps_fs", it changes into a fresh directory each test makes, where apps_fs is the partition's file system.
Everything else of the gadget is a fake defined in the harness: the partition table, the FAT mount (which "formats" by making
the directory), NVS as a map for each partition (it fails a case that touches the pairing's "nvs"), the reset reason, the clocks,
the RAM store, the alarms and the I/O worker. The headers they need are tests/muse_apps_store_fakes/ and tests/link_fakes/.
The partition table and the overlay are read here as text.
"""

import csv
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_store_harness.c"
SOURCES = [HARNESS, APPS / "apps_store.c", APPS / "apps_pkg.c", APPS / "apps_json.c", APPS / "apps_vm.c",
           APPS / "apps_suggest.c", ROOT / "tools/muse/apphost/sha256.c"]
INCLUDES = [TESTS / "muse_apps_store_fakes", TESTS / "link_fakes", APPS, apps_host.LUA_SRC, TESTS]

# The reset reasons that count toward the quarantine (the controller's ruling): a crash of some kind. Every other reason clears the
# marker before it is read.
CRASH_RESETS = {"ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT", "ESP_RST_WDT", "ESP_RST_BROWNOUT", "ESP_RST_CPU_LOCKUP"}


def reset_reasons():
    """The names of esp_reset_reason_t in the order of tests/link_fakes/esp_system.h (IDF v6.0.1's), so that index i is its value."""
    text = (TESTS / "link_fakes/esp_system.h").read_text()
    body = re.search(r"typedef enum \{(.*?)\} esp_reset_reason_t;", text, re.S).group(1)
    names = re.findall(r"\b(ESP_RST_[A-Z_]+)\b", body)
    assert names[0] == "ESP_RST_UNKNOWN" and "= 0" in body.split(",")[0], body
    return names


class Store(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = apps_host.build("muse_apps_store_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=INCLUDES, defines=['APPS_STORE_BASE="apps_fs"'])

    def run_case(self, case, *args):
        """Runs one case in a directory of its own; returns its stdout."""
        with tempfile.TemporaryDirectory() as fs:
            try:
                done = apps_host.run(self.harness, case, fs, *args, timeout=120)
            except subprocess.TimeoutExpired:
                self.fail(f"{case} took more than 120 s")
        self.assertEqual(done.returncode, 0, f"{case}:\n{done.stdout[-3000:]}{done.stderr[-8000:]}")
        return done.stdout

    # ---- The brief's ----

    def test_commit_creates_and_replaces(self):
        self.run_case("commit_creates_and_replaces")

    def test_recovery_restores_old_when_app_missing(self):
        self.run_case("recovery_restores_old_when_app_missing")

    def test_recovery_deletes_stray_stage(self):  # the stage is the app's sibling, .stage-<id> (P4)
        self.run_case("recovery_deletes_stray_stage")

    def test_the_old_layouts_stage_is_deleted(self):  # /apps/.stage/<id>, from before P4: the board has one
        self.run_case("old_layout_stage_is_deleted")

    def test_remove(self):  # the folder and the app's s, a and q keys; refused for the app that runs (D18)
        self.run_case("remove")

    def test_list_reads_names(self):
        self.run_case("list_reads_names")

    def test_unchanged_skips_commit(self):
        self.run_case("unchanged_skips_commit")

    def test_values_roundtrip(self):  # s%08x in the "apps" namespace of apps_nvs (D18)
        self.run_case("values_roundtrip")

    def test_values_that_cannot_be_read_are_not_an_empty_store(self):  # D: NULL, so the launch is refused; only a missing key is {}
        self.run_case("values_that_cannot_be_read_are_not_empty")

    def test_ids_whose_keys_collide_keep_their_keys_apart(self):  # E: timer and zleca55_, both 0x6ad0de1a
        self.run_case("keys_that_collide_stay_apart")

    def test_full_nvs_erases_only_apps_nvs(self):
        self.run_case("full_nvs_erases_only_apps_nvs")

    def test_ram_mode_when_partition_missing(self):
        self.run_case("ram_mode_when_partition_missing")

    def test_marker_quarantines_after_two_resets(self):  # and D19: the same id keeps the count, another resets it
        self.run_case("marker_quarantines_after_two_resets")

    # ---- The rulings' ----

    def test_new_nvs_version_erases_only_apps_nvs(self):
        self.run_case("new_nvs_version_erases_only_apps_nvs")

    def test_list_before_mount_is_empty(self):  # D6, E25: nothing listed and nothing loaded until the mount job has run
        self.run_case("list_before_mount_is_empty")

    def test_a_refused_mount_is_asked_again_later(self):  # every 2 s
        self.run_case("a_refused_mount_is_asked_again_later")

    def test_only_a_crash_reset_counts_toward_the_quarantine(self):
        reasons = reset_reasons()
        self.assertTrue(CRASH_RESETS <= set(reasons), "IDF v6.0.1 has every reason of the ruling, CPU lockup too")
        for value, name in enumerate(reasons):
            with self.subTest(name):
                expected = "quarantined" if name in CRASH_RESETS else "cleared"
                self.assertEqual(self.run_case("reset_reason_counts", value).split(), [expected])

    def test_quarantine_refuses_until_reinstalled(self):  # and drops the app's alarms, stored and armed; an unchanged push clears it too
        self.run_case("quarantine_refuses_until_reinstalled")

    def test_the_boot_line_says_the_markers_count(self):  # J: as read, before this boot's reset is counted or cleared
        self.run_case("boot_line_says_the_count")

    def test_the_records_are_handed_to_the_app_task_at_the_mount(self):  # P1, and AA: times outside 2025 to 2100 go
        self.run_case("records_are_handed_over_at_the_mount")

    def test_a_record_that_cannot_be_read_now_is_kept(self):  # S: only one that was read and is wrong goes
        self.run_case("a_record_that_cannot_be_read_now_is_kept")

    def test_records_survive_a_list_that_failed(self):  # S: no "not installed" drop without the list
        self.run_case("records_survive_a_list_that_failed")

    def test_alarm_records_round_trip(self):
        self.run_case("alarm_records_round_trip")

    def test_a_sync_follows_the_touched_names_and_keeps_the_rest(self):  # P1
        self.run_case("sync_alarms_keeps_untouched_names")

    def test_first_mount_formats(self):  # the brief's config, and the log line with the time, the room and apps_nvs's use
        self.run_case("first_mount_formats")

    def test_a_second_mount_is_not_new(self):
        self.run_case("a_second_mount_is_not_new")

    def test_an_unmountable_partition_is_ram_mode(self):
        self.run_case("an_unmountable_partition_is_ram_mode")

    def test_a_commit_that_cant_be_written_leaves_the_app(self):
        self.run_case("a_commit_that_cant_be_written_leaves_the_app")

    def test_a_damaged_copy_is_not_loaded(self):
        self.run_case("a_damaged_copy_is_not_loaded")

    def test_out_of_memory_reading_the_manifest_is_not_damage(self):  # T
        self.run_case("out_of_memory_reading_the_manifest")

    def test_the_launchers_order_is_the_stores(self):  # P2: installs and launches, across a remount with no clock
        self.run_case("order_follows_installs_and_launches")

    def test_the_orders_write_waits_for_the_budget(self):  # P3
        self.run_case("order_waits_for_the_budget")

    def test_every_case_of_the_harness_is_run_here(self):
        """X: the cases the harness has and the run_case calls of this file are the same set, both ways."""
        done = apps_host.run(self.harness, "--cases")  # takes no directory
        self.assertEqual(done.returncode, 0, done.stderr)
        called = set(re.findall(r'self\.run_case\("([a-z_]+)"', Path(__file__).read_text()))
        cases = set(done.stdout.split())
        self.assertEqual(cases - called, set(), "a case of the harness that no test runs")
        self.assertEqual(called - cases, set(), "a test that runs a case the harness doesn't have")


class Partitions(unittest.TestCase):
    """The table and the overlay: partitions_muse.csv as it is, then the two entries of the brief, and the pairing's nvs where it was."""

    @staticmethod
    def rows(path):
        lines = [line for line in path.read_text().splitlines() if line.strip() and not line.lstrip().startswith("#")]
        return [[cell.strip() for cell in row][:5] for row in csv.reader(lines)]

    def test_the_apps_table_is_the_muse_table_and_two_entries(self):
        base = self.rows(ROOT / "partitions_muse.csv")
        apps = self.rows(ROOT / "partitions_muse_apps.csv")
        self.assertEqual(apps[:len(base)], base, "no existing entry moves or changes")
        self.assertEqual(apps[len(base):], [["apps", "data", "fat", "0x830000", "0x600000"],
                                            ["apps_nvs", "data", "nvs", "0xE30000", "0x10000"]])
        self.assertIn(["nvs", "data", "nvs", "0x11000", "0xC000"], apps, "the pairing's NVS stays where it is")

    def test_the_new_entries_fit_the_flash_and_overlap_nothing(self):
        def size(text):
            return int(text[:-1]) << 20 if text.endswith("M") else int(text, 0)

        spans = sorted((int(row[3], 16), int(row[3], 16) + size(row[4]), row[0])
                       for row in self.rows(ROOT / "partitions_muse_apps.csv"))
        for (start, end, name), (next_start, _, next_name) in zip(spans, spans[1:]):
            self.assertLessEqual(end, next_start, f"{name} overlaps {next_name}")
        self.assertLessEqual(spans[-1][1], 16 << 20, "inside 16 MB")

    def test_the_header_comment_is_adapted(self):
        text = (ROOT / "partitions_muse_apps.csv").read_text()
        self.assertTrue(text.startswith("# Copyright (c) Meta Platforms, Inc. and affiliates."))
        self.assertIn("Muse apps", text)
        self.assertNotIn("Hatch", text)

    def test_the_overlay_picks_the_table(self):  # its other lines are test_muse_apps_wiring.py's (the ruling's place for them)
        overlay = (ROOT / "devices/sdkconfig.muse-apps").read_text().splitlines()
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_muse_apps.csv"', overlay)

    def test_apps_store_is_in_the_device_build(self):  # X6
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        block = re.search(r"if\(CONFIG_MUSE_APPS\)(.*?)endif\(\)", cmake, re.S).group(1)
        self.assertIn('"apps/apps_store.c"', block)
        self.assertIn('"apps/apps_values.c"', block)
        self.assertNotIn("apps_store_ram.c", block)


if __name__ == "__main__":
    unittest.main()
