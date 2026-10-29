-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The board check of sys.keep_awake (the "keep_awake" permission). start() asks for the screen to stay lit while this app is in
-- front. Push it with --run and leave the gadget alone for longer than its idle timeout (muse_settings_sleep_s: 120 s unless it
-- was set otherwise): no "sleeping (auto-sleep)" line comes. Then `>app.send keep_awake off` takes the request back, and the same
-- screen goes to sleep by itself after the timeout, which shows the first wait was the request's doing. `on` asks again.
local app = {}

local function awake(on)
    sys.keep_awake(on)
    print("keep_awake " .. (on and "on" or "off"))
end

function app.start()
    ui.label{text = "keep awake", font = "sans28", align = "center"}
    awake(true)
end

function app.message(event)
    if event ~= "on" and event ~= "off" then
        error("use on or off")
    end
    awake(event == "on")
    return {keep_awake = event == "on"}
end

return app
