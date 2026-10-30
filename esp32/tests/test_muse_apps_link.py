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

"""Link's app.* commands (main/apps_link.c, CONFIG_MUSE_APPS): Muse's way to the gadget's apps.

tests/muse_apps_link_harness.c compiles the whole of apps_link.c with the real cJSON against fakes of the apps
(muse_apps_install, muse_apps_send, muse_apps_write_guide, app.control's calls, muse_apps_id_valid,
muse_apps_json_object_valid, muse_apps_set_vm_ops), the session
(noise_ctrl_send_command_result), the VM file reader and writer (vm_file.h) and the PSRAM heap: link.register's entries and
their size, the install, app.api and app.control handlers (never waiting, a request copied and freed once the result is
sent, also when the session has ended; app.control's actions checked as typed), and vm_file behind the apps' VM ops. The
wiring is checked in the firmware's own sources: advertised, dispatched, started and built only with the option.

The guide Muse reads (components/muse/apps/apps_guide.md, embedded in the firmware and written to the VM by app.api) is
checked against the bindings: every function of every apps_<module>_funcs[] table, and every widget's properties and
events, is in its API reference, and nothing else is (D4, E6). The skill (skills/gadget-muse-apps/SKILL.md) is the guide,
verbatim, under the repo's six skill headings (D21). Needs the cJSON sources: CJSON_SOURCE_DIR (~/.cache/muse-cjson), or an
IDF build's managed_components.
"""

import codecs
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_wiring import c_function, open_conditions, strip_c_comments
from test_muse_apps_rt import MEASURED_RATE, header_number

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
MUSE = ROOT / "components/muse"
TESTS = ROOT / "tests"
HARNESS = TESTS / "muse_apps_link_harness.c"

# The room link.register has for the apps' commands, in bytes. The registration's size costs the boot's lowest internal and DMA
# free: it is sent at the moment the Link's setup sets that floor, its Wi-Fi TX buffers in flight (DMA RAM), so the entries are a
# sentence each and app.api's guide carries the rest (I2). The budget is the trimmed entries' size as this test prints it (1,299
# bytes on 2026-10-30) rounded up to the next 128, so that growing them is a deliberate change to this number. The first budget was
# what fit under the 7 KB cap: 7,168 less the 2,669 bytes of the board's `sent link.register (N bytes)` boot line without the apps.
APPS_REGISTER_BUDGET = 1408

INSTALL_DESCRIPTION = "Install and run the app in workspace/apps/<id>/; returns errors, logs and a widget dump."
API_DESCRIPTION = "Call before writing an app: writes the app guide to workspace/apps/_guide/ and returns the gadget's limits."
CONTROL_DESCRIPTION = "Manage gadget apps; app.api's guide says what each action returns."
SEND_DESCRIPTION = "Send one of an app's messages to it, starting it if needed; returns its reply."

GUIDE = MUSE / "apps/apps_guide.md"
SKILLS = ROOT.parent / "skills"
SKILL = SKILLS / "gadget-muse-apps/SKILL.md"
APPS = MUSE / "apps"
TIMER = APPS / "examples/timer"
# The headings every skill of the repo has, in order (skills/*/SKILL.md; D21).
SKILL_HEADINGS = ["Identify the Device", "Prerequisites", "Workflow", "Verify the Result", "Limits", "Sources"]

# What apps_link.c and the headers it includes need of ESP-IDF and LVGL, as the little they use.
STAGED = {
    "sdkconfig.h": ("#pragma once\n#define CONFIG_MUSE_APPS 1\n#define CONFIG_SPIRAM 1\n"
                    "#define CONFIG_HOMEHUB_DAEMON_REQUESTS 1\n"),
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n",
    "lvgl.h": "#pragma once\ntypedef struct lv_obj lv_obj_t;\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_heap_caps.h": ("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define MALLOC_CAP_8BIT (1u << 2)\n"
                        "#define MALLOC_CAP_SPIRAM (1u << 10)\n"
                        "void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);\nvoid heap_caps_free(void *ptr);\n"),
}


