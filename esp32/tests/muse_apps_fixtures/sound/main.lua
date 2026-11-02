-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The sound fixture (games Task 3): the board's check of app sound. A box flaps up at each `press` and falls back, with a short
-- square tone for each flap, as a game's flap would sound, while a timer.every(40) moves it: the board reads the bench's
-- "sound start <ms> ms" lines of 20 presses, and the frames and cpu_pct with sound and with `mute`. `chord` starts four tones
-- at once, for the mixer's lock held over a render of four voices. `alarm` arms a 5 s alarm with sound and starts the looping
-- clip: the ring's start stops the clip (Muse wins the speaker) and alarm() ends the ring, leaving the clip's handle as it was,
-- so the silence after the ring is the cut's, not the app's. loop.wav is two notes synthesized for this fixture (C5, E5).
-- The screen stays lit while it is in front. Nothing here is an example of how to write an app.
local app = {}

local FLAP = -0.5        -- px a ms, up
local GRAVITY = 0.0015   -- px a ms, a ms
local LOW, HIGH = 150, -150

local box = ui.box{w = 60, h = 60, radius = 12, bg = "#f2b632", align = "center"}
local count = ui.label{text = "0", align = "center", y = -170, color = "#ffffff"}
local flaps, muted, rang = 0, false, nil
local y, vy, last = 0, 0, nil
local tick, clip = nil, nil

local function step()
    local now = time.mono()
    local dt = math.min(100, now - (last or now))
    last = now
    vy = vy + GRAVITY * dt
    y = y + vy * dt
    if y > LOW then
        y, vy = LOW, 0
    elseif y < HIGH then
        y, vy = HIGH, 0
    end
    box:set{y = math.floor(y + 0.5)}
end

local function ticking(on)
    if on and tick == nil then
        last = nil
        tick = timer.every(40, step)
    elseif not on and tick ~= nil then
        tick:cancel()
        tick = nil
    end
end

local function looping(on)
    if on and clip == nil then
        clip = sound.play("loop.wav", {loop = true, volume = 60})
    elseif not on and clip ~= nil then
        clip:stop()
        clip = nil
    end
end

function app.start()
    sys.keep_awake(true)
    ticking(true)
end

function app.show()
    ticking(true)
end

function app.hide()
    ticking(false)
    clip = nil  -- its sound ended with the hide
end

function app.alarm(name)
    rang = name
    print("rang " .. name)
    sound.stop()
end

function app.message(event, data)
    data = type(data) == "table" and data or {}
    if event == "press" then
        flaps = flaps + 1
        vy = FLAP
        count:set{text = tostring(flaps)}
        local name = nil
        if not muted then
            name = tostring(sound.tone{hz = 520, to_hz = 780, ms = 60, wave = "square", volume = 40})
        end
        return {flaps = flaps, muted = muted, sound = name}
    elseif event == "mute" then
        if data.on == nil then
            muted = not muted
        else
            muted = data.on == true
        end
        return {muted = muted}
    elseif event == "chord" then
        local names = {}
        for i, hz in ipairs({262, 330, 392, 523}) do
            names[i] = tostring(sound.tone{hz = hz, ms = 1500, volume = 25})
        end
        return {sounds = names}
    elseif event == "loop" then
        looping(data.on ~= false)
        return {looping = clip ~= nil}
    elseif event == "alarm" then
        alarm.after("ring", 5000)
        looping(true)
        return {armed = 5000}
    elseif event == "state" then
        return {flaps = flaps, muted = muted, looping = clip ~= nil, y = math.floor(y + 0.5), rang = rang}
    end
    error("unknown event " .. tostring(event))
end

return app
