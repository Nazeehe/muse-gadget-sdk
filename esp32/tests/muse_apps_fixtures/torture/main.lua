-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The torture fixture: what the deepest C recursion, JSON, floats and a full screen of widgets cost the gadget.
-- tools/muse/app.py pushes it, and the board's own numbers are read after each step (>app.status's stack_free,
-- the bench's >mem). Nothing here is an example of how to write an app.
--
-- Every chain below nests C-boundary calls (the ones LUAI_MAXCCALLS counts) until Lua says "C stack overflow".
-- chain(stop, leaf) stops one level short of that when it is given the depth `stop` it should reach, and runs
-- leaf() there: the innermost level that ran, with the C stack as deep as the limit lets it get.
local app = {}

local hits = {count = 0, last = ""}

-- ---- The chains ----

local chains = {}

function chains.pcall(stop, leaf)
    local depth, err, result = 0, nil, nil
    local function level()
        depth = depth + 1
        if depth == stop then
            result = leaf()
            return
        end
        local ok, why = pcall(level)
        if not ok then
            err = why
        end
    end
    pcall(level)
    return depth, err, result
end

function chains.resume(stop, leaf)
    local depth, err, result = 0, nil, nil
    local function level()
        depth = depth + 1
        if depth == stop then
            result = leaf()
            return
        end
        local ok, why = coroutine.resume(coroutine.create(level))
        if not ok then
            err = why
        end
    end
    coroutine.resume(coroutine.create(level))
    return depth, err, result
end

-- __index recursing through table.concat (a luaL_Buffer on the C stack at every level).
function chains.index(stop, leaf)
    local depth, result = 0, nil
    local meta = {}
    meta.__index = function()
        depth = depth + 1
        if depth == stop then
            result = leaf()
            return ""
        end
        return table.concat(setmetatable({}, meta), "", 1, 1)
    end
    local ok, err = pcall(table.concat, setmetatable({}, meta), "", 1, 1)
    return depth, (not ok) and err or nil, result
end

-- __tostring recursing through string.format("%s").
function chains.tostring(stop, leaf)
    local depth, result = 0, nil
    local meta = {}
    meta.__tostring = function()
        depth = depth + 1
        if depth == stop then
            result = leaf()
            return ""
        end
        return string.format("%s", setmetatable({}, meta))
    end
    local ok, err = pcall(string.format, "%s", setmetatable({}, meta))
    return depth, (not ok) and err or nil, result
end

local CHAIN_NAMES = {"pcall", "resume", "index", "tostring"}

-- ---- JSON at the bottom of a chain ----

local JSON_DEPTH = 16

