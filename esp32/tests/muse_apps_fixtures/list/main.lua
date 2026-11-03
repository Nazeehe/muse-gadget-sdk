-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The list fixture (games Task 5): the board's check of a scrolling list. The guide's Lists pattern at its size: a column that
-- scrolls, and 40 rows of wrapped text, each a row box, a bubble at its left or right, by turns, and a label (121 widgets of the
-- 128). The board drags it with the bench's >drag and reads its "scroll frames: ..." summaries; a tap on a row prints "tap <n>",
-- the row's number from the top, and a drag that scrolls taps nothing. `to` scrolls it to a y, and `end` to its end, so the
-- board knows where the rows are when it taps them.
-- The screen stays lit while it is in front. Nothing here is an example of how to write an app.
local app = {}

local ROWS = 40
local WORDS = {
    "On my way.",
    "Did you see the game last night? The second half was something else.",
    "Running ten minutes late, save me a seat by the window please.",
    "Sure!",
    "The meeting moved to three, same room as last week.",
    "Can you pick up bread on the way home?",
    "Thanks, that helps a lot.",
    "I left the keys under the blue pot by the door, next to the bike.",
}

local list

function app.start()
    sys.keep_awake(true)
    list = ui.box{w = 300, h = 340, align = "center", scroll = "y", layout = "column", gap = 8, pad = 8}
    for i = 1, ROWS do
        local row = ui.box{parent = list, w = 284}
        local mine = i % 2 == 0
        local bubble = ui.box{parent = row, align = mine and "right" or "left", bg = mine and "#2f6fde" or "#2b2f3a",
                              radius = 16, pad = 10}
        ui.label{parent = bubble, w = 200, wrap = true, color = "#ffffff",
                 text = i .. ". " .. WORDS[(i - 1) % #WORDS + 1]}
        row:on("tap", function() print("tap " .. i) end)
    end
end

function app.message(event, data)
    if event == "to" then
        local y = type(data) == "table" and data.y or 0
        list:scroll_to(y)
        return {at = y}
    elseif event == "end" then
        list:scroll_to("end")
        return {at = "end"}
    end
    error("unknown event " .. tostring(event))
end

return app
