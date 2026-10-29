-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Every message the ui module raises: each case is a call that is refused, and its line is the case's name, a tab
-- and the message, without the position Lua puts in front of it. tests/test_muse_apps_ui.py has the messages.
local app = {}

local function bare(message)
    if message:find("main.lua:", 1, true) == 1 then
        local at = message:find(": ", 1, true)
        return message:sub(at + 2)
    end
    return message
end

local cases = {}
local function case(name, call)
    cases[#cases + 1] = {name, call}
end

-- The widgets the cases use.
local w = {}

case("number", function() ui.label{x = "5"} end)
case("unknown_property", function() ui.label{colour = "red"} end)
case("font", function() ui.label{font = "sans24"} end)
case("colour", function() ui.box{bg = "#12"} end)
case("text_too_long", function() ui.label{text = string.rep("a", 257)} end)
case("text_not_utf8", function() ui.label{text = "\xff"} end)
case("deleted_widget", function() w.gone_label:get("text") end)
case("parent_not_a_box", function() ui.label{parent = w.label} end)
case("image_not_in_package", function() ui.image{src = "nope.bin"} end)
case("unknown_event", function() w.button:on("swipe", print) end)
case("missing_name", function() local _ = ui.labl end)

case("arc_min_max_create", function() ui.arc{min = 5, max = 5} end)
case("arc_min_max_set", function() w.arc:set{max = 0} end)
case("bar_min_max_create", function() ui.bar{min = 10, max = 5} end)
case("bar_min_max_set", function() w.bar:set{min = 100} end)

case("nan", function() ui.label{x = 0 / 0} end)
case("not_a_number", function() ui.label{w = {}} end)
case("hidden_not_a_boolean", function() ui.label{hidden = 1} end)
case("wrap_not_a_boolean", function() ui.label{wrap = "yes"} end)
case("text_not_a_string", function() ui.label{text = {}} end)
case("text_a_boolean", function() ui.label{text = true} end)
case("align", function() ui.label{align = "middle"} end)
case("text_align", function() ui.label{text_align = "justify"} end)
case("font_not_a_string", function() ui.label{font = true} end)
case("font_a_number", function() ui.label{font = 20} end)
case("align_a_table", function() ui.label{align = {}} end)
case("text_align_a_boolean", function() ui.label{text_align = true} end)

case("get_unknown_property", function() w.label:get("bg") end)
case("parent_set_later", function() w.label:set{parent = w.box} end)
case("parent_deleted", function() ui.label{parent = w.gone_box} end)
case("nesting_box", function() ui.box{parent = w.deep} end)
case("unknown_method", function() w.label:frobnicate() end)

case("text_nul", function() ui.label{text = "a\0b"} end)
case("needs_a_table", function() ui.label("x") end)
case("needs_a_table_no_value", function() ui.button() end)
case("set_needs_a_table", function() w.label:set("x") end)
case("image_needs_src", function() ui.image{} end)
case("radius_string", function() ui.box{radius = "round"} end)
case("radius_type", function() ui.box{radius = true} end)
case("none_is_for_bg", function() ui.label{color = "none"} end)
case("colour_type", function() ui.box{bg = true} end)
case("colour_number", function() ui.box{bg = 0x1000000} end)
case("handler_type", function() w.button:on("tap", "x") end)
case("handler_missing", function() w.button:on("tap") end)
case("event_type", function() w.button:on(5, print) end)
case("property_key_number", function() ui.label{[1] = "x"} end)
case("property_key_boolean", function() ui.label{[true] = 1} end)
case("get_needs_a_name", function() w.label:get(5) end)
case("assign", function() w.label.text = "x" end)
case("dot_call", function() w.label.set({text = "x"}) end)
case("bg_colour", function() ui.bg("#12") end)
case("bg_not_a_colour_type", function() ui.bg({}) end)
case("polar_type", function() ui.polar("a", 1) end)
case("polar_r_missing", function() ui.polar(1) end)
case("polar_nan", function() ui.polar(0 / 0, 1) end)
case("polar_inf", function() ui.polar(1 / 0, 1) end)

function app.start()
    w.label = ui.label{text = "x"}
    w.button = ui.button{text = "Go"}
    w.arc = ui.arc{}
    w.bar = ui.bar{}
    w.box = ui.box{}
    w.gone_label = ui.label{}
    w.gone_label:delete()
    w.gone_box = ui.box{}
    w.gone_box:delete()
    -- Four boxes, one inside the other: no box may go inside the fourth (a widget that isn't a box may).
    local parent
    for _ = 1, 4 do
        w.deep = ui.box{parent = parent}
        parent = w.deep
    end
    for _, c in ipairs(cases) do
        local ok, message = pcall(c[2])
        print(c[1] .. "\t" .. (ok and "(no error)" or bare(message)))
    end
end

return app
