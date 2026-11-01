-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The spin fixture (connected apps Tasks 1 and 7): the board's spin gate's app. Four layouts, each with a picture
-- spinning at 33 turns a minute and a button under it:
-- - "disc" (the start): a 180 px record, an RGB565A8 image with a circular alpha, spinning whole (Task 1's gate, at the
--   180 px cap Task 7 set).
-- - "body": the same record as Images teaches it, its grooves and sheen drawn still (body.bin) under an 80 px label
--   that spins (label.bin).
-- - "cover": the 180 px record spinning over a still, dimmed 400 px picture (cover.bin), as a now-playing screen.
-- - "square": a 180 px square of spokes with no alpha (square.bin), whose corners swing as it spins: LVGL draws each step in
--   two bands, and a tear between them would break the spokes.
-- The board's gate reads a bench build's "spin frame <ms> ms" and "tearing effect:" lines while it spins, and taps the
-- button: each tap counts on the button and prints "tap <n> <time.mono()>", so the host can time its answer.
-- "layout" picks one, and so does a long press on the button, in turn (for an eye on the board, with no console); "spin",
-- "rotation" and "scale" change the picture that spins, and "state" says what get says.
-- The screen stays lit while it is in front. Nothing here is an example of how to write an app.
local app = {}

local cover = ui.image{src = "cover.bin", hidden = true}
local body = ui.image{src = "body.bin", hidden = true}
local disc = ui.image{src = "disc.bin", spin = 33}
local label = ui.image{src = "label.bin", hidden = true}
local square = ui.image{src = "square.bin", hidden = true}
local button = ui.button{text = "0", y = 150, w = 120, h = 56, radius = 28, bg = "#202a44"}
local taps = 0
local layout = "disc"

local spinners = {disc = disc, body = label, cover = disc, square = square}

local function spinner()
    return spinners[layout]
end

local function state()
    local s = spinner()
    return {layout = layout, rotation = s:get("rotation"), spin = s:get("spin"), scale = s:get("scale"), taps = taps}
end

-- Spinning pictures share 32,400 px: the one that stops goes first.
local function show(name)
    local old, new = spinner(), spinners[name]
    if old ~= new then
        old:set{spin = 0, hidden = true}
    end
    layout = name
    cover:set{hidden = name ~= "cover"}
    body:set{hidden = name ~= "body"}
    new:set{hidden = false, spin = 33}
end

local order = {disc = "body", body = "cover", cover = "square", square = "disc"}

button:on("long_press", function()
    show(order[layout])
    print("layout " .. layout)
end)

button:on("tap", function()
    taps = taps + 1
    button:set{text = tostring(taps)}
    print("tap " .. taps .. " " .. time.mono())
end)

function app.start()
    sys.keep_awake(true)
end

function app.message(event, data)
    if event == "layout" then
        if spinners[data.name] == nil then
            error("layout is disc, body, cover or square")
        end
        show(data.name)
    elseif event == "spin" then
        spinner():set{spin = data.rpm}
    elseif event == "rotation" then
        spinner():set{rotation = data.deg}
    elseif event == "scale" then
        spinner():set{scale = data.pct}
    elseif event ~= "state" then
        error("use layout, spin, rotation, scale or state")
    end
    return state()
end

return app
