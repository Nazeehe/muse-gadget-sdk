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

"""apps_json: the json module Muse apps get, the decoder and encoder the firmware uses itself,
and the pull reader that reads manifests.

Each test runs one case of tests/muse_apps_json_harness.c, which prints what it got when the case
fails. json.* runs through apps_vm_load and apps_vm_call in the real sandbox; the C entry points run
inside a protected call. Every case also ends with the harness's counting allocator at 0 bytes. One
test runs every case again against a Lua built with its own API checks on (stack slots).
"""

import math
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
SOURCES = [ROOT / "tests/muse_apps_json_harness.c", APPS / "apps_json.c", APPS / "apps_vm.c",
           APPS / "apps_suggest.c"]
INCLUDES = [ROOT / "components/lua/src", APPS]

# The text json.encode gives a float is apps_host.json_float_text's: the fewest of 15, 16 and 17 digits that read back,
# and ".0" where that would decode as an integer. These are the doubles whose text the tests pin as literals, the rule's
# corners; the reference is checked against each.
FLOAT_TEXTS = [
    (0.1, "0.1"),  # %.17g gives 0.10000000000000001; 15 digits read back
    (0.1 + 0.2, "0.30000000000000004"),  # needs all 17
    (1 / 3, "0.3333333333333333"),  # 16: 15 don't read back
    (math.pi, "3.141592653589793"),
    (2.0 ** 53, "9007199254740992.0"),  # 16 digits, and no '.'
    (2.0 ** 53 + 2, "9007199254740994.0"),
    (1e15, "1e+15"),  # %.15g switches to an exponent at 10^15
    (1e16, "1e+16"),
    (1e23, "1e+23"),  # the double next to 10^23, which 15 digits read back
    (sys.float_info.max, "1.7976931348623157e+308"),  # 15 and 16 digits round up past DBL_MAX: infinity
    (sys.float_info.min, "2.2250738585072014e-308"),
    (5e-324, "4.94065645841247e-324"),  # a host libc's exact digits; picolibc prints 5e-324 (Task 15's board run)
    (-0.0, "-0.0"),
    (3.0, "3.0"),
]


def float_rows(values) -> str:
    """The stdin of the harness's encode_floats case: a line a double, its IEEE-754 bits, a tab and its text."""
    return "".join(f"{struct.pack('>d', x).hex()}\t{apps_host.json_float_text(x)}\n" for x in values)


def many_floats() -> list[float]:
    """Some thousands of doubles, the same every run: every exponent, the ones a person writes (thirds, tenths,
    rounded to k places), the powers of ten and of two, and the subnormals."""
    rng = random.Random(20261028)
    values = []
    while len(values) < 1500:
        x = struct.unpack(">d", rng.getrandbits(64).to_bytes(8, "big"))[0]
        if math.isfinite(x):
            values.append(x)
    values += [struct.unpack(">d", rng.getrandbits(52).to_bytes(8, "big"))[0] for _ in range(100)]  # subnormals
    values += [rng.uniform(-1e6, 1e6) for _ in range(500)]
    values += [round(rng.uniform(-1000, 1000), rng.randint(0, 12)) + 0.0 for _ in range(500)]
    values += [i / 10 for i in range(-300, 300)] + [i / 7 + 0.1 for i in range(1000)]
    values += [10.0 ** k for k in range(-30, 31)] + [2.0 ** k for k in range(-1074, 1024, 7)]
    return values


class MuseAppsJsonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp.name
        cls.harness = apps_host.build("muse_apps_json_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=INCLUDES)

    def run_case(self, case: str, timeout: float = 60, harness=None, input: str = "") -> subprocess.CompletedProcess:
        """Runs one harness case; its stdin is input (empty for the cases that read none: never the test's own)."""
        try:
            done = apps_host.run(harness or self.harness, case, input=input, timeout=timeout)
        except subprocess.TimeoutExpired:
            done = None
        if done is None:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    # ---- json.decode ----

    def test_decode_types(self):
        self.run_case("decode_types")

    def test_decode_numbers(self):
        # int64 stays an integer, anything else is a float, and the grammar is RFC 8259's.
        self.run_case("decode_numbers")

    def test_decode_strings(self):
        # Escapes, surrogate pairs, \u0000 (kept: Lua strings hold it), raw UTF-8, long strings.
        self.run_case("decode_strings")

    def test_decode_accepts_valid_json(self):
        self.run_case("decode_valid")

    def test_decode_errors(self):
        # Every message exact, bare from the C functions and the pull reader's skip, prefixed by
        # the Lua binding: positions, end of input, ASCII-only text, UTF-8 by RFC 3629.
        self.run_case("decode_errors")

    def test_decode_reads_no_further_than_len(self):
        # A text is its first len bytes, whatever follows in memory: validate, push and the reader's skip
        # agree on prefixes the next byte would change ("12" with len 1 is 1), in place and from an exact copy.
        self.run_case("decode_prefixes")

    def test_decode_limits_both_ways(self):
        # Depth 16 passes and 17 fails, 32768 bytes pass the size check and 32769 fail, for json.decode,
        # apps_json_validate, apps_json_push and the reader.
        self.run_case("decode_limits")

    # ---- json.encode ----

    def test_encode_roundtrip(self):
        self.run_case("encode_roundtrip")

    def test_encode_values(self):
        self.run_case("encode_values")

    def test_encode_floats_in_the_fewest_digits_that_read_back(self):
        # The rule's corners with their texts as literals (and the Python reference agrees with each): what is
        # encoded decodes to the same bits, a zero's sign too, and as a float.
        for x, text in FLOAT_TEXTS:
            self.assertEqual(apps_host.json_float_text(x), text, x.hex())
        done = self.run_case("encode_floats", input=float_rows(x for x, _ in FLOAT_TEXTS))
        self.assertIn(f"encode_floats: {len(FLOAT_TEXTS)} doubles", done.stdout)

    def test_encode_floats_match_a_reference_over_thousands(self):
        # The text is what Python's printf gives by the same rule, for doubles of every exponent, the ones people
        # write, powers of ten and two, and subnormals (a host libc and Python both print exact digits); each
        # decodes to the same bits.
        values = many_floats()
        done = self.run_case("encode_floats", input=float_rows(values), timeout=60)
        self.assertIn(f"encode_floats: {len(values)} doubles", done.stdout)

    def test_encode_strings(self):
        self.run_case("encode_strings")

    def test_encode_errors(self):
        self.run_case("encode_errors")

    def test_encode_limits_both_ways(self):
        self.run_case("encode_limits")

    def test_encode_large_nested(self):
        # Ruling B6: the output grows while lua_next is mid-table. A luaL_Buffer would break here.
        self.run_case("encode_large_nested")

    def test_encode_shared_subtables_bounded(self):
        # Ruling R6.8: about 4^15 visits of one subtable end in the memory cap, not in a spin.
        self.run_case("encode_shared_subtables", timeout=10)

    def test_encode_scan_work_is_charged_to_the_budget(self):
        # Ruling R6.8, amended: lua_next walks every slot a table was ever grown to, empty or not, twice per
        # visit, so each visit is charged slots + 1 instructions. An emptied table shared through nesting is
        # killed by the budget, fast; honest work isn't.
        self.run_case("encode_scan_work", timeout=20)

    def test_decode_and_encode_check_the_clock_in_one_big_table(self):
        # A value costs microseconds on the S3, so every 256 of them in a loop call apps_vm_charge(L, 0).
        self.run_case("clock_checks", timeout=20)

    def test_encode_reads_tables_raw(self):
        # Ruling R6.7: no __index, __len, __pairs or __tostring runs inside json.encode.
        self.run_case("encode_raw")

    # ---- C contracts, bindings, memory ----

    def test_c_contracts(self):
        # Ruling R6.11: stack balance, bare messages, cut err, NULL arguments, a failing realloc_fn.
        self.run_case("c_contracts")

    def test_validate(self):
        self.run_case("validate")

    def test_module_shape_and_bindings(self):
        # json = {encode, decode, null}; every binding calls apps_vm_check first (ruling R6.12).
        self.run_case("module")

    def test_out_of_memory_ends_nomem(self):
        self.run_case("memory_caps", timeout=20)

    def test_allocation_failures_leave_nothing_behind(self):
        # Refuses the n-th allocation for every n until the call passes: OK or NOMEM, never a leak.
        self.run_case("allocation_sweep", timeout=60)

    # ---- The pull reader ----

    def test_pull_reader_reads_manifest_shape(self):
        self.run_case("reader_manifest")

    def test_pull_reader_is_strict(self):
        # Ruling R6.13 and the coordinator's clarification: strings, numbers, commas and depth are
        # checked as the decoder checks them.
        self.run_case("reader_strict")

    def test_compact(self):
        self.run_case("compact")

    def test_mutated_text_gets_one_verdict_and_what_decodes_round_trips(self):
        # 30000 deterministic mutations of valid documents: apps_json_validate, apps_json_push and the
        # reader's skip give the same verdict and message, and what decodes encodes to text that
        # decodes to the same value, number subtypes and all (but for the inf that 1e999 gives).
        self.run_case("fuzz_agreement", timeout=60)

    def test_json_stack_high_water(self):
        # Ruling R6.9: decode and encode at the depth limit, on an exact stack, without sanitizers
        # (which fatten frames). The marks go to the log; the device is the authority (Task 15).
        harness = apps_host.build("muse_apps_json_stack", [*SOURCES, *apps_host.lua_sources()], self.tmp,
                                  include_dirs=INCLUDES, sanitize=False)
        done = self.run_case("stack", harness=harness)
        print(done.stdout, end="", file=sys.stderr, flush=True)

    # ---- Lua stack slots (ruling R6.9) ----
    # A C function has 20 free slots, and 16 levels of table and key want more, so the decoder and the
    # encoder reserve with luaL_checkstack as they go. A push past the reserve goes unseen by a plain
    # Lua (5 slots are spare) but aborts a Lua built with LUA_USE_APICHECK, which these two tests use.

    apicheck = None

    def apicheck_harness(self) -> Path:
        cls = type(self)
        if cls.apicheck is None:
            cls.apicheck = apps_host.build("muse_apps_json_apicheck", [*SOURCES, *apps_host.lua_sources()], self.tmp,
                                           include_dirs=INCLUDES, lua_extra_defines=["LUA_USE_APICHECK"])
        return cls.apicheck

    def test_api_checks_abort_on_a_push_past_the_reserve(self):
        # The canary: without it, a build that dropped the define would pass the next test for nothing.
        done = apps_host.run(self.apicheck_harness(), "overrun")
        self.assertNotEqual(done.returncode, 0, "a push past the 20 slots didn't abort: the API checks are off")
        self.assertIn("stack overflow", done.stdout + done.stderr)

    def test_no_push_goes_past_the_reserve(self):
        # Every case of the harness, Lua's own checks on: deep text through the bindings and the C
        # entry points, the pull reader, the strings that make a luaL_Buffer's box, the output growing.
        harness = self.apicheck_harness()
        listed = apps_host.run(harness, "list")
        self.assertEqual(listed.returncode, 0, listed.stdout + listed.stderr)
        cases = [name for name in listed.stdout.split() if name not in ("overrun", "stack")]
        self.assertIn("stack_discipline", cases)
        stdin = {"encode_floats": float_rows(x for x, _ in FLOAT_TEXTS)}
        for case in cases:
            with self.subTest(case=case):
                self.run_case(case, timeout=90, harness=harness, input=stdin.get(case, ""))


if __name__ == "__main__":
    unittest.main()