-- json.encode of a table nested 16 deep and json.decode of an array nested 16 deep: the most C recursion
-- the json module allows itself. Each ends in a float of 17 digits (0.1 + 0.2), the encoder's three printf and two
-- strtod and the decoder's strtod, at the deepest point.
local function json_leaf()
    local nested = {tail = 0.1 + 0.2}
    for _ = 2, JSON_DEPTH do
        nested = {next = nested}
    end
    local text = json.encode(nested)
    local array = json.decode(string.rep("[", JSON_DEPTH) .. "0.30000000000000004" .. string.rep("]", JSON_DEPTH))
    local levels = 0
    while type(array) == "table" do
        levels = levels + 1
        array = array[1]
    end
    return {encoded = #text, decoded_levels = levels, tail = array}
end

-- ---- Floats ----

-- The text json.encode gives each double: the shortest of 15, 16 and 17 digits that reads back as it (apps_json.c's
-- put_float), the same on a host and on the board but for a subnormal. A host's printf prints a subnormal's exact
-- digits and the board's its shortest, so 5e-324 is the second text on a host and the third on the board. Each
-- row is the double and the text, or the two. `encode_ok` says the encoder's text is one of them, `decode_ok` that
-- each decodes to the double as a float, `round_trip_ok` that what the encoder printed reads back as the double
-- it came from. Last, the integer that doesn't fit a double.
local FLOATS = {
    {0.1, "0.1"},
    {5e-324, "4.94065645841247e-324", "5e-324"},
    {1e100, "1e+100"},
    {-1e-07, "-1e-07"},
}

local function floats()
    local cases, ok, round_trip = {}, true, true
    for i, case in ipairs(FLOATS) do
        local value = case[1]
        local text = json.encode(value)
        local decode_ok = true
        for j = 2, #case do
            local back = json.decode(case[j])
            decode_ok = decode_ok and back == value and math.type(back) == "float"
        end
        local row = {
            expected = case[2],
            other = case[3],
            encoded = text,
            encode_ok = text == case[2] or text == case[3],
            decode_ok = decode_ok,
            round_trip_ok = json.decode(text) == value,
        }
        ok = ok and row.encode_ok and row.decode_ok
        round_trip = round_trip and row.round_trip_ok and row.decode_ok
        cases[i] = row
    end
    local big = json.decode("9007199254740993")
    local integer_ok = math.type(big) == "integer" and big == 9007199254740993
    return {ok = ok and integer_ok, round_trip = round_trip and integer_ok, cases = cases, integer_ok = integer_ok}
end

-- 1000 floats of one of two kinds. At most 15 digits: i / 10, which the encoder prints at its first try. 17 digits:
-- the first 1000 of 1 / (i + 0.3) times `scale` (1 if left out) that "%.16g" doesn't read back, which take all three
-- tries (two printf and strtod that fail, then the third printf) and so cost the most. A scale of 1e300 or 1e-300
-- gives the exponents that strtod has the most to do for. Multiplication, division and addition are correctly
-- rounded, so a host and the board make the same floats.
local function float_set(digits, scale)
    local array = {}
    if digits == 15 then
        for i = 1, 1000 do
            array[i] = i / 10
        end
        return array
    end
    local i = 0
    while #array < 1000 do
        i = i + 1
        local x = 1 / (i + 0.3) * (scale or 1)
        if tonumber(string.format("%.16g", x)) ~= x then
            array[#array + 1] = x
        end
    end
    return array
end

-- 1000 floats of 17 digits, encoded and decoded back: how many come back as the same double.
local function floats_exact()
    local array = float_set(17)
    local back = json.decode(json.encode(array))
    local same = 0
    for i = 1, 1000 do
        if back[i] == array[i] then
            same = same + 1
        end
    end
    return {count = #back, same = same}
end

-- n encodes (or decodes) of an array of 1000 floats, of 17 digits (times `scale`) or of at most 15: the call's own
-- time is what >app.status calls worst_dispatch_ms.
local function floats_time(n, decode, digits, scale)
    local array = float_set(digits, scale)
    local text = json.encode(array)
    local count = 0
    for _ = 1, n do
        if decode then
            count = #json.decode(text)
        else
            count = #json.encode(array)
        end
    end
    return {n = n, decode = decode == true, bytes = #text, count = count}
end

-- ---- The screen ----

local KINDS = {"label", "button", "box", "arc", "bar", "image"}
local COLUMNS, ROWS, CELL_W, CELL_H = 9, 14, 26, 10
local WIDGETS = 128

local function on_event(w, name)
    hits.count = hits.count + 1
    hits.last = tostring(w) .. " " .. name
end

-- 128 widgets: the box chain of the depth the screen allows, a button at the bottom of it, a field of every
-- kind, and a label with the longest text there is, each with a handler or two.
local function widgets()
    ui.clear()
    ui.bg("#000000")
    local made = 0
    local parent = nil
    local y = 70
    for i = 1, 4 do
        local box = ui.box{parent = parent, align = "center", x = 0, y = y, w = 320 - i * 40, h = 190 - i * 28,
                           bg = i % 2 == 0 and "#14202c" or "#0c141c", border = 2, border_color = "#4a6a8a",
                           radius = 16}
        box:on("tap", on_event)
        parent = box
        y = 0
        made = made + 1
    end
    local deepest = ui.button{parent = parent, text = "deepest", align = "center", w = 120, h = 44}
    deepest:on("tap", on_event):on("press", on_event):on("release", on_event):on("long_press", on_event)
    made = made + 1

    local words = string.rep("0123456789abcdef", 16)   -- 256 bytes: as long as text gets
    local i = 0
    while made < WIDGETS do
        i = i + 1
        local kind = KINDS[(i - 1) % #KINDS + 1]
        local col, row = (i - 1) % COLUMNS, ((i - 1) // COLUMNS) % ROWS
        local at = {align = "center", x = (col - 4) * CELL_W, y = -165 + row * CELL_H + CELL_H // 2}
        local w
        if made == WIDGETS - 1 then
            w = ui.label{align = "center", x = 0, y = -100, w = 240, wrap = true, font = "sans14", text = words}
        elseif kind == "label" then
            at.text, at.font, at.w, at.h = tostring(i), "mono8", CELL_W, CELL_H
            w = ui.label(at)
        elseif kind == "button" then
            at.text, at.font, at.w, at.h, at.radius = "b" .. i, "mono8", CELL_W, CELL_H, 3
            w = ui.button(at)
        elseif kind == "box" then
            at.w, at.h, at.bg, at.border, at.radius = CELL_W, CELL_H, "#203040", 1, 2
            w = ui.box(at)
        elseif kind == "arc" then
            at.w, at.h, at.thickness, at.value = CELL_H, CELL_H, 2, i % 100
            w = ui.arc(at)
        elseif kind == "bar" then
            at.w, at.h, at.value = CELL_W, 4, i % 100
            w = ui.bar(at)
        else
            at.src, at.scale = "tile.bin", 100 + i % 3 * 50
            w = ui.image(at)
        end
        if i % 3 == 0 then
            w:on("tap", on_event)
        end
        if i % 10 == 0 then
            w:on("press", on_event):on("release", on_event)
        end
        if i % 25 == 0 then
            w:on("long_press", on_event)
        end
        made = made + 1
    end
    return {count = made, tap = {x = ui.W // 2, y = ui.H // 2 + 70}, deepest = tostring(deepest)}
end

-- ---- The brief's two cases, and what they cost ----

-- __index recursing through Lua code: the C-call limit ends it.
local function index_recursion()
    local t = setmetatable({}, {__index = function(self, k) return self[k + 1] end})
    return pcall(function() return t[1] end)
end

-- The deepest parenthesised nesting that loads: a .lua file of each depth, 36 down to 26 (there is no load() to make
-- one here). Nothing deeper is in the package: the install compiles every file of it as a top-level chunk, and one
-- that nests 37 deep is refused there with "C stack overflow" (LUAI_MAXCCALLS is 40). A require has a few C levels
-- less to spend than that, so the first that loads is a little under 36.
local function deepest_nesting()
    for n = 36, 26, -1 do
        local ok = pcall(require, "deep" .. n)
        if ok then
            return n
        end
    end
    return nil
end

local function run_chains(with_json)
    local out = {}
    for _, name in ipairs(CHAIN_NAMES) do
        local depth, err = chains[name](nil, nil)
        local row = {depth = depth, err = err}
        if with_json then
            local _, _, result = chains[name](depth, json_leaf)
            row.json = result
        end
        out[name] = row
    end
    return out
end

function app.start()
    local ok, err = index_recursion()
    print("index recursion", ok, err)
    print("deepest nesting that loads", deepest_nesting())
    local rows = run_chains(false)
    for _, name in ipairs(CHAIN_NAMES) do
        print("chain", name, rows[name].depth, rows[name].err)
    end
end

function app.message(event, data)
    if event == "chains" then
        return run_chains(false)
    elseif event == "json" then
        return run_chains(true)
    elseif event == "floats" then
        return floats()
    elseif event == "floats_exact" then
        return floats_exact()
    elseif event == "floats_time" then
        return floats_time(data and data.n or 1, data and data.decode, data and data.digits, data and data.scale)
    elseif event == "widgets" then
        return widgets()
    elseif event == "hits" then
        return hits
    end
end

return app
