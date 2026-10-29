-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The board check of the system's alarms: start() arms the alarm "t" for 5 s and alarm() prints "rang t" and shows it. Push it
-- with --run, send `>nap` (the screen asleep, LVGL paused) and wait 8 s: the alarm wakes the screen, brings the app to the front
-- and calls alarm("t"), which the boot log, the app's log and a snapshot show. The alarm rings (sound is on unless the options
-- say not), so the message "stop" ends the ring with sound.stop(): `>app.send alarm5 stop`.
local app = {}
local label

function app.start()
    label = ui.label{text = "waiting", font = "sans28", align = "center"}
    alarm.after("t", 5000)
end

function app.alarm(name)
    print("rang " .. name)
    label:set{text = "rang " .. name}
end

function app.message(event)
    if event == "stop" then
        sound.stop()
    end
    return true
end

return app
