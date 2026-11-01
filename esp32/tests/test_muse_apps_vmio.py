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

"""The apps' `vm` module (apps_vmio.c) and the network worker (apps_net.c): connected apps Task 3.

tests/muse_apps_vmio_harness.c runs the real module over the real runtime (apps_rt, apps_svc, apps_ui, apps_vm, Lua) and the
real worker, a thread of its own made by the fake FreeRTOS there, which calls the VM ops (muse_apps_vm_ops_t) of a scriptable
in-memory VM folder: files, a reader that is busy or held, a session that drops, a write the VM refuses. The JPEG decode is the
real apps_jpeg.c over the fake TJpgDec (tests/fake_tjpgd.h). The case's own thread plays the app task: each round it takes the
worker's results (a result whose app has ended is freed and never runs, as apps_task.c does), ticks the module with "in front and
lit" as the case says, and runs one callback. The clock is the harness's, and moves only when the case moves it. The heap counts
every byte (every case ends at 0), and the harness runs under ASan and UBSan where the compiler can.
tests/test_muse_apps_task.py runs the same module on the real app task: the event, the pause behind or dark, the drop by
generation and the images' release at an app's end.
"""

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_rt import AppHostTestCase, header_number
from test_muse_apps_wiring import c_function, call_arguments, strip_c_comments

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
APPS = MUSE / "apps"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_vmio_harness.c"
SOURCES = [HARNESS, TESTS / "fake_tjpgd.c", APPS / "apps_vmio.c", APPS / "apps_net.c", APPS / "apps_jpeg.c", APPS / "apps_rt.c",
           APPS / "apps_svc.c", APPS / "apps_ui.c", APPS / "apps_vm.c", APPS / "apps_json.c", APPS / "apps_pkg.c",
           APPS / "apps_suggest.c"]
# What apps_net.c and muse_apps.h include of FreeRTOS, ESP-IDF and LVGL: each one a line that includes the task harness's fakes.
FAKE_HEADERS = ["freertos/FreeRTOS.h", "freertos/idf_additions.h", "freertos/queue.h", "freertos/semphr.h", "freertos/task.h",
                "esp_attr.h", "esp_err.h", "esp_heap_caps.h", "esp_log.h", "esp_timer.h", "lvgl.h"]


