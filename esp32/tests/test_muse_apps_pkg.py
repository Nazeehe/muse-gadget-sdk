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

"""apps_pkg: the rules a Muse app's package has to meet: ids, paths, typed files, the manifest and LVGL
images.

Manifests and images are built here and parsed by tests/muse_apps_pkg_harness.c, which prints its verdict
as one JSON line, so every message is asserted exactly in this file. The harness also checks, on every
call, what must hold of any result (valid paths, no duplicate file, a compact messages_json, a message
that is valid text and fits, a manifest all zero after a failure), reads its input from a heap block of
exactly its length and gives err a block of exactly err_len bytes, so a read or a write past either is an
AddressSanitizer report. The C cases at the end run the module's contracts, its memory (messages_json
is a block of PSRAM, built with ESP_PLATFORM against a fake heap) and many generated and damaged
manifests.
"""

import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
SOURCES = [ROOT / "tests/muse_apps_pkg_harness.c", APPS / "apps_pkg.c", APPS / "apps_json.c", APPS / "apps_vm.c",
           APPS / "apps_suggest.c"]
INCLUDES = [ROOT / "components/lua/src", APPS, ROOT / "tests/apps_pkg_fakes"]

# The perms bits, as apps_pkg.h gives them.
SOUND, ALARM, STORE, KEEP_AWAKE = 1, 2, 4, 8

DROP = object()  # a key left out of doc()

# The messages, exactly (apps_pkg.h lists them all).
PATH_RULE = "(a-z 0-9 _ . / -, no '..', at most 64 characters)"
PERMS_VALID = "(valid: sound, alarm, store, keep_awake)"
NAME_MSG = "manifest: name must be 1 to 24 characters"
VERSION_MSG = 'manifest: version must be text of at most 23 characters, like "1.0.0"'
NAME_CONTROL_MSG = "manifest: name has a control character"
VERSION_CONTROL_MSG = "manifest: version has a control character"
API_MISSING_MSG = "manifest: api is missing (this gadget supports api 1)"
MAIN_NOT_LUA_MSG = "manifest: main must name a .lua file"
FILES_NOT_LIST_MSG = 'manifest: files must be a list of paths, like ["util.lua"]'
TOO_MANY_FILES_MSG = "manifest: more than 32 files"
PERMS_NOT_LIST_MSG = f'manifest: permissions must be a list, like ["sound"] {PERMS_VALID}'
MESSAGES_MSG = "manifest: messages must map event names to descriptions (at most 16, each under 200 characters)"
OUT_OF_MEMORY_MSG = "manifest: out of memory"
TOO_BIG_MSG = "manifest: manifest.json is larger than 8 KB"

IMAGE_NOT_LVGL_MSG = "image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)"
IMAGE_COMPRESSED_MSG = "image: compressed images aren't supported"


def image_flags_msg(flags):
    return f"image: flags 0x{flags:04x} aren't supported (only premultiplied alpha)"


def id_msg(id_text, folder="timer"):
    return f"manifest: id '{id_text}' must match ^[a-z][a-z0-9_]{{1,23}}$ and the folder name '{folder}'"


def api_msg(says):
    return f"manifest: this gadget supports api 1 (manifest says api {says})"


def main_path_msg(path):
    return f"manifest: main '{path}' is not an allowed path {PATH_RULE}"


def file_path_msg(index, path):
    return f"manifest: files[{index}] '{path}' is not an allowed path {PATH_RULE}"


def file_type_msg(index, path):
    return f"manifest: files[{index}] '{path}' has an unsupported type (use .lua, .json or .bin)"


def not_yet_msg(permission):
    return f"manifest: permission '{permission}' is not available in this firmware yet (api 1)"


def unknown_permission_msg(permission):
    return f"manifest: unknown permission '{permission}' {PERMS_VALID}"


def memory_msg(top):
    return f"manifest: memory_kb must be a whole number from 64 to {top}"


def image_big_msg(w, h):
    return f"image: {w}x{h} is larger than the 466x466 screen"


def image_empty_msg(w, h):
    return f"image: {w}x{h} has no pixels"


def image_format_msg(cf):
    return f"image: colour format 0x{cf:02x} isn't RGB565, RGB565A8 or A8"


def image_size_msg(got, want):
    return f"image: size {got} doesn't match its header ({want} expected)"


def image_stride_msg(stride, w, need):
    return f"image: stride {stride} is too small for a row of {w} pixels ({need} bytes needed)"


def j(value):
    """value as the compact JSON text a manifest would hold."""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def doc(**raw):
    """A manifest from raw JSON texts: id "timer", name "Timer" and api 1 unless given, then the others in
    the order given. DROP leaves a key out."""
    fields = {"id": '"timer"', "name": '"Timer"', "api": "1", **raw}
    return "{" + ",".join(f'"{key}":{value}' for key, value in fields.items() if value is not DROP) + "}"


# The manifest of the spec (8.4), without its // comments.
TIMER_MANIFEST = """{
  "id": "timer",
  "name": "Timer",
  "version": "1.0.0",
  "api": 1,
  "main": "main.lua",
  "files": ["util.lua", "assets/bell.bin"],
  "permissions": ["sound", "alarm", "store"],
  "memory_kb": 128,
  "messages": {
    "start": "Start a countdown. data: {seconds: whole number 1..5999}. Replies {state, remaining}.",
    "status": "Replies {state, remaining} in seconds."
  }
}"""

TIMER_MESSAGES = ('{"start":"Start a countdown. data: {seconds: whole number 1..5999}. Replies {state, remaining}.",'
                  '"status":"Replies {state, remaining} in seconds."}')


