-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The swipe fixture (swipes Task 3): the board's check of an app's swipes, with no sound. A full-screen box hears every swipe and
-- tap that starts off the list, and prints "swipe <dir>" or "tap". A list that scrolls sits at the right, off the swipes' paths
-- (along y 233 from x 10 to 330, and along x 233 from y 130 to 330), so a drag up or down on it scrolls it and is no swipe; nothing
-- in it has a handler. `last` replies with the last of those prints and how many have come.
-- The screen stays lit while it is in front. Nothing here is an example of how to write an app.
local app = {}

local ROWS = 12
local last, count = nil, 0

local function heard(what)
    last, count = what, count + 1
    print(what)
end

function app.start()
    sys.keep_awake(true)
    local board = ui.box{w = ui.W, h = ui.H}
    board:on("swipe", function(_, _, dir) heard("swipe " .. dir) end)
    board:on("tap", function() heard("tap") end)
    local list = ui.box{align = "right", x = -26, w = 90, h = 150, scroll = "y", layout = "column", gap = 4, pad = 4,
                        bg = "#262626", radius = 12}
    for i = 1, ROWS do
        ui.label{parent = list, text = "row " .. i, font = "sans16"}
    end
end

function app.message(event)
    if event == "last" then
        return {last = last or json.null, count = count}
    end
    error("unknown event " .. tostring(event))
end

return app
