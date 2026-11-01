-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The spinner fixture (connected apps Task 8): the board's check for ui.spinner. A clock-like layout (an edge ring, 10:09 in the
-- big digits, a button under it for the taps) with a 48 px spinner over it, which "mode" shows, hides or removes: the frames and
-- the taps with it have to match the layout without it. The button counts its taps and prints "tap <n> <time.mono()>", so the
-- host can time its answer. The screen stays lit while it is in front. Nothing here is an example of how to write an app.
local app = {}

ui.arc{w = 440, h = 440, value = 100, thickness = 8, color = "#3a6ea5"}
ui.label{text = "10:09", font = "digits96", y = -30}
local button = ui.button{text = "0", y = 150, w = 120, h = 56, radius = 28, bg = "#202a44"}
local spinner = ui.spinner{y = -140, color = "#ffcc00"}
local mode = "shown"
local taps = 0

button:on("tap", function()
    taps = taps + 1
    button:set{text = tostring(taps)}
    print("tap " .. taps .. " " .. time.mono())
end)

local function show(name)
    if name == "none" then
        if spinner ~= nil then
            spinner:delete()
            spinner = nil
        end
    else
        if spinner == nil then
            spinner = ui.spinner{y = -140, color = "#ffcc00"}
        end
        spinner:set{hidden = name == "hidden"}
    end
    mode = name
end

function app.start()
    sys.keep_awake(true)
end

function app.message(event, data)
    if event == "mode" then
        if data.name ~= "shown" and data.name ~= "hidden" and data.name ~= "none" then
            error("mode is shown, hidden or none")
        end
        show(data.name)
    elseif event ~= "state" then
        error("use mode or state")
    end
    return {mode = mode, taps = taps}
end

return app
