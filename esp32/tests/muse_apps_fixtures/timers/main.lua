-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- A timer.after(100) and a timer.every(250), each printing how long after start() it came, in milliseconds on
-- time.mono(). The message "cancel" cancels the repeating one by its handle, and "cancel_by_name" with timer.cancel.
local app = {}
local started, tick

local function since()
    return time.mono() - started
end

function app.start()
    started = time.mono()
    timer.after(100, function() print("after", since()) end)
    tick = timer.every(250, function() print("every", since()) end)
end

function app.message(event)
    if event == "cancel" then
        tick:cancel()
    elseif event == "cancel_by_name" then
        timer.cancel(tick)
    end
    return true
end

return app
