-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- A label of time.format("%a %H:%M"), made again every second, and "no time yet" until the clock is set. The board
-- check of the time module: its picture against the clock on the desk.
local app = {}
local label

local function refresh()
    label:set{text = time.synced() and time.format("%a %H:%M") or "no time yet"}
end

function app.start()
    label = ui.label{text = "", font = "sans28", align = "center"}
    refresh()
    timer.every(1000, refresh)
end

return app
