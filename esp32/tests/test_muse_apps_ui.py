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

"""apps_ui, the `ui` module of Muse apps and the shadow widget table behind it.

Most tests run apphost on an app, a fixture (tests/muse_apps_fixtures/ui_*) or one made on the spot, with the
`tap` and `dump` commands of its script, and read the one-JSON-object-a-line events it prints; a violation of the
UI lock's rules (apphost_port.c) aborts it, and stderr must stay empty. The C cases of tests/muse_apps_ui_harness.c
check what a script can't: the dirty bits and the slots the LVGL task's apply step sees, the lock, the dump cut at
every size, and a heap that refuses at every step. Its counting allocator ends every case at 0 bytes.
"""

import json
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import apps_host
from test_muse_apps_rt import BUDGET_CALL, CALL, START, AppHostTestCase, Run, header_number

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse/apps"
FIXTURES = ROOT / "tests/muse_apps_fixtures"
HARNESS_SOURCES = [ROOT / "tests/muse_apps_ui_harness.c", APPS / "apps_ui.c", APPS / "apps_vm.c",
                   APPS / "apps_json.c", APPS / "apps_suggest.c"]
INCLUDES = [ROOT / "components/lua/src", APPS]

# The properties of each kind, in the order the unknown-property message lists them (the brief's lists).
COMMON = ["align", "x", "y", "w", "h", "hidden", "opacity", "parent"]
PROPS = {
    "label": COMMON + ["text", "font", "color", "text_align", "wrap"],
    "button": COMMON + ["text", "font", "color", "bg", "radius", "border", "border_color"],
    "box": COMMON + ["bg", "radius", "border", "border_color"],
    "arc": COMMON + ["min", "max", "value", "start_angle", "end_angle", "thickness", "color", "track_color"],
    "bar": COMMON + ["min", "max", "value", "color", "track_color", "radius"],
    "image": COMMON + ["src", "scale", "rotation", "spin"],
}
KINDS = list(PROPS)

FONTS = "sans14, sans16, sans20, sans28, mono8, mono16, digits48, digits96"
ALIGNS = "center, top, bottom, left, right, top_left, top_right, bottom_left, bottom_right"
EVENTS = "tap, long_press, press, release"
UI_HAS = "label, button, box, arc, bar, image, bg, clear, polar, W, H, R, SAFE"
COLOUR_HINT = '(use "#rgb", "#rrggbb" or 0xRRGGBB)'
QUOTA = "ui: no more than 128 widgets per app"
QUOTA_PENDING = QUOTA + " (a deleted widget keeps its slot until the screen next updates)"
DELETED = "widget was deleted"


def valid(kind):
    return ", ".join(PROPS[kind])


def unknown_property(kind, name):
    return f"ui.{kind}: unknown property '{name}' (valid: {valid(kind)})"


def lvgl_image(w=2, h=1):
    """A valid LVGL v9 RGB565 .bin image: the 12-byte header, then the pixels."""
    return struct.pack("<BBHHHHH", 0x19, 0x12, 0, w, h, w * 2, 0) + bytes(w * 2 * h)


class UiTestCase(AppHostTestCase):
    def lua_app(self, source, script=None, files=None, manifest=None, **kw):
        """A one-file app whose start() runs `source` (Lua statements), then the script."""
        main = "local app = {}\nfunction app.start()\n" + source + "\nend\nreturn app\n"
        return self.host(self.make_app("scratch", main, files=files, manifest=manifest), script=script, **kw)

    def lua_lines(self, source, **kw):
        """What start() printed, line by line, and nothing else: no warning, no error, no crash."""
        run = self.lua_app(source, **kw)
        self.assertEqual(run.kinds(), ["log"] * len(run.logs()) + ["started"], run.events)
        return run.logs()