class VmioHarness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        stage = Path(tmp.name) / "fake_headers"
        for name in FAKE_HEADERS:
            (stage / name).parent.mkdir(parents=True, exist_ok=True)
            (stage / name).write_text('#include "muse_apps_task_fakes.h"\n')
        (stage / "rom").mkdir(parents=True, exist_ok=True)
        (stage / "rom/tjpgd.h").write_text('#include "fake_tjpgd.h"\n')
        (stage / "sdkconfig.h").write_text("#define CONFIG_MUSE_APPS 1\n#define CONFIG_SPIRAM 1\n")
        cls.harness = apps_host.build("muse_apps_vmio_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=[stage, TESTS, APPS, MUSE, apps_host.LUA_SRC])

    def run_case(self, case, timeout=120):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-12000:])
        return done

    # ---- The brief's ----

    def test_read_json_text_jpg_bin(self):  # each kind, and the size limits
        self.run_case("read_json_text_jpg_bin")

    def test_paths_are_confined(self):  # .., absolute and dotted parts refused at the call, and no request made
        self.run_case("paths_are_confined")

    def test_write_only_outbox(self):
        self.run_case("write_only_outbox")

    def test_command_acks_and_times_out(self):  # an ack after 3 reads, and never
        self.run_case("command_acks_and_times_out")

    def test_command_seq_increases_across_restarts(self):  # an epoch base
        self.run_case("command_seq_increases_across_restarts")

    def test_watch_fires_first_and_on_change_only(self):  # P15: and an error once for each change of outcome
        self.run_case("watch_fires_first_and_on_change_only")

    def test_watch_pauses_when_behind_or_dark(self):  # Review Focus 4: no requests while behind or dark, and the heartbeat stops
        self.run_case("watch_pauses_when_behind_or_dark")

    def test_heartbeat_every_10_s_while_watching(self):
        self.run_case("heartbeat_every_10_s_while_watching")

    def test_one_request_in_flight_and_queue_of_8(self):  # P11: the worker's polls take no place among the 8
        self.run_case("one_request_in_flight_and_queue_of_8")

    def test_own_requests_are_never_starved_by_polls(self):  # R3a: turns alternate, on a clock that moves during the reads
        self.run_case("own_requests_are_never_starved_by_polls")

    def test_busy_reader_retries_then_fails(self):
        self.run_case("busy_reader_retries_then_fails")

    def test_vm_session_drop_mid_read(self):  # Review Focus 2
        self.run_case("vm_session_drop_mid_read")

    def test_vm_result_after_the_app_ended_is_dropped(self):  # Review Focus 1: freed, never run, under ASan
        self.run_case("vm_result_after_the_app_ended_is_dropped")

    def test_callback_error_crashes_like_a_timer(self):
        self.run_case("callback_error_crashes_like_a_timer")

    # ---- The rulings' and the module's own ----

    def test_vm_needs_its_permission_and_sys_has_it(self):
        self.run_case("vm_needs_its_permission")

    def test_a_request_times_out_after_10_s(self):  # APPS_VM_REQUEST_MS, the worker's deadline
        self.run_case("a_request_times_out_after_10_s")

    def test_a_refused_buffer_is_a_clean_error(self):  # the floor refuses the worker's buffer or the decode's block
        self.run_case("a_refused_buffer_is_a_clean_error")

    def test_a_stop_releases_every_callback(self):  # apps_rt's stop: no callback runs after it, and no poll is made
        self.run_case("a_stop_releases_every_callback")

    def test_no_vm_ops_is_not_connected(self):  # a gadget without the VM's ops: every request says so, through its callback
        self.run_case("no_vm_ops_is_not_connected")

    def test_the_watch_handle_is_protected(self):  # X17
        self.run_case("the_watch_handle_is_protected")

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases, "no cases found in the harness")
        self.assertEqual(sorted(cases - tested), [], "harness cases no test runs")
        self.assertEqual(sorted(tested - cases), [], "tests that run a case the harness doesn't have")