def lvgl_image(w=4, h=4, cf=0x12, flags=0, stride=None, magic=0x19, reserved=0, drop=0, extra=0):
    """An LVGL v9 .bin: a 12-byte header and stride * h bytes (RGB565A8: and (stride // 2) * h of alpha),
    `drop` bytes short of that or `extra` over."""
    bpp = 1 if cf == 0x0E else 2
    stride = w * bpp if stride is None else stride
    size = stride * h + ((stride // 2) * h if cf == 0x14 else 0)
    header = struct.pack("<BBHHHHH", magic, cf, flags, w, h, stride, reserved)
    return header + bytes(size - drop + extra)


class MuseAppsPkgTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = Path(tmp.name)
        cls.files = 0
        cls.harness = apps_host.build("muse_apps_pkg_harness", [*SOURCES, *apps_host.lua_sources()], tmp.name,
                                      include_dirs=INCLUDES, defines=["ESP_PLATFORM"])

    # ---- Running the harness ----

    def run_harness(self, *args, input=None, timeout=60) -> subprocess.CompletedProcess:
        try:
            done = apps_host.run(self.harness, *args, input=input, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{args[0]} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done

    def run_case(self, case: str, timeout: float = 60) -> subprocess.CompletedProcess:
        return self.run_harness(case, timeout=timeout)

    def put_file(self, data) -> Path:
        type(self).files += 1
        path = self.tmp / f"input-{self.files}"
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        return path

    def parse(self, text, folder="timer", heap_kb=1024, err_len=1024) -> dict:
        """The harness's verdict on a manifest (str or bytes): {"ok": true, "id": ...} or {"ok": false, "error": ...}.
        folder None is no folder at all."""
        done = self.run_harness("parse", self.put_file(text), "-" if folder is None else folder, heap_kb, err_len)
        return json.loads(done.stdout)

    def ok(self, text, **kwargs) -> dict:
        result = self.parse(text, **kwargs)
        self.assertTrue(result["ok"], f"{text[:300]!r}: {result.get('error')}")
        return result

    def refused(self, text, message, **kwargs):
        self.assertEqual(self.parse(text, **kwargs), {"ok": False, "error": message}, f"{text[:300]!r}")

    def check(self, what, texts) -> list:
        """What apps_id_valid, apps_path_valid or apps_file_kind (what = "id", "path", "kind") say of each text."""
        done = self.run_harness("check", what, input="".join(f"{text}\n" for text in texts))
        answers = done.stdout.split("\n")[:-1]
        self.assertEqual(len(answers), len(texts), done.stdout)
        return [answer == "1" for answer in answers] if what != "kind" else answers

    def image(self, data) -> dict:
        done = self.run_harness("image", self.put_file(data))
        return json.loads(done.stdout)

    def refused_image(self, data, message):
        self.assertEqual(self.image(data), {"ok": False, "error": message})

    # ---- Ids, paths and file kinds ----

    def test_id_rules(self):
        valid = ["ab", "timer", "a1", "a_", "a_b_9", "z9_", "a" * 24]
        invalid = ["", "a", "a" * 25, "Timer", "1a", "_a", "a-b", "a.b", "a b", "ab ", " ab", "aB", "a/b", "a\tb",
                   "éa", "tïmer"]
        got = self.check("id", valid + invalid)
        self.assertEqual(got, [True] * len(valid) + [False] * len(invalid), list(zip(valid + invalid, got)))

    def test_paths(self):
        valid = ["assets/x.bin", "a_b-c.d.lua", "main.lua", "lib/util.lua", "a/b/c/d.json", "x.lua", "0.bin", "-x.lua",
                 "_x.lua", "a/b.c/d.lua", "a" * 60 + ".lua", "/".join(["abcdefg"] * 8)]
        invalid = [
            "/x.lua", ".x.lua", "a/../b.lua", "A.lua", "x y.lua", "a" * 61 + ".lua",
            "/".join(["abcdefgh"] * 7) + "/x.lua",
            # R7.5: every segment is non-empty and doesn't start with '.'.
            "a//b.lua", "a/", "a/./b.lua", "a/.x.lua",
            "", "/", ".", "..", "../x.lua", "a/..", "a/b/..", "./x.lua", "a..b.lua",
            # FAT drops a trailing dot from a segment, so these are "a/b.lua" there.
            "a./b.lua", "x.lua.", "x.",
            "x\ty.lua", "x\\y.lua", "x:y.lua", "x*y.lua", "x~1.lua", " x.lua", "x.lua ", "é.lua", "aé/b.lua",
            "x#y.lua", "x'y.lua", "x\"y.lua", "x%y.lua",
        ]
        got = self.check("path", valid + invalid)
        wrong = [text for text, answer in zip(valid + invalid, got) if answer != (text in valid)]
        self.assertEqual(got, [True] * len(valid) + [False] * len(invalid), wrong)
        self.assertEqual(len(valid[-2]), 64)
        self.assertEqual(len(invalid[5]), 65)

    def test_file_kinds(self):
        texts = ["x.lua", "a/b.json", "i.bin", "x.txt", "lua", "x.LUA", "x.lua.txt", "x.bin.lua", "x.luax", "x.lu", "x",
                 "x.json5", "", ".lua", ".json", "x.jsonx", "bin", "x.Bin", "a.b/c"]
        want = ["lua", "json", "image", "bad", "bad", "bad", "bad", "lua", "bad", "bad", "bad", "bad", "bad", "lua",
                "json", "bad", "bad", "bad", "bad"]
        got = self.check("kind", texts)
        self.assertEqual(got, want, list(zip(texts, got)))

    # ---- The manifest that parses ----

    def test_timer_manifest_parses(self):
        # The spec's example (8.4): the messages are stored compact, from the pretty-printed text (B11).
        result = self.ok(TIMER_MANIFEST)
        self.assertEqual(result["files"], ["main.lua", "util.lua", "assets/bell.bin"])
        self.assertEqual(result["files"][0], "main.lua")
        self.assertEqual(result["perms"], SOUND | ALARM | STORE)
        self.assertEqual(result["memory_kb"], 128)
        self.assertEqual((result["id"], result["name"], result["version"], result["main"]),
                         ("timer", "Timer", "1.0.0", "main.lua"))
        self.assertTrue(result["messages_json"].startswith('{"start":'))
        self.assertEqual(result["messages_json"], TIMER_MESSAGES)
        self.assertEqual(result["warnings"], "")

    def test_defaults(self):
        # No main, memory_kb or permissions: main.lua, 256 KB and no permissions; no version or messages.
        result = self.ok(doc())
        self.assertEqual(result, {"ok": True, "id": "timer", "name": "Timer", "version": "", "main": "main.lua",
                                  "files": ["main.lua"], "perms": 0, "memory_kb": 256, "messages_json": None,
                                  "warnings": ""})

    def test_main_listed_twice_is_deduplicated(self):
        # main, whether it is named or left to its default, is the first file and not listed again.
        named = self.ok(doc(main='"main.lua"', files='["main.lua","util.lua"]'))
        self.assertEqual(named["files"], ["main.lua", "util.lua"])
        self.assertEqual(self.ok(doc(files='["util.lua","main.lua"]'))["files"], ["main.lua", "util.lua"])
        self.assertEqual(self.ok(doc(main='"app.lua"', files='["main.lua","app.lua","main.lua"]'))["files"],
                         ["app.lua", "main.lua"])

    def test_files_keep_their_order_and_drop_repeats(self):
        files = ["b.lua", "a.json", "assets/x.bin", "a.json", "b.lua", "assets/x.bin", "c.lua"]
        self.assertEqual(self.ok(doc(files=j(files)))["files"],
                         ["main.lua", "b.lua", "a.json", "assets/x.bin", "c.lua"])

    def test_permissions_make_a_bit_set(self):
        for permissions, bits in [([], 0), (["sound"], SOUND), (["alarm"], ALARM), (["store"], STORE),
                                  (["keep_awake"], KEEP_AWAKE), (["sound", "alarm", "store", "keep_awake"], 15),
                                  (["store", "store", "sound"], STORE | SOUND)]:
            with self.subTest(permissions=permissions):
                self.assertEqual(self.ok(doc(permissions=j(permissions)))["perms"], bits)

    def test_escapes_in_strings_are_decoded(self):
        result = self.ok(doc(id='"tim\\u0065r"', name='"Caf\\u00e9 \\ud83d\\ude00"', main='"m\\u0061in.lua"',
                             files='["u\\u0074il.lua"]', permissions='["so\\u0075nd"]', version='"1\\u002e0"'))
        self.assertEqual((result["id"], result["name"], result["main"], result["files"], result["perms"],
                          result["version"]),
                         ("timer", "Café \U0001F600", "main.lua", ["main.lua", "util.lua"], SOUND, "1.0"))

    def test_whitespace_and_key_order_dont_matter(self):
        text = ('\n\t{ "api" : 1 ,\r\n "files":[ "a.lua" ,\n"b.bin" ], "name" :"Timer",\n\n"id":\t"timer" , '
                '"messages" : { } }\n ')
        result = self.ok(text)
        self.assertEqual((result["files"], result["messages_json"]), (["main.lua", "a.lua", "b.bin"], "{}"))

    def test_names_and_versions_are_bytes(self):
        # R7.3: a name is 1 to 24 bytes, a version up to 23, whatever the characters.
        self.assertEqual(self.ok(doc(name=j("中" * 8)))["name"], "中" * 8)  # 24 bytes
        self.assertEqual(self.ok(doc(name=j("n" * 24)))["name"], "n" * 24)
        self.assertEqual(self.ok(doc(name=j("n")))["name"], "n")
        self.refused(doc(name=j("中" * 9)), NAME_MSG)  # 27 bytes, 9 characters
        self.refused(doc(name=j("n" * 25)), NAME_MSG)
        self.assertEqual(self.ok(doc(version=j("v" * 23)))["version"], "v" * 23)
        self.assertEqual(self.ok(doc(version=j("é" * 11)))["version"], "é" * 11)  # 22 bytes
        self.assertEqual(self.ok(doc(version='""'))["version"], "")
        self.refused(doc(version=j("v" * 24)), VERSION_MSG)
        self.refused(doc(version=j("é" * 12)), VERSION_MSG)  # 24 bytes, 12 characters

    def test_names_and_versions_refuse_control_characters(self):  # Task 13's F7
        # C0 (a decoded \n or \t, ESC), DEL and C1 (U+0080 to U+009F: U+009B is CSI to a terminal) would reach the
        # console's replies and the hung app's log line as they are: refused where the manifest is read.
        for text in ("A\u009b2J", "a\nb", "tab\there", "\x1b[31m", "del\x7f", "nel\u0085", "\u0080"):
            with self.subTest(text=text):
                self.refused(doc(name=j(text)), NAME_CONTROL_MSG)
                self.refused(doc(version=j(text)), VERSION_CONTROL_MSG)
        self.refused(doc(name='"a\\nb"'), NAME_CONTROL_MSG)  # the JSON escape \n, decoded to a newline
        # The reviewer's manifest: its name is refused first.
        self.refused('{"id":"timer","name":"A\\u009b2J","version":"\\u009b31m","api":1,"\\u009bk":0}', NAME_CONTROL_MSG)
        # Text past C1 is text: a no-break space (U+00A0), accents, other scripts.
        for text in ("\u00a0Caf\u00e9", "\u4e2d\u6587", "x\u00a0y"):
            with self.subTest(text=text):
                self.assertEqual(self.ok(doc(name=j(text)))["name"], text)
                self.assertEqual(self.ok(doc(version=j(text)))["version"], text)
        # The length rule still comes first for a name too long to read.
        self.refused(doc(name=j("\n" * 30)), NAME_MSG)

    def test_an_unknown_keys_controls_are_shown_as_question_marks(self):  # Task 13's F7: apps_install_shown's mapping
        result = self.ok('{"id":"timer","name":"Timer","api":1,"\\u009bk":0,"q\\nz":1,"x\x7fy":2,"\\u00e9t\\u00e9":3}')
        self.assertEqual(result["warnings"], "unknown key '?k' ignored; unknown key 'q?z' ignored; "
                                             "unknown key 'x?y' ignored; unknown key '\u00e9t\u00e9' ignored")

    # ---- The id ----

    def test_id_must_be_the_folder_name(self):
        self.refused(doc(id='"other"'), id_msg("other"))
        self.refused(doc(), id_msg("timer", "other"), folder="other")
        self.refused(doc(), id_msg("timer", ""), folder="")
        self.refused(doc(), id_msg("timer", ""), folder=None)  # no folder: nothing matches

    def test_id_must_match_the_pattern(self):
        # Equal to the folder, but not an id.
        for bad in ["Timer", "1abc", "a", "_ab", "a-b", "a b", ""]:
            with self.subTest(id=bad):
                self.refused(doc(id=j(bad)), id_msg(bad, bad), folder=bad)

    def test_id_of_24_characters_and_not_25(self):
        # B1: ^[a-z][a-z0-9_]{1,23}$ is 2 to 24 characters.
        self.assertEqual(self.ok(doc(id=j("a" * 24)), folder="a" * 24)["id"], "a" * 24)
        self.assertEqual(self.ok(doc(id=j("a" * 2)), folder="aa")["id"], "aa")
        self.refused(doc(id=j("a" * 25)), id_msg("a" * 25, "a" * 25), folder="a" * 25)
        self.refused(doc(id=j("a")), id_msg("a", "a"), folder="a")

    def test_id_of_the_wrong_type_shows_its_json(self):
        # R7.2.
        self.refused(doc(id="5"), id_msg("5"))
        self.refused(doc(id="null"), id_msg("null"))
        self.refused(doc(id="true"), id_msg("true"))
        self.refused(doc(id='["timer"]'), id_msg('["timer"]'))
        self.refused(doc(id='{"a":1}'), id_msg('{"a":1}'))
        self.refused(doc(id="1.5e3"), id_msg("1.5e3"))

    def test_missing_id_shows_empty(self):
        self.refused(doc(id=DROP), id_msg(""))
        self.refused('{"name":"Timer","api":1}', id_msg(""))

    # ---- The name and the version ----

    def test_name_message(self):
        self.refused(doc(name='""'), NAME_MSG)
        self.refused(doc(name=j("n" * 25)), NAME_MSG)
        self.refused(doc(name=j("n" * 300)), NAME_MSG)  # too long to even read
        self.refused(doc(name=DROP), NAME_MSG)
        for wrong in ["5", "null", "true", '["Timer"]', "{}"]:
            self.refused(doc(name=wrong), NAME_MSG)
        self.refused(doc(name='"a\\u0000b"'), NAME_MSG)  # a C string can't hold it

    def test_version_message(self):
        for wrong in ["1", "1.0", "null", "true", '["1.0.0"]', "{}", '"\\u0000"']:
            with self.subTest(version=wrong):
                self.refused(doc(version=wrong), VERSION_MSG)
        self.refused(doc(version=j("v" * 300)), VERSION_MSG)

    # ---- The api ----

    def test_api_message(self):
        self.refused(doc(api="2"), api_msg("2"))
        self.refused(doc(api="0"), api_msg("0"))
        self.refused(doc(api="-1"), api_msg("-1"))
        self.refused(doc(api="100"), api_msg("100"))

    def test_api_that_is_not_a_whole_number_shows_its_json(self):
        # R7.2: its raw text stands where the number does; 1.0 and 1e0 are not whole numbers (JSON integers).
        for raw in ['"1"', "1.0", "1e0", "true", "null", "[1]", "{}", '""', "99999999999999999999", "1.5"]:
            with self.subTest(api=raw):
                self.refused(doc(api=raw), api_msg(raw))

    def test_api_is_required(self):
        self.refused(doc(api=DROP), API_MISSING_MSG)

    # ---- The permissions ----

    def test_permissions_not_available_yet(self):
        for name in ["net", "secrets", "muse"]:
            with self.subTest(permission=name):
                self.refused(doc(permissions=j(["sound", name])), not_yet_msg(name))

    def test_unknown_permission(self):
        for name in ["bogus", "Sound", "sounds", "", "keep-awake", "NET"]:
            with self.subTest(permission=name):
                self.refused(doc(permissions=j([name])), unknown_permission_msg(name))

    def test_permission_of_the_wrong_type_shows_its_json(self):
        # R7.2.
        self.refused(doc(permissions='["sound", 7]'), unknown_permission_msg("7"))
        self.refused(doc(permissions="[null]"), unknown_permission_msg("null"))
        self.refused(doc(permissions='[["sound"]]'), unknown_permission_msg('["sound"]'))
        self.refused(doc(permissions='[{"a":1}]'), unknown_permission_msg('{"a":1}'))
        self.refused(doc(permissions='["a\\u0000b"]'), unknown_permission_msg("a\\u0000b"))

    def test_permissions_must_be_a_list(self):
        for wrong in ['"sound"', "{}", '{"sound":true}', "1", "null", "true"]:
            with self.subTest(permissions=wrong):
                self.refused(doc(permissions=wrong), PERMS_NOT_LIST_MSG)

    # ---- The files ----

    def test_files_path_message(self):
        self.refused(doc(files='["../x.lua"]'), file_path_msg(0, "../x.lua"))
        self.refused(doc(files='["a.lua","/x.lua"]'), file_path_msg(1, "/x.lua"))
        self.refused(doc(files='["a.lua","b.lua","A.lua"]'), file_path_msg(2, "A.lua"))
        self.refused(doc(files='["a//b.lua"]'), file_path_msg(0, "a//b.lua"))
        self.refused(doc(files='[""]'), file_path_msg(0, ""))

    def test_file_of_the_wrong_type_shows_its_json(self):
        # R7.2.
        self.refused(doc(files='["a.lua", true]'), file_path_msg(1, "true"))
        self.refused(doc(files="[5]"), file_path_msg(0, "5"))
        self.refused(doc(files="[null]"), file_path_msg(0, "null"))
        self.refused(doc(files='[["a.lua"]]'), file_path_msg(0, '["a.lua"]'))
        self.refused(doc(files='[{"path":"a.lua"}]'), file_path_msg(0, '{"path":"a.lua"}'))
        self.refused(doc(files='["a\\u0000b.lua"]'), file_path_msg(0, "a\\u0000b.lua"))

    def test_files_index_counts_the_manifests_own_list(self):
        # R7.4: files[%d] is a place in the manifest's list, not in the file list it makes (main and the repeats
        # take places there).
        self.refused(doc(files='["main.lua","util.lua","util.lua","../x.lua"]'), file_path_msg(3, "../x.lua"))
        self.refused(doc(files='["main.lua","main.lua","a.lua","x.txt"]'), file_type_msg(3, "x.txt"))

    def test_files_type_message(self):
        self.refused(doc(files='["a.txt"]'), file_type_msg(0, "a.txt"))
        self.refused(doc(files='["a.lua","notes"]'), file_type_msg(1, "notes"))
        self.refused(doc(files='["a.LUA"]'), file_path_msg(0, "a.LUA"))  # not a path at all
        self.refused(doc(files='["a.lua.txt"]'), file_type_msg(0, "a.lua.txt"))
        self.refused(doc(files='["a/b.png"]'), file_type_msg(0, "a/b.png"))

    def test_files_must_be_a_list(self):
        for wrong in ['"util.lua"', "{}", '{"a":"b.lua"}', "1", "null", "true"]:
            with self.subTest(files=wrong):
                self.refused(doc(files=wrong), FILES_NOT_LIST_MSG)

    def test_more_than_32_files(self):
        # R7.4: main and the others, repeats not counted: 32 pass and 33 don't.
        def lua(n):
            return [f"f{i:02d}.lua" for i in range(n)]
        self.assertEqual(len(self.ok(doc(files=j(lua(31))))["files"]), 32)
        self.refused(doc(files=j(lua(32))), TOO_MANY_FILES_MSG)
        self.refused(doc(files=j(lua(40))), TOO_MANY_FILES_MSG)
        # Repeats and main don't count: 31 and then everything again, main among it.
        self.assertEqual(len(self.ok(doc(files=j(lua(31) + lua(31) + ["main.lua"])))["files"]), 32)
        self.assertEqual(len(self.ok(doc(main='"f00.lua"', files=j(lua(32))))["files"]), 32)  # main is one of the 32
        self.refused(doc(main='"app.lua"', files=j(lua(32))), TOO_MANY_FILES_MSG)
        # The 33rd distinct one is the one refused, and a bad one before it is refused first.
        self.refused(doc(files=j(lua(32) + ["../x.lua"])), TOO_MANY_FILES_MSG)
        self.refused(doc(files=j(lua(10) + ["../x.lua"] + lua(32))), file_path_msg(10, "../x.lua"))

    def test_over_long_path_gets_the_path_message(self):
        # B14: a path is read into 256 bytes, so one past 64 characters is refused by name, whatever its length.
        for n in [65, 100, 255]:
            path = "a" * (n - 4) + ".lua"
            with self.subTest(length=n):
                self.refused(doc(files=j([path])), file_path_msg(0, path))
                self.refused(doc(main=j(path)), main_path_msg(path))
        # Past what is read, the message quotes the start of it.
        for n in [256, 300, 5000]:
            path = "a" * (n - 4) + ".lua"
            with self.subTest(length=n):
                self.refused(doc(files=j([path])), file_path_msg(0, "a" * 32 + "..."))
                self.refused(doc(main=j(path)), main_path_msg("a" * 32 + "..."))
        self.assertEqual(self.ok(doc(files=j(["a" * 60 + ".lua"])))["files"][1], "a" * 60 + ".lua")  # 64 passes

    # ---- The main file ----

    def test_main_must_name_a_lua_file(self):
        for main in ["main.json", "main.bin", "main", "main.txt", "lua", "a/b.json"]:
            with self.subTest(main=main):
                self.refused(doc(main=j(main)), MAIN_NOT_LUA_MSG)

    def test_main_path_message(self):
        self.refused(doc(main='"../main.lua"'), main_path_msg("../main.lua"))
        self.refused(doc(main='"Main.lua"'), main_path_msg("Main.lua"))
        self.refused(doc(main='"/main.lua"'), main_path_msg("/main.lua"))
        self.refused(doc(main='""'), main_path_msg(""))
        self.refused(doc(main="5"), main_path_msg("5"))
        self.refused(doc(main="null"), main_path_msg("null"))
        self.refused(doc(main='["main.lua"]'), main_path_msg('["main.lua"]'))
        self.refused(doc(main='"a\\u0000.lua"'), main_path_msg("a\\u0000.lua"))
        self.assertEqual(self.ok(doc(main='"lib/app.lua"'))["files"], ["lib/app.lua"])

    # ---- The memory ----

    def test_memory_kb_message(self):
        top = memory_msg(1024)
        for raw in ["63", "0", "-64", "1025", "100000", '"128"', "128.5", "1e2", "1.0", "null", "true", "[]", "{}",
                    "99999999999999999999"]:
            with self.subTest(memory_kb=raw):
                self.refused(doc(memory_kb=raw), top)
        for kb in [64, 65, 128, 256, 1000, 1024]:
            self.assertEqual(self.ok(doc(memory_kb=str(kb)))["memory_kb"], kb)

    def test_memory_limits_follow_the_heap(self):
        # The most is 1024 or the device's CONFIG_MUSE_APPS_HEAP_MAX_KB, whichever is less; the message says it.
        self.refused(doc(memory_kb="513"), memory_msg(512), heap_kb=512)
        self.assertEqual(self.ok(doc(memory_kb="512"), heap_kb=512)["memory_kb"], 512)
        self.refused(doc(memory_kb="1025"), memory_msg(1024), heap_kb=4096)  # never past 1024
        self.assertEqual(self.ok(doc(memory_kb="1024"), heap_kb=4096)["memory_kb"], 1024)
        self.assertEqual(self.ok(doc(memory_kb="256"), heap_kb=256)["memory_kb"], 256)
        self.refused(doc(memory_kb="257"), memory_msg(256), heap_kb=256)
        # A heap under 64 (the option's range starts at 256) counts as 64.
        self.refused(doc(memory_kb="65"), memory_msg(64), heap_kb=32)
        self.assertEqual(self.ok(doc(memory_kb="64"), heap_kb=32)["memory_kb"], 64)

    def test_default_memory_is_256_or_what_the_heap_has(self):
        self.assertEqual(self.ok(doc(), heap_kb=1024)["memory_kb"], 256)
        self.assertEqual(self.ok(doc(), heap_kb=256)["memory_kb"], 256)
        self.assertEqual(self.ok(doc(), heap_kb=128)["memory_kb"], 128)
        self.assertEqual(self.ok(doc(), heap_kb=64)["memory_kb"], 64)
        self.assertEqual(self.ok(doc(), heap_kb=0)["memory_kb"], 64)

    # ---- The messages ----

    def test_messages_message(self):
        for raw in ["[]", '"start"', "5", "null", "true", '{"a":1}', '{"a":null}', '{"a":["b"]}', '{"a":{"b":"c"}}',
                    '{"a":"b","c":2}', '{"a":true}']:
            with self.subTest(messages=raw):
                self.refused(doc(messages=raw), MESSAGES_MSG)

    def test_messages_at_most_16(self):
        def events(n):
            return j({f"e{i:02d}": "d" for i in range(n)})
        self.assertEqual(self.ok(doc(messages=events(16)))["messages_json"], events(16))
        self.refused(doc(messages=events(17)), MESSAGES_MSG)
        self.refused(doc(messages=events(100)), MESSAGES_MSG)

    def test_messages_under_200_bytes_each(self):
        # R7.3: bytes, for the description and for the event's name.
        self.assertEqual(self.ok(doc(messages=j({"a": "x" * 199})))["messages_json"], j({"a": "x" * 199}))
        self.refused(doc(messages=j({"a": "x" * 200})), MESSAGES_MSG)
        self.refused(doc(messages=j({"a": "x" * 5000})), MESSAGES_MSG)
        self.assertEqual(self.ok(doc(messages=j({"a": "é" * 99})))["messages_json"], j({"a": "é" * 99}))  # 198 bytes
        self.assertEqual(self.ok(doc(messages=j({"a": "é" * 99 + "x"})))["messages_json"], j({"a": "é" * 99 + "x"}))
        self.refused(doc(messages=j({"a": "é" * 100})), MESSAGES_MSG)  # 100 characters, 200 bytes
        self.assertEqual(self.ok(doc(messages=j({"x" * 199: "d"})))["messages_json"], j({"x" * 199: "d"}))
        self.refused(doc(messages=j({"x" * 200: "d"})), MESSAGES_MSG)
        self.refused(doc(messages=j({"x" * 3000: "d"})), MESSAGES_MSG)
        self.refused(doc(messages='{"a":"b\\u0000c"}'), MESSAGES_MSG)

    def test_messages_json_is_compact_and_as_written(self):
        # B11: only the whitespace outside strings goes; escapes and the order stay as they were.
        text = '{\n\t"b" : "x  y\\n z" ,\r\n  "a":\t"\\u00e9\\"" , "":""\n}'
        self.assertEqual(self.ok(doc(messages=text))["messages_json"], '{"b":"x  y\\n z","a":"\\u00e9\\"","":""}')
        self.assertEqual(self.ok(doc(messages="{ }"))["messages_json"], "{}")
        self.assertIsNone(self.ok(doc())["messages_json"])

    # ---- Unknown keys ----

    def test_unknown_key_warns_and_suggests(self):
        # B8: apps_suggest's rule, against the manifest's keys.
        result = self.ok(doc(permission='["sound"]'))
        self.assertEqual(result["warnings"], "unknown key 'permission' ignored (did you mean 'permissions'?)")
        self.assertEqual(result["perms"], 0)  # the misspelt key does nothing
        for key, near in [("file", "files"), ("Name", "name"), ("mian", "main"), ("memory_k", "memory_kb"),
                          ("messages_", "messages"), ("versions", "version"), ("ids", "id")]:
            with self.subTest(key=key):
                self.assertEqual(self.ok(doc(**{key: "1"}))["warnings"],
                                 f"unknown key '{key}' ignored (did you mean '{near}'?)")

    def test_unrelated_unknown_key_gets_no_suggestion(self):
        # B8: no alias table, so "entry" (a main by another name) is just unknown.
        for key in ["entry", "colour", "author", "x", "description", "memory", "e"]:
            with self.subTest(key=key):
                self.assertEqual(self.ok(doc(**{key: "1"}))["warnings"], f"unknown key '{key}' ignored")
        self.assertEqual(self.ok(doc(**{"k" * 70: "1"}))["warnings"], f"unknown key '{'k' * 70}' ignored")

    def test_unknown_keys_are_joined_in_order_and_once_each(self):
        text = '{"id":"timer","author":"me","name":"Timer","api":1,"permission":[],"author":"you","entry":1}'
        self.assertEqual(self.ok(text)["warnings"],
                         "unknown key 'author' ignored; "
                         "unknown key 'permission' ignored (did you mean 'permissions'?); "
                         "unknown key 'entry' ignored")

    def test_a_key_that_holds_another_warnings_text_doesnt_stand_for_it(self):
        # A repeat is dropped only when that whole warning is already there, between its separators: not the end of
        # another's, nor its start.
        ends_like_it = "x unknown key 'a"
        self.assertEqual(self.ok(doc(**{ends_like_it: "1", "a": "1"}))["warnings"],
                         f"unknown key '{ends_like_it}' ignored; unknown key 'a' ignored")
        starts_like_it = "a' ignored y"
        self.assertEqual(self.ok(doc(**{starts_like_it: "1", "a": "1"}))["warnings"],
                         f"unknown key '{starts_like_it}' ignored; unknown key 'a' ignored")
        # And a repeat is found wherever it is: after text that only looks like it.
        text = '{"id":"timer","name":"Timer","api":1,"x unknown key \'a":1,"a":1,"b":1,"a":2}'
        self.assertEqual(self.ok(text)["warnings"],
                         "unknown key 'x unknown key 'a' ignored; unknown key 'a' ignored; unknown key 'b' ignored")

    def test_unknown_keys_are_skipped_whatever_they_hold(self):
        text = doc(extra='{"a":[1,2.5e3,{"b":null,"c":"\\u00e9"}],"d":true}', second="[[[]]]")
        self.assertEqual(self.ok(text)["warnings"], "unknown key 'extra' ignored; unknown key 'second' ignored")

    def test_warning_that_does_not_fit_is_dropped_whole(self):
        # R7.7: warnings holds 511 characters. Each of these is 25 and takes 27 with its "; ", so 19 fit.
        keys = [f"k{i:02d}" for i in range(25)]
        result = self.ok(doc(**{key: "1" for key in keys}))
        self.assertEqual(result["warnings"], "; ".join(f"unknown key '{key}' ignored" for key in keys[:19]))
        self.assertEqual(len(result["warnings"]), 511)

    def test_a_warning_that_would_make_512_is_dropped_and_one_that_makes_511_isnt(self):
        # 18 of 25 characters and their separators are 484; a warning of 26 would make 512, which leaves no room for
        # the end of the string, and one of 25 makes 511.
        keys = [f"k{i:02d}" for i in range(18)]
        short = "; ".join(f"unknown key '{key}' ignored" for key in keys)
        self.assertEqual(len(short), 484)
        self.assertEqual(self.ok(doc(**{key: "1" for key in [*keys, "zzzz"]}))["warnings"], short)
        self.assertEqual(self.ok(doc(**{key: "1" for key in [*keys, "zzzz", "k99"]}))["warnings"],
                         short + "; unknown key 'k99' ignored")
        self.assertEqual(len(short + "; unknown key 'k99' ignored"), 511)

    def test_a_dropped_warning_doesnt_stop_later_ones(self):
        # A long one that doesn't fit is dropped whole; a short one after it that does is kept.
        long_a, long_b = "a" * 250, "b" * 250
        text = doc(first="1", **{long_a: "1", long_b: "1"}, last="2")
        self.assertEqual(self.ok(text)["warnings"],
                         f"unknown key 'first' ignored; unknown key '{long_a}' ignored; unknown key 'last' ignored")

    # ---- Malformed JSON and size ----

    def test_syntax_error_is_the_readers_message(self):
        # R7.1: "manifest: " and apps_json.h's bare message, with its 1-based position.
        def at(text, needle, back=0):
            return text.index(needle) + 1 + back

        text = '{"id":"timer","name":"Timer","api":1,}'
        self.refused(text, f"manifest: unexpected '}}' at byte {at(text, '}')}")
        self.refused('{"id": }', "manifest: unexpected '}' at byte 8")
        self.refused('{"id":"timer"', "manifest: unexpected end of input at byte 14")
        text = '{"id":"timer"} x'
        self.refused(text, f"manifest: unexpected 'x' at byte {at(text, 'x')}")
        text = '{"id":"timer"}{}'
        self.refused(text, f"manifest: unexpected '{{' at byte {at(text, '}{', 1)}")
        text = '{"id" "timer"}'
        self.refused(text, "manifest: unexpected '\"' at byte " + str(at(text, ' "timer', 1)))
        self.refused('{id:"timer"}', "manifest: unexpected 'i' at byte 2")
        self.refused("{'id':'timer'}", "manifest: unexpected ''' at byte 2")
        text = '{"id":"timer",,"name":"Timer"}'
        self.refused(text, f"manifest: unexpected ',' at byte {at(text, ',,', 1)}")
        text = '{"id":"a\\x"}'
        self.refused(text, f"manifest: unexpected 'x' at byte {at(text, 'x')}")
        text = '{"id":"a\\u00zz"}'
        self.refused(text, f"manifest: invalid \\u escape at byte {at(text, chr(92))}")  # at the backslash
        text = '{"id":"timer","x":01}'
        self.refused(text, f"manifest: unexpected '1' at byte {at(text, '01', 1)}")
        text = '{"id":"timer","x":NaN}'
        self.refused(text, f"manifest: unexpected 'N' at byte {at(text, 'N')}")
        text = '{"id":"timer"} // note'
        self.refused(text, f"manifest: unexpected '/' at byte {at(text, '//')}")

    def test_a_document_that_is_not_an_object(self):
        self.refused("", "manifest: unexpected end of input at byte 1")
        self.refused("  \n ", "manifest: unexpected end of input at byte 5")
        self.refused("[]", "manifest: unexpected '[' at byte 1")
        self.refused('"timer"', "manifest: unexpected '\"' at byte 1")
        self.refused("5", "manifest: unexpected '5' at byte 1")
        self.refused("null", "manifest: unexpected 'n' at byte 1")
        self.refused("\ufeff" + doc(), "manifest: unexpected byte 0xEF at byte 1")  # no BOM

    def test_syntax_errors_in_values_the_manifest_ignores(self):
        # The reader checks what it skips, so a bad value under a key nobody reads is an error too.
        text = doc(x='"@"').encode()
        for raw in [b"\xc3(", b"\xff", b"\xe2\x82"]:
            bad = text.replace(b"@", raw)
            self.refused(bad, f"manifest: invalid UTF-8 at byte {bad.index(raw) + 1}")
        text = doc(x='"a\tb"')
        self.refused(text, f"manifest: unexpected byte 0x09 at byte {text.index(chr(9)) + 1}")
        text = doc(x="[1,2,]")
        self.refused(text, f"manifest: unexpected ']' at byte {text.index(',]') + 2}")
        text = doc(x="1e")
        self.refused(text, f"manifest: unexpected '}}' at byte {len(text)}")
        text = doc(x="9" * 201)
        self.refused(text, f"manifest: number longer than 200 characters at byte {text.index('999') + 1}")
        self.refused(b'{"id":"timer"\x00}', "manifest: unexpected byte 0x00 at byte 14")

    def test_nesting_up_to_16_levels(self):
        # The object is the first level.
        self.assertEqual(self.ok(doc(x="[" * 15 + "]" * 15))["warnings"], "unknown key 'x' ignored")
        self.refused(doc(x="[" * 16 + "]" * 16), "manifest: nested deeper than 16")

    def test_a_key_the_reader_cant_hold_is_the_readers_error(self):
        # A key is read into 256 bytes, so one longer than 255, or with \u0000 in it, is an error of the reader's
        # (R7.1), at the key's opening quote or at the escape. A key of 255 bytes is just an unknown one.
        text = doc()[:-1] + ',"' + "k" * 256 + '":1}'
        at = text.index('"k') + 1
        self.refused(text, f"manifest: string longer than 255 bytes at byte {at}")
        text = doc()[:-1] + ',"a\\u0000b":1}'
        at = text.index("\\u0000") + 1
        self.refused(text, f"manifest: \\u0000 isn't allowed in this string at byte {at}")
        self.assertEqual(self.ok(doc(**{"k" * 255: "1"}))["warnings"], f"unknown key '{'k' * 255}' ignored")

    def test_manifest_size_is_at_most_8_kb(self):
        # R7.1: 8192 bytes pass; 8193, however it is made up, don't.
        text = doc()
        self.ok(text + " " * (8192 - len(text)))
        self.refused(text + " " * (8193 - len(text)), TOO_BIG_MSG)
        self.refused(text + " " * (40000 - len(text)), TOO_BIG_MSG)
        self.refused("x" * 9000, TOO_BIG_MSG)  # size comes before syntax
        padded = doc(x=j("y" * 8200))
        self.refused(padded, TOO_BIG_MSG)

    # ---- What a message quotes ----

    def test_a_value_of_the_wrong_type_is_cut_to_32_bytes(self):
        # R7.2: 32 bytes of its JSON text, then "...". 32 are shown whole.
        raw = j(["a" * 28])
        self.assertEqual(len(raw), 32)
        self.refused(doc(id=raw), id_msg(raw))
        raw = j(["a" * 29])
        self.refused(doc(id=raw), id_msg(raw[:32] + "..."))
        raw = j(["a" * 500])
        self.refused(doc(id=raw), id_msg(raw[:32] + "..."))
        self.refused(doc(files=j([["b" * 100]])), file_path_msg(0, j(["b" * 100])[:32] + "..."))
        self.refused(doc(permissions=j([{"k": "v" * 100}])), unknown_permission_msg(j({"k": "v" * 100})[:32] + "..."))
        self.refused(doc(api=j("1" * 100)), api_msg(j("1" * 100)[:32] + "..."))
        self.refused(doc(api="1" + "0" * 100), api_msg("1" + "0" * 31 + "..."))

    def test_the_cut_never_splits_a_character(self):
        # The text is '["' and what follows; the cut at 32 bytes falls after 0 to size - 1 bytes of a character
        # of 2, 3 or 4 bytes, and the message keeps what comes before that character.
        for char in ["é", "中", "\U0001F600"]:
            size = len(char.encode())
            for kept in range(size):
                begins = 32 - kept
                raw = '["' + "x" * (begins - 2) + char * 8 + '"]'
                with self.subTest(char=char, cut_after=kept):
                    self.assertEqual(raw.encode()[begins:begins + size].decode(), char)
                    self.refused(doc(id=raw), id_msg(raw.encode()[:begins].decode() + "..."))

    def test_a_string_is_quoted_as_read(self):
        # Of the right type it is shown whole, decoded, up to the 255 bytes the reader takes.
        self.refused(doc(id=j("B" * 100)), id_msg("B" * 100))
        self.refused(doc(id=j("B" * 255)), id_msg("B" * 255))
        self.refused(doc(id='"B\\u00e9\\n"'), id_msg("Bé\n"))
        self.refused(doc(files=j(["é.lua"])), file_path_msg(0, "é.lua"))
        self.refused(doc(permissions=j(["中"])), unknown_permission_msg("中"))

    def test_a_string_too_long_to_read_is_quoted_from_its_json(self):
        # Past 255 bytes the reader has no room for it, so the message shows the start of what was written (inside
        # the quotes), and a string with \u0000 in it, which can't be read either, the same.
        self.refused(doc(id=j("B" * 256)), id_msg("B" * 32 + "..."))
        self.refused(doc(id=j("B" * 5000)), id_msg("B" * 32 + "..."))
        self.refused(doc(id='"a\\u0000b"'), id_msg("a\\u0000b"))
        self.refused(doc(id=j("\\n" * 200)), id_msg(j("\\n" * 200)[1:33] + "..."))
        self.refused(doc(permissions=j(["P" * 256])), unknown_permission_msg("P" * 32 + "..."))

    def test_the_folder_name_is_quoted_cut_too(self):
        folder = "f" * 40
        self.refused(doc(), id_msg("timer", "f" * 32 + "..."), folder=folder)

    # ---- Several problems: which comes first ----

    def test_the_first_problem_in_the_documented_order_is_the_one_reported(self):
        # id, name, version, api, main, files, permissions, memory_kb, messages; each is reported while every
        # later one is wrong too.
        problems = [
            ("id", '"Bad"', id_msg("Bad")),
            ("name", '""', NAME_MSG),
            ("version", "1", VERSION_MSG),
            ("api", "2", api_msg("2")),
            ("main", '"main.json"', MAIN_NOT_LUA_MSG),
            ("files", '["../x"]', file_path_msg(0, "../x")),
            ("permissions", '["net"]', not_yet_msg("net")),
            ("memory_kb", "1", memory_msg(1024)),
            ("messages", "[]", MESSAGES_MSG),
        ]
        for i, (key, _, message) in enumerate(problems):
            text = doc(**{k: v for k, v, _ in problems[i:]})
            with self.subTest(first=key):
                self.refused(text, message)

    def test_the_order_is_the_documented_one_whatever_the_order_in_the_text(self):
        text = ('{"messages":[],"memory_kb":1,"permissions":["net"],"files":["../x"],"main":"main.json",'
                '"api":2,"version":1,"name":"","id":"Bad"}')
        self.refused(text, id_msg("Bad"))

    def test_a_missing_required_key_is_reported_after_the_problems_with_the_keys_there(self):
        self.refused('{"permissions":["net"]}', not_yet_msg("net"))
        self.refused('{"id":"timer","files":["../x"]}', file_path_msg(0, "../x"))
        self.refused('{"id":"timer","name":"Timer","messages":[]}', MESSAGES_MSG)
        # Then id, name and api in that order.
        self.refused("{}", id_msg(""))
        self.refused('{"name":"Timer"}', id_msg(""))
        self.refused('{"id":"timer"}', NAME_MSG)
        self.refused('{"id":"timer","name":"Timer"}', API_MISSING_MSG)
        self.refused('{"id":"timer","api":1}', NAME_MSG)

    def test_a_syntax_error_comes_before_any_problem_with_a_value(self):
        text = '{"id":"Bad","name":"","api":2,"x":}'
        self.refused(text, f"manifest: unexpected '}}' at byte {len(text)}")
        text = doc(id='"Bad"', files='["../x"]') + "x"
        self.refused(text, f"manifest: unexpected 'x' at byte {len(text)}")

    # ---- Repeated keys ----

    def test_a_repeated_key_has_its_last_value(self):
        # R7.6, as json.decode: a first value that was wrong doesn't matter once a later one replaces it.
        text = '{"id":"Bad","name":"x","id":"timer","name":"Timer","api":2,"api":1}'
        self.assertEqual(self.ok(text)["name"], "Timer")
        self.refused('{"id":"timer","name":"Timer","api":1,"api":2}', api_msg("2"))
        self.refused('{"id":"timer","id":"Bad","name":"Timer","api":1}', id_msg("Bad"))
        self.refused('{"id":"timer","id":"timer","name":"Timer","name":"","api":1}', NAME_MSG)
        wrong = doc(files='["../x"]', permissions='["net"]', memory_kb="1", messages="[]", main='"x.json"')
        result = self.ok(wrong[:-1] + ',"files":["a.lua"],"permissions":["store"],"memory_kb":99,'
                         '"messages":{"a":"b"},"main":"m.lua"}')
        self.assertEqual((result["files"], result["perms"], result["memory_kb"], result["messages_json"]),
                         (["m.lua", "a.lua"], STORE, 99, '{"a":"b"}'))
        # A key given twice isn't missing, and the second can be the wrong type.
        self.refused('{"id":"timer","name":"Timer","api":1,"files":["a.lua"],"files":"a.lua"}', FILES_NOT_LIST_MSG)

    def test_a_repeated_key_spelt_two_ways_is_one_key(self):
        text = '{"id":"Bad","i\\u0064":"timer","name":"Timer","api":1}'
        self.assertEqual(self.ok(text)["id"], "timer")

    # ---- Errors that are cut ----

    def test_err_is_cut_on_a_character_boundary(self):
        # err_len is the buffer's size, so a message keeps at most err_len - 1 bytes, backed up to a character's start.
        for char in ["é", "中", "\U0001F600"]:
            text = doc(id=j("B" + char))
            data = id_msg("B" + char).encode()
            for err_len in [*range(1, 4), *range(13, 24), len(data) - 1, len(data), len(data) + 1, len(data) + 2]:
                keep = min(err_len - 1, len(data))
                if keep < len(data):
                    while keep > 0 and (data[keep] & 0xC0) == 0x80:
                        keep -= 1
                with self.subTest(char=char, err_len=err_len):
                    self.assertEqual(self.parse(text, err_len=err_len), {"ok": False, "error": data[:keep].decode()})
        self.assertEqual(self.parse(doc(id="5"), err_len=1), {"ok": False, "error": ""})

    # ---- Images ----

    def test_image_check(self):
        # A 4x4 RGB565 passes with its size; so do the other two formats.
        self.assertEqual(self.image(lvgl_image()), {"ok": True, "w": 4, "h": 4})
        self.assertEqual(self.image(lvgl_image(w=5, h=3, cf=0x0E)), {"ok": True, "w": 5, "h": 3})
        self.assertEqual(self.image(lvgl_image(w=3, h=7, cf=0x14)), {"ok": True, "w": 3, "h": 7})
        # Wrong magic, compressed, 467 wide, a byte short: each its message.
        self.refused_image(lvgl_image(magic=0x18), IMAGE_NOT_LVGL_MSG)
        self.refused_image(lvgl_image(flags=0x0008), IMAGE_COMPRESSED_MSG)
        self.refused_image(lvgl_image(w=467), image_big_msg(467, 4))
        self.refused_image(lvgl_image(drop=1), image_size_msg(43, 44))

    def test_image_is_an_lvgl_v9_file(self):
        self.refused_image(b"", IMAGE_NOT_LVGL_MSG)
        self.refused_image(lvgl_image()[:11], IMAGE_NOT_LVGL_MSG)
        self.refused_image(lvgl_image()[:1], IMAGE_NOT_LVGL_MSG)
        self.refused_image(lvgl_image(magic=0x00), IMAGE_NOT_LVGL_MSG)
        self.refused_image(lvgl_image(magic=0x1A), IMAGE_NOT_LVGL_MSG)
        self.refused_image(b"\x89PNG\r\n\x1a\n" + bytes(40), IMAGE_NOT_LVGL_MSG)
        self.refused_image(b"BM" + bytes(60), IMAGE_NOT_LVGL_MSG)
        # Only the header: a valid one, with no pixels to go with it.
        self.refused_image(lvgl_image()[:12], image_size_msg(12, 44))

    def test_image_compressed_flag(self):
        # Compressed is the flag with its own message, whatever else is set with it.
        self.refused_image(lvgl_image(flags=0x0008), IMAGE_COMPRESSED_MSG)
        self.refused_image(lvgl_image(flags=0x0009), IMAGE_COMPRESSED_MSG)
        self.refused_image(lvgl_image(flags=0x0018), IMAGE_COMPRESSED_MSG)
        self.refused_image(lvgl_image(flags=0xFFFF), IMAGE_COMPRESSED_MSG)

    def test_image_flags_are_none_or_premultiplied(self):
        # No flag, and premultiplied alpha (0x0001, which LVGLImage.py sets for a premultiplied image), are an image
        # a descriptor can be made of and drawn in place. Allocated (0x0010) would have LVGL take the descriptor for
        # a draw buffer, and none of the others means anything to an image that is not.
        self.assertEqual(self.image(lvgl_image(flags=0x0000)), {"ok": True, "w": 4, "h": 4})
        self.assertEqual(self.image(lvgl_image(flags=0x0001)), {"ok": True, "w": 4, "h": 4})
        self.assertEqual(self.image(lvgl_image(w=5, h=3, cf=0x0E, flags=0x0001)), {"ok": True, "w": 5, "h": 3})
        self.assertEqual(self.image(lvgl_image(w=3, h=7, cf=0x14, flags=0x0001)), {"ok": True, "w": 3, "h": 7})
        for bit in (0x0002, 0x0004, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000,
                    0x8000):
            for flags in (bit, bit | 0x0001):
                with self.subTest(flags=hex(flags)):
                    self.refused_image(lvgl_image(flags=flags), image_flags_msg(flags))
        # What LVGL's own snapshot and draw buffers carry, and everything but compressed.
        self.refused_image(lvgl_image(flags=0x0030), image_flags_msg(0x0030))
        self.refused_image(lvgl_image(flags=0xFFF7), image_flags_msg(0xFFF7))
        # The message is the same for every format.
        self.refused_image(lvgl_image(cf=0x0E, flags=0x0010), image_flags_msg(0x0010))
        self.refused_image(lvgl_image(cf=0x14, flags=0x0020), image_flags_msg(0x0020))

    def test_image_size_against_the_screen(self):
        # 1 to 466 each way, and 466 passes.
        self.assertEqual(self.image(lvgl_image(w=1, h=1)), {"ok": True, "w": 1, "h": 1})
        self.assertEqual(self.image(lvgl_image(w=466, h=466)), {"ok": True, "w": 466, "h": 466})
        self.assertEqual(self.image(lvgl_image(w=466, h=2, cf=0x14)), {"ok": True, "w": 466, "h": 2})
        self.refused_image(lvgl_image(w=467, h=4), image_big_msg(467, 4))
        self.refused_image(lvgl_image(w=4, h=467), image_big_msg(4, 467))
        self.refused_image(lvgl_image(w=500, h=500), image_big_msg(500, 500))
        self.refused_image(lvgl_image(w=65535, h=65535, stride=2), image_big_msg(65535, 65535))
        self.refused_image(lvgl_image(w=0, h=4), image_empty_msg(0, 4))
        self.refused_image(lvgl_image(w=4, h=0), image_empty_msg(4, 0))
        self.refused_image(lvgl_image(w=0, h=0), image_empty_msg(0, 0))
        self.refused_image(lvgl_image(w=0, h=500), image_empty_msg(0, 500))

    def test_image_colour_formats(self):
        for cf in [0x00, 0x01, 0x06, 0x0F, 0x10, 0x11, 0x13, 0x15, 0x1B, 0xFF]:
            with self.subTest(cf=cf):
                self.refused_image(lvgl_image(cf=cf), image_format_msg(cf))
        for cf, size in [(0x12, 44), (0x14, 60), (0x0E, 28)]:
            with self.subTest(cf=cf):
                data = lvgl_image(cf=cf)
                self.assertEqual(len(data), size)
                self.assertTrue(self.image(data)["ok"])

    def test_image_size_must_match_its_header(self):
        self.refused_image(lvgl_image(drop=1), image_size_msg(43, 44))
        self.refused_image(lvgl_image(extra=1), image_size_msg(45, 44))
        self.refused_image(lvgl_image(drop=32), image_size_msg(12, 44))
        self.refused_image(lvgl_image(extra=1000), image_size_msg(1044, 44))
        # The alpha plane counts for RGB565A8: 12 + 32 + 16.
        self.assertTrue(self.image(lvgl_image(cf=0x14))["ok"])
        self.refused_image(lvgl_image(cf=0x14, drop=1), image_size_msg(59, 60))
        self.refused_image(lvgl_image(cf=0x14, drop=16), image_size_msg(44, 60))
        self.refused_image(lvgl_image(cf=0x14, extra=1), image_size_msg(61, 60))
        self.refused_image(lvgl_image(cf=0x0E, drop=1), image_size_msg(27, 28))
        self.refused_image(lvgl_image(cf=0x0E, extra=1), image_size_msg(29, 28))

    def test_image_stride(self):
        # A row may be padded, never short: stride >= w * bytes per pixel, the size following the stride.
        self.assertEqual(self.image(lvgl_image(stride=16)), {"ok": True, "w": 4, "h": 4})  # 12 + 16 * 4
        self.assertEqual(self.image(lvgl_image(cf=0x14, stride=16)), {"ok": True, "w": 4, "h": 4})  # 12 + 64 + 8 * 4
        # An odd stride: the alpha plane has 7 // 2 = 3 bytes a row.
        self.assertEqual(self.image(lvgl_image(w=3, cf=0x14, stride=7)), {"ok": True, "w": 3, "h": 4})
        self.assertEqual(self.image(lvgl_image(cf=0x0E, stride=9)), {"ok": True, "w": 4, "h": 4})
        self.refused_image(lvgl_image(stride=7), image_stride_msg(7, 4, 8))
        self.refused_image(lvgl_image(stride=0), image_stride_msg(0, 4, 8))
        self.refused_image(lvgl_image(cf=0x14, stride=7), image_stride_msg(7, 4, 8))
        self.refused_image(lvgl_image(cf=0x0E, stride=3), image_stride_msg(3, 4, 4))
        self.refused_image(lvgl_image(w=466, h=1, stride=931), image_stride_msg(931, 466, 932))
        # A stride that would make the size overflow a 16-bit count is still checked against the bytes there.
        self.refused_image(lvgl_image(w=4, h=4, stride=65535)[:100], image_size_msg(100, 12 + 65535 * 4))

    def test_image_checks_come_in_a_fixed_order(self):
        # magic, compressed, other flags, no pixels, too big, colour format, stride, size.
        bad_all = struct.pack("<BBHHHHH", 0x18, 0x10, 0x0018, 500, 500, 1, 0) + bytes(3)
        self.refused_image(bad_all, IMAGE_NOT_LVGL_MSG)
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x10, 0x0018, 500, 500, 1, 0) + bytes(3), IMAGE_COMPRESSED_MSG)
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x10, 0x0010, 0, 500, 1, 0) + bytes(3), image_flags_msg(0x0010))
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x10, 0x0000, 0, 500, 1, 0) + bytes(3),
                           image_empty_msg(0, 500))
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x10, 0x0000, 500, 500, 1, 0) + bytes(3),
                           image_big_msg(500, 500))
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x10, 0x0000, 4, 4, 1, 0) + bytes(3), image_format_msg(0x10))
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x12, 0x0000, 4, 4, 1, 0) + bytes(3),
                           image_stride_msg(1, 4, 8))
        self.refused_image(struct.pack("<BBHHHHH", 0x19, 0x12, 0x0000, 4, 4, 8, 0) + bytes(3), image_size_msg(15, 44))

    def test_image_header_is_little_endian_and_reserved_is_ignored(self):
        data = lvgl_image(w=0x0104, h=1, stride=0x0208)  # 260 x 1, stride 520
        self.assertEqual(self.image(data), {"ok": True, "w": 260, "h": 1})
        self.assertTrue(self.image(lvgl_image(reserved=0xFFFF))["ok"])

    def test_real_lvglimage_outputs_pass(self):
        # Not this file's own headers: what LVGL 9.5.0's scripts/LVGLImage.py writes when it is run as the guide tells Muse to
        # (--ofmt BIN --compress NONE, once for each of --cf RGB565, RGB565A8 and A8) on three PNGs of 5x3, 4x4 and 7x2
        # pixels (the controller's ruling from Task 7's review). Flags 0, and the size and stride the check wants.
        images = ROOT / "tests/muse_apps_fixtures/lvgl_images"
        formats = {"RGB565": 0x12, "RGB565A8": 0x14, "A8": 0x0E}
        sizes = {"a5x3": (5, 3), "b4x4": (4, 4), "c7x2": (7, 2)}
        self.assertEqual(sorted(p.relative_to(images).as_posix() for p in images.rglob("*")),
                         sorted([*formats, *(f"{cf}/{name}.bin" for cf in formats for name in sizes)]))
        for cf, code in formats.items():
            for name, (w, h) in sizes.items():
                with self.subTest(cf=cf, image=name):
                    data = (images / cf / f"{name}.bin").read_bytes()
                    magic, got_cf, flags, got_w, got_h = struct.unpack_from("<BBHHH", data)
                    self.assertEqual((magic, got_cf, flags, got_w, got_h), (0x19, code, 0, w, h))
                    self.assertEqual(self.image(data), {"ok": True, "w": w, "h": h})
                    # The same bytes cut short or with one more are not an image: it is the size that passed them.
                    self.refused_image(data[:-1], image_size_msg(len(data) - 1, len(data)))
                    self.refused_image(data + b"\0", image_size_msg(len(data) + 1, len(data)))

    # ---- The C cases ----

    def test_c_contracts(self):
        # NULL arguments, err missing or too small, an err cut on a character boundary, no folder, and clear and
        # free taking NULL.
        self.run_case("contracts")

    def test_messages_json_is_psram_owned_by_the_manifest_and_never_leaks(self):
        # R7.8: one heap_caps_malloc of MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, last of all; freed by the caller's
        # free_fn (NULL: free); a refused allocation is "manifest: out of memory" with the manifest zero.
        self.run_case("memory")

    def test_pkg_free_frees_every_blob_and_the_messages(self):
        self.run_case("pkg_free")

    def test_the_compile_message_names_the_file_and_shows_its_lines(self):
        # "compile failed: ", Lua's message and the lines around the one it names, for the install pipeline and
        # apphost's check alike: a file of the package is matched by its whole path at the first ":<digits>:", and
        # anything else (no such file, no line, a line the file lacks) is shown as it is; cut at cap on a character.
        self.run_case("compile_message")

    def test_the_compile_order_is_main_first_then_the_other_lua_files(self):
        # Fix round 1, E: apps_pkg_compile_order, the one order of the install's compile and apphost's check.
        self.run_case("compile_order")

    def test_the_compiled_chunk_check_is_the_installs_wording(self):
        # Fix round 1, F: apps_pkg_check_source, which the install pipeline's step 3 and apphost's loading share.
        self.run_case("check_source")

    def test_text_that_leaves_the_runtime_is_cleaned_by_one_rule(self):
        # Fix round 1, A: apps_text_clean (the runtime's messages and the compile message share it): each control byte
        # but tab and a kept newline, DEL, each C1 character and each byte of broken UTF-8 is one '?'; U+00A0 is text.
        self.run_case("text_clean")

    def test_every_prefix_of_a_manifest_is_refused(self):
        # A text is its first len bytes: cut anywhere, each is an error and nothing is read past the end.
        self.run_case("truncations")

    def test_generated_manifests_get_the_verdict_of_their_parts(self):
        # 20000 manifests built from mostly valid values: each of nine keys absent, given or given twice, unknown
        # keys, any order, any whitespace. It passes when the last value of every key is valid and id, name and api
        # are there, and not otherwise.
        self.run_case("combos", timeout=120)

    def test_damaged_manifests_are_refused_cleanly_or_still_valid(self):
        # 30000 manifests with bytes changed, dropped, inserted, repeated, cut, or a token put in.
        self.run_case("fuzz", timeout=120)


if __name__ == "__main__":
    unittest.main()