class UiTest(UiTestCase):
    # ---- The brief's tests ----

    def test_create_set_get_delete(self):
        run = self.host(FIXTURES / "ui_basic")
        self.assertEqual(run.logs(), [
            "label#0\tHello\t10\t20\tsans28\t#ff8800",
            "World\t11",
            "nil\tnil\tcenter\tfalse\t100",
            "false\t" + DELETED,
            "label#0 (deleted)",
        ])
        self.assertEqual(run.kinds(), ["log"] * 5 + ["started"])

    def test_validation_messages(self):
        run = self.host(FIXTURES / "ui_messages")
        got = [tuple(line.split("\t", 1)) for line in run.logs()]
        want = [
            # The brief's messages.
            ("number", "ui.label: 'x' must be a number, got string"),
            ("unknown_property", "ui.label: unknown property 'colour' (valid: align, x, y, w, h, hidden, opacity, "
                                 "parent, text, font, color, text_align, wrap)"),
            ("font", f"ui.label: font 'sans24' is not one of {FONTS}"),
            ("colour", f"ui.box: bg '#12' is not a colour {COLOUR_HINT}"),
            ("text_too_long", "ui.label: text is longer than 256 bytes"),
            ("text_not_utf8", "ui.label: text is not valid UTF-8"),
            ("deleted_widget", DELETED),
            ("parent_not_a_box", "ui.label: parent must be a ui.box"),
            ("image_not_in_package", "ui.image: src 'nope.bin' is not one of this app's images "
                                     '(manifest.json "files")'),
            ("unknown_event", f"ui.button: unknown event 'swipe' (valid: {EVENTS})"),
            ("missing_name", f"ui.labl doesn't exist (ui has: {UI_HAS})"),
            # B12: min stays under max.
            ("arc_min_max_create", "ui.arc: min must be less than max"),
            ("arc_min_max_set", "ui.arc: min must be less than max"),
            ("bar_min_max_create", "ui.bar: min must be less than max"),
            ("bar_min_max_set", "ui.bar: min must be less than max"),
            # R9.1 and R9.2: types.
            ("nan", "ui.label: 'x' must be a number, got NaN"),
            ("not_a_number", "ui.label: 'w' must be a number, got table"),
            ("hidden_not_a_boolean", "ui.label: 'hidden' must be a boolean, got number"),
            ("wrap_not_a_boolean", "ui.label: 'wrap' must be a boolean, got string"),
            ("text_not_a_string", "ui.label: 'text' must be a string, got table"),
            ("text_a_boolean", "ui.label: 'text' must be a string, got boolean"),
            ("align", f"ui.label: align 'middle' is not one of {ALIGNS}"),
            ("text_align", "ui.label: text_align 'justify' is not one of left, center, right"),
            # F3: the font style for a value that is not text, shown by its type
            ("font_not_a_string", f"ui.label: font '<boolean>' is not one of {FONTS}"),
            ("font_a_number", f"ui.label: font '20' is not one of {FONTS}"),
            ("align_a_table", f"ui.label: align '<table>' is not one of {ALIGNS}"),
            ("text_align_a_boolean", "ui.label: text_align '<boolean>' is not one of left, center, right"),
            # R9.3 to R9.6: get, parent, nesting, methods.
            ("get_unknown_property", unknown_property("label", "bg")),
            ("parent_set_later", "ui.label: parent can only be set when the widget is created"),
            ("parent_deleted", "ui.label: parent was deleted"),
            ("nesting_box", "ui.box: boxes nest at most 4 deep"),
            ("unknown_method", "ui.label: unknown method 'frobnicate' (valid: set, get, on, delete)"),
            # What the brief leaves to the implementation.
            ("text_nul", "ui.label: text can't contain a NUL byte"),
            ("needs_a_table", "ui.label: needs a table of properties, like ui.label{...}, got string"),
            ("needs_a_table_no_value", "ui.button: needs a table of properties, like ui.button{...}, got no value"),
            ("set_needs_a_table", "ui.label: set needs a table of properties, like w:set{...}, got string"),
            ("image_needs_src", 'ui.image: src is required (a .bin image of this app\'s manifest.json "files")'),
            ("radius_string", 'ui.box: radius \'round\' is not a number or "circle"'),
            ("radius_type", "ui.box: 'radius' must be a number or \"circle\", got boolean"),
            ("none_is_for_bg", f"ui.label: color 'none' is not a colour {COLOUR_HINT}"),
            ("colour_type", f"ui.box: bg must be a colour string or number {COLOUR_HINT}, got boolean"),
            ("colour_number", f"ui.box: bg '16777216' is not a colour {COLOUR_HINT}"),
            ("handler_type", "ui.button: handler for 'tap' must be a function (or nil to remove it), got string"),
            ("handler_missing", "ui.button: handler for 'tap' must be a function (or nil to remove it), "
                                "got no value"),
            ("event_type", "ui.button: on needs the name of an event, got number"),
            ("property_key_number", unknown_property("label", "1")),
            ("property_key_boolean", unknown_property("label", "<boolean>")),
            ("get_needs_a_name", "ui.label: get needs the name of a property, got number"),
            ("assign", "ui.label: can't assign w.text: change a widget with w:set{text = ...}"),
            ("dot_call", "ui: set needs a widget: call it as w:set(...)"),
            ("bg_colour", f"ui.bg: '#12' is not a colour {COLOUR_HINT}"),
            ("bg_not_a_colour_type", f"ui.bg: needs a colour {COLOUR_HINT}, got table"),
            ("polar_type", "ui.polar: 'deg' must be a number, got string"),
            ("polar_r_missing", "ui.polar: 'r' must be a number, got no value"),
            ("polar_nan", "ui.polar: 'deg' must be a number, got NaN"),
            ("polar_inf", "ui.polar: 'deg' must be a finite number, got inf"),
        ]
        self.assertEqual(got, want)
        self.assertEqual(run.kinds(), ["log"] * len(want) + ["started"])

    def test_what_an_app_supplied_is_cut_and_made_printable_in_a_message(self):
        lines = self.lua_lines("""
            local function msg(f, t) local _, e = pcall(f, t) return e end
            print(msg(ui.label, {[string.rep("n", 60)] = 1}))
            print(msg(ui.label, {["tab\\tnew\\nline\\1x\\127y"] = 1}))
            print(msg(ui.label, {font = string.rep("f", 40)}))
            print(msg(ui.label, {font = string.rep("f", 41)}))
            print(msg(ui.label, {["\\xc3\\xa9" .. string.rep("n", 37) .. "\\xc3\\xa9\\xc3\\xa9"] = 1}))
            print(msg(ui.label, {["\\xff\\xfe"] = 1}))
            print(msg(ui.label, {["\\xc3\\xa9\\xe2\\x82"] = 1}))
            print(msg(ui.box, {bg = string.rep("#", 50)}))
            print(msg(ui.button, {radius = "\\0"}))
            -- print makes a control byte '?' too, so look for the bytes in the message itself
            local e = msg(ui.label, {["a\\tb\\nc\\1d\\127e\\0f"] = 1})
            print(e:find("\\t", 1, true), e:find("\\n", 1, true), e:find("\\1", 1, true), e:find("\\127", 1, true),
                  e:find("\\0", 1, true))
        """)
        want = [
            unknown_property("label", "n" * 40 + "..."),
            unknown_property("label", "tab?new?line?x?y"),
            f"ui.label: font '{'f' * 40}' is not one of {FONTS}",
            f"ui.label: font '{'f' * 40}...' is not one of {FONTS}",
            unknown_property("label", "\u00e9" + "n" * 37 + "..."),  # a character is never split by the cut
            unknown_property("label", "??"),
            unknown_property("label", "\u00e9??"),  # a valid character stays, a broken one is a '?' a byte
            f"ui.box: bg '{'#' * 40}...' is not a colour {COLOUR_HINT}",
            'ui.button: radius \'?\' is not a number or "circle"',
            "nil\tnil\tnil\tnil\tnil",
        ]
        self.assertEqual(lines, want)

    def test_the_first_problem_is_the_same_whatever_order_the_table_is_in(self):
        # An unknown key comes first, whatever else is wrong; then the properties are read in a fixed order.
        lines = self.lua_lines("""
            local function msg(t) local _, e = pcall(ui.label, t) return e end
            for _, t in ipairs({{y = "b", x = "a"}, {x = "a", y = "b"}, {h = "b", w = "a", y = "c", x = "d"}}) do
                print(msg(t))
            end
            print(msg({x = "a", colour = 1}))
            print(msg({colour = 1, x = "a", text = {}}))
            print(msg({text = {}, font = "huge", align = "middle"}))
        """)
        want = [
            "ui.label: 'x' must be a number, got string",
            "ui.label: 'x' must be a number, got string",
            "ui.label: 'x' must be a number, got string",
            unknown_property("label", "colour"),
            unknown_property("label", "colour"),
            f"ui.label: align 'middle' is not one of {ALIGNS}",
        ]
        self.assertEqual(lines, want)

    def test_errors_carry_the_position_of_the_call(self):
        # Lua puts "main.lua:LINE: " in front, so an app's author finds the call: here, line 3 of main.lua.
        run = self.run_app("local app = {}\nfunction app.start()\n    ui.label{x = 'a'}\nend\nreturn app", rc=1)
        self.assertEqual(run.events[0]["event"], "crash")
        self.assertTrue(run.events[0]["message"].startswith("main.lua:3: ui.label: 'x' must be a number, got string\n"))
        run = self.run_app("local app = {}\nfunction app.start()\n    local w = ui.label{}\n    w:delete()\n"
                           "    w:set{}\nend\nreturn app", rc=1)
        self.assertTrue(run.events[0]["message"].startswith("main.lua:5: widget was deleted\n"))

    def test_quota_128(self):
        run = self.lua_app("""
            for i = 1, 128 do ui.label{text = tostring(i)} end
            print(pcall(ui.label, {}))
            print(pcall(ui.button, {}))
            ui.clear()
            for i = 1, 128 do ui.box{} end
            print(pcall(ui.box, {}))
        """)
        self.assertEqual(run.logs(), ["false\t" + QUOTA, "false\t" + QUOTA, "false\t" + QUOTA])

    def test_a_slot_waits_for_the_screen_to_be_updated_after_a_delete(self):
        # The apply step deletes an object after a callback: inside it, a deleted widget still holds its slot (the
        # stand-in in apphost applies between commands, as the gadget does between callbacks).
        run = self.lua_app("""
            for i = 1, 128 do ui.label{} end
            first = ui.label{}
        """, rc=1)
        self.assertRegex(run.messages("crash")[0].split("\n")[0], r"^main\.lua:\d+: " + re.escape(QUOTA) + "$")
        main = """
            local app = {}
            local labels = {}
            function app.start() for i = 1, 128 do labels[i] = ui.label{text = tostring(i)} end end
            function app.message(event)
                if event == "swap" then
                    labels[7]:delete()
                    print(pcall(ui.label, {}))
                end
                if event == "again" then
                    local w = ui.label{text = "back"}
                    print(tostring(w), w:get("text"))
                    print(pcall(ui.label, {}))
                end
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=["message swap", "message again"])
        self.assertEqual(run.logs(), ["false\t" + QUOTA_PENDING, "label#6\tback", "false\t" + QUOTA])

    def test_stale_handle_after_clear(self):
        lines = self.lua_lines("""
            local a = ui.label{text = "a"}
            local b = ui.button{text = "b"}
            ui.clear()
            print(pcall(a.get, a, "text"))
            print(pcall(b.set, b, {text = "x"}))
            print(pcall(b.on, b, "tap", print))
            print(pcall(b.delete, b))
            print(tostring(a), tostring(b))
            local c = ui.label{text = "c"}
            print(tostring(c), c:get("text"), pcall(a.get, a, "text"))
            print(a == c, c == c)
        """)
        self.assertEqual(lines, ["false\t" + DELETED] * 4 + [
            "label#0 (deleted)\tbutton#1 (deleted)",
            "label#0\tc\tfalse\t" + DELETED,
            "false\ttrue",
        ])

    def test_a_touch_from_before_ui_clear_runs_nothing(self):  # F11
        # The most common stale event: a tap queued while a handler clears the screen. The gen it carries is the
        # one of the widget that was there, which the clear ended.
        main = """
            local app = {}
            function app.start()
                ui.label{text = "a"}
                ui.button{text = "b"}:on("tap", function() print("old b") end)
            end
            function app.message(event)
                if event == "clear" then ui.clear() end
                if event == "again" then
                    ui.label{text = "c"}
                    ui.button{text = "d"}:on("tap", function() print("new d") end)
                end
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=[
            "tap #1@0",          # before the clear: it runs
            "message clear",
            "tap #1@0",          # the gen of a widget that is gone: nothing runs, nothing is said
            "tap #1",            # the slot is empty now
            "message again",     # a new button takes the slot, on the next gen
            "tap #1@0",          # still the old one's
            "tap #1@1",
            "state"])
        self.assertEqual(run.logs(), ["old b", "new d"])
        self.assertEqual(run.messages("error"), ["no widget in slot 1"])
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})
        self.assertEqual(run.of("crash"), [])

    def test_the_metatables_apps_can_reach_are_protected(self):  # F1, X17
        lines = self.lua_lines("""
            local w = ui.label{}
            print(type(getmetatable(w)), type(getmetatable(ui)))
            print(pcall(function() getmetatable(w).__gc = function() end end))
            print(pcall(function() getmetatable(w).__index = nil end))
            print(pcall(function() getmetatable(ui).__index = nil end))
            print(pcall(setmetatable, ui, {}))
            print(pcall(setmetatable, w, {}))
            print(w:get("x"), ui.W, pcall(function() return ui.nothing end))
        """)
        self.assertEqual(lines[0], "string\tstring")
        for line in lines[1:4]:
            self.assertRegex(line, r"^false\tmain\.lua:\d+: attempt to index a string value")
        self.assertEqual(lines[4], "false\tcannot change a protected metatable")
        self.assertRegex(lines[5], r"^false\t.*table expected, got muse\.widget")
        self.assertRegex(lines[6], r"^0\t466\tfalse\tmain\.lua:\d+: ui\.nothing doesn't exist \(ui has: ")

    def test_a_finalizer_cannot_be_given_to_handles(self):  # F1
        # The review's probe: __gc put on the handles' metatable, then thousands of handles made and dropped in one
        # callback. Lua runs finalizers with the hooks off, beyond the budget and the kill.
        lines = self.lua_lines("""
            ran = 0
            local w = ui.label{}
            print((pcall(function() getmetatable(w).__gc = function() ran = ran + 1 end end)))
            for i = 1, 3000 do ui.label{}:delete() end
            for i = 1, 3000 do local garbage = {i, i} end
            print(ran)
        """)
        self.assertEqual(lines, ["false", "0"])

    def test_every_metatable_c_code_makes_is_protected(self):  # X17
        # luaL_setmetatable and lua_setmetatable from C register a finalizer whenever the metatable holds __gc, and
        # skip the sandbox's setmetatable, which refuses one: an app that could add __gc to a metatable C puts on
        # new objects would get finalizers. Each metatable made with luaL_newmetatable is given __metatable, within a
        # few lines of the call (a later metatable's __metatable must not stand in for it).
        made = 0
        for path in sorted(APPS.glob("*.c")):
            lines = path.read_text().splitlines()
            for n, line in enumerate(lines):
                if re.search(r"\bluaL_newmetatable\s*\(", line):
                    made += 1
                    self.assertRegex("\n".join(lines[n:n + 8]), r'lua_setfield\([^;]*"__metatable"\)',
                                     f"{path.name}:{n + 1}: the metatable made here is not given __metatable")
        self.assertGreater(made, 0, "no luaL_newmetatable to check: has this test lost its target?")

    def test_a_handle_does_not_alias_a_widget_of_another_kind_after_the_gen_wraps(self):  # F4
        # A slot's gen has 16 bits: after 65536 deletes it is the same again, and a handle kept from before matches
        # the slot and the gen of whatever lives there. It has to match the kind too, or it writes a box's state
        # into a label, or makes a label the parent of a widget.
        lines = self.lua_lines("""
            local old = ui.box{}
            old:delete()
            for i = 1, 65535 do ui.label{}:delete() end
            local label = ui.label{text = "now"}
            print(tostring(old), tostring(label), old == label, rawequal(old, label))
            print(pcall(old.get, old, "radius"))
            print(pcall(old.set, old, {radius = 5}))
            print(pcall(ui.label, {parent = old}))
            print(pcall(old.delete, old))
            print(label:get("text"))
        """)
        self.assertEqual(lines, [
            "box#0 (deleted)\tlabel#0\tfalse\tfalse",
            "false\t" + DELETED,
            "false\t" + DELETED,
            "false\tui.label: parent was deleted",
            "false\t" + DELETED,
            "now",
        ])

    def test_a_handle_is_one_object_while_the_app_holds_it(self):  # F5
        run = self.host(FIXTURES / "ui_identity", script=['tap "Go"', 'tap "Go"'])
        self.assertEqual(run.logs(), ["true\ttrue", "go\ttrue\ttrue\ttap", "go\ttrue\ttrue\ttap"])

    def test_tap_dispatch_and_click_alias(self):
        run = self.host(FIXTURES / "ui_tap", script=['tap "Go"', 'tap "Alias"', 'tap "Plain"', 'tap #0', 'tap #1'])
        self.assertEqual(run.logs(), ["tapped\tbutton#0\ttap", "clicked\tbutton#1\ttap", "tapped\tbutton#0\ttap",
                                      "clicked\tbutton#1\ttap"])  # the label has no handler: its tap runs nothing
        self.assertEqual(run.kinds(), ["started"] + ["log"] * 4)

    def test_utf8_text(self):
        run = self.run_app(
            "local app = {}\nfunction app.start()\n"
            '    local w = ui.label{text = "Time’s up ✓ \U0001F600"}\n'
            "    print(w:get('text'), #w:get('text'))\n"
            "    local bad = {'\\xff', '\\xe2\\x82', '\\xc0\\x80', '\\xed\\xa0\\x80', '\\xf4\\x90\\x80\\x80', 'ok\\x80',\n"
            "                 '\\xf0\\x9f\\x98'}\n"
            "    for i, text in ipairs(bad) do print(i, pcall(ui.label, {text = text})) end\n"
            "    print(#ui.label{text = string.rep('a', 256)}:get('text'))\n"
            "    print(pcall(ui.label, {text = string.rep('a', 255) .. '\\xc3\\xa9'}))\n"
            "    print(pcall(ui.label, {text = string.rep('\\xc3\\xa9', 128)}))\n"
            "    print(pcall(ui.label, {text = string.rep('\\xc3\\xa9', 129)}))\n"
            "end\nreturn app", script=["dump"])
        bad = "false\tui.label: text is not valid UTF-8"
        text = "Time’s up ✓ \U0001F600"
        self.assertEqual(run.logs(), [
            f"{text}\t{len(text.encode())}",
            *[f"{i}\t{bad}" for i in range(1, 8)],
            "256",
            "false\tui.label: text is longer than 256 bytes",  # 257 bytes: the limit counts bytes
            "true\tlabel#2",
            "false\tui.label: text is longer than 256 bytes",
        ])
        dump = run.of("dump")[0]["widgets"]
        self.assertEqual(dump[0]["text"], text)

    def test_polar_and_constants(self):
        lines = self.lua_lines("""
            print(ui.polar(90, 100))
            print(ui.polar(0, 100))
            print(ui.polar(180, 100))
            print(ui.polar(270, 100))
            print(ui.polar(45, 100))
            print(ui.polar(30, 100))
            print(ui.polar(360, 100))
            print(ui.polar(-90, 100))
            print(ui.polar(450, 100))
            print(ui.polar(1e9 * 360 + 90, 7))
            print(ui.polar(360e12 + 90, 1e6))
            print(ui.polar(-360e12 + 90, 1e6))
            print(ui.polar(90, -50))
            print(ui.polar(90.5, 2.5))
            print(ui.polar(0, 1e300))
            local x, y = ui.polar(10, 100)
            print(math.type(x), math.type(y), select('#', ui.polar(1, 1)))
            print(ui.W, ui.H, ui.R, ui.SAFE, math.type(ui.W))
        """)
        self.assertEqual(lines, [
            "100\t0", "0\t-100", "0\t100", "-100\t0", "71\t-71", "50\t-87", "0\t-100", "-100\t0", "100\t0",
            "7\t0", "1000000\t0", "1000000\t0",  # an angle too big for a double to keep its fraction: reduced first
            "-50\t0", "2\t0", "0\t-1000000", "integer\tinteger\t2", "466\t466\t233\t330\tinteger"])

    def test_event_for_deleted_widget_is_dropped(self):
        main = """
            local app = {}
            function app.start()
                local a = ui.button{text = "A"}
                local b = ui.button{text = "B", y = 60}
                b:on("tap", function() print("B tapped") end)
                a:on("tap", function() print("A tapped") b:delete() end)
            end
            function app.message(event)
                local c = ui.button{text = "C", y = 120}
                c:on("tap", function() print("C tapped") end)
                print(tostring(c))
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=[
            'tap "B"', 'tap "A"',
            "tap #1@0",             # B's old gen: nothing runs, nothing is said
            "tap #1",               # the slot now: it is empty
            'tap "B"',
            "message make",         # C takes B's slot, on the next gen
            "tap #1@0",             # still B's: dropped, and C's handler must not run
            "tap #1@1",
            "tap #1",
            "state"])
        self.assertEqual(run.logs(), ["B tapped", "A tapped", "button#1", "C tapped", "C tapped"])
        self.assertEqual(run.messages("error"), ["no widget in slot 1", 'no button or label has the text "B"'])
        self.assertEqual(run.events[-1], {"event": "state", "state": "running"})
        self.assertEqual(run.of("crash"), [])

    def test_tap_by_text_means_the_first_button_or_label(self):
        main = """
            local app = {}
            function app.start()
                ui.box{}:on("tap", function() print("box") end)
                ui.arc{}:on("tap", function() print("arc") end)
                ui.label{text = "Same"}:on("tap", function() print("label") end)
                ui.button{text = "Same"}:on("tap", function() print("button") end)
                ui.button{text = ""}:on("tap", function() print("empty button") end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=['tap "Same"', 'tap ""'])
        self.assertEqual(run.logs(), ["label", "empty button"])  # a box or an arc has no text to match

    def test_dump_shape(self):
        run = self.host(FIXTURES / "ui_dump", script=["dump"])
        self.assertEqual(run.lines[-1],
                         '{"event":"dump","widgets":[{"id":"label#0","kind":"label","text":"Time","x":5,"y":-10,'
                         '"w":null,"h":null,"hidden":false,"overflow":0},{"id":"button#1","kind":"button",'
                         '"text":"OK","x":0,"y":0,"w":120,"h":48,"hidden":true,"overflow":0}]}')
        for widget in run.events[-1]["widgets"]:
            self.assertEqual(list(widget), ["id", "kind", "text", "x", "y", "w", "h", "hidden", "overflow"])

    # ---- The rulings ----

    def test_no_text_is_empty_text(self):  # B2, E3
        lines = self.lua_lines("""
            local a, b = ui.label{}, ui.button{}
            print(a:get("text") == "", b:get("text") == "")
            a:set{text = "x"}
            a:set{text = ""}
            print(a:get("text") == "", #a:get("text"))
            print(ui.label{text = nil}:get("text") == "")
        """)
        self.assertEqual(lines, ["true\ttrue", "true\t0", "true"])
        run = self.lua_app('ui.label{} ui.button{} ui.box{}', script=["dump"])
        self.assertEqual([w["text"] for w in run.of("dump")[0]["widgets"]], ["", "", None])

    def test_deleting_a_box_deletes_what_is_inside(self):  # B4
        lines = self.lua_lines("""
            local box = ui.box{}
            local mid = ui.box{parent = box}
            local child = ui.label{parent = mid, text = "in"}
            local other = ui.button{parent = box, text = "x"}
            local outside = ui.label{text = "out"}
            box:delete()
            for _, w in ipairs({box, mid, child, other}) do
                print(pcall(w.get, w, "x"))
            end
            print(outside:get("text"))
            print(pcall(ui.label, {parent = mid}))
        """)
        self.assertEqual(lines, ["false\t" + DELETED] * 4 + ["out", "false\tui.label: parent was deleted"])
        run = self.lua_app('local b = ui.box{} ui.label{parent = b} ui.label{} b:delete()', script=["dump"])
        self.assertEqual([w["id"] for w in run.of("dump")[0]["widgets"]], ["label#2"])

    def test_the_value_stays_between_min_and_max(self):  # B12
        lines = self.lua_lines("""
            local arc = ui.arc{value = 150}
            local bar = ui.bar{value = -4}
            print(arc:get("value"), bar:get("value"), arc:get("min"), arc:get("max"), bar:get("track_color"))
            arc:set{value = 40}
            arc:set{max = 30}
            print(arc:get("value"), arc:get("max"))
            arc:set{min = 10, max = 20}
            print(arc:get("value"))
            arc:set{min = 100, max = 200}
            print(arc:get("value"), arc:get("min"), arc:get("max"))
            print(pcall(arc.set, arc, {max = 100}))
            print(arc:get("min"), arc:get("max"))
            print(ui.arc{min = -50, max = 50}:get("value"), ui.arc{min = 5, max = 10}:get("value"))
            arc:set{min = -1e9, max = 1e9, value = 1e12}
            print(arc:get("min"), arc:get("max"), arc:get("value"))
            print(bar:get("radius"), ui.bar{radius = "circle"}:get("radius"))
        """)
        self.assertEqual(lines, [
            "100\t0\t0\t100\t#262626", "30\t30", "20", "100\t100\t200",
            "false\tui.arc: min must be less than max", "100\t200", "0\t5",
            "-1000000\t1000000\t1000000", "0\tcircle",
        ])

    def test_the_dump_is_cut_to_whole_entries(self):  # E7
        run = self.lua_app("for i = 1, 128 do ui.label{text = 'widget number ' .. i} end",
                           script=["dump 3072", "dump", "dump 3", "dump 2", "dump 0", "dump x"])
        cut, whole, tiny, too_small = run.events[1], run.events[2], run.events[3], run.events[4]
        self.assertGreater(cut["dropped"], 0)
        self.assertEqual(len(cut["widgets"]) + cut["dropped"], 128)
        self.assertLess(len(json.dumps(cut["widgets"], separators=(",", ":"))), 3072)
        self.assertEqual(cut["widgets"], whole["widgets"][:len(cut["widgets"])])
        self.assertEqual((len(whole["widgets"]), "dropped" in whole), (128, False))
        self.assertEqual(tiny, {"event": "dump", "widgets": [], "dropped": 128})
        self.assertEqual(too_small, {"event": "dump", "widgets": [], "dropped": 128})  # no room for "[]": it says so
        self.assertEqual(run.messages("error"), ["dump needs a number of bytes: dump [n]"] * 2)

    def test_images_are_known_before_main_runs(self):  # B15
        files = {"logo.bin": lvgl_image(), "art/icon.bin": lvgl_image(4, 4), "data.json": "{}", "util.lua": "return 1",
                 "fake.lua": lvgl_image()}  # a file is an image by its .bin name, whatever its bytes say
        main = ("local top = ui.image{src = 'art/icon.bin', scale = 150}\n"
                "print(top:get('src'), top:get('scale'), tostring(top))\n"
                "local app = {}\n"
                "function app.start()\n"
                "    local w = ui.image{src = 'logo.bin'}\n"
                "    print(w:get('src'), w:get('scale'), pcall(ui.image, {src = 'data.json'}))\n"
                "    print(pcall(ui.image, {src = 'util.lua'}))\n"
                "    print(pcall(ui.image, {src = 'fake.lua'}))\n"
                "    print(pcall(ui.image, {src = 'LOGO.bin'}))\n"
                "    print(pcall(ui.image, {src = 'logo'}))\n"
                "    print(pcall(ui.image, {src = 'art/'}))\n"
                "    print(pcall(ui.image, {src = 'logo.bin\\0'}))\n"
                "    w:set{src = 'art/icon.bin'}\n"
                "    print(w:get('src'))\n"
                "end\nreturn app\n")
        # fake.lua isn't Lua, so the install's compile would refuse the package: this is the runtime's own rule.
        run = self.host(self.make_app("scratch", main, files=files), compile=False)
        message = "ui.image: src '%s' is not one of this app's images (manifest.json \"files\")"
        self.assertEqual(run.logs(), [
            "art/icon.bin\t150\timage#0",
            "logo.bin\t100\tfalse\t" + message % "data.json",
            "false\t" + message % "util.lua",
            "false\t" + message % "fake.lua",
            "false\t" + message % "LOGO.bin",
            "false\t" + message % "logo",  # a prefix of an image's path is not the path
            "false\t" + message % "art/",
            "false\t" + message % "logo.bin?",  # and a NUL is not the end of it (shown as '?')
            "art/icon.bin",
        ])
        self.assertEqual(run.kinds(), ["log"] * 9 + ["started"])

    def test_binding_tables_are_named(self):  # X8
        source = (APPS / "apps_ui.c").read_text()

        def strings(array):
            body = re.search(rf"static const char \*const {array}\[\] = \{{(.*?)\}};", source, re.S)
            self.assertIsNotNone(body, array)
            return re.findall(r'"([^"]*)"', body.group(1))

        def reg(array):
            body = re.search(rf"static const luaL_Reg {array}\[\] = \{{(.*?)\}};", source, re.S)
            self.assertIsNotNone(body, array)
            return re.findall(r'\{"(\w+)",', body.group(1))

        self.assertEqual(reg("apps_ui_funcs"), ["label", "button", "box", "arc", "bar", "image", "bg", "clear", "polar"])
        self.assertEqual(reg("apps_ui_methods"), ["set", "get", "on", "delete"])
        self.assertEqual(strings("apps_ui_kinds"), KINDS)
        self.assertEqual(strings("apps_ui_events"), EVENTS.split(", "))
        for kind in KINDS:
            self.assertEqual(strings(f"apps_ui_props_{kind}"), PROPS[kind], kind)

    # ---- Numbers, types, what get returns (R9.1 to R9.3) ----

    def test_numbers_are_rounded_and_clamped(self):  # R9.1
        lines = self.lua_lines("""
            local w = ui.label{}
            local function put(name, v) w:set{[name] = v} print(name, v, w:get(name)) end
            for _, v in ipairs({10.4, 10.5, -10.5, -10.4, 0.49, -0.49, 3.0, 466.4, 466.5, 1e300, -1e300, 1 / 0, -1 / 0,
                                2 ^ 53, math.maxinteger, math.mininteger}) do
                put("x", v)
            end
            put("w", 0.4) put("w", 0.5) put("w", 466.5) put("w", -3) put("h", 1 / 0) put("h", -1 / 0)
            put("opacity", 100.4) put("opacity", 100.5) put("opacity", -0.5) put("opacity", -0.4)
            print(pcall(w.set, w, {x = 0 / 0}))
            print(w:get("x"))
            local b = ui.box{}
            put = function(name, v) b:set{[name] = v} print(name, v, b:get(name)) end
            put("border", 20.5) put("border", -1) put("radius", 233.4) put("radius", 233.5) put("radius", -5)
            local arc = ui.arc{}
            put = function(name, v) arc:set{[name] = v} print(name, v, arc:get(name)) end
            put("thickness", 0.2) put("thickness", 64.5) put("start_angle", -1) put("end_angle", 360.4)
            put("end_angle", 361) put("start_angle", 359.5)
            local img = ui.image{src = "a.bin"}  -- F10: scale has a range of its own, 10 to 400
            put = function(name, v) img:set{[name] = v} print(name, v, img:get(name)) end
            put("scale", 9.4) put("scale", 0) put("scale", 9.5) put("scale", 10) put("scale", 399.5) put("scale", 400)
            put("scale", 400.5) put("scale", 1 / 0) put("scale", -1 / 0) put("scale", -5)
        """, files={"a.bin": lvgl_image()})
        def row(name, v, got):
            return f"{name}\t{v}\t{got}"
        want = [row("x", "10.4", 10), row("x", "10.5", 11), row("x", "-10.5", -11), row("x", "-10.4", -10),
                row("x", "0.49", 0), row("x", "-0.49", 0), row("x", "3.0", 3), row("x", "466.4", 466),
                row("x", "466.5", 466), row("x", "1e+300", 466), row("x", "-1e+300", -466), row("x", "inf", 466),
                row("x", "-inf", -466), row("x", "9.007199254741e+15", 466), row("x", "9223372036854775807", 466),
                row("x", "-9223372036854775808", -466),
                row("w", "0.4", 1), row("w", "0.5", 1), row("w", "466.5", 466), row("w", "-3", 1),
                row("h", "inf", 466), row("h", "-inf", 1),
                row("opacity", "100.4", 100), row("opacity", "100.5", 100), row("opacity", "-0.5", 0),
                row("opacity", "-0.4", 0),
                "false\tui.label: 'x' must be a number, got NaN", "-466",
                row("border", "20.5", 20), row("border", "-1", 0), row("radius", "233.4", 233),
                row("radius", "233.5", 233), row("radius", "-5", 0),
                row("thickness", "0.2", 1), row("thickness", "64.5", 64), row("start_angle", "-1", 0),
                row("end_angle", "360.4", 360), row("end_angle", "361", 360), row("start_angle", "359.5", 360),
                row("scale", "9.4", 10), row("scale", "0", 10), row("scale", "9.5", 10), row("scale", "10", 10),
                row("scale", "399.5", 400), row("scale", "400", 400), row("scale", "400.5", 400),
                row("scale", "inf", 400), row("scale", "-inf", 10), row("scale", "-5", 10)]
        self.assertEqual(lines, want)

    def test_rotation_and_spin_ranges(self):  # connected apps Task 1: an image's angle and its turns a minute
        lines = self.lua_lines("""
            local img = ui.image{src = "a.bin"}
            local function put(name, v) img:set{[name] = v} print(name, v, img:get(name)) end
            put("rotation", -1) put("rotation", 359) put("rotation", 360) put("rotation", 90.5) put("rotation", 1 / 0)
            put("rotation", -1 / 0) put("rotation", 45)
            put("spin", 61) put("spin", 33.4) put("spin", -1) put("spin", 1 / 0) put("spin", 0)
            print(pcall(img.set, img, {rotation = "90"}))
            print(pcall(img.set, img, {spin = "fast"}))
            print(pcall(img.set, img, {rotation = 0 / 0}))
            print(pcall(img.set, img, {spin = true}))
            print(img:get("rotation"), img:get("spin"), math.type(img:get("rotation")))
            local made = ui.image{src = "a.bin", rotation = 400, spin = 100}
            print(made:get("rotation"), made:get("spin"))
            print(pcall(ui.image, {src = "a.bin", spin = "33"}))
        """, files={"a.bin": lvgl_image()})
        def row(name, v, got):
            return f"{name}\t{v}\t{got}"
        rpm_max = header_number("apps_ui.h", "APPS_SPIN_RPM_MAX")
        self.assertEqual(rpm_max, 60)
        degrees = "'rotation' must be a number of degrees, 0 to 359"
        turns = f"'spin' must be a number of turns a minute, 0 to {rpm_max}"
        self.assertEqual(lines, [
            row("rotation", "-1", 0), row("rotation", "359", 359), row("rotation", "360", 359), row("rotation", "90.5", 91),
            row("rotation", "inf", 359), row("rotation", "-inf", 0), row("rotation", "45", 45),
            row("spin", "61", 60), row("spin", "33.4", 33), row("spin", "-1", 0), row("spin", "inf", 60), row("spin", "0", 0),
            f"false\tui.image: {degrees}, got string",
            f"false\tui.image: {turns}, got string",
            f"false\tui.image: {degrees}, got NaN",
            f"false\tui.image: {turns}, got boolean",
            "45\t0\tinteger",  # a set that failed changed nothing
            "359\t60",
            f"false\tui.image: {turns}, got string",
        ])

    def test_spin_is_capped_at_200_px_drawn(self):
        # connected apps Task 1, fix round 0 (R1): an image spins only while both its sides as drawn (the header's w and h times scale,
        # to the nearest px as apps_lv.c's scaled_side has it) are at most APPS_SPIN_SIDE_MAX. The call that would break that is the
        # error: a create, a set of spin, or a set of scale or src while it spins. spin = 0 always goes, and a refused set changes
        # nothing. A picture too large itself is that error, before what spins at once (test_spin_total_area_is_capped) is counted.
        side_max = header_number("apps_ui.h", "APPS_SPIN_SIDE_MAX")
        self.assertEqual(side_max, 200)
        lines = self.lua_lines("""
            local function try(f, ...) local ok, err = pcall(f, ...) print(ok, err) end
            local a = ui.image{src = "s200.bin", spin = 33}
            print(a:get("spin"))
            try(ui.image, {src = "w201.bin", spin = 33})
            local b = ui.image{src = "w201.bin"}
            try(b.set, b, {spin = 10})
            print(b:get("spin"))
            try(ui.image, {src = "s120.bin", scale = 200, spin = 33})
            local c = ui.image{src = "s120.bin", scale = 200}
            try(c.set, c, {spin = 1})
            a:set{spin = 0}  -- room for d among what spins at once (R1a)
            local d = ui.image{src = "s150.bin", spin = 20}
            try(d.set, d, {scale = 150})
            print(d:get("scale"), d:get("spin"))
            d:set{scale = 133}
            print(d:get("scale"))
            try(d.set, d, {scale = 134})
            d:set{scale = 150, spin = 0}
            print(d:get("scale"), d:get("spin"))
            a:set{spin = 33}
            try(a.set, a, {src = "w201.bin"})
            print(a:get("src"), a:get("spin"))
            a:set{src = "w201.bin", spin = 0}
            b:set{spin = 0}
            local e = ui.image{src = "w201.bin", scale = 400, spin = 0}
            print(a:get("src"), b:get("spin"), e:get("spin"))
            local f = ui.image{src = "s120.bin", scale = 400}
            f:set{src = "s50.bin", spin = 60}
            print(f:get("scale"), f:get("spin"))
        """, files={"s200.bin": lvgl_image(200, 200), "w201.bin": lvgl_image(201, 200), "s120.bin": lvgl_image(120, 120),
                    "s150.bin": lvgl_image(150, 150), "s50.bin": lvgl_image(50, 50)})

        def refused(w, h):
            return f"false\tui.image: spin: an image may spin only up to {side_max} x {side_max} px on screen (this one is {w} x {h})"

        self.assertEqual(lines, [
            "33",
            refused(201, 200),
            refused(201, 200), "0",
            refused(240, 240),
            refused(240, 240),
            refused(225, 225), "100\t20",  # a refused set changed nothing
            "133",  # 150 px at 133% is 199.5, drawn as 200
            refused(201, 201),
            "150\t0",
            refused(201, 200), "s200.bin\t33",
            "w201.bin\t0\t0",
            "400\t60",  # 50 px at 400% is 200
        ])

    def test_spin_total_area_is_capped(self):
        # connected apps Task 1, fix round 1 (R1a): the images that spin at once and show (not hidden, nor in a hidden box) cover at
        # most APPS_SPIN_AREA_MAX px together, their drawn w x h added up. A call that would raise the total past it is the error:
        # a spin made or given, a scale or src while it spins, or a hidden spinner (or the box it's in) shown. A refused call changes
        # nothing; a delete, spin = 0 or a hide frees room.
        side_max = header_number("apps_ui.h", "APPS_SPIN_SIDE_MAX")
        area_max = header_number("apps_ui.h", "APPS_SPIN_AREA_MAX")
        self.assertEqual(area_max, 40000)
        self.assertEqual(area_max, side_max * side_max, "the message says the total as one picture's sides")
        lines = self.lua_lines("""
            local function try(f, ...) local ok, err = pcall(f, ...) print(ok, err) end
            local a = ui.image{src = "s150.bin", spin = 10}
            try(ui.image, {src = "s150.bin", spin = 10})
            local b = ui.image{src = "s150.bin"}
            try(b.set, b, {spin = 10})
            print(b:get("spin"))
            a:delete()
            b:set{spin = 10}
            print(b:get("spin"))
            b:set{spin = 0}
            local c = ui.image{src = "s200.bin", spin = 33}
            try(ui.image, {src = "s50.bin", spin = 33})
            c:set{spin = 0}
            local d = ui.image{src = "s140.bin", spin = 20}
            local e = ui.image{src = "s140.bin", spin = 20}
            print(d:get("spin"), e:get("spin"))
            try(e.set, e, {scale = 110})
            try(e.set, e, {src = "s150.bin"})
            print(e:get("scale"), e:get("src"))
            e:set{hidden = true}
            local f = ui.image{src = "s140.bin", spin = 20}
            try(e.set, e, {hidden = false})
            print(e:get("hidden"))
            local g = ui.image{src = "s140.bin", spin = 20, hidden = true}
            local box = ui.box{hidden = true}
            local h = ui.image{src = "s140.bin", spin = 20, parent = box}
            try(box.set, box, {hidden = false})
            print(box:get("hidden"))
            f:set{spin = 0}
            e:set{hidden = false}
            print(e:get("hidden"))
            d:set{spin = 0}
            e:set{scale = 50}
            box:set{hidden = false}
            print(box:get("hidden"))
            ui.clear()
            local i = ui.image{src = "s200.bin", spin = 60}
            print(i:get("spin"))
        """, files={"s200.bin": lvgl_image(200, 200), "s150.bin": lvgl_image(150, 150), "s140.bin": lvgl_image(140, 140),
                    "s50.bin": lvgl_image(50, 50)})

        def refused(kind, n):
            return (f"false\tui.{kind}: spin: images spinning at once may cover at most {side_max} x {side_max} px together "
                    f"(these would cover {n} px)")

        self.assertEqual(lines, [
            refused("image", 45000),  # two 150 px
            refused("image", 45000), "0",
            "10",  # a deleted spinner freed its area
            refused("image", 42500),  # 200 px and 50 px
            "20\t20",  # 140 px and 140 px: 39,200
            refused("image", 19600 + 154 * 154),  # e grows while it spins
            refused("image", 19600 + 22500),  # e's src grows while it spins
            "100\ts140.bin",
            refused("image", 3 * 19600),  # a hidden spinner shown: d, f and e
            "true",
            refused("box", 3 * 19600),  # a box shown with a spinner in it (g is hidden itself): d, f and h
            "true",
            "false",  # f stopped, so e shows: d and e, 39,200
            "false",  # d stopped and e at 70 px, so the box shows: e and h, 24,500
            "60",  # ui.clear() freed it all: one 200 px
        ])

    def test_the_spin_fixture(self):  # connected apps Tasks 1 and 7: the board's spin gate's app, and its pictures
        def image(name):
            data = (FIXTURES / "spin" / name).read_bytes()
            magic, cf, flags, w, h, stride, _ = struct.unpack_from("<BBHHHHH", data)
            self.assertEqual((magic, flags, stride), (0x19, 0, w * 2), name)
            self.assertEqual(len(data), 12 + w * h * (3 if cf == 0x14 else 2), name)
            return cf, w, h, data

        side = header_number("apps_ui.h", "APPS_SPIN_SIDE_MAX")
        # disc.bin: 200 x 200 RGB565A8, the most an image may spin (APPS_SPIN_SIDE_MAX, fix round 0); its alpha a circle.
        # body.bin: the same record without its label, still; label.bin: 80 px, the label that spins on it (Task 7).
        for name, size in (("disc.bin", side), ("body.bin", side), ("label.bin", 80)):
            cf, w, h, data = image(name)
            self.assertEqual((cf, w, h), (0x14, size, size), f"{name}: a {size} x {size} RGB565A8 image")
            alpha = data[12 + w * h * 2:]

            def a(x, y):
                return alpha[y * w + x]

            last, mid = w - 1, w // 2
            self.assertEqual([a(0, 0), a(last, 0), a(0, last), a(last, last), a(w * 15 // 200, w * 15 // 200)], [0] * 5,
                             f"{name}: the corners are clear")
            self.assertEqual([a(mid, mid), a(mid, 3), a(3, mid), a(last - 3, mid), a(mid, last - 3)], [255] * 5,
                             f"{name}: the disc is whole")
            colours = {data[12 + 2 * (y * w + x):14 + 2 * (y * w + x)] for x in range(w // 4, w * 3 // 4, 4)
                       for y in range(w // 4, w * 3 // 4, 4)}
            self.assertGreater(len(colours), 8, f"{name}: detail")
        # cover.bin: R2's screen's still, dimmed picture, 400 px, no alpha; square.bin: 200 px of spokes, no alpha, whose corners
        # swing as it spins (LVGL draws it in two bands).
        self.assertEqual(image("cover.bin")[:3], (0x12, 400, 400))
        self.assertEqual(image("square.bin")[:3], (0x12, side, side))
        run = self.host(FIXTURES / "spin", script=['tap "0"', 'tap "1"', 'message state', 'message spin {"rpm": 0}',
                                                   'message rotation {"deg": 90}', 'message spin {"rpm": 99}',
                                                   'message scale {"pct": 50}', 'message layout {"name": "body"}',
                                                   'message layout {"name": "cover"}', 'message layout {"name": "square"}',
                                                   'message layout {"name": "disc"}'])
        self.assertEqual(run.kinds(), ["keep_awake", "started", "log", "log"] + ["reply"] * 9)
        self.assertRegex(run.logs()[1], r"^tap 2 \d+$")
        self.assertEqual([e["data"] for e in run.of("reply")], [
            {"layout": "disc", "rotation": 0, "spin": 33, "scale": 100, "taps": 2},
            {"layout": "disc", "rotation": 0, "spin": 0, "scale": 100, "taps": 2},
            {"layout": "disc", "rotation": 90, "spin": 0, "scale": 100, "taps": 2},
            {"layout": "disc", "rotation": 90, "spin": 60, "scale": 100, "taps": 2},
            {"layout": "disc", "rotation": 90, "spin": 60, "scale": 50, "taps": 2},
            {"layout": "body", "rotation": 0, "spin": 33, "scale": 100, "taps": 2},
            {"layout": "cover", "rotation": 90, "spin": 33, "scale": 50, "taps": 2},
            {"layout": "square", "rotation": 0, "spin": 33, "scale": 100, "taps": 2},
            {"layout": "disc", "rotation": 90, "spin": 33, "scale": 50, "taps": 2}])

    def test_types_other_than_numbers(self):  # R9.2
        lines = self.lua_lines("""
            local a = ui.label{text = 42}
            local b = ui.label{text = 4.5}
            local c = ui.label{text = 1 / 4}
            local d = ui.label{text = 10 // 2 * 1.0}
            print(a:get("text"), b:get("text"), c:get("text"), d:get("text"), ui.label{text = -0.0}:get("text"))
            local w = ui.label{hidden = true, wrap = true}
            print(w:get("hidden"), w:get("wrap"))
            w:set{hidden = false}
            print(w:get("hidden"), w:get("wrap"))
            for _, name in ipairs({"align", "font", "text_align"}) do
                local ok, err = pcall(w.set, w, {[name] = 3})
                print(ok, err)
            end
            print(ui.label{text = "#fff"}:get("text"), ui.label{text = "none"}:get("text"))
        """)
        self.assertEqual(lines[:2], ["42\t4.5\t0.25\t5.0\t-0.0", "true\ttrue"])
        self.assertEqual(lines[2], "false\ttrue")
        self.assertEqual(lines[3:], [
            f"false\tui.label: align '3' is not one of {ALIGNS}",
            f"false\tui.label: font '3' is not one of {FONTS}",
            "false\tui.label: text_align '3' is not one of left, center, right",
            "#fff\tnone",
        ])

    def test_colours(self):
        lines = self.lua_lines("""
            local w = ui.label{}
            for _, c in ipairs({"#fff", "#FFF", "#f80", "#F8a", "#102030", "#AbCdEf", 0, 0xFF8800, 16777215, 255.0, 1e3}) do
                w:set{color = c}
                print(tostring(c), w:get("color"))
            end
            local b = ui.button{}
            print(b:get("bg"), b:get("border_color"))
            b:set{bg = "none"}
            print(b:get("bg"))
            b:set{bg = "#123", border_color = 0x00ff00}
            print(b:get("bg"), b:get("border_color"))
            for _, bad in ipairs({"", "fff", "#ff", "#ffff", "#fffff", "#fffffff", "#ggg", "#ffg", "red", "None", " #fff",
                                  "#fff ", "#12345g", 0x1000000, -1, 0.5, 1e10}) do
                local ok, err = pcall(w.set, w, {color = bad})
                print(ok, err)
            end
            print(w:get("color"))
        """)
        self.assertEqual(lines[:11], [
            "#fff\t#ffffff", "#FFF\t#ffffff", "#f80\t#ff8800", "#F8a\t#ff88aa", "#102030\t#102030", "#AbCdEf\t#abcdef",
            "0\t#000000", "16746496\t#ff8800", "16777215\t#ffffff", "255.0\t#0000ff", "1000.0\t#0003e8"])
        self.assertEqual(lines[11:14], ["#262626\t#ffffff", "none", "#112233\t#00ff00"])
        shown = ["", "fff", "#ff", "#ffff", "#fffff", "#fffffff", "#ggg", "#ffg", "red", "None", " #fff", "#fff ",
                 "#12345g", "16777216", "-1", "0.5", "10000000000.0"]
        self.assertEqual(lines[14:-1], [f"false\tui.label: color '{s}' is not a colour {COLOUR_HINT}" for s in shown])
        self.assertEqual(lines[-1], "#0003e8")  # a colour that was refused changed nothing

    def test_get_returns_what_set_takes(self):  # R9.3
        lines = self.lua_lines("""
            local box = ui.box{radius = "circle", bg = "#abc"}
            local w = ui.label{parent = box, align = "bottom_right", font = "digits96", text_align = "right",
                               w = 100.4, text = "x"}
            print(box:get("radius"), box:get("bg"), box:get("border"), box:get("parent"))
            print(w:get("align"), w:get("font"), w:get("text_align"), w:get("w"), w:get("h"), w:get("wrap"))
            local p = w:get("parent")
            print(tostring(p), p == box, rawequal(p, box), p:get("bg"))
            box:set{radius = 12}
            print(box:get("radius"), math.type(box:get("radius")))
            local seen = ""
            for _, a in ipairs({"center", "top", "bottom", "left", "right", "top_left", "top_right", "bottom_left",
                                "bottom_right"}) do
                w:set{align = a}
                seen = seen .. w:get("align") .. " "
            end
            print(seen)
            seen = ""
            for _, f in ipairs({"sans14", "sans16", "sans20", "sans28", "mono8", "mono16", "digits48", "digits96"}) do
                w:set{font = f}
                seen = seen .. w:get("font") .. " "
            end
            print(seen)
            print(ui.arc{}:get("w"))
        """)
        self.assertEqual(lines, [
            "circle\t#aabbcc\t0\tnil",
            "bottom_right\tdigits96\tright\t100\tnil\tfalse",
            "box#0\ttrue\ttrue\t#aabbcc",
            "12\tinteger",
            "center top bottom left right top_left top_right bottom_left bottom_right ",
            "sans14 sans16 sans20 sans28 mono8 mono16 digits48 digits96 ",
            "nil",
        ])

    def test_get_parent_gives_the_handle_of_a_box_in_a_reused_slot(self):
        lines = self.lua_lines("""
            local old = ui.box{}
            ui.clear()
            local box = ui.box{}
            local w = ui.label{parent = box}
            local p = w:get("parent")
            print(tostring(old), tostring(p), p == box, p == old, p:get("radius"))
        """)
        self.assertEqual(lines, ["box#0 (deleted)\tbox#0\ttrue\tfalse\t0"])

    def test_every_kind_lists_its_properties_and_defaults(self):
        defaults = {
            "align": "center", "x": 0, "y": 0, "w": None, "h": None, "hidden": False, "opacity": 100, "parent": None,
            "text": "", "font": "sans20", "color": "#ffffff", "text_align": "center", "wrap": False, "bg": "none",
            "radius": 0, "border": 0, "border_color": "#ffffff", "min": 0, "max": 100, "value": 0,
            "start_angle": 0, "end_angle": 360, "thickness": 12, "track_color": "#262626", "src": "logo.bin",
            "scale": 100, "rotation": 0, "spin": 0,
        }
        per_kind = {"button": {"bg": "#262626", "radius": 16}}  # the rest follow the table above
        for kind in KINDS:
            with self.subTest(kind):
                args = "{src = 'logo.bin'}" if kind == "image" else "{}"
                names = PROPS[kind]
                source = f"local w = ui.{kind}{args}\n" + "".join(
                    f"print({name!r}, w:get({name!r}))\n" for name in names)
                source += f"print(pcall(ui.{kind}, {{nothing = 1}}))\nprint(pcall(w.get, w, 'nothing'))"
                run = self.lua_app(source, files={"logo.bin": lvgl_image()})
                got = [line.split("\t") for line in run.logs()]
                want = []
                for name in names:
                    value = per_kind.get(kind, {}).get(name, defaults[name])
                    shown = "nil" if value is None else str(value).lower() if isinstance(value, bool) else str(value)
                    want.append([name, shown])
                self.assertEqual(got[:len(names)], want)
                self.assertEqual(got[len(names):], [["false", unknown_property(kind, "nothing")],
                                                    ["false", unknown_property(kind, "nothing")]])

    # ---- Parent, nesting, methods, events (R9.4 to R9.7) ----

    def test_parent_is_set_when_the_widget_is_created(self):  # R9.4
        lines = self.lua_lines("""
            local box = ui.box{}
            local w = ui.label{parent = box}
            print(pcall(w.set, w, {parent = box}))
            print(pcall(w.set, w, {text = "kept", parent = box}))
            print(w:get("text") == "", w:get("parent") == box)
            print(pcall(ui.label, {parent = w}))
            print(pcall(ui.label, {parent = 5}))
            print(pcall(ui.label, {parent = "box#0"}))
            print(pcall(ui.label, {parent = {}}))
            print(pcall(ui.label, {parent = ui.button{}}))
            print(pcall(ui.label, {parent = ui.arc{}}))
        """)
        once = "ui.label: parent can only be set when the widget is created"
        not_box = "ui.label: parent must be a ui.box"
        self.assertEqual(lines, ["false\t" + once, "false\t" + once, "true\ttrue"] + ["false\t" + not_box] * 6)

    def test_boxes_nest_at_most_four_deep(self):  # R9.5, and the controller's ruling of 2026-10-29: the message is the rule
        lines = self.lua_lines("""
            local boxes = {}
            local parent
            for i = 1, 6 do
                local ok, box = pcall(ui.box, {parent = parent})
                print(i, ok, ok and tostring(box) or box)
                if ok then parent = box boxes[i] = box end
            end
            print((pcall(ui.label, {parent = boxes[4]})))
            print((pcall(ui.arc, {parent = boxes[4]})))
            print(pcall(ui.box, {parent = boxes[4]}))
            local lone = ui.box{}
            print((pcall(ui.box, {parent = lone})), (pcall(ui.button, {parent = lone})))
            boxes[1]:delete()
            local fresh = ui.box{}
            print((pcall(ui.box, {parent = fresh})), (pcall(ui.label, {parent = boxes[4]})))
            print(pcall(ui.box, {parent = boxes[4]}))
        """)
        deep = "ui.box: boxes nest at most 4 deep"
        self.assertEqual(lines, [
            "1\ttrue\tbox#0", "2\ttrue\tbox#1", "3\ttrue\tbox#2", "4\ttrue\tbox#3",
            "5\tfalse\t" + deep, "6\tfalse\t" + deep,   # the fifth box is nested deeper than four: refused
            "true", "true",                               # a widget that isn't a box goes in the fourth: four box ancestors
            "false\t" + deep,                             # a box doesn't
            "true\ttrue",                                 # a lone box takes a box and a button
            "true\tfalse",                                # boxes[1]'s delete took the chain with it: boxes[4] is gone
            "false\tui.box: parent was deleted",
        ])

    def test_handles_have_methods_not_fields(self):  # R9.6
        lines = self.lua_lines("""
            local w = ui.label{text = "a"}
            for _, name in ipairs({"set", "get", "on", "delete"}) do print(name, type(w[name])) end
            print(pcall(function() return w.text end))
            print(pcall(function() return w[1] end))
            print(pcall(function() w.text = "b" end))
            print(pcall(function() w[1] = "b" end))
            print(w:get("text"))
            local x = ui.arc{}
            print(pcall(function() return x:colour() end))
            print(tostring(w), tostring(x), w == w, w == x, w ~= x, type(w), getmetatable(w) ~= nil)
        """)
        self.assertEqual(lines[:4], ["set\tfunction", "get\tfunction", "on\tfunction", "delete\tfunction"])
        pos = r"main\.lua:\d+: "
        for line, want in zip(lines[4:8], [
                "ui.label: unknown method 'text' (valid: set, get, on, delete)",
                "ui.label: unknown method '<number>' (valid: set, get, on, delete)",
                "ui.label: can't assign w.text: change a widget with w:set{text = ...}",
                "ui.label: can't assign to a widget: change it with w:set{...}"]):
            self.assertRegex(line, "^false\t" + pos + re.escape(want) + "$")
        self.assertEqual(lines[8], "a")
        self.assertRegex(lines[9], "^false\t" + pos + re.escape("ui.arc: unknown method 'colour' (valid: set, get, on, delete)"))
        self.assertEqual(lines[10], "label#0\tarc#1\ttrue\tfalse\ttrue\tuserdata\ttrue")

    def test_set_and_on_return_the_widget_so_calls_chain(self):
        lines = self.lua_lines("""
            local w = ui.label{text = "a"}
            print(w:set{x = 1} == w, w:on("tap", print) == w, w:on("tap", nil) == w, w:set{} == w)
            local c = ui.button{text = "b"}:set{text = "c"}:set{x = 5}:on("press", print)
            print(c:get("text"), c:get("x"), tostring(c))
            print(select("#", w:set{}), select("#", w:on("tap", print)), select("#", w:delete()))
        """)
        self.assertEqual(lines, ["true\ttrue\ttrue\ttrue", "c\t5\tbutton#1", "1\t1\t0"])

    def test_events_on_every_kind(self):  # R9.7
        main = """
            local app = {}
            local hits = {}
            function app.start()
                for _, kind in ipairs({"label", "button", "box", "arc", "bar"}) do
                    local w = ui[kind]{y = #hits * 20}
                    hits[#hits + 1] = w
                    for _, event in ipairs({"tap", "click", "long_press", "press", "release"}) do
                        print(kind, event, w:on(event, function(self, name) print("ran", kind, event, name) end) == w)
                    end
                end
                local img = ui.image{src = "a.bin"}
                img:on("tap", function() print("image tapped") end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main, files={"a.bin": lvgl_image()}),
                        script=["tap #0", "tap #1", "tap #2", "tap #3", "tap #4", "tap #5"])
        logs = run.logs()
        self.assertEqual(len(logs), 25 + 6)
        self.assertEqual(logs[:5], [f"label\t{e}\ttrue" for e in ("tap", "click", "long_press", "press", "release")])
        # The handler of "click" replaced the one of "tap", on every kind (it names the event it was given for), and
        # a tap still reaches it as "tap".
        self.assertEqual(logs[25:], ["ran\tlabel\tclick\ttap", "ran\tbutton\tclick\ttap", "ran\tbox\tclick\ttap",
                                     "ran\tarc\tclick\ttap", "ran\tbar\tclick\ttap", "image tapped"])

    def test_on_replaces_and_nil_removes(self):  # R9.7
        main = """
            local app = {}
            function app.start()
                local b = ui.button{text = "B"}
                b:on("tap", function() print("first") end)
                b:on("tap", function() print("second") end)
                local c = ui.button{text = "C", y = 50}
                c:on("tap", function() print("c") end)
                c:on("press", function() print("press") end)
                c:on("tap", nil)
                c:on("press", nil)
                c:on("press", nil)
                local d = ui.button{text = "D", y = 100}
                d:on("click", function(w, name) print("d", name) d:on("tap", nil) end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=['tap "B"', 'tap "C"', 'tap "D"', 'tap "D"', 'tap "B"'])
        self.assertEqual(run.logs(), ["second", "d\ttap", "second"])

    def test_tap_in_a_handler_that_fails_crashes_the_app(self):
        main = """
            local app = {}
            function app.start()
                ui.button{text = "Bad"}:on("tap", function() error("boom") end)
                ui.button{text = "Good", y = 50}:on("tap", function() print("fine") end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=['tap "Bad"', 'tap "Good"', "state"], rc=1)
        self.assertEqual(run.kinds(), ["started", "crash", "state"])
        self.assertTrue(run.of("crash")[0]["message"].startswith("main.lua:4: boom\nstack traceback:"))
        self.assertEqual(run.events[-1], {"event": "state", "state": "crashed"})  # and the second tap ran nothing

    def test_a_tap_handler_has_the_budget_of_a_callback(self):
        # start() may run START instructions and any other callback CALL (apps_vm.h): a handler between the two is stopped,
        # one of 3/4 of CALL isn't, and start() itself runs the one between them.
        slow, quick = (CALL + START) // 2, CALL * 3 // 4
        main = f"""
            local app = {{}}
            function app.start()
                for i = 1, {slow} do end
                ui.button{{text = "Slow"}}:on("tap", function() for i = 1, {slow} do end print("slow") end)
                ui.button{{text = "Quick", y = 50}}:on("tap", function() for i = 1, {quick} do end print("quick") end)
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), script=['tap "Quick"', 'tap "Quick"', 'tap "Slow"'], rc=1)
        self.assertEqual(run.logs(), ["quick", "quick"])
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])

    def test_a_property_table_is_charged_to_the_budget_by_its_size(self):
        # lua_next visits every slot of a table, empty or not, and no count hook fires inside C: each call is
        # charged the slots it visits. The table is refused at its first key, so what a call costs is the charge.
        main = """
            local app = {}
            local t = {}
            for i = 1, 4096 do t[i] = i end
            function app.message(event)
                for i = 1, tonumber(event) do pcall(ui.label, t) end
                return "done " .. event
            end
            return app
        """
        # A call costs 4097: what a tenth of CALL holds is a reply, and what 1.25 CALL holds is over the budget.
        few, many = CALL // 10 // 4097, CALL * 5 // 4 // 4097
        run = self.host(self.make_app("scratch", main), script=[f"message {few}", f"message {many}"], rc=1)
        self.assertEqual(run.kinds(), ["started", "reply", "crash"])
        self.assertEqual(run.messages("crash"), [BUDGET_CALL])

    def test_a_tap_runs_nothing_unless_the_app_is_running(self):
        main = ("local app = {}\nfunction app.start() ui.button{text = 'B'}:on('tap', function() print('ran') end) end\n"
                "function app.stop() end\nreturn app")
        run = self.host(self.make_app("scratch", main), script=['tap "B"', "stop", 'tap "B"', "start", 'tap "B"'])
        self.assertEqual(run.logs(), ["ran", "ran"])

    def test_tap_and_dump_commands_say_what_is_wrong(self):
        lines = ['tap', 'tap Go', 'tap #', 'tap #x', 'tap #1@', 'tap #1@x', 'tap #1 2', 'tap #128', 'tap #3@65536',
                 'tap "no end', 'tap "Go" "x"', 'tap ""', 'tap "\\ud800"', 'tap #5', 'dump x', 'dump -1', 'dump 1 2']
        run = self.lua_app("ui.button{text = 'Go'}", script=lines)
        slots = "tap needs a slot of 0 to 127 and, after an @, a gen of 0 to 65535: tap #3 or tap #3@1"
        text = "tap needs the text as a JSON string: "
        self.assertEqual(run.messages("error"), [
            'tap needs #<slot>[@<gen>] or a text in quotes: tap #3 or tap "Go"',
            'tap needs #<slot>[@<gen>] or a text in quotes: tap #3 or tap "Go"',
            slots, slots, slots, slots, slots, slots, slots,
            text + "unexpected end of input at byte 8", text + "unexpected '\"' at byte 6",  # nothing after the text
            'no button or label has the text ""', text + "invalid \\u escape at byte 2",
            "no widget in slot 5", "dump needs a number of bytes: dump [n]", "dump needs a number of bytes: dump [n]",
            "dump needs a number of bytes: dump [n]",
        ])

    def test_cycles_of_apps_with_widgets_leak_nothing(self):
        main = """
            local app = {}
            function app.start()
                local box = ui.box{}
                for i = 1, 20 do
                    local w = ui.label{parent = box, text = "label number " .. i}
                    w:on("tap", function() return i end)
                end
                ui.button{text = "x"}:set{text = "a longer text than before", x = 4}
                ui.label{text = "gone"}:delete()
            end
            return app
        """
        run = self.host(self.make_app("scratch", main), cycles=25)
        self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})
        run = self.run_app("return {start = function() for i = 1, 129 do ui.label{text = 'x' .. i} end end}",
                           cycles=10, rc=1)  # the 129th fails: a crashed launch frees its widgets too
        self.assertEqual(run.events[-1], {"event": "leak_check", "bytes": 0})

    def test_the_screen_is_applied_between_commands(self):
        # apphost takes every request as applied after each command, as the gadget does after a frame, so an
        # app that deletes and makes widgets all day does not run out of slots here.
        main = """
            local app = {}
            local current
            function app.message(event)
                if current then current:delete() end
                current = ui.label{text = event}
            end
            return app
        """
        script = [f"message m{i}" for i in range(400)] + ["dump"]
        run = self.host(self.make_app("scratch", main), script=script)
        self.assertEqual(len(run.of("error")), 0)
        self.assertEqual([w["text"] for w in run.events[-1]["widgets"]], ["m399"])

    def test_the_largest_entries_of_a_full_table_fit_in_the_default_dump(self):
        # A control byte takes 6 bytes of JSON, so 256 of them in a text are the widest entry there is: the header
        # promises under 2 KB, and apphost's default size is 2 KB for each of the 128 slots.
        run = self.lua_app("for i = 1, 128 do ui.label{text = string.rep('\\1', 256), x = -466, y = -466, w = 466, "
                           "h = 466, hidden = true} end", script=["dump"])
        dump = run.of("dump")[0]
        self.assertNotIn("dropped", dump)
        self.assertEqual(len(dump["widgets"]), 128)
        self.assertEqual(dump["widgets"][127]["text"], "\x01" * 256)
        entry = len(json.dumps(dump["widgets"][127], separators=(",", ":"), ensure_ascii=True))
        self.assertGreater(entry, 1536)
        self.assertLess(len(run.lines[-1]), 128 * 2048)
        self.assertLess(entry, 2048)

    def test_text_in_the_dump_is_json(self):
        run = self.lua_app("ui.label{text = 'q\"uote\\\\back\\n\\ttab\\1ctl \\xe2\\x80\\x99'}", script=["dump"])
        self.assertEqual(run.of("dump")[0]["widgets"][0]["text"], 'q"uote\\back\n\ttab\x01ctl ’')

    def test_the_device_build_and_the_host_runner_both_compile_apps_ui(self):
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        block = cmake[cmake.index("if(CONFIG_MUSE_APPS)"):]
        self.assertIn('"apps/apps_ui.c"', block[:block.index("endif()")])
        self.assertIn(APPS / "apps_ui.c", apps_host.apphost_sources())

    def test_apps_ui_runs_under_lua_api_checks(self):
        # A Lua built with LUA_USE_APICHECK aborts on any push past the stack slots a C function reserved, which
        # a plain Lua, with 5 spare, never shows. Run the fixtures that use the whole module on it.
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        exe = apps_host.build("apphost_apicheck", [*apps_host.apphost_sources(), *apps_host.lua_sources()], tmp.name,
                              include_dirs=[apps_host.LUA_SRC, apps_host.APPS, apps_host.APPHOST],
                              lua_extra_defines=["LUA_USE_APICHECK"])
        for fixture, script in (("ui_basic", "dump\n"), ("ui_messages", "dump\n"),
                                ("ui_tap", 'tap "Go"\ntap "Alias"\ndump\n'), ("ui_dump", "dump\n"),
                                ("ui_identity", 'tap "Go"\ndump\n')):
            with self.subTest(fixture):
                done = apps_host.run(exe, "--script", "-", FIXTURES / fixture, input=script.encode(), text=False)
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(done.stderr, b"")
                run = Run(done)
                self.assertEqual(run.kinds()[-1], "dump")


class HarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.harness = apps_host.build("muse_apps_ui_harness", [*HARNESS_SOURCES, *apps_host.lua_sources()],
                                      tmp.name, include_dirs=INCLUDES)

    def run_case(self, case, timeout=300):
        try:
            done = apps_host.run(self.harness, case, timeout=timeout)
        except subprocess.TimeoutExpired:
            self.fail(f"{case} took more than {timeout} s")
        self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr)
        return done

    def test_what_each_call_tells_the_apply_step(self):
        self.run_case("dirty_bits")

    def test_every_property_lands_in_its_dirty_group(self):
        self.run_case("dirty_groups")

    def test_a_set_of_rotation_turns_an_image_that_has_spun(self):  # connected apps Task 1
        self.run_case("rotation_sets")

    def test_spin_refuses_an_unsized_src(self):  # connected apps Task 1, fix round 1 (I2): the spin cap fails closed
        self.run_case("spin_refuses_an_unsized_src")

    def test_a_dying_vm_gets_no_work_from_any_binding(self):
        self.run_case("vm_check")

    def test_slots_and_the_quota(self):
        self.run_case("slot_reuse")

    def test_deleting_a_box_flags_everything_inside(self):
        self.run_case("cascade")

    def test_a_box_deleted_over_a_lower_slot_leaves_nothing_alive(self):  # F8
        self.run_case("cascade_lower_slot")

    def test_every_change_takes_the_lock(self):  # F9
        self.run_case("lock_taken")

    def test_the_metatables_of_handles_and_ui_cannot_be_given_a_finalizer(self):  # F1
        self.run_case("metatables")

    def test_a_handle_is_one_object_and_the_cache_of_them_is_weak(self):  # F5
        self.run_case("identity")

    def test_a_remembered_handle_is_given_out_only_for_the_widget_it_names(self):  # F5
        self.run_case("identity_checks_the_life")

    def test_the_apply_side_calls_accept_no_table(self):  # F7
        self.run_case("null_table")

    def test_clear_frees_every_slot_at_once(self):
        self.run_case("clear")

    def test_the_background(self):
        self.run_case("bg")

    def test_push_handler_checks_slot_gen_and_event(self):
        self.run_case("push_handler")

    def test_push_handler_reserves_the_slots_it_pushes(self):
        # Ledger 36: on a Lua with LUA_USE_APICHECK a push past the slots a C function reserved aborts; the case
        # leaves push_handler 0 to 6 free slots on its highest path (a handle the app holds: 5).
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        exe = apps_host.build("muse_apps_ui_harness_apicheck", [*HARNESS_SOURCES, *apps_host.lua_sources()], tmp.name,
                              include_dirs=INCLUDES, lua_extra_defines=["LUA_USE_APICHECK"])
        done = apps_host.run(exe, "push_handler_reserves_its_slots", timeout=300)
        self.assertEqual(done.returncode, 0, done.stdout[-2000:] + done.stderr)

    def test_a_deleted_widgets_handlers_are_let_go_of(self):
        self.run_case("handlers_dropped")

    def test_the_image_table(self):
        self.run_case("assets")

    def test_the_lock_is_never_held_across_an_allocation_or_a_raise(self):
        self.run_case("lock_discipline")

    def test_the_table_survives_a_heap_that_refuses_at_every_step(self):
        self.run_case("refused")

    def test_a_create_that_raises_leaves_no_widget_behind(self):  # F5
        self.run_case("create_all_or_nothing")

    def test_a_fifth_nested_box_is_refused_and_flags_nothing(self):  # R9.5, the table's invariant
        self.run_case("box_depth")

    def test_a_touch_survives_a_heap_that_refuses_at_every_step(self):
        self.run_case("touch_refused")

    def test_dump_uses_the_laid_out_geometry_once_there_is_one(self):
        out = self.run_case("dump_resolved").stdout.strip()
        widgets = json.loads(out)
        self.assertEqual(out, json.dumps(widgets, separators=(",", ":"), ensure_ascii=False))  # exactly this text
        self.assertEqual([(w["id"], w["x"], w["y"], w["w"], w["h"], w["overflow"]) for w in widgets[:3]], [
            ("label#0", 117, 200, 232, 24, 14),   # laid out: the resolved rectangle
            ("button#1", 0, 0, 120, 48, 0),       # rw is 0, so what the other three say counts for nothing
            ("box#2", 0, 0, 200, None, 0)])
        self.assertEqual(widgets[4]["text"], 'say "hi"\\ \n\t\x01 ’ \U0001F600')
        self.assertEqual(len(widgets[6]["text"]), 250)
        self.assertEqual([w["kind"] for w in widgets], ["label", "button", "box", "arc", "label", "bar", "label"])
        self.assertEqual([w["text"] for w in widgets[2:4]], [None, None])

    def test_dump_at_every_size_is_valid_json_of_whole_entries(self):
        out = self.run_case("dump_caps").stdout.split("\n")
        full_line = next(line for line in out if line.startswith("FULL "))
        _, full_len, full_text = full_line.split(" ", 2)
        full = json.loads(full_text)
        self.assertEqual(len(full_text.encode()), int(full_len))
        self.assertEqual(len(full), 7)

        def compact(widgets):
            return json.dumps(widgets, separators=(",", ":"), ensure_ascii=False)

        self.assertEqual(compact(full), full_text)
        caps = [line.split(" ", 4) for line in out if line.startswith("CAP ")]
        self.assertEqual([int(c[1]) for c in caps], list(range(len(full_text.encode()) + 5)))
        sizes = set()
        for _, cap, n, dropped, text in caps:
            cap, n, dropped = int(cap), int(n), int(dropped)
            with self.subTest(cap=cap):
                if cap < 3:  # no room for "[]": nothing is written, and everything is dropped
                    self.assertEqual((n, text, dropped), (0, "", 7))
                    continue
                got = json.loads(text)
                self.assertEqual(len(text.encode()), n)
                self.assertLess(n, cap)  # with the NUL, in cap bytes
                self.assertEqual(got, full[:len(got)])
                self.assertEqual(dropped, 7 - len(got))
                # Maximal: the next entry, which was dropped, would not have fitted.
                if dropped:
                    self.assertGreaterEqual(len(compact(full[:len(got) + 1]).encode()) + 1, cap + 1)
                sizes.add(len(got))
        self.assertEqual(sizes, set(range(8)))  # every number of entries, from none to all


if __name__ == "__main__":
    unittest.main()