class VmioStatic(unittest.TestCase):
    def test_the_worker_is_its_own_task_on_core_0(self):
        # "muse_apps_net": an 8 KB stack in PSRAM, priority 3 (with the I/O worker, over the app task), core 0, and a queue of 8 jobs.
        # It is neither the I/O worker nor the Noise session's task: a stalled VM read holds only it (the brief).
        net = strip_c_comments((APPS / "apps_net.c").read_text())
        args = call_arguments(net, "xTaskCreatePinnedToCoreWithCaps")
        self.assertEqual(len(args), 8, args)
        self.assertEqual(args[1], '"muse_apps_net"')
        self.assertEqual(args[2], "APPS_NET_STACK")
        self.assertEqual(args[4], "APPS_NET_PRIORITY")
        self.assertEqual(args[6], "APPS_NET_CORE")
        self.assertEqual(args[7], "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT")
        header = dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", (APPS / "apps_net.h").read_text(), re.M))
        self.assertEqual((header["APPS_NET_STACK"], header["APPS_NET_PRIORITY"], header["APPS_NET_CORE"], header["APPS_NET_JOBS"]),
                         ("8192", "3", "0", "8"))
        queue = call_arguments(net, "xQueueCreateWithCaps")
        self.assertEqual([queue[0], queue[2]], ["APPS_NET_JOBS", "MALLOC_CAP_SPIRAM"])
        self.assertNotIn("apps_io_submit", net, "VM requests never run on the I/O worker")
        # R3b: an app's reads are app_read, which yields the reader to the firmware's own (main/apps_link.c: VM_FILE_OWNER_APP).
        self.assertIn("ops->app_read(", net)
        self.assertNotRegex(net, r"ops->read\(")
        link = strip_c_comments((ROOT / "main/apps_link.c").read_text())
        self.assertIn("static const muse_apps_vm_ops_t VM_OPS = {vm_read, vm_write, vm_app_read};", link)
        self.assertIn("return read_as(VM_FILE_OWNER_APP,", c_function(link, "static bool vm_app_read("))
        self.assertIn("return read_as(VM_FILE_OWNER_INSTALL,", c_function(link, "static bool vm_read("))
        for name in ("apps_vmio.c", "apps_task.c"):
            self.assertNotIn("muse_apps_vm_ops()", strip_c_comments((APPS / name).read_text()), f"{name} never calls the VM itself")

    def test_the_decode_is_the_workers_and_the_image_add_the_app_tasks(self):  # P2, P14
        net = strip_c_comments((APPS / "apps_net.c").read_text())
        vmio = strip_c_comments((APPS / "apps_vmio.c").read_text())
        self.assertIn("apps_jpeg_decode(", net)
        self.assertNotIn("apps_jpeg_decode(", vmio)
        self.assertNotIn("apps_rt_image_add(", net)
        self.assertIn("apps_rt_image_add(", vmio)
        self.assertNotIn("apps_json_push(", net, "JSON is decoded into Lua on the app task (P14)")

    def test_the_vm_bytes_go_through_the_floor(self):  # every buffer for VM bytes is apps_port_realloc's
        for name in ("apps_net.c", "apps_vmio.c"):
            source = strip_c_comments((APPS / name).read_text())
            for call in ("malloc(", "calloc(", "heap_caps_malloc(", "heap_caps_calloc(", "realloc(", "heap_caps_realloc("):
                with self.subTest(name, call=call):
                    self.assertIsNone(re.search(rf"(?<![\w]){re.escape(call)}", source.replace("apps_port_realloc(", "")),
                                      f"{name} allocates VM bytes outside the floor")

    def test_locks_and_atomics_are_internal_and_plain_state_psram(self):  # S3 PSRAM has no CAS
        net = (APPS / "apps_net.c").read_text()
        self.assertRegex(net, r"(?m)^static StaticSemaphore_t s_lock_buf;")
        self.assertRegex(net, r"(?m)^static EXT_RAM_BSS_ATTR [^\n]*s_done")
        self.assertNotRegex(net, r"(?m)^static EXT_RAM_BSS_ATTR [^\n]*(atomic|Semaphore)")
        task = (APPS / "apps_task.c").read_text()
        self.assertRegex(task, r"(?m)^static atomic_bool s_vm_posted;")

    def test_binding_tables_and_the_protected_handle(self):  # X8, X17
        source = strip_c_comments((APPS / "apps_vmio.c").read_text())

        def table(name):
            body = re.search(rf"static const luaL_Reg {name}\[\] = \{{(.*?)\n\}};", source, re.S)
            self.assertIsNotNone(body, f"no `static const luaL_Reg {name}[]`")
            return re.findall(r'\{"(\w+)",', body.group(1))

        # X8: the Lua global is `vm`, so its table is apps_vm_funcs (the guide test reads module names from the tables' names).
        self.assertEqual(table("apps_vm_funcs"), ["read", "write", "command", "watch"])
        self.assertEqual(table("apps_watch_methods"), ["cancel"])
        self.assertIn('lua_setglobal(L, "vm")', source)
        match = re.search(r"luaL_newmetatable\(L, (\w+)\);(.*?)lua_setfield\(L, -2, \"__metatable\"\)", source, re.S)
        self.assertIsNotNone(match, "the watch handle's metatable has no __metatable")
        self.assertIn('#define APPS_WATCH_MT "muse.watch"', (APPS / "apps_vmio.h").read_text())
        functions = re.findall(r"^static int (\w+)\(lua_State \*L\) \{\n(.*?)^\}", source, re.S | re.M)
        bindings = [(name, body) for name, body in functions if re.search(rf'\{{"\w+", {name}\}}', source)]
        self.assertGreaterEqual(len(bindings), 5)
        for name, body in bindings:
            with self.subTest(name):
                self.assertTrue(body.lstrip().startswith("apps_vm_check(L);"), f"{name} doesn't start with apps_vm_check(L)")

    def test_the_constants(self):  # the brief's numbers, which the guide quotes (D12: test_muse_apps_link.py pins the guide to them)
        for name, value in (("APPS_VM_QUEUE", 8), ("APPS_VM_WATCHES_MAX", 4), ("APPS_VM_REQUEST_MS", 10000),
                            ("APPS_VM_ACK_MS", 15000), ("APPS_VM_ACK_POLL_MS", 1000), ("APPS_VM_HEARTBEAT_MS", 10000),
                            ("APPS_VM_BUSY_RETRIES", 3), ("APPS_VM_BUSY_WAIT_MS", 500), ("APPS_VM_WATCH_MAX", 1024),
                            ("APPS_VM_WRITE_MAX", 8192), ("APPS_VM_ARGS_MAX", 4096), ("APPS_VM_NAME_MAX", 32),
                            ("APPS_VM_EVERY_MIN_S", 1), ("APPS_VM_EVERY_MAX_S", 60), ("APPS_VM_EVERY_DEFAULT_S", 2)):
            with self.subTest(name):
                self.assertEqual(header_number("apps_vmio.h", name), value)
        header = (APPS / "apps_vmio.h").read_text()
        for name, kb in (("APPS_VM_JSON_MAX", 32), ("APPS_VM_TXT_MAX", 16), ("APPS_VM_JPEG_MAX", 256)):
            with self.subTest(name):
                self.assertRegex(header, rf"(?m)^#define {name} \({kb} \* 1024\)")
        self.assertRegex(header, r"(?m)^#define APPS_VM_BIN_MAX APPS_IMAGE_MAX\b")
        # P3: the acks' schema is written down where the code reads it.
        self.assertIn('{"acks":[{"seq":<int>,"result":<any JSON>}, ...]}', header)

    def test_the_app_task_drops_a_result_whose_app_has_ended(self):  # Review Focus 1, on the real app task's code
        task = strip_c_comments((APPS / "apps_task.c").read_text())
        take = c_function(task, "static void take_vm_results(void)")
        self.assertRegex(take, r"if \(app_runs\(job->app_gen\)\) \{\s*apps_vmio_take\(T\.rt, job, [^)]*\);\s*\} else \{\s*"
                               r"apps_vm_job_free\(job\);")
        self.assertIn("take_vm_results();", c_function(task, "static void housekeeping(void)"))


