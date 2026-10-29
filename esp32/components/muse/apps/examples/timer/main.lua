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
local shown = true                       -- a launch puts the app in front; show() and hide() say when it isn't

local LABELS = { idle = "Start", running = "Pause", paused = "Resume", done = "OK" }
local HINTS  = { idle = "", running = "", paused = "Paused", done = "Time's up" }

local changed                            -- below: it renders, and a render can find the countdown done

-- Milliseconds left. While running the alarm knows: 0 is a countdown that has rung and isn't told yet (app.alarm is
-- coming), nil is no alarm at all. The system can take an app's alarms away, and a countdown without one never rings:
-- it is done, on the screen and in the store, and not running at 0:00 for ever.
local function remaining()
  if s.state ~= "running" then return s.left end
  local left = alarm.remaining(ALARM)
  if left == nil then
    s.state, s.left = "done", 0
    changed()
    return 0
  end
  return left
end

-- The saved record if it is one this app wrote, else nil: the store is keyed by the app's id and outlives a reinstall,
-- so anything may be in it.
local function stored()
  local t = store.get("timer")
  if type(t) ~= "table" or LABELS[t.state] == nil then return nil end
  local total = type(t.total) == "number" and math.tointeger(t.total)
  local left = type(t.left) == "number" and math.tointeger(t.left)
  if not total or not left or total < 1000 or total > MAX_S * 1000 or left < 0 or left > total then return nil end
  return { state = t.state, total = total, left = left }
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

function changed()
  store.set("timer", s)
  ticking(s.state == "running" and shown)
  render()
end

-- Arms the system's alarm for ms: returns nothing when it is armed, else why it refused (no free slot, no memory). Arm
-- first, then change anything: a refusal leaves the state, the store and the screen as they were. The pcall is of
-- alarm.after itself, not of a function that calls it, so the reason is the system's words with no position in front.
local function arm(ms)
  local ok, why = pcall(alarm.after, ALARM, ms, { sound = true, wake = true })
  if not ok then return why end
end

-- A countdown of ms. Returns why the system refused its alarm, or nothing.
local function begin(ms)
  local rang = remaining() == 0          -- done, or rung and not told yet: the ring is this timer's own
  local why = arm(ms)
  if why then return why end
  if rang then sound.stop() end          -- the ring is the gadget's: end only this timer's, not another app's
  s.total, s.left, s.state = ms, ms, "running"
  changed()
end

local function pause()
  if s.state ~= "running" then return end
  local left = remaining()
  if s.state ~= "running" then return end  -- its alarm was gone: the read found it done, and there is nothing to pause
  s.left = left
  alarm.cancel(ALARM)
  s.state = "paused"
  changed()
end

-- Returns why the system refused its alarm, or nothing.
local function resume()
  if s.state ~= "paused" then return end
  local why = arm(math.max(1, s.left))   -- a pause that came as the alarm rang has 0 left, which alarm.after refuses
  if why then return why end
  s.state = "running"
  changed()
end

local function reset()
  local rang = remaining() == 0          -- before the cancel: done, or rung and not told yet
  alarm.cancel(ALARM)
  if rang then sound.stop() end          -- only this timer's own ring
  s.state, s.left = "idle", s.total
  changed()
end

local function adjust(delta_s)
  local secs = math.max(60, math.min(MAX_S, s.total // 1000 + delta_s))
  s.total, s.left = secs * 1000, secs * 1000
  changed()
end

function app.start()
  s = stored() or s                      -- a running countdown whose alarm has gone is found done by the first render

  ui.bg("#000000")
  -- Progress ring just inside the round edge; angles are clock degrees (0 = 12 o'clock).
  arc = ui.arc{ align = "center", w = 440, h = 440, min = 0, max = 1000, value = 1000,
                start_angle = 0, end_angle = 360, thickness = 16,
                color = "#ffb000", track_color = "#262626" }
  hint = ui.label{ align = "center", y = -120, w = 320, wrap = true, font = "sans20", color = "#a0a0a0", text = "" }
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
    local why
    if s.state == "idle" then why = begin(s.total)
    elseif s.state == "running" then pause()
    elseif s.state == "paused" then why = resume()
    else reset() end                     -- done: silence the alarm, back to idle
    if why then hint:set{ text = why } end   -- the system refused the alarm: nothing changed but this
  end)

  changed()
end

-- Muse controls the timer with app.send {id="timer", event=..., data=...}.
function app.message(event, data)
  if event == "start" then
    local secs = type(data) == "table" and math.tointeger(data.seconds)
    if not secs or secs < 1 or secs > MAX_S then
      error("start needs data.seconds, a whole number from 1 to 5999")
    end
    local why = begin(secs * 1000)
    if why then error(why, 0) end        -- the system refused the alarm: the reply is its reason, and nothing changed
    sys.show()                           -- bring the timer to the front
  elseif event == "pause" then pause()
  elseif event == "resume" then
    local why = resume()
    if why then error(why, 0) end
  elseif event == "reset" then reset()
  elseif event ~= "status" then
    error("unknown event '" .. tostring(event) .. "'; use start, pause, resume, reset or status")
  end
  local left = remaining()               -- first: it can find the countdown done, and the state is what it leaves
  return { state = s.state, remaining = (left + 999) // 1000 }
end

-- The system has already woken the screen, shown this tile and started the alarm sound.
function app.alarm(name)
  if name ~= ALARM then return end
  -- A firing is told after the app has handled what was queued before it, so a start can have armed the alarm again
  -- since: this firing is not that countdown's, which is still counting.
  local left = alarm.remaining(ALARM)
  if left and left > 0 then return end
  s.state, s.left = "done", 0
  changed()
end

function app.show() shown = true; ticking(s.state == "running"); render() end
function app.hide() shown = false; ticking(false) end

return app
