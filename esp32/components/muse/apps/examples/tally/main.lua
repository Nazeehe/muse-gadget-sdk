-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Tally, the guide's connected example (Muse apps api 1): a count that its hook keeps on the VM (hook.sh, beside this
-- file). The hook writes state/count.json and then a new state/rev; the app watches state/rev and reads the count when it
-- changes. "+1" and "Reset" are commands the hook applies and acks; "Ask" puts a question to Muse. A spinner shows while
-- any of them waits. See the guide's Connected apps.
local app = {}

local QUESTION = "Say hello in five words"
local STATUS_MAX = 120                   -- a label holds 256 bytes: replies and errors are cut to this

local count, status, spinner, watch
local waiting = 0                        -- commands and asks that wait: the spinner shows while there are any

-- s cut to STATUS_MAX bytes at a character's start, with "..." when it was longer.
local function fit(s)
  s = tostring(s)
  if #s <= STATUS_MAX then return s end
  local n = STATUS_MAX - 3
  while n > 0 and (s:byte(n + 1) & 0xC0) == 0x80 do n = n - 1 end
  return s:sub(1, n) .. "..."
end

local function say(text) status:set{ text = fit(text) } end

local function wait(delta)
  waiting = waiting + delta
  spinner:set{ hidden = waiting == 0 }
end

-- The count from the hook's state, if it is one: the file is the hook's, so check it.
local function show(v)
  if type(v) == "table" and math.type(v.count) == "integer" then
    count:set{ text = tostring(v.count) }
  end
end

-- The hook changed its files: read the count.
local function on_rev(rev, err)
  if rev == nil then say(err) return end
  vm.read("state/count.json", function(v, why)
    if v == nil then say(why) return end
    show(v)
  end)
end

-- A command for the hook. Its ack's result is {ok = true, count = n}, or {ok = false, error = "..."}.
local function command(name, args)
  wait(1)
  local ok, err = pcall(vm.command, name, args, function(acked, result)
    wait(-1)
    if not acked then say(result) return end              -- "timed out", or the VM's words
    if type(result) == "table" and result.ok == false then say(result.error) return end
    show(result)
    say("")
  end)
  if not ok then wait(-1) say(err) end                    -- refused at the call: the clock isn't set, ...
end

-- A question for Muse: the call refuses a second ask, or one within 30 s of the last, so it goes through pcall.
local function ask()
  wait(1)
  local ok, err = pcall(muse.ask, QUESTION, function(reply, why)
    wait(-1)
    say(reply or why)
  end)
  if not ok then wait(-1) say(err) end
end

function app.start()
  ui.bg("#000000")
  ui.button{ text = "Ask", font = "sans20", align = "center", y = -120, w = 110, h = 48 }:on("tap", ask)
  spinner = ui.spinner{ align = "center", x = 85, y = -120, w = 36, h = 36, thickness = 5, hidden = true }
  count = ui.label{ text = "-", font = "digits96", align = "center", y = -30 }
  status = ui.label{ text = "", font = "sans16", color = "#9aa0a6", align = "center", y = 50, w = 300, wrap = true }
  ui.button{ text = "+1", font = "sans28", align = "center", x = -70, y = 115, w = 110, h = 56 }:on("tap", function()
    command("add", { n = 1 })
  end)
  ui.button{ text = "Reset", font = "sans20", align = "center", x = 70, y = 115, w = 110, h = 56 }:on("tap", function()
    command("reset")
  end)
  watch = vm.watch("state/rev", 1, on_rev)  -- reads only while the app is in front and the screen is lit
end

return app