class LinkCommands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cjson = apps_host.cjson_object()
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        staged = Path(tmp.name) / "staged"
        staged.mkdir()
        for name, text in STAGED.items():
            (staged / name).write_text(text)
        cls.harness = apps_host.build("muse_apps_link_harness", [HARNESS, MAIN / "apps_link.c"], tmp.name,
                                      include_dirs=[staged, MAIN, MUSE, apps_host.CJSON_DIR], objects=[cjson])

    def run_case(self, case):
        try:
            done = apps_host.run(self.harness, case, timeout=60)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than 60 s")
        self.assertEqual(done.returncode, 0, done.stdout[-4000:] + done.stderr[-4000:])
        return done.stdout

    def test_register_entries_fit_budget(self):
        # D11, re-asserted by Tasks 24 and 25: all four of the apps' entries, app.send's and app.control's snapshot scale included,
        # are advertised, and fit the budget (the headroom rule would leave app.control out when they didn't; it still dispatches,
        # and app.api's payload names it).
        stdout = self.run_case("register_entries")
        printed = {}
        for line in stdout.splitlines():
            tag, _, text = line.partition(" ")
            if tag == "@register":
                self.assertNotIn(tag, printed, stdout)
                printed[tag] = text
        self.assertEqual(sorted(printed), ["@register"], stdout)
        everything = json.loads(printed["@register"])
        self.assertEqual(sorted(everything), ["app.api", "app.control", "app.install", "app.send"])
        size = len(printed["@register"].encode())
        sizes = ", ".join(f"{name} {len(json.dumps({name: entry}, separators=(',', ':')).encode()) - 2}"
                          for name, entry in sorted(everything.items()))
        print(f"link.register: the four apps' entries are {size} of {APPS_REGISTER_BUDGET} bytes ({sizes}); all advertised",
              file=sys.stderr, flush=True)
        self.assertLessEqual(size, APPS_REGISTER_BUDGET)
        self.assertEqual(everything["app.install"], {
            "description": INSTALL_DESCRIPTION,
            "required": {"id": {"type": "string", "description": "App id (the folder name)."}},
            "optional": {"run": {"type": "boolean", "description": "Run it after installing (default true)."}},
            "timeout_ms": 120000,
        })
        self.assertEqual(everything["app.api"], {"description": API_DESCRIPTION, "required": {}, "optional": {},
                                                 "timeout_ms": 30000})
        control = everything["app.control"]
        self.assertEqual(control["description"], CONTROL_DESCRIPTION)
        self.assertEqual(sorted(control["required"]), ["action"])
        self.assertEqual(sorted(control["optional"]), ["id", "scale"])
        self.assertEqual(control["required"]["action"]["type"], "string")
        self.assertEqual(control["optional"]["id"]["type"], "string")
        self.assertEqual(control["optional"]["scale"], {"type": "integer", "description": "1 full size (default), 2 half"})
        self.assertEqual(control["timeout_ms"], 30000)
        send = everything["app.send"]
        self.assertEqual(send["description"], SEND_DESCRIPTION)
        self.assertEqual({k: v["type"] for k, v in send["required"].items()}, {"id": "string", "event": "string"})
        self.assertEqual({k: v["type"] for k, v in send["optional"].items()}, {"data": "object", "show": "boolean"})
        self.assertEqual(send["timeout_ms"], 10000)
        for name, entry in everything.items():
            for kind in ("required", "optional"):
                for param, spec in entry[kind].items():
                    with self.subTest(name, param=param):
                        self.assertTrue(spec.get("description"), "Muse reads every parameter's description")
        self.assertNotRegex(printed["@register"], re.compile("hatch", re.I), "say Muse, never Hatch")

    def test_install_handler_copies_request(self):  # and frees it once the result is sent, sent or dropped
        self.run_case("install_handler")
        self.run_case("handles")

    def test_control_actions_validated(self):  # the action as typed, the id for all but list, the sync and async ones, scale
        self.run_case("control_handler")

    def test_api_is_async_and_frees_its_context(self):  # D2: _async, the request copied, freed once done has sent
        self.run_case("api_handler")

    def test_send_data_forms(self):  # D25: an object printed compact, a JSON string as it is; objects only, 4096 bytes at most
        self.run_case("send_data_forms")

    def test_send_done_frees_context(self):  # the request copied, freed once done has sent: also for an ended session
        self.run_case("send_done_frees_context")

    def test_send_failures_pass_through(self):  # D25: not installed and no reply, from a fake muse_apps_send, sent whole
        self.run_case("send_failures_pass_through")

    def test_vm_ops_put_vm_file_behind_the_apps(self):  # D3: the deadline forwarded; vm_file's answers as the apps' codes
        self.run_case("vm_ops_are_vm_file")

def guide_section(text, heading):
    """The text of the guide's "## heading" section, its heading line included, up to the next "## "."""
    start = text.index(f"\n## {heading}\n") + 1
    end = text.find("\n## ", start)
    return text[start:end + 1 if end >= 0 else len(text)]


def guide_body(text):
    """The guide without its title line: what the skill holds verbatim."""
    first, _, rest = text.partition("\n")
    assert first.startswith("# "), "the guide starts with its title"
    return rest.lstrip("\n")


def c_literals(source):
    """The string literals of a C source, as the compiler has them: comments and character literals left out, adjacent
    literals joined, escapes decoded (UTF-8 bytes as text)."""
    literals, run, end = [], None, 0
    for m in re.finditer(r'"((?:\\.|[^"\\\n])*)"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', source, re.S):
        if m.group(1) is None:
            continue  # a comment or a character literal
        text = codecs.escape_decode(m.group(1).encode())[0].decode("utf-8", "replace")
        between = re.sub(r"/\*.*?\*/|//[^\n]*", "", source[end:m.start()], flags=re.S)
        if run is not None and between.strip() == "":
            run += text
        else:
            if run is not None:
                literals.append(run)
            run = text
        end = m.end()
    if run is not None:
        literals.append(run)
    return literals


def c_table(source, name):
    """The quoted names of a C array `name[] = {...};` (a luaL_Reg table's or a list of strings), in order."""
    match = re.search(rf"\b{name}\[\]\s*=\s*\{{(.*?)\}};", source, re.S)
    assert match, f"no {name}[] in the source"
    return re.findall(r'"(\w+)"', match.group(1))


