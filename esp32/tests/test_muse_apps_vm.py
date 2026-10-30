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

"""apps_vm, the Lua state every Muse app runs in: the capped heap, the sandbox, text-only
loading, instruction budgets and the kill.

Each test runs one case of tests/muse_apps_vm_harness.c, which prints what it got when the
case fails. Every case also ends with the harness's counting allocator at 0 bytes.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_rt import header_number

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
SOURCES = [ROOT / "tests/muse_apps_vm_harness.c", APPS / "apps_vm.c", APPS / "apps_suggest.c"]
INCLUDES = [ROOT / "components/lua/src", APPS]


class MuseAppsVmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        cls.harness = apps_host.build("muse_apps_vm_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=INCLUDES)

    def run_case(self, case: str, timeout: float = 60, harness=None) -> subprocess.CompletedProcess:
        try:
            done = apps_host.run(harness or self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            done = None
        if done is None:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    def test_bytecode_is_refused(self):
        self.run_case("bytecode")

    def test_bom_and_crlf_load(self):
        self.run_case("bom_crlf")

    def test_gc_metatable_refused(self):
        self.run_case("gc_metatable")

    def test_strict_globals_suggest(self):
        self.run_case("strict_globals")

    def test_sandbox_is_the_registry_globals(self):
        # C installs with lua_setglobal and reads with lua_rawget; chunks see the same table.
        self.run_case("registry_globals")

    def test_sandbox_contents(self):
        self.run_case("sandbox_contents")

    def test_patterns_absent_with_guidance(self):
        self.run_case("patterns")

    def test_the_string_and_global_metatables_are_protected(self):  # M3, X17: getmetatable gives a name for these too
        self.run_case("protected_string_and_global_metatables")
        # And the sandbox's code sets it (X17's static rule, for the two metatables apps_vm.c makes without luaL_newmetatable).
        source = (APPS / "apps_vm.c").read_text()
        strict = source[source.index("/* Strict globals:"):]
        strict = strict[:strict.index("lua_setmetatable(L, -2);")]
        self.assertIn('lua_setfield(L, -2, "__metatable");', strict, "the strict globals' metatable")
        self.assertRegex(source, r'lua_getmetatable\(L, -1\);\n\s*lua_pushliteral\(L, "string"\);\n\s*lua_setfield\(L, -2, "__metatable"\);',
                         "the strings' metatable")

    def test_plain_find_keeps_string_find_semantics(self):
        self.run_case("plain_find")

    def test_split_trim(self):
        self.run_case("split_trim")

    def test_find_is_linear_on_adversarial_input(self):
        # A 16 MB heap. KMP takes milliseconds; Lua's own memchr+memcmp search takes about 5 s.
        self.run_case("find_linear", timeout=2)

    def test_split_is_linear_on_adversarial_input(self):
        self.run_case("split_linear", timeout=2)

    # Lua's own rep, concat, move, insert, remove and sort can loop in C without
    # allocating, out of reach of the cap and of any count hook. Each must answer at once
    # instead.
    def test_rep_of_nothing_returns_at_once(self):
        self.run_case("rep_bounded", timeout=2)

    def test_move_bounded_by_the_heap(self):
        self.run_case("move_bounded", timeout=2)

    def test_insert_bounded_by_the_heap(self):
        self.run_case("insert_bounded", timeout=2)

    def test_remove_bounded_by_the_heap(self):
        self.run_case("remove_bounded", timeout=2)

    def test_sort_bounded_by_the_heap(self):
        self.run_case("sort_bounded", timeout=2)

    def test_concat_bounded_by_the_heap(self):
        self.run_case("concat_bounded", timeout=2)

    def test_len_taken_once_through_a_proxy(self):
        # A __len that answers small, then huge, can't slip a small count past the check.
        self.run_case("len_proxy", timeout=2)

    def test_did_you_mean_work_is_capped(self):
        self.run_case("suggest_capped", timeout=2)

    def test_allocator_caps_and_counts(self):
        self.run_case("allocator")

    def test_create_destroy_cycles_leak_nothing(self):
        self.run_case("cycles")

    def test_new_fails_cleanly_without_memory(self):
        self.run_case("new_without_memory")

    def test_excerpt_format(self):
        self.run_case("excerpt")

    def test_suggest_rule(self):
        self.run_case("suggest")

    # Budgets and the kill (Task 5). The kill cases' timeouts are short: a kill that doesn't
    # land leaves a loop running until the budget, seconds later, or for ever.
    def test_infinite_loop_killed(self):
        # D12: the harness prints APPS_BUDGET_CALL as it was compiled, and the reason and the count it got; the message
        # expected is built from that number, which is apps_vm.h's (Task 27 set it from the board's speed).
        out = self.run_case("infinite_loop", timeout=10).stdout
        printed = dict(line[1:].split(" ", 1) for line in out.splitlines() if line.startswith("@"))
        budget = int(printed["budget_call"])
        self.assertEqual(budget, header_number("apps_vm.h", "APPS_BUDGET_CALL"))
        self.assertEqual(printed["reason"],
                         f"stopped: ran more than {budget} VM instructions in one callback (use timer.every for long work)")
        self.assertEqual(int(printed["ran"]), budget + 1000)  # stopped at the count hook's first check past it

    def test_wall_clock_backstop(self):
        self.run_case("wall_clock", timeout=10)

    def test_close_variables_refused(self):
        # Ruling: <close> runs app code during an unwind, so it is refused at load. A generic for's
        # closing value still runs while an error unwinds (apps_vm.h); by then a kill or running out of
        # memory is final, so it can't swallow either.
        self.run_case("tbc_refused", timeout=10)

    def test_short_coroutines_are_counted(self):
        # A coroutine that runs fewer than 1000 instructions never reaches the hook, so creating
        # one counts as 1000. Without that, the tree in this case runs for minutes.
        self.run_case("short_coroutines", timeout=10)

    def test_kill_not_swallowed(self):
        self.run_case("kill_not_swallowed", timeout=30)

    def test_memory_exhaustion_is_a_kill(self):
        self.run_case("memory_kill", timeout=10)

    def test_a_refusal_below_the_cap_is_the_gadgets(self):  # C1: the floor's refusal says the gadget is short, not "raise memory_kb"
        self.run_case("a_refusal_below_the_cap_is_the_gadgets", timeout=10)

    def test_stack_growth_oom_not_swallowed(self):
        # Regression for the generic-for closing value that swallowed an out-of-memory error
        # through a Lua stack-growth refusal its own retry cleared. Sweeps a cap window.
        self.run_case("stack_growth_oom", timeout=30)

    def test_garbage_churn_near_the_cap_completes(self):
        # Ruling A2: a refusal is answered by an emergency collection, not a kill.
        self.run_case("churn", timeout=10)

    def test_external_kill(self):
        self.run_case("external_kill", timeout=10)

    def test_yield_through_pcall(self):
        self.run_case("yield_through_pcall")

    def test_deep_recursion_errors_not_crashes(self):
        self.run_case("deep_recursion")

    def test_c_stack_cases_fit_task_stack(self):
        # The brief's test_c_stack_cases_fit_32k_thread, renamed for the Task 5 ruling: the app
        # task's stack is 56 KB on the device, so each case runs to Lua's C-call limit on an
        # exact 96 KB region (host frames are 1.5-1.7 times Xtensa's), in a build without the
        # sanitizers, which fatten frames. The high-water marks go to the log.
        harness = apps_host.build("muse_apps_vm_stack", [*SOURCES, *apps_host.lua_sources()], self.tmp,
                                  include_dirs=INCLUDES, sanitize=False)
        done = self.run_case("c_stack", harness=harness)
        print(done.stdout, end="", file=sys.stderr, flush=True)

    def test_load_on_a_thread_is_apps_vm_load_with_that_threads_c_call_room(self):
        # apps_vm_load_on: one implementation with apps_vm_load (the same rules), and the parser's recursion
        # counted against the thread it runs on, as every other C recursion is.
        self.assertIn("LUAI_MAXCCALLS=40", apps_host.lua_defines(), "the harness's nesting is calibrated to the limit")
        self.run_case("load_on", timeout=30)

    def test_traceback_has_app_frames(self):
        self.run_case("traceback")

    def test_table_slots_are_what_lua_next_walks(self):
        # apps_vm_table_slots: the array and hash slots of a table, which stay after it is emptied.
        self.run_case("table_slots", timeout=10)

    def test_charge_counts_and_kills(self):
        # apps_vm_charge, for a C function that loops where no count hook fires: it counts, kills a call
        # over its budget or APPS_WALL_MS, raises for a dying VM, and does nothing outside a call.
        self.run_case("charge", timeout=10)

    def test_call_contract(self):
        # Stack and err on every outcome, in_call and its start, per-call budgets, a call
        # inside a call, apps_vm_check.
        self.run_case("call_contract", timeout=10)


if __name__ == "__main__":
    unittest.main()
