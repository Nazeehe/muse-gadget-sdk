-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The bench fixture (Task 27): the VM's speed on the gadget, and how late a callback runs while something else holds
-- the CPU. "run" starts a timer.every whose callback runs n turns of a loop and times them with time.mono(); "result"
-- says what the run did. The VM's instructions per second are the `instructions` of >app.status read before and after
-- a run, divided by the run's busy_ms (E13): kind "loop" is `for i = 1, n do end`, one VM instruction a turn, and kind
-- "mix" is table, arithmetic and call work. "fill" prints lines and makes labels, so that a status is a few KB, and
-- "spin" never returns: the call budget stops it, and the time it ran is the budget's on the board.
-- Nothing here is an example of how to write an app.
local app = {}

local label = ui.label{text = "bench", font = "sans28"}
local last = {kind = "loop", n = 0, calls = 0, turns = 0, busy_ms = 0, elapsed_ms = 0, worst_ms = 0, worst_gap_ms = 0,
              done = true}
local ticker

local function add(a, b)
    return a + b
end

-- n turns of the kind's loop.
local function spin(kind, n)
    if kind == "loop" then
        for i = 1, n do end
        return
    end
    local t = {}
    for i = 1, n do
        local k = (i & 63) + 1
        t[k] = add(t[k] or 0, i)
    end
end

local function whole(data, key, low, high, default)
    local v = type(data) == "table" and data[key] or nil
    if v == nil then
        return default
    end
    v = math.tointeger(v)
    if v == nil or v < low or v > high then
        error(key .. " must be a whole number from " .. low .. " to " .. high)
    end
    return v
end

local function finish()
    if ticker ~= nil then
        ticker:cancel()
        ticker = nil
    end
    last.done = true
    sys.keep_awake(false)
    label:set{text = "done"}
    print(string.format("bench %s: %d calls of %d, %d turns, busy %d ms, elapsed %d ms, worst %d ms, worst gap %d ms",
                        last.kind, last.calls, last.n, last.turns, last.busy_ms, last.elapsed_ms, last.worst_ms,
                        last.worst_gap_ms))
end

local function run(data)
    local seconds = whole(data, "seconds", 1, 60, 10)
    local period = whole(data, "period_ms", 40, 1000, 500)
    local n = whole(data, "n", 1, 400000, 400000)
    local kind = type(data) == "table" and data.kind or "loop"
    if kind ~= "loop" and kind ~= "mix" then
        error("kind must be loop or mix")
    end
    if ticker ~= nil then
        ticker:cancel()
    end
    last = {kind = kind, n = n, calls = 0, turns = 0, busy_ms = 0, elapsed_ms = 0, worst_ms = 0, worst_gap_ms = 0,
            done = false}
    local first, previous
    sys.keep_awake(true)
    label:set{text = "running"}
    ticker = timer.every(period, function()
        local began = time.mono()
        if first == nil then
            first = began
        elseif began - previous > last.worst_gap_ms then
            last.worst_gap_ms = began - previous
        end
        previous = began
        spin(kind, n)
        local ended = time.mono()
        last.calls = last.calls + 1
        last.turns = last.turns + n
        last.busy_ms = last.busy_ms + (ended - began)
        if ended - began > last.worst_ms then
            last.worst_ms = ended - began
        end
        last.elapsed_ms = ended - first
        if ended - first >= seconds * 1000 then
            finish()
        end
    end)
    return {running = true}
end

local function fill(data)
    local lines = whole(data, "lines", 0, 50, 50)
    local widgets = whole(data, "widgets", 0, 100, 60)
    local text = string.rep("0123456789", 10)
    for i = 1, lines do
        print(text)
    end
    for i = 1, widgets do
        ui.label{text = "w" .. i, font = "sans14", x = (i % 10) * 30 - 135, y = (i // 10) * 20 - 100}
    end
    return {lines = lines, widgets = widgets}
end

function app.message(event, data)
    if event == "run" then
        return run(data)
    elseif event == "result" then
        return last
    elseif event == "fill" then
        return fill(data)
    elseif event == "spin" then
        while true do end  -- the call budget stops it, and the app crashes with the budget's reason
    end
    error("unknown event '" .. tostring(event) .. "'; use run, result, fill or spin")
end

function app.hide()
    -- No timer runs behind the face either way; a run cut short by a hide says so.
    if ticker ~= nil then
        finish()
    end
end

return app