class GuideAndSkill(unittest.TestCase):
    def test_guide_matches_bindings(self):
        # D4, E6 (a): every <module>.<fn> of every apps_<module>_funcs[] in components/muse/apps/*.c, json and string's split
        # and trim among them, is a line of the guide's API reference that starts "- `<module>.<fn>(" or "- `<module>.<fn>{",
        # and the reference has no such line for anything else.
        bindings = set()
        for path in sorted(APPS.glob("*.c")):
            source = strip_c_comments(path.read_text())
            for module in re.findall(r"static const luaL_Reg apps_(\w+)_funcs\[\]", source):
                bindings |= {f"{module}.{fn}" for fn in c_table(source, f"apps_{module}_funcs")}
        self.assertIn("json.decode", bindings)
        self.assertIn("string.split", bindings)
        self.assertIn("sys.log", bindings)
        self.assertGreater(len(bindings), 40, "the tables weren't read")
        guide = GUIDE.read_text()
        reference = guide_section(guide, "API reference")
        documented = {f"{m}.{f}" for m, f in re.findall(r"^- `(\w+)\.(\w+)[({]", reference, re.M)}
        self.assertEqual(sorted(bindings - documented), [], "bindings the guide's API reference leaves out")
        self.assertEqual(sorted(documented - bindings), [], "the guide's API reference names functions the gadget doesn't have")
        # Each documented once, so a reader finds one answer.
        lines = re.findall(r"^- `(\w+\.\w+)[({]", reference, re.M)
        self.assertEqual(sorted(name for name in set(lines) if lines.count(name) > 1), [], "documented twice")

        # (b): each widget kind's line, "- `ui.<kind>` properties: ...; events: ...", has apps_ui.c's tables, in their order.
        ui = strip_c_comments((APPS / "apps_ui.c").read_text())
        kinds = c_table(ui, "apps_ui_kinds")
        events = c_table(ui, "apps_ui_events")
        self.assertEqual(kinds, ["label", "button", "box", "arc", "bar", "image"])
        lines = {kind: (props, evs) for kind, props, evs in
                 re.findall(r"^- `ui\.(\w+)` properties: ([^;\n]+); events: ([^\n]+)$", reference, re.M)}
        self.assertEqual(sorted(lines), sorted(kinds), "one properties line per kind")
        for kind in kinds:
            with self.subTest(kind):
                props, evs = lines[kind]
                self.assertEqual(props.rstrip(".").split(", "), c_table(ui, f"apps_ui_props_{kind}"))
                self.assertEqual(evs.rstrip(".").split(", "), events)

        # Methods and constants are documented, not set-compared (D4).
        for method in c_table(ui, "apps_ui_methods"):
            self.assertRegex(reference, rf"`w:{method}[({{]", f"w:{method}")
        svc = strip_c_comments((APPS / "apps_svc.c").read_text())
        for method in c_table(svc, "apps_timer_methods"):
            self.assertRegex(reference, rf"`h:{method}\(", f"h:{method}")
        for constant in ("ui.W", "ui.H", "ui.R", "ui.SAFE", "json.null", "sys.api"):
            self.assertIn(f"`{constant}`", reference, constant)

    def test_skill_is_the_guide(self):
        # D21: front matter (name, description), the six headings every skill of the repo has, and the guide verbatim under
        # "## Workflow". Markdown only, and Muse throughout.
        skill = SKILL.read_text()
        match = re.match(r"---\n(.*?)\n---\n", skill, re.S)
        self.assertIsNotNone(match, "no YAML front matter")
        front, body = match.group(1), skill[match.end():]
        self.assertRegex(front, r"(?m)^name: gadget-muse-apps$")
        self.assertRegex(front, r"(?m)^description: \S")
        guide = GUIDE.read_text()
        text = guide_body(guide)
        self.assertIn(text, body, "the skill doesn't hold the guide verbatim")
        own = body.replace(text, "")
        self.assertEqual(re.findall(r"^## (.+)$", own, re.M), SKILL_HEADINGS)
        self.assertLess(body.index("\n## Workflow\n"), body.index(text))
        self.assertLess(body.index(text), body.index("\n## Verify the Result\n"))
        for heading in SKILL_HEADINGS:  # none of the guide's own headings could be taken for one of the skill's
            self.assertNotIn(f"\n## {heading}\n", "\n" + text)
        self.assertEqual(sorted(p.name for p in SKILL.parent.iterdir()), ["SKILL.md"], "Markdown only")
        for name, words in (("the guide", guide), ("the skill", skill)):
            with self.subTest(name):
                self.assertNotRegex(words, re.compile("hatch", re.I), "say Muse, never Hatch")
        # One entry in the catalog, in its format, and the count line counts it.
        catalog = (SKILLS / "CATALOG.md").read_text()
        self.assertEqual(catalog.count("(gadget-muse-apps/SKILL.md)"), 1)
        self.assertRegex(catalog, r"(?m)^- \[[^\]]+\]\(gadget-muse-apps/SKILL\.md\) — \S")
        count = int(re.match(r"# Skill Catalog\n\n(\d+) active skills", catalog).group(1))
        self.assertEqual(count, len([p for p in SKILLS.iterdir() if (p / "SKILL.md").is_file()]))
        self.assertEqual(count, len(re.findall(r"^- \[[^\]]+\]\(gadget-[\w-]+/SKILL\.md\)", catalog, re.M)))

    def test_guide_and_timer_are_embedded(self):
        # The firmware carries the guide and Task 19's timer (EMBED_FILES under the option), and app.api writes those bytes:
        # the symbols apps_control.c reads are the ones the build makes of the three files' names, which no other embedded file
        # of the component shares.
        cmake = (MUSE / "CMakeLists.txt").read_text()
        block = cmake[cmake.index("    if(CONFIG_MUSE_APPS)\n"):]
        block = block[:block.index("    endif()")]
        files = ["apps/apps_guide.md", "apps/examples/timer/manifest.json", "apps/examples/timer/main.lua"]
        self.assertIn('list(APPEND embed ' + " ".join(f'"{f}"' for f in files) + ")", block)
        embedded = re.findall(r'list\(APPEND embed ([^)]*)\)', cmake)
        names = [Path(f).name for line in embedded for f in re.findall(r'"([^"]+)"', line)]
        for f in files:
            self.assertEqual(names.count(Path(f).name), 1, f"another embedded file makes {f}'s symbols")
        control = (APPS / "apps_control.c").read_text()
        for f in files:
            symbol = "_binary_" + re.sub(r"[^A-Za-z0-9]", "_", Path(f).name)
            for end in ("start", "end"):
                self.assertIn(f'__asm__("{symbol}_{end}")', control)
        for f in files:
            self.assertTrue((MUSE / f).is_file(), f)
        self.assertEqual(sorted(p.name for p in TIMER.iterdir()), ["main.lua", "manifest.json"], "the timer is two files")

    def test_common_errors_are_the_firmwares_words(self):
        # The guide's "Common errors" quote the gadget's messages, each item "- `<message>`: <the fix>": every piece of each
        # message between its <placeholders>, "..." and numbers (which the code may spell through a macro), and between the
        # prefixes the code puts in front (": "), is inside one string literal of the apps' sources (comments don't count),
        # so a paraphrase fails.
        literals = [text for p in [*sorted(APPS.glob("*.[ch]")), MUSE / "muse_apps.h", MAIN / "apps_link.c", MAIN / "vm_file.c",
                                   MAIN / "vm_file.h"]
                    for text in c_literals(p.read_text())]
        self.assertIn("busy: another install is running", literals)  # a #define's
        self.assertNotIn("The one install pipeline of Muse apps", "\n".join(literals))  # a comment's
        corpus = "\n".join(literals)
        section = guide_section(GUIDE.read_text(), "Common errors")
        quoted = re.findall(r"^- `([^`\n]+)`", section, re.M)
        self.assertGreater(len(quoted), 30, "the section quotes too few messages")
        for message in quoted:
            for piece in re.split(r"<[^>]+>|\.\.\.|…|\d+|: ", message):
                piece = piece.strip(" '\":;,.()%[]{}")
                if len(piece) >= 6:
                    with self.subTest(message, piece=piece):
                        self.assertIn(piece, corpus, "not the firmware's words")

    def test_guide_says_what_to_do_when_a_snapshot_has_no_room(self):
        # N1: a snapshot is refused before anything is drawn when the gadget has no room for it. The guide's Common errors quote each
        # of the three refusals as muse_apps.h has them, with what to do, and its snapshot entry says that it happens.
        header = (MUSE / "muse_apps.h").read_text()
        section = " ".join(guide_section(GUIDE.read_text(), "Common errors").split())
        for name, fix in (("MUSE_APPS_SNAPSHOT_TRY_SCALE_2", "`scale: 2`"), ("MUSE_APPS_SNAPSHOT_STOP_APP", "stop it with `app.control`"),
                          ("MUSE_APPS_SNAPSHOT_NO_MEMORY", "try again in a few seconds")):
            with self.subTest(name):
                match = re.search(rf'^#define {name} (?:\\\n\s*)?"([^"]+)"', header, re.M)
                self.assertIsNotNone(match, f"no #define {name} in muse_apps.h")
                self.assertRegex(section, rf"- `{re.escape(match.group(1))}`: [^`]*{re.escape(fix)}")
        guide = " ".join(GUIDE.read_text().split())
        self.assertIn("it is refused before anything is drawn (`out_of_memory`), and a full-size one says when `scale: 2` would fit",
                      guide)

    def test_guide_says_what_to_do_when_a_launch_has_no_room_beside_the_app_that_runs(self):
        # N3: a launch beside another app is refused up front when it can't fit even once that app ends, or when its files don't fit
        # beside it now. The guide's Common errors quote both as apps_pkg.h has them (%s an <id>, %u an <n>), with what to do, and
        # its memory paragraph says that the app that runs is left running.
        header = (APPS / "apps_pkg.h").read_text()
        section = " ".join(guide_section(GUIDE.read_text(), "Common errors").split())
        for name, fix in (("APPS_MSG_SHORT_ONCE_ENDED", "lower `memory_kb` or the images. The app that runs keeps running."),
                          ("APPS_MSG_SHORT_BESIDE", "stop it with `app.control`, then launch the app again")):
            with self.subTest(name):
                match = re.search(rf'^#define {name} (?:\\\n\s*)?"([^"]+)"', header, re.M)
                self.assertIsNotNone(match, f"no #define {name} in apps_pkg.h")
                quoted = match.group(1).replace("%s", "<id>").replace("%u", "<n>")
                self.assertRegex(section, rf"- `{re.escape(quoted)}`: [^`]*{re.escape(fix)}")
        guide = " ".join(GUIDE.read_text().split())
        self.assertIn("A launch while another app runs counts what that app frees when it ends: it is refused up front, with the app that "
                      "runs left running, when it wouldn't fit even then, or when its files don't fit beside that app now.", guide)

    def test_guide_quotes_the_cpu_limits_as_the_firmware_has_them(self):
        # Task 27 set the budgets from the board's measured speed (D12: a test pins each number the guide quotes; the skill
        # holds the guide verbatim). Each sentence with its numbers made from the headers, with thousands separators.
        call = header_number("apps_vm.h", "APPS_BUDGET_CALL")
        start = header_number("apps_vm.h", "APPS_BUDGET_START")
        wall_s = header_number("apps_vm.h", "APPS_WALL_MS") // 1000
        busy = header_number("apps_rt.h", "APPS_DUTY_BUSY_PCT")
        kill_s = header_number("apps_rt.h", "APPS_DUTY_KILL_WINDOWS")
        hung_s = header_number("apps_task.h", "APPS_TASK_HUNG_MS") // 1000
        call_s = round(call / MEASURED_RATE, 1)  # what CALL is in seconds of the simplest loop on the gadget
        guide = " ".join(GUIDE.read_text().split())
        for sentence in (
                f"Each callback has a budget of Lua VM instructions ({call:,}, about {call_s:g} s on the gadget; {start:,} for "
                f"main.lua's top level and `start()`) and {wall_s} s of time",
                f"an app that uses more than {busy}% of the CPU for {kill_s} s is stopped",
                f"A call that hasn't returned after {hung_s} s marks the app hung",
                f"A message has the budget of any callback ({call:,} instructions, {wall_s} s)",
                # M4: what is true. The VM stops Lua; C that runs long is the supervisor's, and the gadget restarts for neither.
                "The gadget stops an app that runs away; it doesn't restart for one.",
                "a call held up inside the gadget's own code is bounded by a supervisor instead."):
            with self.subTest(sentence):
                self.assertIn(sentence, guide)
        self.assertNotIn("can't hang or crash", guide)
        # No other count of instructions is quoted.
        quoted = {int(n.replace(",", "")) for n in re.findall(r"\b(\d{1,3}(?:,\d{3})+) (?:VM )?instructions", guide)}
        quoted |= {int(n.replace(",", "")) for n in re.findall(r"instructions \((\d{1,3}(?:,\d{3})+)", guide)}
        self.assertEqual(quoted - {call, start}, set())

    def test_agents_md_and_kconfig_quote_the_cpu_limits_as_the_firmware_has_them(self):
        # The same numbers in esp32/AGENTS.md's "Muse apps" section and the option's Kconfig help (D12), and the speed they come
        # from as test_muse_apps_rt.py's MEASURED_RATE has it.
        call = header_number("apps_vm.h", "APPS_BUDGET_CALL")
        start = header_number("apps_vm.h", "APPS_BUDGET_START")
        duty = header_number("apps_rt.h", "APPS_DUTY_INSTR_PER_S")
        wall_s = header_number("apps_vm.h", "APPS_WALL_MS") // 1000
        busy = header_number("apps_rt.h", "APPS_DUTY_BUSY_PCT")
        throttle = header_number("apps_rt.h", "APPS_DUTY_THROTTLE_WINDOWS")
        kill = header_number("apps_rt.h", "APPS_DUTY_KILL_WINDOWS")
        hung_s = header_number("apps_task.h", "APPS_TASK_HUNG_MS") // 1000
        rate = f"{MEASURED_RATE / 1e6:.1f} M instructions a second"
        agents = (ROOT / "AGENTS.md").read_text()
        section = " ".join(agents[agents.index("\n## Muse apps\n"):agents.index("\n## Configuration gotchas\n")].split())
        for sentence in (
                f"a callback may run {call:,} Lua VM instructions and `main.lua`'s top level with `start()` {start:,}, each in "
                f"at most {wall_s} s; the 1.75C runs about {rate} of the simplest loop",
                f"so that is about {call / MEASURED_RATE:.1f} s and {start / MEASURED_RATE:.1f} s",
                f"A second with more than {duty:,} instructions or {busy * 10} ms of calls is over {busy}% of the CPU: {throttle} "
                f"such seconds in a row throttle the app's timers, {kill} stop the app, and a call that hasn't returned after "
                f"{hung_s} s marks it hung"):
            with self.subTest(sentence):
                self.assertIn(sentence, section)
        kconfig = " ".join((MUSE / "Kconfig").read_text().split())
        self.assertIn(f"runs about {rate}", kconfig)

    def test_agents_md_and_kconfig_quote_the_memory_and_storage_limits_as_the_code_has_them(self):
        # D12 for the code constants esp32/AGENTS.md's "Muse apps" section and the option's Kconfig help quote: the stacks, the
        # snapshot's admission, the partitions, the launcher's rows, the path limit and the Link's TX margin. The measured
        # numbers beside them (bytes free, image and RAM costs) are measurements and aren't pinned.
        def kb(n):
            self.assertEqual(n % 1024, 0)
            return n // 1024

        def define_kb(path, name):  # `#define NAME (n * 1024)`
            match = re.search(rf"^#define\s+{name}\s+\((\d+) \* 1024\)", path.read_text(), re.M)
            self.assertIsNotNone(match, f"no #define {name} in {path.name}")
            return int(match.group(1))

        task_stack = header_number("apps_task.c", "APPS_TASK_STACK")
        io_stack = header_number("apps_io.h", "APPS_IO_STACK")
        reserve_mb = define_kb(APPS / "apps_pkg.h", "APPS_RAM_RESERVE") / 1024
        # N1: the snapshot's one block at each scale (apps_snapshot.c's block_of, for the 466 x 466 screen), the encoder's
        # allowance and the app task's wait for it.
        side = header_number("apps_snapshot.h", "APPS_SNAPSHOT_SIDE")
        jpeg_max = define_kb(APPS / "apps_snapshot.h", "APPS_SNAPSHOT_JPEG_MAX") * 1024
        encoder_kb = define_kb(APPS / "apps_snapshot.h", "APPS_SNAPSHOT_ENCODER_BYTES")
        screen = side * side * 2
        block1 = max(screen, (screen + 15) // 16 * 16 + jpeg_max)
        half = (side // 2) * (side // 2) * 2
        block2 = max(screen, (half + 15) // 16 * 16 + jpeg_max)
        wait_s = header_number("apps_port.h", "APPS_PORT_SNAPSHOT_WAIT_MS") / 1000
        tx_margin_kb = define_kb(MAIN / "noise_tunnel_internal.h", "NOISE_TX_DMA_RESERVE_BYTES")
        rows = header_number("apps_lv.h", "APPS_LV_LAUNCHER_ROWS")
        path_max = header_number("apps_pkg.h", "APPS_PATH_MAX")
        table = {}
        for line in (ROOT / "partitions_muse_apps.csv").read_text().splitlines():
            cells = [c.strip() for c in line.split(",")]
            if cells[0] in ("apps", "apps_nvs"):  # the two the overlay adds (offset and size in hex)
                table[cells[0]] = (int(cells[3], 16), int(cells[4], 16))
        apps_offset, apps_size = table["apps"]
        nvs_offset, nvs_size = table["apps_nvs"]
        self.assertEqual(apps_size % (1024 * 1024), 0)
        words = {4: "four"}
        agents = (ROOT / "AGENTS.md").read_text()
        section = " ".join(agents[agents.index("\n## Muse apps\n"):agents.index("\n## Configuration gotchas\n")].split())
        for phrase in (
                f"lists the {words[rows]} most recently used apps",
                f"PSRAM holds the app task's {kb(task_stack)} KB stack",
                f"free of {task_stack:,}",
                f"the I/O worker's {kb(io_stack)} KB stack",
                f"of {io_stack:,}, and the LVGL task",
                f"JPEG's room: {block1:,} bytes at scale 1 and {block2:,} at scale 2, with {encoder_kb} KB",
                f"waits for the snapshot, up to {wait_s:g} s",
                f"apps always leave {reserve_mb:g} MB of PSRAM to the rest of the gadget",
                f"under the Link's {tx_margin_kb} KB TX margin",
                f"`apps`, a {apps_size // (1024 * 1024)} MB FAT at `{apps_offset:#X}`".replace("0X", "0x"),
                f"`apps_nvs`, {kb(nvs_size)} KB at `{nvs_offset:#X}`".replace("0X", "0x"),
                f"at most {path_max} characters"):
            with self.subTest(phrase):
                self.assertIn(phrase, section)
        kconfig = " ".join((MUSE / "Kconfig").read_text().split())
        for phrase in (f"and a {kb(task_stack)} KB task stack",
                       f"block of {block1:,} bytes (scale 1) or {block2:,} (scale 2), taken only with that, {encoder_kb * 1024:,} "
                       f"bytes for the encoder and the {reserve_mb:g} MB free",
                       f"the apps always leave {reserve_mb:g} MB of it free for the rest of the firmware",
                       f"{apps_size // (1024 * 1024)} MB for the apps' files and {kb(nvs_size)} KB for what they save"):
            with self.subTest(phrase):
                self.assertIn(phrase, kconfig)

    def test_guide_says_what_room_an_app_has(self):
        # C1's (d): an app's files and its heap share free PSRAM, less the APPS_RAM_RESERVE the apps leave to the rest of the gadget;
        # app.api derives that room from free PSRAM (limits.room_kb) rather than quoting it, and the guide says that the largest app
        # and the largest heap don't both fit with Muse's session up, and that updating the app that runs holds its files twice (D12:
        # the numbers are the headers').
        pkg = (APPS / "apps_pkg.h").read_text()
        app = re.search(r"^#define APPS_APP_MAX \((\d+) \* 1024 \* 1024\)", pkg, re.M)
        self.assertIsNotNone(app)
        self.assertRegex(pkg, r"(?m)^#define APPS_RAM_RESERVE \(1024 \* 1024\)", "the reserve the guide's words rest on")
        heap = re.search(r"config MUSE_APPS_HEAP_MAX_KB\n(?:\s+.*\n)*?\s+default (\d+)", (MUSE / "Kconfig").read_text())
        self.assertIsNotNone(heap)
        control = (APPS / "apps_control.c").read_text()
        self.assertIn('"room_kb"', control)
        self.assertIn("APPS_RAM_RESERVE", control)
        guide = " ".join(GUIDE.read_text().split())
        for sentence in ("`app.api`'s `limits.room_kb` is what an app's files and its `memory_kb` may take together right now",
                         f"the {app.group(1)} MB of files and a {heap.group(1)} KB heap don't both fit",
                         "installing the app that runs again needs room for two copies of its files"):
            with self.subTest(sentence):
                self.assertIn(sentence, guide)

    def test_guide_says_what_the_asleep_warning_means(self):
        # Task 26: an install onto a dark screen warns "layout not checked: the screen was asleep", the firmware's MSG_ASLEEP:
        # the overflow check didn't run, and a snapshot (which wakes the screen) checks the layout. The Common errors entry
        # quotes it (test_common_errors_are_the_firmwares_words checks the words).
        task = (APPS / "apps_task.c").read_text()
        self.assertIn('#define MSG_ASLEEP "layout not checked: the screen was asleep"', task)
        section = " ".join(guide_section(GUIDE.read_text(), "Common errors").split())
        self.assertRegex(section, r"- `layout not checked: the screen was asleep`: [^`]*didn't check[^`]*`app\.control` "
                                  r"`snapshot`[^`]*wakes")

    def test_guide_states_the_layout_and_alarm_facts(self):
        # Facts an app goes wrong without, with no error to tell it (fix round 1, A and B), each still true of the code and said
        # in the guide in words a reader finds: the guide's text for each must match every pattern of its fact.
        lv = (APPS / "apps_lv.c").read_text()
        ui = (APPS / "apps_ui.c").read_text()
        alarm = (APPS / "apps_alarm.h").read_text()
        rt = (APPS / "apps_rt.h").read_text()
        # The code that makes them true.
        self.assertIn("w->w < 0 ? LV_SIZE_CONTENT : w->w", lv)  # no w or h: the size of the content, which an arc or bar hasn't
        self.assertIn("w->bg = APPS_COLOR_NONE;", ui)  # a box's background is none until the app gives one
        self.assertIn("must be a number, got NaN", c_function(ui, "static void read_number("))  # and an infinity clamps
        self.assertRegex(c_function(ui, "static int w_get("),  # the declared w and h, not the laid-out size
                         r"v < 0 && \(p == P_W \|\| p == P_H\)\) \{\s*/\* sizes to content \*/\s*lua_pushnil\(L\);")
        self.assertRegex(alarm, r"#define APPS_ALARM_BUDGET 6\b")
        self.assertRegex(alarm, r"#define APPS_ALARM_BUDGET_MS 60000\b")
        self.assertRegex((APPS / "apps_ui.h").read_text(), r"#define APPS_UI_BOX_DEPTH_MAX 4\b")
        self.assertRegex(rt, r"#define APPS_STORE_MAX_WAIT_MS 10000\b")
        self.assertRegex(rt, r"#define APPS_STORE_FLUSH_MS 2000\b")
        guide = " ".join(GUIDE.read_text().split())  # one line: a fact may wrap
        facts = {
            "an arc or a bar with no w and h draws nothing": [r"[Aa]n arc or a bar has no content of its own", r"draws nothing"],
            "a box without bg is invisible": [r"[Aa] box without `bg` \(or a `border`\) is invisible"],
            "a button with no w and h is as small as its text": [r"button with no `w` and `h` is only as big as its text"],
            "a box's border moves its children": [r"border moves them in"],
            "a box clips its children": [r"clips what sticks out of the box"],
            "boxes nest 4 deep, a box counting itself": [r"boxes nest at most 4 deep", r"may go in the fourth"],
            "w:get gives nil for a w or h that sizes to content": [
                r"`w:get\(\"w\"\)` and `w:get\(\"h\"\)` are nil while the widget sizes to its content"],
            "a NaN property is an error, an infinity clamps": [r"NaN is an error", r"an infinity is clamped"],
            "the alarm firing limit": [r"more than 6 times within a minute", r"all of its alarms are cancelled",
                                       r"`timer\.every` for anything that repeats"],
            "alarm.after refuses 0": [r"`math\.max\(1, ms\)`"],
            "a fired alarm that isn't told yet reads 0": [r"0 and nil both mean it has ended"],
            "an alarm armed before the clock is set is kept once the clock is set": [
                r"armed before the gadget has set its clock is kept once the clock is set"],
            "sound.stop ends any app's ring": [r"`sound\.stop\(\)` ends any app's ring"],
            "globals: use local": [r"[Dd]eclare everything `local`"],
            "the store's cache is outside memory_kb": [r"outside the Lua heap"],
            "a restart loses at most the last 10 s of store changes": [r"loses at most the last 10 s"],
        }
        for fact, patterns in facts.items():
            for pattern in patterns:
                with self.subTest(fact, pattern=pattern):
                    self.assertRegex(guide, pattern, f"the guide doesn't say: {fact}")


class LinkWiring(unittest.TestCase):
    def test_the_registration_is_printed_into_psram_in_an_apps_build(self):
        # I2: with the apps the registration is near 4 KB, where malloc picks internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096),
        # and every size build_register_json tries takes it while the Link's setup sets the boot's lowest internal and DMA free. An
        # apps build prints it into PSRAM; a build without the apps is as it was (X5).
        body = c_function((MAIN / "noise_control.cpp").read_text(), "static char *build_register_json(")
        self.assertRegex(body, r"#if CONFIG_MUSE_APPS\n[^#]*json = \(char \*\)heap_caps_malloc\(size, MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\);"
                               r"\n#else\n\s*json = \(char \*\)malloc\(size\);\n#endif")

    def test_adapter_reads_vm_files_own_words(self):
        # The adapter tells a dropped session, a timeout, a busy writer and a lack of memory by vm_file's texts: each it
        # compares against must be one of vm_file.c's literals, or the code falls back to "failed" (and the guide goes inline).
        link = (MAIN / "apps_link.c").read_text()
        words = re.findall(r'strcmp\((?:err|why), "([^"]+)"\) == 0', link)
        for word in ("the VM connection dropped", "could not send the request to the VM", "write timed out"):
            self.assertIn(word, words)
        literals = set(c_literals((MAIN / "vm_file.c").read_text()))
        self.assertEqual([w for w in words if w not in literals], [])

    def test_every_case_of_the_harness_has_a_test(self):
        cases = set(re.findall(r'^\s*\{"(\w+)", \w+\},$', HARNESS.read_text(), re.M))
        tested = set(re.findall(r'self\.run_case\("(\w+)"', Path(__file__).read_text()))
        self.assertTrue(cases)
        self.assertEqual(sorted(cases - tested), [])
        self.assertEqual(sorted(tested - cases), [])

    def test_advertised_and_dispatched_only_under_the_option(self):
        # link.register: after the BLE commands, before commands_v2 is attached, only with the option.
        noise = (MAIN / "noise_control.cpp").read_text()
        register = c_function(noise, "static char *build_register_json(void)")
        self.assertEqual(noise.count("apps_link_register(commands);"), 1)
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(noise, noise.index("apps_link_register(commands);")))
        self.assertLess(register.index("ble_commands_register(commands);"), register.index("apps_link_register(commands);"))
        self.assertLess(register.index("apps_link_register(commands);"),
                        register.index('cJSON_AddItemToObject(params, "commands_v2", commands);'))
        # on_ws_command: next to the BLE commands' dispatch, before anything else, only with the option.
        app = (MAIN / "app.c").read_text()
        handler = c_function(app, "static cJSON *on_ws_command(")
        self.assertEqual(app.count("apps_link_handles(command)"), 1)
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(app, app.index("apps_link_handles(command)")))
        self.assertRegex(handler, r"if \(apps_link_handles\(command\)\) \{\s*return apps_link_handle\(command, params, "
                                  r"request_id, session_generation\);\s*\}")
        self.assertLess(handler.index("ble_commands_handles(command)"), handler.index("apps_link_handles(command)"))
        self.assertLess(handler.index("apps_link_handles(command)"), handler.index('"device.list_vms"'))
        # The VM's files behind the apps: set as Muse starts (muse_glue_start), only with the option (X5, E12).
        glue = (MAIN / "muse_glue.c").read_text()
        self.assertEqual(glue.count("apps_link_init();"), 1)
        self.assertIn("apps_link_init();", c_function(glue, "void muse_glue_start(void)"))
        self.assertIn("if CONFIG_MUSE_APPS", open_conditions(glue, glue.index("apps_link_init();")))
        # Every other mention of the file in main/, includes included, is under the option.
        for path in sorted([*MAIN.glob("*.c"), *MAIN.glob("*.cpp")]):
            if path.name == "apps_link.c":
                continue
            source = strip_c_comments(path.read_text())
            for match in re.finditer(r"apps_link", source):
                with self.subTest(path.name, at=match.start()):
                    self.assertIn("if CONFIG_MUSE_APPS", open_conditions(source, match.start()))
        # Built only with the option.
        cmake = (MAIN / "CMakeLists.txt").read_text()
        self.assertEqual(cmake.count('"apps_link.c"'), 1)
        block = re.search(r"^if\(CONFIG_MUSE_APPS\)\n(.*?)^endif\(\)", cmake, re.M | re.S)
        self.assertIsNotNone(block, "no if(CONFIG_MUSE_APPS) block in main/CMakeLists.txt")
        self.assertIn('list(APPEND GADGET_SRCS "apps_link.c")', block.group(1))


if __name__ == "__main__":
    unittest.main()