class VmOnAppHost(AppHostTestCase):
    """apphost, app.py check's runner, has no VM: its port refuses every vm request in the gadget's words for a VM it can't reach,
    and the callbacks run on `advance`, after the timers, as the app task's round runs them (apphost.c: run_vm)."""

    def test_every_request_is_not_connected(self):
        main = """
            local app = {}
            function app.message(event)
                vm.read("a.json", function(v, err) print("read", v, err) end)
                vm.write("outbox/x.json", {n = 1}, function(ok, err) print("write", ok, err) end)
                vm.command("go", nil, function(ok, err) print("command", ok, err) end)
                vm.watch("state/rev", 1, function(text, err) print("watch", text, err) end)
                print("asked")
            end
            return app
        """
        app = self.make_app("scratch", main, manifest={"permissions": ["vm"]})
        run = self.host(app, script=["sync 1767225600000", "message go", "advance 10", "advance 5000"])
        no = "the VM is not connected"
        # Nothing during the call; then turns alternate (R3a): the heartbeat's write, the read, the watch's first read, the
        # write and the command. The watch's next reads fail the same way, so they are quiet.
        self.assertEqual(run.logs(), ["asked", f"read\tnil\t{no}", f"watch\tnil\t{no}", f"write\tfalse\t{no}",
                                      f"command\tfalse\t{no}"])
        self.assertEqual([k for k in run.kinds() if k in ("warning", "error", "crash")], [], run.events)

    def test_the_port_says_not_connected(self):
        port = strip_c_comments((ROOT / "tools/muse/apphost/apphost_port.c").read_text())
        body = c_function(port, "bool apps_port_vm_submit(struct apps_vm_job *job)")
        self.assertIn('snprintf(job->err, sizeof job->err, "%s", APPS_VM_MSG_NOT_CONNECTED);', body)
        self.assertIn("return false;", body)
        listed = (ROOT / "tools/muse/apphost/sources.txt").read_text().split()
        self.assertIn("components/muse/apps/apps_vmio.c", listed)
        self.assertNotIn("components/muse/apps/apps_net.c", listed)  # no worker on the host


if __name__ == "__main__":
    unittest.main()
