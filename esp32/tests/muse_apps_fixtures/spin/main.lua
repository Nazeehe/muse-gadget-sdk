-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The spin fixture (connected apps Task 1): a 400 px disc, an RGB565A8 image with a circular alpha, spinning at 33 turns a
-- minute, and a button over it. The board's spin gate reads a bench build's "spin frame <ms> ms" lines while it spins, and
-- taps the button: each tap counts on the button and prints "tap <n> <time.mono()>", so the host can time its answer.
-- "spin", "rotation" and "scale" change the disc, and "state" says what get says. The screen stays lit while it is in front.
-- Nothing here is an example of how to write an app.
local app = {}

local disc = ui.image{src = "disc.bin", spin = 33}
local button = ui.button{text = "0", y = 150, w = 120, h = 56, radius = 28, bg = "#202a44"}
local taps = 0

local function state()
    return {rotation = disc:get("rotation"), spin = disc:get("spin"), scale = disc:get("scale"), taps = taps}
end

button:on("tap", function()
    taps = taps + 1
    button:set{text = tostring(taps)}
    print("tap " .. taps .. " " .. time.mono())
end)

function app.start()
    sys.keep_awake(true)
end

function app.message(event, data)
    if event == "spin" then
        disc:set{spin = data.rpm}
    elseif event == "rotation" then
        disc:set{rotation = data.deg}
    elseif event == "scale" then
        disc:set{scale = data.pct}
    elseif event ~= "state" then
        error("use spin, rotation, scale or state")
    end
    return state()
end

return app
