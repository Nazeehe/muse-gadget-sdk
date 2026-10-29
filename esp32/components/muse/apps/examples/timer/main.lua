-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Countdown timer for the Muse gadget (Muse apps api 1).
-- While running, the system alarm is the source of truth for time left, so the
-- countdown keeps going while the screen sleeps or another app is in use, and
-- rings even if this app isn't loaded.
local app = {}

local ALARM = "done"
local MAX_S = 99 * 60 + 59

-- Persisted state (store). total/left are milliseconds.
local s = { state = "idle", total = 5 * 60 * 1000, left = 5 * 60 * 1000 }

local arc, digits, hint, main_btn, minus, plus, reset_btn
local ticker

local LABELS = { idle = "Start", running = "Pause", paused = "Resume", done = "OK" }
local HINTS  = { idle = "", running = "", paused = "Paused", done = "Time's up" }

local function remaining()
  if s.state == "running" then
    return alarm.remaining(ALARM) or 0
  end
  return s.left
end

local function fmt(ms)
  local t = (ms + 999) // 1000           -- round up: 0:00 shows only when it rings
  return string.format("%d:%02d", t // 60, t % 60)
end

local function render()
  local left = remaining()
  digits:set{ text = fmt(left) }
  arc:set{ value = s.total > 0 and left * 1000 // s.total or 0 }
  hint:set{ text = HINTS[s.state] }
  main_btn:set{ text = LABELS[s.state] }
  minus:set{ hidden = s.state ~= "idle" }
  plus:set{ hidden = s.state ~= "idle" }
  reset_btn:set{ hidden = s.state ~= "running" and s.state ~= "paused" }
end

local function ticking(on)
  if on and not ticker then
    ticker = timer.every(250, render)
  elseif not on and ticker then
    ticker:cancel()
    ticker = nil
  end
end

local function changed()
  store.set("timer", s)
  ticking(s.state == "running")
  render()
end

local function begin(ms)
  s.total, s.left, s.state = ms, ms, "running"
  alarm.after(ALARM, ms, { sound = true, wake = true })
  changed()
end

local function pause()
  if s.state ~= "running" then return end
  s.left = alarm.remaining(ALARM) or 0
  alarm.cancel(ALARM)
  s.state = "paused"
  changed()
end

local function resume()
  if s.state ~= "paused" then return end
  s.state = "running"
  alarm.after(ALARM, s.left, { sound = true, wake = true })
  changed()
end

local function reset()
  alarm.cancel(ALARM)
  sound.stop()
  s.state, s.left = "idle", s.total
  changed()
end

local function adjust(delta_s)
  local secs = math.max(60, math.min(MAX_S, s.total // 1000 + delta_s))
  s.total, s.left = secs * 1000, secs * 1000
  changed()
end

function app.start()
  s = store.get("timer", s)
  if s.state == "running" and not alarm.remaining(ALARM) then
    s.state, s.left = "done", 0          -- it rang while the app wasn't loaded
  end

  ui.bg("#000000")
  -- Progress ring just inside the round edge; angles are clock degrees (0 = 12 o'clock).
  arc = ui.arc{ align = "center", w = 440, h = 440, min = 0, max = 1000, value = 1000,
                start_angle = 0, end_angle = 360, thickness = 16,
                color = "#ffb000", track_color = "#262626" }
  hint = ui.label{ align = "center", y = -120, font = "sans20", color = "#a0a0a0", text = "" }
  digits = ui.label{ align = "center", y = -35, font = "digits96", color = "#ffffff", text = "" }
  minus = ui.button{ align = "center", x = -70, y = 60, w = 110, h = 56, radius = 28,
                     bg = "#262626", color = "#ffffff", font = "sans20", text = "-1 min" }
  plus = ui.button{ align = "center", x = 70, y = 60, w = 110, h = 56, radius = 28,
                    bg = "#262626", color = "#ffffff", font = "sans20", text = "+1 min" }
  reset_btn = ui.button{ align = "center", y = 60, w = 140, h = 56, radius = 28,
                         bg = "#262626", color = "#ffffff", font = "sans20", text = "Reset" }
  main_btn = ui.button{ align = "center", y = 135, w = 170, h = 64, radius = 32,
                        bg = "#ffb000", color = "#000000", font = "sans28", text = "Start" }

  minus:on("tap", function() adjust(-60) end)
  plus:on("tap", function() adjust(60) end)
  reset_btn:on("tap", reset)
  main_btn:on("tap", function()
    if s.state == "idle" then begin(s.total)
    elseif s.state == "running" then pause()
    elseif s.state == "paused" then resume()
    else reset() end                     -- done: silence the alarm, back to idle
  end)

  changed()
end

-- Muse controls the timer with app.send {id="timer", event=..., data=...}.
function app.message(event, data)
  if event == "start" then
    local secs = math.tointeger(data and data.seconds)
    if not secs or secs < 1 or secs > MAX_S then
      error("start needs data.seconds, a whole number from 1 to 5999")
    end
    begin(secs * 1000)
    sys.show()                           -- bring the timer to the front
  elseif event == "pause" then pause()
  elseif event == "resume" then resume()
  elseif event == "reset" then reset()
  elseif event ~= "status" then
    error("unknown event '" .. tostring(event) .. "'; use start, pause, resume, reset or status")
  end
  return { state = s.state, remaining = (remaining() + 999) // 1000 }
end

-- The system has already woken the screen, shown this tile and started the alarm sound.
function app.alarm(name)
  if name == ALARM then
    s.state, s.left = "done", 0
    changed()
  end
end

function app.show() ticking(s.state == "running"); render() end
function app.hide() ticking(false) end

return app
